#include "composer/Choreography.h"

#include "composer/ComposerText.h"
#include "composer/VisualDirection.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using json = nlohmann::json;

namespace composer {

namespace {

const char* const ACTION_IDS[] = { "set-template", "set-palette", "set-timing", "exclude", "confirm", "lock" };
const char* const PROMINENCE_IDS[] = { "lead", "support", "accent", "rest" };
const char* const DECISION_IDS[] = { "automatic", "manual-edit", "approved-preference", "locked" };
const char* const COMPATIBILITY_IDS[] = { "compatible", "limited", "unknown", "incompatible" };
const char* const LEVEL_IDS[] = { "conservative", "balanced", "creative" };

uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t Mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

double Unit(uint64_t seed, const std::string& key) {
    return static_cast<double>(Mix(seed ^ Fnv(key)) >> 11) * (1.0 / 9007199254740992.0);
}

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

int ToFrame(double ms, int frame) {
    return static_cast<int>(std::lround(ms / frame)) * frame;
}

bool Has(const std::vector<std::string>& list, const std::string& item) {
    return std::find(list.begin(), list.end(), item) != list.end();
}

enum class Band {
    Rest,
    Low,
    Mid,
    High,
    Climax
};

// How each level chooses. Conservative keeps one choice per kind of section, Balanced
// lets repeated sections differ, Creative lets every segment differ.
struct Style {
    int pool;
    bool keyOccurrence;
    bool keyOrdinal;
    bool rotateChoices;
    int accentEvery; // every how many downbeats an accent fires; 0 none
    double jitter;
    bool rotateTransitions;
};

Style StyleFor(AutomationLevel level) {
    switch (level) {
    case AutomationLevel::Conservative:
        return { 2, false, false, false, 0, 0.0, false };
    case AutomationLevel::Creative:
        return { 4, true, true, true, 1, 0.35, true };
    default:
        return { 3, true, false, true, 2, 0.15, true };
    }
}

double CompatibilityFactor(Compatibility c) {
    return c == Compatibility::Compatible ? 1.0 : c == Compatibility::Limited ? 0.6 : c == Compatibility::Unknown ? 0.4 : 0.0;
}

int ProminenceRank(Prominence p) {
    return p == Prominence::Lead ? 0 : p == Prominence::Support ? 1 : p == Prominence::Accent ? 2 : 3;
}

class Engine {
public:
    Engine(const ChoreographyInput& in, const StyleProfile& profile, Choreography& out) :
        _in(in),
        _plan(*in.plan),
        _caps(*in.capabilities),
        _lib(*in.library),
        _profile(profile),
        _out(out),
        _level(in.level.value_or(in.plan->level)),
        _seed(in.seed.value_or(in.plan->seed)),
        _style(StyleFor(_level)),
        _frame(std::max(1, in.frameMS)) {}

    void Run() {
        if (_in.alignment == nullptr || _in.lyricTrack.empty()) {
            _noTrack = true;
        }
        for (const PlanSegment& s : _plan.segments) {
            Segment(s);
        }
        // A real song seldom ends on a frame, and the last segment ends with the song:
        // nothing may run past the last whole frame.
        const int lastFrameEnd = _plan.durationMS - _plan.durationMS % _frame;
        for (ChoreographyEvent& e : _out.events) {
            e.endMS = std::min(e.endMS, lastFrameEnd);
        }
        _out.events.erase(std::remove_if(_out.events.begin(), _out.events.end(), [](const ChoreographyEvent& e) { return e.endMS <= e.startMS; }), _out.events.end());
        for (MediaCue& m : _out.mediaCues) {
            m.endMS = std::min(m.endMS, lastFrameEnd);
        }
        ApplyCorrections();
        Continuity();
        std::sort(_out.events.begin(), _out.events.end(), [](const ChoreographyEvent& a, const ChoreographyEvent& b) {
            return a.startMS != b.startMS ? a.startMS < b.startMS : a.target != b.target ? a.target < b.target : a.layer < b.layer;
        });
        std::sort(_out.warnings.begin(), _out.warnings.end());
        _out.warnings.erase(std::unique(_out.warnings.begin(), _out.warnings.end()), _out.warnings.end());
    }

private:
    Band BandOf(const PlanSegment& s) const {
        if (Has(s.flags, "silence")) {
            return Band::Rest;
        }
        if (Has(s.flags, "climax")) {
            return Band::Climax;
        }
        return s.energy >= _profile.highEnergy ? Band::High : s.energy >= _profile.lowEnergy ? Band::Mid : Band::Low;
    }

    std::string Key(const PlanSegment& s) const {
        std::string key = s.repeatGroup >= 0 ? fmt::format("g{}", s.repeatGroup) : fmt::format("s{}", s.sectionIndex);
        if (_style.keyOccurrence && s.repeatGroup >= 0) {
            key += fmt::format(":o{}", s.occurrence);
        }
        if (_style.keyOrdinal) {
            key += "/" + s.id.substr(s.id.find('.') + 1);
        }
        return key;
    }

    std::vector<int> Downbeats(int from, int to) const {
        std::vector<int> out;
        if (_in.analysis == nullptr) {
            return out;
        }
        for (const MusicalEvent& e : _in.analysis->events) {
            if (e.type == MusicalEventType::Downbeat && e.timeMS >= from && e.timeMS < to) {
                out.push_back(ToFrame(e.timeMS, _frame));
            }
        }
        return out;
    }

    // Templates for one decision, best first, with why each was kept.
    struct Option {
        const EffectTemplate* t;
        CompatibilityResult compat;
        double score;
    };

    std::vector<Option> Options(const PlanSegment& s, const PlanAssignment& a, const TargetInfo& target, const std::set<std::string>& usedEffects, const char* binding) {
        std::vector<Option> options;
        const double energy = s.energy;
        for (const EffectTemplate& t : _lib.templates) {
            if (!Has(t.intents, a.intent) || std::find(t.prominences.begin(), t.prominences.end(), a.prominence) == t.prominences.end() || t.binding != binding || !t.media.empty()) {
                continue;
            }
            if (!t.sections.empty() && !Has(t.sections, s.sectionKind)) {
                continue;
            }
            constexpr double ENERGY_TOLERANCE = 0.05;
            if (energy < t.energyMin - ENERGY_TOLERANCE || energy > t.energyMax + ENERGY_TOLERANCE) {
                continue;
            }
            CompatibilityResult compat = EvaluateCompatibility(t, target);
            if (compat.level == Compatibility::Incompatible) {
                continue;
            }
            const double middle = (t.energyMin + t.energyMax) / 2;
            const double half = std::max(0.1, (t.energyMax - t.energyMin) / 2);
            const double energyFit = std::clamp(1.0 - std::fabs(energy - middle) / (half * 2), 0.1, 1.0);
            const auto weight = _profile.templateWeights.find(t.id);
            double score = t.weight * (weight == _profile.templateWeights.end() ? 1.0 : weight->second) * CompatibilityFactor(compat.level) * energyFit;
            score *= MotifFactor(target, t);
            // Two figures running the same effect at once compete; a different one
            // is preferred when there is one. A scene's base is the exception: the
            // references run one effect across many families on purpose.
            if (usedEffects.contains(t.effect) && a.intent != "ensemble-bed") {
                score *= 0.5;
            }
            score += _style.jitter * (Unit(_seed, Key(s) + "/" + a.target + "/" + t.id) - 0.5);
            options.push_back({ &t, std::move(compat), score });
        }
        std::sort(options.begin(), options.end(), [](const Option& x, const Option& y) { return x.score != y.score ? x.score > y.score : x.t->id < y.t->id; });
        return options;
    }

    // With a scene, the motif's effects for the figure's family come first, in the order
    // of the scene's variant, so the whole house moves the same way in a scene.
    double MotifFactor(const TargetInfo& target, const EffectTemplate& t) const {
        if (_scene == nullptr) {
            return 1.0;
        }
        std::string family = _family;
        if (family.empty()) {
            const TargetInfo* model = target.type == TargetType::SubModel ? _caps.Find(target.parent) : &target;
            family = model != nullptr && model->type == TargetType::Model ? FamilyIdOf(*model) : std::string();
        }
        const std::vector<std::string>& preferred = MotifEffects(_scene->motif, family);
        if (preferred.empty()) {
            return 1.0;
        }
        const auto at = std::find(preferred.begin(), preferred.end(), t.effect);
        if (at == preferred.end()) {
            return 0.6;
        }
        const size_t rank = static_cast<size_t>(at - preferred.begin());
        const size_t first = _scene->variant % preferred.size();
        return rank == first ? 2.5 : 1.8;
    }

