#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Music written in code for the Composer's tests: small, deterministic, and free of
// anyone's rights. Not part of any generation.
namespace composer::testaudio {

inline constexpr int RATE = 44100;
inline constexpr double PI = 3.14159265358979323846;

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

    // A voice-like sound: harmonics shaped by three formants, as a throat and mouth do.
    // `pitch` gives the fundamental at each moment; `amplitude` its loudness 0-1.
    template <typename Pitch, typename Amplitude>
    void Voice(double at, double duration, Pitch pitch, Amplitude amplitude, double level, double pan = 0.0, double breath = 0.02) {
        const size_t start = static_cast<size_t>(at * RATE);
        const size_t length = static_cast<size_t>(duration * RATE);
        double phase = 0;
        for (size_t i = 0; i < length && start + i < _left.size(); ++i) {
            const double t = static_cast<double>(i) / RATE;
            const double f0 = pitch(t);
            const double a = amplitude(t);
            phase += 2 * PI * f0 / RATE;
            double v = 0;
            const int harmonics = std::min(30, static_cast<int>(4000.0 / std::max(60.0, f0)));
            for (int k = 1; k <= harmonics; ++k) {
                v += Formants(k * f0) / k * std::sin(k * phase);
            }
            v = level * a * (v * 0.9 + breath * Noise());
            Add(start + i, v * (1 - pan), v * (1 + pan));
        }
    }

    // Speech: syllables of gliding pitch, a consonant's hiss at some of them, gaps between
    // words and a breath every few seconds. Never steady, never held.
    void Speech(double at, double duration, double baseHz, double level, double pan = 0.0) {
        const double syllable = 0.19;
        const auto amplitude = [=](double t) {
            const double phrase = std::fmod(t, 2.4);
            if (phrase > 2.0) {
                return 0.0; // a breath
            }
            const double word = std::fmod(phrase, syllable * 3 + 0.11);
            if (word > syllable * 3) {
                return 0.0; // between words
            }
            // Each syllable opens with a consonant: no voice for its first quarter.
            const double s = std::fmod(word, syllable) / syllable;
            if (s < 0.25) {
                return 0.0;
            }
            return std::sin(PI * (s - 0.25) / 0.75) * (0.75 + 0.25 * std::sin(2 * PI * 0.7 * t));
        };
        const auto pitch = [=](double t) {
            const double s = std::fmod(t, syllable) / syllable;
            const int n = static_cast<int>(t / syllable);
            const double accent = (n % 3 == 0) ? 1.18 : (n % 3 == 1 ? 1.02 : 0.9);
            return baseHz * accent * (1.08 - 0.16 * s) * (1.0 - 0.05 * std::fmod(t, 2.4) / 2.4);
        };
        Voice(at, duration, pitch, amplitude, level, pan, 0.05);
        for (double t = 0; t < duration; t += syllable) {
            const double phrase = std::fmod(t, 2.4);
            if (phrase < 2.0 && std::fmod(phrase, syllable * 3 + 0.11) < syllable * 3) {
                Hiss(at + t, 0.04, level * 0.2, pan);
            }
        }
    }

    // Singing: held notes with a vibrato, joined smoothly, one note after another.
    void Sing(double at, double duration, const std::vector<double>& melody, double level, double pan = 0.0, double vibratoPhase = 0.0, double detune = 1.0) {
        const double note = duration / static_cast<double>(std::max<size_t>(1, melody.size()));
        const auto pitch = [=](double t) {
            const size_t n = std::min(melody.size() - 1, static_cast<size_t>(t / note));
            const double into = t - static_cast<double>(n) * note;
            double f = melody[n];
            if (n > 0 && into < 0.05) {
                f = melody[n - 1] + (melody[n] - melody[n - 1]) * into / 0.05;
            }
            return detune * f * (1 + 0.012 * std::sin(2 * PI * 5.5 * t + vibratoPhase) * std::min(1.0, into / 0.25));
        };
        const auto amplitude = [=](double t) { return std::min(1.0, t / 0.06) * std::min(1.0, (duration - t) / 0.12) * (0.9 + 0.1 * std::sin(2 * PI * 0.5 * t)); };
        Voice(at, duration, pitch, amplitude, level, pan, 0.01);
    }

