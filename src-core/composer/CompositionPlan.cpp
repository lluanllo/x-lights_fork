#include "composer/CompositionPlan.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>

using json = nlohmann::json;

namespace composer {

namespace {

const char* const PROMINENCE_IDS[] = { "lead", "support", "accent", "rest" };
const char* const DECISION_IDS[] = { "automatic", "manual-edit", "approved-preference", "locked" };
const char* const LEVEL_IDS[] = { "conservative", "balanced", "creative" };
const char* const ACTION_IDS[] = { "exclude", "include", "set-prominence", "set-density", "move-boundary", "lock", "accept", "reject" };

enum class Band {
    Rest,
    Low,
    Mid,
    High,
    Climax
};

// How well a role suits a lead at each energy: the big props carry loud passages, the
// small ones quiet ones. Planner logic, not taste; the profile weighs roles on top.
double Fit(Role role, Band band) {
    struct Row {
        Role role;
        double low, mid, high;
    };
    static const Row ROWS[] = {
        { Role::MegaTree, 0.6, 0.85, 1.0 }, { Role::Matrix, 0.7, 0.85, 1.0 }, { Role::Group, 0.3, 0.6, 0.95 },
        { Role::SmallTree, 0.9, 0.8, 0.7 }, { Role::Figure, 0.9, 0.8, 0.7 }, { Role::Window, 0.8, 0.7, 0.6 },
        { Role::Arch, 0.6, 0.7, 0.7 }, { Role::Star, 0.8, 0.7, 0.6 }, { Role::Line, 0.5, 0.6, 0.6 },
        { Role::Prop, 0.7, 0.6, 0.5 }, { Role::Face, 0.6, 0.5, 0.4 },
    };
    for (const Row& row : ROWS) {
        if (row.role == role) {
            return band == Band::Low ? row.low : band == Band::Mid ? row.mid : row.high;
        }
    }
    return 0.3;
}

double AccentFit(Role role) {
    switch (role) {
    case Role::Line:
    case Role::Arch:
    case Role::Star:
    case Role::Window:
        return 1.0;
    case Role::Prop:
    case Role::SmallTree:
        return 0.8;
    default:
        return 0.5;
    }
}

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

// Our own conversion to [0, 1): standard distributions differ between libraries.
double Unit(uint64_t seed, const std::string& key) {
    return static_cast<double>(Mix(seed ^ Fnv(key)) >> 11) * (1.0 / 9007199254740992.0);
}

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

int ToFrame(double ms, int frame) {
    return static_cast<int>(std::lround(ms / frame)) * frame;
}

// Where a cut comes from decides what may remove it.
enum class Cut {
    Split,
    Climax,
    Section,
    User
};

struct Candidate {
    int target = -1; // index into capabilities
    Role role = Role::Prop;
    double confidence = 0;
    int lit = 0; // models it lights
    bool unknownRole = false;
};

class Planner {
public:
    Planner(const PlannerInput& in, const StyleProfile& profile, CompositionPlan& plan) :
        _in(in),
        _a(*in.analysis),
        _caps(*in.capabilities),
        _profile(profile),
        _level(profile.Level(in.options.level)),
        _plan(plan),
        _frame(std::max(1, in.options.frameMS)) {}

    void Run() {
        CollectCandidates();
        CollectLyrics();
        Segment();
        for (PlanSegment& s : _plan.segments) {
            Describe(s);
        }
        for (size_t i = 0; i < _plan.segments.size(); ++i) {
            Assign(i);
        }
        ApplyCorrections();
        Finish();
    }

private:
    // ------------------------------------------------------------ candidates

    void CollectCandidates() {
        for (size_t i = 0; i < _caps.targets.size(); ++i) {
            const TargetInfo& t = _caps.targets[i];
            if (t.type == TargetType::Model) {
                _models.push_back(static_cast<int>(i));
            }
            if (_caps.LightsExcluded(t.name)) {
                continue; // a group would light the excluded model through the back door
            }
            if (t.type == TargetType::SubModel && !(_in.options.useSubModels && !t.roles.empty() && t.roles[0].status == RoleStatus::Confirmed)) {
                continue;
            }
            if (t.Can(Capability::Geometric).support == Support::No) {
                continue;
            }
            Candidate c;
            c.target = static_cast<int>(i);
            const RoleAssignment* visual = nullptr;
            for (const RoleAssignment& r : t.roles) {
                if (r.role != Role::Face && r.role != Role::Excluded) {
                    visual = &r;
                    break;
                }
            }
            if (visual == nullptr && !t.roles.empty()) {
                visual = &t.roles.front();
            }
            if (visual == nullptr) {
                c.role = Role::Prop;
                c.confidence = 0.2;
                c.unknownRole = true;
            } else {
                c.role = visual->role;
                c.confidence = visual->confidence;
            }
            if (c.confidence < _level.minRoleConfidence) {
                continue;
            }
            for (uint64_t word : _caps.modelBits[i]) {
                c.lit += static_cast<int>(std::popcount(word));
            }
            if (c.lit == 0) {
                continue;
            }
            _candidates.push_back(c);
            if (t.type == TargetType::Model && t.Can(Capability::Face).support == Support::Yes) {
                _faces.push_back(c);
            }
            if (t.Can(Capability::Text).support == Support::Yes) {
                _texts.push_back(c);
            }
        }
        if (_models.empty()) {
            _plan.warnings.push_back("layout-has-no-models");
        }
    }

    // ------------------------------------------------------------ lyrics

    struct Span {
        int start;
        int end;
        bool chorus;
        double confidence;
    };

    void CollectLyrics() {
        if (_in.lyrics == nullptr || _in.lyrics->Empty()) {
            _plan.warnings.push_back("no-lyrics");
            return;
        }
        std::set<std::string> chorusLines;
        for (const LyricBlock& block : _in.lyrics->blocks) {
            if (block.kind == LyricBlockKind::Chorus) {
                for (const LyricLine& line : block.lines) {
                    chorusLines.insert(line.id);
                }
            }
        }
        if (_in.alignment != nullptr) {
            for (const AlignedPhrase& p : _in.alignment->phrases) {
                if (p.startMS >= 0 && p.endMS > p.startMS && !p.instrumental) {
                    _phrases.push_back({ p.startMS, p.endMS, chorusLines.contains(p.id), p.confidence });
                }
            }
        }
        _plan.lyricsSynchronized = !_phrases.empty();
        if (!_plan.lyricsSynchronized) {
            // The words are known but not when they are sung: no face or text action.
            _plan.warnings.push_back("lyrics-not-synchronized");
        }
        if (!chorusLines.empty() && !_plan.lyricsSynchronized) {
            _plan.warnings.push_back("user-chorus-without-times");
        }
    }

    // ------------------------------------------------------------ segments

    double MeanIntensity(int from, int to) const {
        double sum = 0;
        int n = 0;
        for (const EnergyPoint& p : _a.energy) {
            if (p.timeMS >= from && p.timeMS < to) {
                sum += p.intensity;
                ++n;
            }
        }
        return n == 0 ? 0 : sum / n;
    }

    int SectionAt(int ms) const {
        for (size_t i = 0; i < _a.sections.size(); ++i) {
            if (ms >= _a.sections[i].startMS && ms < _a.sections[i].endMS) {
                return static_cast<int>(i);
            }
        }
        return _a.sections.empty() ? -1 : static_cast<int>(_a.sections.size()) - 1;
    }

    bool IsSilentSection(int section) const {
        if (section < 0) {
            return false;
        }
        const auto& ev = _a.sections[section].evidence;
        return std::find(ev.begin(), ev.end(), "silence") != ev.end();
    }

