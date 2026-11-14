#include "composer/ProceduralField.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

#define COMPOSER_TEXT(text) text

namespace composer {

namespace {

constexpr double PI = 3.14159265358979323846;

double Smooth(double e0, double e1, double x) {
    if (e1 <= e0) {
        return x >= e1 ? 1.0 : 0.0;
    }
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}

// The light a front gives a place `tau` seconds after it arrived: it rises over the
// attack, holds while the front's thickness passes, and falls over the release.
double Front(const FieldSpec& s, double tau) {
    if (tau < 0) {
        return 0;
    }
    const double hold = s.width / std::max(1e-6, s.speed);
    return Smooth(0, std::max(1e-4, s.attack), tau) * (1.0 - Smooth(hold, hold + std::max(1e-4, s.release), tau));
}

double Loss(const FieldSpec& s, double d) {
    return s.loss > 0 ? std::exp(-s.loss * std::max(0.0, d)) : 1.0;
}

// A band 0-1 over a repeating phase: lit near the start of each period.
double Band(double phase, double share) {
    const double f = phase - std::floor(phase);
    return f < share ? std::sin(PI * f / std::max(1e-6, share)) : 0.0;
}

// The flow's phases from its seed: deterministic, the same on every machine.
double SeedPhase(uint32_t seed, int k) {
    uint32_t h = seed * 2654435761u + static_cast<uint32_t>(k) * 40503u + 0x9E3779B9u;
    h ^= h >> 15;
    h *= 2246822519u;
    h ^= h >> 13;
    return (h % 10000) / 10000.0 * 2 * PI;
}

// The envelope of the whole gesture: in over the attack, out over the release at its end.
double Life(const FieldSpec& s, double t) {
    if (t < 0 || t > s.duration) {
        return 0;
    }
    return Smooth(0, std::max(1e-4, s.attack), t) * (1.0 - Smooth(s.duration - std::max(1e-4, s.release), s.duration, t));
}

std::string Num(double v) {
    std::string s = fmt::format("{:.4f}", v);
    while (s.size() > 1 && s.back() == '0') {
        s.pop_back();
    }
    if (!s.empty() && s.back() == '.') {
        s.pop_back();
    }
    return s == "-0" ? "0" : s;
}

} // namespace

double FieldDistance(const FieldSpec& s, double x, double y) {
    const double dx = x - s.ox;
    const double dy = y - s.oy;
    if (s.shape == "linear") {
        const double a = s.angle * PI / 180.0;
        return dx * std::cos(a) + dy * std::sin(a);
    }
    if (s.shape == "elliptic") {
        return std::sqrt((dx / std::max(1e-6, s.ex)) * (dx / std::max(1e-6, s.ex)) + (dy / std::max(1e-6, s.ey)) * (dy / std::max(1e-6, s.ey)));
    }
    return std::sqrt(dx * dx + dy * dy);
}

double ArrivalTime(const FieldSpec& s, double x, double y) {
    switch (s.function) {
    case FieldFunction::Wave:
    case FieldFunction::Ripple:
    case FieldFunction::Expansion:
    case FieldFunction::Sweep:
    case FieldFunction::Decay:
    case FieldFunction::Echo: {
        if (s.function == FieldFunction::Expansion && s.contract) {
            return -1; // lit from the start, it goes dark from the outside in
        }
        return std::max(0.0, FieldDistance(s, x, y)) / std::max(1e-6, s.speed);
    }
    default:
        return -1;
    }
}

std::pair<double, double> StagePoint(const FieldSpec& s, double sx, double sy, double sz) {
    if (s.transform.size() != 8) {
        return { sx, sy };
    }
    const std::vector<double>& m = s.transform;
    return { m[0] * sx + m[1] * sy + m[2] * sz + m[3], m[4] * sx + m[5] * sy + m[6] * sz + m[7] };
}

FieldSample EvaluateField(const FieldSpec& s, double x, double y, double t) {
    FieldSample out;
    if (t < 0) {
        return out;
    }
    const double d = FieldDistance(s, x, y);
    const double v = std::max(1e-6, s.speed);
    double i = 0;
    double position = std::clamp(std::fabs(d), 0.0, 1.0);
    switch (s.function) {
    case FieldFunction::Pulse:
        i = Life(s, t);
        position = 0;
        break;
    case FieldFunction::Wave:
        i = Loss(s, d) * Front(s, t - std::max(0.0, d) / v);
        break;
    case FieldFunction::Ripple:
    case FieldFunction::Echo: {
        double g = 1;
        for (int k = 0; k < std::max(1, s.repeats); ++k) {
            i = std::max(i, g * Front(s, t - std::max(0.0, d) / v - k * s.period));
            g *= s.gain;
        }
        i *= Loss(s, d);
        break;
    }
    case FieldFunction::Expansion: {
        // Lit inside the edge, the edge soft over the front's width; out at the end.
        const double edge = s.contract ? std::max(0.0, v * (s.duration - t)) : v * t;
        i = Loss(s, d) * (1.0 - Smooth(edge - s.width, edge, std::fabs(d))) * Life(s, t);
        break;
    }
    case FieldFunction::Sweep: {
        const double edge = v * t;
        i = Loss(s, d) * (1.0 - Smooth(edge - s.width, edge, d)) * (d >= -s.width ? 1.0 : 0.0) * Life(s, t);
        position = std::clamp(d, 0.0, 1.0);
        break;
    }
    case FieldFunction::Chase: {
        const double a = s.angle * PI / 180.0;
        const double along = (x - s.ox) * std::cos(a) + (y - s.oy) * std::sin(a);
        const double spacing = std::max(1e-3, v * s.period);
        i = Band((along - v * t) / spacing, std::clamp(s.width / spacing, 0.05, 1.0)) * Life(s, t);
        position = (along - v * t) / spacing - std::floor((along - v * t) / spacing);
        break;
    }
    case FieldFunction::Spiral: {
        const double theta = std::atan2(y - s.oy, x - s.ox);
        const double phase = theta / (2 * PI) * std::max(1.0, s.turns) + d * s.scale - t / std::max(1e-3, s.period);
        i = Band(phase, 0.35) * Loss(s, d) * Life(s, t);
        position = phase - std::floor(phase);
        break;
    }
    case FieldFunction::Flow: {
        const double a = s.angle * PI / 180.0;
        const double u = x * std::cos(a) + y * std::sin(a);
        const double w = -x * std::sin(a) + y * std::cos(a);
        const double omega = 2 * PI / std::max(1e-3, s.period);
        const double f = std::sin(s.scale * u * 2 * PI - omega * t + SeedPhase(s.seed, 1)) * std::cos(s.scale * 0.7 * w * 2 * PI + omega * 0.5 * t + SeedPhase(s.seed, 2));
        i = (0.5 + 0.5 * f) * Life(s, t);
        position = 0.5 + 0.5 * f;
        break;
    }
    case FieldFunction::Fade:
        i = Life(s, t);
        position = 0;
        break;
    case FieldFunction::Gradient: {
        const double a = s.angle * PI / 180.0;
        const double along = std::clamp((x - s.ox) * std::cos(a) + (y - s.oy) * std::sin(a) + 0.5, 0.0, 1.0);
        i = 0.35 + 0.65 * along;
        position = along;
        break;
    }
    case FieldFunction::Decay: {
        const double tau = t - std::max(0.0, d) / v;
        i = tau < 0 ? 0 : Smooth(0, std::max(1e-4, s.attack), tau) * std::exp(-tau / std::max(1e-3, s.release)) * Loss(s, d);
        break;
    }
    }
    out.intensity = static_cast<float>(std::clamp(i * std::clamp(s.level, 0.0, 1.0), 0.0, 1.0));
    out.position = static_cast<float>(std::clamp(position, 0.0, 1.0));
    return out;
}

namespace {
const std::vector<std::pair<FieldFunction, const char*>>& Ids() {
    static const std::vector<std::pair<FieldFunction, const char*>> I = {
        { FieldFunction::Pulse, "pulse" },   { FieldFunction::Wave, "wave" },   { FieldFunction::Ripple, "ripple" },     { FieldFunction::Expansion, "expansion" },
        { FieldFunction::Sweep, "sweep" },   { FieldFunction::Chase, "chase" }, { FieldFunction::Spiral, "spiral" },     { FieldFunction::Flow, "flow" },
        { FieldFunction::Fade, "fade" },     { FieldFunction::Gradient, "gradient" }, { FieldFunction::Decay, "decay" }, { FieldFunction::Echo, "echo" }
    };
    return I;
}
} // namespace

const char* FieldFunctionId(FieldFunction f) {
    for (const auto& [k, id] : Ids()) {
        if (k == f) {
            return id;
        }
    }
    return "wave";
}

bool FieldFunctionFromId(const std::string& id, FieldFunction& f) {
    for (const auto& [k, name] : Ids()) {
        if (id == name) {
            f = k;
            return true;
        }
    }
    return false;
}

const std::vector<FieldFunction>& AllFieldFunctions() {
    static std::vector<FieldFunction> all;
    if (all.empty()) {
        for (const auto& [k, id] : Ids()) {
            all.push_back(k);
        }
    }
    return all;
}

std::string FieldFunctionPhrase(FieldFunction f) {
    switch (f) {
    case FieldFunction::Pulse: return COMPOSER_TEXT("A flash of the whole part");
    case FieldFunction::Wave: return COMPOSER_TEXT("A wave that travels from a point");
    case FieldFunction::Ripple: return COMPOSER_TEXT("Ripples, one after another");
    case FieldFunction::Expansion: return COMPOSER_TEXT("Light that spreads out (or gathers in)");
    case FieldFunction::Sweep: return COMPOSER_TEXT("A sweep across");
    case FieldFunction::Chase: return COMPOSER_TEXT("Bands that chase each other");
    case FieldFunction::Spiral: return COMPOSER_TEXT("A turning spiral");
    case FieldFunction::Flow: return COMPOSER_TEXT("A slow, flowing pattern");
    case FieldFunction::Fade: return COMPOSER_TEXT("A gentle fade in and out");
    case FieldFunction::Gradient: return COMPOSER_TEXT("A still slope of light");
    case FieldFunction::Decay: return COMPOSER_TEXT("Lit at once, then dying away");
    case FieldFunction::Echo: return COMPOSER_TEXT("A wave and its echoes");
    }
    return "";
}

std::string ToText(const FieldSpec& s) {
    std::string out = fmt::format("fn={};ox={};oy={};ang={};shape={};ex={};ey={};v={};w={};dur={};att={};rel={};loss={};per={};rep={};gain={};turns={};scale={};contract={};seed={};level={};at={}",
                                  FieldFunctionId(s.function), Num(s.ox), Num(s.oy), Num(s.angle), s.shape, Num(s.ex), Num(s.ey), Num(s.speed), Num(s.width), Num(s.duration), Num(s.attack),
                                  Num(s.release), Num(s.loss), Num(s.period), s.repeats, Num(s.gain), Num(s.turns), Num(s.scale), s.contract ? 1 : 0, s.seed, Num(s.level), Num(s.at));
    if (s.transform.size() == 8) {
        out += ";m=";
        for (size_t k = 0; k < 8; ++k) {
            out += (k > 0 ? "," : "") + fmt::format("{:.6g}", s.transform[k]);
        }
    }
    if (!s.only.empty()) {
        out += ";only=" + s.only;
    }
    if (!s.skip.empty()) {
        out += ";skip=" + s.skip;
    }
    return out;
}

bool ValidateField(const FieldSpec& s, std::string& error) {
    const auto bad = [&](const char* what) {
        error = std::string("field: ") + what;
        return false;
    };
    const auto finite = [](double v) { return v == v && v > -1e9 && v < 1e9; };
    for (double v : { s.ox, s.oy, s.angle, s.ex, s.ey, s.speed, s.width, s.duration, s.attack, s.release, s.loss, s.period, s.gain, s.turns, s.scale, s.level, s.at }) {
        if (!finite(v)) {
            return bad("a value is not a number");
        }
    }
    if (s.shape != "radial" && s.shape != "linear" && s.shape != "elliptic") {
        return bad("shape must be radial, linear or elliptic");
    }
    if (s.speed <= 0 || s.speed > 50) {
        return bad("speed must be over 0 and at most 50 stage units a second");
    }
    if (s.width <= 0 || s.width > 2) {
        return bad("width must be over 0 and at most 2");
    }
    if (s.duration <= 0 || s.duration > 600) {
        return bad("duration must be over 0 and at most 600 s");
    }
    if (s.attack < 0 || s.release < 0 || s.attack > 60 || s.release > 60) {
        return bad("attack and release must be 0-60 s");
    }
    if (s.loss < 0 || s.loss > 50) {
        return bad("loss must be 0-50");
    }
    if (s.period <= 0 || s.period > 60) {
        return bad("period must be over 0 and at most 60 s");
    }
    if (s.repeats < 1 || s.repeats > 16) {
        return bad("repeats must be 1-16");
    }
    if (s.gain < 0 || s.gain > 1 || s.level < 0 || s.level > 1) {
        return bad("gain and level must be 0-1");
    }
    if (s.ex <= 0 || s.ey <= 0 || s.turns <= 0 || s.scale <= 0) {
        return bad("ellipse, turns and scale must be over 0");
    }
    if (s.at < -60 || s.at > 600) {
        return bad("a light joins a gesture from 60 s before it to 600 s after");
    }
    if (!s.transform.empty() && s.transform.size() != 8) {
        return bad("the figure's place needs 8 numbers");
    }
    for (double v : s.transform) {
        if (!finite(v)) {
            return bad("the figure's place is not a number");
        }
    }
    return true;
}

bool FromText(const std::string& text, FieldSpec& out, std::string& error) {
    FieldSpec s;
    size_t at = 0;
    while (at < text.size()) {
        const size_t end = std::min(text.find(';', at), text.size());
        const std::string item = text.substr(at, end - at);
        at = end + 1;
        if (item.empty()) {
            continue;
        }
        const size_t eq = item.find('=');
        if (eq == std::string::npos) {
            error = "field: '" + item + "' has no value";
            return false;
        }
        const std::string key = item.substr(0, eq);
        const std::string value = item.substr(eq + 1);
        const double num = std::strtod(value.c_str(), nullptr);
        if (key == "fn") {
            if (!FieldFunctionFromId(value, s.function)) {
                error = "field: unknown function '" + value + "'";
                return false;
            }
        } else if (key == "ox") {
            s.ox = num;
        } else if (key == "oy") {
            s.oy = num;
        } else if (key == "ang") {
            s.angle = num;
        } else if (key == "shape") {
            s.shape = value;
        } else if (key == "ex") {
            s.ex = num;
        } else if (key == "ey") {
            s.ey = num;
        } else if (key == "v") {
            s.speed = num;
        } else if (key == "w") {
            s.width = num;
        } else if (key == "dur") {
            s.duration = num;
        } else if (key == "att") {
            s.attack = num;
        } else if (key == "rel") {
            s.release = num;
        } else if (key == "loss") {
            s.loss = num;
        } else if (key == "per") {
            s.period = num;
        } else if (key == "rep") {
            s.repeats = static_cast<int>(std::strtol(value.c_str(), nullptr, 10));
        } else if (key == "gain") {
            s.gain = num;
        } else if (key == "turns") {
            s.turns = num;
        } else if (key == "scale") {
            s.scale = num;
        } else if (key == "contract") {
            s.contract = value == "1";
        } else if (key == "seed") {
            s.seed = static_cast<uint32_t>(std::strtoul(value.c_str(), nullptr, 10));
        } else if (key == "level") {
            s.level = num;
        } else if (key == "at") {
            s.at = num;
        } else if (key == "m") {
            s.transform.clear();
            size_t p = 0;
            while (p <= value.size()) {
                const size_t c = std::min(value.find(',', p), value.size());
                s.transform.push_back(std::strtod(value.substr(p, c - p).c_str(), nullptr));
                p = c + 1;
                if (c == value.size()) {
                    break;
                }
            }
        } else if (key == "only") {
            s.only = value;
        } else if (key == "skip") {
            s.skip = value;
        } else {
            error = "field: unknown setting '" + key + "'";
            return false;
        }
    }
    if (!ValidateField(s, error)) {
        return false;
    }
    out = std::move(s);
    return true;
}

std::vector<char> LightSet(const std::string& ranges, int count, bool emptyMeansAll) {
    std::vector<char> set(static_cast<size_t>(std::max(0, count)), static_cast<char>(ranges.empty() && emptyMeansAll ? 1 : 0));
    size_t at = 0;
    while (at < ranges.size()) {
        const size_t end = std::min(ranges.find(',', at), ranges.size());
        const std::string item = ranges.substr(at, end - at);
        at = end + 1;
        const size_t dash = item.find('-');
        long a = std::strtol(item.c_str(), nullptr, 10);
        long b = dash == std::string::npos ? a : std::strtol(item.c_str() + dash + 1, nullptr, 10);
        if (a > b) {
            std::swap(a, b);
        }
        for (long n = std::max(1L, a); n <= b && n <= count; ++n) {
            set[static_cast<size_t>(n - 1)] = 1;
        }
    }
    return set;
}

} // namespace composer
