#include "composer/MediaDirection.h"

#include "composer/VisualDirection.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>
#include <set>

using nlohmann::json;

namespace composer {

namespace {

uint64_t Hash(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

double Unit(uint64_t seed, const std::string& key) {
    return static_cast<double>(Hash(fmt::format("{}/{}", seed, key)) % 100000) / 100000.0;
}

bool Has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// Categories that suit any song a little: they never fight a theme.
bool Neutral(const std::string& category) {
    return category == "lights" || category == "backgrounds" || category == "stars" || category == "other";
}

std::string BandOf(const PlanSegment& s, const SceneLook* scene) {
    if (scene != nullptr && !scene->band.empty()) {
        return scene->band;
    }
    const double e = s.targetIntensity > 0 ? s.targetIntensity : s.energy;
    return s.priority >= 3 ? "climax" : e < 0.35 ? "low" : e < 0.65 ? "mid" : "high";
}

struct Writer {
    const json* md = nullptr;
    std::vector<std::pair<std::string, std::string>> settings;
    std::vector<std::string> problems;

    // A metadata property as the effect panel saves it, checked before it is kept.
    void Set(const std::string& id, const std::string& value) {
        const json* property = nullptr;
        if (md != nullptr && md->contains("properties")) {
            for (const json& p : (*md)["properties"]) {
                if (p.value("id", std::string()) == id) {
                    property = &p;
                    break;
                }
            }
        }
        SettingShape shape;
        std::string error;
        if (property == nullptr || !ShapeOf(*property, shape, error)) {
            problems.push_back(fmt::format("{}: {}", id, property == nullptr ? std::string("unknown property") : error));
            return;
        }
        const std::string problem = CheckSetting(*md, shape.key, value);
        if (!problem.empty()) {
            problems.push_back(fmt::format("{}={}: {}", shape.key, value, problem));
            return;
        }
        settings.emplace_back(shape.key, value);
    }
    void Raw(const std::string& key, const std::string& value) {
        const std::string problem = md != nullptr ? CheckSetting(*md, key, value) : std::string("no metadata");
        if (!problem.empty()) {
            problems.push_back(fmt::format("{}={}: {}", key, value, problem));
            return;
        }
        settings.emplace_back(key, value);
    }
};

class Director {
public:
    explicit Director(const MediaPlanInput& in) : _in(in), _plan(*in.plan), _caps(*in.capabilities) {}

    MediaPlan Run() {
        _out.themeCategories = _in.themeCategories;
        _out.themeId = _in.direction != nullptr ? _in.direction->themeId : std::string();
        if (_in.library != nullptr) {
            for (const MediaItem& m : _in.library->items) {
                _out.usableItems += m.Usable() ? 1 : 0;
            }
        }
        const double byLevel = _plan.level == AutomationLevel::Conservative ? 0.12 : _plan.level == AutomationLevel::Creative ? 0.32 : 0.22;
        _out.maxShare = _in.options.maxShare >= 0 ? std::min(1.0, _in.options.maxShare) : byLevel;
        FindTargets();
        Manual();
        if (_in.options.automatic && _out.usableItems > 0 && !_targets.empty() && _in.metadata != nullptr) {
            Automatic();
        } else if (_in.options.automatic) {
            _out.reasons.push_back({ _out.usableItems == 0 ? "no-usable-media" : _targets.empty() ? "no-media-target" : "no-metadata", "" });
        }
        std::sort(_out.assignments.begin(), _out.assignments.end(), [](const MediaAssignment& a, const MediaAssignment& b) { return a.startMS != b.startMS ? a.startMS < b.startMS : a.target < b.target; });
        long long ms = 0;
        for (const MediaAssignment& a : _out.assignments) {
            ms += a.endMS - a.startMS;
        }
        _out.share = _plan.durationMS > 0 ? std::round(static_cast<double>(ms) / _plan.durationMS * 1000.0) / 1000.0 : 0;
        _out.enabled = !_out.assignments.empty();
        return _out;
    }

private:
    const MediaPlanInput& _in;
    const CompositionPlan& _plan;
    const ModelCapabilities& _caps;
    MediaPlan _out;
    std::vector<const TargetInfo*> _targets;
    std::set<std::string> _removed;              // segment ids, or "segment|target"
    std::map<std::string, std::string> _byLook;  // look key -> media id
    std::map<std::string, std::string> _lookOf;  // media id -> the look it serves
    std::map<std::string, std::string> _targetOfLook;

    const SceneLook* Scene(const PlanSegment& s) const {
        return _in.direction != nullptr && _in.direction->enabled ? _in.direction->Scene(s.id) : nullptr;
    }

    // The largest matrices and mega trees that show a picture as one.
    void FindTargets() {
        for (const TargetInfo& t : _caps.targets) {
            const RoleAssignment* role = t.PrimaryRole();
            if (t.type != TargetType::Model || t.Excluded() || _caps.LightsExcluded(t.name) || role == nullptr || (role->role != Role::Matrix && role->role != Role::MegaTree) ||
                t.Can(Capability::Image).support != Support::Yes) {
                continue;
            }
            _targets.push_back(&t);
        }
        std::sort(_targets.begin(), _targets.end(), [](const TargetInfo* a, const TargetInfo* b) {
            if (a->noController != b->noController) {
                return !a->noController;
            }
            const long long aa = static_cast<long long>(a->bufferWi) * a->bufferHt;
            const long long bb = static_cast<long long>(b->bufferWi) * b->bufferHt;
            return aa != bb ? aa > bb : a->name < b->name;
        });
        if (static_cast<int>(_targets.size()) > _in.options.maxTargets) {
            _targets.resize(static_cast<size_t>(_in.options.maxTargets));
        }
    }

    const PlanSegment* SegmentFor(const MediaCorrection& c) const {
        for (const PlanSegment& s : _plan.segments) {
            if (s.id == c.segmentId && (c.startMS < 0 || std::abs(s.startMS - c.startMS) < 2000)) {
                return &s;
            }
        }
        if (c.startMS >= 0) {
            for (const PlanSegment& s : _plan.segments) {
                if (c.startMS >= s.startMS && c.startMS < s.endMS) {
                    return &s;
                }
            }
        }
        return nullptr;
    }

    const MediaItem* ItemFor(const MediaCorrection& c) const {
        if (_in.library == nullptr) {
            return nullptr;
        }
        if (const MediaItem* m = _in.library->Find(c.mediaId); m != nullptr) {
            return m;
        }
        if (!c.path.empty()) {
            return _in.library->FindByPath(MediaAbsolutePath(c.path, _in.showDir));
        }
        return nullptr;
    }

    // Why this target cannot show a picture in this segment, or empty when it can.
    std::string Busy(const PlanSegment& s, const TargetInfo& t) const {
        for (const PlanAssignment& a : s.assignments) {
            if (a.prominence == Prominence::Rest) {
                continue;
            }
            if (a.target == t.name) {
                if (a.intent == "lyric-text") {
                    return "lyrics-text-here";
                }
                if (a.intent == "face-sing") {
                    return "face-sings-here";
                }
                continue;
            }
            const TargetInfo* other = _caps.Find(a.target);
            if (other == nullptr) {
                continue;
            }
            if (other->type == TargetType::Group && Has(other->flatModels, t.name)) {
                return "group-lights-it:" + other->name;
            }
            if (other->type == TargetType::SubModel && other->parent == t.name) {
                return "submodel-lit:" + other->name;
            }
        }
        return {};
    }

    bool Removed(const PlanSegment& s, const std::string& target) const {
        return _removed.contains(s.id) || _removed.contains(s.id + "|" + target);
    }

    bool Assigned(const PlanSegment& s) const {
        return std::any_of(_out.assignments.begin(), _out.assignments.end(), [&](const MediaAssignment& a) { return a.segmentId == s.id; });
    }

    void Manual() {
        for (const MediaCorrection& c : _in.corrections) {
            if (c.action != MediaCorrection::Action::Remove) {
                continue;
            }
            const PlanSegment* s = SegmentFor(c);
            if (s == nullptr) {
                _out.warnings.push_back("media-correction-segment-missing:" + c.segmentId);
                continue;
            }
            _removed.insert(c.target.empty() ? s->id : s->id + "|" + c.target);
        }
        for (const MediaCorrection& c : _in.corrections) {
            if (c.action == MediaCorrection::Action::Remove) {
                continue;
            }
            const PlanSegment* s = SegmentFor(c);
            if (s == nullptr) {
                _out.warnings.push_back("media-correction-segment-missing:" + c.segmentId);
                continue;
            }
            const TargetInfo* t = _caps.Find(c.target.empty() && !_targets.empty() ? _targets.front()->name : c.target);
            if (t == nullptr || t->Excluded() || t->Can(Capability::Image).support == Support::No || t->Can(Capability::Image).support == Support::Limited) {
                _out.warnings.push_back(fmt::format("media-correction-target-unusable:{}:{}", s->id, c.target));
                continue;
            }
            const MediaItem* m = ItemFor(c);
            if (m == nullptr || !m->Usable()) {
                // The person's choice stays in the sidecar; the show just has no effect
                // pointing at a file that is not there.
                _out.warnings.push_back(fmt::format("media-missing:{}:{}", s->id, c.path.empty() ? c.mediaId : c.path));
                continue;
            }
            MediaAssignment a;
            if (!Make(*s, *t, *m, a)) {
                continue;
            }
            a.state = c.action == MediaCorrection::Action::Lock ? DecisionState::Locked : DecisionState::ManualEdit;
            a.reasons.insert(a.reasons.begin(), { c.action == MediaCorrection::Action::Lock ? "locked-by-person" : "chosen-by-person", m->name });
            const std::string busy = Busy(*s, *t);
            if (!busy.empty()) {
                a.constraints.push_back(busy);
                _out.warnings.push_back(fmt::format("media-over-plan:{}:{}:{}", s->id, t->name, busy));
            }
            if (const SceneLook* scene = Scene(*s); scene != nullptr) {
                _byLook[scene->lookKey] = m->id;
                _lookOf[m->id] = scene->lookKey;
            }
            _out.assignments.push_back(std::move(a));
        }
    }

    double CategoryFit(const MediaItem& m, std::string& why) const {
        if (_in.themeCategories.empty()) {
            why = "no-theme";
            return 0.5;
        }
        double best = -1;
        for (const std::string& c : m.categories) {
            const auto at = std::find(_in.themeCategories.begin(), _in.themeCategories.end(), c);
            if (at != _in.themeCategories.end()) {
                const double fit = 1.0 - 0.15 * static_cast<double>(at - _in.themeCategories.begin());
                if (fit > best) {
                    best = fit;
                    why = c;
                }
            }
        }
        if (best > 0) {
            return best;
        }
        // A file of another theme stays out; one with no theme of its own may fill in.
        const bool neutral = m.categories.empty() || std::all_of(m.categories.begin(), m.categories.end(), Neutral);
        why = neutral ? "neutral" : "other-theme";
        return neutral ? 0.3 : 0;
    }

    static double AspectFit(double item, const TargetInfo& t) {
        const double target = t.bufferHt > 0 ? static_cast<double>(t.bufferWi) / t.bufferHt : 1.0;
        if (item <= 0 || target <= 0) {
            return 0;
        }
        return std::exp(-std::fabs(std::log(item / target)) * 1.2);
    }

    // The settings for one file on one figure, every one checked against the metadata.
    bool Make(const PlanSegment& s, const TargetInfo& t, const MediaItem& m, MediaAssignment& a) {
        a.segmentId = s.id;
        a.target = t.name;
        a.startMS = s.startMS;
        a.endMS = s.endMS;
        a.mediaId = m.id;
        a.name = m.name;
        a.kind = m.kind;
        a.effect = m.kind == MediaKind::Video ? "Video" : "Pictures";
        const std::string kept = m.relative.empty() ? m.path : m.relative;
        a.path = kept;
        if (m.relative.empty()) {
            a.constraints.push_back("outside-show");
        }
        Writer w;
        w.md = _in.metadata != nullptr ? (*_in.metadata)(a.effect) : nullptr;
        if (w.md == nullptr) {
            _out.warnings.push_back("media-no-metadata:" + a.effect);
            return false;
        }
        const double target = t.bufferHt > 0 ? static_cast<double>(t.bufferWi) / t.bufferHt : 1.0;
        const double ratio = m.aspect > 0 ? m.aspect / target : 1.0;
        const bool close = std::fabs(std::log(ratio)) < 0.15;
        const bool fills = ratio > 0.5 && ratio < 2.0;
        if (a.effect == "Pictures") {
            w.Raw("E_TEXTCTRL_Pictures_Filename", kept);
            w.Set("Scaling", close ? "Scale To Fit" : fills ? "Scale Keep Aspect Ratio Crop" : "Scale Keep Aspect Ratio");
            if (m.kind == MediaKind::Animation) {
                w.Set("LoopGIF", "1");
                a.constraints.push_back("loops");
            }
        } else {
            w.Raw("E_FILEPICKERCTRL_Video_Filename", kept);
            const bool loops = m.durationMS > 0 && m.durationMS < s.endMS - s.startMS;
            w.Set("Video_DurationTreatment", loops ? "Loop" : "Normal");
            w.Set("Video_AspectRatio", close ? "0" : "1");
            if (loops) {
                a.constraints.push_back("loops");
            }
        }
        if (!close) {
            a.constraints.push_back(fills && a.effect == "Pictures" ? "cropped" : "letterboxed");
        }
        if (!w.problems.empty()) {
            for (const std::string& p : w.problems) {
                _out.warnings.push_back(fmt::format("media-setting-invalid:{}:{}", s.id, p));
            }
            return false;
        }
        a.settings = std::move(w.settings);
        a.reasons.push_back({ "aspect", fmt::format("{:.2f} on {:.2f} ({})", m.aspect, target, close ? "fits" : fills ? "fills, edges cut" : "whole, with bands") });
        return true;
    }

    void Automatic() {
        const double budget = _out.maxShare * _plan.durationMS;
        long long used = 0;
        for (const MediaAssignment& a : _out.assignments) {
            used += a.endMS - a.startMS;
        }
        // Sections with fewer words first, the instrumental ones best: a picture there
        // does not compete with the lyrics for the eye.
        struct Slot {
            const PlanSegment* s;
            double score;
        };
        std::vector<Slot> slots;
        for (const PlanSegment& s : _plan.segments) {
            const int length = s.endMS - s.startMS;
            if (s.priority <= 0 || length < _in.options.minSegmentMS || Has(s.flags, "silence") || Assigned(s) || _removed.contains(s.id)) {
                continue;
            }
            double score = 1.0 - 0.6 * std::clamp(s.vocalCoverage, 0.0, 1.0);
            if (s.sectionKind == "intro" || s.sectionKind == "bridge" || s.sectionKind == "instrumental" || s.sectionKind == "outro" || Has(s.flags, "intro") || Has(s.flags, "ending")) {
                score += 0.3;
            }
            score += 0.05 * Unit(_plan.seed, "media-slot/" + s.id);
            slots.push_back({ &s, score });
        }
        std::sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) { return a.score != b.score ? a.score > b.score : a.s->startMS < b.s->startMS; });
        std::set<int> taken; // section indexes with media, to keep two apart
        for (const MediaAssignment& a : _out.assignments) {
            for (const PlanSegment& s : _plan.segments) {
                if (s.id == a.segmentId) {
                    taken.insert(s.sectionIndex);
                }
            }
        }
        for (const Slot& slot : slots) {
            const PlanSegment& s = *slot.s;
            const int length = s.endMS - s.startMS;
            if (used + length > budget) {
                continue;
            }
            const SceneLook* scene = Scene(s);
            const std::string look = scene != nullptr ? scene->lookKey : fmt::format("group{}", s.repeatGroup);
            const auto again = s.repeatGroup >= 0 || scene != nullptr ? _byLook.find(look) : _byLook.end();
            const bool repeat = again != _byLook.end();
            // Next to another picture the song would look like a slideshow; a repeated
            // section is the exception, it keeps its picture.
            if (!repeat && (taken.contains(s.sectionIndex) || taken.contains(s.sectionIndex - 1) || taken.contains(s.sectionIndex + 1))) {
                continue;
            }
            const TargetInfo* target = nullptr;
            std::string busy;
            // A repeated section shows its picture where it was shown before, when it can.
            std::vector<const TargetInfo*> order = _targets;
            if (repeat) {
                const auto was = _targetOfLook.find(look);
                if (was != _targetOfLook.end()) {
                    std::stable_partition(order.begin(), order.end(), [&](const TargetInfo* t) { return t->name == was->second; });
                }
            }
            for (const TargetInfo* t : order) {
                if (Removed(s, t->name)) {
                    continue;
                }
                busy = Busy(s, *t);
                if (busy.empty()) {
                    target = t;
                    break;
                }
            }
            if (target == nullptr) {
                _out.reasons.push_back({ "segment-without-free-figure", fmt::format("{}: {}", s.id, busy.empty() ? std::string("removed") : busy) });
                continue;
            }
            const std::string band = BandOf(s, scene);
            const MediaItem* best = nullptr;
            double bestScore = 0;
            std::vector<Reason> bestReasons;
            if (repeat) {
                best = _in.library->Find(again->second);
                bestScore = 1.0;
                bestReasons.push_back({ "repeat-keeps-media", look });
            } else {
                for (const MediaItem& m : _in.library->items) {
                    if (!m.Usable() || m.disabled || _lookOf.contains(m.id)) {
                        continue;
                    }
                    std::string why;
                    const double category = CategoryFit(m, why);
                    if (category <= 0) {
                        continue;
                    }
                    const double aspect = AspectFit(m.aspect, *target);
                    double kind = 0;
                    if (band == "high" || band == "climax") {
                        kind = m.kind == MediaKind::Video ? 0.25 : m.kind == MediaKind::Animation ? 0.15 : 0;
                    } else if (band == "low" || band == "rest") {
                        kind = m.kind == MediaKind::Image ? 0.15 : m.kind == MediaKind::Video && m.durationMS >= length ? 0.05 : 0;
                    } else {
                        kind = m.kind == MediaKind::Video ? 0.1 : 0.05;
                    }
                    const double light = (band == "high" || band == "climax") && m.brightness >= 0.25 ? 0.05 : 0;
                    const double score = category * 0.55 + aspect * 0.35 + kind + light + 0.02 * Unit(_plan.seed, s.id + "/" + m.id);
                    if (score > bestScore || (score == bestScore && best != nullptr && m.id < best->id)) {
                        best = &m;
                        bestScore = score;
                        bestReasons = { { "theme-category", why == "no-theme" || why == "neutral" ? why : fmt::format("{} ({})", why, _out.themeId) },
                                        { "energy", fmt::format("{}: {}", band, MediaKindId(m.kind)) } };
                    }
                }
            }
            constexpr double MIN_SCORE = 0.35;
            if (best == nullptr || bestScore < MIN_SCORE) {
                _out.reasons.push_back({ "no-media-fits", fmt::format("{} on {}{}", s.id, target->name, best == nullptr ? std::string() : fmt::format(" (best {:.2f})", bestScore)) });
                continue;
            }
            MediaAssignment a;
            if (!Make(s, *target, *best, a)) {
                continue;
            }
            a.score = std::round(bestScore * 1000.0) / 1000.0;
            a.reasons.insert(a.reasons.end(), bestReasons.begin(), bestReasons.end());
            a.reasons.push_back({ "no-lyrics-text", target->name });
            a.reasons.push_back({ "share", fmt::format("{:.0f}% of {:.0f}%", (used + length) * 100.0 / std::max(1, _plan.durationMS), _out.maxShare * 100) });
            if (s.vocalCoverage < 0.2) {
                a.reasons.push_back({ "instrumental", fmt::format("{:.2f}", s.vocalCoverage) });
            }
            _byLook[look] = best->id;
            _lookOf[best->id] = look;
            _targetOfLook.emplace(look, target->name);
            taken.insert(s.sectionIndex);
            used += length;
            _out.assignments.push_back(std::move(a));
        }
    }
};

} // namespace

