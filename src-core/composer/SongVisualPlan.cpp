#include "composer/SongVisualPlan.h"

#include "composer/MusicStructure.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using json = nlohmann::json;

namespace composer {

namespace {

bool Has(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

double Unit(uint64_t seed, const std::string& key) {
    return static_cast<double>(Fnv(std::to_string(seed) + "/" + key) % 1000003ULL) / 1000003.0;
}

struct Rgb {
    double r = 0, g = 0, b = 0;
};

Rgb FromHsv(double h, double s, double v) {
    h = std::fmod(std::fmod(h, 360.0) + 360.0, 360.0);
    const double c = v * s;
    const double x = c * (1 - std::fabs(std::fmod(h / 60.0, 2.0) - 1));
    const double m = v - c;
    Rgb o;
    if (h < 60) {
        o = { c, x, 0 };
    } else if (h < 120) {
        o = { x, c, 0 };
    } else if (h < 180) {
        o = { 0, c, x };
    } else if (h < 240) {
        o = { 0, x, c };
    } else if (h < 300) {
        o = { x, 0, c };
    } else {
        o = { c, 0, x };
    }
    return { o.r + m, o.g + m, o.b + m };
}

Rgb FromHex(const std::string& hex) {
    if (hex.size() != 7 || hex[0] != '#') {
        return {};
    }
    return { std::strtol(hex.substr(1, 2).c_str(), nullptr, 16) / 255.0, std::strtol(hex.substr(3, 2).c_str(), nullptr, 16) / 255.0, std::strtol(hex.substr(5, 2).c_str(), nullptr, 16) / 255.0 };
}

std::string Hex(const Rgb& c) {
    const auto byte = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    return fmt::format("#{:02X}{:02X}{:02X}", byte(c.r), byte(c.g), byte(c.b));
}

double HueOf(const Rgb& c, double& saturation) {
    const double mx = std::max({ c.r, c.g, c.b });
    const double mn = std::min({ c.r, c.g, c.b });
    saturation = mx <= 0 ? 0 : (mx - mn) / mx;
    if (mx - mn < 1e-6) {
        return 0;
    }
    double h = 0;
    const double d = mx - mn;
    if (mx == c.r) {
        h = std::fmod((c.g - c.b) / d, 6.0);
    } else if (mx == c.g) {
        h = (c.b - c.r) / d + 2;
    } else {
        h = (c.r - c.g) / d + 4;
    }
    h *= 60;
    return h < 0 ? h + 360 : h;
}

bool IsHex(const std::string& s) {
    return s.size() == 7 && s[0] == '#' && std::all_of(s.begin() + 1, s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

// The classic Christmas colours, the pole the slider pulls towards.
const std::vector<std::pair<double, std::string>>& ChristmasPole() {
    static const std::vector<std::pair<double, std::string>> POLE = { { 355, "#D0001A" }, { 145, "#00A33D" }, { 42, "#FFB627" } };
    return POLE;
}

Rgb TowardsChristmas(const Rgb& c, double influence) {
    if (influence <= 0) {
        return c;
    }
    double s = 0;
    const double h = HueOf(c, s);
    if (s < 0.15) {
        return c; // white stays white
    }
    const auto& pole = ChristmasPole();
    std::string nearest = pole.front().second;
    double best = 1e9;
    for (const auto& [hue, hex] : pole) {
        const double d = std::fabs(std::fmod(h - hue + 540.0, 360.0) - 180.0);
        if (d < best) {
            best = d;
            nearest = hex;
        }
    }
    const Rgb t = FromHex(nearest);
    return { c.r + (t.r - c.r) * influence, c.g + (t.g - c.g) * influence, c.b + (t.b - c.b) * influence };
}

std::string ArcStep(double arc, double previous, bool first, bool last, const std::string& kind) {
    if (first) {
        return "establish";
    }
    if (last) {
        return "close";
    }
    if (arc >= 0.8 && (kind == "chorus" || kind == "unknown" || kind == "instrumental")) {
        return "peak";
    }
    if (arc < 0.25) {
        return "rest";
    }
    if (arc < previous - 0.15) {
        return "release";
    }
    return "build";
}

} // namespace

const SurfaceRoleAt* VisualPlanScene::Role(const std::string& family) const {
    for (const SurfaceRoleAt& r : roles) {
        if (r.family == family) {
            return &r;
        }
    }
    return nullptr;
}

const VisualPlanScene* SongVisualPlan::Scene(const std::string& segmentId) const {
    for (const VisualPlanScene& s : scenes) {
        if (s.segmentId == segmentId) {
            return &s;
        }
    }
    return nullptr;
}

double AnchorHue(const SongAnalysis& analysis, const SongVisualBrief* brief, std::string& reason) {
    if (brief != nullptr) {
        for (const std::string& c : brief->colors) {
            double s = 0;
            const double h = HueOf(FromHex(c), s);
            if (s >= 0.25) {
                reason = fmt::format("the first colour the lyrics suggest ({}, {})", c, brief->source);
                return h;
            }
        }
    }
    // The circle of fifths laid on the colour wheel: C red, G orange, D yellow... A
    // convention, as Scriabin's was, chosen so that related keys get related colours.
    static const std::vector<std::string> FIFTHS = { "C", "G", "D", "A", "E", "B", "F#", "C#", "G#", "D#", "A#", "F" };
    static const std::map<std::string, std::string> FLATS = { { "Db", "C#" }, { "Eb", "D#" }, { "Gb", "F#" }, { "Ab", "G#" }, { "Bb", "A#" } };
    const std::string key = analysis.key;
    const size_t space = key.find(' ');
    std::string tonic = key.substr(0, space);
    if (const auto f = FLATS.find(tonic); f != FLATS.end()) {
        tonic = f->second;
    }
    const auto at = std::find(FIFTHS.begin(), FIFTHS.end(), tonic);
    if (at != FIFTHS.end()) {
        const double hue = static_cast<double>(at - FIFTHS.begin()) * 30.0;
        reason = fmt::format("the key, {}, on the circle of fifths laid over the colour wheel (a convention, not a measurement)", key);
        return hue;
    }
    reason = "no key and no colours from the lyrics: a warm neutral";
    return 30;
}

double CompanionHue(double anchorHue, const SongVisualBrief* brief, std::string& reason) {
    if (brief != nullptr) {
        int coloured = 0;
        for (const std::string& c : brief->colors) {
            double s = 0;
            const double h = HueOf(FromHex(c), s);
            if (s < 0.25) {
                continue;
            }
            // The first coloured one is the anchor; the next far enough from it is the companion.
            if (++coloured >= 2 && std::fabs(std::fmod(h - anchorHue + 540.0, 360.0) - 180.0) >= 60) {
                reason = fmt::format("the next colour the lyrics suggest ({})", c);
                return h;
            }
        }
    }
    reason = "the anchor's near opposite";
    return std::fmod(anchorHue + 150.0, 360.0);
}

ColorChoice ColorsForPart(double anchorHue, double companionHue, const std::string& kind, double energy, bool minor, double christmasInfluence) {
    // Two hues carry a whole song, as in the references (two hues hold most of the
    // colour time): the anchor and its companion, each full or light; the parts change
    // which one leads, and white crowns the chorus.
    struct Shape {
        std::string tones; // A anchor, a anchor light, C companion, c companion light
        double saturation;
        double value;
        bool white;
        const char* why;
    };
    static const std::map<std::string, Shape> SHAPES = {
        { "intro", { "A", 0.55, 0.7, false, "the anchor alone, soft: the show starts quiet" } },
        { "verse", { "Aa", 0.75, 0.8, false, "the anchor, full and light: a verse tells, it does not shout" } },
        { "pre-chorus", { "AC", 0.85, 0.85, false, "the companion appears: the chorus is coming" } },
        { "chorus", { "AC", 0.95, 1.0, true, "the anchor against its companion, with white: the chorus is the fullest" } },
        { "bridge", { "Cc", 0.8, 0.85, false, "the companion leads: the bridge goes somewhere else" } },
        { "interlude", { "CA", 0.6, 0.75, false, "the companion, soft: a pause without words" } },
        { "instrumental", { "CA", 0.7, 0.85, false, "the companion first: the music without words" } },
        { "outro", { "A", 0.5, 0.65, false, "back to the anchor, soft: the close" } },
    };
    const std::string shapeKind = SHAPES.contains(kind) ? kind : energy >= 0.65 ? "chorus" : energy < 0.35 ? "intro" : "verse";
    const Shape& shape = SHAPES.at(shapeKind);
    const double hueShift = minor ? -20.0 : 0.0;
    const double s = std::clamp(shape.saturation * (minor ? 0.9 : 1.0), 0.0, 1.0);
    const double v = std::clamp(shape.value * (0.75 + 0.25 * energy), 0.35, 1.0);
    const double influence = std::clamp(christmasInfluence, 0.0, 1.0);
    ColorChoice out;
    for (char tone : shape.tones) {
        const double hue = (tone == 'A' || tone == 'a' ? anchorHue : companionHue) + hueShift;
        const bool light = tone == 'a' || tone == 'c';
        const std::string hex = Hex(TowardsChristmas(FromHsv(hue, light ? s * 0.55 : s, light ? std::min(1.0, v + 0.1) : v), influence));
        if (std::find(out.colors.begin(), out.colors.end(), hex) == out.colors.end()) {
            out.colors.push_back(hex);
        }
    }
    if (shape.white) {
        out.colors.push_back("#FFFFFF");
    }
    out.accent = shape.white ? "#FFFFFF" : Hex(TowardsChristmas(FromHsv(companionHue + hueShift, 0.6, 1.0), influence));
    out.reason = fmt::format("{}; {}{}", shape.why, minor ? "minor key, a little cooler; " : "", influence <= 0 ? std::string("the song's own colours") : fmt::format("{:.0f}% towards the Christmas colours", influence * 100));
    return out;
}

SongVisualPlan PlanSongVisuals(const VisualPlanInput& in) {
    SongVisualPlan p;
    p.options = in.options;
    if (in.plan == nullptr || in.analysis == nullptr || in.direction == nullptr) {
        p.warnings.push_back("missing-required-input");
        return p;
    }
    p.enabled = true;
    const CompositionPlan& plan = *in.plan;
    const ShowVisualDirection& d = *in.direction;
    p.briefSource = in.brief != nullptr ? in.brief->source : std::string();
    const double anchor = AnchorHue(*in.analysis, in.brief, p.anchorReason);
    std::string companionReason;
    const double companion = CompanionHue(anchor, in.brief, companionReason);
    p.anchorHue = fmt::format("{:.0f}", anchor);
    p.anchorReason += fmt::format("; companion {:.0f}, {}", companion, companionReason);
    const bool minor = in.analysis->key.find("minor") != std::string::npos;

    std::vector<const PlanSegment*> music;
    for (const PlanSegment& s : plan.segments) {
        if (!Has(s.flags, "silence")) {
            music.push_back(&s);
        }
    }
    double low = 1, high = 0;
    for (const PlanSegment* s : music) {
        const double e = s->targetIntensity > 0 ? s->targetIntensity : s->energy;
        low = std::min(low, e);
        high = std::max(high, e);
    }
    std::set<std::string> families;
    std::string protagonist;
    for (const VisualFamily& f : d.families) {
        if (f.targets.empty()) {
            continue;
        }
        families.insert(f.id);
        if (f.id == "matrix" || (f.id == "mega-tree" && protagonist.empty())) {
            protagonist = f.id;
        }
    }
    if (protagonist.empty() && !d.families.empty()) {
        protagonist = d.families.front().id;
    }

    std::vector<const Recipe*> pool;
    if (in.options.recipes && in.recipes != nullptr) {
        for (const Recipe& r : in.recipes->recipes) {
            if (r.excluded) {
                continue;
            }
            int present = 0;
            bool leadPresent = false;
            for (const RecipeSurface& s : r.surfaces) {
                if (families.contains(s.family)) {
                    ++present;
                    leadPresent = leadPresent || s.role == "lead";
                }
            }
            if (present >= 2 || (present >= 1 && leadPresent)) {
                pool.push_back(&r);
            }
        }
        if (pool.empty() && !in.recipes->recipes.empty()) {
            p.warnings.push_back("no-recipe-fits-this-layout");
        }
    }

    std::map<std::string, int> uses;
    std::map<int, std::string> byRepeat;
    std::map<int, ColorChoice> coloursByRepeat;
    std::string previousRecipe;
    double previousArc = 0;
    for (size_t i = 0; i < music.size(); ++i) {
        const PlanSegment& s = *music[i];
        VisualPlanScene scene;
        scene.segmentId = s.id;
        scene.startMS = s.startMS;
        scene.endMS = s.endMS;
        scene.sectionKind = s.sectionKind;
        scene.repeatGroup = s.repeatGroup;
        const double e = s.targetIntensity > 0 ? s.targetIntensity : s.energy;
        scene.arc = Round3(high - low < 0.05 ? 0.5 : (e - low) / (high - low));
        scene.arcStep = ArcStep(scene.arc, previousArc, i == 0, i + 1 == music.size(), s.sectionKind);
        scene.reasons.push_back({ "arc", fmt::format("{} at {:.2f} of the song's range", scene.arcStep, scene.arc) });

        // ---------------------------------------------------------- the recipe
        const Recipe* chosen = nullptr;
        if (!pool.empty()) {
            const auto motif = byRepeat.find(s.repeatGroup);
            if (s.repeatGroup >= 0 && motif != byRepeat.end()) {
                for (const Recipe* r : pool) {
                    if (r->id == motif->second) {
                        chosen = r;
                        scene.motif = fmt::format("repeat-group {}", s.repeatGroup);
                        scene.recipeScore = 1;
                        scene.reasons.push_back({ "recipe", fmt::format("{}: the music comes back, and so does its recipe", r->id) });
                    }
                }
            }
            if (chosen == nullptr) {
                double best = -1;
                std::string why;
                for (const Recipe* r : pool) {
                    const double energyFit = std::clamp(1.0 - std::fabs(r->energy - scene.arc), 0.1, 1.0);
                    double context = 1.0;
                    const std::map<std::string, std::vector<std::string>> WANTS = {
                        { "establish", { "intro", "low" } }, { "close", { "outro", "low" } }, { "peak", { "climax", "high" } }, { "rest", { "low" } }, { "build", { "mid", "high" } }, { "release", { "mid", "low" } }
                    };
                    for (const std::string& w : WANTS.at(scene.arcStep)) {
                        if (std::find(r->contexts.begin(), r->contexts.end(), w) != r->contexts.end()) {
                            context += 0.3;
                        }
                    }
                    if (std::find(r->contexts.begin(), r->contexts.end(), s.sectionKind) != r->contexts.end()) {
                        context += 0.3;
                    }
                    double function = 1.0;
                    if (scene.arcStep == "rest" || scene.arcStep == "close") {
                        function = r->function == "calm" ? 1.3 : r->function == "accent" ? 0.6 : 1.0;
                    } else if (scene.arcStep == "peak") {
                        function = r->function == "accent" || r->function == "bed" ? 1.25 : r->function == "calm" ? 0.5 : 1.0;
                    }
                    int present = 0;
                    for (const RecipeSurface& rs : r->surfaces) {
                        present += families.contains(rs.family) ? 1 : 0;
                    }
                    const double coverage = static_cast<double>(present) / static_cast<double>(r->surfaces.size());
                    const int used = uses[r->id];
                    const double variety = 1.0 / (1.0 + 2.0 * in.options.variety * used) * (r->id == previousRecipe ? 1.0 - 0.7 * in.options.variety : 1.0);
                    const double score = energyFit * context * function * (0.5 + 0.5 * coverage) * (0.5 + 0.5 * r->confidence) * (r->favorite ? 1.5 : 1.0) * variety *
                                         (1.0 + 0.05 * (Unit(in.options.seed, s.id + "/" + r->id) - 0.5));
                    if (score > best) {
                        best = score;
                        chosen = r;
                        why = fmt::format("{}: energy fit {:.2f}, context x{:.2f}, function x{:.2f}, {} of {} surfaces here, used {} times{}", r->id, energyFit, context, function, present,
                                          r->surfaces.size(), used, r->favorite ? ", a favourite" : "");
                    }
                }
                scene.recipeScore = Round3(best);
                scene.reasons.push_back({ "recipe", why });
            }
        }
        if (chosen != nullptr) {
            scene.recipeId = chosen->id;
            scene.recipeName = chosen->name;
            uses[chosen->id]++;
            previousRecipe = chosen->id;
            if (s.repeatGroup >= 0 && !byRepeat.contains(s.repeatGroup)) {
                byRepeat[s.repeatGroup] = chosen->id;
            }
        }

        // --------------------------------------------------- who does what here
        const bool quiet = scene.arcStep == "rest" || scene.arcStep == "establish" || scene.arcStep == "close";
        int resting = 0;
        int index = 0;
        for (const std::string& family : families) {
            SurfaceRoleAt role;
            role.family = family;
            const RecipeSurface* rs = chosen != nullptr ? chosen->Surface(family) : nullptr;
            if (rs != nullptr) {
                role.role = rs->role == "bed" ? "complement" : rs->role;
                role.why = "the recipe's " + rs->role;
            } else if (family == protagonist && (scene.arcStep == "peak" || s.sectionKind == "chorus")) {
                role.role = "lead";
                role.why = "the protagonist leads the chorus";
            } else if (quiet && family != protagonist && (index + static_cast<int>(i)) % 2 == 0) {
                role.role = "rest";
                role.why = "a quiet part: the house breathes";
                ++resting;
            } else {
                role.role = "complement";
                role.why = "accompanies";
            }
            scene.roles.push_back(role);
            ++index;
        }
        // Never everything at rest: what lights a quiet part is what makes it quiet.
        if (resting * 2 > static_cast<int>(scene.roles.size())) {
            for (SurfaceRoleAt& r : scene.roles) {
                if (r.role == "rest" && resting * 2 > static_cast<int>(scene.roles.size())) {
                    r.role = "complement";
                    r.why = "kept on: too much of the house would rest";
                    --resting;
                }
            }
        }

        // ---------------------------------------------------------- colours
        if (in.options.colorEngine) {
            const auto again = coloursByRepeat.find(s.repeatGroup);
            const ColorChoice c = s.repeatGroup >= 0 && again != coloursByRepeat.end()
                                      ? again->second
                                      : ColorsForPart(anchor, companion, s.sectionKind, scene.arc, minor, in.options.christmasInfluence);
            if (s.repeatGroup >= 0) {
                coloursByRepeat.try_emplace(s.repeatGroup, c);
            }
            scene.colors = c.colors;
            scene.accent = c.accent;
            scene.colorReason = c.reason + (s.repeatGroup >= 0 && again != coloursByRepeat.end() ? "; the colours of its first time" : "");
        }
        previousArc = scene.arc;
        p.scenes.push_back(std::move(scene));
    }

    // The signature of the composition.
    std::map<std::string, int> count;
    for (const VisualPlanScene& s : p.scenes) {
        if (!s.recipeId.empty()) {
            count[s.recipeId]++;
            ++p.recipeScenes;
        }
    }
    p.distinctRecipes = static_cast<int>(count.size());
    int most = 0;
    for (const auto& [id, n] : count) {
        most = std::max(most, n);
    }
    p.longestRepeatShare = p.scenes.empty() ? 0 : Round3(static_cast<double>(most) / static_cast<double>(p.scenes.size()));
    if (p.recipeScenes >= 4 && p.longestRepeatShare > 0.5) {
        p.warnings.push_back("monotony: one recipe plays more than half the scenes");
    }
    // Where the show peaks, for the report.
    size_t peak = 0;
    for (size_t i = 0; i < p.scenes.size(); ++i) {
        if (p.scenes[i].arc > p.scenes[peak].arc) {
            peak = i;
        }
    }
    const double where = p.scenes.empty() ? 0 : static_cast<double>(peak) / static_cast<double>(p.scenes.size());
    p.arcShape = where < 0.34 ? "early peak" : where < 0.67 ? "middle peak" : "late peak";
    return p;
}

void ApplyPlanColors(const SongVisualPlan& plan, ShowVisualDirection& direction) {
    if (!plan.enabled || !plan.options.colorEngine) {
        return;
    }
    for (SceneLook& look : direction.scenes) {
        const VisualPlanScene* s = plan.Scene(look.segmentId);
        if (s == nullptr || s->colors.empty()) {
            continue;
        }
        look.colors = s->colors;
        look.accent = s->accent;
        look.reasons.push_back({ "color-engine", s->colorReason });
    }
}

std::string ToJson(const SongVisualPlan& p) {
    json scenes = json::array();
    for (const VisualPlanScene& s : p.scenes) {
        json roles = json::array();
        for (const SurfaceRoleAt& r : s.roles) {
            roles.push_back({ { "family", r.family }, { "role", r.role }, { "why", r.why } });
        }
        json reasons = json::array();
        for (const Reason& r : s.reasons) {
            reasons.push_back({ { "code", r.code }, { "detail", r.detail } });
        }
        scenes.push_back({ { "segment", s.segmentId }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "sectionKind", s.sectionKind }, { "repeatGroup", s.repeatGroup }, { "arc", s.arc },
                           { "arcStep", s.arcStep }, { "recipe", s.recipeId }, { "recipeName", s.recipeName }, { "recipeScore", s.recipeScore }, { "motif", s.motif }, { "roles", roles },
                           { "colors", s.colors }, { "accent", s.accent }, { "colorReason", s.colorReason }, { "reasons", reasons } });
    }
    json root = { { "schemaVersion", p.schemaVersion }, { "version", p.version }, { "enabled", p.enabled }, { "arcShape", p.arcShape }, { "anchorHue", p.anchorHue },
                  { "anchorReason", p.anchorReason }, { "briefSource", p.briefSource },
                  { "options", { { "recipes", p.options.recipes }, { "colorEngine", p.options.colorEngine }, { "christmasInfluence", p.options.christmasInfluence }, { "variety", p.options.variety }, { "seed", p.options.seed } } },
                  { "distinctRecipes", p.distinctRecipes }, { "recipeScenes", p.recipeScenes }, { "longestRepeatShare", p.longestRepeatShare }, { "warnings", p.warnings }, { "scenes", scenes } };
    return root.dump(1);
}

std::string VisualPlanReport(const SongVisualPlan& p) {
    std::string out = fmt::format("Song visual plan {} — {}\n", p.version, p.enabled ? "on" : "off");
    out += fmt::format("arc: {}; colours from hue {} ({}); Christmas influence {:.0f}%; variety {:.0f}%\n", p.arcShape, p.anchorHue, p.anchorReason, p.options.christmasInfluence * 100, p.options.variety * 100);
    out += fmt::format("recipes: {} distinct over {} scenes; the most used plays {:.0f}% of the scenes\n", p.distinctRecipes, p.recipeScenes, p.longestRepeatShare * 100);
    for (const std::string& w : p.warnings) {
        out += "warning: " + w + "\n";
    }
    for (const VisualPlanScene& s : p.scenes) {
        out += fmt::format("{:>7}-{:<7} {:<12} {:<10} arc {:.2f}  {}\n", s.startMS, s.endMS, s.sectionKind, s.arcStep, s.arc, s.recipeId.empty() ? std::string("(no recipe)") : s.recipeName);
        std::string roles;
        for (const SurfaceRoleAt& r : s.roles) {
            roles += r.family + "=" + r.role + " ";
        }
        out += "    roles: " + roles + "\n";
        std::string colours;
        for (const std::string& c : s.colors) {
            colours += c + " ";
        }
        out += "    colours: " + colours + "(" + s.colorReason + ")\n";
        for (const Reason& r : s.reasons) {
            out += "    " + r.code + ": " + r.detail + "\n";
        }
    }
    return out;
}

bool ParseVisualBrief(const std::string& text, const std::string& source, SongVisualBrief& brief, std::string& error) {
    static const std::set<std::string> MOODS = { "joyful", "tender", "epic", "playful", "nostalgic", "dark", "calm", "festive" };
    SongVisualBrief out;
    out.source = source;
    try {
        const json root = json::parse(text);
        if (!root.is_object()) {
            error = "not an object";
            return false;
        }
        const std::string mood = root.value("mood", std::string());
        if (MOODS.contains(mood)) {
            out.mood = mood;
        } else if (!mood.empty()) {
            out.warnings.push_back("mood-unknown:" + mood.substr(0, 20));
        }
        if (root.contains("colors") && root["colors"].is_array()) {
            for (const json& c : root["colors"]) {
                std::string hex = c.is_string() ? c.get<std::string>() : std::string();
                std::transform(hex.begin(), hex.end(), hex.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
                if (IsHex(hex) && out.colors.size() < 5) {
                    out.colors.push_back(hex);
                } else {
                    out.warnings.push_back("color-dropped");
                }
            }
        }
        if (root.contains("imagery") && root["imagery"].is_array()) {
            for (const json& i : root["imagery"]) {
                if (i.is_string() && out.imagery.size() < 8) {
                    out.imagery.push_back(i.get<std::string>().substr(0, 40));
                }
            }
        }
        if (root.contains("christmas") && root["christmas"].is_number()) {
            out.christmas = std::clamp(root["christmas"].get<double>(), 0.0, 1.0);
        }
        if (root.contains("energy") && root["energy"].is_number()) {
            out.energy = std::clamp(root["energy"].get<double>(), 0.0, 1.0);
        }
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    if (out.mood.empty() && out.colors.empty() && out.imagery.empty() && out.christmas < 0 && out.energy < 0) {
        error = "nothing usable";
        return false;
    }
    brief = std::move(out);
    return true;
}

std::string ToJson(const SongVisualBrief& b) {
    return json({ { "source", b.source }, { "mood", b.mood }, { "colors", b.colors }, { "imagery", b.imagery }, { "christmas", b.christmas }, { "energy", b.energy }, { "warnings", b.warnings } }).dump(1);
}

SongVisualBrief LocalVisualBrief(const std::string& lyricsText, const std::string& title) {
    static const std::set<std::string> CHRISTMAS = { "navidad", "navideno", "navidena", "belen", "nochebuena", "reyes", "pastores", "pastorcito", "villancico", "nieve", "santa", "noel",
                                                     "christmas", "xmas", "jingle", "sleigh", "reno", "renos", "estrella", "campanas", "campana", "pesebre", "nino", "jesus", "mistletoe",
                                                     "snow", "bells", "reindeer", "nacimiento", "portal" };
    static const std::map<std::string, std::string> COLOURS = { { "rojo", "#D0001A" }, { "roja", "#D0001A" }, { "rojos", "#D0001A" }, { "rojas", "#D0001A" }, { "verdes", "#00A33D" }, { "dorados", "#FFB627" }, { "doradas", "#FFB627" },
                                                                { "azules", "#1E6BFF" }, { "blancos", "#FFFFFF" }, { "blancas", "#FFFFFF" }, { "rosas", "#FF4FA0" }, { "morados", "#8A3FFF" }, { "red", "#D0001A" }, { "verde", "#00A33D" }, { "green", "#00A33D" },
                                                                { "oro", "#FFB627" }, { "dorado", "#FFB627" }, { "dorada", "#FFB627" }, { "gold", "#FFB627" }, { "azul", "#1E6BFF" },
                                                                { "blue", "#1E6BFF" }, { "plata", "#C0C8D8" }, { "silver", "#C0C8D8" }, { "rosa", "#FF4FA0" }, { "pink", "#FF4FA0" },
                                                                { "morado", "#8A3FFF" }, { "purple", "#8A3FFF" }, { "blanco", "#FFFFFF" }, { "blanca", "#FFFFFF" }, { "white", "#FFFFFF" } };
    static const std::map<std::string, std::string> MOODS = { { "fiesta", "playful" }, { "baila", "playful" }, { "bailar", "playful" }, { "alegre", "joyful" }, { "alegria", "joyful" },
                                                              { "feliz", "joyful" }, { "amor", "tender" }, { "corazon", "tender" }, { "paz", "calm" }, { "silencio", "calm" },
                                                              { "recuerdo", "nostalgic" }, { "happy", "joyful" }, { "dance", "playful" }, { "love", "tender" } };
    SongVisualBrief b;
    b.source = "local";
    const std::vector<std::string> words = LineWords(lyricsText + " " + title);
    int christmas = 0;
    std::map<std::string, int> moods;
    for (const std::string& w : words) {
        christmas += CHRISTMAS.contains(w) ? 1 : 0;
        if (const auto c = COLOURS.find(w); c != COLOURS.end() && std::find(b.colors.begin(), b.colors.end(), c->second) == b.colors.end() && b.colors.size() < 4) {
            b.colors.push_back(c->second);
        }
        if (const auto m = MOODS.find(w); m != MOODS.end()) {
            moods[m->second]++;
        }
    }
    b.christmas = words.empty() ? -1 : std::min(1.0, christmas / 4.0);
    int best = 0;
    for (const auto& [m, n] : moods) {
        if (n > best) {
            best = n;
            b.mood = m;
        }
    }
    return b;
}

} // namespace composer
