#pragma once

#include <string>

// Internal entry point behind --composer-learning-test: a person's review of a show's
// pictures and videos, the explicit approval, the sidecar that keeps them, a person's
// labels on the library, the model measured against them, and the preferences learned
// from reviews. Everything is built by the test under a new folder of `workDir`. 0 when
// every check passed.
namespace composer {

[[nodiscard]] int RunLearningTest(const std::string& workDir);

// Internal tool behind --composer-review: applies the decisions of `decisionsFile` to
// the review of `sequencePath` (its sidecar; the storyboard from `reportDir`), keeps it,
// learns from it in the show's preferences and writes the corrections that make the
// next version (for --composer-generate --corrections) to `correctionsOut`. It never
// approves a show: only a person does, in the window. Decisions file:
//   { "author": "...", "clips": [ { "decision": "<id>" | "file": "<name>", "lane": "<screen>",
//     "state": "approved|rejected|needs-fix|pending", "flags": [...], "note": "...",
//     "replace": "<file name or media id>" } ] }
[[nodiscard]] int RunReviewCommand(const std::string& showDir, const std::string& sequencePath, const std::string& reportDir, const std::string& decisionsFile, const std::string& correctionsOut);

} // namespace composer
