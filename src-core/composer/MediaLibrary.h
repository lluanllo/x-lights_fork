#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

class xlImage;

// The pictures and videos a show may use, found in folders a person chose. Nothing is
// moved, renamed or deleted: the library only reads the files and keeps what it learnt
// (size, dimensions, length, categories, a thumbnail) in the show's Show Composer folder.
namespace composer {

enum class MediaKind { Image, Animation, Video, Unsupported };
enum class MediaStatus { Ok, Missing, Unreadable, Unsupported };

struct MediaColor {
    std::string hex; // "#rrggbb"
    double share = 0; // of the picture
};

// Something the picture seems to show. `score` is the model's calibrated probability
// (or a rule's guess); `level` is how far it may be trusted: "strong", "probable" or
// "weak". Never a certainty.
struct MediaConcept {
    std::string id; // from resources/composer/visual-concepts.json
    double score = 0;
    double similarity = 0; // the model's cosine, kept to explain a choice
    std::string level;
    std::string source; // "model" or "rules"
};

// A stretch of a video between two cuts.
struct MediaScene {
    int startMS = 0;
    int endMS = 0;
    double brightness = 0;
    double motion = 0; // 0..1, how much the picture changes per second
    double energy = 0; // 0..1
    std::string color;
    uint64_t dhash = 0;
    std::vector<MediaConcept> concepts;
};

struct MediaVideoAnalysis {
    int samples = 0;
    int sampledMS = 0; // how far the samples reached; less than the length when stopped early
    std::vector<MediaScene> scenes;
    double motion = 0;
    int cuts = 0;
    std::vector<std::pair<int, int>> darkSpans; // ms
    double loop = 0;   // 0..1, how well the end runs into the start
    double energy = 0; // 0..1
    bool shortUse = false; // reads in a few seconds
    bool longUse = false;  // holds a long stretch without tiring
};

// What the library learnt by looking at a file, kept with it. `key` says which file
// version, analyser, model and parameters produced it: when any of them changes the
// file is looked at again, otherwise this is reused.
struct MediaAnalysis {
    std::string key;
    std::string analyzer; // version of the code that measured it
    std::string model;    // "" (no model) or the model's id@version
    std::string mode;     // "" (not analysed), "basic" or "semantic"
    std::string error;
    std::vector<MediaColor> colors; // dominant, largest first
    double brightness = 0;
    double contrast = 0;
    double saturation = 0;
    double colorfulness = 0;
    double complexity = 0; // edge density
    double dark = 0;       // share of nearly black pixels
    double white = 0;      // share of nearly white, colourless pixels
    double highlights = 0; // share of clearly lit pixels: what a dark picture still shows
    std::string orientation; // "panoramic", "landscape", "square", "portrait", "tall"
    double poiX = 0.5;       // point of interest, 0..1 from the left
    double poiY = 0.5;       // 0..1 from the top
    std::string subject;     // where the main subject sits: "center", "left", "right", "top", "bottom"
    uint64_t dhash = 0;      // perceptual hash, for duplicates
    std::vector<MediaConcept> concepts; // best first
    std::vector<std::string> description; // concept ids that describe it, strongest first
    std::vector<std::string> categories;  // fixed categories the concepts suggest
    double matrixFit = 0; // 0..1
    double treeFit = 0;   // 0..1
    std::vector<std::string> fitReasons;
    std::vector<std::string> issues; // "very-dark", "duplicate-of:<id>", ...
    MediaVideoAnalysis video;

    [[nodiscard]] bool Done() const { return !mode.empty(); }
};

// What a person says a file shows, kept apart from the model's analysis, which is never
// changed: the model's words stay to be compared with the person's.
struct MediaHumanLabels {
    std::vector<std::string> confirmed; // concepts the model proposed and the person agrees with
    std::vector<std::string> rejected;  // concepts the model proposed that are wrong
    std::vector<std::string> added;     // concepts the model missed
    std::vector<std::string> excluded;  // concepts never to attach to it, whatever a model says later
    // Uses it is not for: "matrix", "tree", "calm", "intense", "opening", "ending".
    std::vector<std::string> notFor;
    // The person went through everything the model proposed: only then can the file
    // measure the model (an unconfirmed, unrejected concept is otherwise unknown).
    bool complete = false;
    std::string reviewer;
    std::string reviewedAt;

    [[nodiscard]] bool Empty() const { return confirmed.empty() && rejected.empty() && added.empty() && excluded.empty() && notFor.empty() && !complete; }
};

inline const std::vector<std::string>& MediaUses() {
    static const std::vector<std::string> u = { "matrix", "tree", "calm", "intense", "opening", "ending" };
    return u;
}

struct MediaItem {
    // "m:" and a hash of the file's contents: the same file keeps its id when it is
    // moved or renamed, which is how a moved file is found again.
    std::string id;
    std::string path;     // absolute, UTF-8
    std::string relative; // to the show folder, '/'-separated; empty when outside it
    std::string folder;   // the library folder it was found in
    std::string name;
    std::string extension; // lowercase, no dot
    MediaKind kind = MediaKind::Unsupported;
    MediaStatus status = MediaStatus::Ok;
    std::string problem; // why it is not usable, for a person
    uint64_t bytes = 0;
    int64_t modified = 0; // seconds since the epoch
    int width = 0;
    int height = 0;
    int durationMS = 0; // videos and animations
    int frames = 0;     // animations
    double aspect = 0;  // width / height
    std::string hash;   // of the size and the first and last 64 KiB
    std::vector<std::string> categories; // ids from MediaCategories()
    std::vector<std::string> tags;       // a person's own words
    std::string categorySource;          // "name" (suggested) or "manual"
    std::string thumbnail;               // relative to the show folder
    double brightness = 0;               // mean light of the thumbnail, 0..1
    std::string color;                   // its average colour, "#rrggbb"
    bool disabled = false;               // kept out of automatic choices by a person
    bool reviewed = false;               // a person looked at its categories
    MediaAnalysis analysis;              // what the analysis (and the model) said, as it said it
    MediaHumanLabels human;

