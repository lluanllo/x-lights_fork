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
inline constexpr const char* STAGE_DIRECTOR_ART_VERSION = "2.0.0";
inline constexpr const char* STAGE_DIRECTOR_ORCHESTRA_VERSION = "3.0.0";

// How far the director develops the song's visual ideas (phase 20) and how much of the
// installation it really commands (phase 21). Each level adds to the one before. F19 is the
// director as it was: an older show regenerates the same, and a comparison can change only this.
enum class ArtLevel {
    F19 = 0,          // scenes chosen one by one
    Direction = 1,    // + a role in the song for each scene, figures that answer instead of all at once, gestures seen as much as they matter
    Motifs = 2,       // + visual motifs: an idea (who, which gesture, from where, in what order) that comes back
    Full = 3,         // + the idea developed: participation that grows and shrinks, a peak prepared, an ending that closes
    Compositions = 4, // + each scene built on a composition of the library, chosen for the whole of its circumstances
    Presence = 5,     // + every figure's own effects follow what the scene asks of it: lead, accompany, answer, glow or rest
    Orchestra = 6     // + the compositions linked: figures join, gather and fade at the joins instead of cutting
};

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
    // How the colours spread over the gesture (FieldSpec::colorBy): "field", "distance",
    // "trail" or "time". The colour direction sets it with the colours; "field" before it.
    std::string colorBy = "field";
    std::string colorReason;
    std::string trigger; // the moment of the music that starts it: "<kind>@<ms>"
    std::string related; // a gesture it answers or echoes
    double confidence = 0;
    std::vector<std::string> reasons;
    bool manual = false;
    // Phase 20 (ArtLevel Direction and above; empty before):
    std::string relation; // how its figures follow in time: "unison", "answer", "propagation", "sequence", "overlap", "continuation", "anticipation", "progressive-end"
    std::string presence; // how much it should be seen: "background", "response", "protagonist", "climax"
    std::string motif;    // the visual motif it carries ("" none)
    // Phase 21 (ArtLevel Compositions and above):
    std::string composition; // the library composition it realises ("" none)
};

// One appearance of a visual motif: what a scene does with it.
struct MotifVariant {
    std::string scene;
    int startMS = 0;
    std::string operation; // "present", "hold", "develop", "intensify", "reduce", "transform", "split", "recall", "resolve"
    std::string narrative; // the scene's role in the song
    std::vector<std::string> figures;
    std::string gesture;   // the gesture id that carries it here ("" none)
    std::string kind;      // its gesture kind here
    double level = 1;
    std::string why;
};

// A visual idea, not a colour: who carries it, with which gesture, from where on the stage,
// in what order in time, dressed in which colour motif, and every scene that brings it back,
// develops it, transforms it or closes it.
struct VisualMotif {
    std::string id;          // "M1", "M2"...
    std::string intent;      // what it says, plain words key
    std::string origin;      // where it was born: "scene:<id>"
    std::string composition; // the composition it was built from ("" none)
    std::string protagonist;
    std::vector<std::string> companions;
    std::vector<std::string> figures;           // everyone carrying it when presented
    std::map<std::string, std::string> recipes; // figure -> its lead effect when presented
    std::string gestureKind; // the gesture that carries it ("" none yet)
    std::string movement;    // the field it draws
    std::string temporal;    // "unison", "answer", "propagation", "sequence", "overlap"
    std::string anchor;      // where on the stage it starts
    std::string spatial;     // plain words: "from Mega Tree outwards", "left to right"...
    std::string colorMotif;  // the colour direction's motif dressing it
    std::vector<std::string> triggers; // the moments of the music that start it
    std::string entry;       // "from-one", "together", "grows"
    std::string exit;        // "cut", "fades", "gathers"
    std::vector<std::string> constraints;
    std::vector<MotifVariant> variants;
    std::string explanation;
};

