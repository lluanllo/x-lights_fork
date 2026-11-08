#pragma once

#include <string>

// Internal entry point behind --composer-learning-test: a person's review of a show's
// pictures and videos, the explicit approval, the sidecar that keeps them, a person's
// labels on the library, the model measured against them, and the preferences learned
// from reviews. Everything is built by the test under a new folder of `workDir`. 0 when
// every check passed.
namespace composer {

[[nodiscard]] int RunLearningTest(const std::string& workDir);

} // namespace composer
