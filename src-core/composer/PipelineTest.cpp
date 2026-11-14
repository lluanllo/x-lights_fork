#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "composer/PipelineTest.h"

#include "composer/ArtisticPreferences.h"
#include "composer/VisualReview.h"

#include "composer/ComposerReview.h"
#include "composer/GenerationSidecar.h"
#include "composer/MediaAnalysis.h"
#include "composer/MediaLibrary.h"
#include "composer/MediaTest.h"
#include "composer/VisualTest.h"
#include "composer/TemplateLibrary.h"
#include "composer/PlanTest.h"
#include "composer/SequenceWriter.h"
#include "composer/ShowGenerationPipeline.h"
#include "composer/SyntheticAudio.h"
#include "composer/WriterTest.h"
#include "models/Model.h"
#include "models/ModelGroup.h"
#include "models/ModelScreenLocation.h"
#include "models/Node.h"
#include "render/FSEQFile.h"
#include "render/HeadlessRenderContext.h"
#include "utils/Color.h"
#include "utils/xlImage.h"
#include "utils/ExternalHooks.h"

#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <numeric>
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
        TestLrcFiles();
        TestVoices();
        TestMedia();
        TestStoryboard();
        TestScreens();
        TestReviewLoop();
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

    void TestVoices() {
        Title("A2c. Voices: a narrator, two singers and a choir, each face on its own voice");
        const std::string text = "[Narrador]\nHabía una vez una casa encendida\n\n[Voz: Ana]\n¿Dónde está el niño?\n\n[Voz: Luis]\nPingüino en la nieve\n\n[Voz: Coro]\n¡Año nuevo, señor!\n";
        const LyricDocument doc = ParsePlainLyrics(text);
        LyricSession session;
        session.trackName = "Composer Lyrics";
        session.lyrics = text;
        const std::vector<std::pair<int, int>> times = { { 2000, 6000 }, { 9000, 13000 }, { 13500, 17000 }, { 25000, 29000 } };
        size_t k = 0;
        for (const LyricLine* line : doc.Lines()) {
            LyricCorrection mark;
            mark.targetId = line->id;
            mark.startMS = times[k].first;
            mark.endMS = times[k].second;
            session.corrections.push_back(mark);
            ++k;
        }
        // The narrator is heard and not seen: a person's choice, kept with the show.
        VoiceDef narrator = *doc.cast.Find("v:narrador");
        narrator.facesOff = true;
        session.voices = { narrator };
        const fs::path sessionPath = _root / "voces-session.json";
        std::string error;
        Check(SaveLyricSession(sessionPath.string(), session, error), "a session with the marks and a voice choice " + error);
        GenerationRequest r = Request("voces");
        r.lyricSessionPath = sessionPath.string();
        const GenerationRun run = Generate(r);
        Check(run.ok && run.cast.voices.size() == 4 && run.voiceTracks.size() == 4, fmt::format("published with {} voices and {} voice tracks", run.cast.voices.size(), run.voiceTracks.size()));
        const SequenceSpec read = Reopen(run);
        const VoiceDef* ana = run.cast.Find("v:ana");
        Check(ana != nullptr && !ana->faces.empty() && Track(read, "Composer Lyrics - Ana") != nullptr && Track(read, "Composer Lyrics - Coro") != nullptr && Track(read, "Composer Lyrics") != nullptr,
              "the sequence has the whole lyrics track and one per voice");
        bool ownTrack = true;
        int faces = 0;
        for (const EffectSpec& e : read.effects) {
            if (e.effect != "Faces") {
                continue;
            }
            ++faces;
            const std::string track = Setting(e, "E_CHOICE_Faces_TimingTrack");
            ownTrack = ownTrack && track.rfind("Composer Lyrics - ", 0) == 0 && Track(read, track) != nullptr;
        }
        Check(faces > 0 && ownTrack, fmt::format("{} face effects, each on a voice's track that is in the sequence", faces));
        bool narratorSilent = true;
        for (const EffectSpec& e : read.effects) {
            narratorSilent = narratorSilent && !(e.effect == "Faces" && e.startMS < 6000 && e.endMS > 2000 && Setting(e, "E_CHOICE_Faces_TimingTrack") == "Composer Lyrics - Narrador");
        }
        Check(narratorSilent, "the narrator, set to have no face, moves none");
        GenerationSidecar sidecar;
        Check(ReadSidecar(run.sidecarPath, sidecar, error) == SidecarStatus::Ok && sidecar.lyrics.voices.size() == 1 && sidecar.lyrics.voices[0].facesOff, "the voice choice is in the sidecar");
        GenerationRequest again = Request("voces-otra-vez");
        again.previousSidecarPath = run.sidecarPath;
        const GenerationRun second = Generate(again);
        Check(second.ok && second.voiceTracks.size() == 4 && second.cast.Find("v:narrador") != nullptr && second.cast.Find("v:narrador")->facesOff &&
                  second.cast.Find("v:ana")->faces == ana->faces,
              "regenerated from the sidecar alone: the same voices, the same faces");
    }

    static std::string FullName(const EffectSpec& e) {
        return e.subModel.empty() ? e.target : e.target + "/" + e.subModel;
    }

    static bool HasReason(const MediaAssignment& a, const char* code) {
        return std::any_of(a.reasons.begin(), a.reasons.end(), [&](const Reason& r) { return r.code == code; });
    }

    static const RenderMetric* Metric(const GenerationRun& run, const EffectSpec& e) {
        for (const RenderMetric& m : run.renderValidation.metrics) {
            if (m.target == FullName(e) && m.effect == e.effect && m.startMS == e.startMS) {
                return &m;
            }
        }
        return nullptr;
    }

    // A song with every kind of section, cut where the test wants them.
    static bool WriteMontageSong(const fs::path& path) {
        testaudio::SyntheticSong song(96);
        const auto music = [&](double from, double to, double kick, double click, double tone, double freq) {
            for (double t = from; t < to - 0.01; t += 0.5) {
                song.Kick(t, kick);
                if (click > 0) {
                    song.Click(t + 0.25, click);
                }
            }
            for (double t = from; t < to - 0.01; t += 2.0) {
                song.Tone(t, std::min(2.0, to - t), freq, tone);
                song.Tone(t, std::min(2.0, to - t), freq * 1.5, tone * 0.6);
            }
        };
        song.Tone(0, 8, 196, 0.08, 0.5);
        for (double t = 0; t < 8; t += 1.0) {
            song.Kick(t, 0.12);
        }
        music(8, 20, 0.35, 0.12, 0.1, 220);   // verse
        music(20, 32, 0.75, 0.4, 0.25, 262);  // chorus
        music(32, 44, 0.35, 0.12, 0.1, 220);  // verse
        music(44, 56, 0.75, 0.4, 0.25, 262);  // chorus
        music(56, 64, 0.3, 0.0, 0.15, 175);   // bridge
        music(64, 76, 0.6, 0.5, 0.05, 330);   // instrumental
        music(76, 88, 0.95, 0.6, 0.3, 262);   // last chorus, the climax
        song.Tone(88, 8, 196, 0.06, 0.3);     // ending
        return song.Save(path.string());
    }

    static std::string ShotKey(const StoryboardShot& s) {
        return fmt::format("{}|{}|{}|{}", s.lane, s.startMS, s.endMS, s.mediaId);
    }

    void TestStoryboard() {
        Title("A2h. A folder of pictures and videos made into a montage for the matrix and the mega tree");
        const fs::path folder = _show / "Medios Fase 11";
        std::string error;
        Check(WriteVisualFixtures(folder.string(), error), "a folder of original pictures and videos, many of them, made by the test " + error);
        std::map<std::string, std::string> before;
        for (const auto& e : fs::recursive_directory_iterator(folder)) {
            if (e.is_regular_file()) {
                before[e.path().string()] = Sha256OfFile(e.path().string());
            }
        }
        const fs::path song = _root / "montaje.wav";
        const fs::path lrc = _root / "montaje.lrc";
        Check(WriteMontageSong(song) && WriteText(lrc, "[00:09.00]Nieve en la ventana\n[00:13.50]Luces en el árbol\n[00:33.00]Estrellas de invierno\n[00:37.50]Nieve en el camino\n[00:43.50]\n"),
              "a 96 second song with an intro, verses with words, a repeated chorus, a bridge, an instrumental, a climax and an ending");
        const std::string profile = (fs::path(DefaultTemplateDirectory()).parent_path() / "profiles" / "reference-winter-cinematic.json").string();
        const std::vector<std::pair<int, SectionKind>> layout = { { 0, SectionKind::Intro },  { 8000, SectionKind::Verse },        { 20000, SectionKind::Chorus },
                                                                  { 32000, SectionKind::Verse }, { 44000, SectionKind::Chorus },     { 56000, SectionKind::Bridge },
                                                                  { 64000, SectionKind::Instrumental }, { 76000, SectionKind::Chorus }, { 88000, SectionKind::Outro } };
        const auto request = [&](const std::string& name, AutomationLevel level) {
            GenerationRequest r = Request(name);
            r.audioPath = song.string();
            r.lyricsPath = lrc.string();
            r.profilePath = profile;
            r.level = level;
            r.mediaFolders = { folder.string() };
            r.scanMedia = true;
            for (size_t i = 0; i < layout.size(); ++i) {
                SectionCorrection c;
                c.startMS = layout[i].first;
                c.endMS = i + 1 < layout.size() ? layout[i + 1].first : 96000;
                c.kind = layout[i].second;
                r.corrections.analysis.sections.push_back(c);
            }
            return r;
        };
        const GenerationRun run = Generate(request("montaje", AutomationLevel::Balanced));
        Check(run.ok, "published with the folder analysed and the montage planned");
        if (!run.ok) {
            return;
        }
        const VisualMediaStoryboard& b = run.media.storyboard;
        printf("%s", StoryboardReport(b).c_str());
        const auto lane = [&](const char* role) {
            const auto it = std::find_if(b.lanes.begin(), b.lanes.end(), [&](const StoryboardLane& l) { return l.role == role; });
            return it != b.lanes.end() ? &*it : nullptr;
        };
        const StoryboardLane* matrix = lane("matrix");
        const StoryboardLane* tree = lane("mega-tree");
        Check(matrix != nullptr && matrix->target == "Matriz Prueba" && tree != nullptr && tree->target == "Arbol Grande" && tree->degrees >= 360,
              "two screens planned together: the matrix and the 360 degree mega tree");
        const auto stats = [&](const std::string& target) {
            const auto it = std::find_if(b.stats.lanes.begin(), b.stats.lanes.end(), [&](const StoryboardLaneStats& l) { return l.lane == target; });
            return it != b.stats.lanes.end() ? *it : StoryboardLaneStats();
        };
        const StoryboardLaneStats ms = stats("Matriz Prueba");
        const StoryboardLaneStats ts = stats("Arbol Grande");
        Check(ms.shots >= 3 && ms.ofFree >= 0.4, fmt::format("the matrix is mostly made up: {} shots, {:.0f}% of its free time", ms.shots, ms.ofFree * 100));
        Check(ts.shots >= 2 && ts.ofFree >= 0.2, fmt::format("so is the mega tree: {} shots, {:.0f}% of its free time", ts.shots, ts.ofFree * 100));
        Check(b.stats.distinctFiles >= 5 && b.stats.images > 0 && b.stats.videos > 0, fmt::format("{} different files, {} pictures and {} videos", b.stats.distinctFiles, b.stats.images + b.stats.animations, b.stats.videos));
        Check(b.stats.consecutiveRepeats == 0, "no file twice in a row on a screen");
        Check(b.stats.repeats == b.stats.repeatsByMusic || b.stats.repeats <= 2, fmt::format("repeats only where the music repeats or a video shows another of its scenes ({} repeats, {} by the music)", b.stats.repeats, b.stats.repeatsByMusic));
        bool together = true, explained = true, levels = true, frames = true;
        std::set<std::string> treeFramings;
        for (const StoryboardShot& s : b.shots) {
            for (const StoryboardShot& o : b.shots) {
                together = together && !(s.sceneId == o.sceneId && s.lane != o.lane && s.mediaId == o.mediaId);
            }
            const auto has = [&](const char* code) { return std::any_of(s.reasons.begin(), s.reasons.end(), [&](const Reason& r) { return r.code == code; }); };
            explained = explained && has("theme-category") && has("aspect") && (has("scene-role") || s.state != DecisionState::Automatic);
            levels = levels && !s.level.empty() && s.score > 0 && s.score <= 1;
            if (s.lane == "Arbol Grande") {
                treeFramings.insert(s.framing);
                frames = frames && (s.framing.rfind("tree-", 0) == 0 || s.framing == "video-wrap" || s.framing == "video-faces" || s.framing.rfind("gif-", 0) == 0);
            } else {
                frames = frames && s.framing.rfind("tree-", 0) != 0;
            }
        }
        Check(together, "the two screens of a scene never show the same file");
        Check(explained && levels, "every shot says why (theme, shape, the scene's part) and how suitable it is, never as a certainty");
        Check(frames, fmt::format("the tree is framed for a tree ({}), the matrix for a matrix", std::accumulate(treeFramings.begin(), treeFramings.end(), std::string(), [](std::string a, const std::string& x) { return a + (a.empty() ? "" : ", ") + x; })));
        // Words on the matrix keep it: no picture there while the lyrics are shown.
        bool lyrics = true;
        for (const PlanSegment& seg : run.plan.segments) {
            const bool words = std::any_of(seg.assignments.begin(), seg.assignments.end(), [](const PlanAssignment& a) { return a.intent == "lyric-text" && a.target == "Matriz Prueba"; });
            for (const StoryboardShot& s : b.shots) {
                lyrics = lyrics && !(words && s.lane == "Matriz Prueba" && s.startMS < seg.endMS && s.endMS > seg.startMS && s.state == DecisionState::Automatic);
            }
        }
        Check(lyrics, "the words on the matrix keep their priority");
        // Cuts inside a scene fall on the beat.
        int cuts = 0, onBeat = 0;
        for (size_t i = 1; i < b.shots.size(); ++i) {
            const StoryboardShot& p = b.shots[i - 1];
            const StoryboardShot& s = b.shots[i];
            if (p.lane != s.lane || p.sceneId != s.sceneId || p.endMS != s.startMS) {
                continue;
            }
            ++cuts;
            onBeat += std::any_of(run.analysis.events.begin(), run.analysis.events.end(), [&](const MusicalEvent& e) {
                return (e.type == MusicalEventType::Downbeat || e.type == MusicalEventType::Beat) && std::abs(e.timeMS - s.startMS) <= 25;
            }) ? 1 : 0;
        }
        Check(cuts == 0 || onBeat * 10 >= cuts * 8, fmt::format("{} of the {} cuts inside a scene fall on a beat or a downbeat", onBeat, cuts));
        bool transitions = true;
        for (const StoryboardShot& s : b.shots) {
            const StoryboardScene* sc = b.Scene(s.sceneId);
            transitions = transitions && sc != nullptr && (s.fadeInMS <= (s.endMS - s.startMS) / 4) && (s.fadeOutMS <= (s.endMS - s.startMS) / 4);
        }
        Check(transitions, "each shot comes in and goes out with a cut or a fade that fits inside it");
        // Videos with several scenes give fragments; the effect starts where the shot says.
        bool fragments = true;
        int fragmentShots = 0;
        for (const StoryboardShot& s : b.shots) {
            if (s.kind != MediaKind::Video) {
                continue;
            }
            const MediaItem* m = run.mediaLibrary.Find(s.mediaId);
            if (m == nullptr || m->analysis.video.scenes.size() < 2) {
                continue;
            }
            ++fragmentShots;
            fragments = fragments && s.fragmentScene >= 0 && s.fragmentStartMS >= 0 && s.fragmentStartMS < m->durationMS;
        }
        Check(fragments, fmt::format("{} shots from videos with several scenes start at the scene they chose", fragmentShots));
        // Written natively, read back, every file there, every effect lit.
        const SequenceSpec read = Reopen(run);
        int files = 0, lit = 0, moving = 0, videos = 0, onTree = 0, derived = 0;
        bool exist = true;
        for (const EffectSpec& e : read.effects) {
            if (e.effect != "Pictures" && e.effect != "Video") {
                continue;
            }
            ++files;
            const std::string file = Setting(e, e.effect == "Video" ? "E_FILEPICKERCTRL_Video_Filename" : "E_TEXTCTRL_Pictures_Filename");
            exist = exist && !file.empty() && FileExists(MediaAbsolutePath(file, _show.string()));
            derived += file.rfind("ShowComposer/media-derived/", 0) == 0 ? 1 : 0;
            onTree += FullName(e) == "Arbol Grande" ? 1 : 0;
            const RenderMetric* m = Metric(run, e);
            lit += m != nullptr && m->framesWithLight > 0 ? 1 : 0;
            if (e.effect == "Video") {
                ++videos;
                moving += m != nullptr && m->distinctFrames > 1 ? 1 : 0;
            }
        }
        Check(files > 0 && exist, fmt::format("{} native Pictures and Video effects read back from the sequence, {} on the tree, {} through derived pictures, every file there", files, onTree, derived));
        Check(lit == files, fmt::format("each lights its figure in the headless render ({} of {})", lit, files));
        Check(videos == 0 || moving == videos, fmt::format("each video moves from frame to frame ({} of {})", moving, videos));
        Check(Count(read, "Text") > 0, "with the words still on the display");
        bool untouched = true;
        for (const auto& [path, hash] : before) {
            untouched = untouched && Sha256OfFile(path) == hash;
        }
        Check(untouched, "no original picture or video was changed");
        Check(FileExists((_root / "reports" / "montaje" / "storyboard.json").string()) && FileExists((_root / "reports" / "montaje" / "storyboard-report.txt").string()),
              "the storyboard is in the report");

        Title("A2i. The same inputs give the same montage; the three levels fill more or less");
        GenerationRequest same = request("montaje-igual", AutomationLevel::Balanced);
        same.scanMedia = false;
        const GenerationRun again = Generate(same);
        Check(again.ok && again.media.storyboard.hash == b.hash, fmt::format("made again, the same storyboard ({} and {})", b.hash, again.media.storyboard.hash));
        double previous = -1;
        bool rising = true;
        std::string coverage;
        for (AutomationLevel level : { AutomationLevel::Conservative, AutomationLevel::Balanced, AutomationLevel::Creative }) {
            const GenerationRun lv = level == AutomationLevel::Balanced ? GenerationRun() : Generate(request(level == AutomationLevel::Conservative ? "montaje-conservador" : "montaje-creativo", level));
            const VisualMediaStoryboard& bb = level == AutomationLevel::Balanced ? b : lv.media.storyboard;
            double total = 0;
            for (const StoryboardLaneStats& l : bb.stats.lanes) {
                total += l.coverage;
            }
            coverage += fmt::format("{}{:.0f}%", coverage.empty() ? "" : ", ", total * 50);
            rising = rising && total >= previous - 0.001;
            previous = total;
        }
        Check(rising, "conservative, balanced and creative fill the screens more and more: " + coverage);

        Title("A2j. A person's changes: lock a scene, take out, replace, move, rebuild, bring back");
        std::string lockScene, removeScene, replaceScene, moveScene;
        for (const StoryboardScene& sc : b.scenes) {
            const auto on = [&](const char* target) { return std::any_of(b.shots.begin(), b.shots.end(), [&](const StoryboardShot& s) { return s.sceneId == sc.id && s.lane == target; }); };
            const bool m = on("Matriz Prueba"), t = on("Arbol Grande");
            if (lockScene.empty() && m && t) {
                lockScene = sc.id;
            } else if (removeScene.empty() && m) {
                removeScene = sc.id;
            } else if (replaceScene.empty() && m) {
                replaceScene = sc.id;
            } else if (moveScene.empty() && t && !m) {
                moveScene = sc.id;
            }
        }
        Check(!lockScene.empty() && !removeScene.empty() && !replaceScene.empty(), fmt::format("scenes to change: lock {}, remove {}, replace {}, move {}", lockScene, removeScene, replaceScene, moveScene));
        if (lockScene.empty() || removeScene.empty() || replaceScene.empty()) {
            return;
        }
        GenerationRequest changed = request("montaje-cambiado", AutomationLevel::Balanced);
        changed.previousSidecarPath = run.sidecarPath;
        changed.scanMedia = false;
        changed.mediaVariant = 1;
        changed.corrections.media = LockStoryboardScene(b, lockScene);
        MediaCorrection off;
        off.action = MediaCorrection::Action::Remove;
        off.segmentId = removeScene;
        off.startMS = b.Scene(removeScene)->startMS;
        off.target = "Matriz Prueba";
        UpsertMediaCorrection(changed.corrections.media, off);
        const MediaItem* treeFile = nullptr;
        for (const MediaItem& m : run.mediaLibrary.items) {
            if (m.name == "árbol de navidad vertical.png") {
                treeFile = &m;
            }
        }
        MediaCorrection put;
        put.action = MediaCorrection::Action::Assign;
        put.segmentId = replaceScene;
        put.startMS = b.Scene(replaceScene)->startMS;
        put.target = "Matriz Prueba";
        put.mediaId = treeFile != nullptr ? treeFile->id : std::string();
        put.path = treeFile != nullptr ? treeFile->relative : std::string();
        UpsertMediaCorrection(changed.corrections.media, put);
        std::string movedMedia;
        if (!moveScene.empty()) {
            for (const StoryboardShot& s : b.shots) {
                if (s.sceneId == moveScene && s.lane == "Arbol Grande" && movedMedia.empty()) {
                    movedMedia = s.mediaId;
                }
            }
            MediaCorrection from;
            from.action = MediaCorrection::Action::Remove;
            from.segmentId = moveScene;
            from.startMS = b.Scene(moveScene)->startMS;
            from.target = "Arbol Grande";
            UpsertMediaCorrection(changed.corrections.media, from);
            MediaCorrection to;
            to.action = MediaCorrection::Action::Lock;
            to.segmentId = moveScene;
            to.startMS = from.startMS;
            to.target = "Matriz Prueba";
            to.mediaId = movedMedia;
            UpsertMediaCorrection(changed.corrections.media, to);
        }
        const GenerationRun second = Generate(changed);
        Check(second.ok, "rebuilt with the changes and another variant");
        if (!second.ok) {
            return;
        }
        const VisualMediaStoryboard& c = second.media.storyboard;
        std::set<std::string> lockedBefore, lockedAfter;
        for (const StoryboardShot& s : b.shots) {
            if (s.sceneId == lockScene) {
                lockedBefore.insert(ShotKey(s));
            }
        }
        for (const StoryboardShot& s : c.shots) {
            if (s.sceneId == lockScene) {
                lockedAfter.insert(ShotKey(s));
            }
        }
        Check(lockedBefore == lockedAfter && std::all_of(c.shots.begin(), c.shots.end(), [&](const StoryboardShot& s) { return s.sceneId != lockScene || s.state == DecisionState::Locked; }),
              "the locked scene is exactly as it was, on both screens");
        Check(std::none_of(c.shots.begin(), c.shots.end(), [&](const StoryboardShot& s) { return s.sceneId == removeScene && s.lane == "Matriz Prueba"; }), "nothing on the matrix where it was taken out");
        Check(treeFile != nullptr && std::any_of(c.shots.begin(), c.shots.end(), [&](const StoryboardShot& s) { return s.sceneId == replaceScene && s.lane == "Matriz Prueba" && s.mediaId == treeFile->id; }),
              "the replaced picture plays where the person put it");
        if (!moveScene.empty()) {
            Check(std::any_of(c.shots.begin(), c.shots.end(), [&](const StoryboardShot& s) { return s.sceneId == moveScene && s.lane == "Matriz Prueba" && s.mediaId == movedMedia; }) &&
                      std::none_of(c.shots.begin(), c.shots.end(), [&](const StoryboardShot& s) { return s.sceneId == moveScene && s.lane == "Arbol Grande"; }),
                  "the file moved from the tree to the matrix");
        }
        Check(c.hash != b.hash && c.variant == 1, "another variant: the rest of the montage is planned again");
        GenerationSidecar sidecar;
        Check(ReadSidecar(second.sidecarPath, sidecar, error) == SidecarStatus::Ok && sidecar.media.history.size() == 1 && sidecar.media.history.front().hash == b.hash && sidecar.media.variant == 1,
              "the sidecar keeps the earlier storyboard to bring it back");
        if (sidecar.media.history.empty()) {
            return;
        }
        GenerationRequest back = request("montaje-recuperado", AutomationLevel::Balanced);
        back.previousSidecarPath = second.sidecarPath;
        back.scanMedia = false;
        back.corrections.media = RestoreStoryboard(sidecar.media.history.front(), c);
        const GenerationRun restored = Generate(back);
        std::set<std::string> was, now;
        for (const StoryboardShot& s : b.shots) {
            was.insert(ShotKey(s));
        }
        for (const StoryboardShot& s : restored.media.storyboard.shots) {
            now.insert(ShotKey(s));
        }
        Check(restored.ok && was == now, fmt::format("the earlier version comes back shot by shot ({} and {} shots)", was.size(), now.size()));
    }

    // The fixture show with more screens: a second matrix, one with no controller, one
    // sharing another's channels, and a tree that goes half way round.
    static bool WriteScreensShow(const fs::path& folder) {
        if (!WritePlanFixture(folder.string())) {
            return false;
        }
        pugi::xml_document doc;
        if (!doc.load_file((folder / "xlights_rgbeffects.xml").wstring().c_str())) {
            return false;
        }
        pugi::xml_node models = doc.child("xrgb").child("models");
        const auto add = [&](const char* name, const char* displayAs, int channel, std::initializer_list<std::pair<const char*, std::string>> attrs) {
            pugi::xml_node m = models.append_child("model");
            m.append_attribute("name") = name;
            m.append_attribute("DisplayAs") = displayAs;
            m.append_attribute("StringType") = "RGB Nodes";
            m.append_attribute("LayoutGroup") = "Default";
            m.append_attribute("StartSide") = "B";
            m.append_attribute("Dir") = "L";
            m.append_attribute("versionNumber") = "8";
            m.append_attribute("StartChannel") = std::to_string(channel).c_str();
            for (const auto& [k, v] : attrs) {
                m.append_attribute(k) = v.c_str();
            }
        };
        add("Matriz Dos", "Matrix", 20001, { { "NumStrings", "1" }, { "NodesPerString", "512" }, { "StrandsPerString", "16" }, { "Vertical", "false" }, { "AlternateNodes", "false" }, { "NoZig", "false" } });
        add("Matriz Compartida", "Matrix", 20001, { { "NumStrings", "1" }, { "NodesPerString", "256" }, { "StrandsPerString", "16" }, { "Vertical", "false" }, { "AlternateNodes", "false" }, { "NoZig", "false" } });
        add("Matriz Sin Controlador", "Matrix", 30001,
            { { "NumStrings", "1" }, { "NodesPerString", "288" }, { "StrandsPerString", "12" }, { "Vertical", "false" }, { "AlternateNodes", "false" }, { "NoZig", "false" }, { "Controller", "No Controller" } });
        add("Arbol Medio", "Tree", 40001, { { "NumStrings", "12" }, { "NodesPerString", "30" }, { "StrandsPerString", "1" }, { "TreeType", "0" }, { "TreeDegrees", "180" }, { "TreeBottomTopRatio", "6" }, { "TreePerspective", "0.2" } });
        return doc.save_file((folder / "xlights_rgbeffects.xml").wstring().c_str());
    }

    // Phase 13: generate, review, correct, close and open again, and generate with what
    // was learned. On a show of its own, so what it learns stays there.
    void TestReviewLoop() {
        Title("A2l. A person reviews the montage, corrects it, and the next show learns from it");
        const fs::path show = _root / "aprendizaje" / "show";
        Check(WriteScreensShow(show), "a show of its own for what is learned");
        const fs::path folder = _show / "Medios Fase 11";
        const std::string profile = (fs::path(DefaultTemplateDirectory()).parent_path() / "profiles" / "reference-winter-cinematic.json").string();
        const std::vector<std::pair<int, SectionKind>> layout = { { 0, SectionKind::Intro },  { 8000, SectionKind::Verse },        { 20000, SectionKind::Chorus },
                                                                  { 32000, SectionKind::Verse }, { 44000, SectionKind::Chorus },     { 56000, SectionKind::Bridge },
                                                                  { 64000, SectionKind::Instrumental }, { 76000, SectionKind::Chorus }, { 88000, SectionKind::Outro } };
        int n = 0;
        const auto request = [&](const std::string& name) {
            GenerationRequest r;
            r.audioPath = (_root / "montaje.wav").string();
            r.showDir = show.string();
            r.sequenceName = name + ".xsq";
            r.reportDir = (_root / "reports" / name).string();
            r.level = AutomationLevel::Balanced;
            r.seed = 7;
            r.lyricsPath = (_root / "montaje.lrc").string();
            r.profilePath = profile;
            r.mediaFolders = { folder.string() };
            r.scanMedia = n++ == 0;
            for (size_t i = 0; i < layout.size(); ++i) {
                SectionCorrection c;
                c.startMS = layout[i].first;
                c.endMS = i + 1 < layout.size() ? layout[i + 1].first : 96000;
                c.kind = layout[i].second;
                r.corrections.analysis.sections.push_back(c);
            }
            return r;
        };
        const GenerationRun base = Generate(request("aprender-1"));
        Check(base.ok, "a show generated with its montage");
        if (!base.ok) {
            return;
        }
        GenerationSidecar sidecar;
        std::string error;
        const bool readOk = ReadSidecar(base.sidecarPath, sidecar, error) == SidecarStatus::Ok;
        Check(readOk && sidecar.review.clips.size() == base.media.storyboard.shots.size() &&
                  CountReview(sidecar.review).pending == static_cast<int>(sidecar.review.clips.size()) && sidecar.visualReview == "not-reviewed",
              fmt::format("its sidecar has a review of the {} shots, all pending, and the show is not reviewed", sidecar.review.clips.size()));
        Check(FileExists((fs::path(base.request.reportDir) / "review.json").string()), "and the report has it too");
        // The person reviews three shots of the main screen.
        std::vector<const ClipReview*> main;
        for (const ClipReview& c : sidecar.review.clips) {
            if (c.lane == "Matriz Dos" && main.size() < 3) {
                main.push_back(&c);
            }
        }
        if (main.size() < 3) {
            Check(false, "three shots on the main screen to review");
            return;
        }
        const std::string ok = main[0]->decisionId, no = main[1]->decisionId, swap = main[2]->decisionId;
        const std::string noMedia = main[1]->mediaId, noScene = main[1]->sceneId;
        std::set<std::string> inScene;
        for (const StoryboardShot& s : base.media.storyboard.shots) {
            if (s.sceneId == main[2]->sceneId) {
                inScene.insert(s.mediaId);
            }
        }
        const MediaItem* other = nullptr;
        for (const MediaItem& m : base.mediaLibrary.items) {
            if (other == nullptr && m.Usable() && m.kind == MediaKind::Image && !inScene.contains(m.id) && m.analysis.issues.empty() && !m.disabled) {
                other = &m;
            }
        }
        VisualReview review = sidecar.review;
        DecideClip(review, ok, ClipState::Approved, {}, "", "2026-11-08 18:00");
        DecideClip(review, no, ClipState::Rejected, { "not-music" }, "no pega con la canción", "2026-11-08 18:01");
        Check(other != nullptr && ReplaceClipMedia(review, swap, other->id, other->relative.empty() ? other->path : other->relative, "2026-11-08 18:02"), "one approved, one rejected, one given another file");
        std::string why;
        Check(!ApproveShow(review, "Juan", "now", why), "the show is not approved: nothing was checked in xLights");
        // Learned and kept with the show; the corrections and the review go to the next version.
        ArtisticPreferences prefs;
        Check(ReadPreferences(PreferencesPath(show.string()), prefs, error) && prefs.signals.empty(), "nothing learned before");
        Check(LearnFromReview(prefs, review, &base.mediaLibrary, base.profile.id) > 0 && SavePreferences(PreferencesPath(show.string()), prefs, error), "the decisions are learned and kept with the show");
        GenerationRequest r2 = request("aprender-2");
        r2.corrections.media = ReviewCorrections(review, base.media.storyboard);
        r2.corrections.review = review;
        const GenerationRun next = Generate(r2);
        Check(next.ok, "the corrections applied: another version made");
        if (!next.ok) {
            return;
        }
        const auto shotOf = [](const GenerationRun& run, const std::string& decision) {
            const auto it = std::find_if(run.media.storyboard.shots.begin(), run.media.storyboard.shots.end(), [&](const StoryboardShot& s) { return s.decisionId == decision; });
            return it != run.media.storyboard.shots.end() ? &*it : nullptr;
        };
        const StoryboardShot* kept = shotOf(next, ok);
        const StoryboardShot* before = shotOf(base, ok);
        Check(kept != nullptr && before != nullptr && kept->mediaId == before->mediaId && kept->startMS == before->startMS && kept->endMS == before->endMS && kept->state == DecisionState::Locked,
              "the approved shot is exactly as it was, now locked");
        bool rejectedGone = true;
        for (const StoryboardShot& s : next.media.storyboard.shots) {
            rejectedGone = rejectedGone && !(s.sceneId == noScene && s.lane == "Matriz Dos" && s.mediaId == noMedia);
        }
        Check(rejectedGone, "the rejected file is not on that screen in that scene any more");
        const StoryboardShot* swapped = shotOf(next, swap);
        Check(swapped != nullptr && other != nullptr && swapped->mediaId == other->id, "the shot given another file shows it, in its place");
        int held = 0, same = 0;
        for (const ClipReview& c : review.clips) {
            if (c.state != ClipState::Pending || !c.replacementId.empty()) {
                continue;
            }
            ++held;
            const StoryboardShot* a = shotOf(base, c.decisionId);
            const StoryboardShot* b = shotOf(next, c.decisionId);
            same += a != nullptr && b != nullptr && a->mediaId == b->mediaId && a->startMS == b->startMS && a->endMS == b->endMS ? 1 : 0;
        }
        Check(held > 0 && same == held, fmt::format("the {} shots nobody judged are not made again ({} the same)", held, same));
        GenerationSidecar reopened;
        Check(ReadSidecar(next.sidecarPath, reopened, error) == SidecarStatus::Ok && reopened.review.Find(ok) != nullptr && reopened.review.Find(ok)->state == ClipState::Approved &&
                  reopened.review.Find(swap) != nullptr && reopened.review.Find(swap)->state == ClipState::Pending && reopened.visualReview == "not-reviewed",
              "closed and opened again: the approval is still there, the replaced shot is to be seen again, the show still not approved");
        bool locksKept = true;
        for (const MediaCorrection& c : r2.corrections.media) {
            locksKept = locksKept && std::any_of(reopened.media.corrections.begin(), reopened.media.corrections.end(), [&](const MediaCorrection& k) { return k.decisionId == c.decisionId && k.action == c.action; });
        }
        Check(locksKept, "every correction is kept in the sidecar for the versions to come");
        // Another montage: the person's choices survive; the review's holds can be let go.
        GenerationRequest r3 = request("aprender-3");
        r3.corrections.media = reopened.media.corrections;
        ReleaseReviewHolds(r3.corrections.media);
        r3.corrections.review = reopened.review;
        r3.mediaVariant = 1;
        const GenerationRun third = Generate(r3);
        const StoryboardShot* still = third.ok ? shotOf(third, ok) : nullptr;
        Check(third.ok && still != nullptr && still->mediaId == before->mediaId && still->startMS == before->startMS && shotOf(third, swap) != nullptr && shotOf(third, swap)->mediaId == other->id,
              "rebuilt as another montage: the approved and the replaced shots stay");
        // What was learned shapes a new show; with learning off it is as if nothing was.
        ArtisticPreferences taste;
        for (const char* seq : { "gusto-a.xsq", "gusto-b.xsq" }) {
            VisualReview x = BuildReview(base.media.storyboard, seq, "s", nullptr, &base.mediaLibrary);
            for (const ClipReview& c : x.clips) {
                DecideClip(x, c.decisionId, c.kind == "video" ? ClipState::Rejected : ClipState::Approved, {}, "", "t");
            }
            LearnFromReview(taste, x, &base.mediaLibrary, base.profile.id);
        }
        const std::map<std::string, double> learned = EffectivePreferences(taste);
        Check(learned.contains("kind.video") && learned.at("kind.video") < -0.3, fmt::format("every video turned down in two reviews: a taste for pictures ({:.2f})", learned.contains("kind.video") ? learned.at("kind.video") : 0.0));
        Check(SavePreferences(PreferencesPath(show.string()), taste, error), "kept with the show");
        const auto videos = [](const GenerationRun& run) {
            return static_cast<int>(std::count_if(run.media.storyboard.shots.begin(), run.media.storyboard.shots.end(), [](const StoryboardShot& s) { return s.kind == MediaKind::Video; }));
        };
        const GenerationRun withTaste = Generate(request("aprender-gusto"));
        GenerationRequest plain = request("aprender-sin");
        plain.learning = false;
        const GenerationRun without = Generate(plain);
        Check(withTaste.ok && without.ok && without.media.storyboard.hash == base.media.storyboard.hash && without.preferences.empty(),
              "with learning off the montage is the one made before anything was learned");
        Check(withTaste.ok && withTaste.preferences.contains("kind.video") && videos(withTaste) < videos(base),
              fmt::format("with it, fewer videos: {} instead of {}", videos(withTaste), videos(base)));
        bool said = false;
        for (const StoryboardShot& s : withTaste.media.storyboard.shots) {
            said = said || std::any_of(s.reasons.begin(), s.reasons.end(), [](const Reason& r) { return r.code == "preference"; });
        }
        Check(said, "and each shot it touched says so");
    }

    void TestScreens() {
        Title("A2k. Screens: who shows the words, who the pictures, and the trees");
        const fs::path show = _root / "pantallas" / "show";
        Check(WriteScreensShow(show), "a show with two more matrices (one with no controller, one sharing channels) and a 180 degree tree");
        const fs::path folder = _show / "Medios Fase 11";
        const fs::path song = _root / "montaje.wav";
        const fs::path lrc = _root / "pantallas.lrc";
        Check(WriteText(lrc, "[00:09.00]Nieve en la ventana\n[00:11.50]\n[00:33.00]Estrellas de invierno\n[00:35.50]\n"), "lyrics with pauses between their phrases");
        const std::string profile = (fs::path(DefaultTemplateDirectory()).parent_path() / "profiles" / "reference-winter-cinematic.json").string();
        const std::vector<std::pair<int, SectionKind>> layout = { { 0, SectionKind::Intro }, { 8000, SectionKind::Verse }, { 20000, SectionKind::Chorus }, { 32000, SectionKind::Verse },
                                                                  { 44000, SectionKind::Chorus }, { 56000, SectionKind::Bridge }, { 64000, SectionKind::Instrumental }, { 76000, SectionKind::Chorus },
                                                                  { 88000, SectionKind::Outro } };
        int n = 0;
        const auto request = [&](const std::string& name, const ScreenSettings& screens) {
            GenerationRequest r;
            r.audioPath = song.string();
            r.showDir = show.string();
            r.sequenceName = name + ".xsq";
            r.reportDir = (_root / "reports" / name).string();
            r.level = AutomationLevel::Balanced;
            r.seed = 7;
            r.lyricsPath = lrc.string();
            r.profilePath = profile;
            r.mediaFolders = { folder.string() };
            r.scanMedia = n++ == 0;
            for (size_t i = 0; i < layout.size(); ++i) {
                SectionCorrection c;
                c.startMS = layout[i].first;
                c.endMS = i + 1 < layout.size() ? layout[i + 1].first : 96000;
                c.kind = layout[i].second;
                r.corrections.analysis.sections.push_back(c);
            }
            r.corrections.screens = screens;
            return r;
        };
        const auto lane = [](const VisualMediaStoryboard& b, const std::string& target) {
            const auto it = std::find_if(b.lanes.begin(), b.lanes.end(), [&](const StoryboardLane& l) { return l.target == target; });
            return it != b.lanes.end() ? &*it : nullptr;
        };
        const auto shotsOn = [](const VisualMediaStoryboard& b, const std::string& target) {
            return static_cast<int>(std::count_if(b.shots.begin(), b.shots.end(), [&](const StoryboardShot& s) { return s.lane == target; }));
        };
        // Words and pictures never share a moment on a figure.
        const auto apart = [&](const GenerationRun& run) {
            const SequenceSpec read = Reopen(run);
            for (const EffectSpec& e : read.effects) {
                if (e.effect != "Pictures" && e.effect != "Video") {
                    continue;
                }
                for (const EffectSpec& t : read.effects) {
                    if (t.effect == "Text" && FullName(t) == FullName(e) && t.startMS < e.endMS && t.endMS > e.startMS) {
                        return false;
                    }
                }
            }
            return true;
        };
        const auto lit = [&](const GenerationRun& run) {
            const SequenceSpec read = Reopen(run);
            int all = 0, ok = 0;
            for (const EffectSpec& e : read.effects) {
                if (e.effect == "Pictures" || e.effect == "Video") {
                    ++all;
                    const RenderMetric* m = Metric(run, e);
                    ok += m != nullptr && m->framesWithLight > 0 ? 1 : 0;
                }
            }
            return std::make_pair(ok, all);
        };

        ScreenSettings smart;
        smart.set = true;
        smart.mode = LyricsMode::Smart;
        const GenerationRun a = Generate(request("pantallas-inteligente", smart));
        Check(a.ok, "published with the words shared smartly");
        if (!a.ok) {
            return;
        }
        printf("%s", SurfaceReport(a.media.surfaces).c_str());
        const Surface* lyrics = a.media.surfaces.Find("Matriz Prueba");
        const Surface* main = nullptr;
        for (const Surface& s : a.media.surfaces.surfaces) {
            main = s.role == SurfaceRole::Main ? &s : main;
        }
        const Surface* shared = a.media.surfaces.Find("Matriz Compartida");
        const Surface* loose = a.media.surfaces.Find("Matriz Sin Controlador");
        Check(a.media.surfaces.surfaces.size() >= 6, fmt::format("{} surfaces found from the models' properties, not their names", a.media.surfaces.surfaces.size()));
        Check(lyrics != nullptr && lyrics->role == SurfaceRole::Lyrics && main != nullptr && main->target == "Matriz Dos", fmt::format("the matrix with words keeps them; the main screen is {}", main ? main->target : "none"));
        Check(shared != nullptr && !shared->conflicts.empty() && shared->role != SurfaceRole::Main, "a matrix sharing another's channels is seen and never the main screen");
        Check(loose != nullptr && loose->noController && loose->role != SurfaceRole::Main, "a matrix with no controller is seen as such, and is not the main screen");
        // The best screen loses its controller: for the installation the program no
        // longer gives it pictures; for a preview it may; a person may still choose it.
        {
            ModelCapabilities caps = a.capabilities;
            for (TargetInfo& t : caps.targets) {
                if (t.name == "Matriz Dos") {
                    t.noController = true;
                    t.controller = "No Controller";
                }
            }
            ScreenSettings install;
            install.set = true;
            const SurfacePlan forReal = PlanSurfaces(caps, &a.plan, install);
            const Surface* dos = forReal.Find("Matriz Dos");
            Check(dos != nullptr && dos->role == SurfaceRole::Support && dos->availability == "preview" && std::find(dos->reasons.begin(), dos->reasons.end(), "support:no-controller") != dos->reasons.end(),
                  fmt::format("for the installation, a screen with no controller gets no pictures by itself ({}, {})", dos != nullptr ? SurfaceRoleId(dos->role) : "-", dos != nullptr ? dos->availability : "-"));
            bool wiredOnly = true;
            for (const Surface& s : forReal.surfaces) {
                wiredOnly = wiredOnly && (s.roleSource != "auto" || s.role == SurfaceRole::Support || s.role == SurfaceRole::Off || s.role == SurfaceRole::Lyrics || !s.noController);
            }
            Check(wiredOnly, "every screen the program chose for pictures is wired");
            ScreenSettings preview = install;
            preview.preview = true;
            const SurfacePlan forPreview = PlanSurfaces(caps, &a.plan, preview);
            const Surface* p = forPreview.Find("Matriz Dos");
            Check(p != nullptr && p->role == SurfaceRole::Main && p->availability == "preview", "for a preview on the computer it may be the main screen again");
            ScreenSettings chosen = install;
            ScreenChoice main2;
            main2.target = "Matriz Dos";
            main2.role = SurfaceRole::Main;
            main2.roleSet = true;
            chosen.screens = { main2 };
            const SurfacePlan byPerson = PlanSurfaces(caps, &a.plan, chosen);
            const Surface* c = byPerson.Find("Matriz Dos");
            Check(c != nullptr && c->role == SurfaceRole::Main && c->roleSource == "person" &&
                      std::find(byPerson.warnings.begin(), byPerson.warnings.end(), "surface-no-controller:Matriz Dos") != byPerson.warnings.end(),
                  "a person may still choose it, and is warned it will only be seen on the computer");
            std::string error;
            ScreenSettings back;
            Check(ScreenSettingsFrom(ScreenSettingsJson(preview), back, error) && back.preview && ScreenSettingsFrom(ScreenSettingsJson(install), back, error) && !back.preview, "preview or installation is kept with the show");
        }
        const VisualMediaStoryboard& b = a.media.storyboard;
        const StoryboardLane* gl = lane(b, "Matriz Prueba");
        Check(gl != nullptr && gl->gapsOnly && lane(b, "Matriz Dos") != nullptr && lane(b, "Arbol Grande") != nullptr, fmt::format("{} screens in the montage: the main matrix, the tree and the matrix with words between its phrases", b.lanes.size()));
        const int between = static_cast<int>(std::count_if(b.shots.begin(), b.shots.end(), [](const StoryboardShot& s) { return s.overlay; }));
        Check(between > 0, fmt::format("{} pictures between the phrases of the matrix with words", between));
        Check(apart(a), "and never over a phrase");
        // Continuity: the calm breathes, the strong music cuts, and nothing changes for nothing.
        {
            double calmMS = 0, strongMS = 0;
            int calm = 0, strong = 0, shortShots = 0, badCuts = 0, badFades = 0;
            for (const StoryboardShot& s : b.shots) {
                const StoryboardScene* sc = b.Scene(s.sceneId);
                if (sc == nullptr) {
                    continue;
                }
                const bool loud = sc->energy >= 0.65 || sc->role == "climax";
                const bool quiet = sc->energy < 0.45 && sc->role != "climax";
                (loud ? strongMS : calmMS) += loud || quiet ? s.endMS - s.startMS : 0;
                strong += loud ? 1 : 0;
                calm += quiet ? 1 : 0;
                shortShots += s.endMS - s.startMS < 2000 ? 1 : 0;
                badCuts += s.transition == "cut" && sc->energy < 0.65 ? 1 : 0;
                badFades += s.transition == "crossfade" && sc->energy >= 0.6 && sc->role == "climax" ? 1 : 0;
            }
            const double calmMean = calm > 0 ? calmMS / calm : 0, strongMean = strong > 0 ? strongMS / strong : 0;
            Check(calm == 0 || strong == 0 || calmMean > strongMean, fmt::format("calm scenes keep a shot longer ({:.1f} s) than strong ones ({:.1f} s)", calmMean / 1000, strongMean / 1000));
            Check(shortShots == 0, fmt::format("{} shots too short to be seen", shortShots));
            Check(badCuts == 0 && badFades == 0, fmt::format("a cut on the beat only in strong music ({} elsewhere), no cross-fade into the climax ({})", badCuts, badFades));
            Check(b.stats.coherence >= 0 && b.stats.coherence <= 1 && b.stats.crossScreen >= 0 && b.stats.crossScreen <= 1 && !b.stats.transitions.empty(),
                  fmt::format("how the shots follow each other ({:.2f}) and sit together on the screens ({:.2f}) is measured", b.stats.coherence, b.stats.crossScreen));
        }
        const auto [okA, allA] = lit(a);
        Check(allA > 0 && okA == allA, fmt::format("each of the {} picture and video effects lights its figure", allA));

        ScreenSettings first;
        first.set = true;
        first.mode = LyricsMode::LyricsFirst;
        const GenerationRun c = Generate(request("pantallas-letra-primero", first));
        Check(c.ok && shotsOn(c.media.storyboard, "Matriz Prueba") == 0 && lane(c.media.storyboard, "Matriz Prueba") == nullptr, "words first: the matrix with words shows only words");

        ScreenSettings free;
        free.set = true;
        free.mode = LyricsMode::Free;
        ScreenChoice m1;
        m1.target = "Matriz Prueba";
        m1.role = SurfaceRole::Main;
        m1.roleSet = true;
        ScreenChoice off;
        off.target = "Matriz Dos";
        off.role = SurfaceRole::Off;
        off.roleSet = true;
        free.screens = { m1, off };
        const GenerationRun d = Generate(request("pantallas-libre", free));
        const Surface* chosen = d.media.surfaces.Find("Matriz Prueba");
        Check(d.ok && chosen != nullptr && chosen->role == SurfaceRole::Main && chosen->roleSource == "person" && shotsOn(d.media.storyboard, "Matriz Dos") == 0 && shotsOn(d.media.storyboard, "Matriz Prueba") > 0,
              "free: the person's roles stand, a screen turned off shows nothing");
        Check(d.ok && apart(d), "and still no picture over a phrase");

        ScreenSettings half;
        half.set = true;
        ScreenChoice t180;
        t180.target = "Arbol Medio";
        t180.role = SurfaceRole::Tree;
        t180.roleSet = true;
        ScreenChoice bigOff;
        bigOff.target = "Arbol Grande";
        bigOff.role = SurfaceRole::Off;
        bigOff.roleSet = true;
        half.screens = { t180, bigOff };
        const GenerationRun e = Generate(request("pantallas-arbol-180", half));
        const StoryboardLane* l180 = e.ok ? lane(e.media.storyboard, "Arbol Medio") : nullptr;
        bool flat = true;
        for (const StoryboardShot& s : e.media.storyboard.shots) {
            if (s.lane == "Arbol Medio") {
                flat = flat && s.framing.rfind("tree-", 0) != 0 && s.pieces == 1 && s.framing != "video-faces";
            }
        }
        Check(l180 != nullptr && l180->degrees >= 179 && l180->degrees <= 181 && shotsOn(e.media.storyboard, "Arbol Medio") > 0 && flat,
              fmt::format("a 180 degree tree: {} shots framed for its one face, never wrapped round", shotsOn(e.media.storyboard, "Arbol Medio")));
        const auto [okE, allE] = lit(e);
        Check(allE > 0 && okE == allE, "and they light it");

        ScreenSettings turned;
        turned.set = true;
        ScreenChoice front;
        front.target = "Arbol Grande";
        front.frontOffset = 0.5;
        front.turn = -1;
        turned.screens = { front };
        GenerationRequest turnedRequest = request("pantallas-arbol-girado", turned);
        // A video a person puts on the tree, so the faces are seen whatever the montage picks.
        const MediaItem* clip = nullptr;
        for (const MediaItem& m : a.mediaLibrary.items) {
            clip = clip == nullptr && m.name == "poco movimiento.mp4" ? &m : clip;
        }
        for (const StoryboardScene& s : a.media.storyboard.scenes) {
            if (clip != nullptr && s.startMS == 20000) {
                MediaCorrection put;
                put.action = MediaCorrection::Action::Assign;
                put.segmentId = s.id;
                put.startMS = s.startMS;
                put.target = "Arbol Grande";
                put.mediaId = clip->id;
                put.path = clip->relative.empty() ? clip->path : clip->relative;
                UpsertMediaCorrection(turnedRequest.corrections.media, put);
            }
        }
        Check(!turnedRequest.corrections.media.empty(), "a video put on the tree by hand");
        const GenerationRun f = Generate(turnedRequest);
        int three = 0;
        bool synced = true;
        const SequenceSpec read = Reopen(f);
        std::map<int, std::vector<const EffectSpec*>> byStart;
        for (const EffectSpec& x : read.effects) {
            if (FullName(x) == "Arbol Grande" && x.effect == "Video" && !Setting(x, "B_CUSTOM_SubBuffer").empty()) {
                byStart[x.startMS].push_back(&x);
            }
        }
        for (const auto& [start, pieces] : byStart) {
            three += pieces.size() == 3 ? 1 : 0;
            for (const EffectSpec* x : pieces) {
                synced = synced && x->endMS == pieces.front()->endMS && Setting(*x, "E_TEXTCTRL_Video_Starttime") == Setting(*pieces.front(), "E_TEXTCTRL_Video_Starttime") &&
                         Setting(*x, "E_FILEPICKERCTRL_Video_Filename") == Setting(*pieces.front(), "E_FILEPICKERCTRL_Video_Filename");
            }
        }
        Check(f.ok && !byStart.empty() && synced, fmt::format("a video on both faces of the tree: {} shots drawn by pieces that start, end and read the video together", byStart.size()));
        Check(three > 0, "with the front turned half way round, a face crosses the tree's seam and takes two pieces (three in all)");
        bool rolled = false;
        for (const StoryboardShot& s : f.media.storyboard.shots) {
            for (const StoryboardShot& o : a.media.storyboard.shots) {
                rolled = rolled || (s.lane == "Arbol Grande" && o.lane == s.lane && s.mediaId == o.mediaId && s.framing == o.framing && s.effect == "Pictures" && s.path != o.path);
            }
        }
        Check(rolled || std::none_of(f.media.storyboard.shots.begin(), f.media.storyboard.shots.end(), [](const StoryboardShot& s) { return s.lane == "Arbol Grande" && s.effect == "Pictures"; }),
              "a picture on the tree is prepared turned to the new front");
        const auto [okF, allF] = lit(f);
        Check(allF > 0 && okF == allF, "everything lights its figure");
    }

    void TestMedia() {
        Title("A2d. Pictures and videos from the show's own files, with two voices, a choir, a narrator and the words");
        const fs::path media = _show / "Medios";
        const fs::path outside = _root / "medios fuera";
        std::string error;
        Check(WriteMediaFixtures(media.string(), error), "original pictures, a GIF, an SVG and a video, made by the test, in the show " + error);
        std::error_code ec;
        fs::create_directories(outside, ec);
        fs::copy_file(media / "panorama luces.png", outside / "luces de fuera.png", fs::copy_options::overwrite_existing, ec);
        // An animation is never framed through a derived picture: it is used as it is.
        fs::copy_file(media / "bucle fuego.gif", outside / "fuego de fuera.gif", fs::copy_options::overwrite_existing, ec);
        const std::string text = "[Narrador]\nHabía una vez una casa encendida\n\n[Voz: Ana]\n¿Dónde está el niño?\n\n[Voz: Luis]\nPingüino en la nieve\n\n[Voz: Coro]\n¡Año nuevo, señor!\n";
        const LyricDocument doc = ParsePlainLyrics(text);
        LyricSession session;
        session.trackName = "Composer Lyrics";
        session.lyrics = text;
        const std::vector<std::pair<int, int>> times = { { 2000, 6000 }, { 9000, 13000 }, { 13500, 17000 }, { 25000, 29000 } };
        size_t k = 0;
        for (const LyricLine* line : doc.Lines()) {
            LyricCorrection mark;
            mark.targetId = line->id;
            mark.startMS = times[k].first;
            mark.endMS = times[k].second;
            session.corrections.push_back(mark);
            ++k;
        }
        const fs::path sessionPath = _root / "medios-session.json";
        Check(SaveLyricSession(sessionPath.string(), session, error), "the lyrics, marked " + error);
        const std::string profile = (fs::path(DefaultTemplateDirectory()).parent_path() / "profiles" / "reference-winter-cinematic.json").string();
        GenerationRequest r = Request("medios");
        r.lyricSessionPath = sessionPath.string();
        r.profilePath = profile;
        r.mediaFolders = { media.string(), outside.string() };
        r.scanMedia = true;
        const GenerationRun run = Generate(r);
        Check(run.ok, "published with the media library searched");
        if (!run.ok) {
            return;
        }
        Check(FileExists(MediaLibraryPath(_show.string())) && run.mediaLibrary.items.size() == 9, fmt::format("the library has the {} files and is kept in the show", run.mediaLibrary.items.size()));
        Check(!run.media.assignments.empty(), fmt::format("{} pictures or videos chosen by themselves, theme {}", run.media.assignments.size(), run.media.themeId));
        Check(run.media.share <= run.media.maxShare + 0.001, fmt::format("{:.0f}% of the song, within {:.0f}%", run.media.share * 100, run.media.maxShare * 100));
        std::map<std::string, std::set<std::string>> looks;
        bool explained = true;
        bool onScreen = true;
        for (const MediaAssignment& a : run.media.assignments) {
            const SceneLook* scene = run.direction.Scene(a.segmentId);
            looks[a.mediaId].insert(scene != nullptr ? scene->lookKey : a.segmentId);
            explained = explained && HasReason(a, "aspect") && (HasReason(a, "theme-category") || HasReason(a, "repeat-keeps-media"));
            const TargetInfo* t = run.capabilities.Find(a.target);
            // A mega tree's support stays "unknown" for the planner; the storyboard frames
            // pictures for it (Phase 11).
            const RoleAssignment* role = t != nullptr ? t->PrimaryRole() : nullptr;
            onScreen = onScreen && t != nullptr &&
                       (t->Can(Capability::Image).support == Support::Yes || (role != nullptr && role->role == Role::MegaTree && t->Can(Capability::Image).support == Support::Unknown));
        }
        Check(explained && onScreen, "each says why: its theme category, its shape against the figure's; each on a figure that shows pictures");
        Check(std::all_of(looks.begin(), looks.end(), [](const auto& kv) { return kv.second.size() == 1; }), "no file shown in two different scenes");
        const SequenceSpec read = Reopen(run);
        int files = 0;
        bool relative = true;
        bool apart = true;
        bool lit = true;
        for (const EffectSpec& e : read.effects) {
            if (e.effect != "Pictures" && e.effect != "Video") {
                continue;
            }
            ++files;
            const std::string file = Setting(e, e.effect == "Video" ? "E_FILEPICKERCTRL_Video_Filename" : "E_TEXTCTRL_Pictures_Filename");
            relative = relative && !fs::path(fs::u8path(file)).is_absolute() && FileExists(MediaAbsolutePath(file, _show.string()));
            for (const EffectSpec& t : read.effects) {
                apart = apart && !(t.effect == "Text" && FullName(t) == FullName(e) && t.startMS < e.endMS && t.endMS > e.startMS);
            }
            const RenderMetric* m = Metric(run, e);
            lit = lit && m != nullptr && m->framesWithLight > 0;
        }
        Check(files > 0 && relative, fmt::format("{} native picture and video effects, each pointing at its file by a path relative to the show", files));
        Check(apart, "never words and a picture on the same figure at once");
        Check(lit, "each one lights its figure in the render");
        Check(Count(read, "Faces") > 0 && Count(read, "Text") > 0 && Track(read, "Composer Lyrics - Ana") != nullptr, "with faces on their voices' tracks and the words on the display");
        GenerationSidecar sidecar;
        Check(ReadSidecar(run.sidecarPath, sidecar, error) == SidecarStatus::Ok && sidecar.media.folders.size() == 2 && sidecar.media.folders[0] == "Medios" && sidecar.media.used.size() == run.media.assignments.size(),
              "the sidecar keeps the folders (relative inside the show) and what plays where");

        Title("A2e. A person's choices: no picture here, this video there, and it stays");
        const MediaAssignment removed = run.media.assignments.front();
        const MediaItem* video = nullptr;
        for (const MediaItem& m : run.mediaLibrary.items) {
            video = m.kind == MediaKind::Video && m.Usable() ? &m : video;
        }
        const PlanSegment* free = nullptr;
        for (const PlanSegment& seg : run.plan.segments) {
            const bool hasMedia = std::any_of(run.media.assignments.begin(), run.media.assignments.end(), [&](const MediaAssignment& a) { return a.segmentId == seg.id; });
            const bool words = std::any_of(seg.assignments.begin(), seg.assignments.end(), [](const PlanAssignment& a) { return a.intent == "lyric-text" && a.target == "Matriz Prueba"; });
            if (free == nullptr && !hasMedia && !words && seg.priority > 0 && seg.endMS - seg.startMS >= 4000) {
                free = &seg;
            }
        }
        Check(video != nullptr && free != nullptr, "a video in the library, and a segment with nothing on the matrix to put it in");
        if (video == nullptr || free == nullptr) {
            return;
        }
        GenerationRequest again = Request("medios-elegidos");
        again.previousSidecarPath = run.sidecarPath;
        MediaCorrection none;
        none.action = MediaCorrection::Action::Remove;
        none.segmentId = removed.segmentId;
        none.startMS = removed.startMS;
        MediaCorrection lock;
        lock.action = MediaCorrection::Action::Lock;
        lock.segmentId = free->id;
        lock.startMS = free->startMS;
        lock.target = "Matriz Prueba";
        lock.mediaId = video->id;
        lock.path = video->relative;
        // The automatic picture a person liked, kept as it is.
        MediaCorrection keep;
        keep.action = MediaCorrection::Action::Lock;
        for (const MediaAssignment& a : run.media.assignments) {
            if (a.effect == "Pictures" && a.segmentId != removed.segmentId) {
                keep.segmentId = a.segmentId;
                keep.startMS = a.startMS;
                keep.target = a.target;
                keep.mediaId = a.mediaId;
                keep.path = a.path;
            }
        }
        again.corrections.media = { none, lock };
        if (!keep.segmentId.empty()) {
            again.corrections.media.push_back(keep);
        }
        const GenerationRun second = Generate(again);
        const MediaAssignment* locked = second.media.Find(free->id, "Matriz Prueba");
        Check(second.ok && locked != nullptr && locked->mediaId == video->id && locked->state == DecisionState::Locked, "the video plays where the person put it, locked");
        Check(std::none_of(second.media.assignments.begin(), second.media.assignments.end(), [&](const MediaAssignment& a) { return a.segmentId == removed.segmentId; }), "and nothing plays where they took it out");
        const SequenceSpec chosen = Reopen(second);
        bool videoMoves = false;
        for (const EffectSpec& e : chosen.effects) {
            const RenderMetric* m = e.effect == "Video" ? Metric(second, e) : nullptr;
            videoMoves = videoMoves || (m != nullptr && m->distinctFrames > 1);
        }
        Check(Count(chosen, "Video") > 0 && Count(chosen, "Pictures") + Count(chosen, "Video") >= 2 && Count(chosen, "Faces") > 0 && Count(chosen, "Text") > 0 && videoMoves,
              fmt::format("two voices, a choir, a narrator, words, {} pictures and {} videos, the video moving in the render", Count(chosen, "Pictures"), Count(chosen, "Video")));
        GenerationRequest third = Request("medios-otra-vez");
        third.previousSidecarPath = second.sidecarPath;
        const GenerationRun again3 = Generate(third);
        const MediaAssignment* kept = again3.media.Find(free->id, "Matriz Prueba");
        Check(again3.ok && kept != nullptr && kept->mediaId == video->id && kept->state == DecisionState::Locked && again3.media.Find(removed.segmentId, removed.target) == nullptr,
              "made again from the sidecar alone, offline: the same choices");

        Title("A2f. A file outside the show: used where it is, copied only when the person agrees");
        // A still picture from outside is framed through a derived picture kept in the
        // show, so it travels with it without copying the original.
        const MediaItem* farPicture = run.mediaLibrary.FindByPath((outside / "luces de fuera.png").string());
        bool derivedInside = true;
        int farUses = 0;
        for (const GenerationRun* g : { &run, &second }) {
            for (const MediaAssignment& a : g->media.assignments) {
                if (farPicture != nullptr && a.mediaId == farPicture->id) {
                    ++farUses;
                    derivedInside = derivedInside && (a.path.rfind("ShowComposer/media-derived/", 0) == 0 ? FileExists(MediaAbsolutePath(a.path, _show.string())) : a.path == farPicture->path);
                }
            }
        }
        Check(derivedInside && !FileExists((_show / "Images" / "luces de fuera.png").string()),
              fmt::format("an outside picture ({} uses) is shown through a derived picture inside the show, or from where it is; the original is not copied", farUses));
        const MediaItem* far = run.mediaLibrary.FindByPath((outside / "fuego de fuera.gif").string());
        const PlanSegment* other = nullptr;
        for (const PlanSegment& seg : run.plan.segments) {
            const bool used = std::any_of(second.media.assignments.begin(), second.media.assignments.end(), [&](const MediaAssignment& a) { return a.segmentId == seg.id; });
            if (other == nullptr && !used && &seg != free && seg.id != removed.segmentId && seg.priority > 0 && seg.endMS - seg.startMS >= 4000) {
                other = &seg;
            }
        }
        Check(far != nullptr && far->relative.empty() && other != nullptr, "the outside file is in the library, with no path relative to the show");
        if (far == nullptr || other == nullptr) {
            return;
        }
        MediaCorrection put;
        put.action = MediaCorrection::Action::Assign;
        put.segmentId = other->id;
        put.startMS = other->startMS;
        put.target = "Matriz Prueba";
        put.mediaId = far->id;
        put.path = far->path;
        GenerationRequest asked = Request("medios-fuera");
        asked.previousSidecarPath = second.sidecarPath;
        asked.corrections.media = { put };
        const GenerationRun noCopy = Generate(asked);
        const MediaAssignment* where = noCopy.media.Find(other->id, "Matriz Prueba");
        Check(noCopy.ok && where != nullptr && where->path == far->path && HasNote(noCopy, GenerationStage::Choreography, "media-outside-show") && !FileExists((_show / "Images" / "fuego de fuera.gif").string()),
              "without consent: used from where it is, said so, nothing copied");
        asked.sequenceName = "medios-copiados.xsq";
        asked.reportDir = (_root / "reports" / "medios-copiados").string();
        asked.copyMediaIntoShow = true;
        const GenerationRun copied = Generate(asked);
        const MediaAssignment* inShow = copied.media.Find(other->id, "Matriz Prueba");
        Check(copied.ok && inShow != nullptr && inShow->path == "Images/fuego de fuera.gif" && FileExists((_show / "Images" / "fuego de fuera.gif").string()) && FileExists(far->path),
              "with consent: copied into the show's Images folder, the original left where it was");

        Title("A2g. A file that went missing");
        const fs::path moved = _root / "video apartado.mp4";
        fs::rename(fs::u8path(video->path), moved, ec);
        GenerationRequest gone = Request("medios-sin-video");
        gone.previousSidecarPath = second.sidecarPath;
        const GenerationRun lost = Generate(gone);
        const SequenceSpec lostRead = Reopen(lost);
        // No effect reads the missing file (motion clips made from pictures are Video effects too).
        bool noneMissing = true;
        for (const EffectSpec& e : lostRead.effects) {
            if (e.effect == "Video" || e.effect == "Pictures") {
                const std::string file = Setting(e, e.effect == "Video" ? "E_FILEPICKERCTRL_Video_Filename" : "E_TEXTCTRL_Pictures_Filename");
                noneMissing = noneMissing && FileExists(MediaAbsolutePath(file, _show.string())) && file.find("video estrellas") == std::string::npos;
            }
        }
        Check(lost.ok && noneMissing && std::any_of(lost.media.warnings.begin(), lost.media.warnings.end(), [](const std::string& w) { return w.rfind("media-missing:", 0) == 0; }),
              "the sequence is still made, with no effect pointing at a missing file, and it says which");
        GenerationSidecar keptChoice;
        Check(ReadSidecar(lost.sidecarPath, keptChoice, error) == SidecarStatus::Ok &&
                  std::any_of(keptChoice.media.corrections.begin(), keptChoice.media.corrections.end(), [&](const MediaCorrection& c) { return c.mediaId == video->id && c.action == MediaCorrection::Action::Lock; }),
              "the person's choice is kept, to play again once the file is back");
        fs::rename(moved, fs::u8path(video->path), ec);
    }

    void TestLrcFiles() {
        Title("A2b. LRC files: repaired when it is safe, refused when a time is wrong");
        // 20.037 s: the song does not end on a frame, and its last line is sung up to the end.
        const fs::path odd = _root / "cancion-impar.wav";
        testaudio::SyntheticSong song(20.037);
        for (double t = 0; t < 20; t += 0.5) {
            song.Kick(t, 0.4);
        }
        song.Tone(2, 17, 220, 0.1);
        const fs::path lastLine = _root / "ultima-linea.lrc";
        (void)WriteText(lastLine, "\xEF\xBB\xBF[ti:Prueba]\r\n[00:02.00]¿Dónde está el niño?\r\n[00:18.50]Una musiquilla que suena tal que así\r\n");
        if (!song.Save(odd.string())) {
            Check(false, "the odd-length song could not be written");
            return;
        }
        GenerationRequest r = Request("lrc-final");
        r.audioPath = odd.string();
        r.lyricsPath = lastLine.string();
        const GenerationRun ending = Generate(r, GenerationStage::Lyrics);
        const StageStatus status = ending.Stage(GenerationStage::Lyrics).status;
        int lastEnd = 0;
        for (const auto& layer : ending.lyricTrack.layers) {
            for (const TimingMark& m : layer) {
                lastEnd = std::max(lastEnd, m.endMS);
            }
        }
        Check(status != StageStatus::Failed && ending.lyricTrack.layers.size() == 3 && lastEnd <= ending.analysis.durationMS && lastEnd % r.frameMS == 0,
              fmt::format("a last line near the end of a {} ms song: synchronized up to {} ms, on a frame", ending.analysis.durationMS, lastEnd));

        // A line sung twice in a row (as in many carols) is two phrases on the matrix, not
        // one long effect the validator would refuse.
        const fs::path twice = _root / "dos-veces.lrc";
        (void)WriteText(twice, "[00:02.00]Nieve en el tejado, nieve en el tejado\n[00:06.00]Nieve en el tejado, nieve en el tejado\n[00:10.00]\n");
        GenerationRequest rt = Request("lrc-dos-veces");
        rt.lyricsPath = twice.string();
        const GenerationRun repeated = Generate(rt);
        std::map<std::string, int> shown;
        for (const ChoreographyEvent& e : repeated.choreography.events) {
            for (const auto& [k, v] : e.settings) {
                if (e.effect == "Text" && k == "E_TEXTCTRL_Text" && v == "Nieve en el tejado, nieve en el tejado") {
                    ++shown[e.target];
                }
            }
        }
        Check(repeated.ok && !shown.empty() && std::all_of(shown.begin(), shown.end(), [](const auto& kv) { return kv.second == 2; }),
              fmt::format("a line sung twice in a row is published as two phrases ({} figures show it)", shown.size()));

        _expectSuccess = false;
        const auto refused = [&](const char* name, const std::string& lrc, const char* note) {
            const fs::path path = _root / (std::string(name) + ".lrc");
            (void)WriteText(path, lrc);
            GenerationRequest bad = Request(name);
            bad.lyricsPath = path.string();
            const GenerationRun run = Generate(bad);
            const bool said = std::any_of(run.Stage(GenerationStage::Lyrics).notes.begin(), run.Stage(GenerationStage::Lyrics).notes.end(), [&](const std::string& n) { return n.find(note) != std::string::npos; });
            Check(!run.ok && run.Stage(GenerationStage::Lyrics).status == StageStatus::Failed && !FileExists(run.sequencePath) && said, fmt::format("{}: refused before writing, saying {}", name, note));
        };
        refused("lrc-marca-rota", "[00:09.00]¿Dónde está el niño?\n[00:1x.00]Pingüino en la nieve\n", "malformed-time line 2");
        refused("lrc-negativa", "[-00:01.00]¿Dónde está el niño?\n[00:13.00]Pingüino en la nieve\n", "negative-time line 1");
        refused("lrc-fuera", "[00:09.00]¿Dónde está el niño?\n[01:30.00]Pingüino en la nieve\n", "time-past-song-end at 90000 ms");
        refused("lrc-vacio", "", "empty-file");
        refused("lrc-sin-tiempos", "¿Dónde está el niño?\nPingüino en la nieve\n", "no-timestamps");
        _expectSuccess = true;

        const fs::path unordered = _root / "desordenada.lrc";
        (void)WriteText(unordered, "[00:13.00]Pingüino en la nieve\r\n[00:09.00]¿Dónde está el niño?\r\n[00:17.00]Cigüeña sobre el tejado\r\n");
        GenerationRequest fixed = Request("lrc-reparada");
        fixed.lyricsPath = unordered.string();
        const GenerationRun repaired = Generate(fixed);
        Check(repaired.ok && HasNote(repaired, GenerationStage::Lyrics, "lyrics-repaired") && repaired.lyrics.Lines().front()->startMS == 9000, "lines out of order: sorted, published, and the repair in the report");
    }

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
    r.themesPath = o.themesPath;
    r.characterPath = o.characterPath;
    r.transcriptPath = o.transcriptPath;
    r.render = o.render;
    r.renderTimeoutMS = o.renderTimeoutMS;
    for (size_t at = 0; at < o.mediaFolders.size();) {
        const size_t end = std::min(o.mediaFolders.find(';', at), o.mediaFolders.size());
        if (end > at) {
            r.mediaFolders.push_back(o.mediaFolders.substr(at, end - at));
        }
        at = end + 1;
    }
    r.scanMedia = o.scanMedia;
    if (o.mediaManualOnly) {
        r.mediaAutomatic = false;
    }
    if (!o.mediaShare.empty()) {
        r.mediaMaxShare = std::clamp(std::strtod(o.mediaShare.c_str(), nullptr), 0.0, 1.0);
    }
    r.copyMediaIntoShow = o.copyMedia;
    r.analyzeMedia = o.analyzeMedia;
    r.visionModelDir = o.visionModel;
    if (!o.mediaVariant.empty()) {
        r.mediaVariant = static_cast<int>(std::strtol(o.mediaVariant.c_str(), nullptr, 10));
    }
    r.learning = !o.noLearning;
    r.recipesPath = o.recipesPath;
    if (o.understanding == "on" || o.understanding == "off") {
        r.understanding = o.understanding == "on";
    }
    r.stems = ParseStemList(o.stems);
    if (o.noRecipes) {
        r.useRecipes = false;
    }
    if (o.noColorEngine) {
        r.colorEngine = false;
    }
    if (!o.christmas.empty()) {
        r.christmasInfluence = std::clamp(std::strtod(o.christmas.c_str(), nullptr) / 100.0, 0.0, 1.0);
    }
    if (!o.variety.empty()) {
        r.variety = std::clamp(std::strtod(o.variety.c_str(), nullptr) / 100.0, 0.0, 1.0);
    }
    r.briefPath = o.briefPath;
    if (!o.lyricsMode.empty() || !o.screens.empty() || !o.screensFor.empty()) {
        ScreenSettings ss;
        ss.set = true;
        if (!o.screensFor.empty() && o.screensFor != "installation" && o.screensFor != "preview") {
            printf("composer-generate: REFUSED: --screens-for is installation or preview\n");
            return 2;
        }
        ss.preview = o.screensFor == "preview";
        if (!o.lyricsMode.empty() && !LyricsModeFromId(o.lyricsMode, ss.mode)) {
            printf("composer-generate: REFUSED: unknown lyrics mode '%s' (lyrics-first, smart or free)\n", o.lyricsMode.c_str());
            return 2;
        }
        for (size_t at = 0; at < o.screens.size();) {
            const size_t end = std::min(o.screens.find(';', at), o.screens.size());
            const std::string item = o.screens.substr(at, end - at);
            const size_t eq = item.find('=');
            if (eq != std::string::npos) {
                ScreenChoice c;
                c.target = item.substr(0, eq);
                if (!SurfaceRoleFromId(item.substr(eq + 1), c.role)) {
                    printf("composer-generate: REFUSED: unknown screen role in '%s'\n", item.c_str());
                    return 2;
                }
                c.roleSet = true;
                ss.screens.push_back(c);
            }
            at = end + 1;
        }
        MergeScreenSettings(r.corrections.screens, ss);
    }
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
    std::map<std::string, std::vector<xlImage>> sheets; // one contact sheet per model, and one of the house
    const auto save = [&](const xlImage& image, const fs::path& out) {
        std::vector<uint8_t> png;
        return image.SaveAsPNG(png) && WriteText(out, std::string(png.begin(), png.end()));
    };
    const auto readFrame = [&](int ms) {
        const int frame = ms / std::max(1, file->getStepTime());
        std::fill(data.begin(), data.end(), 0);
        std::unique_ptr<FSEQFile::FrameData> frameData(file->getFrame(frame));
        return frameData != nullptr && frameData->readFrame(data.data(), channels);
    };
    if (std::find(o.models.begin(), o.models.end(), "@house") != o.models.end()) {
        // The whole display as the layout places it, every light at its place.
        struct Dot {
            Model* model;
            uint32_t node;
            float x, y;
        };
        std::vector<Dot> dots;
        float minX = 1e30f, minY = 1e30f, maxX = -1e30f, maxY = -1e30f;
        for (const auto& [name, model] : ctx.AllModels) {
            if (model == nullptr || dynamic_cast<ModelGroup*>(model) != nullptr) {
                continue;
            }
            for (uint32_t n = 0; n < model->GetNodeCount(); ++n) {
                const NodeBaseClass* node = model->GetNode(n);
                for (uint32_t c = 0; c < node->Coords.size(); ++c) {
                    float x = node->Coords[c].screenX, y = node->Coords[c].screenY, z = node->Coords[c].screenZ;
                    model->GetModelScreenLocation().TranslatePoint(x, y, z);
                    dots.push_back({ model, n, x, y });
                    minX = std::min(minX, x);
                    maxX = std::max(maxX, x);
                    minY = std::min(minY, y);
                    maxY = std::max(maxY, y);
                }
            }
        }
        const int W = 1200;
        const double span = std::max(1.0f, maxX - minX);
        const int H = std::clamp(static_cast<int>(W * std::max(1.0f, maxY - minY) / span), 200, 900);
        const double sx = (W - 20) / span, sy = (H - 20) / std::max(1.0, static_cast<double>(maxY - minY));
        const double k = std::min(sx, sy);
        for (int ms : o.timesMS) {
            if (!readFrame(ms)) {
                continue;
            }
            xlImage image(W, H);
            for (int y = 0; y < H; ++y) {
                for (int x = 0; x < W; ++x) {
                    image.SetRGBA(x, y, 12, 12, 16, 255);
                }
            }
            int lit = 0;
            for (const Dot& d : dots) {
                NodeBaseClass* node = d.model->GetNode(d.node);
                if (node->ActChan + node->GetChanCount() > channels) {
                    continue;
                }
                node->SetFromChannels(&data[node->ActChan]);
                xlColor color;
                node->GetColor(color);
                const bool on = color.red != 0 || color.green != 0 || color.blue != 0;
                lit += on ? 1 : 0;
                const int px = 10 + static_cast<int>((d.x - minX) * k);
                const int py = H - 10 - static_cast<int>((d.y - minY) * k);
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (px + dx >= 0 && px + dx < W && py + dy >= 0 && py + dy < H) {
                            image.SetRGBA(px + dx, py + dy, on ? color.red : 34, on ? color.green : 34, on ? color.blue : 34, 255);
                        }
                    }
                }
            }
            const fs::path out = fs::path(o.outDir) / fmt::format("house_{:06d}ms.png", ms);
            if (save(image, out)) {
                ++written;
                sheets["house"].push_back(image);
                printf("composer-snapshots: the house at %d ms: %d of %zu lights on -> %s\n", ms, lit, dots.size(), out.string().c_str());
            }
        }
    }
    for (const std::string& name : o.models) {
        if (name == "@house") {
            continue;
        }
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
            sheets[safe].push_back(image);
            printf("composer-snapshots: %s at %d ms (frame %d): %d of %u nodes lit, picture %016llx -> %s\n", name.c_str(), ms, frame, lit, model->GetNodeCount(), static_cast<unsigned long long>(digest), out.string().c_str());
        }
    }
    // Contact sheets: every time of one model, or of the house, side by side.
    for (const auto& [name, images] : sheets) {
        if (images.size() < 2) {
            continue;
        }
        const int cols = std::min<int>(4, static_cast<int>(images.size()));
        const int rows = (static_cast<int>(images.size()) + cols - 1) / cols;
        int cw = 0, ch = 0;
        for (const xlImage& i : images) {
            cw = std::max(cw, i.GetWidth());
            ch = std::max(ch, i.GetHeight());
        }
        const double shrink = std::min(1.0, 2400.0 / (cols * (cw + 8)));
        const int tw = std::max(1, static_cast<int>(cw * shrink)), th = std::max(1, static_cast<int>(ch * shrink));
        xlImage sheet(cols * (tw + 8) + 8, rows * (th + 8) + 8);
        for (int y = 0; y < sheet.GetHeight(); ++y) {
            for (int x = 0; x < sheet.GetWidth(); ++x) {
                sheet.SetRGBA(x, y, 70, 70, 70, 255);
            }
        }
        for (size_t k = 0; k < images.size(); ++k) {
            const xlImage thumb = ResampleImage(images[k], std::max(1, static_cast<int>(images[k].GetWidth() * shrink)), std::max(1, static_cast<int>(images[k].GetHeight() * shrink)));
            const int ox = 8 + static_cast<int>(k % cols) * (tw + 8), oy = 8 + static_cast<int>(k / cols) * (th + 8);
            for (int y = 0; y < thumb.GetHeight(); ++y) {
                for (int x = 0; x < thumb.GetWidth(); ++x) {
                    sheet.SetRGBA(ox + x, oy + y, thumb.GetRed(x, y), thumb.GetGreen(x, y), thumb.GetBlue(x, y), 255);
                }
            }
        }
        const fs::path out = fs::path(o.outDir) / fmt::format("contact-sheet_{}.png", name);
        if (save(sheet, out)) {
            printf("composer-snapshots: contact sheet of %s, %zu times -> %s\n", name.c_str(), images.size(), out.string().c_str());
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
