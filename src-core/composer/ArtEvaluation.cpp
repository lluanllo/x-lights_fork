#include "composer/ArtEvaluation.h"

#include "composer/ColorSpace.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>

namespace composer {

namespace {

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

} // namespace

const CompositionSign* CompositionEvaluation::Sign(const std::string& id) const {
    for (const CompositionSign& s : signs) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

CompositionEvaluation EvaluateComposition(const ColorCheck& c, const ChromaticScore* score) {
    CompositionEvaluation e;
    if (!c.ran) {
        return e;
    }
    e.ran = true;
    const auto add = [&](const char* id, double value, const std::string& reading, const std::string& why) { e.signs.push_back({ id, Round3(value), reading, why }); };

    const double cohesion = 1.0 - c.hueSpread;
    add("cohesion", cohesion, cohesion >= 0.8 ? "cohesive" : cohesion >= 0.6 ? "related" : "scattered",
        fmt::format("the coloured light at any moment points to {} hue (spread {:.2f}; 0 one hue, 1 all round the circle)", cohesion >= 0.8 ? "one" : cohesion >= 0.6 ? "a few related" : "many different", c.hueSpread));
    add("variety", c.sceneVariation, c.sceneVariation < 0.15 ? "repetitive" : c.sceneVariation <= 0.45 ? "evolving" : "restless",
        fmt::format("from one scene to the next the colour mix changes {:.0f}%", c.sceneVariation * 100));
    add("contrast", c.figureDifference, c.figureDifference < 12 ? "flat" : c.figureDifference <= 28 ? "clear" : "harsh",
        fmt::format("the lit figures differ by {:.1f} dE on average", c.figureDifference));
    add("hierarchy", c.topTwoShare, c.topTwoShare >= 0.75 ? "clear lead" : c.topTwoShare >= 0.6 ? "shared" : "no lead",
        fmt::format("the two largest families hold {:.0f}% of the light at once", c.topTwoShare * 100));
    add("distribution", c.colorsPerFigure, c.colorsPerFigure <= 1.8 ? "one idea per figure" : c.colorsPerFigure <= 2.4 ? "some busy figures" : "busy figures",
        fmt::format("a lit figure wears {:.1f} families of colour at once", c.colorsPerFigure));
    add("overload", c.overloadedScenes, c.overloadedScenes == 0 ? "none" : c.overloadedScenes <= 3 ? "a few scenes" : "many scenes",
        fmt::format("{} scenes with more than four families sharing the house at once", c.overloadedScenes));
    if (!c.gestures.empty()) {
        add("gestures", c.gestureHouseContrast, c.gestureHouseContrast < 10 ? "blend in" : c.gestureHouseContrast <= 25 ? "read" : "jump out",
            fmt::format("the gestures differ from the rest of the house by {:.1f} dE (median)", c.gestureHouseContrast));
    }
    if (score != nullptr && score->enabled && !score->scenes.empty()) {
        int music = 0, continuing = 0;
        for (const ChromaticScenePlan& s : score->scenes) {
            if (s.motifOperation == "rest") {
                continue;
            }
            ++music;
            continuing += s.motifOperation == "develop" || s.motifOperation == "recall" || s.motifOperation == "intensify" || s.motifOperation == "resolve" || s.motifOperation == "hold" ? 1 : 0;
        }
        const double continuity = music > 0 ? static_cast<double>(continuing) / music : 0;
        add("continuity", continuity, continuity >= 0.75 ? "a steady thread" : continuity >= 0.45 ? "a thread with turns" : "many new ideas",
            fmt::format("{} of {} musical scenes keep, recall, develop or resolve a colour idea heard before", continuing, music));
        int recalled = 0;
        for (const ChromaticMotif& m : score->motifDefs) {
            recalled += m.scenes.size() > 1 ? 1 : 0;
        }
        add("motifs", static_cast<double>(score->motifDefs.size()), recalled > 0 ? "recurring" : score->motifDefs.empty() ? "none" : "not recalled",
            fmt::format("{} colour ideas, {} of them coming back", score->motifDefs.size(), recalled));
        // The song's identity: how much of the light is in the family of its first motif.
        const std::string anchor = score->motifDefs.empty() ? score->song.Hex("anchor") : score->motifDefs.front().anchor;
        const std::string family = ColorFamilyId(ColorFamily(OklchOfHex(anchor)));
        double share = 0;
        for (const ColorShare& f : c.families) {
            share += f.family == family ? f.share : 0;
        }
        add("identity", share, share >= 0.4 ? "present" : share >= 0.2 ? "shared" : "lost",
            fmt::format("{:.0f}% of the light is in the family of the song's identity ({})", share * 100, family));
    }
    return e;
}

std::string ToJson(const CompositionEvaluation& e) {
    nlohmann::json signs = nlohmann::json::array();
    for (const CompositionSign& s : e.signs) {
        signs.push_back({ { "id", s.id }, { "value", s.value }, { "reading", s.reading }, { "why", s.why } });
    }
    return nlohmann::json({ { "kind", "composer-composition-evaluation" }, { "schemaVersion", e.schemaVersion }, { "ran", e.ran }, { "signs", signs },
                            { "score", nullptr }, { "review", "not-reviewed" } })
        .dump(1);
}

std::string CompositionReport(const CompositionEvaluation& e) {
    if (!e.ran) {
        return "composition not read: no measurement of the render\n";
    }
    std::string out = "Signs of the composition (measurements, never a grade):\n";
    for (const CompositionSign& s : e.signs) {
        out += fmt::format("  {:<12} {:<20} {}\n", s.id, s.reading, s.why);
    }
    return out;
}

} // namespace composer
