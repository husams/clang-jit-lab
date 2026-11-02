#!/usr/bin/env python3
"""Deploy the HTML edition of the lab (site/) to a Kubernetes cluster.

    scripts/deploy.py apply  [options]   rebuild site/, render and apply, wait, verify
    scripts/deploy.py render [options]   rebuild site/ and write out/deploy/manifest.json only
    scripts/deploy.py status [options]   show the deployed objects and probe the URL
    scripts/deploy.py delete [options]   remove everything this script created

Options:
    --context CTX        kubectl context (default: the current context)
    --namespace NS       namespace (default: docs; created if missing)
    --host NAME          host name routed by the Istio gateway (default: clang-jit.senussi.me)
    --alias NAME         additional host name routed to the same site (repeatable)
    --gateway NS/NAME    Istio Gateway to bind the VirtualService to (default: istio-system/main-gateway)
    --gateway-ip IP      address used to probe the URL without DNS (default: 192.168.1.200)
    --no-build           use site/ as it is, without running scripts/build_site.py

What gets created (all labelled app.kubernetes.io/name=clang-repl-lab):
    ConfigMaps  clang-repl-lab-site-<n>-<hash>   the site's files, packed into ConfigMaps of at
                                                 most ~700 KB each (a ConfigMap is limited to 1 MiB)
    ConfigMap   clang-repl-lab-nginx-<hash>      nginx server block (port 8080, /healthz, gzip)
    Deployment  clang-repl-lab                   nginxinc/nginx-unprivileged, read-only root
                                                 filesystem, the ConfigMaps as one projected volume
    Service     clang-repl-lab                   ClusterIP, port 80 -> 8080
    VirtualService clang-repl-lab                HOST on the Istio gateway

The ConfigMap names carry a content hash, so new content means new names and a rolling
update of the Deployment; ConfigMaps of older versions are deleted after the rollout.
Objects are applied with server-side apply (a client-side apply would try to store each
ConfigMap again in an annotation, which is limited to 256 KB).

DNS is not managed here: HOST needs an A record pointing at the gateway's address.
"""
import argparse
import base64
import hashlib
import http.client
import json
import socket
import ssl
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "site"
OUT = ROOT / "out" / "deploy"
APP = "clang-repl-lab"
IMAGE = "nginxinc/nginx-unprivileged:1.29-alpine"
FIELD_MANAGER = "clang-repl-lab-deploy"
BIN_LIMIT = 700 * 1024  # bytes of file content per ConfigMap (limit is 1 MiB for the whole object)

NGINX_CONF = r"""server {
    listen 8080;
    server_name _;
    root /usr/share/nginx/html;
    index index.html;

    gzip on;
    gzip_min_length 1024;
    gzip_comp_level 5;
    gzip_types text/plain text/css application/javascript application/json image/svg+xml;

    location = /healthz {
        access_log off;
        default_type text/plain;
        return 200 "ok\n";
    }

    location /assets/ {
        add_header Cache-Control "public, max-age=300";
    }

    location / {
        try_files $uri $uri/ =404;
        add_header Cache-Control "no-cache";
    }
}
"""


def log(msg):
    print(msg, flush=True)


def die(msg):
    sys.exit(f"deploy: {msg}")


class Kube:
    def __init__(self, context):
        self.context = context

    def run(self, *args, input=None, check=True, capture=True):
        cmd = ["kubectl", f"--context={self.context}", *args]
        p = subprocess.run(cmd, input=input, text=True,
                           stdout=subprocess.PIPE if capture else None,
                           stderr=subprocess.PIPE if capture else None)
        if check and p.returncode != 0:
            die(f"{' '.join(cmd[:6])} ... failed:\n{(p.stderr or '').strip()}")
        return p


def labels(component):
    return {"app.kubernetes.io/name": APP, "app.kubernetes.io/component": component,
            "app.kubernetes.io/part-of": "docs", "app.kubernetes.io/managed-by": "scripts-deploy.py"}


def short_hash(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p if isinstance(p, bytes) else p.encode())
    return h.hexdigest()[:10]


def site_files():
    files = sorted(p for p in SITE.rglob("*") if p.is_file())
    if not files or not (SITE / "index.html").exists():
        die(f"{SITE} has no index.html; run python3 scripts/build_site.py")
    return files


def pack(files):
    """First-fit-decreasing packing of the files into ConfigMap-sized bins."""
    bins = []
    for f in sorted(files, key=lambda p: p.stat().st_size, reverse=True):
        size = f.stat().st_size
        if size > BIN_LIMIT:
            die(f"{f.relative_to(ROOT)} is {size} bytes, more than one ConfigMap can hold")
        for b in bins:
            if b["size"] + size <= BIN_LIMIT:
                b["files"].append(f)
                b["size"] += size
                break
        else:
            bins.append({"files": [f], "size": size})
    for b in bins:
        b["files"].sort()
    return bins


