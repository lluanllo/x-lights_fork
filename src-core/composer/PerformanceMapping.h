#pragma once

#include "composer/AudioUnderstanding.h"
#include "composer/FigureSemantics.h"
#include "composer/VocalUnderstanding.h"

#include <string>
#include <utility>
#include <vector>

// Who performs what: each voice track and each kind of musical moment given to figures,
// with the part of the figure that shows it and what the figure does. A figure only
// represents a voice while that voice is heard: when the narration ends, the tree stops
// speaking; in an instrumental, no mouth moves.
//
// A person's assignments win and persist; what is automatic says why, and every problem
// (a figure with no mouth, an empty region, no controller, two soloists on one figure,
// too few figures for a choir) is named.
namespace composer {

struct LyricAlignment;
struct VoiceCast;

inline constexpr int PERFORMANCE_MAP_SCHEMA_VERSION = 1;

struct PerformerAssignment {
    std::string source;      // "track:<voice track id>", "activity:instrumental", "activity:percussion", "event:accent"
    std::string sourceLabel; // "Voz A", "Coro", "Instrumental"...
    std::vector<std::string> figures;
    std::string function;    // "speak", "sing", "choral-response", "instrumental-lead", "accent", "background"
    std::string region;      // "mouth", "face", "foliage", "outline", "" (the whole figure)
    std::string state = "automatic"; // "automatic", "manual"
    double confidence = 0;
    std::vector<std::string> reasons;
    std::vector<std::string> conflicts;
    // When the show means the figures to perform: the phrases of the voice in the lyrics.
    // Empty: whenever the voice or the moment is heard.
    std::vector<std::pair<int, int>> windows;
};

struct PerformanceMap {
    int schemaVersion = PERFORMANCE_MAP_SCHEMA_VERSION;
    std::vector<PerformerAssignment> assignments;
    std::vector<std::string> conflicts; // stable codes with their subject

    [[nodiscard]] const PerformerAssignment* For(const std::string& source) const;
    // What a figure does, and when: its assignments' windows of activity.
    [[nodiscard]] std::vector<const PerformerAssignment*> OfFigure(const std::string& figure) const;
};

struct MappingInput {
    const AudioUnderstanding* audio = nullptr;
    const VocalUnderstanding* voices = nullptr;
    const FigureCatalog* geometry = nullptr;
    const FigureSemantics* semantics = nullptr;
    const std::vector<PerformerAssignment>* manual = nullptr; // a person's, by source
};

[[nodiscard]] PerformanceMap MapPerformance(const MappingInput& input);

// When an assignment is active: its voice track's interventions, or the stretches of
// its kind of activity or event.
[[nodiscard]] std::vector<std::pair<int, int>> ActiveWindows(const PerformerAssignment& a, const AudioUnderstanding& audio, const VocalUnderstanding& voices);

// A person's assignments are kept per song, next to its understanding
// (UnderstandingStore.h): voices are a song's, figures are the show's.
[[nodiscard]] std::string ToJson(const std::vector<PerformerAssignment>& manual);
[[nodiscard]] bool FromJson(const std::string& text, std::vector<PerformerAssignment>& manual, std::string& error);
[[nodiscard]] std::string ToJson(const PerformanceMap& map);

// The voices heard, given to the lyrics: an unnamed synchronized phrase takes the voice
// heard most while it sounds, when that voice has figures with a face, and those figures
// become its faces, so a face sings only its own phrases. A voice the lyrics name keeps
// its name; a person's figures for it win. Phrases nobody is heard in stay unnamed.
struct CastFromUnderstanding {
    int phrasesNamed = 0;
    int voicesAdded = 0;
    int facesChanged = 0;
    std::vector<std::string> notes;
};
[[nodiscard]] CastFromUnderstanding ApplyPerformersToCast(const VocalUnderstanding& voices, const PerformanceMap& map, const FigureSemantics& semantics, LyricAlignment& alignment,
                                                          VoiceCast& cast);

// The assignments as the show was made: each voice's figures are the faces its voice
// sings with in the sequence, active during its phrases of the lyrics; a voice with no
// synchronized phrase, or no face, performs nowhere, and says why.
void SyncPerformersWithCast(PerformanceMap& map, const VocalUnderstanding& voices, const VoiceCast& cast, const LyricAlignment* alignment);

[[nodiscard]] std::string FunctionPhrase(const std::string& function);
[[nodiscard]] std::string ConflictPhrase(const std::string& code);

} // namespace composer
