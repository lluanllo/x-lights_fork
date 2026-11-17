#pragma once

#include "composer/GenerationValidator.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

// Before a show goes out: everything a generated sequence needs to play as it was made, checked
// in one place and worded for the person who will switch it on. The audio and its length, the
// lyrics, the song's structure, the layout it was made for, the figures and controllers, the
// pictures and videos and whether their paths travel with the show, the gestures, recipes and
// colours, the effects' times, a person's locks, the review and the render. Every finding says
// how much it matters: what stops the show, what may make it wrong, what to look at, and what is
// only an artistic suggestion. Nothing here approves anything: only a person does, after seeing
// it in xLights. No controller is switched on and no file of the show is changed.
namespace composer {

inline constexpr int PRODUCTION_SCHEMA_VERSION = 1;

struct ProductionIssue {
    std::string level;  // "blocker" (cannot go out), "risk" (may be wrong), "warning", "artistic"
    std::string area;   // one of ProductionAreas()
    std::string code;   // stable
    std::string text;   // English template, the msgid; "{}" filled from args, in order
    std::vector<std::string> args;
};

struct ProductionCheckItem {
    std::string area;
    std::string status; // "ok", "warning", "problem", "blocked", "not-checked"
    std::string summary; // English template ("{}" from args)
    std::vector<std::string> args;
};

struct ProductionMedia {
    std::string setting;  // the effect setting that names it
    std::string target;
    int startMS = 0;
    std::string path;     // as the sequence names it
    std::string resolved; // where it was found ("" not found)
    bool travels = false; // inside the show folder or a media folder: it moves with the show
};

// Two figures wired to the same lights, as the show uses them (phase 21).
struct ProductionSharedLights {
    std::string a, b;
    int lights = 0;            // lights in common
    std::string relation;      // "a-inside-b", "b-inside-a", "same", "partial"
    int bothLitMS = 0;         // how long both get effects at once (their own, or through a group)
    std::string drawnLast;     // the one whose row xLights draws later: it shows on the shared lights
    std::string priority;      // the one a person chose to win ("" none)
    bool complementary = false; // never lit at once: no conflict, they take turns
};

// What a person can rely on, kept apart: the machine's checks never become an approval.
struct ProductionStatus {
    bool generated = false;     // made by the composer (its notes are there)
    bool technical = false;     // opened, validated, nothing that stops it
    bool rendered = false;      // the native renderer made it, with no error
    bool renderMatches = false; // and the same as the .fseq saved with it
    bool mediaComplete = false; // every picture and video found
    bool roundTrip = false;     // saved again by xLights and read back the same
    bool fseqFinal = false;     // a render (.fseq) where xLights plays it from, checked frame by frame against the sequence
    bool packaged = false;      // a production folder was made
    std::string review = "pending"; // the person's: "pending", "in-progress", "approved"
    std::string reviewer;
    std::string reviewedAt;
};

struct ProductionReport {
    int schemaVersion = PRODUCTION_SCHEMA_VERSION;
    bool ran = false;
    bool cancelled = false;
    std::string sequence;
    std::string sidecar;
    std::string fseq;
    std::string audio;
    int durationMS = 0;
    std::string xlightsVersion;
    std::string layoutFingerprint;
    std::string specHash;
    std::vector<ProductionCheckItem> checks;
    std::vector<ProductionIssue> issues;
    std::vector<ProductionMedia> media;
    std::vector<ProductionSharedLights> sharedLights;
    std::map<std::string, std::string> hashes; // file -> SHA-256
    int channels = 0;        // the layout's channels
    int models = 0;
    int controllers = 0;
    int controllersActive = 0;
    ProductionStatus status;

