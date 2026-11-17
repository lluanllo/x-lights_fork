#include "composer/ProductionCheck.h"

#include "composer/GenerationSidecar.h"
#include "composer/ShowGenerationPipeline.h"
#include "composer/SongAnalysis.h"
#include "composer/StageDirector.h"
#include "models/Model.h"
#include "models/ModelGroup.h"
#include "outputs/Controller.h"
#include "outputs/OutputManager.h"
#include "render/FSEQFile.h"
#include "render/HeadlessRenderContext.h"
#include "utils/ExternalHooks.h"
#include "utils/FileUtils.h"
#include "xLightsVersion.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <memory>
#include <regex>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace composer {

namespace {

using json = nlohmann::json;

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool Under(const std::string& path, const std::string& folder) {
    if (folder.empty() || path.empty()) {
        return false;
    }
    std::error_code ec;
    const std::string p = Lower(fs::weakly_canonical(fs::path(path), ec).generic_string());
    std::string f = Lower(fs::weakly_canonical(fs::path(folder), ec).generic_string());
    if (!f.empty() && f.back() != '/') {
        f += '/';
    }
    return p.rfind(f, 0) == 0;
}

// A setting that names a file of the show: a picture, a video, a shader, a model, a sound.
bool NamesAFile(const std::string& key, const std::string& value) {
    if (value.empty() || value.size() < 5) {
        return false;
    }
    static const std::set<std::string> EXT = { ".mp4", ".avi", ".mov", ".mkv", ".m4v", ".webm", ".wmv", ".mpg", ".mpeg", ".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp",
                                                ".svg", ".fs", ".glsl", ".obj", ".gltf", ".glb", ".mp3", ".wav", ".ogg", ".m4a", ".flac" };
    const std::string ext = Lower(fs::path(value).extension().string());
    const bool fileKey = key.find("Filename") != std::string::npos || key.find("FILEPICKER") != std::string::npos || key.find("_File") != std::string::npos;
    return EXT.contains(ext) && (fileKey || value.find('/') != std::string::npos || value.find('\\') != std::string::npos);
}

std::string Now() {
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm {};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

class Checker {
public:
    explicit Checker(const ProductionInput& in) : _in(in) {}

    ProductionReport Run() {
        ProductionReport& r = _r;
        r.sequence = _in.sequencePath;
        r.xlightsVersion = xlights_version_string;
        for (const std::string& a : ProductionAreas()) {
            r.checks.push_back({ a, "not-checked", "not checked", {} });
        }
        if (_in.sequencePath.empty() || !FileExists(_in.sequencePath)) {
            Issue("blocker", "layout", "sequence-missing", "The sequence {} is not there", { _in.sequencePath });
            Set("layout", "blocked", "the sequence is not there");
            r.ran = true;
            return r;
        }
        std::error_code ec;
        _work = _in.workDir.empty() ? (fs::path(_in.sequencePath).parent_path() / "ShowComposer" / "production-check" / fs::path(_in.sequencePath).stem()).string() : _in.workDir;
        fs::create_directories(_work, ec);
        Step("the notes the composer left");
        Sidecar();
        if (Cancelled()) return Stop();
        Step("the show and its layout");
        if (!Open()) {
            r.ran = true;
            return r;
        }
        if (Cancelled()) return Stop();
        Step("the audio");
        Audio();
        Step("the lyrics and the song's parts");
        Lyrics();
        Structure();
        if (Cancelled()) return Stop();
        Step("the figures and controllers");
        Figures();
        Step("the pictures and videos");
        Media();
        if (Cancelled()) return Stop();
        Step("the effects");
        Effects();
        Direction();
        Locks();
        Review();
        if (Cancelled()) return Stop();
        if (_in.render) {
            Step("the render");
            Render();
            if (Cancelled()) return Stop();
        }
        if (_in.roundTrip) {
            Step("saving it again in xLights");
            RoundTrip();
        }
        Hashes();
        r.channels = static_cast<int>(_ctx.GetSeqData().NumChannels());
        r.status.generated = _hasSidecar;
        r.status.technical = r.Count("blocker") == 0 && _validationErrors == 0;
        r.ran = true;
        return r;
    }

private:
    void Step(const std::string& what) const {
        if (_in.progress) {
            _in.progress(what);
        }
    }
    bool Cancelled() const { return _in.cancel && _in.cancel(); }
    ProductionReport Stop() {
        _r.cancelled = true;
        _r.ran = true;
        return _r;
    }
    void Issue(const char* level, const char* area, const char* code, const char* text, std::vector<std::string> args = {}) {
        _r.issues.push_back({ level, area, code, text, std::move(args) });
    }
    void Set(const char* area, const char* status, const std::string& summary, std::vector<std::string> args = {}) {
        for (ProductionCheckItem& c : _r.checks) {
            if (c.area == area) {
                c.status = status;
                c.summary = summary;
                c.args = std::move(args);
            }
        }
    }

    void Sidecar() {
        _r.sidecar = SidecarPathFor(_in.sequencePath);
        std::string error;
        const SidecarStatus st = ReadSidecar(_r.sidecar, _sc, error);
        _hasSidecar = st == SidecarStatus::Ok;
        if (!_hasSidecar) {
            Issue("warning", "review", "sidecar-missing", "The composer's notes for this sequence were not found: it is checked as any sequence");
        }
    }

    bool Open() {
        if (!_ctx.LoadShowFolder(_in.showDir)) {
            Issue("blocker", "layout", "show-unreadable", "The show folder {} has no layout xLights can read", { _in.showDir });
            Set("layout", "blocked", "the layout cannot be read");
            return false;
        }
        if (!_ctx.OpenSequence(_in.sequencePath)) {
            Issue("blocker", "layout", "sequence-unreadable", "xLights cannot open the sequence");
            Set("layout", "blocked", "xLights cannot open the sequence");
            return false;
        }
        _spec = SpecFromSequence(_ctx);
        _r.specHash = SpecHash(_spec);
        _r.durationMS = static_cast<int>(_ctx.GetSeqData().NumFrames() * _ctx.GetSeqData().FrameTime());
        _r.layoutFingerprint = LayoutFingerprint(_ctx);
        for (const auto& [name, model] : _ctx.AllModels) {
            if (model != nullptr && model->GetDisplayAs() != DisplayAsType::ModelGroup) {
                ++_r.models;
            }
        }
        if (_hasSidecar && !_sc.layoutFingerprint.empty() && _sc.layoutFingerprint != _r.layoutFingerprint) {
            Issue("risk", "layout", "layout-changed", "The layout changed after the show was made: some effects may light other lights. Generate it again or check it in xLights");
            Set("layout", "problem", "the layout changed after the show was made");
        } else {
            Set("layout", "ok", "{} models; the layout is the one the show was made for", { std::to_string(_r.models) });
        }
        return true;
    }

    void Audio() {
        std::string media = _spec.mediaFile;
        if (media.empty() && _hasSidecar) {
            media = _sc.audioPath;
        }
        if (media.empty()) {
            Issue("blocker", "audio", "audio-none", "The sequence has no song");
            Set("audio", "blocked", "no song");
            return;
        }
        std::string found = FileExists(media) ? media : FileUtils::FixFile(_in.showDir, media);
        if (found.empty() || !FileExists(found)) {
            Issue("blocker", "audio", "audio-missing", "The song {} is not there: the show would play in silence", { fs::path(media).filename().string() });
            Set("audio", "blocked", "the song is not there");
            return;
        }
        _r.audio = found;
        if (_hasSidecar && !_sc.audioHash.empty()) {
            const std::string hash = Sha256OfFile(found);
            if (!hash.empty() && hash != _sc.audioHash) {
                Issue("risk", "audio", "audio-other", "The song file is not the one the show was made with: the lights may not follow it");
                Set("audio", "problem", "another file than the one the show was made with");
                return;
            }
        }
        if (!Under(found, _in.showDir) && !InMediaFolder(found)) {
            Issue("warning", "audio", "audio-outside", "The song is outside the show folder: copy it into the show before moving it to another computer");
        }
        if (_hasSidecar && _sc.durationMS > 0 && std::abs(_sc.durationMS - _r.durationMS) > 1000) {
            Issue("risk", "audio", "duration-differs", "The sequence lasts {} s and the song {} s", { fmt::format("{:.1f}", _r.durationMS / 1000.0), fmt::format("{:.1f}", _sc.durationMS / 1000.0) });
        }
        Set("audio", "ok", "{} ({} s)", { fs::path(found).filename().string(), fmt::format("{:.1f}", _r.durationMS / 1000.0) });
    }

    bool InMediaFolder(const std::string& path) const {
        for (const std::string& f : _ctx.GetMediaFolders()) {
            if (Under(path, f)) {
                return true;
            }
        }
        return false;
    }

    void Lyrics() {
        int faces = 0;
        for (const EffectSpec& e : _spec.effects) {
            faces += e.effect == "Faces";
        }
        const std::string quality = _hasSidecar ? _sc.lyricsQuality : std::string();
        if (faces == 0) {
            Set("lyrics", "ok", "no face sings in this show");
            return;
        }
        if (quality.empty() || quality == "none" || quality == "unsynchronized") {
            Issue("risk", "lyrics", "lyrics-unsynchronized", "{} faces sing but the lyrics are not synchronized: the mouths will not follow the voice", { std::to_string(faces) });
            Set("lyrics", "problem", "the lyrics are not synchronized");
        } else if (quality == "approximate" || quality == "experimental") {
            Issue("warning", "lyrics", "lyrics-approximate", "The lyrics' times are approximate: watch the mouths once in xLights");
            Set("lyrics", "warning", "lyrics synchronized approximately");
        } else {
            Set("lyrics", "ok", "lyrics synchronized; {} face effects", { std::to_string(faces) });
        }
    }

    void Structure() {
        const int tracks = static_cast<int>(_spec.timingTracks.size());
        const AnalysisCorrections& ac = _sc.analysisCorrections;
        if (_hasSidecar && ac.events.empty() && ac.sections.empty() && ac.tempoFactor == 1) {
            Issue("artistic", "structure", "structure-unchecked", "Nobody has checked the song's parts and beats: if a change of light falls wrong, correct them in step 2");
        }
        Set("structure", tracks > 0 ? "ok" : "warning", "{} timing tracks", { std::to_string(tracks) });
    }

    void Figures() {
        OutputManager* om = _ctx.GetOutputManager();
        if (om != nullptr) {
            for (const Controller* c : om->GetControllers()) {
                ++_r.controllers;
                _r.controllersActive += c->IsActive() ? 1 : 0;
            }
        }
        if (_r.controllers > 0 && _r.controllersActive == 0) {
            Issue("warning", "controllers", "controllers-inactive", "No controller is switched on in this show folder: the lights will not come on until they are switched on in xLights (the composer never switches them on)");
            Set("controllers", "warning", "{} controllers, none switched on", { std::to_string(_r.controllers) });
        } else {
            Set("controllers", "ok", "{} controllers, {} switched on", { std::to_string(_r.controllers), std::to_string(_r.controllersActive) });
        }
        // Figures whose lights are also another figure's: what one shows, the other shows too.
        std::vector<const Model*> models;
        std::set<std::string> used;
        for (const EffectSpec& e : _spec.effects) {
            used.insert(e.target);
        }
        for (const auto& [name, model] : _ctx.AllModels) {
            if (model != nullptr && model->GetDisplayAs() != DisplayAsType::ModelGroup && model->GetLastChannel() >= model->GetFirstChannel() && model->GetLastChannel() > 0) {
                models.push_back(model);
            }
        }
        int shared = 0, unwired = 0;
        std::string example;
        // When each model gets effects: its own rows and the groups that hold it.
        std::map<std::string, std::vector<std::pair<int, int>>> lit;
        std::map<std::string, size_t> row; // where xLights first draws a model's light
        for (size_t i = 0; i < _spec.effects.size(); ++i) {
            const EffectSpec& e = _spec.effects[i];
            const Model* m = _ctx.AllModels[e.target];
            std::vector<std::string> reached;
            if (const ModelGroup* g = dynamic_cast<const ModelGroup*>(m); g != nullptr) {
                for (const Model* x : g->GetFlatModels(true, false)) {
                    reached.push_back(x->GetName());
                }
            } else {
                reached.push_back(e.target);
            }
            for (const std::string& r : reached) {
                lit[r].push_back({ e.startMS, e.endMS });
                row.emplace(r, i);
            }
        }
        const auto both = [&](const std::string& a, const std::string& b) {
            // The time both have light at once (each list merged first).
            const auto merged = [&](const std::string& n) {
                std::vector<std::pair<int, int>> v = lit[n];
                std::sort(v.begin(), v.end());
                std::vector<std::pair<int, int>> out;
                for (const auto& x : v) {
                    if (!out.empty() && x.first <= out.back().second) {
                        out.back().second = std::max(out.back().second, x.second);
                    } else {
                        out.push_back(x);
                    }
                }
                return out;
            };
            const auto va = merged(a), vb = merged(b);
            int total = 0;
            size_t i = 0, j = 0;
            while (i < va.size() && j < vb.size()) {
                total += std::max(0, std::min(va[i].second, vb[j].second) - std::max(va[i].first, vb[j].first));
                (va[i].second < vb[j].second ? i : j)++;
            }
            return total;
        };
        // A person's choice of which one wins, kept with the song's stage edits.
        std::set<std::pair<std::string, std::string>> chosen;
        if (_hasSidecar && !_sc.audioHash.empty()) {
            std::string text, error;
            StageEdits edits;
            std::ifstream f(StageEditsPath(_in.showDir, _sc.audioHash));
            if (f) {
                std::stringstream ss;
                ss << f.rdbuf();
                if (FromJson(ss.str(), edits, error)) {
                    for (const StageEdit& e : edits.edits) {
                        const size_t bar = e.value.find('|');
                        if (e.action == "priority" && bar != std::string::npos) {
                            chosen.insert({ e.value.substr(0, bar), e.value.substr(bar + 1) });
                        }
                    }
                }
            }
        }
        for (size_t i = 0; i < models.size(); ++i) {
            for (size_t j = i + 1; j < models.size(); ++j) {
                const Model* a = models[i];
                const Model* b = models[j];
                if (a->GetFirstChannel() <= b->GetLastChannel() && b->GetFirstChannel() <= a->GetLastChannel() && (used.contains(a->GetName()) || used.contains(b->GetName()) || lit.contains(a->GetName()) || lit.contains(b->GetName()))) {
                    ++shared;
                    if (example.empty()) {
                        example = a->GetName() + " / " + b->GetName();
                    }
                    ProductionSharedLights s;
                    s.a = a->GetName();
                    s.b = b->GetName();
                    const uint32_t lo = std::max(a->GetFirstChannel(), b->GetFirstChannel());
                    const uint32_t hi = std::min(a->GetLastChannel(), b->GetLastChannel());
                    const uint32_t per = std::max<uint32_t>(1, static_cast<uint32_t>(a->GetChanCountPerNode()));
                    s.lights = static_cast<int>((hi - lo + per) / per);
                    const bool aInB = a->GetFirstChannel() >= b->GetFirstChannel() && a->GetLastChannel() <= b->GetLastChannel();
                    const bool bInA = b->GetFirstChannel() >= a->GetFirstChannel() && b->GetLastChannel() <= a->GetLastChannel();
                    s.relation = aInB && bInA ? "same" : aInB ? "a-inside-b" : bInA ? "b-inside-a" : "partial";
                    s.bothLitMS = both(s.a, s.b);
                    s.complementary = s.bothLitMS == 0;
                    if (row.contains(s.a) && row.contains(s.b)) {
                        s.drawnLast = row[s.a] > row[s.b] ? s.a : s.b;
                    }
                    s.priority = chosen.contains({ s.a, s.b }) ? s.a : chosen.contains({ s.b, s.a }) ? s.b : std::string();
                    if (s.complementary) {
                        Issue("artistic", "figures", "shared-lights-turns", "{} and {} share {} lights and take turns: no conflict", { s.a, s.b, std::to_string(s.lights) });
                    } else if (!s.priority.empty()) {
                        Issue("warning", "figures", "shared-lights-priority", "{} and {} share {} lights; both play for {} s and {} wins, as a person chose", { s.a, s.b, std::to_string(s.lights),
                                                                                                                                                     fmt::format("{:.1f}", s.bothLitMS / 1000.0), s.priority });
                    } else {
                        Issue("warning", "figures", "shared-lights-conflict", "{} and {} share {} lights and both play for {} s: on those lights {} shows. Choose which one wins in step 4", { s.a, s.b,
                              std::to_string(s.lights), fmt::format("{:.1f}", s.bothLitMS / 1000.0), s.drawnLast.empty() ? s.a : s.drawnLast });
                    }
                    _r.sharedLights.push_back(std::move(s));
                }
            }
        }
        for (const Model* m : models) {
            if (used.contains(m->GetName()) && m->GetControllerName().empty() && !m->IsControllerConnectionValid() && m->GetControllerName() != "No Controller") {
                ++unwired;
            }
        }
        (void)example;
        Set("figures", shared > 0 ? "warning" : "ok", "{} figures used; {} pairs share lights", { std::to_string(used.size()), std::to_string(shared) });
        (void)unwired;
    }

    void Media() {
        std::set<std::string> seen;
        int missing = 0, outside = 0;
        for (const EffectSpec& e : _spec.effects) {
            for (const auto& [k, v] : e.settings) {
                if (!NamesAFile(k, v) || !seen.insert(v).second) {
                    continue;
                }
                ProductionMedia m;
                m.setting = k;
                m.target = e.target;
                m.startMS = e.startMS;
                m.path = v;
                std::string found = FileExists(v) ? v : FileUtils::FixFile(_in.showDir, v);
                if (!found.empty() && FileExists(found)) {
                    m.resolved = found;
                    m.travels = Under(found, _in.showDir) || InMediaFolder(found);
                }
                if (m.resolved.empty()) {
                    ++missing;
                    Issue("risk", "media", "media-missing", "{} at {} s on {} is not there: it would show black", { fs::path(v).filename().string(), fmt::format("{:.1f}", e.startMS / 1000.0), e.target });
                } else if (!m.travels) {
                    ++outside;
                    Issue("risk", "media", "media-outside", "{} is outside the show folder: it will not move with the show (the production folder copies it)", { fs::path(v).filename().string() });
                }
                _r.media.push_back(std::move(m));
            }
        }
        _r.status.mediaComplete = missing == 0;
        const std::string n = std::to_string(_r.media.size());
        if (missing > 0) {
            Set("media", "problem", "{} pictures and videos, {} not there", { n, std::to_string(missing) });
        } else if (outside > 0) {
            Set("media", "warning", "{} pictures and videos, {} outside the show folder", { n, std::to_string(outside) });
        } else {
            Set("media", "ok", "{} pictures and videos, all in the show", { n });
        }
        int videos = 0;
        for (const ProductionMedia& m : _r.media) {
            const std::string ext = Lower(fs::path(m.path).extension().string());
            videos += ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".mkv" || ext == ".m4v" || ext == ".webm" || ext == ".wmv" || ext == ".mpg" || ext == ".mpeg";
        }
        Set("video", "ok", "{} videos", { std::to_string(videos) });
        for (const ProductionMedia& m : _r.media) {
            const std::string ext = Lower(fs::path(m.path).extension().string());
            const bool video = ext == ".mp4" || ext == ".avi" || ext == ".mov" || ext == ".mkv" || ext == ".m4v" || ext == ".webm" || ext == ".wmv" || ext == ".mpg" || ext == ".mpeg";
            if (video && m.resolved.empty()) {
                Set("video", "problem", "a video is not there");
            } else if (video && !m.travels) {
                Set("video", "warning", "a video is outside the show folder");
            }
        }
    }

    void Effects() {
        const ValidationReport v = ValidateSequenceFile(_ctx, _in.sequencePath, nullptr, Strictness::FromFile, _in.cancel);
        _validationErrors = v.ErrorCount();
        int late = 0;
        for (const EffectSpec& e : _spec.effects) {
            if (_r.durationMS > 0 && e.endMS > _r.durationMS + 50) {
                ++late;
            }
        }
        for (const ValidationIssue& i : v.issues) {
            if (i.severity == IssueSeverity::Error && _r.issues.size() < 400) {
                Issue("risk", "effects", "validation-error", "{} ({} at {} s)", { DescribeIssue(i), i.model, fmt::format("{:.1f}", std::max(0, i.startMS) / 1000.0) });
            }
        }
        if (late > 0) {
            Issue("risk", "effects", "effects-after-end", "{} effects go on after the song ends", { std::to_string(late) });
        }
        if (v.ErrorCount() > 0 || late > 0) {
            Set("effects", "problem", "{} effects; {} errors, {} after the end", { std::to_string(_spec.effects.size()), std::to_string(v.ErrorCount()), std::to_string(late) });
        } else {
            Set("effects", v.WarningCount() > 0 ? "warning" : "ok", "{} effects, no error, {} notes", { std::to_string(_spec.effects.size()), std::to_string(v.WarningCount()) });
        }
    }

    void Direction() {
        int gestures = 0;
        std::set<std::string> palettes;
        for (const EffectSpec& e : _spec.effects) {
            gestures += e.effect == "Stage Field";
            palettes.insert(e.palette);
        }
        Set("gestures", "ok", "{} gesture effects; {} visual ideas", { std::to_string(gestures), std::to_string(_hasSidecar ? _sc.stage.motifs : 0) });
        const bool recipes = _hasSidecar && _sc.visualPlan.recipes;
        Set("recipes", "ok", recipes ? "the artistic director's recipes were used" : "made without recipes");
        Set("palettes", "ok", _hasSidecar && _sc.color.enabled ? "{} different palettes; colour direction on" : "{} different palettes; colour direction off", { std::to_string(palettes.size()) });
    }

    void Locks() {
        int locked = 0;
        if (_hasSidecar) {
            for (const ChoreographyCorrection& c : _sc.choreographyCorrections) {
                locked += c.action == ChoreographyCorrection::Action::Lock ? 1 : 0;
            }
        }
        Set("locks", "ok", "{} decisions locked by a person, kept as they are", { std::to_string(locked) });
    }

    void Review() {
        if (_hasSidecar) {
            _r.status.review = _sc.review.state.empty() ? "pending" : _sc.review.state;
            _r.status.reviewer = _sc.review.reviewer;
            _r.status.reviewedAt = _sc.review.approvedAt;
            if (_sc.review.state == "approved" && !_sc.review.specHash.empty() && _sc.review.specHash != _r.specHash) {
                _r.status.review = "pending";
                Issue("warning", "review", "review-other-version", "The approval was for another version of this show: it has to be seen again");
            }
        }
        if (_r.status.review != "approved") {
            Issue("warning", "review", "review-pending", "Nobody has approved this show yet: watch it in xLights before the lights go on");
        }
        Set("review", _r.status.review == "approved" ? "ok" : "warning", ProductionReviewPhrase(_r.status.review));
    }

    void Render() {
        const std::string fseq = (fs::path(_work) / (fs::path(_in.sequencePath).stem().string() + ".check.fseq")).string();
        if (!_ctx.RenderAndWait(_in.renderTimeoutMS) || !_ctx.WriteFseq(fseq)) {
            Issue("blocker", "render", "render-failed", "The native renderer could not render the sequence");
            Set("render", "blocked", "it does not render");
            return;
        }
        const ValidationReport v = ValidateRender(_ctx, fseq, _spec, Strictness::FromFile, _in.cancel);
        _r.status.rendered = v.ErrorCount() == 0 && !v.cancelled;
        std::string saved = _in.fseqPath;
        if (saved.empty()) {
            const std::string name = fs::path(_in.sequencePath).stem().string() + ".fseq";
            for (const fs::path& p : { fs::path(_in.sequencePath).replace_extension(".fseq"), fs::path(_ctx.GetFseqDirectory()) / name, fs::path(_in.showDir) / name }) {
                if (FileExists(p.string())) {
                    saved = p.string();
                    break;
                }
            }
        }
        _r.fseq = saved;
        if (!saved.empty() && FileExists(saved)) {
            std::string why;
            _r.status.renderMatches = SameFrames(saved, fseq, why);
            _r.status.fseqFinal = _r.status.renderMatches;
            if (!_r.status.renderMatches) {
                Issue("warning", "render", "render-differs", "The saved render (.fseq) is not the same as a new one: render it again in xLights before the show");
            }
        } else {
            Issue("warning", "render", "render-not-saved", "There is no saved render (.fseq) where xLights plays it from: make the final render in step 5, or save the sequence in xLights");
        }
        if (v.ErrorCount() > 0) {
            Issue("risk", "render", "render-errors", "The render has {} problems: some effects do not light as planned", { std::to_string(v.ErrorCount()) });
            Set("render", "problem", "{} problems in the render", { std::to_string(v.ErrorCount()) });
        } else {
            Set("render", "ok", _r.status.renderMatches ? "renders with the native renderer, the same as the saved one" : "renders with the native renderer");
        }
    }

    void RoundTrip() {
        const std::string again = (fs::path(_work) / (fs::path(_in.sequencePath).stem().string() + ".roundtrip.xsq")).string();
        std::string error;
        if (!ResaveSequence(_ctx, _spec, again, error)) {
            Issue("risk", "effects", "roundtrip-unsaved", "xLights could not save the sequence again: {}", { error });
            return;
        }
        HeadlessRenderContext reopened;
        if (!reopened.LoadShowFolder(_in.showDir) || !reopened.OpenSequence(again)) {
            Issue("risk", "effects", "roundtrip-unreadable", "The sequence saved again by xLights cannot be read back");
            return;
        }
        const SequenceSpec back = SpecFromSequence(reopened);
        _r.status.roundTrip = SpecHash(back) == _r.specHash;
        if (!_r.status.roundTrip) {
            Issue("risk", "effects", "roundtrip-changed", "Saved again by xLights, the sequence is not the same: open it in xLights and check it");
        }
        std::error_code ec;
        fs::remove(again, ec);
    }

    void Hashes() {
        for (const std::string& f : { _in.sequencePath, _r.sidecar, _r.fseq, _r.audio }) {
            if (!f.empty() && FileExists(f)) {
                _r.hashes[fs::path(f).filename().string()] = Sha256OfFile(f);
            }
        }
    }

    const ProductionInput& _in;
    ProductionReport _r;
    HeadlessRenderContext _ctx;
    GenerationSidecar _sc;
    bool _hasSidecar = false;
    SequenceSpec _spec;
    std::string _work;
    int _validationErrors = 0;
};

std::string Unique(const fs::path& wanted) {
    std::error_code ec;
    if (!fs::exists(wanted, ec)) {
        return wanted.string();
    }
    for (int n = 2; n < 1000; ++n) {
        const fs::path p = wanted.string() + "-" + std::to_string(n);
        if (!fs::exists(p, ec)) {
            return p.string();
        }
    }
    return (wanted.string() + "-" + Now());
}

bool CopyNew(const fs::path& from, const fs::path& to, std::string& error) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (fs::exists(to, ec)) {
        error = "exists: " + to.string();
        return false;
    }
    fs::copy_file(from, to, fs::copy_options::none, ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    return true;
}

} // namespace

const std::vector<std::string>& ProductionAreas() {
    static const std::vector<std::string> AREAS = { "audio", "lyrics", "structure", "layout", "figures", "controllers", "media", "video", "gestures", "recipes", "palettes", "effects", "locks", "review", "render" };
    return AREAS;
}

int ProductionReport::Count(const std::string& level) const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(), [&](const ProductionIssue& i) { return i.level == level; }));
}

