#include "composer/StageDirector.h"

#include "composer/AudioUnderstanding.h"
#include "composer/ModelCapabilities.h"
#include "composer/VocalUnderstanding.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#define COMPOSER_TEXT(text) text

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

const std::vector<std::pair<StageMode, const char*>>& ModeIds() {
    static const std::vector<std::pair<StageMode, const char*>> M = {
        { StageMode::Protagonist, "protagonist" }, { StageMode::Duo, "duo" }, { StageMode::Group, "group" }, { StageMode::Unison, "unison" },
        { StageMode::Propagation, "propagation" }, { StageMode::LocalWaves, "local-waves" }, { StageMode::Cascade, "cascade" }, { StageMode::CallResponse, "call-response" },
        { StageMode::Counterpoint, "counterpoint" }, { StageMode::FullEnsemble, "full-ensemble" }, { StageMode::Rest, "rest" }
    };
    return M;
}

const std::vector<std::pair<GestureKind, const char*>>& KindIds() {
    static const std::vector<std::pair<GestureKind, const char*>> K = {
        { GestureKind::PropagatedWave, "propagated-wave" }, { GestureKind::LocalWaves, "local-waves" }, { GestureKind::Cascade, "cascade" },
        { GestureKind::JointPulse, "joint-pulse" }, { GestureKind::CallResponse, "call-response" }, { GestureKind::Echo, "echo" },
        { GestureKind::SpatialSweep, "spatial-sweep" }, { GestureKind::Expansion, "expansion" }, { GestureKind::Contraction, "contraction" },
        { GestureKind::Counterpoint, "counterpoint" }, { GestureKind::AmbientField, "ambient-field" }
    };
    return K;
}

// The cost of a role against a scene's attention budget.
double Cost(const std::string& role) {
    return role == "protagonist" ? 2.0 : role == "complement" || role == "responder" ? 1.0 : role == "accent" ? 0.5 : role == "background" ? 0.25 : 0.0;
}

// A moment of the music worth considering for a gesture.
struct Moment {
    int ms = 0;
    std::string kind; // "accent", "climax", "drop", "reentry", "crescendo", "tail"
    double weight = 0; // importance: strength x confidence
};

struct Figure {
    const StageFigure* stage = nullptr;
    const FigureGeometry* geometry = nullptr;
    const FigureProfile* profile = nullptr;
    bool face = false;   // a face xLights draws
    bool allFace = false; // its mouth and eyes are nearly all of it: a gesture would light nothing else
    bool excluded = false;
};

class Director {
public:
    explicit Director(const StageDirectorInput& in) : _in(in) {}

    GlobalStageScore Run() {
        GlobalStageScore score;
        score.durationMS = _in.analysis != nullptr ? _in.analysis->durationMS : 0;
        if (_in.plan == nullptr || _in.geometry == nullptr || _in.space == nullptr) {
            score.warnings.push_back("missing-input");
            return score;
        }
        Figures();
        if (_figures.empty()) {
            score.warnings.push_back("no-figure-on-stage");
            return score;
        }
        score.limits = _in.space->limits;
        for (const Figure& f : _figures) {
            if (f.allFace) {
                score.limits.push_back("all-face:" + f.stage->name); // its light is its singing: no gestures over it
            }
        }
        Moments();
        Singing();
        Sharing(score);
        for (const std::string& f : _locked) {
            score.limits.push_back("locked:" + f); // a person locked or edited its effects: the director leaves it as it is
        }
        for (const PlanSegment& s : _in.plan->segments) {
            score.scenes.push_back(Scene(s, score));
        }
        ApplyEdits(score);
        Identity(score);
        return score;
    }

private:
    // ------------------------------------------------------------- what is there
    void Figures() {
        for (const StageFigure& f : _in.space->figures) {
            if (!f.placed || f.lights < 1 || f.noController) {
                continue; // a figure only in the preview takes no part in a gesture of the house
            }
            Figure fig;
            fig.stage = &f;
            fig.geometry = _in.geometry->Find(f.name);
            fig.profile = _in.semantics != nullptr ? _in.semantics->Find(f.name) : nullptr;
            if (fig.geometry == nullptr) {
                continue;
            }
            if (fig.profile != nullptr) {
                const FigureRegion* mouth = fig.profile->Region("mouth");
                fig.face = fig.profile->mouth == "images" || (mouth != nullptr && mouth->source.rfind("face-definition", 0) == 0);
                std::set<int> kept;
                for (const FigureRegion& r : fig.profile->regions) {
                    if ((r.id == "mouth" || r.id == "eyes") && r.real) {
                        kept.insert(r.nodes.begin(), r.nodes.end());
                    }
                }
                fig.allFace = !kept.empty() && kept.size() * 5 >= static_cast<size_t>(f.lights) * 4;
            }
            if (_in.capabilities != nullptr) {
                const TargetInfo* t = _in.capabilities->Find(f.name);
                fig.excluded = t != nullptr && t->Excluded();
            }
            if (!fig.excluded) {
                _figures.push_back(fig);
            }
        }
        // Left to right, then bottom to top: the order a cascade reads in.
        std::stable_sort(_figures.begin(), _figures.end(), [](const Figure& a, const Figure& b) {
            return a.stage->cx != b.stage->cx ? a.stage->cx < b.stage->cx : a.stage->cy < b.stage->cy;
        });
    }

    const Figure* Find(const std::string& name) const {
        for (const Figure& f : _figures) {
            if (f.stage->name == name) {
                return &f;
            }
        }
        return nullptr;
    }

    // When each figure's face sings, from the choreography's face effects.
    void Singing() {
        if (_in.choreography == nullptr) {
            return;
        }
        for (const ChoreographyEvent& e : _in.choreography->events) {
            if (e.effect == "Faces" || e.intent == "face-sing") {
                const size_t slash = e.target.find('/');
                const std::string figure = slash == std::string::npos ? e.target : e.target.substr(0, slash);
                _singing[figure].push_back({ e.startMS, e.endMS });
                std::string face = "?";
                for (const auto& [k, v] : e.settings) {
                    if (k == "E_CHOICE_Faces_FaceDefinition") {
                        face = v;
                    }
                }
                _facesUsed[figure].insert(face);
            }
            if (e.state == DecisionState::Locked || e.state == DecisionState::ManualEdit) {
                const size_t slash = e.target.find('/');
                _locked.insert(slash == std::string::npos ? e.target : e.target.substr(0, slash));
            }
        }
    }

    // Whether a figure's lights belong to a face singing then: its own face, or another
    // model wired to the same channels (a layout may describe one prop twice).
    bool Sings(const std::string& figure, int a, int b) const {
        const auto singsItself = [&](const std::string& name) {
            const auto it = _singing.find(name);
            if (it == _singing.end()) {
                return false;
            }
            for (const auto& [x, y] : it->second) {
                if (x < b && a < y) {
                    return true;
                }
            }
            return false;
        };
        if (singsItself(figure)) {
            return true;
        }
        const auto shared = _sharing.find(figure);
        if (shared != _sharing.end()) {
            for (const std::string& other : shared->second) {
                if (singsItself(other)) {
                    return true;
                }
            }
        }
        return false;
    }

    // Models wired to the same channels as a figure on the stage.
    void Sharing(GlobalStageScore& score) {
        if (_in.capabilities == nullptr) {
            return;
        }
        std::vector<const TargetInfo*> models;
        for (const TargetInfo& t : _in.capabilities->targets) {
            if (t.type == TargetType::Model && t.lastChannel >= t.firstChannel && t.lastChannel > 0) {
                models.push_back(&t);
            }
        }
        for (const Figure& f : _figures) {
            const TargetInfo* t = _in.capabilities->Find(f.stage->name);
            if (t == nullptr || t->lastChannel == 0) {
                continue;
            }
            for (const TargetInfo* o : models) {
                if (o->name != t->name && o->firstChannel <= t->lastChannel && t->firstChannel <= o->lastChannel) {
                    _sharing[t->name].push_back(o->name);
                    if (t->name < o->name) {
                        score.limits.push_back("shares-lights:" + t->name + "|" + o->name);
                    }
                }
            }
        }
    }

    void Moments() {
        const SongAnalysis& a = *_in.analysis;
        for (const MusicalEvent& e : a.events) {
            if (e.type == MusicalEventType::Onset && e.intensity >= 0.5) {
                _moments.push_back({ e.timeMS, "accent", e.intensity * std::max(0.5, e.confidence) });
            } else if (e.type == MusicalEventType::Climax) {
                _moments.push_back({ e.timeMS, "climax", std::max(0.8, e.intensity) });
            }
        }
        if (_in.audio != nullptr) {
            for (const SoundEvent& e : _in.audio->events) {
                if (e.kind == "drop" || e.kind == "reentry" || e.kind == "crescendo" || e.kind == "tail") {
                    _moments.push_back({ e.startMS, e.kind, std::max(0.6, e.strength) * std::max(0.5, e.confidence) });
                } else if (e.kind == "accent" && e.confidence >= 0.6) {
                    _moments.push_back({ e.startMS, "accent", e.strength * e.confidence });
                }
            }
        }
        std::sort(_moments.begin(), _moments.end(), [](const Moment& x, const Moment& y) { return x.ms != y.ms ? x.ms < y.ms : x.kind < y.kind; });
    }

    std::vector<Moment> MomentsIn(int a, int b) const {
        std::vector<Moment> out;
        for (const Moment& m : _moments) {
            if (m.ms >= a && m.ms < b) {
                out.push_back(m);
            }
        }
        return out;
    }

