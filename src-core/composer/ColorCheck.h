#pragma once

#include "composer/FigureGeometry.h"
#include "composer/StageDirector.h"
#include "composer/StageSpace.h"

#include <set>
#include <string>
#include <vector>

// The colours the native render of a whole show really put on the installation, read from
// its .fseq light by light: which families of colour the house wore and for how long, how
// different the figures were from one another, how the colour moved from scene to scene,
// and whether a gesture's light stood out from what was lit before it on the same bulbs.
// Numbers for a person and for comparing versions, never a verdict: a scene in one colour
// can be exactly right, and many colours are not a good palette.
namespace composer {

inline constexpr int COLOR_CHECK_SCHEMA_VERSION = 1;
// A frame where one family of colour holds at least this share of the house's light is
// "in one colour". The developer's observation (about 60%) as a sign, not a target.
inline constexpr double MONOTONE_SHARE = 0.6;

struct ColorShare {
    std::string family; // ColorFamilyId
    double share = 0;
};

struct SceneColor {
    std::string id;
    std::string mode;
    int startMS = 0;
    int endMS = 0;
    std::string dominant;      // the family holding most of the light
    double dominantShare = 0;  // its share of the scene's light
    double monotoneShare = 0;  // of its lit frames, those in one colour
    double entropy = 0;        // 0 one family, 1 every family alike
    double effectiveColors = 0; // e^entropy: about how many families share the light
    double figureDifference = 0; // mean difference between lit figures' colours (ΔE x100)
    double figureFamilies = 0;   // mean number of different families the lit figures wear
    double accentFrames = 0;     // share of frames with a minority colour (3-20% of the light)
    // Cohesion (phase 19): how much the colours lit at once belong together.
    double familiesAtOnce = 0;   // mean number of families each over 5% of the house's light
    double topTwoShare = 0;      // mean share of the light held by the two largest families
    double hueSpread = 0;        // 0 every coloured light in one hue, 1 hues all round the circle
    double minorLight = 0;       // mean share of the light outside the two largest families
    double colorsPerFigure = 0;  // mean number of families over 10% inside one lit figure
    double chroma = 0;           // mean OKLCH chroma of lit lights
    double lightness = 0;
    std::string meanHex;
    std::vector<ColorShare> families;
    bool intended = false; // the colour plan meant this scene in one family
};

struct FigureColorCheck {
    std::string name;
    std::string dominant;
    double dominantShare = 0;
    double litShare = 0; // of the song's frames, those with this figure lit
    int sceneDominants = 0; // how many different dominant families it wore over the scenes
    std::vector<ColorShare> families;
};

struct GestureColorCheck {
    std::string gesture;
    std::string figure;
    int atMS = 0;
    double againstBefore = 0; // ΔE between its lights at the gesture and the same lights just before
    double againstHouse = 0;  // ΔE between its lights at the gesture and the rest of the lit house
    std::string gestureHex;
    std::string beforeHex;
    std::string houseHex;
    int lights = 0; // lights that changed
};

struct ColorCheck {
    int schemaVersion = COLOR_CHECK_SCHEMA_VERSION;
    bool ran = false;
    std::string problem;
    int frames = 0;
    int frameMS = 0;
    int figures = 0;
    int lights = 0;
    int litFrames = 0;
    // The whole show.
    std::string dominant;
    double dominantShare = 0;
    double entropy = 0;
    double effectiveColors = 0;
    double monotoneShare = 0;       // of lit frames, in one colour
    int longestMonotoneMS = 0;
    int longestMonotoneStartMS = 0;
    std::string longestMonotoneFamily;
    int sceneDominants = 0;         // distinct dominant families over the scenes
    double sceneVariation = 0;      // mean change of the colour mix from a scene to the next, 0-1
    double figureDifference = 0;
    double figureFamilies = 0;
    double accentFrames = 0;
    double familiesAtOnce = 0;
    double topTwoShare = 0;
    double hueSpread = 0;
    double minorLight = 0;
    double colorsPerFigure = 0;
    int overloadedScenes = 0;       // scenes with more than four families over 5% at once, on average
    double gestureContrast = 0;     // median ΔE against what was lit before
    double gestureHouseContrast = 0;
    int gesturesFaint = 0;          // gestures under 15 ΔE against what was there
    std::vector<ColorShare> families;
    std::vector<SceneColor> scenes;
    std::vector<FigureColorCheck> figureChecks;
    std::vector<GestureColorCheck> gestures;
    // Each: "<code>: <plain explanation>", for a person.
    std::vector<std::string> warnings;
};

// `score` may be null (ten-second stretches, no gestures). `intended`: ids of scenes the
// colour plan made in one family on purpose; they are not counted as monotony.
[[nodiscard]] ColorCheck CheckColors(const std::string& fseqPath, const FigureCatalog& geometry, const StageSpace& space, const GlobalStageScore* score,
                                     const std::set<std::string>* intended = nullptr);

[[nodiscard]] std::string ToJson(const ColorCheck& check);
[[nodiscard]] std::string ColorCheckReport(const ColorCheck& check);

} // namespace composer
