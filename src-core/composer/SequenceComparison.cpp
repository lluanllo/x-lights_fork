#include "composer/SequenceComparison.h"

#include "composer/ColorSpace.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace composer {

namespace {

using json = nlohmann::json;

// How bright an effect is asked to be, 0-100 (100 when it does not say).
double Brightness(const EffectSpec& e) {
    for (const auto& [k, v] : e.settings) {
        if (k.find("Brightness") != std::string::npos || k.find("_Intensity") != std::string::npos) {
            return std::strtod(v.c_str(), nullptr);
        }
    }
    for (const std::string& part : { std::string("C_SLIDER_Brightness=") }) {
        const size_t at = e.palette.find(part);
        if (at != std::string::npos) {
            return std::strtod(e.palette.c_str() + at + part.size(), nullptr);
        }
    }
    return 100;
}

// Different hues among colours: families 40 degrees apart or more, white and grey apart.
int HueGroups(const std::vector<std::string>& colors) {
    std::vector<double> hues;
    bool neutral = false;
    for (const std::string& c : colors) {
        const Oklch k = OklchOfHex(c);
        if (k.C < NEUTRAL_CHROMA) {
            neutral = true;
            continue;
        }
        bool near = false;
        for (double h : hues) {
            near |= HueDistance(h, k.h) < 40;
        }
        if (!near) {
            hues.push_back(k.h);
        }
    }
    return static_cast<int>(hues.size()) + (neutral ? 1 : 0);
}

std::string Join(const std::vector<std::string>& v) {
    std::string out;
    for (const std::string& s : v) {
        out += (out.empty() ? "" : " ") + s;
    }
    return out;
}

} // namespace

std::vector<std::string> PaletteColors(const std::string& palette) {
    std::map<int, std::string> slots;
    std::set<int> active;
    size_t at = 0;
    while (at < palette.size()) {
        const size_t end = std::min(palette.find(',', at), palette.size());
        const std::string item = palette.substr(at, end - at);
        at = end + 1;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        const std::string key = item.substr(0, eq);
        const std::string value = item.substr(eq + 1);
        if (key.rfind("C_BUTTON_Palette", 0) == 0) {
            const int slot = static_cast<int>(std::strtol(key.c_str() + 16, nullptr, 10));
            std::string hex = value;
            if (value.rfind("Active=", 0) == 0) {
                const size_t c = value.find("^c=");
                hex = c == std::string::npos ? std::string() : value.substr(c + 3, 7);
            }
            Rgb rgb;
            if (ParseHex(hex, rgb)) {
                slots[slot] = ToHex(rgb);
            }
        } else if (key.rfind("C_CHECKBOX_Palette", 0) == 0 && value == "1") {
            active.insert(static_cast<int>(std::strtol(key.c_str() + 18, nullptr, 10)));
        }
    }
    std::vector<std::string> out;
    for (const auto& [slot, hex] : slots) {
        if (active.contains(slot)) {
            out.push_back(hex);
        }
    }
    return out;
}