    // ------------------------------------------------------------- a scene
    StageScene Scene(const PlanSegment& seg, GlobalStageScore& score) {
        StageScene sc;
        sc.id = seg.id;
        sc.startMS = seg.startMS;
        sc.endMS = seg.endMS;
        sc.sectionKind = seg.sectionKind;
        sc.confidence = 0.6;
        if (seg.repeatGroup >= 0) {
            sc.motif = "group:" + std::to_string(seg.repeatGroup);
        }
        // Who sings or speaks here: the faces the plan gave a voice to.
        std::vector<std::string> faces;
        std::set<std::string> voices;
        for (const PlanAssignment& a : seg.assignments) {
            if (a.intent == "face-sing" && Find(a.target) != nullptr) {
                faces.push_back(a.target);
                voices.insert(a.voice);
            }
        }
        const bool silence = std::find(seg.flags.begin(), seg.flags.end(), "silence") != seg.flags.end() || seg.priority == 0;
        const bool climax = seg.priority >= 3;
        const bool spoken = Spoken(seg.startMS, seg.endMS);
        const bool answers = Answers(seg.startMS, seg.endMS);
        const bool vocal = !faces.empty() || seg.vocalCoverage >= 0.3;
        // Context and mode, with the reason a person can read.
        if (silence && faces.empty()) {
            sc.context = "silence";
            sc.mode = StageMode::Rest;
            sc.reasons.push_back("the music rests: the installation rests too");
        } else if (spoken && !faces.empty()) {
            sc.context = "narration";
            sc.mode = StageMode::Protagonist;
            sc.reasons.push_back("a voice tells the story: one figure speaks, the rest listen");
        } else if (answers && faces.size() >= 2) {
            sc.context = "choir";
            sc.mode = StageMode::CallResponse;
            sc.reasons.push_back("a voice calls and the choir answers: the figures answer each other");
        } else if (vocal && voices.size() >= 2) {
            sc.context = "duet";
            sc.mode = StageMode::Duo;
            sc.reasons.push_back("two voices: two figures work together");
        } else if (vocal && climax) {
            sc.context = "climax";
            sc.mode = StageMode::FullEnsemble;
            sc.reasons.push_back("the song's peak with its voice: the whole house joins");
        } else if (vocal && seg.priority >= 2) {
            sc.context = "solo";
            sc.mode = StageMode::Group;
            sc.reasons.push_back("a strong sung part: the singer leads and a group accents with it");
        } else if (vocal) {
            sc.context = "solo";
            sc.mode = StageMode::Protagonist;
            sc.reasons.push_back("a voice sings quietly: it has the attention, the others stay discreet");
        } else if (climax) {
            sc.context = "climax";
            sc.mode = StageMode::FullEnsemble;
            sc.reasons.push_back("the instruments' peak: the whole house joins");
        } else if (seg.priority >= 2) {
            sc.context = "instrumental";
            sc.mode = StageMode::Propagation;
            sc.reasons.push_back("strong instruments with no voice: a gesture travels across the house");
        } else {
            sc.context = "instrumental";
            sc.mode = StageMode::LocalWaves;
            sc.reasons.push_back("quiet instruments: a few figures answer the music in small waves of their own");
        }
        // A person's choices for this scene come first: its roles and gestures follow them.
        _excludedHere.clear();
        for (const StageEdit* e : EditsFor(seg)) {
            StageMode m;
            if (e->action == "mode" && StageModeFromId(e->value, m)) {
                sc.mode = m;
                sc.manual = true;
                sc.reasons.push_back("chosen by a person");
            } else if (e->action == "exclude") {
                _excludedHere.insert(e->value);
            }
        }
        // The attention a scene can spend: a quiet scene little, a peak all of it.
        sc.budget = sc.mode == StageMode::Rest ? 0.5 : climax ? 1e9 : seg.priority >= 2 ? 6.0 : sc.mode == StageMode::Protagonist ? 3.0 : 4.5;
        Roles(sc, seg, faces);
        Gestures(sc, seg, faces, score);
        for (const SceneRole& r : sc.roles) {
            sc.spent += Cost(r.role);
            _lastUsed[r.figure] = r.role == "rest" ? _lastUsed[r.figure] : _sceneIndex;
        }
        ++_sceneIndex;
        return sc;
    }

    // The edits that belong to a scene: those whose remembered start is nearest this one.
    std::vector<const StageEdit*> EditsFor(const PlanSegment& seg) const {
        std::vector<const StageEdit*> out;
        if (_in.edits == nullptr) {
            return out;
        }
        for (const StageEdit& e : _in.edits->edits) {
            const PlanSegment* best = nullptr;
            for (const PlanSegment& s : _in.plan->segments) {
                if (std::abs(s.startMS - e.sceneStartMS) <= 1500 && (best == nullptr || std::abs(s.startMS - e.sceneStartMS) < std::abs(best->startMS - e.sceneStartMS))) {
                    best = &s;
                }
            }
            if (best == &seg) {
                out.push_back(&e);
            }
        }
        return out;
    }

    bool Spoken(int a, int b) const {
        if (_in.voices == nullptr) {
            return false;
        }
        int spoken = 0, other = 0;
        for (const Intervention& i : _in.voices->interventions) {
            const int o = std::max(0, std::min(b, i.endMS) - std::max(a, i.startMS));
            (i.emission == "spoken" ? spoken : other) += o;
        }
        return spoken > 0 && spoken >= other;
    }

    bool Answers(int a, int b) const {
        if (_in.voices == nullptr) {
            return false;
        }
        for (const Intervention& i : _in.voices->interventions) {
            if (i.startMS >= a && i.startMS < b) {
                for (const std::string& r : i.relations) {
                    if (r.rfind("answers:", 0) == 0) {
                        return true;
                    }
                }
            }
        }
        return false;
    }

    // Least recently used first, then left to right: variety with no dice.
    std::vector<const Figure*> ByRest(const std::set<std::string>& taken) const {
        std::vector<const Figure*> out;
        for (const Figure& f : _figures) {
            if (!taken.contains(f.stage->name)) {
                out.push_back(&f);
            }
        }
        std::stable_sort(out.begin(), out.end(), [&](const Figure* a, const Figure* b) {
            const auto la = _lastUsed.find(a->stage->name);
            const auto lb = _lastUsed.find(b->stage->name);
            const int ua = la == _lastUsed.end() ? -1 : la->second;
            const int ub = lb == _lastUsed.end() ? -1 : lb->second;
            return ua < ub;
        });
        return out;
    }

    void Roles(StageScene& sc, const PlanSegment& seg, const std::vector<std::string>& faces) {
        std::set<std::string> taken;
        double spent = 0;
        const auto give = [&](const std::string& figure, const char* role, const std::string& why) {
            if (taken.contains(figure) || _excludedHere.contains(figure)) {
                return;
            }
            taken.insert(figure);
            sc.roles.push_back({ figure, role, why });
            spent += Cost(role);
        };
        for (const std::string& f : faces) {
            give(f, "protagonist", "its voice is heard here");
        }
        // The instruments' lead when no voice leads: the figure the plan featured.
        if (faces.empty()) {
            for (const PlanAssignment& a : seg.assignments) {
                if (a.prominence == Prominence::Lead && a.intent != "lyric-text" && Find(a.target) != nullptr) {
                    give(a.target, "protagonist", "the plan features it here");
                    break;
                }
            }
        }
        const char* fill = sc.mode == StageMode::FullEnsemble || sc.mode == StageMode::Propagation ? "complement"
                           : sc.mode == StageMode::Protagonist                                  ? "background"
                           : sc.mode == StageMode::Rest                                         ? "background"
                                                                                                : "accent";
        for (const Figure* f : ByRest(taken)) {
            if (spent + Cost(fill) > sc.budget + 1e-9) {
                break;
            }
            give(f->stage->name, fill, sc.mode == StageMode::FullEnsemble ? "the whole house takes part" : "it has rested longest");
        }
        for (const Figure& f : _figures) {
            if (!taken.contains(f.stage->name)) {
                sc.roles.push_back({ f.stage->name, "rest", "the attention is elsewhere" });
            }
        }
    }

