#pragma once

#include "composer/Choreography.h"
#include "composer/MusicAssist.h"
#include "composer/VisualDirection.h"
#include "composer/ComposerTypes.h"
#include "composer/CompositionPlan.h"
#include "composer/GenerationSidecar.h"
#include "composer/GenerationValidator.h"
#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/ModelCapabilities.h"
#include "composer/SongAnalysis.h"
#include "composer/TemplateLibrary.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class HeadlessRenderContext;

// From a song to a native xLights sequence: every Composer phase, in order, each one
// keeping its data and its findings, and nothing published until the file has been
// reopened, validated and rendered.
//
// Each stage is a function of its own over the same GenerationRun, so a stage can be
// run, inspected and repeated alone; GenerateShow only calls them in order and stops
// at the first that fails. Nothing here writes to the layout or the networks file,
// and nothing overwrites a file: the only files created are the new sequence, its
// sidecar and fseq, the analysis cache and the diagnostics in the report folder.
namespace composer {

inline constexpr const char* PIPELINE_VERSION = "1.0.0";

enum class GenerationStage {
    Inputs,
    Layout,
    Analysis,
    Lyrics,
    Plan,
    Choreography,
    Spec,
    Write,
    Publish
};
inline constexpr int GENERATION_STAGE_COUNT = 9;

enum class StageStatus {
    NotRun,
    Ok,
    Warning, // done, with findings a person should read
    Failed,
    Cancelled
};

struct StageOutcome {
    StageStatus status = StageStatus::NotRun;
    long long ms = 0;
    std::vector<std::string> notes; // "code: detail", stable codes first
};

struct GenerationRequest {
    std::string audioPath;    // required; whatever AudioManager decodes
    std::string showDir;      // required: the show folder whose layout is used
    std::string sequenceName; // "<name>.xsq", published at <showDir>/<name>.xsq; never replaced
    std::string reportDir;    // required: analysis, plan, choreography, fseq and reports; must be new or empty
    std::string profilePath;  // empty: the neutral starter profile
    std::string rolesPath;    // empty: <showDir>/ShowComposer/roles.json when it exists
    std::string templatesDir; // empty: resources/composer/templates
    std::string cacheDir;     // empty: <showDir>/ShowComposer/cache
    std::string lyricsPath;   // plain text, LRC or SRT (UTF-8)
    std::string timingPath;   // an .xtiming holding a synchronized lyrics track
    std::string timingTrack;  // which track of timingPath; empty: the first with three layers
    std::string lyricSessionPath; // the marks of the "Sync Lyrics" dialog
    std::string correctionsPath;  // a CorrectionSet file
    std::string previousSidecarPath; // regenerate: its corrections and locks are kept
    std::string themesPath;     // empty: resources/composer/themes.json
    std::string characterPath;  // a song description saved by the music assistant
    std::string transcriptPath; // a transcription saved by the music assistant: word times for the lyrics
    std::optional<AutomationLevel> level; // default: balanced, or the previous sidecar's
    std::optional<uint64_t> seed;         // default: 1, or the previous sidecar's
    int frameMS = 25;
    bool render = true;
    int renderTimeoutMS = 0; // 0: no limit
    AnalysisOptions analysisOptions;
    CorrectionSet corrections; // added to the files' and the previous sidecar's
    std::string lyricTrackName = "Composer Lyrics";
    std::string mediaCueTrackName = "Composer Media Cues";
    CancelCheck cancel;
    std::function<void(GenerationStage stage, const std::string& what)> progress;
};

// Who took part and what the render showed of it, to find a figure that the plan used
// and that never lit up.
struct TargetCoverage {
    std::string target;
    int plannedSegments = 0; // segments where the plan gave it something other than rest
    int events = 0;
    int written = 0;
    int effectsLit = 0;  // written effects whose channels lit in the render
    int framesLit = 0;
    std::string note; // why it has no event, when the choreography said
};

struct RenderSummary {
    bool ran = false;
    int frames = 0;
    int expectedFrames = 0;
    int stepMS = 0;
    uint32_t channels = 0;
    uint32_t layoutChannels = 0;
    long long fileBytes = 0;
    long long renderMS = 0;
};

// Everything one generation made and found, stage by stage.
struct GenerationRun {
    GenerationRequest request;
    std::array<StageOutcome, GENERATION_STAGE_COUNT> stages{};
    bool ok = false;
    bool cancelled = false;
    std::vector<std::string> problems;