const MediaAssignment* MediaPlan::Find(const std::string& segmentId, const std::string& target) const {
    const auto it = std::find_if(assignments.begin(), assignments.end(), [&](const MediaAssignment& a) { return a.segmentId == segmentId && a.target == target; });
    return it != assignments.end() ? &*it : nullptr;
}

MediaPlan PlanMedia(const MediaPlanInput& input) {
    if (input.plan == nullptr || input.capabilities == nullptr) {
        MediaPlan out;
        out.warnings.push_back("media-input-incomplete");
        return out;
    }
    Director d(input);
    return d.Run();
}

void ApplyMediaPlan(CompositionPlan& plan, const MediaPlan& media, const ModelCapabilities& caps) {
    for (const MediaAssignment& m : media.assignments) {
        const auto s = std::find_if(plan.segments.begin(), plan.segments.end(), [&](const PlanSegment& p) { return p.id == m.segmentId; });
        const TargetInfo* t = caps.Find(m.target);
        if (s == plan.segments.end() || t == nullptr) {
            continue;
        }
        auto a = std::find_if(s->assignments.begin(), s->assignments.end(), [&](const PlanAssignment& p) { return p.target == m.target; });
        if (a == s->assignments.end()) {
            PlanAssignment fresh;
            fresh.target = t->name;
            fresh.type = t->type;
            const RoleAssignment* role = t->PrimaryRole();
            fresh.role = role != nullptr ? role->role : Role::Matrix;
            fresh.confidence = role != nullptr ? role->confidence : 0.5;
            s->assignments.push_back(std::move(fresh));
            std::sort(s->assignments.begin(), s->assignments.end(), [](const PlanAssignment& x, const PlanAssignment& y) { return x.target < y.target; });
            a = std::find_if(s->assignments.begin(), s->assignments.end(), [&](const PlanAssignment& p) { return p.target == m.target; });
        }
        a->prominence = Prominence::Lead;
        a->intent = "media";
        a->intensity = std::max(a->intensity, s->targetIntensity);
        a->state = m.state;
        a->reasons.push_back({ "media", m.name });
    }
}

