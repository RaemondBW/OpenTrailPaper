# Archived analysis

Working documents from earlier investigations, recovered from agent worktrees
under `.claude/worktrees/` before those were deleted. They were never committed,
yet `src/power_mgmt.cpp` and `sdkconfig.defaults.pm` cited `CPU_SLEEP_SPIKE.md`
by name — a reference that pointed at nothing for anyone who cloned the repo.

Point-in-time, not maintained. Where they disagree with
`../battery-life.md` or `../pm-rebuild-baseline.md`, those are current.

| File | What it covers |
|---|---|
| `cpu-sleep-spike.md` | The original CPU light-sleep spike: why the stock Arduino framework compiles PM out, and what enabling it would take |
| `cpu-sleep-libbuild.md` | Rebuilding the Arduino S3 libraries with `CONFIG_PM_ENABLE` via esp32-arduino-lib-builder |
| `display-ghosting-analysis.md`, `display-ghosting-v3.md` | E-paper ghosting and grey drift |
| `battery-analysis.md` | Early power apportionment, superseded by the measurements in `../battery-life.md` |
| `navigation-analysis.md` | Route/turn-by-turn analysis, predating the algorithm fixes and host tests in `tools/route_test` |