SequenceComparison CompareSequences(const SequenceSpec& generated, const SequenceSpec& corrected, const ChromaticScore* score) {
    SequenceComparison c;
    c.generatedEffects = static_cast<int>(generated.effects.size());
    c.correctedEffects = static_cast<int>(corrected.effects.size());
    if (generated.effects.empty() && corrected.effects.empty()) {
        c.problem = "nothing-to-compare";
        return c;
    }
    const auto sceneAt = [&](int ms) -> std::string {
        if (score == nullptr) {
            return "";
        }
        const ChromaticScenePlan* s = score->SceneAt(ms);
        return s == nullptr ? std::string() : s->id;
    };
    // Effects of the same figure and layer that overlap most are the same decision.
    std::vector<char> usedAfter(corrected.effects.size(), 0);
    std::map<std::string, int> keptPerFigure, generatedPerFigure, correctedPerFigure;
    std::map<std::string, int> maxLayerBefore;
    for (const EffectSpec& e : generated.effects) {
        generatedPerFigure[e.target]++;
        maxLayerBefore[e.target] = std::max(maxLayerBefore[e.target], e.layer);
    }
    for (const EffectSpec& e : corrected.effects) {
        correctedPerFigure[e.target]++;
    }
    std::map<std::string, std::vector<int>> sceneGroupsBefore, sceneGroupsAfter;
    for (const EffectSpec& g : generated.effects) {
        sceneGroupsBefore[sceneAt((g.startMS + g.endMS) / 2)].push_back(HueGroups(PaletteColors(g.palette)));
        size_t best = corrected.effects.size();
        int overlap = 0;
        for (size_t i = 0; i < corrected.effects.size(); ++i) {
            const EffectSpec& k = corrected.effects[i];
            if (usedAfter[i] || k.target != g.target || k.layer != g.layer) {
                continue;
            }
            const int o = std::min(g.endMS, k.endMS) - std::max(g.startMS, k.startMS);
            if (o > overlap) {
                overlap = o;
                best = i;
            }
        }
        if (best == corrected.effects.size() || overlap * 2 < std::min(g.endMS - g.startMS, corrected.effects[best].endMS - corrected.effects[best].startMS)) {
            continue; // reported below as removed
        }
        usedAfter[best] = 1;
        ++c.matched;
        keptPerFigure[g.target]++;
        const EffectSpec& k = corrected.effects[best];
        const std::string scene = sceneAt((g.startMS + g.endMS) / 2);
        if (k.effect != g.effect) {
            c.changes.push_back({ "effect", g.target, g.layer, k.startMS, k.endMS, scene, g.effect, k.effect, "", 0, "the effect was replaced" });
        }
        const std::vector<std::string> before = PaletteColors(g.palette);
        const std::vector<std::string> after = PaletteColors(k.palette);
        if (before != after && !after.empty()) {
            const int gb = HueGroups(before), ga = HueGroups(after);
            SequenceChange ch { "colour", g.target, g.layer, k.startMS, k.endMS, scene, Join(before), Join(after), "", 0, "" };
            if (ga < gb) {
                ch.suggestion = "color.cohesion";
                ch.sign = 1;
                ch.why = fmt::format("fewer different hues on this effect ({} instead of {})", ga, gb);
            } else if (ga > gb) {
                ch.suggestion = "color.cohesion";
                ch.sign = -1;
                ch.why = fmt::format("more different hues on this effect ({} instead of {})", ga, gb);
            } else {
                ch.why = "other colours, as many hues";
            }
            c.changes.push_back(ch);
        }
        const double bb = Brightness(g), ba = Brightness(k);
        if (std::fabs(ba - bb) >= 10) {
            c.changes.push_back({ "intensity", g.target, g.layer, k.startMS, k.endMS, scene, fmt::format("{:.0f}", bb), fmt::format("{:.0f}", ba), fmt::format("figure.{}.prominence", g.target),
                                  ba > bb ? 1.0 : -1.0, ba > bb ? "brighter" : "dimmer" });
        }
        if (std::abs(k.startMS - g.startMS) > 100 || std::abs(k.endMS - g.endMS) > 100) {
            c.changes.push_back({ "timing", g.target, g.layer, k.startMS, k.endMS, scene, fmt::format("{}-{}", g.startMS, g.endMS), fmt::format("{}-{}", k.startMS, k.endMS), "", 0,
                                  "moved or resized" });
        }
    }
    // What was taken out and what was added.
    for (const EffectSpec& g : generated.effects) {
        bool matched = false;
        for (size_t i = 0; i < corrected.effects.size() && !matched; ++i) {
            const EffectSpec& k = corrected.effects[i];
            matched = usedAfter[i] && k.target == g.target && k.layer == g.layer && std::min(g.endMS, k.endMS) - std::max(g.startMS, k.startMS) > 0;
        }
        if (!matched) {
            const bool figureGone = !correctedPerFigure.contains(g.target);
            c.changes.push_back({ figureGone ? "figure-removed" : "layer-removed", g.target, g.layer, g.startMS, g.endMS, sceneAt((g.startMS + g.endMS) / 2), g.effect, "",
                                  fmt::format("figure.{}.prominence", g.target), -1, figureGone ? "the figure was left dark" : "this effect was taken out" });
        }
    }
    for (size_t i = 0; i < corrected.effects.size(); ++i) {
        if (usedAfter[i]) {
            continue;
        }
        const EffectSpec& k = corrected.effects[i];
        sceneGroupsAfter[sceneAt((k.startMS + k.endMS) / 2)].push_back(HueGroups(PaletteColors(k.palette)));
        const bool figureNew = !generatedPerFigure.contains(k.target);
        const bool layerNew = !figureNew && k.layer > maxLayerBefore[k.target];
        c.changes.push_back({ figureNew ? "figure-added" : layerNew ? "layer-added" : "effect", k.target, k.layer, k.startMS, k.endMS, sceneAt((k.startMS + k.endMS) / 2), "", k.effect,
                              fmt::format("figure.{}.prominence", k.target), 1, figureNew ? "a figure the show left out was brought in" : layerNew ? "a layer was added on top" : "an effect was added" });
    }
    for (size_t i = 0; i < corrected.effects.size(); ++i) {
        if (usedAfter[i]) {
            const EffectSpec& k = corrected.effects[i];
            sceneGroupsAfter[sceneAt((k.startMS + k.endMS) / 2)].push_back(HueGroups(PaletteColors(k.palette)));
        }
    }
    const auto mean = [](const std::map<std::string, std::vector<int>>& m) {
        double sum = 0;
        int n = 0;
        for (const auto& [scene, groups] : m) {
            for (int g : groups) {
                sum += g;
                ++n;
            }
        }
        return n > 0 ? std::round(sum / n * 100.0) / 100.0 : 0.0;
    };
    c.familiesBefore = mean(sceneGroupsBefore);
    c.familiesAfter = mean(sceneGroupsAfter);
    std::stable_sort(c.changes.begin(), c.changes.end(), [](const SequenceChange& a, const SequenceChange& b) { return a.startMS < b.startMS; });
    c.ran = true;
    return c;
}

