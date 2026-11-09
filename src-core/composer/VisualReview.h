#pragma once

#include "composer/MediaDirection.h"
#include "composer/MediaLibrary.h"
#include "composer/MediaStoryboard.h"

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <string>
#include <vector>

// A person's review of a generated show: each picture and video shot with what the
// program decided and why, what the person thinks of it, and whether they watched the
// sequence in xLights. A render that runs proves nothing about how a show looks: only a
// person approves, after saying what they checked. The review is kept in the sidecar
// and carried to the next version of the show by each shot's stable decision id.
namespace composer {

enum class ClipState { Pending, Approved, Rejected, NeedsFix };

[[nodiscard]] const char* ClipStateId(ClipState state);
[[nodiscard]] bool ClipStateFromId(const std::string& id, ClipState& state);

// Why a shot does not work, in a person's words: "not-music" (does not fit the music),
// "not-scene", "repeats", "wrong-screen", "wrong-motion", "wrong-transition".
[[nodiscard]] const std::vector<std::string>& ClipFlags();

struct ClipReview {
    std::string decisionId; // stable across versions
    std::string shotId;     // in this version's storyboard
    std::string lane;
    std::string sceneId;
    std::string sceneRole;
    int startMS = 0;
    int endMS = 0;
    std::string mediaId;
    std::string name;
    std::string kind;       // "image", "video", "animation"
    std::string source;     // the original, as kept
    std::string framing;    // the movement or framing applied
    std::string transition; // how it comes in
    std::string transitionIn;
    int fadeInMS = 0;
    std::string transitionOut;
    int fadeOutMS = 0;
    std::vector<std::string> seen; // the concepts it shows, strongest first
    double confidence = -1;        // the strongest concept's score; -1 with no model
    double score = 0;
    std::string why;               // the reasons' codes, in short
    // What the person thinks.
    ClipState state = ClipState::Pending;
    std::vector<std::string> flags;
    std::string note;
    std::string replacementId;   // another file to put there instead
    std::string replacementPath;
    bool neverUse = false;       // the person wants the file out of every automatic choice
    std::string decidedAt;
};

// What a person says they did before approving: all five, or no approval.
struct ReviewChecklist {
    bool opened = false;      // opened the sequence in xLights
    bool composition = false; // looked at the visual composition
    bool surfaces = false;    // looked at the main screens
    bool motion = false;      // checked the transitions and the movements
    bool finished = false;    // finished the review

    [[nodiscard]] bool Complete() const { return opened && composition && surfaces && motion && finished; }
};

// Version 2 added the evaluation of the whole show; version 1 files read as before.
inline constexpr int VISUAL_REVIEW_VERSION = 2;

// What a person thinks of the whole show, beside the clip by clip review. Only a person
// fills it in; the measures are kept with it so the two can be compared, never mixed.
// Criteria: musicality, variety, complexity, coordination, colour, direction,
// continuity, personality, multimedia. Each 1-5; absent when not rated.
struct ShowEvaluation {
    std::map<std::string, int> ratings;
    int overall = 0;    // 0-100, the person's own score (the "60%"); 0: not given
    bool quick = false; // only the score and a few words
    std::vector<std::string> tags; // "monotonous", "too-busy", "too-calm", "too-christmas", "not-christmas", "liked"
    std::string notes;
    // The observations the person chose to turn into rules for the next shows.
    std::vector<std::string> rules; // "more-variety", "less-variety", "more-christmas", "more-song-colours", "keep-recipes"
    std::string reviewer;
    std::string at;
    std::map<std::string, double> measured; // the objective measures of the same show, for comparison

    [[nodiscard]] bool Empty() const { return ratings.empty() && overall == 0 && tags.empty() && notes.empty() && rules.empty(); }
};

[[nodiscard]] const std::vector<std::string>& EvaluationCriteria();
[[nodiscard]] const std::vector<std::string>& EvaluationTags();
[[nodiscard]] const std::vector<std::string>& EvaluationRules();
// The rules the ratings and words suggest; the person picks which ones to keep.
[[nodiscard]] std::vector<std::string> SuggestRules(const ShowEvaluation& evaluation);

struct VisualReview {
    int version = VISUAL_REVIEW_VERSION;
    std::string sequence;
    std::string specHash;       // what was reviewed: an approval only holds for it
    std::string storyboardHash;
    std::vector<ClipReview> clips;
    ReviewChecklist checklist;
    std::string reviewer;
    // "pending" (nothing checked), "in-progress" (some clips or steps done), "approved"
    // (only by ApproveShow, a person's explicit act).
    std::string state = "pending";
    std::string approvedAt;
    bool legacy = false; // approved before reviews had clips (a Phase 8 record)
    ShowEvaluation evaluation;

    [[nodiscard]] bool Empty() const { return clips.empty() && !checklist.opened && state == "pending" && evaluation.Empty(); }
    [[nodiscard]] const ClipReview* Find(const std::string& decisionId) const;
    [[nodiscard]] ClipReview* Find(const std::string& decisionId);
};

struct ReviewCounts {
    int clips = 0;
    int pending = 0;
    int approved = 0;
    int rejected = 0;
    int needsFix = 0;
};

[[nodiscard]] ReviewCounts CountReview(const VisualReview& review);

// The review of a new storyboard: each shot's clip, with what the person said about the
// same decision in `previous` when it still shows the same file. An approval of the
// whole show is only kept when the show is the same (`specHash`).
// `library` (may be null) gives what each file shows and how sure the model was.
[[nodiscard]] VisualReview BuildReview(const VisualMediaStoryboard& storyboard, const std::string& sequence, const std::string& specHash, const VisualReview* previous,
                                       const MediaLibrary* library = nullptr);

// Sets a clip's state, flags and note; marks the review in progress. False when the clip
// is not in the review.
bool DecideClip(VisualReview& review, const std::string& decisionId, ClipState state, const std::vector<std::string>& flags, const std::string& note, const std::string& at);
bool ReplaceClipMedia(VisualReview& review, const std::string& decisionId, const std::string& mediaId, const std::string& path, const std::string& at);

// The whole show approved by a person: only when every step of the checklist is ticked,
// a reviewer is named, and no clip is still rejected or waiting for a fix. Otherwise it
// says why and changes nothing.
bool ApproveShow(VisualReview& review, const std::string& reviewer, const std::string& at, std::string& why);

// The corrections that apply the review without making the rest again: approved and
// pending shots kept exactly as they are, a replaced shot locked with its new file, a
// rejected shot's file avoided there (the planner chooses another), and nothing else.
[[nodiscard]] std::vector<MediaCorrection> ReviewCorrections(const VisualReview& review, const VisualMediaStoryboard& storyboard);
// Drops the "keep as it was" locks a review left, so the montage can be planned again.
void ReleaseReviewHolds(std::vector<MediaCorrection>& corrections);

// The person's evaluation of the whole show, checked: a reviewer, ratings 1-5 of known
// criteria, a score 0-100, known words and rules. Otherwise it says why and changes nothing.
bool EvaluateShow(VisualReview& review, const ShowEvaluation& evaluation, std::string& why);

[[nodiscard]] nlohmann::json VisualReviewJson(const VisualReview& review);
[[nodiscard]] bool VisualReviewFrom(const nlohmann::json& object, VisualReview& review, std::string& error);
// A Phase 8 "<sequence>.composer-review.json" record, read as an approved review.
[[nodiscard]] bool LegacyReviewFrom(const nlohmann::json& object, VisualReview& review);

} // namespace composer
