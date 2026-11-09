#pragma once

#include <string>

// Internal entry point behind --composer-recipe-test: recipes (model, persistence, a
// person's decisions, portable settings, templates), the artistic director and the
// colour engine, and a choreography that plays them, on the synthetic fixture.
namespace composer {

// Returns 0 when every check passed, 1 otherwise. `showDir` is not used yet.
[[nodiscard]] int RunRecipeTest(const std::string& workDir, const std::string& showDir);

} // namespace composer
