#include "composer/ShowGenerationPipeline.h"

#include "composer/ArtisticPreferences.h"
#include "composer/MusicStructure.h"

#include "composer/SequenceWriter.h"
#include "composer/MediaFraming.h"
#include "composer/VisualModel.h"
#include "effects/EffectManager.h"
#include "render/FSEQFile.h"
#include "render/HeadlessRenderContext.h"
#include "render/SequenceElements.h"
#include "render/SequenceFile.h"
#include "utils/ExternalHooks.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <system_error>
#include <tuple>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

GenerationRun::GenerationRun() = default;
GenerationRun::~GenerationRun() = default;
GenerationRun::GenerationRun(GenerationRun&&) noexcept = default;
GenerationRun& GenerationRun::operator=(GenerationRun&&) noexcept = default;

namespace {

constexpr const char* STAGE_IDS[] = { "inputs", "layout", "analysis", "lyrics", "plan", "choreography", "spec", "write", "publish" };
constexpr const char* STATUS_IDS[] = { "not-run", "ok", "warning", "failed", "cancelled" };

long long ElapsedMS(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

bool ReadText(const std::string& path, std::string& text) {
    ObtainAccessToURL(path, false);
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream content;
    content << in.rdbuf();
    text = content.str();
    return true;
}

// Only for the report folder, which belongs to this run.
bool WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    return !out.fail();
}

// Creates `path` with `text`, failing rather than replacing anything already there:
// the content goes to a temporary first, and a hard link cannot land on a file.
bool PublishNewFile(const fs::path& path, const std::string& text, std::string& error) {
    fs::path temporary = path;
    temporary += ".composer-tmp";
    if (FileExists(temporary.string())) {
        error = fmt::format("a temporary file is in the way: '{}'", temporary.string());
        return false;
    }
    if (!WriteText(temporary, text)) {
        error = fmt::format("cannot write '{}'", temporary.string());
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    std::error_code ec;
    fs::create_hard_link(temporary, path, ec);
    std::error_code ignored;
    fs::remove(temporary, ignored);
    if (ec) {
        error = fmt::format("cannot create '{}': {}", path.string(), ec.message());
        return false;
    }
    return true;
}

std::string ProtectedHashes(const std::string& showDir) {
    std::string hashes;
    for (const char* name : { "xlights_rgbeffects.xml", "xlights_networks.xml" }) {
        const fs::path file = fs::path(showDir) / name;
        hashes += fmt::format("{}={};", name, FileExists(file.string()) ? Sha256OfFile(file.string()) : std::string("absent"));
    }
    return hashes;
}

bool IsDirectoryEmpty(const fs::path& dir) {
    std::error_code ec;
    return fs::is_empty(dir, ec) && !ec;
}

void Note(GenerationRun& run, GenerationStage stage, const std::string& code, const std::string& detail) {
    StageOutcome& s = run.Stage(stage);
    s.notes.push_back(detail.empty() ? code : code + ": " + detail);
    if (s.status == StageStatus::Ok) {
        s.status = StageStatus::Warning;
    }
}

bool Fail(GenerationRun& run, GenerationStage stage, const std::string& code, const std::string& detail) {
    const std::string text = detail.empty() ? code : code + ": " + detail;
    run.Stage(stage).notes.push_back(text);
    run.Stage(stage).status = StageStatus::Failed;
    run.problems.push_back(fmt::format("{}: {}", StageId(stage), text));
    return false;
}

bool CancelRequested(GenerationRun& run, GenerationStage stage) {
    if (run.request.cancel && run.request.cancel()) {
        run.cancelled = true;
        run.Stage(stage).status = StageStatus::Cancelled;
        run.Stage(stage).notes.push_back("cancelled");
        return true;
    }
    return false;
}

void Progress(const GenerationRun& run, GenerationStage stage, const std::string& what) {
    if (run.request.progress) {
        run.request.progress(stage, what);
    }
}

template <typename Body>
bool RunStage(GenerationRun& run, GenerationStage stage, Body body) {
    StageOutcome& s = run.Stage(stage);
    s = StageOutcome();
    s.status = StageStatus::Ok;
    if (CancelRequested(run, stage)) {
        return false;
    }
    Progress(run, stage, "start");
    const auto start = std::chrono::steady_clock::now();
    const bool ok = body();
    s.ms = ElapsedMS(start);
    if (!ok && s.status != StageStatus::Cancelled && s.status != StageStatus::Failed) {
        s.status = StageStatus::Failed;
    }
    Progress(run, stage, StageStatusId(s.status));
    return ok;
}

void AddValidationNotes(GenerationRun& run, GenerationStage stage, const ValidationReport& report, const char* what) {
    std::map<std::string, int> counts;
    for (const ValidationIssue& i : report.issues) {
        if (i.severity == IssueSeverity::Error) {
            run.Stage(stage).notes.push_back(fmt::format("{} error {}: {}", what, i.code, DescribeIssue(i)));
        } else {
            ++counts[i.code + (i.severity == IssueSeverity::Warning ? " (warning)" : " (info)")];
        }
    }
    for (const auto& [code, count] : counts) {
        run.Stage(stage).notes.push_back(fmt::format("{} {} x{}", what, code, count));
    }
    if (report.WarningCount() > 0 && run.Stage(stage).status == StageStatus::Ok) {
        run.Stage(stage).status = StageStatus::Warning;
    }
}

const TimingTrackSpec* PickLyricTrack(const std::vector<TimingTrackSpec>& tracks, const std::string& wanted) {
    for (const TimingTrackSpec& t : tracks) {
        if (!wanted.empty() ? t.name == wanted : t.layers.size() >= 3) {
            return &t;
        }
    }
    return nullptr;
}

std::string FullName(const EffectSpec& e) {
    return e.subModel.empty() ? e.target : e.target + "/" + e.subModel;
}

// What a choreography warning such as "no-template:<target>:beat-accent" says about one target.
std::map<std::string, std::string> WarningsByTarget(const Choreography& c) {
    std::map<std::string, std::string> notes;
    for (const std::string& w : c.warnings) {
        const size_t first = w.find(':');
        const size_t last = w.rfind(':');
        if (first == std::string::npos || last == first) {
            continue;
        }
        const std::string target = w.substr(first + 1, last - first - 1);
        std::string& note = notes[target];
        note += (note.empty() ? "" : ", ") + w.substr(0, first) + ":" + w.substr(last + 1);
    }
    for (const MediaCue& m : c.mediaCues) {
        std::string& note = notes[m.target];
        note += (note.empty() ? "" : ", ") + std::string("media-cue:") + m.kind;
    }
    return notes;
}

// Overlapping cues go on layers of their own: a timing layer holds no overlap.
std::vector<std::vector<TimingMark>> CueLayers(const std::vector<MediaCue>& cues) {
    std::vector<MediaCue> sorted = cues;
    std::sort(sorted.begin(), sorted.end(), [](const MediaCue& a, const MediaCue& b) { return a.startMS != b.startMS ? a.startMS < b.startMS : a.target < b.target; });
    std::vector<std::vector<TimingMark>> layers;
    for (const MediaCue& m : sorted) {
        TimingMark mark{ fmt::format("{} {}", m.kind, m.target), m.startMS, m.endMS };
        auto layer = std::find_if(layers.begin(), layers.end(), [&](const std::vector<TimingMark>& l) { return l.back().endMS <= mark.startMS; });
        if (layer == layers.end()) {
            layers.push_back({ mark });
        } else {
            layer->push_back(mark);
        }
    }
    return layers;
}

// What the choreography says the sequence must show, checked before anything is
// written: an error here is a sequence that would look wrong while rendering fine.
void CheckChoreographyAgainstSong(GenerationRun& run) {
    const Choreography& c = run.choreography;
    std::set<std::string> seen;
    int textOutside = 0;
    int missingParameters = 0;
    int inSilence = 0;
    for (const ChoreographyEvent& e : c.events) {
        const std::string key = fmt::format("{}|{}|{}|{}", e.target, e.layer, e.startMS, e.endMS);
        if (!seen.insert(key).second) {
            run.findings.push_back(fmt::format("error duplicate-effect: {} layer {} {}-{} ms", e.target, e.layer, e.startMS, e.endMS));
        }
        if (e.endMS > c.durationMS || e.startMS < 0) {
            run.findings.push_back(fmt::format("error outside-song: {} {}-{} ms of {}", e.target, e.startMS, e.endMS, c.durationMS));
        }
        if (e.provenance != "manual") {
            if (const EffectTemplate* t = run.library.Find(e.templateId)) {
                for (const TemplateParameter& p : t->parameters) {
                    const bool present = std::any_of(e.settings.begin(), e.settings.end(), [&](const auto& kv) { return kv.first == p.key; });
                    if (!present) {
                        ++missingParameters;
                        run.findings.push_back(fmt::format("error default-parameter: {} on {} lost {}", e.templateId, e.target, p.key));
                    }
                }
            }
        }
        if (e.effect == "Text") {
            std::string text;
            for (const auto& [k, v] : e.settings) {
                if (k == "E_TEXTCTRL_Text") {
                    text = v;
                }
            }
            const int slack = c.frameMS;
            const bool inPhrase = std::any_of(run.alignment.phrases.begin(), run.alignment.phrases.end(), [&](const AlignedPhrase& p) {
                return p.startMS >= 0 && p.text == text && e.startMS >= p.startMS - slack && e.endMS <= p.endMS + slack;
            });
            if (!inPhrase) {
                ++textOutside;
                run.findings.push_back(fmt::format("error text-out-of-time: \"{}\" on {} at {}-{} ms is not a synchronized phrase", text, e.target, e.startMS, e.endMS));
            }
        }
        if (e.effect == "Faces" && run.lyricTrack.layers.empty()) {
            run.findings.push_back(fmt::format("error face-without-track: {} at {} ms", e.target, e.startMS));
        }
    }
    for (const MusicalEvent& s : run.analysis.events) {
        if (s.type != MusicalEventType::Silence || s.durationMS < run.profile.minRestMS) {
            continue;
        }
        for (const ChoreographyEvent& e : c.events) {
            const int overlap = std::min(e.endMS, s.timeMS + s.durationMS) - std::max(e.startMS, s.timeMS);
            if (overlap > 2 * c.frameMS) {
                ++inSilence;
            }
        }
    }
    if (inSilence > 0) {
        run.findings.push_back(fmt::format("warning light-in-silence: {} events overlap a silence of the song by more than two frames", inSilence));
    }
    run.findings.push_back(fmt::format("info checked: {} events, {} text events outside their phrase, {} template parameters missing", c.events.size(), textOutside, missingParameters));
}

int FindingErrors(const std::vector<std::string>& findings) {
    return static_cast<int>(std::count_if(findings.begin(), findings.end(), [](const std::string& f) { return f.rfind("error ", 0) == 0; }));
}

void BuildCoverage(GenerationRun& run) {
    std::map<std::string, TargetCoverage> byTarget;
    for (const PlanSegment& s : run.plan.segments) {
        for (const PlanAssignment& a : s.assignments) {
            if (a.prominence != Prominence::Rest) {
                byTarget[a.target].plannedSegments++;
            }
        }
    }
    for (const ChoreographyEvent& e : run.choreography.events) {
        byTarget[e.target].events++;
    }
    for (const EffectSpec& e : run.spec.effects) {
        byTarget[FullName(e)].written++;
    }
    for (const RenderMetric& m : run.renderValidation.metrics) {
        TargetCoverage& t = byTarget[m.target];
        t.effectsLit += m.framesWithLight > 0 ? 1 : 0;
        t.framesLit += m.framesWithLight;
    }
    const std::map<std::string, std::string> notes = WarningsByTarget(run.choreography);
    run.coverage.clear();
    for (auto& [name, t] : byTarget) {
        t.target = name;
        if (auto n = notes.find(name); n != notes.end()) {
            t.note = n->second;
        }
        run.coverage.push_back(t);
    }
}

void ReadRenderSummary(GenerationRun& run, HeadlessRenderContext& ctx, const std::string& fseq) {
    RenderSummary& r = run.render;
    std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(fseq));
    if (file == nullptr) {
        return;
    }
    r.frames = static_cast<int>(file->getNumFrames());
    r.stepMS = file->getStepTime();
    r.channels = static_cast<uint32_t>(file->getChannelCount());
    r.layoutChannels = static_cast<uint32_t>(ctx.GetSeqData().NumChannels());
    r.expectedFrames = run.spec.durationMS / std::max(1, run.spec.frameMS);
    std::error_code ec;
    r.fileBytes = static_cast<long long>(fs::file_size(fseq, ec));
}

GenerationSidecar MakeSidecar(const GenerationRun& run) {
    GenerationSidecar s;
    s.pipelineVersion = PIPELINE_VERSION;
    s.analyzerVersion = run.analysis.analyzerVersion;
    s.plannerVersion = run.plan.plannerVersion;
    s.choreographerVersion = run.choreography.engineVersion;
    s.sequenceFile = fs::path(run.sequencePath).filename().string();
    s.audioFile = fs::path(run.request.audioPath).filename().string();
    s.audioPath = fs::absolute(fs::path(run.request.audioPath)).string();
    s.audioHash = run.analysis.audioHash;
    s.audioSize = run.analysis.fileSize;
    s.analysisOptionsKey = run.analysis.optionsKey;
    s.analysisHash = run.plan.analysisHash;
    s.layoutFingerprint = run.layoutFingerprint;
    s.profileId = run.profile.id;
    s.profileHash = run.plan.profileHash;
    s.libraryHash = run.library.hash;
    s.planHash = HashText(ToJson(run.plan));
    s.choreographyHash = HashText(ToJson(run.choreography));
    s.specHash = SpecHash(run.spec);
    s.lyricsHash = run.plan.lyricsHash;
    s.lyricsQuality = run.hasLyrics ? run.alignment.quality : "none";
    s.level = run.plan.level;
    s.seed = run.plan.seed;
    s.frameMS = run.spec.frameMS;
    s.durationMS = run.spec.durationMS;
    s.effects = static_cast<int>(run.spec.effects.size());
    s.tracks = run.trackNames;
    s.mediaCues = run.choreography.mediaCues;
    s.planCorrections = run.corrections.plan;
    s.choreographyCorrections = run.corrections.choreography;
    s.analysisCorrections = run.corrections.analysis;
    s.lyrics = run.lyricSession;
    s.media = run.mediaSession;
    s.media.corrections = run.corrections.media;
    s.generationId = HashText(fmt::format("{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}", s.audioHash, s.analysisOptionsKey, s.analyzerVersion, s.layoutFingerprint, s.profileHash, s.libraryHash, s.lyricsHash,
                                          LevelId(s.level), s.seed, s.frameMS, s.pipelineVersion, s.plannerVersion, s.choreographerVersion, ToJson(run.corrections)));
    const ValidationReport& v = run.write.validation;
    s.validationErrors = v.ErrorCount() + run.renderValidation.ErrorCount();
    s.validationWarnings = v.WarningCount() + run.renderValidation.WarningCount();
    s.rendered = run.render.ran;
    // The review of this version: what the person said about the same decisions before
    // (handed over with the corrections, or in the sidecar this run continues).
    const VisualReview* before = !run.corrections.review.Empty() ? &run.corrections.review : !run.previous.review.Empty() ? &run.previous.review : nullptr;
    s.review = BuildReview(run.media.storyboard, s.sequenceFile, s.specHash, before, &run.mediaLibrary);
    s.visualReview = s.review.state == "approved" ? "approved" : "not-reviewed";
    return s;
}

// Saves an opened sequence again through SequenceFile, as xLights does when a person
// saves it, keeping the audio and the timing.
bool ResaveSequence(HeadlessRenderContext& ctx, const SequenceSpec& spec, const std::string& path, std::string& error) {
    SequenceFile file(path, static_cast<uint32_t>(spec.frameMS));
    file.SetSequenceDurationMS(static_cast<int>(ctx.GetSeqData().NumFrames() * ctx.GetSeqData().FrameTime()));
    file.SetSequenceTiming(std::to_string(spec.frameMS) + " ms");
    if (spec.mediaFile.empty()) {
        file.SetSequenceType("Animation");
    } else {
        file.SetSequenceType("Media");
        file.SetMediaFile(ctx.GetShowDirectory(), spec.mediaFile, false);
    }
    if (!file.Save(ctx.GetSequenceElements())) {
        error = fmt::format("SequenceFile::Save failed on '{}'", path);
        return false;
    }
    return true;
}

} // namespace