namespace {

DecisionState StateFrom(const std::string& id) {
    for (DecisionState s : { DecisionState::Automatic, DecisionState::ManualEdit, DecisionState::ApprovedPreference, DecisionState::Locked }) {
        if (id == DecisionStateId(s)) {
            return s;
        }
    }
    return DecisionState::Automatic;
}

} // namespace

void UpsertMediaCorrection(std::vector<MediaCorrection>& list, const MediaCorrection& c) {
    const bool wholeSegment = c.action == MediaCorrection::Action::Remove && c.target.empty();
    std::erase_if(list, [&](const MediaCorrection& o) { return o.segmentId == c.segmentId && (wholeSegment || o.target.empty() || o.target == c.target); });
    list.push_back(c);
}

json MediaCorrectionsJson(const std::vector<MediaCorrection>& list) {
    json out = json::array();
    for (const MediaCorrection& c : list) {
        json j = { { "action", MediaActionId(c.action) }, { "segment", c.segmentId }, { "startMS", c.startMS } };
        if (!c.target.empty()) {
            j["target"] = c.target;
        }
        if (!c.mediaId.empty()) {
            j["media"] = c.mediaId;
        }
        if (!c.path.empty()) {
            j["path"] = c.path;
        }
        out.push_back(std::move(j));
    }
    return out;
}

