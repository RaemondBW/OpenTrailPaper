# Turn-by-turn navigation bug — root-cause analysis

**Symptom (from a real ride):** the route drawn on the map was correct, but the
turn-by-turn prompts ran *several maneuvers ahead* of the rider — it announced
turns far in advance and showed the wrong "upcoming" turn. The geometry is fine;
the mapping from *current position → which maneuver is next* (and the distance to
it) is wrong.

All navigation logic lives in `src/routes.cpp` / `src/routes.h`. The BLE upload
path (`src/ble_server.cpp`) and the UI glue (`src/ui_dashboard.cpp`,
`src/ui_render.cpp`) were reviewed and are **not** at fault.

---

## 1. How a GPS fix flows to a displayed next-turn

1. **GPS fix** — every valid fix (~1 Hz) calls
   `routes::updateProgress(lat, lon)` from `src/gps_service.cpp:577`.

2. **Project rider onto route** — `updateProgress` (`src/routes.cpp:294`):
   - Searches a forward window `[progIdx-8, progIdx+120)` for the nearest route
     point with `nearestRouteIdx` (`src/routes.cpp:302-305`).
   - **If that window match is > 60 m away, it re-acquires against the *entire*
     route** (`src/routes.cpp:306-310`, pre-fix) and takes the global match if
     it is any bit closer.
   - Sets `progIdx` to the chosen index (`src/routes.cpp:311`) — this is the
     rider's along-route position, the single anchor everything else keys off.

3. **Advance the maneuver cursor** — `advanceManeuverCursor`
   (`src/routes.cpp:77-82`) walks `curManeuver` forward while a maneuver's
   snapped position is behind the rider:
   `cumM[maneuverIdx[curManeuver]] <= cumM[progIdx] - 5.0f`.

4. **Report the next turn** — the banner calls `routes::nextTurn(...)`
   (`src/ui_dashboard.cpp:595` → `src/routes.cpp:378`), which returns
   `maneuvers[curManeuver].instr` and distance
   `along = cumM[maneuverIdx[curManeuver]] - cumM[progIdx]`
   (`src/routes.cpp:381`).

Every output (which turn, and how far) is therefore a function of two indices:
`progIdx` (from step 2) and `maneuverIdx[]` (snapped once at upload). If either
is placed too far *forward* on the route, `advanceManeuverCursor` skips real
turns and `nextTurn` reports a maneuver that is still ahead of the rider.

The maneuver coordinates themselves are fine: they arrive as int32 `1e7`
lat/lon over BLE (`src/ble_server.cpp:148-157`) and are stored as plain WGS84
degrees — the same frame/units as the parsed route points. `finishManeuvers`
(`src/routes.cpp:347`) runs `mapManeuversToRoute` *after* the geometry is loaded
(`0x03` before `0x05` in the protocol), so the ordering is correct. No unit or
coordinate-scaling mismatch exists.

---

## 2. Root cause

**Primary: the whole-route re-acquire in `updateProgress` is unbounded
(`src/routes.cpp:306-310`, pre-fix).**

```cpp
int idx = nearestRouteIdx(lat, lon, from, to, d2);   // forward window
if (d2 > 60.0f * 60.0f) {                             // window match > 60 m
    float fd2;
    int fidx = nearestRouteIdx(lat, lon, 0, nPts, fd2);   // ENTIRE route
    if (fidx >= 0 && fd2 < d2) { idx = fidx; d2 = fd2; }  // any bit closer wins
}
```

On a route that passes near itself — a loop, an out-and-back, a lollipop, or any
self-crossing — the **globally** nearest route point to the rider can belong to a
*later* leg that runs alongside the rider's *current* leg. As soon as a single
GPS fix drifts more than 60 m from the windowed match (normal urban jitter, tree
cover, a momentary multipath spike, or riding slightly wide of the line), the
fallback fires and snaps `progIdx` onto that later, parallel leg — jumping it far
ahead along the route.

This is self-latching. After the jump, the next fix's window is
`[newProgIdx-8, newProgIdx+120)`; the rider's true point is now well *behind*
`newProgIdx`, so the window match is again far (> 60 m), the global re-acquire
fires again, and it re-selects the parallel-ahead leg. The tiny 8-point backward
allowance (`src/routes.cpp:302`) cannot claw progress back. `progIdx` therefore
stays inflated for the rest of the ride, `cumM[progIdx]` is too large,
`advanceManeuverCursor` skips several maneuvers, and `nextTurn` reports turns
that are still ahead — exactly the observed "several maneuvers ahead, for the
whole ride" behaviour. The drawn route is unaffected because it is rendered from
the raw point list, not from `progIdx`.

