#pragma once

#include <functional>
#include <map>
#include <string>
#include <vector>

// What happens in a recording, beside the musical analysis (SongAnalysis keeps the beats,
// bars and sections): who is speaking or singing, a choir, the instruments alone, the
// drums, silences and pauses, and the shape of the sound (attacks, sustains, decays,
// tails, fades, drops and re-entries). Activities overlap: a voice speaks while the music
// plays. Local and deterministic; nothing is sent anywhere.
//
// It is a reading of the sound, not a separation of it: a choir heard as a choir stays a
// choir, and no individual singer is extracted from a mix. Every segment and event says
// how sure it is and what it was read from, and a person can correct any of them.
namespace composer {

inline constexpr int AUDIO_UNDERSTANDING_SCHEMA_VERSION = 1;
inline constexpr const char* AUDIO_UNDERSTANDING_VERSION = "1.0.0";
inline constexpr int UNDERSTANDING_HOP_MS = 20;
inline constexpr int UNDERSTANDING_CURVE_MS = 100;

// One step of the sound, every 100 ms in the saved curve (every 20 ms while analysing).
struct SoundFrame {
    int timeMS = 0;
    float rms = 0;        // 0-1 of full scale
    float low = 0;        // band shares of this frame's energy, 0-1
    float mid = 0;
    float high = 0;
    float flux = 0;       // spectral change, relative to the song
    float centroid = 0;   // Hz
    float harmonic = 0;   // share of steady (tonal) energy, 0-1
    float percussive = 0; // share of short (struck) energy, 0-1
    float f0 = 0;         // Hz, 0 when no clear pitch
    float voicing = 0;    // how clear the pitch is, 0-1
    float voice = 0;      // how much it sounds like a voice, 0-1
    float music = 0;      // how much accompaniment is heard, 0-1
};

// A stretch where something is heard. Kinds: "speech", "singing", "chorus",
// "a-cappella" (a voice and nothing else), "music" (any accompaniment),
// "instrumental" (music with no voice), "percussion", "bass", "silence", "pause"
// (the voice stops while the music goes on), "noise".
struct ActivitySegment {
    std::string id; // "<kind>#<n>": stable while the order of that kind does not change
    std::string kind;
    int startMS = 0;
    int endMS = 0;
    double confidence = 0;
    std::string track; // the voice it belongs to ("voice:A", "chorus"), or ""
    std::string provenance = "detected"; // "detected", "stem:<role>", "lyrics", "transcript", "corrected", "added"
    std::vector<std::string> evidence;
};

// A moment in the shape of the sound. Kinds: "attack", "accent", "sustain", "decay",
// "tail", "fade-in", "fade-out", "crescendo", "diminuendo", "silence", "pause", "drop",
// "reentry", "resolution".
struct SoundEvent {
    std::string id;
    std::string kind;
    int startMS = 0;
    int endMS = 0;
    double strength = 0; // 0-1
    double confidence = 0;
    std::string provenance = "detected";
    std::vector<std::string> evidence;
};

// One recording given with the mix: a voice, a choir, the instruments...
struct StemInput {
    std::string path;
    std::string role; // "mix", "voice", "chorus", "narration", "instrumental", "drums", "bass", "other"
    std::string name; // shown to a person: "Voz de Ana", "Coro"
};

struct StemInfo {
    std::string role;
    std::string name;
    std::string file; // the file name only
    std::string hash;
    int durationMS = 0;
    bool usable = false;
    std::string problem;
};

struct UnderstandingOptions {
    double silenceDb = -50;  // never quieter than this counts as sound
    int minSilenceMS = 400;  // shorter dips are not silences
    int minPauseMS = 350;
    int minVoiceMS = 300;
    bool estimatePitch = true;
};

// What a person changed: kept apart and applied after every analysis.
struct UnderstandingCorrection {
    enum class Action {
        Move,       // new start and end
        Split,      // at `atMS`
        Merge,      // with `otherId`
        Reclassify, // new `kind`
        Reassign,   // new `track`
        Remove,
        Add,        // a new segment: kind, start, end, track
        Confirm
    };
    Action action = Action::Confirm;
    std::string on = "activity"; // "activity" or "event": what `targetId` names
    std::string targetId; // the segment's id when it was corrected
    int anchorMS = -1;    // its start then: finds it again if a new analysis renumbered it
    int startMS = -1;
    int endMS = -1;
    int atMS = -1;
    std::string otherId;
    std::string kind;
    std::string track;
    std::string note;
};

struct AudioUnderstanding {
    int schemaVersion = AUDIO_UNDERSTANDING_SCHEMA_VERSION;
    std::string version = AUDIO_UNDERSTANDING_VERSION;
    std::string audioHash;
    std::string optionsKey;
    int durationMS = 0;
    int channels = 0;
    bool stereo = false; // a real stereo mix, not two equal channels
    std::vector<StemInfo> stems;
    std::vector<SoundFrame> curve;
    std::vector<ActivitySegment> activities;
    std::vector<SoundEvent> events;
    std::vector<std::string> warnings; // stable codes
    std::vector<UnderstandingCorrection> corrections;

    [[nodiscard]] std::vector<const ActivitySegment*> Of(const std::string& kind) const;
    [[nodiscard]] const ActivitySegment* Find(const std::string& id) const;
    [[nodiscard]] bool Has(const std::string& kind, int fromMS, int toMS) const;
};

using UnderstandingProgress = std::function<void(int percent, const char* stage)>;
using UnderstandingCancel = std::function<bool()>;

struct UnderstandingResult {
    bool ok = false;
    bool cancelled = false;
    std::string errorCode;
    std::string errorDetail;
    AudioUnderstanding understanding;
};

// The mix (and any stems) read into frames, activities and events. Blocking: call it
// from a worker. Stems are read on their own and win over the mix for their role.
[[nodiscard]] UnderstandingResult UnderstandAudio(const std::string& mixPath, const std::vector<StemInput>& stems, const UnderstandingOptions& options = {},
                                                  const UnderstandingProgress& progress = {}, const UnderstandingCancel& cancel = {});
// The same from samples already in memory (the tests, and stems), mono or stereo.
[[nodiscard]] AudioUnderstanding UnderstandSamples(const std::vector<float>& left, const std::vector<float>& right, int sampleRate, const UnderstandingOptions& options = {},
                                                   const UnderstandingCancel& cancel = {});

// What a cached reading must match to be reused: this reader's version and its options.
[[nodiscard]] std::string UnderstandingOptionsKey(const UnderstandingOptions& options);

// Applies the corrections stored in `u`: never twice, never over a person's decision.
void ApplyUnderstandingCorrections(AudioUnderstanding& u);
// Restores the automatic reading: drops every correction.
void ResetUnderstandingCorrections(AudioUnderstanding& u);

[[nodiscard]] const char* CorrectionActionId(UnderstandingCorrection::Action action);
[[nodiscard]] std::string ToJson(const AudioUnderstanding& u);
[[nodiscard]] bool FromJson(const std::string& text, AudioUnderstanding& u, std::string& error);

// Words a person reads: "Habla", "Canta", "Entra el coro", "Suena un instrumento"...
[[nodiscard]] std::string ActivityPhrase(const std::string& kind);
[[nodiscard]] std::string EventPhrase(const std::string& kind);

} // namespace composer
