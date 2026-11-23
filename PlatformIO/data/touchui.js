/* touchui.js - shared phone touch handling for every Forbes Automotive web UI.
 *
 * MASTER COPY: <Drive>:\My Drive\PlatformIO\_SharedTheme\touchui.js (+ touchui.css)
 * Edit it there, then copy both files unchanged into each product's data/ (same as ota.js).
 * Based on OpenHaldex-C6 V11.00.12 (Expert PT cells + draggable charts) + V11.00.14 (full-screen chart: sideways, pinch zoom). See README_TOUCHUI.md.
 *
 * Load before app.js:
 *   <link rel="stylesheet" href="touchui.css?v=<ver>">
 *   <script src="touchui.js?v=<ver>"></script>
 *
 * Globals:
 *   initTouchGuards()       - runs once by itself when this file loads (calling it again does nothing)
 *   buzz()                  - short vibration where the phone supports it
 *   attachLongPress(el, holdMs, onTap, onLong)
 *   chartLayer(host)        - the child an SVG chart is drawn into, so the drag overlay survives redraws
 *   showDragBubble(x, y, text) / hideDragBubble()
 *   attachCurveDrag(host, opts)
 *   TOUCHUI_HOLD_MS         - default hold before a chart point can be dragged (450 ms)
 */