const char* StageId(GenerationStage stage) {
    return STAGE_IDS[static_cast<int>(stage)];
}

bool StageFromId(const std::string& id, GenerationStage& stage) {
    for (int i = 0; i < GENERATION_STAGE_COUNT; ++i) {
        if (id == STAGE_IDS[i]) {
            stage = static_cast<GenerationStage>(i);
            return true;
        }
    }
    return false;
}

const char* StageStatusId(StageStatus status) {
    return STATUS_IDS[static_cast<int>(status)];
}

std::string SpecHash(const SequenceSpec& spec) {
    std::vector<std::string> effects;
    for (const EffectSpec& e : spec.effects) {
        std::vector<std::pair<std::string, std::string>> settings = e.settings;
        std::sort(settings.begin(), settings.end());
        std::string line = fmt::format("{}|{}|{}|{}|{}|{}|{}", FullName(e), e.layer, e.startMS, e.endMS, e.effect, e.palette, static_cast<int>(e.kind));
        for (const auto& [k, v] : settings) {
            line += "|" + k + "=" + v;
        }
        effects.push_back(std::move(line));
    }
    std::sort(effects.begin(), effects.end());
    std::string all = fmt::format("{}|{}|{}|{}|", spec.frameMS, spec.durationMS, spec.modelBlending, fs::path(spec.mediaFile).filename().string());
    for (const TimingTrackSpec& t : spec.timingTracks) {
        all += t.name + "{";
        for (const auto& layer : t.layers) {
            for (const TimingMark& m : layer) {
                all += fmt::format("{}:{}-{};", m.label, m.startMS, m.endMS);
            }
            all += "/";
        }
        all += "}";
    }
    for (const MarkerSpec& m : spec.markers) {
        all += fmt::format("M{}:{}:{}-{};", m.id, m.target, m.startMS, m.endMS);
    }
    for (const std::string& e : effects) {
        all += e + "\n";
    }
    return HashText(all);
}

// ------------------------------------------------------------------ stages

bool CheckInputs(GenerationRun& run) {
    return RunStage(run, GenerationStage::Inputs, [&]() {
        const GenerationRequest& r = run.request;
        const GenerationStage S = GenerationStage::Inputs;
        if (r.audioPath.empty() || !FileExists(r.audioPath)) {
            return Fail(run, S, "audio-missing", r.audioPath);
        }
        if (r.showDir.empty() || !FileExists((fs::path(r.showDir) / "xlights_rgbeffects.xml").string())) {
            return Fail(run, S, "show-missing", r.showDir);
        }
        const fs::path name(r.sequenceName);
        if (r.sequenceName.empty() || name.has_parent_path() || name.extension() != ".xsq" || name.stem().empty()) {
            return Fail(run, S, "sequence-name-invalid", r.sequenceName + " (a file name ending in .xsq, without folders)");
        }
        if (r.frameMS <= 0 || 1000 % r.frameMS != 0) {
            return Fail(run, S, "frame-invalid", std::to_string(r.frameMS));
        }
        ObtainAccessToURL(r.showDir, true);
        run.sequencePath = (fs::path(r.showDir) / name).string();
        run.sidecarPath = SidecarPathFor(run.sequencePath);
        fs::path backup = run.sequencePath;
        backup.replace_extension(".xbkp");
        for (const std::string& existing : { run.sequencePath, backup.string(), run.sidecarPath }) {
            if (FileExists(existing)) {
                return Fail(run, S, "output-exists", existing + " is kept as it is; choose another name");
            }
        }
        if (r.reportDir.empty()) {
            return Fail(run, S, "report-dir-missing", "");
        }
        std::error_code ec;
        const fs::path report(r.reportDir);
        if (fs::exists(report, ec) && !IsDirectoryEmpty(report)) {
            return Fail(run, S, "report-dir-not-empty", r.reportDir);
        }
        if (fs::weakly_canonical(report, ec) == fs::weakly_canonical(fs::path(r.showDir), ec)) {
            return Fail(run, S, "report-dir-is-show", r.reportDir);
        }
        fs::create_directories(report, ec);
        if (ec) {
            return Fail(run, S, "report-dir-unwritable", ec.message());
        }
        run.fseqPath = (report / (name.stem().string() + ".fseq")).string();
        for (const auto& [what, path] : std::vector<std::pair<const char*, std::string>>{ { "profile", r.profilePath }, { "roles", r.rolesPath }, { "lyrics", r.lyricsPath }, { "timing", r.timingPath }, { "lyric-session", r.lyricSessionPath }, { "corrections", r.correctionsPath } }) {
            if (!path.empty() && !FileExists(path)) {
                return Fail(run, S, fmt::format("{}-missing", what), path);
            }
        }
        if (!r.templatesDir.empty() && !fs::is_directory(fs::path(r.templatesDir), ec)) {
            return Fail(run, S, "templates-missing", r.templatesDir);
        }

        run.corrections = CorrectionSet();
        if (!r.previousSidecarPath.empty()) {
            std::string error;
            const SidecarStatus status = ReadSidecarFor(r.previousSidecarPath, Sha256OfFile(r.audioPath, r.cancel), run.previous, error);
            if (status != SidecarStatus::Ok) {
                return Fail(run, S, fmt::format("previous-sidecar-{}", SidecarStatusId(status)), r.previousSidecarPath + ": " + error);
            }
            run.hasPrevious = true;
            run.corrections.plan = run.previous.planCorrections;
            run.corrections.choreography = run.previous.choreographyCorrections;
            run.corrections.lyrics = run.previous.lyrics.corrections;
            run.corrections.analysis = run.previous.analysisCorrections;
            run.corrections.media = run.previous.media.corrections;
            // The assistant's answers the earlier show used, from the cache: no network.
            for (const auto& [kept, given, what] : { std::tuple<const std::string*, std::string*, const char*>{ &run.previous.transcriptCache, &run.request.transcriptPath, "transcript" },
                                                     { &run.previous.characterCache, &run.request.characterPath, "song description" } }) {
                if (given->empty() && !kept->empty()) {
                    const fs::path cached = fs::path(r.showDir) / fs::u8path(*kept);
                    if (FileExists(cached.string())) {
                        *given = cached.string();
                        Note(run, S, "assist-from-cache", fmt::format("{} {}", what, *kept));
                    } else {
                        Note(run, S, "assist-cache-missing", fmt::format("{} {}", what, *kept));
                    }
                }
            }
            Note(run, S, "previous-sidecar", fmt::format("{}: {} plan, {} choreography, {} lyric and {} music corrections kept", run.previous.sequenceFile, run.corrections.plan.size(), run.corrections.choreography.size(),
                                                         run.corrections.lyrics.size(), run.corrections.analysis.events.size() + run.corrections.analysis.sections.size() + (run.corrections.analysis.bar.firstDownbeatMS >= 0 ? 1 : 0)));
            run.Stage(S).status = StageStatus::Ok;
        }
        if (!r.correctionsPath.empty()) {
            std::string text;
            std::string error;
            CorrectionSet file;
            if (!ReadText(r.correctionsPath, text) || !FromJson(text, file, error)) {
                return Fail(run, S, "corrections-invalid", r.correctionsPath + ": " + error);
            }
            MergeCorrections(run.corrections, file);
        }
        MergeCorrections(run.corrections, r.corrections);
        if (!run.request.level) {
            run.request.level = run.hasPrevious ? run.previous.level : AutomationLevel::Balanced;
        }
        if (!run.request.seed) {
            run.request.seed = run.hasPrevious ? run.previous.seed : 1;
        }
        run.protectedHashes = ProtectedHashes(r.showDir);
        return true;
    });
}