    std::vector<int> EventTimes(MusicalEventType type, int from, int to) const {
        std::vector<int> times;
        for (const MusicalEvent& e : _a.events) {
            if (e.type == type && e.timeMS > from && e.timeMS < to) {
                times.push_back(e.timeMS);
            }
        }
        return times;
    }

    // A long stretch is cut where the music changes most, on a downbeat if there is
    // one, so the halves still feel like units.
    void SplitLong(int from, int to, std::vector<std::pair<int, Cut>>& cuts) const {
        if (to - from <= _profile.maxSegmentMS) {
            return;
        }
        std::vector<int> points = EventTimes(MusicalEventType::Downbeat, from + _profile.minSegmentMS, to - _profile.minSegmentMS);
        if (points.empty()) {
            points = EventTimes(MusicalEventType::Beat, from + _profile.minSegmentMS, to - _profile.minSegmentMS);
        }
        if (points.empty()) {
            for (int p = from + _profile.minSegmentMS; p <= to - _profile.minSegmentMS; p += 1000) {
                points.push_back(p);
            }
        }
        if (points.empty()) {
            return;
        }
        int best = points[points.size() / 2];
        double bestChange = -1;
        constexpr int SIDE_MS = 2000;
        for (int p : points) {
            const double change = std::fabs(MeanIntensity(p - SIDE_MS, p) - MeanIntensity(p, p + SIDE_MS));
            // Ties go to the point nearest the middle, so equal music splits evenly.
            const double balance = 1.0 - std::fabs((p - from) - (to - p)) / static_cast<double>(to - from);
            const double score = change + 0.05 * balance;
            if (score > bestChange) {
                bestChange = score;
                best = p;
            }
        }
        best = ToFrame(best, _frame);
        cuts.push_back({ best, Cut::Split });
        SplitLong(from, best, cuts);
        SplitLong(best, to, cuts);
    }

    void Segment() {
        const int duration = _a.durationMS;
        _plan.durationMS = duration;
        std::map<int, Cut> cuts;
        const auto addCut = [&](int ms, Cut kind) {
            if (ms <= 0 || ms >= duration) {
                return;
            }
            ms = ToFrame(ms, _frame);
            auto it = cuts.find(ms);
            if (it == cuts.end() || it->second < kind) {
                cuts[ms] = kind;
            }
        };
        for (const SongSection& s : _a.sections) {
            addCut(s.startMS, Cut::Section);
        }
        for (const PlanCorrection& c : _in.corrections) {
            if ((c.action == PlanCorrection::Action::MoveBoundary || c.action == PlanCorrection::Action::Lock) && c.startMS >= 0 && c.endMS > c.startMS) {
                addCut(c.startMS, Cut::User);
                addCut(c.endMS, Cut::User);
            }
        }
        for (const MusicalEvent& e : _a.events) {
            if (e.type != MusicalEventType::Climax) {
                continue;
            }
            // A climax that starts close to a section change belongs to that change.
            for (int edge : { e.timeMS, e.timeMS + e.durationMS }) {
                const bool nearSection = std::any_of(_a.sections.begin(), _a.sections.end(), [&](const SongSection& s) { return std::abs(s.startMS - edge) < _profile.minSegmentMS / 2; });
                if (!nearSection) {
                    addCut(edge, Cut::Climax);
                }
            }
        }

        // Long stretches are split inside, never across, the cuts above.
        std::vector<int> base = { 0 };
        for (const auto& [ms, kind] : cuts) {
            base.push_back(ms);
        }
        base.push_back(duration);
        for (size_t i = 0; i + 1 < base.size(); ++i) {
            if (!IsSilentSection(SectionAt(base[i]))) {
                std::vector<std::pair<int, Cut>> splits;
                SplitLong(base[i], base[i + 1], splits);
                for (const auto& [ms, kind] : splits) {
                    addCut(ms, kind);
                }
            }
        }

        struct Piece {
            int start;
            int end;
            Cut left; // the cut that opens it
        };
        std::vector<Piece> pieces;
        int start = 0;
        Cut left = Cut::Section; // the start of the song: nothing to merge into
        for (const auto& [ms, kind] : cuts) {
            pieces.push_back({ start, ms, left });
            start = ms;
            left = kind;
        }
        pieces.push_back({ start, duration, left });

        // Short pieces join a neighbour of the same section; silences long enough to be
        // a rest, and whatever a person bounded, stay as they are.
        const auto keep = [&](const Piece& p, size_t i) {
            const bool userBounded = p.left == Cut::User && (i + 1 >= pieces.size() || pieces[i + 1].left == Cut::User);
            const bool rest = IsSilentSection(SectionAt(p.start)) && p.end - p.start >= _profile.minRestMS;
            return userBounded || rest || p.end - p.start >= _profile.minSegmentMS;
        };
        bool merged = true;
        while (merged && pieces.size() > 1) {
            merged = false;
            for (size_t i = 0; i < pieces.size(); ++i) {
                if (keep(pieces[i], i)) {
                    continue;
                }
                const int section = SectionAt(pieces[i].start);
                const bool canLeft = i > 0 && pieces[i].left != Cut::User && (SectionAt(pieces[i - 1].start) == section || pieces[i].left != Cut::Section);
                const bool canRight = i + 1 < pieces.size() && pieces[i + 1].left != Cut::User && (SectionAt(pieces[i + 1].start) == section || pieces[i + 1].left != Cut::Section);
                // Failing both, a short section joins whichever neighbour is not a person's.
                const bool forcedLeft = !canLeft && !canRight && i > 0 && pieces[i].left != Cut::User;
                const bool forcedRight = !canLeft && !canRight && !forcedLeft && i + 1 < pieces.size() && pieces[i + 1].left != Cut::User;
                if (canLeft || forcedLeft) {
                    pieces[i - 1].end = pieces[i].end;
                    pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(i));
                } else if (canRight || forcedRight) {
                    pieces[i + 1].start = pieces[i].start;
                    pieces[i + 1].left = pieces[i].left;
                    pieces.erase(pieces.begin() + static_cast<std::ptrdiff_t>(i));
                } else {
                    continue;
                }
                merged = true;
                break;
            }
        }

        // No more changes per minute than the profile allows, counted per section so a
        // section's segments never depend on another's.
        std::map<int, std::vector<size_t>> bySection;
        for (size_t i = 0; i < pieces.size(); ++i) {
            bySection[SectionAt(pieces[i].start)].push_back(i);
        }
        std::set<size_t> removed;
        for (auto& [section, members] : bySection) {
            const int length = pieces[members.back()].end - pieces[members.front()].start;
            const int allowed = std::max(1, static_cast<int>(std::floor(_profile.maxChangesPerMinute * length / 60000.0 + 0.5)));
            std::vector<size_t> live = members;
            while (static_cast<int>(live.size()) > allowed) {
                size_t best = live.size();
                int bestLength = 0;
                for (size_t k = 1; k < live.size(); ++k) {
                    const Piece& p = pieces[live[k]];
                    if (p.left == Cut::User) {
                        continue;
                    }
                    const int combined = p.end - pieces[live[k - 1]].start;
                    if (best == live.size() || combined < bestLength) {
                        best = k;
                        bestLength = combined;
                    }
                }
                if (best == live.size()) {
                    break;
                }
                pieces[live[best - 1]].end = pieces[live[best]].end;
                removed.insert(live[best]);
                live.erase(live.begin() + static_cast<std::ptrdiff_t>(best));
            }
        }
        std::map<int, int> ordinals;
        for (size_t i = 0; i < pieces.size(); ++i) {
            if (removed.contains(i)) {
                continue;
            }
            PlanSegment s;
            s.startMS = pieces[i].start;
            s.endMS = pieces[i].end;
            s.sectionIndex = SectionAt(s.startMS);
            s.id = fmt::format("s{}.{}", s.sectionIndex, ordinals[s.sectionIndex]++);
            if (pieces[i].left == Cut::User) {
                s.reasons.push_back({ "boundary-by-user", "" });
            }
            _plan.segments.push_back(std::move(s));
        }
    }

