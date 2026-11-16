#include "composer/SequenceComparison.h"

#include "composer/ColorSpace.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
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
    // What kind of correction each looks like: a nudge in time is technical, a figure left
    // dark is something that should not have lit, the rest is a matter of taste. A gesture
    // (Stage Field) added or taken out is said as such and teaches nothing about the figure.
    for (SequenceChange& ch : c.changes) {
        const bool gesture = ch.before == "Stage Field" || ch.after == "Stage Field";
        if (gesture && (ch.kind == "layer-removed" || ch.kind == "figure-removed")) {
            ch.kind = "gesture-removed";
            ch.suggestion.clear();
            ch.sign = 0;
            ch.why = "a gesture was taken out";
        } else if (gesture && (ch.kind == "layer-added" || ch.kind == "figure-added" || (ch.kind == "effect" && ch.before.empty()))) {
            ch.kind = "gesture-added";
            ch.suggestion.clear();
            ch.sign = 0;
            ch.why = "a gesture was added";
        }
        ch.category = ch.kind == "timing" ? "technical" : ch.kind == "figure-removed" ? "necessary" : "aesthetic";
        if (score != nullptr) {
            if (const ChromaticScenePlan* s = score->SceneAt((ch.startMS + ch.endMS) / 2); s != nullptr) {
                ch.sceneStartMS = s->startMS;
            }
        }
    }
    c.ran = true;
    return c;
}

std::string ChangeKey(const SequenceChange& ch) {
    return fmt::format("{}|{}|{}|{}|{}|{}", ch.kind, ch.figure, ch.layer, ch.startMS, ch.before, ch.after);
}

std::vector<size_t> OfferedChanges(const SequenceComparison& comparison, const ComparisonDecisions& decisions) {
    const std::set<std::string> rejected(decisions.rejected.begin(), decisions.rejected.end());
    std::vector<size_t> out;
    for (size_t i = 0; i < comparison.changes.size(); ++i) {
        if (!rejected.contains(ChangeKey(comparison.changes[i]))) {
            out.push_back(i);
        }
    }
    return out;
}

DecisionOutcome ApplyComparisonDecisions(ArtisticPreferences& prefs, StageEdits& stage, ChromaticEdits& colour, ComparisonDecisions& record, const SequenceComparison& comparison,
                                         const std::vector<ComparisonDecision>& decisions, const std::string& sequence) {
    DecisionOutcome out;
    record.sequence = sequence;
    std::vector<size_t> learn;
    const std::map<std::string, double> effective = EffectivePreferences(prefs);
    for (const ComparisonDecision& d : decisions) {
        if (d.change >= comparison.changes.size()) {
            continue;
        }
        const SequenceChange& ch = comparison.changes[d.change];
        if (d.choice == "learn") {
            learn.push_back(d.change);
        } else if (d.choice == "general") {
            if (ch.suggestion.empty() || ch.sign == 0) {
                out.notes.push_back(fmt::format("{} at {:.1f} s: it says nothing for other songs", ch.kind, ch.startMS / 1000.0));
                continue;
            }
            const auto it = effective.find(ch.suggestion);
            const double now = it == effective.end() ? 0 : it->second;
            SetPreference(prefs, ch.suggestion, std::clamp(now + 0.3 * ch.sign, -1.0, 1.0));
            ++out.general;
        } else if (d.choice == "song") {
            // Only this song: what the director can be told for it.
            if (ch.kind == "figure-removed" || ch.kind == "figure-added") {
                if (ch.sceneStartMS < 0) {
                    out.notes.push_back(fmt::format("{} at {:.1f} s: its scene is not known", ch.figure, ch.startMS / 1000.0));
                    continue;
                }
                stage.edits.push_back({ ch.kind == "figure-removed" ? "exclude" : "include", ch.sceneStartMS, -1, ch.figure, "from a corrected version" });
                ++out.songEdits;
            } else if (ch.kind == "gesture-removed") {
                if (ch.sceneStartMS < 0) {
                    out.notes.push_back(fmt::format("the gesture at {:.1f} s: its scene is not known", ch.startMS / 1000.0));
                    continue;
                }
                stage.edits.push_back({ "no-gestures", ch.sceneStartMS, -1, "", "from a corrected version" });
                ++out.songEdits;
            } else if (ch.kind == "colour" && !ch.after.empty()) {
                std::string value = ch.after;
                std::replace(value.begin(), value.end(), ' ', ',');
                colour.edits.push_back({ "figure-color", ch.sceneStartMS, ch.figure, "", value, "from a corrected version" });
                ++out.songEdits;
            } else {
                out.notes.push_back(fmt::format("{} at {:.1f} s: the corrected version keeps it; the composer has no way to ask for it in this song", ch.kind, ch.startMS / 1000.0));
            }
        } else if (d.choice == "reject") {
            const std::string key = ChangeKey(ch);
            if (std::find(record.rejected.begin(), record.rejected.end(), key) == record.rejected.end()) {
                record.rejected.push_back(key);
            }
            ++out.rejected;
        } else {
            ++out.ignored;
        }
    }
    out.learned = learn.empty() ? 0 : LearnFromComparison(prefs, comparison, learn, sequence);
    record.decisions = decisions;
    return out;
}