const ProductionCheckItem* ProductionReport::Check(const std::string& area) const {
    for (const ProductionCheckItem& c : checks) {
        if (c.area == area) {
            return &c;
        }
    }
    return nullptr;
}

ProductionReport CheckProduction(const ProductionInput& input) {
    Checker c(input);
    return c.Run();
}

std::string FillProductionText(const std::string& text, const std::vector<std::string>& args, const ProductionTranslate& translate) {
    std::string t = translate ? translate(text.c_str()) : text;
    std::string out;
    size_t at = 0, n = 0;
    while (true) {
        const size_t p = t.find("{}", at);
        if (p == std::string::npos) {
            out += t.substr(at);
            break;
        }
        out += t.substr(at, p - at);
        out += n < args.size() ? args[n] : std::string();
        ++n;
        at = p + 2;
    }
    return out;
}

std::string ProductionLevelPhrase(const std::string& level) {
    if (level == "blocker") return "Stops the show";
    if (level == "risk") return "May make it wrong";
    if (level == "warning") return "To look at";
    return "Artistic suggestion";
}

std::string ProductionAreaPhrase(const std::string& a) {
    if (a == "audio") return "Song";
    if (a == "lyrics") return "Lyrics";
    if (a == "structure") return "Parts of the song";
    if (a == "layout") return "Layout";
    if (a == "figures") return "Figures";
    if (a == "controllers") return "Controllers";
    if (a == "media") return "Pictures and videos";
    if (a == "video") return "Videos";
    if (a == "gestures") return "Gestures";
    if (a == "recipes") return "Recipes";
    if (a == "palettes") return "Colours";
    if (a == "effects") return "Effects";
    if (a == "locks") return "Locks";
    if (a == "review") return "Review";
    if (a == "render") return "Render";
    return a;
}

