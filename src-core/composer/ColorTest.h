#pragma once

#include <string>

// Internal entry points for the colour direction: --composer-color-test (the colour
// space, harmonies, palettes, the colour director, gesture colours against their
// background, the measurement of a render, and the artistic analysis with a simulated
// service) and --composer-color-measure.
namespace composer {

// Returns 0 when every check passed, 1 otherwise.
[[nodiscard]] int RunColorTest(const std::string& workDir);

// What colours a rendered show (any version) really put on the installation, measured on
// the show folder's layout (a copy with every controller inactive). `scorePath` (a
// stage-score.json) and `chromaPath` (a color-score.json) may be empty. Writes
// color-check.json and color-check.txt to `outDir`. Returns 0 written, 1 failed, 2 refused.
[[nodiscard]] int RunColorMeasure(const std::string& showDir, const std::string& fseqPath, const std::string& scorePath, const std::string& chromaPath, const std::string& outDir);

} // namespace composer
