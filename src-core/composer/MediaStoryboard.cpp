#include "composer/MediaStoryboard.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

using nlohmann::json;

namespace composer {

namespace {

uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

json ReasonsJson(const std::vector<Reason>& reasons) {
    json out = json::array();
    for (const Reason& r : reasons) {
        out.push_back({ { "code", r.code }, { "detail", r.detail } });
    }
    return out;
}

std::vector<Reason> ReasonsFrom(const json& list) {
    std::vector<Reason> out;
    for (const json& r : list) {
        out.push_back({ r.value("code", std::string()), r.value("detail", std::string()) });
    }
    return out;
}

DecisionState StateFrom(const std::string& id) {
    for (DecisionState s : { DecisionState::Automatic, DecisionState::ManualEdit, DecisionState::ApprovedPreference, DecisionState::Locked }) {
        if (id == DecisionStateId(s)) {
            return s;
        }
    }
    return DecisionState::Automatic;
}

std::string Clock(int ms) {
    return fmt::format("{}:{:02}.{:01}", ms / 60000, ms / 1000 % 60, ms / 100 % 10);
}

} // namespace

const StoryboardScene* VisualMediaStoryboard::Scene(const std::string& id) const {
    const auto it = std::find_if(scenes.begin(), scenes.end(), [&](const StoryboardScene& s) { return s.id == id; });
    return it != scenes.end() ? &*it : nullptr;
}

const char* MediaShotLevel(double score) {
    return score >= 0.7 ? "very-suitable" : score >= 0.55 ? "acceptable" : "doubtful";
}

void ComputeStoryboardStats(VisualMediaStoryboard& b, int durationMS) {
    StoryboardStats s;
    std::set<std::string> files;
    std::map<std::string, std::string> firstScene; // media -> scene it was first used in
    std::map<std::string, std::string> lastOnLane;  // lane -> media of its previous shot
    double sum = 0;
    for (const StoryboardLane& l : b.lanes) {
        StoryboardLaneStats ls;
        ls.lane = l.target;
        for (const StoryboardShot& sh : b.shots) {
            if (sh.lane == l.target) {
                ++ls.shots;
                ls.pictureMS += sh.endMS - sh.startMS;
            }
        }
        ls.coverage = durationMS > 0 ? std::round(static_cast<double>(ls.pictureMS) / durationMS * 1000.0) / 1000.0 : 0;
        const double free = l.freeShare * durationMS;
        ls.ofFree = free > 0 ? std::round(std::min(1.0, ls.pictureMS / free) * 1000.0) / 1000.0 : 0;
        s.lanes.push_back(ls);
    }
    std::vector<const StoryboardShot*> ordered;
    for (const StoryboardShot& sh : b.shots) {
        ordered.push_back(&sh);
    }
    std::stable_sort(ordered.begin(), ordered.end(), [](const StoryboardShot* x, const StoryboardShot* y) { return x->startMS != y->startMS ? x->startMS < y->startMS : x->lane < y->lane; });
    for (const StoryboardShot* sh : ordered) {
        ++s.shots;
        s.images += sh->kind == MediaKind::Image ? 1 : 0;
        s.animations += sh->kind == MediaKind::Animation ? 1 : 0;
        s.videos += sh->kind == MediaKind::Video ? 1 : 0;
        s.derived += std::find(sh->constraints.begin(), sh->constraints.end(), "derived") != sh->constraints.end() ? 1 : 0;
        files.insert(sh->mediaId);
        const auto first = firstScene.find(sh->mediaId);
        if (first == firstScene.end()) {
            firstScene[sh->mediaId] = sh->sceneId;
        } else if (first->second != sh->sceneId) {
            ++s.repeats;
            const StoryboardScene* scene = b.Scene(sh->sceneId);
            const StoryboardScene* was = b.Scene(first->second);
            if (scene != nullptr && was != nullptr && !scene->lookKey.empty() && scene->lookKey == was->lookKey) {
                ++s.repeatsByMusic;
            }
        }
        const auto last = lastOnLane.find(sh->lane);
        if (last != lastOnLane.end() && last->second == sh->mediaId) {
            const StoryboardScene* scene = b.Scene(sh->sceneId);
            // Two shots of one video are two fragments; a repeat only counts across scenes.
            if (scene != nullptr && sh->fragmentScene < 0) {
                ++s.consecutiveRepeats;
            }
        }
        lastOnLane[sh->lane] = sh->mediaId;
        const std::string level = sh->level;
        s.veryGood += level == "very-suitable" ? 1 : 0;
        s.acceptable += level == "acceptable" ? 1 : 0;
        s.doubtful += level == "doubtful" ? 1 : 0;
        sum += sh->score;
    }
    s.distinctFiles = static_cast<int>(files.size());
    s.gaps = static_cast<int>(b.gaps.size());
    s.missing = static_cast<int>(std::count_if(b.warnings.begin(), b.warnings.end(), [](const std::string& w) { return w.rfind("media-missing", 0) == 0; }));
    s.meanScore = s.shots > 0 ? std::round(sum / s.shots * 1000.0) / 1000.0 : 0;
    b.stats = s;
}

json StoryboardJson(const VisualMediaStoryboard& b) {
    json lanes = json::array();
    for (const StoryboardLane& l : b.lanes) {
        lanes.push_back({ { "target", l.target }, { "role", l.role }, { "buffer", { l.bufferWi, l.bufferHt } }, { "faceAspect", l.faceAspect }, { "degrees", l.degrees }, { "freeShare", l.freeShare },
                          { "goal", l.goal }, { "assumptions", l.assumptions } });
    }
    json scenes = json::array();
    for (const StoryboardScene& s : b.scenes) {
        scenes.push_back({ { "id", s.id }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "section", s.sectionKind }, { "band", s.band }, { "role", s.role }, { "lookKey", s.lookKey },
                           { "repeatOf", s.repeatOf }, { "energy", s.energy }, { "vocals", s.vocals } });
    }
    json shots = json::array();
    for (const StoryboardShot& s : b.shots) {
        json j = { { "id", s.id }, { "scene", s.sceneId }, { "lane", s.lane }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "media", s.mediaId }, { "name", s.name },
                   { "kind", MediaKindId(s.kind) }, { "effect", s.effect }, { "path", s.path }, { "source", s.source }, { "framing", s.framing }, { "transitionIn", s.transitionIn },
                   { "fadeInMS", s.fadeInMS }, { "transitionOut", s.transitionOut }, { "fadeOutMS", s.fadeOutMS }, { "score", s.score }, { "level", s.level },
                   { "state", DecisionStateId(s.state) }, { "reasons", ReasonsJson(s.reasons) }, { "constraints", s.constraints } };
        if (s.kind == MediaKind::Video) {
            j["fragment"] = { { "startMS", s.fragmentStartMS }, { "scene", s.fragmentScene } };
        }
        shots.push_back(std::move(j));
    }
    json gaps = json::array();
    for (const StoryboardGap& g : b.gaps) {
        gaps.push_back({ { "lane", g.lane }, { "scene", g.sceneId }, { "startMS", g.startMS }, { "endMS", g.endMS }, { "reason", g.reason } });
    }
    json discarded = json::array();
    for (const StoryboardDiscard& d : b.discarded) {
        discarded.push_back({ { "media", d.mediaId }, { "name", d.name }, { "reason", d.reason }, { "best", d.best } });
    }
    json lanesStats = json::array();
    for (const StoryboardLaneStats& l : b.stats.lanes) {
        lanesStats.push_back({ { "lane", l.lane }, { "shots", l.shots }, { "pictureMS", l.pictureMS }, { "coverage", l.coverage }, { "ofFree", l.ofFree } });
    }
    const StoryboardStats& st = b.stats;
    json stats = { { "lanes", std::move(lanesStats) }, { "shots", st.shots }, { "images", st.images }, { "animations", st.animations }, { "videos", st.videos },
                   { "distinctFiles", st.distinctFiles }, { "repeats", st.repeats }, { "repeatsByMusic", st.repeatsByMusic }, { "consecutiveRepeats", st.consecutiveRepeats },
                   { "derived", st.derived }, { "gaps", st.gaps }, { "missing", st.missing }, { "verySuitable", st.veryGood }, { "acceptable", st.acceptable },
                   { "doubtful", st.doubtful }, { "meanScore", st.meanScore } };
    return { { "format", "xjuanjilights-storyboard" }, { "version", b.version }, { "seed", b.seed }, { "variant", b.variant }, { "level", b.level }, { "theme", b.themeId },
             { "analysis", b.analysisMode }, { "lanes", std::move(lanes) }, { "scenes", std::move(scenes) }, { "shots", std::move(shots) }, { "gaps", std::move(gaps) },
             { "discarded", std::move(discarded) }, { "stats", std::move(stats) }, { "warnings", b.warnings }, { "hash", b.hash } };
}