bool LoadLayout(GenerationRun& run) {
    return RunStage(run, GenerationStage::Layout, [&]() {
        const GenerationRequest& r = run.request;
        const GenerationStage S = GenerationStage::Layout;
        run.context = std::make_unique<HeadlessRenderContext>();
        if (!run.context->LoadShowFolder(r.showDir)) {
            return Fail(run, S, "layout-unreadable", r.showDir);
        }
        RoleOverrides roles;
        bool haveRoles = false;
        const std::string rolesPath = !r.rolesPath.empty() ? r.rolesPath : (fs::path(r.showDir) / "ShowComposer" / "roles.json").string();
        if (FileExists(rolesPath)) {
            std::string text;
            std::string error;
            if (!ReadText(rolesPath, text) || !FromJson(text, roles, error)) {
                return Fail(run, S, "roles-invalid", rolesPath + ": " + error);
            }
            haveRoles = true;
        }
        run.capabilities = ExtractCapabilities(*run.context, haveRoles ? &roles : nullptr);
        run.layoutFingerprint = LayoutFingerprint(*run.context);
        run.metadata = MetadataFrom(run.context->GetEffectManager());
        run.library = LoadTemplateLibrary(r.templatesDir.empty() ? DefaultTemplateDirectory() : r.templatesDir, run.metadata);
        if (run.library.templates.empty()) {
            return Fail(run, S, "templates-empty", r.templatesDir.empty() ? DefaultTemplateDirectory() : r.templatesDir);
        }
        for (const RejectedTemplate& t : run.library.rejected) {
            Note(run, S, "template-rejected", fmt::format("{}: {}", t.file, t.errors.empty() ? std::string() : t.errors.front()));
        }
        run.profile = NeutralStarterProfile();
        if (!r.profilePath.empty()) {
            std::string text;
            std::string error;
            if (!ReadText(r.profilePath, text) || !FromJson(text, run.profile, error)) {
                return Fail(run, S, "profile-invalid", r.profilePath + ": " + error);
            }
        }
        if (!run.profile.learned) {
            Note(run, S, "profile-is-starting-parameters", run.profile.id);
        }
        int noController = 0;
        int excluded = 0;
        for (const TargetInfo& t : run.capabilities.targets) {
            noController += t.noController ? 1 : 0;
            excluded += t.Excluded() ? 1 : 0;
        }
        run.Stage(S).notes.push_back(fmt::format("layout: {} models, {} groups, {} submodels, {} without controller (kept), {} excluded, roles.json {}", run.capabilities.Count(TargetType::Model),
                                                 run.capabilities.Count(TargetType::Group), run.capabilities.Count(TargetType::SubModel), noController, excluded, haveRoles ? "applied" : "not present"));
        run.Stage(S).notes.push_back(fmt::format("templates: {} valid, {} rejected", run.library.templates.size(), run.library.rejected.size()));
        return true;
    });
}

bool AnalyzeAudio(GenerationRun& run) {
    return RunStage(run, GenerationStage::Analysis, [&]() {
        const GenerationRequest& r = run.request;
        const GenerationStage S = GenerationStage::Analysis;
        const fs::path cacheDir = r.cacheDir.empty() ? fs::path(r.showDir) / "ShowComposer" / "cache" : fs::path(r.cacheDir);
        std::error_code ec;
        fs::create_directories(cacheDir, ec);
        if (ec) {
            return Fail(run, S, "cache-unwritable", ec.message());
        }
        const std::string hash = Sha256OfFile(r.audioPath, r.cancel);
        if (CancelRequested(run, S)) {
            return false;
        }
        if (hash.empty()) {
            return Fail(run, S, "audio-unreadable", r.audioPath);
        }
        // The name only finds the entry; whether it can be reused is decided by the
        // bytes, the analyzer version and the options stored inside it.
        run.analysisCachePath = (cacheDir / (hash.substr(0, 16) + ".analysis.json")).string();
        const AnalysisProgress progress = [&](int percent, const char* stage) { Progress(run, GenerationStage::Analysis, fmt::format("{} {}%", stage, percent)); };
        const AnalysisResult result = AnalyzeSongCached(r.audioPath, run.analysisCachePath, r.analysisOptions, progress, r.cancel);
        if (result.status == AnalysisStatus::Cancelled) {
            run.cancelled = true;
            run.Stage(S).status = StageStatus::Cancelled;
            return false;
        }
        if (result.status != AnalysisStatus::Ok) {
            return Fail(run, S, "audio-unreadable", fmt::format("{} {}", result.errorCode, result.errorDetail));
        }
        run.analysis = result.analysis;
        run.analysisFromCache = result.reusedCache;
        const AnalysisCorrections& fixes = run.corrections.analysis;
        if (!fixes.events.empty() || !fixes.sections.empty() || fixes.bar.firstDownbeatMS >= 0 || fixes.tempoFactor != 1) {
            // A person's beats, bars and sections win over the detection, here as in the cache.
            MergeAnalysisCorrections(run.analysis.corrections, fixes);
            ApplyCorrections(run.analysis);
            run.Stage(GenerationStage::Analysis).notes.push_back(fmt::format("music corrections applied: {} marks, {} sections{}{}", fixes.events.size(), fixes.sections.size(), fixes.bar.firstDownbeatMS >= 0 ? ", the bar" : "", fixes.tempoFactor != 1 ? fmt::format(", the pulse x{}", fixes.tempoFactor) : std::string()));
        }
        const SongAnalysis& a = run.analysis;
        if (a.durationMS < 2 * r.frameMS) {
            return Fail(run, S, "audio-too-short", std::to_string(a.durationMS));
        }
        int counts[10] = {};
        for (const MusicalEvent& e : a.events) {
            counts[static_cast<int>(e.type)]++;
        }
        run.Stage(S).notes.push_back(fmt::format("{} ms, {:.1f} BPM ({:.2f}), {} beats, {} downbeats, {} onsets, {} sections, {} silences, {} climax; cache {}", a.durationMS, a.globalBpm, a.bpmConfidence,
                                                 counts[static_cast<int>(MusicalEventType::Beat)], counts[static_cast<int>(MusicalEventType::Downbeat)], counts[static_cast<int>(MusicalEventType::Onset)],
                                                 a.sections.size(), counts[static_cast<int>(MusicalEventType::Silence)], counts[static_cast<int>(MusicalEventType::Climax)], run.analysisFromCache ? "reused" : "written"));
        for (const std::string& w : a.warnings) {
            Note(run, S, "analysis-warning", w);
        }
        Note(run, S, "music-quality", a.musicQuality);
        return true;
    });
}