std::string ProductionReviewPhrase(const std::string& r) {
    if (r == "approved") return "approved by a person";
    if (r == "in-progress") return "being reviewed";
    return "nobody has reviewed it yet";
}

std::string ProductionText(const ProductionReport& r, const ProductionTranslate& translate) {
    const auto T = [&](const std::string& s) { return translate ? translate(s.c_str()) : s; };
    std::ostringstream o;
    o << T("Show check") << ": " << fs::path(r.sequence).filename().string() << "\n";
    o << "xLights " << r.xlightsVersion << "\n\n";
    const auto yes = [&](bool b) { return b ? T("yes") : T("no"); };
    o << T("Made by the composer") << ": " << yes(r.status.generated) << "\n";
    o << T("Technical generation correct") << ": " << yes(r.status.technical) << "\n";
    o << T("Render correct") << ": " << yes(r.status.rendered) << "\n";
    o << T("Pictures and videos complete") << ": " << yes(r.status.mediaComplete) << "\n";
    o << T("Saved again by xLights and the same") << ": " << yes(r.status.roundTrip) << "\n";
    o << T("Final render where xLights plays it, checked") << ": " << yes(r.status.fseqFinal) << "\n";
    o << T("Production folder prepared") << ": " << yes(r.status.packaged) << "\n";
    o << T("Visual review") << ": " << T(ProductionReviewPhrase(r.status.review));
    if (r.status.review == "approved" && !r.status.reviewer.empty()) {
        o << " (" << r.status.reviewer << (r.status.reviewedAt.empty() ? "" : ", " + r.status.reviewedAt) << ")";
    }
    o << "\n";
    o << T("Ready for the real installation") << ": " << (r.ReadyForInstallation() ? T("yes") : T("no: only once a person has approved it")) << "\n\n";
    for (const ProductionCheckItem& c : r.checks) {
        o << "- " << T(ProductionAreaPhrase(c.area)) << ": " << FillProductionText(c.summary, c.args, translate) << "\n";
    }
    for (const char* level : { "blocker", "risk", "warning", "artistic" }) {
        bool first = true;
        for (const ProductionIssue& i : r.issues) {
            if (i.level != level) {
                continue;
            }
            if (first) {
                o << "\n" << T(ProductionLevelPhrase(level)) << ":\n";
                first = false;
            }
            o << "  * " << FillProductionText(i.text, i.args, translate) << "\n";
        }
    }
    if (r.cancelled) {
        o << "\n" << T("The check was stopped before the end.") << "\n";
    }
    return o.str();
}