    [[nodiscard]] int Count(const std::string& level) const;
    [[nodiscard]] bool CanGoOut() const { return ran && !cancelled && Count("blocker") == 0; }
    // Ready for the real installation: only when a person approved it, and the checked render,
    // media and technical checks are all there. Passing the checks alone never makes it ready.
    [[nodiscard]] bool ReadyForInstallation() const {
        return CanGoOut() && status.review == "approved" && status.technical && status.fseqFinal && status.mediaComplete;
    }
    [[nodiscard]] const ProductionCheckItem* Check(const std::string& area) const;
};

struct ProductionInput {
    std::string showDir;
    std::string sequencePath; // the .xsq
    std::string fseqPath;     // "" : <sequence>.fseq beside it, else the show's
    bool render = true;       // render again with the native renderer
    bool roundTrip = true;    // save again and read back
    std::string workDir;      // where the checks write their files ("" : a folder beside the sequence's report)
    int renderTimeoutMS = 0;
    CancelCheck cancel;
    std::function<void(const std::string& what)> progress;
};

// The areas in the order a person reads them.
[[nodiscard]] const std::vector<std::string>& ProductionAreas();

[[nodiscard]] ProductionReport CheckProduction(const ProductionInput& input);

// A folder with what the show needs to be produced elsewhere: the sequence, its notes, the
// pictures and videos that are not already where the show will be, the lyrics, the check's
// report, the layout's description (not the layout), the xLights version, every file's hash
// and the review state. Nothing is overwritten: an existing folder gets a new name. The
// audio is referred to (name, size, hash), not copied, unless `copyAudio`.
struct ProductionPackageInput {
    std::string showDir;
    std::string sequencePath;
    std::string destination; // "" : <show>/ShowComposer/produccion/<sequence>
    bool copyAudio = false;
    const ProductionReport* report = nullptr; // the check to include (recommended)
};

struct ProductionPackage {
    bool ok = false;
    std::string folder;
    std::vector<std::string> copied;   // relative to the folder
    std::vector<std::string> referred; // files named but not copied (already where the show is)
    std::vector<std::string> problems;
};

[[nodiscard]] ProductionPackage BuildProductionPackage(const ProductionPackageInput& input);

// The final render (phase 21): the sequence opened as xLights opens it, rendered by the native
// renderer and written where xLights plays it from (the show's render folder, as the desktop
// saves it), then read back and checked: its header, channels, frames, frame time, length and
// audio against the sequence and the layout, and every frame against what was rendered. An
// earlier render there is kept aside (never overwritten); a render that fails a check is not
// left in its place. No controller is switched on.
struct FinalRenderInput {
    std::string showDir;
    std::string sequencePath;
    std::string destination; // "" : where xLights saves it (the show's render folder)
    int renderTimeoutMS = 0;
    CancelCheck cancel;
    std::function<void(const std::string& what)> progress;
};

struct FinalRender {
    bool ok = false;
    std::string fseq;     // where it is
    std::string previous; // where an earlier render was kept ("" there was none)
    std::string sha256;
    int channels = 0, layoutChannels = 0;
    int frames = 0, frameMS = 0, durationMS = 0, sequenceDurationMS = 0;
    std::string media, sequenceMedia;
    std::string specHash;
    bool channelsMatch = false, timingMatches = false, framesMatch = false, mediaMatches = false;
    std::vector<std::string> problems; // plain words keys ("{}" from problemArgs)
};

[[nodiscard]] FinalRender RenderFinal(const FinalRenderInput& input);
[[nodiscard]] std::string ToJson(const FinalRender& render);
// Whether two renders hold the same light, frame by frame (their files may differ in what they embed).
[[nodiscard]] bool SameFrames(const std::string& fseqA, const std::string& fseqB, std::string& why);
// --composer-final-render
[[nodiscard]] int RunFinalRenderCommand(const std::string& showDir, const std::string& sequencePath, const std::string& outDir);

using ProductionTranslate = std::function<std::string(const char* msgid)>;
// The template filled with its arguments, translated first when `translate` is given.
[[nodiscard]] std::string FillProductionText(const std::string& text, const std::vector<std::string>& args, const ProductionTranslate& translate = {});
[[nodiscard]] std::string ProductionLevelPhrase(const std::string& level);  // English, a msgid
[[nodiscard]] std::string ProductionAreaPhrase(const std::string& area);    // English, a msgid
[[nodiscard]] std::string ProductionReviewPhrase(const std::string& review); // English, a msgid
// The whole report in plain words, for a person and for the package.
[[nodiscard]] std::string ProductionText(const ProductionReport& report, const ProductionTranslate& translate = {});

[[nodiscard]] std::string ToJson(const ProductionReport& report);
[[nodiscard]] bool FromJson(const std::string& text, ProductionReport& report, std::string& error);

// --composer-production: the check (and the package with `packageDir`), into `outDir`.
[[nodiscard]] int RunProductionCommand(const std::string& showDir, const std::string& sequencePath, const std::string& outDir, const std::string& packageDir, bool render);

} // namespace composer