bool PrepareLyrics(GenerationRun& run) {
    return RunStage(run, GenerationStage::Lyrics, [&]() {
        const GenerationRequest& r = run.request;
        const GenerationStage S = GenerationStage::Lyrics;
        run.hasLyrics = false;
        run.lyrics = LyricDocument();
        run.alignment = LyricAlignment();
        run.lyricTrack = TimingTrackSpec();
        run.lyricSession = LyricSession();
        run.lyricSession.trackName = r.lyricTrackName;

        std::vector<TimingTrackSpec> imported;
        const TimingTrackSpec* existing = nullptr;
        if (!r.timingPath.empty()) {
            std::string error;
            if (!ImportTimingFile(r.timingPath, r.frameMS, imported, error)) {
                return Fail(run, S, "timing-invalid", error);
            }
            existing = PickLyricTrack(imported, r.timingTrack);
            if (existing == nullptr) {
                return Fail(run, S, "timing-no-lyrics-track", r.timingTrack.empty() ? r.timingPath + " has no track with phrases, words and phonemes" : r.timingTrack);
            }
        }
        // Regenerating: the lyrics track of the earlier sequence holds whatever a person
        // moved in it since, so it wins over the times computed back then.
        TimingTrackSpec previousTrack;
        if (existing == nullptr && run.hasPrevious) {
            const auto named = run.previous.tracks.find("composer.lyrics");
            const fs::path previousSequence = fs::path(r.previousSidecarPath).parent_path() / run.previous.sequenceFile;
            if (named != run.previous.tracks.end()) {
                if (FileExists(previousSequence.string()) && run.context->OpenSequence(previousSequence.string())) {
                    const SequenceSpec previous = SpecFromSequence(*run.context);
                    (void)run.context->CloseSequence();
                    for (const TimingTrackSpec& t : previous.timingTracks) {
                        if (t.name == named->second && t.layers.size() >= 3) {
                            previousTrack = t;
                            existing = &previousTrack;
                        }
                    }
                }
                Note(run, S, existing != nullptr ? "lyrics-track-from-previous-sequence" : "previous-lyrics-track-missing", previousSequence.string());
            }
        }
        LyricSession session;
        if (!r.lyricSessionPath.empty()) {
            std::string error;
            if (!LoadLyricSession(r.lyricSessionPath, session, error)) {
                return Fail(run, S, "lyric-session-invalid", error);
            }
        }
        std::string text;
        if (!r.lyricsPath.empty()) {
            if (!ReadText(r.lyricsPath, text)) {
                return Fail(run, S, "lyrics-unreadable", r.lyricsPath);
            }
        } else if (!session.lyrics.empty()) {
            text = session.lyrics;
        } else if (run.hasPrevious && !run.previous.lyrics.lyrics.empty()) {
            text = run.previous.lyrics.lyrics;
        }
        if (!r.lyricsPath.empty() && existing == nullptr && text.find_first_not_of(" \t\r\n") == std::string::npos) {
            // Chosen as the lyrics and empty: a wrong file, not a song without lyrics.
            Note(run, S, "lyrics-error", "empty-file: " + r.lyricsPath);
            return Fail(run, S, "lyrics-empty-file", r.lyricsPath);
        }
        if (text.empty() && existing == nullptr) {
            Note(run, S, "no-lyrics", "no text, no faces");
            return true;
        }
        // A synchronized track is the source of truth for its text and its times, as the
        // aligner's provider A expects; the text given beside it is only kept.
        run.lyrics = existing != nullptr ? LyricsFromTrack(*existing) : ParseLyricsFile(r.lyricsPath, text);
        if (existing != nullptr && !text.empty()) {
            Note(run, S, "lyrics-from-track", fmt::format("the track '{}' wins over the text and times given with it", existing->name));
            // The track has the times; who sings each line is only in the text, so the
            // voices go over to the track's lines with the same words, in order.
            LyricDocument tagged = ParseLyricsFile(r.lyricsPath, text);
            if (!tagged.cast.Empty()) {
                std::vector<const LyricLine*> from = tagged.Lines();
                size_t next = 0;
                int matched = 0;
                for (LyricBlock& block : run.lyrics.blocks) {
                    for (LyricLine& line : block.lines) {
                        for (size_t k = next; k < from.size(); ++k) {
                            if (LyricKey(from[k]->text) == LyricKey(line.text)) {
                                line.voices = from[k]->voices;
                                line.voice = from[k]->voice;
                                line.delivery = from[k]->delivery;
                                next = k + 1;
                                ++matched;
                                break;
                            }
                        }
                    }
                }
                run.lyrics.cast = tagged.cast;
                Note(run, S, "voices-from-text", fmt::format("{} of {} lines of the track took their voices from the text", matched, run.lyrics.Lines().size()));
            }
        }
        CheckLyricsAgainstSong(run.lyrics, run.analysis.durationMS);
        for (const LyricIssue& issue : run.lyrics.issues) {
            Note(run, S, issue.error ? "lyrics-error" : issue.repaired ? "lyrics-repaired" : "lyrics-issue", DescribeLyricIssue(issue));
        }
        if (run.lyrics.ErrorCount() > 0) {
            const auto first = std::find_if(run.lyrics.issues.begin(), run.lyrics.issues.end(), [](const LyricIssue& i) { return i.error; });
            return Fail(run, S, "lyrics-times-invalid", fmt::format("{} problems in the lyrics' times, the first: {}", run.lyrics.ErrorCount(), DescribeLyricIssue(*first)));
        }
        if (run.lyrics.Empty()) {
            Note(run, S, "lyrics-empty", r.lyricsPath);
            return true;
        }
        run.hasLyrics = true;
        std::vector<LyricCorrection> corrections = session.corrections;
        for (const LyricCorrection& c : run.corrections.lyrics) {
            UpsertCorrection(corrections, c);
        }
        AlignerInput in;
        in.document = &run.lyrics;
        in.existingTrack = existing;
        in.durationMS = run.analysis.durationMS;
        in.corrections = corrections;
        if (!r.transcriptPath.empty() && existing == nullptr) {
            // A transcription saved earlier: its words, matched to the lyrics, give the
            // times; the lyrics stay the text.
            std::string json;
            std::string error;
            if (!ReadText(r.transcriptPath, json) || !FromJson(json, run.transcript, error)) {
                return Fail(run, S, "transcript-invalid", r.transcriptPath + (error.empty() ? std::string() : ": " + error));
            }
            run.transcriptAlignment = AlignTranscriptToLyrics(run.lyrics, run.transcript);
            Note(run, S, "transcript", fmt::format("{} {}: {} of {} lyric words heard ({:.0f}%), {} words heard that are not in the lyrics", run.transcript.provider, run.transcript.model,
                                                   run.transcriptAlignment.matchedWords, run.transcriptAlignment.lyricWords, run.transcriptAlignment.coverage * 100, run.transcriptAlignment.extraWords));
            for (const std::string& w : run.transcriptAlignment.warnings) {
                Note(run, S, "transcript-warning", w);
            }
            if (!run.transcriptAlignment.anchors.empty()) {
                in.anchors = &run.transcriptAlignment.anchors;
            }
        }
        AlignerOptions options;
        options.frameMS = r.frameMS;
        run.alignment = AlignLyrics(in, options);
        run.corrections.lyrics = corrections;
        run.lyricSession.lyrics = text.empty() && existing != nullptr ? std::string() : text;
        run.lyricSession.corrections = corrections;
        // A person's voice names and faces come with the marks, from this session or the
        // earlier show's.
        run.lyricSession.voices = !session.voices.empty() ? session.voices : run.hasPrevious ? run.previous.lyrics.voices : std::vector<VoiceDef>();
        for (const VoiceDef& v : run.corrections.voices) {
            std::erase_if(run.lyricSession.voices, [&](const VoiceDef& o) { return o.id == v.id; });
            run.lyricSession.voices.push_back(v);
        }

        int synced = 0;
        for (const AlignedPhrase& p : run.alignment.phrases) {
            synced += p.startMS >= 0 && !p.instrumental ? 1 : 0;
        }
        run.Stage(S).notes.push_back(fmt::format("{} lines, language {}, provider {}, quality {}, {} phrases synchronized, {} words, {} mouth shapes", run.lyrics.Lines().size(), run.alignment.language,
                                                 ProviderId(run.alignment.provider), run.alignment.quality, synced, run.alignment.words.size(), run.alignment.phonemes.size()));
        for (const AlignmentConflict& c : run.alignment.conflicts) {
            Note(run, S, "lyrics-correction-conflict", c.targetId + " " + c.reason);
        }
        if (synced == 0) {
            // The text is kept for the sidecar, so the marks can be made later; no time is invented.
            Note(run, S, "lyrics-not-synchronized", "the text is kept; faces and lyric text wait for its marks");
            return true;
        }
        TimingTrackSpec track = BuildLyricTrack(run.alignment, r.lyricTrackName);
        const ValidationReport report = ValidateLyrics(run.alignment, track, run.analysis.durationMS, r.frameMS);
        AddValidationNotes(run, S, report, "lyrics");
        if (report.ErrorCount() > 0) {
            return Fail(run, S, "lyrics-invalid", fmt::format("{} errors", report.ErrorCount()));
        }
        run.lyricTrack = std::move(track);
        if (r.lyricStructure) {
            // The words say which part is the chorus, which the audio alone cannot.
            const StructureResult structure = RefineSectionsWithLyrics(run.analysis, TimedLinesFrom(run.alignment));
            if (structure.applied) {
                Note(run, S, "structure", fmt::format("sections from the lyrics: {} choruses, {} verses, {} parts sung again", structure.choruses, structure.verses, structure.families));
                for (const std::string& n : structure.notes) {
                    Note(run, S, "structure-section", n);
                }
            } else {
                Note(run, S, "structure-kept", structure.reason);
            }
        }
        run.cast = EffectiveCast(run.lyrics.cast, run.lyricSession.voices);
        if (!run.cast.Empty()) {
            std::vector<std::string> warnings;
            AssignVoiceFaces(run.cast, run.capabilities, &warnings);
            run.voiceTracks = BuildVoiceTracks(run.alignment, run.cast, r.lyricTrackName);
            for (const VoiceDef& v : run.cast.voices) {
                int phrases = 0;
                for (const AlignedPhrase& p : run.alignment.phrases) {
                    phrases += p.startMS >= 0 && std::find(p.voices.begin(), p.voices.end(), v.id) != p.voices.end() ? 1 : 0;
                }
                Note(run, S, "voice", fmt::format("{} ({}, {}): {} phrases, faces {}", v.name, VoiceRoleId(v.role), DeliveryId(v.delivery), phrases, v.facesOff ? std::string("off") : v.faces.empty() ? std::string("none") : std::accumulate(v.faces.begin(), v.faces.end(), std::string(), [](const std::string& a, const std::string& b) { return a.empty() ? b : a + ", " + b; })));
            }
            for (const std::string& w : warnings) {
                Note(run, S, "voice-warning", w);
            }
            int unnamed = 0;
            for (const AlignedPhrase& p : run.alignment.phrases) {
                unnamed += p.startMS >= 0 && !p.instrumental && p.voices.empty() ? 1 : 0;
            }
            if (unnamed > 0) {
                Note(run, S, "voice-unknown", fmt::format("{} phrases have no voice: no face sings them until one is given", unnamed));
            }
        }
        return true;
    });
}

bool PlanShow(GenerationRun& run) {
    return RunStage(run, GenerationStage::Plan, [&]() {
        const GenerationStage S = GenerationStage::Plan;
        // A figure a person excluded for the whole song is out of this show the way
        // roles.json would take it out: dark, and no group lights it either.
        for (const PlanCorrection& c : run.corrections.plan) {
            if (c.action != PlanCorrection::Action::Exclude || c.startMS >= 0) {
                continue;
            }
            const int index = run.capabilities.IndexOf(c.target);
            if (index < 0) {
                Note(run, S, "excluded-figure-missing", c.target);
                continue;
            }
            TargetInfo& t = run.capabilities.targets[index];
            if (!t.Excluded()) {
                t.roles.insert(t.roles.begin(), { Role::Excluded, RoleStatus::ExcludedManually, "correction", 1.0 });
                run.Stage(S).notes.push_back("excluded by a correction: " + c.target);
            }
        }
        PlannerInput in;
        in.analysis = &run.analysis;
        in.capabilities = &run.capabilities;
        if (run.hasLyrics) {
            in.lyrics = &run.lyrics;
            in.alignment = &run.alignment;
            in.cast = run.cast.Empty() ? nullptr : &run.cast;
        }
        in.profile = &run.profile;
        in.options.level = *run.request.level;
        in.options.seed = *run.request.seed;
        in.options.frameMS = run.request.frameMS;
        in.corrections = run.corrections.plan;
        run.plan = PlanComposition(in);
        run.planValidation = ValidateCompositionPlan(run.plan, run.capabilities, &run.profile);
        AddValidationNotes(run, S, run.planValidation, "plan");
        for (const PlanConflict& c : run.plan.conflicts) {
            Note(run, S, "plan-conflict", fmt::format("{} {}-{} ms: {}", c.target, c.startMS, c.endMS, c.reason));
        }
        std::set<std::string> participants;
        for (const PlanSegment& s : run.plan.segments) {
            for (const PlanAssignment& a : s.assignments) {
                if (a.prominence != Prominence::Rest) {
                    participants.insert(a.target);
                }
            }
        }
        run.Stage(S).notes.push_back(fmt::format("{} segments, {} participating targets, level {}, seed {}", run.plan.segments.size(), participants.size(), LevelId(run.plan.level), run.plan.seed));
        if (run.planValidation.ErrorCount() > 0) {
            return Fail(run, S, "plan-invalid", fmt::format("{} errors", run.planValidation.ErrorCount()));
        }
        return true;
    });
}