std::string ToJson(const ProductionReport& r) {
    json checks = json::array();
    for (const ProductionCheckItem& c : r.checks) {
        checks.push_back({ { "area", c.area }, { "status", c.status }, { "summary", FillProductionText(c.summary, c.args) }, { "template", c.summary }, { "args", c.args } });
    }
    json issues = json::array();
    for (const ProductionIssue& i : r.issues) {
        issues.push_back({ { "level", i.level }, { "area", i.area }, { "code", i.code }, { "text", FillProductionText(i.text, i.args) }, { "template", i.text }, { "args", i.args } });
    }
    json media = json::array();
    for (const ProductionMedia& m : r.media) {
        media.push_back({ { "setting", m.setting }, { "target", m.target }, { "startMS", m.startMS }, { "path", m.path }, { "resolved", m.resolved }, { "travels", m.travels } });
    }
    json shared = json::array();
    for (const ProductionSharedLights& s : r.sharedLights) {
        shared.push_back({ { "a", s.a }, { "b", s.b }, { "lights", s.lights }, { "relation", s.relation }, { "bothLitMS", s.bothLitMS }, { "drawnLast", s.drawnLast },
                           { "priority", s.priority }, { "complementary", s.complementary } });
    }
    return json({ { "kind", "composer-production-check" },
                  { "schemaVersion", r.schemaVersion },
                  { "ran", r.ran },
                  { "cancelled", r.cancelled },
                  { "sequence", r.sequence },
                  { "sidecar", r.sidecar },
                  { "fseq", r.fseq },
                  { "audio", r.audio },
                  { "durationMS", r.durationMS },
                  { "xlightsVersion", r.xlightsVersion },
                  { "layoutFingerprint", r.layoutFingerprint },
                  { "specHash", r.specHash },
                  { "models", r.models },
                  { "controllers", r.controllers },
                  { "controllersActive", r.controllersActive },
                  { "checks", checks },
                  { "issues", issues },
                  { "media", media },
                  { "sharedLights", shared },
                  { "channels", r.channels },
                  { "readyForInstallation", r.ReadyForInstallation() },
                  { "hashes", r.hashes },
                  { "counts", { { "blocker", r.Count("blocker") }, { "risk", r.Count("risk") }, { "warning", r.Count("warning") }, { "artistic", r.Count("artistic") } } },
                  { "status",
                    { { "generated", r.status.generated },
                      { "fseqFinal", r.status.fseqFinal },
                      { "packaged", r.status.packaged },
                      { "technical", r.status.technical },
                      { "rendered", r.status.rendered },
                      { "renderMatches", r.status.renderMatches },
                      { "mediaComplete", r.status.mediaComplete },
                      { "roundTrip", r.status.roundTrip },
                      { "review", r.status.review },
                      { "reviewer", r.status.reviewer },
                      { "reviewedAt", r.status.reviewedAt } } } })
        .dump(1);
}