(function () {
  'use strict';

  var HOLD_MS = 450;
  window.TOUCHUI_HOLD_MS = HOLD_MS;

  // Text can still be selected (and the long-press menu still appears) here.
  var ALLOW_TEXT = 'input:not([type="range"]):not([type="checkbox"]):not([type="radio"])' +
    ':not([type="button"]):not([type="submit"]):not([type="reset"]),' +
    'textarea, select, pre, code, [contenteditable]:not([contenteditable="false"]), .allow-select';

  // ===== 1. Touch guards =====
  // a) A scroll never moves a slider: a range only changes on a sideways drag
  //    (> 10 px and > 1.5x the vertical move). Otherwise its input / change events
  //    are stopped before the page sees them and the value is put back.
  //    touchui.css gives every range touch-action: pan-y.
  // b) No long-press copy / context menu: pages use long presses for real actions.
  //    Text boxes, select, pre, code and .allow-select keep it. The no-select /
  //    no-callout CSS is in touchui.css.
  function initTouchGuards() {
    if (window.__touchGuardsOn) return;
    window.__touchGuardsOn = true;

    var guard = null; // { el, x, y, value, drag }
    var rangeAt = function (target) {
      return target && target.closest ? target.closest('input[type="range"]') : null;
    };
    document.addEventListener('touchstart', function (event) {
      var el = rangeAt(event.target);
      if (!el || el.disabled) return;
      var touch = event.touches[0];
      guard = { el: el, x: touch.clientX, y: touch.clientY, value: el.value, drag: false };
    }, { passive: true, capture: true });
    document.addEventListener('touchmove', function (event) {
      if (!guard || guard.drag) return;
      var touch = event.touches[0];
      var dx = Math.abs(touch.clientX - guard.x);
      var dy = Math.abs(touch.clientY - guard.y);
      if (dx > 10 && dx > dy * 1.5) guard.drag = true;
    }, { passive: true, capture: true });
    var hold = function (event) {
      if (!guard || guard.drag || event.target !== guard.el) return;
      event.stopImmediatePropagation(); // no display update, no request to the unit
      guard.el.value = guard.value;
    };
    document.addEventListener('input', hold, true);
    document.addEventListener('change', hold, true);
    var end = function () {
      var ended = guard;
      if (!ended) return;
      if (!ended.drag) ended.el.value = ended.value;
      setTimeout(function () { if (guard === ended) guard = null; }, 100); // change comes after touchend
    };
    document.addEventListener('touchend', end, true);
    document.addEventListener('touchcancel', end, true);

    document.addEventListener('contextmenu', function (event) {
      var t = event.target;
      if (t && t.closest && t.closest(ALLOW_TEXT)) return;
      event.preventDefault();
    });
  }

  function buzz() {
    try { if (navigator.vibrate) navigator.vibrate(30); } catch (e) { /* not supported */ }
  }

  // ===== 2. Long press =====
  // Tap / long press on one element. Moving more than 10 px before the hold
  // completes is a scroll: nothing happens. The element darkens while held
  // (.hold-press / .pressing in touchui.css, timed by --hold-ms).
  function attachLongPress(el, holdMs, onTap, onLong) {
    var timer = null;
    var startX = 0;
    var startY = 0;
    var fired = false;
    el.classList.add('hold-press');
    var stop = function () {
      if (timer) { clearTimeout(timer); timer = null; }
      el.classList.remove('pressing');
    };
    el.addEventListener('pointerdown', function (event) {
      if (event.pointerType === 'mouse' && event.button !== 0) return;
      fired = false;
      startX = event.clientX;
      startY = event.clientY;
      el.style.setProperty('--hold-ms', holdMs + 'ms');
      el.classList.add('pressing');
      timer = setTimeout(function () {
        timer = null;
        el.classList.remove('pressing');
        fired = true;
        onLong();
      }, holdMs);
    });
    el.addEventListener('pointermove', function (event) {
      if (timer && Math.hypot(event.clientX - startX, event.clientY - startY) > 10) stop();
    });
    ['pointerup', 'pointercancel', 'pointerleave'].forEach(function (name) { el.addEventListener(name, stop); });
    el.addEventListener('contextmenu', function (event) { event.preventDefault(); }); // Android: no menu / cancel mid-hold
    el.addEventListener('click', function (event) {
      if (fired) { // the long press already acted: this click is its tail
        fired = false;
        event.preventDefault();
        event.stopPropagation();
        return;
      }
      if (onTap) onTap();
    });
  }

  // ===== 3. Draggable charts =====
  // An SVG chart draws into this child, so the touch overlay attachCurveDrag puts
  // over the chart survives every redraw (a touch whose target is removed from
  // the page stops reaching its listeners). A canvas chart doesn't need it.
  function chartLayer(host) {
    var layer = host.querySelector(':scope > .chart-svg');
    if (!layer) {
      layer = document.createElement('div');
      layer.className = 'chart-svg';
      host.prepend(layer);
    }
    return layer;
  }

  var dragBubble = null;
  function showDragBubble(x, y, text) {
    if (!dragBubble) {
      dragBubble = document.createElement('div');
      dragBubble.className = 'drag-bubble';
      document.body.appendChild(dragBubble);
    }
    dragBubble.textContent = text;
    dragBubble.style.left = x + 'px';
    dragBubble.style.top = y + 'px';
    dragBubble.hidden = false;
  }
  function hideDragBubble() { if (dragBubble) dragBubble.hidden = true; }

  // Hold a point on a chart, slide up / down to change it, release to set it.
  // Sliding off the chart cancels (the point snaps back). Until the hold completes
  // a touch is left alone, so an up / down swipe over the chart scrolls the page;
  // once it has, touchmove is cancelled so the page stays put while you drag.
  //
  // The chart is the first <svg> or <canvas> in host. Coordinates are in its own
  // units: SVG viewBox units, or canvas.width / canvas.height pixels (set
  // opts.cssUnits for a canvas drawn in CSS pixels, e.g. one scaled with
  // ctx.setTransform(devicePixelRatio...)). An SVG that keeps its shape
  // (preserveAspectRatio, the default) is letterboxed when its box isn't the
  // viewBox's shape - full screen does that - so the drawing's real place on
  // screen is worked out from the viewBox. A canvas fills its box (object-fit:
  // contain is letterboxed the same way as an SVG).
  //
  // Full screen (OpenHaldex V11.00.12, rotate + zoom V11.00.14): a button in the
  // chart's corner opens it over the whole window - a bigger chart = finer steps,
  // easier picking. On a phone it also asks the browser for real full screen locked
  // sideways (Android). Where that isn't possible (iPhone, rotation lock) and the
  // phone is still upright, the chart is drawn turned 90° (chart-rotated) - turn
  // the phone to the left to read it. In full screen two fingers pinch / pan and
  // one finger on empty space pans once zoomed; the points work the same.
  // Escape or the button closes it. On by default for an SVG chart (the viewBox
  // scales it). A canvas chart has to redraw at the new size itself, so it is off
  // unless opts.expand is true - then redraw in opts.onExpand (also called when
  // the screen turns or resizes while it is open).
  // opts:
  //   pick(x, y, radius) -> node {value, locked} or null   (required)
  //   unitsPerStep(node) -> chart units the finger moves for one step of value (required;
  //                         unitsPerPct is accepted as the old name)
  //   show(node, value, active) -> put value in the model and redraw   (required)
  //   done(node)         -> after a release that set a value (save, table, unsaved)
  //   label(node, from, to) -> bubble text
  //   min, max, step     -> value limits / rounding (default 0, 100, 1); a number or fn(node)
  //   holdMs             -> hold time (default TOUCHUI_HOLD_MS)
  //   lockedText(node)   -> message for a locked point (shown with the page's showNotification)
  //   onLocked(node)     -> or handle a locked point yourself
  //   cssUnits           -> canvas only: coordinates in CSS pixels
  //   expand             -> full-screen button (default: true for SVG, false for canvas)
  //   onExpand(on)       -> after opening / closing / re-laying out full screen (a canvas chart redraws here)
  function attachCurveDrag(host, opts) {
    var per = opts.unitsPerStep || opts.unitsPerPct;
    var holdMs = opts.holdMs || HOLD_MS;
    var opt = function (v, node, dflt) { return typeof v === 'function' ? v(node) : (v === undefined ? dflt : v); };

    host.classList.add('chart-drag');
    var overlay = document.createElement('div');
    overlay.className = 'chart-touch';
    overlay.addEventListener('contextmenu', function (event) { event.preventDefault(); }); // a long hold is ours
    host.appendChild(overlay);

    var grab = null; // { node, x, y, lx, ly, k, timer, armed, cancelled, from, value }
    var frame = 0;
    var lastTouchMs = 0;

    // Turned 90° (chart-rotated): touches arrive in screen coordinates, so the
    // maths below works in the chart's own ("local") frame - its top-left corner,
    // its x across, its y down.
    var rotated = function () { return host.classList.contains('chart-rotated'); };
    var toLocal = function (x, y) { return rotated() ? { x: y, y: window.innerWidth - x } : { x: x, y: y }; };
    var rectToLocal = function (r) {
      return rotated()
        ? { left: r.top, top: window.innerWidth - r.right, width: r.height, height: r.width }
        : { left: r.left, top: r.top, width: r.width, height: r.height };
    };
    var surface = function () { return host.querySelector('svg, canvas'); };
    // what a pinch zoom scales: the SVG's layer (chartLayer) or the canvas itself
    var zoomTarget = function () { return host.querySelector(':scope > .chart-svg') || host.querySelector(':scope > canvas'); };

    // Where the drawing really is (local coordinates), and chart units per pixel
    // across (kx) and down (ky). A pinch zoom is already in the rect.
    var surfaceBox = function () {
      var el = surface();
      if (!el) return null;
      var rect = rectToLocal(el.getBoundingClientRect());
      if (!rect.width || !rect.height) return null;
      var w, h, contain;
      if (el.tagName.toLowerCase() === 'svg') {
        var vb = el.viewBox && el.viewBox.baseVal;
        w = vb && vb.width ? vb.width : rect.width;
        h = vb && vb.height ? vb.height : rect.height;
        contain = !/^\s*none/.test(el.getAttribute('preserveAspectRatio') || '');
      } else if (opts.cssUnits) {
        w = el.clientWidth || rect.width; // unzoomed CSS px
        h = el.clientHeight || rect.height;
        contain = false;
      } else {
        w = el.width || rect.width;
        h = el.height || rect.height;
        var fit = window.getComputedStyle(el).objectFit;
        contain = fit === 'contain' || fit === 'scale-down';
      }
      var sx = rect.width / w; // screen px per chart unit
      var sy = rect.height / h;
      if (contain) sx = sy = Math.min(sx, sy);
      var left = rect.left + (rect.width - w * sx) / 2;
      var top = rect.top + (rect.height - h * sy) / 2;
      return { left: left, top: top, right: left + w * sx, bottom: top + h * sy, kx: 1 / sx, ky: 1 / sy };
    };
    var offChart = function (x, y) { // local coordinates
      var box = surfaceBox();
      if (!box) return true;
      var margin = 10;
      // zoomed in, the drawing runs past the screen: the screen edge is the limit
      var vis = rectToLocal(host.getBoundingClientRect());
      var left = Math.max(box.left, vis.left), right = Math.min(box.right, vis.left + vis.width);
      var top = Math.max(box.top, vis.top), bottom = Math.min(box.bottom, vis.top + vis.height);
      return x < left - margin || x > right + margin || y < top - margin || y > bottom + margin;
    };

    // ----- full screen, sideways, pinch zoom -----
    var isPhone = function () { return !!(window.matchMedia && window.matchMedia('(pointer: coarse)').matches); };
    var expanded = function () { return host.classList.contains('chart-expanded'); };
    var view = { z: 1, tx: 0, ty: 0 }; // pinch zoom of the drawing (full screen only)
    var gesture = null;                // { d0, z0, cx, cy } while pinching / panning
    var ZOOM_MAX = 4;
    var applyView = function () {
      var target = zoomTarget();
      if (!target) return;
      target.style.transformOrigin = view.z === 1 ? '' : '0 0';
      target.style.transform = view.z === 1 ? '' : 'translate(' + view.tx + 'px, ' + view.ty + 'px) scale(' + view.z + ')';
    };
    var resetView = function () { view = { z: 1, tx: 0, ty: 0 }; gesture = null; applyView(); };
    var layout = function (notify) {
      var rot = expanded() && isPhone() && window.innerHeight > window.innerWidth;
      host.classList.toggle('chart-rotated', rot);
      document.body.classList.toggle('chart-rotated', rot);
      host.style.width = rot ? window.innerHeight + 'px' : '';
      host.style.height = rot ? window.innerWidth + 'px' : '';
      host.style.left = rot ? window.innerWidth + 'px' : '';
      resetView();
      if (notify && opts.onExpand) opts.onExpand(expanded());
    };

    if (opts.expand === undefined ? !!host.querySelector('svg') || !host.querySelector('canvas') : opts.expand) {
      var expandBtn = document.createElement('button');
      expandBtn.type = 'button';
      expandBtn.className = 'chart-expand';
      host.appendChild(expandBtn);

      var openedFs = false; // this chart asked for browser full screen
      var enterFullscreen = function () {
        var el = document.documentElement;
        var req = el.requestFullscreen || el.webkitRequestFullscreen;
        if (!isPhone() || !req || document.fullscreenElement || document.webkitFullscreenElement) return;
        var locked = function () {
          openedFs = true;
          var lock = null;
          try { if (screen.orientation && screen.orientation.lock) lock = screen.orientation.lock('landscape'); } catch (e) { /* none */ }
          if (lock && lock.then) lock.then(function () { layout(true); }, function () { layout(true); }); // no lock: CSS turns it
          else layout(true);
        };
        try {
          var p = req.call(el, { navigationUI: 'hide' }); // must start inside the tap
          if (p && p.then) p.then(locked, function () { /* not allowed: the CSS rotation still applies */ });
          else locked();
        } catch (e) { /* not allowed */ }
      };
      var leaveFullscreen = function () {
        try { if (screen.orientation && screen.orientation.unlock) screen.orientation.unlock(); } catch (e) { /* none */ }
        if (!openedFs) return;
        openedFs = false;
        var exit = document.exitFullscreen || document.webkitExitFullscreen;
        if (exit && (document.fullscreenElement || document.webkitFullscreenElement)) {
          try { var p = exit.call(document); if (p && p.catch) p.catch(function () {}); } catch (e) { /* already out */ }
        }
      };
      var setExpanded = function (on, notify) {
        host.classList.toggle('chart-expanded', on);
        document.body.classList.toggle('chart-open', on);
        expandBtn.textContent = on ? '✕' : '⤢'; // ✕ / ⤢
        expandBtn.setAttribute('aria-label', on ? 'Close full screen' : 'Full screen');
        if (!on) leaveFullscreen();
        layout(notify);
      };
      setExpanded(false, false);
      expandBtn.addEventListener('click', function () {
        var on = !expanded();
        setExpanded(on, true);
        if (on) enterFullscreen();
      });
      document.addEventListener('keydown', function (event) {
        if (event.key === 'Escape' && expanded()) setExpanded(false, true);
      });
      // Android back / swipe out of browser full screen: close the chart too
      ['fullscreenchange', 'webkitfullscreenchange'].forEach(function (name) {
        document.addEventListener(name, function () {
          if (!document.fullscreenElement && !document.webkitFullscreenElement && openedFs) {
            openedFs = false;
            if (expanded()) setExpanded(false, true);
          } else if (expanded()) layout(true);
        });
      });
      window.addEventListener('resize', function () { if (expanded()) layout(true); });
      window.addEventListener('orientationchange', function () { if (expanded()) setTimeout(function () { layout(true); }, 250); });
    }

    // Pinch / pan: keep the drawing point that was under the fingers under them.
    // Local coordinates; the zoomed element's own top-left (inside the padding) is its origin.
    var layerOrigin = function () {
      var t = zoomTarget();
      return t ? { x: t.offsetLeft, y: t.offsetTop, w: t.offsetWidth, h: t.offsetHeight } : null;
    };
    var touchesLocal = function (touches) {
      return Array.prototype.slice.call(touches, 0, 2).map(function (t) { return toLocal(t.clientX, t.clientY); });
    };
    var mid = function (pts) { return pts.length > 1 ? { x: (pts[0].x + pts[1].x) / 2, y: (pts[0].y + pts[1].y) / 2 } : pts[0]; };
    var gestureStart = function (pts) {
      var o = layerOrigin();
      if (!o || !pts.length) return;
      var m = mid(pts);
      gesture = {
        d0: pts.length > 1 ? Math.hypot(pts[0].x - pts[1].x, pts[0].y - pts[1].y) : 0,
        z0: view.z,
        cx: (m.x - o.x - view.tx) / view.z, // drawing point under the fingers (unzoomed px)
        cy: (m.y - o.y - view.ty) / view.z,
      };
    };
    var gestureMove = function (pts) {
      var o = layerOrigin();
      if (!gesture || !o || !pts.length) return;
      var m = mid(pts);
      var z = gesture.z0;
      if (gesture.d0 > 20 && pts.length > 1) z = gesture.z0 * Math.hypot(pts[0].x - pts[1].x, pts[0].y - pts[1].y) / gesture.d0;
      z = Math.max(1, Math.min(ZOOM_MAX, z));
      // never pull the drawing's edge inside the screen
      var tx = Math.min(0, Math.max(o.w * (1 - z), m.x - o.x - z * gesture.cx));
      var ty = Math.min(0, Math.max(o.h * (1 - z), m.y - o.y - z * gesture.cy));
      view = { z: z, tx: z === 1 ? 0 : tx, ty: z === 1 ? 0 : ty };
      applyView();
    };

    // ----- hold / slide / release -----
    var redraw = function () {
      if (frame) return;
      frame = requestAnimationFrame(function () {
        frame = 0;
        if (grab && grab.armed && !grab.cancelled) opts.show(grab.node, grab.value, true);
      });
    };
    var label = function (node, from, to) { return opts.label ? opts.label(node, from, to) : String(to); };

    var arm = function () {
      if (!grab) return;
      grab.timer = null;
      if (grab.node.locked) {
        if (opts.onLocked) opts.onLocked(grab.node);
        else if (opts.lockedText && typeof window.showNotification === 'function') window.showNotification(opts.lockedText(grab.node));
        grab = null;
        return;
      }
      grab.armed = true;
      buzz();
      opts.show(grab.node, grab.value, true);
      showDragBubble(grab.x, grab.y, label(grab.node, grab.from, grab.value));
    };
    // x, y = screen coordinates (the bubble goes there); the maths is local
    var start = function (x, y) {
      var box = surfaceBox();
      if (!box) return false;
      var p = toLocal(x, y);
      var node = opts.pick((p.x - box.left) * box.kx, (p.y - box.top) * box.ky, 22 * box.kx);
      if (!node) return false; // not on a point: an ordinary touch
      grab = { node: node, x: x, y: y, lx: p.x, ly: p.y, k: box.ky, armed: false, cancelled: false, from: node.value, value: node.value };
      grab.timer = setTimeout(arm, holdMs);
      return true;
    };
    // true = this move belongs to a drag (the page must not scroll)
    var move = function (x, y) {
      if (!grab) return false;
      if (!grab.armed) {
        if (Math.hypot(x - grab.x, y - grab.y) > 10) { // moved before the hold: a scroll
          clearTimeout(grab.timer);
          grab = null;
        }
        return false;
      }
      if (grab.cancelled) return true;
      var p = toLocal(x, y);
      if (offChart(p.x, p.y)) {
        grab.cancelled = true;
        opts.show(grab.node, grab.from, false); // snap back
        showDragBubble(x, y, 'Cancelled');
        buzz();
        return true;
      }
      var node = grab.node;
      var lo = opt(opts.min, node, 0);
      var hi = opt(opts.max, node, 100);
      var step = opt(opts.step, node, 1);
      var units = per(node) || 1;
      var raw = grab.from + (((grab.ly - p.y) * grab.k) / units) * step;
      var value = Math.max(lo, Math.min(hi, Math.round(raw / step) * step));
      value = +value.toFixed(6); // no 0.30000000000000004
      if (value !== grab.value) {
        grab.value = value;
        redraw();
      }
      showDragBubble(x, y, label(node, grab.from, grab.value));
      return true;
    };
    var end = function (keep) {
      if (!grab) return;
      if (grab.timer) clearTimeout(grab.timer);
      var ended = grab;
      grab = null;
      if (frame) { cancelAnimationFrame(frame); frame = 0; }
      if (!ended.armed) return;
      if (ended.cancelled || !keep) {
        opts.show(ended.node, ended.from, false);
        setTimeout(hideDragBubble, ended.cancelled ? 600 : 0);
        return;
      }
      hideDragBubble();
      opts.show(ended.node, ended.value, false);
      if (opts.done) opts.done(ended.node);
    };

    overlay.addEventListener('touchstart', function (event) {
      lastTouchMs = Date.now();
      if (event.touches.length !== 1) { // a second finger: pinch / scroll, not a drag
        end(false);
        if (expanded()) gestureStart(touchesLocal(event.touches));
        return;
      }
      gesture = null;
      var t = event.touches[0];
      if (!start(t.clientX, t.clientY) && expanded() && view.z > 1) gestureStart(touchesLocal(event.touches)); // pan
    }, { passive: true });
    overlay.addEventListener('touchmove', function (event) {
      if (gesture) {
        gestureMove(touchesLocal(event.touches));
        if (event.cancelable) event.preventDefault();
        return;
      }
      var touch = event.touches[0];
      var hadGrab = !!grab;
      if (move(touch.clientX, touch.clientY) && event.cancelable) event.preventDefault();
      // zoomed in: a swipe that began on a point (before the hold) pans instead
      if (hadGrab && !grab && expanded() && view.z > 1) gestureStart(touchesLocal(event.touches));
    }, { passive: false });
    overlay.addEventListener('touchend', function (event) {
      lastTouchMs = Date.now();
      if (gesture) { // one of two fingers lifted: carry on panning with the other
        gesture = null;
        if (event.touches.length === 1 && view.z > 1) gestureStart(touchesLocal(event.touches));
        return;
      }
      end(true);
    });
    overlay.addEventListener('touchcancel', function () { lastTouchMs = Date.now(); gesture = null; end(false); });

    // Mouse (desktop): same hold, then drag. Phones also send mouse events after a
    // touch - ignore those.
    overlay.addEventListener('mousedown', function (event) {
      if (event.button !== 0 || grab || Date.now() - lastTouchMs < 800) return;
      if (start(event.clientX, event.clientY)) event.preventDefault();
    });
    window.addEventListener('mousemove', function (event) { if (grab && Date.now() - lastTouchMs >= 800) move(event.clientX, event.clientY); });
    window.addEventListener('mouseup', function () { if (Date.now() - lastTouchMs >= 800) end(true); });
  }

  window.initTouchGuards = initTouchGuards;
  window.buzz = buzz;
  window.attachLongPress = attachLongPress;
  window.chartLayer = chartLayer;
  window.showDragBubble = showDragBubble;
  window.hideDragBubble = hideDragBubble;
  window.attachCurveDrag = attachCurveDrag;

  initTouchGuards();
})();