    // ------------------------------------------------------------ description

    Band BandOf(const PlanSegment& s) const {
        if (std::find(s.flags.begin(), s.flags.end(), "silence") != s.flags.end()) {
            return Band::Rest;
        }
        if (std::find(s.flags.begin(), s.flags.end(), "climax") != s.flags.end()) {
            return Band::Climax;
        }
        return s.energy >= _profile.highEnergy ? Band::High : s.energy >= _profile.lowEnergy ? Band::Mid : Band::Low;
    }

    void Describe(PlanSegment& s) {
        const int length = s.endMS - s.startMS;
        s.energy = Round3(MeanIntensity(s.startMS, s.endMS));
        if (s.sectionIndex >= 0) {
            const SongSection& section = _a.sections[s.sectionIndex];
            s.sectionKind = SectionKindId(section.kind);
            s.repeatGroup = section.repeatGroup;
            for (int k = 0; k < s.sectionIndex; ++k) {
                s.occurrence += section.repeatGroup >= 0 && _a.sections[k].repeatGroup == section.repeatGroup ? 1 : 0;
            }
        }
        int silent = 0;
        int climax = 0;
        for (const MusicalEvent& e : _a.events) {
            const int overlap = std::max(0, std::min(s.endMS, e.timeMS + e.durationMS) - std::max(s.startMS, e.timeMS));
            if (e.type == MusicalEventType::Silence) {
                silent += overlap;
            } else if (e.type == MusicalEventType::Climax) {
                climax += overlap;
            } else if (e.type == MusicalEventType::Ending && e.timeMS >= s.startMS && e.timeMS < s.endMS) {
                s.flags.push_back("ending");
            }
        }
        if (IsSilentSection(s.sectionIndex) || silent * 5 >= length * 4) {
            s.flags.push_back("silence");
        }
        if (climax * 2 >= length) {
            s.flags.push_back("climax");
        }
        if (s.sectionKind == "intro") {
            s.flags.push_back("intro");
        }
        int sung = 0;
        bool chorus = false;
        for (const Span& p : _phrases) {
            const int overlap = std::max(0, std::min(s.endMS, p.end) - std::max(s.startMS, p.start));
            sung += overlap;
            chorus = chorus || (p.chorus && overlap * 2 >= std::min(length, p.end - p.start));
        }
        if (_phrases.empty()) {
            for (const VocalSegment& v : _a.vocalSegments) {
                if (v.confidence >= _in.options.minVocalConfidence) {
                    sung += std::max(0, std::min(s.endMS, v.endMS) - std::max(s.startMS, v.startMS));
                }
            }
        }
        s.vocalCoverage = length > 0 ? Round3(std::min(1.0, static_cast<double>(sung) / length)) : 0;
        if (s.vocalCoverage > 0) {
            s.flags.push_back("lyrics");
        }
        if (chorus) {
            s.flags.push_back("user-chorus");
        }
        std::sort(s.flags.begin(), s.flags.end());
    }

    // ------------------------------------------------------------ assignment

    std::string Key(const PlanSegment& s) const {
        // A repeated section rotates by occurrence: the second chorus need not repeat
        // the first one's choices. Nothing in the key comes from other sections.
        return s.repeatGroup >= 0 ? fmt::format("g{}:{}", s.repeatGroup, s.occurrence) : fmt::format("s{}", s.sectionIndex);
    }

    bool Conflicts(int target, const std::vector<int>& chosen) const {
        if (_profile.groupsMayOverlapMembers) {
            return std::find(chosen.begin(), chosen.end(), target) != chosen.end();
        }
        for (int other : chosen) {
            if (_caps.Overlap(_caps.targets[target].name, _caps.targets[other].name)) {
                return true;
            }
        }
        return false;
    }

    double Weight(const std::map<Role, double>& preference, Role role) const {
        const auto found = preference.find(role);
        return found == preference.end() ? 0.3 : found->second;
    }

    PlanAssignment Make(const Candidate& c, Prominence p, const char* intent, double intensity) const {
        PlanAssignment a;
        const TargetInfo& t = _caps.targets[c.target];
        a.target = t.name;
        a.type = t.type;
        a.role = c.role;
        a.prominence = p;
        a.intent = intent;
        const double share = p == Prominence::Lead ? 1.0 : p == Prominence::Accent ? 0.85 : 0.7;
        a.intensity = Round3(intensity * share);
        a.confidence = Round3(c.confidence);
        a.reasons.push_back({ "role", RoleId(c.role) });
        if (c.unknownRole) {
            a.reasons.push_back({ "role-unknown", "" });
        }
        return a;
    }