    // The palette: a family that suits the energy, and within the families the template
    // accepts, one no other figure of this segment is using yet.
    void ChoosePalette(ChoreographyEvent& e, const EffectTemplate& t, Band band, const std::string& key, int rank, double norm) {
        if (t.maxColors == 0 || t.paletteFamilies.empty()) {
            return;
        }
        if (_scene != nullptr && !_scene->colors.empty()) {
            // The scene's colours for every figure, each starting from a different one so
            // neighbours are not identical.
            const std::vector<std::string>& colors = _scene->colors;
            const int count = std::clamp(_profile.ensemble.colorsPerEvent, std::max(1, t.minColors), std::max(1, std::min<int>(t.maxColors, static_cast<int>(colors.size()))));
            e.colors.clear();
            for (int k = 0; k < count; ++k) {
                e.colors.push_back(colors[(static_cast<size_t>(rank) + k) % colors.size()]);
            }
            e.paletteId = "scene:" + _scene->lookKey + ":" + _scene->paletteKind;
            e.reasons.push_back({ "scene-palette", _scene->paletteKind });
            (void)band;
            (void)key;
            (void)norm;
            return;
        }
        static const std::vector<std::string> LOW = { "soft", "cool", "warm", "festive" };
        static const std::vector<std::string> MID = { "warm", "cool", "festive", "soft" };
        static const std::vector<std::string> HIGH = { "festive", "warm", "cool", "soft" };
        const std::vector<std::string>& order = band == Band::Low || band == Band::Rest ? LOW : band == Band::Mid ? MID : HIGH;
        std::vector<const PaletteDef*> candidates;
        std::string family;
        for (const std::string& f : order) {
            if (!Has(t.paletteFamilies, f)) {
                continue;
            }
            std::vector<const PaletteDef*> palettes;
            for (const PaletteDef& p : _lib.palettes) {
                if (p.family == f) {
                    palettes.push_back(&p);
                }
            }
            std::sort(palettes.begin(), palettes.end(), [](const PaletteDef* a, const PaletteDef* b) { return a->id < b->id; });
            const size_t start = palettes.empty() ? 0 : (Fnv(key + "/" + f) + static_cast<uint64_t>(rank)) % palettes.size();
            for (size_t k = 0; k < palettes.size(); ++k) {
                candidates.push_back(palettes[(start + k) % palettes.size()]);
            }
        }
        if (candidates.empty()) {
            return;
        }
        const auto others = [&](const PaletteDef* p) {
            const auto found = _segmentPalettes.find(p->id);
            if (found == _segmentPalettes.end()) {
                return 0;
            }
            return static_cast<int>(found->second.size()) - (found->second.contains(e.target) ? 1 : 0);
        };
        const PaletteDef* p = candidates.front();
        for (const PaletteDef* c : candidates) {
            if (others(c) < others(p)) {
                p = c;
            }
        }
        _segmentPalettes[p->id].insert(e.target);
        family = p->family;
        const int wanted = static_cast<int>(std::lround(t.minColors + (t.maxColors - t.minColors) * norm));
        const int count = std::clamp(wanted, std::max(1, t.minColors), std::min<int>(t.maxColors, static_cast<int>(p->colors.size())));
        e.paletteId = p->id;
        e.colors.assign(p->colors.begin(), p->colors.begin() + count);
        e.reasons.push_back({ "palette-family", family });
    }

    void SetParameters(ChoreographyEvent& e, const EffectTemplate& t, double norm, const std::string& key, int block) const {
        for (const TemplateParameter& p : t.parameters) {
            std::string value;
            if (p.kind == TemplateParameter::Kind::Fixed) {
                value = p.value;
            } else if (p.kind == TemplateParameter::Kind::Energy) {
                const double v = p.low + (p.high - p.low) * norm;
                value = p.integer ? std::to_string(static_cast<long long>(std::llround(v))) : fmt::format("{:.{}f}", v, p.decimals);
            } else {
                const size_t index = _style.rotateChoices ? (Fnv(key + "/" + e.target + "/" + p.id) + static_cast<uint64_t>(block)) % p.choices.size() : 0;
                value = p.choices[index];
            }
            e.settings.emplace_back(p.key, value);
        }
    }

    ChoreographyEvent Base(const PlanSegment& s, const PlanAssignment& a, const TargetInfo& target, const Option& o, int start, int end, const std::string& suffix) const {
        ChoreographyEvent e;
        e.id = s.id + "/" + a.target + "/" + suffix;
        e.target = a.target;
        e.type = target.type;
        e.role = a.role;
        e.prominence = a.prominence;
        e.intent = a.intent;
        e.segmentId = s.id;
        e.startMS = start;
        e.endMS = end;
        e.effect = o.t->effect;
        e.templateId = o.t->id;
        e.templateVersion = o.t->version;
        e.intensity = a.intensity;
        e.compatibility = o.compat.level;
        e.constraints = o.compat.reasons;
        e.confidence = Round3(a.confidence * CompatibilityFactor(o.compat.level));
        e.provenance = fmt::format("template:{}@{}", o.t->id, o.t->version);
        e.needsReview = o.compat.level != Compatibility::Compatible;
        e.reasons.push_back({ "template", o.t->id });
        e.reasons.push_back({ "energy", fmt::format("{:.2f}", s.energy) });
        for (const std::string& r : o.compat.reasons) {
            e.reasons.push_back({ "compatibility", r });
        }
        return e;
    }

    void Segment(const PlanSegment& s) {
        const Band band = BandOf(s);
        if (band == Band::Rest) {
            return;
        }
        std::vector<const PlanAssignment*> active;
        for (const PlanAssignment& a : s.assignments) {
            if (a.prominence != Prominence::Rest) {
                active.push_back(&a);
            }
        }
        std::sort(active.begin(), active.end(), [](const PlanAssignment* x, const PlanAssignment* y) {
            return ProminenceRank(x->prominence) != ProminenceRank(y->prominence) ? ProminenceRank(x->prominence) < ProminenceRank(y->prominence) : x->target < y->target;
        });
        const std::string key = Key(s);
        std::set<std::string> usedEffects;
        _segmentPalettes.clear();
        _scene = _in.direction != nullptr && _in.direction->enabled ? _in.direction->Scene(s.id) : nullptr;
        int rank = 0;
        for (const PlanAssignment* a : active) {
            const TargetInfo* target = _caps.Find(a->target);
            if (target == nullptr) {
                _out.conflicts.push_back({ a->target, s.startMS, s.endMS, "target-missing" });
                continue;
            }
            if (_caps.LightsExcluded(a->target)) {
                _out.conflicts.push_back({ a->target, s.startMS, s.endMS, "target-excluded" });
                continue;
            }
            if (a->intent == "face-sing") {
                Face(s, *a, *target, band, key, rank++);
            } else if (a->intent == "lyric-text") {
                Text(s, *a, *target, band, key, rank++, usedEffects);
            } else {
                Visual(s, *a, *target, band, key, rank++, usedEffects);
            }
        }
        if (_scene != nullptr) {
            Ensemble(s, band, key, rank, usedEffects);
        }
        _scene = nullptr;
    }

    // ------------------------------------------------------------ ensemble