bool StoryboardFrom(const json& j, VisualMediaStoryboard& b, std::string& error) {
    b = VisualMediaStoryboard();
    if (!j.is_object() || j.value("format", std::string()) != "xjuanjilights-storyboard") {
        error = "storyboard-format";
        return false;
    }
    b.version = j.value("version", std::string());
    b.seed = j.value("seed", uint64_t(0));
    b.variant = j.value("variant", 0);
    b.level = j.value("level", std::string());
    b.themeId = j.value("theme", std::string());
    b.analysisMode = j.value("analysis", std::string());
    for (const json& l : j.value("lanes", json::array())) {
        StoryboardLane lane;
        lane.target = l.value("target", std::string());
        lane.role = l.value("role", std::string());
        if (l.contains("buffer") && l["buffer"].size() == 2) {
            lane.bufferWi = l["buffer"][0].get<int>();
            lane.bufferHt = l["buffer"][1].get<int>();
        }
        lane.faceAspect = l.value("faceAspect", 0.0);
        lane.degrees = l.value("degrees", 0.0);
        lane.freeShare = l.value("freeShare", 0.0);
        lane.goal = l.value("goal", 0.0);
        lane.assumptions = l.value("assumptions", std::vector<std::string>());
        b.lanes.push_back(std::move(lane));
    }
    for (const json& s : j.value("scenes", json::array())) {
        StoryboardScene sc;
        sc.id = s.value("id", std::string());
        sc.startMS = s.value("startMS", 0);
        sc.endMS = s.value("endMS", 0);
        sc.sectionKind = s.value("section", std::string());
        sc.band = s.value("band", std::string());
        sc.role = s.value("role", std::string());
        sc.lookKey = s.value("lookKey", std::string());
        sc.repeatOf = s.value("repeatOf", std::string());
        sc.energy = s.value("energy", 0.0);
        sc.vocals = s.value("vocals", 0.0);
        b.scenes.push_back(std::move(sc));
    }
    for (const json& s : j.value("shots", json::array())) {
        StoryboardShot sh;
        sh.id = s.value("id", std::string());
        sh.sceneId = s.value("scene", std::string());
        sh.lane = s.value("lane", std::string());
        sh.startMS = s.value("startMS", 0);
        sh.endMS = s.value("endMS", 0);
        sh.mediaId = s.value("media", std::string());
        sh.name = s.value("name", std::string());
        sh.kind = MediaKindFromId(s.value("kind", std::string()));
        sh.effect = s.value("effect", std::string());
        sh.path = s.value("path", std::string());
        sh.source = s.value("source", std::string());
        sh.framing = s.value("framing", std::string());
        sh.transitionIn = s.value("transitionIn", std::string());
        sh.fadeInMS = s.value("fadeInMS", 0);
        sh.transitionOut = s.value("transitionOut", std::string());
        sh.fadeOutMS = s.value("fadeOutMS", 0);
        sh.score = s.value("score", 0.0);
        sh.level = s.value("level", std::string());
        sh.state = StateFrom(s.value("state", std::string()));
        sh.reasons = ReasonsFrom(s.value("reasons", json::array()));
        sh.constraints = s.value("constraints", std::vector<std::string>());
        if (s.contains("fragment") && s["fragment"].is_object()) {
            sh.fragmentStartMS = s["fragment"].value("startMS", 0);
            sh.fragmentScene = s["fragment"].value("scene", -1);
        }
        b.shots.push_back(std::move(sh));
    }
    for (const json& g : j.value("gaps", json::array())) {
        b.gaps.push_back({ g.value("lane", std::string()), g.value("scene", std::string()), g.value("startMS", 0), g.value("endMS", 0), g.value("reason", std::string()) });
    }
    for (const json& d : j.value("discarded", json::array())) {
        b.discarded.push_back({ d.value("media", std::string()), d.value("name", std::string()), d.value("reason", std::string()), d.value("best", 0.0) });
    }
    if (j.contains("stats") && j["stats"].is_object()) {
        const json& s = j["stats"];
        for (const json& l : s.value("lanes", json::array())) {
            b.stats.lanes.push_back({ l.value("lane", std::string()), l.value("shots", 0), l.value("pictureMS", 0), l.value("coverage", 0.0), l.value("ofFree", 0.0) });
        }
        b.stats.shots = s.value("shots", 0);
        b.stats.images = s.value("images", 0);
        b.stats.animations = s.value("animations", 0);
        b.stats.videos = s.value("videos", 0);
        b.stats.distinctFiles = s.value("distinctFiles", 0);
        b.stats.repeats = s.value("repeats", 0);
        b.stats.repeatsByMusic = s.value("repeatsByMusic", 0);
        b.stats.consecutiveRepeats = s.value("consecutiveRepeats", 0);
        b.stats.derived = s.value("derived", 0);
        b.stats.gaps = s.value("gaps", 0);
        b.stats.missing = s.value("missing", 0);
        b.stats.veryGood = s.value("verySuitable", 0);
        b.stats.acceptable = s.value("acceptable", 0);
        b.stats.doubtful = s.value("doubtful", 0);
        b.stats.meanScore = s.value("meanScore", 0.0);
    }
    b.warnings = j.value("warnings", std::vector<std::string>());
    b.hash = j.value("hash", std::string());
    return true;
}

