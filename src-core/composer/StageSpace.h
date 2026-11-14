#pragma once

#include "composer/FigureGeometry.h"
#include "composer/FigureSemantics.h"

#include <string>
#include <utility>
#include <vector>

// One space for the whole installation. Four kinds of place are kept apart:
//   1. a light inside its model (xLights' model space, what a render buffer carries);
//   2. a model inside the layout (xLights' layout units, after position, size, rotation);
//   3. the stage: the layout squeezed into a 0-1 square, the unit gestures are measured in;
//   4. real distances, only when a person says the layout matches the house (calibration).
// The layout is a drawing, not a survey: unless a person says otherwise the stage is
// "schematic", and nothing here claims metres it does not know.
namespace composer {

inline constexpr int STAGE_SPACE_SCHEMA_VERSION = 1;

// A person's correction of where a figure really is (stage units), and whether the
// layout as a whole matches the real house.
struct StageCalibration {
    std::string figure;
    double dx = 0;
    double dy = 0;
    double scale = 1;
    std::string note;
};

struct StageCalibrations {
    int schemaVersion = STAGE_SPACE_SCHEMA_VERSION;
    bool physical = false; // the layout was drawn to scale
    double metresPerUnit = 0; // when physical: a stage unit in metres
    std::vector<StageCalibration> figures;
    [[nodiscard]] const StageCalibration* Find(const std::string& figure) const;
};

struct StageFigure {
    std::string name;
    std::string family;
    std::string topology;
    int lights = 0;
    bool noController = false;
    bool placed = false; // its lights have layout positions
    double minX = 0, minY = 0, maxX = 0, maxY = 0; // stage box
    double cx = 0, cy = 0;
    // model space -> stage: a11 a12 a13 b1 a21 a22 a23 b2 (calibration included)
    std::vector<double> transform;
    double fitError = 0;   // the worst light's distance from where the transform puts it (stage units)
    std::string precision; // "exact", "approximate", "unknown"
    bool calibrated = false;
};

struct StageSpace {
    int schemaVersion = STAGE_SPACE_SCHEMA_VERSION;
    double originX = 0, originY = 0, size = 1; // layout units -> stage: (w - origin) / size
    bool physical = false;
    double metresPerUnit = 0;
    std::vector<StageFigure> figures;
    std::vector<std::string> limits;

    [[nodiscard]] const StageFigure* Find(const std::string& name) const;
    [[nodiscard]] std::pair<double, double> ToStage(double wx, double wy) const;
};

[[nodiscard]] StageSpace BuildStageSpace(const FigureCatalog& geometry, const StageCalibrations* calibrations);
// Every light of a figure on the stage, in its node order (index 0-based).
[[nodiscard]] std::vector<std::pair<double, double>> LightsOnStage(const FigureGeometry& figure, const StageFigure& stage);

// Where a gesture starts. Ids are stable while the layout keeps the names:
//   "node:<figure>:<light number, 1-based>", "region:<figure>:<region>", "figure:<figure>",
//   "center:<figure>", "group:<group>", "point:<x>,<y>" (stage units).
struct StageAnchor {
    std::string id;
    double x = 0;
    double y = 0;
    bool ok = false;
    std::string problem; // "figure-missing", "light-missing", "region-missing", "not-placed", "malformed"
};
[[nodiscard]] StageAnchor ResolveAnchor(const std::string& id, const StageSpace& space, const FigureCatalog& geometry, const FigureSemantics* semantics);

[[nodiscard]] std::string StageCalibrationsPath(const std::string& showDir);
[[nodiscard]] std::string ToJson(const StageCalibrations& c);
[[nodiscard]] bool FromJson(const std::string& text, StageCalibrations& c, std::string& error);
[[nodiscard]] std::string ToJson(const StageSpace& space);

// Every wired light of the installation at its place on the stage, with its first channel
// in a render: what a picture of the whole house is drawn from.
struct StageLight {
    float x = 0;
    float y = 0;
    int channel = -1;
};
struct StageFigureLights {
    std::string figure;
    std::vector<StageLight> lights; // in node order
};
[[nodiscard]] std::string StageLightsJson(const FigureCatalog& geometry, const StageSpace& space);
[[nodiscard]] bool StageLightsFromJson(const std::string& text, std::vector<StageFigureLights>& figures, std::string& error);

} // namespace composer