def key_for(rel):
    """ConfigMap keys cannot contain '/', so assets/app.js is stored as assets__app.js."""
    return rel.replace("/", "__")


def render(ns, host, gateway, aliases=()):
    files = site_files()
    bins = pack(files)
    objects, sources = [], []
    content_hash = []
    for i, b in enumerate(bins, 1):
        data, binary, items = {}, {}, []
        for f in b["files"]:
            rel = f.relative_to(SITE).as_posix()
            raw = f.read_bytes()
            try:
                data[key_for(rel)] = raw.decode("utf-8")
            except UnicodeDecodeError:
                binary[key_for(rel)] = base64.b64encode(raw).decode()
            items.append({"key": key_for(rel), "path": rel})
        h = short_hash(json.dumps(data, sort_keys=True), json.dumps(binary, sort_keys=True))
        content_hash.append(h)
        name = f"{APP}-site-{i}-{h}"
        cm = {"apiVersion": "v1", "kind": "ConfigMap",
              "metadata": {"name": name, "namespace": ns, "labels": labels("site")}, "data": data}
        if binary:
            cm["binaryData"] = binary
        objects.append(cm)
        sources.append({"configMap": {"name": name, "items": items}})

    conf_name = f"{APP}-nginx-{short_hash(NGINX_CONF)}"
    objects.append({"apiVersion": "v1", "kind": "ConfigMap",
                    "metadata": {"name": conf_name, "namespace": ns, "labels": labels("nginx-conf")},
                    "data": {"default.conf": NGINX_CONF}})

    selector = {"app.kubernetes.io/name": APP}
    probe = {"httpGet": {"path": "/healthz", "port": "http"}}
    objects.append({
        "apiVersion": "apps/v1", "kind": "Deployment",
        "metadata": {"name": APP, "namespace": ns, "labels": labels("web")},
        "spec": {
            "replicas": 1,
            "revisionHistoryLimit": 3,
            "selector": {"matchLabels": selector},
            "strategy": {"type": "RollingUpdate", "rollingUpdate": {"maxUnavailable": 0, "maxSurge": 1}},
            "template": {
                "metadata": {"labels": {**selector, "app.kubernetes.io/component": "web"},
                             "annotations": {"clang-repl-lab/content": short_hash(*content_hash, conf_name)}},
                "spec": {
                    "automountServiceAccountToken": False,
                    "securityContext": {"runAsNonRoot": True, "runAsUser": 101, "runAsGroup": 101,
                                        "fsGroup": 101, "seccompProfile": {"type": "RuntimeDefault"}},
                    "containers": [{
                        "name": "nginx",
                        "image": IMAGE,
                        "ports": [{"name": "http", "containerPort": 8080}],
                        "readinessProbe": {**probe, "initialDelaySeconds": 2, "periodSeconds": 10},
                        "livenessProbe": {**probe, "initialDelaySeconds": 10, "periodSeconds": 30},
                        "resources": {"requests": {"cpu": "10m", "memory": "32Mi"},
                                      "limits": {"memory": "128Mi"}},
                        "securityContext": {"allowPrivilegeEscalation": False, "readOnlyRootFilesystem": True,
                                            "capabilities": {"drop": ["ALL"]}},
                        "volumeMounts": [
                            {"name": "site", "mountPath": "/usr/share/nginx/html", "readOnly": True},
                            {"name": "nginx-conf", "mountPath": "/etc/nginx/conf.d", "readOnly": True},
                            {"name": "tmp", "mountPath": "/tmp"},
                        ],
                    }],
                    "volumes": [
                        {"name": "site", "projected": {"sources": sources}},
                        {"name": "nginx-conf", "configMap": {"name": conf_name}},
                        {"name": "tmp", "emptyDir": {"sizeLimit": "64Mi"}},
                    ],
                },
            },
        },
    })
    objects.append({
        "apiVersion": "v1", "kind": "Service",
        "metadata": {"name": APP, "namespace": ns, "labels": labels("web")},
        "spec": {"type": "ClusterIP", "selector": selector,
                 "ports": [{"name": "http", "port": 80, "targetPort": "http"}]},
    })
    objects.append({
        "apiVersion": "networking.istio.io/v1", "kind": "VirtualService",
        "metadata": {"name": APP, "namespace": ns, "labels": labels("web")},
        "spec": {"hosts": list(dict.fromkeys([host, *aliases])), "gateways": [gateway],
                 "http": [{"route": [{"destination": {"host": f"{APP}.{ns}.svc.cluster.local",
                                                      "port": {"number": 80}}}]}]},
    })
    return {"apiVersion": "v1", "kind": "List", "items": objects}, bins