    void Assign(size_t index) {
        PlanSegment& s = _plan.segments[index];
        const Band band = BandOf(s);
        const LevelSettings& L = _level;
        std::vector<int> chosen;
        int lit = 0;
        const auto take = [&](const Candidate& c, PlanAssignment a) {
            chosen.push_back(c.target);
            lit += c.lit;
            s.assignments.push_back(std::move(a));
        };

        const double levelIntensity = _in.options.level == AutomationLevel::Conservative ? 0.8 : _in.options.level == AutomationLevel::Creative ? 1.1 : 1.0;
        const bool userChorus = std::find(s.flags.begin(), s.flags.end(), "user-chorus") != s.flags.end();
        s.targetIntensity = band == Band::Rest ? 0 : band == Band::Climax ? 1.0 : Round3(std::min(1.0, s.energy * levelIntensity + (userChorus ? 0.1 : 0)));
        s.priority = band == Band::Rest ? 0 : band == Band::Climax ? 3 : (band == Band::High || userChorus) ? 2 : 1;

        if (band == Band::Rest) {
            s.reasons.push_back({ "silence", "" });
            FillRests(s, chosen, "silence");
            return;
        }
        s.reasons.push_back({ band == Band::Climax ? "climax" : band == Band::High ? "energy-high" : band == Band::Mid ? "energy-mid" : "energy-low", fmt::format("{:.2f}", s.energy) });
        if (s.sectionKind == "unknown") {
            s.reasons.push_back({ "section-unknown-energy-used", "" });
        }
        if (userChorus) {
            s.reasons.push_back({ "user-tagged-chorus", "" });
        }
        const std::string key = Key(s);

        // A sung phrase gives the faces the lead, if a face can sing it.
        const bool vocal = _plan.lyricsSynchronized && s.vocalCoverage >= 0.2;
        if (vocal) {
            std::vector<Candidate> faces = _faces;
            std::sort(faces.begin(), faces.end(), [&](const Candidate& a, const Candidate& b) {
                return Unit(_in.options.seed, key + "/face/" + _caps.targets[a.target].name) < Unit(_in.options.seed, key + "/face/" + _caps.targets[b.target].name);
            });
            int taken = 0;
            for (const Candidate& c : faces) {
                if (taken >= L.maxFaces || Conflicts(c.target, chosen)) {
                    continue;
                }
                PlanAssignment a = Make(c, Prominence::Lead, "face-sing", s.targetIntensity);
                a.reasons.push_back({ "lyrics-synced-phrase", fmt::format("{:.2f}", s.vocalCoverage) });
                a.reasons.push_back({ "face-definition", "" });
                take(c, std::move(a));
                ++taken;
            }
            if (_faces.empty()) {
                s.reasons.push_back({ "no-face-can-sing", "" });
            }
        }

        // Visual leads: the best candidates for this energy take turns.
        int leads = band == Band::Climax ? L.maxLeads : band == Band::High ? std::min(2, L.maxLeads) : 1;
        if (_in.options.level == AutomationLevel::Creative && band == Band::Mid) {
            leads = std::min(2, L.maxLeads);
        }
        std::vector<std::pair<double, int>> ranked;
        for (size_t i = 0; i < _candidates.size(); ++i) {
            const Candidate& c = _candidates[i];
            if (c.unknownRole) {
                continue; // an unclassified target never leads
            }
            const double score = Weight(_profile.leadPreference, c.role) * c.confidence * Fit(c.role, band);
            ranked.push_back({ score, static_cast<int>(i) });
        }
        std::sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : _caps.targets[_candidates[a.second].target].name < _caps.targets[_candidates[b.second].target].name;
        });
        const std::string previousLead = PreviousLead(index);
        const int ordinal = static_cast<int>(std::strtol(s.id.substr(s.id.find('.') + 1).c_str(), nullptr, 10));
        for (int k = 0; k < leads && !ranked.empty(); ++k) {
            const int pool = std::min<int>(L.leadPool, static_cast<int>(ranked.size()));
            int pick = 0;
            if (L.variation <= 0) {
                pick = (s.occurrence + k) % pool;
            } else {
                const std::string turn = fmt::format("{}/lead/{}/{}", key, k, L.rotateWithinSection ? ordinal : 0);
                pick = std::min(pool - 1, static_cast<int>(Unit(_in.options.seed, turn) * pool));
            }
            for (int step = 0; step < static_cast<int>(ranked.size()); ++step) {
                const int at = (pick + step) % static_cast<int>(ranked.size());
                const Candidate& c = _candidates[ranked[at].second];
                const std::string& name = _caps.targets[c.target].name;
                const bool repeats = L.rotateWithinSection && name == previousLead && pool > 1 && step == 0;
                if (repeats || Conflicts(c.target, chosen)) {
                    continue;
                }
                PlanAssignment a = Make(c, Prominence::Lead, "feature", s.targetIntensity);
                a.reasons.push_back({ "lead-score", fmt::format("{:.2f}", ranked[at].first) });
                a.reasons.push_back({ "rotation", fmt::format("{} of {}", pick + 1, pool) });
                if (s.occurrence > 0) {
                    a.reasons.push_back({ "repeat-occurrence", std::to_string(s.occurrence) });
                }
                take(c, std::move(a));
                break;
            }
        }

        // Words on a sung phrase go to a target that can show text, as support unless it
        // already leads.
        if (_plan.lyricsSynchronized && s.vocalCoverage >= (_in.options.level == AutomationLevel::Conservative ? 0.5 : 0.2) && !_texts.empty()) {
            const Candidate* best = nullptr;
            for (const Candidate& c : _texts) {
                if (best == nullptr || _caps.targets[c.target].bufferWi * _caps.targets[c.target].bufferHt > _caps.targets[best->target].bufferWi * _caps.targets[best->target].bufferHt) {
                    best = &c;
                }
            }
            const auto existing = std::find_if(s.assignments.begin(), s.assignments.end(), [&](const PlanAssignment& a) { return a.target == _caps.targets[best->target].name; });
            if (existing != s.assignments.end()) {
                if (existing->intent == "feature") {
                    existing->intent = "lyric-text";
                    existing->reasons.push_back({ "text-capability", "" });
                }
            } else if (!Conflicts(best->target, chosen)) {
                PlanAssignment a = Make(*best, Prominence::Support, "lyric-text", s.targetIntensity);
                a.reasons.push_back({ "text-capability", "" });
                a.reasons.push_back({ "lyrics-synced-phrase", fmt::format("{:.2f}", s.vocalCoverage) });
                take(*best, std::move(a));
            }
        }

        FillSupports(s, band, key, chosen, lit, -1);

