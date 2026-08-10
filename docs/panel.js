// The device's dashboard, drawn live in the browser.
//
// The hero and the tour both used to be img/dashboard.png — a photograph of a
// screen, frozen at 247 W and 1:47:12 forever. The whole claim of this device is
// that those numbers hold steady and legible while you ride, and a still image
// is the one thing that cannot make that claim: it looks equally like a working
// bike computer and a rendering of one.
//
// So the panel is drawn instead, at the device's own 540x960, off the same ride
// the iOS tutorial uses (companion-ios/Sources/RideSim.swift — the formulas are
// deliberately identical, so the phone and the website are showing the same
// rider). The static PNG stays in the markup underneath as the no-JS fallback.
(function () {
  "use strict";

  var W = 540, H = 960;

  // --- the ride ---------------------------------------------------------
  //
  // Pure functions of elapsed seconds: nothing accumulates, so a tab that
  // sleeps for an hour wakes up showing where the ride would actually be
  // rather than an hour behind.

  var START_ELAPSED = 1 * 3600 + 47 * 60 + 12;   // the still's 1:47:12
  var START_DIST = 54.8;                          // ...and its 54.8 km
  var START_CLOCK = 14 * 3600 + 25 * 60;

  function ride(t) {
    var speed = 30 + 6 * Math.sin(t / 11) + 2 * Math.sin(t / 3.3);
    // Distance is the integral of that speed in closed form, so the two can
    // never disagree — the one inconsistency a cyclist spots instantly.
    var dist = START_DIST +
      (30 * t - 66 * (Math.cos(t / 11) - 1) - 6.6 * (Math.cos(t / 3.3) - 1)) / 3600;
    return {
      t: t,
      speed: speed,
      // Power leads speed slightly, the way it does on a climb.
      power3s: 232 + 62 * Math.sin(t / 9 + 0.7) + 8 * Math.sin(t / 2.4),
      hr: 152 + 9 * Math.sin(t / 9 - 0.9) + 2 * Math.sin(t / 23),
      distance: dist,
      elapsed: START_ELAPSED + t,
      clock: START_CLOCK + t,
      battery: Math.max(0, 76 - Math.floor(t / 420)),
      sats: 11 + Math.round(Math.sin(t / 13))
    };
  }

  function pad(n) { return (n < 10 ? "0" : "") + n; }

  function hms(sec) {
    var s = Math.max(0, Math.floor(sec));
    return Math.floor(s / 3600) + ":" + pad(Math.floor(s / 60) % 60) + ":" + pad(s % 60);
  }

  function hm(sec) {
    var s = Math.max(0, Math.floor(sec)) % 86400;
    return Math.floor(s / 3600) + ":" + pad(Math.floor(s / 60) % 60);
  }

  /// Which of the seven FTP zones a wattage is in, against the 250 W default.
  function zone(watts) {
    var pct = watts / 250 * 100;
    if (pct < 55) return 1;
    if (pct < 75) return 2;
    if (pct < 90) return 3;
    if (pct < 105) return 4;
    if (pct < 120) return 5;
    if (pct < 150) return 6;
    return 7;
  }

  // --- the panel, as the firmware lays it out ---------------------------
  //
  // This is a port of src/ui_render.cpp's dashboard, by way of the same port
  // the iOS app already runs (DashPreview in DashboardEditorView.swift). The
  // first version of this file guessed its type sizes and drew full-width
  // rules between the fields, which is not what the device does: every field
  // is a BOXED cell with its own border, and the type is picked off fixed
  // ladders rather than chosen to look about right.
  //
  // Geometry is in device pixels and scaled once at the end, so the numbers
  // below are the firmware's own constants.

  var MARGIN = 24, GUTTER = 12, PAD = 16;
  var STATUS_H = 64, STEP = 12, HALF_STEP = 6, RULE = 2;
  var CONTENT_W = W - 2 * MARGIN;
  /// Barlow's cap height is 0.72 em: converts a device cap height into the
  /// point size that draws a capital that tall.
  var CAP = 0.72;

  var COND = '700 %dpx "Barlow Condensed", Impact, sans-serif';
  var LABEL = '600 %dpx "Barlow", Arial, sans-serif';

  function font(spec, px) { return spec.replace("%d", Math.round(px * 10) / 10); }

  // kValueLadder, strictly descending. `cap` is the digit height the firmware
  // measures against; the advances are what its textWidth() returns. These are
  // the host build's font tables — guessing them is how the preview drifted.
  var VALUE_LADDER = [
    { cap: 158, w8: 103, w1: 73, wDot: 35, wColon: 39 },
    { cap: 120, w8: 78,  w1: 56, wDot: 27, wColon: 30 },
    { cap: 95,  w8: 61,  w1: 44, wDot: 21, wColon: 23 },
    { cap: 81,  w8: 53,  w1: 38, wDot: 18, wColon: 20 },
    { cap: 69,  w8: 45,  w1: 32, wDot: 16, wColon: 17 },
    { cap: 58,  w8: 38,  w1: 27, wDot: 13, wColon: 14 },
    { cap: 46,  w8: 30,  w1: 21, wDot: 10, wColon: 11 },
    { cap: 30,  w8: 20,  w1: 14, wDot: 7,  wColon: 8 },
    { cap: 15,  w8: 12,  w1: 12, wDot: 6,  wColon: 7 }
  ];
  /// The hero steps through its own ladder: XL, H, M, V.
  var HERO_LADDER = [VALUE_LADDER[0], VALUE_LADDER[2], VALUE_LADDER[4], VALUE_LADDER[6]];
  /// kLabelLadder. `perChar` is the tracked caption's average advance.
  var LABEL_LADDER = [
    { cap: 28, ascender: 38, perChar: 30.1 },
    { cap: 15, ascender: 19, perChar: 16.6 },
    { cap: 11, ascender: 14, perChar: 12.4 }
  ];
  var UNIT_FACE = { cap: 15, ascender: 19, perChar: 13.5 };

  /// The layout the device ships with, as rows of cells. `half` fields share a
  /// row; the hero takes one to itself.
  var ROWS = [
    [{ field: "power3s", size: "hero" }],
    [{ field: "hr", size: "medium" }, { field: "speed", size: "medium" }],
    [{ field: "ridetime", size: "medium" }, { field: "distance", size: "medium" }]
  ];
  var WEIGHT = { small: 2, medium: 3, large: 4, hero: 8 };

  function label(field) {
    return { power3s: "POWER · 3S", hr: "HEART RATE", speed: "SPEED",
             ridetime: "RIDE TIME", distance: "DISTANCE" }[field];
  }
  function unit(field) {
    return { power3s: "W", hr: "BPM", speed: "KM/H", distance: "KM" }[field] || "";
  }
  /// dashSizingHint(): the WIDEST string a field can produce. The device sizes
  /// type for the worst case so the number never resizes mid-ride.
  function hint(field) {
    return { power3s: "888", hr: "888", speed: "88.8",
             ridetime: "88:88:88", distance: "888.8" }[field];
  }
  /// A plausible value, used only for fitting the hero — as the firmware does.
  function sample(field) {
    return { power3s: "247", hr: "156", speed: "32.4",
             ridetime: "1:47:12", distance: "54.8" }[field];
  }

  function advance(face, s) {
    var w = 0;
    for (var i = 0; i < s.length; i++) {
      var c = s[i];
      w += c === "." ? face.wDot : c === ":" ? face.wColon : c === "1" ? face.w1 : face.w8;
    }
    return w;
  }

  // MARK: layout — ui_render.cpp's packer, in device pixels

  function place() {
    var weights = ROWS.map(function (row) {
      return Math.max.apply(null, row.map(function (it) { return WEIGHT[it.size]; }));
    });
    var total = weights.reduce(function (a, b) { return a + b; }, 0);
    var availH = (H - STATUS_H - MARGIN) - (ROWS.length - 1) * GUTTER;
    var halfW = (CONTENT_W - GUTTER) / 2;

    var out = [], y = STATUS_H + MARGIN - STEP;
    ROWS.forEach(function (row, r) {
      var rowH = r === ROWS.length - 1 ? H - MARGIN - y : availH * weights[r] / total;
      row.forEach(function (item, c) {
        var w = row.length === 2 ? halfW : CONTENT_W;
        var x = row.length === 2 ? (c === 0 ? MARGIN : MARGIN + halfW + GUTTER) : MARGIN;
        out.push({
          x: x, y: y, w: w, h: rowH, item: item,
          // The hero only gets hero treatment alone on a row with the height
          // to carry it, exactly as the device decides.
          hero: item.size === "hero" && row.length === 1 && rowH >= 200
        });
      });
      y += rowH + GUTTER;
    });
    return size(out);
  }

  /// Second pass: equalise type across each size class, as the device does. A
  /// cell that sizes itself independently makes identical boxes disagree.
  function size(cells) {
    var classIdx = {}, labelIdx = 0;

    cells.forEach(function (p) {
      if (p.hero) return;
      var availW = p.w - 2 * PAD;
      var u = unit(p.item.field);
      var unitW = u ? UNIT_FACE.perChar * u.length + 6 : 0;
      var valH = p.h - PAD * 2 - LABEL_LADDER[1].ascender - HALF_STEP;
      var idx = VALUE_LADDER.length - 1;
      for (var i = 0; i < VALUE_LADDER.length; i++) {
        if (VALUE_LADDER[i].cap <= valH &&
            advance(VALUE_LADDER[i], hint(p.item.field)) + unitW <= availW) { idx = i; break; }
      }
      var key = p.item.size + "-" + (p.w > CONTENT_W * 3 / 4 ? "wide" : "narrow");
      classIdx[key] = Math.max(classIdx[key] || 0, idx);

      var li = LABEL_LADDER.length - 1;
      for (var j = 0; j < LABEL_LADDER.length; j++) {
        if (LABEL_LADDER[j].perChar * label(p.item.field).length <= availW) { li = j; break; }
      }
      labelIdx = Math.max(labelIdx, li);
    });

    cells.forEach(function (p) {
      p.label = LABEL_LADDER[labelIdx];
      if (p.hero) { p.value = heroFace(p); return; }
      var key = p.item.size + "-" + (p.w > CONTENT_W * 3 / 4 ? "wide" : "narrow");
      p.value = VALUE_LADDER[classIdx[key] || 0];
    });
    return cells;
  }

  function heroFace(p) {
    var innerW = p.w - 2 * PAD;
    var u = unit(p.item.field);
    var unitW = u ? UNIT_FACE.perChar * u.length + 10 : 0;
    var barH = 18 + STEP;                       // the power field carries a zone bar
    var top = PAD + LABEL_LADDER[1].ascender + HALF_STEP;
    var bot = p.h - PAD - barH;
    for (var i = 0; i < HERO_LADDER.length; i++) {
      if (advance(HERO_LADDER[i], sample(p.item.field)) + unitW <= innerW &&
          HERO_LADDER[i].cap <= bot - top) return HERO_LADDER[i];
    }
    return HERO_LADDER[HERO_LADDER.length - 1];
  }

  // MARK: drawing

  function drawDashboard(ctx, t) {
    var r = ride(t);
    ctx.save();
    ctx.fillStyle = "#fff";
    ctx.fillRect(0, 0, W, H);
    ctx.fillStyle = "#000";
    ctx.strokeStyle = "#000";
    ctx.textBaseline = "alphabetic";

    statusBar(ctx, r);
    ctx.fillRect(0, STATUS_H - 3, W, 3);        // the rule under the status bar

    place().forEach(function (p) { cell(ctx, p, r); });
    ctx.restore();
  }

  function cell(ctx, p, r) {
    // Every field is a box. The firmware strokes RULE passes of a rect; the
    // dividers this used to draw were a different panel entirely.
    ctx.lineWidth = RULE;
    ctx.strokeRect(p.x + RULE / 2, p.y + RULE / 2, p.w - RULE, p.h - RULE);

    var text = value(p.item.field, r);
    var u = unit(p.item.field);
    var cx = p.x + p.w / 2;

    if (p.hero) {
      caption(ctx, label(p.item.field), cx, p.y + PAD + p.label.ascender, p.label, "center");
      var barH = 18 + STEP;
      var top = p.y + PAD + p.label.ascender + HALF_STEP;
      var bot = p.y + p.h - PAD - barH;
      pair(ctx, text, u, cx, (top + bot) / 2, p.value, p.w - 2 * PAD);
      zoneBar(ctx, p, zone(r.power3s));
    } else {
      caption(ctx, label(p.item.field), p.x + PAD, p.y + PAD + p.label.ascender,
              p.label, "left");
      // ui_render.cpp: the value box is everything 10 px below the caption's
      // baseline, less the bottom padding.
      var vTop = p.y + PAD + p.label.ascender + 6 + 10;
      var vBot = p.y + p.h - PAD;
      pair(ctx, text, u, cx, (vTop + vBot) / 2, p.value, p.w - 2 * PAD);
    }
  }

  function caption(ctx, text, x, baseline, face, align) {
    ctx.font = font(LABEL, face.cap / CAP);
    ctx.textAlign = align;
    var track = face.cap * 0.18;
    if ("letterSpacing" in ctx) {
      ctx.letterSpacing = track + "px";
      ctx.fillText(text, x, baseline);
      ctx.letterSpacing = "0px";
      return;
    }
    var total = 0, i;
    for (i = 0; i < text.length; i++) total += ctx.measureText(text[i]).width + track;
    total -= track;
    var cx = align === "center" ? x - total / 2 : x;
    ctx.textAlign = "left";
    for (i = 0; i < text.length; i++) {
      ctx.fillText(text[i], cx, baseline);
      cx += ctx.measureText(text[i]).width + track;
    }
  }

  /// Value and unit are ONE object, centred as a pair — centring the number
  /// alone pushes it off-axis by half the unit's width. The pair is squeezed if
  /// Barlow Condensed happens to run wider than the Impact metrics the face was
  /// chosen with, which is the same guard the app's preview keeps.
  function pair(ctx, text, u, cx, middle, face, maxW) {
    var px = face.cap / CAP;
    ctx.textAlign = "left";
    ctx.font = font(COND, px);
    var vw = ctx.measureText(text).width;
    ctx.font = font(LABEL, UNIT_FACE.cap / CAP);
    var uw = u ? ctx.measureText(u).width + 6 : 0;

    if (vw + uw > maxW) {
      px *= Math.max(0.5, maxW / (vw + uw));
      ctx.font = font(COND, px);
      vw = ctx.measureText(text).width;
    }
    var baseline = middle + face.cap / 2;
    var left = cx - (vw + uw) / 2;
    ctx.font = font(COND, px);
    ctx.fillText(text, left, baseline);
    if (u) {
      ctx.font = font(LABEL, UNIT_FACE.cap / CAP);
      ctx.fillText(u, left + vw + 6, baseline);
    }
  }

  /// Seven FTP zone segments across the bottom of the hero, filled to the
  /// current zone.
  function zoneBar(ctx, p, filled) {
    var width = p.w - 2 * PAD;
    var seg = (width - 6 * HALF_STEP) / 7, h = 18;
    var y = p.y + p.h - PAD - h;
    ctx.lineWidth = 1;
    for (var i = 0; i < 7; i++) {
      var x = p.x + PAD + i * (seg + HALF_STEP);
      if (i < filled) ctx.fillRect(x, y, seg, h);
      else ctx.strokeRect(x + 0.5, y + 0.5, seg - 1, h - 1);
    }
  }

  function value(field, r) {
    switch (field) {
      case "power3s":  return String(Math.round(r.power3s));
      case "hr":       return String(Math.round(r.hr));
      case "speed":    return r.speed.toFixed(1);
      case "ridetime": return hms(r.elapsed);
      case "distance": return r.distance.toFixed(1);
      default:         return "--";
    }
  }

  function statusBar(ctx, r) {
    // Sized off the caption ladder rather than by eye: at the size this was
    // first drawn, the battery ran into "PWR".
    var face = LABEL_LADDER[0];                 // the panel's biggest caption face
    var px = face.cap / CAP;
    var baseline = PAD + face.cap;
    ctx.textAlign = "left";
    ctx.font = font(COND, px);
    var x = 14;
    var clock = hm(r.clock);
    ctx.fillText(clock, x, baseline);
    x += ctx.measureText(clock).width + 12;

    ctx.lineWidth = 3;
    ctx.strokeRect(x + 1.5, baseline - 26, 16, 28);   // the phone-link glyph
    x += 30;

    // Satellites: filled dots for lock quality, hollow for the rest, so the row
    // never changes width as the sky opens and closes.
    var lit = Math.max(0, Math.min(4, r.sats - 8));
    for (var i = 0; i < 4; i++) {
      ctx.beginPath();
      ctx.arc(x + 6, baseline - 11, 5.5, 0, Math.PI * 2);
      if (i < lit) ctx.fill(); else ctx.stroke();
      x += 17;
    }

    ctx.font = font(COND, px);
    ctx.fillText("· HR · PWR", x + 4, baseline);

    var bw = 52, bh = 26, bx = W - 14 - 6 - bw, by = baseline - 24;
    ctx.lineWidth = 3.5;
    ctx.strokeRect(bx, by, bw, bh);
    ctx.fillRect(bx + 4, by + 4, (bw - 8) * r.battery / 100, bh - 8);
    ctx.fillRect(bx + bw + 2, by + 8, 5, 10);
    ctx.textAlign = "right";
    ctx.font = font(COND, px);
    ctx.fillText(r.battery + "%", bx - 10, baseline);
  }

  // --- driving it -------------------------------------------------------

  /// Runs `canvas` as a live panel. Redraws once a second, because that is how
  /// often the e-paper it is imitating can redraw — and it stops entirely when
  /// the canvas is off screen, the tab is hidden, or the visitor has asked for
  /// reduced motion, in which case it paints the opening frame and leaves it.
  function live(canvas) {
    var ctx = canvas.getContext("2d");
    var start = performance.now();
    var timer = null, visible = false;

    function paint() {
      drawDashboard(ctx, (performance.now() - start) / 1000);
    }

    var still = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;

    function run() {
      stop();
      if (still || !visible || document.hidden) return;
      timer = setInterval(paint, 1000);
    }
    function stop() { if (timer) { clearInterval(timer); timer = null; } }

    // Fonts first: a panel that paints in Helvetica and then reflows into
    // Barlow Condensed a moment later is worse than one that arrives late.
    var ready = document.fonts && document.fonts.ready ? document.fonts.ready : Promise.resolve();
    ready.then(function () {
      paint();
      canvas.classList.add("ready");
      run();
    });

    if (window.IntersectionObserver) {
      new IntersectionObserver(function (entries) {
        visible = entries[0].isIntersecting;
        if (visible) paint();
        run();
      }, { threshold: 0.05 }).observe(canvas);
    } else {
      visible = true;
      run();
    }
    document.addEventListener("visibilitychange", run);

    return { paint: paint };
  }

  window.OTPPanel = { W: W, H: H, ride: ride, drawDashboard: drawDashboard, live: live };

  // The hero device runs itself; the tour's panel is driven by emulator.js,
  // which only shows it on the steps that are actually the dashboard.
  var heroCanvas = document.getElementById("hero-panel");
  if (heroCanvas) live(heroCanvas);
})();