**Secondary/compounding: maneuver snapping was also an unbounded global search
(`mapManeuversToRoute`, `src/routes.cpp:66-73`, pre-fix).** It snapped each
maneuver to its globally nearest route vertex with no ordering constraint. On the
same self-passing routes, a maneuver on the return/overlapping leg could snap to
the *earlier* leg's vertices, giving it a `cumM[]` far smaller than its true
along-route position. `advanceManeuverCursor` then steps past that maneuver early
— independently producing the "skip several turns" symptom even when `progIdx` is
correct.

Both defects share the same failure mode: *an unbounded nearest-point search on a
route that comes back near itself.* This is the classic cause the ride report
describes.

---

## 3. The fix (drafted — uncommitted in this worktree)

All changes are self-contained in `src/routes.cpp`; no header or API change.

### (a) Constrain the re-acquire in `updateProgress` — `src/routes.cpp:294`

- Added persistent off-route state `offRouteFixes` (`src/routes.cpp:37`).
- The whole-route re-acquire now fires **only** after the window match has been
  far for **≥ 4 consecutive fixes** (a real detour / mid-route join, not a single
  jittery sample), **and** only if the global candidate is **decisively** closer
  — `fd2 < d2 * 0.25f`, i.e. less than *half* the distance (values are squared
  metres). A merely-marginally-closer parallel leg can no longer steal progress.
  On any on-route fix the counter resets.

This keeps normal tracking driven purely by the bounded, monotonic forward
window (immune to self-passing geometry), while still allowing genuine
re-acquisition after a sustained detour.

### (b) Monotonic maneuver snapping — `mapManeuversToRoute`, `src/routes.cpp:66`

Maneuvers arrive in route order, so each snap search now starts at the previous
maneuver's snapped index and runs forward (`from = idx` after each). A return-leg
maneuver can no longer snap back onto an earlier overlapping leg, so its `cumM[]`
is always its true along-route position.

### Why this makes next-turn strictly position-driven

`progIdx` now advances monotonically along the rider's actual leg and cannot leap
onto a parallel later leg on a jittery fix; `maneuverIdx[]` is monotonic along the
route. `advanceManeuverCursor` (`src/routes.cpp:78`) and `nextTurn`
(`src/routes.cpp:381`) already compute correctly *given* trustworthy indices —
they needed no change once the two index sources were made robust.

---

## 4. Verification

**Reasoning walkthrough — out-and-back that overlaps (the reported scenario):**
Rider outbound, `progIdx` tracks via the forward window (global fallback never
needed; window match < 60 m). A single fix drifts 70 m off (jitter). *Before:*
global re-acquire fires immediately, finds the overlapping return leg ~5 m away,
`progIdx` jumps ~half the route ahead and latches → turns run many maneuvers
ahead. *After:* one far fix only increments `offRouteFixes` to 1; the window
still yields the (slightly far) correct outbound point, `progIdx` stays on the
true leg; the next on-route fix resets the counter. Prompts stay locked to the
rider.

**Reasoning walkthrough — genuine detour / mid-route join:** rider off-route for
several seconds. `offRouteFixes` climbs past 4; the global match is many times
closer than the stale window match (`fd2 < d2/4`), so the re-acquire fires and
`progIdx` correctly re-locks. Behaviour preserved.

**Suggested host-side test (no hardware):** `nearestRouteIdx`, `cumM[]`,
`advanceManeuverCursor`, `mapManeuversToRoute`, and the `updateProgress`
projection are plain math with no Arduino/SD/BLE dependency. Extract them into a
tiny host harness, build a synthetic out-and-back polyline (outbound then the
reversed outbound offset a few metres, with maneuvers on both legs), feed a
sequence of positions along the outbound leg with one or two injected > 60 m
jitter fixes, and assert `progressIndex()` stays monotonic on the outbound half
and `nextTurn` returns the correct next maneuver + a distance that decreases
monotonically toward each turn. Repeat with a sustained off-route excursion to
confirm re-acquire still recovers.

**Tunables:** `offRouteFixes >= 4` (≈ 4 s at 1 Hz) and the `0.25f` closeness
ratio are conservative; both can be relaxed if field logs show re-acquisition is
too sluggish for a particular routing profile.