bool FromJson(const std::string& text, ProductionReport& out, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (!root.is_object() || root.value("kind", std::string()) != "composer-production-check") {
        error = "not a show check";
        return false;
    }
    if (root.value("schemaVersion", 0) > PRODUCTION_SCHEMA_VERSION) {
        error = "show check: a newer schema";
        return false;
    }
    try {
        ProductionReport r;
        r.ran = root.value("ran", false);
        r.cancelled = root.value("cancelled", false);
        r.sequence = root.value("sequence", std::string());
        r.sidecar = root.value("sidecar", std::string());
        r.fseq = root.value("fseq", std::string());
        r.audio = root.value("audio", std::string());
        r.durationMS = root.value("durationMS", 0);
        r.xlightsVersion = root.value("xlightsVersion", std::string());
        r.layoutFingerprint = root.value("layoutFingerprint", std::string());
        r.specHash = root.value("specHash", std::string());
        r.models = root.value("models", 0);
        r.controllers = root.value("controllers", 0);
        r.controllersActive = root.value("controllersActive", 0);
        for (const json& c : root.value("checks", json::array())) {
            r.checks.push_back({ c.value("area", std::string()), c.value("status", std::string()), c.value("template", std::string()), c.value("args", std::vector<std::string>()) });
        }
        for (const json& i : root.value("issues", json::array())) {
            r.issues.push_back({ i.value("level", std::string()), i.value("area", std::string()), i.value("code", std::string()), i.value("template", std::string()), i.value("args", std::vector<std::string>()) });
        }
        for (const json& m : root.value("media", json::array())) {
            r.media.push_back({ m.value("setting", std::string()), m.value("target", std::string()), m.value("startMS", 0), m.value("path", std::string()), m.value("resolved", std::string()),
                                m.value("travels", false) });
        }
        r.hashes = root.value("hashes", std::map<std::string, std::string>());
        const json st = root.value("status", json::object());
        r.status.generated = st.value("generated", false);
        r.status.fseqFinal = st.value("fseqFinal", false);
        r.status.packaged = st.value("packaged", false);
        r.channels = root.value("channels", 0);
        for (const json& s : root.value("sharedLights", json::array())) {
            r.sharedLights.push_back({ s.value("a", std::string()), s.value("b", std::string()), s.value("lights", 0), s.value("relation", std::string()), s.value("bothLitMS", 0),
                                       s.value("drawnLast", std::string()), s.value("priority", std::string()), s.value("complementary", false) });
        }
        r.status.technical = st.value("technical", false);
        r.status.rendered = st.value("rendered", false);
        r.status.renderMatches = st.value("renderMatches", false);
        r.status.mediaComplete = st.value("mediaComplete", false);
        r.status.roundTrip = st.value("roundTrip", false);
        r.status.review = st.value("review", std::string("pending"));
        r.status.reviewer = st.value("reviewer", std::string());
        r.status.reviewedAt = st.value("reviewedAt", std::string());
        out = std::move(r);
    } catch (const json::exception& e) {
        error = std::string("show check: ") + e.what();
        return false;
    }
    return true;
}