    // A choir: the melody, a third and a fifth above, each voice slightly out of tune
    // with the others and placed apart, as several singers are.
    void Choir(double at, double duration, const std::vector<double>& melody, double level) {
        const double ratios[] = { 1.0, 1.26, 1.5 };
        const double pans[] = { -0.5, 0.0, 0.5 };
        const double detunes[] = { 0.996, 1.0, 1.005 };
        for (int v = 0; v < 3; ++v) {
            std::vector<double> part;
            for (double f : melody) {
                part.push_back(f * ratios[v]);
            }
            Sing(at, duration, part, level / 2.2, pans[v], v * 2.1, detunes[v]);
        }
    }

    // A sustained, wide chord: an accompaniment with no beat.
    void Pad(double at, double duration, double rootHz, double level) {
        for (double r : { 1.0, 1.26, 1.5 }) {
            Tone(at, duration, rootHz * r, level / 3, 0.3, 0.85);
        }
    }

    // A low, steady bass note.
    void Bass(double at, double duration, double frequency, double level) {
        const size_t start = static_cast<size_t>(at * RATE);
        const size_t length = static_cast<size_t>(duration * RATE);
        for (size_t i = 0; i < length && start + i < _left.size(); ++i) {
            const double t = static_cast<double>(i) / RATE;
            const double env = std::min(1.0, t / 0.02) * std::min(1.0, (duration - t) / 0.05);
            const double v = level * env * std::sin(2 * PI * frequency * t);
            Add(start + i, v, v);
        }
    }

    // A beat: a kick on each beat and a click between, for `duration` seconds.
    void Beat(double at, double duration, double bpm, double level) {
        const double beat = 60.0 / bpm;
        for (double t = 0; t + 0.1 < duration; t += beat) {
            Kick(at + t, level);
            Click(at + t + beat / 2, level * 0.5);
        }
    }

    // Broadband hiss, centred or placed.
    void Hiss(double at, double duration, double level, double pan = 0.0) {
        const size_t start = static_cast<size_t>(at * RATE);
        const size_t length = static_cast<size_t>(duration * RATE);
        double previous = 0;
        for (size_t i = 0; i < length && start + i < _left.size(); ++i) {
            const double n = Noise();
            const double v = level * (n - previous) * 0.7;
            previous = n;
            Add(start + i, v * (1 - pan), v * (1 + pan));
        }
    }

    // Loudness changed gradually over a stretch, from `from` to `to` times what is there.
    void Ramp(double at, double duration, double from, double to) {
        const size_t start = static_cast<size_t>(at * RATE);
        const size_t length = static_cast<size_t>(duration * RATE);
        for (size_t i = 0; i < length && start + i < _left.size(); ++i) {
            const double g = from + (to - from) * static_cast<double>(i) / static_cast<double>(std::max<size_t>(1, length));
            _left[start + i] *= static_cast<float>(g);
            _right[start + i] *= static_cast<float>(g);
        }
    }

    // Everything over a stretch multiplied by `gain` (0: silence).
    void Scale(double at, double duration, double gain) { Ramp(at, duration, gain, gain); }

    [[nodiscard]] const std::vector<float>& Left() const { return _left; }
    [[nodiscard]] const std::vector<float>& Right() const { return _right; }

    [[nodiscard]] bool Save(const std::string& path, bool mono = false) const {
        std::ofstream out(std::filesystem::path(path), std::ios::binary);
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
    static double Formants(double f) {
        const double centres[] = { 700, 1200, 2600 };
        const double widths[] = { 130, 160, 260 };
        const double gains[] = { 1.0, 0.7, 0.35 };
        double g = 0.08;
        for (int i = 0; i < 3; ++i) {
            const double x = (f - centres[i]) / widths[i];
            g += gains[i] / (1 + x * x);
        }
        return g;
    }

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

} // namespace composer::testaudio
