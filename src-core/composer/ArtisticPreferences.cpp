#include "composer/ArtisticPreferences.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace composer {

namespace {

constexpr double PRIOR = 2.0;   // values start shy: two agreeing decisions give 0.5
constexpr double DECAY = 0.85;  // per review session
constexpr double FLOOR = 0.25;  // an old decision never weighs less than this

fs::path P(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string ScopeOf(const std::string& key) {
    if (key.rfind("screen.", 0) == 0) {
        return "screen";
    }
    if (key.rfind("scene.", 0) == 0) {
        return "scene";
    }
    if (key.rfind("profile.", 0) == 0) {
        return "profile";
    }
    if (key.rfind("file.", 0) == 0) {
        return "file";
    }
    return "global";
}

bool Moving(const std::string& framing) {
    return framing == "pan-x" || framing == "pan-y" || framing == "zoom-in" || framing == "zoom-out" || framing == "tree-turn";
}

bool Dark(const MediaItem& m) {
    const MediaAnalysis& a = m.analysis;
    const bool issue = std::any_of(a.issues.begin(), a.issues.end(), [](const std::string& i) { return i == "very-dark" || i == "dark-spans"; });
    return issue || (a.Done() && (a.dark >= 0.5 || a.brightness < 0.22));
}

bool Has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

} // namespace

std::vector<Preference> ComputePreferences(const ArtisticPreferences& prefs) {
    std::map<std::string, Preference> byKey;
    std::map<std::string, double> sum;
    std::map<std::string, std::set<std::string>> reviewsOf;
    for (const PreferenceSignal& s : prefs.signals) {
        Preference& p = byKey[s.key];
        p.key = s.key;
        const int age = std::max(0, prefs.session - s.session);
        const double r = std::max(FLOOR, std::pow(DECAY, age));
        const double w = std::clamp(s.weight, 0.0, 1.0) * r;
        sum[s.key] += std::clamp(s.sign, -1.0, 1.0) * w;
        p.support += w;
        ++p.decisions;
        if (std::find(p.origins.begin(), p.origins.end(), s.origin) == p.origins.end()) {
            p.origins.push_back(s.origin);
        }
        reviewsOf[s.key].insert(s.origin.substr(0, s.origin.find('|')));
    }
    for (const auto& [key, o] : prefs.overrides) {
        byKey[key].key = key;
    }
    std::vector<Preference> out;
    for (auto& [key, p] : byKey) {
        p.scope = ScopeOf(key);
        p.learned = p.support > 0 ? sum[key] / (PRIOR + p.support) : 0;
        p.learned = std::round(p.learned * 1000) / 1000;
        p.support = std::round(p.support * 1000) / 1000;
        const auto o = prefs.overrides.find(key);
        p.enabled = o == prefs.overrides.end() || !o->second.disabled;
        if (o != prefs.overrides.end() && o->second.hasValue) {
            p.source = "person";
            p.value = p.enabled ? std::clamp(o->second.value, -1.0, 1.0) : 0;
            p.active = p.enabled;
        } else {
            p.reviews = static_cast<int>(reviewsOf[key].size());
            const bool backed = p.support >= PREFERENCE_MIN_SUPPORT && p.reviews >= PREFERENCE_MIN_REVIEWS;
            p.source = backed ? "learned" : "learned-not-enough";
            p.active = p.enabled && backed && prefs.learning;
            p.value = p.active ? p.learned : 0;
        }
        std::reverse(p.origins.begin(), p.origins.end());
        out.push_back(std::move(p));
    }
    return out;
}

std::map<std::string, double> EffectivePreferences(const ArtisticPreferences& prefs) {
    std::map<std::string, double> out;
    for (const Preference& p : ComputePreferences(prefs)) {
        if (p.active && std::fabs(p.value) > 1e-6) {
            out[p.key] = p.value;
        }
    }
    return out;
}

int LearnFromReview(ArtisticPreferences& prefs, const VisualReview& review, const MediaLibrary* library, const std::string& profileId) {
    std::vector<PreferenceSignal> fresh;
    // A session is a review, not an edit of it: a review learned before keeps its session,
    // so changing a decision there does not make the others look older.
    int session = prefs.session + 1;
    bool known = false;
    for (const PreferenceSignal& s : prefs.signals) {
        if (s.origin.rfind(review.sequence + "|", 0) == 0) {
            session = s.session;
            known = true;
        }
    }
    const auto add = [&](const std::string& origin, const std::string& key, double sign, double weight, const std::string& what) {
        fresh.push_back({ key, sign, weight, session, origin, what });
    };
    for (const ClipReview& c : review.clips) {
        if (c.state == ClipState::Pending) {
            continue; // no decision is not a rejection
        }
        const std::string origin = review.sequence + "|" + c.decisionId;
        const bool yes = c.state == ClipState::Approved;
        const double s = yes ? 1.0 : -1.0;
        const std::string what = yes ? "approved" : c.state == ClipState::Rejected ? "rejected" : "needs-fix";
        const MediaItem* m = library != nullptr ? library->Find(c.mediaId) : nullptr;
        const bool video = c.kind == "video";
        add(origin, "kind.video", video ? s : -s, 0.5, what);
        if (!profileId.empty()) {
            add(origin, "profile." + profileId + ".kind.video", video ? s : -s, 0.5, what);
        }
        if (m != nullptr && Dark(*m)) {
            add(origin, "light.dark", s, 1.0, what);
        }
        const std::string conceptId = !c.seen.empty() ? c.seen.front() : std::string();
        // A whole rejection says little about why: the reasons a person gives say more.
        const bool reasons = !c.flags.empty();
        if (!conceptId.empty() && (yes || !reasons)) {
            add(origin, "concept." + conceptId, s, 0.5, what);
        }
        if (yes) {
            // A file approved on a screen is wanted there: a general taste never takes away
            // what the person kept approving.
            add(origin, "file." + c.mediaId + "@" + c.lane, 1, 1.0, what);
            add(origin, "screen." + c.lane + ".presence", 1, 0.25, what);
            if (!c.transition.empty()) {
                add(origin, "transition." + c.transition, 1, 0.5, what);
            }
            if (Moving(c.framing)) {
                add(origin, "motion.moving", 1, 0.5, what);
                add(origin, "scene." + c.sceneRole + ".motion.moving", 1, 0.5, what);
                if (!profileId.empty()) {
                    add(origin, "profile." + profileId + ".motion.moving", 1, 0.5, what);
                }
            }
        } else {
            // This file on this screen: one rejection alone does not act (it needs two).
            add(origin, "file." + c.mediaId + "@" + c.lane, -1, 1.0, what);
        }
        for (const std::string& f : c.flags) {
            if (f == "wrong-motion" && Moving(c.framing)) {
                add(origin, "motion.moving", -1, 1.0, f);
                add(origin, "scene." + c.sceneRole + ".motion.moving", -1, 1.0, f);
            } else if (f == "wrong-transition" && !c.transition.empty()) {
                add(origin, "transition." + c.transition, -1, 1.0, f);
            } else if (f == "repeats") {
                add(origin, "repetition", -1, 1.0, f);
            } else if (f == "wrong-screen") {
                add(origin, "screen." + c.lane + ".presence", -1, 0.5, f);
                if (!conceptId.empty()) {
                    add(origin, "screen." + c.lane + ".concept." + conceptId, -1, 1.0, f);
                }
            } else if ((f == "not-music" || f == "not-scene") && !conceptId.empty() && !c.sceneRole.empty()) {
                add(origin, "scene." + c.sceneRole + ".concept." + conceptId, -1, 1.0, f);
            }
        }
    }
    // What this review said before is replaced by what it says now.
    std::vector<PreferenceSignal> before;
    for (const PreferenceSignal& s : prefs.signals) {
        if (s.origin.rfind(review.sequence + "|", 0) == 0) {
            before.push_back(s);
        }
    }
    const auto same = [](const std::vector<PreferenceSignal>& a, const std::vector<PreferenceSignal>& b) {
        if (a.size() != b.size()) {
            return false;
        }
        for (size_t i = 0; i < a.size(); ++i) {
            if (a[i].key != b[i].key || a[i].sign != b[i].sign || a[i].weight != b[i].weight || a[i].origin != b[i].origin || a[i].what != b[i].what) {
                return false;
            }
        }
        return true;
    };
    if (same(before, fresh)) {
        return 0;
    }
    // A decision that did not change keeps its session: only new ones are recent.
    for (PreferenceSignal& f : fresh) {
        for (const PreferenceSignal& b : before) {
            if (b.key == f.key && b.origin == f.origin && b.sign == f.sign && b.what == f.what) {
                f.session = b.session;
            }
        }
    }
    std::erase_if(prefs.signals, [&](const PreferenceSignal& s) { return s.origin.rfind(review.sequence + "|", 0) == 0; });
    prefs.signals.insert(prefs.signals.end(), fresh.begin(), fresh.end());
    prefs.session = known ? prefs.session : session;
    int changed = 0;
    for (const PreferenceSignal& f : fresh) {
        changed += std::none_of(before.begin(), before.end(), [&](const PreferenceSignal& b) { return b.key == f.key && b.origin == f.origin && b.sign == f.sign; }) ? 1 : 0;
    }
    for (const PreferenceSignal& b : before) {
        changed += std::none_of(fresh.begin(), fresh.end(), [&](const PreferenceSignal& f) { return b.key == f.key && b.origin == f.origin && b.sign == f.sign; }) ? 1 : 0;
    }
    return std::max(1, changed);
}

int ForgetReview(ArtisticPreferences& prefs, const std::string& sequence) {
    const size_t before = prefs.signals.size();
    std::erase_if(prefs.signals, [&](const PreferenceSignal& s) { return s.origin.rfind(sequence + "|", 0) == 0; });
    return static_cast<int>(before - prefs.signals.size());
}

void SetPreference(ArtisticPreferences& prefs, const std::string& key, double value) {
    PreferenceOverride& o = prefs.overrides[key];
    o.hasValue = true;
    o.value = std::clamp(value, -1.0, 1.0);
}

void EnablePreference(ArtisticPreferences& prefs, const std::string& key, bool enabled) {
    PreferenceOverride& o = prefs.overrides[key];
    o.disabled = !enabled;
    if (!o.disabled && !o.hasValue) {
        prefs.overrides.erase(key);
    }
}

void ResetPreference(ArtisticPreferences& prefs, const std::string& key) {
    std::erase_if(prefs.signals, [&](const PreferenceSignal& s) { return s.key == key; });
    prefs.overrides.erase(key);
}

void ResetAllPreferences(ArtisticPreferences& prefs) {
    const bool learning = prefs.learning;
    prefs = ArtisticPreferences();
    prefs.learning = learning;
}

std::string PreferencesPath(const std::string& showDir) {
    const std::u8string u = (P(showDir) / "ShowComposer" / "preferences.json").u8string();
    return std::string(u.begin(), u.end());
}

json PreferencesJson(const ArtisticPreferences& p) {
    json signals = json::array();
    for (const PreferenceSignal& s : p.signals) {
        signals.push_back({ { "key", s.key }, { "sign", s.sign }, { "weight", s.weight }, { "session", s.session }, { "origin", s.origin }, { "what", s.what } });
    }
    json overrides = json::object();
    for (const auto& [k, o] : p.overrides) {
        json j = { { "disabled", o.disabled } };
        if (o.hasValue) {
            j["value"] = o.value;
        }
        overrides[k] = std::move(j);
    }
    return { { "format", "xjuanjilights-preferences" }, { "version", p.version }, { "learning", p.learning }, { "session", p.session }, { "signals", std::move(signals) },
             { "overrides", std::move(overrides) } };
}

bool PreferencesFrom(const json& j, ArtisticPreferences& p, std::string& error) {
    p = ArtisticPreferences();
    if (!j.is_object() || j.value("format", std::string()) != "xjuanjilights-preferences") {
        error = "preferences: not a preferences file";
        return false;
    }
    p.version = j.value("version", 1);
    if (p.version > PREFERENCES_VERSION) {
        error = "preferences: newer";
        return false;
    }
    p.learning = j.value("learning", true);
    p.session = j.value("session", 0);
    for (const json& s : j.value("signals", json::array())) {
        if (!s.is_object() || s.value("key", std::string()).empty()) {
            continue;
        }
        p.signals.push_back({ s.value("key", std::string()), std::clamp(s.value("sign", 0.0), -1.0, 1.0), std::clamp(s.value("weight", 1.0), 0.0, 1.0), s.value("session", 0),
                              s.value("origin", std::string()), s.value("what", std::string()) });
    }
    const json o = j.value("overrides", json::object());
    if (o.is_object()) {
        for (const auto& [k, v] : o.items()) {
            if (!v.is_object()) {
                continue;
            }
            PreferenceOverride x;
            x.disabled = v.value("disabled", false);
            x.hasValue = v.contains("value") && v["value"].is_number();
            x.value = x.hasValue ? std::clamp(v["value"].get<double>(), -1.0, 1.0) : 0;
            p.overrides[k] = x;
        }
    }
    p.version = PREFERENCES_VERSION;
    return true;
}

bool ReadPreferences(const std::string& path, ArtisticPreferences& p, std::string& error) {
    p = ArtisticPreferences();
    std::error_code ec;
    if (!fs::exists(P(path), ec)) {
        return true;
    }
    std::ifstream in(P(path), std::ios::binary);
    const json j = json::parse(in, nullptr, false);
    if (j.is_discarded()) {
        error = "preferences: damaged";
        return false;
    }
    return PreferencesFrom(j, p, error);
}

bool SavePreferences(const std::string& path, const ArtisticPreferences& p, std::string& error) {
    std::error_code ec;
    fs::create_directories(P(path).parent_path(), ec);
    const fs::path tmp = P(path + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << PreferencesJson(p).dump(1);
        if (!out) {
            error = "preferences: write";
            return false;
        }
    }
    fs::rename(tmp, P(path), ec);
    if (ec) {
        error = "preferences: rename";
        return false;
    }
    return true;
}

} // namespace composer