class _PinnedHTTPS(http.client.HTTPSConnection):
    """HTTPS to HOST (SNI, certificate check, Host header) over a TCP connection to IP."""

    def __init__(self, ip, host):
        super().__init__(host, 443, timeout=10, context=ssl.create_default_context())
        self._ip = ip

    def connect(self):
        sock = socket.create_connection((self._ip, 443), self.timeout)
        self.sock = self._context.wrap_socket(sock, server_hostname=self.host)


def probe(host, ip, path="/"):
    """GET https://HOST/PATH through the gateway address, without relying on DNS."""
    try:
        conn = _PinnedHTTPS(ip, host)
        conn.request("GET", path)
        r = conn.getresponse()
        return r.status, r.read()
    except (OSError, http.client.HTTPException) as e:
        return None, str(e).encode()


def wait_probe(host, ip, want=b"<title>", tries=30):
    status = body = None
    for _ in range(tries):
        status, body = probe(host, ip)
        if status == 200 and want in body:
            return status, body
        time.sleep(2)
    return status, body


def main(argv):
    ap = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    ap.add_argument("action", choices=["apply", "render", "status", "delete"])
    ap.add_argument("--context")
    ap.add_argument("--namespace", default="docs")
    ap.add_argument("--host", default="clang-jit.senussi.me")
    ap.add_argument("--alias", action="append", default=[])
    ap.add_argument("--gateway", default="istio-system/main-gateway")
    ap.add_argument("--gateway-ip", default="192.168.1.200")
    ap.add_argument("--no-build", action="store_true")
    a = ap.parse_args(argv)

    ctx = a.context or subprocess.run(["kubectl", "config", "current-context"], text=True,
                                      stdout=subprocess.PIPE).stdout.strip()
    if not ctx:
        die("no kubectl context; pass --context")
    k = Kube(ctx)
    ns = a.namespace
    sel = f"app.kubernetes.io/name={APP}"

    if a.action in ("apply", "render"):
        if not a.no_build:
            subprocess.run([sys.executable, str(ROOT / "scripts" / "build_site.py")], check=True)
        manifest, bins = render(ns, a.host, a.gateway, a.alias)
        OUT.mkdir(parents=True, exist_ok=True)
        (OUT / "manifest.json").write_text(json.dumps(manifest, indent=2))
        log(f"rendered {len(manifest['items'])} objects ({len(bins)} site ConfigMaps: "
            + ", ".join(f"{len(b['files'])} files/{b['size'] // 1024} KB" for b in bins)
            + f") -> {(OUT / 'manifest.json').relative_to(ROOT)}")
        if a.action == "render":
            return 0

        log(f"context {ctx}, namespace {ns}, host {a.host}")
        if k.run("get", "namespace", ns, check=False).returncode != 0:
            k.run("create", "namespace", ns)
            log(f"created namespace {ns}")
        k.run("apply", "--server-side", f"--field-manager={FIELD_MANAGER}", "--force-conflicts",
              "-f", str(OUT / "manifest.json"))
        log("applied")
        p = k.run("-n", ns, "rollout", "status", f"deployment/{APP}", "--timeout=180s", check=False)
        if p.returncode != 0:
            k.run("-n", ns, "get", "pods", "-l", sel, capture=False, check=False)
            die(f"rollout did not finish:\n{p.stdout}{p.stderr}")
        log("rollout complete")

        keep = {o["metadata"]["name"] for o in manifest["items"] if o["kind"] == "ConfigMap"}
        names = k.run("-n", ns, "get", "configmap", "-l", sel, "-o", "name").stdout.split()
        stale = [n for n in names if n.split("/", 1)[1] not in keep]
        if stale:
            k.run("-n", ns, "delete", *stale)
            log(f"deleted {len(stale)} ConfigMap(s) of the previous version")

        status, body = wait_probe(a.host, a.gateway_ip)
        if status == 200:
            log(f"verified: https://{a.host}/ answers 200 through the gateway at {a.gateway_ip}")
        else:
            log(f"warning: https://{a.host}/ through {a.gateway_ip} gave {status}: {(body or b"")[:200]!r}")
        try:
            addr = socket.gethostbyname(a.host)
            log(f"DNS: {a.host} -> {addr}")
        except OSError:
            log(f"DNS: {a.host} does not resolve yet; add an A record -> {a.gateway_ip}")
        return 0

    if a.action == "status":
        k.run("-n", ns, "get", "deploy,rs,pods,svc,configmap,virtualservice", "-l", sel, "-o", "wide",
              capture=False, check=False)
        status, _ = probe(a.host, a.gateway_ip)
        log(f"https://{a.host}/ via {a.gateway_ip}: {status}")
        return 0

    if a.action == "delete":
        k.run("-n", ns, "delete", "virtualservice,service,deployment,configmap", "-l", sel,
              "--ignore-not-found", capture=False)
        log(f"deleted {APP} from {ns} (namespace kept)")
        return 0
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
