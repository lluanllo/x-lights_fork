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