std::string StoryboardHash(const VisualMediaStoryboard& b) {
    // What decides the show: the shots and their settings' inputs, not the wording.
    std::string key = fmt::format("{}|{}|{}", b.version, b.seed, b.variant);
    for (const StoryboardShot& s : b.shots) {
        key += fmt::format("|{}:{}:{}-{}:{}:{}:{}:{}:{}{}/{}{}", s.lane, s.sceneId, s.startMS, s.endMS, s.mediaId, s.path, s.fragmentStartMS, s.framing, s.transitionIn, s.fadeInMS, s.transitionOut,
                           s.fadeOutMS);
    }
    return fmt::format("{:016x}", Fnv(key));
}

std::string StoryboardReport(const VisualMediaStoryboard& b) {
    std::string out = fmt::format("Visual storyboard {} (theme {}, level {}, analysis {}, variant {}): {} shots, {} files, {} images, {} animations, {} videos, {} derived; repeats {} ({} by the music, {} back to back); {} gaps; mean score {:.2f} ({} very suitable, {} acceptable, {} doubtful); hash {}\n",
                                  b.version, b.themeId.empty() ? std::string("none") : b.themeId, b.level, b.analysisMode, b.variant, b.stats.shots, b.stats.distinctFiles, b.stats.images, b.stats.animations,
                                  b.stats.videos, b.stats.derived, b.stats.repeats, b.stats.repeatsByMusic, b.stats.consecutiveRepeats, b.stats.gaps, b.stats.meanScore, b.stats.veryGood, b.stats.acceptable,
                                  b.stats.doubtful, b.hash);
    for (const StoryboardLane& l : b.lanes) {
        const auto st = std::find_if(b.stats.lanes.begin(), b.stats.lanes.end(), [&](const StoryboardLaneStats& s) { return s.lane == l.target; });
        out += fmt::format("lane {} ({}, {}x{}, real shape {:.2f}{}): free {:.0f}% of the song, goal {:.0f}% of that, {} shots, {:.0f}% of the song, {:.0f}% of its free time\n", l.target, l.role, l.bufferWi,
                           l.bufferHt, l.faceAspect, l.degrees > 0 ? fmt::format(", {:.0f} degrees", l.degrees) : std::string(), l.freeShare * 100, l.goal * 100,
                           st != b.stats.lanes.end() ? st->shots : 0, st != b.stats.lanes.end() ? st->coverage * 100 : 0.0, st != b.stats.lanes.end() ? st->ofFree * 100 : 0.0);
        for (const std::string& a : l.assumptions) {
            out += "  assumed: " + a + "\n";
        }
    }
    for (const StoryboardScene& s : b.scenes) {
        out += fmt::format("scene {} {}-{} {} ({}, {}, energy {:.2f}{})\n", s.id, Clock(s.startMS), Clock(s.endMS), s.role, s.sectionKind, s.band, s.energy,
                           s.repeatOf.empty() ? std::string() : ", repeats " + s.repeatOf);
        for (const StoryboardShot& sh : b.shots) {
            if (sh.sceneId != s.id) {
                continue;
            }
            out += fmt::format("  {} {}-{}: {} ({} {}{}), {}, in {}{} out {}{}, {:.2f} {} [{}]", sh.lane, Clock(sh.startMS), Clock(sh.endMS), sh.name, sh.effect, sh.framing,
                               sh.kind == MediaKind::Video ? fmt::format(" from {:.1f} s", sh.fragmentStartMS / 1000.0) : std::string(), sh.path, sh.transitionIn.empty() ? "cut" : sh.transitionIn,
                               sh.fadeInMS > 0 ? fmt::format(" {} ms", sh.fadeInMS) : std::string(), sh.transitionOut.empty() ? "cut" : sh.transitionOut,
                               sh.fadeOutMS > 0 ? fmt::format(" {} ms", sh.fadeOutMS) : std::string(), sh.score, sh.level, DecisionStateId(sh.state));
            for (const Reason& r : sh.reasons) {
                out += fmt::format("; {} {}", r.code, r.detail);
            }
            out += "\n";
        }
        for (const StoryboardGap& g : b.gaps) {
            if (g.sceneId == s.id) {
                out += fmt::format("  {} {}-{}: no picture ({})\n", g.lane, Clock(g.startMS), Clock(g.endMS), g.reason);
            }
        }
    }
    for (const StoryboardDiscard& d : b.discarded) {
        out += fmt::format("not used: {} ({}, best {:.2f})\n", d.name, d.reason, d.best);
    }
    for (const std::string& w : b.warnings) {
        out += "warning: " + w + "\n";
    }
    return out;
}