namespace {

// The show's pictures and videos, chosen once the scenes are known and before the
// choreography: the figure that shows one takes the "media" intent in the plan.
void PlanShowMedia(GenerationRun& run, GenerationStage S) {
    const GenerationRequest& r = run.request;
    MediaSession& session = run.mediaSession;
    session = run.hasPrevious ? run.previous.media : MediaSession();
    session.used.clear();
    if (!r.mediaFolders.empty()) {
        session.folders.clear();
        for (const std::string& f : r.mediaFolders) {
            const std::string relative = MediaRelativePath(f, r.showDir);
            session.folders.push_back(relative.empty() ? f : relative);
        }
    }
    if (r.mediaAutomatic) {
        session.automatic = *r.mediaAutomatic;
    }
    if (r.mediaMaxShare) {
        session.maxShare = *r.mediaMaxShare;
    }
    if (r.mediaVariant) {
        session.variant = *r.mediaVariant;
    }
    MergeScreenSettings(session.screens, run.corrections.screens);
    // Media only for a show that asked for it, with its folders or a person's choices.
    if (session.folders.empty() && run.corrections.media.empty()) {
        return;
    }
    const std::string libraryPath = r.mediaLibraryPath.empty() ? MediaLibraryPath(r.showDir) : r.mediaLibraryPath;
    MediaLibrary saved;
    std::string error;
    const bool exists = FileExists(libraryPath);
    const bool hasSaved = exists && ReadMediaLibrary(libraryPath, saved, error);
    if (exists && !hasSaved) {
        Note(run, S, "media-library-unreadable", libraryPath + ": " + error);
    }
    std::vector<std::string> folders;
    for (const std::string& f : session.folders) {
        folders.push_back(MediaAbsolutePath(f, r.showDir));
    }
    if (!folders.empty() && (r.scanMedia || !hasSaved)) {
        MediaScanOptions o;
        o.showDir = r.showDir;
        o.folders = folders;
        o.cancelled = r.cancel;
        run.mediaLibrary = ScanMediaFolders(o, hasSaved ? &saved : nullptr);
        if (!SaveMediaLibrary(libraryPath, run.mediaLibrary, error)) {
            Note(run, S, "media-library-not-saved", error);
        }
        Note(run, S, "media-scan", fmt::format("{} files in {} folders", run.mediaLibrary.items.size(), run.mediaLibrary.folders.size()));
        for (const std::string& w : run.mediaLibrary.warnings) {
            Note(run, S, "media-scan-warning", w);
        }
    } else if (hasSaved) {
        run.mediaLibrary = std::move(saved);
        // A file can have gone since the library was made: then it is not used.
        int gone = 0;
        for (MediaItem& m : run.mediaLibrary.items) {
            if (m.status == MediaStatus::Ok && !FileExists(m.path)) {
                m.status = MediaStatus::Missing;
                m.problem = "missing";
                ++gone;
            }
        }
        Note(run, S, "media-library", fmt::format("{} files, {} gone since it was searched", run.mediaLibrary.items.size(), gone));
    }
    if (!run.mediaLibrary.items.empty()) {
        // Files never looked at are analysed here without a model: colours, light and
        // motion, the same on every run. The model only runs when asked for.
        MediaAnalysisOptions ao;
        ao.showDir = r.showDir;
        ao.cancelled = r.cancel;
        VisualConcepts concepts;
        std::string cerr;
        if (ReadVisualConcepts(DefaultVisualConceptsPath(), concepts, cerr)) {
            ao.concepts = &concepts;
        } else {
            Note(run, S, "visual-concepts-unreadable", cerr);
        }
        std::unique_ptr<VisualModel> model;
        ConceptIndex index;
        if (r.analyzeMedia && !r.visionModelDir.empty() && ao.concepts != nullptr) {
            model = OpenVisualModel(r.visionModelDir, cerr);
            if (model != nullptr && BuildConceptIndex(*model, concepts, MediaCacheDir(r.showDir), index, cerr)) {
                ao.model = model.get();
                ao.index = &index;
            } else {
                Note(run, S, "visual-model-unavailable", cerr);
            }
        }
        run.mediaAnalysis = AnalyzeMediaLibrary(run.mediaLibrary, ao);
        const MediaAnalysisSummary& sa = run.mediaAnalysis;
        Note(run, S, "media-analysis", fmt::format("{}: {} usable of {}, {} analysed now, {} from the cache, {} with the model, {} duplicates, {} similar", sa.mode, sa.usable, sa.total, sa.analysed,
                                                   sa.cached, sa.semantic, sa.duplicates, sa.similar));
        if (sa.analysed > 0 && !SaveMediaLibrary(libraryPath, run.mediaLibrary, error)) {
            Note(run, S, "media-library-not-saved", error);
        }
    }
    // Planning reads what a person said each file shows over what the model said.
    MediaLibrary planning = run.mediaLibrary;
    {
        VisualConcepts vocabulary;
        std::string verr;
        const bool haveVocabulary = ReadVisualConcepts(DefaultVisualConceptsPath(), vocabulary, verr);
        ApplyHumanLabels(planning, haveVocabulary ? &vocabulary : nullptr);
        int labelled = 0;
        for (const MediaItem& m : planning.items) {
            labelled += m.human.Empty() ? 0 : 1;
        }
        if (labelled > 0) {
            Note(run, S, "media-human-labels", fmt::format("{} files with a person's labels", labelled));
        }
    }
    MediaPlanInput in;
    in.plan = &run.plan;
    in.capabilities = &run.capabilities;
    in.library = &planning;
    in.analysis = &run.analysis;
    in.options.variant = session.variant;
    in.screens = &session.screens;
    if (run.hasLyrics) {
        in.alignment = &run.alignment;
    }
    in.metadata = &run.metadata;
    in.direction = run.direction.enabled ? &run.direction : nullptr;
    if (run.direction.enabled) {
        for (const ThemeDef& t : run.themes.themes) {
            if (t.id == run.direction.themeId) {
                in.themeCategories = t.mediaCategories;
            }
        }
    }
    // What the maker taught the program, unless this show is made without it.
    if (r.learning && !r.showDir.empty()) {
        ArtisticPreferences prefs;
        std::string perr;
        if (ReadPreferences(PreferencesPath(r.showDir), prefs, perr)) {
            run.preferences = EffectivePreferences(prefs);
        } else {
            Note(run, S, "preferences-unreadable", perr);
        }
    }
    if (!run.preferences.empty()) {
        std::string list;
        for (const auto& [k, v] : run.preferences) {
            list += fmt::format("{}{} {:+.2f}", list.empty() ? "" : ", ", k, v);
        }
        Note(run, S, "preferences", list);
        in.preferences = &run.preferences;
    } else {
        Note(run, S, "preferences", r.learning ? "none learned yet" : "learning off for this show");
    }
    in.profileId = run.profile.id;
    in.showDir = r.showDir;
    in.options.automatic = session.automatic;
    in.options.maxShare = session.maxShare;
    in.corrections = run.corrections.media;
    run.media = PlanMedia(in);
    for (MediaAssignment& a : run.media.assignments) {
        const auto outside = std::find(a.constraints.begin(), a.constraints.end(), "outside-show");
        if (outside == a.constraints.end()) {
            continue;
        }
        if (!r.copyMediaIntoShow) {
            Note(run, S, "media-outside-show", a.path);
            continue;
        }
        const MediaItem* m = run.mediaLibrary.Find(a.mediaId);
        std::string copied;
        if (m == nullptr || !CopyMediaIntoShow(*m, r.showDir, a.effect == "Video" ? "Videos" : "Images", copied, error)) {
            Note(run, S, "media-copy-failed", fmt::format("{}: {}", a.path, error));
            continue;
        }
        const std::string relative = MediaRelativePath(copied, r.showDir);
        for (auto& [key, value] : a.settings) {
            if (value == a.path) {
                value = relative;
            }
        }
        a.path = relative;
        a.constraints.erase(outside);
        a.reasons.push_back({ "copied-into-show", relative });
        Note(run, S, "media-copied", relative);
    }
    ApplyMediaPlan(run.plan, run.media, run.capabilities);
    // The storyboard before this one is kept, to bring it back if a person prefers it.
    const VisualMediaStoryboard& board = run.media.storyboard;
    if (run.hasPrevious && !run.previous.media.storyboardHash.empty() && run.previous.media.storyboardHash != board.hash && !run.previous.media.used.empty()) {
        StoryboardVersion v;
        v.number = session.history.empty() ? 1 : session.history.back().number + 1;
        v.hash = run.previous.media.storyboardHash;
        v.created = run.previous.generationId;
        v.variant = run.previous.media.variant;
        for (const MediaUse& u : run.previous.media.used) {
            v.shots.push_back({ u.segmentId, u.target, u.startMS, u.endMS, u.mediaId, u.source.empty() ? u.path : u.source, std::max(0, u.fragmentStartMS), u.framing });
        }
        session.history.push_back(std::move(v));
        while (session.history.size() > STORYBOARD_HISTORY) {
            session.history.erase(session.history.begin());
        }
    }
    session.storyboardHash = board.hash;
    for (const Surface& sf : run.media.surfaces.surfaces) {
        if (sf.role != SurfaceRole::Support && sf.role != SurfaceRole::Off) {
            Note(run, S, "screen", fmt::format("{}: {} ({}), {} lights, words {:.0f}%, free {:.0f}%", sf.target, SurfaceRoleId(sf.role), sf.roleSource, sf.pixels, sf.lyricShare * 100, sf.freeShare * 100));
        }
    }
    for (const std::string& w : run.media.surfaces.warnings) {
        Note(run, S, "screen-warning", w);
    }
    // Derived pictures and clips nothing has used for two months go.
    if (!r.showDir.empty()) {
        std::vector<std::string> keep;
        for (const MediaAssignment& a : run.media.assignments) {
            if (a.path.rfind("ShowComposer/media-derived/", 0) == 0) {
                keep.push_back(a.path.substr(std::string("ShowComposer/media-derived/").size()));
            }
        }
        const int pruned = PruneMediaDerived(r.showDir, keep, 60);
        if (pruned > 0) {
            Note(run, S, "media-derived-pruned", fmt::format("{} files unused for 60 days", pruned));
        }
    }
    if (!board.lanes.empty()) {
        Note(run, S, "storyboard", fmt::format("{} shots on {} screens, {} files ({} images, {} videos), {} derived, {} repeats ({} by the music), {} gaps, mean {:.2f}, analysis {}, hash {}", board.stats.shots,
                                               board.lanes.size(), board.stats.distinctFiles, board.stats.images + board.stats.animations, board.stats.videos, board.stats.derived, board.stats.repeats,
                                               board.stats.repeatsByMusic, board.stats.gaps, board.stats.meanScore, board.analysisMode, board.hash));
        for (const StoryboardLaneStats& l : board.stats.lanes) {
            Note(run, S, "storyboard-lane", fmt::format("{}: {} shots, {:.0f}% of the song, {:.0f}% of its free time", l.lane, l.shots, l.coverage * 100, l.ofFree * 100));
        }
    }
    for (const MediaAssignment& a : run.media.assignments) {
        MediaUse use{ a.segmentId, a.target, a.startMS, a.endMS - a.overlapMS, a.mediaId, a.path, a.effect, a.state };
        use.source = a.source.empty() ? a.path : a.source;
        use.fragmentStartMS = a.kind == MediaKind::Video ? a.fragmentStartMS : -1;
        use.framing = a.framing;
        session.used.push_back(std::move(use));
        Note(run, S, "media", fmt::format("{} {}-{} ms on {}: {} ({})", a.effect, a.startMS, a.endMS, a.target, a.name, DecisionStateId(a.state)));
    }
    for (const std::string& w : run.media.warnings) {
        Note(run, S, "media-warning", w);
    }
    if (!run.media.assignments.empty() || !run.mediaLibrary.items.empty()) {
        Note(run, S, "media-plan", fmt::format("{} pictures and videos, {:.0f}% of the song (at most {:.0f}%), {} usable files", run.media.assignments.size(), run.media.share * 100, run.media.maxShare * 100,
                                               run.media.usableItems));
    }
}

} // namespace

