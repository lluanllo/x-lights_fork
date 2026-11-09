#include "composer/MediaDirection.h"

#include "composer/MediaAnalysis.h"
#include "composer/MediaFraming.h"
#include "composer/LyricAligner.h"
#include "composer/SongAnalysis.h"
#include "composer/VisualDirection.h"
#include "composer/VisualModel.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
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

double R3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

bool Has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

bool HasPrefix(const std::vector<std::string>& v, const std::string& prefix) {
    return std::any_of(v.begin(), v.end(), [&](const std::string& s) { return s.rfind(prefix, 0) == 0; });
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

// The part a scene plays in the song, from its section, flags and energy: a guide for
// what suits it, never a fixed recipe.
std::string RoleOf(const PlanSegment& s, const std::string& band) {
    if (Has(s.flags, "climax") || s.priority >= 3 || band == "climax") {
        return "climax";
    }
    if (s.sectionKind == "intro" || Has(s.flags, "intro")) {
        return "establish";
    }
    if (s.sectionKind == "outro" || Has(s.flags, "ending")) {
        return "ending";
    }
    if (s.sectionKind == "chorus" || Has(s.flags, "user-chorus")) {
        return "chorus";
    }
    if (s.sectionKind == "bridge") {
        return "bridge";
    }
    if (s.sectionKind == "instrumental" || s.sectionKind == "interlude") {
        return "instrumental";
    }
    if (s.sectionKind == "verse" || s.sectionKind == "pre-chorus") {
        return "verse";
    }
    return band == "high" ? "chorus" : band == "low" || band == "rest" ? "breath" : "verse";
}

int RoleRank(const std::string& role) {
    // The opening presents the theme and the ending closes it: they are not left to
    // whatever the strongest scenes did not use.
    static const std::vector<std::string> order = { "climax", "chorus", "establish", "ending", "instrumental", "bridge", "verse", "breath" };
    const auto it = std::find(order.begin(), order.end(), role);
    return it != order.end() ? static_cast<int>(it - order.begin()) : 9;
}

const json* FindProperty(const json& list, const std::string& id) {
    for (const json& p : list) {
        if (p.contains("controls") && p["controls"].is_array()) {
            if (const json* inner = FindProperty(p["controls"], id); inner != nullptr) {
                return inner;
            }
        }
        if (p.value("id", std::string()) == id) {
            return &p;
        }
    }
    return nullptr;
}

struct Writer {
    const json* md = nullptr;
    std::vector<std::pair<std::string, std::string>> settings;
    std::vector<std::string> problems;

    const json* Property(const std::string& id) const {
        return md != nullptr && md->contains("properties") ? FindProperty((*md)["properties"], id) : nullptr;
    }

    // A metadata property as the effect panel saves it, checked before it is kept.
    void Set(const std::string& id, const std::string& value) {
        const json* property = Property(id);
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
        Put(shape.key, value);
    }
    // A property whose maximum the metadata leaves to the file (the video's start time):
    // its kind is checked against the metadata, its range against what the file allows.
    void SetWithin(const std::string& id, double value, double lo, double hi) {
        const json* property = Property(id);
        SettingShape shape;
        std::string error;
        if (property == nullptr || !ShapeOf(*property, shape, error) || !shape.numeric) {
            problems.push_back(fmt::format("{}: {}", id, property == nullptr ? std::string("unknown property") : error.empty() ? std::string("not a number") : error));
            return;
        }
        if (value < lo || value > hi) {
            problems.push_back(fmt::format("{}={:.2f}: outside {:.2f}..{:.2f}", shape.key, value, lo, hi));
            return;
        }
        Put(shape.key, fmt::format("{:.2f}", value));
    }
    void Raw(const std::string& key, const std::string& value) {
        const std::string problem = md != nullptr ? CheckSetting(*md, key, value) : std::string("no metadata");
        if (!problem.empty()) {
            problems.push_back(fmt::format("{}={}: {}", key, value, problem));
            return;
        }
        Put(key, value);
    }
    void Put(const std::string& key, const std::string& value) {
        const auto it = std::find_if(settings.begin(), settings.end(), [&](const auto& kv) { return kv.first == key; });
        if (it != settings.end()) {
            it->second = value;
        } else {
            settings.emplace_back(key, value);
        }
    }
};

struct Planned {
    StoryboardShot shot;
    std::vector<std::pair<std::string, std::string>> settings;
    std::vector<MediaPiece> pieces;
};

class Director {
public:
    explicit Director(const MediaPlanInput& in) : _in(in), _plan(*in.plan), _caps(*in.capabilities) {}

    MediaPlan Run() {
        _out.themeCategories = _in.themeCategories;
        _out.themeId = _in.direction != nullptr ? _in.direction->themeId : std::string();
        _seed = _plan.seed ^ (Hash(fmt::format("variant{}", _in.options.variant)) * (_in.options.variant != 0 ? 1u : 0u));
        _sb.seed = _plan.seed;
        _sb.variant = _in.options.variant;
        _sb.level = _plan.level == AutomationLevel::Conservative ? "conservative" : _plan.level == AutomationLevel::Creative ? "creative" : "balanced";
        _sb.themeId = _out.themeId;
        _minScore = _plan.level == AutomationLevel::Conservative ? 0.58 : _plan.level == AutomationLevel::Creative ? 0.45 : 0.5;
        int semantic = 0, analysed = 0;
        if (_in.library != nullptr) {
            for (const MediaItem& m : _in.library->items) {
                if (m.Usable()) {
                    ++_out.usableItems;
                    analysed += m.analysis.Done() ? 1 : 0;
                    semantic += m.analysis.mode == "semantic" ? 1 : 0;
                }
            }
        }
        _sb.analysisMode = analysed == 0 ? "none" : semantic == 0 ? "basic" : semantic == analysed ? "semantic" : "mixed";
        {
            std::string error;
            (void)ReadVisualConcepts(DefaultVisualConceptsPath(), _concepts, error);
        }
        BuildScenes();
        FindLanes();
        BuildCandidates();
        Manual();
        if (_in.options.automatic && !_cands.empty() && !_lanes.empty() && _in.metadata != nullptr) {
            Automatic();
        } else if (_in.options.automatic) {
            _out.reasons.push_back({ _out.usableItems == 0 ? "no-usable-media" : _lanes.empty() ? "no-media-target" : _cands.empty() ? "no-media-fits-theme" : "no-metadata", "" });
        }
        Transitions();
        Gaps();
        Finish();
        return _out;
    }

private:
    struct Lane {
        const TargetInfo* t = nullptr;
        FrameTarget frame;
        std::string role;
        double freeMS = 0;
        double budgetMS = 0;
        double reserveMS = 0; // only for the opening and the ending
        double usedMS = 0;
        bool main = true;
        std::string reason;
        std::string screen = "media"; // its role as a screen
        bool gapsOnly = false;        // shows words: pictures only between its phrases
        bool phraseLevel = false;     // words keep only their phrases, not the whole segment
        std::vector<std::string> sharesWith; // figures on the same channels
        int complexAgo = 99;          // shots since its last zoom or wipe
    };
    struct Scene {
        const PlanSegment* seg = nullptr;
        const SceneLook* look = nullptr;
        StoryboardScene info;
    };
    struct Cand {
        const MediaItem* m = nullptr;
        double theme = 0;
        std::string themeWhy;
        double energy = 0.5;
        std::vector<std::string> categories;
        double best = 0;
        bool used = false;
    };
    struct Eval {
        const Cand* c = nullptr;
        double score = -1;
        MediaFraming framing;
        std::vector<Reason> why;
    };

    const MediaPlanInput& _in;
    const CompositionPlan& _plan;
    const ModelCapabilities& _caps;
    MediaPlan _out;
    VisualMediaStoryboard& _sb = _out.storyboard;
    uint64_t _seed = 0;
    double _minScore = 0.5;
    std::vector<Scene> _scenes; // by time
    std::vector<Lane> _lanes;
    std::vector<Cand> _cands;
    std::set<std::string> _removed; // scene ids, or "scene|target"
    std::set<std::string> _avoid;   // "scene|target|media" a person did not want
    std::set<std::string> _avoidKeys; // "scene|target" with something a person did not want
    std::vector<Planned> _planned;
    std::set<std::string> _manual;                     // "scene|lane" with a person's shots
    std::map<std::string, std::string> _noFit;         // "scene|lane" -> why nothing was placed
    std::map<std::string, std::set<std::string>> _sceneUses; // media -> scenes it plays in
    std::map<std::string, std::set<int>> _fragmentUses;      // media -> fragments used
    std::map<std::string, std::string> _firstOnLook;         // "look|lane" -> media
    VisualConcepts _concepts;                                // what the concepts are, for relations

    const SceneLook* LookOf(const PlanSegment& s) const {
        return _in.direction != nullptr && _in.direction->enabled ? _in.direction->Scene(s.id) : nullptr;
    }

    Scene MakeScene(const PlanSegment& s) const {
        Scene sc;
        sc.seg = &s;
        sc.look = LookOf(s);
        StoryboardScene& i = sc.info;
        i.id = s.id;
        i.startMS = s.startMS;
        i.endMS = s.endMS;
        i.sectionKind = s.sectionKind;
        i.band = BandOf(s, sc.look);
        i.role = RoleOf(s, i.band);
        i.lookKey = sc.look != nullptr ? sc.look->lookKey : s.repeatGroup >= 0 ? fmt::format("group{}", s.repeatGroup) : std::string();
        i.energy = R3(std::clamp(s.targetIntensity > 0 ? s.targetIntensity : s.energy, 0.0, 1.0));
        i.vocals = R3(s.vocalCoverage);
        i.key = SegmentKey(s);
        return sc;
    }

    // "chorus#2.1": the segment's section kind, which section of that kind it is, and
    // which part of it.
    std::string SegmentKey(const PlanSegment& s) const {
        std::set<int> sections;
        int part = 0;
        for (const PlanSegment& o : _plan.segments) {
            if (o.sectionKind == s.sectionKind && o.sectionIndex <= s.sectionIndex) {
                sections.insert(o.sectionIndex);
            }
            if (o.sectionIndex == s.sectionIndex && o.startMS <= s.startMS) {
                ++part;
            }
        }
        return fmt::format("{}#{}.{}", s.sectionKind.empty() ? std::string("part") : s.sectionKind, sections.size(), std::max(1, part));
    }

    void BuildScenes() {
        for (const PlanSegment& s : _plan.segments) {
            if (s.priority <= 0 || s.endMS - s.startMS < _in.options.minSegmentMS || Has(s.flags, "silence")) {
                continue;
            }
            _scenes.push_back(MakeScene(s));
        }
        std::map<std::string, std::string> first;
        bool established = false;
        for (Scene& sc : _scenes) {
            if (!sc.info.lookKey.empty()) {
                const auto it = first.find(sc.info.lookKey);
                if (it != first.end()) {
                    sc.info.repeatOf = it->second;
                } else {
                    first[sc.info.lookKey] = sc.info.id;
                }
            }
            // The first scene that can show a picture presents the theme.
            if (!established && (sc.info.role == "verse" || sc.info.role == "breath")) {
                sc.info.role = "establish";
            }
            established = true;
        }
    }

    Scene* SceneById(const std::string& id) {
        const auto it = std::find_if(_scenes.begin(), _scenes.end(), [&](const Scene& s) { return s.info.id == id; });
        return it != _scenes.end() ? &*it : nullptr;
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

    bool ImageCapable(const TargetInfo& t) const {
        const RoleAssignment* role = t.PrimaryRole();
        if (t.type != TargetType::Model || t.Excluded() || _caps.LightsExcluded(t.name) || role == nullptr || (role->role != Role::Matrix && role->role != Role::MegaTree)) {
            return false;
        }
        const Support s = t.Can(Capability::Image).support;
        // A mega tree's buffer is its strings side by side, a matrix wrapped round: the
        // capability stays "unknown" for the rest of the planner, but a picture framed
        // for it (two faces, or a panorama all the way round) is drawn as planned.
        return s == Support::Yes || (role->role == Role::MegaTree && s == Support::Unknown && t.bufferWi >= 8 && t.bufferHt >= 16);
    }

    double Goal(const std::string& role) const {
        const bool tree = role == "mega-tree";
        switch (_plan.level) {
        case AutomationLevel::Conservative: return tree ? 0.25 : 0.4;
        case AutomationLevel::Creative: return tree ? 0.65 : 0.9;
        default: return tree ? 0.45 : 0.7;
        }
    }

    // A preference's value, 0 when there is none.
    double Pref(const std::string& key) const {
        if (_in.preferences == nullptr) {
            return 0;
        }
        const auto it = _in.preferences->find(key);
        return it != _in.preferences->end() ? it->second : 0;
    }

    static bool DarkItem(const MediaItem& m) {
        const MediaAnalysis& a = m.analysis;
        return Has(a.issues, "dark-spans") || (a.Done() && (a.dark >= 0.5 || a.brightness < 0.22));
    }

    // What the preferences add to a file's score on a screen in a scene, and the words
    // that say so.
    double Preferred(const MediaItem& m, const Scene& sc, const Lane& lane, const MediaFraming& f, std::string& why) const {
        if (_in.preferences == nullptr || _in.preferences->empty()) {
            return 0;
        }
        double total = 0;
        const auto term = [&](const std::string& key, double v, double k) {
            if (std::fabs(v) > 1e-6) {
                total += k * v;
                why += fmt::format("{}{} {:+.2f}", why.empty() ? "" : ", ", key, k * v);
            }
        };
        const bool video = m.kind == MediaKind::Video;
        const double kind = Pref("kind.video") + (_in.profileId.empty() ? 0 : Pref("profile." + _in.profileId + ".kind.video"));
        term("kind.video", video ? kind : -kind, 0.15);
        if (DarkItem(m)) {
            term("light.dark", Pref("light.dark"), 0.08);
        }
        for (size_t i = 0; i < m.analysis.description.size() && i < 2; ++i) {
            const std::string& c = m.analysis.description[i];
            term("concept." + c, Pref("concept." + c), 0.08);
            term("scene." + sc.info.role + ".concept." + c, Pref("scene." + sc.info.role + ".concept." + c), 0.08);
            term("screen." + lane.t->name + ".concept." + c, Pref("screen." + lane.t->name + ".concept." + c), 0.08);
        }
        term("file." + m.id + "@" + lane.t->name, Pref("file." + m.id + "@" + lane.t->name), 0.15);
        if (f.moves || f.motionClip) {
            const double moving = Pref("motion.moving") + Pref("scene." + sc.info.role + ".motion.moving") + (_in.profileId.empty() ? 0 : Pref("profile." + _in.profileId + ".motion.moving"));
            term("motion.moving", moving, 0.05);
        }
        return total;
    }

    // How much of its free time each kind of screen aims to fill.
    double GoalOf(const Lane& l) const {
        // A maker's taste for more or fewer pictures, on the whole and on this screen.
        const double taste = (1.0 + 0.3 * Pref("amount")) * (1.0 + 0.3 * Pref("screen." + l.t->name + ".presence"));
        return std::clamp(BaseGoalOf(l) * taste, 0.05, 0.95);
    }

    double BaseGoalOf(const Lane& l) const {
        const int level = _plan.level == AutomationLevel::Conservative ? 0 : _plan.level == AutomationLevel::Creative ? 2 : 1;
        if (l.screen == "secondary") {
            return std::array<double, 3>{ 0.3, 0.5, 0.7 }[level];
        }
        if (l.screen == "media") {
            return std::array<double, 3>{ 0.25, 0.4, 0.6 }[level];
        }
        if (l.screen == "lyrics") {
            return std::array<double, 3>{ 0.4, 0.6, 0.8 }[level];
        }
        return Goal(l.role);
    }

    Lane MakeLane(const TargetInfo& t, double freeMS, bool main) const {
        Lane l;
        l.t = &t;
        l.frame = FrameTargetOf(t);
        const RoleAssignment* role = t.PrimaryRole();
        l.role = (role != nullptr && role->role == Role::MegaTree) || l.frame.tree ? "mega-tree" : "matrix";
        l.freeMS = freeMS;
        l.main = main;
        Budget(l);
        return l;
    }

    void Budget(Lane& l) const {
        const double duration = std::max(1, _plan.durationMS);
        l.budgetMS = GoalOf(l) * l.freeMS;
        if (_in.options.maxShare >= 0) {
            l.budgetMS = std::min(l.budgetMS, std::min(1.0, _in.options.maxShare) * duration);
        }
        l.reserveMS = 0.15 * l.budgetMS;
    }

    // Whether the figure shows words in the segment.
    bool Words(const PlanSegment& s, const TargetInfo& t) const {
        return std::any_of(s.assignments.begin(), s.assignments.end(), [&](const PlanAssignment& a) { return a.target == t.name && a.intent == "lyric-text" && a.prominence != Prominence::Rest; });
    }

    // Where in the scene this screen can show a picture: all of it, nothing, or, on a
    // screen with words and phrase-level sharing, the stretches between its phrases.
    std::vector<std::pair<int, int>> Windows(const Scene& sc, const Lane& l, std::string& why) const {
        const PlanSegment& s = *sc.seg;
        const int start = sc.info.startMS, end = sc.info.endMS;
        // A figure sharing its lights must be dark for a picture to show.
        for (const PlanAssignment& a : s.assignments) {
            if (a.prominence != Prominence::Rest && Has(l.sharesWith, a.target)) {
                why = "shares-lights-with:" + a.target;
                return {};
            }
        }
        const std::string busy = Busy(s, *l.t);
        const bool words = busy == "lyrics-text-here";
        if (!busy.empty() && !words) {
            why = busy;
            return {};
        }
        if (!words) {
            return { { start, end } };
        }
        if (!l.phraseLevel) {
            why = busy;
            return {};
        }
        std::vector<std::pair<int, int>> taken;
        if (_in.alignment != nullptr) {
            for (const AlignedPhrase& p : _in.alignment->phrases) {
                if (p.startMS < 0 || p.instrumental || p.endMS <= start || p.startMS >= end) {
                    continue;
                }
                taken.emplace_back(p.startMS - 600, p.endMS + 600);
            }
        }
        std::sort(taken.begin(), taken.end());
        std::vector<std::pair<int, int>> out;
        int at = start;
        for (const auto& [a, b] : taken) {
            if (a - at >= 3000) {
                out.emplace_back(at, a);
            }
            at = std::max(at, b);
        }
        if (end - at >= 3000) {
            out.emplace_back(at, end);
        }
        if (out.empty()) {
            why = "no-pause-between-phrases";
        }
        return out;
    }

    // The screens from the display's surfaces and their roles: the main screen, the mega
    // tree, a second screen, extra media screens, and, when words and pictures share a
    // screen, the screen with words between its phrases.
    void FindLanes() {
        const ScreenSettings settings = _in.screens != nullptr ? *_in.screens : ScreenSettings();
        _out.surfaces = PlanSurfaces(_caps, &_plan, settings);
        _sb.lyricsMode = LyricsModeId(settings.mode);
        const auto rank = [](SurfaceRole r) {
            switch (r) {
            case SurfaceRole::Main: return 0;
            case SurfaceRole::Tree: return 1;
            case SurfaceRole::Secondary: return 2;
            case SurfaceRole::Media: return 3;
            case SurfaceRole::Lyrics: return 4;
            default: return 9;
            }
        };
        std::vector<const Surface*> order;
        for (const Surface& s : _out.surfaces.surfaces) {
            order.push_back(&s);
        }
        std::stable_sort(order.begin(), order.end(), [&](const Surface* a, const Surface* b) { return rank(a->role) < rank(b->role); });
        const double songMS = std::max(1, _plan.durationMS);
        for (const Surface* s : order) {
            if (rank(s->role) == 9 || (s->role == SurfaceRole::Lyrics && settings.mode != LyricsMode::Smart)) {
                continue;
            }
            const TargetInfo* t = _caps.Find(s->target);
            if (t == nullptr || !s->nativeOk) {
                continue;
            }
            Lane l = MakeLane(*t, 0, true);
            l.screen = SurfaceRoleId(s->role);
            l.gapsOnly = s->role == SurfaceRole::Lyrics;
            l.sharesWith = s->conflicts;
            l.phraseLevel = settings.mode != LyricsMode::LyricsFirst;
            if (const ScreenChoice* c = settings.Find(s->target); c != nullptr) {
                if (c->frontOffset >= 0) {
                    l.frame.frontOffset = c->frontOffset;
                }
                if (c->turn != 0) {
                    l.frame.turn = c->turn;
                }
            }
            double free = 0;
            for (const Scene& sc : _scenes) {
                std::string why;
                for (const auto& [a, b] : Windows(sc, l, why)) {
                    free += b - a;
                }
            }
            l.freeMS = free;
            Budget(l);
            l.reason = fmt::format("{} ({}): {} lights, seen {:.0f}%, free {:.0f}% of the song{}", SurfaceRoleId(s->role), s->roleSource, s->pixels, s->visibility * 100, free / songMS * 100,
                                   l.gapsOnly ? ", between its phrases" : "");
            for (const Surface& other : _out.surfaces.surfaces) {
                if (other.target != s->target && (other.kind == s->kind) && std::none_of(order.begin(), order.end(), [&](const Surface* o) { return o == &other && rank(o->role) < 9; })) {
                    l.reason += fmt::format("; {}: {}", other.target, SurfaceRoleId(other.role));
                }
            }
            _lanes.push_back(std::move(l));
        }
        for (const Lane& l : _lanes) {
            _out.maxShare = std::max(_out.maxShare, R3((l.budgetMS + l.reserveMS) / songMS));
        }
    }

    Lane* LaneOf(const std::string& target) {
        const auto it = std::find_if(_lanes.begin(), _lanes.end(), [&](const Lane& l) { return l.t->name == target; });
        return it != _lanes.end() ? &*it : nullptr;
    }

    double ThemeFit(const std::vector<std::string>& categories, std::string& why) const {
        if (_in.themeCategories.empty()) {
            why = "no-theme";
            return 0.6;
        }
        double best = -1;
        for (const std::string& c : categories) {
            const auto at = std::find(_in.themeCategories.begin(), _in.themeCategories.end(), c);
            if (at != _in.themeCategories.end()) {
                const double fit = 1.0 - 0.12 * static_cast<double>(at - _in.themeCategories.begin());
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
        const bool neutral = categories.empty() || std::all_of(categories.begin(), categories.end(), Neutral);
        why = neutral ? "neutral" : "other-theme";
        return neutral ? 0.35 : 0;
    }

    static double ItemEnergy(const MediaItem& m) {
        const MediaAnalysis& a = m.analysis;
        if (!a.Done()) {
            return m.kind == MediaKind::Video ? 0.55 : m.kind == MediaKind::Animation ? 0.5 : 0.35;
        }
        double e = m.kind == MediaKind::Image ? std::clamp(0.15 + 0.35 * a.complexity + 0.25 * a.saturation + 0.25 * a.contrast, 0.0, 1.0) : a.video.energy;
        return std::clamp(e, 0.0, 1.0);
    }

    void Discard(const MediaItem& m, const std::string& reason) {
        _sb.discarded.push_back({ m.id, m.name, reason, 0 });
    }

    void BuildCandidates() {
        if (_in.library == nullptr) {
            return;
        }
        for (const MediaItem& m : _in.library->items) {
            if (m.status == MediaStatus::Missing) {
                continue;
            }
            if (!m.Usable()) {
                Discard(m, m.status == MediaStatus::Unsupported ? "not-supported" : "damaged");
                continue;
            }
            const std::vector<std::string>& issues = m.analysis.issues;
            if (m.disabled) {
                Discard(m, "only-when-chosen");
            } else if (HasPrefix(issues, "duplicate-of:")) {
                Discard(m, "duplicate");
            } else if (Has(issues, "very-dark")) {
                Discard(m, "very-dark");
            } else if (Has(issues, "low-resolution")) {
                Discard(m, "too-small");
            } else if (Has(issues, "damaged")) {
                Discard(m, "damaged");
            } else {
                Cand c;
                c.m = &m;
                c.categories = EffectiveCategories(m);
                c.theme = ThemeFit(c.categories, c.themeWhy);
                c.energy = ItemEnergy(m);
                if (c.theme <= 0) {
                    Discard(m, "off-theme");
                    continue;
                }
                _cands.push_back(std::move(c));
            }
        }
    }

    double RoleFit(const Cand& c, const Scene& sc, const Lane& lane) const {
        const MediaItem& m = *c.m;
        const MediaAnalysis& a = m.analysis;
        const bool moving = m.kind != MediaKind::Image;
        const double motion = moving ? (a.Done() ? a.video.motion : 0.4) : 0;
        const std::string& role = sc.info.role;
        double r = 0.5;
        if (role == "establish") {
            r = 0.45 + (lane.role == "matrix" && (a.orientation == "panoramic" || a.orientation == "landscape") ? 0.2 : 0.05) + (c.energy < 0.5 ? 0.15 : 0);
            for (const MediaConcept& k : a.concepts) {
                if (k.level != "weak" && (k.id == "snowy-landscape" || k.id == "forest" || k.id == "mountains" || k.id == "savanna" || k.id == "stars" || k.id == "village" || k.id == "city-night")) {
                    r += 0.15;
                    break;
                }
            }
        } else if (role == "verse" || role == "breath") {
            r = 0.4 + 0.3 * (1.0 - a.complexity) + (a.brightness <= 0.55 ? 0.2 : 0) - (motion > 0.5 ? 0.2 : 0) + (role == "breath" ? 0.1 * (1.0 - c.energy) : 0);
        } else if (role == "chorus") {
            const bool strong = std::any_of(a.concepts.begin(), a.concepts.end(), [](const MediaConcept& k) { return k.level == "strong"; });
            r = 0.35 + 0.3 * a.contrast + 0.2 * a.colorfulness + (strong ? 0.1 : 0) + (moving ? 0.05 : 0);
        } else if (role == "instrumental") {
            r = 0.3 + 0.5 * motion + 0.2 * a.complexity;
        } else if (role == "bridge") {
            r = 0.5 + 0.4 * Novelty(c, sc);
        } else if (role == "climax") {
            r = 0.25 + 0.5 * c.energy + (m.kind == MediaKind::Video ? 0.2 : 0);
        } else if (role == "ending") {
            r = 0.45 + (m.kind == MediaKind::Image || a.video.loop >= 0.6 ? 0.2 : 0) + 0.3 * (1.0 - c.energy);
        }
        return std::clamp(r, 0.0, 1.0);
    }

    // Whether the file brings something the scene before did not show.
    double Novelty(const Cand& c, const Scene& sc) const {
        const auto at = std::find_if(_scenes.begin(), _scenes.end(), [&](const Scene& s) { return &s == &sc; });
        if (at == _scenes.begin() || at == _scenes.end()) {
            return 0.5;
        }
        const std::string before = std::prev(at)->info.id;
        for (const Planned& p : _planned) {
            if (p.shot.sceneId != before) {
                continue;
            }
            for (const Cand& o : _cands) {
                if (o.m->id == p.shot.mediaId) {
                    for (const std::string& cat : o.categories) {
                        if (Has(c.categories, cat)) {
                            return 0;
                        }
                    }
                }
            }
        }
        return 1;
    }

    double Quality(const Cand& c, const Scene& sc, int shotMS) const {
        const MediaItem& m = *c.m;
        const MediaAnalysis& a = m.analysis;
        double q = 1;
        if (!a.Done()) {
            q -= 0.1;
        }
        if (Has(a.issues, "very-bright")) {
            q -= 0.3;
        }
        if (Has(a.issues, "static-video")) {
            q -= sc.info.role == "instrumental" || sc.info.role == "climax" || sc.info.role == "chorus" ? 0.4 : 0.1;
        }
        if (m.kind == MediaKind::Video && m.durationMS > 0 && m.durationMS < shotMS && a.video.loop < 0.6) {
            q -= Has(a.issues, "too-short") ? 0.35 : 0.15;
        }
        if (Has(a.issues, "dark-spans")) {
            q -= 0.15;
        }
        if (Has(a.issues, "extreme-aspect")) {
            q -= 0.05;
        }
        if (HasPrefix(a.issues, "similar-to:")) {
            q -= 0.05;
        }
        return std::clamp(q, 0.0, 1.0);
    }

    // How two files relate: the concepts they show and the categories they share, and
    // what kind of relation it is. 1 is the same file.
    double Relation(const MediaItem& a, const MediaItem& b, std::string& kind) const {
        kind.clear();
        if (a.id == b.id) {
            kind = "same";
            return 1;
        }
        const auto weight = [](const MediaConcept& c) { return c.level == "strong" ? 1.0 : c.level == "probable" ? 0.6 : 0.25; };
        std::map<std::string, double> va, vb;
        for (const MediaConcept& c : a.analysis.concepts) {
            va[c.id] = std::max(va[c.id], weight(c));
        }
        for (const MediaConcept& c : b.analysis.concepts) {
            vb[c.id] = std::max(vb[c.id], weight(c));
        }
        double dot = 0, na = 0, nb = 0;
        for (const auto& [k, x] : va) {
            na += x * x;
            const auto it = vb.find(k);
            if (it != vb.end()) {
                dot += x * it->second;
            }
        }
        for (const auto& [k, y] : vb) {
            nb += y * y;
        }
        const double cosine = na > 0 && nb > 0 ? dot / std::sqrt(na * nb) : 0;
        const std::vector<std::string> ca = EffectiveCategories(a), cb = EffectiveCategories(b);
        int shared = 0;
        for (const std::string& c : ca) {
            shared += Has(cb, c) ? 1 : 0;
        }
        const size_t all = ca.size() + cb.size() - static_cast<size_t>(shared);
        const double jaccard = all > 0 ? static_cast<double>(shared) / all : 0;
        const double sim = R3(0.6 * cosine + 0.4 * jaccard);
        const auto sharedGroup = [&](const char* group) {
            for (const MediaConcept& x : a.analysis.concepts) {
                if (x.level == "weak") {
                    continue;
                }
                const VisualConceptDef* d = _concepts.Find(x.id);
                if (d == nullptr || d->group != group) {
                    continue;
                }
                for (const MediaConcept& y : b.analysis.concepts) {
                    if (y.id == x.id && y.level != "weak") {
                        return true;
                    }
                }
            }
            return false;
        };
        if (sharedGroup("scene")) {
            kind = "same-ambience";
        } else if (sharedGroup("subject")) {
            kind = "same-motif";
        } else if (sim >= 0.3) {
            kind = "related";
        } else if (sim >= 0.15 && ComplementaryHues(a, b)) {
            kind = "complementary";
        }
        return sim;
    }

    static bool HueOf(const MediaItem& m, double& h) {
        if (m.analysis.colors.empty()) {
            return false;
        }
        const long x = std::strtol(m.analysis.colors.front().hex.c_str() + 1, nullptr, 16);
        const double r = (x >> 16) & 255, g = (x >> 8) & 255, bl = x & 255;
        const double mx = std::max({ r, g, bl }), mn = std::min({ r, g, bl }), d = mx - mn;
        if (mx <= 0 || d / mx < 0.2) {
            return false;
        }
        h = mx == r ? 60.0 * std::fmod((g - bl) / d, 6.0) : mx == g ? 60.0 * ((bl - r) / d + 2.0) : 60.0 * ((r - g) / d + 4.0);
        h = h < 0 ? h + 360 : h;
        return true;
    }

    static bool ComplementaryHues(const MediaItem& a, const MediaItem& b) {
        double ha = 0, hb = 0;
        if (!HueOf(a, ha) || !HueOf(b, hb)) {
            return false;
        }
        const double d = std::fabs(ha - hb) > 180 ? 360 - std::fabs(ha - hb) : std::fabs(ha - hb);
        return d > 150 && d < 210;
    }

    // The shot placed on this screen nearest before `ms`.
    const Planned* Before(const Lane& lane, int ms) const {
        const Planned* best = nullptr;
        for (const Planned& p : _planned) {
            if (p.shot.lane == lane.t->name && p.shot.endMS <= ms + 1 && (best == nullptr || p.shot.endMS > best->shot.endMS)) {
                best = &p;
            }
        }
        return best;
    }

    // The screen's opening shot, for an ending that answers it.
    const Planned* Opening(const Lane& lane) const {
        const Planned* best = nullptr;
        for (const Planned& p : _planned) {
            const Scene* sc = nullptr;
            for (const Scene& x : _scenes) {
                sc = x.info.id == p.shot.sceneId ? &x : sc;
            }
            if (p.shot.lane == lane.t->name && sc != nullptr && sc->info.role == "establish" && (best == nullptr || p.shot.startMS < best->shot.startMS)) {
                best = &p;
            }
        }
        return best;
    }

    const MediaItem* ItemById(const std::string& id) const {
        return _in.library != nullptr ? _in.library->Find(id) : nullptr;
    }

    // Where a scene sits among the others, to know its neighbours.
    int IndexOf(const std::string& sceneId) const {
        for (size_t i = 0; i < _scenes.size(); ++i) {
            if (_scenes[i].info.id == sceneId) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    bool UsedIn(const std::string& media, int sceneIndex) const {
        if (sceneIndex < 0 || sceneIndex >= static_cast<int>(_scenes.size())) {
            return false;
        }
        const auto it = _sceneUses.find(media);
        return it != _sceneUses.end() && it->second.contains(_scenes[static_cast<size_t>(sceneIndex)].info.id);
    }

    int FreeFragments(const MediaItem& m) const {
        if (m.kind != MediaKind::Video) {
            return 0;
        }
        const int all = static_cast<int>(m.analysis.video.scenes.size());
        const auto it = _fragmentUses.find(m.id);
        return all - (it != _fragmentUses.end() ? static_cast<int>(it->second.size()) : 0);
    }

    Eval Evaluate(const Cand& c, const Scene& sc, const Lane& lane, int shotMS, const std::set<std::string>& otherScreens, bool sameSceneAgain, int atMS = -1) const {
        Eval e;
        e.c = &c;
        const MediaItem& m = *c.m;
        if (otherScreens.contains(m.id)) {
            return e; // the screens of a scene never show the same file
        }
        if (_avoid.contains(sc.info.id + "|" + lane.t->name + "|" + m.id)) {
            return e; // a person did not want it there
        }
        // Uses a person said a file is not for.
        const std::vector<std::string>& notFor = m.human.notFor;
        if (!notFor.empty()) {
            const bool calm = sc.info.energy < 0.4 && sc.info.role != "climax";
            const bool intense = sc.info.energy >= 0.65 || sc.info.role == "climax";
            if ((Has(notFor, "tree") && lane.frame.tree) || (Has(notFor, "matrix") && !lane.frame.tree) || (Has(notFor, "calm") && calm) || (Has(notFor, "intense") && intense) ||
                (Has(notFor, "opening") && sc.info.role == "establish") || (Has(notFor, "ending") && sc.info.role == "ending")) {
                return e;
            }
        }
        const auto uses = _sceneUses.find(m.id);
        const bool usedBefore = uses != _sceneUses.end() && !uses->second.empty();
        const std::string lookLane = sc.info.lookKey + "|" + lane.t->name;
        const auto first = sc.info.lookKey.empty() ? _firstOnLook.end() : _firstOnLook.find(lookLane);
        const int at = IndexOf(sc.info.id);
        // The music's repeat brings its picture back, but not straight after itself:
        // that would only be one long shot of the same thing.
        const bool continuity = first != _firstOnLook.end() && first->second == m.id && !UsedIn(m.id, at - 1) && !UsedIn(m.id, at + 1);
        double repeat = 0;
        if (usedBefore && !continuity && !sameSceneAgain) {
            // Only a video with scenes left can come back, with another of its scenes.
            if (FreeFragments(m) <= 0) {
                return e;
            }
            repeat -= 0.15 * static_cast<double>(uses->second.size()) * (1.0 - 0.5 * Pref("repetition"));
        }
        const int idx = at;
        if (!continuity && !sameSceneAgain && (UsedIn(m.id, idx - 1) || UsedIn(m.id, idx + 1))) {
            repeat -= 0.3; // back to back would look like a slideshow stuck on one picture
        }
        // A clip much shorter than the shot would only loop to fill time.
        if (m.kind == MediaKind::Video && m.durationMS > 0 && m.durationMS * 3 < shotMS && m.analysis.video.loop < 0.6) {
            return e;
        }
        e.framing = PlanFraming(m, lane.frame, shotMS, sc.info.energy, sc.info.role, _seed ^ Hash(sc.info.id + lane.t->name + m.id));
        const double generic = m.analysis.Done() ? (lane.role == "mega-tree" ? m.analysis.treeFit : m.analysis.matrixFit) : 0.55;
        const double fit = 0.5 * generic + 0.5 * e.framing.score;
        double energy = 1.0 - std::fabs(c.energy - sc.info.energy);
        if ((sc.info.band == "high" || sc.info.band == "climax") && m.kind == MediaKind::Video) {
            energy += 0.1;
        } else if ((sc.info.band == "low" || sc.info.band == "rest") && m.kind == MediaKind::Image) {
            energy += 0.05;
        }
        energy = std::clamp(energy, 0.0, 1.0);
        const double role = RoleFit(c, sc, lane);
        const double quality = Quality(c, sc, shotMS);
        // The story on this screen: a file related to the shot before carries it on, one
        // nearly the same adds nothing, and the ending answers the opening.
        double story = 0;
        std::string relation;
        if (const Planned* before = Before(lane, atMS >= 0 ? atMS : sc.info.startMS); before != nullptr && before->shot.mediaId != m.id) {
            if (const MediaItem* b = ItemById(before->shot.mediaId); b != nullptr) {
                const double sim = Relation(*b, m, relation);
                story += relation == "same-ambience" || relation == "same-motif" || relation == "related" ? 0.06 : relation == "complementary" ? 0.03 : 0;
                story -= sim > 0.85 ? 0.05 : 0;
                if (sc.info.role == "bridge") {
                    story = -story / 2; // the bridge changes the air
                }
            }
        }
        if (sc.info.role == "ending") {
            if (const Planned* opening = Opening(lane); opening != nullptr && opening->shot.mediaId != m.id) {
                if (const MediaItem* o = ItemById(opening->shot.mediaId); o != nullptr) {
                    std::string k;
                    if (Relation(*o, m, k) >= 0.3 || !k.empty()) {
                        story += 0.08;
                        relation = "answers-the-opening";
                    }
                }
            }
        }
        // A motif the screen already showed twice is enough: the story moves on.
        if (!m.analysis.description.empty()) {
            const std::string& motif = m.analysis.description.front();
            int shown = 0;
            for (const Planned& p : _planned) {
                if (p.shot.lane != lane.t->name || p.shot.mediaId == m.id) {
                    continue;
                }
                const MediaItem* o = ItemById(p.shot.mediaId);
                shown += o != nullptr && !o->analysis.description.empty() && o->analysis.description.front() == motif ? 1 : 0;
            }
            if (shown >= 2) {
                story -= 0.04 * (shown - 1);
                relation = "enough-of-" + motif;
            }
        }
        std::string liked;
        const double taste = Preferred(m, sc, lane, e.framing, liked);
        double score = 0.30 * c.theme + 0.20 * fit + 0.15 * energy + 0.20 * role + 0.15 * quality + repeat + story + taste + (continuity ? 0.15 : 0) +
                       0.01 * Unit(_seed, sc.info.id + "/" + lane.t->name + "/" + m.id);
        e.score = R3(std::clamp(score, 0.0, 1.0));
        if (!liked.empty()) {
            e.why.push_back({ "preference", liked });
        }
        if (!relation.empty() && story > 0) {
            e.why.push_back({ "story", relation });
        } else if (relation.rfind("enough-of-", 0) == 0) {
            e.why.push_back({ "variety", relation });
        }
        e.why.push_back({ "theme-category", c.themeWhy == "no-theme" || c.themeWhy == "neutral" ? c.themeWhy : fmt::format("{} ({})", c.themeWhy, _out.themeId) });
        std::string seen;
        for (const std::string& id : m.analysis.description) {
            seen += (seen.empty() ? "" : ", ") + id;
        }
        if (!seen.empty()) {
            e.why.push_back({ "content", fmt::format("{} ({})", seen, m.analysis.mode) });
        }
        e.why.push_back({ "aspect", fmt::format("{:.2f} on {:.2f}: {}", m.aspect, lane.frame.tree && lane.frame.wraps && e.framing.layout == "wrap" ? lane.frame.wrapAspect : lane.frame.faceAspect, e.framing.strategy) });
        e.why.push_back({ "energy", fmt::format("{} {:.2f} for {:.2f} ({})", MediaKindId(m.kind), c.energy, sc.info.energy, sc.info.band) });
        e.why.push_back({ "scene-role", fmt::format("{} {:.2f}", sc.info.role, role) });
        if (quality < 1) {
            e.why.push_back({ "quality", fmt::format("{:.2f}", quality) });
        }
        if (continuity) {
            e.why.push_back({ "repeat-keeps-media", sc.info.lookKey });
        } else if (repeat < 0) {
            e.why.push_back({ "repeated", fmt::format("{:.2f}", repeat) });
        }
        return e;
    }

    std::vector<Eval> Ranked(const Scene& sc, const Lane& lane, int shotMS, const std::set<std::string>& otherScreens, const std::set<std::string>& exclude, bool sameScene, int atMS = -1) {
        std::vector<Eval> out;
        for (Cand& c : _cands) {
            if (exclude.contains(c.m->id)) {
                continue;
            }
            Eval e = Evaluate(c, sc, lane, shotMS, otherScreens, sameScene, atMS);
            c.best = std::max(c.best, e.score);
            if (e.score >= _minScore) {
                out.push_back(std::move(e));
            }
        }
        std::sort(out.begin(), out.end(), [](const Eval& a, const Eval& b) { return a.score != b.score ? a.score > b.score : a.c->m->id < b.c->m->id; });
        return out;
    }

    // How well two files sit together in one scene on the two screens.
    double Complement(const Eval& a, const Eval& b, const Scene& sc) const {
        double v = 0;
        bool shared = false;
        for (const std::string& cat : a.c->categories) {
            shared = shared || Has(b.c->categories, cat);
        }
        v += shared ? 0.06 : (a.c->theme > 0.35 && b.c->theme > 0.35 ? 0.02 : 0);
        std::string kind;
        (void)Relation(*a.c->m, *b.c->m, kind);
        v += kind == "same-ambience" || kind == "same-motif" ? 0.05 : kind == "complementary" ? 0.03 : 0;
        if (a.c->m->analysis.orientation != b.c->m->analysis.orientation) {
            v += 0.02;
        }
        const auto hue = [](const MediaItem& m, double& h) {
            if (m.analysis.colors.empty()) {
                return false;
            }
            const long x = std::strtol(m.analysis.colors.front().hex.c_str() + 1, nullptr, 16);
            const double r = (x >> 16) & 255, g = (x >> 8) & 255, bl = x & 255;
            const double mx = std::max({ r, g, bl }), mn = std::min({ r, g, bl }), d = mx - mn;
            if (mx <= 0 || d / mx < 0.2) {
                return false;
            }
            h = mx == r ? 60.0 * std::fmod((g - bl) / d, 6.0) : mx == g ? 60.0 * ((bl - r) / d + 2.0) : 60.0 * ((r - g) / d + 4.0);
            h = h < 0 ? h + 360 : h;
            return true;
        };
        double ha = 0, hb = 0;
        if (hue(*a.c->m, ha) && hue(*b.c->m, hb)) {
            const double d = std::fabs(ha - hb) > 180 ? 360 - std::fabs(ha - hb) : std::fabs(ha - hb);
            v += d < 45 || (d > 150 && d < 210) ? 0.02 : 0;
        }
        if (sc.info.energy < 0.4 && a.c->m->kind == MediaKind::Video && b.c->m->kind == MediaKind::Video && a.c->m->analysis.video.motion > 0.3 && b.c->m->analysis.video.motion > 0.3) {
            v -= 0.05; // two busy videos at once in a quiet scene
        }
        return v;
    }

    // Which part of a video suits the shot: its energy, light and what was used before.
    std::pair<int, int> Fragment(const MediaItem& m, int shotMS, double energy) const {
        const std::vector<MediaScene>& scenes = m.analysis.video.scenes;
        if (m.kind != MediaKind::Video || scenes.empty()) {
            return { -1, 0 };
        }
        const auto used = _fragmentUses.find(m.id);
        int best = 0;
        double bestScore = -1e9;
        for (size_t i = 0; i < scenes.size(); ++i) {
            const MediaScene& s = scenes[i];
            double v = 1.0 - std::fabs(s.energy - energy);
            if (used != _fragmentUses.end() && used->second.contains(static_cast<int>(i))) {
                v -= 0.5;
            }
            for (const auto& [f, t] : m.analysis.video.darkSpans) {
                if (f < s.endMS && t > s.startMS) {
                    v -= 0.4;
                }
            }
            v += s.endMS - s.startMS >= shotMS ? 0.1 : 0;
            if (v > bestScore) {
                bestScore = v;
                best = static_cast<int>(i);
            }
        }
        int start = scenes[static_cast<size_t>(best)].startMS;
        if (m.durationMS - start < shotMS && m.durationMS >= shotMS) {
            start = m.durationMS - shotMS;
        }
        return { best, std::max(0, start) };
    }

    int Snap(int t, int lo, int hi) const {
        if (_in.analysis == nullptr) {
            return t;
        }
        int best = -1;
        for (MusicalEventType type : { MusicalEventType::Downbeat, MusicalEventType::Beat }) {
            for (const MusicalEvent& ev : _in.analysis->events) {
                if (ev.type != type || ev.timeMS < lo || ev.timeMS > hi || std::abs(ev.timeMS - t) > 1500) {
                    continue;
                }
                if (best < 0 || std::abs(ev.timeMS - t) < std::abs(best - t)) {
                    best = ev.timeMS;
                }
            }
            if (best >= 0) {
                return best;
            }
        }
        return t;
    }

    std::string Kept(const MediaItem& m) const {
        return m.relative.empty() ? m.path : m.relative;
    }

    // The shot with its effect settings, every one checked against the effect's metadata.
    bool Make(const Scene& sc, const Lane& lane, const MediaItem& m, int start, int end, const MediaFraming& framing, std::pair<int, int> fragment, DecisionState state, double score,
              std::vector<Reason> why, Planned& out) {
        StoryboardShot& s = out.shot;
        s.sceneId = sc.info.id;
        s.lane = lane.t->name;
        s.startMS = start;
        s.endMS = end;
        s.mediaId = m.id;
        s.name = m.name;
        s.kind = m.kind;
        s.effect = m.kind == MediaKind::Video ? "Video" : "Pictures";
        s.source = Kept(m);
        s.framing = framing.strategy;
        s.score = R3(score);
        s.level = MediaShotLevel(score);
        s.state = state;
        s.reasons = std::move(why);
        s.constraints = framing.constraints;
        s.path = s.source;
        bool clip = false;
        if (framing.motionClip) {
            std::string relative = "ShowComposer/media-derived/" + framing.ClipName(m, end - start), error;
            if (!_in.options.writeDerived || _in.showDir.empty() || MakeMotionClip(m, framing, end - start, _in.showDir, relative, error)) {
                clip = true;
                s.path = relative;
                s.effect = "Video";
                s.constraints.push_back("motion-clip");
                s.reasons.push_back({ "motion-clip", relative });
            } else {
                _out.warnings.push_back(fmt::format("media-clip-failed:{}:{}:{}", sc.info.id, m.name, error));
            }
        }
        if (clip) {
        } else if (framing.derived) {
            std::string relative, error;
            relative = "ShowComposer/media-derived/" + framing.DerivedName(m);
            if (_in.options.writeDerived && !_in.showDir.empty() && !MakeDerivedPicture(m, framing, _in.showDir, relative, error)) {
                _out.warnings.push_back(fmt::format("media-derived-failed:{}:{}:{}", sc.info.id, m.name, error));
                return false;
            }
            s.path = relative;
            s.reasons.push_back({ "derived", relative });
        } else if (m.relative.empty()) {
            s.constraints.push_back("outside-show");
        }
        Writer w;
        w.md = _in.metadata != nullptr ? (*_in.metadata)(s.effect) : nullptr;
        if (w.md == nullptr) {
            _out.warnings.push_back("media-no-metadata:" + s.effect);
            return false;
        }
        w.Raw(s.effect == "Video" ? "E_FILEPICKERCTRL_Video_Filename" : "E_TEXTCTRL_Pictures_Filename", s.path);
        for (const auto& [id, value] : clip ? framing.clipSettings : framing.settings) {
            w.Set(id, value);
        }
        // A video on a wrapping tree: one effect per piece of its columns.
        out.pieces.clear();
        const bool whole = framing.pieces.size() == 1 && framing.pieces.front().x0 <= 1e-6 && framing.pieces.front().x1 >= 1 - 1e-6;
        if (!clip && s.kind == MediaKind::Video && !framing.pieces.empty() && lane.frame.wraps && !whole) {
            for (const FramePiece& fp : framing.pieces) {
                Writer pw;
                pw.md = w.md;
                pw.Set("Video_CropLeft", fmt::format("{}", static_cast<int>(std::lround(fp.cropX0 * 100))));
                pw.Set("Video_CropRight", fmt::format("{}", static_cast<int>(std::lround(fp.cropX1 * 100))));
                for (const std::string& problem : pw.problems) {
                    w.problems.push_back(problem);
                }
                out.pieces.push_back({ fmt::format("{:.2f}x0.00x{:.2f}x100.00", fp.x0 * 100, fp.x1 * 100), pw.settings });
            }
            s.pieces = static_cast<int>(out.pieces.size());
        }
        if (whole && !clip && s.kind == MediaKind::Video) {
            // All the way round in one piece: the frame's crop is the shot's own.
            w.Set("Video_CropLeft", fmt::format("{}", static_cast<int>(std::lround(framing.pieces.front().cropX0 * 100))));
            w.Set("Video_CropRight", fmt::format("{}", static_cast<int>(std::lround(framing.pieces.front().cropX1 * 100))));
        }
        if (s.kind == MediaKind::Video && !clip) {
            s.fragmentScene = fragment.first;
            s.fragmentStartMS = std::max(0, fragment.second);
            if (m.durationMS > 0 && s.fragmentStartMS >= m.durationMS - 100) {
                s.fragmentStartMS = 0;
            }
            w.SetWithin("Video_Starttime", s.fragmentStartMS / 1000.0, 0, std::max(0.0, (m.durationMS - 100) / 1000.0));
            const bool loops = m.durationMS > 0 && m.durationMS - s.fragmentStartMS < end - start;
            w.Set("Video_DurationTreatment", loops ? "Loop" : "Normal");
            if (loops) {
                s.constraints.push_back("loops");
            }
            if (s.fragmentStartMS > 0 || m.analysis.video.scenes.size() > 1) {
                s.reasons.push_back({ "fragment", fmt::format("{:.1f} s{}", s.fragmentStartMS / 1000.0, fragment.first >= 0 ? fmt::format(", its scene {}", fragment.first + 1) : std::string()) });
            }
        }
        if (!w.problems.empty()) {
            for (const std::string& p : w.problems) {
                _out.warnings.push_back(fmt::format("media-setting-invalid:{}:{}", sc.info.id, p));
            }
            return false;
        }
        out.settings = std::move(w.settings);
        return true;
    }

    void Record(const Planned& p, const Scene& sc) {
        _sceneUses[p.shot.mediaId].insert(sc.info.id);
        if (p.shot.fragmentScene >= 0) {
            _fragmentUses[p.shot.mediaId].insert(p.shot.fragmentScene);
        }
        if (!sc.info.lookKey.empty()) {
            _firstOnLook.emplace(sc.info.lookKey + "|" + p.shot.lane, p.shot.mediaId);
        }
        for (Cand& c : _cands) {
            if (c.m->id == p.shot.mediaId) {
                c.used = true;
            }
        }
    }

    const PlanSegment* SegmentFor(const MediaCorrection& c) const {
        for (const PlanSegment& s : _plan.segments) {
            if (s.id == c.segmentId && (c.startMS < 0 || std::abs(s.startMS - c.startMS) < 2000)) {
                return &s;
            }
        }
        // A decision names its part of the song: found again when the times moved.
        if (const size_t a = c.decisionId.find('/'); a != std::string::npos) {
            const size_t b = c.decisionId.find('/', a + 1);
            const std::string key = c.decisionId.substr(a + 1, b == std::string::npos ? std::string::npos : b - a - 1);
            for (const PlanSegment& s : _plan.segments) {
                if (SegmentKey(s) == key) {
                    return &s;
                }
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

    void Manual() {
        for (const MediaCorrection& c : _in.corrections) {
            if (c.action == MediaCorrection::Action::Avoid) {
                if (const PlanSegment* s = SegmentFor(c); s != nullptr) {
                    _avoid.insert(s->id + "|" + c.target + "|" + c.mediaId);
                    _avoidKeys.insert(s->id + "|" + c.target);
                }
                continue;
            }
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
            if (c.action == MediaCorrection::Action::Remove || c.action == MediaCorrection::Action::Avoid) {
                continue;
            }
            const PlanSegment* s = SegmentFor(c);
            if (s == nullptr) {
                _out.warnings.push_back("media-correction-segment-missing:" + c.segmentId);
                continue;
            }
            Scene* sc = SceneById(s->id);
            if (sc == nullptr) {
                // A person may put a picture where the planner would not look.
                _scenes.push_back(MakeScene(*s));
                std::sort(_scenes.begin(), _scenes.end(), [](const Scene& a, const Scene& b) { return a.info.startMS < b.info.startMS; });
                sc = SceneById(s->id);
            }
            const std::string targetName = c.target.empty() && !_lanes.empty() ? _lanes.front().t->name : c.target;
            const TargetInfo* t = _caps.Find(targetName);
            if (t == nullptr || t->Excluded() || t->Can(Capability::Image).support == Support::No || t->Can(Capability::Image).support == Support::Limited) {
                _out.warnings.push_back(fmt::format("media-correction-target-unusable:{}:{}", s->id, c.target));
                continue;
            }
            Lane* lane = LaneOf(t->name);
            if (lane == nullptr) {
                _lanes.push_back(MakeLane(*t, 0, false));
                _lanes.back().screen = "media";
                lane = &_lanes.back();
            }
            const MediaItem* m = ItemFor(c);
            if (m == nullptr || !m->Usable()) {
                // The person's choice stays in the sidecar; the show just has no effect
                // pointing at a file that is not there.
                _out.warnings.push_back(fmt::format("media-missing:{}:{}", s->id, c.path.empty() ? c.mediaId : c.path));
                _noFit[s->id + "|" + t->name] = "media-missing";
                continue;
            }
            // Found by its decision in a segment that moved: the shot moves with it.
            const int shift = c.startMS >= 0 && c.shotStartMS >= 0 ? s->startMS - c.startMS : 0;
            const int start = c.shotStartMS >= 0 ? std::clamp(c.shotStartMS + shift, s->startMS, s->endMS) : s->startMS;
            const int end = c.shotEndMS >= 0 ? std::clamp(c.shotEndMS + shift, start, s->endMS) : s->endMS;
            if (end - start < 200) {
                continue;
            }
            const MediaFraming framing = PlanFraming(*m, lane->frame, end - start, sc->info.energy, sc->info.role, _seed ^ Hash(sc->info.id + t->name + m->id));
            std::pair<int, int> fragment = Fragment(*m, end - start, sc->info.energy);
            if (c.fragmentStartMS >= 0) {
                fragment = { fragment.first, c.fragmentStartMS };
            }
            const bool locked = c.action == MediaCorrection::Action::Lock;
            std::string why;
            const std::vector<std::string> cats = EffectiveCategories(*m);
            ThemeFit(cats, why);
            std::vector<Reason> reasons = { { locked ? "locked-by-person" : "chosen-by-person", m->name },
                                            { "theme-category", why },
                                            { "aspect", fmt::format("{:.2f} on {:.2f}: {}", m->aspect, lane->frame.faceAspect, framing.strategy) } };
            Planned p;
            if (!Make(*sc, *lane, *m, start, end, framing, fragment, locked ? DecisionState::Locked : DecisionState::ManualEdit, 1.0, std::move(reasons), p)) {
                continue;
            }
            const std::string busy = Busy(*s, *t);
            if (!busy.empty()) {
                p.shot.constraints.push_back(busy);
                _out.warnings.push_back(fmt::format("media-over-plan:{}:{}:{}", s->id, t->name, busy));
            }
            lane->usedMS += end - start;
            _manual.insert(sc->info.id + "|" + t->name);
            Record(p, *sc);
            _planned.push_back(std::move(p));
        }
    }

    // How long a shot of this file should last in this scene: long enough to be
    // understood, shorter when it moves a lot, longer when it is very good.
    int ShotTarget(const Scene& sc, const Cand* c, double score) const {
        const std::string& r = sc.info.role;
        double ms = r == "climax" ? 6000 : r == "chorus" || r == "instrumental" ? 8000 : r == "establish" ? 14000 : r == "ending" || r == "breath" ? 14000 : r == "bridge" ? 10000 : 12000;
        if (sc.info.band == "high") {
            ms = std::min(ms, 8000.0);
        }
        if (c != nullptr) {
            const MediaAnalysis& a = c->m->analysis;
            if (score >= 0.8) {
                ms *= 1.25;
            }
            if (c->m->kind != MediaKind::Image && a.video.motion > 0.5) {
                ms *= 0.8;
            }
            if (a.complexity > 0.35) {
                ms *= 1.15;
            }
        }
        return static_cast<int>(std::max(3500.0, ms));
    }

    // Places a screen's shots in the scene's windows: the chosen file first, then, in a
    // long window, other parts of the same video or other files, cut on the beats; a
    // shot that still works is not cut just because a bar went by.
    void PlaceShots(const Scene& sc, Lane& lane, const Eval& first, std::set<std::string>& inScene, const std::vector<std::pair<int, int>>& windows) {
        const int minShot = _in.options.minShotMS;
        const bool overlay = Words(*sc.seg, *lane.t);
        bool firstUsed = false;
        const MediaItem* previous = nullptr;
        for (const auto& [start, end] : windows) {
            const int len = end - start;
            const int target = ShotTarget(sc, firstUsed ? nullptr : first.c, first.score);
            int n = len > target * 13 / 10 ? static_cast<int>(std::lround(static_cast<double>(len) / target)) : 1;
            n = std::max(1, std::min(n, len / std::max(1, minShot)));
            std::vector<int> cuts = { start };
            for (int k = 1; k < n; ++k) {
                const int want = start + static_cast<int>(static_cast<long long>(len) * k / n);
                const int at = Snap(want, cuts.back() + minShot, end - minShot);
                if (at - cuts.back() >= minShot && end - at >= minShot) {
                    cuts.push_back(at);
                }
            }
            cuts.push_back(end);
            for (size_t k = 0; k + 1 < cuts.size(); ++k) {
                const int from = cuts[k];
                const int to = cuts[k + 1];
                const int shotMS = to - from;
                Eval chosen;
                if (!firstUsed) {
                    chosen = first;
                    // Framed for the shot's real length: a camera movement depends on it.
                    chosen.framing = PlanFraming(*first.c->m, lane.frame, shotMS, sc.info.energy, sc.info.role, _seed ^ Hash(sc.info.id + lane.t->name + first.c->m->id));
                    firstUsed = true;
                } else {
                    if (previous != nullptr && previous->kind == MediaKind::Video && FreeFragments(*previous) > 0) {
                        const auto c = std::find_if(_cands.begin(), _cands.end(), [&](const Cand& x) { return x.m == previous; });
                        if (c != _cands.end()) {
                            chosen = Evaluate(*c, sc, lane, shotMS, {}, true, from);
                        }
                    }
                    if (chosen.c == nullptr || chosen.score < _minScore) {
                        std::vector<Eval> ranked = Ranked(sc, lane, shotMS, inScene, {}, false, from);
                        chosen = ranked.empty() ? Eval() : ranked.front();
                    }
                    if (chosen.c == nullptr || chosen.score < _minScore) {
                        if (!_planned.empty() && _planned.back().shot.lane == lane.t->name && _planned.back().shot.sceneId == sc.info.id && _planned.back().shot.endMS == from) {
                            _planned.back().shot.endMS = to;
                            lane.usedMS += shotMS;
                        }
                        continue;
                    }
                }
                const MediaItem& m = *chosen.c->m;
                const std::pair<int, int> fragment = Fragment(m, shotMS, sc.info.energy);
                Planned p;
                if (!Make(sc, lane, m, from, to, chosen.framing, fragment, DecisionState::Automatic, chosen.score, chosen.why, p)) {
                    continue;
                }
                p.shot.overlay = overlay;
                p.shot.reasons.push_back({ overlay ? "between-phrases" : "no-lyrics-text", lane.t->name });
                p.shot.reasons.push_back({ "screen", fmt::format("{} ({})", lane.screen, lane.t->name) });
                p.shot.reasons.push_back({ "share", fmt::format("{:.0f}% of the figure's free time used of {:.0f}%", (lane.usedMS + shotMS) * 100.0 / std::max(1.0, lane.freeMS), GoalOf(lane) * 100) });
                if (sc.seg->vocalCoverage < 0.2) {
                    p.shot.reasons.push_back({ "instrumental", fmt::format("{:.2f}", sc.seg->vocalCoverage) });
                }
                lane.usedMS += shotMS;
                Record(p, sc);
                inScene.insert(m.id);
                previous = &m;
                _planned.push_back(std::move(p));
            }
        }
    }

    void Automatic() {
        std::vector<Scene*> order;
        for (Scene& sc : _scenes) {
            order.push_back(&sc);
        }
        // The scenes that matter most choose first, so the best files go where they show.
        // A section the music repeats chooses after the new ones: variety first, and
        // what is left can still bring back the first one's picture.
        const auto rank = [](const Scene* s) { return s->info.repeatOf.empty() ? RoleRank(s->info.role) * 2 : std::max(RoleRank(s->info.role), RoleRank("bridge")) * 2 + 1; };
        std::stable_sort(order.begin(), order.end(), [&](const Scene* a, const Scene* b) {
            const int ra = rank(a), rb = rank(b);
            if (ra != rb) {
                return ra < rb;
            }
            if (a->seg->priority != b->seg->priority) {
                return a->seg->priority > b->seg->priority;
            }
            return a->info.startMS < b->info.startMS;
        });
        for (Scene* scp : order) {
            const Scene& sc = *scp;
            // Files a person put on a screen of this scene.
            std::set<std::string> inScene;
            for (const Planned& p : _planned) {
                if (p.shot.sceneId == sc.info.id) {
                    inScene.insert(p.shot.mediaId);
                }
            }
            // Every screen at once, the main one first: each picks the file that suits
            // the scene and sits best with what the screens before it chose.
            std::vector<Eval> chosen;
            std::string firstName;
            for (Lane& l : _lanes) {
                if (!l.main) {
                    continue;
                }
                const std::string key = sc.info.id + "|" + l.t->name;
                // A screen with a person's shots here is theirs, unless a review rejected a
                // shot of it: then only the stretch it leaves is chosen again.
                const bool refill = _manual.contains(key) && _avoidKeys.contains(key);
                if (_manual.contains(key) && !refill) {
                    continue;
                }
                if (_removed.contains(sc.info.id) || _removed.contains(key)) {
                    _noFit[key] = "removed";
                    continue;
                }
                std::string why;
                std::vector<std::pair<int, int>> windows = Windows(sc, l, why);
                if (refill) {
                    std::vector<std::pair<int, int>> taken;
                    for (const Planned& p : _planned) {
                        if (p.shot.sceneId == sc.info.id && p.shot.lane == l.t->name) {
                            taken.emplace_back(p.shot.startMS, p.shot.endMS);
                        }
                    }
                    std::vector<std::pair<int, int>> free;
                    for (const auto& [a, b] : windows) {
                        int at = a;
                        std::vector<std::pair<int, int>> in = taken;
                        std::sort(in.begin(), in.end());
                        for (const auto& [ta, tb] : in) {
                            if (tb <= at || ta >= b) {
                                continue;
                            }
                            if (ta - at >= _in.options.minShotMS) {
                                free.emplace_back(at, ta);
                            }
                            at = std::max(at, tb);
                        }
                        if (b - at >= _in.options.minShotMS) {
                            free.emplace_back(at, b);
                        }
                    }
                    windows = free;
                    if (windows.empty()) {
                        continue;
                    }
                }
                if (windows.empty()) {
                    _noFit[key] = why;
                    continue;
                }
                int len = 0;
                for (const auto& [a, b] : windows) {
                    len += b - a;
                }
                const bool bookend = sc.info.role == "establish" || sc.info.role == "ending";
                // The pauses between phrases are what a screen with words is there for: the
                // louder scenes, which choose first, cannot spend them.
                const bool pauses = l.gapsOnly && Words(*sc.seg, *l.t);
                if (!pauses && !refill && l.usedMS + len > l.budgetMS + (bookend ? l.reserveMS : 0)) {
                    _noFit[key] = "goal-reached";
                    continue;
                }
                const int firstLen = windows.front().second - windows.front().first;
                std::vector<Eval> ranked = Ranked(sc, l, std::min(firstLen, ShotTarget(sc, nullptr, 0)), inScene, {}, false, windows.front().first);
                if (ranked.size() > 6) {
                    ranked.resize(6);
                }
                const Eval* best = nullptr;
                double bestValue = -1;
                for (const Eval& e : ranked) {
                    double v = e.score;
                    for (const Eval& o : chosen) {
                        v += Complement(e, o, sc);
                    }
                    if (v > bestValue + 1e-9) {
                        bestValue = v;
                        best = &e;
                    }
                }
                if (best == nullptr) {
                    _noFit[key] = "no-media-fits";
                    continue;
                }
                const Eval keep = *best;
                const size_t before = _planned.size();
                PlaceShots(sc, l, keep, inScene, windows);
                if (!firstName.empty()) {
                    for (size_t i = before; i < _planned.size(); ++i) {
                        _planned[i].shot.reasons.push_back({ "pair", fmt::format("{} with {}", firstName, keep.c->m->name) });
                    }
                } else {
                    firstName = keep.c->m->name;
                }
                chosen.push_back(keep);
            }
        }
    }

    bool OnBeat(int ms) const {
        if (_in.analysis == nullptr) {
            return false;
        }
        return std::any_of(_in.analysis->events.begin(), _in.analysis->events.end(),
                           [&](const MusicalEvent& e) { return (e.type == MusicalEventType::Downbeat || e.type == MusicalEventType::Beat) && std::abs(e.timeMS - ms) <= 30; });
    }

    // How each shot comes in, from the music and the shots: a cut on the beat when the
    // music is strong, a cross-fade between related pictures or in the calm, a zoom into
    // the climax, a wipe now and then at a change of scene, a plain fade otherwise, and a
    // long rest before the lights take over. A complex one is never used twice in a row.
    void Transitions() {
        std::stable_sort(_planned.begin(), _planned.end(), [](const Planned& a, const Planned& b) { return a.shot.lane != b.shot.lane ? a.shot.lane < b.shot.lane : a.shot.startMS < b.shot.startMS; });
        std::map<std::string, int> sinceComplex;
        for (size_t i = 0; i < _planned.size(); ++i) {
            StoryboardShot& s = _planned[i].shot;
            const Scene* sc = SceneById(s.sceneId);
            const double energy = sc != nullptr ? sc->info.energy : 0.5;
            const int len = s.endMS - s.startMS;
            StoryboardShot* prev = i > 0 && _planned[i - 1].shot.lane == s.lane ? &_planned[i - 1].shot : nullptr;
            const StoryboardShot* next = i + 1 < _planned.size() && _planned[i + 1].shot.lane == s.lane ? &_planned[i + 1].shot : nullptr;
            int& since = sinceComplex[s.lane];
            ++since;
            const bool touch = prev != nullptr && prev->endMS == s.startMS;
            if (!touch) {
                s.transition = "fade";
                s.transitionIn = "Fade";
                s.fadeInMS = energy >= 0.6 ? 200 : 700;
            } else {
                const Scene* psc = SceneById(prev->sceneId);
                const bool sceneChange = prev->sceneId != s.sceneId;
                std::string relation;
                const MediaItem* a = ItemById(prev->mediaId);
                const MediaItem* b = ItemById(s.mediaId);
                const double sim = a != nullptr && b != nullptr ? Relation(*a, *b, relation) : 0;
                const bool related = sim >= 0.3 || relation == "same-ambience" || relation == "same-motif";
                const int plen = prev->endMS - prev->startMS;
                if (energy >= 0.65 && OnBeat(s.startMS)) {
                    s.transition = "cut";
                } else if (sceneChange && sc != nullptr && sc->info.role == "climax" && since >= 3) {
                    s.transition = "zoom";
                } else if (!s.overlay && !prev->overlay && energy < 0.6 && (related || energy < 0.4 || !sceneChange)) {
                    s.transition = "crossfade";
                } else if (sceneChange && since >= 4 && energy >= 0.4 && psc != nullptr && psc->info.role != sc->info.role) {
                    s.transition = "wipe";
                } else {
                    s.transition = "fade";
                }
                // A way in the maker keeps turning down is not used.
                if (Pref("transition." + s.transition) <= -0.3) {
                    s.transition = s.transition != "fade" && Pref("transition.fade") > -0.3 ? "fade" : "cut";
                    s.reasons.push_back({ "preference", "transition." + s.transition });
                }
                if (s.transition == "cut") {
                    s.transitionIn.clear();
                    s.fadeInMS = 0;
                    prev->transitionOut.clear();
                    prev->fadeOutMS = 0;
                } else if (s.transition == "crossfade") {
                    // Both on screen for a moment, on two layers: the old fades as the new comes.
                    const int overlap = std::clamp(std::min(plen, len) / 5, 400, 1500);
                    s.layer = prev->layer == 0 ? 3 : 0;
                    prev->overlapMS = overlap;
                    prev->transitionOut = "Fade";
                    prev->fadeOutMS = overlap;
                    s.transitionIn = "Fade";
                    s.fadeInMS = overlap;
                } else if (s.transition == "zoom" || s.transition == "wipe") {
                    s.transitionIn = s.transition == "zoom" ? "Zoom" : "Wipe";
                    s.fadeInMS = s.transition == "zoom" ? 300 : 400;
                    prev->transitionOut.clear();
                    prev->fadeOutMS = 0;
                    since = 0;
                } else {
                    s.transitionIn = "Fade";
                    s.fadeInMS = std::clamp(len / 8, 300, 1000);
                    prev->transitionOut = "Fade";
                    prev->fadeOutMS = std::clamp(plen / 8, 300, 1000);
                }
            }
            if (next == nullptr || next->startMS > s.endMS) {
                // The lights take over: a long rest when the music is calm.
                const bool rest = energy < 0.4 && (next == nullptr || next->startMS - s.endMS > 8000);
                s.transitionOut = "Fade";
                s.fadeOutMS = rest ? 1500 : energy >= 0.6 ? 300 : 800;
                if (rest) {
                    s.constraints.push_back("rests");
                }
            }
            const int cap = s.transition == "crossfade" ? len / 3 : len / 4;
            s.fadeInMS = std::min(s.fadeInMS, cap);
            if (s.overlapMS <= 0) {
                s.fadeOutMS = std::min(s.fadeOutMS, (s.endMS - s.startMS) / 3);
            }
            if (s.fadeInMS <= 0) {
                s.transitionIn.clear();
            }
            if (s.fadeOutMS <= 0) {
                s.transitionOut.clear();
            }
        }
    }

    void Gaps() {
        for (const Scene& sc : _scenes) {
            for (const Lane& l : _lanes) {
                if (!l.main) {
                    continue;
                }
                const bool covered = std::any_of(_planned.begin(), _planned.end(), [&](const Planned& p) { return p.shot.sceneId == sc.info.id && p.shot.lane == l.t->name; });
                if (l.gapsOnly && !covered && _noFit.find(sc.info.id + "|" + l.t->name) == _noFit.end()) {
                    continue; // the screen with words just shows them
                }
                if (covered) {
                    continue;
                }
                const auto why = _noFit.find(sc.info.id + "|" + l.t->name);
                _sb.gaps.push_back({ l.t->name, sc.info.id, sc.info.startMS, sc.info.endMS, why != _noFit.end() ? why->second : std::string("not-planned") });
            }
        }
    }

    void Finish() {
        for (Lane& l : _lanes) {
            StoryboardLane out;
            out.target = l.t->name;
            out.role = l.role;
            out.bufferWi = l.frame.bufferWi;
            out.bufferHt = l.frame.bufferHt;
            out.faceAspect = R3(l.frame.faceAspect);
            out.degrees = l.t->degrees;
            out.freeShare = R3(l.freeMS / std::max(1, _plan.durationMS));
            out.goal = l.main ? GoalOf(l) : 0;
            out.reason = l.main ? l.reason : std::string("a person's choice");
            out.screen = l.screen;
            out.gapsOnly = l.gapsOnly;
            out.assumptions = l.frame.assumptions;
            _sb.lanes.push_back(std::move(out));
        }
        for (const Scene& sc : _scenes) {
            _sb.scenes.push_back(sc.info);
        }
        int n = 0;
        std::map<std::string, int> perKey;
        for (Planned& p : _planned) {
            p.shot.id = fmt::format("{}/{}/{}", p.shot.sceneId, p.shot.lane, perKey[p.shot.sceneId + "|" + p.shot.lane]++);
            ++n;
            MediaAssignment a;
            a.segmentId = p.shot.sceneId;
            a.target = p.shot.lane;
            a.startMS = p.shot.startMS;
            a.endMS = p.shot.endMS + p.shot.overlapMS;
            a.overlapMS = p.shot.overlapMS;
            a.mediaId = p.shot.mediaId;
            a.path = p.shot.path;
            a.name = p.shot.name;
            a.kind = p.shot.kind;
            a.effect = p.shot.effect;
            a.settings = p.settings;
            a.state = p.shot.state;
            a.score = p.shot.score;
            a.reasons = p.shot.reasons;
            a.constraints = p.shot.constraints;
            a.shotId = p.shot.id;
            a.source = p.shot.source;
            a.framing = p.shot.framing;
            a.fragmentStartMS = p.shot.fragmentStartMS;
            a.layer = p.shot.layer;
            a.overlay = p.shot.overlay;
            a.transition = p.shot.transition;
            a.pieces = p.pieces;
            a.transitionIn = p.shot.transitionIn;
            a.fadeInMS = p.shot.fadeInMS;
            a.transitionOut = p.shot.transitionOut;
            a.fadeOutMS = p.shot.fadeOutMS;
            a.level = p.shot.level;
            _out.assignments.push_back(std::move(a));
            _sb.shots.push_back(p.shot);
        }
        (void)n;
        {
            std::map<std::string, std::string> keys;
            for (const StoryboardScene& sc : _sb.scenes) {
                keys[sc.id] = sc.key;
            }
            std::vector<size_t> order(_sb.shots.size());
            for (size_t i = 0; i < order.size(); ++i) {
                order[i] = i;
            }
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return _sb.shots[a].startMS < _sb.shots[b].startMS; });
            std::map<std::string, int> nth;
            std::map<std::string, std::string> byShot;
            for (const size_t i : order) {
                StoryboardShot& sh = _sb.shots[i];
                sh.decisionId = fmt::format("{}/{}/{}", sh.lane, keys[sh.sceneId], ++nth[sh.sceneId + "|" + sh.lane]);
                byShot[sh.id] = sh.decisionId;
            }
            for (MediaAssignment& a : _out.assignments) {
                a.decisionId = byShot[a.shotId];
            }
        }
        std::sort(_out.assignments.begin(), _out.assignments.end(), [](const MediaAssignment& a, const MediaAssignment& b) { return a.startMS != b.startMS ? a.startMS < b.startMS : a.target < b.target; });
        for (const Cand& c : _cands) {
            if (!c.used) {
                _sb.discarded.push_back({ c.m->id, c.m->name, c.best < _minScore ? "low-score" : "not-needed", c.best });
            }
        }
        std::sort(_sb.discarded.begin(), _sb.discarded.end(), [](const StoryboardDiscard& a, const StoryboardDiscard& b) { return a.name != b.name ? a.name < b.name : a.mediaId < b.mediaId; });
        _sb.warnings = _out.warnings;
        // The story the screens tell: how each shot relates to the one before on its
        // screen, and the screens to each other at the same moment.
        double along = 0, across = 0;
        int nAlong = 0, nAcross = 0;
        for (size_t i = 0; i < _sb.shots.size(); ++i) {
            StoryboardShot& sh = _sb.shots[i];
            const MediaItem* m = ItemById(sh.mediaId);
            for (size_t k = i; k-- > 0;) {
                const StoryboardShot& b = _sb.shots[k];
                if (b.lane != sh.lane) {
                    break;
                }
                if (b.mediaId != sh.mediaId) {
                    const MediaItem* o = ItemById(b.mediaId);
                    if (m != nullptr && o != nullptr) {
                        std::string kind;
                        along += Relation(*o, *m, kind);
                        ++nAlong;
                        sh.relation = kind;
                    }
                    break;
                }
            }
            for (size_t k = 0; k < i; ++k) {
                const StoryboardShot& b = _sb.shots[k];
                if (b.lane != sh.lane && b.startMS < sh.endMS && b.endMS > sh.startMS && b.mediaId != sh.mediaId) {
                    const MediaItem* o = ItemById(b.mediaId);
                    if (m != nullptr && o != nullptr) {
                        std::string kind;
                        across += Relation(*o, *m, kind);
                        ++nAcross;
                    }
                }
            }
        }
        _sb.stats.coherence = nAlong > 0 ? R3(along / nAlong) : 0;
        _sb.stats.crossScreen = nAcross > 0 ? R3(across / nAcross) : 0;
        ComputeStoryboardStats(_sb, _plan.durationMS);
        _sb.hash = StoryboardHash(_sb);
        double share = 0;
        for (const StoryboardLaneStats& l : _sb.stats.lanes) {
            const auto lane = std::find_if(_lanes.begin(), _lanes.end(), [&](const Lane& x) { return x.t->name == l.lane; });
            if (lane != _lanes.end() && lane->main) {
                share = std::max(share, l.coverage);
            }
        }
        _out.share = R3(share);
        _out.enabled = !_out.assignments.empty();
        if (_out.usableItems > 0 && _sb.analysisMode == "basic") {
            _out.reasons.push_back({ "analysis-basic", "no image-text model: colours, light, shape and motion only" });
        }
    }
};

} // namespace

const MediaAssignment* MediaPlan::Find(const std::string& segmentId, const std::string& target) const {
    const auto it = std::find_if(assignments.begin(), assignments.end(), [&](const MediaAssignment& a) { return a.segmentId == segmentId && a.target == target; });
    return it != assignments.end() ? &*it : nullptr;
}

std::vector<const MediaAssignment*> MediaPlan::FindAll(const std::string& segmentId, const std::string& target) const {
    std::vector<const MediaAssignment*> out;
    for (const MediaAssignment& a : assignments) {
        if (a.segmentId == segmentId && a.target == target) {
            out.push_back(&a);
        }
    }
    return out;
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
        if (m.overlay) {
            continue; // between the phrases of a screen that keeps its words
        }
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
    // "Not this file here" sits beside any other choice: only the same one is replaced.
    if (c.action == MediaCorrection::Action::Avoid) {
        std::erase_if(list, [&](const MediaCorrection& o) { return o.action == c.action && o.segmentId == c.segmentId && o.target == c.target && o.mediaId == c.mediaId; });
        list.push_back(c);
        return;
    }
    const bool wholeSegment = c.action == MediaCorrection::Action::Remove && c.target.empty();
    std::erase_if(list, [&](const MediaCorrection& o) {
        if (o.segmentId != c.segmentId || o.action == MediaCorrection::Action::Avoid) {
            return false;
        }
        if (wholeSegment || o.target.empty()) {
            return true;
        }
        if (o.target != c.target) {
            return false;
        }
        // A choice for the whole segment replaces its shots, and a shot replaces the
        // same shot or a whole-segment choice.
        return c.shotStartMS < 0 || o.shotStartMS < 0 || o.shotStartMS == c.shotStartMS;
    });
    list.push_back(c);
}

std::vector<MediaCorrection> RestoreStoryboard(const StoryboardVersion& v, const VisualMediaStoryboard& current) {
    std::vector<MediaCorrection> out;
    std::set<std::string> had; // "scene|lane"
    for (const StoryboardVersion::Shot& s : v.shots) {
        MediaCorrection c;
        c.action = MediaCorrection::Action::Lock;
        c.segmentId = s.sceneId;
        c.target = s.lane;
        c.mediaId = s.mediaId;
        c.path = s.source;
        c.shotStartMS = s.startMS;
        c.shotEndMS = s.endMS;
        c.fragmentStartMS = s.fragmentStartMS;
        const StoryboardScene* scene = current.Scene(s.sceneId);
        c.startMS = scene != nullptr ? scene->startMS : s.startMS;
        UpsertMediaCorrection(out, c);
        had.insert(s.sceneId + "|" + s.lane);
    }
    for (const StoryboardScene& sc : current.scenes) {
        for (const StoryboardLane& l : current.lanes) {
            if (had.contains(sc.id + "|" + l.target)) {
                continue;
            }
            MediaCorrection c;
            c.action = MediaCorrection::Action::Remove;
            c.segmentId = sc.id;
            c.startMS = sc.startMS;
            c.target = l.target;
            UpsertMediaCorrection(out, c);
        }
    }
    return out;
}

std::vector<MediaCorrection> LockStoryboardScene(const VisualMediaStoryboard& b, const std::string& sceneId) {
    std::vector<MediaCorrection> out;
    const StoryboardScene* scene = b.Scene(sceneId);
    if (scene == nullptr) {
        return out;
    }
    std::set<std::string> lanes;
    for (const StoryboardShot& s : b.shots) {
        if (s.sceneId != sceneId) {
            continue;
        }
        MediaCorrection c;
        c.action = MediaCorrection::Action::Lock;
        c.segmentId = sceneId;
        c.startMS = scene->startMS;
        c.target = s.lane;
        c.mediaId = s.mediaId;
        c.path = s.source;
        c.shotStartMS = s.startMS;
        c.shotEndMS = s.endMS;
        c.fragmentStartMS = s.kind == MediaKind::Video ? s.fragmentStartMS : -1;
        UpsertMediaCorrection(out, c);
        lanes.insert(s.lane);
    }
    for (const StoryboardLane& l : b.lanes) {
        if (!lanes.contains(l.target)) {
            MediaCorrection c;
            c.action = MediaCorrection::Action::Remove;
            c.segmentId = sceneId;
            c.startMS = scene->startMS;
            c.target = l.target;
            UpsertMediaCorrection(out, c);
        }
    }
    return out;
}

std::vector<MediaCorrection> LockStoryboardLane(const VisualMediaStoryboard& b, const std::string& lane) {
    std::vector<MediaCorrection> out;
    if (b.Lane(lane) == nullptr) {
        return out;
    }
    for (const StoryboardScene& sc : b.scenes) {
        bool any = false;
        for (const StoryboardShot& s : b.shots) {
            if (s.sceneId != sc.id || s.lane != lane) {
                continue;
            }
            MediaCorrection c;
            c.action = MediaCorrection::Action::Lock;
            c.segmentId = sc.id;
            c.startMS = sc.startMS;
            c.target = lane;
            c.mediaId = s.mediaId;
            c.path = s.source;
            c.shotStartMS = s.startMS;
            c.shotEndMS = s.endMS;
            c.fragmentStartMS = s.kind == MediaKind::Video ? s.fragmentStartMS : -1;
            UpsertMediaCorrection(out, c);
            any = true;
        }
        if (!any) {
            MediaCorrection c;
            c.action = MediaCorrection::Action::Remove;
            c.segmentId = sc.id;
            c.startMS = sc.startMS;
            c.target = lane;
            UpsertMediaCorrection(out, c);
        }
    }
    return out;
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
        if (c.shotStartMS >= 0) {
            j["shot"] = { c.shotStartMS, c.shotEndMS };
        }
        if (c.fragmentStartMS >= 0) {
            j["fragmentStartMS"] = c.fragmentStartMS;
        }
        if (!c.decisionId.empty()) {
            j["decision"] = c.decisionId;
        }
        if (!c.origin.empty() && c.origin != "person") {
            j["origin"] = c.origin;
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
        if (j.contains("shot") && j["shot"].is_array() && j["shot"].size() == 2) {
            c.shotStartMS = j["shot"][0].get<int>();
            c.shotEndMS = j["shot"][1].get<int>();
        }
        c.fragmentStartMS = j.value("fragmentStartMS", -1);
        c.decisionId = j.value("decision", std::string());
        c.origin = j.value("origin", std::string("person"));
        if (c.segmentId.empty() && c.startMS < 0 && c.decisionId.empty()) {
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
        json j = { { "segment", u.segmentId }, { "target", u.target }, { "startMS", u.startMS }, { "endMS", u.endMS }, { "media", u.mediaId }, { "path", u.path }, { "effect", u.effect },
                   { "state", DecisionStateId(u.state) } };
        if (!u.source.empty() && u.source != u.path) {
            j["source"] = u.source;
        }
        if (u.fragmentStartMS >= 0) {
            j["fragmentStartMS"] = u.fragmentStartMS;
        }
        if (!u.framing.empty()) {
            j["framing"] = u.framing;
        }
        used.push_back(std::move(j));
    }
    json root = { { "folders", s.folders }, { "automatic", s.automatic }, { "corrections", MediaCorrectionsJson(s.corrections) }, { "used", std::move(used) } };
    if (s.maxShare >= 0) {
        root["maxShare"] = s.maxShare;
    }
    if (s.variant != 0) {
        root["variant"] = s.variant;
    }
    if (!s.storyboardHash.empty()) {
        root["storyboard"] = s.storyboardHash;
    }
    if (!s.screens.Empty()) {
        root["screens"] = ScreenSettingsJson(s.screens);
    }
    if (!s.history.empty()) {
        json history = json::array();
        for (const StoryboardVersion& v : s.history) {
            history.push_back(StoryboardVersionJson(v));
        }
        root["history"] = std::move(history);
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
    s.variant = j.value("variant", 0);
    s.storyboardHash = j.value("storyboard", std::string());
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
        m.source = u.value("source", m.path);
        m.fragmentStartMS = u.value("fragmentStartMS", -1);
        m.framing = u.value("framing", std::string());
        s.used.push_back(std::move(m));
    }
    if (j.contains("screens") && !ScreenSettingsFrom(j["screens"], s.screens, error)) {
        return false;
    }
    for (const json& v : j.value("history", json::array())) {
        s.history.push_back(StoryboardVersionFrom(v));
    }
    return true;
}

const char* MediaActionId(MediaCorrection::Action action) {
    switch (action) {
    case MediaCorrection::Action::Assign: return "assign";
    case MediaCorrection::Action::Lock: return "lock";
    case MediaCorrection::Action::Avoid: return "avoid";
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
    } else if (id == "avoid") {
        action = MediaCorrection::Action::Avoid;
    } else {
        return false;
    }
    return true;
}

std::string ToJson(const MediaPlan& p) {
    json root = { { "version", p.version }, { "enabled", p.enabled }, { "themeId", p.themeId }, { "themeCategories", p.themeCategories }, { "usableItems", p.usableItems },
                  { "maxShare", p.maxShare }, { "share", p.share }, { "warnings", p.warnings }, { "storyboard", p.storyboard.hash } };
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
                         { "reasons", std::move(why) }, { "constraints", a.constraints }, { "shot", a.shotId }, { "source", a.source }, { "framing", a.framing },
                         { "level", a.level } });
    }
    root["assignments"] = std::move(list);
    return root.dump(2);
}

std::string MediaReport(const MediaPlan& p) {
    std::string out = fmt::format("Media: {} shots, {:.0f}% of the song on the busiest screen (at most {:.0f}%), {} usable files, theme {}\n", p.assignments.size(), p.share * 100, p.maxShare * 100,
                                  p.usableItems, p.themeId.empty() ? std::string("none") : p.themeId);
    const auto clock = [](int ms) { return fmt::format("{}:{:02}.{:01}", ms / 60000, ms / 1000 % 60, ms / 100 % 10); };
    for (const MediaAssignment& a : p.assignments) {
        out += fmt::format("  {} - {} {} on {}: {} ({}, {}, {})", clock(a.startMS), clock(a.endMS), a.effect, a.target, a.name, MediaKindId(a.kind), a.framing, DecisionStateId(a.state));
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
