#pragma once

#include <string>

// Internal entry points behind --composer-validate and --composer-validator-test.
// Neither is a user feature, and neither modifies a sequence, a layout or a network file.
namespace composer {

// Checks a saved sequence, and optionally its render, against the show folder.
// Without `useWriterSpec` the expectations come from the sequence itself.
// Returns the process exit code: 0 no errors, 1 errors found, 2 refused.
[[nodiscard]] int RunValidateCommand(const std::string& showDir, const std::string& xsqPath, const std::string& fseqPath, bool useWriterSpec);

// Runs every validator case on a small synthetic show written under `workDir`, in a
// new folder: no real layouts, music or commercial sequences are involved, so it can
// run anywhere. Returns the process exit code: 0 all cases passed, 1 otherwise.
[[nodiscard]] int RunValidatorTest(const std::string& workDir);

} // namespace composer