        if (band != Band::Low && L.accents > 0 && !EventTimes(MusicalEventType::Beat, s.startMS, s.endMS).empty()) {
            std::vector<std::pair<double, int>> accents;
            for (size_t i = 0; i < _candidates.size(); ++i) {
                const Candidate& c = _candidates[i];
                if (_caps.targets[c.target].Can(Capability::Geometric).support != Support::Yes || _caps.targets[c.target].type == TargetType::Group) {
                    continue;
                }
                const double jitter = L.variation * (Unit(_in.options.seed, key + "/accent/" + _caps.targets[c.target].name) - 0.5);
                accents.push_back({ AccentFit(c.role) * c.confidence + jitter, static_cast<int>(i) });
            }
            std::sort(accents.begin(), accents.end(), [&](const auto& a, const auto& b) {
                return a.first != b.first ? a.first > b.first : _caps.targets[_candidates[a.second].target].name < _caps.targets[_candidates[b.second].target].name;
            });
            int taken = 0;
            for (const auto& [score, i] : accents) {
                if (taken >= L.accents) {
                    break;
                }
                const Candidate& c = _candidates[i];
                if (Conflicts(c.target, chosen)) {
                    continue;
                }
                PlanAssignment a = Make(c, Prominence::Accent, "beat-accent", s.targetIntensity);
                a.reasons.push_back({ "beats-in-segment", "" });
                take(c, std::move(a));
                ++taken;
            }
        }
        FillRests(s, chosen, "density-budget");
    }

    // Supports fill the light budget the energy allows, largest gaps first.
    void FillSupports(PlanSegment& s, Band band, const std::string& key, std::vector<int>& chosen, int& lit, double densityOverride) {
        const double base = band == Band::Climax ? _profile.densityClimax : band == Band::High ? _profile.densityHigh : band == Band::Mid ? _profile.densityMid : _profile.densityLow;
        const double density = densityOverride >= 0 ? densityOverride : std::min(1.0, base * _level.densityScale);
        const int budget = std::max(1, static_cast<int>(std::lround(density * static_cast<double>(_models.size()))));
        const int maxParticipants = std::max(1, static_cast<int>(std::lround(_profile.maxParticipants * _level.densityScale)));
        std::vector<std::pair<double, int>> ranked;
        for (size_t i = 0; i < _candidates.size(); ++i) {
            const Candidate& c = _candidates[i];
            const double jitter = _level.variation * 0.5 * (Unit(_in.options.seed, key + "/support/" + _caps.targets[c.target].name) - 0.5);
            ranked.push_back({ Weight(_profile.supportPreference, c.role) * c.confidence * Fit(c.role, band) + jitter, static_cast<int>(i) });
        }
        std::sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) {
            return a.first != b.first ? a.first > b.first : _caps.targets[_candidates[a.second].target].name < _caps.targets[_candidates[b.second].target].name;
        });
        for (const auto& [score, i] : ranked) {
            if (lit >= budget || static_cast<int>(chosen.size()) >= maxParticipants) {
                break;
            }
            const Candidate& c = _candidates[i];
            // A group bigger than what is left of the budget would flood the scene.
            if (c.lit > (budget - lit) * 3 / 2 + 1 || Conflicts(c.target, chosen)) {
                continue;
            }
            PlanAssignment a = Make(c, Prominence::Support, "accompany", s.targetIntensity);
            a.reasons.push_back({ "support-score", fmt::format("{:.2f}", score) });
            chosen.push_back(c.target);
            lit += c.lit;
            s.assignments.push_back(std::move(a));
        }
        s.density = _models.empty() ? 0 : Round3(static_cast<double>(lit) / static_cast<double>(_models.size()));
    }

    // Every model not lit says so, and why: the plan accounts for the whole layout.
    void FillRests(PlanSegment& s, const std::vector<int>& chosen, const char* why) {
        std::vector<uint64_t> lit(_caps.modelBits.empty() ? 0 : _caps.modelBits[0].size(), 0);
        for (int c : chosen) {
            for (size_t w = 0; w < lit.size(); ++w) {
                lit[w] |= _caps.modelBits[c][w];
            }
        }
        int ordinal = 0;
        for (int m : _models) {
            const bool on = (lit[ordinal / 64] >> (ordinal % 64)) & 1ULL;
            ++ordinal;
            if (on) {
                continue;
            }
            const TargetInfo& t = _caps.targets[m];
            PlanAssignment a;
            a.target = t.name;
            a.type = t.type;
            a.role = t.PrimaryRole() != nullptr ? t.PrimaryRole()->role : Role::Prop;
            a.prominence = Prominence::Rest;
            a.intent = "rest";
            a.confidence = 1;
            a.reasons.push_back({ t.Excluded() ? "excluded" : why, "" });
            s.assignments.push_back(std::move(a));
        }
        std::sort(s.assignments.begin(), s.assignments.end(), [](const PlanAssignment& a, const PlanAssignment& b) { return a.target < b.target; });
    }

    std::string PreviousLead(size_t index) const {
        if (index == 0 || _plan.segments[index - 1].sectionIndex != _plan.segments[index].sectionIndex) {
            return "";
        }
        for (const PlanAssignment& a : _plan.segments[index - 1].assignments) {
            if (a.prominence == Prominence::Lead && a.intent != "face-sing") {
                return a.target;
            }
        }
        return "";
    }

    // ------------------------------------------------------------ corrections

    bool InRange(const PlanSegment& s, const PlanCorrection& c) const {
        if (c.startMS < 0) {
            return true;
        }
        const int middle = (s.startMS + s.endMS) / 2;
        return middle >= c.startMS && middle < c.endMS;
    }

    // What a manual decision would need; an answer here is reported, never applied.
    std::string Refusal(const std::string& target, Prominence p, const std::string& intent) const {
        const TargetInfo* t = _caps.Find(target);
        if (t == nullptr) {
            return "target-missing";
        }
        if (p == Prominence::Rest) {
            return "";
        }
        if (t->Excluded()) {
            return "target-excluded";
        }
        if (_caps.LightsExcluded(target)) {
            return "lights-excluded-model";
        }
        if (intent == "face-sing" && t->Can(Capability::Face).support != Support::Yes) {
            return "capability-no:face";
        }
        if (intent == "lyric-text" && t->Can(Capability::Text).support == Support::No) {
            return "capability-no:text";
        }
        if (t->Can(Capability::Geometric).support == Support::No) {
            return "capability-no:geometric";
        }
        return "";
    }

    void Rest(PlanSegment& s, const std::string& target, DecisionState state, const char* why) {
        for (auto it = s.assignments.begin(); it != s.assignments.end();) {
            if (it->target == target) {
                it = s.assignments.erase(it);
            } else {
                ++it;
            }
        }
        const TargetInfo* t = _caps.Find(target);
        if (t != nullptr && t->type == TargetType::Model) {
            PlanAssignment a;
            a.target = target;
            a.type = t->type;
            a.role = t->PrimaryRole() != nullptr ? t->PrimaryRole()->role : Role::Prop;
            a.prominence = Prominence::Rest;
            a.intent = "rest";
            a.confidence = 1;
            a.state = state;
            a.reasons.push_back({ why, "" });
            s.assignments.push_back(std::move(a));
        }
    }

    void Put(PlanSegment& s, PlanAssignment a, int startMS, int endMS) {
        // Manual wins over automatic; two manual decisions on the same lights are a
        // conflict for the person to settle.
        std::vector<std::string> displaced;
        for (const PlanAssignment& other : s.assignments) {
            if (other.target == a.target || other.prominence == Prominence::Rest || !_caps.Overlap(other.target, a.target)) {
                continue;
            }
            if (other.state == DecisionState::Automatic || _profile.groupsMayOverlapMembers) {
                if (!_profile.groupsMayOverlapMembers) {
                    displaced.push_back(other.target);
                }
            } else {
                _plan.conflicts.push_back({ a.target, startMS, endMS, "group-conflict-with-manual:" + other.target });
                return;
            }
        }
        for (const std::string& d : displaced) {
            Rest(s, d, DecisionState::Automatic, "displaced-by-manual");
        }
        for (auto it = s.assignments.begin(); it != s.assignments.end();) {
            it = it->target == a.target ? s.assignments.erase(it) : it + 1;
        }
        s.assignments.push_back(std::move(a));
    }

    void ApplyCorrections() {
        _plan.corrections = _in.corrections;
        for (const PlanCorrection& c : _in.corrections) {
            if (c.action != PlanCorrection::Action::Lock) {
                continue;
            }
            for (PlanSegment& s : _plan.segments) {
                if (!InRange(s, c)) {
                    continue;
                }
                std::vector<PlanAssignment> kept;
                for (PlanAssignment a : c.assignments) {
                    const std::string refusal = Refusal(a.target, a.prominence, a.intent);
                    if (!refusal.empty()) {
                        _plan.conflicts.push_back({ a.target, c.startMS, c.endMS, refusal });
                        continue;
                    }
                    a.state = DecisionState::Locked;
                    kept.push_back(std::move(a));
                }
                s.assignments = std::move(kept);
                std::vector<int> chosen;
                for (const PlanAssignment& a : s.assignments) {
                    if (a.prominence != Prominence::Rest && _caps.IndexOf(a.target) >= 0) {
                        chosen.push_back(_caps.IndexOf(a.target));
                    }
                }
                s.assignments.erase(std::remove_if(s.assignments.begin(), s.assignments.end(), [](const PlanAssignment& a) { return a.prominence == Prominence::Rest; }), s.assignments.end());
                FillRests(s, chosen, "locked-segment");
                for (PlanAssignment& a : s.assignments) {
                    if (a.prominence == Prominence::Rest) {
                        a.state = DecisionState::Locked;
                    }
                }
                s.state = DecisionState::Locked;
                s.reasons.push_back({ "locked-by-user", "" });
            }
        }
        for (const PlanCorrection& c : _in.corrections) {
            if (c.action == PlanCorrection::Action::Lock) {
                continue;
            }
            for (PlanSegment& s : _plan.segments) {
                if (!InRange(s, c)) {
                    continue;
                }
                if (c.action == PlanCorrection::Action::MoveBoundary) {
                    if (s.state == DecisionState::Automatic) {
                        s.state = DecisionState::ManualEdit;
                    }
                    continue;
                }
                if (s.state == DecisionState::Locked) {
                    _plan.conflicts.push_back({ c.target, c.startMS, c.endMS, "inside-locked-segment" });
                    continue;
                }
                switch (c.action) {
                case PlanCorrection::Action::Exclude:
                    Rest(s, c.target, DecisionState::ManualEdit, "excluded-by-user");
                    break;
                case PlanCorrection::Action::Reject: {
                    const auto it = std::find_if(s.assignments.begin(), s.assignments.end(), [&](const PlanAssignment& a) { return a.target == c.target && a.prominence != Prominence::Rest && a.state == DecisionState::Automatic; });
                    if (it != s.assignments.end()) {
                        Rest(s, c.target, DecisionState::ManualEdit, "rejected-by-user");
                    }
                    break;
                }
                case PlanCorrection::Action::Include:
                case PlanCorrection::Action::SetProminence: {
                    const TargetInfo* t = _caps.Find(c.target);
                    const std::string refusal = Refusal(c.target, c.prominence, "feature");
                    if (!refusal.empty() || t == nullptr) {
                        _plan.conflicts.push_back({ c.target, c.startMS, c.endMS, refusal.empty() ? "target-missing" : refusal });
                        break;
                    }
                    if (c.prominence == Prominence::Rest) {
                        Rest(s, c.target, DecisionState::ManualEdit, "set-by-user");
                        break;
                    }
                    Candidate cand;
                    cand.target = _caps.IndexOf(c.target);
                    cand.role = t->PrimaryRole() != nullptr ? t->PrimaryRole()->role : Role::Prop;
                    cand.confidence = 1;
                    const char* intent = c.prominence == Prominence::Lead ? "feature" : c.prominence == Prominence::Accent ? "beat-accent" : "accompany";
                    PlanAssignment a = Make(cand, c.prominence, intent, s.targetIntensity);
                    a.state = DecisionState::ManualEdit;
                    a.reasons.push_back({ "set-by-user", "" });
                    Put(s, std::move(a), c.startMS, c.endMS);
                    break;
                }
                case PlanCorrection::Action::SetDensity: {
                    s.assignments.erase(std::remove_if(s.assignments.begin(), s.assignments.end(), [](const PlanAssignment& a) {
                                            return a.state == DecisionState::Automatic && (a.prominence == Prominence::Support || a.prominence == Prominence::Rest);
                                        }),
                                        s.assignments.end());
                    std::vector<int> chosen;
                    int lit = 0;
                    for (const PlanAssignment& a : s.assignments) {
                        const int i = _caps.IndexOf(a.target);
                        if (i >= 0 && a.prominence != Prominence::Rest) {
                            chosen.push_back(i);
                            for (uint64_t w : _caps.modelBits[i]) {
                                lit += static_cast<int>(std::popcount(w));
                            }
                        }
                    }
                    s.assignments.erase(std::remove_if(s.assignments.begin(), s.assignments.end(), [](const PlanAssignment& a) { return a.prominence == Prominence::Rest; }), s.assignments.end());
                    FillSupports(s, BandOf(s), Key(s), chosen, lit, std::clamp(c.density, 0.0, 1.0));
                    FillRests(s, chosen, "density-budget");
                    s.state = DecisionState::ManualEdit;
                    s.reasons.push_back({ "density-set-by-user", fmt::format("{:.2f}", c.density) });
                    break;
                }
                case PlanCorrection::Action::Accept:
                    for (PlanAssignment& a : s.assignments) {
                        if (a.state == DecisionState::Automatic) {
                            a.state = DecisionState::ApprovedPreference;
                        }
                    }
                    s.state = DecisionState::ApprovedPreference;
                    break;
                default:
                    break;
                }
                std::sort(s.assignments.begin(), s.assignments.end(), [](const PlanAssignment& a, const PlanAssignment& b) { return a.target < b.target; });
            }
        }
    }

    void Finish() {
        std::map<std::string, int> leadTime;
        int sung = 0;
        for (const PlanSegment& s : _plan.segments) {
            if (std::find(s.flags.begin(), s.flags.end(), "silence") == s.flags.end()) {
                sung += s.endMS - s.startMS;
            }
            for (const PlanAssignment& a : s.assignments) {
                if (a.prominence == Prominence::Lead && a.intent != "face-sing") {
                    leadTime[a.target] += s.endMS - s.startMS;
                }
            }
        }
        for (const auto& [target, ms] : leadTime) {
            if (sung > 0 && static_cast<double>(ms) / sung > _profile.maxLeadShare) {
                _plan.warnings.push_back(fmt::format("lead-share-high:{}", target));
            }
        }
        std::sort(_plan.warnings.begin(), _plan.warnings.end());
    }

    const PlannerInput& _in;
    const SongAnalysis& _a;
    const ModelCapabilities& _caps;
    const StyleProfile& _profile;
    const LevelSettings& _level;
    CompositionPlan& _plan;
    int _frame;
    std::vector<int> _models;
    std::vector<Candidate> _candidates;
    std::vector<Candidate> _faces;
    std::vector<Candidate> _texts;
    std::vector<Span> _phrases;
};