bool ChoreographShow(GenerationRun& run) {
    return RunStage(run, GenerationStage::Choreography, [&]() {
        const GenerationStage S = GenerationStage::Choreography;
        ChoreographyInput in;
        in.plan = &run.plan;
        in.capabilities = &run.capabilities;
        in.library = &run.library;
        in.profile = &run.profile;
        in.analysis = &run.analysis;
        if (run.hasLyrics) {
            in.alignment = &run.alignment;
        }
        // Faces only move on a track that exists: without one they are left out and said so.
        in.lyricTrack = run.lyricTrack.layers.empty() ? std::string() : run.lyricTrack.name;
        for (const VoiceDef& v : run.cast.voices) {
            const std::string name = VoiceTrackName(run.request.lyricTrackName, v);
            if (std::any_of(run.voiceTracks.begin(), run.voiceTracks.end(), [&](const TimingTrackSpec& t) { return t.name == name; })) {
                in.voiceTracks[v.id] = name;
            }
        }
        in.frameMS = run.request.frameMS;
        in.corrections = run.corrections.choreography;
        if (run.profile.ensemble.enabled) {
            const std::string catalog = run.request.themesPath.empty() ? DefaultThemeCatalogPath() : run.request.themesPath;
            std::string error;
            if (!LoadThemeCatalog(catalog, run.themes, error)) {
                return Fail(run, S, "themes-invalid", error);
            }
            if (!run.request.characterPath.empty()) {
                std::string text;
                if (!ReadText(run.request.characterPath, text) || !FromJson(text, run.character, error)) {
                    return Fail(run, S, "character-invalid", run.request.characterPath + (error.empty() ? std::string() : ": " + error));
                }
                run.hasCharacter = true;
            }
            DirectionInput d;
            d.plan = &run.plan;
            d.capabilities = &run.capabilities;
            d.profile = &run.profile;
            d.catalog = &run.themes;
            d.analysis = &run.analysis;
            d.lyrics = run.hasLyrics ? &run.lyrics : nullptr;
            d.character = run.hasCharacter ? &run.character : nullptr;
            d.title = run.request.audioPath;
            run.direction = DirectShow(d);
            in.direction = &run.direction;
            Note(run, S, "visual-direction", fmt::format("theme {} from {} ({:.2f}), {} families, {} scenes", run.direction.themeId, run.direction.themeSource, run.direction.themeConfidence,
                                                         run.direction.families.size(), run.direction.scenes.size()));
        }
        PlanShowMedia(run, S);
        in.media = &run.media;
        run.choreography = Choreograph(in);
        if (run.direction.enabled) {
            run.directionMetrics = MeasureDirection(run.direction, run.choreography, run.capabilities);
            const DirectionMetrics& dm = run.directionMetrics;
            Note(run, S, "direction-metrics", fmt::format("coverage {:.2f}, palette coherence {:.2f}, two hues {:.2f}, starts together {:.2f}, {:.0f} events a minute, {} hits, repeats kept {:.2f}, looks {:.2f}",
                                                          dm.coverageMean, dm.paletteCoherence, dm.hueConcentration, dm.startsTogether, dm.eventsPerMinute, dm.hits, dm.repeatConsistency, dm.lookDiversity));
        }
        run.choreographyValidation = ValidateChoreography(run.choreography, run.plan, run.capabilities, run.library, run.metadata);
        AddValidationNotes(run, S, run.choreographyValidation, "choreography");
        for (const ChoreographyConflict& c : run.choreography.conflicts) {
            Note(run, S, "choreography-conflict", fmt::format("{} {}-{} ms: {}", c.target, c.startMS, c.endMS, c.reason));
        }
        std::map<std::string, int> warnings;
        for (const std::string& w : run.choreography.warnings) {
            warnings[w.substr(0, w.find(':'))]++;
        }
        for (const auto& [code, count] : warnings) {
            Note(run, S, "not-choreographed", fmt::format("{} x{}", code, count));
        }
        const ChoreographyMetrics& m = run.choreography.metrics;
        run.Stage(S).notes.push_back(fmt::format("{} events on {} targets, {} templates (top {} {:.0f}%), {} palettes, {} accents, {} to review, {} media cues", m.events, m.targets, m.distinctTemplates, m.topTemplate,
                                                 m.topTemplateShare * 100, m.distinctPalettes, m.accents, m.needsReview, m.mediaCues));
        if (run.choreographyValidation.ErrorCount() > 0) {
            return Fail(run, S, "choreography-invalid", fmt::format("{} errors", run.choreographyValidation.ErrorCount()));
        }
        return true;
    });
}

bool BuildSequence(GenerationRun& run) {
    return RunStage(run, GenerationStage::Spec, [&]() {
        const GenerationStage S = GenerationStage::Spec;
        std::vector<TimingTrackSpec> tracks;
        run.trackNames.clear();
        for (const TimingTrackExport& t : BuildTimingTracks(run.analysis, run.request.frameMS)) {
            tracks.push_back({ t.name, { t.marks } });
            run.trackNames[t.id] = t.name;
        }
        if (!run.lyricTrack.layers.empty()) {
            tracks.push_back(run.lyricTrack);
            run.trackNames["composer.lyrics"] = run.lyricTrack.name;
            for (const TimingTrackSpec& t : run.voiceTracks) {
                tracks.push_back(t);
            }
            for (const VoiceDef& v : run.cast.voices) {
                run.trackNames["composer.voice." + v.id] = VoiceTrackName(run.request.lyricTrackName, v);
            }
        }
        if (!run.choreography.mediaCues.empty()) {
            tracks.push_back({ run.request.mediaCueTrackName, CueLayers(run.choreography.mediaCues) });
            run.trackNames["composer.media-cues"] = run.request.mediaCueTrackName;
        }
        std::set<std::string> names;
        for (const TimingTrackSpec& t : tracks) {
            if (!names.insert(t.name).second || run.context->AllModels[t.name] != nullptr) {
                return Fail(run, S, "track-name-taken", t.name);
            }
        }
        run.spec = ToSequenceSpec(run.choreography, tracks);
        run.spec.mediaFile = fs::absolute(fs::path(run.request.audioPath)).string();
        run.spec.layoutFingerprint = run.layoutFingerprint;
        // A media cue only suggests where a picture could go while the figure keeps its
        // effect, so it is a timing mark and a sidecar entry, never a marker: a marker
        // reserves its interval dark until the file arrives.
        run.findings.clear();
        CheckChoreographyAgainstSong(run);
        int effectsOnGroups = 0;
        int effectsOnSubModels = 0;
        int layersAboveZero = 0;
        for (const EffectSpec& e : run.spec.effects) {
            effectsOnGroups += e.kind == TargetKind::Group ? 1 : 0;
            effectsOnSubModels += e.subModel.empty() ? 0 : 1;
            layersAboveZero += e.layer > 0 ? 1 : 0;
        }
        run.Stage(S).notes.push_back(fmt::format("{} effects ({} on groups, {} on submodels, {} above layer 0), {} timing tracks, {} markers, audio {}", run.spec.effects.size(), effectsOnGroups, effectsOnSubModels,
                                                 layersAboveZero, run.spec.timingTracks.size(), run.spec.markers.size(), fs::path(run.spec.mediaFile).filename().string()));
        for (const std::string& f : run.findings) {
            if (f.rfind("info ", 0) != 0) {
                Note(run, S, "finding", f);
            }
        }
        const int errors = FindingErrors(run.findings);
        if (errors > 0) {
            return Fail(run, S, "spec-invalid", fmt::format("{} findings are errors", errors));
        }
        if (run.spec.effects.empty()) {
            Note(run, S, "no-effects", "the song has nothing to light");
        }
        return true;
    });
}

