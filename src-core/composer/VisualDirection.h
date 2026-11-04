#pragma once

#include "composer/CompositionPlan.h"
#include "composer/LyricDocument.h"
#include "composer/ModelCapabilities.h"
#include "composer/SongAnalysis.h"

#include <string>
#include <vector>

// The look of a whole show before any effect is chosen: a theme and its colours, the
// families of figures that move together, and for every segment of the plan which
// colours, which kind of movement and how much of the display. Choreography follows
// it so that the house reads as one picture, the way the reference shows do.
//
// Measured inputs (the song analysis, the layout) and interpretation (the theme, the
// mood) are kept apart: every interpreted choice says where it came from and how sure
// it is, and nothing here changes the analysis.
namespace composer {

inline constexpr int VISUAL_DIRECTION_SCHEMA_VERSION = 1;
inline constexpr const char* VISUAL_DIRECTION_VERSION = "1.0.0";

struct ThemeDef {
    std::string id;
    std::string name;
    std::string source; // "reference-measured" or "starter"
    std::vector<std::string> measuredFrom;
    std::vector<std::vector<std::string>> home;
    std::vector<std::vector<std::string>> contrast;
    std::string accent = "#FFFFFF";
    std::vector<std::string> characterThemes; // SongCharacter themes that point here
    std::vector<std::string> keywords;        // lowercase words or phrases in lyrics or title
};

struct ThemeCatalog {
    std::vector<ThemeDef> themes;
    std::string hash;
    [[nodiscard]] const ThemeDef* Find(const std::string& id) const;
};

// resources/composer/themes.json, wherever the templates are found.
[[nodiscard]] std::string DefaultThemeCatalogPath();
[[nodiscard]] bool LoadThemeCatalog(const std::string& path, ThemeCatalog& catalog, std::string& error);
[[nodiscard]] bool ParseThemeCatalog(const std::string& text, ThemeCatalog& catalog, std::string& error);

// Figures that move together: every model of the layout belongs to one family.
struct FamilyTarget {
    std::string target; // a group covering the family exactly, or one model
    TargetType type = TargetType::Model;
    std::vector<std::string> models;
    int nodes = 0;
    std::string calmTarget; // a ring or outline submodel lit alone in calm scenes, or ""
};

struct VisualFamily {
    std::string id; // "outline", "windows", "arches", "trees", "stars", "mega-tree", "matrix", "snowflakes", "figures", "props", "faces"
    Role role = Role::Prop;
    std::vector<FamilyTarget> targets;
    int nodes = 0;
    bool protagonist = false; // matrix and faces: the plan decides them first
};

struct SceneLook {
    std::string segmentId;
    int startMS = 0;
    int endMS = 0;
    std::string band;        // "rest", "low", "mid", "high", "climax"
    std::string paletteKind; // "home", "contrast", "climax"
    std::vector<std::string> colors;
    std::string accent;
    std::string motif;       // "glow", "flow", "drive", "burst"
    int variant = 0;         // which of the motif's effects, the same when the section repeats
    std::string lookKey;     // repeated sections share it
    double coverage = 0;     // share of the display's nodes it asks for
    double hitsPerMinute = 0;
    std::vector<Reason> reasons;
};

struct ShowVisualDirection {
    int schemaVersion = VISUAL_DIRECTION_SCHEMA_VERSION;
    std::string version = VISUAL_DIRECTION_VERSION;
    bool enabled = false; // only with a profile whose ensemble is on
    std::string profileId;
    // Interpretation, with where it came from.
    std::string themeId;
    std::string themeName;
    double themeConfidence = 0;
    std::string themeSource; // "song-character", "lyrics", "profile-default"
    std::vector<std::string> themeEvidence;
    std::vector<std::string> primary;
    std::vector<std::string> secondary;
    std::vector<std::string> accent;
    // Rules the choreography follows, in words for the report.
    std::vector<std::string> rules;
    std::vector<VisualFamily> families;
    std::vector<SceneLook> scenes;
    std::vector<std::string> motifs;
    std::vector<std::string> warnings;

    [[nodiscard]] const SceneLook* Scene(const std::string& segmentId) const;
};

struct SongCharacter;

struct DirectionInput {
    const CompositionPlan* plan = nullptr;           // required
    const ModelCapabilities* capabilities = nullptr; // required
    const StyleProfile* profile = nullptr;           // null: the neutral profile, ensemble off
    const ThemeCatalog* catalog = nullptr;           // null or empty: no theme, ensemble off
    const SongAnalysis* analysis = nullptr;
    const LyricDocument* lyrics = nullptr;
    const SongCharacter* character = nullptr; // an AI description, if the person asked for one
    std::string title;                        // the song's file name, for keywords
};

[[nodiscard]] ShowVisualDirection DirectShow(const DirectionInput& input);
[[nodiscard]] std::string ToJson(const ShowVisualDirection& direction);
[[nodiscard]] std::string DirectionReport(const ShowVisualDirection& direction);

// Which family a model or group belongs to, by its role and name.
[[nodiscard]] std::string FamilyIdOf(const TargetInfo& target);

// The effects a motif prefers on a family, best first.
[[nodiscard]] const std::vector<std::string>& MotifEffects(const std::string& motif, const std::string& family);

// How the generated show compares with what the references measured. None of these
// says it looks good: they catch a show that is scattered, monotonous or half dark.
struct DirectionMetrics {
    double paletteCoherence = 0;  // of event time, the share in its scene's colours
    double hueConcentration = 0;  // the two most used hues' share of colour time
    double lookDiversity = 0;     // distinct looks over sections that do not repeat
    double repeatConsistency = 0; // repeated sections that kept their look
    double coverageMean = 0;      // node share under an effect, over music
    double startsTogether = 0;    // event starts shared with two or more others
    double eventsPerMinute = 0;
    int hits = 0;
    int groupEvents = 0;
    int subModelEvents = 0;
    int layersUsed = 0;
};

struct Choreography;
[[nodiscard]] DirectionMetrics MeasureDirection(const ShowVisualDirection& direction, const Choreography& choreography, const ModelCapabilities& capabilities);
[[nodiscard]] std::string ToJson(const DirectionMetrics& metrics);

} // namespace composer