    // Every family the plan left free gets a base effect, until the scene's share of the
    // display is lit; the plan's figures keep what they had and nothing is lit twice.
    void Ensemble(const PlanSegment& s, Band band, const std::string& key, int& rank, std::set<std::string>& usedEffects) {
        const EnsembleSettings& es = _profile.ensemble;
        const double scale = std::clamp(_profile.Level(_level).densityScale, 0.7, 1.0);
        std::set<std::string> claimed;
        std::vector<std::string> claimedTargets;
        for (const PlanAssignment& a : s.assignments) {
            if (a.prominence == Prominence::Rest) {
                continue;
            }
            const TargetInfo* t = _caps.Find(a.target);
            if (t == nullptr) {
                continue;
            }
            claimedTargets.push_back(a.target);
            if (t->type == TargetType::Group) {
                claimed.insert(t->flatModels.begin(), t->flatModels.end());
            } else {
                claimed.insert(t->type == TargetType::SubModel ? t->parent : t->name);
            }
        }
        long long total = 0;
        for (const VisualFamily& f : _in.direction->families) {
            total += f.nodes;
        }
        long long covered = 0;
        for (const std::string& m : claimed) {
            const TargetInfo* t = _caps.Find(m);
            covered += t != nullptr && !t->noController ? t->nodes : 0;
        }
        // The outline draws the house, so it comes first; the rest in an order that
        // changes from look to look but not between repeats of one.
        std::vector<const VisualFamily*> order;
        for (const VisualFamily& f : _in.direction->families) {
            if (f.id != "faces") {
                order.push_back(&f);
            }
        }
        std::stable_sort(order.begin(), order.end(), [&](const VisualFamily* a, const VisualFamily* b) {
            const bool ao = a->id == "outline";
            const bool bo = b->id == "outline";
            if (ao != bo) {
                return ao;
            }
            return Unit(_seed, _scene->lookKey + "/" + a->id) < Unit(_seed, _scene->lookKey + "/" + b->id);
        });
        const double wanted = _scene->coverage * scale * static_cast<double>(total);
        std::map<std::string, std::vector<std::string>> bedTargets;
        std::vector<std::string> familyOrder;
        std::vector<std::string> ensembleTargets;
        for (const VisualFamily* f : order) {
            if (total > 0 && static_cast<double>(covered) >= wanted) {
                break;
            }
            for (const FamilyTarget& ft : f->targets) {
                std::vector<FamilyTarget> parts;
                const bool shared = std::any_of(ft.models.begin(), ft.models.end(), [&](const std::string& m) { return claimed.contains(m); }) ||
                                    (ft.type == TargetType::Group && std::any_of(claimedTargets.begin(), claimedTargets.end(), [&](const std::string& c) { return _caps.Overlap(c, ft.target); }));
                if (!shared) {
                    parts.push_back(ft);
                } else if (ft.type == TargetType::Group) {
                    for (const std::string& m : ft.models) {
                        const TargetInfo* info = _caps.Find(m);
                        if (!claimed.contains(m) && info != nullptr) {
                            parts.push_back({ m, TargetType::Model, { m }, info->nodes, "" });
                        }
                    }
                }
                for (const FamilyTarget& part : parts) {
                    const bool calm = band == Band::Low && es.outlinesWhenCalm && !part.calmTarget.empty();
                    const std::string name = calm ? part.calmTarget : part.target;
                    const TargetInfo* info = _caps.Find(name);
                    if (info == nullptr || _caps.LightsExcluded(name)) {
                        continue;
                    }
                    // Lights shared with a figure the plan uses (a face's channels wired
                    // into a window, say) stay with that figure.
                    // So do lights shared by two families (a wiring the layout reports): the
                    // first one lit keeps them, as both cannot show at once.
                    const auto shares = [&](const std::string& c) { return c != name && _caps.Overlap(c, name); };
                    if (std::any_of(claimedTargets.begin(), claimedTargets.end(), shares) || std::any_of(ensembleTargets.begin(), ensembleTargets.end(), shares)) {
                        _out.warnings.push_back(fmt::format("ensemble-shares-lights:{}:{}", name, s.id));
                        continue;
                    }
                    ensembleTargets.push_back(name);
                    PlanAssignment a;
                    a.target = name;
                    a.type = info->type;
                    a.role = f->role;
                    a.prominence = Prominence::Support;
                    a.intent = "ensemble-bed";
                    a.intensity = s.targetIntensity > 0 ? s.targetIntensity : s.energy;
                    a.confidence = 0.6;
                    _family = f->id;
                    const size_t before = _out.events.size();
                    Visual(s, a, *info, band, key, rank++, usedEffects);
                    _family.clear();
                    for (size_t i = before; i < _out.events.size(); ++i) {
                        ChoreographyEvent& e = _out.events[i];
                        // Layer 0 is drawn on top: the base goes under, so the hits show.
                        e.layer = 1;
                        e.reasons.push_back({ "ensemble", f->id + (calm ? " (outline only: calm scene)" : "") });
                        e.reasons.push_back({ "motif", _scene->motif });
                    }
                    if (_out.events.size() > before) {
                        covered += part.nodes;
                        if (!calm) {
                            bedTargets[f->id].push_back(name);
                        }
                    }
                }
            }
            if (bedTargets.contains(f->id)) {
                familyOrder.push_back(f->id);
            }
        }
        Hits(s, band, key, bedTargets, familyOrder);
    }

    // Short hits on the beat, one family after another, over their base: what makes
    // the reference shows pulse with the music.
    void Hits(const PlanSegment& s, Band band, const std::string& key, const std::map<std::string, std::vector<std::string>>& bedTargets, const std::vector<std::string>& familyOrder) {
        const EnsembleSettings& es = _profile.ensemble;
        const double perMinute = _scene->hitsPerMinute * _profile.Level(_level).densityScale;
        if (perMinute <= 0 || _in.analysis == nullptr || bedTargets.empty()) {
            return;
        }
        std::vector<std::pair<int, bool>> beats; // time, downbeat
        for (const MusicalEvent& ev : _in.analysis->events) {
            if ((ev.type == MusicalEventType::Beat || ev.type == MusicalEventType::Downbeat) && ev.timeMS >= s.startMS && ev.timeMS < s.endMS) {
                beats.push_back({ ToFrame(ev.timeMS, _frame), ev.type == MusicalEventType::Downbeat });
            }
        }
        std::sort(beats.begin(), beats.end());
        beats.erase(std::unique(beats.begin(), beats.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), beats.end());
        if (beats.empty()) {
            _out.warnings.push_back("ensemble-hits-without-beats:" + s.id);
            return;
        }
        // The round props and the repeated ones carry the hits first, as in the references.
        static const std::vector<std::string> PREFERENCE = { "snowflakes", "stars", "arches", "trees", "props", "figures", "windows", "outline", "mega-tree", "matrix" };
        std::vector<std::string> families;
        for (const std::string& f : PREFERENCE) {
            if (bedTargets.contains(f)) {
                families.push_back(f);
            }
        }
        (void)familyOrder;
        // Dense styles light several families on one beat; sparse ones skip beats.
        const double wanted = perMinute * (s.endMS - s.startMS) / 60000.0;
        const double perBeat = wanted / static_cast<double>(beats.size());
        const int perHit = std::clamp(static_cast<int>(std::lround(perBeat)), 1, static_cast<int>(families.size()));
        const int every = perBeat >= 1 ? 1 : std::max(1, static_cast<int>(std::lround(1.0 / std::max(0.01, perBeat))));
        std::map<std::string, int> busyUntil;
        int fired = 0;
        for (size_t k = 0; k < beats.size(); k += static_cast<size_t>(every)) {
            const int start = beats[k].first;
            const int next = k + 1 < beats.size() ? beats[k + 1].first : s.endMS;
            const int end = ToFrame(std::min({ start + es.hitMS, s.endMS, std::max(start + _frame, next - _frame) }), _frame);
            if (end - start < _frame) {
                continue;
            }
            for (int h = 0; h < perHit; ++h) {
                const std::string& family = families[static_cast<size_t>(fired * perHit + h) % families.size()];
                for (const std::string& name : bedTargets.at(family)) {
                    const TargetInfo* info = _caps.Find(name);
                    if (info == nullptr || busyUntil[name] > start) {
                        continue;
                    }
                    PlanAssignment a;
                    a.target = name;
                    a.type = info->type;
                    a.role = RoleOf(*info);
                    a.prominence = Prominence::Accent;
                    a.intent = "ensemble-hit";
                    a.intensity = 0.9;
                    a.confidence = 0.5;
                    _family = family;
                    const std::set<std::string> none;
                    const std::vector<Option> options = Options(s, a, *info, none, "");
                    _family.clear();
                    if (options.empty()) {
                        continue;
                    }
                    const Option& o = options[static_cast<size_t>(Fnv(_scene->lookKey + "/" + family)) % std::min<size_t>(2, options.size())];
                    ChoreographyEvent e = Base(s, a, *info, o, start, end, fmt::format("h{}", k));
                    e.layer = 0;
                    e.reasons.push_back({ beats[k].second ? "downbeat" : "beat", std::to_string(start) });
                    e.reasons.push_back({ "ensemble-hit", family });
                    SetParameters(e, *o.t, 1.0, key, static_cast<int>(k));
                    const std::vector<std::string>& colors = _scene->colors;
                    e.colors = { beats[k].second ? _scene->accent : colors[static_cast<size_t>(fired) % colors.size()] };
                    e.paletteId = "scene:" + _scene->lookKey + ":" + (beats[k].second ? "accent" : _scene->paletteKind);
                    busyUntil[name] = end;
                    _out.events.push_back(std::move(e));
                }
            }
            ++fired;
        }
    }