// What each figure is in a scene.
struct SceneRole {
    std::string figure;
    std::string role; // "protagonist", "complement", "responder", "accent", "background", "rest"
    std::string why;
};

// A composition the selector weighed for a scene, and what it found (phase 21).
struct CompositionCandidate {
    std::string id;
    double score = 0;
    std::string verdict;              // "chosen", "possible", "rejected"
    std::vector<std::string> reasons; // plain-words keys: "+<why>" counted for, "-<why>" against, "x<why>" rules it out
    int figures = 0;                  // how many figures it would use here
};

// What a figure does in a scene, and how much of its own activity the scene lets it keep.
// The state is what the score asks of it; the policy is what is done to its effects.
struct FigurePresence {
    std::string figure;
    std::string state;  // "protagonist", "complement", "response", "ambient", "rest", "protected"
    std::string policy; // "normal", "reduced", "suspended", "protected"
    std::string why;    // plain-words key
};

// How one composition hands over to the next (phase 21, ArtLevel Orchestra).
struct CompositionTransition {
    std::string from; // scene ids
    std::string to;
    int atMS = 0;
    std::string kind; // "continuation", "variation", "response", "propagation", "gathering", "transformation", "resolution", "cut"
    std::string why;  // plain-words key
    std::vector<std::string> joining; // figures that come in here
    std::vector<std::string> leaving; // figures that go out here
    int softened = 0;                 // effects given a fade or a staggered start/end
    std::vector<std::string> checks;  // what the join was checked for: "abrupt-cut:<n>", "level-jump", ...
};

// Two figures wired to the same lights: what one shows, the other shows too.
struct SharedLights {
    std::string a, b;
    int lights = 0;          // lights in common
    std::string relation;    // "a-inside-b", "b-inside-a", "same", "partial"
    std::string priority;    // the figure whose decision wins on the shared lights ("" none chosen)
    std::string prioritySource; // "person", "presence", ""
    int scenesBothLit = 0;   // scenes where both would light their own effects
    int scenesResolved = 0;  // of those, scenes where one gave way
    std::string note;        // plain-words key
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
    // Phase 20 (ArtLevel Direction and above; empty before):
    std::string narrative;   // its role in the song: "presentation", "development", "contrast", "answer", "preparation", "climax", "intensification", "rest", "transformation", "recovery", "resolution"
    std::string attention;   // how many carry it: "one", "two", "few", "group", "all", "propagation", "rest"
    std::string visualMotif; // the visual motif it uses ("" none)
    std::string motifOperation;
    std::string accentFigure; // who takes the accents
    std::string changes;     // what changes from the scene before, plain words key
    std::string leadsTo;     // how it connects with the next, plain words key
    bool focus = false;      // the attention is on a few: the others' own effects rest
    // Phase 21 (ArtLevel Compositions and above; empty before):
    std::string composition;                      // the library composition the scene is built on ("" the director's own way)
    std::vector<std::string> compositionReasons;  // why, plain-words keys
    std::string compositionFallback;              // why none was used ("" one was)
    std::string compositionAdapted;               // how it was fitted to the figures there ("" as it is)
    std::vector<CompositionCandidate> candidates; // every composition weighed
    double ambience = 0;                          // ArtLevel Presence: share of the figures not taking part kept as a quiet glow
    std::vector<FigurePresence> presence;         // ArtLevel Presence: every figure on the stage
    [[nodiscard]] std::string RoleOf(const std::string& figure) const;
    [[nodiscard]] const FigurePresence* PresenceOf(const std::string& figure) const;
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
    int art = 0;                     // the ArtLevel it was directed with
    std::vector<VisualMotif> motifs; // ArtLevel Motifs and above
    // Phase 21:
    double ambientLevel = 0.4;                      // how bright the quiet glow of figures not taking part is (0-1)
    std::vector<CompositionTransition> transitions; // ArtLevel Orchestra
    std::vector<SharedLights> sharedLights;         // ArtLevel Presence
    std::vector<std::string> presenceNotes;         // what applying the presence did, for the report

