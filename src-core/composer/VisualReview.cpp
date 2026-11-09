#include "composer/VisualReview.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

using nlohmann::json;

namespace composer {

namespace {

bool Has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

// "<ids> (<mode>)" from a shot's "content" reason.
std::vector<std::string> SeenOf(const StoryboardShot& s) {
    std::vector<std::string> out;
    for (const Reason& r : s.reasons) {
        if (r.code != "content") {
            continue;
        }
        const std::string ids = r.detail.substr(0, r.detail.find(" ("));
        size_t at = 0;
        while (at < ids.size()) {
            const size_t end = std::min(ids.find(", ", at), ids.size());
            out.push_back(ids.substr(at, end - at));
            at = end + 2;
        }
    }
    return out;
}

void Touch(VisualReview& r) {
    if (r.state == "pending") {
        r.state = "in-progress";
    }
}

} // namespace

const char* ClipStateId(ClipState s) {
    switch (s) {
    case ClipState::Approved: return "approved";
    case ClipState::Rejected: return "rejected";
    case ClipState::NeedsFix: return "needs-fix";
    default: return "pending";
    }
}

bool ClipStateFromId(const std::string& id, ClipState& s) {
    if (id == "pending") {
        s = ClipState::Pending;
    } else if (id == "approved") {
        s = ClipState::Approved;
    } else if (id == "rejected") {
        s = ClipState::Rejected;
    } else if (id == "needs-fix") {
        s = ClipState::NeedsFix;
    } else {
        return false;
    }
    return true;
}

const std::vector<std::string>& ClipFlags() {
    static const std::vector<std::string> f = { "not-music", "not-scene", "repeats", "wrong-screen", "wrong-motion", "wrong-transition" };
    return f;
}

const ClipReview* VisualReview::Find(const std::string& id) const {
    const auto it = std::find_if(clips.begin(), clips.end(), [&](const ClipReview& c) { return c.decisionId == id; });
    return it != clips.end() ? &*it : nullptr;
}

ClipReview* VisualReview::Find(const std::string& id) {
    const auto it = std::find_if(clips.begin(), clips.end(), [&](const ClipReview& c) { return c.decisionId == id; });
    return it != clips.end() ? &*it : nullptr;
}

ReviewCounts CountReview(const VisualReview& r) {
    ReviewCounts c;
    for (const ClipReview& k : r.clips) {
        ++c.clips;
        c.pending += k.state == ClipState::Pending ? 1 : 0;
        c.approved += k.state == ClipState::Approved ? 1 : 0;
        c.rejected += k.state == ClipState::Rejected ? 1 : 0;
        c.needsFix += k.state == ClipState::NeedsFix ? 1 : 0;
    }
    return c;
}

VisualReview BuildReview(const VisualMediaStoryboard& b, const std::string& sequence, const std::string& specHash, const VisualReview* previous, const MediaLibrary* library) {
    VisualReview r;
    r.sequence = sequence;
    r.specHash = specHash;
    r.storyboardHash = b.hash;
    bool carried = false;
    for (const StoryboardShot& s : b.shots) {
        ClipReview c;
        c.decisionId = s.decisionId.empty() ? s.id : s.decisionId;
        c.shotId = s.id;
        c.lane = s.lane;
        c.sceneId = s.sceneId;
        if (const StoryboardScene* sc = b.Scene(s.sceneId); sc != nullptr) {
            c.sceneRole = sc->role;
        }
        c.startMS = s.startMS;
        c.endMS = s.endMS;
        c.mediaId = s.mediaId;
        c.name = s.name;
        c.kind = MediaKindId(s.kind);
        c.source = s.source;
        c.framing = s.framing;
        c.transition = s.transition;
        c.transitionIn = s.transitionIn;
        c.fadeInMS = s.fadeInMS;
        c.transitionOut = s.transitionOut;
        c.fadeOutMS = s.fadeOutMS;
        c.seen = SeenOf(s);
        c.score = s.score;
        for (const Reason& reason : s.reasons) {
            if (reason.code == "theme-category" || reason.code == "scene-role" || reason.code == "story" || reason.code == "between-phrases" || reason.code == "locked-by-person" ||
                reason.code == "chosen-by-person" || reason.code == "repeat-keeps-media") {
                c.why += (c.why.empty() ? "" : "; ") + reason.code + (reason.detail.empty() ? std::string() : ": " + reason.detail);
            }
        }
        if (library != nullptr) {
            if (const MediaItem* m = library->Find(s.mediaId); m != nullptr && !m->analysis.concepts.empty()) {
                c.confidence = m->analysis.concepts.front().score;
            }
        }
        // What the person said about the same decision, while it shows the same file.
        if (previous != nullptr) {
            if (const ClipReview* old = previous->Find(c.decisionId); old != nullptr) {
                const bool sameFile = old->mediaId == c.mediaId || (!old->replacementId.empty() && old->replacementId == c.mediaId);
                if (sameFile) {
                    c.state = old->state == ClipState::NeedsFix && old->replacementId == c.mediaId ? ClipState::Pending : old->state;
                    c.flags = old->flags;
                    c.note = old->note;
                    c.decidedAt = old->decidedAt;
                    carried = carried || c.state != ClipState::Pending;
                }
            }
        }
        r.clips.push_back(std::move(c));
    }
    std::sort(r.clips.begin(), r.clips.end(), [](const ClipReview& a, const ClipReview& b) { return a.startMS != b.startMS ? a.startMS < b.startMS : a.lane < b.lane; });
    if (previous != nullptr) {
        r.reviewer = previous->reviewer;
        // An approval holds for what was seen: the same show keeps it, another one is
        // reviewed again (its checklist starts over).
        if (previous->state == "approved" && previous->specHash == specHash && !specHash.empty()) {
            r.state = "approved";
            r.checklist = previous->checklist;
            r.approvedAt = previous->approvedAt;
            r.legacy = previous->legacy;
        } else if (carried) {
            r.state = "in-progress";
        }
        // An evaluation is of one show: it stays with that show and is not carried to another.
        if (previous->specHash == specHash && !specHash.empty() && !previous->evaluation.Empty()) {
            r.evaluation = previous->evaluation;
            if (r.state == "pending") {
                r.state = "in-progress";
            }
        }
    }
    return r;
}

bool DecideClip(VisualReview& r, const std::string& id, ClipState state, const std::vector<std::string>& flags, const std::string& note, const std::string& at) {
    ClipReview* c = r.Find(id);
    if (c == nullptr) {
        return false;
    }
    c->state = state;
    c->flags.clear();
    for (const std::string& f : flags) {
        if (Has(ClipFlags(), f) && !Has(c->flags, f)) {
            c->flags.push_back(f);
        }
    }
    if (!note.empty()) {
        c->note = note;
    }
    c->decidedAt = at;
    if (r.state == "approved") {
        // Changing a decision after approving reopens the review.
        r.state = "in-progress";
        r.approvedAt.clear();
    }
    Touch(r);
    return true;
}

bool ReplaceClipMedia(VisualReview& r, const std::string& id, const std::string& mediaId, const std::string& path, const std::string& at) {
    ClipReview* c = r.Find(id);
    if (c == nullptr || mediaId.empty()) {
        return false;
    }
    c->replacementId = mediaId;
    c->replacementPath = path;
    c->state = ClipState::NeedsFix;
    c->decidedAt = at;
    if (r.state == "approved") {
        r.state = "in-progress";
        r.approvedAt.clear();
    }
    Touch(r);
    return true;
}

bool ApproveShow(VisualReview& r, const std::string& reviewer, const std::string& at, std::string& why) {
    if (!r.checklist.Complete()) {
        why = "checklist-incomplete";
        return false;
    }
    if (reviewer.empty()) {
        why = "no-reviewer";
        return false;
    }
    const ReviewCounts n = CountReview(r);
    if (n.rejected > 0 || n.needsFix > 0) {
        why = "corrections-not-applied";
        return false;
    }
    r.state = "approved";
    r.reviewer = reviewer;
    r.approvedAt = at;
    return true;
}

std::vector<MediaCorrection> ReviewCorrections(const VisualReview& r, const VisualMediaStoryboard& b) {
    std::vector<MediaCorrection> out;
    for (const ClipReview& c : r.clips) {
        const StoryboardShot* shot = nullptr;
        for (const StoryboardShot& s : b.shots) {
            shot = shot == nullptr && s.id == c.shotId ? &s : shot;
        }
        const StoryboardScene* scene = b.Scene(c.sceneId);
        if (shot == nullptr || scene == nullptr) {
            continue;
        }
        MediaCorrection m;
        m.segmentId = c.sceneId;
        m.startMS = scene->startMS;
        m.target = c.lane;
        m.shotStartMS = shot->startMS;
        m.shotEndMS = shot->endMS;
        m.decisionId = c.decisionId;
        if (!c.replacementId.empty()) {
            m.action = MediaCorrection::Action::Lock;
            m.mediaId = c.replacementId;
            m.path = c.replacementPath;
            m.origin = "person";
        } else if (c.state == ClipState::Rejected || c.state == ClipState::NeedsFix) {
            // Only this file in this place: the planner chooses another for the shot.
            m.action = MediaCorrection::Action::Avoid;
            m.mediaId = c.mediaId;
            m.shotStartMS = m.shotEndMS = -1;
            m.origin = "person";
        } else {
            m.action = MediaCorrection::Action::Lock;
            m.mediaId = c.mediaId;
            m.path = c.source;
            m.fragmentStartMS = shot->kind == MediaKind::Video ? shot->fragmentStartMS : -1;
            m.origin = c.state == ClipState::Approved ? "person" : "review";
        }
        UpsertMediaCorrection(out, m);
    }
    return out;
}

void ReleaseReviewHolds(std::vector<MediaCorrection>& list) {
    std::erase_if(list, [](const MediaCorrection& c) { return c.origin == "review"; });
}

const std::vector<std::string>& EvaluationCriteria() {
    static const std::vector<std::string> C = { "musicality", "variety", "complexity", "coordination", "colour", "direction", "continuity", "personality", "multimedia" };
    return C;
}

const std::vector<std::string>& EvaluationTags() {
    static const std::vector<std::string> T = { "liked", "monotonous", "too-busy", "too-calm", "too-christmas", "not-christmas" };
    return T;
}

const std::vector<std::string>& EvaluationRules() {
    static const std::vector<std::string> R = { "more-variety", "less-variety", "more-christmas", "more-song-colours", "keep-recipes" };
    return R;
}

std::vector<std::string> SuggestRules(const ShowEvaluation& e) {
    std::vector<std::string> out;
    const auto has = [&](const char* tag) { return std::find(e.tags.begin(), e.tags.end(), tag) != e.tags.end(); };
    const auto rating = [&](const char* c) {
        const auto it = e.ratings.find(c);
        return it == e.ratings.end() ? 0 : it->second;
    };
    const auto add = [&](const char* rule) {
        if (std::find(out.begin(), out.end(), rule) == out.end()) {
            out.push_back(rule);
        }
    };
    if (has("monotonous") || (rating("variety") > 0 && rating("variety") <= 2)) {
        add("more-variety");
    }
    if (has("too-busy") || (rating("continuity") > 0 && rating("continuity") <= 2)) {
        add("less-variety");
    }
    if (has("not-christmas")) {
        add("more-christmas");
    }
    if (has("too-christmas")) {
        add("more-song-colours");
    }
    if (has("liked") || e.overall >= 70 || rating("direction") >= 4) {
        add("keep-recipes");
    }
    return out;
}

bool EvaluateShow(VisualReview& review, const ShowEvaluation& e, std::string& why) {
    const auto known = [](const std::vector<std::string>& list, const std::string& x) { return std::find(list.begin(), list.end(), x) != list.end(); };
    if (e.reviewer.empty()) {
        why = "no-reviewer";
        return false;
    }
    for (const auto& [criterion, value] : e.ratings) {
        if (!known(EvaluationCriteria(), criterion) || value < 1 || value > 5) {
            why = "rating:" + criterion;
            return false;
        }
    }
    if (e.overall < 0 || e.overall > 100) {
        why = "overall";
        return false;
    }
    for (const std::string& t : e.tags) {
        if (!known(EvaluationTags(), t)) {
            why = "tag:" + t;
            return false;
        }
    }
    for (const std::string& r : e.rules) {
        if (!known(EvaluationRules(), r)) {
            why = "rule:" + r;
            return false;
        }
    }
    if ((known(e.rules, "more-variety") && known(e.rules, "less-variety")) || (known(e.rules, "more-christmas") && known(e.rules, "more-song-colours"))) {
        why = "rules-contradict";
        return false;
    }
    review.evaluation = e;
    if (e.quick) {
        review.evaluation.ratings.clear();
    }
    if (review.state == "pending") {
        review.state = "in-progress";
    }
    return true;
}

json VisualReviewJson(const VisualReview& r) {
    json clips = json::array();
    for (const ClipReview& c : r.clips) {
        json j = { { "decision", c.decisionId }, { "shot", c.shotId }, { "lane", c.lane }, { "scene", c.sceneId }, { "sceneRole", c.sceneRole }, { "startMS", c.startMS }, { "endMS", c.endMS },
                   { "media", c.mediaId }, { "name", c.name }, { "kind", c.kind }, { "source", c.source }, { "framing", c.framing }, { "transition", c.transition },
                   { "transitionIn", c.transitionIn }, { "fadeInMS", c.fadeInMS }, { "transitionOut", c.transitionOut }, { "fadeOutMS", c.fadeOutMS }, { "seen", c.seen },
                   { "confidence", c.confidence }, { "score", c.score }, { "why", c.why }, { "state", ClipStateId(c.state) } };
        if (!c.flags.empty()) {
            j["flags"] = c.flags;
        }
        if (!c.note.empty()) {
            j["note"] = c.note;
        }
        if (!c.replacementId.empty()) {
            j["replacement"] = { { "media", c.replacementId }, { "path", c.replacementPath } };
        }
        if (c.neverUse) {
            j["neverUse"] = true;
        }
        if (!c.decidedAt.empty()) {
            j["decidedAt"] = c.decidedAt;
        }
        clips.push_back(std::move(j));
    }
    const ReviewChecklist& k = r.checklist;
    json root = { { "version", r.version }, { "sequence", r.sequence }, { "specHash", r.specHash }, { "storyboard", r.storyboardHash }, { "state", r.state }, { "reviewer", r.reviewer },
                  { "checklist", { { "opened", k.opened }, { "composition", k.composition }, { "surfaces", k.surfaces }, { "motion", k.motion }, { "finished", k.finished } } },
                  { "clips", std::move(clips) } };
    if (!r.approvedAt.empty()) {
        root["approvedAt"] = r.approvedAt;
    }
    if (!r.evaluation.Empty()) {
        const ShowEvaluation& e = r.evaluation;
        root["evaluation"] = { { "ratings", e.ratings }, { "overall", e.overall }, { "quick", e.quick }, { "tags", e.tags }, { "notes", e.notes }, { "rules", e.rules },
                               { "reviewer", e.reviewer }, { "at", e.at }, { "measured", e.measured } };
    }
    if (r.legacy) {
        root["legacy"] = true;
    }
    return root;
}

bool VisualReviewFrom(const json& j, VisualReview& r, std::string& error) {
    r = VisualReview();
    if (!j.is_object()) {
        error = "review: not an object";
        return false;
    }
    r.version = j.value("version", 1);
    if (r.version > VISUAL_REVIEW_VERSION) {
        error = "review: newer";
        return false;
    }
    r.sequence = j.value("sequence", std::string());
    r.specHash = j.value("specHash", std::string());
    r.storyboardHash = j.value("storyboard", std::string());
    r.state = j.value("state", std::string("pending"));
    if (r.state != "pending" && r.state != "in-progress" && r.state != "approved") {
        error = "review: unknown state";
        return false;
    }
    r.reviewer = j.value("reviewer", std::string());
    r.approvedAt = j.value("approvedAt", std::string());
    r.legacy = j.value("legacy", false);
    if (j.contains("evaluation") && j["evaluation"].is_object()) {
        const json& e = j["evaluation"];
        const json ratings = e.value("ratings", json::object());
        for (const auto& [criterion, value] : ratings.items()) {
            if (value.is_number_integer() && value.get<int>() >= 1 && value.get<int>() <= 5) {
                r.evaluation.ratings[criterion] = value.get<int>();
            }
        }
        r.evaluation.overall = std::clamp(e.value("overall", 0), 0, 100);
        r.evaluation.quick = e.value("quick", false);
        r.evaluation.tags = e.value("tags", std::vector<std::string>());
        r.evaluation.notes = e.value("notes", std::string());
        r.evaluation.rules = e.value("rules", std::vector<std::string>());
        r.evaluation.reviewer = e.value("reviewer", std::string());
        r.evaluation.at = e.value("at", std::string());
        r.evaluation.measured = e.value("measured", std::map<std::string, double>());
    }
    // Read as the current version from now on.
    r.version = VISUAL_REVIEW_VERSION;
    const json k = j.value("checklist", json::object());
    if (k.is_object()) {
        r.checklist.opened = k.value("opened", false);
        r.checklist.composition = k.value("composition", false);
        r.checklist.surfaces = k.value("surfaces", false);
        r.checklist.motion = k.value("motion", false);
        r.checklist.finished = k.value("finished", false);
    }
    for (const json& x : j.value("clips", json::array())) {
        if (!x.is_object()) {
            continue;
        }
        ClipReview c;
        c.decisionId = x.value("decision", std::string());
        c.shotId = x.value("shot", std::string());
        c.lane = x.value("lane", std::string());
        c.sceneId = x.value("scene", std::string());
        c.sceneRole = x.value("sceneRole", std::string());
        c.startMS = x.value("startMS", 0);
        c.endMS = x.value("endMS", 0);
        c.mediaId = x.value("media", std::string());
        c.name = x.value("name", std::string());
        c.kind = x.value("kind", std::string());
        c.source = x.value("source", std::string());
        c.framing = x.value("framing", std::string());
        c.transition = x.value("transition", std::string());
        c.transitionIn = x.value("transitionIn", std::string());
        c.fadeInMS = x.value("fadeInMS", 0);
        c.transitionOut = x.value("transitionOut", std::string());
        c.fadeOutMS = x.value("fadeOutMS", 0);
        c.seen = x.value("seen", std::vector<std::string>());
        c.confidence = x.value("confidence", -1.0);
        c.score = x.value("score", 0.0);
        c.why = x.value("why", std::string());
        if (!ClipStateFromId(x.value("state", std::string("pending")), c.state)) {
            error = "review: unknown clip state";
            return false;
        }
        c.flags = x.value("flags", std::vector<std::string>());
        c.note = x.value("note", std::string());
        if (x.contains("replacement") && x["replacement"].is_object()) {
            c.replacementId = x["replacement"].value("media", std::string());
            c.replacementPath = x["replacement"].value("path", std::string());
        }
        c.neverUse = x.value("neverUse", false);
        c.decidedAt = x.value("decidedAt", std::string());
        if (c.decisionId.empty()) {
            error = "review: a clip with no decision";
            return false;
        }
        r.clips.push_back(std::move(c));
    }
    // Approved is only ever a person's act with every step ticked: a file saying
    // otherwise is not trusted.
    if (r.state == "approved" && !r.legacy && (!r.checklist.Complete() || r.reviewer.empty())) {
        r.state = "in-progress";
    }
    return true;
}

bool LegacyReviewFrom(const json& j, VisualReview& r) {
    if (!j.is_object() || j.value("state", std::string()) != "approved" || j.value("reviewedBy", std::string()).empty()) {
        return false;
    }
    r = VisualReview();
    r.sequence = j.value("sequence", std::string());
    r.reviewer = j.value("reviewedBy", std::string());
    r.approvedAt = j.value("reviewedAt", std::string());
    r.state = "approved";
    r.legacy = true;
    return true;
}

} // namespace composer
