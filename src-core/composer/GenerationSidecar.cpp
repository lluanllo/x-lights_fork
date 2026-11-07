#include "composer/GenerationSidecar.h"

#include "utils/ExternalHooks.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

constexpr const char* LEVEL_IDS[] = { "conservative", "balanced", "creative" };
constexpr const char* STATUS_IDS[] = { "ok", "missing", "damaged", "newer", "incompatible" };

// The corrections go through the serializers that own them, so the sidecar never
// holds a second definition of their form.
json PlanCorrectionsJson(const std::vector<PlanCorrection>& corrections) {
    CompositionPlan holder;
    holder.corrections = corrections;
    return json::parse(ToJson(holder)).at("corrections");
}

json ChoreographyCorrectionsJson(const std::vector<ChoreographyCorrection>& corrections) {
    Choreography holder;
    holder.corrections = corrections;
    return json::parse(ToJson(holder)).at("corrections");
}

json LyricCorrectionsJson(const std::vector<LyricCorrection>& corrections) {
    LyricSession holder;
    holder.corrections = corrections;
    return json::parse(ToJson(holder)).at("corrections");
}

json AnalysisCorrectionsJson(const AnalysisCorrections& corrections) {
    SongAnalysis holder;
    holder.corrections = corrections;
    return json::parse(ToJson(holder)).at("corrections");
}

bool AnalysisCorrectionsFrom(const json& object, AnalysisCorrections& out, std::string& error) {
    json holder = json::parse(ToJson(SongAnalysis()));
    holder["corrections"] = object;
    SongAnalysis read;
    if (!object.is_object() || !FromJson(holder.dump(), read, error)) {
        error = "analysis corrections: " + (error.empty() ? std::string("not an object") : error);
        return false;
    }
    out = std::move(read.corrections);
    return true;
}

bool PlanCorrectionsFrom(const json& list, std::vector<PlanCorrection>& out, std::string& error) {
    CompositionPlan holder;
    if (!list.is_array() || !FromJson(json{ { "schemaVersion", PLAN_SCHEMA_VERSION }, { "corrections", list } }.dump(), holder, error)) {
        error = "plan corrections: " + (error.empty() ? std::string("not a list") : error);
        return false;
    }
    out = std::move(holder.corrections);
    return true;
}

bool ChoreographyCorrectionsFrom(const json& list, std::vector<ChoreographyCorrection>& out, std::string& error) {
    Choreography holder;
    if (!list.is_array() || !FromJson(json{ { "schemaVersion", CHOREOGRAPHY_SCHEMA_VERSION }, { "corrections", list } }.dump(), holder, error)) {
        error = "choreography corrections: " + (error.empty() ? std::string("not a list") : error);
        return false;
    }
    out = std::move(holder.corrections);
    return true;
}

bool LyricCorrectionsFrom(const json& list, std::vector<LyricCorrection>& out, std::string& error) {
    LyricSession holder;
    if (!list.is_array() || !FromJson(json{ { "schemaVersion", LYRIC_ALIGNMENT_SCHEMA_VERSION }, { "corrections", list } }.dump(), holder, error)) {
        error = "lyric corrections: " + (error.empty() ? std::string("not a list") : error);
        return false;
    }
    out = std::move(holder.corrections);
    return true;
}

} // namespace

std::string ToJson(const GenerationSidecar& s) {
    json root;
    root["schemaVersion"] = s.schemaVersion;
    root["generator"] = { { "pipelineVersion", s.pipelineVersion }, { "analyzerVersion", s.analyzerVersion }, { "plannerVersion", s.plannerVersion }, { "choreographerVersion", s.choreographerVersion } };
    root["generationId"] = s.generationId;
    root["sequenceFile"] = s.sequenceFile;
    root["inputs"] = { { "audioFile", s.audioFile },
                       { "audioPath", s.audioPath },
                       { "audioHash", s.audioHash },
                       { "audioSize", s.audioSize },
                       { "analysisOptionsKey", s.analysisOptionsKey },
                       { "analysisHash", s.analysisHash },
                       { "layoutFingerprint", s.layoutFingerprint },
                       { "profileId", s.profileId },
                       { "profileHash", s.profileHash },
                       { "libraryHash", s.libraryHash },
                       { "lyricsHash", s.lyricsHash },
                       { "lyricsQuality", s.lyricsQuality },
                       { "level", LEVEL_IDS[static_cast<int>(s.level)] },
                       { "seed", s.seed },
                       { "frameMS", s.frameMS } };
    root["outputs"] = { { "planHash", s.planHash }, { "choreographyHash", s.choreographyHash }, { "specHash", s.specHash }, { "durationMS", s.durationMS }, { "effects", s.effects } };
    root["tracks"] = s.tracks;
    json cues = json::array();
    for (const MediaCue& m : s.mediaCues) {
        cues.push_back({ { "id", m.id }, { "target", m.target }, { "startMS", m.startMS }, { "endMS", m.endMS }, { "kind", m.kind }, { "reason", m.reason } });
    }
    root["mediaCues"] = std::move(cues);
    root["corrections"] = { { "plan", PlanCorrectionsJson(s.planCorrections) }, { "choreography", ChoreographyCorrectionsJson(s.choreographyCorrections) }, { "analysis", AnalysisCorrectionsJson(s.analysisCorrections) } };
    root["lyrics"] = json::parse(ToJson(s.lyrics));
    root["validation"] = { { "errors", s.validationErrors }, { "warnings", s.validationWarnings }, { "rendered", s.rendered }, { "visualReview", s.visualReview } };
    if (!s.transcriptCache.empty() || !s.characterCache.empty()) {
        root["assist"] = { { "transcript", s.transcriptCache }, { "character", s.characterCache } };
    }
    if (!s.media.Empty()) {
        root["media"] = MediaSessionJson(s.media);
    }
    return root.dump(1);
}