std::vector<StageLibraryEntry> ProposeCompositions(const SequenceSpec& generated, const SequenceSpec& corrected, const ChromaticScore* score, const std::string& sequence) {
    // An effect of the corrected show that the generated one did not have at that place.
    const auto had = [&](const EffectSpec& k) {
        for (const EffectSpec& g : generated.effects) {
            if (g.target == k.target && g.effect == k.effect && std::min(g.endMS, k.endMS) - std::max(g.startMS, k.startMS) > (k.endMS - k.startMS) / 2) {
                return true;
            }
        }
        return false;
    };
    std::vector<const EffectSpec*> added;
    for (const EffectSpec& k : corrected.effects) {
        if (k.effect != "Faces" && k.effect != "Text" && !had(k)) {
            added.push_back(&k);
        }
    }
    std::stable_sort(added.begin(), added.end(), [](const EffectSpec* a, const EffectSpec* b) { return a->startMS < b->startMS; });
    std::vector<StageLibraryEntry> out;
    std::vector<char> taken(added.size(), 0);
    for (size_t i = 0; i < added.size(); ++i) {
        if (taken[i]) {
            continue;
        }
        // The same effect started on other figures within a second: one idea.
        std::vector<size_t> group = { i };
        std::set<std::string> figures = { added[i]->target };
        for (size_t j = i + 1; j < added.size() && added[j]->startMS - added[i]->startMS <= 1000; ++j) {
            if (!taken[j] && added[j]->effect == added[i]->effect && !figures.contains(added[j]->target)) {
                group.push_back(j);
                figures.insert(added[j]->target);
            }
        }
        if (figures.size() < 3) {
            continue;
        }
        int spread = 0;
        for (size_t k : group) {
            taken[k] = 1;
            spread = std::max(spread, added[k]->startMS - added[i]->startMS);
        }
        StageLibraryEntry e;
        e.id = fmt::format("composition:learned-{}-{}", added[i]->effect, added[i]->startMS);
        std::replace(e.id.begin(), e.id.end(), ' ', '-');
        e.kind = "composition";
        e.custom = true;
        e.source = "learned:" + sequence;
        e.level = figures.size() >= 6 ? "global" : "group";
        e.title = fmt::format("{} on {} figures", added[i]->effect, figures.size());
        e.description = fmt::format("Made by a person in the corrected show at {:.1f} s", added[i]->startMS / 1000.0);
        e.effects = { added[i]->effect };
        e.figures.assign(figures.begin(), figures.end());
        e.minFigures = static_cast<int>(figures.size());
        e.maxFigures = static_cast<int>(figures.size());
        e.temporal = spread <= 100 ? "unison" : "sequence";
        e.beats = 0;
        if (score != nullptr) {
            if (const ChromaticScenePlan* s = score->SceneAt(added[i]->startMS); s != nullptr && !s->sectionKind.empty()) {
                e.contexts = { s->sectionKind };
            }
        }
        e.palette = PaletteColors(added[i]->palette);
        out.push_back(std::move(e));
    }
    return out;
}

std::string ToJson(const ComparisonDecisions& d) {
    json list = json::array();
    for (const ComparisonDecision& x : d.decisions) {
        list.push_back({ { "change", x.change }, { "choice", x.choice } });
    }
    return json({ { "kind", "composer-comparison-decisions" }, { "schemaVersion", d.schemaVersion }, { "sequence", d.sequence }, { "decisions", list }, { "rejected", d.rejected } }).dump(1);
}

bool FromJson(const std::string& text, ComparisonDecisions& d, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("kind", std::string()) != "composer-comparison-decisions") {
        error = "not a file of decisions about a corrected version";
        return false;
    }
    d = ComparisonDecisions();
    d.sequence = root.value("sequence", std::string());
    for (const json& x : root.value("decisions", json::array())) {
        d.decisions.push_back({ x.value("change", static_cast<size_t>(0)), x.value("choice", std::string()) });
    }
    d.rejected = root.value("rejected", std::vector<std::string>());
    return true;
}

std::string ComparisonDecisionsPath(const std::string& showDir, const std::string& sequence) {
    return (std::filesystem::path(showDir) / "ShowComposer" / "corrections" / (std::filesystem::path(sequence).stem().string() + ".decisions.json")).string();
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
                            { "before", ch.before }, { "after", ch.after }, { "suggestion", ch.suggestion }, { "sign", ch.sign }, { "why", ch.why }, { "category", ch.category },
                            { "sceneStartMS", ch.sceneStartMS } });
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
                              j.value("before", std::string()), j.value("after", std::string()), j.value("suggestion", std::string()), j.value("sign", 0.0), j.value("why", std::string()),
                              j.value("category", std::string()), j.value("sceneStartMS", -1) });
    }
    return true;
}

} // namespace composer