bool WriteAndPublish(GenerationRun& run) {
    bool sidecarPublished = false;
    const bool ok = RunStage(run, GenerationStage::Write, [&]() {
        const GenerationRequest& r = run.request;
        const GenerationStage S = GenerationStage::Write;
        const fs::path report(r.reportDir);
        const std::string stem = fs::path(run.sequencePath).stem().string();
        const PublishCheck check = [&](const std::string& temporary, WriteResult& result) {
            if (CancelRequested(run, S)) {
                result.problems.push_back("cancelled");
                return false;
            }
            std::error_code ec;
            const auto size = fs::file_size(temporary, ec);
            const size_t perEffect = run.spec.effects.empty() ? 0 : static_cast<size_t>(size) / run.spec.effects.size();
            if (ec || size < 512 || (!run.spec.effects.empty() && perEffect < 40)) {
                result.problems.push_back(fmt::format("the written file has an implausible size: {} bytes for {} effects", static_cast<long long>(size), run.spec.effects.size()));
                return false;
            }
            const auto keepForDiagnosis = [&]() {
                std::error_code ignored;
                fs::copy_file(temporary, report / (stem + ".failed.xsq"), fs::copy_options::skip_existing, ignored);
            };
            if (r.render) {
                Progress(run, S, "render");
                HeadlessRenderContext ctx;
                if (!ctx.LoadShowFolder(r.showDir) || !ctx.OpenSequence(temporary)) {
                    result.problems.push_back("the written sequence could not be reopened for the render");
                    keepForDiagnosis();
                    return false;
                }
                const auto start = std::chrono::steady_clock::now();
                const bool rendered = ctx.RenderAndWait(r.renderTimeoutMS) && ctx.WriteFseq(run.fseqPath);
                run.render.renderMS = ElapsedMS(start);
                if (!rendered) {
                    result.problems.push_back("the headless render did not finish or the fseq could not be written");
                    keepForDiagnosis();
                    return false;
                }
                run.render.ran = true;
                ReadRenderSummary(run, ctx, run.fseqPath);
                Progress(run, S, "check render");
                run.renderValidation = ValidateRender(ctx, run.fseqPath, run.spec, Strictness::Generated, r.cancel);
                (void)ctx.CloseSequence();
                if (run.renderValidation.cancelled) {
                    run.cancelled = true;
                    result.problems.push_back("cancelled");
                    return false;
                }
                BuildCoverage(run);
                if (run.render.layoutChannels != 0 && run.render.channels != run.render.layoutChannels) {
                    // As on the desktop, the file only carries the controllers' channels.
                    run.findings.push_back(fmt::format("info fseq-channels: the file has {} channels and the layout {}; models without a controller are outside the file", run.render.channels, run.render.layoutChannels));
                }
                for (const TargetCoverage& t : run.coverage) {
                    if (t.plannedSegments > 0 && t.events == 0) {
                        run.findings.push_back(fmt::format("warning planned-not-lit: {} takes part in {} segments and has no effect ({})", t.target, t.plannedSegments, t.note.empty() ? "no reason given" : t.note));
                    }
                    if (t.written > 0 && t.effectsLit < t.written) {
                        run.findings.push_back(fmt::format("error effect-without-light: {} has {} effects and {} lit", t.target, t.written, t.effectsLit));
                    }
                }
                if (run.renderValidation.ErrorCount() > 0 || FindingErrors(run.findings) > 0) {
                    for (const ValidationIssue& i : run.renderValidation.issues) {
                        if (i.severity == IssueSeverity::Error) {
                            result.problems.push_back(fmt::format("{}: {}", i.code, DescribeIssue(i)));
                        }
                    }
                    for (const std::string& f : run.findings) {
                        if (f.rfind("error ", 0) == 0) {
                            result.problems.push_back(f);
                        }
                    }
                    keepForDiagnosis();
                    std::error_code ignored;
                    fs::rename(run.fseqPath, report / (stem + ".failed.fseq"), ignored);
                    return false;
                }
            }
            run.write.validation = result.validation;
            run.sidecar = MakeSidecar(run);
            // The assistant's answers go in the show's cache, so the sidecar can name them.
            const auto keep = [&](const std::string& source, const std::string& key, const char* kind) -> std::string {
                if (source.empty()) {
                    return {};
                }
                const fs::path target = AssistCachePath(r.showDir, key.empty() ? HashText(source) : key, kind);
                std::error_code ec;
                if (fs::weakly_canonical(fs::path(source), ec) != fs::weakly_canonical(target, ec)) {
                    fs::create_directories(target.parent_path(), ec);
                    fs::copy_file(fs::path(source), target, fs::copy_options::overwrite_existing, ec);
                    if (ec) {
                        return {};
                    }
                }
                const std::u8string relative = fs::relative(target, fs::path(r.showDir), ec).generic_u8string();
                return std::string(relative.begin(), relative.end());
            };
            run.sidecar.transcriptCache = run.transcript.words.empty() ? std::string() : keep(r.transcriptPath, run.transcript.cacheKey, "transcript");
            run.sidecar.characterCache = run.hasCharacter ? keep(r.characterPath, run.character.cacheKey, "character") : std::string();
            std::string error;
            if (!PublishNewFile(run.sidecarPath, ToJson(run.sidecar), error)) {
                result.problems.push_back("sidecar: " + error);
                return false;
            }
            sidecarPublished = true;
            return true;
        };
        Progress(run, S, "write");
        run.write = WriteSequenceSafely(r.showDir, run.spec, r.sequenceName, check);
        AddValidationNotes(run, S, run.write.validation, "sequence");
        if (run.render.ran) {
            AddValidationNotes(run, S, run.renderValidation, "render");
        }
        for (const std::string& f : run.findings) {
            if (f.rfind("warning ", 0) == 0) {
                Note(run, S, "finding", f);
            }
        }
        for (const std::string& left : run.write.leftoverFiles) {
            Note(run, S, "leftover-temporary", left);
        }
        if (!run.write.ok) {
            if (sidecarPublished) {
                std::error_code ignored;
                fs::remove(run.sidecarPath, ignored); // its sequence was never published
            }
            if (run.cancelled) {
                run.Stage(S).status = StageStatus::Cancelled;
                return false;
            }
            for (const std::string& p : run.write.problems) {
                run.Stage(S).notes.push_back("not published: " + p);
            }
            return Fail(run, S, "not-published", fmt::format("{} problems", run.write.problems.size()));
        }
        run.Stage(S).notes.push_back(fmt::format("published {} ({} effects, {} timing tracks)", run.write.finalPath, run.spec.effects.size(), run.spec.timingTracks.size()));
        return true;
    });
    if (!ok) {
        return false;
    }

    // After publishing, the sequence goes once more through xLights as a person would
    // use it: reopened from where it now lives, saved again, and reopened from that save.
    return RunStage(run, GenerationStage::Publish, [&]() {
        const GenerationRequest& r = run.request;
        const GenerationStage S = GenerationStage::Publish;
        const fs::path report(r.reportDir);
        HeadlessRenderContext opened;
        if (!opened.LoadShowFolder(r.showDir) || !opened.OpenSequence(run.sequencePath)) {
            return Fail(run, S, "published-unreadable", run.sequencePath);
        }
        const ValidationReport fromFile = ValidateSequenceFile(opened, run.sequencePath, &run.spec, Strictness::Generated);
        AddValidationNotes(run, S, fromFile, "published");
        const std::string original = SpecHash(SpecFromSequence(opened));
        run.resavedPath = (report / (fs::path(run.sequencePath).stem().string() + ".resaved.xsq")).string();
        std::string error;
        if (!ResaveSequence(opened, run.spec, run.resavedPath, error)) {
            return Fail(run, S, "resave-failed", error);
        }
        (void)opened.CloseSequence();
        HeadlessRenderContext reopened;
        if (!reopened.LoadShowFolder(r.showDir) || !reopened.OpenSequence(run.resavedPath)) {
            return Fail(run, S, "resaved-unreadable", run.resavedPath);
        }
        const ValidationReport resaved = ValidateSequenceFile(reopened, run.resavedPath, &run.spec, Strictness::FromFile);
        const std::string again = SpecHash(SpecFromSequence(reopened));
        (void)reopened.CloseSequence();
        AddValidationNotes(run, S, resaved, "resaved");
        run.Stage(S).notes.push_back(fmt::format("round trip: published {} and saved again {}: {}", original, again, original == again ? "identical" : "DIFFERENT"));
        if (fromFile.ErrorCount() > 0) {
            return Fail(run, S, "published-invalid", fmt::format("{} errors", fromFile.ErrorCount()));
        }
        if (original != again || resaved.ErrorCount() > 0) {
            return Fail(run, S, "round-trip-changed", fmt::format("{} errors after saving again", resaved.ErrorCount()));
        }
        return true;
    });
}

void WriteDiagnostics(GenerationRun& run) {
    if (run.request.reportDir.empty() || run.Stage(GenerationStage::Inputs).status == StageStatus::Failed) {
        return;
    }
    const fs::path report(run.request.reportDir);
    std::error_code ec;
    if (!fs::is_directory(report, ec)) {
        return;
    }
    bool ok = true;
    if (run.Stage(GenerationStage::Layout).status == StageStatus::Ok || run.Stage(GenerationStage::Layout).status == StageStatus::Warning) {
        ok &= WriteText(report / "capabilities.json", ToJson(run.capabilities));
    }
    if (!run.analysis.audioHash.empty()) {
        ok &= WriteText(report / "analysis.json", ToJson(run.analysis));
    }
    if (run.hasLyrics) {
        ok &= WriteText(report / "lyrics-alignment.json", ToJson(run.alignment));
    }
    if (!run.plan.segments.empty()) {
        ok &= WriteText(report / "composition-plan.json", ToJson(run.plan));
        ok &= WriteText(report / "composition-plan-report.txt", PlanReport(run.plan));
    }
    if (run.Stage(GenerationStage::Choreography).status != StageStatus::NotRun) {
        ok &= WriteText(report / "choreography.json", ToJson(run.choreography));
        ok &= WriteText(report / "choreography-report.txt", ChoreographyReport(run.choreography));
        if (run.direction.enabled) {
            ok &= WriteText(report / "visual-direction.json", ToJson(run.direction));
            ok &= WriteText(report / "visual-direction-report.txt", DirectionReport(run.direction));
            ok &= WriteText(report / "direction-metrics.json", ToJson(run.directionMetrics));
        }
        if (!run.mediaLibrary.items.empty() || !run.media.assignments.empty() || !run.media.warnings.empty()) {
            ok &= WriteText(report / "media-plan.json", ToJson(run.media));
            ok &= WriteText(report / "media-report.txt", MediaReport(run.media));
        }
        if (!run.media.surfaces.surfaces.empty()) {
            ok &= WriteText(report / "screens.json", SurfacePlanJson(run.media.surfaces).dump(2));
            ok &= WriteText(report / "screens-report.txt", SurfaceReport(run.media.surfaces));
        }
        if (!run.media.storyboard.lanes.empty()) {
            ok &= WriteText(report / "storyboard.json", StoryboardJson(run.media.storyboard).dump(2));
            if (!run.sidecar.review.Empty()) {
                ok &= WriteText(report / "review.json", VisualReviewJson(run.sidecar.review).dump(2));
            }
            ok &= WriteText(report / "storyboard-report.txt", StoryboardReport(run.media.storyboard));
        }
        if (!run.mediaLibrary.items.empty()) {
            ok &= WriteText(report / "media-analysis.txt", MediaAnalysisReport(run.mediaLibrary, run.mediaAnalysis));
        }
    }
    if (!run.transcriptAlignment.anchors.empty()) {
        nlohmann::json lines = nlohmann::json::array();
        for (const TranscriptLineMatch& l : run.transcriptAlignment.lines) {
            lines.push_back({ { "lineId", l.lineId }, { "words", l.words }, { "matched", l.matched }, { "confidence", l.confidence } });
        }
        ok &= WriteText(report / "transcript-alignment.json", nlohmann::json({ { "coverage", run.transcriptAlignment.coverage }, { "matchedWords", run.transcriptAlignment.matchedWords },
                                                                                 { "lyricWords", run.transcriptAlignment.lyricWords }, { "extraWords", run.transcriptAlignment.extraWords },
                                                                                 { "warnings", run.transcriptAlignment.warnings }, { "lines", lines } })
                                                                     .dump(1));
    }
    std::string validation;
    const auto dump = [&](const char* what, const ValidationReport& v) {
        for (const ValidationIssue& i : v.issues) {
            const char* severity = i.severity == IssueSeverity::Error ? "error" : i.severity == IssueSeverity::Warning ? "warning" : "info";
            const std::string when = i.startMS >= 0 ? fmt::format(" [{}-{} ms]", i.startMS, i.endMS) : std::string();
            validation += fmt::format("{}\t{}\t{}\t{}\t{}{}\n", what, severity, i.code, i.model, DescribeIssue(i), when);
        }
    };
    dump("plan", run.planValidation);
    dump("choreography", run.choreographyValidation);
    dump("sequence", run.write.validation);
    dump("render", run.renderValidation);
    for (const std::string& f : run.findings) {
        validation += "checks\t" + f + "\n";
    }
    ok &= WriteText(report / "validation.tsv", validation);
    ok &= WriteText(report / "summary.json", SummaryJson(run));
    ok &= WriteText(report / "generation-report.txt", GenerationReport(run));
    if (!ok) {
        run.problems.push_back("diagnostics: some files could not be written in " + report.string());
    }
}

