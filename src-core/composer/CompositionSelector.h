#pragma once

#include "composer/StageDirector.h"

#include <map>
#include <string>
#include <vector>

// Which composition of the library a scene is built on (phase 21). Every composition is
// weighed against the whole of the scene's circumstances, never one word: the kind of part,
// the voice, the scene's role in the song, how strong it is, the moments it has, how long it
// lasts, how many figures can take part and of what shape, the idea it carries, what the
// scenes before used, and a person's favourites, exclusions and choices. Each criterion adds
// or takes points with its reason; some rule a composition out. The highest score wins, ties
// by the library's order: the same scene always gets the same composition. When none fits
// well enough, the scene keeps the director's own way, and says why.
namespace composer {

// What the selector knows of a scene.
struct SelectorScene {
    std::string context;     // "narration", "solo", "duet", "choir", "instrumental", "silence", "climax"
    std::string sectionKind; // "intro", "verse", "chorus", "bridge", "interlude", "outro"...
    std::string narrative;   // "presentation", "development"... ("" before ArtLevel Direction)
    std::string motifOperation;
    std::string motifComposition; // the composition the scene's idea was built on ("" none)
    std::string motifGesture;     // the gesture kind the idea travels with here ("" none)
    StageMode mode = StageMode::Group;
    bool modeByPerson = false;
    std::string personChoice; // a person's composition for this scene ("none": none)
    std::string previous;     // the composition of the scene before ("" none)
    int previousRun = 0;      // how many scenes in a row used it
    std::map<std::string, int> used; // compositions used so far, how many times
    int priority = 1;
    int durationMS = 0;
    double beat = 0.5;
    int available = 0; // figures that can take part (placed, wired, not locked, not all face)
    std::map<std::string, int> topologies; // available figures by topology
    int faces = 0;    // figures whose voice is heard here
    int voices = 0;
    bool accentRun = false; // a short burst of strong hits standing alone
    int strongHits = 0;
    bool tail = false;      // the sound dies away somewhere in it
    bool drop = false;
    bool beforePeak = false; // the next scene is the peak
    bool peakBegins = false; // the peak starts here
    bool lastScene = false;
    int target = 0;          // the figures the director would use (0: by the budget)
    // Phase 22: the composition the plan for the whole song gives this scene ("" none planned).
    std::string planned;
    std::vector<std::string> plannedReasons; // why, for the whole song
    // Phase 22's calibration of where the built-in compositions suit (0 before it): "neighbours
    // answer" suits a solo voice too (the singer starts, the neighbours answer).
    int calibration = 0;
};

struct CompositionChoice {
    const StageLibraryEntry* entry = nullptr; // nullptr: the director's own way
    int figures = 0;                          // how many figures it uses here
    std::string adapted;                      // plain-words key: how it was fitted ("" as it is)
    std::vector<std::string> reasons;         // why it won
    std::string fallback;                     // why none was used
    std::vector<CompositionCandidate> candidates;
};

[[nodiscard]] CompositionChoice SelectComposition(const StageLibrary& library, const SelectorScene& scene);

// The compositions of the whole song, planned together (phase 22). A composition that suits a
// scene best on its own is not always the best for the song: the plan weighs each scene's own
// score (from SelectComposition, without what the scenes around it chose) with the song as a
// whole: an idea brought back with the composition it was born with is continuity and costs
// nothing; the same composition, or one of an equivalent structure (the same gesture), in the
// scene before, or used again and again without bringing an idea back, costs points. A bounded
// search keeps the best plans scene by scene; ties go to the plan whose earlier choices come
// first in the library: the same song always gets the same plan.
struct PlanScene {
    std::string scene;         // the scene's id
    SelectorScene facts;       // what the selector knows of it (previous and used are ignored)
    std::string motif;         // the visual idea it carries ("" none)
    int origin = -1;           // the index of the scene that presented that idea (-1 none, or itself)
};

struct PlannedComposition {
    std::string scene;
    std::string id;          // "" the director's own way
    double local = 0;        // its score in the scene on its own
    double song = 0;         // what the song adds or takes (continuity, variety)
    std::string kind;        // why it is here: "recall" (the idea comes back with it), "only" (nothing else fits),
                             // "best" (also the best on its own), "song" (another scored more here but the song needed variety),
                             // "person" (a person chose it), "none"
    std::string localBest;   // the best on its own in this scene
    double localBestScore = 0;
    std::vector<std::string> reasons;      // plain-words keys, for the whole song
    std::vector<std::pair<std::string, double>> alternatives; // other compositions possible, with their score in the song here
};

struct CompositionPlanResult {
    std::vector<PlannedComposition> scenes;
    double total = 0;
    int explored = 0; // plans weighed
};

[[nodiscard]] CompositionPlanResult PlanCompositions(const StageLibrary& library, const std::vector<PlanScene>& scenes);

// The time a composition needs to be seen whole in a scene.
[[nodiscard]] int CompositionNeedsMS(const StageLibraryEntry& entry, double beat, int figures);

} // namespace composer