    static Role RoleOf(const TargetInfo& t) {
        const RoleAssignment* r = t.PrimaryRole();
        return r != nullptr ? r->role : Role::Prop;
    }

    double Norm(const PlanSegment& s, const EffectTemplate& t) const {
        const double e = s.targetIntensity > 0 ? s.targetIntensity : s.energy;
        return std::clamp((e - t.energyMin) / std::max(0.05, t.energyMax - t.energyMin), 0.0, 1.0);
    }

    const Option* Pick(const std::vector<Option>& options, const PlanSegment& s, const std::string& target, std::string& rotation) const {
        if (options.empty()) {
            return nullptr;
        }
        const int pool = std::min<int>(_style.pool, static_cast<int>(options.size()));
        const int index = static_cast<int>(Fnv(Key(s) + "/" + target) % static_cast<uint64_t>(pool));
        rotation = fmt::format("{} of {}", index + 1, pool);
        return &options[index];
    }

    void Visual(const PlanSegment& s, const PlanAssignment& a, const TargetInfo& target, Band band, const std::string& key, int rank, std::set<std::string>& usedEffects) {
        const std::vector<Option> options = Options(s, a, target, usedEffects, "");
        std::string rotation;
        // A scene's base follows its motif exactly; the variety comes from the motif's
        // variant from scene to scene, not from a draw per figure.
        const bool bed = a.intent == "ensemble-bed";
        const Option* o = bed ? (options.empty() ? nullptr : &options.front()) : Pick(options, s, a.target, rotation);
        if (bed) {
            rotation = "motif";
        }
        if (o == nullptr) {
            _out.warnings.push_back(fmt::format("no-template:{}:{}", a.target, a.intent));
            return;
        }
        usedEffects.insert(o->t->effect);
        if (a.prominence == Prominence::Lead && s.priority >= 2) {
            Cue(s, a, target);
        }
        const EffectTemplate& t = *o->t;
        const double norm = Norm(s, t);
        if (t.rhythm == "downbeat") {
            Accents(s, a, target, *o, band, key, rank, norm, rotation);
            return;
        }
        // Long segments become blocks no longer than the template allows, cut on a
        // downbeat when there is one near the even split.
        const int length = s.endMS - s.startMS;
        const int blocks = std::max(1, static_cast<int>(std::ceil(static_cast<double>(length) / t.maxMS)));
        const std::vector<int> downbeats = Downbeats(s.startMS + 1, s.endMS);
        std::vector<int> cuts = { s.startMS };
        for (int b = 1; b < blocks; ++b) {
            int cut = ToFrame(s.startMS + static_cast<double>(length) * b / blocks, _frame);
            int best = cut;
            int distance = length / blocks / 4;
            for (int d : downbeats) {
                if (std::abs(d - cut) < distance) {
                    distance = std::abs(d - cut);
                    best = d;
                }
            }
            cuts.push_back(best);
        }
        cuts.push_back(s.endMS);
        for (size_t b = 0; b + 1 < cuts.size(); ++b) {
            ChoreographyEvent e = Base(s, a, target, *o, cuts[b], cuts[b + 1], std::to_string(b));
            e.reasons.push_back({ "score", fmt::format("{:.2f}", o->score) });
            e.reasons.push_back({ "rotation", rotation });
            if (cuts[b + 1] - cuts[b] < t.minMS) {
                e.needsReview = true;
                e.constraints.push_back("shorter-than-template-minimum");
            }
            SetParameters(e, t, norm, key, static_cast<int>(b));
            ChoosePalette(e, t, band, key, rank, norm);
            _out.events.push_back(std::move(e));
        }
    }

    // Short events on downbeats: an accent, never one per onset or per beat.
    void Accents(const PlanSegment& s, const PlanAssignment& a, const TargetInfo& target, const Option& o, Band band, const std::string& key, int rank, double norm, const std::string& rotation) {
        // Named per target, so a figure the plan used and nothing lit has its reason.
        if (_style.accentEvery == 0) {
            _out.warnings.push_back(fmt::format("accents-off-at-level:{}:{}", a.target, a.intent));
            return;
        }
        const std::vector<int> downbeats = Downbeats(s.startMS, s.endMS);
        if (downbeats.empty()) {
            _out.warnings.push_back(fmt::format("accents-without-downbeats:{}:{}", a.target, a.intent));
            return;
        }
        const EffectTemplate& t = *o.t;
        int fired = 0;
        for (size_t k = 0; k < downbeats.size(); k += static_cast<size_t>(_style.accentEvery)) {
            const int start = downbeats[k];
            const int next = k + 1 < downbeats.size() ? downbeats[k + 1] : s.endMS;
            const int length = std::min({ t.maxMS, std::max(t.minMS, (next - start) / 2), next - start, s.endMS - start });
            const int end = ToFrame(start + length, _frame);
            if (end - start < t.minMS) {
                continue;
            }
            ChoreographyEvent e = Base(s, a, target, o, start, end, fmt::format("a{}", fired++));
            e.reasons.push_back({ "downbeat", std::to_string(start) });
            e.reasons.push_back({ "rotation", rotation });
            SetParameters(e, t, norm, key, static_cast<int>(k));
            ChoosePalette(e, t, band, key, rank, norm);
            _out.events.push_back(std::move(e));
        }
        if (fired == 0) {
            _out.warnings.push_back(fmt::format("accents-do-not-fit:{}:{}", a.target, a.intent));
        }
    }

    void Text(const PlanSegment& s, const PlanAssignment& a, const TargetInfo& target, Band band, const std::string& key, int rank, std::set<std::string>& usedEffects) {
        if (_in.alignment == nullptr) {
            _out.warnings.push_back(fmt::format("text-without-lyrics:{}:{}", a.target, a.intent));
            return;
        }
        const std::vector<Option> options = Options(s, a, target, usedEffects, "lyrics-phrase");
        if (options.empty()) {
            _out.warnings.push_back(fmt::format("no-template:{}:lyric-text", a.target));
            return;
        }
        const Option& o = options.front();
        const EffectTemplate& t = *o.t;
        int k = 0;
        for (const AlignedPhrase& p : _in.alignment->phrases) {
            if (p.startMS < 0 || p.instrumental) {
                continue;
            }
            const int start = ToFrame(std::max(p.startMS, s.startMS), _frame);
            const int end = ToFrame(std::min(p.endMS, s.endMS), _frame);
            if (end - start < t.minMS) {
                continue;
            }
            ChoreographyEvent e = Base(s, a, target, o, start, end, fmt::format("p{}", k++));
            SetParameters(e, t, Norm(s, t), key, k);
            // Only the person's words, and moving when they do not fit: about six
            // columns a character, which the render confirms.
            const size_t characters = text::Decode(p.text).size();
            const bool fits = static_cast<int>(characters) * 6 <= target.bufferWi;
            e.settings.emplace_back("E_TEXTCTRL_Text", p.text);
            e.settings.emplace_back("E_CHOICE_Text_Dir", fits ? "none" : "left");
            e.reasons.push_back({ "phrase", p.id });
            if (!fits) {
                e.constraints.push_back("text-scrolls-to-fit");
                e.compatibility = e.compatibility == Compatibility::Compatible ? Compatibility::Limited : e.compatibility;
            }
            e.needsReview = true; // legibility is only judged by eye
            ChoosePalette(e, t, band, key, rank, 0);
            _out.events.push_back(std::move(e));
        }
        if (k == 0) {
            _out.warnings.push_back(fmt::format("text-without-phrase-in-segment:{}:{}", a.target, a.intent));
        }
        usedEffects.insert(t.effect);
    }

