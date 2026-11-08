#pragma once

#include "composer/CompositionPlan.h"
#include "composer/MediaLibrary.h"
#include "composer/MediaStoryboard.h"
#include "composer/MediaSurfaces.h"
#include "composer/TemplateLibrary.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

// Which picture or video plays where. The show's main screens, the largest matrix and
// the mega tree, are planned together as a visual storyboard: scene by scene, with the
// files that suit the theme, the scene's part in the song, its energy and each figure's
// shape, cut into shots on the downbeats, framed for each figure and changing with the
// music. Words on a figure win over a picture there, and a face singing keeps its figure.
//
// Everything chosen says why; nothing is marked as looking good, which only a person
// can say after watching it.
namespace composer {

inline constexpr const char* MEDIA_DIRECTOR_VERSION = "3.0.0";

// One effect of a shot that needs several: a video on both faces of a mega tree is
// drawn by two or three Video effects, each on a sub-buffer of the tree's columns with
// the part of the frame that face shows, all starting together so they stay in step.
struct MediaPiece {
    std::string subBuffer; // B_CUSTOM_SubBuffer: "left x bottom x right x top", percentages
    std::vector<std::pair<std::string, std::string>> settings; // replace the shot's ones
};

struct MediaCorrection {
    enum class Action {
        Assign, // `mediaId` (or `path`) plays on `target` in the segment: a person's choice
        Lock,   // as Assign, and kept exactly so when the song is generated again
        Remove, // no picture or video in the segment (on `target` only, if given)
        Avoid   // not `mediaId` on `target` in the segment: the planner chooses something else
    };
    Action action = Action::Assign;
    std::string segmentId;
    int startMS = -1; // where the segment started, to find it when ids change
    std::string target;
    std::string mediaId;
    std::string path; // as kept: relative to the show folder when inside it
    // One shot inside the segment rather than the whole of it (a version brought back,
    // a scene locked as it was); -1: the whole segment.
    int shotStartMS = -1;
    int shotEndMS = -1;
    int fragmentStartMS = -1; // videos: where in the file; -1: chosen by the planner
    // The shot's stable decision ("<screen>/<scene key>/<n>"): finds the segment again
    // when the song's segments move or are renamed.
    std::string decisionId;
    // "person" (said in the window or a file) or "review" (kept as it was while a review
    // changed other shots; "Rebuild the Montage" lets those go).
    std::string origin;
};

struct MediaAssignment {
    std::string segmentId;
    std::string target;
    int startMS = 0;
    int endMS = 0;
    std::string mediaId;
    std::string path; // as written in the effect: relative to the show when inside it
    std::string name;
    MediaKind kind = MediaKind::Image;
    std::string effect; // "Pictures" or "Video"
    std::vector<std::pair<std::string, std::string>> settings; // E_ keys, checked against the metadata
    DecisionState state = DecisionState::Automatic;
    double score = 0;
    std::vector<Reason> reasons;
    std::vector<std::string> constraints; // "cropped", "letterboxed", "outside-show", "loops", "derived", "pans", ...
    std::string shotId;
    std::string decisionId; // stable: "<screen>/<scene key>/<n>"
    std::string source; // the original file, as kept; `path` may be a derived picture
    std::string framing;
    int fragmentStartMS = 0;
    std::string transitionIn; // "" is a cut
    int fadeInMS = 0;
    std::string transitionOut;
    int fadeOutMS = 0;
    std::string level; // "very-suitable", "acceptable", "doubtful"
    int layer = 0;
    int overlapMS = 0;    // included in endMS: the cross-fade under the next shot
    bool overlay = false; // between the phrases of a screen showing words
    std::string transition;
    std::vector<MediaPiece> pieces; // empty: one effect
};

struct MediaOptions {
    bool automatic = true; // false: only what a person assigned
    double maxShare = -1;  // of the song's time on any one figure; below 0: by level
    int minSegmentMS = 3000;
    int minShotMS = 2000;
    int maxTargets = 2;     // the main matrix and the mega tree
    int variant = 0;        // another montage from the same files ("rebuild")
    bool writeDerived = true; // make the derived pictures (only tests turn it off)
};

struct MediaPlan {
    std::string version = MEDIA_DIRECTOR_VERSION;
    bool enabled = false;
    std::string themeId;
    std::vector<std::string> themeCategories;
    int usableItems = 0;
    double maxShare = 0;
    double share = 0; // of the song's time with a picture or a video
    std::vector<MediaAssignment> assignments; // by start
    std::vector<std::string> warnings;
    std::vector<Reason> reasons;
    VisualMediaStoryboard storyboard;
    SurfacePlan surfaces;

