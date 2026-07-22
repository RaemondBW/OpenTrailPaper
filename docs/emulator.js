// Interactive device walkthrough for the "Take the tour" section. It auto-plays
// through the real firmware screens (the same rendered PNGs the gallery uses):
// each step shows a screen, flashes a tap where you'd touch the panel, then the
// screen actually updates to the result of that tap. An always-visible list
// expands the step it's currently on.
//
// tap coordinates are percentages of the 540x960 screen, matching the on-device
// touch zones in src/ui_dashboard.cpp:
//   - status bar (y < 64 px  -> ~3.5%) opens the menu
//   - menu rows: top 96, height 148 (row centres ~18%, ~33%, ~48.5%, ~64%, ~79%)
//   - ride-summary buttons sit at y 830..960 (~93%)
//   - the nav prompt's START button sits at ~79%
// negative x taps point just off the left edge, at the physical side button.
const steps = [
  {
    from: "dashboard", to: "map", tap: [50, 55],
    title: "The dashboard",
    text: "Power, heart rate, speed, distance and ride time — big enough to read mid-pedal. Tap the screen to switch to the map.",
    hint: "Tap the screen",
  },
  {
    from: "map", to: "menu", tap: [50, 3.5],
    title: "The map",
    text: "An offline map that follows your GPS, drawn from tiles on the SD card — no signal needed. Tap the status bar up top to open the menu.",
    hint: "Tap the top status bar",
  },
  {
    from: "menu", to: "sensors", tap: [50, 48.5],
    title: "The menu",
    text: "Start a ride, load a route, pair sensors, browse history or change settings. Tap Sensors to manage your gear.",
    hint: "Tap the Sensors row",
  },
  {
    from: "summary", to: "dashboard", tap: [50, 93],
    title: "Ride summary",
    text: "Finish a ride and you get distance, moving time, average and normalized power, heart rate and ascent. Tap Save to keep it as a .fit file.",
    hint: "Tap Save",
  },
  {
    from: "nav_prompt", to: "nav_banner", tap: [50, 79],
    title: "Turn-by-turn navigation",
    text: "Send a route from the phone and the device asks to start. Tap START and the first turn drops in over the map with the distance to go.",
    hint: "Tap START",
  },
  {
    from: "dashboard", to: "dashboard", tap: [-6, 46], backlight: true,
    title: "Ride after dark",
    text: "Press the side button to switch on the backlight — the e-paper stays readable at night. Press again to turn it off.",
    hint: "Press the side button",
  },
];

const STEP_MS = 4600;   // total dwell per step
const TAP_AT = 1100;    // when the tap flashes
const RESULT_AT = 1650; // when the screen updates to the result of the tap

const $ = (id) => document.getElementById(id);

if ($("emu")) init();

function init() {
  const img = $("emu-img"), tap = $("emu-tap"), flash = $("emu-flash"), glow = $("emu-glow");
  const stepsEl = $("emu-steps"), dotsEl = $("emu-dots"), root = $("emu");
  let i = 0, advanceTimer = null, tapTimer = null, resultTimer = null;

  // Build the always-visible step list (an accordion; the active row expands).
  steps.forEach((s, k) => {
    const li = document.createElement("li");
    li.className = "emu-step";
    const head = document.createElement("button");
    head.type = "button";
    head.className = "emu-step-head";
    head.innerHTML = `<span class="emu-step-n">${k + 1}</span><span class="emu-step-title"></span>`;
    head.querySelector(".emu-step-title").textContent = s.title;
    head.addEventListener("click", () => select(k, true));

    const body = document.createElement("div");
    body.className = "emu-step-body";
    const inner = document.createElement("div");
    const p = document.createElement("p");
    p.textContent = s.text;
    const hint = document.createElement("p");
    hint.className = "emu-hint";
    hint.innerHTML = `<span class="emu-hand" aria-hidden="true">☝</span><span></span>`;
    hint.querySelector("span:last-child").textContent = s.hint;
    inner.append(p, hint);
    body.append(inner);
    li.append(head, body);
    stepsEl.append(li);

    const dot = document.createElement("button");
    dot.type = "button";
    dot.className = "emu-dot";
    dot.setAttribute("aria-label", "Step " + (k + 1) + ": " + s.title);
    dot.addEventListener("click", () => select(k, true));
    dotsEl.append(dot);
  });
  const items = [...stepsEl.children];
  const dots = [...dotsEl.children];

  function replay(el, cls) {
    el.classList.remove(cls);
    void el.offsetWidth;     // reflow so the one-shot animation restarts
    el.classList.add(cls);
  }

  function select(k, manual) {
    i = (k + steps.length) % steps.length;
    const s = steps[i];
    img.src = "img/" + s.from + ".png";
    glow.classList.remove("on");
    items.forEach((li, k2) => li.classList.toggle("on", k2 === i));
    dots.forEach((d, k2) => d.classList.toggle("on", k2 === i));

    clearTimeout(tapTimer);
    clearTimeout(resultTimer);
    tap.classList.remove("go");

    // 1) flash the tap point, 2) then apply the result of that tap.
    tap.style.left = s.tap[0] + "%";
    tap.style.top = s.tap[1] + "%";
    tapTimer = setTimeout(() => replay(tap, "go"), TAP_AT);
    resultTimer = setTimeout(() => {
      if (s.backlight) {
        glow.classList.add("on");        // the backlight comes on, screen unchanged
      } else {
        img.src = "img/" + s.to + ".png";
        replay(flash, "go");             // brief e-paper-style refresh flash
      }
    }, RESULT_AT);

    schedule();
  }

  function schedule() {
    clearTimeout(advanceTimer);
    advanceTimer = setTimeout(() => select(i + 1, false), STEP_MS);
  }

  $("emu-prev").addEventListener("click", () => select(i - 1, true));
  $("emu-next").addEventListener("click", () => select(i + 1, true));

  // Pause auto-play while the visitor is reading (hovering the component).
  root.addEventListener("mouseenter", () => clearTimeout(advanceTimer));
  root.addEventListener("mouseleave", schedule);

  select(0, false);
}
