#pragma once
// The committed dev configuration (engine/host/dev/engine-dev.json): the first snapshot generation
// until Live Sets exist (Epic 4). It is the JSON mapping of the ws.v1 `State` message: up to 8 parts
// (timbre reference, insert chain, send levels, output), master settings and initial performance
// values. Unknown fields, unknown enum names and out-of-range values make the load fail.
#include <string>

#include "ws_v1.pb.h"

namespace velora::host {

// Reads and parses `path` into `out`. False with a message naming the file (and the JSON problem).
// Semantic validation (ranges, parts, classes) happens in ControlPlane::load_initial.
bool load_dev_config(const std::string& path, ws::v1::State& out, std::string& err);

// Same for JSON text (tests).
bool parse_dev_config(const std::string& json, ws::v1::State& out, std::string& err);

}  // namespace velora::host