    // Inputs
    std::string sequencePath;
    std::string sidecarPath;
    std::string fseqPath;
    std::string protectedHashes; // the layout and networks files, before anything ran
    GenerationSidecar previous;
    bool hasPrevious = false;
    CorrectionSet corrections; // everything that will be applied

    // Layout
    std::unique_ptr<HeadlessRenderContext> context;
    ModelCapabilities capabilities;
    MetadataLookup metadata;
    TemplateLibrary library;
    StyleProfile profile;
    std::string layoutFingerprint;

    // Analysis
    SongAnalysis analysis;
    bool analysisFromCache = false;
    std::string analysisCachePath;

    // Lyrics
    LyricDocument lyrics;
    LyricAlignment alignment;
    LyricSession lyricSession;
    TimingTrackSpec lyricTrack; // empty layers: nothing synchronized to write
    VoiceCast cast;             // the voices with their faces; empty: one unnamed voice
    std::vector<TimingTrackSpec> voiceTracks;
    bool hasLyrics = false;

    // Plan and choreography
    CompositionPlan plan;
    ShowVisualDirection direction;
    DirectionMetrics directionMetrics;
    ThemeCatalog themes;
    SongCharacter character;
    bool hasCharacter = false;
    Transcript transcript;
    TranscriptAlignment transcriptAlignment;
    Choreography choreography;
    ValidationReport planValidation;
    ValidationReport choreographyValidation;

    // Sequence
    SequenceSpec spec;
    std::map<std::string, std::string> trackNames; // stable id -> name
    WriteResult write;
    ValidationReport renderValidation;
    RenderSummary render;
    std::vector<TargetCoverage> coverage;
    std::vector<std::string> findings; // what the checks after the render saw, "code: detail"
    std::string resavedPath;
    GenerationSidecar sidecar;

    GenerationRun();
    ~GenerationRun();
    GenerationRun(GenerationRun&&) noexcept;
    GenerationRun& operator=(GenerationRun&&) noexcept;

    [[nodiscard]] StageOutcome& Stage(GenerationStage stage) { return stages[static_cast<int>(stage)]; }
    [[nodiscard]] const StageOutcome& Stage(GenerationStage stage) const { return stages[static_cast<int>(stage)]; }
};

// The stages, in order. Each returns false when the run cannot go on, with the reason
// in its StageOutcome and in `problems`.
[[nodiscard]] bool CheckInputs(GenerationRun& run);
[[nodiscard]] bool LoadLayout(GenerationRun& run);
[[nodiscard]] bool AnalyzeAudio(GenerationRun& run);
[[nodiscard]] bool PrepareLyrics(GenerationRun& run);
[[nodiscard]] bool PlanShow(GenerationRun& run);
[[nodiscard]] bool ChoreographShow(GenerationRun& run);
[[nodiscard]] bool BuildSequence(GenerationRun& run);
// Writes, reopens, validates and renders the temporary file, and publishes the
// sequence and its sidecar only when all of that passed.
[[nodiscard]] bool WriteAndPublish(GenerationRun& run);
// Diagnostics into the report folder, whatever happened before.
void WriteDiagnostics(GenerationRun& run);

// Every stage up to `last`, then the diagnostics.
[[nodiscard]] GenerationRun GenerateShow(const GenerationRequest& request, GenerationStage last = GenerationStage::Publish);

[[nodiscard]] const char* StageId(GenerationStage stage);
[[nodiscard]] bool StageFromId(const std::string& id, GenerationStage& stage);
[[nodiscard]] const char* StageStatusId(StageStatus status);
// The run in words: stages, numbers, findings and what still needs a person.
[[nodiscard]] std::string GenerationReport(const GenerationRun& run);
[[nodiscard]] std::string SummaryJson(const GenerationRun& run);
// What was written, as a stable hash: effects and tracks in order, settings sorted.
[[nodiscard]] std::string SpecHash(const SequenceSpec& spec);

} // namespace composer
