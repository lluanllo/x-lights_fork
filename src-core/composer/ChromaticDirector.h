#pragma once

#include "composer/ColorSpace.h"
#include "composer/StageDirector.h"

#include <map>
#include <string>
#include <vector>

// The colour direction of a whole show, as a lighting designer would give it: a family of
// colours for the song with a hierarchy (an anchor that sets the identity, a secondary
// that varies it, an accent for what matters, a contrast to separate, a neutral to rest),
// a palette for each scene drawn from that family and changing when the music does, a
// colour role for each figure so the house is not one colour with moving effects, the
// colours of each gesture chosen against what is lit under it, and transitions that move
// figure by figure instead of the whole house at once.
//
// It sits on what the song visual plan and the stage director already decided (the
// scenes, the arc, who leads) and does not repeat them. Deterministic and explainable:
// every choice carries its reason, the same inputs give the same score, and a person's
// edits and a figure's own colours win. Colours are chosen in OKLCH (ColorSpace.h) and
// written as the "#RRGGBB" xLights uses, always inside its gamut.
namespace composer {

inline constexpr int CHROMATIC_SCORE_SCHEMA_VERSION = 1;
inline constexpr const char* CHROMATIC_DIRECTOR_VERSION = "1.0.0";

struct CompositionPlan;
struct SongAnalysis;
struct SongVisualPlan;
struct SongVisualBrief;
struct Choreography;
struct ModelCapabilities;

enum class Harmony {
    Monochromatic,      // one hue, in lightness and saturation
    Analogous,          // neighbours on the circle
    Complementary,      // a hue and its opposite
    SplitComplementary, // a hue and the two neighbours of its opposite
    Triadic,            // three hues a third of the circle apart
    Tetradic,           // two pairs of opposites
    WarmCool            // a warm family against a cool one
};

// One colour of a palette and what it is for.
struct PaletteColor {
    std::string role; // "anchor", "secondary", "accent", "contrast", "transition", "neutral", "shadow", "highlight"
    std::string hex;
    double L = 0, C = 0, h = 0; // OKLCH, after fitting the gamut
    std::string why;
};

// A family of colours, not a list: every colour is related to the anchor by a harmony.
struct ColorPaletteProfile {
    std::string id;
    Harmony harmony = Harmony::Analogous;
    double anchorHue = 0;
    double saturation = 0.8; // 0-1 of what the gamut allows
    double lightness = 0.68;
    std::vector<PaletteColor> colors;
    std::string reason;

    [[nodiscard]] const PaletteColor* Get(const std::string& role) const;
    [[nodiscard]] std::string Hex(const std::string& role) const; // "" when missing
};

// What a palette is built from.
struct PaletteRequest {
    double anchorHue = 0;
    Harmony harmony = Harmony::Analogous;
    double saturation = 0.8; // 0-1
    double lightness = 0.68; // OKLCH L of the anchor
    double contrast = 0.5;   // 0 soft relations, 1 strong lightness and chroma differences
    int direction = 1;       // +1 / -1: which way round the circle related hues go
    // Hues the song suggests (its own colours, a person's, the Christmas ones with their
    // weight): a related hue near one of them moves to it, so the palette speaks of the
    // song instead of only of geometry.
    std::vector<std::pair<double, double>> suggested; // hue, weight 0-1
    std::string id;
};

[[nodiscard]] ColorPaletteProfile BuildPalette(const PaletteRequest& request);

// A colour role of a figure in a scene, and the colours it gets.
struct FigureColorRole {
    std::string figure;
    std::string role; // "main", "complement", "accent", "response", "background", "neutral", "decorative", "identity", "manual"
    std::vector<std::string> colors;
    std::string why;
};

struct ChromaticTransition {
    std::string kind; // "cut", "fade", "hue-drift", "saturation", "lightness", "introduce", "dominant-change", "return", "hold"
    int startMS = 0;   // the transition runs from here to the scene's end
    int endMS = 0;
    std::vector<std::string> order; // figures in the order they change (the first earliest)
    std::string why;
};

// The colours of one gesture, chosen against what is lit under it.
struct GestureChromaticProfile {
    std::string gesture;
    std::string start;     // where it leaves from
    std::string front;     // its front
    std::string secondary; // what it brings on the way
    std::string tail;      // what it leaves behind
    std::string arrival;   // where it ends
    std::string colorBy;   // FieldSpec::colorBy
    std::vector<std::string> background; // the colours lit under it on its figures
    double contrast = 0;   // ΔE between its front and the closest of the background colours
    std::string why;
};

struct ChromaticScenePlan {
    std::string id; // the plan segment's id (the stage scene's)
    int startMS = 0;
    int endMS = 0;
    std::string sectionKind;
    std::string mode; // the stage mode, when the stage was directed
    double energy = 0;
    ColorPaletteProfile palette;
    std::string dominant;               // the colour that sets the scene's identity
    std::vector<std::string> secondary; // the variation
    std::vector<std::string> accents;
    std::vector<std::string> contrasts;
    double saturation = 0;
    double intensity = 0;
    bool monochrome = false; // in one family on purpose
    std::string evolution;   // "establish", "develop", "expand", "shift", "climax", "resolve", "return", "hold", "rest"
    std::string motif;       // "motif:<repeat group>": a returning part comes back to its colours
    std::vector<FigureColorRole> figures;
    std::vector<GestureChromaticProfile> gestures;
    ChromaticTransition toNext;
    std::vector<std::string> reasons;
    bool manual = false; // a person changed it
    bool locked = false; // a person fixed its palette

