#pragma once

#include "composer/Choreography.h"
#include "composer/CompositionPlan.h"
#include "composer/LyricAligner.h"
#include "composer/MediaDirection.h"
#include "composer/SongAnalysis.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// What the Composer keeps next to a generated sequence, <name>.composer.json: how it
// was made, so that it can be identified and made again, and every decision a person
// took, so that making it again never loses one. The sequence itself only holds native
// xLights objects and stays valid if this file is lost, moved or damaged.
namespace composer {

inline constexpr int SIDECAR_SCHEMA_VERSION = 1;

struct GenerationSidecar {
    int schemaVersion = SIDECAR_SCHEMA_VERSION;
    std::string pipelineVersion;
    std::string analyzerVersion;
    std::string plannerVersion;
    std::string choreographerVersion;
    std::string generationId; // the same inputs and versions give the same id
    std::string sequenceFile; // the .xsq it belongs to, by file name
    std::string audioFile;
    std::string audioPath; // where it was read from, to find it again on this machine
    std::string audioHash; // SHA-256 of the audio's bytes
    long audioSize = 0;
    std::string analysisOptionsKey;
    std::string analysisHash;
    std::string layoutFingerprint;
    std::string profileId;
    std::string profileHash;
    std::string libraryHash;
    std::string planHash;
    std::string choreographyHash;
    std::string specHash; // what was written, independent of the XML's formatting
    std::string lyricsHash;
    std::string lyricsQuality; // LyricAlignment::quality, "none" without lyrics
    AutomationLevel level = AutomationLevel::Balanced;
    uint64_t seed = 0;
    int frameMS = 25;
    int durationMS = 0;
    int effects = 0;
    std::map<std::string, std::string> tracks; // stable id -> the name it has in the sequence
    std::vector<MediaCue> mediaCues;
    std::vector<PlanCorrection> planCorrections;
    std::vector<ChoreographyCorrection> choreographyCorrections;
    AnalysisCorrections analysisCorrections; // beats, bars and sections a person fixed
    LyricSession lyrics; // the text as given and its manual marks, synchronized or not
    int validationErrors = 0;
    int validationWarnings = 0;
    bool rendered = false;
    std::string visualReview = "not-reviewed"; // only a person changes it
    // Answers of the music assistant this show used, kept in the show's cache so a
    // regeneration finds them with no network. Relative to the show folder.
    std::string transcriptCache;
    std::string characterCache;
    // The media folders, a person's picture and video choices, and what plays where.
    MediaSession media;
};

enum class SidecarStatus {
    Ok,
    Missing,
    Damaged,     // not JSON, or a required field is missing or of the wrong type
    Newer,       // written by a later schema: not read, never rewritten
    Incompatible // read, but made for another song
};

[[nodiscard]] std::string ToJson(const GenerationSidecar& sidecar);
[[nodiscard]] SidecarStatus FromJson(const std::string& text, GenerationSidecar& sidecar, std::string& error);
[[nodiscard]] SidecarStatus ReadSidecar(const std::string& path, GenerationSidecar& sidecar, std::string& error);
// Incompatible when `audioHash` is not empty and differs from the sidecar's.
[[nodiscard]] SidecarStatus ReadSidecarFor(const std::string& path, const std::string& audioHash, GenerationSidecar& sidecar, std::string& error);
[[nodiscard]] const char* SidecarStatusId(SidecarStatus status);
// "<stem>.composer.json" beside "<stem>.xsq".
[[nodiscard]] std::string SidecarPathFor(const std::string& sequencePath);

// The corrections a person can hand over in a file of their own, in the same form the
// sidecar keeps them: { "plan": [...], "choreography": [...], "lyrics": [...], "analysis": {...} }.
struct CorrectionSet {
    std::vector<PlanCorrection> plan;
    std::vector<ChoreographyCorrection> choreography;
    std::vector<LyricCorrection> lyrics;
    AnalysisCorrections analysis;
    std::vector<MediaCorrection> media;
    std::vector<VoiceDef> voices; // a person's names, roles and faces for the voices
    ScreenSettings screens;       // a person's screens and how words and pictures share them

    [[nodiscard]] bool Empty() const;
};

// Adds `extra` to `into`: lists are appended, and a bar correction in `extra` replaces one in `into`.
void MergeCorrections(CorrectionSet& into, const CorrectionSet& extra);
void MergeAnalysisCorrections(AnalysisCorrections& into, const AnalysisCorrections& extra);

[[nodiscard]] std::string ToJson(const CorrectionSet& corrections);
[[nodiscard]] bool FromJson(const std::string& text, CorrectionSet& corrections, std::string& error);

} // namespace composer
