#include "composer/SongAnalysis.h"

#include "media/AudioManager.h"
#include "media/ChordDetector.h"
#include "media/OnsetDetector.h"
#include "media/TempoDetector.h"
#include "render/SongStructureManager.h"
#include "utils/ExternalHooks.h"
#include "xLightsVersion.h"

#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <numeric>
#include <set>
#include <system_error>

namespace fs = std::filesystem;

namespace composer {

namespace {

// Values are rounded when they are made, so that an analysis in memory and the same
// analysis read back from its JSON are equal.
double Round3(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

double Clamp01(double value) {
    return std::max(0.0, std::min(1.0, value));
}

template <typename T>
double Percentile(std::vector<T> values, double fraction) {
    if (values.empty()) {
        return 0;
    }
    const size_t index = std::min(values.size() - 1, static_cast<size_t>(fraction * static_cast<double>(values.size())));
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(index), values.end());
    return static_cast<double>(values[index]);
}

double Median(const std::vector<double>& values) {
    return Percentile(values, 0.5);
}

constexpr double REFERENCE_FULL_SCALE_DB_FLOOR = -120.0;

double ToDb(double rms) {
    return rms <= 0 ? REFERENCE_FULL_SCALE_DB_FLOOR : std::max(REFERENCE_FULL_SCALE_DB_FLOOR, 20.0 * std::log10(rms));
}

class Sha256 {
public:
    void Update(const unsigned char* data, size_t size) {
        for (size_t i = 0; i < size; ++i) {
            _block[_blockSize++] = data[i];
            _bits += 8;
            if (_blockSize == 64) {
                Transform();
                _blockSize = 0;
            }
        }
    }

    std::string Finish() {
        const std::uint64_t bits = _bits;
        const unsigned char pad = 0x80;
        Update(&pad, 1);
        const unsigned char zero = 0;
        while (_blockSize != 56) {
            Update(&zero, 1);
        }
        for (int shift = 56; shift >= 0; shift -= 8) {
            const unsigned char byte = static_cast<unsigned char>(bits >> shift);
            Update(&byte, 1);
        }
        std::string hex;
        for (std::uint32_t word : _state) {
            hex += fmt::format("{:08x}", word);
        }
        return hex;
    }

private:
    static std::uint32_t Rotate(std::uint32_t value, int bits) {
        return (value >> bits) | (value << (32 - bits));
    }

    void Transform() {
        static constexpr std::array<std::uint32_t, 64> K = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };
        std::array<std::uint32_t, 64> w{};
        for (size_t i = 0; i < 16; ++i) {
            w[i] = (static_cast<std::uint32_t>(_block[i * 4]) << 24) | (static_cast<std::uint32_t>(_block[i * 4 + 1]) << 16) | (static_cast<std::uint32_t>(_block[i * 4 + 2]) << 8) | static_cast<std::uint32_t>(_block[i * 4 + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            const std::uint32_t s0 = Rotate(w[i - 15], 7) ^ Rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = Rotate(w[i - 2], 17) ^ Rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::array<std::uint32_t, 8> v = _state;
        for (size_t i = 0; i < 64; ++i) {
            const std::uint32_t s1 = Rotate(v[4], 6) ^ Rotate(v[4], 11) ^ Rotate(v[4], 25);
            const std::uint32_t ch = (v[4] & v[5]) ^ (~v[4] & v[6]);
            const std::uint32_t t1 = v[7] + s1 + ch + K[i] + w[i];
            const std::uint32_t s0 = Rotate(v[0], 2) ^ Rotate(v[0], 13) ^ Rotate(v[0], 22);
            const std::uint32_t maj = (v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]);
            const std::uint32_t t2 = s0 + maj;
            v = { t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6] };
        }
        for (size_t i = 0; i < 8; ++i) {
            _state[i] += v[i];
        }
    }

    std::array<std::uint32_t, 8> _state = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    std::array<unsigned char, 64> _block{};
    size_t _blockSize = 0;
    std::uint64_t _bits = 0;
};

constexpr const char* SOURCE_ONSET = "OnsetDetector.DetectOnsets";
constexpr const char* SOURCE_BEAT = "beat-tracker:local-autocorrelation+dynamic-programming";
constexpr const char* SOURCE_DOWNBEAT = "downbeat:accent-pattern";
constexpr const char* SOURCE_SECTION = "sections:self-similarity-novelty";
constexpr const char* SOURCE_ENERGY = "energy-curve";
constexpr const char* SOURCE_SILENCE = "silence:raw-rms";
constexpr const char* SOURCE_VOCAL = "filter:VOCALS-center-channel";
constexpr const char* SOURCE_MANUAL = "manual-correction";

constexpr int ONSET_FRAME_SIZE = 1024;
constexpr int ONSET_HOP_SIZE = 512;
constexpr int FEATURE_HOP_MS = 500;
constexpr int NOTES_LOW_END = 60;   // MIDI 59 is about 247 Hz
constexpr int NOTES_MID_END = 96;   // MIDI 95 is about 1976 Hz
constexpr int EVENT_MATCH_MS = 50;
constexpr double MIN_BEAT_CONFIDENCE = 0.1;
constexpr double MAX_VOCAL_CONFIDENCE = 0.3;
constexpr double MIN_SECTION_NOVELTY = 0.2;
constexpr double MIN_REPEAT_SIMILARITY = 0.6;
constexpr double MIN_DOWNBEAT_CONFIDENCE = 0.35;

class Analysis {
public:
    Analysis(AudioManager& audio, const AnalysisOptions& options, const AnalysisProgress& progress, const AnalysisCancel& cancel, AnalysisResult& result) :
        _audio(audio),
        _options(options),
        _progress(progress),
        _cancel(cancel),
        _result(result),
        _a(result.analysis) {}

    void Run() {
        const std::array<std::pair<void (Analysis::*)(), const char*>, 11> stages = { {
            { &Analysis::StageEnergy, "energy" },
            { &Analysis::StageOnsets, "onsets" },
            { &Analysis::StageTempo, "tempo" },
            { &Analysis::StageBeats, "beats" },
            { &Analysis::StageHarmony, "harmony" },
            { &Analysis::StageDownbeats, "downbeats" },
            { &Analysis::StageIntensity, "intensity" },
            { &Analysis::StageVocals, "vocals" },
            { &Analysis::StageSections, "sections" },
            { &Analysis::StageHighlights, "highlights" },
            { &Analysis::StageEvents, "events" },
        } };
        int done = 0;
        for (const auto& [stage, name] : stages) {
            if (IsCancelled()) {
                return;
            }
            Report(15 + done * 80 / static_cast<int>(stages.size()), name);
            const auto start = std::chrono::steady_clock::now();
            (this->*stage)();
            _result.stageMS[name] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            ++done;
        }
    }

    bool IsCancelled() {
        if (!_cancelled && _cancel && _cancel()) {
            _cancelled = true;
        }
        return _cancelled;
    }

private:
    void Report(int percent, const char* stage) {
        if (_progress) {
            _progress(percent, stage);
        }
    }

    double RangeRms(long start, long count) {
        const float* left = _audio.GetRawLeftDataPtr(start);
        const float* right = _audio.GetRawRightDataPtr(start);
        if (left == nullptr || count <= 0) {
            return 0;
        }
        double sum = 0;
        for (long i = 0; i < count; ++i) {
            sum += static_cast<double>(left[i]) * left[i];
        }
        if (right != nullptr && right != left) {
            for (long i = 0; i < count; ++i) {
                sum += static_cast<double>(right[i]) * right[i];
            }
            sum *= 0.5;
        }
        return std::sqrt(sum / static_cast<double>(count));
    }

    // Absolute, not relative to the song: a whisper is not silence in a quiet song,
    // and a song is not made of silence for being quiet.
    void FindSilences(const std::vector<double>& rms) {
        const int interval = _options.energyIntervalMS;
        int runStart = -1;
        for (size_t i = 0; i <= rms.size(); ++i) {
            const bool silent = i < rms.size() && ToDb(rms[i]) < _options.silenceDb;
            if (silent && runStart < 0) {
                runStart = static_cast<int>(i);
            } else if (!silent && runStart >= 0) {
                const int startMS = runStart * interval;
                const int endMS = std::min(_a.durationMS, static_cast<int>(i) * interval);
                if (endMS - startMS >= _options.minSilenceMS || startMS == 0 || endMS == _a.durationMS) {
                    _silences.push_back({ startMS, endMS });
                }
                runStart = -1;
            }
        }
        _a.musicStartMS = 0;
        _a.musicEndMS = _a.durationMS;
        if (!_silences.empty() && _silences.front().startMS == 0) {
            _a.musicStartMS = _silences.front().endMS;
        }
        if (!_silences.empty() && _silences.back().endMS == _a.durationMS) {
            _a.musicEndMS = _silences.back().startMS;
        }
        if (_a.musicEndMS <= _a.musicStartMS) {
            _a.musicStartMS = 0;
            _a.musicEndMS = 0;
            _a.warnings.push_back("no-musical-content");
        }
    }

    bool InSilence(int timeMS) const {
        return std::any_of(_silences.begin(), _silences.end(), [&](const TimeRange& s) { return timeMS >= s.startMS && timeMS < s.endMS; });
    }

    void StageEnergy() {
        const int interval = _options.energyIntervalMS;
        _audio.SetFrameInterval(interval);
        const FrameData* first = _audio.GetFrameData(0, "");
        const int frames = (_a.durationMS + interval - 1) / interval;
        if (first == nullptr || frames <= 0) {
            _a.warnings.push_back("energy-unavailable");
            return;
        }
        const long samples = _audio.GetRate() * interval / 1000;
        _a.energy.resize(static_cast<size_t>(frames));
        _rms.resize(static_cast<size_t>(frames));
        std::vector<double> variationRaw(static_cast<size_t>(frames), 0);
        _spectra.assign(static_cast<size_t>(frames), {});
        const std::vector<float>* previous = nullptr;
        for (int i = 0; i < frames; ++i) {
            if (i % 256 == 0 && IsCancelled()) {
                return;
            }
            const long start = i * samples;
            const long count = std::min(samples, _audio.GetTrackSize() - start);
            _rms[i] = RangeRms(start, count);

            EnergyPoint& point = _a.energy[i];
            point.timeMS = i * interval;
            const FrameData* frame = _audio.GetFrameData(i, "");
            if (frame != nullptr && !frame->vu.empty()) {
                const auto& vu = frame->vu;
                const auto bandPeak = [&](int from, int to) {
                    float peak = 0;
                    for (int n = from; n < to && n < static_cast<int>(vu.size()); ++n) {
                        peak = std::max(peak, vu[n]);
                    }
                    return static_cast<double>(peak);
                };
                point.low = Round3(Clamp01(bandPeak(0, NOTES_LOW_END)));
                point.mid = Round3(Clamp01(bandPeak(NOTES_LOW_END, NOTES_MID_END)));
                point.high = Round3(Clamp01(bandPeak(NOTES_MID_END, static_cast<int>(vu.size()))));
                if (previous != nullptr && previous->size() == vu.size()) {
                    double change = 0;
                    for (size_t n = 0; n < vu.size(); ++n) {
                        change += std::fabs(vu[n] - (*previous)[n]);
                    }
                    variationRaw[i] = change / static_cast<double>(vu.size());
                }
                previous = &vu;
                _spectra[i] = &vu;
            }
            point.rms = Round3(Clamp01(_rms[i]));
        }
        FindSilences(_rms);

        std::vector<double> active;
        for (int i = 0; i < frames; ++i) {
            if (!InSilence(i * interval)) {
                active.push_back(_rms[i]);
            }
        }
        // A percentile, so that one loud transient does not flatten the whole curve.
        const double top = std::max(1e-6, Percentile(active, 0.98));
        const double topVariation = std::max(1e-6, Percentile(variationRaw, 0.98));
        for (int i = 0; i < frames; ++i) {
            EnergyPoint& point = _a.energy[i];
            const bool silent = InSilence(i * interval);
            point.total = silent ? 0 : Round3(Clamp01(_rms[i] / top));
            point.variation = silent ? 0 : Round3(Clamp01(variationRaw[i] / topVariation));
            if (silent) {
                point.low = point.mid = point.high = 0;
            }
        }
    }

    // DetectOnsets dates a peak half a hop into its frame. With a Hann window and a hop
    // of half a window, the peak frame has the transient near its centre, within a
    // quarter of a window either way, so the centre is the unbiased estimate.
    int EnvelopeFrameToMS(size_t frame) const {
        return static_cast<int>(std::lround((static_cast<double>(frame) * ONSET_HOP_SIZE + ONSET_FRAME_SIZE / 2.0) * 1000.0 / static_cast<double>(_envelopeRate)));
    }

    int MSToEnvelopeFrame(int ms) const {
        const double frame = (static_cast<double>(ms) * _envelopeRate / 1000.0 - ONSET_FRAME_SIZE / 2.0) / ONSET_HOP_SIZE;
        return std::max(0, static_cast<int>(std::lround(frame)));
    }

