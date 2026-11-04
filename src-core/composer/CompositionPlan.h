#pragma once

#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/ModelCapabilities.h"
#include "composer/SongAnalysis.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Who does what, and when: the plan of a show before any effect is chosen.
//
// The planner reads the song analysis, the lyrics if there are any, the capabilities
// of the installation and a style profile, and decides for every stretch of the song
// which targets lead, which support, which accent the beat and which rest. Every
// decision carries the reasons behind it, so a person can follow and overrule it.
//
// Deterministic: the same inputs, seed and level give the same plan byte for byte, and
// a section's decisions depend only on that section's music, never on what was chosen
// elsewhere, so changing one section leaves the others as they were.
namespace composer {

inline constexpr int PLAN_SCHEMA_VERSION = 1;
inline constexpr const char* PLANNER_VERSION = "1.0.0";

enum class Prominence {
    Lead,
    Support,
    Accent,
    Rest
};

enum class DecisionState {
    Automatic,
    ManualEdit,
    ApprovedPreference,
    Locked
};

enum class AutomationLevel {
    Conservative,
    Balanced,
    Creative
};

// Per level: how much happens and how much it varies.
struct LevelSettings {
    double densityScale = 1.0;
    double variation = 0.5;   // 0: always the best-scored candidate of the rotation
    int leadPool = 3;         // how many top candidates take turns as lead
    int maxLeads = 2;
    double minRoleConfidence = 0.4;
    int accents = 2;
    int maxFaces = 2;
    bool rotateWithinSection = true;
};

// The whole display working together, as the reference shows do: every family of
// figures under a base effect in the scene's colours, short hits on the beat above it,
// and the same look when a section comes back. Off in the neutral profile; a profile
// made from reference shows turns it on with what was measured in them.
struct EnsembleSettings {
    bool enabled = false;
    // Share of the display's nodes under an effect, by energy band.
    double coverageLow = 0.45;
    double coverageMid = 0.75;
    double coverageHigh = 0.9;
    double coverageClimax = 1.0;
    // Short hits a minute on the whole display, by band; 0 turns them off.
    double hitsPerMinuteMid = 0;
    double hitsPerMinuteHigh = 60;
    double hitsPerMinuteClimax = 120;
    int hitMS = 350;
    double fadeOutShare = 0.3;   // of base events that fade out
    double contrastShare = 0.25; // of non-repeated sections that leave the home palette
    int colorsPerEvent = 2;
    bool outlinesWhenCalm = true; // calm sections light rings and outlines rather than whole props
    std::string defaultTheme = "christmas-classic";
    std::vector<std::string> sources; // the reference studies this was measured from
};

// Starting values, not anyone's taste: the "Neutral Starter Profile". A profile learned
// from approved shows will replace it, through the same fields.
struct StyleProfile {
    std::string id = "neutral-starter";
    std::string name = "Neutral Starter Profile";
    bool learned = false;
    int version = 1;
    int minSegmentMS = 4000;
    int maxSegmentMS = 16000;
    double maxChangesPerMinute = 8;
    int minRestMS = 1500;         // a silence shorter than this is not a rest of its own
    double densityLow = 0.15;     // share of the eligible targets active, by energy
    double densityMid = 0.3;
    double densityHigh = 0.5;
    double densityClimax = 0.75;
    int maxParticipants = 12;
    double lowEnergy = 0.35;
    double highEnergy = 0.6;
    double maxLeadShare = 0.5;    // checked, never enforced across sections
    bool groupsMayOverlapMembers = false;
    std::map<Role, double> leadPreference;
    std::map<Role, double> supportPreference;
    std::map<std::string, double> templateWeights; // choreography template id -> weight; empty: all equal
    LevelSettings conservative;
    LevelSettings balanced;
    LevelSettings creative;
    EnsembleSettings ensemble;

