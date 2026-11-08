#pragma once

#include "composer/CompositionPlan.h"
#include "composer/ModelCapabilities.h"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <utility>
#include <vector>

// The display's surfaces for pictures and videos: every figure whose lights form a
// picture (matrices, mega trees, dense pixel grids, a matrix's submodels), what each
// can show, and the part it plays in a show: the main screen, a second one, the mega
// tree, the lyrics, a narration, extra media, support for the lights, or nothing.
// The roles are chosen from what the figures are, not from their names, and a person
// can change any of them; the choice is kept with the show.
namespace composer {

enum class SurfaceRole { Main, Secondary, Tree, Lyrics, Narration, Media, Support, Off };

// How the lyrics and the pictures share the screens:
// - LyricsFirst: a screen showing words keeps them for the whole song; pictures go elsewhere.
// - Smart: words appear when they are sung; between phrases, in the opening and the
//   instrumentals, that screen can show pictures too.
// - Free: every role is the person's; a screen showing words still never shows a picture
//   over a phrase.
enum class LyricsMode { LyricsFirst, Smart, Free };

struct ScreenChoice {
    std::string target;
    SurfaceRole role = SurfaceRole::Support;
    bool roleSet = false;
    double frontOffset = -1; // trees: the share of a turn the front column sits at; -1: automatic
    int turn = 0;            // trees: 1 clockwise, -1 counter-clockwise, 0 automatic
};

struct ScreenSettings {
    bool set = false; // anything chosen at all
    LyricsMode mode = LyricsMode::Smart;
    // A show for the real installation (the default) only gives pictures automatically to
    // screens wired to a controller; a preview may use any screen that draws a picture.
    bool preview = false;
    std::vector<ScreenChoice> screens;

    [[nodiscard]] const ScreenChoice* Find(const std::string& target) const;
    [[nodiscard]] bool Empty() const { return !set; }
};

struct Surface {
    std::string target;
    std::string kind; // "matrix", "mega-tree", "tree", "grid", "submodel"
    int bufferWi = 0;
    int bufferHt = 0;
    int pixels = 0;
    double density = 0;      // lights per buffer cell: 1 for a full matrix
    double aspect = 0;       // real width over height (the layout's proportions)
    std::string orientation; // "landscape", "portrait", "square", "panoramic", "tall", "round"
    double degrees = 0;      // trees
    double visibility = 0;   // its real area against the largest surface, 0..1
    Support image = Support::Unknown;
    Support video = Support::Unknown;
    bool nativeOk = false;   // Pictures and Video draw it as a picture
    bool noController = false;
    // Where it can be used: "layout" (it is there but draws no picture), "preview" (it
    // draws pictures but no controller drives it: only seen on the screen) or
    // "installation" (it draws pictures and is wired). Controllers set inactive (as in
    // tests) still count as wired: what matters is that the figure has one.
    std::string availability;
    std::vector<std::string> conflicts; // figures sharing its channels
    double lyricShare = 0;   // of the song with words planned on it
    double freeShare = 0;    // of the song free of words and singing faces
    SurfaceRole role = SurfaceRole::Support;
    std::string roleSource; // "auto" or "person"
    double score = 0;
    std::vector<std::string> reasons;
};

struct SurfacePlan {
    LyricsMode mode = LyricsMode::Smart;
    bool preview = false;
    std::vector<Surface> surfaces; // best first
    std::vector<std::string> warnings;

    [[nodiscard]] const Surface* Find(const std::string& target) const;
};

[[nodiscard]] const char* SurfaceRoleId(SurfaceRole role);
[[nodiscard]] bool SurfaceRoleFromId(const std::string& id, SurfaceRole& role);
[[nodiscard]] const char* LyricsModeId(LyricsMode mode);
[[nodiscard]] bool LyricsModeFromId(const std::string& id, LyricsMode& mode);

// Every surface of the display with its role: a person's choices first, then the
// largest, most visible, free and well wired figure as the main screen, the mega tree,
// a second screen when there is one worth it, and the rest as support.
[[nodiscard]] SurfacePlan PlanSurfaces(const ModelCapabilities& caps, const CompositionPlan* plan, const ScreenSettings& settings);

// A later choice for a screen replaces the earlier one; the mode is the latest.
void MergeScreenSettings(ScreenSettings& into, const ScreenSettings& extra);
[[nodiscard]] nlohmann::json ScreenSettingsJson(const ScreenSettings& settings);
[[nodiscard]] bool ScreenSettingsFrom(const nlohmann::json& object, ScreenSettings& settings, std::string& error);
[[nodiscard]] nlohmann::json SurfacePlanJson(const SurfacePlan& plan);
[[nodiscard]] std::string SurfaceReport(const SurfacePlan& plan);

} // namespace composer
