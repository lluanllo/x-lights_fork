#include "composer/AnalyzerTest.h"

#include "composer/SongAnalysis.h"
#include "media/AudioManager.h"
#include "render/Element.h"
#include "render/EffectLayer.h"
#include "render/Effect.h"
#include "render/HeadlessRenderContext.h"
#include "render/SequenceElements.h"
#include "render/SequenceFile.h"
#include "render/SongStructureManager.h"
#include "utils/ExternalHooks.h"

#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace composer {

namespace {

// Decided before the first run, and not to be loosened to fit a result.
// One frame of a 25 ms sequence: an onset cannot be shown any finer than that.
constexpr int ONSET_TOLERANCE_MS = 25;
// Half of the 70 ms usual in beat tracking evaluations, as the plan set for this phase.
constexpr int BEAT_TOLERANCE_MS = 35;
// The energy curve's 100 ms step, and half of it for a level measured over a window.
constexpr int SILENCE_TOLERANCE_MS = 150;
// Two steps of the half-second grid the sections are found on.
constexpr int SECTION_TOLERANCE_MS = 1000;
constexpr double MIN_BEAT_F = 0.90;
constexpr double MIN_ONSET_RECALL = 0.95;
constexpr double MIN_ONSET_PRECISION = 0.90;
constexpr double MAX_TEMPO_ERROR = 0.05;

constexpr int RATE = 44100;
constexpr double PI = 3.14159265358979323846;

// Writes its own music: small, deterministic, and free of anyone's rights.
class SyntheticSong {
public:
    explicit SyntheticSong(double seconds) {
        const size_t samples = static_cast<size_t>(seconds * RATE);
        _left.assign(samples, 0.0f);
        _right.assign(samples, 0.0f);
    }

    [[nodiscard]] double Seconds() const { return static_cast<double>(_left.size()) / RATE; }

    // A noise burst: sharp, like a stick on a drum.
    void Click(double at, double level, double decayMS = 8.0) {
        const size_t start = static_cast<size_t>(at * RATE);
        const size_t length = static_cast<size_t>(decayMS * 6 * RATE / 1000);
        for (size_t i = 0; i < length && start + i < _left.size(); ++i) {
            const double value = level * Noise() * std::exp(-static_cast<double>(i) * 1000.0 / (decayMS * RATE));
            Add(start + i, value, value);
        }
    }

    // A falling low tone, like a kick drum.
    void Kick(double at, double level) {
        const size_t start = static_cast<size_t>(at * RATE);
        const size_t length = static_cast<size_t>(0.12 * RATE);
        for (size_t i = 0; i < length && start + i < _left.size(); ++i) {
            const double t = static_cast<double>(i) / RATE;
            const double value = level * std::sin(2 * PI * (70 * t + 30 * (1 - std::exp(-t * 30)) / 30)) * std::exp(-t * 28);
            Add(start + i, value, value);
        }
    }

    // A note with a few harmonics. `attack` in seconds: long ones have no audible onset.
    void Tone(double at, double duration, double frequency, double level, double attack = 0.005, double spread = 0.0) {
        const size_t start = static_cast<size_t>(at * RATE);
        const size_t length = static_cast<size_t>(duration * RATE);
        for (size_t i = 0; i < length && start + i < _left.size(); ++i) {
            const double t = static_cast<double>(i) / RATE;
            const double envelope = std::min(1.0, t / std::max(attack, 1e-4)) * std::min(1.0, (duration - t) / 0.01);
            const double wave = std::sin(2 * PI * frequency * t) + 0.4 * std::sin(4 * PI * frequency * t) + 0.2 * std::sin(6 * PI * frequency * t);
            const double value = level * envelope * wave / 1.6;
            // A phase-inverted part is what the center-channel filter cancels.
            Add(start + i, value * (1 + spread), value * (1 - spread));
        }
    }

    // Wide noise with nothing in the center, for a mix around a centered voice.
    void WideNoise(double at, double duration, double level) {
        const size_t start = static_cast<size_t>(at * RATE);
        const size_t length = static_cast<size_t>(duration * RATE);
        for (size_t i = 0; i < length && start + i < _left.size(); ++i) {
            const double value = level * Noise();
            Add(start + i, value, -value);
        }
    }

    [[nodiscard]] bool Save(const std::string& path, bool mono = false) const {
        std::ofstream out(fs::path(path), std::ios::binary);
        const std::uint16_t channels = mono ? 1 : 2;
        const std::uint32_t dataBytes = static_cast<std::uint32_t>(_left.size() * channels * 2);
        const auto u32 = [&](std::uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
        const auto u16 = [&](std::uint16_t v) { out.write(reinterpret_cast<const char*>(&v), 2); };
        out.write("RIFF", 4);
        u32(36 + dataBytes);
        out.write("WAVEfmt ", 8);
        u32(16);
        u16(1);
        u16(channels);
        u32(RATE);
        u32(RATE * channels * 2);
        u16(static_cast<std::uint16_t>(channels * 2));
        u16(16);
        out.write("data", 4);
        u32(dataBytes);
        const auto sample = [](float v) { return static_cast<std::int16_t>(std::lround(std::max(-1.0f, std::min(1.0f, v)) * 32767.0f)); };
        for (size_t i = 0; i < _left.size(); ++i) {
            const std::int16_t l = sample(_left[i]);
            out.write(reinterpret_cast<const char*>(&l), 2);
            if (!mono) {
                const std::int16_t r = sample(_right[i]);
                out.write(reinterpret_cast<const char*>(&r), 2);
            }
        }
        return static_cast<bool>(out);
    }

private:
    void Add(size_t index, double left, double right) {
        _left[index] += static_cast<float>(left);
        _right[index] += static_cast<float>(right);
    }

    // xorshift: the same noise on every platform and every run.
    double Noise() {
        _noise ^= _noise << 13;
        _noise ^= _noise >> 17;
        _noise ^= _noise << 5;
        return static_cast<double>(_noise) / 2147483648.0 - 1.0;
    }

    std::vector<float> _left;
    std::vector<float> _right;
    std::uint32_t _noise = 2463534242u;
};

std::vector<int> TimesOf(const SongAnalysis& analysis, MusicalEventType type) {
    std::vector<int> times;
    for (const MusicalEvent& event : analysis.events) {
        if (event.type == type) {
            times.push_back(event.timeMS);
        }
    }
    return times;
}

struct Match {
    double precision = 0;
    double recall = 0;
    double f = 0;
    double medianError = 0; // found minus truth, in ms
};

Match Compare(const std::vector<int>& found, const std::vector<int>& truth, int tolerance) {
    Match match;
    std::vector<bool> used(found.size(), false);
    std::vector<int> errors;
    for (int t : truth) {
        int best = -1;
        int bestDistance = tolerance + 1;
        for (size_t f = 0; f < found.size(); ++f) {
            const int distance = std::abs(found[f] - t);
            if (!used[f] && distance < bestDistance) {
                best = static_cast<int>(f);
                bestDistance = distance;
            }
        }
        if (best >= 0) {
            used[best] = true;
            errors.push_back(found[best] - t);
        }
    }
    const double hits = static_cast<double>(errors.size());
    match.precision = found.empty() ? 0 : hits / static_cast<double>(found.size());
    match.recall = truth.empty() ? 0 : hits / static_cast<double>(truth.size());
    match.f = match.precision + match.recall <= 0 ? 0 : 2 * match.precision * match.recall / (match.precision + match.recall);
    if (!errors.empty()) {
        std::nth_element(errors.begin(), errors.begin() + static_cast<std::ptrdiff_t>(errors.size() / 2), errors.end());
        match.medianError = errors[errors.size() / 2];
    }
    return match;
}

std::vector<int> SectionStarts(const SongAnalysis& analysis) {
    std::vector<int> starts;
    for (const SongSection& section : analysis.sections) {
        starts.push_back(section.startMS);
    }
    return starts;
}

const SongSection* SectionAt(const SongAnalysis& analysis, int timeMS) {
    for (const SongSection& section : analysis.sections) {
        if (timeMS >= section.startMS && timeMS < section.endMS) {
            return &section;
        }
    }
    return nullptr;
}

bool HasKind(const SongAnalysis& analysis, SectionKind kind) {
    return std::any_of(analysis.sections.begin(), analysis.sections.end(), [&](const SongSection& s) { return s.kind == kind; });
}

bool HasWarning(const SongAnalysis& analysis, const std::string& warning) {
    return std::find(analysis.warnings.begin(), analysis.warnings.end(), warning) != analysis.warnings.end();
}

double MeanOver(const SongAnalysis& analysis, int fromMS, int toMS, double EnergyPoint::*member) {
    double sum = 0;
    int count = 0;
    for (const EnergyPoint& point : analysis.energy) {
        if (point.timeMS >= fromMS && point.timeMS < toMS) {
            sum += point.*member;
            ++count;
        }
    }
    return count == 0 ? 0 : sum / count;
}

// Lets xLights' own importer run without a window: the headless context does not make
// timing tracks, and the importer needs them from its context.
class TimingImportContext : public HeadlessRenderContext {
public:
    TimingElement* AddTimingElement(const std::string& name, const std::string& /*subType*/ = "") override {
        Element* element = GetSequenceElements().AddElement(name, "timing", true, false, false, false, false);
        if (element != nullptr && element->GetEffectLayerCount() == 0) {
            element->AddEffectLayer();
        }
        return dynamic_cast<TimingElement*>(element);
    }
};

class Suite {
public:
    explicit Suite(const fs::path& root) :
        _root(root) {}

