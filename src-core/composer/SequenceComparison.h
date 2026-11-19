#pragma once

#include "composer/ArtisticPreferences.h"
#include "composer/ChromaticDirector.h"
#include "composer/ComposerTypes.h"
#include "composer/StageDirector.h"

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
    // Phase 20: what kind of correction it looks like, and where its scene starts.
    std::string category;  // "aesthetic" (a matter of taste), "technical" (a nudge in time), "necessary" (something that should not light)
    int sceneStartMS = -1;
    // Phase 21: who plays in a scene ("presence-reduced", "presence-grown", "composition-grown",
    // "responders", "composition-recalled"): the figures concerned.
    std::vector<std::string> figures;
    // Phase 22: what kind of correction it is: "technical", "musical", "chromatic", "composition",
    // "presence", "protagonism", "recipe" (a correction is not a taste until the person says so).
    // New kinds: "protagonist-changed" (before: the figure that led, after: the one that leads now),
    // "composition-changed" (before: the composition, after: "answer" or "unison"), "response-added".
    std::string nature;
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

// What a person decides about each change (phase 20). Nothing is learned unless chosen:
//   "learn"   a taste signal, like a review (two shows before it acts);
//   "song"    only this song: an edit of its stage or colours, kept with the song;
//   "general" the person's own value for the preference, now (reversible);
//   "ignore"  nothing; "reject" nothing, and never offered again for this sequence;
//   "composition" (phase 21) the figures of a change kept as a composition of the person's own.
struct ComparisonDecision {
    size_t change = 0;
    std::string choice;
};

struct ComparisonDecisions {
    int schemaVersion = SEQUENCE_COMPARISON_SCHEMA_VERSION;
    std::string sequence;
    std::vector<ComparisonDecision> decisions;
    std::vector<std::string> rejected; // ChangeKey of every change rejected, kept across comparisons
};

struct DecisionOutcome {
    int learned = 0;
    int general = 0;
    int songEdits = 0;
    int ignored = 0;
    int rejected = 0;
    int compositions = 0;
    std::vector<std::string> notes; // a change that could not be applied as asked, and why
};

// A stable key of a change, to remember a rejected one.
[[nodiscard]] std::string ChangeKey(const SequenceChange& change);
// The changes still offered: the rejected ones left out.
[[nodiscard]] std::vector<size_t> OfferedChanges(const SequenceComparison& comparison, const ComparisonDecisions& decisions);
DecisionOutcome ApplyComparisonDecisions(ArtisticPreferences& prefs, StageEdits& stage, ChromaticEdits& colour, ComparisonDecisions& record, const SequenceComparison& comparison,
                                         const std::vector<ComparisonDecision>& decisions, const std::string& sequence, StageLibrary* library = nullptr);

// Who plays in each scene of the corrected show against the generated one (phase 21), read
// scene by scene against the stage score: figures a person quietened or took out, figures
// they brought in, figures they made answer one another, a scene that brings back the
// figures of an earlier one, one figure's idea turned into a group's. `groups` gives each
// xLights group's figures (a group's effect lights them all). Observations only.
[[nodiscard]] std::vector<SequenceChange> PresenceChanges(const SequenceSpec& generated, const SequenceSpec& corrected, const GlobalStageScore& stage,
                                                          const std::map<std::string, std::vector<std::string>>& groups);
// Whether a change speaks of who plays (it can be kept as a composition).
[[nodiscard]] bool IsPresenceChange(const SequenceChange& change);
// The nature of a change from its kind (phase 22).
[[nodiscard]] std::string ChangeNature(const SequenceChange& change);

// Figures a person made work together in the corrected show (several figures given the same
// effect at nearly the same time where the show had none), offered as compositions to keep.
// Nothing is saved: a person confirms each one.
[[nodiscard]] std::vector<StageLibraryEntry> ProposeCompositions(const SequenceSpec& generated, const SequenceSpec& corrected, const ChromaticScore* score, const std::string& sequence);

[[nodiscard]] std::string ToJson(const ComparisonDecisions& decisions);
[[nodiscard]] bool FromJson(const std::string& text, ComparisonDecisions& decisions, std::string& error);
[[nodiscard]] std::string ComparisonDecisionsPath(const std::string& showDir, const std::string& sequence);

// The colours of a palette string, in slot order (a colour curve gives its first colour).
[[nodiscard]] std::vector<std::string> PaletteColors(const std::string& palette);

[[nodiscard]] std::string ToJson(const SequenceComparison& comparison);
[[nodiscard]] bool FromJson(const std::string& text, SequenceComparison& comparison, std::string& error);

} // namespace composer