    // ------------------------------------------------------------- gestures
    void Gestures(StageScene& sc, const PlanSegment& seg, const std::vector<std::string>& faces, GlobalStageScore& score) {
        // The gestures a person placed come first, whatever the mode; the automatic ones fit
        // around them.
        std::vector<std::pair<int, int>> placed;
        for (const StageEdit* e : EditsFor(seg)) {
            GestureKind kind;
            if (e->action != "gesture" || !GestureKindFromId(e->value, kind) || e->atMS < sc.startMS || e->atMS >= sc.endMS) {
                continue;
            }
            _manual = true;
            const bool made = Make(sc, seg, kind, { e->atMS, "manual", 0.9 }, faces, score);
            _manual = false;
            if (made) {
                placed.push_back({ e->atMS, e->atMS });
            } else {
                score.warnings.push_back(fmt::format("edit-gesture-not-made:{}@{}", e->value, e->atMS));
            }
        }
        if (sc.mode == StageMode::Rest) {
            return;
        }
        std::vector<Moment> moments = MomentsIn(sc.startMS, sc.endMS);
        // A run of 2-5 close hits (a few beeps): one figure for each, in turn.
        std::vector<Moment> accents;
        for (const Moment& m : moments) {
            if (m.kind == "accent" && m.weight >= 0.45) {
                accents.push_back(m);
            }
        }
        const int maxGestures = sc.mode == StageMode::Protagonist ? 1 : seg.priority >= 3 ? 4 : seg.priority >= 2 ? 3 : 2;
        int made = 0;
        std::vector<std::pair<int, int>> busy = placed; // the scene's gestures so far, by their hits
        const auto roomFor = [&](int a, int b) {
            if (made >= maxGestures || b >= sc.endMS - 300) {
                return false;
            }
            for (const auto& [x, y] : busy) {
                if (b > x - 1500 && a < y + 1500) {
                    return false;
                }
            }
            return true;
        };
        const auto room = [&](int ms) { return roomFor(ms, ms); };
        // Special moments first: a drop gathers the light, a re-entry spreads it, a tail echoes.
        for (const Moment& m : moments) {
            if (!room(m.ms)) {
                continue;
            }
            GestureKind kind;
            if (m.kind == "drop") {
                kind = GestureKind::Contraction;
            } else if (m.kind == "reentry" || m.kind == "climax") {
                kind = GestureKind::Expansion;
            } else if (m.kind == "tail") {
                kind = GestureKind::Echo;
            } else if (m.kind == "crescendo") {
                kind = GestureKind::SpatialSweep;
            } else {
                continue;
            }
            if (Make(sc, seg, kind, m, faces, score)) {
                ++made;
                busy.push_back({ m.ms, m.ms });
            }
        }
        // A short burst of hits standing alone (a few beeps): one figure for each, in turn.
        // A steady pulse is not a burst: a run longer than five hits, or one with no quiet
        // around it, is the beat going on, and gets no cascade.
        bool cascaded = false;
        for (size_t i = 0; i < accents.size() && made < maxGestures && !cascaded;) {
            size_t j = i + 1;
            int widest = 0;
            while (j < accents.size() && accents[j].ms - accents[j - 1].ms >= 120 && accents[j].ms - accents[j - 1].ms <= 700) {
                widest = std::max(widest, accents[j].ms - accents[j - 1].ms);
                ++j;
            }
            const size_t count = j - i;
            const int before = i == 0 ? accents[i].ms - sc.startMS : accents[i].ms - accents[i - 1].ms;
            const int after = j == accents.size() ? sc.endMS - accents[j - 1].ms : accents[j].ms - accents[j - 1].ms;
            bool strongEach = true;
            for (size_t k = i; k < j; ++k) {
                strongEach = strongEach && accents[k].weight >= 0.55;
            }
            if (count >= 3 && count <= 5 && strongEach && before >= widest * 3 / 2 && after >= widest * 3 / 2 && roomFor(accents[i].ms, accents[j - 1].ms) && sc.mode != StageMode::Protagonist) {
                std::vector<Moment> run(accents.begin() + static_cast<long>(i), accents.begin() + static_cast<long>(j));
                if (Cascade(sc, run, score)) {
                    ++made;
                    busy.push_back({ run.front().ms, run.back().ms });
                    cascaded = true;
                }
            }
            i = j;
        }
        // The strongest remaining hit: the scene's main gesture, chosen by the mode.
        std::vector<Moment> strong = accents;
        std::stable_sort(strong.begin(), strong.end(), [](const Moment& a, const Moment& b) { return a.weight > b.weight; });
        for (const Moment& m : strong) {
            if (made >= maxGestures) {
                break;
            }
            if (!room(m.ms)) {
                continue;
            }
            GestureKind kind = GestureKind::JointPulse;
            switch (sc.mode) {
            case StageMode::Propagation: kind = GestureKind::PropagatedWave; break;
            case StageMode::LocalWaves: kind = GestureKind::LocalWaves; break;
            case StageMode::FullEnsemble: kind = GestureKind::PropagatedWave; break;
            case StageMode::CallResponse: kind = GestureKind::CallResponse; break;
            case StageMode::Duo: kind = GestureKind::Counterpoint; break;
            case StageMode::Protagonist: kind = GestureKind::AmbientField; break;
            default: {
                // A group answers its hits in turn with a flash, small waves of their own and a
                // wave from the singer. The same music (its repeat group) starts the same way,
                // so a chorus keeps its idea; another part starts elsewhere in the turn.
                static constexpr GestureKind turn[] = { GestureKind::JointPulse, GestureKind::LocalWaves, GestureKind::PropagatedWave };
                const int base = seg.repeatGroup >= 0 ? seg.repeatGroup : _sceneIndex;
                kind = turn[(base + made) % 3];
                break;
            }
            }
            if (Make(sc, seg, kind, m, faces, score)) {
                ++made;
                busy.push_back({ m.ms, m.ms });
            }
        }
        if (made == 0) {
            sc.reasons.push_back("no moment strong enough for a gesture: the scene keeps its own effects");
        }
    }

    bool Allowed(GestureKind k) const {
        if (_in.library == nullptr) {
            return true;
        }
        const StageLibraryEntry* e = _in.library->Find(GestureKindId(k));
        return e == nullptr || !e->excluded;
    }

    std::vector<std::string> Active(const StageScene& sc, bool withProtagonists) const {
        std::vector<std::string> out;
        for (const SceneRole& r : sc.roles) {
            const Figure* f = Find(r.figure);
            if (r.role != "rest" && (withProtagonists || r.role != "protagonist") && f != nullptr && !f->allFace && !_locked.contains(r.figure)) {
                out.push_back(r.figure);
            }
        }
        return out;
    }

    // The colours of the scene, as the colour engine chose them for its lead effects.
    std::vector<std::string> Colors(const StageScene& sc) const {
        std::vector<std::string> out;
        if (_in.choreography != nullptr) {
            for (const ChoreographyEvent& e : _in.choreography->events) {
                if (e.segmentId == sc.id && e.prominence == Prominence::Lead && !e.colors.empty()) {
                    for (const std::string& c : e.colors) {
                        if (std::find(out.begin(), out.end(), c) == out.end() && out.size() < 3) {
                            out.push_back(c);
                        }
                    }
                }
            }
        }
        if (out.empty()) {
            out = { "#FFFFFF" };
        }
        return out;
    }

    double Beat(int ms) const {
        double beat = 0.5;
        for (const TempoPoint& t : _in.analysis->tempo) {
            if (t.timeMS <= ms && t.bpm > 30) {
                beat = 60.0 / t.bpm;
            }
        }
        return beat;
    }

    // The farthest light of the given figures from a point.
    double Reach(const std::vector<std::string>& figures, double ox, double oy) const {
        double reach = 0;
        for (const std::string& f : figures) {
            const Figure* fig = Find(f);
            if (fig == nullptr) {
                continue;
            }
            reach = std::max({ reach, std::hypot(fig->stage->minX - ox, fig->stage->minY - oy), std::hypot(fig->stage->maxX - ox, fig->stage->maxY - oy),
                               std::hypot(fig->stage->minX - ox, fig->stage->maxY - oy), std::hypot(fig->stage->maxX - ox, fig->stage->minY - oy) });
        }
        return reach;
    }

    // Where a gesture starts: a bulb of the scene's protagonist when there is one, else of
    // the most central figure taking part.
    std::string Origin(const StageScene& sc, const std::vector<std::string>& active) const {
        std::string figure;
        for (const SceneRole& r : sc.roles) {
            if (r.role == "protagonist") {
                figure = r.figure;
                break;
            }
        }
        if (figure.empty() && !active.empty()) {
            double best = 1e9;
            for (const std::string& f : active) {
                const Figure* fig = Find(f);
                const double d = fig != nullptr ? std::hypot(fig->stage->cx - 0.5, fig->stage->cy - 0.5) : 1e9;
                if (d < best) {
                    best = d;
                    figure = f;
                }
            }
        }
        const Figure* fig = Find(figure);
        if (fig == nullptr || fig->geometry->nodeMap.empty()) {
            return "point:0.5,0.5";
        }
        // Its light nearest its centre: a bulb, not an abstract point.
        const auto lights = LightsOnStage(*fig->geometry, *fig->stage);
        size_t best = 0;
        double bestD = 1e9;
        for (size_t k = 0; k < lights.size(); ++k) {
            const double d = std::hypot(lights[k].first - fig->stage->cx, lights[k].second - fig->stage->cy);
            if (d < bestD) {
                bestD = d;
                best = k;
            }
        }
        return fmt::format("node:{}:{}", figure, best + 1);
    }

