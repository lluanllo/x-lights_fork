#include "composer/ChromaticDirector.h"

#include "composer/AudioUnderstanding.h"
#include "composer/Choreography.h"
#include "composer/CompositionPlan.h"
#include "composer/ModelCapabilities.h"
#include "composer/SongAnalysis.h"
#include "composer/SongVisualPlan.h"
#include "composer/VisualDirection.h"
#include "composer/VisualReview.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#define COMPOSER_TEXT(text) text

namespace composer {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

// The Christmas colours on OKLCH's circle: red, green, gold and an icy blue.
constexpr double CHRISTMAS_RED = 27;
constexpr double CHRISTMAS_GREEN = 145;
constexpr double CHRISTMAS_GOLD = 82;
constexpr double ICE_BLUE = 232;
constexpr double DISTINCT = 10;   // ΔE under which two colours of a palette read as one
constexpr double STANDS_OUT = 18; // ΔE from which a gesture's front reads against its background

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

bool HasFlag(const std::vector<std::string>& flags, const char* f) {
    return std::find(flags.begin(), flags.end(), f) != flags.end();
}

// A related hue moves to a colour the song suggests when one is near enough.
double Snap(double hue, const std::vector<std::pair<double, double>>& suggested, std::string& snappedTo) {
    double best = 36;
    double target = hue;
    double weight = 0;
    for (const auto& [h, w] : suggested) {
        const double d = HueDistance(hue, h);
        if (w > 0 && d < best) {
            best = d;
            target = h;
            weight = w;
        }
    }
    if (weight <= 0) {
        return hue;
    }
    double diff = WrapHue(target - hue);
    if (diff > 180) {
        diff -= 360;
    }
    snappedTo = fmt::format("{:.0f}", target);
    return WrapHue(hue + diff * std::clamp(weight, 0.0, 1.0) * 0.85);
}

PaletteColor Make(const std::string& role, double L, double C, double h, const std::string& why) {
    L = std::clamp(L, 0.2, 0.97);
    h = WrapHue(h);
    C = std::clamp(C, 0.0, std::min(0.32, MaxChroma(L, h)));
    PaletteColor p;
    p.role = role;
    const Rgb rgb = ToRgbInGamut(Oklch { L, C, h });
    p.hex = ToHex(rgb);
    const Oklch back = ToOklch(rgb);
    p.L = Round3(back.L);
    p.C = Round3(back.C);
    p.h = std::round(back.h);
    p.why = why;
    return p;
}

double ChromaAt(double L, double h, double saturation) {
    return std::clamp(saturation, 0.0, 1.0) * std::min(0.3, MaxChroma(L, h));
}

struct Offsets {
    double secondary, accent, contrast;
};

Offsets OffsetsOf(Harmony h) {
    switch (h) {
    case Harmony::Monochromatic: return { 0, 0, 0 };
    case Harmony::Analogous: return { 30, -30, 60 };
    case Harmony::Complementary: return { 25, 180, 180 };
    case Harmony::SplitComplementary: return { 150, 210, 180 };
    case Harmony::Triadic: return { 120, 240, 120 }; // the contrast: the second vertex, at another lightness
    case Harmony::Tetradic: return { 60, 180, 240 };
    case Harmony::WarmCool: return { 0, 25, 0 }; // the cool or warm side is found from the anchor
    }
    return { 30, -30, 60 };
}

// How much contrast of hue a harmony carries, 0-1.
double HarmonyContrast(Harmony h) {
    switch (h) {
    case Harmony::Monochromatic: return 0.05;
    case Harmony::Analogous: return 0.3;
    case Harmony::WarmCool: return 0.6;
    case Harmony::SplitComplementary: return 0.7;
    case Harmony::Triadic: return 0.75;
    case Harmony::Complementary: return 0.85;
    case Harmony::Tetradic: return 0.95;
    }
    return 0.3;
}

const std::vector<Harmony>& AllHarmonies() {
    static const std::vector<Harmony> all = { Harmony::Monochromatic, Harmony::Analogous, Harmony::Complementary, Harmony::SplitComplementary, Harmony::Triadic, Harmony::Tetradic, Harmony::WarmCool };
    return all;
}

// How well a mood suits a harmony, 0-1: a starting point, never a rule.
double MoodFit(const std::string& mood, Harmony h) {
    static const std::map<std::string, std::map<Harmony, double>> FIT = {
        { "festive", { { Harmony::Complementary, 0.85 }, { Harmony::Triadic, 0.85 }, { Harmony::SplitComplementary, 0.75 }, { Harmony::Tetradic, 0.7 }, { Harmony::WarmCool, 0.6 }, { Harmony::Analogous, 0.45 }, { Harmony::Monochromatic, 0.2 } } },
        { "joyful", { { Harmony::Triadic, 0.85 }, { Harmony::SplitComplementary, 0.8 }, { Harmony::Complementary, 0.7 }, { Harmony::Tetradic, 0.7 }, { Harmony::Analogous, 0.55 }, { Harmony::WarmCool, 0.55 }, { Harmony::Monochromatic, 0.25 } } },
        { "playful", { { Harmony::Triadic, 0.9 }, { Harmony::Tetradic, 0.85 }, { Harmony::SplitComplementary, 0.7 }, { Harmony::Complementary, 0.6 }, { Harmony::Analogous, 0.45 }, { Harmony::WarmCool, 0.5 }, { Harmony::Monochromatic, 0.2 } } },
        { "epic", { { Harmony::Complementary, 0.9 }, { Harmony::WarmCool, 0.8 }, { Harmony::SplitComplementary, 0.7 }, { Harmony::Triadic, 0.55 }, { Harmony::Analogous, 0.5 }, { Harmony::Tetradic, 0.45 }, { Harmony::Monochromatic, 0.45 } } },
        { "tender", { { Harmony::Analogous, 0.9 }, { Harmony::Monochromatic, 0.8 }, { Harmony::WarmCool, 0.5 }, { Harmony::SplitComplementary, 0.45 }, { Harmony::Complementary, 0.35 }, { Harmony::Triadic, 0.3 }, { Harmony::Tetradic, 0.2 } } },
        { "calm", { { Harmony::Analogous, 0.9 }, { Harmony::Monochromatic, 0.8 }, { Harmony::WarmCool, 0.55 }, { Harmony::SplitComplementary, 0.45 }, { Harmony::Complementary, 0.35 }, { Harmony::Triadic, 0.3 }, { Harmony::Tetradic, 0.2 } } },
        { "nostalgic", { { Harmony::Analogous, 0.85 }, { Harmony::WarmCool, 0.7 }, { Harmony::Monochromatic, 0.65 }, { Harmony::SplitComplementary, 0.5 }, { Harmony::Complementary, 0.45 }, { Harmony::Triadic, 0.35 }, { Harmony::Tetradic, 0.25 } } },
        { "dark", { { Harmony::Monochromatic, 0.85 }, { Harmony::Complementary, 0.65 }, { Harmony::Analogous, 0.6 }, { Harmony::WarmCool, 0.6 }, { Harmony::SplitComplementary, 0.45 }, { Harmony::Triadic, 0.3 }, { Harmony::Tetradic, 0.25 } } },
    };
    const auto m = FIT.find(mood);
    if (m == FIT.end()) {
        return h == Harmony::Analogous || h == Harmony::SplitComplementary ? 0.6 : h == Harmony::Monochromatic ? 0.35 : 0.5;
    }
    const auto f = m->second.find(h);
    return f == m->second.end() ? 0.4 : f->second;
}

// The hues a harmony relates to the anchor, for checking which the song's own colours match.
std::vector<double> RelatedHues(Harmony h, double anchor, int dir) {
    if (h == Harmony::WarmCool) {
        return { Warmth(anchor) >= 0 ? ICE_BLUE : 60.0 };
    }
    const Offsets o = OffsetsOf(h);
    std::vector<double> out;
    for (double off : { o.secondary, o.accent, o.contrast }) {
        if (off != 0) {
            out.push_back(WrapHue(anchor + dir * off));
        }
    }
    return out;
}

// --------------------------------------------------------------- figure facts

struct FigureFacts {
    std::string name;
    std::string family;
    int nodes = 0;
    double cx = 0.5;
    bool group = false;
    std::vector<std::string> flatFamilies;
    std::vector<std::string> identity; // its own colours, the largest part first
    bool character = false;
};

} // namespace

// =========================================================== palettes

const PaletteColor* ColorPaletteProfile::Get(const std::string& role) const {
    for (const PaletteColor& c : colors) {
        if (c.role == role) {
            return &c;
        }
    }
    return nullptr;
}

std::string ColorPaletteProfile::Hex(const std::string& role) const {
    const PaletteColor* c = Get(role);
    return c == nullptr ? std::string() : c->hex;
}

ColorPaletteProfile BuildPalette(const PaletteRequest& r) {
    ColorPaletteProfile p;
    p.id = r.id;
    p.harmony = r.harmony;
    p.anchorHue = WrapHue(r.anchorHue);
    p.saturation = std::clamp(r.saturation, 0.0, 1.0);
    p.lightness = std::clamp(r.lightness, 0.35, 0.85);
    const int dir = r.direction >= 0 ? 1 : -1;
    const double k = 0.6 + 0.8 * std::clamp(r.contrast, 0.0, 1.0); // how far lightness spreads
    const double h0 = p.anchorHue;
    // Each hue at its fullest for a light: near the cusp of the gamut, moved by how light
    // the palette is asked to be (0.66 the middle).
    const double lift = p.lightness - 0.66;
    // Lighter only where the hue's fullest is not already light: a lighter cyan or yellow is
    // only whiter.
    const auto at = [&](double h, double extra) {
        const double cusp = CuspLightness(h);
        return std::clamp(cusp + lift + (extra > 0 && cusp > 0.8 ? -0.25 * extra : extra), 0.3, 0.9);
    };
    // How far from the anchor's lightness a colour may go and still show its hue.
    const auto keepHue = [&](double L, double h) { return std::clamp(L, std::max(0.3, CuspLightness(h) - 0.18), std::min(0.9, CuspLightness(h) + 0.1)); };
    const double L0 = at(h0, 0);
    const double s = p.saturation;
    const auto chroma = [&](double L, double h, double factor) { return ChromaAt(L, h, s * factor); };
    p.colors.push_back(Make("anchor", L0, chroma(L0, h0, 1.0), h0, COMPOSER_TEXT("sets the identity")));

    std::string snapped;
    const auto related = [&](double off, const char* role) {
        std::string to;
        const double h = Snap(h0 + dir * off, r.suggested, to);
        if (!to.empty()) {
            snapped += fmt::format("{}{} to {}", snapped.empty() ? "" : ", ", role, to);
        }
        return h;
    };
    if (r.harmony == Harmony::Monochromatic) {
        const double L1 = std::min(0.9, L0 + 0.1 * k);
        p.colors.push_back(Make("secondary", L1, chroma(L1, h0, 0.55), h0, COMPOSER_TEXT("the same hue, lighter and softer")));
        p.colors.push_back(Make("accent", 0.9, chroma(0.9, h0, 0.35), h0, COMPOSER_TEXT("the same hue almost white: it shines without changing colour")));
        const double L3 = std::max(0.32, L0 - 0.22 * k);
        p.colors.push_back(Make("contrast", L3, chroma(L3, h0, 0.95), h0, COMPOSER_TEXT("the same hue, deep: contrast of light, not of colour")));
    } else if (r.harmony == Harmony::WarmCool) {
        const bool warm = Warmth(h0) >= 0;
        std::string to;
        const double other = Snap(warm ? ICE_BLUE : 60.0, r.suggested, to);
        const double L1 = at(other, 0.02);
        p.colors.push_back(Make("secondary", L1, chroma(L1, other, 0.95), other, warm ? COMPOSER_TEXT("a cool colour against the warm anchor") : COMPOSER_TEXT("a warm colour against the cool anchor")));
        const double ha = related(25, "accent");
        const double La = at(ha, 0.08);
        p.colors.push_back(Make("accent", La, chroma(La, ha, 1.0), ha, COMPOSER_TEXT("a neighbour of the anchor, bright")));
        const double away = (0.08 + 0.22 * std::clamp(r.contrast, 0.0, 1.0)) * (CuspLightness(other) >= L0 ? 1.0 : -1.0);
        const double L3 = keepHue(L0 + away, other);
        p.colors.push_back(Make("contrast", L3, chroma(L3, other, 0.7), other, COMPOSER_TEXT("the other temperature, lighter")));
    } else {
        const Offsets o = OffsetsOf(r.harmony);
        const double hs = related(o.secondary, "secondary");
        const double ha = related(o.accent, "accent");
        const double hc = related(o.contrast, "contrast");
        const double L1 = at(hs, 0.02);
        p.colors.push_back(Make("secondary", L1, chroma(L1, hs, 0.95), hs, COMPOSER_TEXT("varies the anchor")));
        const double La = at(ha, 0.08);
        p.colors.push_back(Make("accent", La, chroma(La, ha, 1.0), ha, COMPOSER_TEXT("bright, for what matters")));
        // Its lightness apart from the anchor's, more so with more contrast: towards where
        // its own hue is fullest.
        const double away = (0.08 + 0.22 * std::clamp(r.contrast, 0.0, 1.0)) * (CuspLightness(hc) >= L0 ? 1.0 : -1.0);
        const double L3 = keepHue(L0 + away, hc);
        p.colors.push_back(Make("contrast", L3, chroma(L3, hc, 0.85), hc, COMPOSER_TEXT("separates one thing from another")));
    }
    // A secondary that came out as the anchor is no variation: turn it a little.
    if (DeltaE(p.colors[0].hex, p.colors[1].hex) < DISTINCT && r.harmony != Harmony::Monochromatic) {
        const PaletteColor& old = p.colors[1];
        p.colors[1] = Make("secondary", old.L, old.C, old.h + dir * 22, COMPOSER_TEXT("varies the anchor (turned: it was too close to it)"));
    }
    const Oklch anchor { p.colors[0].L, p.colors[0].C, p.colors[0].h };
    const Oklch secondary { p.colors[1].L, p.colors[1].C, p.colors[1].h };
    const Oklch mid = Mix(anchor, secondary, 0.5);
    p.colors.push_back(Make("transition", mid.L, mid.C, mid.h, COMPOSER_TEXT("halfway from the anchor to the secondary")));
    const bool warm = Warmth(h0) >= 0;
    p.colors.push_back(Make("neutral", 0.94, 0.022, warm ? 75 : 240, warm ? COMPOSER_TEXT("a warm white to rest") : COMPOSER_TEXT("a cool white to rest")));
    p.colors.push_back(Make("shadow", std::max(0.28, L0 * 0.58), chroma(std::max(0.28, L0 * 0.58), h0, 0.8), h0, COMPOSER_TEXT("the anchor deep, for a background")));
    p.colors.push_back(Make("highlight", 0.88, chroma(0.88, h0, 0.4), h0, COMPOSER_TEXT("the anchor pale, for sparkles")));
    p.reason = fmt::format("{} round {:.0f}°{}", HarmonyPhrase(r.harmony), h0, snapped.empty() ? std::string() : "; moved towards the song's colours: " + snapped);
    return p;
}