bool MediaCorrectionsFrom(const json& list, std::vector<MediaCorrection>& out, std::string& error) {
    out.clear();
    if (!list.is_array()) {
        error = "media corrections: not a list";
        return false;
    }
    for (const json& j : list) {
        MediaCorrection c;
        if (!j.is_object() || !MediaActionFromId(j.value("action", std::string()), c.action)) {
            error = "media corrections: unknown action";
            return false;
        }
        c.segmentId = j.value("segment", std::string());
        c.startMS = j.value("startMS", -1);
        c.target = j.value("target", std::string());
        c.mediaId = j.value("media", std::string());
        c.path = j.value("path", std::string());
        if (c.segmentId.empty() && c.startMS < 0) {
            error = "media corrections: no segment";
            return false;
        }
        out.push_back(std::move(c));
    }
    return true;
}

json MediaSessionJson(const MediaSession& s) {
    json used = json::array();
    for (const MediaUse& u : s.used) {
        used.push_back({ { "segment", u.segmentId }, { "target", u.target }, { "startMS", u.startMS }, { "endMS", u.endMS }, { "media", u.mediaId }, { "path", u.path }, { "effect", u.effect },
                         { "state", DecisionStateId(u.state) } });
    }
    json root = { { "folders", s.folders }, { "automatic", s.automatic }, { "corrections", MediaCorrectionsJson(s.corrections) }, { "used", std::move(used) } };
    if (s.maxShare >= 0) {
        root["maxShare"] = s.maxShare;
    }
    return root;
}

