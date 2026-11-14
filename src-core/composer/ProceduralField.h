#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Light as a function of place and time. A gesture is a field over the whole stage: every
// light of every figure that takes part asks the same field what it shows at its own
// place, so a wave that leaves one bulb reaches the next figure when it should, without
// starting again in each one. Deterministic: the same field, place and time always give
// the same light; nothing here is random.
//
// Places are in the normalized stage space (StageSpace.h): the layout's figures inside a
// square 0-1, distances in that unit. Times are seconds from the gesture's start.
namespace composer {

inline constexpr int PROCEDURAL_FIELD_VERSION = 1;

enum class FieldFunction {
    Pulse,     // the whole region at once: up, held, down
    Wave,      // one front travelling from the origin
    Ripple,    // fronts one after another, each weaker
    Expansion, // a lit area growing from the origin (or shrinking to it)
    Sweep,     // a straight front crossing the stage, lit behind it
    Chase,     // bands running along a direction, over and over
    Spiral,    // arms turning round the origin
    Flow,      // a slow, smooth moving pattern
    Fade,      // the whole region fading in and out
    Gradient,  // a still slope of light along a direction
    Decay,     // lit at once, then dying away (from the origin outwards)
    Echo       // the same wave again, later and weaker
};

struct FieldSpec {
    FieldFunction function = FieldFunction::Wave;
    // Where it starts and where it goes (stage units, 0-1).
    double ox = 0.5;
    double oy = 0.5;
    double angle = 0;       // direction in degrees: 0 rightwards, 90 upwards
    std::string shape = "radial"; // "radial", "linear", "elliptic"
    double ex = 1, ey = 1;  // the ellipse's stretch along x and y
    // How it moves (seconds and stage units).
    double speed = 0.8;     // stage units a second
    double width = 0.12;    // thickness of a front, stage units
    double duration = 2.0;  // the gesture's life
    double attack = 0.05;   // rise of the light where the front arrives
    double release = 0.4;   // fall of the light behind the front
    double loss = 0;        // weakening with distance: light x e^(-loss x distance)
    double period = 0.5;    // seconds between ripples, echoes and chase bands
    int repeats = 1;        // ripples or echoes
    double gain = 0.5;      // each ripple or echo, this much of the one before
    double turns = 1;       // spiral arms
    double scale = 4;       // spatial frequency of a flow or a spiral
    bool contract = false;  // an expansion that closes on the origin instead
    uint32_t seed = 0;      // only the flow's phases; the same seed, the same flow
    double level = 1;       // the brightest it gets, 0-1
    // When this light starts, from the gesture's start: an effect placed later than the
    // gesture (a far figure) is the same field, joined at its own moment.
    double at = 0;
    // The figure's place on the stage: stage = A x (model space) + b, one row per axis.
    // Identity when not given (a test, or a figure placed by hand).
    std::vector<double> transform; // a11 a12 a13 b1 a21 a22 a23 b2
    // Which lights: xLights' numbers (1-based) of the lights it may light, and the ones it
    // must never touch (a mouth, a text); empty: all of them.
    std::string only;
    std::string skip;
};

struct FieldSample {
    float intensity = 0; // 0-1
    float position = 0;  // 0-1, where in the palette its colour is taken
};

[[nodiscard]] FieldSample EvaluateField(const FieldSpec& spec, double x, double y, double t);
// When the front reaches a place (seconds from the gesture's start); -1 for a field with
// no front (pulse, fade, gradient, flow, spiral, chase).
[[nodiscard]] double ArrivalTime(const FieldSpec& spec, double x, double y);
// The distance the field measures from its origin (radial, elliptic or along the direction).
[[nodiscard]] double FieldDistance(const FieldSpec& spec, double x, double y);
// A light's place on the stage from its place in its model.
[[nodiscard]] std::pair<double, double> StagePoint(const FieldSpec& spec, double sx, double sy, double sz);

[[nodiscard]] const char* FieldFunctionId(FieldFunction f);
[[nodiscard]] bool FieldFunctionFromId(const std::string& id, FieldFunction& f);
[[nodiscard]] const std::vector<FieldFunction>& AllFieldFunctions();
// Plain words for a person: what the function does.
[[nodiscard]] std::string FieldFunctionPhrase(FieldFunction f);

// "fn=wave;ox=0.5;oy=0.3;..." and back, locale-independent; unknown keys are refused so a
// typing mistake is not silently ignored. Validation: every value in its range.
[[nodiscard]] std::string ToText(const FieldSpec& spec);
[[nodiscard]] bool FromText(const std::string& text, FieldSpec& spec, std::string& error);
[[nodiscard]] bool ValidateField(const FieldSpec& spec, std::string& error);

// "1-10,15" (1-based) into a membership table of `count` lights.
[[nodiscard]] std::vector<char> LightSet(const std::string& ranges, int count, bool emptyMeansAll);

} // namespace composer