    void StageOnsets() {
        OnsetDetectorOptions options;
        options.frameSize = ONSET_FRAME_SIZE;
        options.hopSize = ONSET_HOP_SIZE;
        options.sensitivity = static_cast<float>(_options.onsetSensitivity);
        const OnsetEnvelope envelope = ComputeOnsetEnvelope(&_audio, options);
        if (envelope.flux.empty()) {
            _a.warnings.push_back("onsets-unavailable");
            return;
        }
        _envelopeRate = envelope.sampleRate;
        const double top = std::max<double>(1e-6, Percentile(envelope.flux, 0.99));
        _envelope.resize(envelope.flux.size());
        for (size_t i = 0; i < envelope.flux.size(); ++i) {
            _envelope[i] = InSilence(EnvelopeFrameToMS(i)) ? 0.0f : static_cast<float>(std::min(1.0, envelope.flux[i] / top));
        }
        if (IsCancelled()) {
            return;
        }

        const double latency = (ONSET_FRAME_SIZE / 2.0 - ONSET_HOP_SIZE / 2.0) * 1000.0 / _envelopeRate;
        const double typical = std::max(1e-6, static_cast<double>(Percentile(_envelope, 0.5)));
        for (long detected : DetectOnsets(&_audio, options)) {
            const int timeMS = static_cast<int>(std::lround(static_cast<double>(detected) + latency));
            if (timeMS >= _a.durationMS || InSilence(timeMS)) {
                continue;
            }
            const size_t frame = std::min(_envelope.size() - 1, static_cast<size_t>(MSToEnvelopeFrame(timeMS)));
            double value = 0;
            for (size_t f = frame > 2 ? frame - 2 : 0; f <= frame + 2 && f < _envelope.size(); ++f) {
                value = std::max<double>(value, _envelope[f]);
            }
            if (value < _options.minOnsetIntensity) {
                continue;
            }
            MusicalEvent event;
            event.timeMS = timeMS;
            event.type = MusicalEventType::Onset;
            event.intensity = Round3(value);
            event.confidence = Round3(Clamp01(0.3 + 0.7 * Clamp01((value / std::max(typical, 0.02) - 1.0) / 8.0)));
            event.source = SOURCE_ONSET;
            _onsets.push_back(std::move(event));
        }
    }

    // A local tempo for every half second: autocorrelation of the onset envelope over
    // a few seconds, then the cheapest path through time. The path is what lets tempo
    // drift or jump without following every octave error of a single window.
    void StageTempo() {
        if (_envelope.empty()) {
            return;
        }
        TempoDetectorOptions tempoOptions;
        tempoOptions.minBPM = static_cast<float>(_options.minBpm);
        tempoOptions.maxBPM = static_cast<float>(_options.maxBpm);
        const TempoResult initial = DetectTempo(&_audio, tempoOptions);
        _detectTempoBpm = initial.bpm;
        _a.initialBpm = Round3(initial.bpm);
        _a.initialBpmConfidence = Round3(initial.confidence);
        const double priorBpm = initial.confidence >= 0.3 ? initial.bpm : 120.0;

        const double hopMS = ONSET_HOP_SIZE * 1000.0 / _envelopeRate;
        const int minLag = std::max(2, static_cast<int>(std::floor(60000.0 / _options.maxBpm / hopMS)));
        const int maxLag = static_cast<int>(std::ceil(60000.0 / _options.minBpm / hopMS));
        const int window = static_cast<int>(6000.0 / hopMS);
        const int step = static_cast<int>(500.0 / hopMS);
        const int frames = static_cast<int>(_envelope.size());
        const int lags = maxLag - minLag + 1;
        const int windows = std::max(1, frames / step);

        // An attack lasts a frame or two, and a period rarely falls on a whole number of
        // frames, so the raw envelope misses its own peak at the true lag and finds it at
        // whichever multiple happens to land closer. Widening the peaks removes that bias.
        std::vector<float> smooth(_envelope.size(), 0.0f);
        for (int i = 0; i < frames; ++i) {
            float sum = 0;
            float weight = 0;
            for (int o = -2; o <= 2; ++o) {
                if (i + o >= 0 && i + o < frames) {
                    const float w = static_cast<float>(3 - std::abs(o));
                    sum += w * _envelope[i + o];
                    weight += w;
                }
            }
            smooth[i] = sum / weight;
        }

        std::vector<float> score(static_cast<size_t>(windows) * lags, 0.0f);
        _rhythmicity.assign(windows, 0.0);
        std::vector<double> correlation(static_cast<size_t>(2 * maxLag + 2), 0.0);
        std::vector<std::vector<float>> plain(windows);
        std::vector<std::vector<double>> full(windows);
        std::vector<double> prominence(windows, 0.0);
        double globalMean = 0;
        for (float value : smooth) {
            globalMean += value;
        }
        globalMean /= frames;
        double globalVariance = 0;
        for (float value : smooth) {
            globalVariance += (value - globalMean) * (value - globalMean);
        }
        globalVariance = std::max(1e-12, globalVariance / frames);
        for (int w = 0; w < windows; ++w) {
            if (w % 32 == 0 && IsCancelled()) {
                return;
            }
            const int center = w * step + step / 2;
            const int from = std::max(0, center - window / 2);
            const int to = std::min(frames, center + window / 2);
            if (to - from < maxLag * 2) {
                continue;
            }
            double mean = 0;
            int activeFrames = 0;
            for (int i = from; i < to; ++i) {
                mean += smooth[i];
                activeFrames += InSilence(EnvelopeFrameToMS(i)) ? 0 : 1;
            }
            mean /= (to - from);
            if (activeFrames * 3 < to - from) {
                continue; // mostly silence: nothing to measure a period from
            }
            double zero = 0;
            for (int i = from; i < to; ++i) {
                zero += (smooth[i] - mean) * (smooth[i] - mean);
            }
            if (zero <= 1e-9) {
                continue;
            }
            // The correlation is relative to the window's own variation, so even the
            // ripple of a sustained note can look periodic. A pulse must also move the
            // envelope by a fair share of what the song's attacks do.
            prominence[w] = std::min(1.0, std::sqrt(zero / (to - from) / globalVariance) / 0.25);
            for (int lag = minLag; lag <= 2 * maxLag && lag < static_cast<int>(correlation.size()); ++lag) {
                double sum = 0;
                for (int i = from; i + lag < to; ++i) {
                    sum += (smooth[i] - mean) * (smooth[i + lag] - mean);
                }
                correlation[lag] = sum / zero;
            }
            plain[w].resize(lags);
            full[w].assign(correlation.begin(), correlation.end());
            for (int l = 0; l < lags; ++l) {
                const int lag = minLag + l;
                const double direct = std::max(0.0, correlation[lag]);
                const double harmonic = 2 * lag <= 2 * maxLag ? std::max(0.0, correlation[2 * lag]) : 0.0;
                const double bpm = 60000.0 / (lag * hopMS);
                // DetectTempo assumes one period for the whole song, so where the tempo
                // changes its answer can sit an octave away from part of it. Its double and
                // half stay nearly as plausible, and the onsets decide between them.
                const double octaves = std::log2(bpm / priorBpm);
                const auto around = [](double distance) { return std::exp(-0.5 * distance * distance / (0.9 * 0.9)); };
                const double prior = std::max({ around(octaves), 0.85 * around(octaves - 1), 0.85 * around(octaves + 1) });
                // A period whose half fits almost as well is the slow reading of a faster
                // pulse: every other beat lands on it too, so its own peak proves nothing.
                const int halfLag = lag / 2;
                const double half = halfLag >= minLag ? std::max(correlation[halfLag], correlation[halfLag + (lag & 1)]) : 0.0;
                const double slowReading = direct > 0 && half >= 0.8 * direct ? 0.7 : 1.0;
                score[static_cast<size_t>(w) * lags + l] = static_cast<float>((direct + 0.5 * harmonic) * prior * slowReading);
                plain[w][l] = static_cast<float>(direct);
            }
        }

        // The cost of a change is in log-tempo: a tenth faster costs the same at any tempo.
        // A jump of exactly an octave is nearly always the same pulse read at its double
        // or half, not the band changing tempo, and flipping between the two readings
        // breaks the beat grid the bar is counted on.
        constexpr double CHANGE_COST = 5.0;
        constexpr double OCTAVE_JUMP_COST = 40.0;
        constexpr double OCTAVE_REACH = 0.08;
        std::vector<float> best(score.begin(), score.begin() + lags);
        std::vector<int> back(static_cast<size_t>(windows) * lags, 0);
        std::vector<float> next(lags);
        std::vector<double> logLag(lags);
        for (int l = 0; l < lags; ++l) {
            logLag[l] = std::log(static_cast<double>(minLag + l));
        }
        for (int w = 1; w < windows; ++w) {
            if (w % 64 == 0 && IsCancelled()) {
                return;
            }
            for (int l = 0; l < lags; ++l) {
                double top = -1e18;
                int from = l;
                for (int p = 0; p < lags; ++p) {
                    const double distance = std::fabs(logLag[l] - logLag[p]);
                    const double octave = std::fabs(distance - std::log(2.0)) < OCTAVE_REACH ? OCTAVE_JUMP_COST : 0.0;
                    const double candidate = best[p] - CHANGE_COST * distance - octave;
                    if (candidate > top) {
                        top = candidate;
                        from = p;
                    }
                }
                next[l] = static_cast<float>(top + score[static_cast<size_t>(w) * lags + l]);
                back[static_cast<size_t>(w) * lags + l] = from;
            }
            best = next;
        }
        int current = static_cast<int>(std::max_element(best.begin(), best.end()) - best.begin());
        std::vector<int> path(windows, 0);
        for (int w = windows - 1; w >= 0; --w) {
            path[w] = current;
            current = back[static_cast<size_t>(w) * lags + current];
        }
        FoldOctaves(path, plain, minLag);
        _period.assign(windows, 0);
        for (int w = 0; w < windows; ++w) {
            _period[w] = minLag + path[w];
            if (!plain[w].empty()) {
                _rhythmicity[w] = Clamp01(plain[w][path[w]]) * prominence[w];
            }
        }
        _tempoStep = step;
        ListTempoCandidates(full, prominence, hopMS);
    }

    // Even with the jump made expensive, a song whose second half doubles its
    // percussion can be followed at 105 and then at 210: the same pulse, counted two
    // ways. The reading that covers most of the song decides, and the stretches read
    // an octave away are read again inside the chosen octave. A real change of tempo
    // that is not an octave stays as it was.
    void FoldOctaves(std::vector<int>& path, const std::vector<std::vector<float>>& plain, int minLag) {
        std::map<int, int> votes;
        for (size_t w = 0; w < path.size(); ++w) {
            if (!plain[w].empty()) {
                ++votes[path[w]];
            }
        }
        if (votes.empty()) {
            return;
        }
        // Readings within a few percent of each other are one reading.
        int reference = minLag + votes.begin()->first;
        int bestVotes = -1;
        for (const auto& [l, count] : votes) {
            int near = 0;
            for (const auto& [other, otherCount] : votes) {
                if (std::fabs(std::log(static_cast<double>(minLag + other) / (minLag + l))) < 0.06) {
                    near += otherCount;
                }
            }
            if (near > bestVotes) {
                bestVotes = near;
                reference = minLag + l;
            }
        }
        int folded = 0;
        for (size_t w = 0; w < path.size(); ++w) {
            const double ratio = static_cast<double>(minLag + path[w]) / reference;
            const bool octaveAway = std::fabs(std::log(ratio) - std::log(2.0)) < 0.08 || std::fabs(std::log(ratio) + std::log(2.0)) < 0.08;
            if (!octaveAway || plain[w].empty()) {
                continue;
            }
            const double target = (minLag + path[w]) / (ratio > 1 ? 2.0 : 0.5);
            int bestLag = -1;
            float bestFit = -1;
            for (int l = 0; l < static_cast<int>(plain[w].size()); ++l) {
                if (std::fabs(std::log((minLag + l) / target)) < 0.06 && plain[w][l] > bestFit) {
                    bestFit = plain[w][l];
                    bestLag = l;
                }
            }
            if (bestLag >= 0) {
                path[w] = bestLag;
                ++folded;
            }
        }
        if (folded > 0) {
            _a.warnings.push_back("tempo-octave-folded");
        }
    }

    // The pulse that was followed, and its half and double with how well each fits
    // the same windows: the octave of a pulse is the classic doubt, and a person can
    // choose another reading without the analysis being thrown away.
    void ListTempoCandidates(const std::vector<std::vector<double>>& full, const std::vector<double>& prominence, double hopMS) {
        std::vector<double> lagsUsed;
        double fit[3] = { 0, 0, 0 };
        double weight = 0;
        for (size_t w = 0; w < full.size(); ++w) {
            if (full[w].empty() || prominence[w] <= 0) {
                continue;
            }
            const int lag = static_cast<int>(std::lround(_period[w]));
            const int size = static_cast<int>(full[w].size());
            const auto at = [&](double l) {
                const int lo = static_cast<int>(std::floor(l));
                const int hi = static_cast<int>(std::ceil(l));
                if (lo < 1 || hi >= size) {
                    return 0.0;
                }
                return std::max(0.0, std::max(full[w][lo], full[w][hi]));
            };
            lagsUsed.push_back(lag);
            fit[0] += prominence[w] * at(lag);
            fit[1] += prominence[w] * at(lag * 2.0);
            fit[2] += prominence[w] * at(lag / 2.0);
            weight += prominence[w];
        }
        if (lagsUsed.empty() || weight <= 0) {
            return;
        }
        const double bpm = 60000.0 / (Median(lagsUsed) * hopMS);
        const double sum = std::max(1e-9, fit[0] + fit[1] + fit[2]);
        const std::pair<double, const char*> readings[3] = { { bpm, "followed" }, { bpm / 2, "half" }, { bpm * 2, "double" } };
        for (int i = 0; i < 3; ++i) {
            if (readings[i].first < 40 || readings[i].first > 260 || (i > 0 && fit[i] <= 0)) {
                continue;
            }
            _a.tempoCandidates.push_back({ Round3(readings[i].first), Round3(fit[i] / sum), readings[i].second });
        }
    }

    double PeriodAt(int frame) const {
        const double position = (frame - _tempoStep / 2.0) / _tempoStep;
        if (position <= 0) {
            return _period.front();
        }
        const size_t index = static_cast<size_t>(position);
        if (index + 1 >= _period.size()) {
            return _period.back();
        }
        const double fraction = position - static_cast<double>(index);
        return _period[index] * (1 - fraction) + _period[index + 1] * fraction;
    }

