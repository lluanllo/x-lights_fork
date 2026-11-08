#pragma once

#include <string>
#include <vector>

// Entry points behind --composer-generate and --composer-pipeline-test. Neither opens a
// window; both only create new files and never touch a layout or a networks file.
namespace composer {

struct GenerateCommandOptions {
    std::string audioPath;
    std::string showDir;
    std::string sequenceName;
    std::string reportDir;
    std::string profilePath;
    std::string rolesPath;
    std::string templatesDir;
    std::string cacheDir;
    std::string lyricsPath;
    std::string timingPath;
    std::string timingTrack;
    std::string lyricSessionPath;
    std::string correctionsPath;
    std::string previousSidecarPath;
    std::string themesPath;
    std::string characterPath;
    std::string transcriptPath;
    std::string level; // empty: the default or the previous sidecar's
    std::string seed;  // empty: likewise
    std::string stopAfter; // a stage id; empty: every stage
    std::string cancelFile; // the generation stops if this file appears
    bool render = true;
    int renderTimeoutMS = 0;
    std::string mediaFolders; // separated by ';'
    bool scanMedia = false;
    bool mediaManualOnly = false;
    std::string mediaShare; // 0-1; empty: by level
    bool copyMedia = false; // the person agreed to copy files outside the show into it
    bool analyzeMedia = false; // look at the files again before planning
    std::string visionModel;   // the image-text model's folder, for --analyze-media
    std::string mediaVariant;  // another montage from the same files
    std::string lyricsMode;    // "lyrics-first", "smart" or "free"
    std::string screens;       // "Name=role;Name=role", roles as in screens.json
};

// Returns the process exit code: 0 published, 1 failed, 2 refused, 3 cancelled.
[[nodiscard]] int RunGenerateCommand(const GenerateCommandOptions& options);

// Pictures of what a render drew on some models at some times, one PNG per model and
// time, laid out on each model's own buffer: evidence a person can look at, not a check.
struct SnapshotOptions {
    std::string showDir; // a copy with every controller inactive
    std::string fseqPath;
    std::vector<std::string> models;
    std::vector<int> timesMS;
    std::string outDir;
    int scale = 8;
};

// Returns 0 written, 1 failed, 2 refused.
[[nodiscard]] int RunSnapshotCommand(const SnapshotOptions& options);

struct PipelineTestOptions {
    std::string outDir;
    std::string showDir;   // a copy of a real show with every controller inactive; empty: synthetic only
    std::string audioPath; // a real song for the runs on that show
    std::string lyricsPath;
    std::string timingPath;
    std::string level; // empty: the three levels
};

// The synthetic cases on a fixture show and WAV files written under a new folder of
// `outDir`, then, with a show and a song, the three levels on that show. Returns 0
// when every check passed, 1 otherwise.
[[nodiscard]] int RunPipelineTest(const PipelineTestOptions& options);

} // namespace composer