// =========================================================== the director

const FigureColorRole* ChromaticScenePlan::RoleOf(const std::string& figure) const {
    for (const FigureColorRole& r : figures) {
        if (r.figure == figure) {
            return &r;
        }
    }
    return nullptr;
}

const ChromaticScenePlan* ChromaticScore::SceneAt(int ms) const {
    for (const ChromaticScenePlan& s : scenes) {
        if (ms >= s.startMS && ms < s.endMS) {
            return &s;
        }
    }
    return nullptr;
}

const ChromaticScenePlan* ChromaticScore::Scene(const std::string& id) const {
    for (const ChromaticScenePlan& s : scenes) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

namespace {

struct SceneSource {
    std::string id;
    int startMS = 0;
    int endMS = 0;
    std::string sectionKind;
    std::string mode;
    const StageScene* stage = nullptr;
    const PlanSegment* segment = nullptr;
    const VisualPlanScene* visual = nullptr;
};

class Director {
public:
    explicit Director(const ChromaticInput& in) :
        _in(in), _o(in.options) {
    }

    ChromaticScore Run() {
        ChromaticScore score;
        score.options = _o;
        if (_in.plan == nullptr || _in.analysis == nullptr) {
            score.warnings.push_back("missing-required-input");
            return score;
        }
        if (!_o.enabled) {
            score.warnings.push_back("colour-direction-off");
            return score;
        }
        score.enabled = true;
        score.edits = _in.edits == nullptr ? 0 : static_cast<int>(_in.edits->edits.size());
        Facts();
        Identity(score);
        Scenes(score);
        for (size_t i = 0; i < _sources.size(); ++i) {
            ChromaticScenePlan sp = Scene(score, i);
            Roles(score, sp, _sources[i]);
            score.scenes.push_back(std::move(sp));
        }
        for (size_t i = 0; i < score.scenes.size(); ++i) {
            Gestures(score, score.scenes[i], i);
            Transition(score, i);
        }
        for (const ChromaticMotif& m : score.motifDefs) {
            score.motifs.push_back(fmt::format("{} {} ({} scenes)", m.id, m.kind, m.scenes.size()));
        }
        Check(score);
        return score;
    }

private:
    // ------------------------------------------------------------ facts
    void Facts() {
        if (_in.capabilities != nullptr) {
            for (const TargetInfo& t : _in.capabilities->targets) {
                FigureFacts f;
                f.name = t.name;
                f.family = FamilyIdOf(t);
                f.nodes = t.nodes;
                f.group = t.type == TargetType::Group;
                if (f.group) {
                    std::set<std::string> fams;
                    for (const std::string& m : t.flatModels) {
                        if (const TargetInfo* mt = _in.capabilities->Find(m); mt != nullptr) {
                            fams.insert(FamilyIdOf(*mt));
                        }
                    }
                    f.flatFamilies.assign(fams.begin(), fams.end());
                }
                if (_in.space != nullptr) {
                    if (const StageFigure* sf = _in.space->Find(t.name); sf != nullptr) {
                        f.cx = sf->cx;
                    }
                }
                _facts[f.name] = f;
            }
        }
        if (_in.semantics != nullptr) {
            for (const FigureProfile& p : _in.semantics->profiles) {
                FigureFacts& f = _facts[p.figure];
                f.name = p.figure;
                std::vector<const FigureRegion*> regions;
                bool manual = false;
                for (const FigureRegion& r : p.regions) {
                    if (r.real && !r.color.empty() && !r.nodes.empty() && r.id != "mouth" && r.id != "eyes" && r.id != "face" && r.id != "text" && r.id != "image" &&
                        (r.confirmed || r.confidence >= 0.8)) {
                        regions.push_back(&r);
                        manual |= r.colorSource == "manual";
                    }
                }
                std::stable_sort(regions.begin(), regions.end(), [](const FigureRegion* a, const FigureRegion* b) { return a->nodes.size() > b->nodes.size(); });
                for (const FigureRegion* r : regions) {
                    if (std::find(f.identity.begin(), f.identity.end(), r->color) == f.identity.end()) {
                        f.identity.push_back(r->color);
                    }
                }
                const bool character = std::find(p.roles.begin(), p.roles.end(), "character") != p.roles.end() || std::find(p.roles.begin(), p.roles.end(), "narrator") != p.roles.end();
                // A character keeps its colours; any figure whose colours a person chose too,
                // and every figure with parts of its own colour when the maker asked for it.
                f.character = !f.identity.empty() && (character || manual || _o.respectIdentity);
            }
        }
        if (_in.edits != nullptr) {
            for (const ChromaticEdit& e : _in.edits->edits) {
                if (e.action == "identity" && !e.figure.empty() && e.sceneStartMS < 0) {
                    _pinned.insert(e.figure);
                } else if (e.action == "no-identity" && !e.figure.empty() && e.sceneStartMS < 0) {
                    _unpinned.insert(e.figure);
                }
            }
        }
    }

    // ------------------------------------------------------------ the song
    void Identity(ChromaticScore& score) {
        const SongVisualBrief* b = _in.brief;
        score.briefSource = b != nullptr ? b->source : std::string();
        if (b != nullptr && !b->origin.empty() && b->origin != "local") {
            score.briefSource += fmt::format(" ({}{})", b->origin == "request" ? "asked now" : b->origin == "cache" ? "kept answer" : "read from a file",
                                             b->analyzedAt.empty() ? std::string() : ", answered " + b->analyzedAt.substr(0, 10));
        }
        double energy = 0;
        int counted = 0;
        for (const PlanSegment& seg : _in.plan->segments) {
            if (!HasFlag(seg.flags, "silence")) {
                energy += seg.energy;
                ++counted;
            }
        }
        energy = counted > 0 ? energy / counted : 0.5;
        score.mood = b != nullptr && !b->mood.empty() ? b->mood : b != nullptr && !b->character.empty() ? MoodOfCharacter(b->character.front()) : energy >= 0.6 ? "festive" : "calm";
        _mood = score.mood;
        // The hues the song speaks of, with their weight.
        std::vector<double> songHues;
        for (const std::string& c : _o.songColors) {
            const Oklch k = OklchOfHex(c);
            if (k.C >= NEUTRAL_CHROMA) {
                _suggested.push_back({ k.h, 1.0 });
                songHues.push_back(k.h);
            }
        }
        if (b != nullptr) {
            // At most its first three colours: a reading suggests an identity, not a list to use.
            for (size_t i = 0; i < b->colors.size() && i < 3; ++i) {
                const std::string& c = b->colors[i];
                const Oklch k = OklchOfHex(c);
                if (k.C >= NEUTRAL_CHROMA) {
                    _suggested.push_back({ k.h, 0.8 });
                    if (_o.songColors.empty()) {
                        songHues.push_back(k.h);
                    }
                }
            }
        }
        // What the maker taught about warmth: a pull towards that side of the circle.
        if (std::fabs(_o.warmth) >= 0.1) {
            _suggested.push_back({ _o.warmth > 0 ? 55.0 : ICE_BLUE, 0.4 * std::fabs(_o.warmth) });
        }
        // How warm the interpretation feels the song: a pull towards that side of the circle.
        if (b != nullptr && b->warmth >= -1 && std::fabs(b->warmth) >= 0.3) {
            _suggested.push_back({ b->warmth > 0 ? 55.0 : ICE_BLUE, 0.35 * std::fabs(b->warmth) });
        }
        if (b != nullptr && b->colorIntensity >= 0) {
            _intensity = 0.75 + 0.35 * b->colorIntensity;
        }
        _intensity *= 0.8 + 0.4 * std::clamp(_o.saturation, 0.0, 1.0);
        const double xmas = std::clamp(_o.christmasInfluence, 0.0, 1.0);
        if (xmas > 0.05) {
            _suggested.push_back({ CHRISTMAS_RED, 0.9 * xmas });
            _suggested.push_back({ CHRISTMAS_GREEN, 0.9 * xmas });
            _suggested.push_back({ CHRISTMAS_GOLD, 0.7 * xmas });
            _suggested.push_back({ ICE_BLUE, 0.4 * xmas });
        }
        // The anchor.
        double anchor = 30;
        if (!_o.songColors.empty() && !songHues.empty()) {
            anchor = songHues.front();
            score.identityReason = COMPOSER_TEXT("the colours a person chose for the song");
        } else if (!songHues.empty()) {
            anchor = songHues.front();
            score.identityReason = fmt::format("the first colour the song suggests ({})", score.briefSource.empty() ? std::string("its words") : score.briefSource);
        } else if (_in.visualPlan != nullptr && !_in.visualPlan->anchorHue.empty()) {
            // The visual plan's hue is HSV; its colour is read back in OKLCH.
            const double hsv = std::strtod(_in.visualPlan->anchorHue.c_str(), nullptr);
            anchor = ToOklch(HsvToRgb(hsv)).h;
            score.identityReason = "the song visual plan's starting hue: " + _in.visualPlan->anchorReason;
        } else {
            score.identityReason = COMPOSER_TEXT("nothing suggests a colour: a warm one");
        }
        if (xmas >= 0.5 && _o.songColors.empty()) {
            std::string to;
            const double snapped = Snap(anchor, { { CHRISTMAS_RED, xmas }, { CHRISTMAS_GREEN, xmas }, { CHRISTMAS_GOLD, xmas } }, to);
            if (!to.empty()) {
                score.identityReason += fmt::format("; {:.0f}% towards the Christmas colours", xmas * 100);
            }
            anchor = snapped;
        }
        _anchor = anchor;
        // The song's harmony: the mood, the contrast asked for, and the song's own colours.
        double best = -1;
        Harmony chosen = Harmony::Analogous;
        std::string why;
        for (Harmony h : AllHarmonies()) {
            double v = MoodFit(_mood, h);
            v += 0.5 * (1.0 - std::fabs(HarmonyContrast(h) - (0.25 + 0.6 * _o.contrast)));
            int matches = 0;
            for (double sh : songHues) {
                for (double rh : RelatedHues(h, anchor, 1)) {
                    matches += HueDistance(sh, rh) <= 30 ? 1 : 0;
                }
                for (double rh : RelatedHues(h, anchor, -1)) {
                    matches += HueDistance(sh, rh) <= 30 ? 1 : 0;
                }
            }
            v += 0.2 * std::min(3, matches);
            if (v > best + 1e-9) {
                best = v;
                chosen = h;
                why = fmt::format("{} suits a {} song{}", HarmonyPhrase(h), _mood, matches > 0 ? fmt::format(", and {} of the song's colours fall on it", matches) : std::string());
            }
        }
        _songHarmony = chosen;
        _direction = DirectionTowards(chosen, anchor, songHues);
        // Warmer or cooler, as the maker taught: the neighbours of the main colour lean that way.
        if (std::fabs(_o.warmth) >= 0.3) {
            const double target = _o.warmth < 0 ? ICE_BLUE : 55.0;
            _direction = WrapHue(target - anchor) <= 180 ? 1 : -1;
        }
        PaletteRequest r;
        r.anchorHue = anchor;
        r.harmony = chosen;
        r.saturation = 0.85;
        r.lightness = 0.66;
        r.contrast = _o.contrast;
        r.direction = _direction;
        r.suggested = _suggested;
        r.id = "song";
        score.song = BuildPalette(r);
        score.song.reason = why + "; " + score.song.reason;
        ApplyPaletteEdits(score.song, -1);
    }

    static Rgb HsvToRgb(double h) {
        // A full colour of the given HSV hue, to read its OKLCH hue.
        const double hh = WrapHue(h) / 60.0;
        const double x = 1 - std::fabs(std::fmod(hh, 2.0) - 1);
        const int i = static_cast<int>(hh);
        switch (i) {
        case 0: return { 1, x, 0 };
        case 1: return { x, 1, 0 };
        case 2: return { 0, 1, x };
        case 3: return { 0, x, 1 };
        case 4: return { x, 0, 1 };
        default: return { 1, 0, x };
        }
    }

    // Which way round the circle the song's related colours go: towards its own colours.
    static int DirectionTowards(Harmony h, double anchor, const std::vector<double>& hues) {
        int plus = 0, minus = 0;
        for (double sh : hues) {
            for (double rh : RelatedHues(h, anchor, 1)) {
                plus += HueDistance(sh, rh) <= 30 ? 1 : 0;
            }
            for (double rh : RelatedHues(h, anchor, -1)) {
                minus += HueDistance(sh, rh) <= 30 ? 1 : 0;
            }
        }
        return minus > plus ? -1 : 1;
    }

    // ------------------------------------------------------------ scenes
    void Scenes(ChromaticScore& score) {
        (void)score;
        const auto segment = [&](const std::string& id) -> const PlanSegment* {
            for (const PlanSegment& s : _in.plan->segments) {
                if (s.id == id) {
                    return &s;
                }
            }
            return nullptr;
        };
        if (_in.stage != nullptr && !_in.stage->scenes.empty()) {
            for (const StageScene& sc : _in.stage->scenes) {
                SceneSource s;
                s.id = sc.id;
                s.startMS = sc.startMS;
                s.endMS = sc.endMS;
                s.sectionKind = sc.sectionKind;
                s.mode = StageModeId(sc.mode);
                s.stage = &sc;
                s.segment = segment(sc.id);
                s.visual = _in.visualPlan != nullptr ? _in.visualPlan->Scene(sc.id) : nullptr;
                _sources.push_back(s);
            }
        } else {
            for (const PlanSegment& seg : _in.plan->segments) {
                SceneSource s;
                s.id = seg.id;
                s.startMS = seg.startMS;
                s.endMS = seg.endMS;
                s.sectionKind = seg.sectionKind;
                s.segment = &seg;
                s.visual = _in.visualPlan != nullptr ? _in.visualPlan->Scene(seg.id) : nullptr;
                _sources.push_back(s);
            }
        }
        for (const SceneSource& src : _sources) {
            if (src.segment != nullptr && src.segment->repeatGroup >= 0) {
                _groupCount[src.segment->repeatGroup]++;
            }
        }
        // Who plays in each scene: the targets of its effects.
        if (_in.choreography != nullptr) {
            for (const ChoreographyEvent& e : _in.choreography->events) {
                for (size_t i = 0; i < _sources.size(); ++i) {
                    const SceneSource& s = _sources[i];
                    if (e.segmentId == s.id || (e.segmentId.empty() && e.startMS >= s.startMS && e.startMS < s.endMS)) {
                        _players[i].insert(e.target);
                        break;
                    }
                }
            }
        }
    }

    const ChromaticEdit* SceneEdit(const std::string& action, int startMS, const std::string& role = "") const {
        if (_in.edits == nullptr) {
            return nullptr;
        }
        const ChromaticEdit* found = nullptr;
        for (const ChromaticEdit& e : _in.edits->edits) {
            if (e.action == action && (role.empty() || e.role == role) && ((e.sceneStartMS < 0 && startMS < 0) || (e.sceneStartMS >= 0 && startMS >= 0 && std::abs(e.sceneStartMS - startMS) <= 1500))) {
                found = &e; // the last one wins
            }
        }
        return found;
    }

    void ApplyPaletteEdits(ColorPaletteProfile& p, int startMS) {
        if (_in.edits == nullptr) {
            return;
        }
        for (const ChromaticEdit& e : _in.edits->edits) {
            const bool here = (e.sceneStartMS < 0 && startMS < 0) || (e.sceneStartMS >= 0 && startMS >= 0 && std::abs(e.sceneStartMS - startMS) <= 1500);
            if (!here) {
                continue;
            }
            const auto put = [&](const std::string& role, const std::string& hex, const std::string& why) {
                Rgb rgb;
                if (!ParseHex(hex, rgb)) {
                    return;
                }
                const Oklch k = ToOklch(rgb);
                for (PaletteColor& c : p.colors) {
                    if (c.role == role) {
                        c.hex = ToHex(rgb);
                        c.L = Round3(k.L);
                        c.C = Round3(k.C);
                        c.h = std::round(k.h);
                        c.why = why;
                    }
                }
            };
            if (e.action == "color" && !e.role.empty()) {
                put(e.role, e.value, COMPOSER_TEXT("chosen by a person"));
            } else if (e.action == "accent") {
                put("accent", e.value, COMPOSER_TEXT("chosen by a person"));
            } else if (e.action == "lock-palette") {
                size_t at = 0;
                while (at < e.value.size()) {
                    const size_t end = std::min(e.value.find(';', at), e.value.size());
                    const std::string item = e.value.substr(at, end - at);
                    at = end + 1;
                    const size_t eq = item.find('=');
                    if (eq != std::string::npos) {
                        put(item.substr(0, eq), item.substr(eq + 1), COMPOSER_TEXT("kept as a person fixed it"));
                    }
                }
            }
        }
    }

    // What a scene is in the song's colour story.
    std::string EvolutionOf(const SceneSource& s, size_t index, int occurrence, bool& climax) const {
        climax = false;
        const PlanSegment* seg = s.segment;
        if ((seg != nullptr && HasFlag(seg->flags, "silence")) || s.mode == "rest") {
            return "rest";
        }
        if (seg != nullptr && (HasFlag(seg->flags, "climax") || seg->priority >= 3)) {
            climax = true;
        }
        if (s.visual != nullptr && s.visual->arcStep == "peak" && seg != nullptr && seg->energy >= 0.75) {
            climax = true;
        }
        const std::string& k = s.sectionKind;
        if (occurrence > 0) {
            return climax ? "climax" : "return";
        }
        if (k == "intro" || (index == 0 && k != "chorus")) {
            return "establish";
        }
        if (k == "outro" || (seg != nullptr && HasFlag(seg->flags, "ending"))) {
            return "resolve";
        }
        if (k == "bridge" || k == "interlude" || k == "instrumental" || k == "solo") {
            return "shift";
        }
        if (climax) {
            return "climax";
        }
        if (k == "chorus" || k == "pre-chorus") {
            return "expand";
        }
        return "develop";
    }

    // How much a moment matters in the song's colour story, 0-1.
    static double ImportanceOf(const std::string& evolution) {
        if (evolution == "rest") return 0;
        if (evolution == "establish" || evolution == "resolve") return 0.25;
        if (evolution == "develop" || evolution == "hold" || evolution == "return") return 0.45;
        if (evolution == "expand" || evolution == "shift") return 0.7;
        if (evolution == "climax") return 1.0;
        return 0.45;
    }

    // Two colours of different hue groups: far enough apart round the circle to be read
    // as two colours, not as one in two lights.
    static bool OtherHue(const PaletteColor* a, const PaletteColor* b) {
        return a != nullptr && b != nullptr && a->C >= NEUTRAL_CHROMA && b->C >= NEUTRAL_CHROMA && HueDistance(a->h, b->h) >= 40;
    }

    // The grammar of a scene: which colours of its palette it really uses. Always the main
    // colour with its light and deep versions and the neutral; a second hue only when the
    // budget allows two; the accent's hue as a lasting colour only with three.
    static std::vector<std::string> Grammar(const ColorPaletteProfile& p, int budget) {
        std::vector<std::string> used = { "anchor", "highlight", "shadow", "neutral" };
        const PaletteColor* a = p.Get("anchor");
        const auto sameHue = [&](const char* role) { return !OtherHue(a, p.Get(role)); };
        for (const char* role : { "secondary", "transition", "contrast", "accent" }) {
            if (p.Get(role) != nullptr && sameHue(role)) {
                used.push_back(role); // the same hue in another light: no new colour
            }
        }
        if (budget >= 2) {
            // One other hue: the secondary when it is one, else the contrast.
            const char* second = OtherHue(a, p.Get("secondary")) ? "secondary" : OtherHue(a, p.Get("contrast")) ? "contrast" : nullptr;
            if (second != nullptr && std::find(used.begin(), used.end(), second) == used.end()) {
                used.push_back(second);
            }
        }
        if (budget >= 3) {
            for (const char* role : { "accent", "contrast", "secondary" }) {
                if (p.Get(role) != nullptr && std::find(used.begin(), used.end(), role) == used.end()) {
                    used.push_back(role);
                    break;
                }
            }
        }
        return used;
    }

    static bool Uses(const ChromaticScenePlan& sp, const std::string& role) {
        return std::find(sp.used.begin(), sp.used.end(), role) == sp.used.end() ? false : true;
    }

    // The other hue the scene uses, if any: the role name.
    static std::string SecondHue(const ChromaticScenePlan& sp) {
        const PaletteColor* a = sp.palette.Get("anchor");
        for (const char* role : { "secondary", "contrast", "accent" }) {
            if (Uses(sp, role) && OtherHue(a, sp.palette.Get(role))) {
                return role;
            }
        }
        return "";
    }

    ColorPaletteProfile MotifPalette(const std::string& id) const {
        const auto it = _motifPalettes.find(id);
        return it == _motifPalettes.end() ? ColorPaletteProfile() : it->second;
    }

    ChromaticMotif& NewMotif(ChromaticScore& score, const std::string& kind, const ColorPaletteProfile& p, const std::string& from, const std::string& sceneId, const std::string& why) {
        ChromaticMotif m;
        m.id = std::string(1, static_cast<char>('A' + std::min<size_t>(25, score.motifDefs.size())));
        m.kind = kind;
        m.anchor = p.Hex("anchor");
        const PaletteColor* a = p.Get("anchor");
        m.second = OtherHue(a, p.Get("secondary")) ? p.Hex("secondary") : OtherHue(a, p.Get("contrast")) ? p.Hex("contrast") : std::string();
        m.accent = p.Hex("accent");
        m.from = from;
        m.firstScene = sceneId;
        m.why = why;
        _motifPalettes[m.id] = p;
        score.motifDefs.push_back(m);
        return score.motifDefs.back();
    }

    ChromaticMotif* FindMotif(ChromaticScore& score, const std::string& id) {
        for (ChromaticMotif& m : score.motifDefs) {
            if (m.id == id) {
                return &m;
            }
        }
        return nullptr;
    }

    // What the interpretation says should keep its identity or may change here.
    bool BriefSays(const std::vector<std::string>& parts, const std::string& sectionKind) const {
        std::string kind = sectionKind == "interlude" || sectionKind == "solo" ? "instrumental" : sectionKind;
        return std::find(parts.begin(), parts.end(), kind) != parts.end();
    }

    ChromaticScenePlan Scene(ChromaticScore& score, size_t index) {
        const SceneSource& s = _sources[index];
        ChromaticScenePlan sp;
        sp.id = s.id;
        sp.startMS = s.startMS;
        sp.endMS = s.endMS;
        sp.sectionKind = s.sectionKind;
        sp.mode = s.mode;
        const PlanSegment* seg = s.segment;
        sp.energy = Round3(seg == nullptr ? 0.5 : seg->targetIntensity > 0 ? seg->targetIntensity : seg->energy);
        const int group = seg != nullptr ? seg->repeatGroup : -1;
        const int occurrence = group >= 0 ? _seen[group]++ : 0;
        bool climax = false;
        sp.evolution = EvolutionOf(s, index, occurrence, climax);
        if (_o.evolution < 0.15 && sp.evolution != "rest") {
            sp.evolution = index == 0 ? "establish" : "hold";
        }
        sp.reasons.push_back(fmt::format("{}: {}", sp.evolution, EvolutionPhrase(sp.evolution)));

        // ---- the budget: how many hues may live together in this moment.
        // A returning part matters as what it is: a chorus that comes back is still a chorus.
        double importance = ImportanceOf(sp.evolution);
        if (sp.evolution == "return" && (s.sectionKind == "chorus" || s.sectionKind == "pre-chorus")) {
            importance = ImportanceOf("expand");
        }
        const double style = std::clamp(_o.style, 0.0, 1.0);
        int budget = 1 + static_cast<int>(std::floor(importance * (0.8 + 2.0 * style) + 0.1));
        if (_players[index].size() < 4) {
            budget = std::min(budget, 2); // few figures: a third hue would only scatter them
        }
        if (SceneEdit("fewer-colours", s.startMS) != nullptr || SceneEdit("fewer-colours", -1) != nullptr) {
            budget = std::max(1, budget - 1);
            sp.manual = true;
        }
        if (SceneEdit("monochrome", s.startMS) != nullptr) {
            budget = 1;
            sp.manual = true;
        }
        sp.hueBudget = std::clamp(budget, 1, 3);
        if (_o.art >= 1 && !score.scenes.empty() && (SceneEdit("no-new-colours", s.startMS) != nullptr || SceneEdit("no-new-colours", -1) != nullptr)) {
            sp.hueBudget = std::min(sp.hueBudget, std::max(1, score.scenes.back().hueBudget));
            sp.manual = true;
        }
        double accents = std::clamp(0.1 + 0.6 * _o.accents * importance * (0.5 + style), 0.0, 0.6);
        if (SceneEdit("fewer-accents", s.startMS) != nullptr || SceneEdit("fewer-accents", -1) != nullptr) {
            accents *= 0.4;
            sp.manual = true;
        }
        sp.accentShare = Round3(accents);
        sp.reasons.push_back(fmt::format("{} hue{} at once ({:.0f}% of the colour budget asked for by a {} moment; style {:.0f}%)", sp.hueBudget, sp.hueBudget == 1 ? "" : "s",
                                         importance * 100, sp.evolution, style * 100));

        // ---- the motif: what this scene does with the song's colour ideas.
        double wanted = 0.4;
        if (sp.evolution == "establish" || sp.evolution == "resolve") {
            wanted = 0.2;
        } else if (sp.evolution == "rest") {
            wanted = 0.05;
        } else if (sp.evolution == "expand" || sp.evolution == "shift") {
            wanted = 0.65;
        } else if (sp.evolution == "climax") {
            wanted = 0.9;
        }
        wanted = std::clamp(wanted + (_o.contrast - 0.5) * 0.6, 0.0, 1.0);
        if (SceneEdit("more-contrast", s.startMS) != nullptr || SceneEdit("more-contrast", -1) != nullptr) {
            wanted = std::min(1.0, wanted + 0.3);
            sp.manual = true;
        }
        if (SceneEdit("less-contrast", s.startMS) != nullptr || SceneEdit("less-contrast", -1) != nullptr) {
            wanted = std::max(0.0, wanted - 0.3);
            sp.manual = true;
        }
        std::string feeling;
        std::vector<std::pair<double, double>> partHues;
        if (const BriefPart* part = BriefPartFor(s.sectionKind); part != nullptr) {
            feeling = part->feeling;
            sp.feeling = part->feeling;
            sp.feelingColor = part->color;
            sp.feelingSource = _in.brief->source + (_in.brief->origin.empty() ? std::string() : ":" + _in.brief->origin);
            if (part->intensity >= 0) {
                wanted = std::clamp(wanted + (part->intensity - 0.5) * 0.4, 0.0, 1.0);
            }
            if (!part->color.empty()) {
                const Oklch k = OklchOfHex(part->color);
                if (k.C >= NEUTRAL_CHROMA) {
                    partHues.push_back({ k.h, 0.7 });
                }
            }
            sp.reasons.push_back(fmt::format("the interpretation ({}): this part feels {}{}{}", _in.brief->source, feeling.empty() ? std::string("-") : feeling,
                                             part->intensity >= 0 ? fmt::format(", intensity {:.1f}", part->intensity) : std::string(),
                                             part->color.empty() ? std::string() : ", suggests " + part->color));
        }
        const bool keepHere = _in.brief != nullptr && BriefSays(_in.brief->keep, s.sectionKind);
        const bool changeHere = _in.brief != nullptr && BriefSays(_in.brief->change, s.sectionKind);
        // Phase 20: keeping the identity keeps the main colour, not the whole scene: a part
        // may still grow stronger or bring a contrast next to it. Only a person freezes more:
        // the main colour ("keep-main-colour"), no new families ("no-new-colours") or the palette (a lock).
        const bool refined = _o.art >= 1;
        const bool keepMain = SceneEdit("keep-main-colour", s.startMS) != nullptr || SceneEdit("keep-main-colour", -1) != nullptr;
        const bool noNew = SceneEdit("no-new-colours", s.startMS) != nullptr || SceneEdit("no-new-colours", -1) != nullptr;
        const bool blocksGrowth = refined ? noNew : keepHere;
        const double sat = std::clamp((0.72 + 0.26 * sp.energy + (sp.evolution == "climax" ? 0.06 : 0.0) - (sp.evolution == "rest" ? 0.25 : 0.0)) * _intensity, 0.3, 1.0);
        const double light = std::clamp(0.62 + 0.08 * sp.energy, 0.5, 0.75);
        const auto build = [&](double hue, double contrast) {
            Harmony h = ChooseHarmony(sp, contrast, index, feeling);
            if (SceneEdit("monochrome", s.startMS) != nullptr) {
                h = Harmony::Monochromatic;
            } else if (h == Harmony::Monochromatic && SceneEdit("no-monochrome", s.startMS) != nullptr) {
                h = Harmony::Analogous;
                sp.manual = true;
            }
            PaletteRequest r = RequestFor(hue, h, sat, light, contrast);
            r.suggested.insert(r.suggested.end(), partHues.begin(), partHues.end());
            r.id = sp.id;
            sp.reasons.push_back(_harmonyWhy);
            return BuildPalette(r);
        };
        const ChromaticEdit* recall = SceneEdit("recall-motif", s.startMS);
        const ChromaticScenePlan* previous = score.scenes.empty() ? nullptr : &score.scenes.back();
        std::string motif;
        std::string op;
        if (sp.evolution == "rest") {
            op = "rest";
            sp.palette = build(_anchor, wanted);
        } else if (recall != nullptr && _motifPalettes.contains(recall->value)) {
            op = "recall";
            motif = recall->value;
            sp.palette = MotifPalette(motif);
            sp.manual = true;
            sp.reasons.push_back(COMPOSER_TEXT("a person asked to bring this motif back"));
        } else if (SceneEdit("keep-identity", s.startMS) != nullptr && previous != nullptr && previous->evolution != "rest") {
            op = "hold";
            motif = previous->motif;
            sp.palette = previous->palette;
            sp.manual = true;
            sp.reasons.push_back(COMPOSER_TEXT("a person asked to keep the colours of the scene before"));
        } else if (group >= 0 && occurrence > 0 && _groupMotif.contains(group)) {
            motif = _groupMotif.at(group);
            sp.palette = MotifPalette(motif);
            const bool stronger = sp.energy > _groupEnergy[group] + 0.05 || sp.evolution == "climax" || occurrence + 1 == _groupCount[group];
            op = stronger && !blocksGrowth ? "intensify" : "recall";
            if (op == "intensify") {
                PaletteRequest r = RequestFor(sp.palette.anchorHue, sp.palette.harmony, std::min(1.0, sp.palette.saturation + 0.08), sp.palette.lightness, std::min(1.0, wanted + 0.15));
                r.id = sp.id;
                sp.palette = BuildPalette(r);
                sp.hueBudget = std::min(3, sp.hueBudget + (style >= 0.3 ? 1 : 0));
                sp.reasons.push_back(COMPOSER_TEXT("the same motif as before, more intense: the music comes back stronger"));
            } else {
                sp.reasons.push_back(previous != nullptr && (previous->evolution == "shift" || previous->evolution == "rest") ? COMPOSER_TEXT("the motif comes back after a turn of the music")
                                                                                                                         : COMPOSER_TEXT("the same music, the same colour idea"));
            }
        } else if (_identity.empty()) {
            op = "present";
            sp.palette = build(score.song.anchorHue, std::min(wanted, 0.35));
            motif = NewMotif(score, "identity", sp.palette, "", sp.id, COMPOSER_TEXT("the song's colour identity, presented")).id;
            _identity = motif;
        } else if (sp.evolution == "resolve") {
            op = "resolve";
            motif = _identity;
            sp.palette = MotifPalette(motif);
            sp.palette.saturation = Round3(std::max(0.3, sp.palette.saturation - 0.1));
            sp.reasons.push_back(COMPOSER_TEXT("back to the identity the song started with"));
        } else if (sp.evolution == "shift" && (!keepHere || changeHere) && !keepMain && !noNew) {
            op = "transform";
            const ColorPaletteProfile base = MotifPalette(_current.empty() ? _identity : _current);
            // A turn of the music turns the identity: towards its second colour, or round
            // the circle by a step the style allows.
            const PaletteColor* second = base.Get("secondary");
            const double turn = second != nullptr && OtherHue(base.Get("anchor"), second) ? HueDistance(base.anchorHue, second->h) * 0.6 : 35.0 + 25.0 * style;
            const double hue = WrapHue(base.anchorHue + _direction * std::min(turn, 70.0));
            sp.palette = build(hue, wanted);
            motif = NewMotif(score, "transformed", sp.palette, _identity, sp.id, COMPOSER_TEXT("the identity transformed: the music turns somewhere else")).id;
        } else if ((sp.evolution == "expand" || sp.evolution == "climax") && !blocksGrowth) {
            op = "introduce";
            sp.palette = build(MotifPalette(_identity).anchorHue, wanted);
            motif = NewMotif(score, "contrast", sp.palette, _identity, sp.id, COMPOSER_TEXT("the identity with a contrast: the music opens")).id;
            if (refined && keepHere) {
                sp.reasons.push_back(COMPOSER_TEXT("the part keeps the song's identity: its main colour stays and a contrast joins it"));
            }
        } else {
            op = "develop";
            motif = _current.empty() ? _identity : _current;
            if (FindMotif(score, motif) != nullptr && FindMotif(score, motif)->kind == "contrast" && sp.evolution == "develop") {
                motif = _identity; // a verse after a chorus goes back to the identity
            }
            sp.palette = MotifPalette(motif);
            if (style >= 0.75 && sp.evolution == "develop") {
                // Only an expressive style lets the hue move between verses.
                const int same = _kindCount[s.sectionKind]++;
                if (same > 0) {
                    PaletteRequest r = RequestFor(WrapHue(sp.palette.anchorHue + (same % 2 == 1 ? 1 : -1) * 10.0), sp.palette.harmony, sp.palette.saturation, sp.palette.lightness, wanted);
                    r.id = sp.id;
                    sp.palette = BuildPalette(r);
                    sp.reasons.push_back(COMPOSER_TEXT("an expressive style: the hue moves a little from the verse before"));
                }
            }
            sp.reasons.push_back(keepHere ? COMPOSER_TEXT("the interpretation asks this part to keep the song's identity") : COMPOSER_TEXT("the same colour idea, developed in light and roles"));
        }
        if (group >= 0 && occurrence == 0 && !motif.empty()) {
            _groupMotif[group] = motif;
            _groupEnergy[group] = sp.energy;
        }
        if (!motif.empty()) {
            _current = motif;
            if (ChromaticMotif* m = FindMotif(score, motif)) {
                m->scenes.push_back(sp.id);
            }
        }
        sp.motif = motif;
        sp.motifOperation = op;
        if (SceneEdit("monochrome", s.startMS) != nullptr && sp.palette.harmony != Harmony::Monochromatic) {
            // One family asked: the same main colour in its lights, whatever the motif was.
            PaletteRequest r = RequestFor(sp.palette.anchorHue, Harmony::Monochromatic, sp.palette.saturation, sp.palette.lightness, wanted);
            r.id = sp.id;
            sp.palette = BuildPalette(r);
            sp.reasons.push_back(COMPOSER_TEXT("a person asked for one family here"));
        }
        if (_o.art >= 1 && !_identity.empty() && (SceneEdit("keep-main-colour", s.startMS) != nullptr || SceneEdit("keep-main-colour", -1) != nullptr)) {
            const ColorPaletteProfile identity = MotifPalette(_identity);
            if (HueDistance(identity.anchorHue, sp.palette.anchorHue) > 12) {
                PaletteRequest r = RequestFor(identity.anchorHue, sp.palette.harmony, sp.palette.saturation, sp.palette.lightness, wanted);
                r.id = sp.id;
                sp.palette = BuildPalette(r);
            }
            sp.manual = true;
            sp.reasons.push_back(COMPOSER_TEXT("a person asked to keep the song's main colour here"));
        }
        ApplyPaletteEdits(sp.palette, s.startMS);
        if (SceneEdit("lock-palette", s.startMS) != nullptr) {
            sp.locked = true;
            sp.manual = true;
        } else if (SceneEdit("color", s.startMS) != nullptr || SceneEdit("accent", s.startMS) != nullptr) {
            sp.manual = true;
        }
        _previous.push_back(sp.palette.harmony);
        sp.used = Grammar(sp.palette, sp.hueBudget);
        sp.monochrome = sp.palette.harmony == Harmony::Monochromatic || (sp.hueBudget == 1 && SecondHue(sp).empty());
        if (sp.evolution == "rest") {
            sp.monochrome = true;
        }
        if (sp.monochrome) {
            sp.reasons.push_back(COMPOSER_TEXT("one family on purpose: the light changes, the colour holds"));
        }
        sp.saturation = Round3(sp.palette.saturation);
        sp.intensity = Round3(std::clamp(0.45 + 0.55 * sp.energy, 0.3, 1.0));
        sp.dominant = sp.palette.Hex("anchor");
        const std::string second = SecondHue(sp);
        sp.secondary = second.empty() ? std::vector<std::string> { sp.palette.Hex("highlight") } : std::vector<std::string> { sp.palette.Hex(second) };
        sp.accents = { Uses(sp, "accent") ? sp.palette.Hex("accent") : sp.palette.Hex("highlight") };
        sp.contrasts = { Uses(sp, "contrast") ? sp.palette.Hex("contrast") : sp.palette.Hex("shadow") };
        // What it keeps from the scene before and what it brings.
        const auto colours = [](const ChromaticScenePlan& p) {
            std::vector<std::string> out;
            for (const std::string& role : p.used) {
                const PaletteColor* c = p.palette.Get(role);
                if (c != nullptr && c->C >= NEUTRAL_CHROMA && role != "shadow" && role != "highlight") {
                    out.push_back(c->hex);
                }
            }
            return out;
        };
        const std::vector<std::string> mine = colours(sp);
        const std::vector<std::string> theirs = previous == nullptr ? std::vector<std::string>() : colours(*previous);
        for (const std::string& c : mine) {
            bool near = false;
            for (const std::string& t : theirs) {
                near |= DeltaE(c, t) < 8;
            }
            (near ? sp.kept : sp.introduced).push_back(c);
        }
        // What the scene wants to say, from its place in the song and how the stage is used.
        if (sp.evolution == "rest") {
            sp.intent = "rest";
        } else if (sp.mode == "call-response") {
            sp.intent = "answer";
        } else if (op == "present") {
            sp.intent = "open";
        } else if (op == "resolve") {
            sp.intent = "close";
        } else if (op == "transform") {
            sp.intent = "turn";
        } else if (sp.evolution == "climax" || op == "intensify") {
            sp.intent = "celebrate";
        } else if (op == "introduce" || sp.evolution == "expand") {
            sp.intent = "lift";
        } else {
            sp.intent = "tell";
        }
        return sp;
    }

    const BriefPart* BriefPartFor(const std::string& sectionKind) const {
        if (_in.brief == nullptr || _in.brief->arc.empty()) {
            return nullptr;
        }
        std::string kind = sectionKind;
        if (kind == "interlude" || kind == "solo" || kind == "instrumental") {
            kind = "instrumental";
        }
        return _in.brief->Part(kind);
    }

    static std::string MoodOfCharacter(const std::string& c) {
        if (c == "intimate") return "tender";
        if (c == "dramatic" || c == "majestic") return "epic";
        if (c == "mysterious") return "dark";
        return c;
    }

    PaletteRequest RequestFor(double hue, Harmony h, double sat, double light, double contrast) const {
        PaletteRequest r;
        r.anchorHue = hue;
        r.harmony = h;
        r.saturation = sat;
        r.lightness = light;
        r.contrast = contrast;
        r.direction = _direction;
        r.suggested = _suggested;
        return r;
    }

    Harmony ChooseHarmony(const ChromaticScenePlan& sp, double wanted, size_t index, const std::string& feeling) {
        double best = -1e9;
        Harmony chosen = _songHarmony;
        for (Harmony h : AllHarmonies()) {
            // What the moment asks for weighs most; the mood (this part's, when the
            // interpretation gives one) and the song's family after it.
            double v = 0.5 * MoodFit(feeling.empty() ? _mood : feeling, h) + 1.6 * (1.0 - std::fabs(HarmonyContrast(h) - wanted));
            if (h == _songHarmony) {
                v += 0.15; // the song's own family
            }
            // Variety: a harmony the last two scenes used costs, as much as variety asks.
            int recent = 0;
            for (size_t k = _previous.size() >= 2 ? _previous.size() - 2 : 0; k < _previous.size(); ++k) {
                recent += _previous[k] == h ? 1 : 0;
            }
            v -= 0.18 * recent * _o.variety;
            if (!_previous.empty() && _previous.back() == h) {
                v += 0.12 * (1.0 - _o.variety); // continuity when little variety is asked
            }
            // One colour only where the music rests or opens and closes quietly.
            if (h == Harmony::Monochromatic && wanted > 0.3) {
                v -= 1;
            }
            // The budget decides how far the harmony may reach: one hue keeps to
            // neighbours, two leave out the harmonies of three or four hues.
            if (sp.hueBudget <= 1 && h != Harmony::Monochromatic && h != Harmony::Analogous) {
                v -= 3;
            } else if (sp.hueBudget == 2 && (h == Harmony::Triadic || h == Harmony::Tetradic)) {
                v -= 3;
            }
            if (v > best + 1e-9) {
                best = v;
                chosen = h;
            }
        }
        (void)index;
        _harmonyWhy = fmt::format("{}: {} contrast wanted ({:.0f}%), {} hue{} allowed", HarmonyPhrase(chosen), sp.evolution, wanted * 100, sp.hueBudget, sp.hueBudget == 1 ? "" : "s");
        return chosen;
    }

    static std::string EvolutionPhrase(const std::string& e) {
        if (e == "establish") return COMPOSER_TEXT("the colours are presented, contained");
        if (e == "develop") return COMPOSER_TEXT("the family develops");
        if (e == "expand") return COMPOSER_TEXT("more contrast and saturation");
        if (e == "climax") return COMPOSER_TEXT("the widest palette of the song");
        if (e == "shift") return COMPOSER_TEXT("another temperature or family");
        if (e == "resolve") return COMPOSER_TEXT("back to the anchor, resolved");
        if (e == "return") return COMPOSER_TEXT("a part that comes back");
        if (e == "hold") return COMPOSER_TEXT("the palette holds");
        if (e == "rest") return COMPOSER_TEXT("quiet");
        return e;
    }

    // ------------------------------------------------------------ roles
    // The colours a role gets: one hue each, in its lights (a figure never mixes two
    // hue groups in one effect), always within what the scene's grammar uses.
    std::vector<std::string> RoleColors(const ChromaticScenePlan& sp, const std::string& role, const FigureFacts* f) const {
        const ColorPaletteProfile& p = sp.palette;
        if (role == "identity" && f != nullptr) {
            std::vector<std::string> out = f->identity;
            out.push_back(p.Hex("neutral"));
            return out;
        }
        const std::string second = SecondHue(sp);
        std::vector<std::string> roles;
        // A neighbour of the main colour (same hue group, still a colour of its own): what
        // accompanies or decorates in a scene of one hue, so it is rich without scattering.
        const auto neighbour = [&](std::initializer_list<const char*> order) -> std::string {
            const PaletteColor* a = p.Get("anchor");
            for (const char* r : order) {
                const PaletteColor* c = p.Get(r);
                if (c != nullptr && Uses(sp, r) && c->C >= NEUTRAL_CHROMA && !OtherHue(a, c) && DeltaE(c->hex, a->hex) >= 10) {
                    return r;
                }
            }
            return "";
        };
        const double style = std::clamp(_o.style, 0.0, 1.0);
        if (role == "main") {
            roles = { "anchor", "highlight", "shadow" };
        } else if (role == "complement") {
            const std::string n = neighbour({ "secondary", "transition", "accent" });
            roles = !second.empty() ? std::vector<std::string> { second, "neutral" } : !n.empty() ? std::vector<std::string> { n, "anchor" } : std::vector<std::string> { "highlight", "anchor", "neutral" };
        } else if (role == "accent") {
            roles = Uses(sp, "accent") ? std::vector<std::string> { "accent", "highlight" } : !second.empty() ? std::vector<std::string> { second, "highlight" }
                                                                                                                : std::vector<std::string> { "highlight", "neutral" };
        } else if (role == "response") {
            roles = second.empty() ? std::vector<std::string> { "highlight", "neutral" } : std::vector<std::string> { second, "neutral" };
        } else if (role == "background") {
            roles = { "shadow", "anchor" };
        } else if (role == "neutral") {
            roles = { "neutral", "highlight" };
        } else if (role == "decorative") {
            // Small figures decorate: in white when the style is contained, else in the other
            // neighbour of the main colour, or its light tone.
            const std::string n = neighbour({ "accent", "transition", "secondary" });
            roles = style < 0.25 || n.empty() ? std::vector<std::string> { "neutral", "highlight" } : std::vector<std::string> { n, "neutral" };
        }
        std::vector<std::string> out;
        for (const std::string& r : roles) {
            const std::string hex = p.Hex(r);
            if (!hex.empty() && (Uses(sp, r) || r == "neutral" || r == "highlight" || r == "shadow") && std::find(out.begin(), out.end(), hex) == out.end()) {
                out.push_back(hex);
            }
        }
        if (out.empty()) {
            out.push_back(p.Hex("anchor"));
        }
        return out;
    }

    void Roles(const ChromaticScore& score, ChromaticScenePlan& sp, const SceneSource& s) {
        (void)score;
        const size_t index = static_cast<size_t>(&s - _sources.data());
        std::vector<std::string> players(_players[index].begin(), _players[index].end());
        std::map<std::string, int> familyNodes;
        for (const std::string& p : players) {
            const auto f = _facts.find(p);
            if (f != _facts.end() && !f->second.group) {
                familyNodes[f->second.family] += std::max(1, f->second.nodes);
            }
        }
        std::vector<std::pair<std::string, int>> families(familyNodes.begin(), familyNodes.end());
        std::stable_sort(families.begin(), families.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        const double style = std::clamp(_o.style, 0.0, 1.0);
        const bool quiet = sp.evolution == "rest" || sp.monochrome;
        // A hierarchy by light: the largest families carry the main colour, the next ones
        // accompany it (in the second hue when the scene has one, else in light tones), the
        // small ones decorate in white and tints. A lasting accent colour on small figures
        // only in an expressive style with room for three hues: otherwise an accent is a
        // moment, not a figure.
        const bool lastingAccents = style >= 0.7 && sp.hueBudget >= 3 && _o.accents >= 0.25 && !quiet;
        std::map<std::string, std::string> familyRole;
        double total = 0;
        for (const auto& [family, nodes] : families) {
            total += nodes;
        }
        // The style moves the line too: an expressive scene gives more of the house to the
        // second colour, a contained one keeps more of it in the main one.
        const double mainLine = 0.7 - 0.25 * style - 0.1 * _o.variety;
        const double secondLine = 0.92 - 0.08 * _o.variety - 0.04 * style;
        double before = 0;
        int small = 0;
        for (const auto& [family, nodes] : families) {
            const double share = total > 0 ? before / total : 0;
            std::string role;
            if (share < mainLine || familyRole.empty()) {
                role = sp.evolution == "shift" && !SecondHue(sp).empty() ? "complement" : "main";
            } else if (share < secondLine) {
                role = sp.evolution == "shift" && !SecondHue(sp).empty() ? "main" : "complement";
            } else {
                role = lastingAccents && (small++ % 2 == 1) ? "accent" : "decorative";
            }
            familyRole[family] = role;
            before += nodes;
        }
        // Many small figures of one family alternate only in an expressive style.
        std::map<std::string, std::string> alternate;
        if (!quiet && sp.energy >= 0.7 && style >= 0.7 && _o.variety >= 0.5) {
            std::map<std::string, std::vector<std::string>> members;
            for (const std::string& p : players) {
                const auto f = _facts.find(p);
                if (f != _facts.end() && !f->second.group) {
                    members[f->second.family].push_back(p);
                }
            }
            for (auto& [family, list] : members) {
                if (list.size() < 4 || family == "matrix" || family == "mega-tree" || family == "faces") {
                    continue;
                }
                std::stable_sort(list.begin(), list.end(), [&](const std::string& a, const std::string& b) { return _facts[a].cx < _facts[b].cx; });
                for (size_t i = 1; i < list.size(); i += 2) {
                    alternate[list[i]] = familyRole[family] == "decorative" ? "complement" : "decorative";
                }
            }
        }
        const StageScene* stage = s.stage;
        // When one figure or two carry the scene, the rest stay discreet instead of competing.
        const bool focused = sp.mode == "protagonist" || sp.mode == "duo" || sp.mode == "call-response";
        for (const std::string& p : players) {
            const auto it = _facts.find(p);
            const FigureFacts* f = it == _facts.end() ? nullptr : &it->second;
            FigureColorRole r;
            r.figure = p;
            const ChromaticEdit* manual = nullptr;
            if (_in.edits != nullptr) {
                for (const ChromaticEdit& e : _in.edits->edits) {
                    if (e.action == "figure-color" && e.figure == p && (e.sceneStartMS < 0 || std::abs(e.sceneStartMS - s.startMS) <= 1500)) {
                        manual = &e;
                    }
                }
            }
            const std::string stageRole = stage != nullptr ? stage->RoleOf(p) : std::string();
            const bool identity = f != nullptr && !f->identity.empty() && !_unpinned.contains(p) && (f->character || _pinned.contains(p));
            if (manual != nullptr) {
                r.role = "manual";
                size_t at = 0;
                while (at < manual->value.size()) {
                    const size_t end = std::min(manual->value.find(',', at), manual->value.size());
                    Rgb rgb;
                    if (ParseHex(manual->value.substr(at, end - at), rgb)) {
                        r.colors.push_back(ToHex(rgb));
                    }
                    at = end + 1;
                }
                r.why = COMPOSER_TEXT("colours a person chose for this figure");
                if (r.colors.empty()) {
                    r.role.clear();
                }
            }
            if (r.role.empty() && identity) {
                r.role = "identity";
                r.why = _pinned.contains(p) ? COMPOSER_TEXT("its own colours: a person fixed them") : COMPOSER_TEXT("a character keeps its own colours");
            }
            if (r.role.empty() && !stageRole.empty() && stageRole != "background") {
                if (stageRole == "protagonist") {
                    // A selective accent: the protagonist stands out only where the scene
                    // is about it and has room for a second hue.
                    r.role = focused && sp.hueBudget >= 2 ? "accent" : "main";
                    r.why = r.role == "accent" ? COMPOSER_TEXT("the protagonist stands out in the scene's accent") : COMPOSER_TEXT("the protagonist carries the scene's colour");
                    sp.protagonist = sp.protagonist.empty() ? p : sp.protagonist;
                } else if (stageRole == "complement") {
                    r.role = "complement";
                    r.why = COMPOSER_TEXT("it accompanies the protagonist in the secondary");
                    sp.companions.push_back(p);
                } else if (stageRole == "responder") {
                    r.role = "response";
                    r.why = COMPOSER_TEXT("it answers in the contrast colour");
                    sp.companions.push_back(p);
                } else if (stageRole == "accent") {
                    r.role = "decorative";
                    r.why = COMPOSER_TEXT("an accent of the scene");
                } else if (stageRole == "rest" && focused) {
                    r.role = "background";
                    r.why = COMPOSER_TEXT("another figure carries the scene: this one stays discreet");
                    sp.discreet.push_back(p);
                }
            }
            if (r.role.empty() && f != nullptr && f->group) {
                if (f->flatFamilies.size() == 1 && familyRole.contains(f->flatFamilies.front())) {
                    r.role = familyRole[f->flatFamilies.front()];
                    r.why = COMPOSER_TEXT("a group of one family: its family's colour");
                } else {
                    r.role = sp.evolution == "climax" || sp.evolution == "expand" ? "main" : "background";
                    r.why = r.role == "main" ? COMPOSER_TEXT("a group of the whole house: the scene's colour") : COMPOSER_TEXT("a group of the whole house: the deep anchor under the figures");
                }
            }
            if (r.role.empty()) {
                const std::string family = f != nullptr ? f->family : std::string();
                if (alternate.contains(p)) {
                    r.role = alternate[p];
                    r.why = COMPOSER_TEXT("every other one of its family, for movement inside it");
                } else if (familyRole.contains(family)) {
                    r.role = familyRole[family];
                    r.why = COMPOSER_TEXT("the colour role of its family in this scene");
                } else {
                    r.role = "main";
                    r.why = COMPOSER_TEXT("the scene's colour");
                }
            }
            // What the maker taught about this figure: it stands out, or it rests.
            if (const auto w = _o.figureWeight.find(p); w != _o.figureWeight.end() && r.role != "identity" && r.role != "manual") {
                if (w->second >= 0.3 && (r.role == "decorative" || r.role == "background" || r.role == "complement")) {
                    r.role = focused && sp.hueBudget >= 2 ? "accent" : "main";
                    r.why = COMPOSER_TEXT("the maker asked for this figure to stand out");
                } else if (w->second <= -0.3 && r.role != "background") {
                    r.role = "background";
                    r.why = COMPOSER_TEXT("the maker asked for this figure to rest");
                    sp.discreet.push_back(p);
                }
            }
            if (r.colors.empty()) {
                r.colors = RoleColors(sp, r.role, f);
            }
            sp.figures.push_back(std::move(r));
        }
        std::sort(sp.figures.begin(), sp.figures.end(), [](const FigureColorRole& a, const FigureColorRole& b) { return a.figure < b.figure; });
        if (sp.protagonist.empty() && !families.empty()) {
            // The largest figure of the main family carries the scene.
            int most = -1;
            for (const FigureColorRole& r : sp.figures) {
                const auto f = _facts.find(r.figure);
                if (f != _facts.end() && !f->second.group && r.role == "main" && f->second.nodes > most) {
                    most = f->second.nodes;
                    sp.protagonist = r.figure;
                }
            }
        }
    }

    // ------------------------------------------------------------ gestures
    // A gesture's colours follow what it does in the scene: most reinforce the scene's own
    // hue in a lighter tone (standing out by light, not by a new colour); only strong moments
    // take the accent; a gesture near the end of a scene can prepare the next one's colour;
    // in a closing scene it resolves to the identity.
    void Gestures(const ChromaticScore& score, ChromaticScenePlan& sp, size_t index) {
        if (_in.stage == nullptr) {
            return;
        }
        const ChromaticScenePlan* next = index + 1 < score.scenes.size() ? &score.scenes[index + 1] : nullptr;
        const double style = std::clamp(_o.style, 0.0, 1.0);
        int accentsLeft = sp.evolution == "climax" ? 2 + static_cast<int>(style * 2) : sp.evolution == "expand" || sp.motifOperation == "intensify" ? 1 + static_cast<int>(style * 2)
                                                                                                                                                    : static_cast<int>(style >= 0.7);
        for (const GlobalGesture& g : _in.stage->gestures) {
            if (g.startMS < sp.startMS || g.startMS >= sp.endMS) {
                continue;
            }
            GestureChromaticProfile gp;
            gp.gesture = g.id;
            std::vector<std::string> identity;
            for (const GestureParticipant& p : g.participants) {
                if (const FigureColorRole* r = sp.RoleOf(p.figure); r != nullptr) {
                    for (size_t k = 0; k < r->colors.size() && k < 2; ++k) {
                        if (std::find(gp.background.begin(), gp.background.end(), r->colors[k]) == gp.background.end()) {
                            gp.background.push_back(r->colors[k]);
                        }
                    }
                    if (r->role == "identity") {
                        identity.insert(identity.end(), r->colors.begin(), r->colors.end());
                    }
                }
            }
            if (gp.background.empty()) {
                gp.background.push_back(sp.palette.Hex("anchor"));
            }
            // A gesture is seen against the house as a whole, not against every colour some
            // figure wears: the mix of each figure's first colour, weighted by its lights.
            Oklab house;
            double weight = 0;
            for (const FigureColorRole& r : sp.figures) {
                const auto f = _facts.find(r.figure);
                if (r.colors.empty() || f == _facts.end() || f->second.group || f->second.nodes <= 0) {
                    continue;
                }
                const Oklab c = ToOklab(OklchOfHex(r.colors.front()));
                const double w = static_cast<double>(f->second.nodes);
                house.L += c.L * w;
                house.a += c.a * w;
                house.b += c.b * w;
                weight += w;
            }
            if (weight > 0) {
                house = { house.L / weight, house.a / weight, house.b / weight };
            } else {
                house = ToOklab(OklchOfHex(sp.palette.Hex("anchor")));
            }
            const auto against = [&](const std::string& hex) {
                double d = DeltaE(ToOklab(OklchOfHex(hex)), house);
                for (const std::string& b : identity) {
                    d = std::min(d, DeltaE(hex, b) + 6);
                }
                return d;
            };
            // What the gesture does here.
            const bool strong = (sp.evolution == "climax" || sp.evolution == "expand" || sp.motifOperation == "intensify" || (_o.art >= 1 && g.presence == "climax")) && accentsLeft > 0 &&
                                sp.energy >= 0.6;
            const bool late = next != nullptr && g.startMS >= sp.startMS + 0.75 * (sp.endMS - sp.startMS) && next->motif != sp.motif && !next->motif.empty();
            std::vector<std::pair<std::string, std::string>> candidates; // hex, why
            if (sp.motifOperation == "resolve") {
                gp.intent = "resolve";
                candidates = { { sp.palette.Hex("highlight"), "highlight" }, { sp.palette.Hex("neutral"), "neutral" }, { sp.palette.Hex("anchor"), "anchor" } };
            } else if (late) {
                gp.intent = "transform";
                candidates = { { next->palette.Hex("anchor"), "next anchor" }, { next->palette.Hex("highlight"), "next highlight" }, { sp.palette.Hex("highlight"), "highlight" } };
            } else if (strong) {
                gp.intent = "accent";
                const std::string second = SecondHue(sp);
                candidates = { { sp.palette.Hex("accent"), "accent" }, { second.empty() ? std::string() : sp.palette.Hex(second), second }, { sp.palette.Hex("highlight"), "highlight" },
                               { sp.palette.Hex("neutral"), "neutral" } };
                --accentsLeft;
            } else {
                gp.intent = "reinforce";
                // The scene's own colour at full strength first: over tints and white it reads;
                // its light tone and white when the figures already wear the main colour.
                candidates = { { sp.palette.Hex("anchor"), "anchor" }, { sp.palette.Hex("highlight"), "highlight" } };
                // Then a colour the scene already uses (its second hue or a neighbour), and
                // white only last.
                const std::string second = SecondHue(sp);
                for (const char* role : { "secondary", "transition", "accent" }) {
                    if (Uses(sp, role) && (second.empty() || second == role) && sp.palette.Get(role) != nullptr && sp.palette.Get(role)->C >= NEUTRAL_CHROMA) {
                        candidates.push_back({ sp.palette.Hex(role), role });
                    }
                }
                if (!second.empty() && second != "secondary" && second != "transition" && second != "accent") {
                    candidates.push_back({ sp.palette.Hex(second), second });
                }
                candidates.push_back({ sp.palette.Hex("neutral"), "neutral" });
            }
            // The first candidate that still reads against what is lit; failing that, the one
            // that reads best. From phase 20 how much it must read depends on what the gesture
            // is: a background stays discreet, an answer is seen, the idea's own gesture stands
            // out, a peak's is what the house is about. Never white first: the scene's colours lead.
            double needed = 18;
            if (_o.art >= 1 && !g.presence.empty()) {
                needed = g.presence == "background" ? 8 : g.presence == "response" ? 18 : g.presence == "protagonist" ? 24 : 28;
                if (g.presence == "climax" || g.presence == "protagonist") {
                    // Colours of the scene that can carry more weight, before its lights and white.
                    std::vector<std::pair<std::string, std::string>> more;
                    for (const char* role : { "accent", "contrast", "secondary" }) {
                        const PaletteColor* c = sp.palette.Get(role);
                        if (c != nullptr && c->C >= NEUTRAL_CHROMA && (Uses(sp, role) || g.presence == "climax")) {
                            more.push_back({ c->hex, role });
                        }
                    }
                    auto at = candidates.end();
                    for (auto it = candidates.begin(); it != candidates.end(); ++it) {
                        if (it->second == "highlight" || it->second == "neutral") {
                            at = it;
                            break;
                        }
                    }
                    candidates.insert(at, more.begin(), more.end());
                }
                gp.presence = g.presence;
            }
            std::string front, frontWhy;
            double best = -1;
            for (const auto& [hex, why] : candidates) {
                if (hex.empty()) {
                    continue;
                }
                const double d = against(hex);
                if (d >= needed) {
                    front = hex;
                    frontWhy = why;
                    break;
                }
                if (d > best) {
                    best = d;
                    front = hex;
                    frontWhy = why;
                }
            }
            gp.front = front;
            gp.contrast = std::round(against(front) * 10.0) / 10.0;
            const Oklch f = OklchOfHex(front);
            // Its second colour stays in the scene's identity: the main colour, or the
            // next scene's when it prepares it.
            gp.secondary = gp.intent == "transform" ? next->palette.Hex("anchor") : sp.palette.Hex("anchor");
            if (gp.secondary == front) {
                gp.secondary = sp.palette.Hex("shadow");
            }
            gp.tail = ToHex(Oklch { std::max(0.3, f.L - 0.18), f.C * 0.8, f.h });
            gp.start = front;
            gp.arrival = gp.secondary;
            switch (g.kind) {
            case GestureKind::PropagatedWave:
            case GestureKind::SpatialSweep:
            case GestureKind::Expansion:
            case GestureKind::Contraction:
                gp.colorBy = gp.intent == "transform" || (_o.evolution >= 0.3 && style >= 0.5) ? "distance" : "trail";
                break;
            case GestureKind::Echo:
            case GestureKind::Cascade:
                gp.colorBy = "trail";
                break;
            default:
                gp.colorBy = "field";
                break;
            }
            gp.why = fmt::format("{}: {} ({}) stands {:.0f} ΔE from what is lit on its figures", gp.intent, front, frontWhy, gp.contrast);
            sp.gestures.push_back(std::move(gp));
        }
    }

    // ------------------------------------------------------------ transitions
    double BeatMS(int ms) const {
        double bpm = 0;
        for (const TempoPoint& t : _in.analysis->tempo) {
            if (t.timeMS <= ms && t.bpm > 30) {
                bpm = t.bpm;
            }
        }
        return bpm > 30 ? 60000.0 / bpm : 500.0;
    }

    bool EventNear(const char* kind, int ms, int within) const {
        if (_in.audio == nullptr) {
            return false;
        }
        for (const SoundEvent& e : _in.audio->events) {
            if (e.kind == kind && std::abs(e.startMS - ms) <= within) {
                return true;
            }
        }
        return false;
    }

    bool EventIn(const char* kind, int from, int to) const {
        if (_in.audio == nullptr) {
            return false;
        }
        for (const SoundEvent& e : _in.audio->events) {
            if (e.kind == kind && e.startMS < to && e.endMS > from) {
                return true;
            }
        }
        return false;
    }

    void Transition(ChromaticScore& score, size_t i) {
        ChromaticScenePlan& sp = score.scenes[i];
        sp.toNext.endMS = sp.endMS;
        sp.toNext.startMS = sp.endMS;
        if (i + 1 >= score.scenes.size()) {
            sp.toNext.kind = "hold";
            sp.toNext.why = COMPOSER_TEXT("the last scene");
            return;
        }
        const ChromaticScenePlan& next = score.scenes[i + 1];
        const double beat = BeatMS(sp.endMS);
        const double hueMove = HueDistance(sp.palette.anchorHue, next.palette.anchorHue);
        double beats = 2;
        const bool soft = _o.gradual >= 0.75; // the maker asked for softer changes: only a real break cuts
        if (EventNear("drop", next.startMS, 300) || EventNear("reentry", next.startMS, 300) || (!soft && (EventNear("accent", next.startMS, 120) || next.evolution == "climax"))) {
            sp.toNext.kind = "cut";
            sp.toNext.why = COMPOSER_TEXT("the music changes at once: so do the colours");
            beats = 0;
        } else if (next.evolution == "rest" || sp.evolution == "rest") {
            sp.toNext.kind = "fade";
            sp.toNext.why = COMPOSER_TEXT("into or out of a quiet part: a soft fade");
        } else if (!next.motif.empty() && next.motif == sp.motif) {
            sp.toNext.kind = "hold";
            sp.toNext.why = COMPOSER_TEXT("the same part goes on in its colours");
            beats = 0;
        } else if (!next.motif.empty() && next.evolution == "return") {
            sp.toNext.kind = "return";
            sp.toNext.why = COMPOSER_TEXT("back to a motif heard before");
        } else if ((next.evolution == "expand" || next.evolution == "climax") && (EventIn("crescendo", sp.endMS - 4000, sp.endMS) || sp.sectionKind == "pre-chorus")) {
            sp.toNext.kind = "introduce";
            sp.toNext.why = COMPOSER_TEXT("the music grows: the next colours come in before it arrives");
            beats = 4;
        } else if (hueMove >= 90) {
            sp.toNext.kind = "dominant-change";
            sp.toNext.why = fmt::format("the main colour moves {:.0f}°: figure by figure over a bar", hueMove);
            beats = 4;
        } else if (hueMove >= 8) {
            sp.toNext.kind = "hue-drift";
            sp.toNext.why = fmt::format("the hue slides {:.0f}°", hueMove);
        } else if (std::fabs(sp.palette.saturation - next.palette.saturation) >= 0.1) {
            sp.toNext.kind = "saturation";
            sp.toNext.why = COMPOSER_TEXT("the same hue, more or less saturated");
        } else if (sp.palette.harmony != next.palette.harmony) {
            sp.toNext.kind = "fade";
            sp.toNext.why = COMPOSER_TEXT("the same anchor in another family");
        } else {
            sp.toNext.kind = "hold";
            sp.toNext.why = COMPOSER_TEXT("the colours hold");
            beats = 0;
        }
        if (_o.evolution < 0.15 && sp.toNext.kind != "cut") {
            beats = std::min(beats, 2.0);
        }
        beats *= 0.5 + std::clamp(_o.gradual, 0.0, 1.0); // 0.5 the middle: as chosen
        const int length = static_cast<int>(std::min({ beats * beat, 0.4 * (sp.endMS - sp.startMS), 4000.0 }));
        sp.toNext.startMS = sp.endMS - std::max(0, length);
        // Figure by figure: from one side of the house to the other, alternating sides.
        std::vector<std::pair<double, std::string>> order;
        for (const FigureColorRole& r : sp.figures) {
            const auto f = _facts.find(r.figure);
            if (f != _facts.end() && !f->second.group) {
                order.push_back({ f->second.cx, r.figure });
            }
        }
        std::sort(order.begin(), order.end());
        if (i % 2 == 1) {
            std::reverse(order.begin(), order.end());
        }
        for (const auto& [x, name] : order) {
            sp.toNext.order.push_back(name);
        }
    }

    // ------------------------------------------------------------ checks
    void Check(ChromaticScore& score) {
        std::map<std::string, int> anchors;
        int mono = 0;
        for (const ChromaticScenePlan& s : score.scenes) {
            anchors[ColorFamilyId(ColorFamily(OklchOfHex(s.dominant)))]++;
            mono += s.monochrome ? 1 : 0;
        }
        if (score.scenes.size() >= 4 && anchors.size() <= 1) {
            score.warnings.push_back("one-main-colour: every scene leads with the same family; the figures' roles carry the variety");
        }
        if (score.scenes.size() >= 4 && mono * 2 > static_cast<int>(score.scenes.size())) {
            score.warnings.push_back("mostly-one-family: more than half the scenes are in one family on purpose");
        }
        if (_in.brief == nullptr || _in.brief->Empty()) {
            score.limits.push_back("no-interpretation: no artistic reading of the song; the colours start from its key and the person's choices");
        }
    }

    const ChromaticInput& _in;
    const ChromaticOptions _o;
    std::map<std::string, FigureFacts> _facts;
    std::set<std::string> _pinned;
    std::set<std::string> _unpinned;
    std::vector<SceneSource> _sources;
    std::map<size_t, std::set<std::string>> _players;
    std::vector<std::pair<double, double>> _suggested;
    std::string _mood;
    double _anchor = 30;
    Harmony _songHarmony = Harmony::Analogous;
    int _direction = 1;
    std::map<int, int> _seen;
    std::map<std::string, int> _kindCount;
    std::map<std::string, ColorPaletteProfile> _motifPalettes; // motif id -> its palette
    std::map<int, std::string> _groupMotif;  // repeat group -> the motif of its first time
    std::map<int, double> _groupEnergy;      // repeat group -> the energy of its first time
    std::map<int, int> _groupCount;          // repeat group -> how many times it plays
    std::string _identity;                   // the song's first motif
    std::string _current;                    // the motif of the last scene
    std::vector<Harmony> _previous;
    std::string _harmonyWhy;
    double _intensity = 1; // how strong the interpretation wants the colours
};

bool UsesRole(const ChromaticScenePlan& sp, const std::string& role) {
    return std::find(sp.used.begin(), sp.used.end(), role) != sp.used.end();
}

const std::set<std::string>& UntouchedEffects() {
    static const std::set<std::string> e = { "Off", "Video", "Pictures", "Faces", "Shader", "Duplicate", "DMX", "Servo", "Moving Head", "State" };
    return e;
}

} // namespace

ChromaticScore DirectColors(const ChromaticInput& input) {
    Director d(input);
    return d.Run();
}

std::string ColorCurveText(int slot, const std::string& from, const std::string& to, double holdUntil) {
    const double hold = std::clamp(holdUntil, 0.0, 0.98);
    const Oklch a = OklchOfHex(from);
    const Oklch b = OklchOfHex(to);
    Oklch m = Mix(a, b, 0.5);
    if (a.C >= NEUTRAL_CHROMA && b.C >= NEUTRAL_CHROMA && HueDistance(a.h, b.h) > 100) {
        m.C *= 0.35; // far apart: through a soft middle, not through a third colour
    }
    const std::string mid = ToHex(m);
    Rgb ra, rb;
    const std::string fa = ParseHex(from, ra) ? ToHex(ra) : from;
    const std::string fb = ParseHex(to, rb) ? ToHex(rb) : to;
    std::string values;
    if (hold > 0.001) {
        values = fmt::format("x=0.000^c={};x={:.3f}^c={};x={:.3f}^c={};x=1.000^c={}", fa, hold, fa, hold + (1.0 - hold) / 2, mid, fb);
    } else {
        values = fmt::format("x=0.000^c={};x=0.500^c={};x=1.000^c={}", fa, mid, fb);
    }
    return fmt::format("Active=TRUE|Id=ID_BUTTON_Palette{}|Values={}|", slot, values);
}

std::vector<std::string> ApplyChromaticScore(const ChromaticScore& score, Choreography& choreography) {
    std::vector<std::string> notes;
    if (!score.enabled || score.scenes.empty()) {
        return notes;
    }
    int recoloured = 0, curves = 0, kept = 0;
    std::map<std::string, int> roles;
    std::map<std::string, int> functions;
    std::map<size_t, int> downbeats;
    for (ChoreographyEvent& e : choreography.events) {
        if (e.colors.empty() || UntouchedEffects().contains(e.effect) || e.state == DecisionState::Locked || e.state == DecisionState::ManualEdit) {
            ++kept;
            continue;
        }
        bool byPerson = false;
        for (const Reason& r : e.reasons) {
            byPerson |= r.code == "palette-set-by-user";
        }
        if (byPerson) {
            ++kept;
            continue;
        }
        const ChromaticScenePlan* sp = score.Scene(e.segmentId);
        if (sp == nullptr) {
            sp = score.SceneAt((e.startMS + e.endMS) / 2);
        }
        const FigureColorRole* role = sp == nullptr ? nullptr : sp->RoleOf(e.target);
        if (role == nullptr || role->colors.empty()) {
            continue;
        }
        const size_t n = e.colors.size();
        std::vector<std::string> source = role->colors;
        std::string function = "base";
        if (role->role != "identity" && role->role != "manual") {
            if (e.prominence == Prominence::Accent) {
                // A hit: on a downbeat it may take the accent, in the share the music
                // earns; otherwise it flashes in the figure's own light tone.
                bool downbeat = false;
                for (const Reason& r : e.reasons) {
                    downbeat |= r.code == "downbeat";
                }
                const size_t at = static_cast<size_t>(sp - score.scenes.data());
                int& hits = downbeats[at];
                const bool earned = downbeat && (hits++ % 10) < static_cast<int>(std::lround(sp->accentShare * 10));
                const std::string accent = UsesRole(*sp, "accent") ? sp->palette.Hex("accent") : sp->palette.Hex("highlight");
                source = earned ? std::vector<std::string> { accent } : std::vector<std::string> { role->colors.front() };
                function = earned ? "accent" : "hit";
            } else if (e.prominence == Prominence::Support && role->colors.size() > 1) {
                std::rotate(source.begin(), source.begin() + 1, source.end()); // the same hue, in its other light
                function = "complement";
            } else if (e.prominence == Prominence::Rest) {
                source = { sp->palette.Hex("shadow") };
                function = "background";
            }
        }
        std::vector<std::string> colors;
        for (size_t k = 0; k < n; ++k) {
            colors.push_back(source[k % source.size()]);
        }
        e.colors = colors;
        functions[function]++;
        e.colorCurves.clear();
        e.paletteId = "chroma:" + sp->id + ":" + role->role;
        e.reasons.push_back({ "color-role", role->role });
        ++recoloured;
        roles[role->role]++;
        // The transition into the next scene, on this figure at its own moment.
        const ChromaticTransition& t = sp->toNext;
        if (t.kind == "cut" || t.kind == "hold" || t.kind.empty() || e.endMS <= t.startMS) {
            continue;
        }
        const size_t at = static_cast<size_t>(sp - score.scenes.data());
        if (at + 1 >= score.scenes.size()) {
            continue;
        }
        const ChromaticScenePlan& next = score.scenes[at + 1];
        const FigureColorRole* nextRole = next.RoleOf(e.target);
        if (nextRole == nullptr || nextRole->colors.empty()) {
            continue;
        }
        double start = t.startMS;
        const auto pos = std::find(t.order.begin(), t.order.end(), e.target);
        if (pos != t.order.end() && t.order.size() > 1) {
            const double share = static_cast<double>(pos - t.order.begin()) / static_cast<double>(t.order.size() - 1);
            start += share * 0.6 * (t.endMS - t.startMS); // the last figure starts with 40% of the time left
        }
        if (e.endMS - start < 50 || e.endMS <= e.startMS) {
            continue;
        }
        const double hold = std::clamp((start - e.startMS) / static_cast<double>(e.endMS - e.startMS), 0.0, 0.98);
        bool any = false;
        e.colorCurves.assign(n, std::string());
        for (size_t k = 0; k < n; ++k) {
            const std::string to = nextRole->colors[k % nextRole->colors.size()];
            if (to != e.colors[k]) {
                e.colorCurves[k] = ColorCurveText(static_cast<int>(k) + 1, e.colors[k], to, hold);
                any = true;
            }
        }
        if (any) {
            ++curves;
            e.reasons.push_back({ "color-transition", t.kind });
        } else {
            e.colorCurves.clear();
        }
    }
    std::string roleText;
    for (const auto& [r, n] : roles) {
        roleText += fmt::format("{}{} x{}", roleText.empty() ? "" : ", ", r, n);
    }
    std::string functionText;
    for (const auto& [f, n] : functions) {
        functionText += fmt::format("{}{} x{}", functionText.empty() ? "" : ", ", f, n);
    }
    notes.push_back(fmt::format("{} effects took their figure's colour role ({}; as {}), {} change colour into the next scene, {} kept as they were", recoloured, roleText, functionText, curves, kept));
    return notes;
}

std::vector<ChromaticEdit> DecisionsFromObservations(const std::vector<ArtisticObservation>& observations, const ChromaticScore& score) {
    std::vector<ChromaticEdit> out;
    for (const ArtisticObservation& o : observations) {
        if (!ObservationIsSongDecision(o)) {
            continue;
        }
        const ChromaticScenePlan* sp = o.sceneStartMS >= 0 ? score.SceneAt(o.sceneStartMS) : nullptr;
        if (o.kind == "keep-combination" && sp != nullptr) {
            std::string value;
            for (const PaletteColor& c : sp->palette.colors) {
                value += (value.empty() ? "" : ";") + c.role + "=" + c.hex;
            }
            out.push_back({ "lock-palette", sp->startMS, "", "", value, "from an observation: keep this combination" });
        } else if (o.kind == "recall-motif" && sp != nullptr) {
            const std::string motif = !o.note.empty() && o.note.size() <= 2 ? o.note : score.motifDefs.empty() ? std::string() : score.motifDefs.front().id;
            if (!motif.empty()) {
                out.push_back({ "recall-motif", sp->startMS, "", "", motif, "from an observation: bring this motif back here" });
            }
        }
    }
    return out;
}

int ColorGestures(const ChromaticScore& score, GlobalStageScore& stage) {
    int n = 0;
    // A visual motif is dressed in the colour motif of the scene it was born in.
    for (VisualMotif& m : stage.motifs) {
        const std::string scene = m.origin.rfind("scene:", 0) == 0 ? m.origin.substr(6) : std::string();
        if (const ChromaticScenePlan* sp = score.Scene(scene); sp != nullptr) {
            m.colorMotif = sp->motif;
        }
    }
    for (const ChromaticScenePlan& sp : score.scenes) {
        for (const GestureChromaticProfile& gp : sp.gestures) {
            for (GlobalGesture& g : stage.gestures) {
                if (g.id != gp.gesture || gp.front.empty()) {
                    continue;
                }
                if (gp.colorBy == "distance") {
                    g.colors = { gp.start, gp.secondary, gp.arrival == gp.secondary ? gp.tail : gp.arrival };
                } else if (gp.colorBy == "trail") {
                    g.colors = { gp.front, gp.secondary, gp.tail };
                } else {
                    g.colors = { gp.front, gp.secondary };
                }
                g.colors.erase(std::unique(g.colors.begin(), g.colors.end()), g.colors.end());
                g.colorBy = gp.colorBy;
                g.colorReason = gp.why;
                ++n;
            }
        }
    }
    return n;
}

// ============================================================= words

const char* HarmonyId(Harmony h) {
    switch (h) {
    case Harmony::Monochromatic: return "monochromatic";
    case Harmony::Analogous: return "analogous";
    case Harmony::Complementary: return "complementary";
    case Harmony::SplitComplementary: return "split-complementary";
    case Harmony::Triadic: return "triadic";
    case Harmony::Tetradic: return "tetradic";
    case Harmony::WarmCool: return "warm-cool";
    }
    return "analogous";
}

bool HarmonyFromId(const std::string& id, Harmony& h) {
    for (Harmony k : AllHarmonies()) {
        if (id == HarmonyId(k)) {
            h = k;
            return true;
        }
    }
    return false;
}

std::string HarmonyPhrase(Harmony h) {
    switch (h) {
    case Harmony::Monochromatic: return COMPOSER_TEXT("One colour in several lights");
    case Harmony::Analogous: return COMPOSER_TEXT("Neighbouring colours");
    case Harmony::Complementary: return COMPOSER_TEXT("Opposite colours");
    case Harmony::SplitComplementary: return COMPOSER_TEXT("A colour and the neighbours of its opposite");
    case Harmony::Triadic: return COMPOSER_TEXT("Three colours far apart");
    case Harmony::Tetradic: return COMPOSER_TEXT("Two pairs of opposites");
    case Harmony::WarmCool: return COMPOSER_TEXT("Warm against cool");
    }
    return "";
}

std::string ColorRolePhrase(const std::string& role) {
    if (role == "main") return COMPOSER_TEXT("Main colour");
    if (role == "complement") return COMPOSER_TEXT("Second colour");
    if (role == "accent") return COMPOSER_TEXT("Accent");
    if (role == "response") return COMPOSER_TEXT("Answer in contrast");
    if (role == "background") return COMPOSER_TEXT("Background");
    if (role == "neutral") return COMPOSER_TEXT("Neutral");
    if (role == "decorative") return COMPOSER_TEXT("Decoration");
    if (role == "identity") return COMPOSER_TEXT("Its own colours");
    if (role == "manual") return COMPOSER_TEXT("Chosen by hand");
    return role;
}

std::string PaletteRolePhrase(const std::string& role) {
    if (role == "anchor") return COMPOSER_TEXT("Main");
    if (role == "secondary") return COMPOSER_TEXT("Secondary");
    if (role == "accent") return COMPOSER_TEXT("Accent");
    if (role == "contrast") return COMPOSER_TEXT("Contrast");
    if (role == "transition") return COMPOSER_TEXT("Transition");
    if (role == "neutral") return COMPOSER_TEXT("Neutral");
    if (role == "shadow") return COMPOSER_TEXT("Deep");
    if (role == "highlight") return COMPOSER_TEXT("Light");
    return role;
}

std::string ChromaticTransitionPhrase(const std::string& kind) {
    if (kind == "cut") return COMPOSER_TEXT("Changes at once");
    if (kind == "fade") return COMPOSER_TEXT("Fades into the next colours");
    if (kind == "hue-drift") return COMPOSER_TEXT("The colour slides");
    if (kind == "saturation") return COMPOSER_TEXT("More or less saturated");
    if (kind == "lightness") return COMPOSER_TEXT("Lighter or deeper");
    if (kind == "introduce") return COMPOSER_TEXT("The next colour comes in early");
    if (kind == "dominant-change") return COMPOSER_TEXT("The main colour changes, figure by figure");
    if (kind == "return") return COMPOSER_TEXT("Back to colours heard before");
    if (kind == "hold") return COMPOSER_TEXT("The colours hold");
    return kind;
}

// ============================================================= files

namespace {

json PaletteJson(const ColorPaletteProfile& p) {
    json colors = json::array();
    for (const PaletteColor& c : p.colors) {
        colors.push_back({ { "role", c.role }, { "hex", c.hex }, { "L", c.L }, { "C", c.C }, { "h", c.h }, { "why", c.why } });
    }
    return { { "id", p.id }, { "harmony", HarmonyId(p.harmony) }, { "anchorHue", Round3(p.anchorHue) }, { "saturation", p.saturation }, { "lightness", p.lightness }, { "colors", colors }, { "reason", p.reason } };
}

json MotifsJson(const std::vector<ChromaticMotif>& motifs) {
    json out = json::array();
    for (const ChromaticMotif& m : motifs) {
        out.push_back({ { "id", m.id }, { "kind", m.kind }, { "anchor", m.anchor }, { "second", m.second }, { "accent", m.accent }, { "from", m.from },
                        { "firstScene", m.firstScene }, { "scenes", m.scenes }, { "why", m.why } });
    }
    return out;
}

ColorPaletteProfile PaletteFrom(const json& j) {
    ColorPaletteProfile p;
    p.id = j.value("id", std::string());
    (void)HarmonyFromId(j.value("harmony", std::string()), p.harmony);
    p.anchorHue = j.value("anchorHue", 0.0);
    p.saturation = j.value("saturation", 0.8);
    p.lightness = j.value("lightness", 0.68);
    p.reason = j.value("reason", std::string());
    for (const json& c : j.value("colors", json::array())) {
        PaletteColor k;
        k.role = c.value("role", std::string());
        k.hex = c.value("hex", std::string());
        k.L = c.value("L", 0.0);
        k.C = c.value("C", 0.0);
        k.h = c.value("h", 0.0);
        k.why = c.value("why", std::string());
        p.colors.push_back(k);
    }
    return p;
}

} // namespace

std::string ToJson(const ChromaticScore& s) {
    json scenes = json::array();
    for (const ChromaticScenePlan& sp : s.scenes) {
        json figures = json::array();
        for (const FigureColorRole& r : sp.figures) {
            figures.push_back({ { "figure", r.figure }, { "role", r.role }, { "colors", r.colors }, { "why", r.why } });
        }
        json gestures = json::array();
        for (const GestureChromaticProfile& g : sp.gestures) {
            gestures.push_back({ { "gesture", g.gesture }, { "start", g.start }, { "front", g.front }, { "secondary", g.secondary }, { "tail", g.tail }, { "arrival", g.arrival },
                                 { "colorBy", g.colorBy }, { "background", g.background }, { "contrast", g.contrast }, { "intent", g.intent }, { "presence", g.presence }, { "why", g.why } });
        }
        scenes.push_back({ { "id", sp.id }, { "startMS", sp.startMS }, { "endMS", sp.endMS }, { "sectionKind", sp.sectionKind }, { "mode", sp.mode }, { "energy", sp.energy },
                           { "palette", PaletteJson(sp.palette) }, { "dominant", sp.dominant }, { "secondary", sp.secondary }, { "accents", sp.accents }, { "contrasts", sp.contrasts },
                           { "saturation", sp.saturation }, { "intensity", sp.intensity }, { "monochrome", sp.monochrome }, { "evolution", sp.evolution }, { "motif", sp.motif }, { "motifOperation", sp.motifOperation }, { "hueBudget", sp.hueBudget }, { "accentShare", sp.accentShare },
                           { "used", sp.used }, { "intent", sp.intent }, { "protagonist", sp.protagonist }, { "companions", sp.companions }, { "discreet", sp.discreet },
                           { "kept", sp.kept }, { "introduced", sp.introduced }, { "feeling", sp.feeling }, { "feelingColor", sp.feelingColor }, { "feelingSource", sp.feelingSource },
                           { "figures", figures }, { "gestures", gestures },
                           { "toNext", { { "kind", sp.toNext.kind }, { "startMS", sp.toNext.startMS }, { "endMS", sp.toNext.endMS }, { "order", sp.toNext.order }, { "why", sp.toNext.why } } },
                           { "reasons", sp.reasons }, { "manual", sp.manual }, { "locked", sp.locked } });
    }
    const ChromaticOptions& o = s.options;
    return json({ { "kind", "composer-color-score" }, { "schemaVersion", s.schemaVersion }, { "version", s.version }, { "enabled", s.enabled },
                  { "options", { { "enabled", o.enabled }, { "variety", o.variety }, { "style", o.style }, { "contrast", o.contrast }, { "accents", o.accents }, { "evolution", o.evolution },
                                 { "christmasInfluence", o.christmasInfluence }, { "saturation", o.saturation }, { "warmth", o.warmth }, { "gradual", o.gradual },
                                 { "respectIdentity", o.respectIdentity }, { "songColors", o.songColors }, { "seed", o.seed } } },
                  { "song", PaletteJson(s.song) }, { "identityReason", s.identityReason }, { "briefSource", s.briefSource }, { "mood", s.mood }, { "scenes", scenes },
                  { "motifs", s.motifs }, { "motifDefs", MotifsJson(s.motifDefs) }, { "edits", s.edits }, { "warnings", s.warnings }, { "limits", s.limits }, { "review", "not-reviewed" } })
        .dump(1);
}

bool FromJson(const std::string& text, ChromaticScore& s, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("kind", std::string()) != "composer-color-score") {
        error = "not a colour score";
        return false;
    }
    if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > CHROMATIC_SCORE_SCHEMA_VERSION) { // 1 reads as a score without motifs
        error = "colour score: schema not read";
        return false;
    }
    try {
        s = ChromaticScore();
        s.enabled = root.value("enabled", false);
        s.version = root.value("version", std::string());
        const json o = root.value("options", json::object());
        s.options.enabled = o.value("enabled", true);
        s.options.variety = o.value("variety", 0.5);
        s.options.style = o.value("style", 0.4);
        s.options.contrast = o.value("contrast", 0.5);
        s.options.accents = o.value("accents", 0.5);
        s.options.evolution = o.value("evolution", 0.5);
        s.options.christmasInfluence = o.value("christmasInfluence", 0.5);
        s.options.saturation = o.value("saturation", 0.5);
        s.options.warmth = o.value("warmth", 0.0);
        s.options.gradual = o.value("gradual", 0.5);
        s.options.respectIdentity = o.value("respectIdentity", false);
        s.options.songColors = o.value("songColors", std::vector<std::string>());
        s.options.seed = o.value("seed", static_cast<uint64_t>(2026));
        s.song = PaletteFrom(root.value("song", json::object()));
        s.identityReason = root.value("identityReason", std::string());
        s.briefSource = root.value("briefSource", std::string());
        s.mood = root.value("mood", std::string());
        s.motifs = root.value("motifs", std::vector<std::string>());
        for (const json& m : root.value("motifDefs", json::array())) {
            ChromaticMotif k;
            k.id = m.value("id", std::string());
            k.kind = m.value("kind", std::string());
            k.anchor = m.value("anchor", std::string());
            k.second = m.value("second", std::string());
            k.accent = m.value("accent", std::string());
            k.from = m.value("from", std::string());
            k.firstScene = m.value("firstScene", std::string());
            k.scenes = m.value("scenes", std::vector<std::string>());
            k.why = m.value("why", std::string());
            s.motifDefs.push_back(k);
        }
        s.edits = root.value("edits", 0);
        s.warnings = root.value("warnings", std::vector<std::string>());
        s.limits = root.value("limits", std::vector<std::string>());
        for (const json& j : root.value("scenes", json::array())) {
            ChromaticScenePlan sp;
            sp.id = j.value("id", std::string());
            sp.startMS = j.value("startMS", 0);
            sp.endMS = j.value("endMS", 0);
            sp.sectionKind = j.value("sectionKind", std::string());
            sp.mode = j.value("mode", std::string());
            sp.energy = j.value("energy", 0.0);
            sp.palette = PaletteFrom(j.value("palette", json::object()));
            sp.dominant = j.value("dominant", std::string());
            sp.secondary = j.value("secondary", std::vector<std::string>());
            sp.accents = j.value("accents", std::vector<std::string>());
            sp.contrasts = j.value("contrasts", std::vector<std::string>());
            sp.saturation = j.value("saturation", 0.0);
            sp.intensity = j.value("intensity", 0.0);
            sp.monochrome = j.value("monochrome", false);
            sp.evolution = j.value("evolution", std::string());
            sp.motif = j.value("motif", std::string());
            sp.motifOperation = j.value("motifOperation", std::string());
            sp.hueBudget = j.value("hueBudget", 1);
            sp.accentShare = j.value("accentShare", 0.0);
            sp.used = j.value("used", std::vector<std::string>());
            sp.intent = j.value("intent", std::string());
            sp.protagonist = j.value("protagonist", std::string());
            sp.companions = j.value("companions", std::vector<std::string>());
            sp.discreet = j.value("discreet", std::vector<std::string>());
            sp.kept = j.value("kept", std::vector<std::string>());
            sp.introduced = j.value("introduced", std::vector<std::string>());
            sp.feeling = j.value("feeling", std::string());
            sp.feelingColor = j.value("feelingColor", std::string());
            sp.feelingSource = j.value("feelingSource", std::string());
            for (const json& f : j.value("figures", json::array())) {
                sp.figures.push_back({ f.value("figure", std::string()), f.value("role", std::string()), f.value("colors", std::vector<std::string>()), f.value("why", std::string()) });
            }
            for (const json& g : j.value("gestures", json::array())) {
                GestureChromaticProfile gp;
                gp.gesture = g.value("gesture", std::string());
                gp.start = g.value("start", std::string());
                gp.front = g.value("front", std::string());
                gp.secondary = g.value("secondary", std::string());
                gp.tail = g.value("tail", std::string());
                gp.arrival = g.value("arrival", std::string());
                gp.colorBy = g.value("colorBy", std::string("field"));
                gp.background = g.value("background", std::vector<std::string>());
                gp.contrast = g.value("contrast", 0.0);
                gp.why = g.value("why", std::string());
                gp.intent = g.value("intent", std::string());
                gp.presence = g.value("presence", std::string());
                sp.gestures.push_back(gp);
            }
            const json t = j.value("toNext", json::object());
            sp.toNext.kind = t.value("kind", std::string());
            sp.toNext.startMS = t.value("startMS", 0);
            sp.toNext.endMS = t.value("endMS", 0);
            sp.toNext.order = t.value("order", std::vector<std::string>());
            sp.toNext.why = t.value("why", std::string());
            sp.reasons = j.value("reasons", std::vector<std::string>());
            sp.manual = j.value("manual", false);
            sp.locked = j.value("locked", false);
            s.scenes.push_back(std::move(sp));
        }
    } catch (const std::exception& e) {
        error = std::string("colour score: ") + e.what();
        return false;
    }
    return true;
}