int LearnFromComparison(ArtisticPreferences& prefs, const SequenceComparison& comparison, const std::vector<size_t>& picked, const std::string& sequence) {
    // The same show as its review: a correction and an evaluation of one show count as one
    // show, so a taste still needs two shows.
    const std::string prefix = sequence + "|corrected:";
    std::erase_if(prefs.signals, [&](const PreferenceSignal& s) { return s.origin.rfind(prefix, 0) == 0; });
    int session = prefs.session + 1;
    for (const PreferenceSignal& s : prefs.signals) {
        if (s.origin.rfind(sequence + "|", 0) == 0) {
            session = s.session;
        }
    }
    int added = 0;
    for (size_t i : picked) {
        if (i >= comparison.changes.size()) {
            continue;
        }
        const SequenceChange& ch = comparison.changes[i];
        if (ch.suggestion.empty() || ch.sign == 0) {
            continue;
        }
        prefs.signals.push_back({ ch.suggestion, ch.sign, 0.5, session, prefix + std::to_string(i), ch.kind });
        ++added;
    }
    if (added > 0) {
        prefs.session = std::max(prefs.session, session);
    }
    return added;
}

std::string ToJson(const SequenceComparison& c) {
    json changes = json::array();
    for (const SequenceChange& ch : c.changes) {
        changes.push_back({ { "kind", ch.kind }, { "figure", ch.figure }, { "layer", ch.layer }, { "startMS", ch.startMS }, { "endMS", ch.endMS }, { "scene", ch.scene },
                            { "before", ch.before }, { "after", ch.after }, { "suggestion", ch.suggestion }, { "sign", ch.sign }, { "why", ch.why } });
    }
    return json({ { "kind", "composer-sequence-comparison" }, { "schemaVersion", c.schemaVersion }, { "ran", c.ran }, { "problem", c.problem },
                  { "generatedEffects", c.generatedEffects }, { "correctedEffects", c.correctedEffects }, { "matched", c.matched }, { "familiesBefore", c.familiesBefore },
                  { "familiesAfter", c.familiesAfter }, { "changes", changes } })
        .dump(1);
}

bool FromJson(const std::string& text, SequenceComparison& c, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("kind", std::string()) != "composer-sequence-comparison") {
        error = "not a comparison of sequences";
        return false;
    }
    c = SequenceComparison();
    c.ran = root.value("ran", false);
    c.problem = root.value("problem", std::string());
    c.generatedEffects = root.value("generatedEffects", 0);
    c.correctedEffects = root.value("correctedEffects", 0);
    c.matched = root.value("matched", 0);
    c.familiesBefore = root.value("familiesBefore", 0.0);
    c.familiesAfter = root.value("familiesAfter", 0.0);
    for (const json& j : root.value("changes", json::array())) {
        c.changes.push_back({ j.value("kind", std::string()), j.value("figure", std::string()), j.value("layer", 0), j.value("startMS", 0), j.value("endMS", 0), j.value("scene", std::string()),
                              j.value("before", std::string()), j.value("after", std::string()), j.value("suggestion", std::string()), j.value("sign", 0.0), j.value("why", std::string()) });
    }
    return true;
}

} // namespace composer