json ReasonsJson(const std::vector<Reason>& reasons) {
    json out = json::array();
    for (const Reason& r : reasons) {
        out.push_back(r.detail.empty() ? json{ { "code", r.code } } : json{ { "code", r.code }, { "detail", r.detail } });
    }
    return out;
}

json AssignmentJson(const PlanAssignment& a) {
    return { { "target", a.target }, { "type", TargetTypeId(a.type) }, { "role", RoleId(a.role) }, { "prominence", ProminenceId(a.prominence) }, { "intent", a.intent }, { "intensity", a.intensity },
             { "confidence", a.confidence }, { "state", DecisionStateId(a.state) }, { "reasons", ReasonsJson(a.reasons) } };
}

template <typename E, size_t N>
bool EnumFrom(const char* const (&ids)[N], const std::string& id, E& out) {
    for (size_t i = 0; i < N; ++i) {
        if (id == ids[i]) {
            out = static_cast<E>(i);
            return true;
        }
    }
    return false;
}

std::vector<Reason> ReasonsFrom(const json& j) {
    std::vector<Reason> out;
    for (const json& r : j) {
        out.push_back({ r.at("code").get<std::string>(), r.value("detail", std::string()) });
    }
    return out;
}

PlanAssignment AssignmentFrom(const json& j) {
    PlanAssignment a;
    a.target = j.at("target").get<std::string>();
    const std::string type = j.value("type", std::string("model"));
    a.type = type == "group" ? TargetType::Group : type == "submodel" ? TargetType::SubModel : TargetType::Model;
    (void)RoleFromId(j.value("role", std::string("prop")), a.role);
    (void)EnumFrom(PROMINENCE_IDS, j.value("prominence", std::string("rest")), a.prominence);
    a.intent = j.value("intent", std::string());
    a.intensity = j.value("intensity", 0.0);
    a.confidence = j.value("confidence", 0.0);
    (void)EnumFrom(DECISION_IDS, j.value("state", std::string("automatic")), a.state);
    a.reasons = ReasonsFrom(j.value("reasons", json::array()));
    return a;
}

json RoleMap(const std::map<Role, double>& m) {
    json out = json::object();
    for (const auto& [role, w] : m) {
        out[RoleId(role)] = w;
    }
    return out;
}

json LevelJson(const LevelSettings& l) {
    return { { "densityScale", l.densityScale }, { "variation", l.variation }, { "leadPool", l.leadPool }, { "maxLeads", l.maxLeads }, { "minRoleConfidence", l.minRoleConfidence },
             { "accents", l.accents }, { "maxFaces", l.maxFaces }, { "rotateWithinSection", l.rotateWithinSection } };
}

LevelSettings LevelFrom(const json& j, const LevelSettings& fallback) {
    LevelSettings l = fallback;
    l.densityScale = j.value("densityScale", l.densityScale);
    l.variation = j.value("variation", l.variation);
    l.leadPool = std::max(1, j.value("leadPool", l.leadPool));
    l.maxLeads = std::max(1, j.value("maxLeads", l.maxLeads));
    l.minRoleConfidence = j.value("minRoleConfidence", l.minRoleConfidence);
    l.accents = std::max(0, j.value("accents", l.accents));
    l.maxFaces = std::max(0, j.value("maxFaces", l.maxFaces));
    l.rotateWithinSection = j.value("rotateWithinSection", l.rotateWithinSection);
    return l;
}

} // namespace