ProductionPackage BuildProductionPackage(const ProductionPackageInput& in) {
    ProductionPackage p;
    if (in.sequencePath.empty() || !FileExists(in.sequencePath)) {
        p.problems.push_back("the sequence is not there");
        return p;
    }
    const fs::path seq(in.sequencePath);
    const fs::path wanted = in.destination.empty() ? fs::path(in.showDir) / "ShowComposer" / "produccion" / seq.stem() : fs::path(in.destination);
    p.folder = Unique(wanted); // never over an earlier package
    std::error_code ec;
    fs::create_directories(p.folder, ec);
    if (ec) {
        p.problems.push_back("the folder cannot be made: " + ec.message());
        return p;
    }
    const fs::path dest(p.folder);
    std::string error;
    const auto copy = [&](const fs::path& from, const fs::path& rel) {
        if (from.empty() || !FileExists(from.string())) {
            return;
        }
        if (CopyNew(from, dest / rel, error)) {
            p.copied.push_back(rel.generic_string());
        } else {
            p.problems.push_back(rel.generic_string() + ": " + error);
        }
    };
    copy(seq, seq.filename());
    const std::string sidecarPath = SidecarPathFor(in.sequencePath);
    copy(sidecarPath, fs::path(sidecarPath).filename());
    GenerationSidecar sc;
    std::string err;
    const bool hasSidecar = ReadSidecar(sidecarPath, sc, err) == SidecarStatus::Ok;
    if (in.report != nullptr) {
        if (!in.report->fseq.empty()) {
            copy(in.report->fseq, fs::path(in.report->fseq).filename());
        }
        // Pictures and videos: only those that would not be where the show is.
        for (const ProductionMedia& m : in.report->media) {
            if (m.resolved.empty()) {
                continue;
            }
            if (m.travels) {
                p.referred.push_back(m.resolved);
            } else {
                copy(m.resolved, fs::path("media") / fs::path(m.resolved).filename());
            }
        }
        if (!in.report->audio.empty()) {
            if (in.copyAudio) {
                copy(in.report->audio, fs::path("audio") / fs::path(in.report->audio).filename());
            } else {
                p.referred.push_back(in.report->audio);
            }
        }
        std::ofstream(dest / "comprobacion.json") << ToJson(*in.report);
        std::ofstream(dest / "comprobacion.txt") << ProductionText(*in.report);
        p.copied.push_back("comprobacion.json");
        p.copied.push_back("comprobacion.txt");
    }
    // The lyrics as the person gave them, kept with the show (local, never published).
    if (hasSidecar && !sc.lyrics.lyrics.empty()) {
        std::ofstream(dest / "letra.txt") << sc.lyrics.lyrics;
        p.copied.push_back("letra.txt");
    }
    // What the show was made for, without the layout itself, and every file's hash.
    json info = { { "kind", "composer-production-package" },
                  { "schemaVersion", PRODUCTION_SCHEMA_VERSION },
                  { "sequence", seq.filename().string() },
                  { "made", Now() },
                  { "xlightsVersion", xlights_version_string },
                  { "layoutFingerprint", hasSidecar ? sc.layoutFingerprint : std::string() },
                  { "specHash", hasSidecar ? sc.specHash : std::string() },
                  { "audio", { { "file", hasSidecar ? sc.audioFile : std::string() }, { "hash", hasSidecar ? sc.audioHash : std::string() }, { "bytes", hasSidecar ? sc.audioSize : 0L } } },
                  { "review", hasSidecar ? sc.review.state : std::string("pending") },
                  { "visualReview", hasSidecar ? sc.visualReview : std::string("not-reviewed") },
                  { "referred", p.referred } };
    if (in.report != nullptr) {
        info["models"] = in.report->models;
        info["controllers"] = in.report->controllers;
        info["controllersActive"] = in.report->controllersActive;
        info["status"] = { { "generated", in.report->status.generated }, { "technical", in.report->status.technical }, { "rendered", in.report->status.rendered },
                           { "mediaComplete", in.report->status.mediaComplete }, { "roundTrip", in.report->status.roundTrip }, { "fseqFinal", in.report->status.fseqFinal },
                           { "packaged", true }, { "review", in.report->status.review }, { "readyForInstallation", in.report->ReadyForInstallation() } };
        // What the show was made for, without the layout itself.
        json pairs = json::array();
        for (const ProductionSharedLights& s : in.report->sharedLights) {
            pairs.push_back({ { "a", s.a }, { "b", s.b }, { "lights", s.lights }, { "relation", s.relation }, { "priority", s.priority }, { "complementary", s.complementary } });
        }
        info["layout"] = { { "fingerprint", in.report->layoutFingerprint }, { "models", in.report->models }, { "channels", in.report->channels },
                           { "controllers", in.report->controllers }, { "controllersActive", in.report->controllersActive }, { "sharedLights", pairs } };
        if (!in.report->fseq.empty() && in.report->status.fseqFinal) {
            info["fseq"] = { { "file", fs::path(in.report->fseq).filename().string() }, { "checkedAgainstSequence", true } };
        }
    }
    json hashes = json::object();
    for (const std::string& rel : p.copied) {
        hashes[rel] = Sha256OfFile((dest / rel).string());
    }
    info["hashes"] = hashes;
    std::ofstream(dest / "paquete.json") << info.dump(1);
    p.copied.push_back("paquete.json");
    p.ok = p.problems.empty();
    return p;
}

