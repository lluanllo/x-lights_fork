#pragma once

#include <string>

// Internal entry points behind --composer-lyrics-test and --composer-faces-test. They
// use their own Spanish lines, never a song's lyrics.
namespace composer {

// Phonetics, lyrics import, alignment, corrections, validation and the old breakdown,
// under a new folder of `workDir`. Returns 0 when every check passed, 1 otherwise.
[[nodiscard]] int RunLyricsTest(const std::string& workDir);

// Writes a sequence whose face sings generated Spanish lyrics on a copy of a show,
// renders it, validates the render and saves pictures of chosen frames. Refuses to
// run unless every controller of that copy is inactive. Returns 0 passed, 1 failed,
// 2 refused.
[[nodiscard]] int RunFacesTest(const std::string& showDir, const std::string& destinationName);

} // namespace composer