std::string ChromaticReport(const ChromaticScore& s) {
    if (!s.enabled) {
        return "Colour direction off\n";
    }
    std::string out = fmt::format("Colour direction {} — mood {}, interpretation from {}\n", s.version, s.mood, s.briefSource.empty() ? std::string("none") : s.briefSource);
    out += fmt::format("song: {} ({}): ", HarmonyPhrase(s.song.harmony), s.identityReason);
    for (const PaletteColor& c : s.song.colors) {
        out += fmt::format("{} {} ", c.role, c.hex);
    }
    out += "\n";
    for (const ChromaticScenePlan& sp : s.scenes) {
        std::map<std::string, int> roles;
        for (const FigureColorRole& r : sp.figures) {
            roles[r.role]++;
        }
        std::string roleText;
        for (const auto& [r, n] : roles) {
            roleText += fmt::format("{}{} {}", roleText.empty() ? "" : ", ", r, n);
        }
        out += fmt::format("  {:>7.1f}-{:<7.1f} s {:<12} {:<9} {:<19} {} {} {} {}{}  [{}]  -> {}\n", sp.startMS / 1000.0, sp.endMS / 1000.0, sp.sectionKind, sp.evolution,
                           HarmonyId(sp.palette.harmony), sp.palette.Hex("anchor"), sp.palette.Hex("secondary"), sp.palette.Hex("accent"), sp.palette.Hex("contrast"),
                           sp.monochrome ? " (one family)" : "", roleText, sp.toNext.kind);
        for (const GestureChromaticProfile& g : sp.gestures) {
            out += fmt::format("      gesture {}: {} -> {} ({}), {:.0f} ΔE over its background\n", g.gesture, g.front, g.secondary, g.colorBy, g.contrast);
        }
    }
    for (const std::string& w : s.warnings) {
        out += "! " + w + "\n";
    }
    for (const std::string& l : s.limits) {
        out += "limit: " + l + "\n";
    }
    return out;
}

