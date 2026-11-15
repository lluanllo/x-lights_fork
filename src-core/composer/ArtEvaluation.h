#pragma once

#include "composer/ChromaticDirector.h"
#include "composer/ColorCheck.h"

#include <string>
#include <vector>

// Reading a composition, not grading it: what the render and the colour plan say about
// cohesion, variety, contrast, hierarchy, continuity, the song's identity, motifs, how the
// colours are spread over the figures, overload and gestures. Each sign is a measured
// value with a plain reading ("cohesive", "scattered"...) and why; there is no overall
// score, because whether a show is beautiful is for the people who watch it.
namespace composer {

inline constexpr int ART_EVALUATION_SCHEMA_VERSION = 1;

struct CompositionSign {
    std::string id;      // "cohesion", "variety", "contrast", "hierarchy", "continuity", "identity", "motifs", "distribution", "overload", "gestures"
    double value = 0;
    std::string reading; // a word for a person: "cohesive", "related", "scattered"...
    std::string why;
};

struct CompositionEvaluation {
    int schemaVersion = ART_EVALUATION_SCHEMA_VERSION;
    bool ran = false;
    std::vector<CompositionSign> signs;
    [[nodiscard]] const CompositionSign* Sign(const std::string& id) const;
};

// `score` may be null (a show made without the colour direction): then the signs of the
// plan (continuity, motifs, identity) are left out.
[[nodiscard]] CompositionEvaluation EvaluateComposition(const ColorCheck& check, const ChromaticScore* score);
[[nodiscard]] std::string ToJson(const CompositionEvaluation& evaluation);
[[nodiscard]] std::string CompositionReport(const CompositionEvaluation& evaluation);

} // namespace composer