    [[nodiscard]] const FigureColorRole* RoleOf(const std::string& figure) const;
};

struct ChromaticOptions {
    bool enabled = true;
    double variety = 0.5;    // "Variedad cromática": how many colour roles share a scene
    double contrast = 0.5;   // "Contraste"
    double accents = 0.5;    // "Intensidad de acentos"
    double evolution = 0.5;  // "Evolución entre escenas": how far the palette moves from scene to scene
    double christmasInfluence = 0.5;
    // From what the maker taught (ArtisticPreferences), the middle when nothing was:
    double saturation = 0.5; // fuller (1) or softer (0) colours
    double warmth = 0;       // -1 cooler, 1 warmer
    double gradual = 0.5;    // slower, softer transitions (1) or quicker (0)
    bool respectIdentity = false; // every figure with parts of its own colour keeps them
    std::vector<std::string> songColors; // the person's colours for the song; first the anchor
    uint64_t seed = 2026;
};

struct ChromaticScore {
    int schemaVersion = CHROMATIC_SCORE_SCHEMA_VERSION;
    std::string version = CHROMATIC_DIRECTOR_VERSION;
    bool enabled = false;
    ChromaticOptions options;
    ColorPaletteProfile song; // the song's colour identity
    std::string identityReason;
    std::string briefSource;  // where the artistic interpretation came from ("groq:<model>", "local", "file", "")
    std::string mood;
    std::vector<ChromaticScenePlan> scenes;
    std::vector<std::string> motifs;
    int edits = 0;
    std::vector<std::string> warnings;
    std::vector<std::string> limits;

    [[nodiscard]] const ChromaticScenePlan* SceneAt(int ms) const;
    [[nodiscard]] const ChromaticScenePlan* Scene(const std::string& id) const;
};

// A person's changes to the colours, kept per song and applied after every new score.
struct ChromaticEdit {
    std::string action;    // "color", "accent", "lock-palette", "more-contrast", "less-contrast", "figure-color", "identity", "no-identity", "monochrome", "no-monochrome"
    int sceneStartMS = -1; // the scene, found again by its start (within 1.5 s); -1: every scene / the song
    std::string figure;    // figure-color, identity, no-identity
    std::string role;      // color: the palette role it replaces ("anchor", "secondary"...)
    std::string value;     // a "#RRGGBB" (color, accent); "#..,#.." (figure-color); "role=#..;role=#.." (lock-palette)
    std::string note;
};

struct ChromaticEdits {
    int schemaVersion = CHROMATIC_SCORE_SCHEMA_VERSION;
    std::vector<ChromaticEdit> edits;
};

struct ChromaticInput {
    const CompositionPlan* plan = nullptr;       // required: the scenes
    const SongAnalysis* analysis = nullptr;      // required: key and tempo
    const SongVisualPlan* visualPlan = nullptr;  // the arc and where the song's colours start
    const SongVisualBrief* brief = nullptr;      // what the song suggests (a service or the lyrics)
    const AudioUnderstanding* audio = nullptr;   // drops, re-entries, crescendos, tails
    const GlobalStageScore* stage = nullptr;     // modes, roles and gestures, when directed
    const FigureCatalog* geometry = nullptr;
    const FigureSemantics* semantics = nullptr;
    const StageSpace* space = nullptr;
    const Choreography* choreography = nullptr;  // which figures play in each scene
    const ModelCapabilities* capabilities = nullptr;
    const ChromaticEdits* edits = nullptr;
    ChromaticOptions options;
};

[[nodiscard]] ChromaticScore DirectColors(const ChromaticInput& input);

// The score into the choreography: every event of a figure takes its role's colours in
// its scene (as many as the effect had), and where a transition runs, a native colour
// curve carries the slot from this scene's colour to the next one's, figure by figure.
// Events a person locked or whose palette a person chose, pictures, videos and faces'
// own effects are left as they are. Returns notes for the report.
std::vector<std::string> ApplyChromaticScore(const ChromaticScore& score, Choreography& choreography);
// The gestures' colours and how they spread, into the stage score (before it is realized).
int ColorGestures(const ChromaticScore& score, GlobalStageScore& stage);

// A native xLights colour curve for a palette slot: `from` until `holdUntil` (0-1 of the
// effect), then through their OKLCH middle to `to` at the end.
[[nodiscard]] std::string ColorCurveText(int slot, const std::string& from, const std::string& to, double holdUntil);

[[nodiscard]] const char* HarmonyId(Harmony h);
[[nodiscard]] bool HarmonyFromId(const std::string& id, Harmony& h);
[[nodiscard]] std::string HarmonyPhrase(Harmony h);
[[nodiscard]] std::string ColorRolePhrase(const std::string& role);
[[nodiscard]] std::string PaletteRolePhrase(const std::string& role);
[[nodiscard]] std::string ChromaticTransitionPhrase(const std::string& kind);

[[nodiscard]] std::string ToJson(const ChromaticScore& score);
[[nodiscard]] bool FromJson(const std::string& text, ChromaticScore& score, std::string& error);
[[nodiscard]] std::string ChromaticReport(const ChromaticScore& score);
[[nodiscard]] std::string ToJson(const ChromaticEdits& edits);
[[nodiscard]] bool FromJson(const std::string& text, ChromaticEdits& edits, std::string& error);
[[nodiscard]] std::string ChromaticEditsPath(const std::string& showDir, const std::string& audioHash);

} // namespace composer
