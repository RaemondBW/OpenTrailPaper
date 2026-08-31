// Interactive device walkthrough for the "Device controls" section. Each list
// item selects the screen it describes. The visitor then presses the real
// control highlighted on the mock device to see where that control leads.
//
// Screen coordinates are percentages of the firmware's 540x960 display:
//   - the status bar is the top 64 px
//   - menu rows start at y=64 and are 148 px high
//   - summary actions begin at y=700; SAVE is the upper-left action
//   - the route prompt's START action is centred around 79% height
const steps = [
  {
    view: "dashboard", control: "home", next: 1,
    title: "The dashboard",
    text: "Shows power, heart rate, speed, distance and ride time. Change the screen order, fields and field sizes in the phone app or by editing /config/dashboard.cfg on the SD card.",
    hint: "Press Home to open the workout page",
  },
  {
    view: "workout_on_target", control: "home", next: 2,
    title: "Structured workout",
    text: "Shows the current target, your live power, time remaining in the block and the full workout profile. Workouts can be created in the app or loaded from the SD card.",
    hint: "Press Home to continue to the map",
  },
  {
    view: "map", control: "screen", tap: [50, 3.333], size: [100, 6.667], next: 3,
    title: "Offline map",
    text: "Reads map tiles from the SD card and follows the device GPS. The map works without a data connection in areas you loaded beforehand.",
    hint: "Tap the status bar to open the menu",
  },
  {
    view: "menu_recording", control: "screen", tap: [50, 45.208], size: [100, 15.417], next: 4,
    title: "Main menu",
    text: "During a ride, the menu can stop recording, load a route, open sensor pairing, list saved rides or open settings.",
    hint: "Tap Sensors to open sensor pairing",
  },
  {
    view: "sensors", control: "home", next: 5,
    title: "Sensor pairing",
    text: "Scans for compatible Bluetooth heart-rate, power and cadence sensors. Tap a listed sensor to save it for reconnection.",
    hint: "Press Home to return to the ride menu",
  },
  {
    view: "menu_recording", control: "screen", tap: [50, 14.375], size: [100, 15.417], next: 6,
    title: "Stop the ride",
    text: "While a ride is recording, the first menu row reads Stop Ride. Tapping it opens the summary before anything is saved. The upper side button is a shortcut for starting or stopping a ride.",
    hint: "Tap Stop Ride to review the ride",
  },
  {
    view: "summary", control: "screen", tap: [26.667, 77.917], size: [44.444, 10], next: 7,
    title: "Ride summary",
    text: "The summary shows distance, total ride time, power, heart rate and estimated ascent. Save ends the recording and writes a FIT file to the SD card.",
    hint: "Tap Save to return to the dashboard",
  },
  {
    view: "nav_prompt", result: "nav_banner", control: "screen", tap: [50, 77.917], size: [91.111, 10], next: 8,
    title: "Route navigation",
    text: "Load a GPX route from the phone app to show turn prompts over the map. Guidance depends on the route file and GPS accuracy.",
    hint: "Tap Start to see the first turn prompt",
  },
  {
    view: "nav_banner", control: "side", backlight: true,
    title: "Front light",
    text: "The lower side button switches the built-in front light on or off. Using it reduces battery life.",
    hint: "Press the lower side button to turn on the light",
  },
];

const $ = (id) => document.getElementById(id);
const SCREEN_ASSET_VERSION = "2";

if ($("emu")) init();

