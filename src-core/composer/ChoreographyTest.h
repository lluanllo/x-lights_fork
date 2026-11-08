#pragma once

#include <cstdint>
#include <string>

// Internal entry point behind --composer-choreography-test. Writes no sequence into a
// real show and connects to nothing; the SequenceWriter check runs on an isolated
// synthetic fixture under `outDir`.
namespace composer {

struct ChoreographyTestOptions {
    std::string outDir;
    std::string showDir;      // optional: also measure on this show (a copy, controllers inactive)
    std::string planPath;     // optional: choreograph this composition-plan.json against `showDir`
    std::string profilePath;  // optional
    std::string templatesDir; // optional: default resources/composer/templates
    std::string level;        // optional: conservative, balanced, creative
    uint64_t seed = 0;        // 0: the plan's
};

// Returns 0 when every check passed, 1 otherwise, 2 refused.
[[nodiscard]] int RunChoreographyTest(const ChoreographyTestOptions& options);

} // namespace composer
