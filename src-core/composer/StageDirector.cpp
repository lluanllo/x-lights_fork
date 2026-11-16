#include "composer/StageDirector.h"

#include "composer/AudioUnderstanding.h"
#include "composer/CompositionSelector.h"
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
        score.art = static_cast<int>(_in.art);
        if (Art(ArtLevel::Direction)) {
            score.version = STAGE_DIRECTOR_ART_VERSION;
            Arc();
        }
        if (Art(ArtLevel::Compositions)) {
            score.version = STAGE_DIRECTOR_ORCHESTRA_VERSION;
        }
        for (const PlanSegment& s : _in.plan->segments) {
            score.scenes.push_back(Scene(s, score));
        }
        ApplyEdits(score);
        Identity(score);
        if (Art(ArtLevel::Direction)) {
            Connect(score);
        }
        if (Art(ArtLevel::Presence)) {
            score.ambientLevel = std::clamp(_in.ambientLevel, 0.05, 1.0);
            for (StageScene& sc : score.scenes) {
                Presence(sc, score);
            }
        }
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
        // The scene's role in the song, and the visual idea it carries.
        _prefer.clear();
        _avoid.clear();
        _target = 0;
        _motifId.clear();
        _hasMotifKind = false;
        _motifLevel = 1;
        _motifAnchor.clear();
        _carried = false;
        if (Art(ArtLevel::Direction)) {
            sc.narrative = _sceneIndex < _narrative.size() ? _narrative[_sceneIndex] : "development";
            if (sc.context == "choir" && sc.narrative == "development") {
                sc.narrative = "answer";
            }
            if (sc.mode == StageMode::Rest) {
                sc.narrative = "rest";
            }
        }
        if (Art(ArtLevel::Full) && !sc.manual) {
            Participation(sc);
        }
        if (Art(ArtLevel::Motifs)) {
            PickMotif(sc, seg, score);
        }
        _comp = nullptr;
        if (Art(ArtLevel::Compositions)) {
            Compose(sc, seg, faces, voices, score);
        }
        // The attention a scene can spend: a quiet scene little, a peak all of it.
        sc.budget = sc.mode == StageMode::Rest ? 0.5 : climax ? 1e9 : seg.priority >= 2 ? 6.0 : sc.mode == StageMode::Protagonist ? 3.0 : 4.5;
        Roles(sc, seg, faces);
        if (Art(ArtLevel::Full)) {
            Open(sc, seg, faces, score);
        }
        Gestures(sc, seg, faces, score);
        if (Art(ArtLevel::Full)) {
            Prepare(sc, seg, faces, score);
        }
        if (Art(ArtLevel::Direction)) {
            Attention(sc, score);
        }
        if (Art(ArtLevel::Motifs)) {
            RecordMotif(sc, seg, score);
        }
        for (const SceneRole& r : sc.roles) {
            sc.spent += Cost(r.role);
            _lastUsed[r.figure] = r.role == "rest" ? _lastUsed[r.figure] : _sceneIndex;
        }
        ++_sceneIndex;
        return sc;
    }

    // ------------------------------------------------------------- the song's arc (phase 20)
    static bool Silent(const PlanSegment& s) {
        return std::find(s.flags.begin(), s.flags.end(), "silence") != s.flags.end() || s.priority == 0;
    }

    static std::string PartKind(const std::string& sectionKind) {
        return sectionKind == "interlude" || sectionKind == "solo" || sectionKind == "breakdown" ? "instrumental" : sectionKind;
    }

    static bool Says(const std::vector<std::string>* parts, const std::string& kind) {
        return parts != nullptr && std::find(parts->begin(), parts->end(), kind) != parts->end();
    }

    // What each scene does in the song, from its own structure: the first presents, a part
    // that comes back recovers, the last one played closes, the one before a peak prepares
    // it. A song with no repeats has nothing to recover and one with no peak prepares none:
    // nothing is imposed that the music does not have.
    void Arc() {
        const std::vector<PlanSegment>& segs = _in.plan->segments;
        _narrative.assign(segs.size(), "development");
        std::vector<size_t> live;
        std::map<int, int> count;
        std::map<int, double> priority;
        std::map<int, bool> chorus;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (!Silent(segs[i])) {
                live.push_back(i);
            }
            if (segs[i].repeatGroup >= 0) {
                count[segs[i].repeatGroup]++;
                priority[segs[i].repeatGroup] += segs[i].priority;
                chorus[segs[i].repeatGroup] = chorus[segs[i].repeatGroup] || segs[i].sectionKind == "chorus";
            }
        }
        // The song's returning idea: the chorus that comes back, else the part that comes back most;
        // a second part that comes back brings the contrasting idea.
        const auto better = [&](int g, int than) {
            return than < 0 || (chorus[g] && !chorus[than]) || (chorus[g] == chorus[than] && (count[g] > count[than] || (count[g] == count[than] && priority[g] > priority[than])));
        };
        int main = -1;
        for (const auto& [g, n] : count) {
            if (n >= 2 && better(g, main)) {
                main = g;
            }
        }
        int second = -1;
        for (const auto& [g, n] : count) {
            if (n >= 2 && g != main && better(g, second)) {
                second = g;
            }
        }
        _mainGroup = main;
        // The peak: the song's own (a part it marks as its peak), else the last block of its
        // returning part, unless that block is clearly quieter than the part's first time.
        bool marked = false;
        for (const PlanSegment& s : segs) {
            marked = marked || (!Silent(s) && s.priority >= 3);
        }
        std::set<size_t> peak;
        if (!marked && main >= 0) {
            size_t last = segs.size();
            for (size_t i = segs.size(); i-- > 0;) {
                if (segs[i].repeatGroup == main) {
                    last = i;
                    break;
                }
            }
            double first = -1;
            for (const PlanSegment& s : segs) {
                if (s.repeatGroup == main) {
                    first = s.targetIntensity > 0 ? s.targetIntensity : s.energy;
                    break;
                }
            }
            if (last < segs.size()) {
                size_t from = last;
                while (from > 0 && segs[from - 1].repeatGroup == main) {
                    --from;
                }
                double energy = 0;
                for (size_t i = from; i <= last; ++i) {
                    energy = std::max(energy, segs[i].targetIntensity > 0 ? segs[i].targetIntensity : segs[i].energy);
                }
                // Not the part's first time, and not clearly quieter than it.
                const bool firstTime = from == 0 || std::none_of(segs.begin(), segs.begin() + static_cast<long>(from), [&](const PlanSegment& x) { return x.repeatGroup == main; });
                if (!firstTime && energy >= first - 0.05) {
                    for (size_t i = from; i <= last; ++i) {
                        peak.insert(i);
                    }
                }
            }
        }
        _peak = peak;
        std::map<int, int> seen;
        std::map<int, double> firstEnergy;
        for (size_t i = 0; i < segs.size(); ++i) {
            const PlanSegment& s = segs[i];
            const int g = s.repeatGroup;
            const int occ = g >= 0 ? seen[g]++ : 0;
            const double energy = s.targetIntensity > 0 ? s.targetIntensity : s.energy;
            if (g >= 0 && occ == 0) {
                firstEnergy[g] = energy;
            }
            std::string& out = _narrative[i];
            if (Silent(s)) {
                out = "rest";
                continue;
            }
            const size_t pos = static_cast<size_t>(std::find(live.begin(), live.end(), i) - live.begin());
            const PlanSegment* next = pos + 1 < live.size() ? &segs[live[pos + 1]] : nullptr;
            const std::string kind = PartKind(s.sectionKind);
            const bool nextIsPeak = next != nullptr && (next->priority >= 3 || peak.contains(live[pos + 1]));
            const bool transforms = ((kind == "instrumental" || kind == "bridge") && !Says(_in.keepParts, kind)) || Says(_in.changeParts, kind);
            if (pos == 0) {
                out = "presentation";
            } else if (live.size() >= 3 && pos + 1 == live.size()) {
                out = "resolution";
            } else if (s.priority >= 3) {
                out = occ > 0 ? "intensification" : "climax";
            } else if (peak.contains(i)) {
                out = "intensification";
            } else if (g < 0 && transforms) {
                out = "transformation";
            } else if (nextIsPeak && !(g == main && occ > 0)) {
                out = "preparation";
            } else if (g >= 0 && occ > 0) {
                out = "recovery";
            } else if (g >= 0 && g == second) {
                out = "contrast";
            } else if (transforms) {
                out = "transformation";
            } else {
                out = "development";
            }
            (void)energy;
        }
    }

    // ------------------------------------------------------------- visual motifs (phase 20)
    static VisualMotif* FindMotif(GlobalStageScore& score, const std::string& id) {
        for (VisualMotif& m : score.motifs) {
            if (m.id == id) {
                return &m;
            }
        }
        return nullptr;
    }

    // The gesture a motif turns into when the music turns: the same idea, travelling another way.
    static GestureKind Transformed(GestureKind k) {
        switch (k) {
        case GestureKind::LocalWaves: return GestureKind::PropagatedWave;
        case GestureKind::JointPulse: return GestureKind::PropagatedWave;
        case GestureKind::PropagatedWave: return GestureKind::SpatialSweep;
        case GestureKind::SpatialSweep: return GestureKind::Expansion;
        case GestureKind::Expansion: return GestureKind::PropagatedWave;
        case GestureKind::Contraction: return GestureKind::Expansion;
        case GestureKind::Echo: return GestureKind::PropagatedWave;
        case GestureKind::CallResponse: return GestureKind::Counterpoint;
        case GestureKind::Counterpoint: return GestureKind::CallResponse;
        case GestureKind::AmbientField: return GestureKind::LocalWaves;
        case GestureKind::Cascade: return GestureKind::PropagatedWave;
        }
        return k;
    }

    // Which idea a scene carries and what it does with it. A part that comes back brings its
    // own idea back; the first scene and the song's returning part present one each.
    void PickMotif(StageScene& sc, const PlanSegment& seg, GlobalStageScore& score) {
        const int g = seg.repeatGroup;
        const int occ = g >= 0 ? _seenGroup[g]++ : 0;
        const std::string& n = sc.narrative;
        if (n == "rest" || sc.mode == StageMode::Rest) {
            return;
        }
        std::string id, op;
        if (g >= 0 && occ > 0 && _groupMotif.contains(g)) {
            id = _groupMotif.at(g);
            op = n == "intensification" || n == "climax" ? "intensify" : n == "resolution" ? "resolve" : n == "preparation" ? "reduce" : "recall";
        } else if (n == "presentation" || n == "contrast" || score.motifs.empty()) {
            op = "present";
        } else {
            // The instrumental transforms the idea the returning part carries; the last scene
            // closes the first idea; anything else develops the idea in play.
            id = n == "resolution" ? score.motifs.front().id
               : n == "transformation" && _mainGroup >= 0 && _groupMotif.contains(_mainGroup) ? _groupMotif.at(_mainGroup)
                                                                                              : _current;
            op = n == "answer" ? "split" : n == "preparation" ? "reduce" : n == "climax" || n == "intensification" ? "intensify" : n == "transformation" ? "transform"
               : n == "resolution" ? "resolve" : n == "recovery" ? "recall" : "develop";
        }
        sc.motifOperation = op;
        if (op == "present") {
            sc.visualMotif = "M" + std::to_string(score.motifs.size() + 1);
            if (n == "contrast" && !score.motifs.empty()) {
                // The chorus's idea answers the first one: other figures carry it (the voice stays).
                const VisualMotif& first = score.motifs.front();
                _avoid.insert(first.figures.begin(), first.figures.end());
            }
            _motifId = sc.visualMotif;
            return;
        }
        VisualMotif* m = FindMotif(score, id);
        if (m == nullptr) {
            sc.motifOperation.clear();
            return;
        }
        sc.visualMotif = m->id;
        _motifId = m->id;
        std::vector<std::string> mine;
        if (!m->protagonist.empty()) {
            mine.push_back(m->protagonist);
        }
        // A part that comes back brings back the figures it had the first time.
        const std::vector<std::string>* figures = op == "recall" && g >= 0 && _groupFigures.contains(g) ? &_groupFigures.at(g) : &m->figures;
        for (const std::string& f : *figures) {
            if (std::find(mine.begin(), mine.end(), f) == mine.end()) {
                mine.push_back(f);
            }
        }
        if (op == "reduce") {
            mine.resize(std::min<size_t>(mine.size(), 2));
        } else if (op == "resolve") {
            mine.resize(std::min<size_t>(mine.size(), 3));
        }
        _prefer = mine;
        GestureKind k;
        if (!m->gestureKind.empty() && GestureKindFromId(m->gestureKind, k)) {
            _hasMotifKind = true;
            _motifKind = op == "transform" ? Transformed(k)
                       : op == "split"     ? GestureKind::CallResponse
                       : op == "resolve"   ? (k == GestureKind::PropagatedWave || k == GestureKind::Expansion || k == GestureKind::SpatialSweep ? GestureKind::Echo : k)
                                           : k;
        }
        _motifLevel = op == "reduce" ? 0.7 : op == "resolve" ? 0.6 : 1.0;
        _motifAnchor = m->anchor;
        if (_target < 0) {
            // The idea's own figures; each time it develops one more takes it up (up to three
            // more), so it grows through the song; an answer needs a few on each side.
            const int own = static_cast<int>(m->figures.size());
            const int grown = op == "develop" ? ++_developed[m->id] : 0;
            _target = op == "develop" ? own + std::min(grown, 3) : op == "split" ? std::max(own, 4) : std::max(static_cast<int>(mine.size()), 2);
        }
    }

    // A peak is prepared and an ending closes (ArtLevel Full): the scene before a peak grows
    // towards it with a sweep in its last bars, the peak opens from where the idea was born,
    // and the last scene gathers the light back.
    void Prepare(StageScene& sc, const PlanSegment& seg, const std::vector<std::string>& faces, GlobalStageScore& score) {
        const auto free = [&](int ms) {
            if (ms < sc.startMS || ms >= sc.endMS - 300) {
                return false;
            }
            for (const std::string& id : sc.gestures) {
                const GlobalGesture* g = score.Gesture(id);
                if (g != nullptr && ms > g->startMS - 1500 && ms < g->endMS + 300) {
                    return false;
                }
            }
            return true;
        };
        const double beat = Beat(sc.startMS);
        const int bars = static_cast<int>(beat * 4000);
        GestureKind kind;
        Moment moment;
        std::string why;
        if (sc.narrative == "preparation" || (BeforePeak() && sc.narrative != "rest")) {
            kind = GestureKind::SpatialSweep;
            // As late as there is room, beat by beat back from a bar before the peak.
            int at = -1;
            for (int t = sc.endMS - bars / 4; t >= std::max(sc.startMS, sc.endMS - 2 * bars); t -= std::max(100, bars / 4)) {
                if (free(t)) {
                    at = t;
                    break;
                }
            }
            if (at < 0) {
                return;
            }
            moment = { at, "anticipation", 0.7 };
            why = "the peak is near: the light grows towards it";
        } else if (sc.narrative == "resolution") {
            kind = GestureKind::Contraction;
            moment = { std::max(sc.startMS, sc.endMS - 2 * bars), "closing", 0.6 };
            why = "the song closes: the light gathers back, figure by figure";
        } else {
            return;
        }
        if (!free(moment.ms) || !Allowed(kind)) {
            return;
        }
        const bool carry = !_motifId.empty() && (kind == GestureKind::Expansion || (kind == GestureKind::Contraction && sc.motifOperation == "resolve"));
        _carrying = carry;
        const bool made = Make(sc, seg, kind, moment, faces, score);
        _carrying = false;
        if (made) {
            GlobalGesture& g = score.gestures.back();
            g.reasons.insert(g.reasons.begin(), why);
            if (kind == GestureKind::Contraction) {
                g.level = std::min(g.level, 0.6);
                g.relation = "progressive-end";
            }
        }
    }

    // The scene right before the peak (not part of it).
    bool BeforePeak() const {
        return _sceneIndex + 1 < _narrative.size() && (_narrative[_sceneIndex + 1] == "climax" || _narrative[_sceneIndex + 1] == "intensification") &&
               _narrative[_sceneIndex] != "intensification" && _narrative[_sceneIndex] != "climax" && _narrative[_sceneIndex] != "rest";
    }

    // Where the peak begins it opens from where the idea was born: placed before the scene's
    // other gestures, which fit round it.
    void Open(StageScene& sc, const PlanSegment& seg, const std::vector<std::string>& faces, GlobalStageScore& score) {
        const bool peak = sc.narrative == "climax" || (sc.narrative == "intensification" && sc.mode == StageMode::FullEnsemble);
        const bool begins = _sceneIndex == 0 || (_narrative[_sceneIndex - 1] != "climax" && _narrative[_sceneIndex - 1] != "intensification");
        if (!peak || !begins || !Allowed(GestureKind::Expansion)) {
            return;
        }
        _carrying = !_motifId.empty();
        const bool made = Make(sc, seg, GestureKind::Expansion, { sc.startMS, "climax-open", 0.9 }, faces, score);
        _carrying = false;
        if (made) {
            _carried = _carried || !_motifId.empty();
            score.gestures.back().reasons.insert(score.gestures.back().reasons.begin(), "the peak opens from where the idea was born");
        }
    }

    void Attention(StageScene& sc, const GlobalStageScore& score) const {
        int on = 0;
        for (const SceneRole& r : sc.roles) {
            if (r.role != "rest") {
                ++on;
                if (sc.accentFigure.empty() && r.role == "accent") {
                    sc.accentFigure = r.figure;
                }
            }
        }
        if (sc.accentFigure.empty()) {
            for (const SceneRole& r : sc.roles) {
                if (r.role == "protagonist") {
                    sc.accentFigure = r.figure;
                    break;
                }
            }
        }
        const int all = static_cast<int>(_figures.size());
        sc.attention = sc.mode == StageMode::Rest || on == 0 ? "rest"
                     : sc.mode == StageMode::Propagation       ? "propagation"
                     : on >= all                               ? "all"
                     : on == 1                                 ? "one"
                     : on == 2                                 ? "two"
                     : on <= 4                                 ? "few"
                                                               : "group";
        (void)score;
    }

    // A scene that presents an idea makes it a motif; every other scene that uses one adds
    // what it did with it.
    void RecordMotif(const StageScene& sc, const PlanSegment& seg, GlobalStageScore& score) {
        if (sc.visualMotif.empty()) {
            return;
        }
        std::vector<std::string> on;
        std::string protagonist;
        for (const SceneRole& r : sc.roles) {
            if (r.role == "protagonist" && protagonist.empty()) {
                protagonist = r.figure;
            }
            if (r.role != "rest" && r.role != "background") {
                on.push_back(r.figure);
            }
        }
        const GlobalGesture* carrier = nullptr;
        for (const std::string& id : sc.gestures) {
            const GlobalGesture* g = score.Gesture(id);
            if (g != nullptr && g->motif == sc.visualMotif) {
                carrier = g;
                break;
            }
        }
        if (carrier == nullptr && sc.motifOperation == "present") {
            for (const std::string& id : sc.gestures) {
                const GlobalGesture* g = score.Gesture(id);
                if (g != nullptr && g->kind != GestureKind::AmbientField && (carrier == nullptr || g->participants.size() > carrier->participants.size())) {
                    carrier = g;
                }
            }
        }
        VisualMotif* m = FindMotif(score, sc.visualMotif);
        if (m == nullptr) {
            if (sc.motifOperation != "present") {
                return;
            }
            score.motifs.push_back({});
            m = &score.motifs.back();
            m->id = sc.visualMotif;
            m->intent = score.motifs.size() == 1 ? "the song's idea" : sc.narrative == "contrast" ? "the returning part's idea, answering the first" : "a new idea";
            m->origin = "scene:" + sc.id;
            m->protagonist = protagonist.empty() && !on.empty() ? on.front() : protagonist;
            for (const std::string& f : on) {
                if (f != m->protagonist) {
                    m->companions.push_back(f);
                }
            }
            m->figures = on;
            if (_in.choreography != nullptr) {
                for (const ChoreographyEvent& e : _in.choreography->events) {
                    if (e.segmentId == sc.id && e.prominence == Prominence::Lead && std::find(on.begin(), on.end(), e.target) != on.end() && !m->recipes.contains(e.target)) {
                        m->recipes[e.target] = e.effect;
                    }
                }
            }
            m->composition = sc.composition;
            m->entry = "from-one";
            m->exit = "fades";
            for (const std::string& f : on) {
                if (_locked.contains(f)) {
                    m->constraints.push_back("locked:" + f);
                }
                const Figure* fig = Find(f);
                if (fig != nullptr && fig->face) {
                    m->constraints.push_back("face-sings:" + f);
                }
            }
            m->explanation = sc.narrative == "presentation" ? "presented first: the rest of the song develops, answers and brings it back"
                                                            : "presented where the song's returning part first plays: it comes back with it";
            if (seg.repeatGroup >= 0 && !_groupMotif.contains(seg.repeatGroup)) {
                _groupMotif[seg.repeatGroup] = m->id;
            }
        } else if (seg.repeatGroup >= 0 && !_groupMotif.contains(seg.repeatGroup) && _seenGroup[seg.repeatGroup] == 1) {
            _groupMotif[seg.repeatGroup] = m->id; // the part's first time carries this idea: its returns bring it back
        }
        if (seg.repeatGroup >= 0 && _seenGroup[seg.repeatGroup] == 1 && !_groupFigures.contains(seg.repeatGroup)) {
            _groupFigures[seg.repeatGroup] = on;
        }
        // The idea's gesture is the first that carries it.
        if (m->gestureKind.empty() && carrier != nullptr) {
            m->gestureKind = GestureKindId(carrier->kind);
            m->movement = carrier->participants.empty() ? std::string() : carrier->participants.front().function;
            m->temporal = carrier->relation;
            m->anchor = carrier->origin;
            m->triggers.push_back(carrier->trigger.substr(0, carrier->trigger.find('@')));
            m->entry = carrier->relation == "unison" ? "together" : "from-one";
            const size_t colon = carrier->origin.find(':');
            const std::string from = carrier->origin.rfind("node:", 0) == 0 ? carrier->origin.substr(colon + 1, carrier->origin.rfind(':') - colon - 1) : std::string();
            m->spatial = carrier->trajectory == "linear" ? "across the house" : !from.empty() ? "outwards from " + from : carrier->relation == "answer" ? "from one figure to its neighbours" : "each figure on its own";
        }
        if (sc.motifOperation != "reduce" && sc.motifOperation != "resolve" && sc.motifOperation != "transform") {
            _current = m->id;
        }
        MotifVariant v;
        v.scene = sc.id;
        v.startMS = sc.startMS;
        v.operation = sc.motifOperation;
        v.narrative = sc.narrative;
        v.figures = on;
        v.gesture = carrier != nullptr ? carrier->id : std::string();
        v.kind = carrier != nullptr ? GestureKindId(carrier->kind) : std::string();
        v.level = carrier != nullptr ? carrier->level : 1;
        v.why = MotifOperationPhrase(sc.motifOperation);
        m->variants.push_back(std::move(v));
    }

    // What changes from one scene to the next and how they connect, in plain words keys.
    void Connect(GlobalStageScore& score) const {
        for (size_t i = 0; i < score.scenes.size(); ++i) {
            StageScene& sc = score.scenes[i];
            const StageScene* prev = i > 0 ? &score.scenes[i - 1] : nullptr;
            const StageScene* next = i + 1 < score.scenes.size() ? &score.scenes[i + 1] : nullptr;
            const auto count = [](const StageScene& s) {
                int n = 0;
                for (const SceneRole& r : s.roles) {
                    n += r.role != "rest";
                }
                return n;
            };
            if (prev == nullptr) {
                sc.changes = "the show begins";
            } else if (sc.narrative == "rest") {
                sc.changes = "the house rests";
            } else if (!sc.visualMotif.empty() && sc.visualMotif != prev->visualMotif && sc.motifOperation == "present") {
                sc.changes = "a new idea appears";
            } else if (!sc.visualMotif.empty() && sc.visualMotif != prev->visualMotif) {
                sc.changes = "an earlier idea comes back";
            } else if (count(sc) > count(*prev) + 1) {
                sc.changes = "more figures join";
            } else if (count(sc) + 1 < count(*prev)) {
                sc.changes = "the light gathers on fewer figures";
            } else {
                sc.changes = "the same idea carries on";
            }
            if (next == nullptr) {
                sc.leadsTo = "the song ends";
            } else if (next->narrative == "climax" || (next->narrative == "intensification" && next->mode == StageMode::FullEnsemble)) {
                sc.leadsTo = "it prepares the peak";
            } else if (next->narrative == "rest") {
                sc.leadsTo = "it fades into a rest";
            } else if (!next->visualMotif.empty() && next->visualMotif == sc.visualMotif) {
                sc.leadsTo = "the idea carries on";
            } else if (!next->visualMotif.empty()) {
                sc.leadsTo = "it hands over to another idea";
            } else {
                sc.leadsTo = "it hands over to the next scene";
            }
        }
    }

    // How many figures carry each kind of scene (ArtLevel Full): few to present, prepare and
    // close; the idea's own figures when it comes back; everyone at a peak.
    void Participation(StageScene& sc) {
        const int all = static_cast<int>(_figures.size());
        const std::string& n = sc.narrative;
        if (n == "presentation") {
            _target = 3;
            sc.focus = true;
        } else if (n == "preparation") {
            _target = 2;
            sc.focus = true;
        } else if (n == "resolution") {
            _target = 3;
            sc.focus = true;
        } else if (n == "contrast") {
            _target = 4;
        } else if (n == "development" || n == "answer" || n == "recovery") {
            _target = -1; // from the motif
        } else if (n == "climax" || n == "intensification" || n == "transformation") {
            _target = all;
            if (n == "intensification" && sc.context != "narration" && sc.context != "duet" && sc.mode != StageMode::FullEnsemble) {
                sc.mode = StageMode::FullEnsemble;
                sc.reasons.push_back("the idea comes back stronger: the whole house joins");
            }
        }
        if (sc.focus) {
            sc.reasons.push_back("the attention is on a few figures: the others rest");
        }
    }

    // ------------------------------------------------------------- compositions (phase 21)
    // The strong hits of a stretch, and whether a short burst of them stands alone (the
    // cascade's own test).
    std::vector<Moment> StrongAccents(int a, int b) const {
        std::vector<Moment> out;
        for (const Moment& m : MomentsIn(a, b)) {
            if (m.kind == "accent" && m.weight >= 0.45) {
                out.push_back(m);
            }
        }
        return out;
    }

    bool Burst(const StageScene& sc) const {
        const std::vector<Moment> accents = StrongAccents(sc.startMS, sc.endMS);
        for (size_t i = 0; i < accents.size();) {
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
            if (count >= 3 && count <= 5 && strongEach && before >= widest * 3 / 2 && after >= widest * 3 / 2) {
                return true;
            }
            i = j;
        }
        return false;
    }

    // The figures a composition can use here: on the stage, not locked by a person, not
    // only a face, not left out of the scene by a person.
    std::vector<const Figure*> Usable() const {
        std::vector<const Figure*> out;
        for (const Figure& f : _figures) {
            if (!f.allFace && !_locked.contains(f.stage->name) && !_excludedHere.contains(f.stage->name)) {
                out.push_back(&f);
            }
        }
        return out;
    }

    // The composition the scene is built on, chosen from the library for the whole of its
    // circumstances, and the scene fitted to it: how many figures take part, who leads, how
    // the house is used. Its gesture becomes the scene's main gesture.
    void Compose(StageScene& sc, const PlanSegment& seg, const std::vector<std::string>& faces, const std::set<std::string>& voices, const GlobalStageScore& score) {
        if (_in.library == nullptr) {
            sc.compositionFallback = "no library of compositions";
            return;
        }
        SelectorScene s;
        s.context = sc.context;
        s.sectionKind = seg.sectionKind;
        s.narrative = sc.narrative;
        s.motifOperation = sc.motifOperation;
        if (const VisualMotif* m = score.Motif(sc.visualMotif); m != nullptr) {
            s.motifComposition = m->composition;
        }
        s.motifGesture = _hasMotifKind ? GestureKindId(_motifKind) : std::string();
        s.mode = sc.mode;
        s.modeByPerson = sc.manual;
        for (const StageEdit* e : EditsFor(seg)) {
            if (e->action == "composition") {
                s.personChoice = e->value;
            }
        }
        s.previous = _lastComposition;
        s.previousRun = _compRun;
        s.used = _compUsed;
        s.priority = seg.priority;
        s.durationMS = sc.endMS - sc.startMS;
        s.beat = Beat(sc.startMS);
        const std::vector<const Figure*> usable = Usable();
        s.available = static_cast<int>(usable.size());
        for (const Figure* f : usable) {
            s.topologies[f->stage->topology]++;
        }
        s.faces = static_cast<int>(faces.size());
        s.voices = static_cast<int>(voices.size());
        s.accentRun = Burst(sc);
        s.beforePeak = Art(ArtLevel::Full) && BeforePeak();
        // Before the peak its last two bars are kept for the light that grows towards it: only the
        // hits before them can start the scene's own gesture.
        const int usableEnd = s.beforePeak ? sc.endMS - static_cast<int>(Beat(sc.startMS) * 8000) : sc.endMS;
        s.strongHits = static_cast<int>(StrongAccents(sc.startMS, std::max(sc.startMS, usableEnd)).size());
        for (const Moment& m : MomentsIn(sc.startMS, sc.endMS)) {
            s.tail = s.tail || m.kind == "tail";
            s.drop = s.drop || m.kind == "drop";
        }
        const bool peak = sc.narrative == "climax" || (sc.narrative == "intensification" && sc.mode == StageMode::FullEnsemble);
        s.peakBegins = peak && (_sceneIndex == 0 || (_narrative[_sceneIndex - 1] != "climax" && _narrative[_sceneIndex - 1] != "intensification"));
        s.lastScene = sc.narrative == "resolution";
        s.target = _target;
        const CompositionChoice choice = SelectComposition(*_in.library, s);
        sc.candidates = choice.candidates;
        if (choice.entry == nullptr) {
            sc.compositionFallback = choice.fallback;
            _lastComposition.clear();
            _compRun = 0;
            return;
        }
        const StageLibraryEntry& e = *choice.entry;
        GestureKind kind = GestureKind::JointPulse;
        (void)GestureKindFromId(e.gesture, kind);
        _comp = &e;
        _compKind = kind;
        sc.composition = e.id;
        sc.compositionReasons = choice.reasons;
        sc.compositionAdapted = choice.adapted;
        _compUsed[e.id]++;
        _compRun = _lastComposition == e.id ? _compRun + 1 : 1;
        _lastComposition = e.id;
        // How the house is used follows the composition (a person's choice of mode stays).
        if (!sc.manual) {
            const StageMode before = sc.mode;
            if (kind == GestureKind::AmbientField) {
                sc.mode = StageMode::Protagonist;
            } else if (kind == GestureKind::CallResponse) {
                sc.mode = StageMode::CallResponse;
            } else if (kind == GestureKind::Counterpoint) {
                sc.mode = StageMode::Counterpoint;
            } else if (e.level == "global" && kind == GestureKind::PropagatedWave && sc.mode != StageMode::FullEnsemble) {
                sc.mode = StageMode::Propagation;
            } else if (e.level == "group" && (sc.mode == StageMode::Protagonist || sc.mode == StageMode::Duo)) {
                sc.mode = StageMode::Group;
            }
            if (sc.mode != before) {
                sc.reasons.push_back("the composition chosen sets how the house is used");
            }
        }
        // How many take part: the composition's own count, fitted to this house. One telling
        // with the house listening keeps a few quiet companions round it (by the budget).
        if (kind == GestureKind::AmbientField) {
            // One figure tells; a few quiet companions round it (more as the idea grows).
            const std::string& n = sc.narrative;
            _target = 1 + (n == "presentation" || n == "preparation" || n == "resolution" || sc.context == "narration" ? 3 : 6);
            sc.focus = true;
        } else if (e.level == "global" && kind != GestureKind::Contraction) {
            _target = static_cast<int>(_figures.size()); // the whole house, faces too
        } else if (kind == GestureKind::Contraction) {
            _target = _target > 0 ? _target : std::max(1, choice.figures); // the light gathers onto the few that close
            sc.focus = true;
        } else {
            _target = std::max(1, choice.figures);
            if (e.level == "local") {
                sc.focus = true;
            }
        }
        // The figures it was made with, or one of the shape it is made for, lead when no voice does.
        std::vector<std::string> lead;
        for (const std::string& f : e.figures) {
            if (const Figure* fig = Find(f); fig != nullptr && std::find(usable.begin(), usable.end(), fig) != usable.end()) {
                lead.push_back(f);
            }
        }
        if (lead.empty() && !e.fits.empty() && faces.empty()) {
            for (const Figure* f : ByRest({})) {
                if (std::find(usable.begin(), usable.end(), f) != usable.end() && std::find(e.fits.begin(), e.fits.end(), f->stage->topology) != e.fits.end()) {
                    lead.push_back(f->stage->name);
                    break;
                }
            }
        }
        if (!lead.empty()) {
            for (const std::string& f : _prefer) {
                if (std::find(lead.begin(), lead.end(), f) == lead.end()) {
                    lead.push_back(f);
                }
            }
            _prefer = lead;
        }
    }

    // ------------------------------------------------------------- presence (phase 21)
    // How much of the house a scene keeps as a quiet glow beyond the figures taking part:
    // nothing where the attention is on one or a few, some where an idea develops, all of it
    // where the whole house plays.
    static double Ambience(const StageScene& sc) {
        const std::string& n = sc.narrative;
        if (sc.mode == StageMode::Rest || n == "rest") {
            return 0;
        }
        if (n == "presentation" || n == "resolution") {
            return 0;
        }
        if (n == "preparation") {
            return 0.15;
        }
        if (sc.context == "narration") {
            return 0.2;
        }
        if (n == "climax" || n == "intensification" || n == "transformation") {
            return 1;
        }
        if (n == "contrast") {
            return 0.25;
        }
        return 0.35; // an idea that develops, answers or comes back
    }

    // The figures that have light of their own in a scene: effects of theirs (or of a group
    // holding them) adding up to a second of it, or a fifth of a shorter scene (a few beats'
    // accents count); a gesture of the scene; or their identity layer.
    std::set<std::string> Lit(const StageScene& sc, const GlobalStageScore& score) const {
        std::set<std::string> out;
        const int need = std::max(1, std::min(1000, (sc.endMS - sc.startMS) / 5));
        if (_in.choreography != nullptr) {
            std::map<std::string, int> covered;
            for (const ChoreographyEvent& e : _in.choreography->events) {
                const int o = std::min(e.endMS, sc.endMS) - std::max(e.startMS, sc.startMS);
                if (o <= 0) {
                    continue;
                }
                const TargetInfo* t = _in.capabilities != nullptr ? _in.capabilities->Find(e.target) : nullptr;
                if (t != nullptr && t->type == TargetType::Group) {
                    for (const std::string& m : t->flatModels) {
                        covered[m] += o;
                    }
                } else {
                    const size_t slash = e.target.find('/');
                    covered[slash == std::string::npos ? e.target : e.target.substr(0, slash)] += o;
                }
            }
            for (const auto& [f, ms] : covered) {
                if (ms >= need) {
                    out.insert(f);
                }
            }
        }
        for (const std::string& id : sc.gestures) {
            if (const GlobalGesture* g = score.Gesture(id); g != nullptr) {
                for (const GestureParticipant& p : g->participants) {
                    out.insert(p.figure);
                }
            }
        }
        for (const StageLayer& l : score.layers) {
            if (l.startMS < sc.endMS && sc.startMS < l.endMS) {
                out.insert(l.figure);
            }
        }
        return out;
    }

    // What each figure is asked to do in the scene, and how much of its own activity it keeps.
    // A figure with nothing of its own to show there is said to rest: the score says what the
    // render will show.
    void Presence(StageScene& sc, const GlobalStageScore& score) const {
        sc.ambience = Ambience(sc);
        sc.presence.clear();
        const bool silent = sc.mode == StageMode::Rest || sc.narrative == "rest";
        const std::set<std::string> lit = Lit(sc, score);
        std::vector<const Figure*> idle; // not taking part: kept as a glow or rested
        for (const Figure& f : _figures) {
            const std::string& name = f.stage->name;
            const std::string role = sc.RoleOf(name);
            FigurePresence p;
            p.figure = name;
            if (_locked.contains(name)) {
                p.state = "protected";
                p.policy = "protected";
                p.why = "a person locked its effects";
            } else if (role == "protagonist") {
                p.state = "protagonist";
                p.policy = "normal";
                p.why = "it carries the scene";
            } else if (role != "protagonist" && role != "rest" && !lit.contains(name)) {
                p.state = "rest";
                p.policy = "suspended";
                p.why = "nothing of its own to show in this scene";
            } else if (role == "complement") {
                p.state = "complement";
                p.policy = "normal";
                p.why = "it backs the protagonist";
            } else if (role == "responder" || role == "accent") {
                p.state = "response";
                p.policy = "normal";
                p.why = "it answers at its moment";
            } else if (role == "background" && !silent) {
                p.state = "ambient";
                p.policy = "reduced";
                p.why = "it keeps the scene company, quietly";
            } else {
                p.state = "rest";
                p.policy = "suspended";
                p.why = "the attention is elsewhere";
                if (lit.contains(name)) {
                    idle.push_back(&f); // only a figure with light of its own can keep a glow
                }
            }
            sc.presence.push_back(std::move(p));
        }
        // The glow: the largest figures first (they hold the house together), never a face
        // (a face that is not singing is quiet).
        std::stable_sort(idle.begin(), idle.end(), [](const Figure* a, const Figure* b) { return a->stage->lights > b->stage->lights; });
        int glow = static_cast<int>(std::lround(sc.ambience * static_cast<double>(idle.size())));
        for (const Figure* f : idle) {
            if (glow <= 0) {
                break;
            }
            if (f->face) {
                continue;
            }
            for (FigurePresence& p : sc.presence) {
                if (p.figure == f->stage->name) {
                    p.state = "ambient";
                    p.policy = "reduced";
                    p.why = "it keeps a quiet glow so the house is not dark";
                }
            }
            --glow;
        }
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
        // A returning idea keeps its protagonist when no voice takes the lead.
        if (faces.empty() && !_prefer.empty() && Find(_prefer.front()) != nullptr && !_locked.contains(_prefer.front())) {
            give(_prefer.front(), "protagonist", "it carries the idea it presented");
        }
        // The instruments' lead when no voice leads: the figure the plan featured.
        if (faces.empty() && taken.empty()) {
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
        // The idea's own figures first, then those that rested longest (an idea answered by
        // other figures leaves its first figures out while there are others).
        std::vector<const Figure*> order;
        for (const std::string& f : _prefer) {
            if (const Figure* fig = Find(f); fig != nullptr) {
                order.push_back(fig);
            }
        }
        // From ArtLevel Compositions the score commands the figures' own effects: the figures
        // the plan gave this scene's accents take part first, so the accents it planned are seen.
        if (Art(ArtLevel::Compositions)) {
            for (const PlanAssignment& a : seg.assignments) {
                const Figure* fig = Find(a.target);
                if (a.prominence == Prominence::Accent && fig != nullptr && !taken.contains(a.target) && !_avoid.contains(a.target) &&
                    std::find(order.begin(), order.end(), fig) == order.end()) {
                    order.push_back(fig);
                }
            }
        }
        for (const Figure* f : ByRest(taken)) {
            if (!_avoid.contains(f->stage->name) && std::find(order.begin(), order.end(), f) == order.end()) {
                order.push_back(f);
            }
        }
        for (const Figure* f : ByRest(taken)) {
            if (std::find(order.begin(), order.end(), f) == order.end()) {
                order.push_back(f);
            }
        }
        const bool counted = Art(ArtLevel::Full) && _target > 0 && sc.mode != StageMode::Rest;
        for (const Figure* f : order) {
            if (counted ? static_cast<int>(taken.size()) >= _target : spent + Cost(fill) > sc.budget + 1e-9) {
                break;
            }
            const bool own = std::find(_prefer.begin(), _prefer.end(), f->stage->name) != _prefer.end();
            give(f->stage->name, fill, own ? "it carries the scene's idea" : sc.mode == StageMode::FullEnsemble ? "the whole house takes part" : "it has rested longest");
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
        // Before the peak, its last two bars are left for the light that grows towards it.
        const int reserved = Art(ArtLevel::Full) && BeforePeak() ? sc.endMS - static_cast<int>(Beat(sc.startMS) * 8000) : sc.endMS;
        std::vector<std::pair<int, int>> busy = placed; // the scene's gestures so far, by their hits
        for (const std::string& id : sc.gestures) {
            if (const GlobalGesture* g = score.Gesture(id); g != nullptr) {
                busy.push_back({ g->startMS, g->startMS }); // the peak's opening, placed before
            }
        }
        const auto roomFor = [&](int a, int b) {
            if (made >= maxGestures || b >= sc.endMS - 300 || b >= reserved) {
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
            const bool opens = kind == GestureKind::Expansion && Art(ArtLevel::Motifs) && !_motifId.empty() && !_carried &&
                               (sc.narrative == "climax" || sc.narrative == "intensification");
            _carrying = opens;
            const bool ok = Make(sc, seg, kind, m, faces, score);
            _carrying = false;
            if (ok) {
                ++made;
                busy.push_back({ m.ms, m.ms });
                _carried = _carried || opens;
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
            // A composition chosen for the scene makes its own gesture: a burst does not take its place.
            const bool composedElse = _comp != nullptr && _compKind != GestureKind::Cascade;
            if (count >= 3 && count <= 5 && strongEach && before >= widest * 3 / 2 && after >= widest * 3 / 2 && roomFor(accents[i].ms, accents[j - 1].ms) && sc.mode != StageMode::Protagonist &&
                !composedElse) {
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
            // The scene's idea is carried by its first main gesture: the motif's own gesture,
            // or the one it turns into here.
            const bool carry = Art(ArtLevel::Motifs) && !_motifId.empty() && !_carried;
            const GestureKind modeKind = kind;
            if (carry && _hasMotifKind && Allowed(_motifKind)) {
                kind = _motifKind;
            }
            const GestureKind ideaKind = kind;
            // The scene's composition sets its main gesture (one the music shapes, a sweep or an
            // opening or a gathering, is made where its moment is).
            const bool composed = _comp != nullptr && _compKind != GestureKind::SpatialSweep && _compKind != GestureKind::Expansion && _compKind != GestureKind::Contraction &&
                                  _compKind != GestureKind::Echo && _compKind != GestureKind::Cascade;
            if (composed) {
                kind = _compKind;
            }
            _carrying = carry;
            _composing = composed;
            bool ok = Make(sc, seg, kind, m, faces, score);
            _composing = false;
            if (!ok && composed && kind != ideaKind) {
                ok = Make(sc, seg, ideaKind, m, faces, score); // the composition's gesture does not fit this hit: the idea's carries it
            }
            if (!ok && carry && ideaKind != modeKind) {
                ok = Make(sc, seg, modeKind, m, faces, score); // the idea's gesture does not fit here: the scene's own carries it
            }
            _carrying = false;
            if (ok) {
                ++made;
                busy.push_back({ m.ms, m.ms });
                _carried = _carried || carry;
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
        // A gesture carrying the scene's idea is drawn by the idea's figures, unless the idea
        // grows here (intensified, transformed): then by everyone taking part.
        const std::string op = _carrying ? sc.motifOperation : std::string();
        if (_carrying && !_prefer.empty() && op != "intensify" && op != "transform") {
            std::vector<std::string> own;
            for (const std::string& f : active) {
                if (std::find(_prefer.begin(), _prefer.end(), f) != _prefer.end()) {
                    own.push_back(f);
                }
            }
            if (own.size() >= (kind == GestureKind::CallResponse || kind == GestureKind::Counterpoint ? 2u : 1u)) {
                active = own;
            }
        }
        if (active.empty()) {
            return false;
        }
        switch (kind) {
        case GestureKind::PropagatedWave:
        case GestureKind::Echo:
        case GestureKind::Expansion:
        case GestureKind::Contraction: {
            g.origin = Origin(sc, active);
            if (_carrying && !_motifAnchor.empty() && ResolveAnchor(_motifAnchor, *_in.space, *_in.geometry, _in.semantics).ok) {
                g.origin = _motifAnchor; // the idea starts where it was born
            }
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
            // Few of them; when the idea is intensified, everyone takes it up.
            // A composition's own gesture takes the figures the composition gave the scene.
            const size_t count = _composing || (_carrying && op == "intensify") ? active.size() : std::min<size_t>(active.size(), kind == GestureKind::LocalWaves ? 4 : 6);
            g.endMS = std::min(sc.endMS, g.startMS + static_cast<int>(std::max(0.6, beat * 2) * 1000));
            g.reasons.push_back(kind == GestureKind::LocalWaves ? "a hit: several figures each make a small wave of their own, together"
                                                                : "a hit: several figures flash together");
            for (size_t k = 0; k < count; ++k) {
                g.participants.push_back({ active[k], "accent", 0, 1.0, kind == GestureKind::LocalWaves ? "ripple" : "pulse", "the same hit, in its own shape" });
            }
            const bool answer = _composing ? _comp->temporal == "answer" : Art(ArtLevel::Direction) && !Unison(sc);
            if (answer && g.participants.size() >= 2) {
                Answering(sc, g, beat);
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
        // The scene's composition, when this is its gesture: its brightness, and the speed and
        // length a person may have set for it.
        if (_comp != nullptr && kind == _compKind) {
            g.composition = _comp->id;
            if (_comp->speed > 0.05 && g.speed > 0.05 &&
                (kind == GestureKind::PropagatedWave || kind == GestureKind::Expansion || kind == GestureKind::Contraction || kind == GestureKind::Echo || kind == GestureKind::SpatialSweep)) {
                int last = 0;
                for (GestureParticipant& p : g.participants) {
                    p.delayMS = static_cast<int>(p.delayMS * g.speed / _comp->speed);
                    last = std::max(last, p.delayMS);
                }
                g.speed = _comp->speed;
                g.endMS = std::min(sc.endMS, std::max(g.endMS, g.startMS + last + 600));
            }
            if (_comp->beats > 0) {
                g.endMS = std::min(sc.endMS, g.startMS + static_cast<int>(_comp->beats * beat * 1000));
            }
            if (_comp->level0 < 1) {
                g.level = std::min(g.level, _comp->level0);
                for (GestureParticipant& p : g.participants) {
                    p.level = std::min(p.level, _comp->level0);
                }
            }
            std::erase_if(g.participants, [&](const GestureParticipant& p) { return g.startMS + p.delayMS + 300 > g.endMS; });
            if (g.participants.empty() || g.endMS <= g.startMS) {
                return false;
            }
        }
        g.id = fmt::format("{}@{}", GestureKindId(kind), g.startMS);
        if (Art(ArtLevel::Direction) && std::find(sc.gestures.begin(), sc.gestures.end(), g.id) != sc.gestures.end()) {
            return false; // one gesture of a kind at a moment
        }
        if (_manual) {
            g.reasons.insert(g.reasons.begin(), "placed by a person");
        }
        if (Art(ArtLevel::Direction)) {
            Describe(sc, g, m.kind);
        }
        sc.gestures.push_back(g.id);
        score.gestures.push_back(std::move(g));
        return true;
    }

    // ------------------------------------------------------------- time and presence (phase 20)
    bool Unison(const StageScene& sc) const {
        return sc.mode == StageMode::Unison || sc.narrative == "climax" || sc.narrative == "intensification";
    }

    // Figures that answer one another instead of all lighting at once: the nearest to the
    // one that starts answers first, half a beat apart (at most three steps), so the order
    // comes from the stage and the time from the music.
    void Answering(const StageScene& sc, GlobalGesture& g, double beat) const {
        std::string source = g.participants.front().figure;
        for (const GestureParticipant& p : g.participants) {
            if (sc.RoleOf(p.figure) == "protagonist") {
                source = p.figure;
                break;
            }
        }
        const Figure* s = Find(source);
        if (s == nullptr) {
            return;
        }
        const auto distance = [&](const std::string& f) {
            const Figure* x = Find(f);
            return x == nullptr ? 1e9 : std::hypot(x->stage->cx - s->stage->cx, x->stage->cy - s->stage->cy);
        };
        std::stable_sort(g.participants.begin(), g.participants.end(), [&](const GestureParticipant& a, const GestureParticipant& b) {
            return (a.figure == source) != (b.figure == source) ? a.figure == source : distance(a.figure) < distance(b.figure);
        });
        const int step = std::max(100, static_cast<int>(beat * 500));
        for (size_t k = 0; k < g.participants.size(); ++k) {
            GestureParticipant& p = g.participants[k];
            p.delayMS = static_cast<int>(std::min<size_t>(k, 3)) * step;
            p.role = k == 0 ? "source" : "responder";
            p.reason = k == 0 ? "it starts the idea" : fmt::format("it answers {:.1f} beats later", p.delayMS / (beat * 1000));
        }
        g.endMS = std::min(sc.endMS, g.endMS + 3 * step);
        g.reasons.push_back("not all at once: the nearest figures answer first, half a beat apart");
    }

    // How the gesture's figures follow in time, and how much it should be seen.
    void Describe(const StageScene& sc, GlobalGesture& g, const std::string& trigger) const {
        if (g.relation.empty()) {
            switch (g.kind) {
            case GestureKind::PropagatedWave:
            case GestureKind::Expansion:
            case GestureKind::SpatialSweep: g.relation = trigger == "anticipation" ? "anticipation" : "propagation"; break;
            case GestureKind::Echo: g.relation = "continuation"; break;
            case GestureKind::Contraction: g.relation = trigger == "closing" ? "progressive-end" : "propagation"; break;
            case GestureKind::CallResponse: g.relation = "answer"; break;
            case GestureKind::Cascade: g.relation = "sequence"; break;
            case GestureKind::Counterpoint:
            case GestureKind::AmbientField: g.relation = "overlap"; break;
            case GestureKind::LocalWaves:
            case GestureKind::JointPulse: {
                bool together = true;
                for (const GestureParticipant& p : g.participants) {
                    together = together && p.delayMS == 0;
                }
                g.relation = together ? "unison" : "answer";
                break;
            }
            }
        }
        if (_carrying) {
            g.motif = _motifId;
            if (_motifLevel < 1) {
                g.level = std::min(g.level, _motifLevel);
                for (GestureParticipant& p : g.participants) {
                    p.level = std::min(p.level, _motifLevel);
                }
            }
        }
        bool protagonist = false;
        for (const GestureParticipant& p : g.participants) {
            protagonist = protagonist || sc.RoleOf(p.figure) == "protagonist";
        }
        g.presence = g.kind == GestureKind::AmbientField || g.level < 0.5                              ? "background"
                   : Unison(sc) || trigger == "climax-open"                                            ? "climax"
                   : _carrying || protagonist || trigger == "anticipation"                             ? "protagonist"
                                                                                                        : "response";
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
        if (_comp != nullptr && _compKind == GestureKind::Cascade) {
            g.composition = _comp->id;
        }
        g.id = fmt::format("{}@{}", GestureKindId(GestureKind::Cascade), g.startMS);
        if (Art(ArtLevel::Direction)) {
            Describe(sc, g, "accents");
        }
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
            if (e.action == "identity" || e.action == "no-identity" || e.action == "gesture" || e.action == "priority") {
                continue; // decided with the scenes' layers and gestures, or where the presence is applied
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
            if (e.action == "mode" || e.action == "composition") {
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
    // Phase 20
    bool Art(ArtLevel l) const { return static_cast<int>(_in.art) >= static_cast<int>(l); }
    std::vector<std::string> _narrative; // per plan segment
    std::map<int, int> _seenGroup;
    std::map<int, std::string> _groupMotif; // repeat group -> the motif its first time carried
    std::string _current;                   // the motif last carried
    std::vector<std::string> _prefer;       // this scene: the idea's figures, protagonist first
    std::set<std::string> _avoid;           // this scene: figures a new idea leaves to the first one
    int _target = 0;                        // this scene: how many figures take part (0: by the budget)
    std::string _motifId;
    bool _hasMotifKind = false;
    GestureKind _motifKind = GestureKind::PropagatedWave;
    double _motifLevel = 1;
    std::string _motifAnchor;
    bool _carried = false;  // this scene's idea already has its gesture
    bool _carrying = false; // the gesture being made carries it
    int _mainGroup = -1;     // the song's returning part
    std::set<size_t> _peak;  // scenes of the peak the song does not mark itself
    std::map<int, std::vector<std::string>> _groupFigures; // repeat group -> the figures its first time had
    std::map<std::string, int> _developed;                 // motif -> how many times it developed
    // Phase 21
    const StageLibraryEntry* _comp = nullptr; // this scene's composition
    GestureKind _compKind = GestureKind::JointPulse;
    bool _composing = false;                  // the gesture being made is the composition's own
    std::string _lastComposition;
    int _compRun = 0;
    std::map<std::string, int> _compUsed;
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

const FigurePresence* StageScene::PresenceOf(const std::string& figure) const {
    for (const FigurePresence& p : presence) {
        if (p.figure == figure) {
            return &p;
        }
    }
    return nullptr;
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

const VisualMotif* GlobalStageScore::Motif(const std::string& id) const {
    for (const VisualMotif& m : motifs) {
        if (m.id == id) {
            return &m;
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
    if (g.colorBy == "distance" || g.colorBy == "trail" || g.colorBy == "time") {
        s.colorBy = g.colorBy;
        if (s.colorBy == "distance") {
            // Spread over as far as the gesture goes: its farthest figure's farthest corner.
            double reach = 0.05;
            for (const GestureParticipant& q : g.participants) {
                if (const StageFigure* qf = space.Find(q.figure); qf != nullptr) {
                    for (const double x : { qf->minX, qf->maxX }) {
                        for (const double y : { qf->minY, qf->maxY }) {
                            reach = std::max(reach, shared ? std::hypot(x - g.ox, y - g.oy) : 0.0);
                        }
                    }
                }
            }
            if (!shared && f != nullptr) {
                reach = std::max({ 0.02, f->maxX - f->minX, f->maxY - f->minY });
            }
            s.reach = std::clamp(reach, 0.02, 10.0);
        }
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
    // From ArtLevel Presence every figure's effects follow its presence (ApplyStagePresence),
    // applied before this.
    for (const StageScene& sc : score.scenes) {
        if (score.art >= static_cast<int>(ArtLevel::Presence)) {
            break;
        }
        if (sc.mode != StageMode::Protagonist && sc.mode != StageMode::Duo && sc.mode != StageMode::Rest && sc.mode != StageMode::CallResponse && !sc.focus) {
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

const char* ArtLevelId(ArtLevel a) {
    switch (a) {
    case ArtLevel::F19: return "f19";
    case ArtLevel::Direction: return "direction";
    case ArtLevel::Motifs: return "motifs";
    case ArtLevel::Full: return "full";
    case ArtLevel::Compositions: return "compositions";
    case ArtLevel::Presence: return "presence";
    case ArtLevel::Orchestra: return "orchestra";
    }
    return "orchestra";
}

bool ArtLevelFromId(const std::string& id, ArtLevel& a) {
    for (ArtLevel l : { ArtLevel::F19, ArtLevel::Direction, ArtLevel::Motifs, ArtLevel::Full, ArtLevel::Compositions, ArtLevel::Presence, ArtLevel::Orchestra }) {
        if (id == ArtLevelId(l)) {
            a = l;
            return true;
        }
    }
    return false;
}

std::string NarrativePhrase(const std::string& n) {
    if (n == "presentation") return COMPOSER_TEXT("presents the idea");
    if (n == "development") return COMPOSER_TEXT("develops it");
    if (n == "contrast") return COMPOSER_TEXT("brings a contrasting idea");
    if (n == "answer") return COMPOSER_TEXT("answers it");
    if (n == "preparation") return COMPOSER_TEXT("prepares the peak");
    if (n == "climax") return COMPOSER_TEXT("is the peak");
    if (n == "intensification") return COMPOSER_TEXT("brings it back stronger");
    if (n == "rest") return COMPOSER_TEXT("rests");
    if (n == "transformation") return COMPOSER_TEXT("transforms it");
    if (n == "recovery") return COMPOSER_TEXT("brings it back");
    if (n == "resolution") return COMPOSER_TEXT("closes the song");
    return n;
}

std::string MotifOperationPhrase(const std::string& o) {
    if (o == "present") return COMPOSER_TEXT("presented");
    if (o == "hold") return COMPOSER_TEXT("kept");
    if (o == "develop") return COMPOSER_TEXT("developed: a couple more figures take it up");
    if (o == "intensify") return COMPOSER_TEXT("intensified: everyone takes it up");
    if (o == "reduce") return COMPOSER_TEXT("reduced to its protagonist, before the peak");
    if (o == "transform") return COMPOSER_TEXT("transformed: the same idea travels another way");
    if (o == "split") return COMPOSER_TEXT("split: some figures call, others answer");
    if (o == "recall") return COMPOSER_TEXT("brought back by its own figures");
    if (o == "resolve") return COMPOSER_TEXT("resolved, quietly");
    return o;
}

std::string AttentionPhrase(const std::string& a) {
    if (a == "one") return COMPOSER_TEXT("one figure");
    if (a == "two") return COMPOSER_TEXT("two figures");
    if (a == "few") return COMPOSER_TEXT("a few figures");
    if (a == "group") return COMPOSER_TEXT("a group of figures");
    if (a == "all") return COMPOSER_TEXT("the whole installation");
    if (a == "propagation") return COMPOSER_TEXT("a gesture travelling across the house");
    if (a == "rest") return COMPOSER_TEXT("a rest");
    return a;
}

std::string RelationPhrase(const std::string& r) {
    if (r == "unison") return COMPOSER_TEXT("all together");
    if (r == "answer") return COMPOSER_TEXT("answering one another");
    if (r == "propagation") return COMPOSER_TEXT("travelling from one to the next");
    if (r == "sequence") return COMPOSER_TEXT("one after another");
    if (r == "overlap") return COMPOSER_TEXT("overlapping");
    if (r == "continuation") return COMPOSER_TEXT("continuing what came before");
    if (r == "anticipation") return COMPOSER_TEXT("anticipating what comes");
    if (r == "progressive-end") return COMPOSER_TEXT("ending figure by figure");
    return r;
}

std::vector<const StageLibraryEntry*> CompositionsFor(const StageLibrary& library, const std::string& context) {
    std::vector<const StageLibraryEntry*> out;
    for (const StageLibraryEntry& e : library.entries) {
        if (e.kind == "composition" && !e.excluded && (context.empty() || std::find(e.contexts.begin(), e.contexts.end(), context) != e.contexts.end())) {
            out.push_back(&e);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const StageLibraryEntry* a, const StageLibraryEntry* b) { return a->favorite > b->favorite; });
    return out;
}

StageLibraryEntry CompositionFromMotif(const VisualMotif& m, const GlobalStageScore& score, const std::string& song) {
    StageLibraryEntry e;
    std::string name = song;
    std::replace(name.begin(), name.end(), ' ', '-');
    e.id = fmt::format("composition:{}-{}", name, m.id);
    e.kind = "composition";
    e.custom = true;
    e.source = "person";
    e.title = fmt::format("{} {}", song, m.id);
    e.description = m.explanation;
    e.level = m.figures.size() <= 1 ? "local" : m.figures.size() >= 8 ? "global" : "group";
    e.gesture = m.gestureKind;
    e.temporal = m.temporal;
    e.figures = m.figures;
    e.minFigures = static_cast<int>(std::max<size_t>(1, m.figures.size()));
    e.maxFigures = static_cast<int>(m.figures.size());
    if (!m.movement.empty()) {
        e.functions = { m.movement };
    }
    for (const auto& [figure, effect] : m.recipes) {
        if (std::find(e.effects.begin(), e.effects.end(), effect) == e.effects.end()) {
            e.effects.push_back(effect);
        }
    }
    const std::string scene = m.origin.rfind("scene:", 0) == 0 ? m.origin.substr(6) : std::string();
    for (const StageScene& sc : score.scenes) {
        if (sc.id == scene && !sc.sectionKind.empty()) {
            e.contexts = { sc.sectionKind };
        }
    }
    e.palette = { "anchor" };
    return e;
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
    // Whole compositions (phase 20): an idea built from the gestures and fields above, at one
    // of three levels. Generic: no figure, show or sequence of anyone's in them.
    const auto composition = [&](const char* id, const char* level, const char* title, const char* description, GestureKind gesture, const char* temporal,
                                 std::vector<std::string> contexts, int minFigures, int maxFigures, std::vector<std::string> palette, double brightness, std::vector<std::string> fits = {}) {
        StageLibraryEntry e;
        e.id = std::string("composition:") + id;
        e.kind = "composition";
        e.title = title;
        e.description = description;
        e.source = "built-in";
        e.level = level;
        e.gesture = GestureKindId(gesture);
        e.temporal = temporal;
        e.contexts = std::move(contexts);
        e.minFigures = minFigures;
        e.maxFigures = maxFigures;
        e.palette = std::move(palette);
        e.level0 = brightness;
        e.fits = std::move(fits);
        for (const StageLibraryEntry& g : l.entries) {
            if (g.id == e.gesture) {
                e.functions = g.functions;
            }
        }
        l.entries.push_back(std::move(e));
    };
    composition("protagonist-glow", "local", COMPOSER_TEXT("One figure tells, the house listens"), COMPOSER_TEXT("A single figure carries the moment; a dim field keeps the others company."),
                GestureKind::AmbientField, "overlap", { "intro", "narration", "solo", "pause" }, 1, 1, { "anchor", "shadow" }, 0.4);
    composition("accent-flash", "local", COMPOSER_TEXT("A flash on the hit"), COMPOSER_TEXT("One or two figures mark a strong hit and fall quiet again."), GestureKind::JointPulse, "unison",
                { "accent", "verse" }, 1, 2, { "accent" }, 1);
    composition("spiral-tree", "local", COMPOSER_TEXT("A tree turning"), COMPOSER_TEXT("A tree's own spiral, kept for a figure that can show it."), GestureKind::LocalWaves, "unison",
                { "instrumental", "verse" }, 1, 1, { "anchor", "highlight" }, 0.9, { "tree", "tree-360" });
    composition("neighbours-answer", "group", COMPOSER_TEXT("Neighbours answer"), COMPOSER_TEXT("One figure starts and its neighbours answer, half a beat apart."), GestureKind::LocalWaves,
                "answer", { "verse", "chorus", "instrumental" }, 3, 4, { "anchor", "secondary" }, 0.9);
    composition("call-and-answer", "group", COMPOSER_TEXT("Call and answer"), COMPOSER_TEXT("The voice calls and the figures answer it in turn."), GestureKind::CallResponse, "answer",
                { "choir", "chorus", "solo" }, 2, 5, { "anchor", "accent" }, 1);
    composition("hits-in-turn", "group", COMPOSER_TEXT("Hits in turn"), COMPOSER_TEXT("A few hits close together: one figure for each, across the house."), GestureKind::Cascade, "sequence",
                { "accent", "instrumental" }, 3, 5, { "accent", "highlight" }, 1);
    composition("two-halves", "group", COMPOSER_TEXT("Two halves"), COMPOSER_TEXT("Left and right do different things at once, like two voices."), GestureKind::Counterpoint, "overlap",
                { "solo", "instrumental", "verse" }, 2, 0, { "anchor", "secondary" }, 0.9);
    composition("wave-house", "global", COMPOSER_TEXT("A wave across the house"), COMPOSER_TEXT("From one bulb, a wave reaches every figure when its distance says."), GestureKind::PropagatedWave,
                "propagation", { "chorus", "instrumental", "climax" }, 4, 0, { "anchor", "highlight" }, 1);
    composition("growing-sweep", "global", COMPOSER_TEXT("The light grows across"), COMPOSER_TEXT("While the music builds, a sweep crosses the house before the peak."), GestureKind::SpatialSweep,
                "anticipation", { "transition", "instrumental" }, 3, 0, { "anchor", "highlight" }, 0.8);
    composition("peak-opens", "global", COMPOSER_TEXT("The peak opens"), COMPOSER_TEXT("At the peak, the light spreads from where the idea was born to the whole house."), GestureKind::Expansion,
                "propagation", { "climax", "chorus" }, 4, 0, { "anchor", "accent", "contrast" }, 1);
    composition("gathers-to-close", "global", COMPOSER_TEXT("The light gathers to close"), COMPOSER_TEXT("The house gathers back, figure by figure, to the one that began."), GestureKind::Contraction,
                "progressive-end", { "ending", "pause" }, 2, 0, { "anchor", "shadow" }, 0.6);
    composition("echo-tail", "global", COMPOSER_TEXT("An echo as the sound dies"), COMPOSER_TEXT("The last gesture comes again, weaker, as the sound fades."), GestureKind::Echo, "continuation",
                { "ending", "transition" }, 2, 0, { "highlight", "neutral" }, 0.5);
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
                if (e.kind == "composition" && p.kind == "composition") {
                    // A person's own values for a built-in composition (how bright, how fast, how many).
                    e.level0 = p.level0;
                    e.speed = p.speed;
                    e.beats = p.beats;
                    e.minFigures = p.minFigures;
                    e.maxFigures = p.maxFigures;
                }
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
             { "endMS", g.endMS }, { "envelope", g.envelope }, { "colors", g.colors }, { "colorBy", g.colorBy }, { "colorReason", g.colorReason }, { "trigger", g.trigger }, { "related", g.related }, { "confidence", g.confidence },
             { "reasons", g.reasons }, { "manual", g.manual }, { "relation", g.relation }, { "presence", g.presence }, { "motif", g.motif } };
}

void AddPhase21(json& j, const GlobalGesture& g) {
    if (!g.composition.empty()) {
        j["composition"] = g.composition;
    }
}

json MotifJson(const VisualMotif& m) {
    json variants = json::array();
    for (const MotifVariant& v : m.variants) {
        variants.push_back({ { "scene", v.scene }, { "startMS", v.startMS }, { "operation", v.operation }, { "narrative", v.narrative }, { "figures", v.figures }, { "gesture", v.gesture },
                             { "kind", v.kind }, { "level", v.level }, { "why", v.why } });
    }
    return { { "id", m.id }, { "intent", m.intent }, { "origin", m.origin }, { "composition", m.composition }, { "protagonist", m.protagonist }, { "companions", m.companions },
             { "figures", m.figures }, { "recipes", m.recipes }, { "gestureKind", m.gestureKind }, { "movement", m.movement }, { "temporal", m.temporal }, { "anchor", m.anchor },
             { "spatial", m.spatial }, { "colorMotif", m.colorMotif }, { "triggers", m.triggers }, { "entry", m.entry }, { "exit", m.exit }, { "constraints", m.constraints },
             { "variants", variants }, { "explanation", m.explanation } };
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
                           { "reasons", sc.reasons }, { "confidence", sc.confidence }, { "manual", sc.manual }, { "narrative", sc.narrative }, { "attention", sc.attention },
                           { "visualMotif", sc.visualMotif }, { "motifOperation", sc.motifOperation }, { "accentFigure", sc.accentFigure }, { "changes", sc.changes },
                           { "leadsTo", sc.leadsTo }, { "focus", sc.focus } });
        json& j = scenes.back();
        if (!sc.composition.empty() || !sc.compositionFallback.empty() || !sc.candidates.empty()) {
            j["composition"] = sc.composition;
            j["compositionReasons"] = sc.compositionReasons;
            j["compositionFallback"] = sc.compositionFallback;
            j["compositionAdapted"] = sc.compositionAdapted;
            json cands = json::array();
            for (const CompositionCandidate& c : sc.candidates) {
                cands.push_back({ { "id", c.id }, { "score", c.score }, { "verdict", c.verdict }, { "reasons", c.reasons }, { "figures", c.figures } });
            }
            j["candidates"] = std::move(cands);
        }
        if (!sc.presence.empty()) {
            j["ambience"] = sc.ambience;
            json ps = json::array();
            for (const FigurePresence& p : sc.presence) {
                ps.push_back({ { "figure", p.figure }, { "state", p.state }, { "policy", p.policy }, { "why", p.why } });
            }
            j["presence"] = std::move(ps);
        }
    }
    json gestures = json::array();
    for (const GlobalGesture& g : s.gestures) {
        json gj = GestureJson(g);
        AddPhase21(gj, g);
        gestures.push_back(std::move(gj));
    }
    json layers = json::array();
    for (const StageLayer& l : s.layers) {
        layers.push_back({ { "figure", l.figure }, { "role", l.role }, { "region", l.region }, { "source", l.source }, { "color", l.color }, { "only", l.only }, { "skip", l.skip },
                           { "order", l.order }, { "opacity", l.opacity }, { "blend", l.blend }, { "startMS", l.startMS }, { "endMS", l.endMS } });
    }
    json motifs = json::array();
    for (const VisualMotif& m : s.motifs) {
        motifs.push_back(MotifJson(m));
    }
    json root({ { "kind", "composer-stage-score" }, { "schemaVersion", s.schemaVersion }, { "version", s.version }, { "durationMS", s.durationMS }, { "scenes", scenes },
                  { "gestures", gestures }, { "layers", layers }, { "limits", s.limits }, { "warnings", s.warnings }, { "art", ArtLevelId(static_cast<ArtLevel>(s.art)) },
                  { "motifs", motifs } });
    if (s.art >= static_cast<int>(ArtLevel::Presence)) {
        root["ambientLevel"] = s.ambientLevel;
        json ts = json::array();
        for (const CompositionTransition& t : s.transitions) {
            ts.push_back({ { "from", t.from }, { "to", t.to }, { "atMS", t.atMS }, { "kind", t.kind }, { "why", t.why }, { "joining", t.joining }, { "leaving", t.leaving },
                           { "softened", t.softened }, { "checks", t.checks } });
        }
        root["transitions"] = std::move(ts);
        json sl = json::array();
        for (const SharedLights& x : s.sharedLights) {
            sl.push_back({ { "a", x.a }, { "b", x.b }, { "lights", x.lights }, { "relation", x.relation }, { "priority", x.priority }, { "prioritySource", x.prioritySource },
                           { "scenesBothLit", x.scenesBothLit }, { "scenesResolved", x.scenesResolved }, { "note", x.note } });
        }
        root["sharedLights"] = std::move(sl);
        root["presenceNotes"] = s.presenceNotes;
    }
    return root.dump(1);
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
            sc.narrative = j.value("narrative", std::string());
            sc.attention = j.value("attention", std::string());
            sc.visualMotif = j.value("visualMotif", std::string());
            sc.motifOperation = j.value("motifOperation", std::string());
            sc.accentFigure = j.value("accentFigure", std::string());
            sc.changes = j.value("changes", std::string());
            sc.leadsTo = j.value("leadsTo", std::string());
            sc.focus = j.value("focus", false);
            sc.composition = j.value("composition", std::string());
            sc.compositionReasons = j.value("compositionReasons", std::vector<std::string>());
            sc.compositionFallback = j.value("compositionFallback", std::string());
            sc.compositionAdapted = j.value("compositionAdapted", std::string());
            for (const json& c : j.value("candidates", json::array())) {
                sc.candidates.push_back({ c.value("id", std::string()), c.value("score", 0.0), c.value("verdict", std::string()), c.value("reasons", std::vector<std::string>()),
                                          c.value("figures", 0) });
            }
            sc.ambience = j.value("ambience", 0.0);
            for (const json& p : j.value("presence", json::array())) {
                sc.presence.push_back({ p.value("figure", std::string()), p.value("state", std::string()), p.value("policy", std::string()), p.value("why", std::string()) });
            }
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
            g.colorBy = j.value("colorBy", std::string("field"));
            g.colorReason = j.value("colorReason", std::string());
            g.trigger = j.value("trigger", std::string());
            g.related = j.value("related", std::string());
            g.confidence = j.value("confidence", 0.0);
            g.reasons = j.value("reasons", std::vector<std::string>());
            g.manual = j.value("manual", false);
            g.relation = j.value("relation", std::string());
            g.presence = j.value("presence", std::string());
            g.motif = j.value("motif", std::string());
            g.composition = j.value("composition", std::string());
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
        ArtLevel art = ArtLevel::F19;
        (void)ArtLevelFromId(root.value("art", std::string("f19")), art);
        s.art = static_cast<int>(art);
        for (const json& j : root.value("motifs", json::array())) {
            VisualMotif m;
            m.id = j.value("id", std::string());
            m.intent = j.value("intent", std::string());
            m.origin = j.value("origin", std::string());
            m.composition = j.value("composition", std::string());
            m.protagonist = j.value("protagonist", std::string());
            m.companions = j.value("companions", std::vector<std::string>());
            m.figures = j.value("figures", std::vector<std::string>());
            m.recipes = j.value("recipes", std::map<std::string, std::string>());
            m.gestureKind = j.value("gestureKind", std::string());
            m.movement = j.value("movement", std::string());
            m.temporal = j.value("temporal", std::string());
            m.anchor = j.value("anchor", std::string());
            m.spatial = j.value("spatial", std::string());
            m.colorMotif = j.value("colorMotif", std::string());
            m.triggers = j.value("triggers", std::vector<std::string>());
            m.entry = j.value("entry", std::string());
            m.exit = j.value("exit", std::string());
            m.constraints = j.value("constraints", std::vector<std::string>());
            m.explanation = j.value("explanation", std::string());
            for (const json& v : j.value("variants", json::array())) {
                MotifVariant x;
                x.scene = v.value("scene", std::string());
                x.startMS = v.value("startMS", 0);
                x.operation = v.value("operation", std::string());
                x.narrative = v.value("narrative", std::string());
                x.figures = v.value("figures", std::vector<std::string>());
                x.gesture = v.value("gesture", std::string());
                x.kind = v.value("kind", std::string());
                x.level = v.value("level", 1.0);
                x.why = v.value("why", std::string());
                m.variants.push_back(std::move(x));
            }
            s.motifs.push_back(std::move(m));
        }
        s.ambientLevel = root.value("ambientLevel", 0.4);
        for (const json& t : root.value("transitions", json::array())) {
            CompositionTransition x;
            x.from = t.value("from", std::string());
            x.to = t.value("to", std::string());
            x.atMS = t.value("atMS", 0);
            x.kind = t.value("kind", std::string());
            x.why = t.value("why", std::string());
            x.joining = t.value("joining", std::vector<std::string>());
            x.leaving = t.value("leaving", std::vector<std::string>());
            x.softened = t.value("softened", 0);
            x.checks = t.value("checks", std::vector<std::string>());
            s.transitions.push_back(std::move(x));
        }
        for (const json& t : root.value("sharedLights", json::array())) {
            SharedLights x;
            x.a = t.value("a", std::string());
            x.b = t.value("b", std::string());
            x.lights = t.value("lights", 0);
            x.relation = t.value("relation", std::string());
            x.priority = t.value("priority", std::string());
            x.prioritySource = t.value("prioritySource", std::string());
            x.scenesBothLit = t.value("scenesBothLit", 0);
            x.scenesResolved = t.value("scenesResolved", 0);
            x.note = t.value("note", std::string());
            s.sharedLights.push_back(std::move(x));
        }
        s.presenceNotes = root.value("presenceNotes", std::vector<std::string>());
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
        json j = { { "id", e.id }, { "kind", e.kind }, { "title", e.title }, { "description", e.description }, { "functions", e.functions }, { "fits", e.fits },
                   { "favorite", e.favorite }, { "excluded", e.excluded }, { "custom", e.custom }, { "source", e.source } };
        if (e.kind == "composition") {
            j["level"] = e.level;
            j["contexts"] = e.contexts;
            j["gesture"] = e.gesture;
            j["temporal"] = e.temporal;
            j["minFigures"] = e.minFigures;
            j["maxFigures"] = e.maxFigures;
            j["regions"] = e.regions;
            j["effects"] = e.effects;
            j["palette"] = e.palette;
            j["brightness"] = e.level0;
            j["speed"] = e.speed;
            j["beats"] = e.beats;
            j["figures"] = e.figures;
            j["limits"] = e.limits;
            j["variantOf"] = e.variantOf;
        }
        list.push_back(std::move(j));
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
            StageLibraryEntry x { e.value("id", std::string()), e.value("kind", std::string()), e.value("title", std::string()), e.value("description", std::string()),
                                  e.value("functions", std::vector<std::string>()), e.value("fits", std::vector<std::string>()), e.value("favorite", false), e.value("excluded", false),
                                  e.value("custom", false), e.value("source", std::string("person")) };
            if (x.kind == "composition") {
                x.level = e.value("level", std::string("group"));
                x.contexts = e.value("contexts", std::vector<std::string>());
                x.gesture = e.value("gesture", std::string());
                x.temporal = e.value("temporal", std::string());
                x.minFigures = e.value("minFigures", 1);
                x.maxFigures = e.value("maxFigures", 0);
                x.regions = e.value("regions", std::vector<std::string>());
                x.effects = e.value("effects", std::vector<std::string>());
                x.palette = e.value("palette", std::vector<std::string>());
                x.level0 = e.value("brightness", 1.0);
                x.speed = e.value("speed", 0.0);
                x.beats = e.value("beats", 0);
                x.figures = e.value("figures", std::vector<std::string>());
                x.limits = e.value("limits", std::vector<std::string>());
                x.variantOf = e.value("variantOf", std::string());
            }
            l.entries.push_back(std::move(x));
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