    void Face(const PlanSegment& s, const PlanAssignment& a, const TargetInfo& target, Band band, const std::string& key, int rank) {
        if (_noTrack) {
            _out.warnings.push_back(fmt::format("face-without-synchronized-track:{}:{}", a.target, a.intent));
            return;
        }
        // The Phase 4 track already holds the mouth; this only points the face at it,
        // and only where it moves. A face of one voice reads that voice's track alone.
        std::string track = _in.lyricTrack;
        std::set<std::string> phrases;
        if (!a.voice.empty()) {
            const auto it = _in.voiceTracks.find(a.voice);
            if (it == _in.voiceTracks.end()) {
                _out.warnings.push_back(fmt::format("face-voice-without-track:{}:{}", a.target, a.voice));
                return;
            }
            track = it->second;
            for (const AlignedPhrase& p : _in.alignment->phrases) {
                if (std::find(p.voices.begin(), p.voices.end(), a.voice) != p.voices.end()) {
                    phrases.insert(p.id);
                }
            }
        }
        std::map<std::string, std::string> phraseOfWord;
        for (const AlignedWord& w : _in.alignment->words) {
            phraseOfWord[w.id] = w.phraseId;
        }
        const bool sung = std::any_of(_in.alignment->phonemes.begin(), _in.alignment->phonemes.end(), [&](const AlignedPhoneme& p) {
            if (p.shape == "rest" || p.startMS >= s.endMS || p.endMS <= s.startMS) {
                return false;
            }
            return a.voice.empty() || phrases.contains(phraseOfWord[p.wordId]);
        });
        if (!sung || target.faces.empty()) {
            _out.warnings.push_back(fmt::format("{}:{}:{}", sung ? "face-without-definition" : "face-without-phonemes-in-segment", a.target, a.intent));
            return;
        }
        const std::set<std::string> none;
        const std::vector<Option> options = Options(s, a, target, none, "face-track");
        if (options.empty()) {
            _out.warnings.push_back(fmt::format("no-template:{}:face-sing", a.target));
            return;
        }
        const Option& o = options.front();
        ChoreographyEvent e = Base(s, a, target, o, s.startMS, s.endMS, "0");
        SetParameters(e, *o.t, 0, key, 0);
        e.settings.emplace_back("E_CHOICE_Faces_FaceDefinition", target.faces.front());
        e.settings.emplace_back("E_CHOICE_Faces_TimingTrack", track);
        e.reasons.push_back({ "lyric-track", track });
        if (!a.voice.empty()) {
            e.reasons.push_back({ "voice", a.voice });
        }
        ChoosePalette(e, *o.t, band, key, rank, 0);
        _out.events.push_back(std::move(e));
    }

    void Cue(const PlanSegment& s, const PlanAssignment& a, const TargetInfo& target) {
        for (const EffectTemplate& t : _lib.templates) {
            if (t.media.empty() || !Has(t.intents, a.intent) || EvaluateCompatibility(t, target).level == Compatibility::Incompatible) {
                continue;
            }
            _out.mediaCues.push_back({ s.id + "/" + a.target + "/media", a.target, s.startMS, s.endMS, t.media, fmt::format("template {} would fit; no file is available yet", t.id) });
            return;
        }
    }

    // ------------------------------------------------------------ continuity

    void Continuity() {
        std::sort(_out.events.begin(), _out.events.end(), [](const ChoreographyEvent& a, const ChoreographyEvent& b) { return a.target != b.target ? a.target < b.target : a.startMS < b.startMS; });
        std::vector<ChoreographyEvent> merged;
        for (ChoreographyEvent& e : _out.events) {
            if (!merged.empty()) {
                ChoreographyEvent& p = merged.back();
                const EffectTemplate* t = _lib.Find(e.templateId);
                const bool touching = p.target == e.target && p.layer == e.layer && p.endMS == e.startMS;
                const bool automatic = p.state == DecisionState::Automatic && e.state == DecisionState::Automatic;
                // The same effect with the same settings is prolonged, not cut and
                // started again, while it stays within the template's length.
                if (touching && automatic && p.templateId == e.templateId && p.settings == e.settings && p.paletteId == e.paletteId && t != nullptr && e.endMS - p.startMS <= t->maxMS) {
                    p.endMS = e.endMS;
                    p.reasons.push_back({ "extended-into", e.segmentId });
                    continue;
                }
            }
            merged.push_back(std::move(e));
        }
        _out.events = std::move(merged);
        for (size_t i = 0; i < _out.events.size(); ++i) {
            ChoreographyEvent& e = _out.events[i];
            if (e.state == DecisionState::Locked) {
                continue;
            }
            const EffectTemplate* t = _lib.Find(e.templateId);
            const bool canFade = t != nullptr && !t->transitions.empty();
            // Neighbours on the same row and layer: a hit above a base is not its neighbour.
            const ChoreographyEvent* before = nullptr;
            for (size_t k = i; k-- > 0 && _out.events[k].target == e.target;) {
                if (_out.events[k].layer == e.layer) {
                    before = &_out.events[k];
                    break;
                }
            }
            const ChoreographyEvent* after = nullptr;
            for (size_t k = i + 1; k < _out.events.size() && _out.events[k].target == e.target; ++k) {
                if (_out.events[k].layer == e.layer) {
                    after = &_out.events[k];
                    break;
                }
            }
            const int duration = e.endMS - e.startMS;
            if (e.intent == "ensemble-hit") {
                // A hit starts at once and dies away, as the references' hits do.
                e.continuation = "new";
                e.transitionIn.clear();
                e.fadeInMS = 0;
                e.transitionOut = "Fade";
                e.fadeOutMS = duration * 3 / 5 / _frame * _frame;
                e.settings.erase(std::remove_if(e.settings.begin(), e.settings.end(), [](const auto& kv) { return kv.first.rfind("T_", 0) == 0; }), e.settings.end());
                if (e.fadeOutMS > 0) {
                    e.settings.emplace_back("T_CHOICE_Out_Transition_Type", e.transitionOut);
                    e.settings.emplace_back("T_TEXTCTRL_Fadeout", fmt::format("{:.2f}", e.fadeOutMS / 1000.0));
                }
                std::sort(e.settings.begin(), e.settings.end());
                continue;
            }
            if (before != nullptr && before->endMS == e.startMS && before->templateId == e.templateId) {
                e.continuation = "vary";
                if (canFade) {
                    e.transitionIn = "Fade";
                    e.fadeInMS = std::min(300, duration / 10);
                }
            } else {
                e.continuation = "new";
                if (canFade) {
                    const size_t pick = _style.rotateTransitions ? Fnv(e.id) % t->transitions.size() : 0;
                    e.transitionIn = t->transitions[pick];
                    const int fade = e.intensity >= 0.7 ? 300 : e.intensity >= 0.4 ? 600 : 1000;
                    e.fadeInMS = std::min(fade, duration / 4);
                }
            }
            if (canFade && (after == nullptr || after->startMS > e.endMS + 500)) {
                e.transitionOut = "Fade";
                e.fadeOutMS = std::min(800, duration / 5);
            }
            e.fadeInMS = e.fadeInMS / _frame * _frame;
            e.fadeOutMS = e.fadeOutMS / _frame * _frame;
            e.settings.erase(std::remove_if(e.settings.begin(), e.settings.end(), [](const auto& kv) { return kv.first.rfind("T_", 0) == 0; }), e.settings.end());
            if (e.fadeInMS > 0) {
                e.settings.emplace_back("T_CHOICE_In_Transition_Type", e.transitionIn);
                e.settings.emplace_back("T_TEXTCTRL_Fadein", fmt::format("{:.2f}", e.fadeInMS / 1000.0));
            } else {
                e.transitionIn.clear();
            }
            if (e.fadeOutMS > 0) {
                e.settings.emplace_back("T_CHOICE_Out_Transition_Type", e.transitionOut);
                e.settings.emplace_back("T_TEXTCTRL_Fadeout", fmt::format("{:.2f}", e.fadeOutMS / 1000.0));
            } else {
                e.transitionOut.clear();
            }
            std::sort(e.settings.begin(), e.settings.end());
        }
    }

    // ------------------------------------------------------------ corrections

    bool InRange(const ChoreographyEvent& e, const ChoreographyCorrection& c) const {
        if (c.startMS < 0) {
            return true;
        }
        const int middle = (e.startMS + e.endMS) / 2;
        return middle >= c.startMS && middle < c.endMS;
    }

