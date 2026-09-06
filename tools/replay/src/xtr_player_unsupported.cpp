// Stub implementation of the .xtr replay API for builds where it cannot be
// compiled.
//
// The real player (xtr_player.cpp) parses the trace with <rex/graphics/
// trace_protocol.h>, a public SDK header that existed in 0.8.1 but was REMOVED
// in 0.10.x. Forward-porting the parser to whatever replaced it is real work and
// is orthogonal to the macOS port, so off-Windows the two entry points resolve
// to this stub: NHL_REPLAY_XTR reports that it is unavailable instead of
// silently doing nothing, and nhllegacy_app.h needs no #ifdefs.
//
// Delete this file and build xtr_player.cpp once the parser is rebased.

#include "xtr_player.h"

#include <rex/logging.h>

namespace nhl::replay {
namespace {

ReplayStats Unsupported(const char* entry) {
  ReplayStats stats{};
  stats.error =
      "xtr replay is not available in this build: the SDK removed "
      "rex/graphics/trace_protocol.h in 0.10.x and the parser has not been "
      "rebased yet";
  REXLOG_ERROR("[nhl-replay] {} unavailable: {}", entry, stats.error);
  return stats;
}

}  // namespace

ReplayStats ReplayTrace(rex::graphics::GraphicsSystem&, const std::string&) {
  return Unsupported("ReplayTrace");
}

ReplayStats BenchmarkReplay(rex::graphics::GraphicsSystem&, const std::string&, int,
                            int) {
  return Unsupported("BenchmarkReplay");
}

}  // namespace nhl::replay