bool MediaSessionFrom(const json& j, MediaSession& s, std::string& error) {
    s = MediaSession();
    if (!j.is_object()) {
        error = "media: not an object";
        return false;
    }
    s.folders = j.value("folders", std::vector<std::string>());
    s.automatic = j.value("automatic", true);
    s.maxShare = j.value("maxShare", -1.0);
    if (j.contains("corrections") && !MediaCorrectionsFrom(j["corrections"], s.corrections, error)) {
        return false;
    }
    for (const json& u : j.value("used", json::array())) {
        MediaUse m;
        m.segmentId = u.value("segment", std::string());
        m.target = u.value("target", std::string());
        m.startMS = u.value("startMS", 0);
        m.endMS = u.value("endMS", 0);
        m.mediaId = u.value("media", std::string());
        m.path = u.value("path", std::string());
        m.effect = u.value("effect", std::string());
        m.state = StateFrom(u.value("state", std::string()));
        s.used.push_back(std::move(m));
    }
    return true;
}

const char* MediaActionId(MediaCorrection::Action action) {
    switch (action) {
    case MediaCorrection::Action::Assign: return "assign";
    case MediaCorrection::Action::Lock: return "lock";
    default: return "remove";
    }
}

bool MediaActionFromId(const std::string& id, MediaCorrection::Action& action) {
    if (id == "assign") {
        action = MediaCorrection::Action::Assign;
    } else if (id == "lock") {
        action = MediaCorrection::Action::Lock;
    } else if (id == "remove") {
        action = MediaCorrection::Action::Remove;
    } else {
        return false;
    }
    return true;
}