    [[nodiscard]] bool Usable() const { return status == MediaStatus::Ok && kind != MediaKind::Unsupported; }
};

struct MediaFolder {
    std::string path;     // absolute, UTF-8
    std::string relative; // to the show folder when inside it
};

struct MediaLibrary {
    // 2: each item may carry its analysis; 3: and a person's labels (older readers refuse
    // it rather than drop the labels when they save).
    int version = 3;
    std::vector<MediaFolder> folders;
    std::vector<MediaItem> items; // by path
    std::vector<std::string> warnings;
    std::map<std::string, std::string> moved; // old path -> new path, from the last scan

    [[nodiscard]] const MediaItem* Find(const std::string& id) const;
    [[nodiscard]] MediaItem* Find(const std::string& id);
    [[nodiscard]] const MediaItem* FindByPath(const std::string& path) const;
};

struct MediaScanOptions {
    std::string showDir;
    std::vector<std::string> folders;
    int maxDepth = 6;
    int maxFiles = 5000;
    bool thumbnails = true;
    std::function<bool()> cancelled;
    std::function<void(int done, const std::string& name)> progress;
};

// The fixed categories a person and the planner share, in the order they are shown.
[[nodiscard]] const std::vector<std::string>& MediaCategories();
// Categories suggested by the words in a file's name and folders: "Nieve en el bosque"
// gives snow and forest. Empty when nothing matches.
[[nodiscard]] std::vector<std::string> SuggestMediaCategories(const std::string& pathText);

[[nodiscard]] const char* MediaKindId(MediaKind kind);
[[nodiscard]] MediaKind MediaKindFromId(const std::string& id);
[[nodiscard]] const char* MediaStatusId(MediaStatus status);
[[nodiscard]] MediaStatus MediaStatusFromId(const std::string& id);
// What a file is by its name alone: an image, an animated GIF, a video or none.
[[nodiscard]] MediaKind MediaKindOfName(const std::string& path);

// A folder that may be searched: refuses a drive or file-system root, the home folder
// itself and system folders, which would mean reading a whole disk. `reason` says why.
[[nodiscard]] bool IsSafeMediaFolder(const std::string& folder, std::string& reason);

// Reads one file: its size, kind, dimensions, length and hash, and whether it opens.
[[nodiscard]] MediaItem ProbeMedia(const std::string& path, const std::string& showDir);

// Searches the folders, without following links or entering hidden folders, up to the
// limits. Categories, tags and choices a person made in `previous` are kept, also for
// files that were moved; a file that is gone stays listed as missing.
[[nodiscard]] MediaLibrary ScanMediaFolders(const MediaScanOptions& options, const MediaLibrary* previous = nullptr);

// A small picture of the file in the show's media cache, made once per file version.
bool MakeMediaThumbnail(MediaItem& item, const std::string& showDir);

// The picture of an image, an animation's first frame or a drawing, at most `maxSide`
// pixels on its longer side (never enlarged, except drawings, drawn at that size).
bool LoadMediaPicture(const MediaItem& item, int maxSide, xlImage& out);
// An animation's frames, at most `limit`, each at most `maxSide` pixels, with their times.
bool LoadMediaAnimation(const MediaItem& item, int maxSide, int limit, std::vector<xlImage>& frames, std::vector<long>& times);

// Looks for missing files in `folders` by their contents; returns how many were found.
int RelocateMissingMedia(MediaLibrary& library, const std::vector<std::string>& folders, const std::string& showDir);

// Copies a file into the show folder (only when a person agreed to it) under
// `subfolder`, never over an existing file: a new name is chosen instead. The original
// is not touched. `copied` is the new absolute path.
bool CopyMediaIntoShow(const MediaItem& item, const std::string& showDir, const std::string& subfolder, std::string& copied, std::string& error);

[[nodiscard]] std::string MediaLibraryPath(const std::string& showDir);
[[nodiscard]] std::string MediaCacheDir(const std::string& showDir);
bool SaveMediaLibrary(const std::string& path, const MediaLibrary& library, std::string& error);
bool ReadMediaLibrary(const std::string& path, MediaLibrary& library, std::string& error);

// A path as the sidecar keeps it: relative to the show folder when inside it.
[[nodiscard]] std::string MediaRelativePath(const std::string& path, const std::string& showDir);
// Back to an absolute path; relative paths are taken from the show folder.
[[nodiscard]] std::string MediaAbsolutePath(const std::string& kept, const std::string& showDir);

} // namespace composer