SidecarStatus FromJson(const std::string& text, GenerationSidecar& sidecar, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return SidecarStatus::Damaged;
    }
    try {
        GenerationSidecar s;
        s.schemaVersion = root.at("schemaVersion").get<int>();
        if (s.schemaVersion > SIDECAR_SCHEMA_VERSION) {
            error = fmt::format("schema {} is newer than {}", s.schemaVersion, SIDECAR_SCHEMA_VERSION);
            return SidecarStatus::Newer;
        }
        if (s.schemaVersion < 1) {
            error = fmt::format("schema {} does not exist", s.schemaVersion);
            return SidecarStatus::Damaged;
        }
        const json& generator = root.at("generator");
        s.pipelineVersion = generator.value("pipelineVersion", std::string());
        s.analyzerVersion = generator.value("analyzerVersion", std::string());
        s.plannerVersion = generator.value("plannerVersion", std::string());
        s.choreographerVersion = generator.value("choreographerVersion", std::string());
        s.generationId = root.at("generationId").get<std::string>();
        s.sequenceFile = root.at("sequenceFile").get<std::string>();
        const json& inputs = root.at("inputs");
        s.audioFile = inputs.value("audioFile", std::string());
        s.audioPath = inputs.value("audioPath", std::string());
        s.audioHash = inputs.at("audioHash").get<std::string>();
        s.audioSize = inputs.value("audioSize", 0L);
        s.analysisOptionsKey = inputs.value("analysisOptionsKey", std::string());
        s.analysisHash = inputs.value("analysisHash", std::string());
        s.layoutFingerprint = inputs.value("layoutFingerprint", std::string());
        s.profileId = inputs.value("profileId", std::string());
        s.profileHash = inputs.value("profileHash", std::string());
        s.libraryHash = inputs.value("libraryHash", std::string());
        s.lyricsHash = inputs.value("lyricsHash", std::string());
        s.lyricsQuality = inputs.value("lyricsQuality", std::string("none"));
        const std::string level = inputs.value("level", std::string("balanced"));
        s.level = level == "conservative" ? AutomationLevel::Conservative : level == "creative" ? AutomationLevel::Creative : AutomationLevel::Balanced;
        s.seed = inputs.value("seed", static_cast<uint64_t>(0));
        s.frameMS = inputs.value("frameMS", 25);
        const json outputs = root.value("outputs", json::object());
        s.planHash = outputs.value("planHash", std::string());
        s.choreographyHash = outputs.value("choreographyHash", std::string());
        s.specHash = outputs.value("specHash", std::string());
        s.durationMS = outputs.value("durationMS", 0);
        s.effects = outputs.value("effects", 0);
        s.tracks = root.value("tracks", std::map<std::string, std::string>());
        for (const json& m : root.value("mediaCues", json::array())) {
            s.mediaCues.push_back({ m.value("id", std::string()), m.value("target", std::string()), m.value("startMS", 0), m.value("endMS", 0), m.value("kind", std::string()), m.value("reason", std::string()) });
        }
        const json& corrections = root.at("corrections");
        if (!PlanCorrectionsFrom(corrections.at("plan"), s.planCorrections, error) || !ChoreographyCorrectionsFrom(corrections.at("choreography"), s.choreographyCorrections, error)) {
            return SidecarStatus::Damaged;
        }
        if (corrections.contains("analysis") && !AnalysisCorrectionsFrom(corrections["analysis"], s.analysisCorrections, error)) {
            return SidecarStatus::Damaged;
        }
        if (root.contains("lyrics") && !FromJson(root["lyrics"].dump(), s.lyrics, error)) {
            error = "lyrics: " + error;
            return SidecarStatus::Damaged;
        }
        const json validation = root.value("validation", json::object());
        s.validationErrors = validation.value("errors", 0);
        s.validationWarnings = validation.value("warnings", 0);
        s.rendered = validation.value("rendered", false);
        s.visualReview = validation.value("visualReview", std::string("not-reviewed"));
        const json assist = root.value("assist", json::object());
        s.transcriptCache = assist.value("transcript", std::string());
        s.characterCache = assist.value("character", std::string());
        if (root.contains("media") && !MediaSessionFrom(root["media"], s.media, error)) {
            return SidecarStatus::Damaged;
        }
        sidecar = std::move(s);
    } catch (const json::exception& e) {
        error = e.what();
        return SidecarStatus::Damaged;
    }
    return SidecarStatus::Ok;
}

