#pragma once

#include "composer/MediaLibrary.h"
#include "composer/ModelCapabilities.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// How a picture or a video sits on a figure: what is seen, whether it moves, and which
// native settings of the Pictures and Video effects do it. Two things the effects
// cannot do by themselves are prepared as a derived picture in the show's Show Composer
// folder: correcting for a figure whose lights are not square (a matrix 49 x 20 that is
// 4 x 3.5 metres), and giving the "vector" movement a picture of a sensible size (its
// scale is a percentage of the picture's own pixels). The original is never changed; a
// derived picture is named by the original's hash and the framing, so the same framing
// makes the same file and reuses it.
namespace composer {

inline constexpr const char* MEDIA_FRAMING_VERSION = "1";

struct FrameTarget {
    std::string name;
    bool tree = false;
    bool wraps = false;     // a tree whose strings go all the way round
    int bufferWi = 0;
    int bufferHt = 0;
    double faceAspect = 1;  // width over height of what one viewer sees, in real units
    double wrapAspect = 0;  // wrapping trees: the circumference over the height
    double pixelAspect = 1; // real width of one buffer pixel over its real height
    std::vector<std::string> assumptions; // what had to be guessed about the figure
    // Wrapping trees: where the front face's centre column is, as a share of the
    // buffer's width (0.25 puts the two faces on the buffer's two halves), and which
    // way a turn goes (1 or -1).
    double frontOffset = 0.25;
    int turn = 1;
};

// A part of a video drawn on some of a tree's columns: `x0`..`x1` of the buffer's width
// show `cropX0`..`cropX1` of the frame's width.
struct FramePiece {
    double x0 = 0;
    double x1 = 1;
    double cropX0 = 0;
    double cropX1 = 1;
};

[[nodiscard]] FrameTarget FrameTargetOf(const TargetInfo& target);

struct MediaFraming {
    // "fill", "subject", "fit", "pan-x", "pan-y",
    // "zoom-in", "zoom-out", "tree-faces", "tree-turn", "tree-wrap", "video-fill",
    // "video-crop", "video-fit", "video-wrap", "gif-fill", "gif-fit"
    std::string strategy;
    double coverage = 1;   // share of the figure the file fills; below 1 there are bands
    double kept = 1;       // share of the file that is seen
    bool poiKept = true;   // the point of interest is on the figure
    bool moves = false;    // a camera movement over a still picture
    bool derived = false;  // needs a derived picture
    // The geometry of the derived picture, in buffer pixels; what its name hashes.
    int derivedWi = 0;
    int derivedHt = 0;
    double cropX = 0, cropY = 0, cropW = 1, cropH = 1; // of the original, 0..1
    std::string layout;    // "cover", "fit", "faces", "wrap"
    bool bandsAcross = false; // "fit": bands above and below (else at the sides)
    std::vector<std::pair<std::string, std::string>> settings; // metadata ids (not keys) and values
    std::vector<std::string> constraints; // "cropped", "letterboxed", "pans", "zooms", "turns", "derived"
    double score = 0;
    std::string why;
    // Videos on a wrapping tree: the pieces that draw them, each its own effect.
    std::vector<FramePiece> pieces;
    // Still pictures moving slowly on a flat figure: a short clip with a soft start and
    // end, drawn from the original between two views (fractions of the picture).
    bool motionClip = false;
    double fromX = 0, fromY = 0, fromW = 1, fromH = 1;
    double toX = 0, toY = 0, toW = 1, toH = 1;
    int clipWi = 0;
    int clipHt = 0;
    std::vector<std::pair<std::string, std::string>> clipSettings; // for the Video effect
    double roll = 0; // trees: the derived picture turned by this share of the width

    [[nodiscard]] std::string ClipName(const MediaItem& item, int durationMS) const;

    [[nodiscard]] std::string DerivedName(const MediaItem& item) const;
};

// The best framing of `item` on `target` for a shot of `durationMS`, at an `energy` of
// 0..1, for a scene of `sceneRole` ("establish", "verse", "chorus", "climax", ...).
// Deterministic; it writes nothing.
[[nodiscard]] MediaFraming PlanFraming(const MediaItem& item, const FrameTarget& target, int durationMS, double energy, const std::string& sceneRole, uint64_t seed);

[[nodiscard]] std::string MediaDerivedDir(const std::string& showDir);

// Makes the derived picture the framing needs (once: an existing one is reused) and
// returns its path relative to the show folder.
bool MakeDerivedPicture(const MediaItem& item, const MediaFraming& framing, const std::string& showDir, std::string& relative, std::string& error);

// Makes the framing's motion clip (once: an existing one is reused): an H.264 video of
// `durationMS` at 20 frames a second through xLights' own VideoWriter.
bool MakeMotionClip(const MediaItem& item, const MediaFraming& framing, int durationMS, const std::string& showDir, std::string& relative, std::string& error);

// The pieces that put `cropX0`..`cropX1` of a frame on `width` of a wrapping buffer
// starting at `start` (shares of the width), split where the buffer's ends meet.
[[nodiscard]] std::vector<FramePiece> SpreadOnColumns(double start, double width, double cropX0, double cropX1);

// Removes derived pictures and clips no generation has used for `maxAgeDays`, keeping
// the ones in `keep` (file names); returns how many were removed.
int PruneMediaDerived(const std::string& showDir, const std::vector<std::string>& keep, int maxAgeDays);

} // namespace composer
