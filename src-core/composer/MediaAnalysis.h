#pragma once

#include "composer/MediaLibrary.h"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <string>

class xlImage;

// Looks inside the library's pictures and videos: colours, light, contrast, detail,
// where the subject is, near-duplicates, how a video moves and cuts, and how well each
// file suits a matrix or a mega tree. With the optional image-text model it also says
// what a file seems to show. Nothing is changed or removed: a file with a problem is
// only marked, with the reason.
namespace composer {

inline constexpr const char* MEDIA_ANALYZER_VERSION = "2";

class VisualModel;
struct ConceptIndex;
struct VisualConcepts;

struct MediaAnalysisOptions {
    std::string showDir;
    VisualModel* model = nullptr;          // null: no semantic analysis
    const ConceptIndex* index = nullptr;   // required with a model
    const VisualConcepts* concepts = nullptr; // what the rules and the model may name
    int videoSampleMS = 250;               // a frame every quarter of a second
    int maxVideoSamples = 240;             // longer videos are sampled more sparsely
    int videoTimeLimitMS = 30000;          // per video; past it the rest is not read
    int maxVideoScenes = 6;                // scenes the model looks at, the longest first
    bool force = false;                    // analyse again even when the cache fits
    std::function<bool()> cancelled;
    std::function<void(int done, int total, const std::string& name)> progress;
};

struct MediaAnalysisSummary {
    int total = 0;
    int images = 0;
    int animations = 0;
    int videos = 0;
    int usable = 0;
    int unsupported = 0;
    int damaged = 0;
    int missing = 0;
    int analysed = 0; // looked at in this run
    int cached = 0;   // an earlier analysis still fitted
    int semantic = 0; // with the model's concepts
    int failed = 0;
    int duplicates = 0;
    int similar = 0;
    bool cancelled = false;
    std::string mode; // "semantic", "basic" or "mixed"
    int elapsedMS = 0;
};

// A picture resized by area averaging (shrinking) or bilinear sampling (enlarging), the
// same on every computer.
[[nodiscard]] xlImage ResampleImage(const xlImage& image, int width, int height);

// The cache key of an analysis of `item` made with `modelId` ("" without a model).
[[nodiscard]] std::string MediaAnalysisKey(const MediaItem& item, const std::string& modelId, const MediaAnalysisOptions& options);

// Measures one picture (any size) into `analysis`; the item's other fields are left.
void AnalyzePicture(const xlImage& picture, MediaAnalysis& analysis);

// Looks at one usable file; returns false when it could not be read.
bool AnalyzeMediaItem(MediaItem& item, const MediaAnalysisOptions& options);

// Every usable file of the library whose analysis is missing or out of date, then the
// duplicates, then the categories the concepts suggest (a person's categories stay).
MediaAnalysisSummary AnalyzeMediaLibrary(MediaLibrary& library, const MediaAnalysisOptions& options);

// Marks files that are the same picture or video as an earlier one ("duplicate-of:") or
// look very alike ("similar-to:"). Recomputed on every analysis: it depends on the
// library, not on the file.
void FindDuplicateMedia(MediaLibrary& library);

// The fixed categories an item counts as: a person's own, or those its name suggested
// together with what the analysis found.
[[nodiscard]] std::vector<std::string> EffectiveCategories(const MediaItem& item);
// The analysis as planning reads it: a person's labels over the model's (rejected and
// excluded concepts gone, confirmed and added ones certain and first, their categories
// added). The stored analysis is not changed. `concepts` may be null.
[[nodiscard]] MediaAnalysis EffectiveAnalysis(const MediaItem& item, const VisualConcepts* concepts);
// The same for every file of a library copy that planning reads.
void ApplyHumanLabels(MediaLibrary& library, const VisualConcepts* concepts);

[[nodiscard]] nlohmann::json MediaAnalysisJson(const MediaAnalysis& analysis);
[[nodiscard]] MediaAnalysis MediaAnalysisFrom(const nlohmann::json& object);
[[nodiscard]] std::string MediaAnalysisReport(const MediaLibrary& library, const MediaAnalysisSummary& summary);

} // namespace composer