    // The first shot of the segment on the target.
    [[nodiscard]] const MediaAssignment* Find(const std::string& segmentId, const std::string& target) const;
    [[nodiscard]] std::vector<const MediaAssignment*> FindAll(const std::string& segmentId, const std::string& target) const;
};

struct ShowVisualDirection;
struct LyricAlignment;
struct SongAnalysis;

struct MediaPlanInput {
    const CompositionPlan* plan = nullptr;           // required
    const ModelCapabilities* capabilities = nullptr; // required
    const MediaLibrary* library = nullptr;           // null or empty: no media
    const MetadataLookup* metadata = nullptr;        // required to write settings
    const ShowVisualDirection* direction = nullptr;  // the scenes and the theme, if any
    const SongAnalysis* analysis = nullptr;          // downbeats to cut shots on
    const LyricAlignment* alignment = nullptr;       // the phrases a screen with words must keep
    const ScreenSettings* screens = nullptr;         // a person's screens and lyrics mode
    // The show maker's preferences (EffectivePreferences: key -> -1..1); null: none.
    const std::map<std::string, double>* preferences = nullptr;
    std::string profileId; // for the preferences of one artistic profile
    std::vector<std::string> themeCategories;        // the theme's media categories, best first
    std::string showDir;
    MediaOptions options;
    std::vector<MediaCorrection> corrections;
};

[[nodiscard]] MediaPlan PlanMedia(const MediaPlanInput& input);

// The plan's figure in each chosen segment takes the "media" intent, as the lead.
void ApplyMediaPlan(CompositionPlan& plan, const MediaPlan& media, const ModelCapabilities& capabilities);

// What the sidecar keeps of the media: the folders, a person's choices and what plays
// where, every path relative to the show folder when inside it.
struct MediaUse {
    std::string segmentId;
    std::string target;
    int startMS = 0;
    int endMS = 0;
    std::string mediaId;
    std::string path;
    std::string effect;
    DecisionState state = DecisionState::Automatic;
    std::string source;       // the original, when `path` is a derived picture
    int fragmentStartMS = -1; // videos
    std::string framing;
};

struct MediaSession {
    std::vector<std::string> folders;
    bool automatic = true;
    double maxShare = -1;
    int variant = 0;
    std::vector<MediaCorrection> corrections;
    std::vector<MediaUse> used;
    std::string storyboardHash;
    ScreenSettings screens;
    std::vector<StoryboardVersion> history; // earlier storyboards, newest last, at most five

    [[nodiscard]] bool Empty() const { return folders.empty() && automatic && maxShare < 0 && variant == 0 && corrections.empty() && used.empty() && history.empty() && screens.Empty(); }
};

inline constexpr size_t STORYBOARD_HISTORY = 5;

// The corrections that bring back an earlier storyboard exactly: each of its shots
// locked as it was, and nothing on a main figure where it had nothing.
[[nodiscard]] std::vector<MediaCorrection> RestoreStoryboard(const StoryboardVersion& version, const VisualMediaStoryboard& current);
// The corrections that keep one scene exactly as it is now.
[[nodiscard]] std::vector<MediaCorrection> LockStoryboardScene(const VisualMediaStoryboard& storyboard, const std::string& sceneId);
// The corrections that keep one screen exactly as it is now, through the whole song;
// the other screens are still chosen again when the montage is rebuilt.
[[nodiscard]] std::vector<MediaCorrection> LockStoryboardLane(const VisualMediaStoryboard& storyboard, const std::string& lane);

// A later choice for the same segment, figure and shot replaces the earlier one; a choice
// for the whole segment replaces its shots; removing a segment's media replaces every
// choice in it.
void UpsertMediaCorrection(std::vector<MediaCorrection>& corrections, const MediaCorrection& correction);

[[nodiscard]] nlohmann::json MediaCorrectionsJson(const std::vector<MediaCorrection>& corrections);
[[nodiscard]] bool MediaCorrectionsFrom(const nlohmann::json& list, std::vector<MediaCorrection>& corrections, std::string& error);
[[nodiscard]] nlohmann::json MediaSessionJson(const MediaSession& session);
[[nodiscard]] bool MediaSessionFrom(const nlohmann::json& object, MediaSession& session, std::string& error);

[[nodiscard]] const char* MediaActionId(MediaCorrection::Action action);
[[nodiscard]] bool MediaActionFromId(const std::string& id, MediaCorrection::Action& action);
[[nodiscard]] std::string ToJson(const MediaPlan& plan);
[[nodiscard]] std::string MediaReport(const MediaPlan& plan);

} // namespace composer