std::string ToJson(const ChromaticEdits& e) {
    json edits = json::array();
    for (const ChromaticEdit& x : e.edits) {
        edits.push_back({ { "action", x.action }, { "sceneStartMS", x.sceneStartMS }, { "figure", x.figure }, { "role", x.role }, { "value", x.value }, { "note", x.note } });
    }
    return json({ { "kind", "composer-color-edits" }, { "schemaVersion", e.schemaVersion }, { "edits", edits } }).dump(1);
}

bool FromJson(const std::string& text, ChromaticEdits& e, std::string& error) {
    static const std::set<std::string> ACTIONS = { "color", "accent", "lock-palette", "more-contrast", "less-contrast", "figure-color", "identity", "no-identity", "monochrome", "no-monochrome",
                                                   "keep-identity", "recall-motif", "fewer-colours", "fewer-accents", "keep-main-colour", "no-new-colours" };
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("kind", std::string()) != "composer-color-edits") {
        error = "not a file of colour edits";
        return false;
    }
    e = ChromaticEdits();
    for (const json& j : root.value("edits", json::array())) {
        if (!j.is_object()) {
            continue;
        }
        ChromaticEdit x;
        x.action = j.value("action", std::string());
        if (!ACTIONS.contains(x.action)) {
            error = "colour edits: unknown action '" + x.action + "'";
            return false;
        }
        x.sceneStartMS = j.value("sceneStartMS", -1);
        x.figure = j.value("figure", std::string());
        x.role = j.value("role", std::string());
        x.value = j.value("value", std::string());
        x.note = j.value("note", std::string());
        e.edits.push_back(x);
    }
    return true;
}

std::string ChromaticEditsPath(const std::string& showDir, const std::string& audioHash) {
    return (fs::path(showDir) / "ShowComposer" / "color" / (audioHash.substr(0, 16) + ".edits.json")).string();
}

} // namespace composer