    double RhythmicityAt(int frame) const {
        const int index = std::min(static_cast<int>(_rhythmicity.size()) - 1, frame / std::max(1, _tempoStep));
        return _rhythmicity[std::max(0, index)];
    }

    // Beat tracking by dynamic programming: each frame picks the earlier beat that best
    // balances a strong onset against keeping to the local tempo. Run per stretch of
    // music, so that a pause never gets invented beats and a resumption can have a
    // different phase.
    void TrackSegment(int from, int to, std::vector<int>& beats) {
        constexpr double TIGHTNESS = 100.0;
        const int length = to - from;
        if (length < 4) {
            return;
        }
        double mean = 0;
        for (int i = from; i < to; ++i) {
            mean += _envelope[i];
        }
        mean /= length;
        double deviation = 0;
        for (int i = from; i < to; ++i) {
            deviation += (_envelope[i] - mean) * (_envelope[i] - mean);
        }
        deviation = std::sqrt(deviation / length);
        if (deviation < 1e-6) {
            return;
        }
        std::vector<float> total(length, 0.0f);
        std::vector<int> previous(length, -1);
        for (int t = 0; t < length; ++t) {
            if (t % 8192 == 0 && IsCancelled()) {
                return;
            }
            const double onset = _envelope[from + t] / deviation;
            const double period = PeriodAt(from + t);
            const int minStep = std::max(1, static_cast<int>(std::lround(period * 0.75)));
            const int maxStep = static_cast<int>(std::lround(period * 1.35));
            double bestValue = 0;
            int bestFrom = -1;
            for (int s = minStep; s <= maxStep && s <= t; ++s) {
                const double ratio = std::log(s / period);
                const double candidate = total[t - s] - TIGHTNESS * ratio * ratio;
                if (candidate > bestValue) {
                    bestValue = candidate;
                    bestFrom = t - s;
                }
            }
            total[t] = static_cast<float>(onset + bestValue);
            previous[t] = bestFrom;
        }
        // The best end is the strongest beat within the last couple of periods; any later
        // end would only be chasing the tail of the last note.
        const int lastPeriod = static_cast<int>(PeriodAt(to - 1) * 2.0);
        int end = length - 1;
        double top = -1;
        for (int t = std::max(0, length - lastPeriod); t < length; ++t) {
            if (total[t] > top) {
                top = total[t];
                end = t;
            }
        }
        std::vector<int> chain;
        for (int t = end; t >= 0; t = previous[t]) {
            chain.push_back(from + t);
        }
        beats.insert(beats.end(), chain.rbegin(), chain.rend());
    }

    void StageBeats() {
        if (_envelope.empty() || _period.empty()) {
            return;
        }
        const int frames = static_cast<int>(_envelope.size());
        std::vector<int> found;
        int start = 0;
        for (const TimeRange& silence : _silences) {
            const int until = std::min(frames, MSToEnvelopeFrame(silence.startMS));
            if (until > start) {
                TrackSegment(start, until, found);
                _segments.push_back({ static_cast<int>(found.size()), 0 });
            }
            start = std::max(start, MSToEnvelopeFrame(silence.endMS));
        }
        if (start < frames) {
            TrackSegment(start, frames, found);
            _segments.push_back({ static_cast<int>(found.size()), 0 });
        }
        if (IsCancelled()) {
            return;
        }

        const double strong = std::max(1e-6, Percentile(_envelope, 0.95));
        std::vector<std::pair<int, int>> undefinedRanges;
        int nextIndex = 0;
        std::vector<int> segmentEnds;
        size_t segment = 0;
        int previousTime = -1;
        for (size_t b = 0; b < found.size(); ++b) {
            while (segment < _segments.size() && static_cast<int>(b) >= _segments[segment].first) {
                segmentEnds.push_back(static_cast<int>(_beats.size()));
                ++segment;
            }
            // The frame can be off by a hop or two after keeping to the tempo.
            int frame = found[b];
            for (int f = std::max(0, found[b] - 2); f <= std::min(frames - 1, found[b] + 2); ++f) {
                if (_envelope[f] > _envelope[frame]) {
                    frame = f;
                }
            }
            const double hit = std::min(1.0, _envelope[frame] / strong);
            const double rhythm = std::min(1.0, RhythmicityAt(frame) / 0.4);
            const double confidence = rhythm * (0.6 + 0.4 * hit);
            const int timeMS = EnvelopeFrameToMS(frame);
            if (confidence < MIN_BEAT_CONFIDENCE) {
                if (undefinedRanges.empty() || timeMS - undefinedRanges.back().second > 2000) {
                    undefinedRanges.push_back({ timeMS, timeMS });
                }
                undefinedRanges.back().second = timeMS;
                continue;
            }
            if (timeMS <= previousTime || timeMS >= _a.durationMS) {
                continue;
            }
            previousTime = timeMS;
            MusicalEvent event;
            event.timeMS = timeMS;
            event.type = MusicalEventType::Beat;
            event.intensity = Round3(hit);
            event.confidence = Round3(confidence);
            event.source = SOURCE_BEAT;
            event.beatIndex = nextIndex++;
            _beats.push_back(std::move(event));
        }
        while (segment < _segments.size()) {
            segmentEnds.push_back(static_cast<int>(_beats.size()));
            ++segment;
        }
        for (size_t s = 0; s < segmentEnds.size(); ++s) {
            _segments[s].first = s == 0 ? 0 : segmentEnds[s - 1];
            _segments[s].second = segmentEnds[s];
        }
        if (!undefinedRanges.empty()) {
            _a.warnings.push_back("rhythm-undefined");
        }

        std::vector<double> intervals;
        for (size_t b = 1; b < _beats.size(); ++b) {
            const int gap = _beats[b].timeMS - _beats[b - 1].timeMS;
            if (_beats[b].beatIndex == _beats[b - 1].beatIndex + 1 && !InSilence(_beats[b].timeMS - gap / 2)) {
                intervals.push_back(gap);
            }
        }
        if (intervals.size() < 4) {
            _a.warnings.push_back("tempo-undetermined");
            return;
        }
        const double typicalInterval = Median(intervals);
        _a.globalBpm = Round3(60000.0 / typicalInterval);
        const double spread = (Percentile(intervals, 0.9) - Percentile(intervals, 0.1)) / typicalInterval;
        _a.tempoIsStable = spread < 0.08;
        double rhythm = 0;
        for (const MusicalEvent& beat : _beats) {
            rhythm += beat.confidence;
        }
        rhythm /= static_cast<double>(_beats.size());
        _a.bpmConfidence = Round3(Clamp01(rhythm * (1.0 - std::min(0.5, spread))));
        if (_detectTempoBpm > 0) {
            const double ratio = _detectTempoBpm / _a.globalBpm;
            if (std::fabs(ratio - 2.0) < 0.1 || std::fabs(ratio - 0.5) < 0.05) {
                _a.warnings.push_back("tempo-octave-ambiguous");
            }
        }

        for (int t = 0; t < _a.durationMS; t += 1000) {
            if (InSilence(t)) {
                continue;
            }
            std::vector<double> local;
            for (size_t b = 1; b < _beats.size(); ++b) {
                if (std::abs(_beats[b].timeMS - t) <= 3000 && _beats[b].beatIndex == _beats[b - 1].beatIndex + 1) {
                    local.push_back(_beats[b].timeMS - _beats[b - 1].timeMS);
                }
            }
            if (local.size() >= 3) {
                const int frame = std::min(frames - 1, MSToEnvelopeFrame(t));
                _a.tempo.push_back({ t, Round3(60000.0 / Median(local)), Round3(Clamp01(RhythmicityAt(frame) / 0.4)) });
            }
        }
    }

    void StageHarmony() {
        const HarmonyAnalysis harmony = DetectChords(&_audio);
        _a.key = harmony.key;
        for (const ChordSegment& chord : harmony.chords) {
            _chordChanges.push_back(static_cast<int>(chord.startMS));
        }
    }

    // A bar shows in what repeats every 3 or 4 beats: which beats are strongest, which
    // have the bass, where the chords change. If no pattern stands out, there is no
    // answer, and a wrong bar would be worse for a choreography than none.
    void StageDownbeats() {
        std::vector<double> bestConfidences;
        int bars[5] = { 0, 0, 0, 0, 0 };
        for (const auto& [first, end] : _segments) {
            const int count = end - first;
            if (count < 12) {
                continue;
            }
            std::vector<double> strength(count), bass(count), change(count);
            for (int i = 0; i < count; ++i) {
                const MusicalEvent& beat = _beats[first + i];
                strength[i] = beat.intensity;
                const size_t frame = std::min(_a.energy.size() - 1, static_cast<size_t>(beat.timeMS / _options.energyIntervalMS));
                bass[i] = _a.energy.empty() ? 0 : _a.energy[frame].low;
                const int previousTime = i > 0 ? _beats[first + i - 1].timeMS : beat.timeMS - 500;
                change[i] = std::any_of(_chordChanges.begin(), _chordChanges.end(), [&](int c) { return c > (previousTime + beat.timeMS) / 2 - 100 && c <= beat.timeMS + 100; }) ? 1 : 0;
            }
            const auto standardize = [](std::vector<double>& values) {
                const double mean = std::accumulate(values.begin(), values.end(), 0.0) / static_cast<double>(values.size());
                double sum = 0;
                for (double value : values) {
                    sum += (value - mean) * (value - mean);
                }
                const double deviation = std::sqrt(sum / static_cast<double>(values.size()));
                for (double& value : values) {
                    value = deviation < 1e-3 ? 0 : (value - mean) / deviation;
                }
            };
            standardize(strength);
            standardize(bass);
            standardize(change);
            std::vector<double> accent(count);
            for (int i = 0; i < count; ++i) {
                accent[i] = strength[i] + bass[i] + 0.5 * change[i];
            }

            struct Hypothesis {
                int meter;
                int phase;
                double score;
            };
            std::vector<Hypothesis> hypotheses;
            const auto scoreOf = [&](int meter, int phase, int fromBeat, int toBeat) {
                double on = 0, off = 0;
                int onCount = 0, offCount = 0;
                for (int i = fromBeat; i < toBeat; ++i) {
                    if (i % meter == phase) {
                        on += accent[i];
                        ++onCount;
                    } else {
                        off += accent[i];
                        ++offCount;
                    }
                }
                return (onCount == 0 || offCount == 0) ? 0.0 : on / onCount - off / offCount;
            };
            for (int meter : { 3, 4 }) {
                if (_options.beatsPerBarOverride != 0 && meter != _options.beatsPerBarOverride) {
                    continue;
                }
                for (int phase = 0; phase < meter; ++phase) {
                    hypotheses.push_back({ meter, phase, scoreOf(meter, phase, 0, count) });
                }
            }
            if (hypotheses.size() < 2) {
                continue;
            }
            std::sort(hypotheses.begin(), hypotheses.end(), [](const Hypothesis& a, const Hypothesis& b) { return a.score > b.score; });
            const Hypothesis& top = hypotheses[0];

            // The same answer must keep coming from separate stretches of the song.
            double agreeing = 0;
            int blocks = 0;
            const int blockSize = top.meter * 4;
            for (int from = 0; from + blockSize <= count; from += blockSize) {
                double bestScore = -1e18;
                int bestPhase = 0;
                for (int phase = 0; phase < top.meter; ++phase) {
                    const double value = scoreOf(top.meter, phase, from, from + blockSize);
                    if (value > bestScore) {
                        bestScore = value;
                        bestPhase = phase;
                    }
                }
                // In a bar of four, accents on the first and third beats are the same
                // bar read half a bar late: it is the pair of beats that agrees.
                agreeing += bestPhase == top.phase ? 1.0 : (top.meter == 4 && bestPhase == (top.phase + 2) % 4) ? 0.5 : 0.0;
                ++blocks;
            }
            const double consistency = blocks == 0 ? 0 : agreeing / blocks;
            const double margin = top.score - hypotheses[1].score;
            // The scores are differences of standardized accents: over a whole song, two
            // thirds of a deviation between the downbeat and the rest is already a bar.
            const double confidence = Clamp01(std::min(margin, top.score) / 1.2) * consistency;
            if (confidence < MIN_DOWNBEAT_CONFIDENCE) {
                continue;
            }
            for (int i = 0; i < count; ++i) {
                const int position = ((i - top.phase) % top.meter + top.meter) % top.meter;
                _beats[first + i].barPosition = position;
                if (position == 0) {
                    MusicalEvent event = _beats[first + i];
                    event.type = MusicalEventType::Downbeat;
                    event.confidence = Round3(std::min(event.confidence, confidence));
                    event.source = SOURCE_DOWNBEAT;
                    _downbeats.push_back(std::move(event));
                }
            }
            bars[top.meter] += count;
            bestConfidences.push_back(confidence);
        }
        if (_downbeats.empty()) {
            _a.warnings.push_back("downbeat-undetermined");
            return;
        }
        _a.beatsPerBar = bars[4] >= bars[3] ? 4 : 3;
        _a.barConfidence = Round3(Median(bestConfidences));
    }

    // Loudness alone would call any loud sustained note intense, so how busy the music
    // is and how bright it sounds count too.
    void StageIntensity() {
        const int interval = _options.energyIntervalMS;
        if (_a.energy.empty()) {
            return;
        }
        std::vector<double> density(_a.energy.size(), 0);
        for (const MusicalEvent& onset : _onsets) {
            const int center = onset.timeMS / interval;
            const int reach = 1000 / interval;
            for (int i = std::max(0, center - reach); i <= center + reach && i < static_cast<int>(density.size()); ++i) {
                density[i] += onset.intensity;
            }
        }
        const double topDensity = std::max(1e-6, Percentile(density, 0.95));
        for (size_t i = 0; i < _a.energy.size(); ++i) {
            EnergyPoint& point = _a.energy[i];
            point.intensity = point.total <= 0 ? 0 : Round3(Clamp01(0.6 * point.total + 0.25 * Clamp01(density[i] / topDensity) + 0.15 * point.high));
        }
        constexpr int MIN_LOW_ENERGY_MS = 2000;
        int start = -1;
        for (size_t i = 0; i <= _a.energy.size(); ++i) {
            const bool low = i < _a.energy.size() && _a.energy[i].intensity < 0.15 && !InSilence(_a.energy[i].timeMS);
            if (low && start < 0) {
                start = static_cast<int>(i);
            } else if (!low && start >= 0) {
                const int from = start * interval;
                const int to = std::min(_a.durationMS, static_cast<int>(i) * interval);
                if (to - from >= MIN_LOW_ENERGY_MS) {
                    _a.lowEnergy.push_back({ from, to });
                }
                start = -1;
            }
        }
    }

