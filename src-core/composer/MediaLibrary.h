#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

// The pictures and videos a show may use, found in folders a person chose. Nothing is
// moved, renamed or deleted: the library only reads the files and keeps what it learnt
// (size, dimensions, length, categories, a thumbnail) in the show's Show Composer folder.
namespace composer {

enum class MediaKind { Image, Animation, Video, Unsupported };
enum class MediaStatus { Ok, Missing, Unreadable, Unsupported };

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

    [[nodiscard]] bool Usable() const { return status == MediaStatus::Ok && kind != MediaKind::Unsupported; }
};

struct MediaFolder {
    std::string path;     // absolute, UTF-8
    std::string relative; // to the show folder when inside it
};

struct MediaLibrary {
    int version = 1;
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
