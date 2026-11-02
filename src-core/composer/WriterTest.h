#pragma once

#include "composer/ComposerTypes.h"

#include <string>

// Internal development test behind --composer-writer-test: writes a fixed
// synthetic sequence to a copy of a show. Not a sequence generator.
namespace composer {

[[nodiscard]] SequenceSpec BuildWriterTestSpec();

// Loading a show pings every active controller, so this refuses to run unless
// the folder's networks file has all of them inactive. That also keeps the test
// away from a real show folder, whose controllers are active.
[[nodiscard]] bool ControllersAreInactive(const std::string& showDir, std::string& error);

void PrintValidationSummary(const char* tool, const ValidationReport& report);

// Returns the process exit code: 0 written and verified, 1 failed, 2 refused.
[[nodiscard]] int RunWriterTest(const std::string& showDir, const std::string& destinationName);

} // namespace composer