    // The center-channel filter only tells that something sits in the middle of the mix,
    // which is often the bass or the kick, so this stays a hint with a low confidence.
    void StageVocals() {
        if (!_options.estimateVocals) {
            return;
        }
        const float* left = _audio.GetRawLeftDataPtr(0);
        const float* right = _audio.GetRawRightDataPtr(0);
        if (_audio.GetChannels() < 2 || left == nullptr || right == nullptr || left == right) {
            _a.warnings.push_back("vocal-estimate-unavailable-mono");
            return;
        }
        double side = 0, mid = 0;
        const long track = _audio.GetTrackSize();
        for (long i = 0; i < track; i += 16) {
            const double m = 0.5 * (left[i] + right[i]);
            const double s = 0.5 * (left[i] - right[i]);
            mid += m * m;
            side += s * s;
        }
        if (side < mid * 0.0004) {
            _a.warnings.push_back("vocal-estimate-unavailable-no-stereo-width");
            return;
        }
        if (IsCancelled()) {
            return;
        }
        const FilteredAudioData* filtered = _audio.EnsureFilteredAudioData(AUDIOSAMPLETYPE::VOCALS, 0, 0);
        if (filtered == nullptr || filtered->data0 == nullptr) {
            _a.warnings.push_back("vocal-estimate-unavailable-filter");
            return;
        }
        constexpr int FRAME_MS = 250;
        const long samples = _audio.GetRate() * FRAME_MS / 1000;
        const int frames = static_cast<int>(track / samples);
        std::vector<double> level(frames, 0);
        for (int f = 0; f < frames; ++f) {
            double sum = 0;
            for (long i = 0; i < samples; ++i) {
                sum += static_cast<double>(filtered->data0[f * samples + i]) * filtered->data0[f * samples + i];
            }
            level[f] = std::sqrt(sum / static_cast<double>(samples));
        }
        std::vector<double> active;
        for (int f = 0; f < frames; ++f) {
            if (!InSilence(f * FRAME_MS)) {
                active.push_back(level[f]);
            }
        }
        const double top = Percentile(active, 0.95);
        const double bottom = Percentile(active, 0.10);
        if (active.empty() || top <= 1e-4 || top < bottom * 4) {
            _a.warnings.push_back("vocal-estimate-inconclusive");
            return;
        }
        int start = -1;
        const auto close = [&](int end) {
            const int from = start * FRAME_MS;
            const int to = std::min(_a.durationMS, end * FRAME_MS);
            if (!_a.vocalSegments.empty() && from - _a.vocalSegments.back().endMS < 500) {
                _a.vocalSegments.back().endMS = to;
            } else {
                _a.vocalSegments.push_back({ from, to, MAX_VOCAL_CONFIDENCE, SOURCE_VOCAL });
            }
        };
        for (int f = 0; f <= frames; ++f) {
            const bool on = f < frames && level[f] > top * 0.25 && !InSilence(f * FRAME_MS);
            if (on && start < 0) {
                start = f;
            } else if (!on && start >= 0) {
                close(f);
                start = -1;
            }
        }
        _a.vocalSegments.erase(std::remove_if(_a.vocalSegments.begin(), _a.vocalSegments.end(), [](const VocalSegment& s) { return s.endMS - s.startMS < 1000; }), _a.vocalSegments.end());
        if (_a.vocalSegments.empty()) {
            _a.warnings.push_back("vocal-estimate-inconclusive");
        } else {
            _a.warnings.push_back("vocal-estimate-low-confidence");
        }
    }

    double VocalShare(int startMS, int endMS) const {
        int covered = 0;
        for (const VocalSegment& segment : _a.vocalSegments) {
            covered += std::max(0, std::min(endMS, segment.endMS) - std::max(startMS, segment.startMS));
        }
        return endMS > startMS ? static_cast<double>(covered) / (endMS - startMS) : 0;
    }

    // Timbre and harmony on a half-second grid, each dimension standardized so that
    // neither loudness nor one loud band decides what counts as similar.
    std::vector<std::vector<float>> BuildFeatures(int& hop) const {
        const int interval = _options.energyIntervalMS;
        hop = std::max(1, FEATURE_HOP_MS / interval);
        const int frames = static_cast<int>(_a.energy.size()) / hop;
        constexpr int CHROMA = 12;
        constexpr int BANDS = 12;
        constexpr int DIMENSIONS = CHROMA + BANDS + 1;
        std::vector<std::vector<float>> features(frames, std::vector<float>(DIMENSIONS, 0.0f));
        for (int f = 0; f < frames; ++f) {
            std::vector<float>& feature = features[f];
            int used = 0;
            for (int i = f * hop; i < (f + 1) * hop; ++i) {
                const std::vector<float>* spectrum = _spectra[i];
                feature[CHROMA + BANDS] += static_cast<float>(_a.energy[i].total);
                if (spectrum == nullptr || spectrum->empty()) {
                    continue;
                }
                ++used;
                for (int n = 48; n < 96 && n < static_cast<int>(spectrum->size()); ++n) {
                    feature[n % CHROMA] += (*spectrum)[n];
                }
                for (int n = 24; n < 120 && n < static_cast<int>(spectrum->size()); ++n) {
                    feature[CHROMA + (n - 24) / 8] += (*spectrum)[n] / 8.0f;
                }
            }
            if (used > 0) {
                for (int d = 0; d < CHROMA + BANDS; ++d) {
                    feature[d] /= static_cast<float>(used);
                }
            }
            feature[CHROMA + BANDS] /= static_cast<float>(hop);
        }
        for (int d = 0; d < DIMENSIONS; ++d) {
            double mean = 0;
            int count = 0;
            for (int f = 0; f < frames; ++f) {
                if (!InSilence(f * FEATURE_HOP_MS)) {
                    mean += features[f][d];
                    ++count;
                }
            }
            mean /= std::max(1, count);
            double sum = 0;
            for (int f = 0; f < frames; ++f) {
                if (!InSilence(f * FEATURE_HOP_MS)) {
                    sum += (features[f][d] - mean) * (features[f][d] - mean);
                }
            }
            const double deviation = std::sqrt(sum / std::max(1, count));
            for (int f = 0; f < frames; ++f) {
                features[f][d] = (InSilence(f * FEATURE_HOP_MS) || deviation < 1e-4) ? 0.0f : static_cast<float>((features[f][d] - mean) / deviation);
            }
        }
        return features;
    }

    void StageSections() {
        if (_a.energy.empty() || _a.musicEndMS <= _a.musicStartMS) {
            SongSection whole;
            whole.endMS = _a.durationMS;
            whole.evidence = { "silence" };
            _a.sections.push_back(std::move(whole));
            return;
        }
        int hop = 1;
        const std::vector<std::vector<float>> features = BuildFeatures(hop);
        const int frames = static_cast<int>(features.size());
        if (frames < 8) {
            AddSections({ _a.musicStartMS, _a.musicEndMS }, {}, {});
            return;
        }

        std::vector<float> norm(frames, 0.0f);
        for (int f = 0; f < frames; ++f) {
            double sum = 0;
            for (float value : features[f]) {
                sum += value * value;
            }
            norm[f] = static_cast<float>(std::sqrt(sum));
        }
        std::vector<float> similarity(static_cast<size_t>(frames) * frames, 0.0f);
        for (int a = 0; a < frames; ++a) {
            if (a % 64 == 0 && IsCancelled()) {
                return;
            }
            for (int b = a; b < frames; ++b) {
                float value = 0;
                if (norm[a] > 1e-4f && norm[b] > 1e-4f) {
                    double dot = 0;
                    for (size_t d = 0; d < features[a].size(); ++d) {
                        dot += features[a][d] * features[b][d];
                    }
                    value = static_cast<float>(dot / (norm[a] * norm[b]));
                }
                similarity[static_cast<size_t>(a) * frames + b] = value;
                similarity[static_cast<size_t>(b) * frames + a] = value;
            }
        }

        // Foote's checkerboard: a boundary is where what comes before resembles itself
        // and what comes after resembles itself, but not each other.
        const int half = std::max(4, std::min(16, _options.minSectionMS / FEATURE_HOP_MS));
        std::vector<double> novelty(frames, 0);
        for (int t = half; t + half < frames; ++t) {
            if (t % 64 == 0 && IsCancelled()) {
                return;
            }
            double within = 0, across = 0;
            int withinCount = 0, acrossCount = 0;
            for (int a = t - half; a < t + half; ++a) {
                for (int b = t - half; b < t + half; ++b) {
                    if (a == b || norm[a] <= 1e-4f || norm[b] <= 1e-4f) {
                        continue;
                    }
                    if ((a < t) == (b < t)) {
                        within += similarity[static_cast<size_t>(a) * frames + b];
                        ++withinCount;
                    } else {
                        across += similarity[static_cast<size_t>(a) * frames + b];
                        ++acrossCount;
                    }
                }
            }
            if (withinCount > 0 && acrossCount > 0) {
                novelty[t] = within / withinCount - across / acrossCount;
            }
        }

        std::vector<std::pair<int, double>> candidates;
        const int minGap = std::max(1, _options.minSectionMS / FEATURE_HOP_MS);
        for (int t = 1; t + 1 < frames; ++t) {
            if (novelty[t] < MIN_SECTION_NOVELTY) {
                continue;
            }
            bool isPeak = true;
            for (int o = std::max(0, t - minGap / 2); o <= std::min(frames - 1, t + minGap / 2) && isPeak; ++o) {
                isPeak = novelty[o] <= novelty[t];
            }
            if (isPeak) {
                candidates.push_back({ t * FEATURE_HOP_MS, novelty[t] });
            }
        }

        std::vector<int> boundaries = { _a.musicStartMS, _a.musicEndMS };
        for (const TimeRange& silence : _silences) {
            boundaries.push_back(silence.startMS);
            boundaries.push_back(silence.endMS);
        }
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        const std::vector<int> hard = boundaries;

        std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        std::map<int, double> strengths;
        for (const auto& [time, strength] : candidates) {
            const int snapped = SnapToBeat(time);
            if (snapped <= _a.musicStartMS || snapped >= _a.musicEndMS || InSilence(snapped)) {
                continue;
            }
            const bool tooClose = std::any_of(boundaries.begin(), boundaries.end(), [&](int other) { return std::abs(other - snapped) < _options.minSectionMS; });
            if (!tooClose) {
                boundaries.push_back(snapped);
                strengths[snapped] = strength;
            }
        }
        std::sort(boundaries.begin(), boundaries.end());
        AddSections(boundaries, strengths, hard);
        FindRepeats(similarity, frames);
        ClassifySections();
    }

    int SnapToBeat(int timeMS) const {
        constexpr int REACH_DOWNBEAT_MS = 1000;
        constexpr int REACH_BEAT_MS = 500;
        int best = timeMS;
        int bestDistance = REACH_DOWNBEAT_MS + 1;
        for (const MusicalEvent& downbeat : _downbeats) {
            const int distance = std::abs(downbeat.timeMS - timeMS);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = downbeat.timeMS;
            }
        }
        if (bestDistance <= REACH_DOWNBEAT_MS) {
            return best;
        }
        bestDistance = REACH_BEAT_MS + 1;
        for (const MusicalEvent& beat : _beats) {
            const int distance = std::abs(beat.timeMS - timeMS);
            if (distance < bestDistance && beat.confidence >= 0.5) {
                bestDistance = distance;
                best = beat.timeMS;
            }
        }
        return best;
    }

    double MeanIntensity(int startMS, int endMS) const {
        double sum = 0;
        int count = 0;
        for (const EnergyPoint& point : _a.energy) {
            if (point.timeMS >= startMS && point.timeMS < endMS) {
                sum += point.intensity;
                ++count;
            }
        }
        return count == 0 ? 0 : sum / count;
    }

    void AddSections(std::vector<int> boundaries, const std::map<int, double>& strengths, const std::vector<int>& hard) {
        boundaries.push_back(0);
        boundaries.push_back(_a.durationMS);
        std::sort(boundaries.begin(), boundaries.end());
        boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());
        for (size_t i = 0; i + 1 < boundaries.size(); ++i) {
            SongSection section;
            section.startMS = boundaries[i];
            section.endMS = boundaries[i + 1];
            if (InSilence(section.startMS) && InSilence(section.endMS - 1)) {
                section.evidence = { "silence" };
                section.confidence = 0.9;
            } else {
                section.energy = Round3(MeanIntensity(section.startMS, section.endMS));
                const auto strength = strengths.find(section.startMS);
                const bool hardStart = std::find(hard.begin(), hard.end(), section.startMS) != hard.end();
                section.confidence = Round3(hardStart ? 0.6 : strength != strengths.end() ? Clamp01(strength->second) : 0.3);
                if (strength != strengths.end()) {
                    section.evidence.push_back("self-similarity-novelty");
                }
                if (hardStart) {
                    section.evidence.push_back("after-silence");
                }
            }
            _a.sections.push_back(std::move(section));
        }
    }

    bool IsSilentSection(const SongSection& section) const {
        return std::find(section.evidence.begin(), section.evidence.end(), "silence") != section.evidence.end();
    }

