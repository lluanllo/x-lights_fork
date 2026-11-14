#pragma once

#include "composer/Choreography.h"
#include "composer/CompositionPlan.h"
#include "composer/FigureSemantics.h"
#include "composer/PerformanceMapping.h"
#include "composer/ProceduralField.h"
#include "composer/SongAnalysis.h"
#include "composer/StageSpace.h"

#include <map>
#include <string>
#include <vector>

// The director of the whole stage. Before any figure's detail it decides, scene by scene,
// how the installation is used: one figure alone, two, a group, all of them, or a rest;
// which moments of the music deserve a gesture (not every hit does), and what kind: a wave
// from one bulb across the house, small waves of their own in several figures, a cascade,
// a joint pulse, a call and its answer, an echo, a sweep, light that spreads or gathers.
// Everything is a decision with its reasons, deterministic: the same song, layout and
// choices give the same score. A person's changes are kept apart and win.
namespace composer {

inline constexpr int STAGE_SCORE_SCHEMA_VERSION = 1;
inline constexpr const char* STAGE_DIRECTOR_VERSION = "1.0.0";

enum class StageMode {
    Protagonist,  // one figure has the attention
    Duo,          // two work together
    Group,        // several share an action
    Unison,       // several make the same accent
    Propagation,  // a gesture travels across the installation
    LocalWaves,   // each figure makes its own small wave
    Cascade,      // one after another
    CallResponse, // one calls, others answer
    Counterpoint, // different actions at once
    FullEnsemble, // the whole installation
    Rest          // quiet
};

enum class GestureKind {
    PropagatedWave,
    LocalWaves,
    Cascade,
    JointPulse,
    CallResponse,
    Echo,
    SpatialSweep,
    Expansion,
    Contraction,
    Counterpoint,
    AmbientField
};

struct GestureParticipant {
    std::string figure;
    std::string role;     // "source", "receiver", "responder", "accent", "background"
    int delayMS = 0;      // from the gesture's start: when this figure's part begins
    double level = 1;
    std::string function; // the field it draws (FieldFunctionId)
    std::string reason;
};

struct GlobalGesture {
    std::string id; // "<kind>@<startMS>"
    GestureKind kind = GestureKind::PropagatedWave;
    std::string origin; // an anchor id (StageSpace.h); "" for gestures each figure draws round itself
    double ox = 0.5, oy = 0.5;
    std::string trajectory = "radial"; // "radial", "linear", "elliptic", "sequence"
    double angle = 0;
    std::vector<GestureParticipant> participants;
    std::vector<std::string> excluded;
    double speed = 0.6; // stage units a second
    double width = 0.08;
    double loss = 0;
    double level = 1;
    int startMS = 0;
    int endMS = 0;
    std::string envelope = "short"; // "short", "long", "swell"
    std::vector<std::string> colors;
    std::string trigger; // the moment of the music that starts it: "<kind>@<ms>"
    std::string related; // a gesture it answers or echoes
    double confidence = 0;
    std::vector<std::string> reasons;
    bool manual = false;
};

// What each figure is in a scene.
struct SceneRole {
    std::string figure;
    std::string role; // "protagonist", "complement", "responder", "accent", "background", "rest"
    std::string why;
};

struct StageScene {
    std::string id; // the plan segment's id
    int startMS = 0;
    int endMS = 0;
    std::string sectionKind;
    StageMode mode = StageMode::Group;
    std::string intent; // plain-words key: "a voice tells", "the instruments lead"...
    std::string context; // "narration", "solo", "duet", "choir", "instrumental", "silence", "climax"
    double budget = 0;   // attention allowed
    double spent = 0;
    std::vector<SceneRole> roles;
    std::vector<std::string> gestures; // ids
    std::string motif; // the repeat group's idea it continues: "group:<n>"
    std::string transition;
    std::vector<std::string> reasons;
    double confidence = 0;
    bool manual = false;
    [[nodiscard]] std::string RoleOf(const std::string& figure) const;
};

// A layer of a figure's own composition: its colours kept while other layers move.
struct StageLayer {
    std::string figure;
    std::string role; // "identity", "movement", "gesture", "face"
    std::string region;
    std::string source; // "field:<function>", "native:<effect>"
    std::string color;
    std::string only;
    std::string skip;
    int order = 0; // 0 top
    double opacity = 1;
    std::string blend = "Normal";
    int startMS = 0;
    int endMS = 0;
};

struct GlobalStageScore {
    int schemaVersion = STAGE_SCORE_SCHEMA_VERSION;
    std::string version = STAGE_DIRECTOR_VERSION;
    int durationMS = 0;
    std::vector<StageScene> scenes;
    std::vector<GlobalGesture> gestures;
    std::vector<StageLayer> layers; // identity layers of the figures composed whole
    std::vector<std::string> limits;
    std::vector<std::string> warnings;