    bool Make(StageScene& sc, const PlanSegment& seg, GestureKind kind, const Moment& m, const std::vector<std::string>& faces, GlobalStageScore& score) {
        if (!_manual && !Allowed(kind)) {
            return false;
        }
        GlobalGesture g;
        g.kind = kind;
        g.manual = _manual;
        g.startMS = m.ms;
        g.trigger = fmt::format("{}@{}", m.kind, m.ms);
        g.colors = Colors(sc);
        g.confidence = std::clamp(m.weight, 0.3, 0.9);
        const double beat = Beat(m.ms);
        std::vector<std::string> active = Active(sc, kind != GestureKind::AmbientField);
        if (active.empty()) {
            return false;
        }
        switch (kind) {
        case GestureKind::PropagatedWave:
        case GestureKind::Echo:
        case GestureKind::Expansion:
        case GestureKind::Contraction: {
            g.origin = Origin(sc, active);
            const StageAnchor a = ResolveAnchor(g.origin, *_in.space, *_in.geometry, _in.semantics);
            g.ox = a.x;
            g.oy = a.y;
            const double reach = std::max(0.05, Reach(active, g.ox, g.oy));
            // It crosses the figures taking part in two beats: the music sets the speed.
            g.speed = std::clamp(reach / (2 * beat), 0.15, 4.0);
            g.width = std::clamp(reach / 8, 0.03, 0.2);
            g.endMS = std::min(sc.endMS, g.startMS + static_cast<int>((reach / g.speed + 0.6) * 1000) + (kind == GestureKind::Echo ? static_cast<int>(beat * 2000) : 0));
            g.envelope = kind == GestureKind::Contraction ? "long" : "short";
            g.loss = kind == GestureKind::Echo ? 0.5 : 0.0;
            g.reasons.push_back(kind == GestureKind::Contraction ? "the music drops: the light gathers back to one point"
                                : kind == GestureKind::Expansion  ? "the music comes back strongly: the light spreads from one point"
                                : kind == GestureKind::Echo       ? "the sound dies away: the gesture echoes, weaker"
                                                                  : "a strong hit: a wave leaves one bulb and crosses the figures taking part");
            for (const std::string& f : active) {
                const Figure* fig = Find(f);
                const auto lights = LightsOnStage(*fig->geometry, *fig->stage);
                double first = 1e9;
                for (const auto& [x, y] : lights) {
                    first = std::min(first, std::hypot(x - g.ox, y - g.oy) / g.speed);
                }
                const bool source = a.ok && g.origin.find(":" + f + ":") != std::string::npos;
                const char* fn = kind == GestureKind::Expansion ? "expansion" : kind == GestureKind::Contraction ? "expansion" : kind == GestureKind::Echo ? "echo" : "wave";
                g.participants.push_back({ f, source ? "source" : "receiver", kind == GestureKind::Contraction ? 0 : static_cast<int>(first * 1000), 1.0, fn,
                                           source ? "the wave starts in one of its bulbs" : fmt::format("the front reaches it {:.2f} s after it leaves", first) });
            }
            break;
        }
        case GestureKind::SpatialSweep: {
            g.trajectory = "linear";
            g.angle = 0;
            double minX = 1, maxX = 0;
            for (const std::string& f : active) {
                const Figure* fig = Find(f);
                minX = std::min(minX, fig->stage->minX);
                maxX = std::max(maxX, fig->stage->maxX);
            }
            g.ox = minX;
            g.oy = 0.5;
            g.origin = fmt::format("point:{:.4f},0.5", minX);
            const double span = std::max(0.05, maxX - minX);
            g.speed = std::clamp(span / (4 * beat), 0.1, 3.0);
            g.width = 0.06;
            g.endMS = std::min(sc.endMS, g.startMS + static_cast<int>((span / g.speed + 0.8) * 1000));
            g.envelope = "swell";
            g.reasons.push_back("the music grows: a sweep crosses the house from left to right, the light growing behind it");
            for (const std::string& f : active) {
                const Figure* fig = Find(f);
                g.participants.push_back({ f, "receiver", static_cast<int>(std::max(0.0, fig->stage->minX - minX) / g.speed * 1000), 1.0, "sweep", "reached in its turn" });
            }
            break;
        }
        case GestureKind::LocalWaves:
        case GestureKind::JointPulse: {
            // Each figure on its own, all on the same hit; few of them.
            const size_t count = std::min<size_t>(active.size(), kind == GestureKind::LocalWaves ? 4 : 6);
            g.endMS = std::min(sc.endMS, g.startMS + static_cast<int>(std::max(0.6, beat * 2) * 1000));
            g.reasons.push_back(kind == GestureKind::LocalWaves ? "a hit: several figures each make a small wave of their own, together"
                                                                : "a hit: several figures flash together");
            for (size_t k = 0; k < count; ++k) {
                g.participants.push_back({ active[k], "accent", 0, 1.0, kind == GestureKind::LocalWaves ? "ripple" : "pulse", "the same hit, in its own shape" });
            }
            break;
        }
        case GestureKind::CallResponse: {
            // The voice that calls, the others answering a beat apart, in order. A face that
            // is singing then calls with its own singing: the answers start a beat later.
            const int step = static_cast<int>(beat * 1000);
            std::vector<std::string> order;
            bool calledBySinging = false;
            for (const std::string& f : faces) {
                const Figure* fig = Find(f);
                if (fig == nullptr) {
                    continue;
                }
                if (fig->allFace || Sings(f, g.startMS, g.startMS + step)) {
                    calledBySinging = true;
                } else {
                    order.push_back(f);
                }
            }
            for (const std::string& f : active) {
                if (std::find(order.begin(), order.end(), f) == order.end() && std::find(faces.begin(), faces.end(), f) == faces.end() && !Sings(f, g.startMS, g.startMS + 6 * step)) {
                    order.push_back(f);
                }
            }
            const int first = calledBySinging ? 1 : 0;
            g.endMS = std::min(sc.endMS, g.startMS + step * static_cast<int>(std::min<size_t>(order.size(), 5) + first) + 600);
            g.reasons.push_back(calledBySinging ? "the voice calls by singing and the other figures answer it, a beat apart" : "one calls and the others answer, a beat apart");
            // Only the answers that fit before the scene ends: a late one would never be seen.
            for (size_t k = 0; k < order.size() && k < 5; ++k) {
                const int delay = static_cast<int>(k + first) * step;
                if (g.startMS + delay + 400 > g.endMS) {
                    break;
                }
                const bool caller = k == 0 && !calledBySinging;
                g.participants.push_back({ order[k], caller ? "source" : "responder", delay, caller ? 1.0 : 0.85, "pulse", caller ? "it calls" : "it answers" });
            }
            if (g.participants.size() < (calledBySinging ? 1u : 2u)) {
                return false; // a call with no answer is not this gesture
            }
            break;
        }
        case GestureKind::Counterpoint: {
            // Two halves of the stage doing different things at once.
            g.endMS = std::min(sc.endMS, g.startMS + static_cast<int>(beat * 4000));
            g.reasons.push_back("two voices: the left of the house flows while the right chases");
            for (const std::string& f : active) {
                const Figure* fig = Find(f);
                const bool left = fig->stage->cx < 0.5;
                g.participants.push_back({ f, "accent", 0, 0.9, left ? "flow" : "chase", left ? "the left half" : "the right half" });
            }
            break;
        }
        case GestureKind::AmbientField: {
            // Under a voice that leads: a slow field joining the figures that accompany it.
            std::vector<std::string> background;
            for (const SceneRole& r : sc.roles) {
                if ((r.role == "background" || r.role == "complement") && Find(r.figure) != nullptr && !Find(r.figure)->allFace && !_locked.contains(r.figure)) {
                    background.push_back(r.figure);
                }
            }
            if (background.empty()) {
                return false;
            }
            g.level = 0.35;
            g.endMS = std::min(sc.endMS, g.startMS + static_cast<int>(beat * 8000));
            g.envelope = "long";
            g.reasons.push_back("a voice leads: a slow, dim field joins the figures around it, none of them a protagonist");
            for (const std::string& f : background) {
                g.participants.push_back({ f, "background", 0, 0.35, "flow", "a quiet companion" });
            }
            break;
        }
        case GestureKind::Cascade:
            return false;
        }
        // A figure the gesture would reach only after the scene has ended takes no part: it
        // would get an effect that never lights.
        std::erase_if(g.participants, [&](const GestureParticipant& p) { return g.startMS + p.delayMS + 300 > g.endMS; });
        // Nor one whose face is singing then: while a face sings, its light is its singing.
        std::erase_if(g.participants, [&](const GestureParticipant& p) { return Sings(p.figure, g.startMS + p.delayMS - 100, g.endMS + 400) || _locked.contains(p.figure); });
        if (g.participants.empty() || g.endMS <= g.startMS) {
            return false;
        }
        g.id = fmt::format("{}@{}", GestureKindId(kind), g.startMS);
        if (_manual) {
            g.reasons.insert(g.reasons.begin(), "placed by a person");
        }
        sc.gestures.push_back(g.id);
        score.gestures.push_back(std::move(g));
        return true;
    }

    bool Cascade(StageScene& sc, const std::vector<Moment>& run, GlobalStageScore& score) {
        if (!Allowed(GestureKind::Cascade)) {
            return false;
        }
        std::vector<std::string> active = Active(sc, true);
        if (active.size() < 2) {
            return false;
        }
        GlobalGesture g;
        g.kind = GestureKind::Cascade;
        g.trajectory = "sequence";
        g.startMS = run.front().ms;
        g.endMS = std::min(sc.endMS, run.back().ms + 700);
        g.trigger = fmt::format("accents@{}x{}", run.front().ms, run.size());
        g.colors = Colors(sc);
        g.confidence = 0.6;
        // One figure (left to right on the stage) for each hit, at the hit's own time; on
        // the last hit the figures that took part answer together.
        const size_t n = std::min(run.size(), active.size());
        std::vector<std::string> chosen;
        for (size_t k = 0; k < n; ++k) {
            chosen.push_back(active[k * active.size() / n]);
        }
        for (size_t k = 0; k < n; ++k) {
            g.participants.push_back({ chosen[k], k == 0 ? "source" : "receiver", run[k].ms - g.startMS, 1.0, "pulse", fmt::format("hit {} of {}", k + 1, run.size()) });
        }
        g.reasons.push_back("a few hits close together: one figure for each, in turn across the house");
        g.id = fmt::format("{}@{}", GestureKindId(GestureKind::Cascade), g.startMS);
        sc.gestures.push_back(g.id);
        score.gestures.push_back(std::move(g));
        return true;
    }

