#include "composer/StagePresence.h"

#include "composer/FigureGeometry.h"
#include "composer/FigureSemantics.h"
#include "composer/ModelCapabilities.h"
#include "composer/StageSpace.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#define COMPOSER_TEXT(text) text

namespace composer {

namespace {

int Rank(const std::string& state) {
    return state == "protected" ? 6 : state == "protagonist" ? 5 : state == "complement" ? 4 : state == "response" ? 3 : state == "ambient" ? 2 : 0;
}

bool Lit(const std::string& policy) {
    return policy == "normal" || policy == "reduced" || policy == "protected";
}

// An effect the score never touches: a face singing, words, a picture or a video, or one a
// person locked or edited.
bool Protected(const ChoreographyEvent& e) {
    return e.intent == "face-sing" || e.intent == "lyric-text" || e.intent == "media" || e.effect == "Faces" || e.effect == "Text" || e.effect == "Pictures" || e.effect == "Video" ||
           e.state == DecisionState::Locked || e.state == DecisionState::ManualEdit || e.provenance.rfind("stage:", 0) == 0;
}

std::string Base(const std::string& target) {
    const size_t slash = target.find('/');
    return slash == std::string::npos ? target : target.substr(0, slash);
}

void SetSetting(ChoreographyEvent& e, const std::string& key, const std::string& value) {
    for (auto& [k, v] : e.settings) {
        if (k == key) {
            v = value;
            return;
        }
    }
    e.settings.emplace_back(key, value);
}

std::string Setting(const ChoreographyEvent& e, const std::string& key) {
    for (const auto& [k, v] : e.settings) {
        if (k == key) {
            return v;
        }
    }
    return {};
}

// A group's own way of laying an effect over its members means nothing on one model.
void DropGroupOnlySettings(ChoreographyEvent& e) {
    std::erase_if(e.settings, [](const std::pair<std::string, std::string>& kv) {
        if (kv.first != "B_CHOICE_BufferStyle") {
            return false;
        }
        const std::string& v = kv.second;
        return v.find("Per Model") != std::string::npos || v.find("Overlay") != std::string::npos || v.find("Stack") != std::string::npos || v.find("Single Line") != std::string::npos;
    });
}

// The figures a target's light reaches: itself (a submodel's parent), or a group's members.
std::vector<std::string> Reaches(const ChoreographyEvent& e, const ModelCapabilities* caps) {
    if (caps != nullptr) {
        if (const TargetInfo* t = caps->Find(e.target); t != nullptr && t->type == TargetType::Group) {
            return t->flatModels;
        }
    }
    return { Base(e.target) };
}

struct Scene {
    int from = 0, to = 0;
    std::map<std::string, const FigurePresence*> presence;
};

} // namespace

std::vector<SharedLights> FindSharedLights(const ModelCapabilities& caps, const std::vector<std::string>& figures, const FigureCatalog* geometry) {
    std::vector<SharedLights> out;
    std::vector<const TargetInfo*> ts;
    for (const std::string& f : figures) {
        const TargetInfo* t = caps.Find(f);
        if (t != nullptr && t->type == TargetType::Model && t->lastChannel >= t->firstChannel && t->lastChannel > 0) {
            ts.push_back(t);
        }
    }
    std::stable_sort(ts.begin(), ts.end(), [](const TargetInfo* a, const TargetInfo* b) { return a->name < b->name; });
    for (size_t i = 0; i < ts.size(); ++i) {
        for (size_t j = i + 1; j < ts.size(); ++j) {
            const TargetInfo* a = ts[i];
            const TargetInfo* b = ts[j];
            if (a->firstChannel > b->lastChannel || b->firstChannel > a->lastChannel) {
                continue;
            }
            const uint32_t lo = std::max(a->firstChannel, b->firstChannel);
            const uint32_t hi = std::min(a->lastChannel, b->lastChannel);
            SharedLights s;
            s.a = a->name;
            s.b = b->name;
            const int per = std::max(1, std::max(a->channelsPerNode, b->channelsPerNode));
            s.lights = static_cast<int>((hi - lo + 1 + per - 1) / per);
            const bool aInB = a->firstChannel >= b->firstChannel && a->lastChannel <= b->lastChannel;
            const bool bInA = b->firstChannel >= a->firstChannel && b->lastChannel <= a->lastChannel;
            s.relation = aInB && bInA ? "same" : aInB ? "a-inside-b" : bInA ? "b-inside-a" : "partial";
            s.note = s.relation == "partial" ? COMPOSER_TEXT("part of their lights are wired to both: check the layout") : COMPOSER_TEXT("one is drawn inside the other on the same lights");
            s.channels = fmt::format("{}-{}", lo + 1, hi + 1);
            s.controllerA = a->controller;
            s.controllerB = b->controller;
            s.lightsA = a->nodes;
            s.lightsB = b->nodes;
            s.kind = s.relation == "same" ? "duplicate" : s.relation == "partial" ? "partial" : "containment";
            s.place = "unknown";
            const FigureGeometry* ga = geometry != nullptr ? geometry->Find(a->name) : nullptr;
            const FigureGeometry* gb = geometry != nullptr ? geometry->Find(b->name) : nullptr;
            if (ga != nullptr && gb != nullptr) {
                // Light by light: a shared light is the one channel two figures' lights both start on.
                std::map<int, const FigureNode*> byChannelB;
                for (const FigureNode& n : gb->nodeMap) {
                    if (n.channel >= 0) {
                        byChannelB[n.channel] = &n;
                    }
                }
                std::vector<int> na, nb;
                std::vector<double> distances;
                for (const FigureNode& n : ga->nodeMap) {
                    if (n.channel < 0 || static_cast<uint32_t>(n.channel) < lo || static_cast<uint32_t>(n.channel) > hi) {
                        continue;
                    }
                    na.push_back(n.index);
                    const auto m = byChannelB.find(n.channel);
                    if (m != byChannelB.end() && n.placed && m->second->placed) {
                        distances.push_back(std::hypot(n.wx - m->second->wx, n.wy - m->second->wy));
                    }
                }
                for (const FigureNode& n : gb->nodeMap) {
                    if (n.channel >= 0 && static_cast<uint32_t>(n.channel) >= lo && static_cast<uint32_t>(n.channel) <= hi) {
                        nb.push_back(n.index);
                    }
                }
                std::sort(na.begin(), na.end());
                std::sort(nb.begin(), nb.end());
                s.nodesA = NodeRangesText(na);
                s.nodesB = NodeRangesText(nb);
                s.exclusiveA = std::max(0, static_cast<int>(ga->nodeMap.size()) - static_cast<int>(na.size()));
                s.exclusiveB = std::max(0, static_cast<int>(gb->nodeMap.size()) - static_cast<int>(nb.size()));
                // How far apart neighbouring lights of a figure are: the measure of "the same place".
                const auto spacing = [](const FigureGeometry& g) {
                    std::vector<double> d;
                    for (size_t k = 1; k < g.nodeMap.size(); ++k) {
                        const FigureNode& p = g.nodeMap[k - 1];
                        const FigureNode& q = g.nodeMap[k];
                        if (p.placed && q.placed) {
                            d.push_back(std::hypot(p.wx - q.wx, p.wy - q.wy));
                        }
                    }
                    if (d.empty()) {
                        return 0.0;
                    }
                    std::nth_element(d.begin(), d.begin() + static_cast<long>(d.size() / 2), d.end());
                    return d[d.size() / 2];
                };
                const double step = std::max(1e-6, std::max(spacing(*ga), spacing(*gb)));
                if (!distances.empty()) {
                    std::nth_element(distances.begin(), distances.begin() + static_cast<long>(distances.size() / 2), distances.end());
                    s.apart = distances[distances.size() / 2] / step;
                    s.place = s.apart <= 2.0 ? "same" : "apart";
                }
                if (s.place == "apart") {
                    s.kind = "misconfigured"; // the same lights drawn at two places: one of the two is likely wired wrong
                    s.note = COMPOSER_TEXT("the same channels drawn in two different places: probably a wiring or start-channel mistake");
                } else if (s.place == "unknown" && s.kind == "partial") {
                    s.kind = "unknown";
                }
            }
            out.push_back(std::move(s));
        }
    }
    return out;
}

std::vector<std::string> ApplyStagePresence(GlobalStageScore& score, const StageDirectorInput& in, Choreography& c) {
    std::vector<std::string> notes;
    const int frame = std::max(1, c.frameMS);
    const auto near = [&](int ms) { return std::max(0, ((ms + frame / 2) / frame) * frame); };
    std::set<std::string> figures;
    for (const StageScene& sc : score.scenes) {
        for (const FigurePresence& p : sc.presence) {
            figures.insert(p.figure);
        }
    }
    if (figures.empty()) {
        return notes;
    }
    // Figures wired to the same lights must not be told different things on them.
    score.sharedLights.clear();
    const bool lightByLight = in.groupPresence >= 0 ? in.groupPresence == 1 : score.art >= static_cast<int>(ArtLevel::Calibrated);
    if (in.capabilities != nullptr) {
        score.sharedLights = FindSharedLights(*in.capabilities, std::vector<std::string>(figures.begin(), figures.end()), lightByLight ? in.geometry : nullptr);
    }
    std::map<std::pair<std::string, std::string>, bool> chosen;                  // (winner, loser) a person picked for the song
    std::map<std::pair<std::string, std::string>, std::vector<int>> chosenHere; // (winner, loser) -> the scenes' starts where a person picked it
    std::set<std::pair<std::string, std::string>> confirmed;                     // pairs a person said share lights on purpose
    if (in.edits != nullptr) {
        for (const StageEdit& e : in.edits->edits) {
            const size_t bar = e.value.find('|');
            if (bar == std::string::npos) {
                continue;
            }
            const std::pair<std::string, std::string> pair = { e.value.substr(0, bar), e.value.substr(bar + 1) };
            if (e.action == "priority" && e.sceneStartMS < 0) {
                chosen[pair] = true;
                chosen.erase({ pair.second, pair.first });
            } else if (e.action == "priority") {
                chosenHere[pair].push_back(e.sceneStartMS);
            } else if (e.action == "shared-ok") {
                confirmed.insert(pair);
                confirmed.insert({ pair.second, pair.first });
            }
        }
    }
    // Light by light: the figure that gives way keeps its own lights and is masked only on the
    // ones it shares (its row drawn before the other's, so the other's light shows there).
    struct Owned {
        std::string figure;
        int from = 0, to = 0;
        std::string nodes;
    };
    std::vector<Owned> owned;
    const auto protectedIn = [&](const std::string& f, int a, int b) {
        for (const ChoreographyEvent& e : c.events) {
            if ((e.effect == "Faces" || e.effect == "Text" || e.intent == "face-sing" || e.intent == "lyric-text" || e.state == DecisionState::Locked || e.state == DecisionState::ManualEdit) &&
                Base(e.target) == f && e.startMS < b && a < e.endMS) {
                return true;
            }
        }
        return false;
    };
    for (SharedLights& s : score.sharedLights) {
        s.confirmed = confirmed.contains({ s.a, s.b });
        const bool songWide = chosen.contains({ s.a, s.b }) || chosen.contains({ s.b, s.a });
        if (chosen.contains({ s.a, s.b })) {
            s.priority = s.a;
            s.prioritySource = "person";
        } else if (chosen.contains({ s.b, s.a })) {
            s.priority = s.b;
            s.prioritySource = "person";
        } else if (!chosenHere[{ s.a, s.b }].empty() || !chosenHere[{ s.b, s.a }].empty()) {
            // Only some scenes chosen: the song's order follows the choice made most often.
            s.priority = chosenHere[{ s.a, s.b }].size() >= chosenHere[{ s.b, s.a }].size() ? s.a : s.b;
            s.prioritySource = "person";
        }
        const auto pickedHere = [&](const std::string& w, const std::string& l, int start) {
            for (int at : chosenHere[{ w, l }]) {
                if (std::abs(at - start) <= 1500) {
                    return true;
                }
            }
            return false;
        };
        for (StageScene& sc : score.scenes) {
            FigurePresence* pa = nullptr;
            FigurePresence* pb = nullptr;
            for (FigurePresence& p : sc.presence) {
                pa = p.figure == s.a ? &p : pa;
                pb = p.figure == s.b ? &p : pb;
            }
            if (pa == nullptr || pb == nullptr || !Lit(pa->policy) || !Lit(pb->policy)) {
                continue;
            }
            ++s.scenesBothLit;
            FigurePresence* winner = nullptr;
            FigurePresence* loser = nullptr;
            // A choice for some scenes only acts there; elsewhere the director decides as without one.
            if (s.prioritySource == "person" && (songWide || pickedHere(s.a, s.b, sc.startMS) || pickedHere(s.b, s.a, sc.startMS))) {
                winner = s.priority == s.a ? pa : pb;
                if (pickedHere(s.a, s.b, sc.startMS)) {
                    winner = pa;
                } else if (pickedHere(s.b, s.a, sc.startMS)) {
                    winner = pb;
                }
                loser = winner == pa ? pb : pa;
                // Light by light where the song's order allows it (the loser's row is drawn first:
                // it is the song's loser), never over a face or words.
                const std::string loserNodes = loser == pa ? s.nodesA : s.nodesB;
                if (lightByLight && s.relation != "same" && !loserNodes.empty() && winner->figure == s.priority && loser->policy != "protected") {
                    if (protectedIn(loser->figure, sc.startMS, sc.endMS)) {
                        s.note = COMPOSER_TEXT("the priority would cover a face that sings or words: that figure keeps its lights there");
                        continue;
                    }
                    owned.push_back({ loser->figure, sc.startMS, sc.endMS, loserNodes });
                    ++s.scenesOwned;
                    ++s.scenesResolved;
                    continue;
                }
            } else if (s.relation != "partial") {
                // One inside the other: the one the scene asks less of gives way, unless it is the
                // larger and still carries the scene (then both stay, and it is said).
                FigurePresence* inner = s.relation == "a-inside-b" ? pa : pb;
                FigurePresence* outer = inner == pa ? pb : pa;
                if (s.relation == "same") {
                    inner = Rank(pa->state) >= Rank(pb->state) ? pb : pa;
                    outer = inner == pa ? pb : pa;
                }
                if (Rank(inner->state) <= Rank(outer->state)) {
                    winner = outer;
                    loser = inner;
                } else if (outer->policy == "reduced") {
                    winner = inner;
                    loser = outer;
                }
                if (winner != nullptr && s.priority.empty()) {
                    s.priority = winner->figure;
                    s.prioritySource = "presence";
                }
            }
            if (winner != nullptr && loser != nullptr && loser->policy != "protected") {
                loser->policy = "suspended";
                loser->why = "it shares its lights with a figure that carries more here";
                ++s.scenesResolved;
                // Nor does it take part in the scene's gestures: its lights are the other's here.
                for (GlobalGesture& g : score.gestures) {
                    if (std::find(sc.gestures.begin(), sc.gestures.end(), g.id) == sc.gestures.end()) {
                        continue;
                    }
                    const size_t before = g.participants.size();
                    std::erase_if(g.participants, [&](const GestureParticipant& p) { return p.figure == loser->figure; });
                    if (g.participants.size() != before) {
                        g.excluded.push_back(loser->figure);
                    }
                }
            }
        }
        if (s.scenesBothLit > s.scenesResolved && s.relation == "partial" && s.note.find("priority") == std::string::npos && s.kind != "misconfigured") {
            s.note = COMPOSER_TEXT("part of their lights are wired to both and both play at once: the row drawn later shows on those lights");
        }
    }
    // The scenes, on the effects' frames.
    std::vector<Scene> scenes;
    for (const StageScene& sc : score.scenes) {
        Scene s;
        s.from = near(sc.startMS);
        s.to = near(sc.endMS);
        for (const FigurePresence& p : sc.presence) {
            s.presence[p.figure] = &p;
        }
        if (s.to > s.from) {
            scenes.push_back(std::move(s));
        }
    }
    const int glow = std::clamp(static_cast<int>(std::lround(score.ambientLevel * 100)), 5, 100);
    // Phase 22: a group effect over figures asked different things stays whole on the group, and
    // each figure that rests or glows is masked on its own row (xLights' "1 is Mask" and
    // "Brightness" layer methods over what the rows before it drew, with model blending on).
    const bool groupMask = in.groupPresence >= 0 ? in.groupPresence == 1 : score.art >= static_cast<int>(ArtLevel::Calibrated);
    score.groupPresence = groupMask ? "mask" : "split";
    score.groupsKept = score.groupsMasked = score.groupsSplit = 0;
    score.groupNotes.clear();
    // A figure that shares lights with another that plays cannot be masked: its mask would take
    // the other's light too. A group over it is split there instead (the safe way of phase 21).
    std::map<const Scene*, std::set<std::string>> unmaskable;
    struct Mask {
        std::string figure;
        int from = 0, to = 0;
        bool rest = true; // else a glow
        std::string group;
    };
    std::vector<Mask> masks;
    int kept = 0, dimmed = 0, rested = 0, split = 0, pieces = 0, untouched = 0, protectedCount = 0;
    std::map<std::string, int> deepest; // target -> its deepest layer before
    for (const ChoreographyEvent& e : c.events) {
        int& d = deepest[e.target];
        d = std::max(d, e.layer);
    }
    std::map<std::string, int> before; // non-protected effects per target, to say which the score rested whole
    for (const ChoreographyEvent& e : c.events) {
        if (!Protected(e)) {
            before[e.target]++;
        }
    }
    std::vector<ChoreographyEvent> out;
    std::vector<ChoreographyEvent> members; // pieces of group effects moved onto one figure
    out.reserve(c.events.size());
    if (groupMask) {
        for (const Scene& s : scenes) {
            for (const SharedLights& x : score.sharedLights) {
                const auto pa = s.presence.find(x.a);
                const auto pb = s.presence.find(x.b);
                if (pa == s.presence.end() || pb == s.presence.end()) {
                    continue;
                }
                const bool aPlays = pa->second->policy != "suspended";
                const bool bPlays = pb->second->policy != "suspended";
                if (aPlays && pb->second->policy != "normal") {
                    unmaskable[&s].insert(x.b);
                }
                if (bPlays && pa->second->policy != "normal") {
                    unmaskable[&s].insert(x.a);
                }
            }
        }
    }
    for (ChoreographyEvent& e : c.events) {
        if (Protected(e)) {
            ++protectedCount;
            out.push_back(std::move(e));
            continue;
        }
        const std::vector<std::string> reach = Reaches(e, in.capabilities);
        bool any = false;
        for (const std::string& f : reach) {
            any = any || figures.contains(f);
        }
        if (!any) {
            ++untouched;
            out.push_back(std::move(e));
            continue;
        }
        const bool group = reach.size() > 1 || (in.capabilities != nullptr && in.capabilities->Find(e.target) != nullptr && in.capabilities->Find(e.target)->type == TargetType::Group);
        // What the scene does to each figure the effect reaches, as one word per stretch.
        const auto policyAt = [&](const Scene* s, const std::string& f) -> std::string {
            if (s == nullptr || !figures.contains(f)) {
                return "normal";
            }
            const auto it = s->presence.find(f);
            return it == s->presence.end() ? "normal" : it->second->policy == "protected" ? "normal" : it->second->policy;
        };
        const auto signature = [&](const Scene* s) {
            std::string first;
            bool same = true;
            for (const std::string& f : reach) {
                const std::string p = policyAt(s, f);
                if (first.empty()) {
                    first = p;
                } else if (p != first) {
                    same = false;
                }
            }
            return same ? first : std::string("split");
        };
        // Cut where the signature changes.
        std::vector<int> cuts = { e.startMS };
        for (const Scene& s : scenes) {
            if (s.from > e.startMS && s.from < e.endMS) {
                cuts.push_back(s.from);
            }
        }
        cuts.push_back(e.endMS);
        const auto sceneAt = [&](int ms) -> const Scene* {
            for (const Scene& s : scenes) {
                if (ms >= s.from && ms < s.to) {
                    return &s;
                }
            }
            return nullptr;
        };
        struct Piece {
            int from, to;
            const Scene* scene;
            std::string sig;
        };
        std::vector<Piece> parts;
        for (size_t k = 0; k + 1 < cuts.size(); ++k) {
            const Scene* s = sceneAt(cuts[k]);
            const std::string sig = signature(s);
            if (!parts.empty() && parts.back().sig == sig && (sig != "split" || parts.back().scene == s)) {
                parts.back().to = cuts[k + 1];
            } else {
                parts.push_back({ cuts[k], cuts[k + 1], s, sig });
            }
        }
        if (parts.size() == 1 && parts[0].sig == "normal") {
            ++kept;
            score.groupsKept += group ? 1 : 0;
            out.push_back(std::move(e));
            continue;
        }
        int n = 0;
        for (const Piece& p : parts) {
            if (p.to - p.from < 2 * frame) {
                continue;
            }
            if (p.sig == "suspended") {
                ++rested;
                continue;
            }
            if (p.sig == "normal" || p.sig == "reduced") {
                ChoreographyEvent x = e;
                x.startMS = p.from;
                x.endMS = p.to;
                if (parts.size() > 1) {
                    x.id += fmt::format("/p{}", n++);
                    if (p.from != e.startMS) {
                        x.transitionIn.clear();
                        x.fadeInMS = 0;
                        std::erase_if(x.settings, [](const auto& kv) { return kv.first == "T_CHOICE_In_Transition_Type" || kv.first == "T_TEXTCTRL_Fadein"; });
                    }
                    if (p.to != e.endMS) {
                        x.transitionOut.clear();
                        x.fadeOutMS = 0;
                        std::erase_if(x.settings, [](const auto& kv) { return kv.first == "T_CHOICE_Out_Transition_Type" || kv.first == "T_TEXTCTRL_Fadeout"; });
                    }
                }
                if (p.sig == "reduced") {
                    x.brightness = std::min(x.brightness, glow);
                    x.reasons.push_back({ "stage-presence", "ambient" });
                    ++dimmed;
                } else {
                    ++pieces;
                }
                out.push_back(std::move(x));
                continue;
            }
            // A group whose figures are asked different things. Kept whole, each figure that rests
            // or glows masked on its own row, when that is safe; else one effect per figure that plays.
            bool maskable = groupMask && p.scene != nullptr;
            if (maskable) {
                for (const std::string& f : reach) {
                    const std::string pol = policyAt(p.scene, f);
                    if (pol != "normal" && unmaskable[p.scene].contains(f)) {
                        maskable = false;
                    }
                }
            }
            if (maskable) {
                ChoreographyEvent x = e;
                x.startMS = p.from;
                x.endMS = p.to;
                if (parts.size() > 1) {
                    x.id += fmt::format("/p{}", n);
                    if (p.from != e.startMS) {
                        x.transitionIn.clear();
                        x.fadeInMS = 0;
                        std::erase_if(x.settings, [](const auto& kv) { return kv.first == "T_CHOICE_In_Transition_Type" || kv.first == "T_TEXTCTRL_Fadein"; });
                    }
                    if (p.to != e.endMS) {
                        x.transitionOut.clear();
                        x.fadeOutMS = 0;
                        std::erase_if(x.settings, [](const auto& kv) { return kv.first == "T_CHOICE_Out_Transition_Type" || kv.first == "T_TEXTCTRL_Fadeout"; });
                    }
                }
                x.reasons.push_back({ "stage-presence", "group-kept-masked" });
                out.push_back(std::move(x));
                for (const std::string& f : reach) {
                    const std::string pol = policyAt(p.scene, f);
                    if (pol == "suspended" || pol == "reduced") {
                        masks.push_back({ f, p.from, p.to, pol == "suspended", e.target });
                        pol == "suspended" ? ++rested : ++dimmed;
                    }
                }
                ++score.groupsMasked;
                ++n;
                continue;
            }
            if (groupMask) {
                score.groupNotes.push_back(fmt::format("split:{}@{}: a figure in it shares lights with another that plays", e.target, p.from));
            }
            ++score.groupsSplit;
            ++split;
            for (const std::string& f : reach) {
                const std::string pol = policyAt(p.scene, f);
                if (pol == "suspended") {
                    ++rested;
                    continue;
                }
                ChoreographyEvent x = e;
                x.id += fmt::format("/p{}/{}", n, f);
                x.target = f;
                x.type = TargetType::Model;
                x.startMS = p.from;
                x.endMS = p.to;
                x.transitionIn.clear();
                x.fadeInMS = 0;
                x.transitionOut.clear();
                x.fadeOutMS = 0;
                std::erase_if(x.settings, [](const auto& kv) { return kv.first.rfind("T_CHOICE_In", 0) == 0 || kv.first.rfind("T_CHOICE_Out", 0) == 0 || kv.first == "T_TEXTCTRL_Fadein" || kv.first == "T_TEXTCTRL_Fadeout"; });
                DropGroupOnlySettings(x);
                if (pol == "reduced") {
                    x.brightness = std::min(x.brightness, glow);
                    ++dimmed;
                }
                x.reasons.push_back({ "stage-presence", "from-group:" + e.target });
                members.push_back(std::move(x));
            }
            ++n;
        }
        (void)group;
    }
    // A group's piece goes under the figure's own effects (where the group's light was), on
    // the first layer free for its time: nothing shares a layer.
    std::stable_sort(members.begin(), members.end(), [](const ChoreographyEvent& a, const ChoreographyEvent& b) { return a.target != b.target ? a.target < b.target : a.startMS < b.startMS; });
    std::map<std::string, std::vector<const ChoreographyEvent*>> byTarget;
    for (const ChoreographyEvent& e : out) {
        byTarget[e.target].push_back(&e);
    }
    std::map<std::string, std::vector<std::pair<int, std::pair<int, int>>>> placed; // target -> (layer, [from, to))
    for (ChoreographyEvent& m : members) {
        const auto free = [&](int layer) {
            for (const ChoreographyEvent* o : byTarget[m.target]) {
                if (o->layer == layer && o->startMS < m.endMS && m.startMS < o->endMS) {
                    return false;
                }
            }
            for (const auto& [l, span] : placed[m.target]) {
                if (l == layer && span.first < m.endMS && m.startMS < span.second) {
                    return false;
                }
            }
            return true;
        };
        int layer = deepest.contains(m.target) ? deepest[m.target] + 1 : 0;
        while (!free(layer)) {
            ++layer;
        }
        m.layer = layer;
        placed[m.target].push_back({ layer, { m.startMS, m.endMS } });
    }
    // The masks: one per figure and stretch (masks of several groups over the same figure and time
    // are one), as deep as the figure's layers go, so its faces and words stay over them.
    {
        std::stable_sort(masks.begin(), masks.end(), [](const Mask& a, const Mask& b) {
            return a.figure != b.figure ? a.figure < b.figure : a.rest != b.rest ? a.rest > b.rest : a.from < b.from;
        });
        std::vector<Mask> merged;
        for (const Mask& m : masks) {
            if (!merged.empty() && merged.back().figure == m.figure && merged.back().rest == m.rest && m.from <= merged.back().to) {
                merged.back().to = std::max(merged.back().to, m.to);
                continue;
            }
            merged.push_back(m);
        }
        const std::string grey = fmt::format("#{:02X}{:02X}{:02X}", glow * 255 / 100, glow * 255 / 100, glow * 255 / 100);
        for (const Mask& m : merged) {
            if (m.to - m.from < 2 * frame) {
                continue;
            }
            ChoreographyEvent x;
            x.id = fmt::format("stage/presence-mask/{}/{}", m.figure, m.from);
            x.target = m.figure;
            x.type = TargetType::Model;
            x.prominence = Prominence::Support;
            x.intent = "presence-mask";
            x.startMS = m.from;
            x.endMS = m.to;
            x.effect = "On";
            x.settings = { { "T_CHOICE_LayerMethod", m.rest ? "1 is Mask" : "Brightness" } };
            x.colors = { m.rest ? std::string("#FFFFFF") : grey };
            x.continuation = "new";
            x.confidence = 1;
            x.provenance = "stage:presence-mask";
            x.reasons.push_back({ "stage-presence", m.rest ? "rest-mask" : "glow-mask" });
            // Below everything on that figure at that time.
            int layer = deepest.contains(m.figure) ? deepest[m.figure] + 1 : 0;
            for (const ChoreographyEvent* o : byTarget[m.figure]) {
                if (o->startMS < x.endMS && x.startMS < o->endMS) {
                    layer = std::max(layer, o->layer + 1);
                }
            }
            for (const auto& [l, span] : placed[m.figure]) {
                if (span.first < x.endMS && x.startMS < span.second) {
                    layer = std::max(layer, l + 1);
                }
            }
            x.layer = layer;
            placed[m.figure].push_back({ layer, { x.startMS, x.endMS } });
            members.push_back(std::move(x));
        }
    }
    // The lights a figure gives way on: a mask of exactly those lights, over everything it does
    // there (its other layers move one down; the score's gestures stay over it in RealizeStage).
    for (const Owned& o : owned) {
        const int from = near(o.from), to = near(o.to);
        if (to - from < 2 * frame) {
            continue;
        }
        for (ChoreographyEvent& e : out) {
            if (e.target == o.figure) {
                ++e.layer;
            }
        }
        for (ChoreographyEvent& e : members) {
            if (e.target == o.figure) {
                ++e.layer;
            }
        }
        FieldSpec f;
        f.function = FieldFunction::Pulse;
        f.duration = (to - from) / 1000.0;
        f.attack = 0;
        f.release = 0;
        f.level = 1;
        f.only = o.nodes;
        if (in.space != nullptr) {
            if (const StageFigure* sf = in.space->Find(o.figure); sf != nullptr) {
                f.transform = sf->transform;
            }
        }
        ChoreographyEvent x;
        x.id = fmt::format("stage/shared-mask/{}/{}", o.figure, from);
        x.target = o.figure;
        x.type = TargetType::Model;
        x.prominence = Prominence::Support;
        x.intent = "shared-mask";
        x.startMS = from;
        x.endMS = to;
        x.layer = 0;
        x.effect = "Stage Field";
        x.settings = { { "E_TEXTCTRL_StageField_Spec", ToText(f) }, { "E_SLIDER_StageField_Intensity", "100" }, { "T_CHOICE_LayerMethod", "1 is Mask" } };
        x.colors = { "#FFFFFF" };
        x.continuation = "new";
        x.confidence = 1;
        x.provenance = "stage:shared-mask";
        x.reasons.push_back({ "stage-presence", "shared-lights" });
        members.push_back(std::move(x));
    }
    int moved = 0, presenceMasks = 0, sharedMasks = 0;
    for (const ChoreographyEvent& m : members) {
        presenceMasks += m.provenance == "stage:presence-mask" ? 1 : 0;
        sharedMasks += m.provenance == "stage:shared-mask" ? 1 : 0;
        moved += m.provenance != "stage:presence-mask" && m.provenance != "stage:shared-mask" ? 1 : 0;
    }
    for (ChoreographyEvent& m : members) {
        out.push_back(std::move(m));
    }
    std::stable_sort(out.begin(), out.end(), [](const ChoreographyEvent& a, const ChoreographyEvent& b) {
        return a.startMS != b.startMS ? a.startMS < b.startMS : a.target != b.target ? a.target < b.target : a.layer < b.layer;
    });
    c.events = std::move(out);
    std::map<std::string, int> after;
    for (const ChoreographyEvent& e : c.events) {
        if (!Protected(e)) {
            after[e.target]++;
        }
    }
    for (const auto& [target, n] : before) {
        if (n > 0 && !after.contains(target)) {
            c.warnings.push_back(fmt::format("stage-rested:{}:the score rests it wherever it had effects", target));
        }
    }
    int resolved = 0, both = 0;
    for (const SharedLights& s : score.sharedLights) {
        resolved += s.scenesResolved;
        both += s.scenesBothLit;
    }
    notes.push_back(fmt::format("presence: {} effects kept whole, {} pieces kept, {} pieces dimmed to {}%, {} pieces rested, {} group effects split into {} figure effects, {} kept whole over {} "
                                "masks on the figures that rest or glow; {} masks on shared lights; {} protected, {} on no figure",
                                kept, pieces, dimmed, glow, rested, split, moved, score.groupsMasked, presenceMasks, sharedMasks, protectedCount, untouched));
    notes.push_back(fmt::format("shared lights: {} pairs, both lit in {} scenes, one gave way in {}", score.sharedLights.size(), both, resolved));
    score.presenceNotes = notes;
    return notes;
}

std::vector<std::string> LinkCompositions(GlobalStageScore& score, const StageDirectorInput& in, Choreography& c) {
    std::vector<std::string> notes;
    score.transitions.clear();
    if (in.space == nullptr || score.scenes.size() < 2) {
        return notes;
    }
    const int frame = std::max(1, c.frameMS);
    const auto near = [&](int ms) { return std::max(0, ((ms + frame / 2) / frame) * frame); };
    const auto down = [&](int ms) { return std::max(0, (ms / frame) * frame); };
    const auto beatAt = [&](int ms) {
        double beat = 0.5;
        if (in.analysis != nullptr) {
            for (const TempoPoint& t : in.analysis->tempo) {
                if (t.timeMS <= ms && t.bpm > 30) {
                    beat = 60.0 / t.bpm;
                }
            }
        }
        return beat;
    };
    const auto active = [](const StageScene& s) {
        int n = 0;
        for (const FigurePresence& p : s.presence) {
            n += p.policy == "normal" ? 1 : 0;
        }
        return n;
    };
    const auto lit = [](const StageScene& s, const std::string& f) {
        const FigurePresence* p = s.PresenceOf(f);
        return p != nullptr && p->policy != "suspended";
    };
    int softenedAll = 0;
    for (size_t i = 1; i < score.scenes.size(); ++i) {
        const StageScene& prev = score.scenes[i - 1];
        const StageScene& next = score.scenes[i];
        if (prev.presence.empty() || next.presence.empty()) {
            continue;
        }
        CompositionTransition t;
        t.from = prev.id;
        t.to = next.id;
        t.atMS = near(next.startMS);
        const int np = active(prev), nn = active(next);
        if (next.mode == StageMode::Rest || next.narrative == "rest") {
            t.kind = "resolution";
            t.why = COMPOSER_TEXT("the music rests: the light fades out");
        } else if (prev.mode == StageMode::Rest || prev.narrative == "rest") {
            t.kind = "propagation";
            t.why = COMPOSER_TEXT("after the rest the light comes back from where the scene starts");
        } else if (next.narrative == "resolution") {
            t.kind = "resolution";
            t.why = COMPOSER_TEXT("the song closes: the light gathers towards the figure that ends it");
        } else if (next.motifOperation == "transform") {
            t.kind = "transformation";
            t.why = COMPOSER_TEXT("the idea turns into another movement: it spreads from where it was");
        } else if (nn + 1 < np) {
            t.kind = "gathering";
            t.why = COMPOSER_TEXT("fewer figures carry the next idea: the others leave from the farthest in");
        } else if (nn > np + 1) {
            t.kind = "propagation";
            t.why = COMPOSER_TEXT("more figures join: they come in from where the next idea starts");
        } else if (next.motifOperation == "split" || next.narrative == "answer") {
            t.kind = "response";
            t.why = COMPOSER_TEXT("the next scene answers this one");
        } else if (!next.composition.empty() && next.composition == prev.composition && next.visualMotif == prev.visualMotif) {
            t.kind = "continuation";
            t.why = COMPOSER_TEXT("the same idea carries on");
        } else {
            t.kind = "variation";
            t.why = COMPOSER_TEXT("the idea changes its shape: the figures that change fade");
        }
        for (const FigurePresence& p : next.presence) {
            if (lit(next, p.figure) && !lit(prev, p.figure)) {
                t.joining.push_back(p.figure);
            }
        }
        for (const FigurePresence& p : prev.presence) {
            if (lit(prev, p.figure) && !lit(next, p.figure)) {
                t.leaving.push_back(p.figure);
            }
        }
        // Where the next idea starts (its protagonist), else where this one was.
        std::string centre;
        for (const StageScene* s : { &next, &prev }) {
            for (const FigurePresence& p : s->presence) {
                if (centre.empty() && p.state == "protagonist") {
                    centre = p.figure;
                }
            }
        }
        const StageFigure* cf = centre.empty() ? nullptr : in.space->Find(centre);
        const double ox = cf != nullptr ? cf->cx : 0.5;
        const double oy = cf != nullptr ? cf->cy : 0.5;
        const auto distance = [&](const std::string& f) {
            const StageFigure* sf = in.space->Find(f);
            return sf == nullptr ? 0.0 : std::hypot(sf->cx - ox, sf->cy - oy);
        };
        double far = 1e-6;
        for (const std::string& f : t.joining) {
            far = std::max(far, distance(f));
        }
        for (const std::string& f : t.leaving) {
            far = std::max(far, distance(f));
        }
        const double beat = beatAt(t.atMS);
        const int span = std::min(static_cast<int>(beat * 2000), std::max(0, (next.endMS - next.startMS) / 3));
        const int fade = std::max(frame, down(std::min(static_cast<int>(beat * 500), 600)));
        const bool stagger = t.kind == "propagation" || t.kind == "transformation";
        // The light gathers inside the music, never into a silence.
        const bool gather = (t.kind == "gathering" || t.kind == "resolution") && next.mode != StageMode::Rest && next.narrative != "rest";
        const std::set<std::string> joining(t.joining.begin(), t.joining.end());
        const std::set<std::string> leaving(t.leaving.begin(), t.leaving.end());
        // Every effect on a figure: to find a free stretch when one goes on a little longer.
        std::map<std::pair<std::string, int>, std::vector<std::pair<int, int>>> busy;
        for (const ChoreographyEvent& e : c.events) {
            busy[{ e.target, e.layer }].push_back({ e.startMS, e.endMS });
        }
        int abrupt = 0;
        for (ChoreographyEvent& e : c.events) {
            if (Protected(e)) {
                continue;
            }
            const std::string f = Base(e.target);
            if (joining.contains(f) && std::abs(e.startMS - t.atMS) <= frame && e.endMS - e.startMS > 4 * frame) {
                // It comes in: staggered by its distance from where the next idea starts.
                if (stagger && span > 0) {
                    const int delay = down(static_cast<int>(span * distance(f) / far));
                    if (e.endMS - (e.startMS + delay) > 4 * frame) {
                        e.startMS += delay;
                    }
                }
                const int len = e.endMS - e.startMS;
                const int in_ = std::max(frame, down(std::min(fade, len / 4)));
                const double before = std::strtod(Setting(e, "T_TEXTCTRL_Fadein").c_str(), nullptr);
                if (before * 1000 < in_) {
                    SetSetting(e, "T_CHOICE_In_Transition_Type", "Fade");
                    SetSetting(e, "T_TEXTCTRL_Fadein", fmt::format("{:.2f}", in_ / 1000.0));
                }
                ++t.softened;
            } else if (leaving.contains(f) && std::abs(e.endMS - t.atMS) <= frame && e.endMS - e.startMS > 4 * frame) {
                // It goes out: where the light gathers, the farthest first and the nearest last.
                if (gather && span > 0) {
                    int stay = down(static_cast<int>(span * (1.0 - distance(f) / far)));
                    for (const auto& [a, b] : busy[{ e.target, e.layer }]) {
                        if (a >= e.endMS && a < e.endMS + stay) {
                            stay = std::max(0, down(a - e.endMS)); // never over its next effect
                        }
                    }
                    e.endMS += stay;
                }
                const int len = e.endMS - e.startMS;
                const int out_ = std::max(frame, down(std::min(fade, len / 4)));
                const double before = std::strtod(Setting(e, "T_TEXTCTRL_Fadeout").c_str(), nullptr);
                if (before * 1000 < out_) {
                    SetSetting(e, "T_CHOICE_Out_Transition_Type", "Fade");
                    SetSetting(e, "T_TEXTCTRL_Fadeout", fmt::format("{:.2f}", out_ / 1000.0));
                }
                ++t.softened;
            }
        }
        // What the join still does abruptly: an effect of a figure leaving that stops dead.
        for (const ChoreographyEvent& e : c.events) {
            if (!Protected(e) && leaving.contains(Base(e.target)) && std::abs(e.endMS - t.atMS) <= frame && Setting(e, "T_TEXTCTRL_Fadeout").empty()) {
                ++abrupt;
            }
        }
        t.checks.push_back(fmt::format("abrupt-cut:{}", abrupt));
        int levelChanges = 0;
        for (const FigurePresence& p : next.presence) {
            const FigurePresence* q = prev.PresenceOf(p.figure);
            if (q != nullptr && ((q->policy == "normal" && p.policy == "reduced") || (q->policy == "reduced" && p.policy == "normal"))) {
                ++levelChanges;
            }
        }
        t.checks.push_back(fmt::format("level-change:{}", levelChanges));
        softenedAll += t.softened;
        score.transitions.push_back(std::move(t));
    }
    // Nothing may share a layer: a figure's effects on one layer must not overlap.
    std::map<std::pair<std::string, int>, std::vector<const ChoreographyEvent*>> rows;
    for (const ChoreographyEvent& e : c.events) {
        rows[{ e.target, e.layer }].push_back(&e);
    }
    int overlaps = 0;
    for (auto& [key, list] : rows) {
        std::stable_sort(list.begin(), list.end(), [](const ChoreographyEvent* a, const ChoreographyEvent* b) { return a->startMS < b->startMS; });
        for (size_t k = 1; k < list.size(); ++k) {
            overlaps += list[k]->startMS < list[k - 1]->endMS ? 1 : 0;
        }
    }
    std::stable_sort(c.events.begin(), c.events.end(), [](const ChoreographyEvent& a, const ChoreographyEvent& b) {
        return a.startMS != b.startMS ? a.startMS < b.startMS : a.target != b.target ? a.target < b.target : a.layer < b.layer;
    });
    std::map<std::string, int> kinds;
    for (const CompositionTransition& t : score.transitions) {
        kinds[t.kind]++;
    }
    std::string kindText;
    for (const auto& [k, n] : kinds) {
        kindText += fmt::format("{}{} x{}", kindText.empty() ? "" : ", ", k, n);
    }
    notes.push_back(fmt::format("transitions: {} joins ({}), {} effects softened, {} overlapping on one layer", score.transitions.size(), kindText, softenedAll, overlaps));
    score.presenceNotes.push_back(notes.back());
    return notes;
}

int OrderSharedLights(SequenceSpec& spec, const GlobalStageScore& score) {
    int moved = 0;
    for (const SharedLights& s : score.sharedLights) {
        if (s.scenesOwned == 0 || s.priority.empty()) {
            continue;
        }
        const std::string& winner = s.priority;
        const std::string loser = winner == s.a ? s.b : s.a;
        size_t w = spec.effects.size(), l = spec.effects.size();
        for (size_t i = 0; i < spec.effects.size(); ++i) {
            const std::string& t = spec.effects[i].target;
            if (t == winner && w == spec.effects.size()) {
                w = i;
            }
            if (t == loser && l == spec.effects.size()) {
                l = i;
            }
        }
        if (w == spec.effects.size() || l == spec.effects.size() || l < w) {
            continue;
        }
        // Only the loser's first effect moves: that alone sets its row; effects keep their times.
        EffectSpec first = spec.effects[l];
        spec.effects.erase(spec.effects.begin() + static_cast<long>(l));
        spec.effects.insert(spec.effects.begin() + static_cast<long>(w), std::move(first));
        ++moved;
    }
    return moved;
}

std::string SharedKindPhrase(const std::string& k) {
    if (k == "containment") return COMPOSER_TEXT("one is drawn inside the other on purpose");
    if (k == "duplicate") return COMPOSER_TEXT("the same lights described twice");
    if (k == "partial") return COMPOSER_TEXT("part of their lights in common, in the same place");
    if (k == "misconfigured") return COMPOSER_TEXT("the same channels drawn in two different places: probably a wiring or start-channel mistake");
    return COMPOSER_TEXT("not known: the layout does not say where their lights are");
}

std::string PresenceStatePhrase(const std::string& s) {
    if (s == "protagonist") return COMPOSER_TEXT("Leads");
    if (s == "complement") return COMPOSER_TEXT("Accompanies");
    if (s == "response") return COMPOSER_TEXT("Answers");
    if (s == "ambient") return COMPOSER_TEXT("Quiet glow");
    if (s == "protected") return COMPOSER_TEXT("Kept as a person left it");
    return COMPOSER_TEXT("Rests");
}

std::string TransitionKindPhrase(const std::string& k) {
    if (k == "continuation") return COMPOSER_TEXT("carries on");
    if (k == "variation") return COMPOSER_TEXT("changes its shape");
    if (k == "response") return COMPOSER_TEXT("answers");
    if (k == "propagation") return COMPOSER_TEXT("spreads to more figures");
    if (k == "gathering") return COMPOSER_TEXT("gathers onto fewer figures");
    if (k == "transformation") return COMPOSER_TEXT("turns into another movement");
    if (k == "resolution") return COMPOSER_TEXT("closes");
    return COMPOSER_TEXT("cuts");
}

} // namespace composer
