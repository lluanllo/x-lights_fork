#pragma once

#include "composer/ComposerTypes.h"

#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct SongStructureRegion;

// Turns an audio file into a timeline the later Composer stages can read. It only
// reads the audio: no effects, no sequences, and nothing shared with the player,
// because every analysis decodes into an AudioManager of its own.
//
// Results are repeatable with the same binary on the same machine. Nothing is
// promised across platforms, whose decoders and sample rates differ.
namespace composer {

inline constexpr int ANALYSIS_SCHEMA_VERSION = 1;
inline constexpr const char* ANALYZER_VERSION = "1.1.0";

// Quality on real music has never been measured, and only a reference reviewed by
// a person can change that.
inline constexpr const char* MUSIC_QUALITY_NOT_VALIDATED = "NOT_VALIDATED";

enum class MusicalEventType {
    Beat,
    Downbeat,
    Onset,
    SectionStart,
    SectionEnd,
    Climax,
    Silence,
    VocalStart,
    VocalEnd,
    Ending
};

enum class SectionKind {
    Intro,
    Verse,
    Chorus,
    Bridge,
    Instrumental,
    Outro,
    Unknown,
    PreChorus, // after Unknown, so the ids written before keep their numbers
    Interlude
};

// Where a value came from, so that a new analysis can tell what it may replace.
enum class Provenance {
    Detected,
    Corrected,
    Locked,
    Confirmed
};

struct MusicalEvent {
    int timeMS = 0;
    int durationMS = 0;
    MusicalEventType type = MusicalEventType::Onset;
    double intensity = 0; // 0-1, relative to this song
    double confidence = 0; // 0-1
    int sectionIndex = -1;
    std::string source;
    int beatIndex = -1;   // beats and downbeats only, when known
    int barPosition = -1; // 0 on a downbeat, -1 when the bar is not known
    Provenance provenance = Provenance::Detected;
};

struct SongSection {
    int startMS = 0;
    int endMS = 0;
    SectionKind kind = SectionKind::Unknown;
    int repeatGroup = -1; // same music, same group; -1 when it does not repeat
    double energy = 0;
    double confidence = 0;
    std::vector<std::string> evidence;
    Provenance provenance = Provenance::Detected;
};

struct EnergyPoint {
    int timeMS = 0;
    double rms = 0;       // 0-1 of full scale, comparable between songs
    double total = 0;     // 0-1, relative to this song
    double low = 0;       // below about 250 Hz
    double mid = 0;       // about 250 Hz to 2 kHz
    double high = 0;      // above about 2 kHz
    double variation = 0; // how much the spectrum moved since the previous point
    double intensity = 0; // loudness and activity together, not loudness alone
};

struct TempoPoint {
    int timeMS = 0;
    double bpm = 0;
    double confidence = 0;
};

// One reading of the pulse. The octave of a pulse is the classic doubt: the same song
// can be counted at 105 or at 210, and both readings fit its attacks.
struct TempoCandidate {
    double bpm = 0;
    double confidence = 0; // share of the fit among the readings listed
    std::string reason;    // "followed", "half", "double"
};

struct TimeRange {
    int startMS = 0;
    int endMS = 0;
};

struct VocalSegment {
    int startMS = 0;
    int endMS = 0;
    double confidence = 0;
    std::string method;
};

// What a person changed. Stored apart from what was detected, and applied again
// after every analysis: a new analysis replaces detections and never these.
struct EventCorrection {
    enum class Action {
        Add,
        Move,
        Remove,
        Confirm
    };
    Action action = Action::Add;
    MusicalEventType type = MusicalEventType::Beat;
    int originalMS = -1; // the detected event, for everything but Add
    int timeMS = 0;
    int durationMS = 0;
    bool locked = true;
};

struct SectionCorrection {
    int startMS = 0;
    int endMS = 0;
    SectionKind kind = SectionKind::Unknown;
    bool locked = true;
};

struct BarCorrection {
    int firstDownbeatMS = -1; // -1: no correction
    int beatsPerBar = 4;
    bool locked = true;
};

struct AnalysisCorrections {
    std::vector<EventCorrection> events;
    std::vector<SectionCorrection> sections;
    BarCorrection bar;
    // A person hearing the pulse at half or double what was followed: 0.5 keeps every
    // other beat, 2 adds one between each pair. 1 changes nothing.
    double tempoFactor = 1;
};

struct AnalysisOptions {
    int energyIntervalMS = 100;
    double onsetSensitivity = 1.5; // DetectOnsets' threshold: higher finds fewer
    double minOnsetIntensity = 0.05; // weaker onsets are noise to a choreography
    double minBpm = 60;
    double maxBpm = 200;
    double silenceDb = -50; // below this level of full scale counts as silent
    int minSilenceMS = 1200; // longer than a beat at the slowest tempo, or the gaps between sparse notes would count
    int minSectionMS = 8000;
    int beatsPerBarOverride = 0; // 0: estimate between 3 and 4
    bool estimateVocals = true;
};

struct SongAnalysis {
    int schemaVersion = ANALYSIS_SCHEMA_VERSION;
    std::string analyzerVersion = ANALYZER_VERSION;
    std::string musicQuality = MUSIC_QUALITY_NOT_VALIDATED;
    std::string audioHash; // SHA-256 of the file's bytes
    std::string optionsKey;
    std::string fileName;
    long fileSize = 0;
    int durationMS = 0;
    int sampleRate = 0;
    int sourceSampleRate = 0;
    int channels = 0;
    std::map<std::string, std::string> metadata;
    double globalBpm = 0;
    double bpmConfidence = 0;
    double initialBpm = 0; // DetectTempo's single estimate, kept to compare
    double initialBpmConfidence = 0;
    bool tempoIsStable = false;
    int beatsPerBar = 0; // 0: not determined
    double barConfidence = 0;
    int musicStartMS = 0;
    int musicEndMS = 0;
    std::string key;
    std::vector<MusicalEvent> events; // sorted by time, then by type
    std::vector<SongSection> sections;
    std::vector<VocalSegment> vocalSegments;
    std::vector<EnergyPoint> energy;
    std::vector<TempoPoint> tempo;
    std::vector<TempoCandidate> tempoCandidates; // the followed reading first
    double appliedTempoFactor = 1; // what corrections.tempoFactor already did to the beats
    std::vector<TimeRange> lowEnergy;
    std::vector<std::string> warnings; // stable codes
    AnalysisCorrections corrections;
};

enum class AnalysisStatus {
    Ok,
    Cancelled,
    Failed
};

struct AnalysisResult {
    AnalysisStatus status = AnalysisStatus::Failed;
    std::string errorCode;
    std::string errorDetail;
    bool reusedCache = false;
    SongAnalysis analysis;
    std::map<std::string, long long> stageMS; // cost of each stage, for measurements
};

using AnalysisProgress = std::function<void(int percent, const char* stage)>;
using AnalysisCancel = std::function<bool()>;

// Blocking. Call it from a worker, or use SongAnalyzer below from a UI thread.
[[nodiscard]] AnalysisResult AnalyzeSong(const std::string& audioPath, const AnalysisOptions& options = {}, const AnalysisProgress& progress = {}, const AnalysisCancel& cancel = {});

// Reuses `jsonPath` when it was made from these same bytes, analyzer and options.
// Otherwise analyzes again, keeping the corrections stored in it, and replaces the
// file only once the new content is complete. Without one, nothing is written.
[[nodiscard]] AnalysisResult AnalyzeSongCached(const std::string& audioPath, const std::string& jsonPath, const AnalysisOptions& options = {}, const AnalysisProgress& progress = {}, const AnalysisCancel& cancel = {});

// One analysis on a thread of its own, so that a UI can poll it and stay responsive.
// Destroying it cancels the analysis and waits for it to stop.
class SongAnalyzer {
public:
    SongAnalyzer() = default;
    ~SongAnalyzer();
    SongAnalyzer(const SongAnalyzer&) = delete;
    SongAnalyzer& operator=(const SongAnalyzer&) = delete;

