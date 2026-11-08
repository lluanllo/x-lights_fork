#pragma once

#include "composer/CompositionPlan.h"
#include "composer/MediaLibrary.h"

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <string>
#include <vector>

// The visual montage of a song on its main screens, the largest matrix and the mega
// tree: which picture or video plays on each, when it starts and ends, which part of a
// video, how it is framed and how it comes in and goes out, and why. Both screens are
// planned together, scene by scene, so they belong to the same scene without showing
// the same thing.
//
// Nothing here is marked as looking good: that is for a person watching it.
namespace composer {

inline constexpr const char* STORYBOARD_VERSION = "1.0.0";

struct StoryboardLane {
    std::string target;
    std::string role; // "matrix" or "mega-tree"
    int bufferWi = 0;
    int bufferHt = 0;
    double faceAspect = 0;
    double degrees = 0;
    double freeShare = 0; // of the song's time the figure is free for pictures
    double goal = 0;      // share of that free time the montage aims to fill
    std::string reason;   // why this figure, and what the others had
    std::vector<std::string> assumptions;
    std::string screen;   // its role as a screen: "main", "secondary", "tree", "media", "lyrics"
    bool gapsOnly = false; // a screen showing words: pictures only between its phrases
};

struct StoryboardScene {
    std::string id; // the plan segment
    int startMS = 0;
    int endMS = 0;
    std::string sectionKind;
    std::string band;
    // "establish", "verse", "chorus", "instrumental", "bridge", "climax", "ending", "breath"
    std::string role;
    std::string lookKey;
    std::string repeatOf; // the scene it repeats, when the music does
    double energy = 0;
    double vocals = 0;
    // Where it sits in the song's form, stable when times move: "chorus#2.1" is the first
    // part of the second chorus.
    std::string key;
};

struct StoryboardShot {
    std::string id; // "<scene>/<lane>/<n>"
    std::string sceneId;
    std::string lane; // target name
    int startMS = 0;
    int endMS = 0;
    std::string mediaId;
    std::string name;
    MediaKind kind = MediaKind::Image;
    std::string effect; // "Pictures" or "Video"
    std::string path;   // what the effect reads: the derived picture, or the file
    std::string source; // the original, as kept (relative to the show when inside)
    int fragmentStartMS = 0; // videos: where in the file it starts
    int fragmentScene = -1;  // videos: which of its scenes
    std::string framing;     // MediaFraming::strategy
    std::string transitionIn;  // "" (a cut) or "Fade"
    int fadeInMS = 0;
    std::string transitionOut;
    int fadeOutMS = 0;
    double score = 0;
    std::string level; // "very-suitable", "acceptable", "doubtful"
    DecisionState state = DecisionState::Automatic;
    std::vector<Reason> reasons;
    std::vector<std::string> constraints;
    int layer = 0;            // the effect layer (two alternate for a cross-fade)
    std::string transition;   // how it comes in: "cut", "fade", "crossfade", "zoom", "wipe", "rest"
    bool overlay = false;     // between the phrases of a screen showing words
    int pieces = 1;           // effects that draw it (a video on both faces of a tree: 2 or 3)
    int overlapMS = 0;        // how far it stays on past its end, fading under the next shot
    std::string relation;     // to the shot before on its screen: "same-ambience", "same-motif", ...
    // The decision, as a person's review and corrections refer to it: "<screen>/<scene
    // key>/<n>", the n-th shot of that screen in that part of the song.
    std::string decisionId;
};

struct StoryboardGap {
    std::string lane;
    std::string sceneId;
    int startMS = 0;
    int endMS = 0;
    std::string reason; // "lyrics-text-here", "face-sings-here", "group-lights-it", "removed", "goal-reached", "no-media-fits", ...
};

// A file that was considered and left out, with why: a person can always ask.
struct StoryboardDiscard {
    std::string mediaId;
    std::string name;
    std::string reason; // "off-theme", "duplicate", "very-dark", "too-small", "disabled", "low-score", ...
    double best = 0;    // its best score anywhere
};

struct StoryboardLaneStats {
    std::string lane;
    int shots = 0;
    int pictureMS = 0;
    double coverage = 0; // of the song
    double ofFree = 0;   // of the figure's free time
};

struct StoryboardStats {
    std::vector<StoryboardLaneStats> lanes;
    int shots = 0;
    int images = 0;
    int animations = 0;
    int videos = 0;
    int distinctFiles = 0;
    int repeats = 0;          // shots reusing a file used in an earlier scene
    int repeatsByMusic = 0;   // of those, in a section the music repeats
    int consecutiveRepeats = 0;
    int derived = 0;
    int gaps = 0;
    int missing = 0;
    int veryGood = 0;
    int acceptable = 0;
    int doubtful = 0;
    double meanScore = 0;
    int meanShotMS = 0;
    std::map<std::string, int> transitions; // kind -> count
    double coherence = 0; // mean relation of each shot to the one before on its screen, 0..1
    double crossScreen = 0; // mean relation between the screens of a scene, 0..1
    int overlays = 0;
    int motionClips = 0;
};

struct VisualMediaStoryboard {
    std::string version = STORYBOARD_VERSION;
    uint64_t seed = 0;
    int variant = 0; // "rebuild the montage" asks for another one
    std::string level;
    std::string themeId;
    std::string analysisMode; // "semantic", "basic", "mixed" or "none"
    std::string lyricsMode;   // how words and pictures share the screens
    std::vector<StoryboardLane> lanes;
    std::vector<StoryboardScene> scenes;
    std::vector<StoryboardShot> shots; // by lane, then start
    std::vector<StoryboardGap> gaps;
    std::vector<StoryboardDiscard> discarded;
    StoryboardStats stats;
    std::vector<std::string> warnings;
    std::string hash; // of what decides the show: same inputs, same hash

    [[nodiscard]] bool Empty() const { return shots.empty(); }
    [[nodiscard]] const StoryboardScene* Scene(const std::string& id) const;
    [[nodiscard]] const StoryboardLane* Lane(const std::string& target) const;
};

// A storyboard as the sidecar keeps an earlier version of it: enough to bring it back.
struct StoryboardVersion {
    int number = 0;
    std::string hash;
    std::string created; // ISO time
    int variant = 0;
    struct Shot {
        std::string sceneId;
        std::string lane;
        int startMS = 0;
        int endMS = 0;
        std::string mediaId;
        std::string source;
        int fragmentStartMS = 0;
        std::string framing;
    };
    std::vector<Shot> shots;
};

[[nodiscard]] const char* MediaShotLevel(double score);
void ComputeStoryboardStats(VisualMediaStoryboard& storyboard, int durationMS);
[[nodiscard]] nlohmann::json StoryboardJson(const VisualMediaStoryboard& storyboard);
bool StoryboardFrom(const nlohmann::json& object, VisualMediaStoryboard& storyboard, std::string& error);
[[nodiscard]] std::string StoryboardHash(const VisualMediaStoryboard& storyboard);
[[nodiscard]] std::string StoryboardReport(const VisualMediaStoryboard& storyboard);
[[nodiscard]] StoryboardVersion StoryboardVersionOf(const VisualMediaStoryboard& storyboard, int number, const std::string& created);
[[nodiscard]] nlohmann::json StoryboardVersionJson(const StoryboardVersion& version);
[[nodiscard]] StoryboardVersion StoryboardVersionFrom(const nlohmann::json& object);

} // namespace composer
