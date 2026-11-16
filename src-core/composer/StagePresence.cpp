#include "composer/StagePresence.h"

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

std::vector<SharedLights> FindSharedLights(const ModelCapabilities& caps, const std::vector<std::string>& figures) {
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
    if (in.capabilities != nullptr) {
        score.sharedLights = FindSharedLights(*in.capabilities, std::vector<std::string>(figures.begin(), figures.end()));
    }
    std::map<std::pair<std::string, std::string>, bool> chosen; // (winner, loser) a person picked
    if (in.edits != nullptr) {
        for (const StageEdit& e : in.edits->edits) {
            const size_t bar = e.value.find('|');
            if (e.action == "priority" && bar != std::string::npos) {
                chosen[{ e.value.substr(0, bar), e.value.substr(bar + 1) }] = true;
            }
        }
    }
    for (SharedLights& s : score.sharedLights) {
        if (chosen.contains({ s.a, s.b })) {
            s.priority = s.a;
            s.prioritySource = "person";
        } else if (chosen.contains({ s.b, s.a })) {
            s.priority = s.b;
            s.prioritySource = "person";
        }
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
            if (s.prioritySource == "person") {
                winner = s.priority == s.a ? pa : pb;
                loser = winner == pa ? pb : pa;
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
        if (s.scenesBothLit > s.scenesResolved && s.relation == "partial") {
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
            // A group whose figures are asked different things: one effect per figure that plays.
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
    const int moved = static_cast<int>(members.size());
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
    notes.push_back(fmt::format("presence: {} effects kept whole, {} pieces kept, {} pieces dimmed to {}%, {} pieces rested, {} group effects split into {} figure effects; {} protected, {} on no figure",
                                kept, pieces, dimmed, glow, rested, split, moved, protectedCount, untouched));
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
