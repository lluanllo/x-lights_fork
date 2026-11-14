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

} // namespace composer