SidecarStatus ReadSidecar(const std::string& path, GenerationSidecar& sidecar, std::string& error) {
    ObtainAccessToURL(path, false);
    if (!FileExists(path)) {
        error = fmt::format("'{}' does not exist", path);
        return SidecarStatus::Missing;
    }
    std::ifstream in(fs::path(path), std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    if (!in && !in.eof()) {
        error = fmt::format("cannot read '{}'", path);
        return SidecarStatus::Damaged;
    }
    return FromJson(text.str(), sidecar, error);
}

SidecarStatus ReadSidecarFor(const std::string& path, const std::string& audioHash, GenerationSidecar& sidecar, std::string& error) {
    const SidecarStatus status = ReadSidecar(path, sidecar, error);
    if (status == SidecarStatus::Ok && !audioHash.empty() && sidecar.audioHash != audioHash) {
        error = "it was made for another audio file";
        return SidecarStatus::Incompatible;
    }
    return status;
}

const char* SidecarStatusId(SidecarStatus status) {
    return STATUS_IDS[static_cast<int>(status)];
}

std::string SidecarPathFor(const std::string& sequencePath) {
    fs::path path(sequencePath);
    path.replace_extension(".composer.json");
    return path.string();
}

bool CorrectionSet::Empty() const {
    return plan.empty() && choreography.empty() && lyrics.empty() && media.empty() && analysis.events.empty() && analysis.sections.empty() && analysis.bar.firstDownbeatMS < 0;
}

void MergeAnalysisCorrections(AnalysisCorrections& into, const AnalysisCorrections& extra) {
    into.events.insert(into.events.end(), extra.events.begin(), extra.events.end());
    into.sections.insert(into.sections.end(), extra.sections.begin(), extra.sections.end());
    if (extra.bar.firstDownbeatMS >= 0) {
        into.bar = extra.bar;
    }
}

void MergeCorrections(CorrectionSet& into, const CorrectionSet& extra) {
    into.plan.insert(into.plan.end(), extra.plan.begin(), extra.plan.end());
    into.choreography.insert(into.choreography.end(), extra.choreography.begin(), extra.choreography.end());
    for (const LyricCorrection& c : extra.lyrics) {
        UpsertCorrection(into.lyrics, c);
    }
    MergeAnalysisCorrections(into.analysis, extra.analysis);
    for (const MediaCorrection& c : extra.media) {
        UpsertMediaCorrection(into.media, c);
    }
}

std::string ToJson(const CorrectionSet& c) {
    json root;
    root["schemaVersion"] = SIDECAR_SCHEMA_VERSION;
    root["plan"] = PlanCorrectionsJson(c.plan);
    root["choreography"] = ChoreographyCorrectionsJson(c.choreography);
    root["lyrics"] = LyricCorrectionsJson(c.lyrics);
    root["analysis"] = AnalysisCorrectionsJson(c.analysis);
    if (!c.media.empty()) {
        root["media"] = MediaCorrectionsJson(c.media);
    }
    return root.dump(1);
}

bool FromJson(const std::string& text, CorrectionSet& corrections, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    if (root.value("schemaVersion", 0) > SIDECAR_SCHEMA_VERSION) {
        error = "written by a newer version";
        return false;
    }
    CorrectionSet out;
    if ((root.contains("plan") && !PlanCorrectionsFrom(root["plan"], out.plan, error)) || (root.contains("choreography") && !ChoreographyCorrectionsFrom(root["choreography"], out.choreography, error)) ||
        (root.contains("lyrics") && !LyricCorrectionsFrom(root["lyrics"], out.lyrics, error)) || (root.contains("analysis") && !AnalysisCorrectionsFrom(root["analysis"], out.analysis, error)) ||
        (root.contains("media") && !MediaCorrectionsFrom(root["media"], out.media, error))) {
        return false;
    }
    corrections = std::move(out);
    return true;
}

} // namespace composer
