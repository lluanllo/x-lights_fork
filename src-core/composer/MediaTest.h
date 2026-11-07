#pragma once

#include <string>

// Internal entry point behind --composer-media-test. Every picture, animation and
// video it reads is made by the test itself under a new folder of `workDir`.
namespace composer {

// The media library: safe folder search, formats, damaged, missing, moved and
// Unicode-named files, thumbnails, categories, copies and the saved library.
// 0 when every check passed.
[[nodiscard]] int RunMediaTest(const std::string& workDir);

// Writes the test's original pictures, animation and video under `folder`, for
// other tests and for a person to try the window with. False when one could not be made.
bool WriteMediaFixtures(const std::string& folder, std::string& error);

} // namespace composer