std::string ToJson(const MediaPlan& p) {
    json root = { { "version", p.version }, { "enabled", p.enabled }, { "themeId", p.themeId }, { "themeCategories", p.themeCategories }, { "usableItems", p.usableItems },
                  { "maxShare", p.maxShare }, { "share", p.share }, { "warnings", p.warnings } };
    json reasons = json::array();
    for (const Reason& r : p.reasons) {
        reasons.push_back({ { "code", r.code }, { "detail", r.detail } });
    }
    root["reasons"] = std::move(reasons);
    json list = json::array();
    for (const MediaAssignment& a : p.assignments) {
        json settings = json::object();
        for (const auto& [k, v] : a.settings) {
            settings[k] = v;
        }
        json why = json::array();
        for (const Reason& r : a.reasons) {
            why.push_back({ { "code", r.code }, { "detail", r.detail } });
        }
        list.push_back({ { "segment", a.segmentId }, { "target", a.target }, { "startMS", a.startMS }, { "endMS", a.endMS }, { "media", a.mediaId }, { "path", a.path }, { "name", a.name },
                         { "kind", MediaKindId(a.kind) }, { "effect", a.effect }, { "settings", std::move(settings) }, { "state", DecisionStateId(a.state) }, { "score", a.score },
                         { "reasons", std::move(why) }, { "constraints", a.constraints } });
    }
    root["assignments"] = std::move(list);
    return root.dump(2);
}