    // Same music means the same path through the similarity matrix, allowing for the
    // two passes starting a couple of seconds apart.
    void FindRepeats(const std::vector<float>& similarity, int frames) {
        const int count = static_cast<int>(_a.sections.size());
        constexpr int MAX_OFFSET = 4;
        std::vector<int> group(count);
        std::iota(group.begin(), group.end(), 0);
        const auto root = [&](int i) {
            while (group[i] != i) {
                i = group[i];
            }
            return i;
        };

        // What any two distant parts of this song share, to compare a repeat against:
        // a song built on one timbre looks similar to itself everywhere.
        std::vector<float> sample;
        for (int a = 0; a < frames; a += 4) {
            for (int b = a + 32; b < frames; b += 4) {
                sample.push_back(similarity[static_cast<size_t>(a) * frames + b]);
            }
        }
        const double typical = Percentile(sample, 0.5);

        for (int i = 0; i < count; ++i) {
            for (int j = i + 1; j < count; ++j) {
                const SongSection& first = _a.sections[i];
                const SongSection& second = _a.sections[j];
                if (IsSilentSection(first) || IsSilentSection(second)) {
                    continue;
                }
                const int lengthA = (first.endMS - first.startMS) / FEATURE_HOP_MS;
                const int lengthB = (second.endMS - second.startMS) / FEATURE_HOP_MS;
                if (std::min(lengthA, lengthB) < 8 || std::min(lengthA, lengthB) * 10 < std::max(lengthA, lengthB) * 7) {
                    continue;
                }
                const int startA = first.startMS / FEATURE_HOP_MS;
                const int startB = second.startMS / FEATURE_HOP_MS;
                double best = -1;
                for (int offset = -MAX_OFFSET; offset <= MAX_OFFSET; ++offset) {
                    double sum = 0;
                    int used = 0;
                    for (int t = 0; t < std::min(lengthA, lengthB); ++t) {
                        const int a = startA + t;
                        const int b = startB + t + offset;
                        if (a >= 0 && a < frames && b >= 0 && b < frames) {
                            sum += similarity[static_cast<size_t>(a) * frames + b];
                            ++used;
                        }
                    }
                    if (used > 0) {
                        best = std::max(best, sum / used);
                    }
                }
                if (best >= MIN_REPEAT_SIMILARITY && best - typical >= 0.2) {
                    group[root(j)] = root(i);
                    _repeatScore = std::max(_repeatScore, best);
                }
            }
        }
        std::map<int, int> members;
        for (int i = 0; i < count; ++i) {
            ++members[root(i)];
        }
        std::map<int, int> numbers;
        for (int i = 0; i < count; ++i) {
            const int r = root(i);
            if (members[r] >= 2) {
                numbers.try_emplace(r, static_cast<int>(numbers.size()));
                _a.sections[i].repeatGroup = numbers[r];
                _a.sections[i].evidence.push_back("self-similarity-repeat");
            }
        }
    }

    // Prudent on purpose: without lyrics nothing here knows what a section means. A name
    // only comes with the evidence that would justify it, and with a modest confidence.
    void ClassifySections() {
        std::vector<SongSection*> active;
        for (SongSection& section : _a.sections) {
            if (!IsSilentSection(section)) {
                active.push_back(&section);
            }
        }
        if (active.empty()) {
            return;
        }
        double weighted = 0;
        double total = 0;
        for (const SongSection* section : active) {
            weighted += section->energy * (section->endMS - section->startMS);
            total += section->endMS - section->startMS;
        }
        const double meanEnergy = total > 0 ? weighted / total : 0;
        const int musicLength = std::max(1, _a.musicEndMS - _a.musicStartMS);
        const auto label = [](SongSection& section, SectionKind kind, double confidence, std::vector<std::string> evidence) {
            section.kind = kind;
            section.confidence = Round3(confidence);
            section.evidence.insert(section.evidence.end(), evidence.begin(), evidence.end());
        };

        if (active.size() >= 3) {
            SongSection& first = *active.front();
            if (first.repeatGroup < 0 && first.energy < meanEnergy * 0.85 && (first.endMS - first.startMS) * 4 <= musicLength) {
                label(first, SectionKind::Intro, 0.4, { "first-section", "low-energy", "not-repeated" });
            }
            SongSection& last = *active.back();
            if (last.repeatGroup < 0 && last.energy < meanEnergy * 0.85 && (last.endMS - last.startMS) * 4 <= musicLength) {
                label(last, SectionKind::Outro, 0.4, { "last-section", "low-energy", "not-repeated" });
            }
        }

        std::map<int, std::vector<SongSection*>> groups;
        for (SongSection* section : active) {
            if (section->repeatGroup >= 0) {
                groups[section->repeatGroup].push_back(section);
            }
        }
        int chorus = -1;
        double chorusEnergy = 0;
        for (const auto& [id, members] : groups) {
            double sum = 0;
            for (const SongSection* member : members) {
                sum += member->energy;
            }
            sum /= static_cast<double>(members.size());
            if (sum > chorusEnergy) {
                chorusEnergy = sum;
                chorus = id;
            }
        }
        if (chorus >= 0) {
            double rest = 0;
            int restCount = 0;
            for (const SongSection* section : active) {
                if (section->repeatGroup != chorus && section->kind == SectionKind::Unknown) {
                    rest += section->energy;
                    ++restCount;
                }
            }
            // Repeating is not enough: it must also stand out from something else.
            if (restCount > 0 && chorusEnergy >= rest / restCount * 1.1) {
                for (SongSection* member : groups[chorus]) {
                    label(*member, SectionKind::Chorus, 0.45, { "repeat-group", "energy-above-rest", "no-lyrics-evidence" });
                }
                for (const auto& [id, members] : groups) {
                    if (id != chorus) {
                        for (SongSection* member : members) {
                            label(*member, SectionKind::Verse, 0.3, { "repeat-group", "energy-below-chorus", "no-lyrics-evidence" });
                        }
                    }
                }
                bool chorusSeen = false;
                int chorusesLeft = static_cast<int>(groups[chorus].size());
                for (SongSection* section : active) {
                    if (section->repeatGroup == chorus) {
                        chorusSeen = true;
                        --chorusesLeft;
                    } else if (chorusSeen && chorusesLeft > 0 && section->repeatGroup < 0 && section->kind == SectionKind::Unknown) {
                        label(*section, SectionKind::Bridge, 0.25, { "between-choruses", "not-repeated", "no-lyrics-evidence" });
                    }
                }
            }
        }

        // Quiet is not instrumental: only a voice estimated elsewhere and absent here is.
        if (!_a.vocalSegments.empty() && VocalShare(_a.musicStartMS, _a.musicEndMS) >= 0.3) {
            for (SongSection* section : active) {
                if (section->kind == SectionKind::Unknown && VocalShare(section->startMS, section->endMS) < 0.1) {
                    label(*section, SectionKind::Instrumental, 0.25, { "vocal-estimate-absent" });
                }
            }
        }
    }

    void StageHighlights() {
        const int interval = _options.energyIntervalMS;
        if (_a.energy.empty() || _a.musicEndMS <= _a.musicStartMS) {
            return;
        }
        const int reach = std::max(1, 1500 / interval);
        std::vector<double> smooth(_a.energy.size(), 0);
        for (int i = 0; i < static_cast<int>(smooth.size()); ++i) {
            double sum = 0;
            int count = 0;
            for (int o = std::max(0, i - reach); o <= i + reach && o < static_cast<int>(smooth.size()); ++o) {
                sum += _a.energy[o].intensity;
                ++count;
            }
            smooth[i] = sum / count;
        }
        std::vector<double> active;
        for (size_t i = 0; i < smooth.size(); ++i) {
            if (_a.energy[i].intensity > 0) {
                active.push_back(smooth[i]);
            }
        }
        const double typical = Median(active);
        const int peak = static_cast<int>(std::max_element(smooth.begin(), smooth.end()) - smooth.begin());
        // A song at one level throughout has no high point to name.
        if (smooth[peak] >= typical * 1.25) {
            int from = peak;
            int to = peak;
            while (from > 0 && smooth[from - 1] >= smooth[peak] * 0.9) {
                --from;
            }
            while (to + 1 < static_cast<int>(smooth.size()) && smooth[to + 1] >= smooth[peak] * 0.9) {
                ++to;
            }
            MusicalEvent climax;
            climax.timeMS = from * interval;
            climax.durationMS = (to - from + 1) * interval;
            climax.type = MusicalEventType::Climax;
            climax.intensity = Round3(smooth[peak]);
            climax.confidence = Round3(Clamp01((smooth[peak] / std::max(typical, 1e-3) - 1.0)));
            climax.source = SOURCE_ENERGY;
            _others.push_back(std::move(climax));
        } else {
            _a.warnings.push_back("no-climax");
        }

        int last = -1;
        for (int i = static_cast<int>(smooth.size()) - 1; i >= 0; --i) {
            if (_a.energy[i].timeMS < _a.musicEndMS && smooth[i] >= typical * 0.4) {
                last = i;
                break;
            }
        }
        MusicalEvent ending;
        ending.timeMS = last >= 0 ? std::min(_a.musicEndMS, last * interval) : _a.musicEndMS;
        ending.durationMS = _a.musicEndMS - ending.timeMS;
        ending.type = MusicalEventType::Ending;
        ending.intensity = last >= 0 ? Round3(smooth[last]) : 0;
        ending.confidence = 0.6;
        ending.source = SOURCE_ENERGY;
        _others.push_back(std::move(ending));

        for (const TimeRange& silence : _silences) {
            MusicalEvent event;
            event.timeMS = silence.startMS;
            event.durationMS = silence.endMS - silence.startMS;
            event.type = MusicalEventType::Silence;
            event.confidence = 0.9;
            event.source = SOURCE_SILENCE;
            _others.push_back(std::move(event));
        }
        for (const VocalSegment& segment : _a.vocalSegments) {
            MusicalEvent start;
            start.timeMS = segment.startMS;
            start.type = MusicalEventType::VocalStart;
            start.confidence = segment.confidence;
            start.source = segment.method;
            _others.push_back(start);
            start.timeMS = segment.endMS;
            start.type = MusicalEventType::VocalEnd;
            _others.push_back(start);
        }
    }

    void StageEvents() {
        _a.events = std::move(_beats);
        _a.events.insert(_a.events.end(), _downbeats.begin(), _downbeats.end());
        _a.events.insert(_a.events.end(), _onsets.begin(), _onsets.end());
        _a.events.insert(_a.events.end(), _others.begin(), _others.end());
    }

    AudioManager& _audio;
    const AnalysisOptions& _options;
    const AnalysisProgress& _progress;
    const AnalysisCancel& _cancel;
    AnalysisResult& _result;
    SongAnalysis& _a;
    bool _cancelled = false;

    std::vector<double> _rms;
    std::vector<const std::vector<float>*> _spectra;
    std::vector<TimeRange> _silences;
    std::vector<float> _envelope;
    int _envelopeRate = 44100;
    double _detectTempoBpm = 0;
    std::vector<double> _period;
    std::vector<double> _rhythmicity;
    int _tempoStep = 1;
    std::vector<std::pair<int, int>> _segments; // beat ranges, one per stretch of music
    std::vector<MusicalEvent> _beats;
    std::vector<MusicalEvent> _downbeats;
    std::vector<MusicalEvent> _onsets;
    std::vector<MusicalEvent> _others;
    std::vector<int> _chordChanges;
    double _repeatScore = 0;
};

void AddSectionEvents(SongAnalysis& analysis) {
    analysis.events.erase(std::remove_if(analysis.events.begin(), analysis.events.end(), [](const MusicalEvent& e) { return e.type == MusicalEventType::SectionStart || e.type == MusicalEventType::SectionEnd; }), analysis.events.end());
    for (const SongSection& section : analysis.sections) {
        const bool silent = std::find(section.evidence.begin(), section.evidence.end(), "silence") != section.evidence.end();
        if (silent) {
            continue;
        }
        MusicalEvent start;
        start.timeMS = section.startMS;
        start.type = MusicalEventType::SectionStart;
        start.intensity = section.energy;
        start.confidence = section.confidence;
        start.source = section.provenance == Provenance::Detected ? SOURCE_SECTION : SOURCE_MANUAL;
        start.provenance = section.provenance;
        analysis.events.push_back(start);
        start.timeMS = section.endMS;
        start.type = MusicalEventType::SectionEnd;
        analysis.events.push_back(start);
    }
}

// Sorted by what a reader looks for first, with every tie broken, so that two runs
// produce the same order and the JSON is stable.
void Normalize(SongAnalysis& analysis) {
    AddSectionEvents(analysis);
    for (MusicalEvent& event : analysis.events) {
        event.sectionIndex = -1;
        for (size_t s = 0; s < analysis.sections.size(); ++s) {
            const SongSection& section = analysis.sections[s];
            if (event.timeMS >= section.startMS && (event.timeMS < section.endMS || (s + 1 == analysis.sections.size() && event.timeMS == section.endMS))) {
                event.sectionIndex = static_cast<int>(s);
                break;
            }
        }
    }
    std::sort(analysis.events.begin(), analysis.events.end(), [](const MusicalEvent& a, const MusicalEvent& b) {
        if (a.timeMS != b.timeMS) {
            return a.timeMS < b.timeMS;
        }
        if (a.type != b.type) {
            return static_cast<int>(a.type) < static_cast<int>(b.type);
        }
        return a.beatIndex < b.beatIndex;
    });
}

Provenance ProvenanceFor(bool locked) {
    return locked ? Provenance::Locked : Provenance::Corrected;
}

