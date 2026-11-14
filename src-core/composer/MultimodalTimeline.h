#pragma once

#include "composer/AudioUnderstanding.h"
#include "composer/PerformanceMapping.h"
#include "composer/VocalUnderstanding.h"

#include <string>
#include <vector>

// What happens in the recording, lane by lane: the song's sections, its beats and bars,
// how the sound moves (attacks, drops, tails, silences), who speaks or sings, the
// instruments' turns, the words, and the figures that show each of them. Every item
// keeps the id of what it shows, so an edit made on it becomes a correction of that
// thing and survives the next analysis (ids are not times: moving an item keeps it).
namespace composer {

struct LyricAlignment;

struct SongAnalysis;

struct TimelineItem {
    std::string id;     // lane-unique and stable: "<lane>/<source id>"
    std::string source; // the id of what it shows: "speech#2", "voice:A#3", "attack#7", a word id, a section index
    int startMS = 0;
    int endMS = 0;
    std::string kind;   // the activity, event, emission or section kind
    std::string label;  // plain words for a person (to translate)
    double confidence = 0;
    std::string provenance; // "detected", "stem:voice", "lyrics", "corrected", "manual"...
    std::string track;      // for voice items
    std::vector<std::string> figures; // for the figures lane
    bool editable = false;
    std::string who;    // the voice's name or the figures' names, as they are (not translated)
};

struct TimelineLane {
    std::string id;    // "structure", "beats", "bars", "energy", "events", "narration", "lead", "chorus", "secondary", "instrumental", "words", "figures"
    std::string title; // plain words
    std::vector<TimelineItem> items;
};

struct MultimodalTimeline {
    int durationMS = 0;
    std::vector<TimelineLane> lanes;

    [[nodiscard]] const TimelineLane* Lane(const std::string& id) const;
    [[nodiscard]] const TimelineItem* Find(const std::string& itemId) const;
    // What sounds or is shown at an instant, across the lanes.
    [[nodiscard]] std::vector<const TimelineItem*> At(int timeMS) const;
};

struct TimelineInput {
    const SongAnalysis* song = nullptr;
    const AudioUnderstanding* audio = nullptr;
    const VocalUnderstanding* voices = nullptr;
    const LyricAlignment* alignment = nullptr;
    const PerformanceMap* performers = nullptr;
};

[[nodiscard]] MultimodalTimeline BuildTimeline(const TimelineInput& input);

// An edit made on the timeline.
struct TimelineEdit {
    enum class Action {
        Move,       // new start and end
        Split,      // at `atMS`
        Merge,      // with `otherItemId`
        Reclassify, // new `kind` ("speech", "singing", "chorus", "instrumental"...)
        Reassign,   // to `track` (a voice) or to `figures` (the figures lane)
        Remove,
        Confirm
    };
    Action action = Action::Confirm;
    std::string itemId;
    int startMS = -1;
    int endMS = -1;
    int atMS = -1;
    std::string otherItemId;
    std::string kind;
    std::string track;
    std::vector<std::string> figures;
};

// What the edit changes, ready to be stored apart from the analysis: a correction of
// the audio understanding, or a person's assignment of figures. `error` says plainly
// why an edit cannot be made (the words are corrected in the lyrics editor; beats and
// sections in the music analysis).
struct TimelineChange {
    bool ok = false;
    std::string error; // stable code
    bool isCorrection = false;
    UnderstandingCorrection correction;
    bool isAssignment = false;
    PerformerAssignment assignment;
};

[[nodiscard]] TimelineChange TranslateEdit(const MultimodalTimeline& timeline, const TimelineEdit& edit, const AudioUnderstanding& audio, const VocalUnderstanding& voices,
                                           const PerformanceMap* performers);

// Where an edit is applied: corrections into `audio.corrections` (applied again at once),
// assignments into `manual`, replacing a person's earlier one for the same source.
void ApplyChange(const TimelineChange& change, AudioUnderstanding& audio, std::vector<PerformerAssignment>& manual);

[[nodiscard]] std::string LaneTitle(const std::string& lane);
[[nodiscard]] std::string TimelineEditError(const std::string& code);

} // namespace composer
