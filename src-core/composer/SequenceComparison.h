#pragma once

#include "composer/ArtisticPreferences.h"
#include "composer/ChromaticDirector.h"
#include "composer/ComposerTypes.h"

#include <string>
#include <vector>

// What a person changed in a show after it was made: the generated sequence against the one
// they corrected in xLights, both read with xLights' own reader. Colours changed, effects
// replaced, layers added or taken out, intensity, timing, figures brought in or left dark.
// A change is only an observation: none becomes a taste unless the person picks it, and a
// picked one teaches like any other review (two shows before it acts, reversible).
namespace composer {

inline constexpr int SEQUENCE_COMPARISON_SCHEMA_VERSION = 1;

struct SequenceChange {
    std::string kind;    // "colour", "effect", "layer-added", "layer-removed", "intensity", "timing", "figure-added", "figure-removed"
    std::string figure;
    int layer = 0;
    int startMS = 0;
    int endMS = 0;
    std::string scene;   // the colour plan's scene it falls in ("" unknown)
    std::string before;  // in short: colours, an effect's name, a value
    std::string after;
    std::string suggestion; // the preference it could teach ("color.cohesion", "figure.<name>.prominence"...; "" none)
    double sign = 0;
    std::string why;
};

struct SequenceComparison {
    int schemaVersion = SEQUENCE_COMPARISON_SCHEMA_VERSION;
    bool ran = false;
    std::string problem;
    int generatedEffects = 0;
    int correctedEffects = 0;
    int matched = 0;
    std::vector<SequenceChange> changes;
    double familiesBefore = 0; // mean hue families per scene in the palettes, before and after
    double familiesAfter = 0;
};

[[nodiscard]] SequenceComparison CompareSequences(const SequenceSpec& generated, const SequenceSpec& corrected, const ChromaticScore* score);

// The changes a person picked become signals of their review of `sequence`; the others
// are left as they were. Returns how many signals were added.
int LearnFromComparison(ArtisticPreferences& prefs, const SequenceComparison& comparison, const std::vector<size_t>& picked, const std::string& sequence);

// The colours of a palette string, in slot order (a colour curve gives its first colour).
[[nodiscard]] std::vector<std::string> PaletteColors(const std::string& palette);

[[nodiscard]] std::string ToJson(const SequenceComparison& comparison);
[[nodiscard]] bool FromJson(const std::string& text, SequenceComparison& comparison, std::string& error);

} // namespace composer