const char* const EVENT_IDS[] = { "beat", "downbeat", "onset", "sectionStart", "sectionEnd", "climax", "silence", "vocalStart", "vocalEnd", "ending" };
const char* const SECTION_IDS[] = { "intro", "verse", "chorus", "bridge", "instrumental", "outro", "unknown", "pre-chorus", "interlude" };
const char* const PROVENANCE_IDS[] = { "detected", "corrected", "locked", "confirmed" };
const char* const ACTION_IDS[] = { "add", "move", "remove", "confirm" };

template <typename Enum, size_t N>
Enum EnumFromId(const char* const (&ids)[N], const std::string& id, Enum fallback) {
    for (size_t i = 0; i < N; ++i) {
        if (id == ids[i]) {
            return static_cast<Enum>(i);
        }
    }
    return fallback;
}

using nlohmann::json;

bool WriteAtomically(const std::string& path, const std::string& content, std::string& error) {
    const std::string temporary = path + ".composer-tmp";
    {
        std::ofstream out(fs::path(temporary), std::ios::binary | std::ios::trunc);
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        out.close();
        if (!out) {
            error = fmt::format("cannot write '{}'", temporary);
            std::error_code ignored;
            fs::remove(temporary, ignored);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        error = fmt::format("cannot replace '{}': {}", path, ec.message());
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

bool ReadWholeFile(const std::string& path, std::string& content) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        return false;
    }
    content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    return true;
}

void Fail(AnalysisResult& result, const char* code, const std::string& detail) {
    result.status = AnalysisStatus::Failed;
    result.errorCode = code;
    result.errorDetail = detail;
}

AnalysisResult AnalyzeWithHash(const std::string& audioPath, const std::string& hash, const AnalysisOptions& options, const AnalysisProgress& progress, const AnalysisCancel& cancel) {
    AnalysisResult result;
    SongAnalysis& analysis = result.analysis;
    if (options.energyIntervalMS < 10 || options.energyIntervalMS > 1000 || options.onsetSensitivity <= 0 || options.minBpm <= 0 || options.maxBpm <= options.minBpm) {
        Fail(result, "ANALYZER_INVALID_OPTIONS", "");
        return result;
    }
    analysis.audioHash = hash;
    analysis.optionsKey = OptionsKey(options);
    std::error_code ec;
    analysis.fileName = fs::path(audioPath).filename().string();
    analysis.fileSize = static_cast<long>(fs::file_size(fs::path(audioPath), ec));

    if (progress) {
        progress(5, "decode");
    }
    const auto start = std::chrono::steady_clock::now();
    // Private, and never handed to anything else: the player's own instance keeps its
    // position, filters and frame data whatever this one does.
    AudioManager audio(audioPath);
    result.stageMS["decode"] = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (!audio.IsOk() || audio.GetTrackSize() <= 0 || audio.LengthMS() <= 0) {
        Fail(result, "ANALYZER_UNSUPPORTED_AUDIO", audio.GetResultMessage());
        return result;
    }
    constexpr long MIN_DURATION_MS = 2000;
    if (audio.LengthMS() < MIN_DURATION_MS) {
        Fail(result, "ANALYZER_AUDIO_TOO_SHORT", std::to_string(audio.LengthMS()));
        return result;
    }
    analysis.durationMS = static_cast<int>(audio.LengthMS());
    analysis.sampleRate = static_cast<int>(audio.GetRate());
    analysis.sourceSampleRate = static_cast<int>(audio.GetSampleRate());
    analysis.channels = audio.GetChannels();
    analysis.metadata = audio.GetMetaData();
    if (!audio.Title().empty()) {
        analysis.metadata["title"] = audio.Title();
    }
    if (!audio.Artist().empty()) {
        analysis.metadata["artist"] = audio.Artist();
    }
    if (!audio.Album().empty()) {
        analysis.metadata["album"] = audio.Album();
    }

    Analysis run(audio, options, progress, cancel, result);
    run.Run();
    if (run.IsCancelled()) {
        result.status = AnalysisStatus::Cancelled;
        result.analysis = SongAnalysis{};
        return result;
    }
    Normalize(analysis);
    std::sort(analysis.warnings.begin(), analysis.warnings.end());
    result.status = AnalysisStatus::Ok;
    if (progress) {
        progress(100, "done");
    }
    return result;
}

} // namespace

const char* EventTypeId(MusicalEventType type) {
    return EVENT_IDS[static_cast<int>(type)];
}

const char* SectionKindId(SectionKind kind) {
    return SECTION_IDS[static_cast<int>(kind)];
}

const char* ProvenanceId(Provenance provenance) {
    return PROVENANCE_IDS[static_cast<int>(provenance)];
}

std::string OptionsKey(const AnalysisOptions& options) {
    return fmt::format("interval={};sensitivity={:.3f};minOnset={:.3f};bpm={:.1f}-{:.1f};silence={:.1f};minSilence={};minSection={};bar={};vocals={}", options.energyIntervalMS, options.onsetSensitivity, options.minOnsetIntensity, options.minBpm, options.maxBpm, options.silenceDb, options.minSilenceMS, options.minSectionMS, options.beatsPerBarOverride, options.estimateVocals ? 1 : 0);
}

std::string Sha256OfFile(const std::string& path, const AnalysisCancel& cancel) {
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        return "";
    }
    Sha256 sha;
    std::vector<char> buffer(1 << 20);
    while (in) {
        if (cancel && cancel()) {
            return "";
        }
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            sha.Update(reinterpret_cast<const unsigned char*>(buffer.data()), static_cast<size_t>(got));
        }
    }
    return sha.Finish();
}

AnalysisResult AnalyzeSong(const std::string& audioPath, const AnalysisOptions& options, const AnalysisProgress& progress, const AnalysisCancel& cancel) {
    AnalysisResult result;
    ObtainAccessToURL(audioPath, false);
    std::error_code ec;
    if (!FileExists(audioPath)) {
        Fail(result, "ANALYZER_FILE_NOT_FOUND", audioPath);
        return result;
    }
    if (fs::file_size(fs::path(audioPath), ec) == 0 || ec) {
        Fail(result, "ANALYZER_FILE_EMPTY", audioPath);
        return result;
    }
    if (progress) {
        progress(1, "hash");
    }
    const auto start = std::chrono::steady_clock::now();
    const std::string hash = Sha256OfFile(audioPath, cancel);
    const auto hashMS = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    if (hash.empty()) {
        if (cancel && cancel()) {
            result.status = AnalysisStatus::Cancelled;
        } else {
            Fail(result, "ANALYZER_FILE_UNREADABLE", audioPath);
        }
        return result;
    }
    result = AnalyzeWithHash(audioPath, hash, options, progress, cancel);
    result.stageMS["hash"] = hashMS;
    return result;
}

AnalysisResult AnalyzeSongCached(const std::string& audioPath, const std::string& jsonPath, const AnalysisOptions& options, const AnalysisProgress& progress, const AnalysisCancel& cancel) {
    AnalysisResult result;
    ObtainAccessToURL(jsonPath, true);
    if (jsonPath.empty()) {
        return AnalyzeSong(audioPath, options, progress, cancel);
    }
    std::error_code ec;
    if (fs::equivalent(fs::path(audioPath), fs::path(jsonPath), ec) && !ec) {
        Fail(result, "ANALYZER_OUTPUT_IS_THE_AUDIO", jsonPath);
        return result;
    }

    SongAnalysis previous;
    bool hasPrevious = false;
    if (FileExists(jsonPath)) {
        std::string text;
        std::string error;
        // Someone else's file, or one damaged by hand. Its corrections may be the only
        // copy, so it is reported rather than replaced.
        if (!ReadWholeFile(jsonPath, text) || !FromJson(text, previous, error)) {
            Fail(result, "ANALYZER_CACHE_UNREADABLE", fmt::format("{}: {}", jsonPath, error));
            return result;
        }
        hasPrevious = true;
    }
    if (hasPrevious && FileExists(audioPath)) {
        const std::string hash = Sha256OfFile(audioPath, cancel);
        if (hash.empty() && cancel && cancel()) {
            result.status = AnalysisStatus::Cancelled;
            return result;
        }
        if (hash == previous.audioHash && previous.schemaVersion == ANALYSIS_SCHEMA_VERSION && previous.analyzerVersion == ANALYZER_VERSION && previous.optionsKey == OptionsKey(options)) {
            result.status = AnalysisStatus::Ok;
            result.reusedCache = true;
            result.analysis = std::move(previous);
            if (progress) {
                progress(100, "cached");
            }
            return result;
        }
    }

    result = AnalyzeSong(audioPath, options, progress, cancel);
    if (result.status != AnalysisStatus::Ok) {
        return result;
    }
    if (hasPrevious) {
        result.analysis.corrections = previous.corrections;
        const bool anyCorrection = !previous.corrections.events.empty() || !previous.corrections.sections.empty() || previous.corrections.bar.firstDownbeatMS >= 0;
        if (anyCorrection && previous.audioHash != result.analysis.audioHash) {
            result.analysis.warnings.push_back("corrections-from-other-audio");
            std::sort(result.analysis.warnings.begin(), result.analysis.warnings.end());
        }
        ApplyCorrections(result.analysis);
    }
    std::string error;
    if (!WriteAtomically(jsonPath, ToJson(result.analysis), error)) {
        Fail(result, "ANALYZER_CACHE_WRITE_FAILED", error);
    }
    return result;
}

SongAnalyzer::~SongAnalyzer() {
    _cancel = true;
    if (_worker.valid()) {
        _worker.wait();
    }
}

void SongAnalyzer::Start(const std::string& audioPath, const std::string& jsonPath, const AnalysisOptions& options) {
    if (_worker.valid()) {
        _cancel = true;
        _worker.wait();
    }
    _cancel = false;
    _progress = 0;
    _worker = std::async(std::launch::async, [this, audioPath, jsonPath, options]() {
        return AnalyzeSongCached(
            audioPath, jsonPath, options, [this](int percent, const char*) { _progress = percent; }, [this]() { return _cancel.load(); });
    });
}

