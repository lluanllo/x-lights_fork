#pragma once

#include "composer/MediaLibrary.h"

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <string>
#include <utility>
#include <vector>

// How well the image model describes a person's own library, measured against what a
// person says each picture shows. The model proposes; a person corrects. Nothing here
// is a metric until a person has labelled the files: labels written by anyone else
// (an assistant, a script) are kept apart and reported as provisional.
namespace composer {

struct MediaValidationEntry {
    std::string mediaId;
    std::string name;
    std::string path; // as found, for a person to open it
    std::string kind; // "image", "animation", "video"
    // What the model proposed, best first.
    std::vector<MediaConcept> proposed;
    std::vector<std::string> description;
    std::vector<std::string> categories;
    double poiX = 0.5, poiY = 0.5;
    double matrixFit = 0, treeFit = 0;
    std::vector<std::string> issues;
    // The labels: who wrote them and what the picture really shows.
    std::string labelSource; // "": not labelled; "person"; "assistant-provisional"
    std::string reviewer;
    std::string reviewedAt;
    std::vector<std::string> present; // concept ids that are really in it
    int poiUseful = -1;      // 1 the point of interest is on what matters, 0 not, -1 not said
    int descriptionOk = -1;  // 1 the description would do for a person, 0 not, -1 not said
    std::string notes;

    [[nodiscard]] bool Labelled() const { return !labelSource.empty(); }
};

struct MediaValidationSet {
    int version = 1;
    std::string model;   // the model's id, or "" for colours, light and motion only
    std::string created;
    std::vector<MediaValidationEntry> entries;
};

// A small set to label: files spread over the library's folders and kinds (videos too),
// the same ones for the same library and seed.
[[nodiscard]] MediaValidationSet BuildMediaValidationSet(const MediaLibrary& library, size_t size, uint64_t seed);
// Labels already written for the same files are carried into a new set.
void CarryMediaLabels(MediaValidationSet& into, const MediaValidationSet& labelled);

[[nodiscard]] nlohmann::json MediaValidationSetJson(const MediaValidationSet& set);
[[nodiscard]] bool MediaValidationSetFrom(const nlohmann::json& object, MediaValidationSet& set, std::string& error);

struct MediaConceptCount {
    int truePositive = 0;
    int falsePositive = 0;
    int falseNegative = 0;
};

struct MediaValidationMetrics {
    std::string source;  // the labels counted: "person" or "assistant-provisional"
    size_t topK = 3;
    int files = 0;       // labelled by that source
    int truePositive = 0;
    int falsePositive = 0;
    int falseNegative = 0;
    double precision = 0; // of the top-K concepts proposed
    double recall = 0;    // of the concepts really there
    double filesWithAHit = 0; // share of files whose top-K has at least one right concept
    int confidentRight = 0;    // of the top-K, only those the model called strong or probable
    int confidentWrong = 0;
    double confidentPrecision = 0;
    std::map<std::string, MediaConceptCount> concepts;
    std::map<std::pair<std::string, std::string>, int> confusion; // (proposed, really there instead) -> times
    int poiAnswered = 0;
    double poiUseful = 0;
    int descriptionAnswered = 0;
    double descriptionOk = 0;
};

// The model's top `topK` concepts against the labels from `source` only.
[[nodiscard]] MediaValidationMetrics EvaluateMediaValidation(const MediaValidationSet& set, const std::string& source, size_t topK);
[[nodiscard]] std::string MediaValidationReport(const MediaValidationSet& set, const std::vector<MediaValidationMetrics>& metrics);

// Internal entry point behind --composer-media-eval: scans and analyses `folders` (with
// the model in `modelDir` when given) into the show's Show Composer folder, twice to
// time the cache, writes a validation set under `outDir`, carries the labels of
// `labelsFile` when given and reports the metrics they allow. 0 on success.
[[nodiscard]] int RunMediaEvaluation(const std::string& showDir, const std::vector<std::string>& folders, const std::string& modelDir, const std::string& outDir, const std::string& labelsFile,
                                     size_t sampleSize);

} // namespace composer