    [[nodiscard]] const StageScene* SceneAt(int ms) const;
    [[nodiscard]] const GlobalGesture* Gesture(const std::string& id) const;
    [[nodiscard]] const VisualMotif* Motif(const std::string& id) const;
};

// A person's changes to the score, kept per song and applied after every new score.
struct StageEdit {
    // "mode", "include", "exclude", "no-gestures", "gesture", "origin", "speed", "identity", "no-identity";
    // phase 21: "composition" (value: a composition id for the scene, "none" the director's own way),
    // "priority" (value "<figure>|<other>": on lights they share, the first one's decision wins; sceneStartMS -1)
    std::string action;
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
    std::string id;    // a gesture kind id, "local:<name>" for a figure's own composition, "composition:<name>"
    std::string kind;  // "gesture", "local", "composition"
    std::string title; // plain words (to translate)
    std::string description;
    std::vector<std::string> functions; // the fields it uses
    std::vector<std::string> fits;      // topologies it suits; empty: any
    bool favorite = false;
    bool excluded = false;
    bool custom = false; // made by a person
    std::string source;  // "built-in", "person", "learned:<sequence>"
    // A composition (phase 20): a whole idea a scene can be built on, from existing gestures
    // and fields. Its contexts are where it suits, never where it must go.
    std::string level;                 // "local" (one figure), "group" (several), "global" (the whole house)
    std::vector<std::string> contexts; // "intro", "narration", "solo", "choir", "instrumental", "accent", "verse", "chorus", "pause", "climax", "transition", "ending"
    std::string gesture;               // the gesture kind that carries it
    std::string temporal;              // "unison", "answer", "propagation", "sequence", "overlap"
    int minFigures = 1;
    int maxFigures = 0;                // 0: as many as the scene gives
    std::vector<std::string> regions;  // parts of a figure it needs ("" any)
    std::vector<std::string> effects;  // native effects it needs (beyond Stage Field)
    std::vector<std::string> palette;  // the palette roles it uses: "anchor", "accent"...
    double level0 = 1;                 // how bright (0-1)
    double speed = 0;                  // stage units a second; 0: from the music
    int beats = 0;                     // how long, in beats; 0: from the music
    std::vector<std::string> figures;  // the figures it was made with (learned or a person's)
    std::vector<std::string> limits;   // plain words keys
    std::string variantOf;             // the composition it varies ("" none)
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
    ArtLevel art = ArtLevel::F19;
    // The interpretation's parts that keep the song's identity or may change (version 3):
    // a direction for which scenes transform, never a rule.
    const std::vector<std::string>* keepParts = nullptr;
    const std::vector<std::string>* changeParts = nullptr;
    // ArtLevel Presence: how bright the figures kept as a quiet glow are (0.05-1); a person's choice.
    double ambientLevel = 0.4;
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
[[nodiscard]] const char* ArtLevelId(ArtLevel a);       // "f19", "direction", "motifs", "full"
[[nodiscard]] bool ArtLevelFromId(const std::string& id, ArtLevel& a);
[[nodiscard]] std::string NarrativePhrase(const std::string& narrative); // plain words (to translate)
[[nodiscard]] std::string MotifOperationPhrase(const std::string& operation);
[[nodiscard]] std::string AttentionPhrase(const std::string& attention);
[[nodiscard]] std::string RelationPhrase(const std::string& relation);
// The built-in compositions that suit a context, favourites first, excluded ones left out.
[[nodiscard]] std::vector<const StageLibraryEntry*> CompositionsFor(const StageLibrary& library, const std::string& context);
// A visual idea of a song kept as a person's own composition (only when they ask): its
// figures, gesture, order in time and the kind of part it was born in.
[[nodiscard]] StageLibraryEntry CompositionFromMotif(const VisualMotif& motif, const GlobalStageScore& score, const std::string& song);

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
