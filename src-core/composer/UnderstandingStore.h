#pragma once

#include "composer/AudioUnderstanding.h"
#include "composer/PerformanceMapping.h"
#include "composer/VocalUnderstanding.h"

#include <string>
#include <vector>

// Where a song's understanding lives, in <show>/ShowComposer/understanding/<audio id>/,
// each kind of thing in its own file so none can overwrite another:
//
//   reading.json      what the analysis heard (a cache: rewritten when the audio, the
//                     reader's version or its options change)
//   corrections.json  what a person corrected in it (never written by an analysis)
//   performers.json   which figures a person gave to each voice (never written by an analysis)
//   voices.json       the voices and interventions derived from all of the above (rewritten)
//
// Reverting keeps the person's files as "*.previous.json", so a revert can be undone.
namespace composer {

struct LyricAlignment;
struct LyricDocument;
struct VoiceCast;
struct Transcript;

inline constexpr int UNDERSTANDING_STORE_SCHEMA_VERSION = 1;

[[nodiscard]] std::string UnderstandingFolder(const std::string& showDir, const std::string& audioHash);

struct UnderstandRequest {
    std::string showDir;   // required
    std::string audioPath; // required
    std::vector<StemInput> stems;
    UnderstandingOptions options;
    bool reuse = true; // false: hear the audio again even if the reading is current
    const LyricAlignment* alignment = nullptr;
    const LyricDocument* lyrics = nullptr;
    const VoiceCast* cast = nullptr;
    const Transcript* transcript = nullptr;
    UnderstandingProgress progress;
    UnderstandingCancel cancel;
};

struct SongUnderstanding {
    std::string folder;
    bool fromCache = false;
    AudioUnderstanding audio; // with the person's corrections applied
    VocalUnderstanding voices;
    std::vector<PerformerAssignment> manual;
    std::vector<std::string> notes; // "reading-reused", "reading-written", "corrections-applied:<n>", "stale-reading:<why>"...
};

struct UnderstandOutcome {
    bool ok = false;
    bool cancelled = false;
    std::string errorCode;
    std::string errorDetail;
    SongUnderstanding song;
};

// Hears the song (or reuses its current reading), applies the person's corrections, and
// derives the voices. Blocking: call it from a worker.
[[nodiscard]] UnderstandOutcome UnderstandSong(const UnderstandRequest& request);
// The voices again, after a correction, without hearing the audio again.
void RefreshVoices(SongUnderstanding& song, const UnderstandRequest& request);

[[nodiscard]] bool SaveCorrections(const std::string& folder, const std::vector<UnderstandingCorrection>& corrections, std::string& error);
[[nodiscard]] bool SaveManualAssignments(const std::string& folder, const std::vector<PerformerAssignment>& manual, std::string& error);
[[nodiscard]] bool SaveDerived(const SongUnderstanding& song, std::string& error);
// Back to the automatic reading: the person's corrections and assignments are set aside
// (kept as *.previous.json); `restore` true brings them back.
[[nodiscard]] bool RevertUnderstanding(const std::string& folder, bool restore, std::string& error);

// "voice=C:/a.wav;chorus=C:/b.wav" and back. Roles: voice, chorus, narration, music.
[[nodiscard]] std::vector<StemInput> ParseStemList(const std::string& text);
[[nodiscard]] std::string StemListText(const std::vector<StemInput>& stems);

[[nodiscard]] bool ReadTextFile(const std::string& path, std::string& text);
[[nodiscard]] bool WriteTextFileAtomically(const std::string& path, const std::string& text, std::string& error);

} // namespace composer
