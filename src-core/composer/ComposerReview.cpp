#include "composer/ComposerReview.h"

#include "utils/ExternalHooks.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace composer {

namespace {

bool ReadIfThere(const fs::path& path, std::string& text) {
    if (!FileExists(path.string())) {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    std::ostringstream content;
    content << in.rdbuf();
    text = content.str();
    return true;
}

} // namespace

bool LoadReportFolder(const std::string& folder, ReportFolder& out, std::string& error) {
    ReportFolder r;
    r.folder = folder;
    const fs::path dir(folder);
    std::string text;
    std::string why;
    const auto fail = [&](const char* file) {
        error = fmt::format("{}: {}", file, why);
        return false;
    };
    if (ReadIfThere(dir / "analysis.json", text)) {
        if (!FromJson(text, r.analysis, why)) {
            return fail("analysis.json");
        }
        r.hasAnalysis = true;
    }
    if (ReadIfThere(dir / "lyrics-alignment.json", text)) {
        if (!FromJson(text, r.alignment, why)) {
            return fail("lyrics-alignment.json");
        }
        r.hasLyrics = true;
    }
    if (ReadIfThere(dir / "composition-plan.json", text)) {
        if (!FromJson(text, r.plan, why)) {
            return fail("composition-plan.json");
        }
        r.hasPlan = true;
    }
    if (ReadIfThere(dir / "choreography.json", text)) {
        if (!FromJson(text, r.choreography, why)) {
            return fail("choreography.json");
        }
        r.hasChoreography = true;
    }
    if (ReadIfThere(dir / "capabilities.json", text)) {
        if (!FromJson(text, r.capabilities, why)) {
            return fail("capabilities.json");
        }
        r.hasCapabilities = true;
    }
    (void)ReadIfThere(dir / "visual-direction-report.txt", r.directionReport);
    (void)ReadIfThere(dir / "visual-direction.json", r.direction);
    (void)ReadIfThere(dir / "direction-metrics.json", r.directionMetrics);
    (void)ReadIfThere(dir / "transcript-alignment.json", r.transcriptAlignment);
    (void)ReadIfThere(dir / "media-plan.json", r.media);
    (void)ReadIfThere(dir / "storyboard.json", r.storyboard);
    (void)ReadIfThere(dir / "screens.json", r.screens);
    (void)ReadIfThere(dir / "review.json", r.review);
    (void)ReadIfThere(dir / "stage-score.json", r.stageScore);
    (void)ReadIfThere(dir / "stage-lights.json", r.stageLights);
    (void)ReadIfThere(dir / "stage-check.json", r.stageCheck);
    if (ReadIfThere(dir / "summary.json", r.summary)) {
        r.hasSummary = true;
    }
    out = std::move(r);
    return true;
}

LevelSummary SummarizeLevel(const ReportFolder& report) {
    LevelSummary s;
    const Choreography& c = report.choreography;
    s.level = c.level;
    s.events = static_cast<int>(c.events.size());
    std::set<std::string> figures;
    std::set<std::string> faces;
    std::set<std::string> texts;
    double intensity = 0;
    for (const ChoreographyEvent& e : c.events) {
        figures.insert(e.target);
        intensity += e.intensity;
        if (e.effect == "Faces") {
            faces.insert(e.target);
        } else if (e.effect == "Text") {
            texts.insert(e.target);
        }
    }
    std::set<std::string> planned;
    for (const PlanSegment& seg : report.plan.segments) {
        for (const PlanAssignment& a : seg.assignments) {
            if (a.prominence != Prominence::Rest) {
                planned.insert(a.target);
            }
        }
    }
    s.figures = static_cast<int>(figures.size());
    s.planned = static_cast<int>(planned.size());
    s.faces = static_cast<int>(faces.size());
    s.texts = static_cast<int>(texts.size());
    s.meanIntensity = s.events > 0 ? intensity / s.events : 0;
    s.accents = c.metrics.accents;
    s.templates = c.metrics.distinctTemplates;
    s.mediaCues = static_cast<int>(c.mediaCues.size());
    s.needsReview = c.metrics.needsReview;
    s.conflicts = static_cast<int>(c.conflicts.size() + report.plan.conflicts.size());
    s.averageActive = c.metrics.averageActive;
    s.topTemplate = c.metrics.topTemplate;
    s.topTemplateShare = c.metrics.topTemplateShare;
    return s;
}

std::vector<FigureSummary> SummarizeFigures(const ReportFolder& report, const TemplateLibrary* library, const CorrectionSet& corrections) {
    std::map<std::string, FigureSummary> byName;
    const auto figure = [&](const std::string& name) -> FigureSummary& {
        FigureSummary& f = byName[name];
        if (f.name.empty()) {
            f.name = name;
            if (const TargetInfo* t = report.capabilities.Find(name)) {
                f.type = t->type;
                if (const RoleAssignment* r = t->PrimaryRole()) {
                    f.role = r->role;
                    f.roleStatus = r->status;
                }
                f.excluded = t->Excluded();
                f.noController = t->noController;
                f.hasFace = !t->faces.empty();
                f.canShowText = t->Can(Capability::Text).support == Support::Yes || t->Can(Capability::Text).support == Support::Limited;
                if (library != nullptr) {
                    for (const EffectTemplate& tmpl : library->templates) {
                        const CompatibilityResult compat = EvaluateCompatibility(tmpl, *t);
                        if (compat.level != Compatibility::Incompatible) {
                            f.templates.emplace_back(tmpl.id, compat.level);
                        }
                    }
                }
            }
        }
        return f;
    };
    for (const PlanSegment& seg : report.plan.segments) {
        for (const PlanAssignment& a : seg.assignments) {
            if (a.prominence == Prominence::Rest) {
                continue;
            }
            FigureSummary& f = figure(a.target);
            f.leadSegments += a.prominence == Prominence::Lead ? 1 : 0;
            f.supportSegments += a.prominence == Prominence::Support ? 1 : 0;
            f.accentSegments += a.prominence == Prominence::Accent ? 1 : 0;
        }
    }
    for (const ChoreographyEvent& e : report.choreography.events) {
        FigureSummary& f = figure(e.target);
        ++f.events;
        if (std::find(f.effects.begin(), f.effects.end(), e.effect) == f.effects.end()) {
            f.effects.push_back(e.effect);
        }
        f.receivesFace = f.receivesFace || e.effect == "Faces";
        f.receivesText = f.receivesText || e.effect == "Text";
        f.locked = f.locked || e.state == DecisionState::Locked;
    }
    for (const TargetInfo& t : report.capabilities.targets) {
        if (t.Excluded() && t.type == TargetType::Model) {
            (void)figure(t.name);
        }
    }
    for (const ChoreographyCorrection& c : corrections.choreography) {
        FigureSummary& f = figure(c.action == ChoreographyCorrection::Action::Lock ? c.event.target : c.target);
        f.locked = f.locked || c.action == ChoreographyCorrection::Action::Lock;
        f.excludedByPerson = f.excludedByPerson || (c.action == ChoreographyCorrection::Action::Exclude && c.startMS < 0);
    }
    for (const PlanCorrection& c : corrections.plan) {
        FigureSummary& f = figure(c.target);
        f.excludedByPerson = f.excludedByPerson || (c.action == PlanCorrection::Action::Exclude && c.startMS < 0);
    }
    for (const ChoreographyConflict& c : report.choreography.conflicts) {
        figure(c.target).conflicts.push_back(c.reason);
    }
    for (const PlanConflict& c : report.plan.conflicts) {
        figure(c.target).conflicts.push_back(c.reason);
    }
    // "code:<target>:<intent>", the target being everything between the first and last colon.
    for (const std::string& w : report.choreography.warnings) {
        const size_t first = w.find(':');
        const size_t last = w.rfind(':');
        if (first == std::string::npos || last == first) {
            continue;
        }
        FigureSummary& f = figure(w.substr(first + 1, last - first - 1));
        const std::string code = w.substr(0, first);
        if (std::find(f.notes.begin(), f.notes.end(), code) == f.notes.end()) {
            f.notes.push_back(code);
        }
    }
    for (const MediaCue& m : report.choreography.mediaCues) {
        FigureSummary& f = figure(m.target);
        if (std::find(f.notes.begin(), f.notes.end(), "media-cue") == f.notes.end()) {
            f.notes.push_back("media-cue");
        }
    }
    std::vector<FigureSummary> out;
    for (auto& [name, f] : byName) {
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<ChoreographyCorrection> LockFigure(const Choreography& choreography, const std::string& target) {
    std::vector<ChoreographyCorrection> locks;
    for (const ChoreographyEvent& e : choreography.events) {
        if (e.target != target || e.state == DecisionState::Locked) {
            continue;
        }
        ChoreographyCorrection c;
        c.action = ChoreographyCorrection::Action::Lock;
        c.target = target;
        c.event = e;
        locks.push_back(std::move(c));
    }
    return locks;
}

int RemoveCorrections(CorrectionSet& corrections, const std::string& target, bool locks, bool exclusions) {
    const size_t before = corrections.choreography.size() + corrections.plan.size();
    corrections.choreography.erase(std::remove_if(corrections.choreography.begin(), corrections.choreography.end(),
                                                  [&](const ChoreographyCorrection& c) {
                                                      const bool isLock = c.action == ChoreographyCorrection::Action::Lock;
                                                      const std::string& name = isLock ? c.event.target : c.target;
                                                      return name == target && ((locks && isLock) || (exclusions && c.action == ChoreographyCorrection::Action::Exclude));
                                                  }),
                                   corrections.choreography.end());
    if (exclusions) {
        corrections.plan.erase(std::remove_if(corrections.plan.begin(), corrections.plan.end(), [&](const PlanCorrection& c) { return c.target == target && c.action == PlanCorrection::Action::Exclude; }), corrections.plan.end());
    }
    return static_cast<int>(before - corrections.choreography.size() - corrections.plan.size());
}

} // namespace composer
