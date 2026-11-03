#pragma once

#include "composer/ComposerTypes.h"
#include "composer/LyricDocument.h"
#include "composer/SongAnalysis.h"
#include "composer/SpanishPhonemizer.h"

#include <string>
#include <vector>

// Gives lyrics their times: phrases, then words, then mouth shapes.
//
// Where a time comes from decides how far it can be trusted, so every element keeps
// its source and confidence. Phrase times come from a synced track, from marks a
// person made while listening, or from LRC and SRT lines. Words and phonemes inside a
// phrase are only an estimate by syllables, and say so. Nothing is spread evenly over
// the song and presented as synchronized.
//
// Corrections a person made are kept apart and win over anything detected, on every
// new alignment. One whose target no longer exists is reported, never dropped.
namespace composer {

inline constexpr int LYRIC_ALIGNMENT_SCHEMA_VERSION = 1;

enum class AlignmentProvider {
    Auto,          // the best of the others that the input allows
    ExistingTrack, // A: a synced xLights track, kept as it is
    ManualPhrases, // B: phrase marks made by a person
    LineTimes,     // C: LRC or SRT line times
    Experimental,  // D: voice activity and syllables; only on request
    None           // text without times: nothing is timed
};

struct AlignedPhrase {
    std::string id; // the LyricLine id
    std::string text;
    std::string voice;
    int startMS = -1; // -1: not synchronized
    int endMS = -1;
    bool instrumental = false;
    double confidence = 0;
    std::string source; // "track", "manual", "lrc", "srt", "experimental"
    Provenance state = Provenance::Detected;
    int originalStartMS = -1; // what was detected, when a correction moved it
    int originalEndMS = -1;
};

struct AlignedWord {
    std::string id; // "<phrase id>/<index>:<key>"
    std::string phraseId;
    std::string text;
    std::string key;
    int startMS = 0;
    int endMS = 0;
    double confidence = 0;
    std::string source; // "track", "syllables", "manual"
    Provenance state = Provenance::Detected;
    std::string pronunciation; // "rules", "exceptions", "track" or "none"
    std::string phonemes;      // PhonemeString, for review
    int originalStartMS = -1;
    int originalEndMS = -1;
};

struct AlignedPhoneme {
    std::string id; // "<word id>/<index>", or "rest:<start>" for a pause
    std::string wordId;
    std::string shape;   // what FacesEffect draws: AI E FV L MBP O U WQ etc rest
    std::string phoneme; // the sound it stands for; several for a merged shape, "" for rest
    int startMS = 0;
    int endMS = 0;
    double confidence = 0;
    Provenance state = Provenance::Detected;
};

struct LyricCorrection {
    enum class Level {
        Phrase,
        Word,
        Phoneme
    };
    Level level = Level::Phrase;
    std::string targetId;
    Provenance state = Provenance::Confirmed; // never Detected
    int startMS = -1;
    int endMS = -1;
    std::string shape;      // phonemes only: a replacement shape, empty to keep it
    int originalStartMS = -1; // what the person saw before changing it
    int originalEndMS = -1;
};

struct AlignmentConflict {
    std::string targetId;
    std::string reason; // "target-missing", "shape-invalid", "outside-phrase"
};

struct LyricAlignment {
    int schemaVersion = LYRIC_ALIGNMENT_SCHEMA_VERSION;
    AlignmentProvider provider = AlignmentProvider::None;
    // "synced", "manual", "approximate", "experimental", "unsynchronized" or "none".
    std::string quality = "none";
    std::string musicQuality = MUSIC_QUALITY_NOT_VALIDATED;
    std::string language;
    std::vector<AlignedPhrase> phrases;
    std::vector<AlignedWord> words;
    std::vector<AlignedPhoneme> phonemes; // pauses included, as "rest"
    std::vector<LyricCorrection> corrections;
    std::vector<AlignmentConflict> conflicts;
    std::vector<std::string> warnings;
};

struct AlignerOptions {
    int frameMS = 25;
    PhonemizerOptions phonemizer;
    bool allowExperimental = false;
    int consonantMS = 60;      // how long a consonant's shape shows inside a syllable
    double phraseFinalHold = 1.5; // singers hold the last syllable of a line
    int lrcMaxSyllableMS = 600;   // an LRC line ends at the next one, or after this per syllable
    double minConfidence = 0.5;   // below this a phrase is reported for review
};

struct AlignerInput {
    const LyricDocument* document = nullptr;
    AlignmentProvider provider = AlignmentProvider::Auto;
    const TimingTrackSpec* existingTrack = nullptr; // provider A
    const SongAnalysis* analysis = nullptr;         // provider D
    int durationMS = 0;                             // 0: unknown
    std::vector<LyricCorrection> corrections;       // manual marks are phrase corrections
};

[[nodiscard]] LyricAlignment AlignLyrics(const AlignerInput& input, const AlignerOptions& options = {});

// Phrases, words and phonemes, the three layers FacesEffect reads, in that order.
// Unsynchronized phrases are left out rather than given made-up times.
[[nodiscard]] TimingTrackSpec BuildLyricTrack(const LyricAlignment& alignment, const std::string& trackName);

// Every layer of every track, in the format xLights imports as "Import Timing Track".
[[nodiscard]] bool WriteTimingTracks(const std::string& path, const std::vector<TimingTrackSpec>& tracks, std::string& error);

[[nodiscard]] const char* ProviderId(AlignmentProvider provider);
[[nodiscard]] const char* CorrectionLevelId(LyricCorrection::Level level);

// What the sync dialog keeps next to a sequence: the lyrics as written and the
// corrections, so that generating again never loses what a person marked.
struct LyricSession {
    int schemaVersion = LYRIC_ALIGNMENT_SCHEMA_VERSION;
    std::string trackName;
    std::string lyrics; // the text as given, in whatever format it came
    std::vector<LyricCorrection> corrections;
};

[[nodiscard]] std::string ToJson(const LyricAlignment& alignment);
[[nodiscard]] bool FromJson(const std::string& text, LyricAlignment& alignment, std::string& error);
[[nodiscard]] std::string ToJson(const LyricSession& session);
[[nodiscard]] bool FromJson(const std::string& text, LyricSession& session, std::string& error);
[[nodiscard]] bool SaveLyricSession(const std::string& path, const LyricSession& session, std::string& error);
[[nodiscard]] bool LoadLyricSession(const std::string& path, LyricSession& session, std::string& error);

// Adds or replaces the correction for that target, keeping the first original time.
void UpsertCorrection(std::vector<LyricCorrection>& corrections, const LyricCorrection& correction);

} // namespace composer
