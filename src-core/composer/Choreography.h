#pragma once

#include "composer/ComposerTypes.h"
#include "composer/CompositionPlan.h"
#include "composer/TemplateLibrary.h"

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// From who does what to exactly which animation: the choreography turns a
// CompositionPlan into native effects with their settings, palettes and transitions.
//
// Every choice is a template from the library, scored against the target, the energy
// and the section, and turned in a deterministic rotation so that the same technique
// does not run all song. A section's choices depend only on that section; continuity
// is added afterwards by extending or varying what a target was already doing.
//
// Nothing here is visually validated: the templates start from documented values, and
// the report marks which decisions a person should look at.
namespace composer {

inline constexpr int CHOREOGRAPHY_SCHEMA_VERSION = 1;
inline constexpr const char* CHOREOGRAPHER_VERSION = "1.0.0";

struct ChoreographyEvent {
    std::string id; // "<segment>/<target>/<block>"
    std::string target; // "Parent/Sub" for a submodel
    TargetType type = TargetType::Model;
    Role role = Role::Prop;
    Prominence prominence = Prominence::Support;
    std::string intent;
    std::string segmentId;
    int startMS = 0;
    int endMS = 0;
    int layer = 0;
    std::string effect;
    std::string templateId;
    int templateVersion = 0;
    std::vector<std::pair<std::string, std::string>> settings; // E_ and T_ keys, by key
    std::string paletteId;
    std::vector<std::string> colors;
    double intensity = 0;
    std::string transitionIn;
    int fadeInMS = 0;
    std::string transitionOut;
    int fadeOutMS = 0;
    std::string continuation; // "new", "vary" or "extend"
    Compatibility compatibility = Compatibility::Compatible;
    std::vector<std::string> constraints;
    std::vector<Reason> reasons;
    double confidence = 0;
    std::string provenance; // "template:<id>@<version>" or "manual"
    DecisionState state = DecisionState::Automatic;
    bool needsReview = false;

    // "C_BUTTON_Palette1=#..,C_CHECKBOX_Palette1=1,..." as SequenceWriter takes it.
    [[nodiscard]] std::string PaletteString() const;
};

// Where a picture or a video could go once the media library exists. Never an effect:
// nothing is pointed at a file that is not there.
struct MediaCue {
    std::string id;
    std::string target;
    int startMS = 0;
    int endMS = 0;
    std::string kind; // "image", "video"
    std::string reason;
};

struct ChoreographyCorrection {
    enum class Action {
        SetTemplate, // the target uses `templateId` in the range
        SetPalette,  // the target uses `paletteId` in the range
        SetTiming,   // the event starting at `startMS` becomes [newStartMS, newEndMS]
        Exclude,     // no event on the target in the range (the whole song with no range)
        Confirm,     // the automatic events in range become approved preferences
        Lock         // `event` stands exactly as given
    };
    Action action = Action::SetTemplate;
    std::string target;
    int startMS = -1;
    int endMS = -1;
    std::string templateId;
    std::string paletteId;
    int newStartMS = -1;
    int newEndMS = -1;
    ChoreographyEvent event;
};

struct ChoreographyConflict {
    std::string target;
    int startMS = -1;
    int endMS = -1;
    std::string reason;
};

// Numbers that catch a choreography that is too repetitive or too busy; none of them
// says it looks good.
struct ChoreographyMetrics {
    int events = 0;
    int targets = 0;
    int distinctTemplates = 0;
    std::string topTemplate;
    double topTemplateShare = 0;    // of all effect time
    double changesPerMinute = 0;    // template changes per target per minute
    double repeatedNeighbours = 0;  // share of a target's segment changes that kept the same template
    double averageActive = 0;       // targets with an event at a time, over music
    int maxActive = 0;
    int distinctPalettes = 0;
    int paletteChanges = 0;
    double restShare = 0;           // of music time with nothing lit
    int accents = 0;
    int needsReview = 0;
    int mediaCues = 0;
};

struct Choreography {
    int schemaVersion = CHOREOGRAPHY_SCHEMA_VERSION;
    std::string engineVersion = CHOREOGRAPHER_VERSION;
    std::string planHash;
    std::string libraryHash;
    std::string profileId;
    uint64_t seed = 0;
    AutomationLevel level = AutomationLevel::Balanced;
    int durationMS = 0;
    int frameMS = 25;
    std::string lyricTrack;
    std::vector<std::string> voiceTracks; // one per voice that faces sing
    std::vector<ChoreographyEvent> events; // by start, target, layer
    std::vector<MediaCue> mediaCues;
    std::vector<ChoreographyCorrection> corrections;
    std::vector<ChoreographyConflict> conflicts;
    std::vector<std::string> warnings;
    ChoreographyMetrics metrics;
};

struct ShowVisualDirection;

struct ChoreographyInput {
    const CompositionPlan* plan = nullptr;           // required
    const ModelCapabilities* capabilities = nullptr; // required
    const TemplateLibrary* library = nullptr;        // required
    const StyleProfile* profile = nullptr;           // null: the neutral starter profile
    const SongAnalysis* analysis = nullptr;          // downbeats for accents and block cuts
    const LyricAlignment* alignment = nullptr;       // phrases for text, phonemes for faces
    std::string lyricTrack;                          // the Phase 4 track faces read
    std::map<std::string, std::string> voiceTracks;  // voice id -> the track of that voice alone
    std::optional<AutomationLevel> level;            // default: the plan's
    std::optional<uint64_t> seed;                    // default: the plan's
    int frameMS = 25;
    std::vector<ChoreographyCorrection> corrections;
    // With a direction whose ensemble is on, every family of figures follows the scene:
    // its colours, its kind of movement and how much of the display it lights.
    const ShowVisualDirection* direction = nullptr;
};

[[nodiscard]] Choreography Choreograph(const ChoreographyInput& input);

[[nodiscard]] ChoreographyMetrics MeasureChoreography(const Choreography& choreography);

// The events as plain SequenceSpec data, for SequenceWriter and GenerationValidator.
// Only for an isolated test fixture in this phase: the real generator comes next.
[[nodiscard]] SequenceSpec ToSequenceSpec(const Choreography& choreography, const std::vector<TimingTrackSpec>& tracks);

[[nodiscard]] const char* ChoreographyActionId(ChoreographyCorrection::Action action);
[[nodiscard]] std::string ToJson(const Choreography& choreography);
[[nodiscard]] bool FromJson(const std::string& text, Choreography& choreography, std::string& error);
[[nodiscard]] std::string ChoreographyReport(const Choreography& choreography);

} // namespace composer
