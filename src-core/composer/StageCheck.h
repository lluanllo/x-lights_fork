#pragma once

#include "composer/FigureGeometry.h"
#include "composer/StageDirector.h"
#include "composer/StageSpace.h"

#include <string>
#include <vector>

// What the native render of a whole show did with the installation: how much of it was
// lit and how many figures, scene by scene; how the attention was spread; and, when the
// show had a stage score, when each figure of each gesture lit up against when the score
// said. Numbers for a person and for comparing versions, never a verdict: whether a
// show looks good is for the people who watch it.
namespace composer {

inline constexpr int STAGE_CHECK_SCHEMA_VERSION = 1;

struct GestureArrival {
    std::string gesture;
    std::string figure;
    int expectedMS = 0;
    int litMS = -1;   // -1: no clear rise of its light was seen
    int errorMS = 0;  // lit minus expected
};

// What one figure did in a scene, against what the score asked of it (phase 21).
struct FigureActivity {
    std::string figure;
    std::string asked;  // "play" (lead, accompany, answer), "glow" (kept quietly), "rest"
    double lit = 0;     // mean share of its lights lit
    double bright = 0;  // mean brightness of its lights (0-1)
    std::string seen;   // "play", "dark"
    bool agrees = false; // asked to play or glow and seen lit, or asked to rest and seen dark
};

struct SceneLight {
    std::string id; // the scene's id, or "<from>-<to>" without a score
    std::string mode;
    int startMS = 0;
    int endMS = 0;
    double litShare = 0;    // mean share of the installation's lights lit
    double figuresLit = 0;  // mean number of figures with a twentieth of their lights lit
    double focus = 0;       // mean share of the light held by the three brightest figures
    // Phase 21: the score against the render, figure by figure.
    int askedPlay = 0, askedGlow = 0, askedRest = 0;
    int seenLit = 0;          // figures lit (a twentieth of their lights, on average)
    int restButLit = 0;       // asked to rest, seen lit
    int restLitByShared = 0;  // of those, lit by a figure wired to the same lights that plays
    int playButDark = 0;      // asked to play, seen dark
    double agreement = 0;     // share of the figures whose light agrees with what was asked
    double brightPlay = 0;    // mean brightness of the figures asked to play
    double brightGlow = 0;    // of those asked to glow
    double brightRest = 0;    // of those asked to rest
    std::vector<FigureActivity> activity;
};

// A gesture read in the render, in time and on the stage (phase 21): what changed, in what
// order, how fast, and whether it was seen as an answer, a wave or a flash. Measurements of
// intent, never a threshold every gesture must reach.
struct GestureReading {
    std::string gesture;
    std::string kind;
    std::string relation;
    std::string presence;
    int participants = 0;
    int seen = 0;              // participants whose light was seen rising at their moment
    double before = 0;         // participants' mean brightness just before (0-1)
    double peak = 0;           // their mean of each one's brightest moment in its part
    double after = 0;          // just after the gesture
    double rise = 0;           // peak - before: the temporal contrast
    double houseChange = 0;    // the figures not in it: mean brightness during minus before
    double contrastVsHouse = 0; // rise - houseChange: how much more the gesture's figures moved than the rest
    double orderAgreement = -2; // Spearman between the order the score asked and the order seen (-1..1; -2 too few)
    double timingFit = -2;     // R^2 of seen against asked arrival times (-2 too few)
    int spreadMS = 0;          // first to last arrival seen
    int expectedSpreadMS = 0;  // first to last arrival asked
    int largestGapMS = 0;      // largest wait between two arrivals seen
    bool firstActsFirst = false; // the figure that starts it is the first seen
    bool answersDistinct = false; // answers seen at least a third of their asked step apart
    int decayMS = -1;          // from the peak back to half way down (-1 not seen)
};

struct StageCheck {
    int schemaVersion = STAGE_CHECK_SCHEMA_VERSION;
    bool ran = false;
    std::string problem; // why it did not run
    int frames = 0;
    int frameMS = 0;
    int figures = 0;  // figures measured: placed, wired, with lights
    int lights = 0;
    double litShare = 0;
    double figuresLit = 0;
    double focus = 0;
    double sceneContrast = 0; // how much the scenes differ in light: spread of their lit share
    std::vector<SceneLight> scenes;
    std::vector<GestureArrival> arrivals;
    int arrivalsSeen = 0;
    int arrivalsOnTime = 0; // within 100 ms
    int worstErrorMS = 0;
    // Phase 21
    double agreement = 0;   // over the scenes: share of figures whose light agrees with the score
    int restButLit = 0;     // over the scenes: figure x scene asked to rest and seen lit
    int restLitByShared = 0; // of those, lit by a figure wired to the same lights that plays
    int playButDark = 0;
    std::vector<GestureReading> readings;
    std::vector<std::string> notes;
};

// `score` may be null: the scenes are then ten-second stretches and no gesture is checked.
[[nodiscard]] StageCheck CheckStage(const std::string& fseqPath, const FigureCatalog& geometry, const StageSpace& space, const GlobalStageScore* score);

[[nodiscard]] std::string ToJson(const StageCheck& check);
// A few lines a person can read.
[[nodiscard]] std::string StageCheckReport(const StageCheck& check);

} // namespace composer
