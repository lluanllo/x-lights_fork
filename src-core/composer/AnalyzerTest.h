#pragma once

#include <string>

// Internal entry points behind --composer-analyze-song, --composer-analyzer-test and
// --composer-analyzer-bench. None opens a window or touches a sequence or a layout.
namespace composer {

struct AnalyzeCommandOptions {
    std::string audioPath;
    std::string outPath;     // empty: <audio>.composer-analysis.json
    std::string xtimingPath; // empty: no timing file
    std::string reportPath;  // empty: summary on stdout only
    std::string cancelFile;  // the analysis stops if this file appears
    std::string referencePath;
    int energyIntervalMS = 100;
    double onsetSensitivity = 1.5;
    int frameMS = 25;
};

// Returns the process exit code: 0 done, 1 failed, 2 refused, 3 cancelled.
[[nodiscard]] int RunAnalyzeCommand(const AnalyzeCommandOptions& options);

// Every case runs on WAV files it writes itself under a new folder of `workDir`: no
// music is involved. Returns the process exit code: 0 all passed, 1 otherwise.
[[nodiscard]] int RunAnalyzerTest(const std::string& workDir);

// Times each stage on synthetic songs of 3, 5 and 10 minutes, kept in `workDir` so
// that their peak memory can be measured from outside with --composer-analyze-song.
[[nodiscard]] int RunAnalyzerBench(const std::string& workDir);

} // namespace composer
