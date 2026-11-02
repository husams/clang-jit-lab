/* Clang JIT Lab - site behaviour. highlight.js is bundled locally. */
(function () {
  'use strict';
  var root = document.documentElement;
  var data = {};
  try { data = JSON.parse(document.getElementById('repllab-data').textContent); } catch (e) { data = { seed: {}, nums: [], page: '' }; }

  /* ---------- storage: localStorage with try/catch, in-memory fallback ---------- */
  var mem = {};
  function lsGet(k, d) {
    try { var v = window.localStorage.getItem(k); if (v === null) return k in mem ? mem[k] : d; return JSON.parse(v); }
    catch (e) { return k in mem ? mem[k] : d; }
  }
  function lsSet(k, v) {
    mem[k] = v;
    try { window.localStorage.setItem(k, JSON.stringify(v)); } catch (e) { /* private mode / file:// quirks */ }
  }
  function lsDel(k) { delete mem[k]; try { window.localStorage.removeItem(k); } catch (e) { /* ignore */ } }

  var $ = function (s, r) { return (r || document).querySelector(s); };
  var $$ = function (s, r) { return Array.prototype.slice.call((r || document).querySelectorAll(s)); };
  var mqMobile = window.matchMedia ? window.matchMedia('(max-width: 900px)') : { matches: false };

  /* ---------- sidebar: collapse to a rail (desktop), drawer (mobile) ---------- */
  var menu = $('#menu'), sbEl = $('#sidebar'), sbBtn = $('#sb-collapse'), railBtn = $('#rail-expand');
  function syncSidebar() { // every control that shows/hides the sidebar reports whether it is showing
    var open = mqMobile.matches ? root.classList.contains('sb-open') : !root.classList.contains('sb-collapsed');
    [menu, sbBtn, railBtn].forEach(function (b) { if (b) b.setAttribute('aria-expanded', open ? 'true' : 'false'); });
  }
  function setCollapsed(c) {
    var had = sbEl && sbEl.contains(document.activeElement); // the control the focus is on is about to hide
    root.classList.toggle('sb-collapsed', c);
    try { window.localStorage.setItem('clangjit.v2.sidebar', c ? 'collapsed' : 'open'); } catch (e) { /* ignore */ }
    syncSidebar();
    if (had && !mqMobile.matches) { var t = c ? railBtn : sbBtn; if (t) t.focus(); }
    if (!c) { activeId = null; spy(); } // the section list may have moved on while it was out of sight
  }
  function closeDrawer() { root.classList.remove('sb-open'); syncSidebar(); }
  function toggleSidebar() {
    if (mqMobile.matches) { root.classList.toggle('sb-open'); syncSidebar(); }
    else setCollapsed(!root.classList.contains('sb-collapsed'));
  }
  if (menu) menu.addEventListener('click', toggleSidebar);
  if (sbBtn) sbBtn.addEventListener('click', function () { setCollapsed(true); });
  if (railBtn) railBtn.addEventListener('click', function () { setCollapsed(false); });
  if (mqMobile.addEventListener) mqMobile.addEventListener('change', syncSidebar);
  syncSidebar();
  var bd = $('#backdrop'); if (bd) bd.addEventListener('click', closeDrawer);
  var sc = $('.sb-close'); if (sc) sc.addEventListener('click', closeDrawer);
  document.addEventListener('keydown', function (e) {
    if (e.key === 'Escape') closeDrawer();
    var tag = (e.target && e.target.tagName) || '', typing = tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT' || !!(e.target && e.target.isContentEditable);
    if (e.key === '/' && tag !== 'INPUT' && tag !== 'TEXTAREA') { e.preventDefault(); var f = $('#filter'); if (f) { if (mqMobile.matches) { root.classList.add('sb-open'); syncSidebar(); } else setCollapsed(false); f.focus(); } }
    if (e.key === '[' && !typing && !e.ctrlKey && !e.metaKey && !e.altKey && !$('.gv-overlay')) { e.preventDefault(); toggleSidebar(); }
  });

  /* ---------- part expand / collapse (persisted) ---------- */
  var openParts = lsGet('clangjit.v2.openParts', null);
  var currentPart = (data.page.match(/^part_(\d+)\.html$/) || [])[1] || null;
  if (!Array.isArray(openParts)) openParts = currentPart ? [currentPart] : [];
  if (currentPart && openParts.indexOf(currentPart) < 0) openParts.push(currentPart);
  function renderParts() {
    $$('.part').forEach(function (p) {
      var n = p.getAttribute('data-part'), o = openParts.indexOf(n) >= 0;
      p.classList.toggle('open', o);
      p.classList.toggle('current', n === currentPart);
      var b = $('.part-toggle', p); if (b) b.setAttribute('aria-expanded', o ? 'true' : 'false');
    });
    // aria-disabled, not disabled: the button the user just pressed keeps focus
    if (expandAll) expandAll.setAttribute('aria-disabled', openParts.length >= $$('.part').length ? 'true' : 'false');
    if (collapseAll) collapseAll.setAttribute('aria-disabled', openParts.length ? 'false' : 'true');
  }
  var expandAll = $('#parts-expand'), collapseAll = $('#parts-collapse');
  $$('.part-toggle').forEach(function (b) {
    b.addEventListener('click', function () {
      var n = b.closest('.part').getAttribute('data-part'), i = openParts.indexOf(n);
      if (i >= 0) openParts.splice(i, 1); else openParts.push(n);
      lsSet('clangjit.v2.openParts', openParts); renderParts();
    });
  });
  if (expandAll) expandAll.addEventListener('click', function () {
    openParts = $$('.part').map(function (p) { return p.getAttribute('data-part'); });
    lsSet('clangjit.v2.openParts', openParts); renderParts();
  });
  if (collapseAll) collapseAll.addEventListener('click', function () { openParts = []; lsSet('clangjit.v2.openParts', openParts); renderParts(); });
  renderParts();
  $$('#tree a').forEach(function (a) { a.addEventListener('click', function () { if (mqMobile.matches) closeDrawer(); }); });

  /* ---------- done checkboxes + progress ---------- */
  var seed = data.seed || {};
  var stored = lsGet('clangjit.v3.done', null);
  if (stored === null) {
    var previous = lsGet('clangjit.v2.done', {});
    stored = {};
    if (previous && typeof previous === 'object') Object.keys(previous).forEach(function (n) {
      var oldFirstPart = /^1\.([1-6])$/.exec(n);
      stored[oldFirstPart ? '1.' + (Number(oldFirstPart[1]) + 3) : n] = previous[n];
    });
    lsSet('clangjit.v3.done', stored);
  }
  if (!stored || typeof stored !== 'object') stored = {};
  function isDone(n) { return n in stored ? !!stored[n] : !!seed[n]; }
  function updateProgress() {
    var nums = data.nums || [], done = 0;
    nums.forEach(function (n) { if (isDone(n)) done++; });
    var pct = nums.length ? Math.round(100 * done / nums.length) : 0;
    var f = $('#overall-fill'); if (f) f.style.width = pct + '%';
    var tf = $('#top-fill'); if (tf) tf.style.width = pct + '%';
    var ot = $('#overall-text'); if (ot) ot.textContent = done + ' / ' + nums.length;
    var tt = $('#top-text'); if (tt) tt.textContent = done + '/' + nums.length;
    $$('#tree li[data-sec]').forEach(function (li) { var n = li.getAttribute('data-sec'); li.classList.toggle('done', !!n && isDone(n)); });
    $$('.pcount').forEach(function (el) {
      var ns = (el.getAttribute('data-nums') || '').split(',').filter(Boolean), d = 0;
      ns.forEach(function (n) { if (isDone(n)) d++; });
      el.textContent = d + '/' + ns.length;
    });
    $$('.rail-part').forEach(function (a) { // ring + tooltip of the collapsed sidebar
      var ns = (a.getAttribute('data-nums') || '').split(',').filter(Boolean), d = 0, nm = a.getAttribute('data-name');
      ns.forEach(function (n) { if (isDone(n)) d++; });
      a.style.setProperty('--p', (ns.length ? Math.round(100 * d / ns.length) : 0) + '%');
      a.title = nm + ' · ' + d + '/' + ns.length + ' done';
      a.setAttribute('aria-label', nm + ', ' + d + ' of ' + ns.length + ' sections done');
    });
    $$('.card[data-nums]').forEach(function (c) {
      var ns = c.getAttribute('data-nums').split(',').filter(Boolean), d = 0;
      ns.forEach(function (n) { if (isDone(n)) d++; });
      var fill = $('.cbar .fill', c) || $('.fill', c); if (fill) fill.style.width = (ns.length ? Math.round(100 * d / ns.length) : 0) + '%';
      var cc = $('.cc', c); if (cc) cc.textContent = d + '/' + ns.length + ' done';
    });
    $$('input[data-sec]').forEach(function (cb) {
      var n = cb.getAttribute('data-sec'); cb.checked = isDone(n);
      var sec = cb.closest('section'); if (sec) sec.classList.toggle('is-done', cb.checked);
    });
  }
  $$('input[data-sec]').forEach(function (cb) {
    cb.addEventListener('change', function () {
      stored[cb.getAttribute('data-sec')] = cb.checked; lsSet('clangjit.v3.done', stored); updateProgress();
    });
  });
  var reset = $('#reset-progress');
  if (reset) reset.addEventListener('click', function () {
    if (!window.confirm('Reset all "done" marks?')) return;
    stored = {}; (data.nums || []).forEach(function (n) { stored[n] = false; });
    lsSet('clangjit.v3.done', stored); updateProgress();
  });
  updateProgress();

  /* ---------- sidebar filter ---------- */
  var filter = $('#filter');
  function runFilter() {
    var q = (filter.value || '').trim().toLowerCase(), terms = q.split(/\s+/).filter(Boolean), any = false;
    $$('.part').forEach(function (p) {
      var ptext = p.getAttribute('data-text') || '', lis = $$('.secs li', p), partHit = false, hits = 0;
      if (!terms.length) { p.classList.remove('hidden'); lis.forEach(function (li) { li.classList.remove('hidden'); }); p.classList.remove('filtered'); return; }
      partHit = terms.every(function (t) { return ptext.indexOf(t) >= 0; });
      lis.forEach(function (li) {
        var hit = partHit || terms.every(function (t) { return (li.getAttribute('data-text') || '').indexOf(t) >= 0; });
        li.classList.toggle('hidden', !hit); if (hit) hits++;
      });
      p.classList.toggle('hidden', !(partHit || hits));
      p.classList.toggle('filtered', true);
      if (partHit || hits) { any = true; p.classList.add('open'); }
    });
    if (!terms.length) { any = true; renderParts(); }
    var nr = $('#no-results'); if (nr) nr.hidden = any;
  }
  if (filter) {
    filter.addEventListener('input', runFilter);
    filter.addEventListener('search', runFilter);
    filter.addEventListener('change', runFilter);
    filter.addEventListener('keyup', runFilter);
    filter.addEventListener('keydown', function (e) { if (e.key === 'Escape') { filter.value = ''; runFilter(); filter.blur(); } });
    window.addEventListener('pageshow', runFilter);
    runFilter();
  }

  /* ---------- scroll spy ---------- */
  var secs = $$('main section.sec');
  var crumb0 = ($('#crumb') || {}).textContent;
  var activeId = null, ticking = false;
  function spy() {
    ticking = false;
    if (!secs.length) return;
    var line = 90, cur = secs[0];
    for (var i = 0; i < secs.length; i++) { if (secs[i].getBoundingClientRect().top <= line) cur = secs[i]; else break; }
    if (window.innerHeight + window.scrollY >= document.body.scrollHeight - 4) cur = secs[secs.length - 1];
    if (cur.id === activeId) return;
    activeId = cur.id;
    $$('#tree li.active').forEach(function (li) { li.classList.remove('active'); });
    var li = $('#tree li[data-id="' + activeId + '"]');
    if (li) {
      li.classList.add('active');
      var sb = $('#sb-full'); // the scroller; nothing to keep in view while it is the rail, a closed drawer, or its part is folded
      if (sb && li.offsetParent !== null && (mqMobile.matches ? root.classList.contains('sb-open') : !root.classList.contains('sb-collapsed'))) {
        var r = li.getBoundingClientRect(), sr = sb.getBoundingClientRect();
        if (r.top < sr.top + 60 || r.bottom > sr.bottom - 20) sb.scrollTop += r.top - sr.top - sr.height / 3;
      }
    }
    var crumb = $('#crumb'), h = $('h2', cur);
    if (crumb && h) crumb.textContent = window.scrollY < 160 ? crumb0 : h.textContent.replace(/^#/, '');
  }
  window.addEventListener('scroll', function () { if (!ticking) { ticking = true; window.requestAnimationFrame(spy); } }, { passive: true });
  window.addEventListener('resize', spy);

  /* ---------- hash targets inside closed <details> ---------- */
  function openForHash() {
    var id = decodeURIComponent((location.hash || '').slice(1));
    if (!id) return;
    var el = document.getElementById(id);
    if (!el) return;
    var changed = false;
    for (var n = el.parentElement; n; n = n.parentElement) { if (n.tagName === 'DETAILS' && !n.open) { n.open = true; changed = true; } }
    if (el.tagName === 'DETAILS' && !el.open) { el.open = true; changed = true; }
    if (changed) el.scrollIntoView();
  }
  window.addEventListener('hashchange', openForHash);

  /* ---------- code blocks: copy + expand ---------- */
  function copyText(text, btn) {
    function ok() { btn.textContent = 'Copied'; btn.classList.add('ok'); setTimeout(function () { btn.textContent = 'Copy'; btn.classList.remove('ok'); }, 1400); }
    function fallback() {
      var ta = document.createElement('textarea'); ta.value = text; ta.style.position = 'fixed'; ta.style.opacity = '0';
      document.body.appendChild(ta); ta.select();
      try { document.execCommand('copy'); ok(); } catch (e) { btn.textContent = 'Press Ctrl+C'; }
      document.body.removeChild(ta);
    }
    if (navigator.clipboard && navigator.clipboard.writeText) navigator.clipboard.writeText(text).then(ok, fallback); else fallback();
  }
  document.addEventListener('click', function (e) {
    var b = e.target.closest ? e.target.closest('button.copy, button.expand') : null;
    if (!b) return;
    var box = b.closest('.code'); if (!box) return;
    if (b.classList.contains('copy')) copyText($('pre code', box).textContent, b);
    else { box.classList.toggle('expanded'); b.textContent = box.classList.contains('expanded') ? 'Collapse' : 'Expand'; }
  });

  /* ---------- diagrams: figure.gv (from ```dot fences; SVG made by Graphviz at build time) ----------
     hover/focus a node or edge to highlight it and its neighbours, click to pin; drag to pan, buttons /
     ctrl+wheel / pinch to zoom, double-click to fit; full-screen overlay; dot source; download as .svg */
  var GV_MIN = 0.3, GV_MAX = 6, GV_STEP = 1.25;
  var GV_SHAPES = { path: 1, polygon: 1, polyline: 1, ellipse: 1, text: 1 };
  var GV_PROPS = ['fill', 'fill-opacity', 'stroke', 'stroke-width', 'stroke-dasharray', 'stroke-linecap', 'stroke-linejoin'];
  var gvOpen = null; // state of the diagram currently shown full screen

  function gvClamp(v, lo, hi) { return Math.max(lo, Math.min(hi, v)); }
  function gvTitle(g) {
    for (var c = g.firstElementChild; c; c = c.nextElementSibling) if (c.localName === 'title') return c;
    return null;
  }
  /* "B1:s" or "ns::f:p1:ne" -> the node it names: drop trailing :port[:compass] until a known node name is left */
  function gvEnd(s, nodes) {
    for (;;) {
      if (s in nodes) return s;
      var i = s.lastIndexOf(':');
      if (i <= 0) return null;
      s = s.slice(0, i);
    }
  }
  /* edge title "tail[:port]->head[:port]" (or "--" in an undirected graph) -> [tail, head] */
  function gvEnds(title, nodes) {
    var seps = ['->', '--'];
    for (var k = 0; k < seps.length; k++) {
      for (var i = title.indexOf(seps[k]); i >= 0; i = title.indexOf(seps[k], i + 1)) {
        var a = gvEnd(title.slice(0, i), nodes), b = gvEnd(title.slice(i + 2), nodes);
        if (a !== null && b !== null) return [a, b];
      }
    }
    return null;
  }
  function gvTarget(el, svg) {
    for (; el && el !== svg; el = el.parentNode) if (el._gv) return el._gv;
    return null;
  }

  /* highlight: the shown target is the pinned one, else the hovered one, else the keyboard-focused one */
  function gvShow(st) {
    var t = st.pinned || st.hov || st.foc || null;
    st.marked.forEach(function (el) { el.classList.remove('gv-focus', 'gv-rel'); });
    st.marked = [];
    st.fig.classList.toggle('is-hl', !!t);
    st.fig.classList.toggle('is-pinned', !!st.pinned);
    Object.keys(st.nodes).forEach(function (k) { st.nodes[k].el.setAttribute('aria-pressed', st.nodes[k] === st.pinned ? 'true' : 'false'); });
    if (!t) return;
    function mark(el, c) { el.classList.add(c); st.marked.push(el); }
    mark(t.el, 'gv-focus');
    if (t.kind === 'node') t.edges.forEach(function (e) { mark(e.el, 'gv-rel'); mark((e.a === t ? e.b : e.a).el, 'gv-rel'); });
    else { mark(t.a.el, 'gv-rel'); mark(t.b.el, 'gv-rel'); }
  }

  /* pan/zoom = the svg viewBox; st.s is the zoom relative to "fit", (st.cx, st.cy) the view centre in viewBox units */
  function gvApply(st) {
    var w = st.bw / st.s, h = st.bh / st.s;
    st.cx = w >= st.bw ? st.bx + st.bw / 2 : gvClamp(st.cx, st.bx, st.bx + st.bw);
    st.cy = h >= st.bh ? st.by + st.bh / 2 : gvClamp(st.cy, st.by, st.by + st.bh);
    st.svg.setAttribute('viewBox', (st.cx - w / 2) + ' ' + (st.cy - h / 2) + ' ' + w + ' ' + h);
    st.view.classList.toggle('is-zoomed', st.s > 1.001);
  }
  function gvPoint(st, x, y) { // client px -> viewBox units (null while not laid out)
    var m = st.svg.getScreenCTM(), pt = st.svg.createSVGPoint();
    if (!m) return null;
    pt.x = x; pt.y = y;
    return pt.matrixTransform(m.inverse());
  }
  function gvZoom(st, f, x, y) { // zoom by factor f, keeping the client point (x, y) still; default: the view centre
    var s = gvClamp(st.s * f, GV_MIN, GV_MAX), r, p;
    if (x === undefined) { r = st.svg.getBoundingClientRect(); x = r.left + r.width / 2; y = r.top + r.height / 2; }
    p = gvPoint(st, x, y);
    if (p) { st.cx = p.x - (p.x - st.cx) * st.s / s; st.cy = p.y - (p.y - st.cy) * st.s / s; }
    st.s = s;
    gvApply(st);
  }
  function gvPan(st, dx, dy) { // client px
    var m = st.svg.getScreenCTM();
    if (!m) return;
    st.cx -= dx / m.a; st.cy -= dy / m.d;
    gvApply(st);
  }
  function gvFit(st) { st.s = 1; st.cx = st.bx + st.bw / 2; st.cy = st.by + st.bh / 2; gvApply(st); }
  function gvReveal(st, g) { // keyboard focus on a node that panning/zooming moved out of view
    var r = g.getBoundingClientRect(), v = st.svg.getBoundingClientRect(), dx = 0, dy = 0;
    if (r.left < v.left) dx = v.left - r.left + 12; else if (r.right > v.right) dx = v.right - r.right - 12;
    if (r.top < v.top) dy = v.top - r.top + 12; else if (r.bottom > v.bottom) dy = v.bottom - r.bottom - 12;
    if (dx || dy) gvPan(st, dx, dy);
  }

  function gvExpand(st) {
    if (gvOpen) return;
    var fig = st.fig, ph = document.createElement('div'), ov = document.createElement('div'), b = $('[data-act="expand"]', fig), down = false;
    ph.className = 'gv-ph'; ph.style.height = fig.offsetHeight + 'px'; // the page behind must not jump
    fig.parentNode.insertBefore(ph, fig);
    ov.className = 'gv-overlay'; ov.setAttribute('role', 'dialog'); ov.setAttribute('aria-modal', 'true'); ov.setAttribute('aria-label', 'Diagram, full screen');
    fig.classList.add('is-full');
    ov.appendChild(fig); document.body.appendChild(ov);
    root.classList.add('gv-lock');
    $$('.topbar, .layout').forEach(function (el) { el.setAttribute('inert', ''); });
    b.setAttribute('aria-label', 'Close full screen'); b.title = 'Close full screen';
    ov.addEventListener('pointerdown', function (e) { down = e.target === ov; });
    ov.addEventListener('click', function (e) { if (down && e.target === ov) gvCollapse(); down = false; });
    ov.addEventListener('keydown', function (e) { // keep Tab inside the dialog
      if (e.key !== 'Tab') return;
      var f = $$('button, [tabindex="0"]', ov).filter(function (el) { return el.offsetParent !== null; }); // SVG nodes report undefined: kept
      if (!f.length) return;
      if (e.shiftKey && document.activeElement === f[0]) { e.preventDefault(); f[f.length - 1].focus(); }
      else if (!e.shiftKey && document.activeElement === f[f.length - 1]) { e.preventDefault(); f[0].focus(); }
    });
    st.ph = ph; st.ov = ov; st.hov = null; gvOpen = st;
    gvShow(st);
    b.focus();
  }
  function gvCollapse() {
    var st = gvOpen, b;
    if (!st) return;
    gvOpen = null;
    b = $('[data-act="expand"]', st.fig);
    st.ph.parentNode.insertBefore(st.fig, st.ph);
    st.ph.parentNode.removeChild(st.ph);
    st.ov.parentNode.removeChild(st.ov);
    st.fig.classList.remove('is-full');
    root.classList.remove('gv-lock');
    $$('.topbar, .layout').forEach(function (el) { el.removeAttribute('inert'); });
    b.setAttribute('aria-label', 'Expand to full screen'); b.title = 'Expand to full screen';
    st.hov = null; gvShow(st);
    b.focus();
  }

  function gvDownload(st) { // the SVG as rendered now (theme colours, fonts) with the styles inlined, on the page background
    var svg = st.svg, fig = st.fig, live, copy, i, el, cs, clone, bg;
    gvMarkOff(st);
    clone = svg.cloneNode(true);
    live = svg.querySelectorAll('*'); copy = clone.querySelectorAll('*');
    for (i = 0; i < live.length; i++) {
      el = copy[i]; cs = window.getComputedStyle(live[i]);
      if (GV_SHAPES[live[i].localName]) {
        GV_PROPS.forEach(function (p) { el.setAttribute(p, cs.getPropertyValue(p)); });
        if (live[i].localName === 'text') ['font-family', 'font-size', 'font-weight', 'paint-order'].forEach(function (p) { el.setAttribute(p, cs.getPropertyValue(p)); });
      } else if (live[i].localName === 'g' && cs.opacity !== '1') el.setAttribute('opacity', cs.opacity);
    }
    $$('.gv-hit', clone).forEach(function (h) { h.parentNode.removeChild(h); });
    $$('[tabindex], [role], [aria-pressed], [aria-label]', clone).forEach(function (n) { ['tabindex', 'role', 'aria-pressed', 'aria-label'].forEach(function (a) { n.removeAttribute(a); }); });
    ['style', 'focusable', 'tabindex'].forEach(function (a) { clone.removeAttribute(a); });
    clone.setAttribute('viewBox', [st.bx, st.by, st.bw, st.bh].join(' '));
    clone.setAttribute('width', st.bw + 'pt'); clone.setAttribute('height', st.bh + 'pt');
    bg = document.createElementNS('http://www.w3.org/2000/svg', 'rect');
    ['x', 'y', 'width', 'height'].forEach(function (a, k) { bg.setAttribute(a, [st.bx, st.by, st.bw, st.bh][k]); });
    bg.setAttribute('fill', window.getComputedStyle(fig).backgroundColor);
    clone.insertBefore(bg, clone.firstChild);
    gvShow(st);
    var url = URL.createObjectURL(new Blob(['<?xml version="1.0" encoding="UTF-8"?>\n' + new XMLSerializer().serializeToString(clone) + '\n'], { type: 'image/svg+xml;charset=utf-8' }));
    var a = document.createElement('a');
    a.href = url; a.download = (fig.getAttribute('data-name') || 'diagram') + '.svg';
    document.body.appendChild(a); a.click(); document.body.removeChild(a);
    setTimeout(function () { URL.revokeObjectURL(url); }, 2000);
  }
  function gvMarkOff(st) { // drop hover/pin emphasis (classes only; gvShow restores it)
    st.marked.forEach(function (el) { el.classList.remove('gv-focus', 'gv-rel'); });
    st.fig.classList.remove('is-hl', 'is-pinned');
  }

  function gvInit(fig) {
    var view = $('.gv-view', fig), svg = $('svg.gv-svg', fig), bar = $('.gv-bar', fig);
    var vb = svg ? (svg.getAttribute('viewBox') || '').split(/\s+/).map(parseFloat) : [];
    if (!view || vb.length !== 4 || vb.some(isNaN)) return;
    var st = { fig: fig, view: view, svg: svg, bx: vb[0], by: vb[1], bw: vb[2], bh: vb[3], s: 1, cx: vb[0] + vb[2] / 2, cy: vb[1] + vb[3] / 2,
               nodes: Object.create(null), edges: [], marked: [], pinned: null, hov: null, foc: null, moved: false };

    /* model: node name <- <title>, edge ends <- "tail->head"; the titles go (they would pop up as native tooltips) */
    $$('g.node', svg).forEach(function (g) {
      var t = gvTitle(g), n;
      if (!t) return;
      n = { kind: 'node', el: g, name: t.textContent, edges: [] };
      st.nodes[n.name] = n; g._gv = n;
      g.setAttribute('tabindex', '0'); g.setAttribute('role', 'button'); g.setAttribute('aria-pressed', 'false');
      g.setAttribute('aria-label', $$('text', g).map(function (x) { return x.textContent; }).join(' ') || n.name);
    });
    $$('g.edge', svg).forEach(function (g) {
      var t = gvTitle(g), ends = t ? gvEnds(t.textContent, st.nodes) : null, e, path, hit;
      if (!ends) return;
      e = { kind: 'edge', el: g, a: st.nodes[ends[0]], b: st.nodes[ends[1]] };
      g._gv = e; st.edges.push(e);
      e.a.edges.push(e); if (e.b !== e.a) e.b.edges.push(e);
      path = $('path', g);
      if (path) { // a wide invisible stroke makes thin edges easy to hit
        hit = path.cloneNode(false); hit.setAttribute('class', 'gv-hit'); hit.removeAttribute('stroke-dasharray');
        g.insertBefore(hit, path);
      }
    });
    $$('g.node > title, g.edge > title, g.cluster > title, g.graph > title', svg).forEach(function (t) { t.parentNode.removeChild(t); });

    svg.addEventListener('mouseover', function (e) { var t = gvTarget(e.target, svg); if (t !== st.hov) { st.hov = t; gvShow(st); } });
    view.addEventListener('mouseleave', function () { st.hov = null; gvShow(st); });
    svg.addEventListener('focusin', function (e) {
      var t = gvTarget(e.target, svg), vis = true;
      try { vis = e.target.matches(':focus-visible'); } catch (x) { /* old browser: treat every focus as keyboard */ }
      if (!t || !vis) return; // a mouse click focuses the node too; only keyboard focus highlights
      st.foc = t; gvShow(st); gvReveal(st, t.el);
    });
    svg.addEventListener('focusout', function () { st.foc = null; gvShow(st); });
    svg.addEventListener('keydown', function (e) {
      var t = e.key === 'Enter' || e.key === ' ' ? gvTarget(e.target, svg) : null;
      if (!t) return;
      e.preventDefault(); st.pinned = st.pinned === t ? null : t; gvShow(st);
    });
    view.addEventListener('click', function (e) { // click pins; click it again, or the background, to clear
      var t = gvTarget(e.target, svg);
      if (st.moved) { st.moved = false; return; } // the click that ends a drag
      st.pinned = t && t !== st.pinned ? t : null; gvShow(st);
    });

    /* drag to pan (mouse, pen, touch), two fingers to pinch-zoom, ctrl/cmd + wheel (and trackpad pinch) to zoom, double-click to fit */
    var ptrs = {}, drag = null, pinch = null;
    function two() { var k = Object.keys(ptrs); return k.length === 2 ? [ptrs[k[0]], ptrs[k[1]]] : null; }
    view.addEventListener('pointerdown', function (e) {
      var k, q;
      if (e.pointerType === 'mouse' && e.button !== 0) return;
      ptrs[e.pointerId] = { x: e.clientX, y: e.clientY };
      k = Object.keys(ptrs);
      if (k.length === 1) { drag = { id: e.pointerId, x: e.clientX, y: e.clientY, lx: e.clientX, ly: e.clientY, on: false }; st.moved = false; }
      else if ((q = two())) pinch = { d: Math.hypot(q[0].x - q[1].x, q[0].y - q[1].y) || 1, s: st.s, mx: (q[0].x + q[1].x) / 2, my: (q[0].y + q[1].y) / 2 };
    });
    view.addEventListener('pointermove', function (e) {
      var p = ptrs[e.pointerId], q, mx, my;
      if (!p) return;
      p.x = e.clientX; p.y = e.clientY;
      if (pinch && (q = two())) {
        mx = (q[0].x + q[1].x) / 2; my = (q[0].y + q[1].y) / 2; st.moved = true;
        gvZoom(st, pinch.s * (Math.hypot(q[0].x - q[1].x, q[0].y - q[1].y) / pinch.d) / st.s, mx, my);
        gvPan(st, mx - pinch.mx, my - pinch.my); pinch.mx = mx; pinch.my = my;
      } else if (drag && drag.id === e.pointerId) {
        if (!drag.on) {
          if (Math.abs(e.clientX - drag.x) + Math.abs(e.clientY - drag.y) < 4) return;
          drag.on = true; st.moved = true;
          try { view.setPointerCapture(e.pointerId); } catch (x) { /* ignore */ }
          if (st.s > 1.001) view.classList.add('is-panning');
        }
        if (st.s > 1.001) gvPan(st, e.clientX - drag.lx, e.clientY - drag.ly);
        drag.lx = e.clientX; drag.ly = e.clientY;
      }
    });
    function release(e) {
      var k, q;
      delete ptrs[e.pointerId];
      k = Object.keys(ptrs);
      view.classList.remove('is-panning');
      if (k.length < 2) pinch = null;
      if (!k.length) drag = null;
      else if ((q = ptrs[k[0]]) && k.length === 1) drag = { id: Number(k[0]), x: q.x, y: q.y, lx: q.x, ly: q.y, on: true }; // carry on with the remaining finger
    }
    view.addEventListener('pointerup', release);
    view.addEventListener('pointercancel', release);
    view.addEventListener('wheel', function (e) {
      var dy;
      if (!(e.ctrlKey || e.metaKey)) return; // a plain wheel keeps scrolling the page
      e.preventDefault();
      dy = e.deltaMode === 1 ? e.deltaY * 16 : e.deltaMode === 2 ? e.deltaY * 100 : e.deltaY;
      gvZoom(st, gvClamp(Math.exp(-dy * 0.01), 0.67, 1.5), e.clientX, e.clientY);
    }, { passive: false });
    var gs = 1; // Safari reports a trackpad pinch as gesture events instead of ctrl+wheel
    view.addEventListener('gesturestart', function (e) { e.preventDefault(); gs = 1; });
    view.addEventListener('gesturechange', function (e) { e.preventDefault(); gvZoom(st, e.scale / gs, e.clientX, e.clientY); gs = e.scale; });
    view.addEventListener('dblclick', function () { gvFit(st); });

    /* toolbar */
    bar.addEventListener('click', function (e) {
      var b = e.target.closest ? e.target.closest('button[data-act]') : null, src, on;
      if (!b) return;
      switch (b.getAttribute('data-act')) {
        case 'zoom-in': gvZoom(st, GV_STEP); break;
        case 'zoom-out': gvZoom(st, 1 / GV_STEP); break;
        case 'fit': gvFit(st); break;
        case 'expand': if (gvOpen === st) gvCollapse(); else gvExpand(st); break;
        case 'download': gvDownload(st); break;
        case 'source':
          src = $('.gv-src', fig); on = src.hidden; src.hidden = !on;
          b.setAttribute('aria-pressed', on ? 'true' : 'false'); b.title = on ? 'Hide dot source' : 'Show dot source';
          break;
      }
    });
  }
  $$('figure.gv').forEach(gvInit);
  document.addEventListener('keydown', function (e) { if (e.key === 'Escape' && gvOpen) { e.preventDefault(); gvCollapse(); } });

  /* graph views (figure.gv-derived: a text output drawn as a graph): Graph / Output tabs; the last choice is
     remembered for every graph view on every page (localStorage repllab.outview) */
  var gvViews = $$('figure.gv-derived');
  function gvPick(view, remember) {
    gvViews.forEach(function (fig) {
      fig.setAttribute('data-view', view);
      $$('.gv-tab', fig).forEach(function (t) {
        var on = t.getAttribute('data-view') === view, panel = document.getElementById(t.getAttribute('aria-controls'));
        t.setAttribute('aria-selected', on ? 'true' : 'false');
        t.tabIndex = on ? 0 : -1;
        if (panel) panel.hidden = !on;
      });
    });
    if (remember) { try { window.localStorage.setItem('clangjit.v2.outview', view); } catch (e) { /* ignore */ } }
  }
  gvViews.forEach(function (fig) {
    var tabs = $('.gv-tabs', fig);
    tabs.addEventListener('click', function (e) {
      var t = e.target.closest ? e.target.closest('.gv-tab') : null;
      if (t) gvPick(t.getAttribute('data-view'), true);
    });
    tabs.addEventListener('keydown', function (e) { // arrow keys / Home / End move between the tabs
      var all = $$('.gv-tab', tabs), i = all.indexOf(document.activeElement), j;
      if (i < 0 || ['ArrowLeft', 'ArrowRight', 'Home', 'End'].indexOf(e.key) < 0) return;
      e.preventDefault();
      j = e.key === 'Home' ? 0 : e.key === 'End' ? all.length - 1 : (i + (e.key === 'ArrowRight' ? 1 : -1) + all.length) % all.length;
      all[j].focus(); gvPick(all[j].getAttribute('data-view'), true);
    });
  });
  try { if (gvViews.length && window.localStorage.getItem('clangjit.v2.outview') === 'output') gvPick('output', false); } catch (e) { /* ignore */ }

  /* ---------- local syntax highlighting ---------- */
  function highlightJitSource(root, depth) {
    $$('span.hljs-string', root).forEach(function (literal) {
      if (literal.dataset.jitHighlighted) return;
      var match = literal.textContent.match(/^R"(cpp\w*|definition|function)\(([\s\S]*)\)\1"$/);
      if (!match || match[2].indexOf('\n') < 0) return;
      var open = document.createElement('span'), source = document.createElement('span'), close = document.createElement('span');
      open.className = close.className = 'jit-delimiter';
      open.textContent = 'R"' + match[1] + '(';
      close.textContent = ')' + match[1] + '"';
      source.className = 'jit-source';
      source.innerHTML = window.hljs.highlight(match[2], { language: 'cpp', ignoreIllegals: true }).value;
      literal.replaceChildren(open, source, close);
      literal.dataset.jitHighlighted = 'yes';
      if (depth < 3) highlightJitSource(source, depth + 1);
    });
  }
  function highlight() {
    if (!window.hljs) return;
    try {
      $$('pre code[class*="language-"]').forEach(function (c) {
        var language = (c.className.match(/\blanguage-([\w+-]+)/) || [])[1];
        if (!c.dataset.highlighted && language && window.hljs.getLanguage(language)) {
          window.hljs.highlightElement(c);
          if (language === 'cpp') highlightJitSource(c, 0);
        }
      });
    } catch (e) { /* leave plain */ }
  }
  highlight(); // Both scripts are deferred: the document and local grammars are ready.

  /* ---------- init ---------- */
  openForHash();
  spy();
})();