const LevelSettings& StyleProfile::Level(AutomationLevel level) const {
    return level == AutomationLevel::Conservative ? conservative : level == AutomationLevel::Creative ? creative : balanced;
}

StyleProfile NeutralStarterProfile() {
    StyleProfile p;
    p.leadPreference = { { Role::MegaTree, 1.0 }, { Role::Matrix, 1.0 }, { Role::Group, 0.8 }, { Role::SmallTree, 0.6 }, { Role::Figure, 0.6 }, { Role::Window, 0.5 },
                         { Role::Arch, 0.5 }, { Role::Star, 0.5 }, { Role::Line, 0.4 }, { Role::Prop, 0.4 }, { Role::Face, 0.3 } };
    p.supportPreference = { { Role::Group, 0.9 }, { Role::Arch, 0.8 }, { Role::Window, 0.8 }, { Role::Line, 0.8 }, { Role::SmallTree, 0.7 }, { Role::Star, 0.7 },
                            { Role::Prop, 0.6 }, { Role::Figure, 0.6 }, { Role::Matrix, 0.5 }, { Role::MegaTree, 0.5 }, { Role::Face, 0.4 } };
    p.conservative = { 0.6, 0.0, 2, 1, 0.6, 0, 1, false };
    p.balanced = { 1.0, 0.35, 3, 2, 0.4, 2, 2, true };
    p.creative = { 1.35, 0.8, 5, 3, 0.3, 4, 4, true };
    return p;
}

std::string ToJson(const StyleProfile& p) {
    json root;
    root["id"] = p.id;
    root["name"] = p.name;
    root["learned"] = p.learned;
    root["version"] = p.version;
    root["note"] = "Starting parameters for a first plan, not a learned style. Replace them with a profile made from approved shows.";
    root["segments"] = { { "minMS", p.minSegmentMS }, { "maxMS", p.maxSegmentMS }, { "maxChangesPerMinute", p.maxChangesPerMinute }, { "minRestMS", p.minRestMS } };
    root["density"] = { { "low", p.densityLow }, { "mid", p.densityMid }, { "high", p.densityHigh }, { "climax", p.densityClimax }, { "maxParticipants", p.maxParticipants } };
    root["energy"] = { { "low", p.lowEnergy }, { "high", p.highEnergy } };
    root["maxLeadShare"] = p.maxLeadShare;
    root["groupsMayOverlapMembers"] = p.groupsMayOverlapMembers;
    root["leadPreference"] = RoleMap(p.leadPreference);
    root["supportPreference"] = RoleMap(p.supportPreference);
    root["templateWeights"] = p.templateWeights;
    root["levels"] = { { "conservative", LevelJson(p.conservative) }, { "balanced", LevelJson(p.balanced) }, { "creative", LevelJson(p.creative) } };
    return root.dump(1);
}

