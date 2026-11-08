#pragma once

#include "composer/SongAnalysis.h"

#include <string>

// Internal entry points behind --composer-model-capabilities and --composer-plan-test.
// Neither writes a sequence, touches a layout or connects to a controller.
namespace composer {

// Capabilities of the show in `showDir` (a copy with every controller inactive) as
// JSON at `outPath`, with a readable report and a roles.json template beside it.
// Reads <showDir>/ShowComposer/roles.json when it exists. Returns 0, 1 failed, 2 refused.
[[nodiscard]] int RunCapabilitiesCommand(const std::string& showDir, const std::string& outPath);

// The planner's cases on a synthetic layout and synthetic songs, written under a new
// folder of `workDir` with composition-plan.json and its report. With `showDir`, also
// measures extraction and planning on that show. Returns 0 all passed, 1 otherwise.
[[nodiscard]] int RunPlanTest(const std::string& workDir, const std::string& showDir);

// The synthetic material the planner test uses, shared with the choreography test:
// a layout of 11 made-up models, 5 groups and a submodel, and songs built in code.
[[nodiscard]] bool WritePlanFixture(const std::string& folder);
[[nodiscard]] SongAnalysis PlanTestSong(double secondVerseEnergy = 0.45);
[[nodiscard]] SongAnalysis PlanTestUnknownSong();
[[nodiscard]] SongAnalysis PlanTestSilentSong();
[[nodiscard]] SongAnalysis PlanTestLongSong(int sections);
[[nodiscard]] const char* PlanTestLyrics();

} // namespace composer
