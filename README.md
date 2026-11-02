# Clang JIT Engine Lab

A hands-on C++ lab for embedding Clang’s incremental compiler and LLVM JIT: create an engine, compile source, call native functions, exchange data with the host, inspect AST and IR, handle object lifetimes, and customize execution. Lessons progress from a small callable function to a complete file-based kernel host.

Start with [the learning path](docs/README.md) and [Part 1](docs/part_1_shell_basics.md). Part 1 begins with concepts, components and API responsibilities, then provides Linux and macOS setup. Reference runs use LLVM 22.1.8.

Read the interactive edition at [clang-jit.senussi.me](https://clang-jit.senussi.me/) or clone [the source repository](https://github.com/husams/clang-jit-lab) to run the examples locally.

```bash
# Follow your platform’s LLVM setup in Part 1 first.
scripts/build.sh
build/bin/p01_engine
python3 scripts/build_site.py
python3 -m http.server 8000 --directory site
```

The Markdown is the source for the interactive HTML edition: dark mode, collapsible navigation, code copying, diagrams, folded hints and progress tracking. Exercises run in native C++ tools.

```bash
scripts/check.sh --quick
scripts/serve.sh 127.0.0.1:8000
```

Track learning in [docs/PROGRESS.md](docs/PROGRESS.md). Authors: [docs/AUTHORING.md](docs/AUTHORING.md). Agents: [CLAUDE.md](CLAUDE.md). Pinned upstream implementation: [reference/README.md](reference/README.md).

Rebuilding local HTML does not publish it. Maintainers with access to the Kubernetes cluster can deploy the HTML edition through its existing Istio gateway:

```bash
python3 scripts/build_site.py
python3 scripts/check_site.py
python3 scripts/deploy.py apply --no-build --alias clang-repl-lab.senussi.me \
  --dns-target 192.168.1.200
```

The deployment serves the lessons and browser progress interface; C++ exercises run on the learner's machine. The previous address, [clang-repl-lab.senussi.me](https://clang-repl-lab.senussi.me/), remains an alias. `--dns-target` asks the cluster's existing ExternalDNS controller to publish both names at the gateway address; without that controller, create DNS records separately. The script writes its rendered manifest to `out/deploy/manifest.json` and waits for the rollout before probing HTTPS. Before an update, save the existing labelled Deployment, Service, VirtualService and ConfigMaps for rollback; the script removes old ConfigMaps after a successful rollout.
