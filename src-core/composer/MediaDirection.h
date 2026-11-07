#pragma once

#include "composer/CompositionPlan.h"
#include "composer/MediaLibrary.h"
#include "composer/TemplateLibrary.h"

#include <string>
#include <utility>
#include <vector>

// Which picture or video plays where: a few sections of the song get one of the show's
// own files on a matrix or a mega tree, chosen by the scene's theme, the file's shape
// against the figure's, the energy and what was already shown. Words on a figure win
// over a picture there, and a face singing keeps its figure.
//
// Everything chosen says why; nothing is marked as looking good, which only a person
// can say after watching it.
namespace composer {

inline constexpr const char* MEDIA_DIRECTOR_VERSION = "1.0.0";

struct MediaCorrection {
    enum class Action {
        Assign, // `mediaId` (or `path`) plays on `target` in the segment: a person's choice
        Lock,   // as Assign, and kept exactly so when the song is generated again
        Remove  // no picture or video in the segment (on `target` only, if given)
    };
    Action action = Action::Assign;
    std::string segmentId;
    int startMS = -1; // where the segment started, to find it when ids change
    std::string target;
    std::string mediaId;
    std::string path; // as kept: relative to the show folder when inside it
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
    std::vector<std::string> constraints; // "cropped", "letterboxed", "outside-show", "loops"
};

struct MediaOptions {
    bool automatic = true; // false: only what a person assigned
    double maxShare = -1;  // of the song's time; below 0: by level
    int minSegmentMS = 4000;
    int maxTargets = 2; // the largest matrices and mega trees
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

    [[nodiscard]] const MediaAssignment* Find(const std::string& segmentId, const std::string& target) const;
};

struct ShowVisualDirection;
struct LyricAlignment;

struct MediaPlanInput {
    const CompositionPlan* plan = nullptr;           // required
    const ModelCapabilities* capabilities = nullptr; // required
    const MediaLibrary* library = nullptr;           // null or empty: no media
    const MetadataLookup* metadata = nullptr;        // required to write settings
    const ShowVisualDirection* direction = nullptr;  // the scenes and the theme, if any
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
};

struct MediaSession {
    std::vector<std::string> folders;
    bool automatic = true;
    double maxShare = -1;
    std::vector<MediaCorrection> corrections;
    std::vector<MediaUse> used;

    [[nodiscard]] bool Empty() const { return folders.empty() && automatic && maxShare < 0 && corrections.empty() && used.empty(); }
};

// A later choice for the same segment and figure replaces the earlier one; removing a
// segment's media replaces every choice in it.
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