bool SongAnalyzer::IsDone() const {
    return !_worker.valid() || _worker.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

AnalysisResult SongAnalyzer::Wait() {
    AnalysisResult result;
    if (_worker.valid()) {
        result = _worker.get();
    }
    return result;
}

MusicSnapshot SnapshotAt(const SongAnalysis& analysis, int timeMS) {
    MusicSnapshot snapshot;
    snapshot.timeMS = timeMS;
    for (size_t s = 0; s < analysis.sections.size(); ++s) {
        const SongSection& section = analysis.sections[s];
        if (timeMS >= section.startMS && timeMS < section.endMS) {
            snapshot.sectionIndex = static_cast<int>(s);
            snapshot.sectionKind = section.kind;
            snapshot.sectionConfidence = section.confidence;
            break;
        }
    }
    if (!analysis.energy.empty()) {
        const auto after = std::lower_bound(analysis.energy.begin(), analysis.energy.end(), timeMS, [](const EnergyPoint& p, int t) { return p.timeMS < t; });
        const EnergyPoint& point = after == analysis.energy.end() ? analysis.energy.back() : (after == analysis.energy.begin() || after->timeMS == timeMS) ? *after : *(after - 1);
        snapshot.energy = point;
    }
    int bestBeat = -1;
    int bestOnset = -1;
    double beatConfidence = 0;
    for (const MusicalEvent& event : analysis.events) {
        const int distance = std::abs(event.timeMS - timeMS);
        if (event.type == MusicalEventType::Beat && (bestBeat < 0 || distance < std::abs(bestBeat - timeMS))) {
            bestBeat = event.timeMS;
            beatConfidence = event.confidence;
        } else if (event.type == MusicalEventType::Onset && (bestOnset < 0 || distance < std::abs(bestOnset - timeMS))) {
            bestOnset = event.timeMS;
        } else if (event.type == MusicalEventType::Silence && timeMS >= event.timeMS && timeMS < event.timeMS + event.durationMS) {
            snapshot.inSilence = true;
        }
        if (distance <= 250 && event.type != MusicalEventType::Beat && event.type != MusicalEventType::Onset) {
            snapshot.nearbyEvents.push_back(event);
        }
    }
    snapshot.nearestBeatMS = bestBeat;
    snapshot.nearestOnsetMS = bestOnset;
    if (!analysis.vocalSegments.empty()) {
        snapshot.vocalActivity = "absent";
        snapshot.vocalConfidence = analysis.vocalSegments.front().confidence;
        for (const VocalSegment& segment : analysis.vocalSegments) {
            if (timeMS >= segment.startMS && timeMS < segment.endMS) {
                snapshot.vocalActivity = "present";
                break;
            }
        }
    }
    snapshot.confidence = Round3((snapshot.sectionConfidence + beatConfidence) / 2.0);
    return snapshot;
}

namespace {

// Halving keeps the beats of the stronger phase; doubling adds one in the middle of
// each pair that is a beat apart. The downbeats on beats that stay are kept, and the
// bar is counted again from them.
void ApplyTempoFactor(SongAnalysis& analysis) {
    const double factor = analysis.corrections.tempoFactor;
    if (factor == analysis.appliedTempoFactor || analysis.appliedTempoFactor != 1 || (factor != 0.5 && factor != 2.0)) {
        return;
    }
    auto& events = analysis.events;
    std::vector<MusicalEvent> beats;
    std::vector<int> downbeatTimes;
    for (const MusicalEvent& e : events) {
        if (e.type == MusicalEventType::Beat) {
            beats.push_back(e);
        } else if (e.type == MusicalEventType::Downbeat) {
            downbeatTimes.push_back(e.timeMS);
        }
    }
    std::sort(beats.begin(), beats.end(), [](const MusicalEvent& a, const MusicalEvent& b) { return a.timeMS < b.timeMS; });
    if (beats.size() < 4) {
        return;
    }
    std::vector<MusicalEvent> kept;
    if (factor == 0.5) {
        double phase[2] = { 0, 0 };
        for (size_t b = 0; b < beats.size(); ++b) {
            const bool down = std::find(downbeatTimes.begin(), downbeatTimes.end(), beats[b].timeMS) != downbeatTimes.end();
            phase[b % 2] += beats[b].intensity + (down ? 1.0 : 0.0);
        }
        const size_t chosen = phase[1] > phase[0] ? 1 : 0;
        for (size_t b = chosen; b < beats.size(); b += 2) {
            kept.push_back(beats[b]);
        }
    } else {
        std::vector<double> gaps;
        for (size_t b = 1; b < beats.size(); ++b) {
            gaps.push_back(beats[b].timeMS - beats[b - 1].timeMS);
        }
        const double typical = Median(gaps);
        for (size_t b = 0; b < beats.size(); ++b) {
            kept.push_back(beats[b]);
            if (b + 1 < beats.size() && beats[b + 1].timeMS - beats[b].timeMS <= typical * 1.5) {
                MusicalEvent middle = beats[b];
                middle.timeMS = (beats[b].timeMS + beats[b + 1].timeMS) / 2;
                middle.intensity = Round3(std::min(beats[b].intensity, beats[b + 1].intensity) * 0.5);
                middle.source = SOURCE_MANUAL;
                kept.push_back(std::move(middle));
            }
        }
    }
    std::vector<MusicalEvent> downbeats;
    int index = 0;
    int position = -1;
    std::vector<int> barLengths;
    for (MusicalEvent& beat : kept) {
        beat.beatIndex = index++;
        if (std::find(downbeatTimes.begin(), downbeatTimes.end(), beat.timeMS) != downbeatTimes.end()) {
            if (position > 0) {
                barLengths.push_back(position + 1);
            }
            position = 0;
            for (const MusicalEvent& e : events) {
                if (e.type == MusicalEventType::Downbeat && e.timeMS == beat.timeMS) {
                    downbeats.push_back(e);
                    break;
                }
            }
        } else if (position >= 0) {
            ++position;
        }
        beat.barPosition = position;
    }
    events.erase(std::remove_if(events.begin(), events.end(), [](const MusicalEvent& e) { return e.type == MusicalEventType::Beat || e.type == MusicalEventType::Downbeat; }), events.end());
    events.insert(events.end(), kept.begin(), kept.end());
    events.insert(events.end(), downbeats.begin(), downbeats.end());
    std::stable_sort(events.begin(), events.end(), [](const MusicalEvent& a, const MusicalEvent& b) { return a.timeMS < b.timeMS; });
    analysis.globalBpm = Round3(analysis.globalBpm * factor);
    for (TempoPoint& point : analysis.tempo) {
        point.bpm = Round3(point.bpm * factor);
    }
    std::vector<double> lengths(barLengths.begin(), barLengths.end());
    analysis.beatsPerBar = lengths.empty() ? 0 : static_cast<int>(std::lround(Median(lengths)));
    analysis.appliedTempoFactor = factor;
}

} // namespace

void ApplyCorrections(SongAnalysis& analysis) {
    ApplyTempoFactor(analysis);
    auto& events = analysis.events;
    const auto applyEvents = [&](bool beats, const auto& findDetected) {
        for (const EventCorrection& correction : analysis.corrections.events) {
            if ((correction.type == MusicalEventType::Beat) != beats) {
                continue;
            }
            if (correction.action == EventCorrection::Action::Remove || correction.action == EventCorrection::Action::Move) {
                const auto found = findDetected(correction.type, correction.originalMS);
                if (found != events.end()) {
                    events.erase(found);
                }
            }
            if (correction.action == EventCorrection::Action::Confirm) {
                const auto found = findDetected(correction.type, correction.originalMS);
                if (found != events.end()) {
                    found->provenance = correction.locked ? Provenance::Locked : Provenance::Confirmed;
                }
            }
            if (correction.action == EventCorrection::Action::Add || correction.action == EventCorrection::Action::Move) {
                const auto duplicate = std::find_if(events.begin(), events.end(), [&](const MusicalEvent& e) { return e.type == correction.type && e.timeMS == correction.timeMS && e.provenance != Provenance::Detected; });
                if (duplicate == events.end()) {
                    const auto near = findDetected(correction.type, correction.timeMS);
                    if (near != events.end()) {
                        events.erase(near);
                    }
                    MusicalEvent event;
                    event.timeMS = correction.timeMS;
                    event.durationMS = correction.durationMS;
                    event.type = correction.type;
                    event.confidence = 1;
                    event.intensity = 1;
                    event.source = SOURCE_MANUAL;
                    event.provenance = ProvenanceFor(correction.locked);
                    events.push_back(std::move(event));
                }
            }
        }
    };
    const auto findDetected = [&](MusicalEventType type, int timeMS) {
        auto best = events.end();
        int bestDistance = EVENT_MATCH_MS + 1;
        for (auto it = events.begin(); it != events.end(); ++it) {
            const int distance = std::abs(it->timeMS - timeMS);
            if (it->type == type && it->provenance == Provenance::Detected && distance < bestDistance) {
                best = it;
                bestDistance = distance;
            }
        }
        return best;
    };

    // Beats first, because the bar is counted over them, and whatever else after it,
    // because a downbeat added by hand must survive the bar being laid out again.
    applyEvents(true, findDetected);
    const BarCorrection& bar = analysis.corrections.bar;
    if (bar.firstDownbeatMS >= 0 && bar.beatsPerBar > 0) {
        events.erase(std::remove_if(events.begin(), events.end(), [](const MusicalEvent& e) { return e.type == MusicalEventType::Downbeat; }), events.end());
        std::vector<MusicalEvent> sortedBeats;
        for (const MusicalEvent& event : events) {
            if (event.type == MusicalEventType::Beat) {
                sortedBeats.push_back(event);
            }
        }
        std::sort(sortedBeats.begin(), sortedBeats.end(), [](const MusicalEvent& a, const MusicalEvent& b) { return a.timeMS < b.timeMS; });
        events.erase(std::remove_if(events.begin(), events.end(), [](const MusicalEvent& e) { return e.type == MusicalEventType::Beat; }), events.end());
        events.insert(events.end(), sortedBeats.begin(), sortedBeats.end());
        std::vector<MusicalEvent*> beats;
        for (MusicalEvent& event : events) {
            if (event.type == MusicalEventType::Beat) {
                beats.push_back(&event);
            }
        }
        size_t anchor = 0;
        for (size_t b = 0; b < beats.size(); ++b) {
            if (std::abs(beats[b]->timeMS - bar.firstDownbeatMS) < std::abs(beats[anchor]->timeMS - bar.firstDownbeatMS)) {
                anchor = b;
            }
        }
        std::vector<MusicalEvent> downbeats;
        for (size_t b = 0; b < beats.size(); ++b) {
            const long offset = static_cast<long>(b) - static_cast<long>(anchor);
            const int position = static_cast<int>(((offset % bar.beatsPerBar) + bar.beatsPerBar) % bar.beatsPerBar);
            beats[b]->barPosition = position;
            if (position == 0) {
                MusicalEvent downbeat = *beats[b];
                downbeat.type = MusicalEventType::Downbeat;
                downbeat.confidence = 1;
                downbeat.source = SOURCE_MANUAL;
                downbeat.provenance = ProvenanceFor(bar.locked);
                downbeats.push_back(std::move(downbeat));
            }
        }
        events.insert(events.end(), downbeats.begin(), downbeats.end());
        analysis.beatsPerBar = bar.beatsPerBar;
        analysis.barConfidence = 1;
    }

    applyEvents(false, findDetected);

    ApplySectionCorrections(analysis);
}

void ApplySectionCorrections(SongAnalysis& analysis) {
    for (const SectionCorrection& correction : analysis.corrections.sections) {
        const int start = std::max(0, correction.startMS);
        const int end = std::min(analysis.durationMS, correction.endMS);
        if (end <= start) {
            continue;
        }
        std::vector<SongSection> kept;
        for (const SongSection& section : analysis.sections) {
            if (section.endMS <= start || section.startMS >= end) {
                kept.push_back(section);
                continue;
            }
            if (section.startMS < start) {
                SongSection left = section;
                left.endMS = start;
                kept.push_back(std::move(left));
            }
            if (section.endMS > end) {
                SongSection right = section;
                right.startMS = end;
                kept.push_back(std::move(right));
            }
        }
        SongSection manual;
        manual.startMS = start;
        manual.endMS = end;
        manual.kind = correction.kind;
        manual.confidence = 1;
        manual.evidence = { "manual" };
        manual.provenance = ProvenanceFor(correction.locked);
        double sum = 0;
        int count = 0;
        for (const EnergyPoint& point : analysis.energy) {
            if (point.timeMS >= start && point.timeMS < end) {
                sum += point.intensity;
                ++count;
            }
        }
        manual.energy = count == 0 ? 0 : Round3(sum / count);
        kept.push_back(std::move(manual));
        std::sort(kept.begin(), kept.end(), [](const SongSection& a, const SongSection& b) { return a.startMS < b.startMS; });
        analysis.sections = std::move(kept);
    }
    Normalize(analysis);
}

std::string ToJson(const SongAnalysis& analysis) {
    json root;
    root["schemaVersion"] = analysis.schemaVersion;
    root["analyzerVersion"] = analysis.analyzerVersion;
    root["musicQuality"] = analysis.musicQuality;
    root["audioHash"] = analysis.audioHash;
    root["optionsKey"] = analysis.optionsKey;
    root["fileName"] = analysis.fileName;
    root["fileSize"] = analysis.fileSize;
    root["durationMS"] = analysis.durationMS;
    root["sampleRate"] = analysis.sampleRate;
    root["sourceSampleRate"] = analysis.sourceSampleRate;
    root["channels"] = analysis.channels;
    root["metadata"] = analysis.metadata;
    root["bpm"] = analysis.globalBpm;
    root["bpmConfidence"] = analysis.bpmConfidence;
    root["initialBpm"] = analysis.initialBpm;
    root["initialBpmConfidence"] = analysis.initialBpmConfidence;
    root["tempoIsStable"] = analysis.tempoIsStable;
    json candidates = json::array();
    for (const TempoCandidate& c : analysis.tempoCandidates) {
        candidates.push_back({ { "bpm", c.bpm }, { "confidence", c.confidence }, { "reason", c.reason } });
    }
    root["tempoCandidates"] = candidates;
    root["appliedTempoFactor"] = analysis.appliedTempoFactor;
    root["beatsPerBar"] = analysis.beatsPerBar;
    root["barConfidence"] = analysis.barConfidence;
    root["musicStartMS"] = analysis.musicStartMS;
    root["musicEndMS"] = analysis.musicEndMS;
    root["key"] = analysis.key;
    root["warnings"] = analysis.warnings;

    json events = json::array();
    json beats = json::array();
    json downbeats = json::array();
    for (const MusicalEvent& e : analysis.events) {
        events.push_back({ { "timeMS", e.timeMS }, { "durationMS", e.durationMS }, { "type", EventTypeId(e.type) }, { "intensity", e.intensity }, { "confidence", e.confidence }, { "sectionIndex", e.sectionIndex }, { "source", e.source }, { "beatIndex", e.beatIndex }, { "barPosition", e.barPosition }, { "provenance", ProvenanceId(e.provenance) } });
        if (e.type == MusicalEventType::Beat) {
            beats.push_back(e.timeMS);
        } else if (e.type == MusicalEventType::Downbeat) {
            downbeats.push_back(e.timeMS);
        }
    }
    root["events"] = std::move(events);
    root["beats"] = std::move(beats);
    root["downbeats"] = std::move(downbeats);

    json sections = json::array();
    for (const SongSection& s : analysis.sections) {
        sections.push_back({ { "startMS", s.startMS }, { "endMS", s.endMS }, { "kind", SectionKindId(s.kind) }, { "repeatGroup", s.repeatGroup }, { "energy", s.energy }, { "confidence", s.confidence }, { "evidence", s.evidence }, { "provenance", ProvenanceId(s.provenance) } });
    }
    root["sections"] = std::move(sections);

    json vocals = json::array();
    for (const VocalSegment& v : analysis.vocalSegments) {
        vocals.push_back({ { "startMS", v.startMS }, { "endMS", v.endMS }, { "confidence", v.confidence }, { "method", v.method } });
    }
    root["vocalSegments"] = std::move(vocals);

    json energy = json::array();
    for (const EnergyPoint& p : analysis.energy) {
        energy.push_back({ { "timeMS", p.timeMS }, { "rms", p.rms }, { "total", p.total }, { "low", p.low }, { "mid", p.mid }, { "high", p.high }, { "variation", p.variation }, { "intensity", p.intensity } });
    }
    root["energy"] = std::move(energy);

    json tempo = json::array();
    for (const TempoPoint& p : analysis.tempo) {
        tempo.push_back({ { "timeMS", p.timeMS }, { "bpm", p.bpm }, { "confidence", p.confidence } });
    }
    root["tempo"] = std::move(tempo);

    json low = json::array();
    for (const TimeRange& r : analysis.lowEnergy) {
        low.push_back({ { "startMS", r.startMS }, { "endMS", r.endMS } });
    }
    root["lowEnergy"] = std::move(low);

    json eventCorrections = json::array();
    for (const EventCorrection& c : analysis.corrections.events) {
        eventCorrections.push_back({ { "action", ACTION_IDS[static_cast<int>(c.action)] }, { "type", EventTypeId(c.type) }, { "originalMS", c.originalMS }, { "timeMS", c.timeMS }, { "durationMS", c.durationMS }, { "locked", c.locked } });
    }
    json sectionCorrections = json::array();
    for (const SectionCorrection& c : analysis.corrections.sections) {
        sectionCorrections.push_back({ { "startMS", c.startMS }, { "endMS", c.endMS }, { "kind", SectionKindId(c.kind) }, { "locked", c.locked } });
    }
    root["corrections"] = { { "events", std::move(eventCorrections) }, { "sections", std::move(sectionCorrections) }, { "bar", { { "firstDownbeatMS", analysis.corrections.bar.firstDownbeatMS }, { "beatsPerBar", analysis.corrections.bar.beatsPerBar }, { "locked", analysis.corrections.bar.locked } } }, { "tempoFactor", analysis.corrections.tempoFactor } };
    return root.dump(1);
}

bool FromJson(const std::string& text, SongAnalysis& analysis, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    // The members are read from a file that anyone can edit, and a wrong type throws.
    try {
        SongAnalysis out;
        out.schemaVersion = root.at("schemaVersion").get<int>();
        out.analyzerVersion = root.at("analyzerVersion").get<std::string>();
        out.musicQuality = root.value("musicQuality", std::string(MUSIC_QUALITY_NOT_VALIDATED));
        out.audioHash = root.at("audioHash").get<std::string>();
        out.optionsKey = root.value("optionsKey", std::string());
        out.fileName = root.value("fileName", std::string());
        out.fileSize = root.value("fileSize", 0L);
        out.durationMS = root.at("durationMS").get<int>();
        out.sampleRate = root.at("sampleRate").get<int>();
        out.sourceSampleRate = root.value("sourceSampleRate", 0);
        out.channels = root.value("channels", 0);
        out.metadata = root.value("metadata", std::map<std::string, std::string>());
        out.globalBpm = root.value("bpm", 0.0);
        out.bpmConfidence = root.value("bpmConfidence", 0.0);
        out.initialBpm = root.value("initialBpm", 0.0);
        out.initialBpmConfidence = root.value("initialBpmConfidence", 0.0);
        out.tempoIsStable = root.value("tempoIsStable", false);
        for (const json& c : root.value("tempoCandidates", json::array())) {
            out.tempoCandidates.push_back({ c.value("bpm", 0.0), c.value("confidence", 0.0), c.value("reason", std::string()) });
        }
        out.appliedTempoFactor = root.value("appliedTempoFactor", 1.0);
        out.beatsPerBar = root.value("beatsPerBar", 0);
        out.barConfidence = root.value("barConfidence", 0.0);
        out.musicStartMS = root.value("musicStartMS", 0);
        out.musicEndMS = root.value("musicEndMS", 0);
        out.key = root.value("key", std::string());
        out.warnings = root.value("warnings", std::vector<std::string>());
        for (const json& e : root.at("events")) {
            MusicalEvent event;
            event.timeMS = e.at("timeMS").get<int>();
            event.durationMS = e.at("durationMS").get<int>();
            event.type = EnumFromId(EVENT_IDS, e.at("type").get<std::string>(), MusicalEventType::Onset);
            event.intensity = e.at("intensity").get<double>();
            event.confidence = e.at("confidence").get<double>();
            event.sectionIndex = e.at("sectionIndex").get<int>();
            event.source = e.at("source").get<std::string>();
            event.beatIndex = e.value("beatIndex", -1);
            event.barPosition = e.value("barPosition", -1);
            event.provenance = EnumFromId(PROVENANCE_IDS, e.value("provenance", std::string("detected")), Provenance::Detected);
            out.events.push_back(std::move(event));
        }
        for (const json& s : root.at("sections")) {
            SongSection section;
            section.startMS = s.at("startMS").get<int>();
            section.endMS = s.at("endMS").get<int>();
            section.kind = EnumFromId(SECTION_IDS, s.at("kind").get<std::string>(), SectionKind::Unknown);
            section.repeatGroup = s.value("repeatGroup", -1);
            section.energy = s.value("energy", 0.0);
            section.confidence = s.value("confidence", 0.0);
            section.evidence = s.value("evidence", std::vector<std::string>());
            section.provenance = EnumFromId(PROVENANCE_IDS, s.value("provenance", std::string("detected")), Provenance::Detected);
            out.sections.push_back(std::move(section));
        }
        for (const json& v : root.value("vocalSegments", json::array())) {
            out.vocalSegments.push_back({ v.at("startMS").get<int>(), v.at("endMS").get<int>(), v.at("confidence").get<double>(), v.at("method").get<std::string>() });
        }
        for (const json& p : root.at("energy")) {
            EnergyPoint point;
            point.timeMS = p.at("timeMS").get<int>();
            point.rms = p.at("rms").get<double>();
            point.total = p.at("total").get<double>();
            point.low = p.at("low").get<double>();
            point.mid = p.at("mid").get<double>();
            point.high = p.at("high").get<double>();
            point.variation = p.at("variation").get<double>();
            point.intensity = p.at("intensity").get<double>();
            out.energy.push_back(point);
        }
        for (const json& p : root.value("tempo", json::array())) {
            out.tempo.push_back({ p.at("timeMS").get<int>(), p.at("bpm").get<double>(), p.at("confidence").get<double>() });
        }
        for (const json& r : root.value("lowEnergy", json::array())) {
            out.lowEnergy.push_back({ r.at("startMS").get<int>(), r.at("endMS").get<int>() });
        }
        const json corrections = root.value("corrections", json::object());
        for (const json& c : corrections.value("events", json::array())) {
            EventCorrection correction;
            correction.action = EnumFromId(ACTION_IDS, c.at("action").get<std::string>(), EventCorrection::Action::Add);
            correction.type = EnumFromId(EVENT_IDS, c.at("type").get<std::string>(), MusicalEventType::Beat);
            correction.originalMS = c.value("originalMS", -1);
            correction.timeMS = c.at("timeMS").get<int>();
            correction.durationMS = c.value("durationMS", 0);
            correction.locked = c.value("locked", true);
            out.corrections.events.push_back(correction);
        }
        for (const json& c : corrections.value("sections", json::array())) {
            out.corrections.sections.push_back({ c.at("startMS").get<int>(), c.at("endMS").get<int>(), EnumFromId(SECTION_IDS, c.at("kind").get<std::string>(), SectionKind::Unknown), c.value("locked", true) });
        }
        if (corrections.contains("bar")) {
            const json& bar = corrections.at("bar");
            out.corrections.bar = { bar.value("firstDownbeatMS", -1), bar.value("beatsPerBar", 4), bar.value("locked", true) };
        }
        const double factor = corrections.value("tempoFactor", 1.0);
        out.corrections.tempoFactor = (factor == 0.5 || factor == 2.0) ? factor : 1.0;
        analysis = std::move(out);
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::vector<TimingTrackExport> BuildTimingTracks(const SongAnalysis& analysis, int frameMS) {
    const int frame = std::max(1, frameMS);
    const int lastFrameEnd = analysis.durationMS - analysis.durationMS % frame;
    const auto toFrame = [&](int ms) {
        return std::min(lastFrameEnd, static_cast<int>(std::lround(static_cast<double>(ms) / frame)) * frame);
    };

    // A mark lasts until the next one, as xLights' own beat tracks do, and no two can
    // share a frame.
    const auto marksOf = [&](MusicalEventType type, const std::function<std::string(const MusicalEvent&)>& label, bool keepDuration) {
        std::vector<TimingMark> marks;
        for (const MusicalEvent& event : analysis.events) {
            if (event.type != type) {
                continue;
            }
            const int start = toFrame(event.timeMS);
            if (start >= lastFrameEnd || (!marks.empty() && marks.back().startMS == start)) {
                continue;
            }
            int end = start + frame;
            if (keepDuration && event.durationMS > 0) {
                end = std::max(end, toFrame(event.timeMS + event.durationMS));
            }
            marks.push_back({ label(event), start, end });
        }
        if (!keepDuration) {
            for (size_t m = 0; m + 1 < marks.size(); ++m) {
                // Across a pause the mark stays short, so that it does not cover the silence.
                const int gap = marks[m + 1].startMS - marks[m].startMS;
                marks[m].endMS = gap <= 3000 ? marks[m + 1].startMS : marks[m].startMS + frame;
            }
        }
        for (size_t m = 0; m + 1 < marks.size(); ++m) {
            marks[m].endMS = std::min(marks[m].endMS, marks[m + 1].startMS);
        }
        return marks;
    };

    std::vector<TimingTrackExport> tracks;
    const auto add = [&](const char* id, const char* name, std::vector<TimingMark> marks) {
        if (!marks.empty()) {
            tracks.push_back({ id, name, std::move(marks) });
        }
    };
    add("composer.beats", "Composer Beats", marksOf(MusicalEventType::Beat, [](const MusicalEvent& e) { return e.barPosition >= 0 ? std::to_string(e.barPosition + 1) : std::string(); }, false));
    int bar = 0;
    add("composer.downbeats", "Composer Downbeats", marksOf(MusicalEventType::Downbeat, [&bar](const MusicalEvent&) { return std::to_string(++bar); }, false));
    add("composer.onsets", "Composer Onsets", marksOf(MusicalEventType::Onset, [](const MusicalEvent&) { return std::string(); }, false));
    add("composer.climax", "Composer Climax", marksOf(MusicalEventType::Climax, [](const MusicalEvent& e) { return std::string(EventTypeId(e.type)); }, true));
    add("composer.silence", "Composer Silence", marksOf(MusicalEventType::Silence, [](const MusicalEvent& e) { return std::string(EventTypeId(e.type)); }, true));

    std::vector<TimingMark> sections;
    for (const SongSection& section : analysis.sections) {
        const int start = toFrame(section.startMS);
        const int end = toFrame(section.endMS);
        if (end > start) {
            std::string label = SectionKindId(section.kind);
            if (section.repeatGroup >= 0) {
                label += fmt::format(" {}", section.repeatGroup + 1);
            }
            sections.push_back({ std::move(label), start, end });
        }
    }
    add("composer.sections", "Composer Sections", std::move(sections));
    return tracks;
}

bool WriteXTiming(const std::string& path, const std::vector<TimingTrackExport>& tracks, std::string& error) {
    pugi::xml_document doc;
    pugi::xml_node declaration = doc.append_child(pugi::node_declaration);
    declaration.append_attribute("version") = "1.0";
    declaration.append_attribute("encoding") = "UTF-8";
    pugi::xml_node root = doc.append_child("timings");
    root.append_attribute("SourceVersion") = xlights_version_string.c_str();
    for (const TimingTrackExport& track : tracks) {
        pugi::xml_node timing = root.append_child("timing");
        timing.append_attribute("name") = track.name.c_str();
        timing.append_attribute("subType") = "";
        timing.append_attribute("SourceVersion") = xlights_version_string.c_str();
        pugi::xml_node layer = timing.append_child("EffectLayer");
        for (const TimingMark& mark : track.marks) {
            pugi::xml_node effect = layer.append_child("Effect");
            effect.append_attribute("label") = mark.label.c_str();
            effect.append_attribute("starttime") = mark.startMS;
            effect.append_attribute("endtime") = mark.endMS;
        }
    }
    const std::string temporary = path + ".composer-tmp";
    if (!doc.save_file(temporary.c_str(), "  ")) {
        error = fmt::format("cannot write '{}'", temporary);
        return false;
    }
    std::error_code ec;
    fs::rename(temporary, path, ec);
    if (ec) {
        error = fmt::format("cannot replace '{}': {}", path, ec.message());
        std::error_code ignored;
        fs::remove(temporary, ignored);
        return false;
    }
    return true;
}

std::vector<SongStructureRegion> BuildSongStructure(const SongAnalysis& analysis) {
    static constexpr std::array<std::uint32_t, 6> COLORS = { 0x40E57373, 0x4081C784, 0x4064B5F6, 0x40FFD54F, 0x40BA68C8, 0x404DB6AC };
    std::vector<SongStructureRegion> regions;
    int id = 1;
    for (const SongSection& section : analysis.sections) {
        const std::uint32_t color = section.repeatGroup >= 0 ? COLORS[static_cast<size_t>(section.repeatGroup) % COLORS.size()] : 0x40808080;
        regions.emplace_back(id++, section.startMS, section.endMS, SectionKindId(section.kind), color);
    }
    return regions;
}

bool ScoreAgainstReference(const SongAnalysis& analysis, const std::string& referencePath, ReferenceScore& score, std::string& error) {
    std::string text;
    if (!ReadWholeFile(referencePath, text)) {
        error = "cannot read the reference";
        return false;
    }
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "the reference is not a JSON object";
        return false;
    }
    const auto fMeasure = [](const std::vector<int>& found, const std::vector<int>& truth, int tolerance) {
        if (truth.empty()) {
            return -1.0;
        }
        std::vector<bool> used(found.size(), false);
        int hits = 0;
        for (int t : truth) {
            for (size_t f = 0; f < found.size(); ++f) {
                if (!used[f] && std::abs(found[f] - t) <= tolerance) {
                    used[f] = true;
                    ++hits;
                    break;
                }
            }
        }
        const double precision = found.empty() ? 0 : static_cast<double>(hits) / static_cast<double>(found.size());
        const double recall = static_cast<double>(hits) / static_cast<double>(truth.size());
        return precision + recall <= 0 ? 0.0 : Round3(2 * precision * recall / (precision + recall));
    };
    std::vector<int> beats, downbeats, boundaries;
    for (const MusicalEvent& event : analysis.events) {
        if (event.type == MusicalEventType::Beat) {
            beats.push_back(event.timeMS);
        } else if (event.type == MusicalEventType::Downbeat) {
            downbeats.push_back(event.timeMS);
        }
    }
    for (const SongSection& section : analysis.sections) {
        boundaries.push_back(section.startMS);
    }
    try {
        score.beatF = fMeasure(beats, root.value("beatsMS", std::vector<int>()), REFERENCE_BEAT_TOLERANCE_MS);
        score.downbeatF = fMeasure(downbeats, root.value("downbeatsMS", std::vector<int>()), REFERENCE_BEAT_TOLERANCE_MS);
        score.sectionF = fMeasure(boundaries, root.value("sectionBoundariesMS", std::vector<int>()), REFERENCE_SECTION_TOLERANCE_MS);
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

} // namespace composer