StoryboardVersion StoryboardVersionOf(const VisualMediaStoryboard& b, int number, const std::string& created) {
    StoryboardVersion v;
    v.number = number;
    v.hash = b.hash;
    v.created = created;
    v.variant = b.variant;
    for (const StoryboardShot& s : b.shots) {
        v.shots.push_back({ s.sceneId, s.lane, s.startMS, s.endMS, s.mediaId, s.source, s.fragmentStartMS, s.framing });
    }
    return v;
}

json StoryboardVersionJson(const StoryboardVersion& v) {
    json shots = json::array();
    for (const StoryboardVersion::Shot& s : v.shots) {
        shots.push_back({ { "scene", s.sceneId }, { "lane", s.lane }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "media", s.mediaId }, { "source", s.source },
                          { "fragmentStartMS", s.fragmentStartMS }, { "framing", s.framing } });
    }
    return { { "number", v.number }, { "hash", v.hash }, { "created", v.created }, { "variant", v.variant }, { "shots", std::move(shots) } };
}

StoryboardVersion StoryboardVersionFrom(const json& j) {
    StoryboardVersion v;
    v.number = j.value("number", 0);
    v.hash = j.value("hash", std::string());
    v.created = j.value("created", std::string());
    v.variant = j.value("variant", 0);
    for (const json& s : j.value("shots", json::array())) {
        v.shots.push_back({ s.value("scene", std::string()), s.value("lane", std::string()), s.value("startMS", 0), s.value("endMS", 0), s.value("media", std::string()), s.value("source", std::string()),
                            s.value("fragmentStartMS", 0), s.value("framing", std::string()) });
    }
    return v;
}

} // namespace composer