    void Start(const std::string& audioPath, const std::string& jsonPath, const AnalysisOptions& options = {});
    void Cancel() { _cancel = true; }
    [[nodiscard]] bool IsDone() const;
    [[nodiscard]] int Progress() const { return _progress; }
    [[nodiscard]] AnalysisResult Wait();

private:
    std::future<AnalysisResult> _worker;
    std::atomic<bool> _cancel{ false };
    std::atomic<int> _progress{ 0 };
};

// What the song is doing at an instant, for a choreography to read.
struct MusicSnapshot {
    int timeMS = 0;
    int sectionIndex = -1;
    SectionKind sectionKind = SectionKind::Unknown;
    double sectionConfidence = 0;
    EnergyPoint energy;
    int nearestBeatMS = -1;
    int nearestOnsetMS = -1;
    bool inSilence = false;
    // "unknown" is an answer: no estimate of the voice exists for this song.
    std::string vocalActivity = "unknown";
    double vocalConfidence = 0;
    double confidence = 0;
    std::vector<MusicalEvent> nearbyEvents; // what starts within 250 ms, beats and onsets aside
};

[[nodiscard]] MusicSnapshot SnapshotAt(const SongAnalysis& analysis, int timeMS);

// Applies the corrections stored in `analysis`. Safe to apply more than once.
void ApplyCorrections(SongAnalysis& analysis);

// Only the section corrections, for when the sections were laid out again after the
// analysis, as the lyrics do. Safe to apply more than once.
void ApplySectionCorrections(SongAnalysis& analysis);

[[nodiscard]] std::string ToJson(const SongAnalysis& analysis);
[[nodiscard]] bool FromJson(const std::string& text, SongAnalysis& analysis, std::string& error);
[[nodiscard]] std::string OptionsKey(const AnalysisOptions& options);
[[nodiscard]] std::string Sha256OfFile(const std::string& path, const AnalysisCancel& cancel = {});
[[nodiscard]] const char* EventTypeId(MusicalEventType type);
[[nodiscard]] const char* SectionKindId(SectionKind kind);
[[nodiscard]] const char* ProvenanceId(Provenance provenance);

// A timing track ready to import. `id` is stable; `name` is only the default the
// file carries, and a UI can replace it with a translation.
struct TimingTrackExport {
    std::string id;
    std::string name;
    std::vector<TimingMark> marks;
};

// Marks fall on the frames of `frameMS`, since xLights would truncate them otherwise.
[[nodiscard]] std::vector<TimingTrackExport> BuildTimingTracks(const SongAnalysis& analysis, int frameMS);
[[nodiscard]] bool WriteXTiming(const std::string& path, const std::vector<TimingTrackExport>& tracks, std::string& error);

// The sections in the form of xLights' own song structure view.
[[nodiscard]] std::vector<SongStructureRegion> BuildSongStructure(const SongAnalysis& analysis);

// A person's review of a real song, kept outside the repository next to the audio:
// { "beatsMS": [...], "downbeatsMS": [...], "sectionBoundariesMS": [...] }
// F-measures are in 0-1, and -1 where the reference has nothing to compare.
struct ReferenceScore {
    double beatF = -1;
    double downbeatF = -1;
    double sectionF = -1;
};

inline constexpr int REFERENCE_BEAT_TOLERANCE_MS = 70;
inline constexpr int REFERENCE_SECTION_TOLERANCE_MS = 3000;

[[nodiscard]] bool ScoreAgainstReference(const SongAnalysis& analysis, const std::string& referencePath, ReferenceScore& score, std::string& error);

} // namespace composer