std::string MediaReport(const MediaPlan& p) {
    std::string out = fmt::format("Media: {} assignments, {:.0f}% of the song (at most {:.0f}%), {} usable files, theme {}\n", p.assignments.size(), p.share * 100, p.maxShare * 100, p.usableItems,
                                  p.themeId.empty() ? std::string("none") : p.themeId);
    const auto clock = [](int ms) { return fmt::format("{}:{:02}.{:01}", ms / 60000, ms / 1000 % 60, ms / 100 % 10); };
    for (const MediaAssignment& a : p.assignments) {
        out += fmt::format("  {} - {} {} on {}: {} ({}, {})", clock(a.startMS), clock(a.endMS), a.effect, a.target, a.name, MediaKindId(a.kind), DecisionStateId(a.state));
        for (const Reason& r : a.reasons) {
            out += fmt::format("; {} {}", r.code, r.detail);
        }
        if (!a.constraints.empty()) {
            out += fmt::format(" [{}]", std::accumulate(std::next(a.constraints.begin()), a.constraints.end(), a.constraints.front(), [](std::string x, const std::string& y) { return x + ", " + y; }));
        }
        out += "\n";
    }
    for (const Reason& r : p.reasons) {
        out += fmt::format("  not used: {} {}\n", r.code, r.detail);
    }
    for (const std::string& w : p.warnings) {
        out += "  warning: " + w + "\n";
    }
    return out;
}

} // namespace composer