    [[nodiscard]] const LevelSettings& Level(AutomationLevel level) const;
};

[[nodiscard]] StyleProfile NeutralStarterProfile();
[[nodiscard]] std::string ToJson(const StyleProfile& profile);
[[nodiscard]] bool FromJson(const std::string& text, StyleProfile& profile, std::string& error);

struct GenerationOptions {
    AutomationLevel level = AutomationLevel::Balanced;
    uint64_t seed = 1;
    int frameMS = 25;
    double minVocalConfidence = 0.5; // the analyzer's voice hint is 0.3, so it stays unused
    bool useSubModels = false;       // submodels only join through roles.json or a correction
};

struct Reason {
    std::string code; // stable: "energy-high", "rotation", "density-budget"...
    std::string detail;
};

struct PlanAssignment {
    std::string target;
    TargetType type = TargetType::Model;
    Role role = Role::Prop;
    Prominence prominence = Prominence::Rest;
    std::string intent; // "feature", "accompany", "beat-accent", "lyric-text", "face-sing", "rest"
    double intensity = 0;
    double confidence = 0;
    DecisionState state = DecisionState::Automatic;
    std::vector<Reason> reasons;
};

struct PlanSegment {
    std::string id; // "s<section>.<ordinal>"
    int startMS = 0;
    int endMS = 0;
    int sectionIndex = -1;
    std::string sectionKind;
    int repeatGroup = -1;
    int occurrence = 0; // which repetition of its repeat group, from 0
    double energy = 0;  // measured
    double targetIntensity = 0;
    double density = 0; // share of eligible targets taking part
    int priority = 0;   // 0 rest, 1 normal, 2 high, 3 climax
    std::vector<std::string> flags; // "silence", "climax", "user-chorus", "lyrics", "intro", "ending"
    double vocalCoverage = 0;
    DecisionState state = DecisionState::Automatic;
    std::vector<PlanAssignment> assignments; // by target name
    std::vector<Reason> reasons;
};

struct PlanCorrection {
    enum class Action {
        Exclude,       // the target rests in the range (the whole song with no range)
        Include,       // the target takes part with `prominence`
        SetProminence,
        SetDensity,
        MoveBoundary,  // a segment exactly [startMS, endMS]
        Lock,          // `assignments` stand as they are in [startMS, endMS]
        Accept,        // the automatic decisions in range become ApprovedPreference
        Reject         // the target's automatic proposal in range is turned down
    };
    Action action = Action::Include;
    int startMS = -1;
    int endMS = -1;
    std::string target;
    Prominence prominence = Prominence::Support;
    double density = -1;
    std::vector<PlanAssignment> assignments;
};

struct PlanConflict {
    std::string target;
    int startMS = -1;
    int endMS = -1;
    std::string reason; // "target-missing", "target-excluded", "capability-no", "group-conflict-with-manual", "inside-locked-segment"
};

struct CompositionPlan {
    int schemaVersion = PLAN_SCHEMA_VERSION;
    std::string plannerVersion = PLANNER_VERSION;
    std::string analysisHash;
    std::string lyricsHash; // empty without lyrics
    std::string layoutHash;
    std::string profileHash;
    std::string profileId;
    uint64_t seed = 0;
    AutomationLevel level = AutomationLevel::Balanced;
    int durationMS = 0;
    bool lyricsSynchronized = false;
    std::vector<PlanSegment> segments;
    std::vector<PlanCorrection> corrections;
    std::vector<PlanConflict> conflicts;
    std::vector<std::string> warnings;
};

struct PlannerInput {
    const SongAnalysis* analysis = nullptr;     // required
    const ModelCapabilities* capabilities = nullptr; // required
    const LyricDocument* lyrics = nullptr;
    const LyricAlignment* alignment = nullptr;
    const StyleProfile* profile = nullptr;      // null: the neutral starter profile
    GenerationOptions options;
    std::vector<PlanCorrection> corrections;
};

[[nodiscard]] CompositionPlan PlanComposition(const PlannerInput& input);

[[nodiscard]] const char* ProminenceId(Prominence p);
[[nodiscard]] const char* DecisionStateId(DecisionState s);
[[nodiscard]] const char* LevelId(AutomationLevel level);
[[nodiscard]] const char* CorrectionActionId(PlanCorrection::Action action);

[[nodiscard]] std::string ToJson(const CompositionPlan& plan);
[[nodiscard]] bool FromJson(const std::string& text, CompositionPlan& plan, std::string& error);
// The plan in words: every segment, who does what and why.
[[nodiscard]] std::string PlanReport(const CompositionPlan& plan);

[[nodiscard]] std::string HashText(const std::string& text);

} // namespace composer