GenerationRun GenerateShow(const GenerationRequest& request, GenerationStage last) {
    GenerationRun run;
    run.request = request;
    using Step = bool (*)(GenerationRun&);
    const std::pair<GenerationStage, Step> steps[] = {
        { GenerationStage::Inputs, &CheckInputs },
        { GenerationStage::Layout, &LoadLayout },
        { GenerationStage::Analysis, &AnalyzeAudio },
        { GenerationStage::Lyrics, &PrepareLyrics },
        { GenerationStage::Plan, &PlanShow },
        { GenerationStage::Choreography, &ChoreographShow },
        { GenerationStage::Spec, &BuildSequence },
        { GenerationStage::Write, &WriteAndPublish },
    };
    bool ok = true;
    for (const auto& [stage, step] : steps) {
        if (static_cast<int>(stage) > static_cast<int>(last)) {
            break;
        }
        if (!step(run)) {
            ok = false;
            break;
        }
    }
    if (run.context) {
        (void)run.context->CloseSequence();
    }
    if (!run.protectedHashes.empty() && ProtectedHashes(request.showDir) != run.protectedHashes) {
        run.problems.push_back("layout-modified: xlights_rgbeffects.xml or xlights_networks.xml changed during the generation");
        ok = false;
    }
    run.ok = ok && !run.cancelled;
    WriteDiagnostics(run);
    return run;
}

// ------------------------------------------------------------------ reports

std::string SummaryJson(const GenerationRun& run) {
    json root;
    root["pipelineVersion"] = PIPELINE_VERSION;
    root["ok"] = run.ok;
    root["cancelled"] = run.cancelled;
    root["sequence"] = run.write.ok ? run.sequencePath : std::string();
    root["sidecar"] = run.write.ok ? run.sidecarPath : std::string();
    root["fseq"] = run.render.ran ? run.fseqPath : std::string();
    root["problems"] = run.problems;
    json stages = json::object();
    for (int i = 0; i < GENERATION_STAGE_COUNT; ++i) {
        stages[STAGE_IDS[i]] = { { "status", STATUS_IDS[static_cast<int>(run.stages[i].status)] }, { "ms", run.stages[i].ms }, { "notes", run.stages[i].notes } };
    }
    root["stages"] = std::move(stages);
    root["level"] = LevelId(run.plan.level);
    root["seed"] = run.plan.seed;
    root["analysisFromCache"] = run.analysisFromCache;
    root["lyricsQuality"] = run.hasLyrics ? run.alignment.quality : "none";
    root["events"] = run.choreography.events.size();
    root["effectsWritten"] = run.write.ok ? run.spec.effects.size() : 0;
    root["timingTracks"] = run.spec.timingTracks.size();
    root["mediaCues"] = run.choreography.mediaCues.size();
    root["generationId"] = run.sidecar.generationId;
    root["specHash"] = run.spec.effects.empty() && run.spec.timingTracks.empty() ? std::string() : SpecHash(run.spec);
    root["choreographyHash"] = run.choreography.events.empty() ? std::string() : HashText(ToJson(run.choreography));
    root["validation"] = { { "planErrors", run.planValidation.ErrorCount() }, { "choreographyErrors", run.choreographyValidation.ErrorCount() }, { "sequenceErrors", run.write.validation.ErrorCount() },
                           { "renderErrors", run.renderValidation.ErrorCount() }, { "warnings", run.planValidation.WarningCount() + run.choreographyValidation.WarningCount() + run.write.validation.WarningCount() + run.renderValidation.WarningCount() } };
    root["render"] = { { "ran", run.render.ran }, { "frames", run.render.frames }, { "expectedFrames", run.render.expectedFrames }, { "stepMS", run.render.stepMS }, { "channels", run.render.channels },
                       { "layoutChannels", run.render.layoutChannels }, { "bytes", run.render.fileBytes }, { "ms", run.render.renderMS } };
    int participants = 0;
    int lit = 0;
    for (const TargetCoverage& t : run.coverage) {
        participants += t.plannedSegments > 0 ? 1 : 0;
        lit += t.framesLit > 0 ? 1 : 0;
    }
    root["participants"] = participants;
    root["targetsLit"] = lit;
    root["visualReview"] = "not-reviewed";
    if (run.direction.enabled) {
        const DirectionMetrics& m = run.directionMetrics;
        root["direction"] = { { "theme", run.direction.themeId }, { "themeName", run.direction.themeName }, { "source", run.direction.themeSource }, { "confidence", run.direction.themeConfidence },
                              { "coverage", m.coverageMean }, { "paletteCoherence", m.paletteCoherence }, { "hueConcentration", m.hueConcentration }, { "startsTogether", m.startsTogether },
                              { "eventsPerMinute", m.eventsPerMinute }, { "hits", m.hits }, { "repeatConsistency", m.repeatConsistency }, { "lookDiversity", m.lookDiversity }, { "layers", m.layersUsed } };
    }
    root["showDir"] = run.request.showDir;
    root["sequenceName"] = run.request.sequenceName;
    root["audio"] = run.request.audioPath;
    root["durationMS"] = run.spec.durationMS > 0 ? run.spec.durationMS : run.analysis.durationMS;
    root["lyricTrack"] = run.lyricTrack.layers.empty() ? std::string() : run.lyricTrack.name;
    nlohmann::json voices = nlohmann::json::array();
    for (const VoiceDef& v : run.cast.voices) {
        int phrases = 0;
        for (const AlignedPhrase& p : run.alignment.phrases) {
            phrases += p.startMS >= 0 && std::find(p.voices.begin(), p.voices.end(), v.id) != p.voices.end() ? 1 : 0;
        }
        voices.push_back({ { "id", v.id }, { "name", v.name }, { "role", VoiceRoleId(v.role) }, { "delivery", DeliveryId(v.delivery) }, { "faces", v.faces }, { "facesOff", v.facesOff }, { "phrases", phrases } });
    }
    root["voices"] = std::move(voices);
    nlohmann::json media = nlohmann::json::array();
    for (const MediaAssignment& a : run.media.assignments) {
        media.push_back({ { "segment", a.segmentId }, { "target", a.target }, { "startMS", a.startMS }, { "endMS", a.endMS }, { "media", a.mediaId }, { "name", a.name }, { "path", a.path },
                          { "effect", a.effect }, { "state", DecisionStateId(a.state) }, { "constraints", a.constraints } });
    }
    root["media"] = { { "assignments", std::move(media) }, { "share", run.media.share }, { "maxShare", run.media.maxShare }, { "usable", run.media.usableItems }, { "library", run.mediaLibrary.items.size() } };
    if (!run.media.storyboard.lanes.empty()) {
        const StoryboardStats& st = run.media.storyboard.stats;
        nlohmann::json lanes = nlohmann::json::array();
        for (const StoryboardLaneStats& l : st.lanes) {
            lanes.push_back({ { "lane", l.lane }, { "shots", l.shots }, { "coverage", l.coverage }, { "ofFree", l.ofFree } });
        }
        nlohmann::json screens = nlohmann::json::array();
        for (const StoryboardLane& l : run.media.storyboard.lanes) {
            screens.push_back({ { "target", l.target }, { "screen", l.screen }, { "gapsOnly", l.gapsOnly } });
        }
        root["storyboard"] = { { "hash", run.media.storyboard.hash }, { "analysis", run.media.storyboard.analysisMode }, { "shots", st.shots }, { "files", st.distinctFiles }, { "images", st.images + st.animations },
                               { "videos", st.videos }, { "derived", st.derived }, { "repeats", st.repeats }, { "repeatsByMusic", st.repeatsByMusic }, { "gaps", st.gaps }, { "meanScore", st.meanScore },
                               { "lanes", std::move(lanes) }, { "screens", std::move(screens) }, { "lyricsMode", run.media.storyboard.lyricsMode }, { "meanShotMS", st.meanShotMS },
                               { "transitions", st.transitions }, { "coherence", st.coherence }, { "crossScreen", st.crossScreen }, { "overlays", st.overlays }, { "motionClips", st.motionClips } };
    }
    json coverage = json::array();
    for (const TargetCoverage& t : run.coverage) {
        coverage.push_back({ { "target", t.target }, { "planned", t.plannedSegments }, { "events", t.events }, { "written", t.written }, { "lit", t.effectsLit }, { "framesLit", t.framesLit }, { "note", t.note } });
    }
    root["coverage"] = std::move(coverage);
    root["findings"] = run.findings;
    json cues = json::array();
    for (const MediaCue& m : run.choreography.mediaCues) {
        cues.push_back({ { "id", m.id }, { "target", m.target }, { "startMS", m.startMS }, { "endMS", m.endMS }, { "kind", m.kind } });
    }
    root["mediaCueList"] = std::move(cues);
    std::set<std::string> faces;
    std::set<std::string> texts;
    for (const ChoreographyEvent& e : run.choreography.events) {
        if (e.effect == "Faces") {
            faces.insert(e.target);
        } else if (e.effect == "Text") {
            texts.insert(e.target);
        }
    }
    root["faceTargets"] = faces;
    root["textTargets"] = texts;
    return root.dump(1);
}

std::string GenerationReport(const GenerationRun& run) {
    std::string out;
    const GenerationRequest& r = run.request;
    out += fmt::format("Show Composer generation {} — {}\n", PIPELINE_VERSION, run.ok ? "PUBLISHED" : run.cancelled ? "CANCELLED" : "NOT PUBLISHED");
    out += fmt::format("audio {}\nshow {}\nsequence {}\nlevel {}, seed {}, frame {} ms\n", r.audioPath, r.showDir, run.sequencePath, LevelId(run.plan.level), run.plan.seed, r.frameMS);
    out += "Nothing here is reviewed by a person: technically valid and rendered is not the same as looking right.\n\n";
    out += "Stages\n";
    for (int i = 0; i < GENERATION_STAGE_COUNT; ++i) {
        const StageOutcome& s = run.stages[i];
        out += fmt::format("  {:<13} {:<9} {:>7} ms\n", STAGE_IDS[i], STATUS_IDS[static_cast<int>(s.status)], s.ms);
        for (const std::string& n : s.notes) {
            out += "      " + n + "\n";
        }
    }
    if (!run.problems.empty()) {
        out += "\nProblems\n";
        for (const std::string& p : run.problems) {
            out += "  " + p + "\n";
        }
    }
    if (run.render.ran) {
        const RenderSummary& s = run.render;
        out += fmt::format("\nRender: {} frames (expected {}), {} ms step, {} channels (layout {}), {} bytes, {} ms\n", s.frames, s.expectedFrames, s.stepMS, s.channels, s.layoutChannels, s.fileBytes, s.renderMS);
    }
    if (!run.coverage.empty()) {
        out += "\nTargets: planned segments, events, effects written, effects lit, frames lit\n";
        for (const TargetCoverage& t : run.coverage) {
            out += fmt::format("  {:<32} {:>3} {:>4} {:>4} {:>4} {:>7}  {}\n", t.target, t.plannedSegments, t.events, t.written, t.effectsLit, t.framesLit, t.note);
        }
    }
    if (!run.findings.empty()) {
        out += "\nChecks\n";
        for (const std::string& f : run.findings) {
            out += "  " + f + "\n";
        }
    }
    out += fmt::format("\nIdentity: generation {}, spec {}, choreography {}\n", run.sidecar.generationId, run.spec.effects.empty() ? std::string("-") : SpecHash(run.spec),
                       run.choreography.events.empty() ? std::string("-") : HashText(ToJson(run.choreography)));
    out += "Visual review: not done. Templates are starting values (visuallyValidated false).\n";
    return out;
}

} // namespace composer