    int Run() {
        TestA();
        TestB();
        TestC();
        TestD();
        TestE();
        TestF();
        TestVocals();
        TestBadFiles();
        TestCacheAndStability();
        TestCorrections();
        TestCancelAndThread();
        TestPlayerUntouched();
        TestSnapshot();
        TestExport();
        TestReference();
        printf("composer-analyzer-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
        printf("composer-analyzer-test: real music quality: %s\n", MUSIC_QUALITY_NOT_VALIDATED);
        return _failed == 0 ? 0 : 1;
    }

private:
    void Check(bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        ok ? ++_passed : ++_failed;
    }

    void Title(const char* title) {
        printf("=== %s\n", title);
        fflush(stdout);
    }

    std::string PathOf(const std::string& name) const {
        return (_root / name).string();
    }

    // What every analysis must hold, whatever the audio: this is what later stages
    // rely on without checking.
    void CheckInvariants(const SongAnalysis& analysis, const char* name) {
        bool times = true;
        bool ranges = true;
        bool sorted = true;
        int previous = -1;
        for (const MusicalEvent& event : analysis.events) {
            times = times && event.timeMS >= 0 && event.timeMS <= analysis.durationMS && event.durationMS >= 0 && event.timeMS + event.durationMS <= analysis.durationMS;
            ranges = ranges && event.confidence >= 0 && event.confidence <= 1 && event.intensity >= 0 && event.intensity <= 1 && !event.source.empty();
            sorted = sorted && event.timeMS >= previous;
            previous = event.timeMS;
        }
        bool contiguous = !analysis.sections.empty() && analysis.sections.front().startMS == 0 && analysis.sections.back().endMS == analysis.durationMS;
        for (size_t s = 0; s + 1 < analysis.sections.size(); ++s) {
            contiguous = contiguous && analysis.sections[s].endMS == analysis.sections[s + 1].startMS && analysis.sections[s].endMS > analysis.sections[s].startMS;
        }
        Check(times, fmt::format("{}: event times are valid integers inside the song", name));
        Check(ranges, fmt::format("{}: intensity and confidence in 0-1, and a source on every event", name));
        Check(sorted, fmt::format("{}: events in time order", name));
        Check(contiguous, fmt::format("{}: sections cover the song without gaps", name));
    }

    AnalysisResult Analyze(const SyntheticSong& song, const std::string& name, const AnalysisOptions& options = {}) {
        const std::string path = PathOf(name + ".wav");
        if (!song.Save(path)) {
            Check(false, fmt::format("writing {}", path));
        }
        AnalysisResult result = AnalyzeSong(path, options);
        Check(result.status == AnalysisStatus::Ok, fmt::format("{}: analyzed ({})", name, result.errorCode));
        if (result.status == AnalysisStatus::Ok) {
            CheckInvariants(result.analysis, name.c_str());
        }
        return result;
    }

    void TestA() {
        Title("A. Clicks at 120 BPM, with accented downbeats");
        constexpr double BEAT = 0.5;
        constexpr double START = 0.5;
        SyntheticSong song(32);
        std::vector<int> beats, downbeats;
        for (int i = 0; START + i * BEAT < 31; ++i) {
            const double at = START + i * BEAT;
            beats.push_back(static_cast<int>(std::lround(at * 1000)));
            if (i % 4 == 0) {
                downbeats.push_back(beats.back());
                song.Kick(at, 0.9);
                song.Click(at, 0.8);
            } else {
                song.Click(at, 0.35);
            }
        }
        const AnalysisResult result = Analyze(song, "a_clicks");
        const SongAnalysis& a = result.analysis;
        Check(std::fabs(a.globalBpm - 120) <= 1, fmt::format("BPM {} (120)", a.globalBpm));
        Check(a.tempoIsStable, "tempo reported as stable");
        const Match beat = Compare(TimesOf(a, MusicalEventType::Beat), beats, BEAT_TOLERANCE_MS);
        Check(beat.f >= MIN_BEAT_F, fmt::format("beats: F {:.3f}, median error {} ms, tolerance {} ms", beat.f, beat.medianError, BEAT_TOLERANCE_MS));
        const Match onset = Compare(TimesOf(a, MusicalEventType::Onset), beats, ONSET_TOLERANCE_MS);
        Check(onset.recall >= MIN_ONSET_RECALL && onset.precision >= MIN_ONSET_PRECISION, fmt::format("onsets: recall {:.3f}, precision {:.3f}, median error {} ms, tolerance {} ms", onset.recall, onset.precision, onset.medianError, ONSET_TOLERANCE_MS));
        const Match down = Compare(TimesOf(a, MusicalEventType::Downbeat), downbeats, BEAT_TOLERANCE_MS);
        Check(down.f >= MIN_BEAT_F, fmt::format("downbeats: F {:.3f}, {} of {}, bar of {}, confidence {}", down.f, TimesOf(a, MusicalEventType::Downbeat).size(), downbeats.size(), a.beatsPerBar, a.barConfidence));

        Title("A2. Same clicks with no accent: the bar cannot be known");
        SyntheticSong flat(32);
        for (int i = 0; START + i * BEAT < 31; ++i) {
            flat.Click(START + i * BEAT, 0.5);
        }
        const AnalysisResult even = Analyze(flat, "a2_even");
        Check(std::fabs(even.analysis.globalBpm - 120) <= 1, fmt::format("BPM {} (120)", even.analysis.globalBpm));
        Check(TimesOf(even.analysis, MusicalEventType::Downbeat).empty() && HasWarning(even.analysis, "downbeat-undetermined"), fmt::format("no downbeats invented ({})", TimesOf(even.analysis, MusicalEventType::Downbeat).size()));

        Title("A3. The first downbeat is not the first beat");
        SyntheticSong shifted(32);
        std::vector<int> shiftedDownbeats;
        for (int i = 0; START + i * BEAT < 31; ++i) {
            const double at = START + i * BEAT;
            if (i % 4 == 2) {
                shiftedDownbeats.push_back(static_cast<int>(std::lround(at * 1000)));
                shifted.Kick(at, 0.9);
                shifted.Click(at, 0.8);
            } else {
                shifted.Click(at, 0.35);
            }
        }
        const AnalysisResult late = Analyze(shifted, "a3_shifted");
        const Match lateDown = Compare(TimesOf(late.analysis, MusicalEventType::Downbeat), shiftedDownbeats, BEAT_TOLERANCE_MS);
        Check(lateDown.f >= MIN_BEAT_F, fmt::format("downbeats on the third beat: F {:.3f}", lateDown.f));

        Title("A4. A bar of three");
        SyntheticSong waltz(32);
        std::vector<int> waltzDownbeats;
        for (int i = 0; START + i * BEAT < 31; ++i) {
            const double at = START + i * BEAT;
            if (i % 3 == 0) {
                waltzDownbeats.push_back(static_cast<int>(std::lround(at * 1000)));
                waltz.Kick(at, 0.9);
                waltz.Click(at, 0.8);
            } else {
                waltz.Click(at, 0.35);
            }
        }
        const AnalysisResult three = Analyze(waltz, "a4_waltz");
        const Match threeDown = Compare(TimesOf(three.analysis, MusicalEventType::Downbeat), waltzDownbeats, BEAT_TOLERANCE_MS);
        Check(three.analysis.beatsPerBar == 3 && threeDown.f >= MIN_BEAT_F, fmt::format("bar of {}, downbeats F {:.3f}", three.analysis.beatsPerBar, threeDown.f));
    }

    void TestB() {
        Title("B. Tempo rising from 90 to 140 BPM");
        constexpr double DURATION = 60;
        SyntheticSong song(DURATION + 2);
        std::vector<int> beats;
        std::vector<std::pair<int, double>> truthBpm;
        double at = 1.0;
        while (at < DURATION) {
            const double bpm = 90 + 50 * std::min(1.0, at / DURATION);
            beats.push_back(static_cast<int>(std::lround(at * 1000)));
            truthBpm.push_back({ beats.back(), bpm });
            song.Click(at, 0.6);
            at += 60.0 / bpm;
        }
        const AnalysisResult result = Analyze(song, "b_rising");
        const SongAnalysis& a = result.analysis;
        const Match beat = Compare(TimesOf(a, MusicalEventType::Beat), beats, BEAT_TOLERANCE_MS);
        Check(beat.f >= MIN_BEAT_F, fmt::format("beats: F {:.3f}, median error {} ms, tolerance {} ms", beat.f, beat.medianError, BEAT_TOLERANCE_MS));
        Check(!a.tempoIsStable, "tempo reported as not stable");
        std::vector<double> errors;
        for (const TempoPoint& point : a.tempo) {
            const double truth = 90 + 50 * std::min(1.0, point.timeMS / (DURATION * 1000));
            errors.push_back(std::fabs(point.bpm - truth) / truth);
        }
        std::sort(errors.begin(), errors.end());
        const double median = errors.empty() ? 1 : errors[errors.size() / 2];
        Check(!errors.empty() && median <= MAX_TEMPO_ERROR, fmt::format("local tempo: median error {:.1f}% over {} points", median * 100, errors.size()));

        Title("B2. Sudden tempo change from 100 to 150 BPM");
        SyntheticSong jump(52);
        std::vector<int> jumpBeats;
        at = 1.0;
        while (at < 50) {
            jumpBeats.push_back(static_cast<int>(std::lround(at * 1000)));
            jump.Click(at, 0.6);
            at += at < 25 ? 0.6 : 0.4;
        }
        const AnalysisResult sudden = Analyze(jump, "b2_jump");
        const Match jumped = Compare(TimesOf(sudden.analysis, MusicalEventType::Beat), jumpBeats, BEAT_TOLERANCE_MS);
        Check(jumped.f >= MIN_BEAT_F, fmt::format("beats: F {:.3f}", jumped.f));
    }

    static void Music(SyntheticSong& song, double from, double to, std::vector<int>* beats = nullptr) {
        for (double at = from; at < to - 0.05; at += 0.5) {
            song.Click(at, 0.5);
            song.Tone(at, 0.4, 220, 0.2);
            if (beats != nullptr) {
                beats->push_back(static_cast<int>(std::lround(at * 1000)));
            }
        }
    }

    void TestC() {
        Title("C. Silence, music, pause, music, silence");
        SyntheticSong song(42);
        Music(song, 3, 18);
        Music(song, 23, 38);
        const AnalysisResult result = Analyze(song, "c_silences");
        const SongAnalysis& a = result.analysis;
        Check(std::abs(a.musicStartMS - 3000) <= SILENCE_TOLERANCE_MS, fmt::format("music starts at {} ms (3000)", a.musicStartMS));
        Check(std::abs(a.musicEndMS - 38000) <= SILENCE_TOLERANCE_MS + 400, fmt::format("music ends at {} ms (38000, last note at 37900)", a.musicEndMS));
        std::vector<const MusicalEvent*> silences;
        for (const MusicalEvent& event : a.events) {
            if (event.type == MusicalEventType::Silence) {
                silences.push_back(&event);
            }
        }
        Check(silences.size() == 3, fmt::format("3 silences ({})", silences.size()));
        if (silences.size() == 3) {
            Check(silences[0]->timeMS == 0 && std::abs(silences[0]->durationMS - 3000) <= SILENCE_TOLERANCE_MS, fmt::format("initial silence: {} ms", silences[0]->durationMS));
            Check(std::abs(silences[1]->timeMS - 18000) <= SILENCE_TOLERANCE_MS + 400 && std::abs(silences[1]->timeMS + silences[1]->durationMS - 23000) <= SILENCE_TOLERANCE_MS, fmt::format("pause: {} to {} ms", silences[1]->timeMS, silences[1]->timeMS + silences[1]->durationMS));
            Check(silences[2]->timeMS + silences[2]->durationMS == a.durationMS, "final silence reaches the end");
        }
        const auto inSilence = [&](int t) { return t < 2900 || (t >= 18500 && t < 22900) || t >= 38500; };
        const auto beats = TimesOf(a, MusicalEventType::Beat);
        const auto onsets = TimesOf(a, MusicalEventType::Onset);
        Check(std::none_of(beats.begin(), beats.end(), inSilence), "no beats in the silences");
        Check(std::none_of(onsets.begin(), onsets.end(), inSilence), "no onsets in the silences");
        Check(std::any_of(beats.begin(), beats.end(), [](int t) { return t > 23000 && t < 25000; }), "beats again after the pause");
        Check(MeanOver(a, 500, 2500, &EnergyPoint::total) == 0 && MeanOver(a, 19000, 22500, &EnergyPoint::total) == 0, "energy is zero in the silences");
        Check(MeanOver(a, 5000, 15000, &EnergyPoint::total) > 0.3, fmt::format("energy in the music: {:.3f}", MeanOver(a, 5000, 15000, &EnergyPoint::total)));
        Check(TimesOf(a, MusicalEventType::Ending).size() == 1, "one ending event");
    }

    void TestD() {
        Title("D. Changes of timbre and of intensity");
        SyntheticSong song(62);
        const auto pattern = [&](double from, double to, double frequency, double level) {
            for (double at = from; at < to - 0.05; at += 0.5) {
                song.Tone(at, 0.45, frequency, level);
            }
        };
        pattern(1, 16, 80, 0.6);
        pattern(16, 31, 800, 0.6);
        pattern(31, 46, 6000, 0.6);
        pattern(46, 61, 800, 0.15);
        const AnalysisResult result = Analyze(song, "d_timbre");
        const SongAnalysis& a = result.analysis;
        const auto band = [&](int from, int to) { return std::array<double, 3>{ MeanOver(a, from, to, &EnergyPoint::low), MeanOver(a, from, to, &EnergyPoint::mid), MeanOver(a, from, to, &EnergyPoint::high) }; };
        const auto bass = band(3000, 15000);
        const auto mids = band(18000, 30000);
        const auto highs = band(33000, 45000);
        Check(bass[0] > bass[1] && bass[0] > bass[2], fmt::format("bass fragment: low {:.3f}, mid {:.3f}, high {:.3f}", bass[0], bass[1], bass[2]));
        Check(mids[1] > mids[0] && mids[1] > mids[2], fmt::format("mid fragment: low {:.3f}, mid {:.3f}, high {:.3f}", mids[0], mids[1], mids[2]));
        Check(highs[2] > highs[0] && highs[2] > highs[1], fmt::format("high fragment: low {:.3f}, mid {:.3f}, high {:.3f}", highs[0], highs[1], highs[2]));
        const double loud = MeanOver(a, 18000, 30000, &EnergyPoint::rms);
        const double quiet = MeanOver(a, 48000, 60000, &EnergyPoint::rms);
        Check(quiet > 0 && std::fabs(loud / quiet - 4.0) <= 1.0, fmt::format("a quarter of the level measures {:.2f} times less (4)", quiet > 0 ? loud / quiet : 0));
        const Match boundaries = Compare(SectionStarts(a), { 16000, 31000, 46000 }, SECTION_TOLERANCE_MS);
        Check(boundaries.recall >= 0.99, fmt::format("3 boundaries found ({:.0f}%), median error {} ms, tolerance {} ms; {} sections", boundaries.recall * 100, boundaries.medianError, SECTION_TOLERANCE_MS, a.sections.size()));
        Check(!HasKind(a, SectionKind::Chorus), "no chorus without repetition");
        Check(HasKind(a, SectionKind::Unknown), "Unknown is used");
    }

    static void MotifA(SyntheticSong& song, double from, double to) {
        const double notes[] = { 261.6, 329.6, 392.0, 523.3 };
        int i = 0;
        for (double at = from; at < to - 0.05; at += 0.25, ++i) {
            song.Tone(at, 0.22, notes[i % 4], 0.3);
            if (i % 2 == 0) {
                song.Kick(at, 0.5);
            }
        }
    }

    static void MotifB(SyntheticSong& song, double from, double to) {
        const double notes[] = { 440.0, 349.2, 293.7 };
        int i = 0;
        for (double at = from; at < to - 0.05; at += 0.75, ++i) {
            song.Tone(at, 0.7, notes[i % 3], 0.22);
            song.Tone(at, 0.7, notes[i % 3] * 3, 0.1);
        }
    }

    static void Pad(SyntheticSong& song, double from, double to, double level) {
        song.Tone(from, to - from, 110, level, 2.0);
        song.Tone(from, to - from, 164.8, level, 2.0);
    }

    void TestE() {
        Title("E. Intro, A, B, A, ending");
        SyntheticSong song(87);
        Pad(song, 1, 16, 0.08);
        MotifA(song, 16, 36);
        MotifB(song, 36, 56);
        MotifA(song, 56, 76);
        Pad(song, 76, 86, 0.06);
        const AnalysisResult result = Analyze(song, "e_repeats");
        const SongAnalysis& a = result.analysis;
        const Match boundaries = Compare(SectionStarts(a), { 16000, 36000, 56000, 76000 }, SECTION_TOLERANCE_MS);
        Check(boundaries.recall >= 0.99, fmt::format("4 boundaries found ({:.0f}%), median error {} ms, tolerance {} ms; {} sections", boundaries.recall * 100, boundaries.medianError, SECTION_TOLERANCE_MS, a.sections.size()));
        const SongSection* first = SectionAt(a, 26000);
        const SongSection* middle = SectionAt(a, 46000);
        const SongSection* second = SectionAt(a, 66000);
        const bool have = first != nullptr && middle != nullptr && second != nullptr;
        Check(have && first->repeatGroup >= 0 && first->repeatGroup == second->repeatGroup, fmt::format("A repeats: groups {} and {}", have ? first->repeatGroup : -9, have ? second->repeatGroup : -9));
        Check(have && middle->repeatGroup != first->repeatGroup, fmt::format("B is not A: group {}", have ? middle->repeatGroup : -9));
        for (const SongSection& section : a.sections) {
            printf("         %d-%d ms %s group %d energy %.3f confidence %.3f\n", section.startMS, section.endMS, SectionKindId(section.kind), section.repeatGroup, section.energy, section.confidence);
        }
        for (const SongSection& section : a.sections) {
            if (section.kind == SectionKind::Chorus) {
                Check(section.confidence <= 0.5, "a chorus without lyrics keeps a modest confidence");
                break;
            }
        }
    }

    void TestF() {
        Title("F. Irregular music with no conventional form");
        SyntheticSong song(72);
        // Fixed gaps, chosen so that no two neighbours suggest a common period.
        const double gaps[] = { 0.31, 0.83, 0.47, 0.19, 0.71, 0.59, 0.23, 0.97, 0.37, 0.67, 0.29, 0.53, 0.89, 0.41, 0.61, 0.17, 0.79 };
        double at = 1;
        int g = 0;
        while (at < 21) {
            song.Click(at, 0.3 + 0.05 * (g % 7));
            at += gaps[g % 17];
            ++g;
        }
        Pad(song, 21, 36, 0.2);
        for (double t = 36; t < 46; t += 0.03) {
            song.Click(t, 0.5, 20);
        }
        const double cluster[] = { 311.1, 466.2, 622.3, 987.8 };
        for (int i = 0; i < 4; ++i) {
            song.Tone(46, 25, cluster[i], 0.15, 1.5);
        }
        const AnalysisResult result = Analyze(song, "f_irregular");
        const SongAnalysis& a = result.analysis;
        Check(!HasKind(a, SectionKind::Chorus), "no chorus invented");
        Check(!HasKind(a, SectionKind::Verse), "no verse invented");
        Check(HasKind(a, SectionKind::Unknown), "Unknown is used");
        Check(a.sections.size() >= 3, fmt::format("abrupt changes split it: {} sections", a.sections.size()));
        const auto downbeats = TimesOf(a, MusicalEventType::Downbeat);
        Check(downbeats.empty() || a.barConfidence < 0.6, fmt::format("no firm bar: {} downbeats, confidence {}", downbeats.size(), a.barConfidence));
        double softConfidence = 0;
        int softBeats = 0;
        for (const MusicalEvent& event : a.events) {
            if (event.type == MusicalEventType::Beat && event.timeMS >= 24000 && event.timeMS < 34000) {
                softConfidence += event.confidence;
                ++softBeats;
            }
        }
        const double softMean = softBeats == 0 ? 0 : softConfidence / softBeats;
        Check(softMean < 0.5, fmt::format("a sustained pad has no firm pulse: {} beats, mean confidence {:.3f}", softBeats, softMean));
        Check(!a.tempoIsStable || a.bpmConfidence < 0.5, fmt::format("tempo not claimed as firm: stable {}, confidence {}", a.tempoIsStable, a.bpmConfidence));
    }

    void TestVocals() {
        Title("Voice estimate: a hint, and only with evidence");
        SyntheticSong song(62);
        for (double at = 1; at < 61; at += 0.5) {
            song.WideNoise(at, 0.4, 0.06);
        }
        const std::vector<std::pair<int, int>> sung = { { 10000, 20000 }, { 35000, 50000 } };
        for (const auto& [from, to] : sung) {
            for (double at = from / 1000.0; at < to / 1000.0; at += 0.6) {
                song.Tone(at, 0.55, 300, 0.5);
            }
        }
        const AnalysisResult result = Analyze(song, "v_center");
        const SongAnalysis& a = result.analysis;
        int overlap = 0;
        int truth = 0;
        for (const auto& [from, to] : sung) {
            truth += to - from;
            for (const VocalSegment& segment : a.vocalSegments) {
                overlap += std::max(0, std::min(to, segment.endMS) - std::max(from, segment.startMS));
            }
        }
        Check(!a.vocalSegments.empty() && overlap * 10 >= truth * 7, fmt::format("centered part found: {} segments cover {}% of it", a.vocalSegments.size(), truth > 0 ? overlap * 100 / truth : 0));
        Check(std::all_of(a.vocalSegments.begin(), a.vocalSegments.end(), [](const VocalSegment& s) { return s.confidence <= 0.3 && !s.method.empty(); }), "low confidence, and the method is recorded");
        Check(HasWarning(a, "vocal-estimate-low-confidence"), "the warning is explicit");

        SyntheticSong sameChannels(32);
        Music(sameChannels, 1, 31);
        const AnalysisResult identical = Analyze(sameChannels, "v_identical");
        Check(identical.analysis.vocalSegments.empty() && TimesOf(identical.analysis, MusicalEventType::VocalStart).empty(), "identical channels give no voice events");
        Check(HasWarning(identical.analysis, "vocal-estimate-unavailable-no-stereo-width"), "and say why");
        Check(!HasKind(identical.analysis, SectionKind::Instrumental), "and nothing is called instrumental");

        const std::string monoPath = PathOf("v_mono.wav");
        Check(sameChannels.Save(monoPath, true), "writing a mono file");
        const AnalysisResult mono = AnalyzeSong(monoPath);
        Check(mono.status == AnalysisStatus::Ok && mono.analysis.channels == 1, fmt::format("a mono WAV loads ({} channel)", mono.analysis.channels));
        Check(mono.analysis.vocalSegments.empty() && HasWarning(mono.analysis, "vocal-estimate-unavailable-mono"), "and gets no voice estimate");
        Check(std::fabs(mono.analysis.globalBpm - 120) <= 1, fmt::format("and still has a tempo: {} BPM", mono.analysis.globalBpm));
    }

    void TestBadFiles() {
        Title("Files that cannot be analyzed");
        AnalysisResult result = AnalyzeSong(PathOf("does_not_exist.wav"));
        Check(result.status == AnalysisStatus::Failed && result.errorCode == "ANALYZER_FILE_NOT_FOUND", fmt::format("missing file: {}", result.errorCode));
        {
            std::ofstream empty(fs::path(PathOf("empty.wav")), std::ios::binary);
        }
        result = AnalyzeSong(PathOf("empty.wav"));
        Check(result.status == AnalysisStatus::Failed && result.errorCode == "ANALYZER_FILE_EMPTY", fmt::format("empty file: {}", result.errorCode));
        {
            std::ofstream garbage(fs::path(PathOf("garbage.wav")), std::ios::binary);
            for (int i = 0; i < 20000; ++i) {
                garbage.put(static_cast<char>((i * 31 + 7) & 0xFF));
            }
        }
        result = AnalyzeSong(PathOf("garbage.wav"));
        Check(result.status == AnalysisStatus::Failed && result.errorCode == "ANALYZER_UNSUPPORTED_AUDIO", fmt::format("not audio: {}", result.errorCode));
        {
            std::ofstream text(fs::path(PathOf("notes.mp3")), std::ios::binary);
            text << "this is a text file with the wrong extension\n";
        }
        result = AnalyzeSong(PathOf("notes.mp3"));
        Check(result.status == AnalysisStatus::Failed && !result.errorCode.empty(), fmt::format("text named .mp3: {}", result.errorCode));

        SyntheticSong tooShort(1);
        Check(tooShort.Save(PathOf("short.wav")), "writing a short file");
        result = AnalyzeSong(PathOf("short.wav"));
        Check(result.status == AnalysisStatus::Failed && result.errorCode == "ANALYZER_AUDIO_TOO_SHORT", fmt::format("one second: {}", result.errorCode));

        AnalysisOptions badOptions;
        badOptions.energyIntervalMS = 0;
        result = AnalyzeSong(PathOf("a_clicks.wav"), badOptions);
        Check(result.status == AnalysisStatus::Failed && result.errorCode == "ANALYZER_INVALID_OPTIONS", fmt::format("invalid options: {}", result.errorCode));

        SyntheticSong silent(10);
        const AnalysisResult quiet = Analyze(silent, "all_silent");
        const SongAnalysis& a = quiet.analysis;
        Check(HasWarning(a, "no-musical-content"), "digital silence is reported");
        Check(TimesOf(a, MusicalEventType::Beat).empty() && TimesOf(a, MusicalEventType::Onset).empty(), "and has no beats or onsets");
        Check(a.sections.size() == 1 && a.sections[0].kind == SectionKind::Unknown, "and one Unknown section");
    }

    void TestCacheAndStability() {
        Title("Cache and serialization");
        const std::string audio = PathOf("cache.wav");
        const std::string json = PathOf("cache.wav.composer-analysis.json");
        SyntheticSong song(20);
        Music(song, 1, 19);
        Check(song.Save(audio), "writing the song");
        const std::string audioHashBefore = Sha256OfFile(audio);
        Check(audioHashBefore.size() == 64, "SHA-256 of the file");

        const AnalysisResult first = AnalyzeSongCached(audio, json);
        Check(first.status == AnalysisStatus::Ok && !first.reusedCache && FileExists(json), "first run writes the file");
        std::error_code ec;
        Check(!fs::exists(json + ".composer-tmp", ec), "no temporary left");
        std::string written;
        {
            std::ifstream in(fs::path(json), std::ios::binary);
            written.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const AnalysisResult second = AnalyzeSongCached(audio, json);
        Check(second.status == AnalysisStatus::Ok && second.reusedCache, "second run reuses it");
        Check(ToJson(second.analysis) == written, "what was read serializes to the same bytes");

        const AnalysisResult again = AnalyzeSong(audio);
        Check(ToJson(again.analysis) == written, "a new analysis of the same file gives the same bytes");

        AnalysisOptions other;
        other.onsetSensitivity = 3.0;
        const AnalysisResult changedOptions = AnalyzeSongCached(audio, json, other);
        Check(changedOptions.status == AnalysisStatus::Ok && !changedOptions.reusedCache, "other options analyze again");
        const AnalysisResult restored = AnalyzeSongCached(audio, json);
        Check(!restored.reusedCache, "and going back does too");

        SyntheticSong edited(20);
        Music(edited, 2, 18);
        Check(edited.Save(audio), "changing the audio");
        const AnalysisResult changedAudio = AnalyzeSongCached(audio, json);
        Check(changedAudio.status == AnalysisStatus::Ok && !changedAudio.reusedCache, "changed audio invalidates the cache");
        Check(changedAudio.analysis.audioHash != audioHashBefore, "and records the new hash");

        const std::string damaged = PathOf("damaged.json");
        {
            std::ofstream out(fs::path(damaged), std::ios::binary);
            out << "{ this is not json";
        }
        const AnalysisResult bad = AnalyzeSongCached(audio, damaged);
        Check(bad.status == AnalysisStatus::Failed && bad.errorCode == "ANALYZER_CACHE_UNREADABLE", fmt::format("a damaged file: {}", bad.errorCode));
        std::string after;
        {
            std::ifstream in(fs::path(damaged), std::ios::binary);
            after.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        Check(after == "{ this is not json", "is left as it was");
        SongAnalysis wrongTypes;
        std::string error;
        Check(!FromJson("{\"schemaVersion\":\"one\"}", wrongTypes, error), "wrong types are an error and not a crash");
    }

    void TestCorrections() {
        Title("Manual corrections survive a new analysis");
        const std::string audio = PathOf("corrections.wav");
        const std::string json = PathOf("corrections.json");
        SyntheticSong song(30);
        Music(song, 1, 29);
        Check(song.Save(audio), "writing the song");
        const AnalysisResult first = AnalyzeSongCached(audio, json);
        SongAnalysis corrected = first.analysis;
        const auto beats = TimesOf(corrected, MusicalEventType::Beat);
        if (beats.size() < 8) {
            Check(false, "enough beats to correct");
            return;
        }
        const int removed = beats[2];
        const int moved = beats[4];
        const int added = 28500;
        const int confirmed = beats[6];
        corrected.corrections.events.push_back({ EventCorrection::Action::Remove, MusicalEventType::Beat, removed, removed, 0, true });
        corrected.corrections.events.push_back({ EventCorrection::Action::Move, MusicalEventType::Beat, moved, moved + 100, 0, true });
        corrected.corrections.events.push_back({ EventCorrection::Action::Add, MusicalEventType::Climax, -1, added, 500, true });
        corrected.corrections.events.push_back({ EventCorrection::Action::Confirm, MusicalEventType::Beat, confirmed, confirmed, 0, false });
        corrected.corrections.sections.push_back({ 5000, 12000, SectionKind::Chorus, true });
        corrected.corrections.bar = { beats[1], 4, true };
        ApplyCorrections(corrected);
        const std::string once = ToJson(corrected);
        ApplyCorrections(corrected);
        Check(ToJson(corrected) == once, "applying them twice changes nothing");

        const auto hasBeatAt = [](const SongAnalysis& a, int time, Provenance provenance) {
            return std::any_of(a.events.begin(), a.events.end(), [&](const MusicalEvent& e) { return e.type == MusicalEventType::Beat && e.timeMS == time && e.provenance == provenance; });
        };
        const auto check = [&](const SongAnalysis& a, const char* when) {
            const auto now = TimesOf(a, MusicalEventType::Beat);
            Check(std::find(now.begin(), now.end(), removed) == now.end(), fmt::format("{}: the removed beat stays removed", when));
            Check(hasBeatAt(a, moved + 100, Provenance::Locked), fmt::format("{}: the moved beat stays, locked", when));
            Check(hasBeatAt(a, confirmed, Provenance::Confirmed), fmt::format("{}: the confirmed beat is marked", when));
            Check(hasBeatAt(a, beats[0], Provenance::Detected), fmt::format("{}: the others remain detected", when));
            Check(std::any_of(a.events.begin(), a.events.end(), [&](const MusicalEvent& e) { return e.type == MusicalEventType::Climax && e.timeMS == added && e.provenance == Provenance::Locked; }), fmt::format("{}: the added climax stays", when));
            const SongSection* chorus = SectionAt(a, 8000);
            Check(chorus != nullptr && chorus->kind == SectionKind::Chorus && chorus->provenance == Provenance::Locked && chorus->startMS == 5000 && chorus->endMS == 12000, fmt::format("{}: the marked section stays", when));
            Check(a.beatsPerBar == 4 && std::any_of(a.events.begin(), a.events.end(), [&](const MusicalEvent& e) { return e.type == MusicalEventType::Downbeat && e.timeMS == beats[1] && e.provenance == Provenance::Locked; }), fmt::format("{}: the first downbeat stays", when));
            bool contiguous = a.sections.front().startMS == 0 && a.sections.back().endMS == a.durationMS;
            for (size_t s = 0; s + 1 < a.sections.size(); ++s) {
                contiguous = contiguous && a.sections[s].endMS == a.sections[s + 1].startMS;
            }
            Check(contiguous, fmt::format("{}: sections still cover the song", when));
        };
        check(corrected, "applied");
        {
            std::ofstream out(fs::path(json), std::ios::binary | std::ios::trunc);
            out << ToJson(corrected);
        }
        AnalysisOptions other;
        other.minOnsetIntensity = 0.1;
        const AnalysisResult reanalyzed = AnalyzeSongCached(audio, json, other);
        Check(reanalyzed.status == AnalysisStatus::Ok && !reanalyzed.reusedCache, "a new analysis ran");
        check(reanalyzed.analysis, "after a new analysis");

        SyntheticSong edited(30);
        Music(edited, 1, 28);
        Check(edited.Save(audio), "changing the audio");
        const AnalysisResult otherAudio = AnalyzeSongCached(audio, json, other);
        Check(otherAudio.status == AnalysisStatus::Ok && otherAudio.analysis.corrections.events.size() == 4, "new audio keeps the corrections");
        Check(HasWarning(otherAudio.analysis, "corrections-from-other-audio"), "and warns that they came from other audio");
    }

    void TestCancelAndThread() {
        Title("Cancellation, and not blocking the caller");
        const std::string audio = PathOf("a_clicks.wav");
        const std::string json = PathOf("cancelled.json");
        std::set<std::string> stages;
        (void)AnalyzeSong(audio, {}, [&](int, const char* stage) { stages.insert(stage); });
        Check(stages.size() >= 10, fmt::format("progress goes through {} stages", stages.size()));
        for (const std::string& stage : stages) {
            if (stage == "done") {
                continue; // nothing is left to cancel
            }
            bool stop = false;
            int polls = 0;
            const AnalysisResult result = AnalyzeSongCached(
                audio, json, {}, [&](int, const char* now) { stop = stop || stage == now; }, [&]() { ++polls; return stop; });
            std::error_code ec;
            Check(result.status == AnalysisStatus::Cancelled && !fs::exists(json, ec) && !fs::exists(json + ".composer-tmp", ec), fmt::format("cancelled at {}: no file left", stage));
        }

        SongAnalyzer analyzer;
        analyzer.Start(audio, json);
        long waited = 0;
        int lastProgress = 0;
        const auto start = std::chrono::steady_clock::now();
        while (!analyzer.IsDone()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            lastProgress = std::max(lastProgress, analyzer.Progress());
            ++waited;
        }
        const long long elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        const AnalysisResult done = analyzer.Wait();
        Check(done.status == AnalysisStatus::Ok && waited > 10, fmt::format("the caller kept running: {} polls in {} ms", waited, elapsed));
        Check(lastProgress > 0, fmt::format("and could read the progress ({}%)", lastProgress));
        std::error_code ec;
        fs::remove(json, ec);

        {
            SongAnalyzer running;
            running.Start(audio, json);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            running.Cancel();
            const AnalysisResult cancelled = running.Wait();
            Check(cancelled.status == AnalysisStatus::Cancelled || cancelled.status == AnalysisStatus::Ok, "cancelling from another thread ends it");
            if (cancelled.status == AnalysisStatus::Cancelled) {
                Check(!fs::exists(json, ec), "without a file");
            }
        }
        {
            SongAnalyzer destroyed;
            destroyed.Start(audio, json);
        }
        Check(true, "destroying a running analyzer waits for it");
        fs::remove(json, ec);
    }

    void TestPlayerUntouched() {
        Title("The player's AudioManager is not affected");
        const std::string audio = PathOf("a_clicks.wav");
        AudioManager player(audio, 25);
        const FrameData* before = player.GetFrameData(100, "");
        const float max = before != nullptr ? before->max : -1;
        const long position = player.Tell();
        const int interval = player.GetFrameInterval();
        const AnalysisResult result = AnalyzeSong(audio);
        Check(result.status == AnalysisStatus::Ok, "analysis ran next to it");
        Check(player.GetFrameInterval() == interval && player.Tell() == position, "interval and position unchanged");
        const FrameData* after = player.GetFrameData(100, "");
        Check(after != nullptr && after->max == max, "frame data unchanged");
        Check(player.GetPlayingState() == MEDIAPLAYINGSTATE::STOPPED, "nothing started playing");
        Check(player.GetFilteredAudioData(AUDIOSAMPLETYPE::VOCALS, 0, 0) == nullptr, "its filters are its own");
    }

    void TestSnapshot() {
        Title("SnapshotAt");
        const std::string json = PathOf("a_snapshot.json");
        const AnalysisResult result = AnalyzeSong(PathOf("c_silences.wav"));
        const SongAnalysis& a = result.analysis;
        const MusicSnapshot music = SnapshotAt(a, 10000);
        Check(music.sectionIndex >= 0 && !music.inSilence, fmt::format("in the music: section {}", music.sectionIndex));
        Check(std::abs(music.nearestBeatMS - 10000) <= 500, fmt::format("nearest beat at {} ms", music.nearestBeatMS));
        Check(std::abs(music.nearestOnsetMS - 10000) <= 500, fmt::format("nearest onset at {} ms", music.nearestOnsetMS));
        Check(music.energy.total > 0, "with energy");
        Check(music.vocalActivity == "unknown", fmt::format("voice is not claimed: {}", music.vocalActivity));
        const MusicSnapshot pause = SnapshotAt(a, 20000);
        Check(pause.inSilence && pause.energy.total == 0, "in the pause: silence and no energy");
        const MusicSnapshot start = SnapshotAt(a, a.musicStartMS);
        Check(std::any_of(start.nearbyEvents.begin(), start.nearbyEvents.end(), [](const MusicalEvent& e) { return e.type == MusicalEventType::SectionStart; }), "a section start is among the nearby events");
        const MusicSnapshot outside = SnapshotAt(a, a.durationMS + 5000);
        Check(outside.sectionIndex == -1, "past the end: no section and no crash");
    }

    void TestExport() {
        Title("Export to xLights timing tracks");
        const AnalysisResult result = AnalyzeSong(PathOf("c_silences.wav"));
        const SongAnalysis& a = result.analysis;
        constexpr int FRAME_MS = 25;
        const std::vector<TimingTrackExport> tracks = BuildTimingTracks(a, FRAME_MS);
        bool onGrid = true;
        bool ordered = true;
        for (const TimingTrackExport& track : tracks) {
            int previous = 0;
            for (const TimingMark& mark : track.marks) {
                onGrid = onGrid && mark.startMS % FRAME_MS == 0 && mark.endMS % FRAME_MS == 0;
                ordered = ordered && mark.startMS >= previous && mark.endMS > mark.startMS && mark.endMS <= a.durationMS;
                previous = mark.endMS;
            }
        }
        Check(tracks.size() >= 4, fmt::format("{} tracks", tracks.size()));
        Check(onGrid, "every mark on a frame");
        Check(ordered, "marks in order, not overlapping, inside the song");

        const std::string xtiming = PathOf("export.xtiming");
        std::string error;
        Check(WriteXTiming(xtiming, tracks, error), fmt::format("file written {}", error));

        // Through xLights' own importer, into a new sequence on an empty test show.
        const fs::path show = _root / "export_show";
        std::error_code ec;
        fs::create_directories(show, ec);
        {
            pugi::xml_document doc;
            pugi::xml_node root = doc.append_child("xrgb");
            root.append_child("models");
            root.append_child("modelGroups");
            Check(doc.save_file((show / "xlights_rgbeffects.xml").string().c_str()), "test show written");
        }
        const std::string sequence = (show / "imported.xsq").string();
        {
            TimingImportContext ctx;
            if (!ctx.LoadShowFolder(show.string())) {
                Check(false, "loading the test show");
                return;
            }
            SequenceElements& elements = ctx.GetSequenceElements();
            elements.SetFrequency(1000.0 / FRAME_MS);
            elements.SetViewsManager(&ctx._sequenceViewManager);
            elements.SetSequenceEnd(a.durationMS);
            SequenceFile file(sequence, FRAME_MS);
            file.SetSequenceType("Animation");
            file.SetSequenceDurationMS(a.durationMS);
            file.SetSequenceTiming(std::to_string(FRAME_MS) + " ms");
            file.ProcessXTiming(std::vector<std::string>{ xtiming }, &ctx);
            Check(file.Save(elements), "saved through SequenceFile");
            (void)ctx.CloseSequence();
        }
        HeadlessRenderContext reopened;
        if (!reopened.LoadShowFolder(show.string()) || !reopened.OpenSequence(sequence)) {
            Check(false, "reopening the imported sequence");
            return;
        }
        SequenceElements& elements = reopened.GetSequenceElements();
        for (const TimingTrackExport& track : tracks) {
            TimingElement* element = elements.GetTimingElement(track.name);
            if (element == nullptr || element->GetEffectLayerCount() != 1) {
                Check(false, fmt::format("track '{}' imported", track.name));
                continue;
            }
            EffectLayer* layer = element->GetEffectLayer(0);
            bool same = layer->GetEffectCount() == static_cast<int>(track.marks.size());
            for (size_t m = 0; same && m < track.marks.size(); ++m) {
                const Effect* mark = layer->GetEffect(static_cast<int>(m));
                same = mark->GetStartTimeMS() == track.marks[m].startMS && mark->GetEndTimeMS() == track.marks[m].endMS && mark->GetEffectName() == track.marks[m].label;
            }
            Check(same, fmt::format("'{}': {} marks identical after import, save and reopen", track.name, track.marks.size()));
        }
        (void)reopened.CloseSequence();

        const std::vector<SongStructureRegion> regions = BuildSongStructure(a);
        Check(regions.size() == a.sections.size() && !regions.empty() && regions.back().endTimeMS == a.durationMS, "sections map to song structure regions");
    }

    void TestReference() {
        Title("Reference songs: the mechanism for a person's review");
        const AnalysisResult result = AnalyzeSong(PathOf("a_clicks.wav"));
        const std::string path = PathOf("reference.json");
        {
            std::ofstream out(fs::path(path), std::ios::binary);
            std::string beats;
            for (int i = 0; 500 + i * 500 < 31000; ++i) {
                beats += fmt::format("{}{}", i == 0 ? "" : ",", 500 + i * 500);
            }
            out << "{\"beatsMS\":[" << beats << "]}";
        }
        ReferenceScore score;
        std::string error;
        Check(ScoreAgainstReference(result.analysis, path, score, error), fmt::format("reference read {}", error));
        Check(score.beatF >= 0.9 && score.downbeatF == -1 && score.sectionF == -1, fmt::format("beats F {}, and what the reference lacks stays -1", score.beatF));
        Check(result.analysis.musicQuality == MUSIC_QUALITY_NOT_VALIDATED, fmt::format("quality stays {}", result.analysis.musicQuality));
    }

    fs::path _root;
    int _passed = 0;
    int _failed = 0;
};

std::atomic<bool> sInterrupted{ false };

void OnInterrupt(int) {
    sInterrupted = true;
}

std::string ReportText(const AnalysisResult& result, const std::string& audioPath) {
    const SongAnalysis& a = result.analysis;
    const auto count = [&](MusicalEventType type) { return TimesOf(a, type).size(); };
    std::string report;
    report += fmt::format("audio: {}\nhash: {}\nreused cache: {}\nmusic quality: {}\n", audioPath, a.audioHash, result.reusedCache ? "yes" : "no", a.musicQuality);
    report += fmt::format("duration: {} ms, {} Hz analysed ({} Hz source), {} channels\n", a.durationMS, a.sampleRate, a.sourceSampleRate, a.channels);
    report += fmt::format("music from {} to {} ms\n", a.musicStartMS, a.musicEndMS);
    report += fmt::format("bpm: {} (confidence {}, {})\n", a.globalBpm, a.bpmConfidence, a.tempoIsStable ? "stable" : "not stable");
    report += fmt::format("bar: {} beats (confidence {}), key: {}\n", a.beatsPerBar, a.barConfidence, a.key.empty() ? "unknown" : a.key);
    report += fmt::format("events: {} beats, {} downbeats, {} onsets, {} silences, {} climax, {} voice segments\n", count(MusicalEventType::Beat), count(MusicalEventType::Downbeat), count(MusicalEventType::Onset), count(MusicalEventType::Silence), count(MusicalEventType::Climax), a.vocalSegments.size());
    report += fmt::format("sections: {}\n", a.sections.size());
    for (const SongSection& section : a.sections) {
        report += fmt::format("  {:>7}-{:>7} ms  {:<12} group {:>2}  energy {:.3f}  confidence {:.3f}\n", section.startMS, section.endMS, SectionKindId(section.kind), section.repeatGroup, section.energy, section.confidence);
    }
    report += "warnings:";
    for (const std::string& warning : a.warnings) {
        report += " " + warning;
    }
    report += "\nstage ms:";
    for (const auto& [stage, ms] : result.stageMS) {
        report += fmt::format(" {}={}", stage, ms);
    }
    report += "\n";
    return report;
}

} // namespace

int RunAnalyzeCommand(const AnalyzeCommandOptions& command) {
    if (command.audioPath.empty()) {
        printf("composer-analyze-song: REFUSED: no audio file\n");
        return 2;
    }
    const std::string json = command.outPath.empty() ? command.audioPath + ".composer-analysis.json" : command.outPath;
    AnalysisOptions options;
    options.energyIntervalMS = command.energyIntervalMS;
    options.onsetSensitivity = command.onsetSensitivity;

    std::signal(SIGINT, OnInterrupt);
    int lastPercent = -1;
    const AnalysisResult result = AnalyzeSongCached(
        command.audioPath, json, options,
        [&](int percent, const char* stage) {
            if (percent != lastPercent) {
                printf("composer-analyze-song: %3d%% %s\n", percent, stage);
                fflush(stdout);
                lastPercent = percent;
            }
        },
        [&]() { return sInterrupted.load() || (!command.cancelFile.empty() && FileExists(command.cancelFile)); });

    if (result.status == AnalysisStatus::Cancelled) {
        printf("composer-analyze-song: CANCELLED: nothing was written\n");
        return 3;
    }
    if (result.status != AnalysisStatus::Ok) {
        printf("composer-analyze-song: FAILED: %s %s\n", result.errorCode.c_str(), result.errorDetail.c_str());
        return 1;
    }
    const std::string report = ReportText(result, command.audioPath);
    printf("%s", report.c_str());
    printf("composer-analyze-song: analysis written to %s\n", json.c_str());
    if (!command.reportPath.empty()) {
        std::ofstream out(fs::path(command.reportPath), std::ios::binary);
        out << report;
        printf("composer-analyze-song: report written to %s\n", command.reportPath.c_str());
    }
    if (!command.xtimingPath.empty()) {
        std::string error;
        if (!WriteXTiming(command.xtimingPath, BuildTimingTracks(result.analysis, command.frameMS), error)) {
            printf("composer-analyze-song: FAILED: %s\n", error.c_str());
            return 1;
        }
        printf("composer-analyze-song: timing tracks written to %s\n", command.xtimingPath.c_str());
    }
    if (!command.referencePath.empty()) {
        ReferenceScore score;
        std::string error;
        if (!ScoreAgainstReference(result.analysis, command.referencePath, score, error)) {
            printf("composer-analyze-song: FAILED: %s\n", error.c_str());
            return 1;
        }
        printf("composer-analyze-song: reference: beats F %.3f, downbeats F %.3f, section boundaries F %.3f (-1: not in the reference)\n", score.beatF, score.downbeatF, score.sectionF);
    }
    return 0;
}

int RunAnalyzerTest(const std::string& workDir) {
    std::error_code ec;
    fs::path root;
    for (int attempt = 0;; ++attempt) {
        root = fs::path(workDir) / fmt::format("composer-analyzer-test-{}", attempt);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-analyzer-test: cannot create '%s': %s\n", root.string().c_str(), ec.message().c_str());
        return 1;
    }
    Suite suite(root);
    return suite.Run();
}

int RunAnalyzerBench(const std::string& workDir) {
    std::error_code ec;
    fs::create_directories(workDir, ec);
    for (const int minutes : { 3, 5, 10 }) {
        const std::string path = (fs::path(workDir) / fmt::format("bench_{}min.wav", minutes)).string();
        if (!FileExists(path)) {
            const double seconds = minutes * 60.0;
            SyntheticSong song(seconds);
            const double chords[][3] = { { 261.6, 329.6, 392.0 }, { 220.0, 261.6, 329.6 }, { 349.2, 440.0, 523.3 }, { 392.0, 493.9, 587.3 } };
            int beat = 0;
            for (double at = 1; at < seconds - 1; at += 0.5, ++beat) {
                const double* chord = chords[(beat / 8) % 4];
                const bool loud = (beat / 64) % 2 == 1;
                song.Click(at, loud ? 0.5 : 0.3);
                if (beat % 4 == 0) {
                    song.Kick(at, 0.7);
                }
                for (int n = 0; n < 3; ++n) {
                    song.Tone(at, 0.45, chord[n], loud ? 0.12 : 0.07);
                }
            }
            if (!song.Save(path)) {
                printf("composer-analyzer-bench: cannot write %s\n", path.c_str());
                return 1;
            }
        }
        const auto start = std::chrono::steady_clock::now();
        const AnalysisResult result = AnalyzeSong(path);
        const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        printf("composer-analyzer-bench: %d min: total %lld ms, status %d, %zu events, %zu sections, %.1f BPM\n ", minutes, static_cast<long long>(total), static_cast<int>(result.status), result.analysis.events.size(), result.analysis.sections.size(), result.analysis.globalBpm);
        for (const auto& [stage, ms] : result.stageMS) {
            printf(" %s=%lld", stage.c_str(), static_cast<long long>(ms));
        }
        printf("\n");
        fflush(stdout);
    }
    return 0;
}

} // namespace composer
