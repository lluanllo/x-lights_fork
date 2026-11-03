#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "composer/PipelineTest.h"

#include "composer/ComposerReview.h"
#include "composer/GenerationSidecar.h"
#include "composer/PlanTest.h"
#include "composer/SequenceWriter.h"
#include "composer/ShowGenerationPipeline.h"
#include "composer/SyntheticAudio.h"
#include "composer/WriterTest.h"
#include "models/Model.h"
#include "models/Node.h"
#include "render/FSEQFile.h"
#include "render/HeadlessRenderContext.h"
#include "utils/Color.h"
#include "utils/xlImage.h"
#include "utils/ExternalHooks.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>

namespace fs = std::filesystem;

namespace composer {

namespace {

bool WriteText(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    out.close();
    return !out.fail();
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

bool LevelFrom(const std::string& id, AutomationLevel& level) {
    for (AutomationLevel l : { AutomationLevel::Conservative, AutomationLevel::Balanced, AutomationLevel::Creative }) {
        if (id == LevelId(l)) {
            level = l;
            return true;
        }
    }
    return false;
}

void PrintRun(const char* tool, const GenerationRun& run) {
    for (int i = 0; i < GENERATION_STAGE_COUNT; ++i) {
        const StageOutcome& s = run.stages[i];
        if (s.status == StageStatus::NotRun) {
            continue;
        }
        printf("%s: %-13s %-9s %6lld ms\n", tool, StageId(static_cast<GenerationStage>(i)), StageStatusId(s.status), s.ms);
        for (const std::string& n : s.notes) {
            printf("%s:     %s\n", tool, n.c_str());
        }
    }
    for (const std::string& p : run.problems) {
        printf("%s: PROBLEM %s\n", tool, p.c_str());
    }
}

// A minute of music written in code: a quiet intro, a verse, a loud chorus, a
// silence, the verse again and a quiet end, at 120 BPM.
bool WriteSyntheticSong(const fs::path& path, double chorusLevel = 0.7) {
    testaudio::SyntheticSong song(60);
    const auto music = [&](double from, double to, double kick, double click, double tone) {
        for (double t = from; t < to - 0.01; t += 0.5) {
            song.Kick(t, kick);
            if (click > 0) {
                song.Click(t + 0.25, click);
            }
        }
        for (double t = from; t < to - 0.01; t += 2.0) {
            song.Tone(t, std::min(2.0, to - t), 220, tone);
            song.Tone(t, std::min(2.0, to - t), 330, tone * 0.6);
        }
    };
    for (double t = 0; t < 8; t += 1.0) {
        song.Kick(t, 0.15);
    }
    song.Tone(0, 8, 196, 0.08, 0.5);
    music(8, 24, 0.4, 0.15, 0.12);
    music(24, 40, chorusLevel, chorusLevel / 2, chorusLevel / 3);
    music(43, 56, 0.4, 0.15, 0.12);
    song.Tone(56, 4, 196, 0.06, 0.3);
    return song.Save(path.string());
}

constexpr const char* LRC_LYRICS = "[00:09.00]¿Dónde está el niño?\n"
                                   "[00:13.00]Pingüino en la nieve\n"
                                   "[00:17.00]Cigüeña sobre el tejado\n"
                                   "[00:21.00]\n"
                                   "[00:25.00]¡Año nuevo, señor!\n"
                                   "[00:30.00]Luces de mañana\n"
                                   "[00:35.00]\n"
                                   "[00:44.00]Vuelve la canción\n"
                                   "[00:50.00]\n";

constexpr const char* PLAIN_LYRICS = "[Estrofa]\n¿Dónde está el niño?\nPingüino en la nieve\nCigüeña sobre el tejado\n\n[Estribillo]\n¡Año nuevo, señor!\nLuces de mañana\n\n[Estrofa]\nVuelve la canción\n";

class Suite {
public:
    Suite(const fs::path& root, const PipelineTestOptions& options) :
        _root(root),
        _options(options) {}

    int Run() {
        if (!Setup()) {
            return 1;
        }
        TestComplete();
        TestLyrics();
        TestFacesAndExclusion();
        TestCorrections();
        TestFailures();
        TestCache();
        TestCancel();
        TestSidecar();
        TestDeterminism();
        TestReviewData();
        TestMusicCorrections();
        TestRealLayout();
        Check(Sha256OfFile((_show / "xlights_rgbeffects.xml").string()) == _layoutHash, "the fixture layout was never modified");
        printf("composer-pipeline-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
        printf("composer-pipeline-test: technically valid and rendered; nothing here was reviewed by a person\n");
        return _failed == 0 ? 0 : 1;
    }

private:
    void Check(bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        fflush(stdout);
        ok ? ++_passed : ++_failed;
    }

    void Title(const char* title) {
        printf("=== %s\n", title);
        fflush(stdout);
    }

    bool Setup() {
        _show = _root / "show";
        if (!WritePlanFixture(_show.string())) {
            printf("composer-pipeline-test: cannot write the fixture show\n");
            return false;
        }
        _layoutHash = Sha256OfFile((_show / "xlights_rgbeffects.xml").string());
        _audio = _root / "song.wav";
        _lrc = _root / "lyrics.lrc";
        _plain = _root / "lyrics.txt";
        if (!WriteSyntheticSong(_audio) || !WriteText(_lrc, LRC_LYRICS) || !WriteText(_plain, PLAIN_LYRICS)) {
            printf("composer-pipeline-test: cannot write the synthetic song or lyrics\n");
            return false;
        }
        return true;
    }

    GenerationRequest Request(const std::string& name) {
        GenerationRequest r;
        r.audioPath = _audio.string();
        r.showDir = _show.string();
        r.sequenceName = name + ".xsq";
        r.reportDir = (_root / "reports" / name).string();
        r.level = AutomationLevel::Balanced;
        r.seed = 7;
        return r;
    }

    GenerationRun Generate(const GenerationRequest& r, GenerationStage last = GenerationStage::Publish) {
        GenerationRun run = GenerateShow(r, last);
        if (!run.ok && !run.cancelled && last == GenerationStage::Publish && _expectSuccess) {
            PrintRun("    ", run);
        }
        return run;
    }

    // The published file, read back the way the desktop opens it.
    SequenceSpec Reopen(const GenerationRun& run) {
        HeadlessRenderContext ctx;
        if (!ctx.LoadShowFolder(_show.string()) || !ctx.OpenSequence(run.sequencePath)) {
            return {};
        }
        SequenceSpec spec = SpecFromSequence(ctx);
        (void)ctx.CloseSequence();
        return spec;
    }

    static int Count(const SequenceSpec& spec, const std::string& effect) {
        return static_cast<int>(std::count_if(spec.effects.begin(), spec.effects.end(), [&](const EffectSpec& e) { return e.effect == effect; }));
    }

    static const TimingTrackSpec* Track(const SequenceSpec& spec, const std::string& name) {
        for (const TimingTrackSpec& t : spec.timingTracks) {
            if (t.name == name) {
                return &t;
            }
        }
        return nullptr;
    }

    static std::string Setting(const EffectSpec& e, const std::string& key) {
        for (const auto& [k, v] : e.settings) {
            if (k == key) {
                return v;
            }
        }
        return {};
    }

    static bool HasNote(const GenerationRun& run, GenerationStage stage, const std::string& code) {
        const auto& notes = run.Stage(stage).notes;
        return std::any_of(notes.begin(), notes.end(), [&](const std::string& n) { return n.rfind(code, 0) == 0; });
    }

    // ------------------------------------------------------------ A. complete

    void TestComplete() {
        Title("A1. The whole pipeline on a synthetic song, without lyrics");
        const auto start = std::chrono::steady_clock::now();
        _base = Generate(Request("completo"));
        const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        Check(_base.ok, fmt::format("published in {} ms: {}", ms, _base.sequencePath));
        if (!_base.ok) {
            return;
        }
        Check(FileExists(_base.sequencePath) && FileExists(_base.sidecarPath) && FileExists(_base.fseqPath), "the sequence, its sidecar and the fseq exist");
        Check(!FileExists((_show / "ShowComposer" / "tmp").string()) || fs::is_empty(_show / "ShowComposer" / "tmp"), "no temporary left behind");
        const SequenceSpec read = Reopen(_base);
        Check(!_base.choreography.events.empty() && read.effects.size() == _base.choreography.events.size(),
              fmt::format("every event became an effect: {} events, {} effects in the reopened file", _base.choreography.events.size(), read.effects.size()));
        Check(read.timingTracks.size() == _base.spec.timingTracks.size() && Track(read, "Composer Beats") != nullptr && Track(read, "Composer Sections") != nullptr,
              fmt::format("{} timing tracks from the analysis, beats and sections among them", read.timingTracks.size()));
        Check(fs::path(read.mediaFile).filename() == _audio.filename(), "the sequence plays the song: " + read.mediaFile);
        Check(!_base.write.validation.Has(issue::SequenceMismatch), "every effect, setting, palette and mark read back is what was planned");
        Check(_base.render.ran && _base.render.frames == _base.render.expectedFrames && _base.render.stepMS == 25,
              fmt::format("rendered: {} frames of {} expected, {} ms step, {} channels for a layout of {}", _base.render.frames, _base.render.expectedFrames, _base.render.stepMS, _base.render.channels, _base.render.layoutChannels));
        int planned = 0;
        int silent = 0;
        int unexplained = 0;
        for (const TargetCoverage& t : _base.coverage) {
            planned += t.plannedSegments > 0 ? 1 : 0;
            silent += t.written > t.effectsLit ? 1 : 0;
            unexplained += t.plannedSegments > 0 && t.events == 0 && t.note.empty() ? 1 : 0;
        }
        Check(planned > 0 && silent == 0, fmt::format("{} targets in the plan, every written effect lit its target", planned));
        Check(unexplained == 0, "a figure the plan used and nothing lit always has its reason in the report");
        Check(_base.renderValidation.ErrorCount() == 0 && _base.write.validation.ErrorCount() == 0, fmt::format("no validation error ({} warnings)", _base.write.validation.WarningCount() + _base.renderValidation.WarningCount()));
        Check(_base.Stage(GenerationStage::Publish).status != StageStatus::Failed && HasNote(_base, GenerationStage::Publish, "round trip") &&
                  _base.Stage(GenerationStage::Publish).notes.back().find("identical") != std::string::npos,
              "reopened, saved again through SequenceFile and reopened: identical");
        Check(Count(read, "Faces") == 0 && Count(read, "Text") == 0 && Track(read, "Composer Lyrics") == nullptr, "without lyrics: no face, no text, no lyrics track");
        Check(FileExists((fs::path(_base.request.reportDir) / "generation-report.txt").string()) && FileExists((fs::path(_base.request.reportDir) / "choreography.json").string()) &&
                  FileExists((fs::path(_base.request.reportDir) / "analysis.json").string()),
              "the report folder has the analysis, the plan, the choreography and the report");
        bool silenceRespected = true;
        for (const MusicalEvent& s : _base.analysis.events) {
            if (s.type == MusicalEventType::Silence && s.durationMS >= 2000) {
                for (const EffectSpec& e : read.effects) {
                    silenceRespected = silenceRespected && !(e.startMS < s.timeMS + s.durationMS - 100 && e.endMS > s.timeMS + 100);
                }
            }
        }
        Check(silenceRespected, "nothing is lit through the silence of the song");
    }

    // ------------------------------------------------------------ lyrics

    void TestLyrics() {
        Title("A2-A4. Lyrics: unsynchronized, synchronized, an existing track and a manual mark");
        GenerationRequest r = Request("letra-sin-sincronizar");
        r.lyricsPath = _plain.string();
        GenerationRun plain = Generate(r);
        const SequenceSpec plainRead = Reopen(plain);
        Check(plain.ok && plain.hasLyrics && plain.lyricTrack.layers.empty(), "plain text: published, with the lyrics kept but no track");
        Check(Count(plainRead, "Faces") == 0 && Count(plainRead, "Text") == 0, "no face moves and no text shows without times");
        Check(HasNote(plain, GenerationStage::Lyrics, "lyrics-not-synchronized"), "the missing synchronization is said");
        GenerationSidecar sidecar;
        std::string error;
        Check(ReadSidecar(plain.sidecarPath, sidecar, error) == SidecarStatus::Ok && sidecar.lyrics.lyrics.find("¿Dónde está el niño?") != std::string::npos && sidecar.lyricsQuality == "unsynchronized",
              "the sidecar keeps the text as written, accents and all, to be synchronized later");

        r = Request("letra-sincronizada");
        r.lyricsPath = _lrc.string();
        _synced = Generate(r);
        const SequenceSpec read = Reopen(_synced);
        const TimingTrackSpec* track = Track(read, "Composer Lyrics");
        Check(_synced.ok && track != nullptr && track->layers.size() == 3, "LRC: published with a lyrics track of phrases, words and mouth shapes");
        std::set<std::string> phrases;
        if (track != nullptr && !track->layers.empty()) {
            for (const TimingMark& m : track->layers[0]) {
                phrases.insert(m.label);
            }
        }
        Check(phrases.count("¿Dónde está el niño?") && phrases.count("Pingüino en la nieve") && phrases.count("Cigüeña sobre el tejado") && phrases.count("¡Año nuevo, señor!"),
              "¿ ¡ á é í ñ ü survive the write, the save and the reopen, byte for byte");
        bool textInTime = Count(read, "Text") > 0;
        for (const EffectSpec& e : read.effects) {
            if (e.effect == "Text") {
                const std::string text = Setting(e, "E_TEXTCTRL_Text");
                textInTime = textInTime && std::any_of(_synced.alignment.phrases.begin(), _synced.alignment.phrases.end(), [&](const AlignedPhrase& p) { return p.text == text && e.startMS >= p.startMS - 25 && e.endMS <= p.endMS + 25; });
            }
        }
        Check(textInTime, fmt::format("{} text effects, each one a real phrase inside its own time", Count(read, "Text")));

        // A synchronized xLights track wins over the times of the LRC.
        const fs::path xtiming = _root / "letra.xtiming";
        TimingTrackSpec moved = _synced.lyricTrack;
        moved.name = "Letra Manual";
        for (auto& layer : moved.layers) {
            for (TimingMark& m : layer) {
                m.startMS += 500;
                m.endMS += 500;
            }
        }
        Check(WriteTimingTracks(xtiming.string(), { moved }, error), "a manual lyrics track exported as .xtiming");
        r = Request("letra-pista-manual");
        r.lyricsPath = _lrc.string();
        r.timingPath = xtiming.string();
        GenerationRun manual = Generate(r);
        Check(manual.ok && manual.alignment.provider == AlignmentProvider::ExistingTrack && !manual.alignment.phrases.empty() &&
                  manual.alignment.phrases.front().startMS == _synced.alignment.phrases.front().startMS + 500,
              "the manual track takes priority over any time computed or read from the LRC");

        // A mark a person made in the sync dialog wins over the LRC line.
        LyricSession session;
        session.trackName = "Composer Lyrics";
        session.lyrics = LRC_LYRICS;
        LyricCorrection mark;
        mark.targetId = _synced.lyrics.Lines().front()->id;
        mark.startMS = 9500;
        mark.endMS = 12500;
        session.corrections.push_back(mark);
        const fs::path sessionPath = _root / "session.json";
        Check(SaveLyricSession(sessionPath.string(), session, error), "a sync dialog session written");
        r = Request("letra-marca-manual");
        r.lyricSessionPath = sessionPath.string();
        GenerationRun marked = Generate(r);
        Check(marked.ok && !marked.alignment.phrases.empty() && marked.alignment.phrases.front().startMS == 9500 && marked.alignment.phrases.front().state != Provenance::Detected,
              "the person's mark is kept as given");
        GenerationSidecar markedSidecar;
        Check(ReadSidecar(marked.sidecarPath, markedSidecar, error) == SidecarStatus::Ok && markedSidecar.lyrics.corrections.size() == 1, "and stored in the sidecar for the next generation");
    }

    // ------------------------------------------------------------ faces

    void TestFacesAndExclusion() {
        Title("A5-A7. Faces only where there is one; an excluded model stays dark");
        if (!_synced.ok) {
            Check(false, "needs the synchronized run");
            return;
        }
        const SequenceSpec read = Reopen(_synced);
        int faces = 0;
        bool onFace = true;
        for (const EffectSpec& e : read.effects) {
            if (e.effect != "Faces") {
                continue;
            }
            ++faces;
            const TargetInfo* t = _synced.capabilities.Find(e.target);
            const std::string definition = Setting(e, "E_CHOICE_Faces_FaceDefinition");
            onFace = onFace && t != nullptr && std::find(t->faces.begin(), t->faces.end(), definition) != t->faces.end() && Setting(e, "E_CHOICE_Faces_TimingTrack") == "Composer Lyrics";
        }
        Check(faces > 0 && onFace, fmt::format("{} face effects, each on a figure that has that face definition, reading the lyrics track", faces));
        Check(!_synced.renderValidation.Has(issue::FaceMouthStatic) && !_synced.renderValidation.Has(issue::FaceTrackMissing), "the mouths moved in the render");
        bool noFaceElsewhere = true;
        for (const EffectSpec& e : read.effects) {
            const TargetInfo* t = _synced.capabilities.Find(e.target);
            noFaceElsewhere = noFaceElsewhere && (e.effect != "Faces" || (t != nullptr && !t->faces.empty()));
        }
        Check(noFaceElsewhere, "no figure without a face definition got a face effect");

        RoleOverrides roles;
        roles.models.push_back({ "Excluido", "", {}, true, "pipeline test" });
        const fs::path rolesPath = _root / "roles.json";
        (void)WriteText(rolesPath, ToJson(roles));
        GenerationRequest r = Request("excluido");
        r.rolesPath = rolesPath.string();
        GenerationRun run = Generate(r);
        const SequenceSpec excludedRead = Reopen(run);
        bool dark = run.ok;
        for (const EffectSpec& e : excludedRead.effects) {
            dark = dark && e.target != "Excluido" && !run.capabilities.LightsExcluded(e.target);
        }
        Check(dark, "the excluded model has no effect, and no group lighting it has one");
        bool noControllerKept = false;
        for (const TargetCoverage& t : run.coverage) {
            noControllerKept = noControllerKept || (t.target == "Sin Controlador" && t.plannedSegments > 0);
        }
        const TargetInfo* free = run.capabilities.Find("Sin Controlador");
        Check(free != nullptr && free->noController && !free->Excluded(), fmt::format("the model with no controller is not excluded{}", noControllerKept ? ", and takes part" : ""));
    }

    // ------------------------------------------------------------ corrections

    void TestCorrections() {
        Title("A8-A9. Locked decisions survive regenerating; an incompatible one is a visible conflict");
        if (!_synced.ok || _synced.choreography.events.empty()) {
            Check(false, "needs the synchronized run");
            return;
        }
        const ChoreographyEvent* chosen = nullptr;
        for (const ChoreographyEvent& e : _synced.choreography.events) {
            if (e.target == "Arbol Grande" && e.effect != "Faces") {
                chosen = &e;
                break;
            }
        }
        if (chosen == nullptr) {
            chosen = &_synced.choreography.events.front();
        }
        ChoreographyCorrection lock;
        lock.action = ChoreographyCorrection::Action::Lock;
        lock.target = chosen->target;
        lock.event = *chosen;
        GenerationRequest r = Request("bloqueo");
        r.lyricsPath = _lrc.string();
        r.corrections.choreography.push_back(lock);
        GenerationRun first = Generate(r);
        Check(first.ok, "generated with one event locked by hand");

        r = Request("bloqueo-regenerado");
        r.previousSidecarPath = first.sidecarPath;
        r.level = AutomationLevel::Creative;
        r.seed = 99;
        GenerationRun again = Generate(r);
        const SequenceSpec read = Reopen(again);
        const bool kept = std::any_of(read.effects.begin(), read.effects.end(), [&](const EffectSpec& e) {
            if (e.target != chosen->target || e.startMS != chosen->startMS || e.endMS != chosen->endMS || e.effect != chosen->effect) {
                return false;
            }
            for (const auto& [k, v] : chosen->settings) {
                if (Setting(e, k) != v) {
                    return false;
                }
            }
            return true;
        });
        Check(again.ok && kept, fmt::format("regenerated from the sidecar with another level and seed: the locked {} on {} at {}-{} ms is still there, settings and all", chosen->effect, chosen->target, chosen->startMS, chosen->endMS));
        Check(again.hasLyrics && again.alignment.quality != "unsynchronized" && Track(read, "Composer Lyrics") != nullptr, "the lyrics came back from the sidecar too");

        ChoreographyCorrection wrong;
        wrong.action = ChoreographyCorrection::Action::SetTemplate;
        wrong.target = "Linea 1";
        wrong.templateId = "tree-spirals";
        r = Request("plantilla-incompatible");
        r.corrections.choreography.push_back(wrong);
        GenerationRun conflict = Generate(r);
        const bool visible = std::any_of(conflict.choreography.conflicts.begin(), conflict.choreography.conflicts.end(), [](const ChoreographyConflict& c) { return c.reason.find("template-incompatible") != std::string::npos; });
        Check(conflict.ok && visible && HasNote(conflict, GenerationStage::Choreography, "choreography-conflict"), "spirals asked for a line: reported as a conflict in the run and its report, the rest published");
        const std::string report = ReadText(fs::path(conflict.request.reportDir) / "generation-report.txt");
        Check(report.find("template-incompatible:tree-spirals") != std::string::npos, "and the conflict is written in the generation report");
    }

    // ------------------------------------------------------------ failures

    void TestFailures() {
        Title("A10-A12. Failures publish nothing and destroy nothing");
        _expectSuccess = false;
        // Writing: the temporary folder cannot be created.
        const fs::path composerDir = _show / "ShowComposer";
        const fs::path tmp = composerDir / "tmp";
        std::error_code ec;
        fs::remove_all(tmp, ec);
        fs::create_directories(composerDir, ec);
        (void)WriteText(tmp, "not a folder");
        GenerationRun write = Generate(Request("error-escritura"));
        Check(!write.ok && write.Stage(GenerationStage::Write).status == StageStatus::Failed && !FileExists(write.sequencePath) && !FileExists(write.sidecarPath),
              "a write that cannot start: not published, no sidecar");
        Check(FileExists((fs::path(write.request.reportDir) / "generation-report.txt").string()), "the diagnostics stay in the report folder");
        fs::remove(tmp, ec);

        // Validation: a locked event whose setting the effect does not allow.
        ChoreographyEvent bad = _base.choreography.events.empty() ? ChoreographyEvent() : _base.choreography.events.front();
        bad.settings.emplace_back("E_SLIDER_Not_A_Setting", "5");
        ChoreographyCorrection lock;
        lock.action = ChoreographyCorrection::Action::Lock;
        lock.target = bad.target;
        lock.event = bad;
        GenerationRequest r = Request("error-validacion");
        r.corrections.choreography.push_back(lock);
        GenerationRun invalid = Generate(r);
        Check(!invalid.ok && invalid.Stage(GenerationStage::Choreography).status == StageStatus::Failed && invalid.Stage(GenerationStage::Write).status == StageStatus::NotRun && !FileExists(invalid.sequencePath),
              "a setting the effect does not have stops the generation before anything is written");

        // An existing output is never replaced.
        if (_base.ok) {
            const std::string before = Sha256OfFile(_base.sequencePath);
            GenerationRequest same = Request("completo");
            same.reportDir = (_root / "reports" / "completo-otra-vez").string();
            GenerationRun exists = Generate(same);
            Check(!exists.ok && exists.Stage(GenerationStage::Inputs).status == StageStatus::Failed && Sha256OfFile(_base.sequencePath) == before, "a sequence of the same name: refused, and the old one is untouched");
        }
        GenerationRequest notEmpty = Request("informe-ocupado");
        notEmpty.reportDir = _base.request.reportDir;
        Check(!Generate(notEmpty).ok, "a report folder with something in it is refused");
        GenerationRequest noAudio = Request("sin-audio");
        noAudio.audioPath = (_root / "no-existe.wav").string();
        Check(!Generate(noAudio).ok, "a missing song is refused");
        const fs::path broken = _root / "roto.wav";
        (void)WriteText(broken, "RIFF this is not audio");
        GenerationRequest notAudio = Request("audio-roto");
        notAudio.audioPath = broken.string();
        GenerationRun unreadable = Generate(notAudio);
        Check(!unreadable.ok && unreadable.Stage(GenerationStage::Analysis).status == StageStatus::Failed && !FileExists(unreadable.sequencePath), "a file AudioManager cannot decode stops at the analysis");
        _expectSuccess = true;
    }

    // ------------------------------------------------------------ cache

    void TestCache() {
        Title("A13-A14. The analysis cache: reused, and invalidated by the bytes and the options");
        if (!_base.ok) {
            return;
        }
        GenerationRun reused = Generate(Request("cache-valida"), GenerationStage::Analysis);
        Check(reused.analysisFromCache && ToJson(reused.analysis) == ToJson(_base.analysis), "the same song: the analysis comes from the cache, identical");

        GenerationRequest r = Request("cache-opciones");
        r.analysisOptions.onsetSensitivity = 2.0;
        GenerationRun otherOptions = Generate(r, GenerationStage::Analysis);
        Check(otherOptions.Stage(GenerationStage::Analysis).status != StageStatus::Failed && !otherOptions.analysisFromCache, "other analysis options: analyzed again");

        const fs::path copy = _root / "song-copy.wav";
        (void)WriteSyntheticSong(copy, 0.9);
        r = Request("cache-contenido");
        r.audioPath = copy.string();
        GenerationRun changed = Generate(r, GenerationStage::Analysis);
        Check(!changed.analysisFromCache && changed.analysis.audioHash != _base.analysis.audioHash && changed.analysisCachePath != _base.analysisCachePath, "other bytes: another entry, analyzed again");
        // The same name with other bytes: the name alone never identifies the song.
        (void)WriteSyntheticSong(copy, 0.5);
        r = Request("cache-mismo-nombre");
        r.audioPath = copy.string();
        GenerationRun renamed = Generate(r, GenerationStage::Analysis);
        Check(!renamed.analysisFromCache && renamed.analysis.audioHash != changed.analysis.audioHash, "the same file name with new content: not taken from the cache");
    }

    // ------------------------------------------------------------ cancel

    void TestCancel() {
        Title("A15. Cancelling");
        _expectSuccess = false;
        for (GenerationStage at : { GenerationStage::Analysis, GenerationStage::Choreography, GenerationStage::Write }) {
            bool stop = false;
            GenerationRequest r = Request(fmt::format("cancelado-{}", StageId(at)));
            r.analysisOptions.onsetSensitivity = 1.7; // not in the cache, so the analysis runs
            r.progress = [&](GenerationStage stage, const std::string& what) {
                if (stage == at && (at != GenerationStage::Write || what == "write")) {
                    stop = true;
                }
            };
            r.cancel = [&]() { return stop; };
            GenerationRun run = Generate(r);
            Check(run.cancelled && !run.ok && !FileExists(run.sequencePath) && !FileExists(run.sidecarPath) && (!fs::exists(_show / "ShowComposer" / "tmp") || fs::is_empty(_show / "ShowComposer" / "tmp")),
                  fmt::format("cancelled during {}: nothing published, nothing left in the temporary folder", StageId(at)));
        }
        _expectSuccess = true;
    }

    // ------------------------------------------------------------ sidecar

    void TestSidecar() {
        Title("The sidecar: versioned, and refused rather than misread");
        if (!_base.ok) {
            return;
        }
        GenerationSidecar s;
        std::string error;
        Check(ReadSidecar(_base.sidecarPath, s, error) == SidecarStatus::Ok && s.sequenceFile == "completo.xsq" && s.effects == static_cast<int>(_base.spec.effects.size()) && s.visualReview == "not-reviewed" &&
                  s.audioHash == _base.analysis.audioHash && !s.generationId.empty(),
              "it identifies the song, the layout, the versions and what was written");
        GenerationSidecar back;
        Check(FromJson(ToJson(s), back, error) == SidecarStatus::Ok && ToJson(back) == ToJson(s), "it reads back to the same bytes");
        const std::string xsq = ReadText(_base.sequencePath);
        Check(!xsq.empty() && xsq.find("generationId") == std::string::npos && xsq.find(".composer.json") == std::string::npos, "the sequence holds no Composer metadata of its own");
        const fs::path damaged = _root / "damaged.composer.json";
        (void)WriteText(damaged, "{ \"schemaVersion\": 1, \"generator\": ");
        Check(ReadSidecar(damaged.string(), back, error) == SidecarStatus::Damaged, "a damaged sidecar: damaged");
        nlohmann::json newer = nlohmann::json::parse(ToJson(s));
        newer["schemaVersion"] = SIDECAR_SCHEMA_VERSION + 1;
        (void)WriteText(damaged, newer.dump(1));
        Check(ReadSidecar(damaged.string(), back, error) == SidecarStatus::Newer, "a newer schema: not read");
        Check(ReadSidecarFor(_base.sidecarPath, std::string(64, '0'), back, error) == SidecarStatus::Incompatible, "made for another song: incompatible");
        _expectSuccess = false;
        GenerationRequest r = Request("sidecar-roto");
        r.previousSidecarPath = damaged.string();
        GenerationRun refused = Generate(r);
        Check(!refused.ok && HasNote(refused, GenerationStage::Inputs, "previous-sidecar-newer"), "regenerating from it is refused, with the reason");
        const fs::path other = _root / "song-copy.wav";
        r = Request("sidecar-otra-cancion");
        r.audioPath = other.string();
        r.previousSidecarPath = _base.sidecarPath;
        refused = Generate(r);
        Check(!refused.ok && HasNote(refused, GenerationStage::Inputs, "previous-sidecar-incompatible"), "and so is regenerating another song from it");
        _expectSuccess = true;
    }

    // ------------------------------------------------------------ determinism

    void TestDeterminism() {
        Title("Reproducibility");
        if (!_base.ok) {
            return;
        }
        GenerationRequest r = Request("repetido");
        GenerationRun again = Generate(r);
        Check(again.ok && SpecHash(again.spec) == SpecHash(_base.spec) && ToJson(again.choreography) == ToJson(_base.choreography) && again.sidecar.generationId == _base.sidecar.generationId,
              fmt::format("the same inputs: the same sequence ({}), choreography and generation id", SpecHash(_base.spec)));
        r = Request("otra-semilla");
        r.seed = 8;
        GenerationRun seed = Generate(r, GenerationStage::Spec);
        Check(SpecHash(seed.spec) != SpecHash(_base.spec), "another seed: another sequence");
        GenerationRun stop = Generate(Request("solo-plan"), GenerationStage::Plan);
        Check(stop.Stage(GenerationStage::Plan).status != StageStatus::Failed && stop.Stage(GenerationStage::Choreography).status == StageStatus::NotRun && !FileExists(stop.sequencePath) &&
                  FileExists((fs::path(stop.request.reportDir) / "composition-plan.json").string()),
              "a run that stops after the plan writes the plan and nothing in the show");
    }

    // ------------------------------------------------------------ what the window reads

    void TestReviewData() {
        Title("What the Show Composer window reads back");
        if (!_synced.ok) {
            Check(false, "needs the synchronized run");
            return;
        }
        ReportFolder report;
        std::string error;
        Check(LoadReportFolder(_synced.request.reportDir, report, error) && report.hasAnalysis && report.hasLyrics && report.hasPlan && report.hasChoreography && report.hasCapabilities && report.hasSummary,
              "the report folder reads back: analysis, lyrics, plan, choreography, capabilities and summary " + error);
        Check(ToJson(report.analysis) == ToJson(_synced.analysis) && ToJson(report.alignment) == ToJson(_synced.alignment) && ToJson(report.capabilities) == ToJson(_synced.capabilities) &&
                  ToJson(report.plan) == ToJson(_synced.plan) && ToJson(report.choreography) == ToJson(_synced.choreography),
              "each one is what the run had, byte for byte");
        const LevelSummary level = SummarizeLevel(report);
        Check(level.events == static_cast<int>(_synced.choreography.events.size()) && level.faces > 0 && level.texts > 0 && level.planned > 0 && level.accents == _synced.choreography.metrics.accents,
              fmt::format("level summary from the choreography itself: {} events, {} figures of {} planned, {} faces, {} showing text", level.events, level.figures, level.planned, level.faces, level.texts));
        const std::vector<FigureSummary> figures = SummarizeFigures(report, &_synced.library, _synced.corrections);
        bool facesRight = true;
        bool textRight = true;
        bool templates = false;
        bool excludedListed = false;
        for (const FigureSummary& f : figures) {
            facesRight = facesRight && (!f.receivesFace || f.hasFace);
            textRight = textRight && (!f.receivesText || f.canShowText);
            templates = templates || (f.name == "Arbol Grande" && std::any_of(f.templates.begin(), f.templates.end(), [](const auto& t) { return t.first == "tree-spirals"; }));
            excludedListed = excludedListed || f.excluded;
        }
        Check(!figures.empty() && facesRight && textRight, "a face only on a figure with a face, text only on one that can show it");
        Check(templates, "a figure lists the templates it can take");
        const std::vector<ChoreographyCorrection> locks = LockFigure(_synced.choreography, "Arbol Grande");
        const auto onTree = std::count_if(_synced.choreography.events.begin(), _synced.choreography.events.end(), [](const ChoreographyEvent& e) { return e.target == "Arbol Grande"; });
        CorrectionSet set;
        set.choreography = locks;
        Check(!locks.empty() && static_cast<long>(locks.size()) == onTree && RemoveCorrections(set, "Arbol Grande", true, false) == static_cast<int>(locks.size()) && set.choreography.empty(),
              fmt::format("locking a figure locks its {} events, and undoing it removes exactly those", locks.size()));
        (void)excludedListed;
    }

    void TestMusicCorrections() {
        Title("Beats and bars fixed by hand win, and come back from the sidecar");
        if (!_base.ok) {
            return;
        }
        int firstBeat = -1;
        int detectedDownbeats = 0;
        for (const MusicalEvent& e : _base.analysis.events) {
            if (e.type == MusicalEventType::Beat && firstBeat < 0 && e.timeMS >= 8000) {
                firstBeat = e.timeMS;
            }
            detectedDownbeats += e.type == MusicalEventType::Downbeat ? 1 : 0;
        }
        GenerationRequest r = Request("compas-a-mano");
        r.corrections.analysis.bar.firstDownbeatMS = firstBeat;
        r.corrections.analysis.bar.beatsPerBar = 4;
        EventCorrection added;
        added.action = EventCorrection::Action::Add;
        added.type = MusicalEventType::Climax;
        added.timeMS = 30000;
        added.durationMS = 4000;
        r.corrections.analysis.events.push_back(added);
        const std::string json = ToJson(r.corrections);
        CorrectionSet back;
        std::string error;
        Check(FromJson(json, back, error) && ToJson(back) == json && back.analysis.bar.firstDownbeatMS == firstBeat, "music corrections read back from their file " + error);
        GenerationRun bar = Generate(r);
        const auto downbeats = std::count_if(bar.analysis.events.begin(), bar.analysis.events.end(), [](const MusicalEvent& e) { return e.type == MusicalEventType::Downbeat; });
        Check(bar.ok && downbeats > 0 && bar.choreography.metrics.accents > 0,
              fmt::format("the song had {} downbeats detected; with the bar marked by hand it has {}, and the choreography {} accents", detectedDownbeats, downbeats, bar.choreography.metrics.accents));
        GenerationSidecar sidecar;
        Check(ReadSidecar(bar.sidecarPath, sidecar, error) == SidecarStatus::Ok && sidecar.analysisCorrections.bar.firstDownbeatMS == firstBeat && sidecar.analysisCorrections.events.size() == 1 && !sidecar.audioPath.empty(),
              "the sidecar keeps them, and where the song is");
        GenerationRequest again = Request("compas-regenerado");
        again.previousSidecarPath = bar.sidecarPath;
        again.seed = 31;
        GenerationRun regenerated = Generate(again, GenerationStage::Choreography);
        const auto kept = std::count_if(regenerated.analysis.events.begin(), regenerated.analysis.events.end(), [](const MusicalEvent& e) { return e.type == MusicalEventType::Downbeat; });
        Check(kept == downbeats, fmt::format("regenerated with another seed: the same {} downbeats", kept));
    }

    // ------------------------------------------------------------ B. real layout

    void TestRealLayout() {
        if (_options.showDir.empty() || _options.audioPath.empty()) {
            return;
        }
        Title("B-C. The real layout (a copy, controllers inactive): every level written, rendered and saved again");
        std::string refused;
        if (!ControllersAreInactive(_options.showDir, refused)) {
            Check(false, "refused: " + refused);
            return;
        }
        const std::string before = Sha256OfFile((fs::path(_options.showDir) / "xlights_rgbeffects.xml").string()) + Sha256OfFile((fs::path(_options.showDir) / "xlights_networks.xml").string());
        const std::string tag = _root.filename().string();
        for (AutomationLevel level : { AutomationLevel::Conservative, AutomationLevel::Balanced, AutomationLevel::Creative }) {
            if (!_options.level.empty() && _options.level != LevelId(level)) {
                continue;
            }
            GenerationRequest r;
            r.audioPath = _options.audioPath;
            r.showDir = _options.showDir;
            r.lyricsPath = _options.lyricsPath;
            r.timingPath = _options.timingPath;
            r.sequenceName = fmt::format("composer-{}-{}.xsq", LevelId(level), tag);
            r.reportDir = (_root / "real" / LevelId(level)).string();
            r.level = level;
            r.seed = 2026;
            r.progress = [](GenerationStage stage, const std::string& what) {
                if (what != "start" && what.find('%') == std::string::npos) {
                    printf("    [%s] %s\n", StageId(stage), what.c_str());
                    fflush(stdout);
                }
            };
            const auto start = std::chrono::steady_clock::now();
            const GenerationRun run = GenerateShow(r);
            const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            PrintRun("    ", run);
            int participants = 0;
            int lit = 0;
            int unexplained = 0;
            for (const TargetCoverage& t : run.coverage) {
                participants += t.plannedSegments > 0 ? 1 : 0;
                lit += t.framesLit > 0 ? 1 : 0;
                unexplained += t.plannedSegments > 0 && t.events == 0 && t.note.empty() ? 1 : 0;
            }
            printf("  %s: %zu events, %zu effects written, %d targets in the plan, %d lit, %d frames, %lld ms in all\n", LevelId(level), run.choreography.events.size(), run.write.ok ? run.spec.effects.size() : 0, participants, lit,
                   run.render.frames, ms);
            Check(run.ok, fmt::format("{}: generated, validated, rendered, published and saved again", LevelId(level)));
            Check(run.write.ok && run.spec.effects.size() == run.choreography.events.size() && run.spec.effects.size() > 32, fmt::format("{}: the whole choreography written ({} effects)", LevelId(level), run.spec.effects.size()));
            Check(run.render.frames == run.render.expectedFrames && run.render.stepMS == r.frameMS && run.render.channels > 0, fmt::format("{}: {} frames, {} channels", LevelId(level), run.render.frames, run.render.channels));
            Check(run.write.ok && unexplained == 0, fmt::format("{}: every figure in the plan was lit, or the report says why not ({} without a reason)", LevelId(level), unexplained));
            if (level == AutomationLevel::Balanced || !_options.level.empty()) {
                GenerationRequest same = r;
                same.sequenceName = fmt::format("composer-{}-{}-again.xsq", LevelId(level), tag);
                same.reportDir = (_root / "real" / (std::string(LevelId(level)) + "-again")).string();
                same.progress = {};
                const GenerationRun repeat = GenerateShow(same, GenerationStage::Spec);
                Check(SpecHash(repeat.spec) == SpecHash(run.spec) && ToJson(repeat.choreography) == ToJson(run.choreography), fmt::format("{}: the same inputs give the same sequence {}", LevelId(level), SpecHash(run.spec)));
            }
        }
        const std::string after = Sha256OfFile((fs::path(_options.showDir) / "xlights_rgbeffects.xml").string()) + Sha256OfFile((fs::path(_options.showDir) / "xlights_networks.xml").string());
        Check(before == after, "the layout and networks files of the show are byte for byte what they were");
    }

    fs::path _root;
    PipelineTestOptions _options;
    fs::path _show;
    fs::path _audio;
    fs::path _lrc;
    fs::path _plain;
    std::string _layoutHash;
    GenerationRun _base;
    GenerationRun _synced;
    bool _expectSuccess = true;
    int _passed = 0;
    int _failed = 0;
};

} // namespace

int RunGenerateCommand(const GenerateCommandOptions& o) {
    GenerationRequest r;
    r.audioPath = o.audioPath;
    r.showDir = o.showDir;
    r.sequenceName = o.sequenceName;
    r.reportDir = o.reportDir;
    r.profilePath = o.profilePath;
    r.rolesPath = o.rolesPath;
    r.templatesDir = o.templatesDir;
    r.cacheDir = o.cacheDir;
    r.lyricsPath = o.lyricsPath;
    r.timingPath = o.timingPath;
    r.timingTrack = o.timingTrack;
    r.lyricSessionPath = o.lyricSessionPath;
    r.correctionsPath = o.correctionsPath;
    r.previousSidecarPath = o.previousSidecarPath;
    r.render = o.render;
    r.renderTimeoutMS = o.renderTimeoutMS;
    if (!o.level.empty()) {
        AutomationLevel level;
        if (!LevelFrom(o.level, level)) {
            printf("composer-generate: REFUSED: unknown level '%s' (conservative, balanced or creative)\n", o.level.c_str());
            return 2;
        }
        r.level = level;
    }
    if (!o.seed.empty()) {
        char* end = nullptr;
        const unsigned long long seed = std::strtoull(o.seed.c_str(), &end, 10);
        if (end == o.seed.c_str() || *end != '\0') {
            printf("composer-generate: REFUSED: the seed must be a whole number: '%s'\n", o.seed.c_str());
            return 2;
        }
        r.seed = seed;
    }
    GenerationStage last = GenerationStage::Publish;
    if (!o.stopAfter.empty() && !StageFromId(o.stopAfter, last)) {
        printf("composer-generate: REFUSED: unknown stage '%s'\n", o.stopAfter.c_str());
        return 2;
    }
    if (!o.cancelFile.empty()) {
        const std::string cancelFile = o.cancelFile;
        r.cancel = [cancelFile]() { return FileExists(cancelFile); };
    }
    r.progress = [](GenerationStage stage, const std::string& what) {
        printf("composer-generate: [%s] %s\n", StageId(stage), what.c_str());
        fflush(stdout);
    };
    const GenerationRun run = GenerateShow(r, last);
    PrintRun("composer-generate", run);
    if (run.cancelled) {
        printf("composer-generate: CANCELLED, nothing published\n");
        return 3;
    }
    if (run.Stage(GenerationStage::Inputs).status == StageStatus::Failed) {
        printf("composer-generate: REFUSED\n");
        return 2;
    }
    if (!run.ok) {
        printf("composer-generate: FAILED, nothing published; diagnostics in %s\n", r.reportDir.c_str());
        return 1;
    }
    if (last != GenerationStage::Publish) {
        printf("composer-generate: stopped after %s as asked; diagnostics in %s\n", StageId(last), r.reportDir.c_str());
        return 0;
    }
    printf("composer-generate: PUBLISHED %s (%zu effects) with %s; render %s; report in %s\n", run.sequencePath.c_str(), run.spec.effects.size(), run.sidecarPath.c_str(), run.render.ran ? run.fseqPath.c_str() : "not run",
           r.reportDir.c_str());
    printf("composer-generate: open it with the show folder %s; nobody has reviewed how it looks\n", r.showDir.c_str());
    return 0;
}

int RunSnapshotCommand(const SnapshotOptions& o) {
    std::string refused;
    if (!ControllersAreInactive(o.showDir, refused)) {
        printf("composer-snapshots: REFUSED: %s\n", refused.c_str());
        return 2;
    }
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(o.showDir)) {
        printf("composer-snapshots: cannot load the show %s\n", o.showDir.c_str());
        return 1;
    }
    std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(o.fseqPath));
    if (file == nullptr) {
        printf("composer-snapshots: cannot read %s\n", o.fseqPath.c_str());
        return 1;
    }
    std::error_code ec;
    fs::create_directories(fs::path(o.outDir), ec);
    const uint32_t channels = file->getMaxChannel();
    file->setReadPattern(FSEQFile::ReadPattern::Bulk);
    std::vector<std::pair<uint32_t, uint32_t>> ranges{ { 0, channels } };
    file->prepareRead(ranges);
    std::vector<uint8_t> data(channels, 0);
    int written = 0;
    for (const std::string& name : o.models) {
        Model* model = ctx.AllModels[name];
        if (model == nullptr) {
            printf("composer-snapshots: no model '%s'\n", name.c_str());
            continue;
        }
        int width = 1;
        int height = 1;
        for (uint32_t n = 0; n < model->GetNodeCount(); ++n) {
            const NodeBaseClass* node = model->GetNode(n);
            for (uint32_t c = 0; c < node->Coords.size(); ++c) {
                width = std::max(width, node->Coords[c].bufX + 1);
                height = std::max(height, node->Coords[c].bufY + 1);
            }
        }
        const int scale = std::max(1, o.scale);
        for (int ms : o.timesMS) {
            const int frame = ms / std::max(1, file->getStepTime());
            std::fill(data.begin(), data.end(), 0);
            std::unique_ptr<FSEQFile::FrameData> frameData(file->getFrame(frame));
            if (frameData == nullptr || !frameData->readFrame(data.data(), channels)) {
                printf("composer-snapshots: frame %d unreadable\n", frame);
                continue;
            }
            xlImage image(width * scale, height * scale);
            for (int y = 0; y < height * scale; ++y) {
                for (int x = 0; x < width * scale; ++x) {
                    image.SetRGBA(x, y, 24, 24, 24, 255);
                }
            }
            int lit = 0;
            uint64_t digest = 1469598103934665603ULL;
            for (uint32_t n = 0; n < model->GetNodeCount(); ++n) {
                NodeBaseClass* node = model->GetNode(n);
                if (node->ActChan + node->GetChanCount() > channels) {
                    continue;
                }
                node->SetFromChannels(&data[node->ActChan]);
                xlColor color;
                node->GetColor(color);
                const bool on = color.red != 0 || color.green != 0 || color.blue != 0;
                lit += on ? 1 : 0;
                digest = (digest ^ (on ? n + 1 : 0) ^ (static_cast<uint64_t>(color.red) << 16 | color.green << 8 | color.blue)) * 1099511628211ULL;
                for (uint32_t c = 0; c < node->Coords.size(); ++c) {
                    // The buffer counts rows from the bottom.
                    const int bx = node->Coords[c].bufX;
                    const int by = height - 1 - node->Coords[c].bufY;
                    for (int dy = 1; dy < scale; ++dy) {
                        for (int dx = 1; dx < scale; ++dx) {
                            image.SetRGBA(bx * scale + dx, by * scale + dy, on ? color.red : 50, on ? color.green : 50, on ? color.blue : 50, 255);
                        }
                    }
                }
            }
            std::vector<uint8_t> png;
            std::string safe = name;
            for (char& ch : safe) {
                if (!std::isalnum(static_cast<unsigned char>(ch))) {
                    ch = '_';
                }
            }
            const fs::path out = fs::path(o.outDir) / fmt::format("{}_{:06d}ms.png", safe, ms);
            if (!image.SaveAsPNG(png) || !WriteText(out, std::string(png.begin(), png.end()))) {
                printf("composer-snapshots: cannot write %s\n", out.string().c_str());
                continue;
            }
            ++written;
            printf("composer-snapshots: %s at %d ms (frame %d): %d of %u nodes lit, picture %016llx -> %s\n", name.c_str(), ms, frame, lit, model->GetNodeCount(), static_cast<unsigned long long>(digest), out.string().c_str());
        }
    }
    printf("composer-snapshots: %d pictures written; they show the render, nobody has judged them\n", written);
    return written > 0 ? 0 : 1;
}

int RunPipelineTest(const PipelineTestOptions& options) {
    fs::path root;
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        root = fs::path(options.outDir) / fmt::format("composer-pipeline-test-{}", i);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-pipeline-test: cannot create %s\n", root.string().c_str());
        return 1;
    }
    Suite suite(root, options);
    return suite.Run();
}

} // namespace composer
