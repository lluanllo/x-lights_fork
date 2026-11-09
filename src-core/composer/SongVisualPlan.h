#pragma once

#include "composer/CompositionPlan.h"
#include "composer/RecipeLibrary.h"
#include "composer/SongAnalysis.h"
#include "composer/VisualDirection.h"

#include <map>
#include <string>
#include <vector>

// The artistic director: one plan for the whole song above the scenes. It reads what
// the plan and the visual direction already decided and adds what they do not: where
// the show builds and where it rests, which recipe each scene plays and why, which
// family of figures leads, accompanies, accents or rests in each part, which motif
// comes back when the music does, and the colours of each part from the song itself.
// Deterministic and explainable: every choice carries its reasons, and nothing here
// changes the analysis, the plan or the direction it was given.
namespace composer {

inline constexpr int SONG_VISUAL_PLAN_SCHEMA_VERSION = 1;
inline constexpr const char* SONG_VISUAL_PLAN_VERSION = "1.0.0";

// What a language model may say about a song, checked before use. Every field is
// optional and only nudges the choices; without it the director decides alone.
struct SongVisualBrief {
    std::string source;              // "groq:<model>", "local" or ""
    std::string mood;                // "joyful", "tender", "epic", "playful", "nostalgic", "dark"
    std::vector<std::string> colors; // "#RRGGBB", what the words suggest
    std::vector<std::string> imagery;
    double christmas = -1; // 0-1, how Christmas the song itself is; -1 unknown
    double energy = -1;    // 0-1
    std::vector<std::string> warnings;
    [[nodiscard]] bool Empty() const { return source.empty(); }
};

struct VisualPlanOptions {
    bool recipes = true;
    bool colorEngine = true;
    double christmasInfluence = 0.5; // 0: the song's own colours; 1: the classic Christmas ones
    double variety = 0.5;            // 0: one recipe as long as it fits; 1: a new one each time it can
    uint64_t seed = 2026;
};

struct SurfaceRoleAt {
    std::string family;
    std::string role; // "lead", "complement", "accent", "rest"
    std::string why;
};

struct VisualPlanScene {
    std::string segmentId;
    int startMS = 0;
    int endMS = 0;
    std::string sectionKind;
    int repeatGroup = -1;
    double arc = 0;      // 0-1: how far up the show's build this part is
    std::string arcStep; // "establish", "build", "peak", "release", "rest", "close"
    std::string recipeId;
    std::string recipeName;
    double recipeScore = 0;
    std::string motif; // a returning part plays the recipe it played before
    std::vector<SurfaceRoleAt> roles;
    std::vector<std::string> colors;
    std::string accent;
    std::string colorReason;
    std::vector<Reason> reasons;

    [[nodiscard]] const SurfaceRoleAt* Role(const std::string& family) const;
};

struct SongVisualPlan {
    int schemaVersion = SONG_VISUAL_PLAN_SCHEMA_VERSION;
    std::string version = SONG_VISUAL_PLAN_VERSION;
    bool enabled = false;
    VisualPlanOptions options;
    std::string arcShape;
    std::string anchorHue; // where the colours start from, and why
    std::string anchorReason;
    std::string briefSource;
    std::vector<VisualPlanScene> scenes;
    // The signature of the composition: how varied it is, measured on the plan.
    int distinctRecipes = 0;
    int recipeScenes = 0;
    double longestRepeatShare = 0; // the share of scenes the most used recipe plays
    std::vector<std::string> warnings;

    [[nodiscard]] const VisualPlanScene* Scene(const std::string& segmentId) const;
};

struct VisualPlanInput {
    const CompositionPlan* plan = nullptr;            // required
    const SongAnalysis* analysis = nullptr;           // required
    const ShowVisualDirection* direction = nullptr;   // the families and scenes; required
    const RecipeLibrary* recipes = nullptr;           // null or empty: no recipes
    const SongVisualBrief* brief = nullptr;
    VisualPlanOptions options;
};

[[nodiscard]] SongVisualPlan PlanSongVisuals(const VisualPlanInput& input);

// Gives the direction's scenes the colours the plan chose, so the choreography and the
// media follow them as they followed the theme's.
void ApplyPlanColors(const SongVisualPlan& plan, ShowVisualDirection& direction);

// The colour engine on its own, for a part of a song: the same inputs give the same
// colours. `anchorHue` in degrees.
struct ColorChoice {
    std::vector<std::string> colors;
    std::string accent;
    std::string reason;
};
[[nodiscard]] ColorChoice ColorsForPart(double anchorHue, const std::string& sectionKind, double energy, bool minor, double christmasInfluence, const std::vector<std::string>& briefColors);
// The hue a song's colours start from: the brief's colours, else its key on the circle
// of fifths (a convention, said as such), else a neutral warm.
[[nodiscard]] double AnchorHue(const SongAnalysis& analysis, const SongVisualBrief* brief, std::string& reason);

[[nodiscard]] std::string ToJson(const SongVisualPlan& plan);
[[nodiscard]] std::string VisualPlanReport(const SongVisualPlan& plan);

// The brief, checked: unknown words dropped, colours that are not "#RRGGBB" dropped,
// numbers clamped. False when nothing usable is left.
[[nodiscard]] bool ParseVisualBrief(const std::string& json, const std::string& source, SongVisualBrief& brief, std::string& error);
[[nodiscard]] std::string ToJson(const SongVisualBrief& brief);
// What the lyrics alone say, with no service: Christmas words, moods and colour words.
[[nodiscard]] SongVisualBrief LocalVisualBrief(const std::string& lyricsText, const std::string& title);

} // namespace composer