    // ------------------------------------------------------------- a person's changes
    void ApplyEdits(GlobalStageScore& score) {
        if (_in.edits == nullptr) {
            return;
        }
        for (const StageEdit& e : _in.edits->edits) {
            if (e.action == "identity" || e.action == "no-identity" || e.action == "gesture") {
                continue; // decided with the scenes' layers and gestures
            }
            StageScene* sc = nullptr;
            for (StageScene& s : score.scenes) {
                if (e.sceneStartMS >= 0 && std::abs(s.startMS - e.sceneStartMS) <= 1500 && (sc == nullptr || std::abs(s.startMS - e.sceneStartMS) < std::abs(sc->startMS - e.sceneStartMS))) {
                    sc = &s;
                }
            }
            if (sc == nullptr) {
                score.warnings.push_back("edit-scene-gone:" + std::to_string(e.sceneStartMS));
                continue;
            }
            sc->manual = true;
            if (e.action == "mode") {
                continue; // decided with the scene, before its roles and gestures
            } else if (e.action == "include" || e.action == "exclude") {
                for (SceneRole& r : sc->roles) {
                    if (r.figure == e.value) {
                        r.role = e.action == "include" ? (r.role == "rest" ? "complement" : r.role) : "rest";
                        r.why = "chosen by a person";
                    }
                }
                if (e.action == "exclude") {
                    for (GlobalGesture& g : score.gestures) {
                        if (std::find(sc->gestures.begin(), sc->gestures.end(), g.id) != sc->gestures.end()) {
                            std::erase_if(g.participants, [&](const GestureParticipant& p) { return p.figure == e.value; });
                            g.excluded.push_back(e.value);
                        }
                    }
                }
            } else if (e.action == "no-gestures") {
                std::erase_if(score.gestures, [&](const GlobalGesture& g) { return std::find(sc->gestures.begin(), sc->gestures.end(), g.id) != sc->gestures.end(); });
                sc->gestures.clear();
                sc->reasons.push_back("a person asked for no gestures here");
            } else if (e.action == "origin" || e.action == "speed") {
                for (GlobalGesture& g : score.gestures) {
                    if (std::find(sc->gestures.begin(), sc->gestures.end(), g.id) == sc->gestures.end() || (e.atMS >= 0 && std::abs(g.startMS - e.atMS) > 1500)) {
                        continue;
                    }
                    if (e.action == "origin") {
                        const StageAnchor a = ResolveAnchor(e.value, *_in.space, *_in.geometry, _in.semantics);
                        if (!a.ok) {
                            score.warnings.push_back("edit-origin-gone:" + e.value);
                            continue;
                        }
                        g.origin = e.value;
                        g.ox = a.x;
                        g.oy = a.y;
                        for (GestureParticipant& p : g.participants) {
                            const Figure* fig = Find(p.figure);
                            if (fig == nullptr) {
                                continue;
                            }
                            double first = 1e9;
                            for (const auto& [x, y] : LightsOnStage(*fig->geometry, *fig->stage)) {
                                first = std::min(first, std::hypot(x - g.ox, y - g.oy) / g.speed);
                            }
                            if (g.kind == GestureKind::PropagatedWave || g.kind == GestureKind::Echo || g.kind == GestureKind::Expansion) {
                                p.delayMS = static_cast<int>(first * 1000);
                            }
                        }
                    } else {
                        const double v = std::strtod(e.value.c_str(), nullptr);
                        if (v > 0.05 && v < 10) {
                            for (GestureParticipant& p : g.participants) {
                                p.delayMS = static_cast<int>(p.delayMS * g.speed / v);
                            }
                            g.speed = v;
                        }
                    }
                    g.manual = true;
                    g.reasons.push_back("changed by a person");
                }
            }
        }
    }

    // ------------------------------------------------------------- figures composed whole
    // A figure that leads or accompanies a scene keeps its own colours (branches, trunk,
    // outline...) under everything it does there; its mouth and eyes are its face's. Only
    // parts known for sure give a colour: a submodel, a name, a face definition or a
    // person's word, never a guess from the shape.
    void Identity(GlobalStageScore& score) {
        const auto sure = [](const FigureRegion& r) {
            return r.confirmed || r.source.rfind("submodel:", 0) == 0 || r.source.rfind("name-hint:", 0) == 0 || r.source.rfind("face-definition:", 0) == 0 ||
                   r.source == "manual";
        };
        // A person's word first: keep a figure's colours wherever it takes part (or in one
        // scene), or never.
        std::set<std::string> always, never;
        std::set<std::pair<std::string, std::string>> here, notHere; // scene id, figure
        if (_in.edits != nullptr) {
            for (const StageEdit& e : _in.edits->edits) {
                if (e.action != "identity" && e.action != "no-identity") {
                    continue;
                }
                const bool on = e.action == "identity";
                if (e.sceneStartMS < 0) {
                    (on ? always : never).insert(e.value);
                    continue;
                }
                for (const PlanSegment& seg : _in.plan->segments) {
                    const auto mine = EditsFor(seg);
                    if (std::find(mine.begin(), mine.end(), &e) != mine.end()) {
                        (on ? here : notHere).insert({ seg.id, e.value });
                    }
                }
            }
        }
        for (const StageScene& sc : score.scenes) {
            for (const SceneRole& r : sc.roles) {
                const bool asked = r.role != "rest" && (always.contains(r.figure) || here.contains({ sc.id, r.figure }));
                if (never.contains(r.figure) || notHere.contains({ sc.id, r.figure })) {
                    continue;
                }
                if (r.role != "protagonist" && r.role != "complement" && !asked) {
                    continue;
                }
                const Figure* f = Find(r.figure);
                if (f == nullptr || f->profile == nullptr || _locked.contains(r.figure)) {
                    continue;
                }
                if (f->face && Sings(r.figure, sc.startMS, sc.endMS)) {
                    // Only where the singing mouth is known light by light: the face sings with
                    // the one definition its mouth was read from. Otherwise a colour under the
                    // face could show through a mouth shape that should be dark.
                    const FigureRegion* mouthRegion = f->profile->Region("mouth");
                    const auto used = _facesUsed.find(r.figure);
                    const bool known = mouthRegion != nullptr && mouthRegion->source.rfind("face-definition:", 0) == 0 && used != _facesUsed.end() && used->second.size() == 1 &&
                                       "face-definition:" + *used->second.begin() == mouthRegion->source;
                    if (!known) {
                        const std::string limit = "no-identity-face-unknown:" + r.figure;
                        if (std::find(score.limits.begin(), score.limits.end(), limit) == score.limits.end()) {
                            score.limits.push_back(limit);
                        }
                        continue;
                    }
                }
                std::vector<const FigureRegion*> regions;
                std::vector<int> protectedNodes;
                for (const FigureRegion& reg : f->profile->regions) {
                    if (reg.id == "mouth" || reg.id == "eyes") {
                        protectedNodes.insert(protectedNodes.end(), reg.nodes.begin(), reg.nodes.end());
                    } else if (reg.real && sure(reg) && !reg.color.empty() && !reg.nodes.empty() && reg.id != "face" && reg.id != "text" && reg.id != "image") {
                        regions.push_back(&reg);
                    }
                }
                if (regions.empty()) {
                    continue;
                }
                // Larger parts lower, so a trunk shows over the body it belongs to.
                std::stable_sort(regions.begin(), regions.end(), [](const FigureRegion* a, const FigureRegion* b) { return a->nodes.size() > b->nodes.size(); });
                std::sort(protectedNodes.begin(), protectedNodes.end());
                protectedNodes.erase(std::unique(protectedNodes.begin(), protectedNodes.end()), protectedNodes.end());
                int order = 0;
                for (const FigureRegion* reg : regions) {
                    StageLayer l;
                    l.figure = r.figure;
                    l.role = "identity";
                    l.region = reg->id;
                    l.source = "field:pulse";
                    l.color = reg->color;
                    l.only = NodeRangesText(reg->nodes);
                    l.skip = NodeRangesText(protectedNodes);
                    l.order = order++;
                    l.opacity = 0.55; // its colour, quietly: the scene's light goes over it
                    l.startMS = sc.startMS;
                    l.endMS = sc.endMS;
                    score.layers.push_back(l);
                }
            }
        }
    }

    const StageDirectorInput& _in;
    std::vector<Figure> _figures;
    std::vector<Moment> _moments;
    std::map<std::string, int> _lastUsed;
    std::set<std::string> _excludedHere;
    std::map<std::string, std::vector<std::pair<int, int>>> _singing;
    std::map<std::string, std::vector<std::string>> _sharing;
    std::map<std::string, std::set<std::string>> _facesUsed; // the face definitions each figure sings with
    std::set<std::string> _locked; // figures with effects a person locked or edited
    int _sceneIndex = 0;
    bool _manual = false; // the gesture being made was placed by a person
};

