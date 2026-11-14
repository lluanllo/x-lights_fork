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

struct SceneLight {
    std::string id; // the scene's id, or "<from>-<to>" without a score
    std::string mode;
    int startMS = 0;
    int endMS = 0;
    double litShare = 0;    // mean share of the installation's lights lit
    double figuresLit = 0;  // mean number of figures with a twentieth of their lights lit
    double focus = 0;       // mean share of the light held by the three brightest figures
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
    std::vector<std::string> notes;
};

// `score` may be null: the scenes are then ten-second stretches and no gesture is checked.
[[nodiscard]] StageCheck CheckStage(const std::string& fseqPath, const FigureCatalog& geometry, const StageSpace& space, const GlobalStageScore* score);

[[nodiscard]] std::string ToJson(const StageCheck& check);
// A few lines a person can read.
[[nodiscard]] std::string StageCheckReport(const StageCheck& check);

} // namespace composer
