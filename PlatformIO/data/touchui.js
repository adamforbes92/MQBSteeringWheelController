/* touchui.js - shared phone touch handling for every Forbes Automotive web UI.
 *
 * MASTER COPY: <Drive>:\My Drive\PlatformIO\_SharedTheme\touchui.js (+ touchui.css)
 * Edit it there, then copy both files unchanged into each product's data/ (same as ota.js).
 * Based on OpenHaldex-C6 V11.00.12 (Expert PT cells + draggable charts). See README_TOUCHUI.md.
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
  // units: SVG viewBox units, or canvas.width pixels (set opts.cssUnits for a canvas
  // drawn in CSS pixels, e.g. one scaled with ctx.setTransform(devicePixelRatio...)).
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
  function attachCurveDrag(host, opts) {
    var per = opts.unitsPerStep || opts.unitsPerPct;
    var holdMs = opts.holdMs || HOLD_MS;
    var opt = function (v, node, dflt) { return typeof v === 'function' ? v(node) : (v === undefined ? dflt : v); };

    host.classList.add('chart-drag');
    var overlay = document.createElement('div');
    overlay.className = 'chart-touch';
    overlay.addEventListener('contextmenu', function (event) { event.preventDefault(); }); // a long hold is ours
    host.appendChild(overlay);

    var grab = null; // { node, x, y, k, timer, armed, cancelled, from, value }
    var frame = 0;
    var lastTouchMs = 0;

    var surfaceBox = function () {
      var el = host.querySelector('svg, canvas');
      if (!el) return null;
      var rect = el.getBoundingClientRect();
      var width = rect.width;
      if (el.tagName.toLowerCase() === 'svg') {
        if (el.viewBox && el.viewBox.baseVal && el.viewBox.baseVal.width) width = el.viewBox.baseVal.width;
      } else if (!opts.cssUnits) {
        width = el.width;
      }
      return { rect: rect, k: width / (rect.width || 1) }; // k = chart units per screen pixel
    };
    var offChart = function (x, y) {
      var rect = host.getBoundingClientRect();
      var margin = 10;
      return x < rect.left - margin || x > rect.right + margin || y < rect.top - margin || y > rect.bottom + margin;
    };
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
    var start = function (x, y) {
      var box = surfaceBox();
      if (!box) return false;
      var node = opts.pick((x - box.rect.left) * box.k, (y - box.rect.top) * box.k, 22 * box.k);
      if (!node) return false; // not on a point: an ordinary touch
      grab = { node: node, x: x, y: y, k: box.k, armed: false, cancelled: false, from: node.value, value: node.value };
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
      if (offChart(x, y)) {
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
      var raw = grab.from + (((grab.y - y) * grab.k) / units) * step;
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
      if (event.touches.length !== 1) { end(false); return; } // a second finger: pinch / scroll
      start(event.touches[0].clientX, event.touches[0].clientY);
    }, { passive: true });
    overlay.addEventListener('touchmove', function (event) {
      var touch = event.touches[0];
      if (move(touch.clientX, touch.clientY) && event.cancelable) event.preventDefault();
    }, { passive: false });
    overlay.addEventListener('touchend', function () { lastTouchMs = Date.now(); end(true); });
    overlay.addEventListener('touchcancel', function () { lastTouchMs = Date.now(); end(false); });

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
