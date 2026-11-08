#pragma once

#include <string>

// Internal entry point behind --composer-visual-test. Every picture and video it reads
// is drawn by the test itself under a new folder of `workDir`.
namespace composer {

// The visual analysis (colours, light, point of interest, duplicates, how videos move
// and cut, suitability, the cache), the framing on a matrix and a 360 degree mega tree,
// and the visual storyboard. With `modelDir` (a folder holding the image-text model)
// it also checks the tokenizer and the model's concepts; without it, it says that no
// semantic analysis was checked. 0 when every check passed.
[[nodiscard]] int RunVisualTest(const std::string& workDir, const std::string& modelDir);

// Writes the test's original pictures and videos under `folder`, for other tests and
// for a person to try the window with. False when one could not be made.
bool WriteVisualFixtures(const std::string& folder, std::string& error);

} // namespace composer