    void ApplyCorrections() {
        _out.corrections = _in.corrections;
        for (const ChoreographyCorrection& c : _in.corrections) {
            switch (c.action) {
            case ChoreographyCorrection::Action::Exclude:
                _out.events.erase(std::remove_if(_out.events.begin(), _out.events.end(), [&](const ChoreographyEvent& e) { return e.target == c.target && InRange(e, c) && e.state != DecisionState::Locked; }), _out.events.end());
                break;
            case ChoreographyCorrection::Action::SetTemplate: {
                const EffectTemplate* t = _lib.Find(c.templateId);
                const TargetInfo* target = _caps.Find(c.target);
                if (t == nullptr || target == nullptr) {
                    _out.conflicts.push_back({ c.target, c.startMS, c.endMS, t == nullptr ? "template-unknown:" + c.templateId : "target-missing" });
                    break;
                }
                const CompatibilityResult compat = EvaluateCompatibility(*t, *target);
                bool applied = false;
                for (ChoreographyEvent& e : _out.events) {
                    if (e.target != c.target || !InRange(e, c) || e.state == DecisionState::Locked) {
                        continue;
                    }
                    if (compat.level == Compatibility::Incompatible || !Has(t->intents, e.intent) || !t->media.empty() || !t->binding.empty()) {
                        _out.conflicts.push_back({ c.target, e.startMS, e.endMS, "template-incompatible:" + c.templateId });
                        continue;
                    }
                    e.settings.clear();
                    e.effect = t->effect;
                    e.templateId = t->id;
                    e.templateVersion = t->version;
                    e.compatibility = compat.level;
                    e.constraints = compat.reasons;
                    e.provenance = "manual";
                    e.state = DecisionState::ManualEdit;
                    SetParameters(e, *t, 0.5, "manual", 0);
                    e.reasons.push_back({ "template-set-by-user", t->id });
                    applied = true;
                }
                const bool reported = std::any_of(_out.conflicts.begin(), _out.conflicts.end(), [&](const ChoreographyConflict& k) { return k.target == c.target && k.reason == "template-incompatible:" + c.templateId; });
                if (!applied && !reported) {
                    // Nothing to change is still an answer the person must see.
                    _out.conflicts.push_back({ c.target, c.startMS, c.endMS, compat.level == Compatibility::Incompatible ? "template-incompatible:" + c.templateId : "no-event-in-range" });
                }
                break;
            }
            case ChoreographyCorrection::Action::SetPalette: {
                const PaletteDef* p = _lib.Palette(c.paletteId);
                if (p == nullptr) {
                    _out.conflicts.push_back({ c.target, c.startMS, c.endMS, "palette-unknown:" + c.paletteId });
                    break;
                }
                for (ChoreographyEvent& e : _out.events) {
                    if (e.target == c.target && InRange(e, c) && e.state != DecisionState::Locked) {
                        const size_t count = std::max<size_t>(1, e.colors.size());
                        e.paletteId = p->id;
                        e.colors.assign(p->colors.begin(), p->colors.begin() + static_cast<std::ptrdiff_t>(std::min(count, p->colors.size())));
                        e.state = DecisionState::ManualEdit;
                        e.reasons.push_back({ "palette-set-by-user", p->id });
                    }
                }
                break;
            }
            case ChoreographyCorrection::Action::SetTiming: {
                auto it = std::find_if(_out.events.begin(), _out.events.end(), [&](const ChoreographyEvent& e) { return e.target == c.target && e.startMS == c.startMS; });
                if (it == _out.events.end() || c.newStartMS < 0 || c.newEndMS <= c.newStartMS || c.newEndMS > _plan.durationMS) {
                    _out.conflicts.push_back({ c.target, c.startMS, c.endMS, it == _out.events.end() ? "event-missing" : "timing-invalid" });
                    break;
                }
                it->startMS = ToFrame(c.newStartMS, _frame);
                it->endMS = ToFrame(c.newEndMS, _frame);
                it->state = DecisionState::ManualEdit;
                it->reasons.push_back({ "timing-set-by-user", "" });
                Clear(*it);
                break;
            }
            case ChoreographyCorrection::Action::Confirm:
                for (ChoreographyEvent& e : _out.events) {
                    if (e.target == c.target && InRange(e, c) && e.state == DecisionState::Automatic) {
                        e.state = DecisionState::ApprovedPreference;
                    }
                }
                break;
            case ChoreographyCorrection::Action::Lock: {
                const ChoreographyEvent& want = c.event;
                const TargetInfo* target = _caps.Find(want.target);
                const EffectTemplate* t = _lib.Find(want.templateId);
                std::string refusal;
                if (target == nullptr) {
                    refusal = "target-missing";
                } else if (_caps.LightsExcluded(want.target)) {
                    refusal = "target-excluded";
                } else if (t == nullptr) {
                    refusal = "template-unknown:" + want.templateId;
                } else if (EvaluateCompatibility(*t, *target).level == Compatibility::Incompatible) {
                    refusal = "template-incompatible:" + want.templateId;
                } else if (want.startMS < 0 || want.endMS <= want.startMS || want.endMS > _plan.durationMS) {
                    refusal = "timing-invalid";
                }
                if (!refusal.empty()) {
                    // The person's decision is not applied silently, and what was there stays.
                    _out.conflicts.push_back({ want.target, want.startMS, want.endMS, refusal });
                    break;
                }
                ChoreographyEvent locked = want;
                locked.state = DecisionState::Locked;
                locked.provenance = "manual";
                // A lock is usually taken on an automatic event that is still there, and
                // regenerating may put another one under the same id elsewhere.
                if (locked.id.rfind("lock:", 0) != 0) {
                    locked.id = "lock:" + locked.id;
                }
                Clear(locked);
                _out.events.push_back(std::move(locked));
                break;
            }
            }
        }
    }

    // Automatic events that would draw on the same lights give way to a manual one in
    // its time: on its own layer, or on a group or submodel sharing its lights.
    void Clear(const ChoreographyEvent& given) {
        const ChoreographyEvent manual = given; // `given` may live in the vector being rebuilt
        std::vector<ChoreographyEvent> kept;
        for (ChoreographyEvent& e : _out.events) {
            const bool itself = e.id == manual.id && e.state != DecisionState::Automatic;
            const bool shares = e.target == manual.target ? e.layer == manual.layer : _caps.Overlap(e.target, manual.target);
            if (!shares || e.endMS <= manual.startMS || e.startMS >= manual.endMS || itself) {
                kept.push_back(std::move(e));
                continue;
            }
            if (e.state != DecisionState::Automatic) {
                _out.conflicts.push_back({ manual.target, manual.startMS, manual.endMS, "overlaps-manual:" + e.id });
                kept.push_back(std::move(e));
                continue;
            }
            if (e.startMS < manual.startMS) {
                ChoreographyEvent left = e;
                left.endMS = manual.startMS;
                if (left.endMS - left.startMS >= _frame) {
                    kept.push_back(std::move(left));
                }
            }
            if (e.endMS > manual.endMS) {
                ChoreographyEvent right = e;
                right.startMS = manual.endMS;
                right.id += "+";
                if (right.endMS - right.startMS >= _frame) {
                    kept.push_back(std::move(right));
                }
            }
        }
        _out.events = std::move(kept);
    }