    [[nodiscard]] const StageScene* SceneAt(int ms) const;
    [[nodiscard]] const GlobalGesture* Gesture(const std::string& id) const;
};

// A person's changes to the score, kept per song and applied after every new score.
struct StageEdit {
    std::string action; // "mode", "include", "exclude", "no-gestures", "gesture", "origin", "speed", "identity", "no-identity"
    int sceneStartMS = -1; // the scene, found again by its start (within 1.5 s)
    int atMS = -1;         // a gesture's start, for "gesture" (a new one), "origin", "speed"
    std::string value;     // a mode id, a figure, a gesture kind id, an anchor id, a speed; identity: a figure (sceneStartMS -1: every scene)
    std::string note;
};

struct StageEdits {
    int schemaVersion = STAGE_SCORE_SCHEMA_VERSION;
    std::vector<StageEdit> edits;
};

// The gestures a person can choose from, with their plain words; favourites are tried
// first and excluded ones never chosen automatically.
struct StageLibraryEntry {
    std::string id;    // a gesture kind id, or "local:<name>" for a figure's own composition
    std::string kind;  // "gesture", "local"
    std::string title; // plain words (to translate)
    std::string description;
    std::vector<std::string> functions; // the fields it uses
    std::vector<std::string> fits;      // topologies it suits; empty: any
    bool favorite = false;
    bool excluded = false;
    bool custom = false; // made by a person
    std::string source;  // "built-in", "person"
};

struct StageLibrary {
    int schemaVersion = STAGE_SCORE_SCHEMA_VERSION;
    std::vector<StageLibraryEntry> entries;
    [[nodiscard]] const StageLibraryEntry* Find(const std::string& id) const;
};

struct StageDirectorInput {
    const CompositionPlan* plan = nullptr;     // required: the scenes
    const SongAnalysis* analysis = nullptr;    // required: the moments
    const AudioUnderstanding* audio = nullptr; // optional: drops, re-entries, tails
    const VocalUnderstanding* voices = nullptr;
    const FigureCatalog* geometry = nullptr;   // required
    const FigureSemantics* semantics = nullptr;
    const StageSpace* space = nullptr;         // required
    const ModelCapabilities* capabilities = nullptr;
    const Choreography* choreography = nullptr; // the colours of each scene
    const StageEdits* edits = nullptr;
    const StageLibrary* library = nullptr;
};

[[nodiscard]] GlobalStageScore DirectStage(const StageDirectorInput& input);

// The score into the choreography: Stage Field events for every gesture (each figure
// joining at its own moment), identity layers for the figures composed whole, the
// layers of those figures moved down to make room, and the background of resting figures
// taken out where the scene asks for attention elsewhere. Returns notes for the report.
std::vector<std::string> RealizeStage(const GlobalStageScore& score, const StageDirectorInput& input, Choreography& choreography);

// xLights draws rows in order, a later row over an earlier one (blended, with model
// blending on). A group that holds a figure with a gesture is moved ahead of that figure, so
// the gesture's light lies over the group's effect and, where it is dark, lets it show.
// Returns how many groups were moved.
int PutGroupsUnderStage(SequenceSpec& spec, const Choreography& choreography, const ModelCapabilities& capabilities);

// The field one participant of a gesture draws, ready for the Stage Field effect.
[[nodiscard]] FieldSpec ParticipantField(const GlobalGesture& g, const GestureParticipant& p, const StageSpace& space, const FigureSemantics* semantics, int effectStartMS);

[[nodiscard]] const char* StageModeId(StageMode m);
[[nodiscard]] bool StageModeFromId(const std::string& id, StageMode& m);
[[nodiscard]] const char* GestureKindId(GestureKind k);
[[nodiscard]] bool GestureKindFromId(const std::string& id, GestureKind& k);
[[nodiscard]] std::string StageModePhrase(StageMode m);
[[nodiscard]] std::string GestureKindPhrase(GestureKind k);
[[nodiscard]] std::string SceneRolePhrase(const std::string& role);

[[nodiscard]] StageLibrary BuiltInStageLibrary();
// The built-in entries with a person's favourites, exclusions and own entries over them.
[[nodiscard]] StageLibrary MergeStageLibrary(const StageLibrary& personal);

[[nodiscard]] std::string ToJson(const GlobalStageScore& score);
[[nodiscard]] bool FromJson(const std::string& text, GlobalStageScore& score, std::string& error);
[[nodiscard]] std::string ToJson(const StageEdits& edits);
[[nodiscard]] bool FromJson(const std::string& text, StageEdits& edits, std::string& error);
[[nodiscard]] std::string ToJson(const StageLibrary& library);
[[nodiscard]] bool FromJson(const std::string& text, StageLibrary& library, std::string& error);
[[nodiscard]] std::string StageEditsPath(const std::string& showDir, const std::string& audioHash);
[[nodiscard]] std::string StageLibraryPath(const std::string& showDir);

} // namespace composer
