#pragma once

#include <string>

// Internal entry point behind --composer-understanding-test: what the recording says
// (silence, speech, singing, a choir, instruments, attacks, drops, tails, noise, broken and
// short files), the voices, the figures' geometry and parts, who performs what, the
// timeline and its corrections, what the render did against what was meant, and what is
// kept on disk. Synthetic audio and the synthetic fixture show only.
namespace composer {

// Returns 0 when every check passed, 1 otherwise.
[[nodiscard]] int RunUnderstandingTest(const std::string& workDir);

// --composer-understand: what a recording says (and its stems, "role=path;role=path",
// roles voice, chorus, narration, music) written to `outDir` as understanding.json,
// voices.json and timeline.json, with a plain report. With `showDir` the reading is also
// kept with the show (ShowComposer/understanding), reused while it is current, and the
// person's corrections there are applied. Nothing leaves the computer. `cancelFile`: it
// stops when that file appears. Returns 0 written, 1 failed, 3 cancelled.
[[nodiscard]] int RunUnderstandCommand(const std::string& audioPath, const std::string& stems, const std::string& outDir, const std::string& showDir = "",
                                       const std::string& cancelFile = "", bool again = false);

// --composer-figures: the figures of a show (-s, a copy with every controller inactive):
// geometry and parts, as figures.json, figure-semantics.json and a plain report.
[[nodiscard]] int RunFiguresCommand(const std::string& showDir, const std::string& outDir);

} // namespace composer
