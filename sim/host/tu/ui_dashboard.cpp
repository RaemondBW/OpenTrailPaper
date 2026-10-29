// src/ui_dashboard.cpp as it is, plus read-only hooks for the control channel
// that need its file-local state. build.sh compiles this TU in place of
// src/ui_dashboard.cpp (and rebuilds it whenever that file changes).
//
// Hooks here only READ: the UI task owns this state; the control thread
// takes a snapshot that may be one frame stale, as a rider glancing at the
// panel would.
#include "../../../src/ui_dashboard.cpp"

#include <sstream>

#include "json.h"

// ---- segments (cov/nav) ---------------------------------------------------
// The SEGMENT page's matcher: what it would show for each relevant segment.
namespace sim {
std::string segmentsJson() {
    static const char* phases[] = {"idle", "armed", "active", "done", "abandoned"};
    const uint32_t now = millis();
    int list[seg::MAX_LIST];
    const int n = segMatcher.list(now, list, seg::MAX_LIST);
    std::ostringstream o;
    o.precision(10);
    o << "{\"count\":" << segments::count() << ",\"loaded\":" << segDb.count
      << ",\"phase\":\"" << phases[segMatcher.phase()] << "\",\"busy\":"
      << (segMatcher.busy() ? "true" : "false") << ",\"list\":[";
    for (int i = 0; i < n; ++i) {
        seg::View v;
        segMatcher.view(now, list[i], v);
        o << (i ? "," : "") << "{\"seg\":" << list[i] << ",\"name\":" << json::esc(v.name)
          << ",\"phase\":\"" << phases[v.phase] << "\",\"elapsed_ms\":" << v.elapsedMs
          << ",\"done_m\":" << v.doneM << ",\"len_m\":" << v.lenM << ",\"pr_s\":" << v.prSec
          << ",\"delta_ms\":" << (v.haveDelta ? std::to_string(v.deltaMs) : "null")
          << ",\"new_pr\":" << (v.newPr ? "true" : "false")
          << ",\"off_route\":" << (v.offRoute ? "true" : "false")
          << ",\"avg_power\":" << (v.avgPowerW == 0xFFFF ? -1 : (int)v.avgPowerW)
          << ",\"avg_hr\":" << (int)v.avgHrBpm
          << ",\"nearest\":" << (v.haveNearest ? json::esc(v.nearestName) : "null")
          << ",\"nearest_m\":" << v.nearestM << "}";
    }
    o << "]}";
    return o.str();
}
}  // namespace sim
// ---- end segments (cov/nav) -----------------------------------------------