std::vector<int> ProtectedNodes(const FigureSemantics* semantics, const std::string& figure) {
    std::vector<int> out;
    const FigureProfile* p = semantics != nullptr ? semantics->Find(figure) : nullptr;
    if (p == nullptr) {
        return out;
    }
    for (const FigureRegion& r : p->regions) {
        if ((r.id == "mouth" || r.id == "eyes") && r.real) {
            out.insert(out.end(), r.nodes.begin(), r.nodes.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

} // namespace

std::string StageScene::RoleOf(const std::string& figure) const {
    for (const SceneRole& r : roles) {
        if (r.figure == figure) {
            return r.role;
        }
    }
    return "rest";
}

const StageScene* GlobalStageScore::SceneAt(int ms) const {
    for (const StageScene& s : scenes) {
        if (ms >= s.startMS && ms < s.endMS) {
            return &s;
        }
    }
    return nullptr;
}

const GlobalGesture* GlobalStageScore::Gesture(const std::string& id) const {
    for (const GlobalGesture& g : gestures) {
        if (g.id == id) {
            return &g;
        }
    }
    return nullptr;
}

const StageLibraryEntry* StageLibrary::Find(const std::string& id) const {
    for (const StageLibraryEntry& e : entries) {
        if (e.id == id) {
            return &e;
        }
    }
    return nullptr;
}

GlobalStageScore DirectStage(const StageDirectorInput& input) {
    Director d(input);
    return d.Run();
}

FieldSpec ParticipantField(const GlobalGesture& g, const GestureParticipant& p, const StageSpace& space, const FigureSemantics* semantics, int effectStartMS) {
    FieldSpec s;
    if (!FieldFunctionFromId(p.function, s.function)) {
        s.function = FieldFunction::Pulse;
    }
    const StageFigure* f = space.Find(p.figure);
    s.transform = f != nullptr ? f->transform : std::vector<double>();
    s.level = std::clamp(p.level * g.level, 0.0, 1.0);
    s.speed = g.speed;
    s.width = g.width;
    s.loss = g.loss;
    s.shape = g.trajectory == "linear" ? "linear" : "radial";
    s.angle = g.angle;
    s.attack = 0.03;
    s.release = g.envelope == "long" ? 0.8 : 0.25;
    s.duration = std::max(0.2, (g.endMS - g.startMS) / 1000.0);
    // Fields of the whole stage keep the gesture's origin; a figure's own wave or pulse is
    // drawn round its own centre, starting at its own moment.
    const bool shared = s.function == FieldFunction::Wave || s.function == FieldFunction::Echo || s.function == FieldFunction::Expansion || s.function == FieldFunction::Sweep;
    if (shared) {
        s.ox = g.ox;
        s.oy = g.oy;
        s.contract = g.kind == GestureKind::Contraction;
        if (s.function == FieldFunction::Echo) {
            s.repeats = 3;
            s.period = 0.45;
            s.gain = 0.55;
        }
        s.at = (effectStartMS - g.startMS) / 1000.0;
    } else {
        s.ox = f != nullptr ? f->cx : 0.5;
        s.oy = f != nullptr ? f->cy : 0.5;
        const double size = f != nullptr ? std::max({ f->maxX - f->minX, f->maxY - f->minY, 0.02 }) : 0.1;
        s.speed = std::clamp(size / 0.35, 0.05, 5.0); // a figure's own wave crosses it in about a third of a second
        s.width = std::clamp(size / 4, 0.01, 0.2);
        s.duration = std::max(0.3, std::min(s.duration - p.delayMS / 1000.0, p.function == "pulse" ? 0.6 : 4.0));
        s.release = p.function == "pulse" ? 0.3 : s.release;
        s.period = p.function == "chase" ? 0.4 : p.function == "flow" ? 3.0 : 0.35;
        s.repeats = p.function == "ripple" ? 2 : 1;
        s.scale = 3;
        s.at = (effectStartMS - (g.startMS + p.delayMS)) / 1000.0; // below 0: the effect waits in the dark for its moment
    }
    const std::vector<int> keep = ProtectedNodes(semantics, p.figure);
    s.skip = NodeRangesText(keep);
    return s;
}

namespace {
// Greedy layers for one figure's effects: each takes the first layer from `base` where
// nothing of its own kind is playing. Returns how many layers were used.
int Stack(std::vector<ChoreographyEvent*>& events, int base) {
    std::stable_sort(events.begin(), events.end(), [](const ChoreographyEvent* a, const ChoreographyEvent* b) { return a->startMS < b->startMS; });
    std::vector<int> busyUntil;
    for (ChoreographyEvent* e : events) {
        size_t k = 0;
        while (k < busyUntil.size() && busyUntil[k] > e->startMS) {
            ++k;
        }
        if (k == busyUntil.size()) {
            busyUntil.push_back(0);
        }
        busyUntil[k] = e->endMS;
        e->layer = base + static_cast<int>(k);
    }
    return static_cast<int>(busyUntil.size());
}
} // namespace

std::vector<std::string> RealizeStage(const GlobalStageScore& score, const StageDirectorInput& in, Choreography& c) {
    std::vector<std::string> notes;
    if (in.space == nullptr) {
        return notes;
    }
    // xLights keeps an effect's times on its frames: every time made here is on one.
    const int frame = std::max(1, c.frameMS);
    const int lastMS = (std::max(score.durationMS, c.durationMS) / frame) * frame;
    const auto down = [&](int ms) { return std::clamp((ms / frame) * frame, 0, lastMS > 0 ? lastMS : ms); };
    const auto near = [&](int ms) { return std::clamp(((ms + frame / 2) / frame) * frame, 0, lastMS > 0 ? lastMS : ms); };
    int gestureEvents = 0, identityEvents = 0, rested = 0, late = 0, lockedOut = 0;
    // A figure whose effects a person locked or edited keeps them as they are: no layer of the
    // score goes on it, so none of its effects has to move.
    std::set<std::string> locked;
    for (const ChoreographyEvent& e : c.events) {
        if (e.state == DecisionState::Locked || e.state == DecisionState::ManualEdit) {
            const size_t slash = e.target.find('/');
            locked.insert(slash == std::string::npos ? e.target : e.target.substr(0, slash));
        }
    }
    // Resting figures: in a scene that gives the attention to a few, the others' own
    // effects stop (their faces, words and pictures stay).
    for (const StageScene& sc : score.scenes) {
        if (sc.mode != StageMode::Protagonist && sc.mode != StageMode::Duo && sc.mode != StageMode::Rest && sc.mode != StageMode::CallResponse) {
            continue;
        }
        const int from = near(sc.startMS);
        const int to = near(sc.endMS);
        std::vector<ChoreographyEvent> kept;
        for (ChoreographyEvent& e : c.events) {
            const bool inScene = e.startMS < to && e.endMS > from;
            const bool keepAlways = e.intent == "face-sing" || e.intent == "lyric-text" || e.intent == "media" || e.prominence == Prominence::Lead || e.effect == "Faces" ||
                                    e.effect == "Text" || e.effect == "Pictures" || e.effect == "Video" || e.provenance.rfind("stage:", 0) == 0 ||
                                    e.state == DecisionState::Locked || e.state == DecisionState::ManualEdit;
            if (!inScene || keepAlways || sc.RoleOf(e.target) != "rest" || in.space->Find(e.target) == nullptr) {
                kept.push_back(std::move(e));
                continue;
            }
            // Cut out of the scene; what lies outside it stays.
            if (e.startMS < from) {
                ChoreographyEvent before = e;
                before.endMS = from;
                before.id += "/before-" + sc.id;
                kept.push_back(std::move(before));
            }
            if (e.endMS > to) {
                ChoreographyEvent after = e;
                after.startMS = to;
                after.id += "/after-" + sc.id;
                kept.push_back(std::move(after));
            }
            ++rested;
        }
        c.events = std::move(kept);
    }
    // The gestures, one Stage Field per figure, each joining at its own moment.
    for (const GlobalGesture& g : score.gestures) {
        for (const GestureParticipant& p : g.participants) {
            if (locked.contains(p.figure)) {
                ++lockedOut;
                continue;
            }
            // A little before it arrives: the front enters, it does not pop.
            const int effectStart = down(std::max(g.startMS, g.startMS + p.delayMS - 100));
            const int endMS = near(std::max(effectStart + 2 * frame, g.endMS + 400));
            if (g.startMS + p.delayMS + 2 * frame >= endMS) {
                ++late; // its moment falls after its effect would end: it would never light
                continue;
            }
            FieldSpec s = ParticipantField(g, p, *in.space, in.semantics, effectStart);
            ChoreographyEvent e;
            e.id = fmt::format("stage/{}/{}", g.id, p.figure);
            e.target = p.figure;
            e.type = TargetType::Model;
            e.prominence = p.role == "source" ? Prominence::Lead : p.role == "background" ? Prominence::Support : Prominence::Accent;
            e.intent = "stage-gesture";
            const StageScene* sc = score.SceneAt(g.startMS);
            e.segmentId = sc != nullptr ? sc->id : "";
            e.startMS = effectStart;
            e.endMS = endMS;
            e.effect = "Stage Field";
            e.settings = { { "E_TEXTCTRL_StageField_Spec", ToText(s) }, { "E_SLIDER_StageField_Intensity", "100" }, { "T_CHOICE_LayerMethod", "Normal" } };
            e.colors = g.colors;
            e.intensity = s.level;
            e.continuation = "new";
            e.confidence = g.confidence;
            e.provenance = "stage:" + g.id;
            e.reasons.push_back({ "stage-gesture", g.id });
            e.reasons.push_back({ "stage-role", p.role });
            c.events.push_back(std::move(e));
            ++gestureEvents;
        }
    }
    // Identity layers, under everything the figure has.
    for (const StageLayer& l : score.layers) {
        if (locked.contains(l.figure)) {
            ++lockedOut;
            continue;
        }
        const int from = near(l.startMS);
        const int to = near(l.endMS);
        if (to <= from) {
            continue;
        }
        FieldSpec s;
        s.function = FieldFunction::Pulse;
        s.duration = (to - from) / 1000.0;
        s.attack = 0.05;
        s.release = 0.05;
        s.level = l.opacity;
        s.only = l.only;
        s.skip = l.skip;
        const StageFigure* f = in.space->Find(l.figure);
        s.transform = f != nullptr ? f->transform : std::vector<double>();
        ChoreographyEvent e;
        e.id = fmt::format("stage/identity/{}/{}/{}", l.figure, l.region, l.startMS);
        e.target = l.figure;
        e.type = TargetType::Model;
        e.prominence = Prominence::Support;
        e.intent = "stage-identity";
        const StageScene* sc = score.SceneAt(l.startMS);
        e.segmentId = sc != nullptr ? sc->id : "";
        e.startMS = from;
        e.endMS = to;
        e.effect = "Stage Field";
        e.settings = { { "E_TEXTCTRL_StageField_Spec", ToText(s) }, { "E_SLIDER_StageField_Intensity", "100" }, { "T_CHOICE_LayerMethod", "Normal" } };
        e.colors = { l.color };
        e.intensity = l.opacity;
        e.continuation = "new";
        e.confidence = 0.8;
        e.provenance = "stage:identity";
        e.reasons.push_back({ "stage-identity", l.region });
        c.events.push_back(std::move(e));
        ++identityEvents;
    }
    // The layers of each figure the score touches, top to bottom: its gestures, then its
    // own effects as they were (in their order), then its identity. Nothing shares a layer.
    std::map<std::string, std::vector<ChoreographyEvent*>> gestures, identity, own;
    for (ChoreographyEvent& e : c.events) {
        if (e.intent == "stage-gesture") {
            gestures[e.target].push_back(&e);
        } else if (e.intent == "stage-identity") {
            identity[e.target].push_back(&e);
        } else {
            own[e.target].push_back(&e);
        }
    }
    std::set<std::string> touched;
    for (const auto& [t, list] : gestures) {
        touched.insert(t);
    }
    for (const auto& [t, list] : identity) {
        touched.insert(t);
    }
    for (const std::string& t : touched) {
        if (locked.contains(t)) {
            continue; // its layers are a person's: none moves
        }
        const int top = gestures.contains(t) ? Stack(gestures[t], 0) : 0;
        int deepest = top - 1;
        if (own.contains(t)) {
            for (ChoreographyEvent* e : own[t]) {
                e->layer += top;
                deepest = std::max(deepest, e->layer);
            }
        }
        if (identity.contains(t)) {
            (void)Stack(identity[t], deepest + 1);
        }
    }
    std::stable_sort(c.events.begin(), c.events.end(), [](const ChoreographyEvent& a, const ChoreographyEvent& b) {
        return a.startMS != b.startMS ? a.startMS < b.startMS : a.target != b.target ? a.target < b.target : a.layer < b.layer;
    });
    c.metrics = MeasureChoreography(c);
    notes.push_back(fmt::format("{} gesture effects, {} identity layers, {} background effects rested, {} reached too late left out, {} left off figures a person locked", gestureEvents,
                                identityEvents, rested, late, lockedOut));
    return notes;
}

int PutGroupsUnderStage(SequenceSpec& spec, const Choreography& choreography, const ModelCapabilities& capabilities) {
    std::set<std::string> staged;
    for (const ChoreographyEvent& e : choreography.events) {
        if (e.provenance.rfind("stage:", 0) == 0) {
            staged.insert(e.target);
        }
    }
    if (staged.empty()) {
        return 0;
    }
    // A row's place is where its target first appears.
    std::map<std::string, size_t> first;
    for (size_t i = 0; i < spec.effects.size(); ++i) {
        first.emplace(spec.effects[i].target, i);
    }
    std::vector<std::string> ahead; // groups to draw before the figures they hold
    for (const auto& [target, row] : first) {
        const TargetInfo* t = capabilities.Find(target);
        if (t == nullptr || t->type != TargetType::Group) {
            continue;
        }
        for (const std::string& m : t->flatModels) {
            if (staged.contains(m) && first.contains(m) && first.at(m) < row) {
                ahead.push_back(target);
                break;
            }
        }
    }
    if (ahead.empty()) {
        return 0;
    }
    std::sort(ahead.begin(), ahead.end(), [&](const std::string& a, const std::string& b) { return first.at(a) < first.at(b); }); // among themselves, as they were
    // Only their first effect moves: that alone sets the row; effects keep their times.
    std::vector<EffectSpec> moved;
    for (const std::string& g : ahead) {
        const size_t row = first.at(g);
        moved.push_back(spec.effects[row]);
        spec.effects[row].target.clear(); // marked, removed below
    }
    std::erase_if(spec.effects, [](const EffectSpec& e) { return e.target.empty(); });
    spec.effects.insert(spec.effects.begin(), moved.begin(), moved.end());
    return static_cast<int>(ahead.size());
}

const char* StageModeId(StageMode m) {
    for (const auto& [k, id] : ModeIds()) {
        if (k == m) {
            return id;
        }
    }
    return "group";
}

bool StageModeFromId(const std::string& id, StageMode& m) {
    for (const auto& [k, name] : ModeIds()) {
        if (id == name) {
            m = k;
            return true;
        }
    }
    return false;
}

const char* GestureKindId(GestureKind k) {
    for (const auto& [kind, id] : KindIds()) {
        if (kind == k) {
            return id;
        }
    }
    return "joint-pulse";
}

bool GestureKindFromId(const std::string& id, GestureKind& k) {
    for (const auto& [kind, name] : KindIds()) {
        if (id == name) {
            k = kind;
            return true;
        }
    }
    return false;
}

std::string StageModePhrase(StageMode m) {
    switch (m) {
    case StageMode::Protagonist: return COMPOSER_TEXT("One figure alone");
    case StageMode::Duo: return COMPOSER_TEXT("Two figures together");
    case StageMode::Group: return COMPOSER_TEXT("A group");
    case StageMode::Unison: return COMPOSER_TEXT("Together on the same hit");
    case StageMode::Propagation: return COMPOSER_TEXT("A wave across the house");
    case StageMode::LocalWaves: return COMPOSER_TEXT("Small waves of their own");
    case StageMode::Cascade: return COMPOSER_TEXT("One after another");
    case StageMode::CallResponse: return COMPOSER_TEXT("Call and answer");
    case StageMode::Counterpoint: return COMPOSER_TEXT("Different things at once");
    case StageMode::FullEnsemble: return COMPOSER_TEXT("The whole house");
    case StageMode::Rest: return COMPOSER_TEXT("Rest");
    }
    return "";
}

std::string GestureKindPhrase(GestureKind k) {
    switch (k) {
    case GestureKind::PropagatedWave: return COMPOSER_TEXT("A wave from one bulb across the house");
    case GestureKind::LocalWaves: return COMPOSER_TEXT("Each figure its own small wave");
    case GestureKind::Cascade: return COMPOSER_TEXT("A cascade, figure after figure");
    case GestureKind::JointPulse: return COMPOSER_TEXT("A flash all together");
    case GestureKind::CallResponse: return COMPOSER_TEXT("A call and its answers");
    case GestureKind::Echo: return COMPOSER_TEXT("An echo, weaker each time");
    case GestureKind::SpatialSweep: return COMPOSER_TEXT("A sweep across the house");
    case GestureKind::Expansion: return COMPOSER_TEXT("Light that spreads out");
    case GestureKind::Contraction: return COMPOSER_TEXT("Light that gathers in");
    case GestureKind::Counterpoint: return COMPOSER_TEXT("Two halves doing different things");
    case GestureKind::AmbientField: return COMPOSER_TEXT("A quiet glow joining figures");
    }
    return "";
}

std::string SceneRolePhrase(const std::string& role) {
    if (role == "protagonist") return COMPOSER_TEXT("Protagonist");
    if (role == "complement") return COMPOSER_TEXT("Accompanies");
    if (role == "responder") return COMPOSER_TEXT("Answers");
    if (role == "accent") return COMPOSER_TEXT("Accents");
    if (role == "background") return COMPOSER_TEXT("In the background");
    return COMPOSER_TEXT("Rests");
}

StageLibrary BuiltInStageLibrary() {
    StageLibrary l;
    const auto gesture = [&](GestureKind k, const char* description, std::vector<std::string> fns) {
        l.entries.push_back({ GestureKindId(k), "gesture", GestureKindPhrase(k), description, std::move(fns), {}, false, false, false, "built-in" });
    };
    gesture(GestureKind::PropagatedWave, COMPOSER_TEXT("A strong hit starts a wave in one bulb; it reaches each figure when its distance says."), { "wave" });
    gesture(GestureKind::LocalWaves, COMPOSER_TEXT("Several figures answer the same hit, each with a small wave of its own shape."), { "ripple" });
    gesture(GestureKind::Cascade, COMPOSER_TEXT("A few hits close together: one figure for each, in turn."), { "pulse" });
    gesture(GestureKind::JointPulse, COMPOSER_TEXT("Several figures flash together on one hit."), { "pulse" });
    gesture(GestureKind::CallResponse, COMPOSER_TEXT("The figure that calls, then the others answering a beat apart."), { "pulse" });
    gesture(GestureKind::Echo, COMPOSER_TEXT("As the sound dies away the wave comes again, weaker."), { "echo" });
    gesture(GestureKind::SpatialSweep, COMPOSER_TEXT("While the music grows, a sweep crosses the house."), { "sweep" });
    gesture(GestureKind::Expansion, COMPOSER_TEXT("When the music comes back, the light spreads from one point."), { "expansion" });
    gesture(GestureKind::Contraction, COMPOSER_TEXT("When the music drops, the light gathers back to one point."), { "expansion" });
    gesture(GestureKind::Counterpoint, COMPOSER_TEXT("With two voices, two halves of the house do different things."), { "flow", "chase" });
    gesture(GestureKind::AmbientField, COMPOSER_TEXT("Under a voice, a dim slow glow joins the figures around it."), { "flow" });
    l.entries.push_back({ "local:identity", "local", COMPOSER_TEXT("A figure's own colours"),
                          COMPOSER_TEXT("Each part in its own colour (foliage, trunk...) under everything; the mouth and eyes left to the face."), { "pulse" }, {}, false, false, false,
                          "built-in" });
    l.entries.push_back({ "local:breath", "local", COMPOSER_TEXT("A slow breath"), COMPOSER_TEXT("The whole figure fading slowly in and out."), { "fade" }, {}, false, false, false, "built-in" });
    l.entries.push_back({ "local:spiral", "local", COMPOSER_TEXT("A spiral round a tree"), COMPOSER_TEXT("Arms turning round the figure's centre."), { "spiral" }, { "tree", "tree-360" }, false,
                          false, false, "built-in" });
    return l;
}

StageLibrary MergeStageLibrary(const StageLibrary& personal) {
    StageLibrary l = BuiltInStageLibrary();
    for (const StageLibraryEntry& p : personal.entries) {
        bool found = false;
        for (StageLibraryEntry& e : l.entries) {
            if (e.id == p.id) {
                e.favorite = p.favorite;
                e.excluded = p.excluded;
                found = true;
            }
        }
        if (!found && p.custom) {
            l.entries.push_back(p);
        }
    }
    return l;
}

namespace {
json GestureJson(const GlobalGesture& g) {
    json ps = json::array();
    for (const GestureParticipant& p : g.participants) {
        ps.push_back({ { "figure", p.figure }, { "role", p.role }, { "delayMS", p.delayMS }, { "level", p.level }, { "function", p.function }, { "reason", p.reason } });
    }
    return { { "id", g.id }, { "kind", GestureKindId(g.kind) }, { "origin", g.origin }, { "ox", g.ox }, { "oy", g.oy }, { "trajectory", g.trajectory }, { "angle", g.angle },
             { "participants", ps }, { "excluded", g.excluded }, { "speed", g.speed }, { "width", g.width }, { "loss", g.loss }, { "level", g.level }, { "startMS", g.startMS },
             { "endMS", g.endMS }, { "envelope", g.envelope }, { "colors", g.colors }, { "trigger", g.trigger }, { "related", g.related }, { "confidence", g.confidence },
             { "reasons", g.reasons }, { "manual", g.manual } };
}
} // namespace

std::string ToJson(const GlobalStageScore& s) {
    json scenes = json::array();
    for (const StageScene& sc : s.scenes) {
        json roles = json::array();
        for (const SceneRole& r : sc.roles) {
            roles.push_back({ { "figure", r.figure }, { "role", r.role }, { "why", r.why } });
        }
        scenes.push_back({ { "id", sc.id }, { "startMS", sc.startMS }, { "endMS", sc.endMS }, { "section", sc.sectionKind }, { "mode", StageModeId(sc.mode) }, { "context", sc.context },
                           { "budget", sc.budget > 1e8 ? -1.0 : sc.budget }, { "spent", sc.spent }, { "roles", roles }, { "gestures", sc.gestures }, { "motif", sc.motif },
                           { "reasons", sc.reasons }, { "confidence", sc.confidence }, { "manual", sc.manual } });
    }
    json gestures = json::array();
    for (const GlobalGesture& g : s.gestures) {
        gestures.push_back(GestureJson(g));
    }
    json layers = json::array();
    for (const StageLayer& l : s.layers) {
        layers.push_back({ { "figure", l.figure }, { "role", l.role }, { "region", l.region }, { "source", l.source }, { "color", l.color }, { "only", l.only }, { "skip", l.skip },
                           { "order", l.order }, { "opacity", l.opacity }, { "blend", l.blend }, { "startMS", l.startMS }, { "endMS", l.endMS } });
    }
    return json({ { "kind", "composer-stage-score" }, { "schemaVersion", s.schemaVersion }, { "version", s.version }, { "durationMS", s.durationMS }, { "scenes", scenes },
                  { "gestures", gestures }, { "layers", layers }, { "limits", s.limits }, { "warnings", s.warnings } })
        .dump(1);
}

bool FromJson(const std::string& text, GlobalStageScore& out, std::string& error) {
    try {
        const json root = json::parse(text);
        if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > STAGE_SCORE_SCHEMA_VERSION) {
            error = "stage score: schema not read";
            return false;
        }
        GlobalStageScore s;
        s.version = root.value("version", std::string());
        s.durationMS = root.value("durationMS", 0);
        for (const json& j : root.value("scenes", json::array())) {
            StageScene sc;
            sc.id = j.value("id", std::string());
            sc.startMS = j.value("startMS", 0);
            sc.endMS = j.value("endMS", 0);
            sc.sectionKind = j.value("section", std::string());
            (void)StageModeFromId(j.value("mode", std::string("group")), sc.mode);
            sc.context = j.value("context", std::string());
            sc.budget = j.value("budget", 0.0);
            sc.spent = j.value("spent", 0.0);
            for (const json& r : j.value("roles", json::array())) {
                sc.roles.push_back({ r.value("figure", std::string()), r.value("role", std::string()), r.value("why", std::string()) });
            }
            sc.gestures = j.value("gestures", std::vector<std::string>());
            sc.motif = j.value("motif", std::string());
            sc.reasons = j.value("reasons", std::vector<std::string>());
            sc.confidence = j.value("confidence", 0.0);
            sc.manual = j.value("manual", false);
            s.scenes.push_back(std::move(sc));
        }
        for (const json& j : root.value("gestures", json::array())) {
            GlobalGesture g;
            g.id = j.value("id", std::string());
            (void)GestureKindFromId(j.value("kind", std::string()), g.kind);
            g.origin = j.value("origin", std::string());
            g.ox = j.value("ox", 0.5);
            g.oy = j.value("oy", 0.5);
            g.trajectory = j.value("trajectory", std::string("radial"));
            g.angle = j.value("angle", 0.0);
            for (const json& p : j.value("participants", json::array())) {
                g.participants.push_back({ p.value("figure", std::string()), p.value("role", std::string()), p.value("delayMS", 0), p.value("level", 1.0), p.value("function", std::string()),
                                           p.value("reason", std::string()) });
            }
            g.excluded = j.value("excluded", std::vector<std::string>());
            g.speed = j.value("speed", 0.6);
            g.width = j.value("width", 0.08);
            g.loss = j.value("loss", 0.0);
            g.level = j.value("level", 1.0);
            g.startMS = j.value("startMS", 0);
            g.endMS = j.value("endMS", 0);
            g.envelope = j.value("envelope", std::string("short"));
            g.colors = j.value("colors", std::vector<std::string>());
            g.trigger = j.value("trigger", std::string());
            g.related = j.value("related", std::string());
            g.confidence = j.value("confidence", 0.0);
            g.reasons = j.value("reasons", std::vector<std::string>());
            g.manual = j.value("manual", false);
            s.gestures.push_back(std::move(g));
        }
        for (const json& j : root.value("layers", json::array())) {
            StageLayer l;
            l.figure = j.value("figure", std::string());
            l.role = j.value("role", std::string());
            l.region = j.value("region", std::string());
            l.source = j.value("source", std::string());
            l.color = j.value("color", std::string());
            l.only = j.value("only", std::string());
            l.skip = j.value("skip", std::string());
            l.order = j.value("order", 0);
            l.opacity = j.value("opacity", 1.0);
            l.blend = j.value("blend", std::string("Normal"));
            l.startMS = j.value("startMS", 0);
            l.endMS = j.value("endMS", 0);
            s.layers.push_back(std::move(l));
        }
        s.limits = root.value("limits", std::vector<std::string>());
        s.warnings = root.value("warnings", std::vector<std::string>());
        out = std::move(s);
    } catch (const json::exception& e) {
        error = std::string("stage score: ") + e.what();
        return false;
    }
    return true;
}

std::string ToJson(const StageEdits& e) {
    json list = json::array();
    for (const StageEdit& x : e.edits) {
        list.push_back({ { "action", x.action }, { "sceneStartMS", x.sceneStartMS }, { "atMS", x.atMS }, { "value", x.value }, { "note", x.note } });
    }
    return json({ { "kind", "composer-stage-edits" }, { "schemaVersion", e.schemaVersion }, { "edits", list } }).dump(1);
}

bool FromJson(const std::string& text, StageEdits& out, std::string& error) {
    try {
        const json root = json::parse(text);
        if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > STAGE_SCORE_SCHEMA_VERSION) {
            error = "stage edits: schema not read";
            return false;
        }
        StageEdits e;
        for (const json& x : root.value("edits", json::array())) {
            e.edits.push_back({ x.value("action", std::string()), x.value("sceneStartMS", -1), x.value("atMS", -1), x.value("value", std::string()), x.value("note", std::string()) });
        }
        out = std::move(e);
    } catch (const json::exception& ex) {
        error = std::string("stage edits: ") + ex.what();
        return false;
    }
    return true;
}

std::string ToJson(const StageLibrary& l) {
    json list = json::array();
    for (const StageLibraryEntry& e : l.entries) {
        list.push_back({ { "id", e.id }, { "kind", e.kind }, { "title", e.title }, { "description", e.description }, { "functions", e.functions }, { "fits", e.fits },
                         { "favorite", e.favorite }, { "excluded", e.excluded }, { "custom", e.custom }, { "source", e.source } });
    }
    return json({ { "kind", "composer-stage-library" }, { "schemaVersion", l.schemaVersion }, { "entries", list } }).dump(1);
}

bool FromJson(const std::string& text, StageLibrary& out, std::string& error) {
    try {
        const json root = json::parse(text);
        if (root.value("schemaVersion", 0) < 1 || root.value("schemaVersion", 0) > STAGE_SCORE_SCHEMA_VERSION) {
            error = "stage library: schema not read";
            return false;
        }
        StageLibrary l;
        for (const json& e : root.value("entries", json::array())) {
            l.entries.push_back({ e.value("id", std::string()), e.value("kind", std::string()), e.value("title", std::string()), e.value("description", std::string()),
                                  e.value("functions", std::vector<std::string>()), e.value("fits", std::vector<std::string>()), e.value("favorite", false), e.value("excluded", false),
                                  e.value("custom", false), e.value("source", std::string("person")) });
        }
        out = std::move(l);
    } catch (const json::exception& ex) {
        error = std::string("stage library: ") + ex.what();
        return false;
    }
    return true;
}

std::string StageEditsPath(const std::string& showDir, const std::string& audioHash) {
    return (fs::path(showDir) / "ShowComposer" / "stage" / (audioHash.substr(0, 16) + ".edits.json")).string();
}

std::string StageLibraryPath(const std::string& showDir) {
    return (fs::path(showDir) / "ShowComposer" / "stage" / "library.json").string();
}

} // namespace composer