bool FromJson(const std::string& text, StyleProfile& profile, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    try {
        StyleProfile p = NeutralStarterProfile();
        p.id = root.value("id", p.id);
        p.name = root.value("name", p.name);
        p.learned = root.value("learned", false);
        p.version = root.value("version", 1);
        const json seg = root.value("segments", json::object());
        p.minSegmentMS = seg.value("minMS", p.minSegmentMS);
        p.maxSegmentMS = std::max(p.minSegmentMS * 2, seg.value("maxMS", p.maxSegmentMS));
        p.maxChangesPerMinute = seg.value("maxChangesPerMinute", p.maxChangesPerMinute);
        p.minRestMS = seg.value("minRestMS", p.minRestMS);
        const json den = root.value("density", json::object());
        p.densityLow = den.value("low", p.densityLow);
        p.densityMid = den.value("mid", p.densityMid);
        p.densityHigh = den.value("high", p.densityHigh);
        p.densityClimax = den.value("climax", p.densityClimax);
        p.maxParticipants = den.value("maxParticipants", p.maxParticipants);
        const json en = root.value("energy", json::object());
        p.lowEnergy = en.value("low", p.lowEnergy);
        p.highEnergy = en.value("high", p.highEnergy);
        p.maxLeadShare = root.value("maxLeadShare", p.maxLeadShare);
        p.groupsMayOverlapMembers = root.value("groupsMayOverlapMembers", p.groupsMayOverlapMembers);
        for (const auto& [key, field] : { std::pair<const char*, std::map<Role, double>*>{ "leadPreference", &p.leadPreference }, { "supportPreference", &p.supportPreference } }) {
            if (root.contains(key)) {
                field->clear();
                for (const auto& [id, w] : root.at(key).items()) {
                    Role role;
                    if (!RoleFromId(id, role)) {
                        error = fmt::format("unknown role '{}' in {}", id, key);
                        return false;
                    }
                    (*field)[role] = w.get<double>();
                }
            }
        }
        if (root.contains("templateWeights")) {
            p.templateWeights = root.at("templateWeights").get<std::map<std::string, double>>();
        }
        const json levels = root.value("levels", json::object());
        p.conservative = LevelFrom(levels.value("conservative", json::object()), p.conservative);
        p.balanced = LevelFrom(levels.value("balanced", json::object()), p.balanced);
        p.creative = LevelFrom(levels.value("creative", json::object()), p.creative);
        profile = std::move(p);
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::string HashText(const std::string& text) {
    return fmt::format("{:016x}", Fnv(text));
}

CompositionPlan PlanComposition(const PlannerInput& in) {
    CompositionPlan plan;
    const StyleProfile neutral = NeutralStarterProfile();
    const StyleProfile& profile = in.profile != nullptr ? *in.profile : neutral;
    plan.seed = in.options.seed;
    plan.level = in.options.level;
    plan.profileId = profile.id;
    plan.profileHash = HashText(ToJson(profile));
    if (in.analysis == nullptr || in.capabilities == nullptr) {
        plan.warnings.push_back("missing-required-input");
        return plan;
    }
    plan.analysisHash = HashText(ToJson(*in.analysis));
    plan.layoutHash = in.capabilities->layoutFingerprint;
    if (in.lyrics != nullptr && !in.lyrics->Empty()) {
        std::string lyrics;
        for (const LyricLine* line : in.lyrics->Lines()) {
            lyrics += line->id + "\n";
        }
        plan.lyricsHash = HashText(lyrics + (in.alignment != nullptr ? ToJson(*in.alignment) : std::string()));
    }
    if (profile.learned == false && profile.id != "neutral-starter") {
        plan.warnings.push_back("profile-not-learned");
    }
    Planner planner(in, profile, plan);
    planner.Run();
    return plan;
}

const char* ProminenceId(Prominence p) {
    return PROMINENCE_IDS[static_cast<int>(p)];
}

const char* DecisionStateId(DecisionState s) {
    return DECISION_IDS[static_cast<int>(s)];
}

const char* LevelId(AutomationLevel level) {
    return LEVEL_IDS[static_cast<int>(level)];
}

const char* CorrectionActionId(PlanCorrection::Action action) {
    return ACTION_IDS[static_cast<int>(action)];
}

std::string ToJson(const CompositionPlan& plan) {
    json root;
    root["schemaVersion"] = plan.schemaVersion;
    root["plannerVersion"] = plan.plannerVersion;
    root["analysisHash"] = plan.analysisHash;
    root["lyricsHash"] = plan.lyricsHash;
    root["layoutHash"] = plan.layoutHash;
    root["profileHash"] = plan.profileHash;
    root["profileId"] = plan.profileId;
    root["seed"] = plan.seed;
    root["level"] = LevelId(plan.level);
    root["durationMS"] = plan.durationMS;
    root["lyricsSynchronized"] = plan.lyricsSynchronized;
    root["warnings"] = plan.warnings;
    json segments = json::array();
    for (const PlanSegment& s : plan.segments) {
        json assignments = json::array();
        for (const PlanAssignment& a : s.assignments) {
            assignments.push_back(AssignmentJson(a));
        }
        segments.push_back({ { "id", s.id }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "sectionIndex", s.sectionIndex }, { "sectionKind", s.sectionKind }, { "repeatGroup", s.repeatGroup },
                             { "occurrence", s.occurrence }, { "energy", s.energy }, { "targetIntensity", s.targetIntensity }, { "density", s.density }, { "priority", s.priority },
                             { "flags", s.flags }, { "vocalCoverage", s.vocalCoverage }, { "state", DecisionStateId(s.state) }, { "reasons", ReasonsJson(s.reasons) }, { "assignments", assignments } });
    }
    root["segments"] = std::move(segments);
    json corrections = json::array();
    for (const PlanCorrection& c : plan.corrections) {
        json assignments = json::array();
        for (const PlanAssignment& a : c.assignments) {
            assignments.push_back(AssignmentJson(a));
        }
        corrections.push_back({ { "action", CorrectionActionId(c.action) }, { "startMS", c.startMS }, { "endMS", c.endMS }, { "target", c.target }, { "prominence", ProminenceId(c.prominence) },
                                { "density", c.density }, { "assignments", assignments } });
    }
    root["corrections"] = std::move(corrections);
    json conflicts = json::array();
    for (const PlanConflict& c : plan.conflicts) {
        conflicts.push_back({ { "target", c.target }, { "startMS", c.startMS }, { "endMS", c.endMS }, { "reason", c.reason } });
    }
    root["conflicts"] = std::move(conflicts);
    return root.dump(1);
}

bool FromJson(const std::string& text, CompositionPlan& plan, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    try {
        CompositionPlan p;
        p.schemaVersion = root.at("schemaVersion").get<int>();
        if (p.schemaVersion > PLAN_SCHEMA_VERSION) {
            error = "written by a newer version";
            return false;
        }
        p.plannerVersion = root.value("plannerVersion", std::string());
        p.analysisHash = root.value("analysisHash", std::string());
        p.lyricsHash = root.value("lyricsHash", std::string());
        p.layoutHash = root.value("layoutHash", std::string());
        p.profileHash = root.value("profileHash", std::string());
        p.profileId = root.value("profileId", std::string());
        p.seed = root.value("seed", static_cast<uint64_t>(0));
        (void)EnumFrom(LEVEL_IDS, root.value("level", std::string("balanced")), p.level);
        p.durationMS = root.value("durationMS", 0);
        p.lyricsSynchronized = root.value("lyricsSynchronized", false);
        p.warnings = root.value("warnings", std::vector<std::string>());
        for (const json& s : root.value("segments", json::array())) {
            PlanSegment seg;
            seg.id = s.at("id").get<std::string>();
            seg.startMS = s.at("startMS").get<int>();
            seg.endMS = s.at("endMS").get<int>();
            seg.sectionIndex = s.value("sectionIndex", -1);
            seg.sectionKind = s.value("sectionKind", std::string());
            seg.repeatGroup = s.value("repeatGroup", -1);
            seg.occurrence = s.value("occurrence", 0);
            seg.energy = s.value("energy", 0.0);
            seg.targetIntensity = s.value("targetIntensity", 0.0);
            seg.density = s.value("density", 0.0);
            seg.priority = s.value("priority", 0);
            seg.flags = s.value("flags", std::vector<std::string>());
            seg.vocalCoverage = s.value("vocalCoverage", 0.0);
            (void)EnumFrom(DECISION_IDS, s.value("state", std::string("automatic")), seg.state);
            seg.reasons = ReasonsFrom(s.value("reasons", json::array()));
            for (const json& a : s.value("assignments", json::array())) {
                seg.assignments.push_back(AssignmentFrom(a));
            }
            p.segments.push_back(std::move(seg));
        }
        for (const json& c : root.value("corrections", json::array())) {
            PlanCorrection corr;
            (void)EnumFrom(ACTION_IDS, c.at("action").get<std::string>(), corr.action);
            corr.startMS = c.value("startMS", -1);
            corr.endMS = c.value("endMS", -1);
            corr.target = c.value("target", std::string());
            (void)EnumFrom(PROMINENCE_IDS, c.value("prominence", std::string("support")), corr.prominence);
            corr.density = c.value("density", -1.0);
            for (const json& a : c.value("assignments", json::array())) {
                corr.assignments.push_back(AssignmentFrom(a));
            }
            p.corrections.push_back(std::move(corr));
        }
        for (const json& c : root.value("conflicts", json::array())) {
            p.conflicts.push_back({ c.value("target", std::string()), c.value("startMS", -1), c.value("endMS", -1), c.value("reason", std::string()) });
        }
        plan = std::move(p);
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::string PlanReport(const CompositionPlan& plan) {
    const auto clock = [](int ms) { return fmt::format("{:02}:{:02}.{:01}", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 100); };
    std::string out = fmt::format("Composition plan {} ({}), level {}, seed {}, profile {}\n", plan.plannerVersion, plan.schemaVersion, LevelId(plan.level), plan.seed, plan.profileId);
    out += fmt::format("analysis {} layout {} lyrics {} profile {}\n", plan.analysisHash, plan.layoutHash, plan.lyricsHash.empty() ? "-" : plan.lyricsHash, plan.profileHash);
    out += fmt::format("{} segments over {}; lyrics {}\n", plan.segments.size(), clock(plan.durationMS), plan.lyricsSynchronized ? "synchronized" : "not synchronized");
    for (const std::string& w : plan.warnings) {
        out += "warning: " + w + "\n";
    }
    for (const PlanConflict& c : plan.conflicts) {
        out += fmt::format("conflict: {} {}-{} {}\n", c.target, c.startMS, c.endMS, c.reason);
    }
    const auto reasons = [](const std::vector<Reason>& rs) {
        std::string s;
        for (const Reason& r : rs) {
            s += (s.empty() ? "" : ", ") + r.code + (r.detail.empty() ? "" : " " + r.detail);
        }
        return s;
    };
    for (const PlanSegment& s : plan.segments) {
        std::string flags;
        for (const std::string& f : s.flags) {
            flags += " " + f;
        }
        out += fmt::format("\n{} - {}  {}  section {} ({}){}  energy {:.2f} intensity {:.2f} density {:.2f} priority {} [{}]\n  because: {}\n", clock(s.startMS), clock(s.endMS), s.id, s.sectionIndex, s.sectionKind, flags,
                           s.energy, s.targetIntensity, s.density, s.priority, DecisionStateId(s.state), reasons(s.reasons));
        int resting = 0;
        for (const PlanAssignment& a : s.assignments) {
            if (a.prominence == Prominence::Rest) {
                ++resting;
                continue;
            }
            out += fmt::format("  {:<8} {:<34} {:<11} {:<12} {:.2f}  {}  because: {}\n", ProminenceId(a.prominence), a.target, RoleId(a.role), a.intent, a.intensity, a.state == DecisionState::Automatic ? "" : DecisionStateId(a.state), reasons(a.reasons));
        }
        out += fmt::format("  rest     {} models\n", resting);
    }
    return out;
}

} // namespace composer