bool SameFrames(const std::string& a, const std::string& b, std::string& why) {
    std::unique_ptr<FSEQFile> fa(FSEQFile::openFSEQFile(a));
    std::unique_ptr<FSEQFile> fb(FSEQFile::openFSEQFile(b));
    if (fa == nullptr || fb == nullptr) {
        why = "unreadable";
        return false;
    }
    if (fa->getNumFrames() != fb->getNumFrames() || fa->getStepTime() != fb->getStepTime() || fa->getMaxChannel() != fb->getMaxChannel()) {
        why = "header";
        return false;
    }
    const uint32_t channels = static_cast<uint32_t>(fa->getMaxChannel());
    fa->prepareRead({ { 0, channels } });
    fb->prepareRead({ { 0, channels } });
    std::vector<uint8_t> da(channels), db(channels);
    for (uint32_t f = 0; f < fa->getNumFrames(); ++f) {
        std::fill(da.begin(), da.end(), 0);
        std::fill(db.begin(), db.end(), 0);
        std::unique_ptr<FSEQFile::FrameData> x(fa->getFrame(f));
        std::unique_ptr<FSEQFile::FrameData> y(fb->getFrame(f));
        if (x == nullptr || y == nullptr || !x->readFrame(da.data(), channels) || !y->readFrame(db.data(), channels) || da != db) {
            why = fmt::format("frame {}", f);
            return false;
        }
    }
    return true;
}

FinalRender RenderFinal(const FinalRenderInput& in) {
    FinalRender out;
    const auto step = [&](const std::string& what) {
        if (in.progress) {
            in.progress(what);
        }
    };
    const auto cancelled = [&]() { return in.cancel && in.cancel(); };
    if (in.sequencePath.empty() || !FileExists(in.sequencePath)) {
        out.problems.push_back("the sequence is not there");
        return out;
    }
    HeadlessRenderContext ctx;
    step("the show and its layout");
    if (!ctx.LoadShowFolder(in.showDir) || !ctx.OpenSequence(in.sequencePath)) {
        out.problems.push_back("xLights cannot open the sequence");
        return out;
    }
    out.specHash = SpecHash(SpecFromSequence(ctx));
    const SequenceData& data = ctx.GetSeqData();
    out.layoutChannels = static_cast<int>(data.NumChannels());
    if (cancelled()) {
        out.problems.push_back("stopped");
        return out;
    }
    step("the render");
    if (!ctx.RenderAndWait(in.renderTimeoutMS)) {
        out.problems.push_back("the native renderer could not render it");
        return out;
    }
    const fs::path seq(in.sequencePath);
    const fs::path folder = in.destination.empty() ? fs::path(ctx.GetFseqDirectory().empty() ? in.showDir : ctx.GetFseqDirectory()) : fs::path(in.destination);
    const fs::path target = folder / (seq.stem().string() + ".fseq");
    const fs::path temporary = folder / (seq.stem().string() + ".composer-new.fseq");
    std::error_code ec;
    fs::create_directories(folder, ec);
    step("writing the render");
    if (!ctx.WriteFseq(temporary.string())) {
        out.problems.push_back("the render could not be written");
        return out;
    }
    // Read back and checked before it takes the place of anything.
    step("checking the render against the sequence");
    std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(temporary.string()));
    if (file == nullptr) {
        out.problems.push_back("the render written cannot be read back");
        fs::remove(temporary, ec);
        return out;
    }
    out.channels = static_cast<int>(file->getMaxChannel()); // its last channel: a render may leave out ranges no model uses
    out.frames = static_cast<int>(file->getNumFrames());
    out.frameMS = file->getStepTime();
    out.durationMS = static_cast<int>(file->getTotalTimeMS());
    // A render may leave out channels no model uses (as the desktop does); none it leaves out may hold light.
    out.channelsMatch = out.channels > 0 && out.channels <= out.layoutChannels;
    out.sequenceDurationMS = static_cast<int>(data.NumFrames() * data.FrameTime());
    out.timingMatches = out.frames == static_cast<int>(data.NumFrames()) && out.frameMS == static_cast<int>(data.FrameTime());
    out.media = fs::path(file->getMediaFilename()).filename().string();
    {
        std::ifstream x(in.sequencePath);
        std::stringstream ss;
        ss << x.rdbuf();
        std::smatch m;
        const std::string text = ss.str();
        if (std::regex_search(text, m, std::regex("<mediaFile>([^<]*)</mediaFile>"))) {
            std::string media = m[1].str();
            std::replace(media.begin(), media.end(), '\\', '/');
            out.sequenceMedia = fs::path(media).filename().string();
        }
    }
    out.mediaMatches = out.sequenceMedia.empty() || out.media == out.sequenceMedia;
    bool same = out.timingMatches;
    if (same) {
        const uint32_t channels = std::min<uint32_t>(static_cast<uint32_t>(file->getMaxChannel()), data.NumChannels());
        file->prepareRead({ { 0, channels } });
        std::vector<uint8_t> frame(channels);
        for (uint32_t f = 0; f < data.NumFrames() && same; ++f) {
            std::fill(frame.begin(), frame.end(), 0);
            std::unique_ptr<FSEQFile::FrameData> fd(file->getFrame(f));
            if (fd == nullptr || !fd->readFrame(frame.data(), channels) || std::memcmp(frame.data(), data[f][0], channels) != 0) {
                same = false;
            }
            const unsigned char* rest = data[f][0];
            for (uint32_t ch = channels; ch < data.NumChannels() && out.channelsMatch; ++ch) {
                out.channelsMatch = rest[ch] == 0;
            }
        }
    }
    out.framesMatch = same;
    file.reset();
    if (!out.channelsMatch) {
        out.problems.push_back("the render leaves out channels that hold light");
    }
    if (!out.timingMatches) {
        out.problems.push_back("the render's frames or frame time are not the sequence's");
    }
    if (!out.framesMatch) {
        out.problems.push_back("the render read back is not what was rendered");
    }
    if (!out.mediaMatches) {
        out.problems.push_back("the render names another song than the sequence");
    }
    if (!out.problems.empty()) {
        fs::remove(temporary, ec);
        return out;
    }
    // An earlier render of this sequence is kept aside, never written over.
    if (FileExists(target.string())) {
        const fs::path keep = fs::path(in.showDir) / "ShowComposer" / "renders-anteriores";
        fs::create_directories(keep, ec);
        std::string stamp = Now();
        std::replace(stamp.begin(), stamp.end(), ':', '-');
        fs::path aside = keep / (seq.stem().string() + "-" + stamp + ".fseq");
        for (int n = 2; fs::exists(aside, ec) && n < 1000; ++n) {
            aside = keep / (seq.stem().string() + "-" + stamp + "-" + std::to_string(n) + ".fseq");
        }
        fs::rename(target, aside, ec);
        if (ec) {
            fs::copy_file(target, aside, ec);
            if (ec) {
                out.problems.push_back("the earlier render could not be kept aside: nothing was changed");
                fs::remove(temporary, ec);
                return out;
            }
        }
        out.previous = aside.string();
    }
    fs::rename(temporary, target, ec);
    if (ec) {
        out.problems.push_back("the render could not be put in its place: " + ec.message());
        return out;
    }
    out.fseq = target.string();
    out.sha256 = Sha256OfFile(out.fseq);
    out.ok = true;
    return out;
}

