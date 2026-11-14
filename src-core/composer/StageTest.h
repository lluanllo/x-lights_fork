#pragma once

#include <string>

// Internal entry point behind --composer-stage-test: the light fields (every function,
// arrival times, fronts, limits, determinism), the stage space (transforms, anchors,
// calibration), the native Stage Field effect rendered by xLights' own engine (a wave that
// crosses figures of different shapes on time, layers that keep a figure's own colours
// and a protected mouth), the global director and its score. Synthetic fixture only.
namespace composer {

// Returns 0 when every check passed, 1 otherwise.
[[nodiscard]] int RunStageTest(const std::string& workDir);

// Behind --composer-stage-measure: what a rendered show (any version, made with or without
// the stage director) did with the installation, measured on the show folder's layout (a
// copy with every controller inactive). `scorePath` may be empty. Writes stage-check.json
// and stage-check.txt to `outDir`. Returns 0 written, 1 failed, 2 refused.
[[nodiscard]] int RunStageMeasure(const std::string& showDir, const std::string& fseqPath, const std::string& scorePath, const std::string& outDir);

} // namespace composer
