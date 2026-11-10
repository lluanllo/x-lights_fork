#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "composer/UnderstandingTest.h"

#include "composer/AudioUnderstanding.h"
#include "composer/FigureGeometry.h"
#include "composer/GenerationSidecar.h"
#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/PlanTest.h"
#include "composer/FigureSemantics.h"
#include "composer/ModelCapabilities.h"
#include "composer/MultimodalTimeline.h"
#include "composer/PerformanceMapping.h"
#include "composer/RenderUnderstanding.h"
#include "composer/SyntheticAudio.h"
#include "composer/UnderstandingStore.h"
#include "composer/VocalUnderstanding.h"
#include "composer/WriterTest.h"
#include "render/FSEQFile.h"
#include "render/HeadlessRenderContext.h"

#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <tuple>

namespace fs = std::filesystem;
using json = nlohmann::json;
using composer::testaudio::SyntheticSong;

namespace composer {

namespace {

// A short melody, sung or by a choir.
const std::vector<double> MELODY = { 220, 247, 262, 247, 220, 196, 220, 262 };

class Suite {
public:
    explicit Suite(const fs::path& root) : _root(root) {}

    int Run() {
        std::error_code ec;
        fs::create_directories(_root, ec);
        TestAcoustic();
        TestFiles();
        TestCorrections();
        TestStore();
        TestVoices();
        TestFigures();
        TestMapping();
        TestTimeline();
        TestRender();
        TestSidecar();
        if (_ctx != nullptr) {
            (void)_ctx->CloseSequence();
        }
        printf("composer-understanding-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
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

    static AudioUnderstanding Hear(const SyntheticSong& s) {
        return UnderstandSamples(s.Left(), s.Right(), testaudio::RATE);
    }

    // The share of [from, to) covered by activities of a kind.
    static double Covered(const AudioUnderstanding& u, const std::string& kind, int from, int to) {
        std::vector<char> on(static_cast<size_t>(std::max(0, to - from) / 10 + 1), 0);
        for (const ActivitySegment* s : u.Of(kind)) {
            for (int t = std::max(from, s->startMS); t < std::min(to, s->endMS); t += 10) {
                on[static_cast<size_t>((t - from) / 10)] = 1;
            }
        }
        return to > from ? static_cast<double>(std::count(on.begin(), on.end(), 1)) / static_cast<double>((to - from) / 10) : 0;
    }

    static int Count(const AudioUnderstanding& u, const std::string& kind) {
        int n = 0;
        for (const SoundEvent& e : u.events) {
            n += e.kind == kind ? 1 : 0;
        }
        return n + static_cast<int>(u.Of(kind).size());
    }

    static const SoundEvent* EventNear(const AudioUnderstanding& u, const std::string& kind, int atMS, int within) {
        const SoundEvent* best = nullptr;
        for (const SoundEvent& e : u.events) {
            if (e.kind == kind && std::abs(e.startMS - atMS) <= within && (best == nullptr || std::abs(e.startMS - atMS) < std::abs(best->startMS - atMS))) {
                best = &e;
            }
        }
        return best;
    }

    void Describe(const char* name, const AudioUnderstanding& u) {
        std::string line;
        for (const ActivitySegment& s : u.activities) {
            line += fmt::format(" {}[{}-{} {:.2f}]", s.kind, s.startMS, s.endMS, s.confidence);
        }
        printf("  · %s activities:%s\n", name, line.c_str());
        line.clear();
        for (const SoundEvent& e : u.events) {
            line += fmt::format(" {}@{}", e.kind, e.startMS);
        }
        printf("  · %s events:%s\n", name, line.c_str());
        if (std::getenv("XL_UNDERSTAND_DEBUG") != nullptr) {
            for (const SoundFrame& f : u.curve) {
                if (f.timeMS % 500 == 0) {
                    printf("    %5d f0 %4.0f voicing %.2f voice %.2f music %.2f harm %.2f perc %.2f\n", f.timeMS, f.f0, f.voicing, f.voice, f.music, f.harmonic, f.percussive);
                }
            }
        }
    }

    void TestAcoustic() {
        Title("What the recording says: synthetic sounds of known content");
        {
            SyntheticSong s(6);
            const AudioUnderstanding u = Hear(s);
            Describe("silence", u);
            Check(Covered(u, "silence", 0, 6000) >= 0.9, "silence: heard as silence");
            Check(u.Of("speech").empty() && u.Of("singing").empty() && u.Of("chorus").empty() && u.Of("music").empty(), "silence: no voice and no music invented");
        }
        {
            SyntheticSong s(10);
            s.Speech(1, 8, 130, 0.35);
            const AudioUnderstanding u = Hear(s);
            Describe("speech", u);
            Check(Covered(u, "speech", 1000, 9000) >= 0.6, fmt::format("speech: someone speaks ({:.0f}% of the narration)", Covered(u, "speech", 1000, 9000) * 100));
            Check(Covered(u, "singing", 1000, 9000) + Covered(u, "chorus", 1000, 9000) <= 0.15, "speech: not taken for singing");
            Check(Covered(u, "music", 0, 10000) <= 0.2, "speech: no music invented under it");
            Check(u.Of("a-cappella").empty(), "speech alone is speech, not a cappella singing");
        }
        {
            SyntheticSong s(10);
            s.Sing(1, 8, MELODY, 0.35);
            const AudioUnderstanding u = Hear(s);
            Describe("singing", u);
            Check(Covered(u, "singing", 1000, 9000) >= 0.6, fmt::format("singing: someone sings ({:.0f}%)", Covered(u, "singing", 1000, 9000) * 100));
            Check(Covered(u, "speech", 1000, 9000) <= 0.2, "singing: not taken for speech");
            Check(Covered(u, "a-cappella", 1000, 9000) >= 0.5, "a voice singing with nothing under it is a cappella");
        }
        {
            SyntheticSong s(10);
            s.Choir(1, 8, MELODY, 0.45);
            const AudioUnderstanding u = Hear(s);
            Describe("choir", u);
            Check(Covered(u, "chorus", 1000, 9000) >= 0.5, fmt::format("choir: the choir sings ({:.0f}%)", Covered(u, "chorus", 1000, 9000) * 100));
            Check(Covered(u, "chorus", 1000, 9000) > Covered(u, "singing", 1000, 9000), "choir: more choir than a single voice");
        }
        {
            SyntheticSong s(17);
            s.Beat(0, 17, 120, 0.4);
            s.Bass(0, 17, 55, 0.25);
            s.Pad(0, 17, 220, 0.12);
            s.Sing(0.5, 7.5, MELODY, 0.4);
            const AudioUnderstanding u = Hear(s);
            Describe("soloist then instrumental", u);
            Check(Covered(u, "singing", 500, 8000) + Covered(u, "chorus", 500, 8000) >= 0.5, fmt::format("soloist: sings over the band ({:.0f}%)", Covered(u, "singing", 500, 8000) * 100));
            Check(Covered(u, "instrumental", 9000, 16500) >= 0.6, fmt::format("then the instruments alone ({:.0f}%)", Covered(u, "instrumental", 9000, 16500) * 100));
            Check(Covered(u, "speech", 9000, 16500) + Covered(u, "singing", 9000, 16500) + Covered(u, "chorus", 9000, 16500) <= 0.15, "no voice invented in the instrumental");
            Check(u.Of("a-cappella").empty() || Covered(u, "a-cappella", 500, 8000) <= 0.2, "with a band under it, it is not a cappella");
        }
        {
            SyntheticSong s(14);
            s.Beat(0, 14, 100, 0.3);
            s.Bass(0, 14, 49, 0.2);
            for (int k = 0; k < 2; ++k) {
                s.Sing(0.5 + k * 6.6, 2.8, { 262, 294, 330, 294 }, 0.4);
                s.Choir(3.8 + k * 6.6, 2.8, { 196, 220, 247, 220 }, 0.5);
            }
            const AudioUnderstanding u = Hear(s);
            Describe("call and response", u);
            const double solo = (Covered(u, "singing", 500, 3300) + Covered(u, "singing", 7100, 9900)) / 2;
            const double choir = (Covered(u, "chorus", 3800, 6600) + Covered(u, "chorus", 10400, 13200)) / 2;
            Check(solo >= 0.4, fmt::format("call: the soloist sings ({:.0f}%)", solo * 100));
            Check(choir >= 0.4, fmt::format("response: the choir answers ({:.0f}%)", choir * 100));
        }
        {
            SyntheticSong s(12);
            s.Beat(0, 12, 110, 0.35);
            s.Bass(0, 12, 55, 0.2);
            s.Pad(0, 12, 196, 0.1);
            s.Speech(2, 8, 125, 0.4);
            const AudioUnderstanding u = Hear(s);
            Describe("voice over music", u);
            Check(Covered(u, "speech", 2000, 10000) >= 0.45, fmt::format("speech over the music ({:.0f}%)", Covered(u, "speech", 2000, 10000) * 100));
            Check(Covered(u, "music", 0, 12000) >= 0.7, "the music is heard all along");
            Check(Covered(u, "singing", 2000, 10000) <= 0.2, "the narrator is not taken for a singer");
        }
        {
            SyntheticSong s(8);
            s.Beat(0, 8, 120, 0.5);
            const AudioUnderstanding u = Hear(s);
            Describe("percussion", u);
            Check(Covered(u, "percussion", 500, 7500) >= 0.5, fmt::format("percussion: drums ({:.0f}%)", Covered(u, "percussion", 500, 7500) * 100));
            Check(u.Of("speech").empty() && u.Of("singing").empty() && u.Of("chorus").empty(), "percussion: no voice");
            Check(!u.events.empty() && Count(u, "attack") + Count(u, "accent") >= 8, "percussion: attacks and accents");
        }
        {
            SyntheticSong s(14);
            s.Pad(0, 14, 220, 0.6);
            s.Bass(0, 14, 55, 0.3);
            s.Ramp(0, 6, 0.04, 1.0);
            s.Ramp(8, 6, 1.0, 0.04);
            const AudioUnderstanding u = Hear(s);
            Describe("crescendo and diminuendo", u);
            Check(Count(u, "crescendo") + Count(u, "fade-in") >= 1, "the music grows");
            Check(Count(u, "diminuendo") + Count(u, "fade-out") >= 1, "the music fades");
        }
        {
            SyntheticSong s(12);
            s.Beat(0, 12, 120, 0.45);
            s.Bass(0, 12, 55, 0.3);
            s.Pad(0, 12, 220, 0.2);
            s.Scale(6, 2, 0.02);
            const AudioUnderstanding u = Hear(s);
            Describe("drop and re-entry", u);
            Check(EventNear(u, "drop", 6000, 600) != nullptr, "the music drops suddenly at 6 s");
            Check(EventNear(u, "reentry", 8000, 600) != nullptr, "and comes back at 8 s");
        }
        {
            SyntheticSong s(8);
            s.Kick(1, 0.9);
            s.Tone(1, 5, 110, 0.6, 0.002);
            s.Ramp(1.2, 4.8, 1.0, 0.0);
            const AudioUnderstanding u = Hear(s);
            Describe("hit and tail", u);
            Check(EventNear(u, "attack", 1000, 200) != nullptr || EventNear(u, "accent", 1000, 200) != nullptr, "the hit is an attack (or an accent)");
            Check(Count(u, "tail") + Count(u, "decay") >= 1, "the sound dies away: a tail");
            Check(Count(u, "resolution") >= 1, "the end of the piece rests: a resolution");
        }
        {
            SyntheticSong s(6);
            s.Hiss(0.5, 5, 0.3);
            const AudioUnderstanding u = Hear(s);
            Describe("noise", u);
            Check(Covered(u, "noise", 500, 5500) >= 0.5, "hiss is noise or ambience");
            Check(u.Of("speech").empty() && u.Of("singing").empty(), "hiss is no voice");
        }
        {
            SyntheticSong s(0.3);
            s.Tone(0, 0.3, 440, 0.3);
            const AudioUnderstanding u = Hear(s);
            Check(u.durationMS == 300 && u.Of("speech").empty() && u.Of("singing").empty(), "a very short sound is read without inventing a voice");
        }
    }

    // ------------------------------------------------------------------ files

    fs::path Wav(const std::string& name, const SyntheticSong& s) {
        const fs::path dir = _root / "audio";
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path p = dir / name;
        s.Save(p.string());
        return p;
    }

    static SyntheticSong NarrationOverMusic(bool voice, bool music) {
        SyntheticSong s(10);
        if (music) {
            s.Beat(0, 10, 110, 0.35);
            s.Bass(0, 10, 55, 0.2);
        }
        if (voice) {
            s.Speech(2, 6, 125, 0.4);
        }
        return s;
    }

    void TestFiles() {
        Title("Files, stems, broken audio, cancelling, the same answer every time");
        const fs::path mix = Wav("mix.wav", NarrationOverMusic(true, true));
        const fs::path voice = Wav("voice.wav", NarrationOverMusic(true, false));
        const fs::path music = Wav("music.wav", NarrationOverMusic(false, true));
        std::vector<int> progress;
        const UnderstandingResult plain = UnderstandAudio(mix.string(), {}, {}, [&](int p, const char*) { progress.push_back(p); });
        Check(plain.ok && !plain.understanding.audioHash.empty() && std::abs(plain.understanding.durationMS - 10000) <= 50, "a WAV file is read through xLights' own audio decoder");
        Check(!progress.empty() && progress.back() == 100 && std::is_sorted(progress.begin(), progress.end()), "progress is reported, in order, up to 100");
        const UnderstandingResult stems = UnderstandAudio(mix.string(), { { voice.string(), "voice", "narradora" }, { music.string(), "music", "base" } });
        bool allFromStem = stems.ok;
        int fromStem = 0;
        for (const ActivitySegment& a : stems.understanding.activities) {
            if (a.kind == "speech" || a.kind == "singing" || a.kind == "chorus") {
                allFromStem = allFromStem && a.provenance == "stem:voice" && a.track == "stem:narradora";
                ++fromStem;
            }
        }
        Check(stems.ok && stems.understanding.stems.size() == 2 && stems.understanding.stems[0].usable && !stems.understanding.stems[0].hash.empty(), "stems: each one read and identified by its contents");
        Check(fromStem > 0 && allFromStem, "stems: the voice heard on its own track replaces the voice guessed in the mix, on its own track");
        const UnderstandingResult missing = UnderstandAudio(mix.string(), { { (_root / "audio" / "no-such.wav").string(), "voice", "x" } });
        Check(missing.ok && std::find_if(missing.understanding.warnings.begin(), missing.understanding.warnings.end(), [](const std::string& w) { return w.rfind("stem-unusable:", 0) == 0; }) != missing.understanding.warnings.end(),
              "a stem that cannot be read is said, and the mix is still understood");
        const fs::path broken = _root / "audio" / "broken.wav";
        {
            std::ofstream out(broken, std::ios::binary);
            out << "RIFF\x10\x00\x00\x00WAVEnot really a sound file";
            for (int i = 0; i < 4000; ++i) {
                out << static_cast<char>((i * 37) & 0xff);
            }
        }
        const UnderstandingResult bad = UnderstandAudio(broken.string(), {});
        Check(!bad.ok && !bad.errorCode.empty(), fmt::format("a damaged file is refused with a reason ({}), without stopping the program", bad.errorCode));
        const UnderstandingResult none = UnderstandAudio((_root / "audio" / "absent.wav").string(), {});
        Check(!none.ok && none.errorCode == "UNDERSTAND_FILE_NOT_FOUND", "a missing file is said to be missing");
        const UnderstandingResult cancelled = UnderstandAudio(mix.string(), {}, {}, {}, []() { return true; });
        Check(cancelled.cancelled && !cancelled.ok, "a cancelled reading stops and says so");
        SyntheticSong tiny(0.3);
        tiny.Tone(0, 0.3, 440, 0.3);
        const UnderstandingResult shortFile = UnderstandAudio(Wav("short.wav", tiny).string(), {});
        Check(!shortFile.ok && shortFile.errorCode == "UNDERSTAND_TOO_SHORT", "a file too short to understand is refused plainly");

        const SyntheticSong song = NarrationOverMusic(true, true);
        const std::string a = ToJson(Hear(song));
        const std::string b = ToJson(Hear(song));
        Check(a == b, "the same audio is understood the same way every time");
        AudioUnderstanding back;
        std::string error;
        Check(FromJson(a, back, error) && ToJson(back) == a, "written and read again, nothing changes");
        json newer = json::parse(a);
        newer["schemaVersion"] = AUDIO_UNDERSTANDING_SCHEMA_VERSION + 1;
        Check(!FromJson(newer.dump(), back, error), "a reading written by a newer version is not read as if it were this one");
    }

    // ------------------------------------------------------------ corrections

    static const ActivitySegment* First(const AudioUnderstanding& u, const std::string& kind) {
        const auto list = u.Of(kind);
        return list.empty() ? nullptr : list.front();
    }

    static UnderstandingCorrection Correct(UnderstandingCorrection::Action action, const ActivitySegment& s) {
        UnderstandingCorrection c;
        c.action = action;
        c.targetId = s.id;
        c.anchorMS = s.startMS;
        return c;
    }

    void TestCorrections() {
        Title("A person's corrections: kept apart, applied after every analysis");
        SyntheticSong s(17);
        s.Beat(0, 17, 120, 0.4);
        s.Bass(0, 17, 55, 0.25);
        s.Pad(0, 17, 220, 0.12);
        s.Sing(0.5, 7.5, MELODY, 0.4);
        AudioUnderstanding u = Hear(s);
        const ActivitySegment* sung = First(u, "singing");
        const ActivitySegment* instrumental = First(u, "instrumental");
        const ActivitySegment* percussion = First(u, "percussion");
        if (sung == nullptr || instrumental == nullptr || percussion == nullptr) {
            Check(false, "the song to correct has singing, an instrumental and percussion");
            return;
        }
        const int sungStart = sung->startMS;
        const int percussionStart = percussion->startMS;
        const int split = (instrumental->startMS + instrumental->endMS) / 2;
        UnderstandingCorrection reclass = Correct(UnderstandingCorrection::Action::Reclassify, *sung);
        reclass.kind = "speech";
        UnderstandingCorrection splitAt = Correct(UnderstandingCorrection::Action::Split, *instrumental);
        splitAt.atMS = split;
        UnderstandingCorrection remove = Correct(UnderstandingCorrection::Action::Remove, *percussion);
        UnderstandingCorrection add;
        add.action = UnderstandingCorrection::Action::Add;
        add.kind = "speech";
        add.startMS = 15000;
        add.endMS = 16500;
        add.track = "voice:Z";
        UnderstandingCorrection reassign = Correct(UnderstandingCorrection::Action::Reassign, *sung);
        reassign.track = "voice:Q";
        const SoundEvent* accent = nullptr;
        for (const SoundEvent& e : u.events) {
            accent = accent == nullptr && e.kind == "accent" ? &e : accent;
        }
        UnderstandingCorrection moveEvent;
        moveEvent.on = "event";
        moveEvent.action = UnderstandingCorrection::Action::Move;
        moveEvent.targetId = accent != nullptr ? accent->id : "";
        moveEvent.anchorMS = accent != nullptr ? accent->startMS : -1;
        moveEvent.startMS = accent != nullptr ? accent->startMS + 120 : -1;
        moveEvent.endMS = moveEvent.startMS + 20;
        const std::vector<UnderstandingCorrection> corrections = { reclass, splitAt, remove, add, reassign, moveEvent };
        u.corrections = corrections;
        ApplyUnderstandingCorrections(u);
        const auto at = [&](const AudioUnderstanding& x, const std::string& kind, int startMS) {
            for (const ActivitySegment& a : x.activities) {
                if (a.kind == kind && std::abs(a.startMS - startMS) <= 40) {
                    return &a;
                }
            }
            return static_cast<const ActivitySegment*>(nullptr);
        };
        const ActivitySegment* nowSpeech = at(u, "speech", sungStart);
        Check(nowSpeech != nullptr && nowSpeech->provenance == "corrected" && nowSpeech->track == "voice:Q", "reclassified as speech and given to another voice: the person wins");
        int halves = 0;
        for (const ActivitySegment* a : u.Of("instrumental")) {
            halves += a->startMS == split || a->endMS == split ? 1 : 0;
        }
        Check(halves == 2, "split in two at the chosen moment");
        Check(u.Of("percussion").empty() || First(u, "percussion")->startMS != percussionStart, "removed: it is gone");
        const ActivitySegment* added = at(u, "speech", 15000);
        Check(added != nullptr && added->provenance == "added" && added->endMS == 16500 && added->track == "voice:Z", "added by hand where the analysis heard nothing");
        bool moved = false;
        for (const SoundEvent& e : u.events) {
            moved = moved || (accent != nullptr && e.provenance == "corrected" && e.startMS == moveEvent.startMS);
        }
        Check(moved, "a moment of the music (an accent) moved by hand");

        const std::string once = ToJson(u);
        ApplyUnderstandingCorrections(u);
        Check(ToJson(u) == once, "applied twice, nothing changes the second time");

        // A new analysis renumbers: the corrections find their segments again.
        AudioUnderstanding again = Hear(s);
        again.corrections = corrections;
        ApplyUnderstandingCorrections(again);
        Check(ToJson(again) == once, "after a new analysis the same corrections give the same result");
        AudioUnderstanding renamed = Hear(s);
        UnderstandingCorrection stale = reclass;
        stale.targetId = "singing#9";
        renamed.corrections = { stale };
        ApplyUnderstandingCorrections(renamed);
        Check(at(renamed, "speech", sungStart) != nullptr, "a segment renumbered by a new analysis is found again by where it was");
        AudioUnderstanding merged = Hear(s);
        merged.corrections = { splitAt };
        ApplyUnderstandingCorrections(merged);
        const ActivitySegment* left = nullptr;
        const ActivitySegment* right = nullptr;
        for (const ActivitySegment* a : merged.Of("instrumental")) {
            left = a->endMS == split ? a : left;
            right = a->startMS == split ? a : right;
        }
        if (left != nullptr && right != nullptr) {
            UnderstandingCorrection join = Correct(UnderstandingCorrection::Action::Merge, *left);
            join.otherId = right->id;
            merged.corrections.push_back(join);
            ApplyUnderstandingCorrections(merged);
        }
        Check(left != nullptr && right != nullptr && merged.Of("instrumental").size() == Hear(s).Of("instrumental").size(), "split and joined again: one piece, as before");
        AudioUnderstanding restored = u;
        ResetUnderstandingCorrections(restored);
        Check(restored.corrections.empty(), "restoring the automatic reading drops every correction");
        AudioUnderstanding read;
        std::string error;
        Check(FromJson(once, read, error) && read.corrections.size() == corrections.size() && read.corrections.back().on == "event", "the corrections are written with the reading and read back");
    }

    // ------------------------------------------------------------------ store

    void TestStore() {
        Title("What is kept on disk, per song: the reading, the corrections and the assignments apart");
        const fs::path show = _root / "store-show";
        std::error_code ec;
        fs::create_directories(show, ec);
        const fs::path song = Wav("store.wav", NarrationOverMusic(true, true));
        UnderstandRequest r;
        r.showDir = show.string();
        r.audioPath = song.string();
        const UnderstandOutcome first = UnderstandSong(r);
        const fs::path folder = first.song.folder;
        Check(first.ok && !first.song.fromCache && fs::exists(folder / "reading.json") && fs::exists(folder / "voices.json"), "the first time the audio is heard and the reading kept in ShowComposer/understanding");
        Check(folder.parent_path().filename() == "understanding" && folder.parent_path().parent_path().filename() == "ShowComposer", "kept inside the show's ShowComposer folder");
        const UnderstandOutcome second = UnderstandSong(r);
        Check(second.ok && second.song.fromCache && ToJson(second.song.audio) == ToJson(first.song.audio), "the second time the reading is reused: same audio, same reader, same options");
        UnderstandRequest other = r;
        other.options.minVoiceMS = 500;
        const UnderstandOutcome changed = UnderstandSong(other);
        Check(changed.ok && !changed.song.fromCache && std::find(changed.song.notes.begin(), changed.song.notes.end(), "stale-reading:reader-changed") != changed.song.notes.end(),
              "with other options it is heard again, and says why");
        (void)UnderstandSong(r);
        const ActivitySegment* speech = First(first.song.audio, "speech");
        std::string error;
        if (speech != nullptr) {
            UnderstandingCorrection c = Correct(UnderstandingCorrection::Action::Reclassify, *speech);
            c.kind = "singing";
            Check(SaveCorrections(folder.string(), { c }, error), "a correction is saved in its own file");
        }
        PerformerAssignment mine;
        mine.source = "track:voice:A";
        mine.figures = { "Arbol Cara" };
        mine.function = "speak";
        Check(SaveManualAssignments(folder.string(), { mine }, error), "a person's assignment is saved in its own file");
        const UnderstandOutcome corrected = UnderstandSong(r);
        Check(corrected.ok && corrected.song.fromCache && speech != nullptr && corrected.song.audio.Find(speech->id) != nullptr && corrected.song.audio.Find(speech->id)->kind == "singing",
              "read again, the reading is reused and the correction applied over it");
        Check(corrected.song.manual.size() == 1 && corrected.song.manual[0].state == "manual" && corrected.song.manual[0].figures == mine.figures, "and the person's assignment is there");
        std::string readingText;
        Check(ReadTextFile((folder / "reading.json").string(), readingText) && json::parse(readingText).value("corrections", json::array()).empty(), "the reading file never holds a person's corrections");
        Check(RevertUnderstanding(folder.string(), false, error) && fs::exists(folder / "corrections.previous.json") && !fs::exists(folder / "corrections.json"), "back to the automatic reading: the corrections are set aside, not deleted");
        const UnderstandOutcome automatic = UnderstandSong(r);
        Check(automatic.ok && automatic.song.audio.corrections.empty() && automatic.song.manual.empty(), "and are no longer applied");
        Check(RevertUnderstanding(folder.string(), true, error) && fs::exists(folder / "corrections.json"), "restored, they come back");
        const UnderstandOutcome back = UnderstandSong(r);
        Check(back.ok && back.song.audio.corrections.size() == 1 && back.song.manual.size() == 1, "and apply again");
        SongUnderstanding quick;
        Check(LoadSongUnderstanding(folder.string(), r, quick, error) && ToJson(quick.audio) == ToJson(back.song.audio) && quick.manual.size() == 1 && ToJson(quick.voices) == ToJson(back.song.voices),
              "read back for the window without the audio: the same as understood");
        {
            std::ofstream out(folder / "corrections.json", std::ios::binary | std::ios::trunc);
            out << "{ damaged";
        }
        const UnderstandOutcome damaged = UnderstandSong(r);
        Check(!damaged.ok && damaged.errorCode == "corrections-unreadable", "a damaged corrections file is said, never silently replaced");
        std::string kept;
        Check(ReadTextFile((folder / "corrections.json").string(), kept) && kept == "{ damaged", "and is left as it was for the person to look at");
        std::vector<std::string> files;
        for (const auto& entry : fs::directory_iterator(folder, ec)) {
            files.push_back(entry.path().filename().string());
        }
        std::sort(files.begin(), files.end());
        Check(files == std::vector<std::string>({ "corrections.json", "performers.json", "reading.json", "voices.json" }), "nothing else is written");
    }

    // ----------------------------------------------------------------- voices

    void TestVoices() {
        Title("Voices: told apart by how they sound, never separated, never identified");
        SyntheticSong s(27);
        s.Speech(0.5, 6, 110, 0.35);
        s.Sing(8, 6, { 330, 370, 392, 370, 330, 294, 330, 392 }, 0.35);
        s.Choir(15.5, 5, MELODY, 0.45);
        s.Speech(22, 4, 110, 0.35);
        const AudioUnderstanding u = Hear(s);
        VocalInput in;
        in.audio = &u;
        const VocalUnderstanding v = UnderstandVoices(in);
        const auto trackAt = [&](int ms) -> std::string {
            for (const Intervention& i : v.interventions) {
                if (i.startMS <= ms && ms < i.endMS) {
                    return i.track;
                }
            }
            return "";
        };
        const std::string narrator = trackAt(2000);
        const std::string singer = trackAt(10000);
        const std::string choir = trackAt(18000);
        std::string heard;
        for (const Intervention& i : v.interventions) {
            heard += fmt::format(" {}[{}-{} {}]", i.track, i.startMS, i.endMS, i.emission);
        }
        printf("  · voices:%s\n", heard.c_str());
        std::set<std::string> spokenOn;
        for (const Intervention& i : v.interventions) {
            if (i.emission == "spoken") {
                spokenOn.insert(i.track);
            }
        }
        Check(!narrator.empty() && narrator == trackAt(23000) && spokenOn.size() == 1, fmt::format("the same speaking voice, in every phrase and heard again later, is one voice ({} voices)", spokenOn.size()));
        Check(!singer.empty() && singer != narrator, fmt::format("a different voice singing is another voice ({})", singer));
        Check(choir == "chorus" && v.Track("chorus") != nullptr && v.Track("chorus")->label == "Coro", "the choir is one track, the choir");
        const VoiceTrack* n = v.Track(narrator);
        const VoiceTrack* g = v.Track(singer);
        Check(n != nullptr && n->label.rfind("Voz ", 0) == 0 && n->emission == "spoken" && n->kind == "narrator", "the speaking voice: Voz A or B, spoken, a narrator");
        Check(g != nullptr && g->emission == "sung" && g->kind == "lead", "the singing voice leads");
        Check(std::any_of(v.limits.begin(), v.limits.end(), [](const std::string& l) { return l.rfind("no-source-separation", 0) == 0; }) &&
                  std::any_of(v.limits.begin(), v.limits.end(), [](const std::string& l) { return l.rfind("diarization-experimental", 0) == 0; }),
              "it says plainly that no voice was separated and that telling voices apart is experimental");
        bool relations = true;
        for (const Intervention& i : v.interventions) {
            relations = relations && !i.relations.empty() && (i.relations.front() == "alone" || i.relations.front() == "over-music");
        }
        Check(relations, "each intervention says whether it is heard alone or over music");

        // The lyrics name the voices: names win over letters, words reach the interventions.
        LyricAlignment al;
        AlignedPhrase p1;
        p1.id = "l1";
        p1.text = "Erase una vez un pino pequeno";
        p1.voices = { "v:narrador" };
        p1.delivery = "spoken";
        p1.startMS = 600;
        p1.endMS = 6400;
        AlignedPhrase p2;
        p2.id = "l2";
        p2.text = "la la la";
        p2.startMS = 8000;
        p2.endMS = 14000;
        AlignedPhrase p3;
        p3.id = "l3";
        p3.text = "una linea que no se oye";
        p3.voices = { "v:eco" };
        p3.startMS = 26200;
        p3.endMS = 26900;
        al.phrases = { p1, p2, p3 };
        AlignedWord w;
        w.id = "l1/0:erase";
        w.phraseId = "l1";
        w.text = "Erase";
        w.startMS = 700;
        w.endMS = 1100;
        al.words = { w };
        VoiceCast cast;
        VoiceDef narr;
        narr.id = "v:narrador";
        narr.name = "Narrador";
        narr.role = VoiceRole::Narrator;
        VoiceDef eco;
        eco.id = "v:eco";
        eco.name = "Eco";
        cast.voices = { narr, eco };
        in.alignment = &al;
        in.cast = &cast;
        const VocalUnderstanding named = UnderstandVoices(in);
        const VoiceTrack* nt = named.Track(narrator);
        Check(nt != nullptr && nt->label == "Narrador" && nt->kind == "narrator", "a voice the lyrics name takes its name");
        const Intervention* told = nullptr;
        const Intervention* lyricOnly = nullptr;
        for (const Intervention& i : named.interventions) {
            told = i.startMS <= 2000 && 2000 < i.endMS ? &i : told;
            lyricOnly = i.provenance == "lyrics" ? &i : lyricOnly;
        }
        Check(told != nullptr && !told->text.empty() && told->wordIds.size() == 1, "its interventions carry their words");
        Check(lyricOnly != nullptr && lyricOnly->track == "lyric:v:eco" && named.Track("lyric:v:eco") != nullptr && named.Track("lyric:v:eco")->label == "Eco",
              "a line the lyrics place where nothing was heard is kept, from the lyrics, on its voice");
        const std::string text = ToJson(named);
        Check(text.find("pino") == std::string::npos && text.find("Erase") == std::string::npos, "the saved voices hold no lyrics, only how many words");
        VocalUnderstanding back;
        std::string error;
        Check(FromJson(text, back, error) && back.tracks.size() == named.tracks.size() && back.interventions.size() == named.interventions.size(), "written and read again");

        // Call and response: the choir answers.
        SyntheticSong cr(14);
        cr.Beat(0, 14, 100, 0.3);
        cr.Bass(0, 14, 49, 0.2);
        for (int k = 0; k < 2; ++k) {
            cr.Sing(0.5 + k * 6.6, 2.8, { 262, 294, 330, 294 }, 0.4);
            cr.Choir(3.8 + k * 6.6, 2.8, { 196, 220, 247, 220 }, 0.5);
        }
        const AudioUnderstanding cu = Hear(cr);
        VocalInput cin;
        cin.audio = &cu;
        const VocalUnderstanding cv = UnderstandVoices(cin);
        int answers = 0, over = 0;
        for (const Intervention& i : cv.interventions) {
            for (const std::string& rel : i.relations) {
                answers += i.track == "chorus" && rel.rfind("answers:", 0) == 0 ? 1 : 0;
                over += rel == "over-music" ? 1 : 0;
            }
        }
        Check(answers >= 1, "call and response: the choir answers the soloist");
        Check(over == static_cast<int>(cv.interventions.size()), "every voice there is over the music");
    }

    // ---------------------------------------------------------------- figures

    bool WriteFigureFixture(const fs::path& folder) {
        if (!WritePlanFixture(folder.string())) {
            return false;
        }
        pugi::xml_document doc;
        const fs::path file = folder / "xlights_rgbeffects.xml";
        if (!doc.load_file(file.string().c_str())) {
            return false;
        }
        pugi::xml_node models = doc.child("xrgb").child("models");
        int channel = 1;
        for (pugi::xml_node m : models.children("model")) {
            channel = std::max(channel, static_cast<int>(std::strtol(m.attribute("StartChannel").value(), nullptr, 10)) + 4000);
        }
        // Made-up figures: a doll whose mouth is only a submodel's name, a drawn tree with
        // a trunk and a crown as submodels.
        const auto custom = [&](const char* name, int w, int h) {
            pugi::xml_node m = models.append_child("model");
            std::string compressed;
            int node = 1;
            for (int row = 0; row < h; ++row) {
                for (int col = 0; col < w; ++col) {
                    compressed += fmt::format("{},{},{};", node++, row, col);
                }
            }
            for (const auto& [k, v] : std::vector<std::pair<std::string, std::string>>{ { "name", name }, { "DisplayAs", "Custom" }, { "StringType", "RGB Nodes" }, { "LayoutGroup", "Default" },
                                                                                       { "StartSide", "B" }, { "Dir", "L" }, { "versionNumber", "8" }, { "StartChannel", std::to_string(channel) },
                                                                                       { "CustomModelCompressed", compressed }, { "CustomWidth", std::to_string(w) },
                                                                                       { "CustomHeight", std::to_string(h) }, { "Depth", "1" } }) {
                m.append_attribute(k.c_str()) = v.c_str();
            }
            channel += w * h * 3;
            return m;
        };
        const auto sub = [](pugi::xml_node m, const char* name, const char* range) {
            pugi::xml_node s = m.append_child("subModel");
            for (const auto& [k, v] : std::vector<std::pair<const char*, const char*>>{ { "name", name }, { "bufferstyle", "Default" }, { "layout", "horizontal" }, { "line0", range }, { "type", "ranges" } }) {
                s.append_attribute(k) = v;
            }
        };
        pugi::xml_node doll = custom("Muneco", 8, 8);
        sub(doll, "Boca", "1-6");
        pugi::xml_node drawn = custom("Arbol Dibujado", 6, 10);
        sub(drawn, "Tronco", "1-12");
        sub(drawn, "Copa", "13-60");
        return doc.save_file(file.string().c_str());
    }

    void TestFigures() {
        Title("Knowing the figures: geometry from xLights' own models, parts tied to real lights");
        _fixture = _root / "figures-show";
        if (!WriteFigureFixture(_fixture)) {
            Check(false, "the made-up show is written");
            return;
        }
        _ctx = std::make_unique<HeadlessRenderContext>();
        if (!_ctx->LoadShowFolder(_fixture.string())) {
            Check(false, "the made-up show loads");
            return;
        }
        _caps = ExtractCapabilities(*_ctx, nullptr);
        _geometry = DescribeFigures(*_ctx, _caps, true);
        const FigureGeometry* matrix = _geometry.Find("Matriz Prueba");
        Check(matrix != nullptr && matrix->topology == "matrix" && matrix->nodes == 1152 && matrix->channels == 3456 && matrix->channelsPerNode == 3,
              "a matrix: 1152 lights, 3456 channels - lights and channels are different numbers");
        Check(matrix != nullptr && matrix->columns == 48 && matrix->rows == 24 && matrix->orientation == "horizontal", "its rows, columns and orientation");
        bool inside = matrix != nullptr && !matrix->nodeMap.empty();
        for (const FigureNode& n : matrix != nullptr ? matrix->nodeMap : std::vector<FigureNode>()) {
            inside = inside && n.x >= 0 && n.x <= 1 && n.y >= 0 && n.y <= 1 && n.channel >= 0;
        }
        Check(inside && matrix->nodeMap[0].channel == 0 && matrix->nodeMap[1].channel == 3, "every light has its place on the figure (0-1) and its first channel");
        const FigureGeometry* tree = _geometry.Find("Arbol Grande");
        Check(tree != nullptr && tree->topology == "tree-360" && tree->branches == 16 && tree->nodes == 800, "a 360 tree with its 16 strings");
        const FigureGeometry* arch = _geometry.Find("Arco 1");
        Check(arch != nullptr && arch->topology == "arch" && arch->segments == 1, "an arch");
        const FigureGeometry* doll = _geometry.Find("Muneco");
        Check(doll != nullptr && doll->topology == "custom" && !doll->limits.empty() && doll->confidence < 0.9, "an irregular figure: its points are kept, and it says what is not known");
        const FigureGeometry* free = _geometry.Find("Sin Controlador");
        Check(free != nullptr && free->noController && free->availability == "preview" && std::find(free->subModels.begin(), free->subModels.end(), "Brazo") != free->subModels.end(),
              "a figure with no controller is known to be preview-only, with its submodels");
        const FigureGeometry* arm = _geometry.Find("Sin Controlador/Brazo");
        bool parentNumbers = arm != nullptr && arm->kind == "submodel" && arm->parent == "Sin Controlador" && arm->nodes == 30;
        for (const FigureNode& n : arm != nullptr ? arm->nodeMap : std::vector<FigureNode>()) {
            parentNumbers = parentNumbers && n.index >= 0 && n.index < 300;
        }
        Check(parentNumbers, "a submodel: its lights numbered as its parent's");
        const FigureGeometry* group = _geometry.Find("Grupo Arcos");
        Check(group != nullptr && group->kind == "group" && group->members.size() == 2, "a group and its members");
        FigureCatalog read;
        std::string error;
        Check(FromJson(ToJson(_geometry, true), read, error) && read.figures.size() == _geometry.figures.size() && read.Find("Matriz Prueba")->nodeMap.size() == 1152 &&
                  read.Find("Matriz Prueba")->nodeMap[1].channel == 3,
              "written and read again, lights and channels included");

        Title("Parts of each figure and what it can play");
        _semantics = DescribeSemantics(*_ctx, _geometry, _caps, nullptr);
        const FigureProfile* face = _semantics.Find("Arbol Cara");
        const FigureRegion* mouth = face != nullptr ? face->Region("mouth") : nullptr;
        Check(face != nullptr && face->mouth == "nodes" && face->CanSpeak() && mouth != nullptr && mouth->real && mouth->nodes.size() == 27 && mouth->source == "face-definition:Boca",
              "a face definition gives a real mouth: its 27 lights");
        Check(face != nullptr && face->Region("eyes") != nullptr && face->Region("eyes")->nodes.size() == 3, "and its eyes");
        Check(mouth != nullptr && mouth->color == DefaultRegionColor("mouth", face->family) && !mouth->color.empty(), "a mouth keeps its own colour");
        Check(face != nullptr && std::find(face->roles.begin(), face->roles.end(), "narrator") != face->roles.end(), "a figure with a mouth can narrate or sing");
        const FigureProfile* named = _semantics.Find("Muneco");
        Check(named != nullptr && named->mouth == "suggested" && !named->CanSpeak() &&
                  std::find(named->notes.begin(), named->notes.end(), "region-to-confirm:mouth") != named->notes.end(),
              "a submodel called Boca only suggests a mouth: it cannot speak until a person confirms it");
        const FigureProfile* drawn = _semantics.Find("Arbol Dibujado");
        const FigureRegion* trunk = drawn != nullptr ? drawn->Region("trunk") : nullptr;
        Check(trunk != nullptr && trunk->nodes.size() == 12 && trunk->color == "#6B3E1E" && drawn->Region("foliage") != nullptr && drawn->Region("foliage")->nodes.size() == 48,
              "a trunk and a crown from submodels: the trunk stays brown whatever the song's colours");
        const FigureProfile* plain = _semantics.Find("Arbol Pequeno");
        Check(plain != nullptr && plain->mouth == "none" && plain->Region("foliage") != nullptr &&
                  std::any_of(plain->notes.begin(), plain->notes.end(), [](const std::string& n) { return n.rfind("no-mouth", 0) == 0; }),
              "a tree with no face: foliage, and it says it has no mouth");
        const FigureProfile* screen = _semantics.Find("Matriz Prueba");
        Check(screen != nullptr && screen->Region("background") && screen->Region("image") && screen->Region("text"), "a matrix: background, pictures and text");
        const FigureProfile* preview = _semantics.Find("Sin Controlador");
        Check(preview != nullptr && std::any_of(preview->notes.begin(), preview->notes.end(), [](const std::string& n) { return n.rfind("no-controller", 0) == 0; }), "a figure with no controller says so");

        FigureChoices choices;
        FigureChoice c;
        c.figure = "Muneco";
        FigureRegion m;
        m.id = "mouth";
        m.source = "manual";
        m.nodes = ParseNodeRanges("1-6", 64);
        c.regions = { m };
        c.colors = { { "mouth", "#FF8800" } };
        c.reviewed = true;
        FigureChoice d;
        d.figure = "Arbol Pequeno";
        d.dropped = { "foliage" };
        choices.figures = { c, d };
        Check(FromJson(ToJson(choices), _choices, error) && _choices.figures.size() == 2, "a person's choices are written and read again");
        Check(WriteTextFileAtomically(FigureChoicesPath(_fixture.string()), ToJson(_choices), error), "kept in ShowComposer/figures.json");
        _semantics = DescribeSemantics(*_ctx, _geometry, _caps, &_choices);
        named = _semantics.Find("Muneco");
        Check(named != nullptr && named->mouth == "nodes" && named->CanSpeak() && named->Region("mouth")->confirmed && named->Region("mouth")->color == "#FF8800" && named->reviewed,
              "confirmed by a person, with its lights: now it can speak, in the colour chosen");
        plain = _semantics.Find("Arbol Pequeno");
        Check(plain != nullptr && plain->Region("foliage") == nullptr, "a part a person removed stays removed");
        FigureSemantics found;
        Check(FromJson(ToJson(DescribeSemantics(*_ctx, _geometry, _caps, nullptr)), found, error) && ToJson(ApplyFigureChoices(found, _geometry, &_choices)) == ToJson(_semantics),
              "the choices applied over what was found, read back from a file, give the same parts: the window needs no show loaded");
        Check(ParseNodeRanges("1-3,5,99", 10) == std::vector<int>({ 0, 1, 2, 4 }) && NodeRangesText({ 0, 1, 2, 4 }) == "1-3,5", "light ranges as xLights writes them, read and written");
    }

    // ---------------------------------------------------------------- mapping

    // A song described by hand, as if heard: a narrator, a soloist, the choir, the
    // instruments' turn, drums all along.
    static void HandMade(AudioUnderstanding& u, VocalUnderstanding& v) {
        u.durationMS = 30000;
        const auto act = [&](const char* id, const char* kind, int a, int b, const char* track) {
            ActivitySegment s;
            s.id = id;
            s.kind = kind;
            s.startMS = a;
            s.endMS = b;
            s.track = track;
            s.confidence = 0.8;
            u.activities.push_back(s);
        };
        act("music#1", "music", 0, 30000, "");
        act("speech#1", "speech", 0, 5000, "voice:A");
        act("singing#1", "singing", 6000, 12000, "voice:B");
        act("chorus#1", "chorus", 12500, 16000, "chorus");
        act("singing#2", "singing", 17000, 19000, "voice:B");
        act("instrumental#1", "instrumental", 20000, 26000, "");
        act("percussion#1", "percussion", 0, 30000, "");
        const auto track = [&](const char* id, const char* label, const char* kind, const char* emission) {
            VoiceTrack t;
            t.id = id;
            t.label = label;
            t.kind = kind;
            t.emission = emission;
            t.confidence = 0.6;
            v.tracks.push_back(t);
        };
        track("chorus", "Coro", "chorus", "sung");
        track("voice:A", "Voz A", "narrator", "spoken");
        track("voice:B", "Voz B", "lead", "sung");
        const auto inter = [&](const char* id, const char* activity, const char* t, int a, int b, const char* emission) {
            Intervention i;
            i.id = id;
            i.activityId = activity;
            i.track = t;
            i.startMS = a;
            i.endMS = b;
            i.emission = emission;
            i.provenance = "detected";
            v.interventions.push_back(i);
        };
        inter("voice:A#1", "speech#1", "voice:A", 0, 5000, "spoken");
        inter("voice:B#1", "singing#1", "voice:B", 6000, 12000, "sung");
        inter("chorus#1", "chorus#1", "chorus", 12500, 16000, "choral");
        inter("voice:B#2", "singing#2", "voice:B", 17000, 19000, "sung");
    }

    void TestMapping() {
        Title("Who performs what: voices and moments given to figures");
        if (_ctx == nullptr) {
            Check(false, "the figures are known");
            return;
        }
        AudioUnderstanding u;
        VocalUnderstanding v;
        HandMade(u, v);
        MappingInput in;
        in.audio = &u;
        in.voices = &v;
        in.geometry = &_geometry;
        in.semantics = &_semantics;
        const PerformanceMap map = MapPerformance(in);
        const PerformerAssignment* narrator = map.For("track:voice:A");
        const PerformerAssignment* lead = map.For("track:voice:B");
        const PerformerAssignment* choir = map.For("track:chorus");
        const auto canSpeak = [&](const PerformerAssignment* a) {
            bool ok = a != nullptr && !a->figures.empty();
            for (const std::string& f : a != nullptr ? a->figures : std::vector<std::string>()) {
                ok = ok && _semantics.Find(f) != nullptr && _semantics.Find(f)->CanSpeak();
            }
            return ok;
        };
        Check(canSpeak(narrator) && narrator->function == "speak" && narrator->state == "automatic" && !narrator->reasons.empty(), "the narrator is given a figure with a mouth, and says why");
        Check(canSpeak(lead) && lead->function == "sing" && lead->figures != narrator->figures, "the soloist another one");
        Check(choir != nullptr && choir->function == "choral-response" && (choir->figures.size() >= 2 || std::find(choir->conflicts.begin(), choir->conflicts.end(), "too-few-figures-for-chorus") != choir->conflicts.end()),
              "the choir: several figures together, or it says there are too few");
        Check(narrator != nullptr && (narrator->region == "mouth" || narrator->region == "face"), "a voice is shown by the mouth (or the face's pictures)");
        const PerformerAssignment* inst = map.For("activity:instrumental");
        bool family = inst != nullptr && !inst->figures.empty();
        for (const std::string& f : inst != nullptr ? inst->figures : std::vector<std::string>()) {
            const FigureGeometry* g = _geometry.Find(f);
            family = family && g != nullptr && (g->family == "mega-tree" || g->family == "matrix");
        }
        Check(family && inst->function == "instrumental-lead", "the instruments' turn goes to the big tree or the matrix");
        const auto windows = ActiveWindows(*narrator, u, v);
        Check(windows.size() == 1 && windows[0] == std::make_pair(0, 5000), "the narrator's figure performs only while it speaks: not in the instrumental");
        Check(ActiveWindows(*lead, u, v).size() == 2 && ActiveWindows(*inst, u, v) == std::vector<std::pair<int, int>>({ { 20000, 26000 } }), "the soloist twice, the instruments once");

        // A person's assignments win, and the problems are named.
        std::vector<PerformerAssignment> manual(3);
        manual[0].source = "track:voice:A";
        manual[0].figures = { "Arbol Pequeno" };
        manual[0].function = "speak";
        manual[1].source = "track:voice:B";
        manual[1].figures = { "Sin Controlador" };
        manual[1].function = "sing";
        manual[2].source = "activity:percussion";
        manual[2].figures = { "Arco 1" };
        manual[2].function = "accent";
        manual[2].region = "mouth";
        std::vector<PerformerAssignment> read;
        std::string error;
        Check(FromJson(ToJson(manual), read, error) && read.size() == 3 && read[0].state == "manual", "a person's assignments are written and read again");
        in.manual = &read;
        const PerformanceMap mine = MapPerformance(in);
        const auto has = [](const PerformerAssignment* a, const std::string& prefix) {
            return a != nullptr && std::any_of(a->conflicts.begin(), a->conflicts.end(), [&](const std::string& c) { return c.rfind(prefix, 0) == 0; });
        };
        Check(mine.For("track:voice:A") != nullptr && mine.For("track:voice:A")->state == "manual" && mine.For("track:voice:A")->figures == std::vector<std::string>({ "Arbol Pequeno" }),
              "the person's choice wins over the automatic one");
        Check(has(mine.For("track:voice:A"), "figure-has-no-mouth"), "and it says the tree has no mouth to speak with");
        Check(has(mine.For("track:voice:B"), "no-controller"), "a figure with no controller is named as such");
        Check(has(mine.For("activity:percussion"), "region-missing"), "a part the figure does not have is named");
        Check(ConflictPhrase("figure-has-no-mouth:Arbol Pequeno") != "figure-has-no-mouth:Arbol Pequeno" && ConflictPhrase("too-few-figures-for-chorus") != "too-few-figures-for-chorus",
              "every problem has plain words");
        std::vector<PerformerAssignment> same(2);
        same[0].source = "track:voice:A";
        same[0].figures = { "Arbol Cara" };
        same[0].function = "speak";
        same[1].source = "track:voice:B";
        same[1].figures = { "Arbol Cara" };
        same[1].function = "sing";
        VocalUnderstanding together = v;
        Intervention extra = together.interventions[0];
        extra.id = "voice:A#2";
        extra.startMS = 6500;
        extra.endMS = 8000;
        together.interventions.push_back(extra);
        MappingInput both = in;
        both.voices = &together;
        both.manual = &same;
        const PerformanceMap clash = MapPerformance(both);
        Check(has(clash.For("track:voice:A"), "simultaneous-voices"), "two voices at once on one mouth are named");

        // The voices heard reach the lyrics: each face sings only its own phrases.
        LyricAlignment al;
        for (const auto& [id, a, b] : std::vector<std::tuple<const char*, int, int>>{ { "l1", 200, 4800 }, { "l2", 6200, 11800 }, { "l3", 20500, 25000 } }) {
            AlignedPhrase p;
            p.id = id;
            p.text = "palabras inventadas";
            p.startMS = a;
            p.endMS = b;
            al.phrases.push_back(p);
        }
        al.phrases[2].instrumental = true;
        VoiceCast cast;
        const CastFromUnderstanding given = ApplyPerformersToCast(v, map, _semantics, al, cast);
        Check(given.phrasesNamed == 2 && given.voicesAdded == 2 && al.phrases[0].voices == std::vector<std::string>({ "u:voice:A" }) && al.phrases[0].delivery == "spoken" &&
                  al.phrases[1].voices == std::vector<std::string>({ "u:voice:B" }) && al.phrases[2].voices.empty(),
              "each unnamed phrase takes the voice heard in it; the instrumental stays nobody's");
        const VoiceDef* a = cast.Find("u:voice:A");
        Check(a != nullptr && a->faces == narrator->figures && a->source == "understanding" && a->role == VoiceRole::Unknown && a->name == "Voz A",
              fmt::format("that voice's faces are its figures, and no role is guessed from the sound ({} / {})", a != nullptr && !a->faces.empty() ? a->faces.front() : "-",
                          narrator->figures.empty() ? "-" : narrator->figures.front()));
        // A mouth chosen by hand describes the figure; only xLights' own faces sing.
        PerformanceMap byHand = map;
        for (PerformerAssignment& x : byHand.assignments) {
            if (x.source == "track:voice:A") {
                x.figures = { "Muneco" };
            }
        }
        LyricAlignment al2;
        for (AlignedPhrase p : al.phrases) {
            p.voices.clear();
            p.voice.clear();
            al2.phrases.push_back(p);
        }
        VoiceCast cast2;
        const CastFromUnderstanding drawn = ApplyPerformersToCast(v, byHand, _semantics, al2, cast2);
        Check(cast2.Find("u:voice:A") != nullptr && cast2.Find("u:voice:A")->faces.empty() && cast2.Find("u:voice:A")->facesOff &&
                  std::find(drawn.notes.begin(), drawn.notes.end(), "mouth-not-drawable:Muneco") != drawn.notes.end(),
              "a mouth chosen by hand is not given to the Faces effect, which draws only xLights' own faces: it is said");
        // A narrator the lyrics name, with no face: it takes the figure proposed for its voice.
        VocalUnderstanding namedV = v;
        for (VoiceTrack& t : namedV.tracks) {
            if (t.id == "voice:A") {
                t.evidence.push_back("named in the lyrics: Narrador");
            }
        }
        VoiceCast lyricCast;
        VoiceDef nd;
        nd.id = "v:narrador";
        nd.name = "Narrador";
        nd.role = VoiceRole::Narrator;
        nd.delivery = Delivery::Spoken;
        lyricCast.voices = { nd };
        LyricAlignment al3;
        AlignedPhrase q;
        q.id = "n1";
        q.text = "palabras inventadas";
        q.voices = { "v:narrador" };
        q.startMS = 300;
        q.endMS = 4700;
        al3.phrases = { q };
        const CastFromUnderstanding filled = ApplyPerformersToCast(namedV, map, _semantics, al3, lyricCast);
        Check(lyricCast.voices[0].faces == narrator->figures && filled.facesChanged == 1, "a narrator the lyrics name, with no face, takes the figure proposed for its voice");
        PerformanceMap synced = map;
        SyncPerformersWithCast(synced, namedV, lyricCast, &al3);
        const PerformerAssignment* sn = synced.For("track:voice:A");
        Check(sn != nullptr && sn->figures == lyricCast.voices[0].faces && ActiveWindows(*sn, u, namedV) == std::vector<std::pair<int, int>>({ { 300, 4700 } }),
              "the assignment as the show was made: the voice's faces, during its own phrases");
        const PerformerAssignment* sb = synced.For("track:voice:B");
        Check(sb != nullptr && sb->figures.empty() && has(sb, "no-lyrics-for-voice") && !ConflictPhrase("no-lyrics-for-voice").empty(),
              "a voice with no phrase of its own in the lyrics performs nowhere, and says why");
        VoiceCast twoCast = lyricCast;
        VoiceDef echo;
        echo.id = "v:eco";
        echo.name = "Eco";
        echo.faces = { "Carita" };
        twoCast.voices.push_back(echo);
        LyricAlignment al4 = al3;
        AlignedPhrase e4 = q;
        e4.id = "e1";
        e4.voices = { "v:eco" };
        e4.startMS = 21000;
        e4.endMS = 23000;
        al4.phrases.push_back(e4);
        PerformanceMap withEcho = map;
        SyncPerformersWithCast(withEcho, namedV, twoCast, &al4);
        const PerformerAssignment* ea = withEcho.For("track:lyric:v:eco");
        Check(ea != nullptr && ea->figures == std::vector<std::string>({ "Carita" }) && ea->windows == std::vector<std::pair<int, int>>({ { 21000, 23000 } }),
              "a voice of the lyrics no heard voice stands for still sings, on its faces, in its phrases");
        const std::vector<TimingTrackSpec> tracks = BuildVoiceTracks(al, cast, "Composer Lyrics");
        bool own = tracks.size() == 2;
        for (const TimingTrackSpec& t : tracks) {
            own = own && t.layers[0].size() == 1;
        }
        Check(own, "one timing track per voice, with only its own phrases: a face does not sing another's line");
    }

    // --------------------------------------------------------------- timeline

    void TestTimeline() {
        Title("What happens in the recording, lane by lane, and its corrections");
        SyntheticSong s(17);
        s.Beat(0, 17, 120, 0.4);
        s.Bass(0, 17, 55, 0.25);
        s.Pad(0, 17, 220, 0.12);
        s.Sing(0.5, 7.5, MELODY, 0.4);
        AudioUnderstanding audio = Hear(s);
        VocalInput vin;
        vin.audio = &audio;
        VocalUnderstanding voices = UnderstandVoices(vin);
        const SongAnalysis song = PlanTestSong();
        MappingInput min;
        min.audio = &audio;
        min.voices = &voices;
        min.geometry = &_geometry;
        min.semantics = &_semantics;
        const PerformanceMap performers = MapPerformance(min);
        TimelineInput tin;
        tin.song = &song;
        tin.audio = &audio;
        tin.voices = &voices;
        tin.performers = &performers;
        MultimodalTimeline t = BuildTimeline(tin);
        std::set<std::string> ids;
        bool unique = true;
        for (const TimelineLane& l : t.lanes) {
            for (const TimelineItem& i : l.items) {
                unique = unique && ids.insert(i.id).second;
            }
        }
        Check(t.lanes.size() == 12 && unique, "twelve lanes, every item with its own id");
        Check(!t.Lane("structure")->items.empty() && !t.Lane("beats")->items.empty() && !t.Lane("bars")->items.empty(), "the song's parts, beats and bars");
        Check(!t.Lane("lead")->items.empty() && !t.Lane("instrumental")->items.empty() && !t.Lane("events")->items.empty(), "the soloist, the instruments alone, hits and drops");
        Check(!t.Lane("figures")->items.empty(), "and the figures that show them");
        Check(LaneTitle("lead") != "lead" && !t.Lane("chorus")->title.empty(), "lanes with plain names");
        Check(!t.At(2000).empty(), "what sounds at a moment can be asked");

        const TimelineItem* section = t.Lane("structure")->items.empty() ? nullptr : &t.Lane("structure")->items.front();
        TimelineEdit e;
        e.itemId = section != nullptr ? section->id : "";
        e.action = TimelineEdit::Action::Move;
        e.startMS = 0;
        e.endMS = 1000;
        Check(TranslateEdit(t, e, audio, voices, &performers).error == "edit-in-music-analysis", "the song's parts are corrected in the music analysis, and it says so");
        const TimelineItem* sung = t.Lane("lead")->items.empty() ? nullptr : &t.Lane("lead")->items.front();
        e = {};
        e.itemId = sung != nullptr ? sung->id : "";
        e.action = TimelineEdit::Action::Reclassify;
        e.kind = "speech";
        TimelineChange change = TranslateEdit(t, e, audio, voices, &performers);
        Check(change.ok && change.isCorrection && change.correction.action == UnderstandingCorrection::Action::Reclassify && change.correction.on == "activity",
              "a voice reclassified on the timeline becomes a correction of the sound it came from");
        std::vector<PerformerAssignment> manual;
        const std::string target = change.correction.targetId;
        ApplyChange(change, audio, manual);
        Check(audio.Find(target) != nullptr && audio.Find(target)->kind == "speech", "applied at once");
        const TimelineItem* inst = nullptr;
        for (const TimelineItem& i : t.Lane("instrumental")->items) {
            inst = inst == nullptr && i.kind == "instrumental" ? &i : inst;
        }
        e = {};
        e.itemId = inst != nullptr ? inst->id : "";
        e.action = TimelineEdit::Action::Split;
        e.atMS = inst != nullptr ? (inst->startMS + inst->endMS) / 2 : 0;
        change = TranslateEdit(t, e, audio, voices, &performers);
        const size_t before = audio.Of("instrumental").size();
        ApplyChange(change, audio, manual);
        Check(change.ok && audio.Of("instrumental").size() == before + 1, fmt::format("split on the timeline ({} {} at {}: {} -> {})", change.error, change.correction.targetId, e.atMS, before, audio.Of("instrumental").size()));
        const TimelineItem* hit = nullptr;
        for (const TimelineItem& i : t.Lane("events")->items) {
            hit = hit == nullptr && (i.kind == "attack" || i.kind == "accent") ? &i : hit;
        }
        e = {};
        e.itemId = hit != nullptr ? hit->id : "";
        e.action = TimelineEdit::Action::Split;
        e.atMS = hit != nullptr ? hit->startMS : 0;
        Check(TranslateEdit(t, e, audio, voices, &performers).error == "moment-cannot-split", "a single moment cannot be split, and it says so");
        e.action = TimelineEdit::Action::Remove;
        change = TranslateEdit(t, e, audio, voices, &performers);
        const std::string hitId = change.correction.targetId;
        ApplyChange(change, audio, manual);
        bool gone = change.ok && change.correction.on == "event";
        for (const SoundEvent& x : audio.events) {
            gone = gone && x.id != hitId;
        }
        Check(gone, "a hit removed on the timeline");
        Check(TranslateEdit(t, e, audio, voices, &performers).error == "item-not-found", "once removed, it is no longer there to edit");
        const TimelineItem* fig = t.Lane("figures")->items.empty() ? nullptr : &t.Lane("figures")->items.front();
        e = {};
        e.itemId = fig != nullptr ? fig->id : "";
        e.action = TimelineEdit::Action::Reassign;
        e.figures = { "Carita" };
        change = TranslateEdit(t, e, audio, voices, &performers);
        ApplyChange(change, audio, manual);
        Check(change.ok && change.isAssignment && manual.size() == 1 && manual[0].figures == std::vector<std::string>({ "Carita" }) && manual[0].state == "manual",
              "other figures chosen on the timeline: a person's assignment");
        e.figures = { "Muneco" };
        ApplyChange(TranslateEdit(t, e, audio, voices, &performers), audio, manual);
        Check(manual.size() == 1 && manual[0].figures == std::vector<std::string>({ "Muneco" }), "chosen again: it replaces the earlier choice, never adds a second one");
        e = {};
        e.itemId = "lead/nothing";
        Check(TranslateEdit(t, e, audio, voices, &performers).error == "item-not-found" && !TimelineEditError("item-not-found").empty(), "an item that is gone is said plainly");

        // Stable ids: after a new analysis with the corrections, the same item is there.
        AudioUnderstanding again = Hear(s);
        again.corrections = audio.corrections;
        ApplyUnderstandingCorrections(again);
        voices = UnderstandVoices({ &again });
        tin.audio = &again;
        tin.voices = &voices;
        const MultimodalTimeline t2 = BuildTimeline(tin);
        const TimelineItem* moved = nullptr;
        for (const TimelineLane& l : t2.lanes) {
            for (const TimelineItem& i : l.items) {
                moved = i.source.find('#') != std::string::npos && i.provenance == "corrected" && i.kind == "spoken" ? &i : moved;
            }
        }
        Check(moved != nullptr, "after a new analysis the corrected voice is still there, still corrected");
    }

    // ----------------------------------------------------------------- render

    std::vector<std::vector<uint8_t>> Frames(int durationMS, int frameMS, const std::function<void(int ms, std::vector<uint8_t>& frame)>& paint) {
        int channels = 0;
        for (const FigureGeometry& g : _geometry.figures) {
            for (const FigureNode& n : g.nodeMap) {
                channels = std::max(channels, n.channel + 3);
            }
        }
        std::vector<std::vector<uint8_t>> frames;
        for (int ms = 0; ms < durationMS; ms += frameMS) {
            std::vector<uint8_t> f(static_cast<size_t>(channels), 0);
            paint(ms, f);
            frames.push_back(std::move(f));
        }
        return frames;
    }

    void Light(std::vector<uint8_t>& frame, const std::string& figure, const std::vector<int>& nodes, uint8_t level) {
        const FigureGeometry* g = _geometry.Find(figure);
        if (g == nullptr) {
            return;
        }
        for (int n : nodes) {
            if (n >= 0 && n < static_cast<int>(g->nodeMap.size()) && g->nodeMap[n].index == n && g->nodeMap[n].channel >= 0) {
                const size_t c = static_cast<size_t>(g->nodeMap[n].channel);
                frame[c] = frame[c + 1] = frame[c + 2] = level;
            }
        }
    }

    std::vector<int> All(const std::string& figure) {
        std::vector<int> out;
        const FigureGeometry* g = _geometry.Find(figure);
        for (int n = 0; g != nullptr && n < g->nodes; ++n) {
            out.push_back(n);
        }
        return out;
    }

    void TestRender() {
        Title("What the render did, against what was meant");
        if (_ctx == nullptr) {
            Check(false, "the figures are known");
            return;
        }
        AudioUnderstanding u;
        u.durationMS = 12000;
        const auto act = [&](const char* id, const char* kind, int a, int b, const char* track) {
            ActivitySegment s;
            s.id = id;
            s.kind = kind;
            s.startMS = a;
            s.endMS = b;
            s.track = track;
            u.activities.push_back(s);
        };
        act("music#1", "music", 0, 12000, "");
        act("speech#1", "speech", 1000, 4000, "voice:A");
        act("instrumental#1", "instrumental", 5000, 9000, "");
        VocalUnderstanding v;
        VoiceTrack track;
        track.id = "voice:A";
        track.label = "Voz A";
        track.kind = "narrator";
        track.emission = "spoken";
        v.tracks.push_back(track);
        Intervention i;
        i.id = "voice:A#1";
        i.activityId = "speech#1";
        i.track = "voice:A";
        i.startMS = 1000;
        i.endMS = 4000;
        v.interventions.push_back(i);
        PerformanceMap map;
        PerformerAssignment speak;
        speak.source = "track:voice:A";
        speak.sourceLabel = "Voz A";
        speak.figures = { "Arbol Cara" };
        speak.function = "speak";
        speak.region = "mouth";
        PerformerAssignment play;
        play.source = "activity:instrumental";
        play.figures = { "Matriz Prueba" };
        play.function = "instrumental-lead";
        map.assignments = { speak, play };
        const FigureProfile* face = _semantics.Find("Arbol Cara");
        const FigureProfile* other = _semantics.Find("Carita");
        if (face == nullptr || other == nullptr || face->Region("mouth") == nullptr || other->Region("mouth") == nullptr) {
            Check(false, "the two faces have mouths");
            return;
        }
        const std::vector<int> mouth = face->Region("mouth")->nodes;
        const std::vector<int> half(mouth.begin(), mouth.begin() + static_cast<long>(mouth.size() / 2));
        const std::vector<int> otherHalf(mouth.begin() + static_cast<long>(mouth.size() / 2), mouth.end());
        const std::vector<int> otherMouth = other->Region("mouth")->nodes;
        const std::vector<std::string> traced = { "Arbol Cara", "Carita", "Matriz Prueba", "Arbol Grande" };
        RenderCheckOptions o;
        RenderIntention intention{ &map, &u, &v, &_semantics };

        // As meant: the tree speaks while the narrator does, the matrix plays the instrumental.
        const auto good = Frames(12000, 50, [&](int ms, std::vector<uint8_t>& f) {
            Light(f, "Arbol Grande", All("Arbol Grande"), 60);
            if (ms >= 1000 && ms < 4000) {
                Light(f, "Arbol Cara", (ms / 100) % 2 == 0 ? half : otherHalf, 255);
            }
            if (ms >= 5000 && ms < 9000) {
                Light(f, "Matriz Prueba", All("Matriz Prueba"), 200);
            }
        });
        const RenderUnderstanding fine = CompareRender(TracesFromFrames(good, 50, _geometry, _semantics, traced, o), intention, o);
        bool honoured = fine.checks.size() == 2;
        for (const IntentionCheck& c : fine.checks) {
            honoured = honoured && c.windows == 1 && c.honoured == 1;
        }
        const auto count = [](const RenderUnderstanding& r, const std::string& kind, const std::string& figure) {
            return std::count_if(r.findings.begin(), r.findings.end(), [&](const RenderFinding& f) { return f.kind == kind && (figure.empty() || f.figure == figure); });
        };
        Check(honoured, "the narrating tree's mouth moved while it spoke, the matrix played the instrumental");
        Check(count(fine, "mouth-moves-without-voice", "") == 0 && count(fine, "expected-dark", "") == 0 && count(fine, "mouth-still", "") == 0, "nothing to point at where everything was as meant");
        Check(count(fine, "figure-silent", "Carita") == 0 && count(fine, "figure-silent", "Arbol Grande") == 0, "a figure with nothing to do may stay dark: it is not pointed at");
        Check(count(fine, "region-never-lit", "Arbol Cara") == 1, "a part of a performing figure that never lit (its eyes) is pointed at, to be looked at");
        Check(fine.review == "pending" && std::any_of(fine.limits.begin(), fine.limits.end(), [](const std::string& l) { return l.rfind("no-approval", 0) == 0; }), "nothing is approved: a person decides");

        // Not as meant.
        const auto bad = Frames(12000, 50, [&](int ms, std::vector<uint8_t>& f) {
            if (ms >= 1000 && ms < 4000) {
                Light(f, "Arbol Cara", mouth, 255);
                Light(f, "Carita", (ms / 100) % 2 == 0 ? std::vector<int>(otherMouth.begin(), otherMouth.begin() + 6) : std::vector<int>(otherMouth.begin() + 6, otherMouth.end()), 255);
            }
            if (ms >= 6000 && ms < 8000) {
                Light(f, "Arbol Cara", (ms / 100) % 2 == 0 ? half : otherHalf, 255);
            }
        });
        const RenderUnderstanding wrong = CompareRender(TracesFromFrames(bad, 50, _geometry, _semantics, traced, o), intention, o);
        Check(count(wrong, "mouth-still", "Arbol Cara") == 1, "the narrator's mouth lit but still: it was meant to speak");
        Check(count(wrong, "voice-on-other-figure", "Carita") == 1, "the voice seems shown on another face");
        const auto moving = std::find_if(wrong.findings.begin(), wrong.findings.end(), [](const RenderFinding& f) { return f.kind == "mouth-moves-without-voice" && f.figure == "Arbol Cara"; });
        Check(moving != wrong.findings.end() && moving->severity == "warning" && moving->detail == "instrumental" && moving->startMS >= 5900 && moving->endMS <= 8100,
              "a mouth moving in the instrumental is a warning");
        Check(count(wrong, "expected-dark", "Matriz Prueba") == 1, "the matrix was meant to play the instrumental and stayed dark");
        Check(count(wrong, "dark-while-music", "") >= 1, "everything dark while the music plays");
        bool spaced = !wrong.captures.empty() && std::is_sorted(wrong.captures.begin(), wrong.captures.end(), [](const CaptureMoment& a, const CaptureMoment& b) { return a.timeMS < b.timeMS; });
        for (size_t k = 1; k < wrong.captures.size(); ++k) {
            spaced = spaced && wrong.captures[k].timeMS - wrong.captures[k - 1].timeMS >= o.captureSpacingMS;
        }
        Check(spaced && std::any_of(wrong.captures.begin(), wrong.captures.end(), [](const CaptureMoment& c) { return c.reason.rfind("finding:", 0) == 0; }),
              "moments to look at, in order, spaced apart, the problems among them");
        Check(!CaptureTimes(wrong).empty() && CaptureTimes(wrong).find(',') != std::string::npos, "given as times for the pictures of the render");
        const std::string text = ToJson(wrong);
        Check(json::parse(text).value("review", std::string()) == "pending" && FindingPhrase("mouth-still") != "mouth-still", "written for the review, with plain words for each problem");
        const RenderUnderstanding again = CompareRender(TracesFromFrames(bad, 50, _geometry, _semantics, traced, o), intention, o);
        Check(ToJson(again) == text, "the same render is judged the same way every time");
        RenderUnderstanding back;
        std::string readError;
        Check(FromJson(text, back, readError) && ToJson(back) == text, "written and read again for the window");

        // From a real .fseq file, as xLights writes it: a figure outside the file is not judged.
        const fs::path fseq = _root / "check.fseq";
        int fileChannels = 0;
        for (const char* name : { "Arbol Cara", "Carita", "Matriz Prueba", "Arbol Grande" }) {
            for (const FigureNode& n : _geometry.Find(name)->nodeMap) {
                fileChannels = std::max(fileChannels, n.channel + 3);
            }
        }
        {
            std::unique_ptr<FSEQFile> file(FSEQFile::createFSEQFile(fseq.string(), 2, FSEQFile::CompressionType::none));
            file->setNumFrames(static_cast<uint32_t>(bad.size()));
            file->setStepTime(50);
            file->setChannelCount(fileChannels);
            file->writeHeader();
            for (size_t k = 0; k < bad.size(); ++k) {
                file->addFrame(static_cast<uint32_t>(k), bad[k].data());
            }
            file->finalize();
        }
        RenderTraces fromFile;
        std::string error;
        const std::vector<std::string> withOutside = { "Arbol Cara", "Carita", "Matriz Prueba", "Arbol Grande", "Sin Controlador" };
        const bool read = ReadRenderTraces(fseq.string(), _geometry, _semantics, withOutside, o, fromFile, error);
        Check(read && std::find(fromFile.outside.begin(), fromFile.outside.end(), "Sin Controlador") != fromFile.outside.end() && fromFile.Figure("Sin Controlador") == nullptr,
              "a figure whose lights are not in the file (no controller) is left out, and said");
        const RenderUnderstanding fromDisk = CompareRender(fromFile, intention, o);
        Check(read && count(fromDisk, "mouth-still", "Arbol Cara") == 1 && count(fromDisk, "voice-on-other-figure", "Carita") == 1, "the file says the same as the frames it holds");
        RenderTraces none;
        Check(!ReadRenderTraces((_root / "absent.fseq").string(), _geometry, _semantics, {}, o, none, error) && error == "render-file-unreadable", "a missing render file is said");
    }

    // ---------------------------------------------------------------- sidecar

    void TestSidecar() {
        Title("The show remembers whether it used the understanding");
        GenerationSidecar s;
        s.understanding = { true, "0123456789abcdef", AUDIO_UNDERSTANDING_VERSION, 3, 1 };
        GenerationSidecar read;
        std::string error;
        Check(FromJson(ToJson(s), read, error) == SidecarStatus::Ok && read.understanding.enabled && read.understanding.audioId == "0123456789abcdef" && read.understanding.corrections == 3 &&
                  read.understanding.manualAssignments == 1,
              "written and read again");
        json old = json::parse(ToJson(s));
        old["schemaVersion"] = 2;
        old.erase("understanding");
        GenerationSidecar before;
        Check(FromJson(old.dump(), before, error) == SidecarStatus::Ok && !before.understanding.enabled && before.schemaVersion == SIDECAR_SCHEMA_VERSION,
              "a show made before this version reads as made without it");
    }

    fs::path _root;
    fs::path _fixture;
    std::unique_ptr<HeadlessRenderContext> _ctx;
    ModelCapabilities _caps;
    FigureCatalog _geometry;
    FigureSemantics _semantics;
    FigureChoices _choices;
    int _passed = 0;
    int _failed = 0;
};

} // namespace

int RunUnderstandingTest(const std::string& workDir) {
    const fs::path root = fs::path(workDir) / fmt::format("understanding-{}", std::chrono::system_clock::now().time_since_epoch().count());
    Suite suite(root);
    return suite.Run();
}

namespace {

bool Write(const fs::path& path, const std::string& text) {
    std::string error;
    return WriteTextFileAtomically(path.string(), text, error);
}

std::string Clock(int ms) {
    return fmt::format("{:02d}:{:02d}.{}", ms / 60000, (ms / 1000) % 60, (ms % 1000) / 100);
}

} // namespace

int RunUnderstandCommand(const std::string& audioPath, const std::string& stems, const std::string& outDir, const std::string& showDir, const std::string& cancelFile, bool again) {
    const auto progress = [](int percent, const char* stage) {
        printf("understand %d%% %s\n", percent, stage);
        fflush(stdout);
    };
    const auto cancel = [cancelFile]() {
        std::error_code ec;
        return !cancelFile.empty() && fs::exists(fs::path(cancelFile), ec);
    };
    UnderstandingResult heard;
    VocalUnderstanding voices;
    std::string folder;
    if (!showDir.empty()) {
        UnderstandRequest r;
        r.showDir = showDir;
        r.audioPath = audioPath;
        r.stems = ParseStemList(stems);
        r.reuse = !again;
        r.progress = progress;
        r.cancel = cancel;
        UnderstandOutcome o = UnderstandSong(r);
        heard.ok = o.ok;
        heard.cancelled = o.cancelled;
        heard.errorCode = o.errorCode;
        heard.errorDetail = o.errorDetail;
        heard.understanding = std::move(o.song.audio);
        voices = std::move(o.song.voices);
        folder = o.song.folder;
        for (const std::string& n : o.song.notes) {
            printf("understand note %s\n", n.c_str());
        }
    } else {
        heard = UnderstandAudio(audioPath, ParseStemList(stems), {}, progress, cancel);
        VocalInput vin;
        vin.audio = &heard.understanding;
        voices = UnderstandVoices(vin);
    }
    if (heard.cancelled) {
        printf("composer-understand: cancelled\n");
        return 3;
    }
    if (!heard.ok) {
        printf("composer-understand: %s %s\n", heard.errorCode.c_str(), heard.errorDetail.c_str());
        return 1;
    }
    if (!folder.empty()) {
        printf("understand folder %s\n", folder.c_str());
    }
    TimelineInput tin;
    tin.audio = &heard.understanding;
    tin.voices = &voices;
    const MultimodalTimeline timeline = BuildTimeline(tin);
    std::string report = fmt::format("{} s, {} stretches, {} moments, {} voices\n", heard.understanding.durationMS / 1000, heard.understanding.activities.size(), heard.understanding.events.size(),
                                     voices.tracks.size());
    for (const ActivitySegment& a : heard.understanding.activities) {
        report += fmt::format("  {} - {}  {}  ({:.0f}%) {}\n", Clock(a.startMS), Clock(a.endMS), ActivityPhrase(a.kind), a.confidence * 100, a.track);
    }
    for (const VoiceTrack& t : voices.tracks) {
        report += fmt::format("  {}: {} {} {:.0f} Hz, {} ms\n", t.label, t.kind, t.emission, t.medianF0, t.totalMS);
    }
    for (const std::string& l : voices.limits) {
        report += "  limit: " + l + "\n";
    }
    json lanes = json::array();
    for (const TimelineLane& l : timeline.lanes) {
        json items = json::array();
        for (const TimelineItem& i : l.items) {
            items.push_back({ { "id", i.id }, { "startMS", i.startMS }, { "endMS", i.endMS }, { "kind", i.kind }, { "confidence", i.confidence }, { "provenance", i.provenance } });
        }
        lanes.push_back({ { "lane", l.id }, { "items", items } });
    }
    const fs::path out(outDir);
    const bool ok = Write(out / "understanding.json", ToJson(heard.understanding)) && Write(out / "voices.json", ToJson(voices)) &&
                    Write(out / "timeline.json", json({ { "durationMS", timeline.durationMS }, { "lanes", lanes } }).dump(1)) && Write(out / "understanding-report.txt", report);
    printf("%s", report.c_str());
    printf("composer-understand: %s (%s)\n", ok ? "written" : "FAILED", out.string().c_str());
    return ok ? 0 : 1;
}

int RunFiguresCommand(const std::string& showDir, const std::string& outDir) {
    std::string refused;
    if (!ControllersAreInactive(showDir, refused)) {
        printf("composer-figures: RECHAZADO: %s\n", refused.c_str());
        return 2;
    }
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(showDir)) {
        printf("composer-figures: the show could not be loaded\n");
        return 1;
    }
    const ModelCapabilities caps = ExtractCapabilities(ctx, nullptr);
    const FigureCatalog figures = DescribeFigures(ctx, caps, true);
    FigureChoices choices;
    std::string text, error;
    if (ReadTextFile(FigureChoicesPath(showDir), text) && !FromJson(text, choices, error)) {
        printf("composer-figures: figures.json: %s\n", error.c_str());
    }
    const FigureSemantics found = DescribeSemantics(ctx, figures, caps, nullptr);
    const FigureSemantics semantics = ApplyFigureChoices(found, figures, &choices);
    std::string report;
    int models = 0, irregular = 0, mouths = 0, suggested = 0, preview = 0;
    for (const FigureGeometry& g : figures.figures) {
        if (g.kind != "model") {
            continue;
        }
        ++models;
        irregular += g.topology == "custom" || g.topology == "unknown" ? 1 : 0;
        preview += g.noController ? 1 : 0;
        const FigureProfile* p = semantics.Find(g.name);
        mouths += p != nullptr && p->CanSpeak() ? 1 : 0;
        suggested += p != nullptr && p->mouth == "suggested" ? 1 : 0;
        std::string regions;
        if (p != nullptr) {
            for (const FigureRegion& r : p->regions) {
                regions += fmt::format(" {}{}", r.id, r.real ? "" : "?");
            }
        }
        report += fmt::format("  {}: {} ({}), {} lights, {}x{}, {}{}; mouth {};{}\n", g.name, TopologyPhrase(g.topology), g.displayAs, g.nodes, g.bufferWi, g.bufferHt, g.orientation,
                              g.noController ? ", no controller" : "", p != nullptr ? p->mouth : "none", regions);
    }
    report = fmt::format("{} models ({} irregular, {} without controller), {} with a mouth tied to lights, {} with a mouth only suggested by a name\n", models, irregular, preview, mouths, suggested) + report;
    const fs::path out(outDir);
    // What was found before a person's choices too: the window applies the choices itself.
    const bool ok = Write(out / "figures.json", ToJson(figures, true)) && Write(out / "figure-semantics.json", ToJson(semantics)) &&
                    Write(out / "figure-semantics-found.json", ToJson(found)) && Write(out / "figures-report.txt", report);
    printf("%s", report.c_str());
    printf("composer-figures: %s (%s)\n", ok ? "written" : "FAILED", out.string().c_str());
    (void)ctx.CloseSequence();
    return ok ? 0 : 1;
}

} // namespace composer