std::string ToJson(const FinalRender& r) {
    return json({ { "kind", "composer-final-render" }, { "ok", r.ok }, { "fseq", r.fseq }, { "previous", r.previous }, { "sha256", r.sha256 }, { "channels", r.channels },
                  { "layoutChannels", r.layoutChannels }, { "frames", r.frames }, { "frameMS", r.frameMS }, { "durationMS", r.durationMS }, { "sequenceDurationMS", r.sequenceDurationMS },
                  { "media", r.media }, { "sequenceMedia", r.sequenceMedia }, { "specHash", r.specHash }, { "channelsMatch", r.channelsMatch }, { "timingMatches", r.timingMatches },
                  { "framesMatch", r.framesMatch }, { "mediaMatches", r.mediaMatches }, { "problems", r.problems } })
        .dump(1);
}

int RunFinalRenderCommand(const std::string& showDir, const std::string& sequencePath, const std::string& outDir) {
    ObtainAccessToURL(showDir, true);
    ObtainAccessToURL(outDir, true);
    std::error_code ec;
    fs::create_directories(outDir, ec);
    const auto say = [](const std::string& line) {
        printf("composer-final-render: %s\n", line.c_str());
        fflush(stdout);
    };
    FinalRenderInput in;
    in.showDir = showDir;
    in.sequencePath = sequencePath;
    in.progress = say;
    const FinalRender r = RenderFinal(in);
    std::ofstream(fs::path(outDir) / "final-render.json") << ToJson(r);
    say(fmt::format("{}: {} frames of {} ms, {} channels (layout {}), frames {}, song {}{}", r.ok ? "written" : "NOT WRITTEN", r.frames, r.frameMS, r.channels, r.layoutChannels,
                    r.framesMatch ? "the same as rendered" : "NOT the same", r.mediaMatches ? "the sequence's" : "NOT the sequence's", r.previous.empty() ? "" : "; the earlier render kept aside"));
    for (const std::string& p : r.problems) {
        say("    " + p);
    }
    return r.ok ? 0 : 1;
}

int RunProductionCommand(const std::string& showDir, const std::string& sequencePath, const std::string& outDir, const std::string& packageDir, bool render) {
    ObtainAccessToURL(showDir, false);
    ObtainAccessToURL(outDir, true);
    std::error_code ec;
    fs::create_directories(outDir, ec);
    ProductionInput in;
    in.showDir = showDir;
    in.sequencePath = sequencePath;
    in.render = render;
    in.workDir = (fs::path(outDir) / "work").string();
    // The same stdio and flush as the other commands: a GUI build may otherwise lose what it says.
    const auto say = [](const std::string& line) {
        printf("composer-production: %s\n", line.c_str());
        fflush(stdout);
    };
    in.progress = say;
    const ProductionReport r = CheckProduction(in);
    std::ofstream(fs::path(outDir) / "production-check.json") << ToJson(r);
    std::ofstream(fs::path(outDir) / "production-check.txt") << ProductionText(r);
    say(fmt::format("{} blockers, {} risks, {} warnings, {} artistic; technical {}, rendered {}, render matches {}, media complete {}, round trip {}, review {}", r.Count("blocker"),
                    r.Count("risk"), r.Count("warning"), r.Count("artistic"), r.status.technical, r.status.rendered, r.status.renderMatches, r.status.mediaComplete, r.status.roundTrip,
                    r.status.review));
    if (!packageDir.empty()) {
        ProductionPackageInput pi;
        pi.showDir = showDir;
        pi.sequencePath = sequencePath;
        pi.destination = packageDir;
        pi.report = &r;
        const ProductionPackage p = BuildProductionPackage(pi);
        say(fmt::format("package {} in {}: {} files copied, {} referred, {} problems; final render included {}", p.ok ? "made" : "INCOMPLETE", p.folder, p.copied.size(), p.referred.size(),
                        p.problems.size(), r.status.fseqFinal ? "yes" : "no"));
        for (const std::string& x : p.problems) {
            say("    " + x);
        }
    }
    return r.CanGoOut() ? 0 : 1;
}

} // namespace composer
