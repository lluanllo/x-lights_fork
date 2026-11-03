#pragma once

#include <string>
#include <utility>
#include <vector>

// Plain data describing a sequence to write. Holds no xLights objects, so a
// spec can be built and compared without a show loaded.
namespace composer {

struct TimingMark {
    std::string label;
    int startMS = 0;
    int endMS = 0;
};

struct TimingTrackSpec {
    std::string name;
    std::vector<std::vector<TimingMark>> layers;
};

enum class TargetKind {
    Any,
    Model,
    Group
};

struct EffectSpec {
    std::string target;   // model or group name
    std::string subModel; // empty when the effect goes on the target itself
    int layer = 0;
    std::string effect;
    // Raw values: the writer escapes them through SettingsMap.
    std::vector<std::pair<std::string, std::string>> settings;
    std::string palette;
    int startMS = 0;
    int endMS = 0;
    TargetKind kind = TargetKind::Any; // what the plan expects `target` to be
};

// A pending audiovisual resource: it reserves an interval on a target, which must
// stay dark until the resource is supplied. Lost with the sidecar without
// invalidating the sequence, so an empty list is always acceptable.
struct MarkerSpec {
    std::string id;
    std::string target;
    int startMS = 0;
    int endMS = 0;
};

struct SequenceSpec {
    int frameMS = 25;
    int durationMS = 0;
    std::vector<TimingTrackSpec> timingTracks;
    std::vector<EffectSpec> effects;
    std::vector<MarkerSpec> markers;
    bool modelBlending = true; // a new sequence defaults to true in SequenceElements
    std::string layoutFingerprint; // empty: not recorded
    std::string mediaFile; // the song; empty: an animation with no audio
};

enum class IssueSeverity {
    Info,
    Warning,
    Error
};

enum class IssueCategory {
    Structural,
    Configuration,
    Render
};

enum class CheckStatus {
    NotRun,
    Passed,
    Failed,
    Cancelled
};

// Everything a UI needs to word the problem in its own language: `code` is stable,
// and DescribeIssue's template is the msgid. No translated text belongs here.
struct ValidationIssue {
    std::string code;
    IssueSeverity severity = IssueSeverity::Error;
    IssueCategory category = IssueCategory::Structural;
    std::string model;
    std::string effect;
    int startMS = -1;
    int endMS = -1;
    std::vector<std::pair<std::string, std::string>> args;
};

// Measured from the .fseq, so a report can show what was seen and not only a verdict.
struct RenderMetric {
    std::string target;
    std::string effect;
    int startMS = 0;
    int endMS = 0;
    int framesWithLight = 0;
    int distinctFrames = 0;
    int maxChannelsLit = 0;
    int channels = 0;
};

struct ValidationReport {
    std::vector<ValidationIssue> issues;
    std::vector<RenderMetric> metrics;
    bool cancelled = false;
    bool structuralRan = false;
    bool configurationRan = false;
    bool renderRan = false;

    [[nodiscard]] int ErrorCount() const;
    [[nodiscard]] int WarningCount() const;
    [[nodiscard]] bool Ok() const { return !cancelled && ErrorCount() == 0; }
    [[nodiscard]] bool Has(const std::string& code) const;
    [[nodiscard]] const ValidationIssue* Find(const std::string& code) const;
    [[nodiscard]] CheckStatus Status(IssueCategory category) const;
    // Nothing in the core looks at the result, so this can never be Passed: only a
    // person, or a regression over a known fixture, can say that.
    [[nodiscard]] CheckStatus VisualStatus() const { return CheckStatus::NotRun; }
    void Merge(const ValidationReport& other);
};

struct WriteResult {
    bool ok = false;
    std::string finalPath;
    std::vector<std::string> problems;
    std::vector<std::string> leftoverFiles;
    ValidationReport validation;
};

} // namespace composer