    const ChoreographyInput& _in;
    const CompositionPlan& _plan;
    const ModelCapabilities& _caps;
    const TemplateLibrary& _lib;
    const StyleProfile& _profile;
    Choreography& _out;
    AutomationLevel _level;
    uint64_t _seed;
    Style _style;
    int _frame;
    bool _noTrack = false;
    std::map<std::string, std::set<std::string>> _segmentPalettes; // palette -> targets, this segment
    const SceneLook* _scene = nullptr; // this segment's, with a direction
    std::string _family;               // the family an ensemble event is for
};

json ReasonsJson(const std::vector<Reason>& reasons) {
    json out = json::array();
    for (const Reason& r : reasons) {
        out.push_back(r.detail.empty() ? json{ { "code", r.code } } : json{ { "code", r.code }, { "detail", r.detail } });
    }
    return out;
}

json EventJson(const ChoreographyEvent& e) {
    json settings = json::array();
    for (const auto& [k, v] : e.settings) {
        settings.push_back({ k, v });
    }
    return { { "id", e.id }, { "target", e.target }, { "type", TargetTypeId(e.type) }, { "role", RoleId(e.role) }, { "prominence", PROMINENCE_IDS[static_cast<int>(e.prominence)] }, { "intent", e.intent },
             { "segment", e.segmentId }, { "startMS", e.startMS }, { "endMS", e.endMS }, { "layer", e.layer }, { "effect", e.effect }, { "template", e.templateId }, { "templateVersion", e.templateVersion },
             { "settings", settings }, { "palette", e.paletteId }, { "colors", e.colors }, { "intensity", e.intensity }, { "transitionIn", e.transitionIn }, { "fadeInMS", e.fadeInMS },
             { "transitionOut", e.transitionOut }, { "fadeOutMS", e.fadeOutMS }, { "continuation", e.continuation }, { "compatibility", COMPATIBILITY_IDS[static_cast<int>(e.compatibility)] },
             { "constraints", e.constraints }, { "reasons", ReasonsJson(e.reasons) }, { "confidence", e.confidence }, { "provenance", e.provenance }, { "state", DECISION_IDS[static_cast<int>(e.state)] },
             { "needsReview", e.needsReview } };
}

template <typename E, size_t N>
E EnumFrom(const char* const (&ids)[N], const std::string& id, E fallback) {
    for (size_t i = 0; i < N; ++i) {
        if (id == ids[i]) {
            return static_cast<E>(i);
        }
    }
    return fallback;
}

ChoreographyEvent EventFrom(const json& j) {
    ChoreographyEvent e;
    e.id = j.at("id").get<std::string>();
    e.target = j.at("target").get<std::string>();
    const std::string type = j.value("type", std::string("model"));
    e.type = type == "group" ? TargetType::Group : type == "submodel" ? TargetType::SubModel : TargetType::Model;
    (void)RoleFromId(j.value("role", std::string("prop")), e.role);
    e.prominence = EnumFrom(PROMINENCE_IDS, j.value("prominence", std::string("support")), Prominence::Support);
    e.intent = j.value("intent", std::string());
    e.segmentId = j.value("segment", std::string());
    e.startMS = j.at("startMS").get<int>();
    e.endMS = j.at("endMS").get<int>();
    e.layer = j.value("layer", 0);
    e.effect = j.value("effect", std::string());
    e.templateId = j.value("template", std::string());
    e.templateVersion = j.value("templateVersion", 0);
    for (const json& kv : j.value("settings", json::array())) {
        e.settings.emplace_back(kv.at(0).get<std::string>(), kv.at(1).get<std::string>());
    }
    e.paletteId = j.value("palette", std::string());
    e.colors = j.value("colors", std::vector<std::string>());
    e.intensity = j.value("intensity", 0.0);
    e.transitionIn = j.value("transitionIn", std::string());
    e.fadeInMS = j.value("fadeInMS", 0);
    e.transitionOut = j.value("transitionOut", std::string());
    e.fadeOutMS = j.value("fadeOutMS", 0);
    e.continuation = j.value("continuation", std::string());
    e.compatibility = EnumFrom(COMPATIBILITY_IDS, j.value("compatibility", std::string("compatible")), Compatibility::Compatible);
    e.constraints = j.value("constraints", std::vector<std::string>());
    for (const json& r : j.value("reasons", json::array())) {
        e.reasons.push_back({ r.at("code").get<std::string>(), r.value("detail", std::string()) });
    }
    e.confidence = j.value("confidence", 0.0);
    e.provenance = j.value("provenance", std::string());
    e.state = EnumFrom(DECISION_IDS, j.value("state", std::string("automatic")), DecisionState::Automatic);
    e.needsReview = j.value("needsReview", false);
    return e;
}

} // namespace

std::string ChoreographyEvent::PaletteString() const {
    std::string out;
    for (size_t i = 0; i < colors.size() && i < 8; ++i) {
        out += fmt::format("{}C_BUTTON_Palette{}={}", out.empty() ? "" : ",", i + 1, colors[i]);
    }
    for (size_t i = 0; i < colors.size() && i < 8; ++i) {
        out += fmt::format(",C_CHECKBOX_Palette{}=1", i + 1);
    }
    return out;
}

Choreography Choreograph(const ChoreographyInput& in) {
    Choreography out;
    const StyleProfile neutral = NeutralStarterProfile();
    const StyleProfile& profile = in.profile != nullptr ? *in.profile : neutral;
    if (in.plan == nullptr || in.capabilities == nullptr || in.library == nullptr) {
        out.warnings.push_back("missing-required-input");
        return out;
    }
    out.planHash = HashText(ToJson(*in.plan));
    out.libraryHash = in.library->hash;
    out.profileId = profile.id;
    out.level = in.level.value_or(in.plan->level);
    out.seed = in.seed.value_or(in.plan->seed);
    out.durationMS = in.plan->durationMS;
    out.frameMS = std::max(1, in.frameMS);
    out.lyricTrack = in.lyricTrack;
    for (const auto& [voice, track] : in.voiceTracks) {
        out.voiceTracks.push_back(track);
    }
    if (profile.id != in.plan->profileId) {
        out.warnings.push_back("profile-differs-from-plan");
    }
    if (!profile.learned) {
        out.warnings.push_back("profile-is-starting-parameters");
    }
    Engine engine(in, profile, out);
    engine.Run();
    out.metrics = MeasureChoreography(out);
    return out;
}

ChoreographyMetrics MeasureChoreography(const Choreography& c) {
    ChoreographyMetrics m;
    m.events = static_cast<int>(c.events.size());
    std::set<std::string> targets;
    std::map<std::string, long long> templateTime;
    std::set<std::string> palettes;
    long long total = 0;
    std::map<std::string, std::vector<const ChoreographyEvent*>> byTarget;
    for (const ChoreographyEvent& e : c.events) {
        targets.insert(e.target);
        templateTime[e.templateId] += e.endMS - e.startMS;
        total += e.endMS - e.startMS;
        if (!e.paletteId.empty()) {
            palettes.insert(e.paletteId);
        }
        m.accents += e.intent == "beat-accent" ? 1 : 0;
        m.needsReview += e.needsReview ? 1 : 0;
        byTarget[e.target].push_back(&e);
    }
    m.targets = static_cast<int>(targets.size());
    m.distinctTemplates = static_cast<int>(templateTime.size());
    m.distinctPalettes = static_cast<int>(palettes.size());
    for (const auto& [id, ms] : templateTime) {
        const double share = total > 0 ? static_cast<double>(ms) / total : 0;
        if (share > m.topTemplateShare) {
            m.topTemplateShare = Round3(share);
            m.topTemplate = id;
        }
    }
    int pairs = 0;
    int repeated = 0;
    int changes = 0;
    for (auto& [target, list] : byTarget) {
        std::sort(list.begin(), list.end(), [](const ChoreographyEvent* a, const ChoreographyEvent* b) { return a->startMS < b->startMS; });
        for (size_t i = 1; i < list.size(); ++i) {
            // Blocks of one segment keep their technique on purpose; repetition is
            // measured where a new segment could have changed it.
            if (list[i]->intent == "beat-accent" || list[i]->segmentId == list[i - 1]->segmentId) {
                continue;
            }
            ++pairs;
            repeated += list[i]->templateId == list[i - 1]->templateId ? 1 : 0;
            changes += list[i]->templateId != list[i - 1]->templateId ? 1 : 0;
            m.paletteChanges += list[i]->paletteId != list[i - 1]->paletteId ? 1 : 0;
        }
    }
    m.repeatedNeighbours = pairs > 0 ? Round3(static_cast<double>(repeated) / pairs) : 0;
    const double minutes = c.durationMS / 60000.0;
    m.changesPerMinute = minutes > 0 && m.targets > 0 ? Round3(changes / (minutes * m.targets)) : 0;
    long long activeSum = 0;
    int samples = 0;
    int idle = 0;
    for (int t = 0; t < c.durationMS; t += 100) {
        int active = 0;
        for (const ChoreographyEvent& e : c.events) {
            active += e.startMS <= t && t < e.endMS ? 1 : 0;
        }
        activeSum += active;
        m.maxActive = std::max(m.maxActive, active);
        idle += active == 0 ? 1 : 0;
        ++samples;
    }
    m.averageActive = samples > 0 ? Round3(static_cast<double>(activeSum) / samples) : 0;
    m.restShare = samples > 0 ? Round3(static_cast<double>(idle) / samples) : 0;
    m.mediaCues = static_cast<int>(c.mediaCues.size());
    return m;
}

SequenceSpec ToSequenceSpec(const Choreography& c, const std::vector<TimingTrackSpec>& tracks) {
    SequenceSpec spec;
    spec.frameMS = c.frameMS;
    spec.durationMS = c.durationMS;
    spec.timingTracks = tracks;
    for (const ChoreographyEvent& e : c.events) {
        EffectSpec effect;
        const size_t slash = e.type == TargetType::SubModel ? e.target.find('/') : std::string::npos;
        effect.target = slash == std::string::npos ? e.target : e.target.substr(0, slash);
        effect.subModel = slash == std::string::npos ? "" : e.target.substr(slash + 1);
        effect.layer = e.layer;
        effect.effect = e.effect;
        effect.settings = e.settings;
        effect.palette = e.PaletteString();
        effect.startMS = e.startMS;
        effect.endMS = e.endMS;
        effect.kind = e.type == TargetType::Group ? TargetKind::Group : e.type == TargetType::Model ? TargetKind::Model : TargetKind::Any;
        spec.effects.push_back(std::move(effect));
    }
    return spec;
}

const char* ChoreographyActionId(ChoreographyCorrection::Action action) {
    return ACTION_IDS[static_cast<int>(action)];
}

std::string ToJson(const Choreography& c) {
    json root;
    root["schemaVersion"] = c.schemaVersion;
    root["engineVersion"] = c.engineVersion;
    root["planHash"] = c.planHash;
    root["libraryHash"] = c.libraryHash;
    root["profileId"] = c.profileId;
    root["seed"] = c.seed;
    root["level"] = LEVEL_IDS[static_cast<int>(c.level)];
    root["durationMS"] = c.durationMS;
    root["frameMS"] = c.frameMS;
    root["lyricTrack"] = c.lyricTrack;
    if (!c.voiceTracks.empty()) {
        root["voiceTracks"] = c.voiceTracks;
    }
    root["warnings"] = c.warnings;
    json events = json::array();
    for (const ChoreographyEvent& e : c.events) {
        events.push_back(EventJson(e));
    }
    root["events"] = std::move(events);
    json cues = json::array();
    for (const MediaCue& m : c.mediaCues) {
        cues.push_back({ { "id", m.id }, { "target", m.target }, { "startMS", m.startMS }, { "endMS", m.endMS }, { "kind", m.kind }, { "reason", m.reason } });
    }
    root["mediaCues"] = std::move(cues);
    json corrections = json::array();
    for (const ChoreographyCorrection& k : c.corrections) {
        corrections.push_back({ { "action", ACTION_IDS[static_cast<int>(k.action)] }, { "target", k.target }, { "startMS", k.startMS }, { "endMS", k.endMS }, { "template", k.templateId },
                                { "palette", k.paletteId }, { "newStartMS", k.newStartMS }, { "newEndMS", k.newEndMS }, { "event", k.action == ChoreographyCorrection::Action::Lock ? EventJson(k.event) : json() } });
    }
    root["corrections"] = std::move(corrections);
    json conflicts = json::array();
    for (const ChoreographyConflict& k : c.conflicts) {
        conflicts.push_back({ { "target", k.target }, { "startMS", k.startMS }, { "endMS", k.endMS }, { "reason", k.reason } });
    }
    root["conflicts"] = std::move(conflicts);
    const ChoreographyMetrics& m = c.metrics;
    root["metrics"] = { { "events", m.events }, { "targets", m.targets }, { "distinctTemplates", m.distinctTemplates }, { "topTemplate", m.topTemplate }, { "topTemplateShare", m.topTemplateShare },
                        { "changesPerMinute", m.changesPerMinute }, { "repeatedNeighbours", m.repeatedNeighbours }, { "averageActive", m.averageActive }, { "maxActive", m.maxActive },
                        { "distinctPalettes", m.distinctPalettes }, { "paletteChanges", m.paletteChanges }, { "restShare", m.restShare }, { "accents", m.accents }, { "needsReview", m.needsReview },
                        { "mediaCues", m.mediaCues } };
    return root.dump(1);
}

bool FromJson(const std::string& text, Choreography& c, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    try {
        Choreography out;
        out.schemaVersion = root.at("schemaVersion").get<int>();
        if (out.schemaVersion > CHOREOGRAPHY_SCHEMA_VERSION) {
            error = "written by a newer version";
            return false;
        }
        out.engineVersion = root.value("engineVersion", std::string());
        out.planHash = root.value("planHash", std::string());
        out.libraryHash = root.value("libraryHash", std::string());
        out.profileId = root.value("profileId", std::string());
        out.seed = root.value("seed", static_cast<uint64_t>(0));
        out.level = EnumFrom(LEVEL_IDS, root.value("level", std::string("balanced")), AutomationLevel::Balanced);
        out.durationMS = root.value("durationMS", 0);
        out.frameMS = root.value("frameMS", 25);
        out.lyricTrack = root.value("lyricTrack", std::string());
        out.voiceTracks = root.value("voiceTracks", std::vector<std::string>());
        out.warnings = root.value("warnings", std::vector<std::string>());
        for (const json& e : root.value("events", json::array())) {
            out.events.push_back(EventFrom(e));
        }
        for (const json& m : root.value("mediaCues", json::array())) {
            out.mediaCues.push_back({ m.value("id", std::string()), m.value("target", std::string()), m.value("startMS", 0), m.value("endMS", 0), m.value("kind", std::string()), m.value("reason", std::string()) });
        }
        for (const json& k : root.value("corrections", json::array())) {
            ChoreographyCorrection corr;
            corr.action = EnumFrom(ACTION_IDS, k.value("action", std::string("set-template")), ChoreographyCorrection::Action::SetTemplate);
            corr.target = k.value("target", std::string());
            corr.startMS = k.value("startMS", -1);
            corr.endMS = k.value("endMS", -1);
            corr.templateId = k.value("template", std::string());
            corr.paletteId = k.value("palette", std::string());
            corr.newStartMS = k.value("newStartMS", -1);
            corr.newEndMS = k.value("newEndMS", -1);
            if (k.contains("event") && k["event"].is_object()) {
                corr.event = EventFrom(k["event"]);
            }
            out.corrections.push_back(std::move(corr));
        }
        for (const json& k : root.value("conflicts", json::array())) {
            out.conflicts.push_back({ k.value("target", std::string()), k.value("startMS", -1), k.value("endMS", -1), k.value("reason", std::string()) });
        }
        out.metrics = MeasureChoreography(out);
        c = std::move(out);
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::string ChoreographyReport(const Choreography& c) {
    const auto clock = [](int ms) { return fmt::format("{:02}:{:02}.{:01}", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 100); };
    const ChoreographyMetrics& m = c.metrics;
    std::string out = fmt::format("Choreography {} ({}), level {}, seed {}, profile {}, plan {}, library {}\n", c.engineVersion, c.schemaVersion, LEVEL_IDS[static_cast<int>(c.level)], c.seed, c.profileId, c.planHash, c.libraryHash);
    out += fmt::format("{} events on {} targets with {} templates; top {} at {:.0f}% of effect time\n", m.events, m.targets, m.distinctTemplates, m.topTemplate, m.topTemplateShare * 100);
    out += fmt::format("changes {:.2f}/min per target, same template next to itself {:.0f}%, active {:.1f} on average (max {}), nothing lit {:.0f}% of the song\n", m.changesPerMinute, m.repeatedNeighbours * 100,
                       m.averageActive, m.maxActive, m.restShare * 100);
    out += fmt::format("{} palettes, {} palette changes, {} accents, {} media cues, {} events to review by eye\n", m.distinctPalettes, m.paletteChanges, m.accents, m.mediaCues, m.needsReview);
    out += "Nothing here is visually validated: every template holds starting values.\n";
    for (const std::string& w : c.warnings) {
        out += "warning: " + w + "\n";
    }
    for (const ChoreographyConflict& k : c.conflicts) {
        out += fmt::format("conflict: {} {}-{} {}\n", k.target, k.startMS, k.endMS, k.reason);
    }
    for (const MediaCue& q : c.mediaCues) {
        out += fmt::format("media cue: {} {} - {} {} ({})\n", q.target, clock(q.startMS), clock(q.endMS), q.kind, q.reason);
    }
    std::string segment;
    std::vector<const ChoreographyEvent*> sorted;
    for (const ChoreographyEvent& e : c.events) {
        sorted.push_back(&e);
    }
    std::stable_sort(sorted.begin(), sorted.end(), [](const ChoreographyEvent* a, const ChoreographyEvent* b) { return a->segmentId != b->segmentId ? a->startMS < b->startMS : a->target < b->target; });
    for (const ChoreographyEvent* e : sorted) {
        if (e->intent == "beat-accent" && e->id.find("/a0") == std::string::npos) {
            continue; // one line per run of accents
        }
        std::string settings;
        for (const auto& [k, v] : e->settings) {
            if (k.rfind("E_", 0) == 0 && k != "E_TEXTCTRL_Text") {
                settings += fmt::format(" {}={}", k.substr(k.find('_', 2) + 1), v);
            }
        }
        out += fmt::format("{} - {}  {:<8} {:<30} {:<12} {:<18} {:<18} {:<8} in:{}{}{}{}\n      {}\n", clock(e->startMS), clock(e->endMS), PROMINENCE_IDS[static_cast<int>(e->prominence)], e->target, e->effect, e->templateId,
                           e->paletteId, e->continuation, e->transitionIn.empty() ? "-" : e->transitionIn, e->needsReview ? "  REVIEW" : "", e->state == DecisionState::Automatic ? "" : std::string("  ") + DECISION_IDS[static_cast<int>(e->state)],
                           e->intent == "beat-accent" ? "  (accents on downbeats)" : "", settings);
    }
    return out;
}

} // namespace composer
