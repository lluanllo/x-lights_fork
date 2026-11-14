#pragma once

#include "composer/Choreography.h"
#include "composer/CompositionPlan.h"
#include "composer/GenerationSidecar.h"
#include "composer/LyricAligner.h"
#include "composer/ModelCapabilities.h"
#include "composer/SongAnalysis.h"
#include "composer/TemplateLibrary.h"

#include <string>
#include <vector>

// What a person reviews before and after generating, read from the report folder a
// pipeline run left behind. Nothing here decides anything: it only gathers and counts
// what the plan, the choreography and the render already said, for a UI to show.
namespace composer {

// One report folder: whichever of these the run got to write.
struct ReportFolder {
    std::string folder;
    bool hasAnalysis = false;
    bool hasLyrics = false;
    bool hasPlan = false;
    bool hasChoreography = false;
    bool hasCapabilities = false;
    bool hasSummary = false;
    SongAnalysis analysis;
    LyricAlignment alignment;
    CompositionPlan plan;
    Choreography choreography;
    ModelCapabilities capabilities;
    std::string summary; // summary.json as written
    std::string directionReport;      // visual-direction-report.txt, when the profile asked for one
    std::string direction;            // visual-direction.json as written
    std::string directionMetrics;     // direction-metrics.json as written
    std::string transcriptAlignment;  // transcript-alignment.json as written
    std::string media;                // media-plan.json as written: the pictures and videos chosen
    std::string storyboard;           // storyboard.json as written: the visual montage
    std::string screens;              // screens.json as written: the display's screens and their roles
    std::string review;               // review.json as written: a person's review of the pictures and videos
    std::string stageScore;           // stage-score.json as written: the stage director's score
    std::string stageLights;          // stage-lights.json as written: every light on the stage
    std::string stageCheck;           // stage-check.json as written: what the render did with the installation
};

// Reads every file it finds; an unreadable one is an error, a missing one is not.
[[nodiscard]] bool LoadReportFolder(const std::string& folder, ReportFolder& out, std::string& error);

// One automation level, in numbers the choreography produced.
struct LevelSummary {
    AutomationLevel level = AutomationLevel::Balanced;
    int events = 0;
    int figures = 0;     // targets with at least one event
    int planned = 0;     // targets the plan used
    int accents = 0;
    int templates = 0;
    int faces = 0;       // targets with a face effect
    int texts = 0;       // targets showing lyrics
    int mediaCues = 0;
    int needsReview = 0;
    int conflicts = 0;
    double meanIntensity = 0; // over the events, 0-1
    double averageActive = 0;
    std::string topTemplate;
    double topTemplateShare = 0;
};

[[nodiscard]] LevelSummary SummarizeLevel(const ReportFolder& report);

// One figure, as a person asks about it.
struct FigureSummary {
    std::string name;
    TargetType type = TargetType::Model;
    Role role = Role::Prop;
    RoleStatus roleStatus = RoleStatus::Suggested;
    bool excluded = false;
    bool noController = false;
    bool hasFace = false;   // has a face definition with something to draw
    bool canShowText = false;
    bool receivesFace = false;
    bool receivesText = false;
    int leadSegments = 0;
    int supportSegments = 0;
    int accentSegments = 0;
    int events = 0;
    std::vector<std::string> effects; // distinct native effects, in order of first use
    std::vector<std::pair<std::string, Compatibility>> templates; // the library templates it can take
    bool locked = false;          // a lock correction holds one of its events
    bool excludedByPerson = false; // an exclusion correction names it
    std::vector<std::string> conflicts; // reasons a correction on it was refused
    std::vector<std::string> notes;     // why the choreography gave it nothing, as stable codes
};

// Every target the plan used, or that a correction names, plus the excluded ones.
// `library` may be null: then no template list.
[[nodiscard]] std::vector<FigureSummary> SummarizeFigures(const ReportFolder& report, const TemplateLibrary* library, const CorrectionSet& corrections);

// Locks every event the choreography gave `target`, so that they stand as they are.
[[nodiscard]] std::vector<ChoreographyCorrection> LockFigure(const Choreography& choreography, const std::string& target);

// Removes the corrections that name `target` of `action`; returns how many.
int RemoveCorrections(CorrectionSet& corrections, const std::string& target, bool locks, bool exclusions);

} // namespace composer
