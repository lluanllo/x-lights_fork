#pragma once

#include "composer/AudioUnderstanding.h"

#include <string>
#include <vector>

// Who speaks or sings, and when: voice tracks and their interventions, read from the
// acoustic activities, the voices the lyrics name (Phase 10), stems, and the words and
// mouth shapes of the alignment (Phases 4, 9 and 14).
//
// Diarization here is a local, experimental grouping of voices by pitch and timbre into
// "Voz A", "Voz B"...: it never identifies a real person, and it is not a separation of
// sources. A choir heard as a choir stays one track; its singers are never extracted.
namespace composer {

struct LyricAlignment;
struct LyricDocument;
struct VoiceCast;
struct Transcript;

inline constexpr int VOCAL_UNDERSTANDING_SCHEMA_VERSION = 1;

struct VoiceTrack {
    std::string id;    // "voice:A", "chorus", "stem:<name>", "lyric:<voice id>"
    std::string label; // "Voz A", "Coro", or the voice's name in the lyrics
    std::string kind;  // "narrator", "lead", "secondary", "chorus", "unknown"
    std::string emission; // "spoken", "sung", "mixed"
    std::string source;   // "acoustic", "stem", "lyrics"
    double medianF0 = 0;
    double confidence = 0;
    int totalMS = 0;
    std::vector<std::string> evidence;
};

struct MouthShape {
    std::string shape; // the face's own names: AI E FV L MBP O U WQ etc rest
    int startMS = 0;
    int endMS = 0;
};

struct Intervention {
    std::string id;        // "<track>#<n>"
    std::string activityId; // the acoustic segment it came from, or ""
    std::string track;
    int startMS = 0;
    int endMS = 0;
    std::string emission; // "spoken", "sung", "choral", "doubtful"
    double confidence = 0;
    std::string text;     // the lines sung or spoken in it, when the lyrics are synchronized
    std::vector<std::string> wordIds;
    std::vector<MouthShape> mouth;
    std::vector<std::string> relations; // "over-music", "alone", "answers:<id>", "overlaps:<id>"
    std::string provenance;
    std::vector<std::string> evidence;
};

struct VocalUnderstanding {
    int schemaVersion = VOCAL_UNDERSTANDING_SCHEMA_VERSION;
    std::vector<VoiceTrack> tracks;
    std::vector<Intervention> interventions;
    std::vector<std::string> limits; // what this reading cannot do, said plainly
    std::vector<std::string> warnings;

    [[nodiscard]] const VoiceTrack* Track(const std::string& id) const;
    [[nodiscard]] std::vector<const Intervention*> Of(const std::string& track) const;
    // The track sounding at an instant, or "" (a choir and a soloist can both sound).
    [[nodiscard]] std::vector<std::string> SoundingAt(int timeMS) const;
};

struct VocalInput {
    const AudioUnderstanding* audio = nullptr; // required
    const LyricAlignment* alignment = nullptr;
    const LyricDocument* lyrics = nullptr;
    const VoiceCast* cast = nullptr;
    const Transcript* transcript = nullptr;
};

[[nodiscard]] VocalUnderstanding UnderstandVoices(const VocalInput& input);
[[nodiscard]] std::string ToJson(const VocalUnderstanding& v);
[[nodiscard]] bool FromJson(const std::string& text, VocalUnderstanding& v, std::string& error);

} // namespace composer