function init() {
  const img = $("emu-img");
  const tap = $("emu-tap");
  const flash = $("emu-flash");
  const glow = $("emu-glow");
  const stepsEl = $("emu-steps");
  const dotsEl = $("emu-dots");
  const home = $("emu-home");
  const side = $("emu-side");
  const help = $("emu-help");
  let i = 0;
  let actionTimer = null;

  // Keep the walkthrough on one fixed ride so the dashboard, menu and summary
  // show exactly the same distance and elapsed time. The hero above remains
  // live; the tour deliberately uses the firmware-rendered still.
  const panel = $("emu-panel");
  if (panel) panel.hidden = true;

  function show(name) {
    img.src = "img/" + name + ".png?v=" + SCREEN_ASSET_VERSION;
    img.alt = steps[i].title + " device screen";
  }

  steps.forEach((step, index) => {
    const li = document.createElement("li");
    li.className = "emu-step";

    const head = document.createElement("button");
    head.type = "button";
    head.className = "emu-step-head";
    head.innerHTML = `<span class="emu-step-n">${index + 1}</span><span class="emu-step-title"></span>`;
    head.querySelector(".emu-step-title").textContent = step.title;
    head.setAttribute("aria-expanded", "false");
    head.addEventListener("click", () => select(index));

    const body = document.createElement("div");
    body.className = "emu-step-body";
    const inner = document.createElement("div");
    const description = document.createElement("p");
    description.textContent = step.text;
    const hint = document.createElement("p");
    hint.className = "emu-hint";
    hint.innerHTML = `<span class="emu-hand" aria-hidden="true">☝</span><span></span>`;
    hint.querySelector("span:last-child").textContent = step.hint;
    inner.append(description, hint);
    body.append(inner);
    li.append(head, body);
    stepsEl.append(li);

    const dot = document.createElement("button");
    dot.type = "button";
    dot.className = "emu-dot";
    dot.setAttribute("aria-label", "View " + (index + 1) + ": " + step.title);
    dot.addEventListener("click", () => select(index));
    dotsEl.append(dot);
  });

  const items = [...stepsEl.children];
  const dots = [...dotsEl.children];

  function replay(el, cls) {
    el.classList.remove(cls);
    void el.offsetWidth;
    el.classList.add(cls);
  }

  // Selecting a list item resets the image to that item's named screen. This
  // keeps every label, description and screenshot in sync.
  function select(index) {
    i = (index + steps.length) % steps.length;
    const step = steps[i];
    clearTimeout(actionTimer);
    show(step.view);
    glow.classList.remove("on");
    items.forEach((item, itemIndex) => {
      const active = itemIndex === i;
      item.classList.toggle("on", active);
      item.querySelector(".emu-step-head").setAttribute("aria-expanded", String(active));
    });
    dots.forEach((dot, dotIndex) => {
      dot.classList.toggle("on", dotIndex === i);
      if (dotIndex === i) dot.setAttribute("aria-current", "step");
      else dot.removeAttribute("aria-current");
    });
    tap.classList.remove("go");
    tap.hidden = step.control !== "screen";
    home.classList.toggle("is-target", step.control === "home");
    side.classList.toggle("is-target", step.control === "side");

    if (step.control === "screen") {
      tap.style.left = step.tap[0] + "%";
      tap.style.top = step.tap[1] + "%";
      tap.style.width = step.size[0] + "%";
      tap.style.height = step.size[1] + "%";
      tap.setAttribute("aria-label", step.hint);
    }
    help.textContent = step.hint;
  }

  function activate(control) {
    const step = steps[i];
    if (step.control !== control) return;
    const target = control === "screen" ? tap : control === "home" ? home : side;
    replay(target, "go");
    // A result screen (currently the route's START action) stays visible for a
    // beat before the walkthrough advances. Its old hit target belongs to the
    // prompt, not the result, so remove it as soon as it has been pressed.
    if (control === "screen" && step.result) tap.hidden = true;
    clearTimeout(actionTimer);
    actionTimer = setTimeout(() => {
      if (step.backlight) {
        const on = glow.classList.toggle("on");
        help.textContent = on
          ? "Front light on — press again to turn it off"
          : step.hint;
      } else if (step.result) {
        show(step.result);
        replay(flash, "go");
        help.textContent = "Turn prompt shown — Front light is next";
        if (Number.isInteger(step.next)) {
          actionTimer = setTimeout(() => select(step.next), 900);
        }
      } else if (Number.isInteger(step.next)) {
        select(step.next);
      }
    }, 280);
  }

  tap.addEventListener("click", () => activate("screen"));
  home.addEventListener("click", () => activate("home"));
  side.addEventListener("click", () => activate("side"));
  $("emu-prev").addEventListener("click", () => select(i - 1));
  $("emu-next").addEventListener("click", () => select(i + 1));

  select(0);
}
