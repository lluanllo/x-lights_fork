#include "composer/AudioUnderstanding.h"

#include "composer/SongAnalysis.h"
#include "media/AudioManager.h"
#include "utils/ExternalHooks.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <filesystem>
#include <functional>
#include <map>
#include <numeric>
#include <set>

#define COMPOSER_TEXT(text) text

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr int WINDOW = 2048;
constexpr int BANDS = 48;

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

double Percentile(std::vector<double> v, double p) {
    if (v.empty()) {
        return 0;
    }
    std::sort(v.begin(), v.end());
    const double at = p * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(at));
    const size_t hi = std::min(v.size() - 1, lo + 1);
    return v[lo] + (v[hi] - v[lo]) * (at - static_cast<double>(lo));
}

double ToDb(double rms) {
    return 20.0 * std::log10(std::max(rms, 1e-7));
}

void Fft(std::vector<std::complex<double>>& a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2 * PI / static_cast<double>(len);
        const std::complex<double> w(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> wn(1);
            for (size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * wn;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                wn *= w;
            }
        }
    }
}

// Analysis frames, every 20 ms, from samples.
struct Raw {
    std::vector<double> rms, low, sub, mid, high, flux, centroid, voiceMid, voiceSide, f0, voicing, harmonic, percussive, poly;
    std::vector<double> total;      // spectral power of the frame
    std::vector<double> voiceShare; // share of it between 300 Hz and 3.4 kHz, where a voice speaks
    std::vector<double> subShare;   // share below 90 Hz
    std::vector<double> flatness;   // 0 tones, 1 noise (100 Hz to 8 kHz)
    std::vector<double> above150;   // share of the power above 150 Hz
    std::vector<std::array<double, BANDS>> bands;
};

class Reader {
public:
    Reader(const std::vector<float>& left, const std::vector<float>& right, int rate, const UnderstandingOptions& options, const UnderstandingCancel& cancel) :
        _left(left), _right(right.empty() ? left : right), _rate(std::max(8000, rate)), _o(options), _cancel(cancel) {}

    AudioUnderstanding Run() {
        AudioUnderstanding u;
        u.durationMS = static_cast<int>(static_cast<long long>(_left.size()) * 1000 / _rate);
        u.channels = &_left == &_right ? 1 : 2;
        double side = 0, midE = 0;
        for (size_t i = 0; i < _left.size(); i += 16) {
            const double m = 0.5 * (_left[i] + _right[i]);
            const double s = 0.5 * (_left[i] - _right[i]);
            midE += m * m;
            side += s * s;
        }
        u.stereo = side > midE * 0.0004;
        if (u.durationMS < 500) {
            u.warnings.push_back("too-short");
            return u;
        }
        Frames();
        if (Cancelled()) {
            return u;
        }
        Pitch();
        Hpss();
        if (Cancelled()) {
            return u;
        }
        Score(u);
        Activities(u);
        Events(u);
        Curve(u);
        Number(u);
        return u;
    }

private:
    bool Cancelled() const { return _cancel && _cancel(); }

    double Sample(size_t i) const { return i < _left.size() ? 0.5 * (_left[i] + _right[i]) : 0.0; }
    double SideSample(size_t i) const { return i < _left.size() ? 0.5 * (_left[i] - _right[i]) : 0.0; }

    void Frames() {
        const size_t hop = static_cast<size_t>(_rate) * UNDERSTANDING_HOP_MS / 1000;
        const size_t frames = _left.size() / hop + 1;
        _hop = hop;
        _n = frames;
        Raw& r = _r;
        for (auto* v : { &r.rms, &r.low, &r.sub, &r.mid, &r.high, &r.flux, &r.centroid, &r.voiceMid, &r.voiceSide, &r.total, &r.voiceShare, &r.subShare, &r.flatness, &r.above150 }) {
            v->assign(frames, 0.0);
        }
        r.bands.assign(frames, {});
        std::vector<double> window(WINDOW);
        for (int i = 0; i < WINDOW; ++i) {
            window[i] = 0.5 - 0.5 * std::cos(2 * PI * i / (WINDOW - 1));
        }
        std::vector<std::complex<double>> a(WINDOW), b(WINDOW);
        const double binHz = static_cast<double>(_rate) / WINDOW;
        // Log-spaced bands from 40 Hz to 11 kHz.
        std::array<int, BANDS + 1> edges{};
        for (int k = 0; k <= BANDS; ++k) {
            const double hz = 40.0 * std::pow(11000.0 / 40.0, static_cast<double>(k) / BANDS);
            edges[k] = std::clamp(static_cast<int>(hz / binHz), 1, WINDOW / 2);
        }
        std::array<double, BANDS> previous{};
        for (size_t f = 0; f < frames; ++f) {
            if (f % 512 == 0 && Cancelled()) {
                return;
            }
            const long center = static_cast<long>(f * hop);
            double energy = 0;
            for (long i = center - static_cast<long>(hop) / 2; i < center + static_cast<long>(hop) / 2; ++i) {
                if (i >= 0) {
                    const double s = Sample(static_cast<size_t>(i));
                    energy += s * s;
                }
            }
            r.rms[f] = std::sqrt(energy / static_cast<double>(hop));
            for (int i = 0; i < WINDOW; ++i) {
                const long at = center - WINDOW / 2 + i;
                a[i] = at >= 0 ? Sample(static_cast<size_t>(at)) * window[i] : 0.0;
                b[i] = at >= 0 ? SideSample(static_cast<size_t>(at)) * window[i] : 0.0;
            }
            Fft(a);
            Fft(b);
            double total = 0, low = 0, sub = 0, mid = 0, high = 0, weighted = 0, vm = 0, vs = 0, logSum = 0, linSum = 0, under150 = 0;
            int flatBins = 0;
            for (int k = 1; k < WINDOW / 2; ++k) {
                const double p = std::norm(a[k]);
                const double hz = k * binHz;
                if (hz >= 100 && hz <= 8000) {
                    logSum += std::log(p + 1e-18);
                    linSum += p;
                    ++flatBins;
                }
                total += p;
                weighted += p * hz;
                (hz < 250 ? low : hz < 2000 ? mid : high) += p;
                if (hz < 90) {
                    sub += p;
                }
                if (hz < 150) {
                    under150 += p;
                }
                if (hz >= 300 && hz <= 3400) {
                    vm += p;
                    vs += std::norm(b[k]);
                }
            }
            const double t = std::max(total, 1e-12);
            r.low[f] = low / t;
            r.sub[f] = sub;
            r.mid[f] = mid / t;
            r.high[f] = high / t;
            r.centroid[f] = weighted / t;
            r.voiceMid[f] = vm;
            r.voiceSide[f] = vs;
            r.total[f] = total;
            r.voiceShare[f] = vm / std::max(total - under150, 1e-12);
            r.above150[f] = (total - under150) / t;
            r.subShare[f] = sub / t;
            r.flatness[f] = flatBins > 0 && linSum > 1e-15 ? std::clamp(std::exp(logSum / flatBins) / (linSum / flatBins), 0.0, 1.0) : 0.0;
            double flux = 0;
            for (int k = 0; k < BANDS; ++k) {
                double e = 0;
                for (int bin = edges[k]; bin < std::max(edges[k] + 1, edges[k + 1]); ++bin) {
                    e += std::abs(a[bin]);
                }
                r.bands[f][k] = e;
                const double lg = std::log1p(e);
                flux += std::max(0.0, lg - previous[k]);
                previous[k] = lg;
            }
            r.flux[f] = flux;
        }
    }

    // YIN on a decimated mono signal: the fundamental and how clear it is, and how many
    // independent pitches sound at once (a choir, a chord).
    void Pitch() {
        Raw& r = _r;
        r.f0.assign(_n, 0.0);
        r.voicing.assign(_n, 0.0);
        r.poly.assign(_n, 0.0);
        if (!_o.estimatePitch) {
            return;
        }
        const int factor = std::max(1, static_cast<int>(std::lround(_rate / 11025.0)));
        const double rate = static_cast<double>(_rate) / factor;
        std::vector<double> d(_left.size() / factor + 1, 0.0);
        for (size_t i = 0; i < d.size(); ++i) {
            double s = 0;
            for (int k = 0; k < factor; ++k) {
                s += Sample(i * factor + k);
            }
            d[i] = s / factor;
        }
        // Where a voice's harmonics are strongest: below it a bass line and kicks would
        // hide its period, above it cymbals and hiss. A missing fundamental still leaves
        // the period, so a low voice is found from its harmonics.
        const auto biquad = [&](bool highPass, double hz) {
            const double w0 = 2 * PI * hz / rate;
            const double alpha = std::sin(w0) / (2 * 0.7071);
            const double cw = std::cos(w0);
            const double b0 = highPass ? (1 + cw) / 2 : (1 - cw) / 2;
            const double b1 = highPass ? -(1 + cw) : 1 - cw;
            const double b2 = b0;
            const double a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
            double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
            for (double& x : d) {
                const double y = (b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2) / a0;
                x2 = x1;
                x1 = x;
                y2 = y1;
                y1 = y;
                x = y;
            }
        };
        biquad(true, 150);
        biquad(true, 150);
        biquad(false, 1800);
        const int win = static_cast<int>(rate * 0.046);
        const int minLag = std::max(2, static_cast<int>(rate / 1100.0));
        const int maxLag = static_cast<int>(rate / 70.0);
        std::vector<double> diff(maxLag + 2), cmnd(maxLag + 2);
        const int fftN = 1024;
        std::vector<std::complex<double>> spec(fftN);
        for (size_t f = 0; f < _n; ++f) {
            if (f % 512 == 0 && Cancelled()) {
                return;
            }
            const long start = static_cast<long>(f * _hop / factor) - win / 2;
            if (start < 0 || start + win + maxLag >= static_cast<long>(d.size()) || r.rms[f] < 1e-4) {
                continue;
            }
            for (int tau = 1; tau <= maxLag; ++tau) {
                double sum = 0;
                for (int i = 0; i < win; ++i) {
                    const double x = d[start + i] - d[start + i + tau];
                    sum += x * x;
                }
                diff[tau] = sum;
            }
            double running = 0;
            cmnd[0] = 1;
            for (int tau = 1; tau <= maxLag; ++tau) {
                running += diff[tau];
                cmnd[tau] = running > 0 ? diff[tau] * tau / running : 1;
            }
            int best = -1;
            for (int tau = minLag; tau < maxLag; ++tau) {
                if (cmnd[tau] < 0.2 && cmnd[tau] <= cmnd[tau + 1]) {
                    best = tau;
                    break;
                }
            }
            if (best < 0) {
                best = static_cast<int>(std::min_element(cmnd.begin() + minLag, cmnd.begin() + maxLag) - cmnd.begin());
            }
            const double clarity = std::clamp(1.0 - cmnd[best], 0.0, 1.0);
            double shift = 0;
            if (best > minLag && best < maxLag) {
                const double a0 = cmnd[best - 1], a1 = cmnd[best], a2 = cmnd[best + 1];
                const double den = a0 - 2 * a1 + a2;
                shift = std::fabs(den) > 1e-12 ? 0.5 * (a0 - a2) / den : 0;
            }
            r.voicing[f] = clarity;
            r.f0[f] = clarity > 0.5 ? rate / (best + shift) : 0;

            // Independent pitches: what is left of the spectrum's peaks once the harmonics
            // of the pitch heard, and of each lower pitch found, are taken away.
            for (int i = 0; i < fftN; ++i) {
                const long at = start + win / 2 - fftN / 2 + i;
                const double w = 0.5 - 0.5 * std::cos(2 * PI * i / (fftN - 1));
                spec[i] = at >= 0 && at < static_cast<long>(d.size()) ? d[at] * w : 0.0;
            }
            Fft(spec);
            const double binHz = rate / fftN;
            std::vector<std::pair<double, double>> peaks; // Hz, magnitude
            double top = 0;
            std::vector<double> mags;
            for (int k = static_cast<int>(90 / binHz); k < static_cast<int>(1100 / binHz); ++k) {
                top = std::max(top, std::abs(spec[k]));
                mags.push_back(std::abs(spec[k]));
            }
            // A note stands well above the spectrum around it; the peaks of a noise do not.
            const double noiseFloor = 5 * Percentile(mags, 0.5);
            for (int k = static_cast<int>(90 / binHz) + 1; k < static_cast<int>(1100 / binHz); ++k) {
                const double m = std::abs(spec[k]);
                if (m > 0.25 * top && m > noiseFloor && m >= std::abs(spec[k - 1]) && m > std::abs(spec[k + 1])) {
                    const double a0 = std::abs(spec[k - 1]), a2 = std::abs(spec[k + 1]);
                    const double den = a0 - 2 * m + a2;
                    const double delta = std::fabs(den) > 1e-12 ? std::clamp(0.5 * (a0 - a2) / den, -0.5, 0.5) : 0.0;
                    peaks.push_back({ (k + delta) * binHz, m });
                }
            }
            std::sort(peaks.begin(), peaks.end());
            std::vector<double> fundamentals;
            const auto explains = [&](double f0, double hz) {
                const double n = std::round(hz / f0);
                return n >= 1 && std::fabs(hz - n * f0) <= std::max(0.05 * n * f0, 2.0 * binHz);
            };
            if (r.f0[f] > 0) {
                fundamentals.push_back(r.f0[f]);
            }
            for (const auto& [hz, m] : peaks) {
                if (!std::any_of(fundamentals.begin(), fundamentals.end(), [&](double f0) { return explains(f0, hz); })) {
                    fundamentals.push_back(hz);
                }
            }
            r.poly[f] = static_cast<double>(fundamentals.size());
        }
    }

    // Steady energy runs along time, struck energy along frequency: medians split them.
    void Hpss() {
        Raw& r = _r;
        r.harmonic.assign(_n, 0.0);
        r.percussive.assign(_n, 0.0);
        constexpr int T = 8;
        constexpr int F = 4;
        std::vector<double> window;
        for (size_t f = 0; f < _n; ++f) {
            double h2 = 0, p2 = 0;
            for (int k = 0; k < BANDS; ++k) {
                window.clear();
                for (long t = static_cast<long>(f) - T; t <= static_cast<long>(f) + T; ++t) {
                    if (t >= 0 && t < static_cast<long>(_n)) {
                        window.push_back(r.bands[t][k]);
                    }
                }
                std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
                const double h = window[window.size() / 2];
                window.clear();
                for (int b = k - F; b <= k + F; ++b) {
                    if (b >= 0 && b < BANDS) {
                        window.push_back(r.bands[f][b]);
                    }
                }
                std::nth_element(window.begin(), window.begin() + window.size() / 2, window.end());
                const double p = window[window.size() / 2];
                h2 += h * h;
                p2 += p * p;
            }
            const double t = std::max(h2 + p2, 1e-12);
            r.harmonic[f] = h2 / t;
            r.percussive[f] = p2 / t;
        }
    }

    // How much each frame sounds like a voice, and like accompaniment.
    void Score(AudioUnderstanding& u) {
        Raw& r = _r;
        _voice.assign(_n, 0.0);
        _music.assign(_n, 0.0);
        std::vector<double> loud;
        for (double v : r.rms) {
            loud.push_back(ToDb(v));
        }
        const double median = Percentile(loud, 0.5);
        _floorDb = std::max(_o.silenceDb, median - 40.0);
        std::vector<double> fluxes(r.flux.begin(), r.flux.end());
        const double fluxMed = Percentile(fluxes, 0.5);
        const double fluxHigh = std::max(fluxMed * 2.5, Percentile(fluxes, 0.9));
        _onset.assign(_n, false);
        for (size_t f = 1; f + 1 < _n; ++f) {
            // A change of the spectrum that also makes the sound louder: a steady note
            // whose spectrum shimmers is not struck again.
            const double before = std::min({ ToDb(r.rms[f - 1]), f >= 2 ? ToDb(r.rms[f - 2]) : ToDb(r.rms[f - 1]), f >= 3 ? ToDb(r.rms[f - 3]) : ToDb(r.rms[f - 1]) });
            const double rise = std::max(ToDb(r.rms[f]), ToDb(r.rms[f + 1])) - before;
            _onset[f] = r.flux[f] > fluxHigh && r.flux[f] >= r.flux[f - 1] && r.flux[f] >= r.flux[f + 1] && ToDb(r.rms[f]) > _floorDb + 6 && (rise >= 2.0 || r.flux[f] > 3 * fluxHigh);
        }
        // A voice starting a syllable (no pitch before, pitch after) is not a drum.
        _struck.assign(_n, false);
        const auto voicedAt = [&](size_t f) { return f < _n && r.f0[f] >= 75 && r.voicing[f] > 0.6 && r.above150[f] > 0.4 && r.voiceShare[f] > 0.3; };
        for (size_t f = 0; f < _n; ++f) {
            if (!_onset[f]) {
                continue;
            }
            // A hit inside a held note still counts: the voice sounded all along before it.
            const bool held = f >= 3 && voicedAt(f - 1) && voicedAt(f - 2) && voicedAt(f - 3);
            const bool after = voicedAt(f) || voicedAt(f + 1) || voicedAt(f + 2) || voicedAt(f + 3);
            _struck[f] = held || !after;
        }
        const int second = 1000 / UNDERSTANDING_HOP_MS;
        for (size_t f = 0; f < _n; ++f) {
            if (ToDb(r.rms[f]) <= _floorDb) {
                continue;
            }
            const double pitched = r.f0[f] >= 75 && r.f0[f] <= 1100 ? r.voicing[f] : 0.0;
            // A voice speaks between 300 Hz and 3.4 kHz; a low instrument's note does not.
            const double band = std::clamp((r.voiceShare[f] - 0.15) / 0.3, 0.0, 1.0);
            double center = 1.0;
            if (u.stereo) {
                const double vm = r.voiceMid[f], vs = r.voiceSide[f];
                center = vm + vs > 0 ? std::clamp((vm - vs) / (vm + vs), 0.0, 1.0) : 0.0;
            }
            _voice[f] = std::clamp(pitched * band * (0.6 + 0.4 * center) * 1.15, 0.0, 1.0);
            int onsets = 0;
            for (long t = static_cast<long>(f) - second; t <= static_cast<long>(f) + second; ++t) {
                onsets += t >= 0 && t < static_cast<long>(_n) && _struck[t] ? 1 : 0;
            }
            const double beat = std::clamp(onsets / 4.0, 0.0, 1.0) * r.percussive[f] * 2.0;
            const double bass = std::clamp((r.subShare[f] - 0.05) / 0.2, 0.0, 1.0);
            double width = 0;
            if (u.stereo) {
                width = std::clamp(r.voiceSide[f] / std::max(1e-12, r.voiceMid[f] + r.voiceSide[f]) * 2.0, 0.0, 1.0);
            }
            const double unvoicedTone = r.harmonic[f] * (1.0 - r.voicing[f]) * (1.0 - std::min(1.0, r.flatness[f] * 2.5));
            _music[f] = std::clamp(0.55 * beat + 0.45 * bass + 0.35 * width + 0.35 * unvoicedTone + 0.25 * std::max(0.0, r.poly[f] - 2), 0.0, 1.0);
        }
        // Smoothed over 200 ms (voice) and 600 ms (music): a syllable gap is not an end.
        _voice = Smooth(_voice, 5);
        _music = Smooth(_music, 15);
    }

    static std::vector<double> Smooth(const std::vector<double>& v, int half) {
        std::vector<double> out(v.size(), 0.0);
        double sum = 0;
        int count = 0;
        for (size_t i = 0; i < v.size() + half; ++i) {
            if (i < v.size()) {
                sum += v[i];
                ++count;
            }
            if (i >= static_cast<size_t>(2 * half + 1)) {
                sum -= v[i - 2 * half - 1];
                --count;
            }
            if (i >= static_cast<size_t>(half)) {
                out[i - half] = count > 0 ? sum / count : 0;
            }
        }
        return out;
    }

    int Ms(size_t frame) const { return static_cast<int>(frame) * UNDERSTANDING_HOP_MS; }

    // Runs of frames where `on` holds, shorter gaps than `bridge` closed, shorter runs
    // than `minimum` dropped.
    std::vector<std::pair<size_t, size_t>> Runs(const std::function<bool(size_t)>& on, int bridgeMS, int minimumMS) const {
        std::vector<std::pair<size_t, size_t>> runs;
        size_t start = 0;
        bool in = false;
        for (size_t f = 0; f <= _n; ++f) {
            const bool now = f < _n && on(f);
            if (now && !in) {
                start = f;
                in = true;
            } else if (!now && in) {
                if (!runs.empty() && Ms(start) - Ms(runs.back().second) < bridgeMS) {
                    runs.back().second = f;
                } else {
                    runs.push_back({ start, f });
                }
                in = false;
            }
        }
        std::erase_if(runs, [&](const auto& r) { return Ms(r.second) - Ms(r.first) < minimumMS; });
        return runs;
    }

    void Add(AudioUnderstanding& u, const std::string& kind, size_t from, size_t to, double confidence, std::vector<std::string> evidence) {
        ActivitySegment s;
        s.kind = kind;
        s.startMS = Ms(from);
        s.endMS = std::min(u.durationMS, Ms(to));
        s.confidence = Round3(std::clamp(confidence, 0.0, 1.0));
        s.evidence = std::move(evidence);
        if (s.endMS > s.startMS) {
            u.activities.push_back(std::move(s));
        }
    }

    double Mean(const std::vector<double>& v, size_t from, size_t to) const {
        double s = 0;
        for (size_t i = from; i < to && i < v.size(); ++i) {
            s += v[i];
        }
        return to > from ? s / static_cast<double>(to - from) : 0;
    }

    void Activities(AudioUnderstanding& u) {
        Raw& r = _r;
        const auto silent = [&](size_t f) { return ToDb(r.rms[f]) <= _floorDb; };
        for (const auto& [a, b] : Runs(silent, 0, _o.minSilenceMS)) {
            Add(u, "silence", a, b, 0.9, { fmt::format("below {:.0f} dB", _floorDb) });
        }
        const auto music = [&](size_t f) { return _music[f] >= 0.3 && !silent(f); };
        const auto musicRuns = Runs(music, 400, 800);
        for (const auto& [a, b] : musicRuns) {
            Add(u, "music", a, b, 0.5 + 0.4 * Mean(_music, a, b), { "beats, bass, width or tones that are not a voice" });
        }
        const auto musicAt = [&](size_t f) { return std::any_of(musicRuns.begin(), musicRuns.end(), [&](const auto& m) { return f >= m.first && f < m.second; }); };
        const auto struck = [&](size_t f) { return r.percussive[f] > 0.55 && _struck[f]; };
        for (const auto& [a, b] : Runs(struck, 1200, 1500)) {
            Add(u, "percussion", a, b, 0.55, { "struck sounds in a row" });
        }
        for (const auto& [a, b] : Runs([&](size_t f) { return r.subShare[f] > 0.12 && !silent(f); }, 600, 1500)) {
            Add(u, "bass", a, b, 0.5, { "steady energy below 90 Hz" });
        }

        // Voices: phrases of voiced frames; a gap shorter than a breath stays inside.
        // Under an accompaniment the voice's pitch is partly masked: less evidence is asked.
        const auto voiced = [&](size_t f) { return _voice[f] >= (_music[f] >= 0.5 ? 0.34 : 0.42); };
        const auto phrases = Runs(voiced, 250, _o.minVoiceMS);
        for (size_t p = 0; p < phrases.size(); ++p) {
            const auto [a, b] = phrases[p];
            // Sung or spoken: singing holds notes (a pitch kept for 160 ms or more, a
            // vibrato included); speech glides from syllable to syllable, however flat its
            // tune. The share of voiced time inside notes, the longest note, voiced share.
            int pitched = 0, polyFrames = 0, inNotes = 0, longestNote = 0, note = 0;
            double noteStart = 0, width = 0;
            const int minNote = 160 / UNDERSTANDING_HOP_MS;
            const auto closeNote = [&]() {
                if (note >= minNote) {
                    inNotes += note;
                    longestNote = std::max(longestNote, note);
                }
                note = 0;
            };
            for (size_t f = a; f < b; ++f) {
                if (r.f0[f] > 0) {
                    ++pitched;
                    const double s = 12.0 * std::log2(r.f0[f] / 440.0);
                    if (note > 0 && std::fabs(s - noteStart) < 0.7) {
                        ++note;
                    } else {
                        closeNote();
                        noteStart = s;
                        note = 1;
                    }
                } else {
                    closeNote();
                }
                polyFrames += r.poly[f] >= 2 ? 1 : 0;
                width += r.voiceSide[f] / std::max(1e-12, r.voiceMid[f] + r.voiceSide[f]);
            }
            closeNote();
            const double frames = static_cast<double>(b - a);
            const double voicedShare = pitched / frames;
            const double stability = pitched > 0 ? static_cast<double>(inNotes) / pitched : 0;
            const double held = std::min(1.0, longestNote * UNDERSTANDING_HOP_MS / 500.0);
            const double sung = 0.5 * stability + 0.2 * voicedShare + 0.3 * held;
            const double polyShare = polyFrames / frames;
            width /= frames;
            const bool withMusic = Mean(_music, a, b) >= 0.3 || musicAt((a + b) / 2);
            std::string kind;
            double confidence = 0;
            std::vector<std::string> evidence = { fmt::format("in held notes {:.0f}%", stability * 100), fmt::format("voiced {:.0f}%", voicedShare * 100),
                                                  fmt::format("longest note {} ms", longestNote * UNDERSTANDING_HOP_MS) };
            // Over music the voice's pitch cannot be followed from the mix (measured on real
            // songs: held notes are not found in sung phrases, and a narration over chords
            // borrows their steadiness). A voice over music is taken as sung, and says it is
            // a guess: a narration needs its own recording, a [Narrador] tag or a correction.
            const bool guessed = withMusic;
            if (guessed || sung >= 0.5) {
                const bool choir = polyShare >= 0.6 || (polyShare >= 0.35 && (!u.stereo || width >= 0.12));
                kind = choir ? "chorus" : "singing";
                confidence = choir ? 0.35 + 0.4 * polyShare : 0.4 + 0.5 * std::min(1.0, (sung - 0.5) * 3);
                if (choir) {
                    evidence.push_back(fmt::format("several pitches at once {:.0f}%", polyShare * 100));
                }
            } else {
                kind = "speech";
                confidence = 0.4 + 0.5 * std::min(1.0, (0.5 - sung) * 3);
            }
            if (guessed) {
                evidence.push_back("over the music, sung is assumed: uncertain");
                confidence = std::min(confidence, 0.4);
            } else if (sung > 0.42 && sung < 0.58) {
                evidence.push_back("between speech and singing: uncertain");
                confidence = std::min(confidence, 0.4);
            }
            Add(u, kind, a, b, confidence, evidence);
            if (!withMusic && kind != "speech") {
                Add(u, "a-cappella", a, b, 0.5, { "a voice and no accompaniment" });
            }
        }
        // Music with no voice is the instruments' turn.
        for (const auto& [a, b] : musicRuns) {
            size_t cursor = a;
            for (const auto& [pa, pb] : phrases) {
                if (pb <= cursor || pa >= b) {
                    continue;
                }
                if (Ms(pa) - Ms(cursor) >= 1500) {
                    Add(u, "instrumental", cursor, pa, 0.55, { "music, no voice" });
                }
                cursor = std::max(cursor, pb);
            }
            if (Ms(b) - Ms(cursor) >= 1500) {
                Add(u, "instrumental", cursor, b, 0.55, { "music, no voice" });
            }
        }
        // A pause: the voice stops for a breath or more while the music goes on.
        for (size_t p = 1; p < phrases.size(); ++p) {
            const size_t a = phrases[p - 1].second;
            const size_t b = phrases[p].first;
            const int gap = Ms(b) - Ms(a);
            if (gap >= _o.minPauseMS && gap <= 4000 && musicAt((a + b) / 2)) {
                Add(u, "pause", a, b, 0.6, { "the voice stops, the music goes on" });
            }
        }
        const auto noise = [&](size_t f) { return !silent(f) && _voice[f] < 0.2 && _music[f] < 0.3 && r.voicing[f] < 0.5 && (r.flatness[f] > 0.25 || r.high[f] > 0.35); };
        for (const auto& [a, b] : Runs(noise, 300, 1000)) {
            Add(u, "noise", a, b, 0.4, { "broadband sound with no pitch or beat" });
        }
    }

    void Event(AudioUnderstanding& u, const std::string& kind, int from, int to, double strength, double confidence, std::vector<std::string> evidence) {
        SoundEvent e;
        e.kind = kind;
        e.startMS = std::max(0, from);
        e.endMS = std::min(u.durationMS, std::max(from, to));
        e.strength = Round3(std::clamp(strength, 0.0, 1.0));
        e.confidence = Round3(std::clamp(confidence, 0.0, 1.0));
        e.evidence = std::move(evidence);
        u.events.push_back(std::move(e));
    }

    void Events(AudioUnderstanding& u) {
        Raw& r = _r;
        std::vector<double> db(_n);
        for (size_t f = 0; f < _n; ++f) {
            db[f] = ToDb(r.rms[f]);
        }
        const std::vector<double> level = Smooth(db, 5);
        std::vector<double> sounding;
        for (double v : level) {
            if (v > _floorDb) {
                sounding.push_back(v);
            }
        }
        const double median = Percentile(sounding, 0.5);
        // Attacks and accents.
        std::vector<double> strengths;
        for (size_t f = 0; f < _n; ++f) {
            if (_onset[f]) {
                strengths.push_back(r.flux[f]);
            }
        }
        const double accentAt = Percentile(strengths, 0.8);
        for (size_t f = 0; f < _n; ++f) {
            if (!_onset[f]) {
                continue;
            }
            const double s = std::clamp(r.flux[f] / std::max(1e-9, Percentile(strengths, 0.98)), 0.0, 1.0);
            const bool accent = r.flux[f] >= accentAt;
            Event(u, accent ? "accent" : "attack", Ms(f), Ms(f) + UNDERSTANDING_HOP_MS, s, accent ? 0.7 : 0.6, { "sudden spectral change" });
        }
        for (const ActivitySegment& s : u.activities) {
            if (s.kind == "silence") {
                Event(u, "silence", s.startMS, s.endMS, 0, 0.9, s.evidence);
            } else if (s.kind == "pause") {
                Event(u, "pause", s.startMS, s.endMS, 0.3, 0.6, s.evidence);
            }
        }
        // Sustains: a level that holds.
        const size_t hold = 800 / UNDERSTANDING_HOP_MS;
        for (size_t a = 0; a + hold < _n;) {
            size_t b = a;
            double lo = level[a], hi = level[a];
            while (b < _n && level[a] > _floorDb + 12) {
                lo = std::min(lo, level[b]);
                hi = std::max(hi, level[b]);
                if (hi - lo > 3) {
                    break;
                }
                ++b;
            }
            if (b - a >= hold) {
                Event(u, "sustain", Ms(a), Ms(b), std::clamp((level[a] - _floorDb) / 40.0, 0.0, 1.0), 0.6, { "the level holds within 3 dB" });
                a = b;
            } else {
                ++a;
            }
        }
        // Decays and tails: after a peak the sound dies away with no new attack.
        for (size_t f = 1; f + 2 < _n; ++f) {
            if (!(level[f] > _floorDb + 12 && level[f] >= level[f - 1] && level[f] > level[f + 1])) {
                continue;
            }
            size_t g = f + 1;
            while (g + 1 < _n && level[g + 1] <= level[g] + 0.3 && !_onset[g + 1] && Ms(g) - Ms(f) < 8000) {
                ++g;
            }
            const double fall = level[f] - level[g];
            const int length = Ms(g) - Ms(f);
            if (fall >= 9 && length >= 150) {
                const bool intoSilence = level[g] <= _floorDb + 3 || (g + 3 < _n && level[g + 3] <= _floorDb + 3);
                Event(u, intoSilence ? "tail" : "decay", Ms(f), Ms(g), std::clamp(fall / 40.0, 0.0, 1.0), 0.6, { fmt::format("falls {:.0f} dB in {} ms", fall, length) });
                f = g;
            }
        }
        // Slow changes: fades, crescendos and diminuendos over seconds.
        const size_t span = 2500 / UNDERSTANDING_HOP_MS;
        const std::vector<double> slow = Smooth(db, 25);
        for (size_t a = 0; a + span < _n;) {
            const double start = slow[a];
            size_t b = a + span;
            const double direction = slow[b] - start;
            if (std::fabs(direction) < 6) {
                a += span / 5;
                continue;
            }
            const double sign = direction > 0 ? 1 : -1;
            while (b + 1 < _n && (slow[b + 1] - slow[b]) * sign > -0.05) {
                ++b;
            }
            const double change = slow[b] - start;
            const bool fromSilence = start <= _floorDb + 6;
            const bool toSilence = slow[b] <= _floorDb + 6;
            std::string kind = change > 0 ? (fromSilence ? "fade-in" : "crescendo") : (toSilence ? "fade-out" : "diminuendo");
            if (std::fabs(change) >= 6) {
                Event(u, kind, Ms(a), Ms(b), std::clamp(std::fabs(change) / 30.0, 0.0, 1.0), 0.55, { fmt::format("{:+.0f} dB over {} ms", change, Ms(b) - Ms(a)) });
            }
            a = b;
        }
        // Drops and re-entries: the music falls away suddenly, or comes back.
        const size_t quick = 300 / UNDERSTANDING_HOP_MS;
        const size_t stay = 600 / UNDERSTANDING_HOP_MS;
        for (size_t f = quick; f + stay < _n; ++f) {
            const double before = level[f - quick];
            const double after = Mean(level, f, f + stay);
            if (before >= median - 3 && before - after >= 10 && SoundsAfter(level, f + stay)) {
                const size_t at = Steepest(db, f - quick, f + stay, -1);
                Event(u, "drop", Ms(at), Ms(at) + 300, std::clamp((before - after) / 30.0, 0.0, 1.0), 0.55, { fmt::format("{:.0f} dB down at once", before - after) });
                f = at + stay;
            }
        }
        const size_t lowFor = 1000 / UNDERSTANDING_HOP_MS;
        for (size_t f = lowFor; f + quick < _n; ++f) {
            const double before = Mean(level, f - lowFor, f);
            const double after = level[f + quick];
            if (after - before >= 10 && after >= median - 6) {
                const size_t at = Steepest(db, f - quick, std::min(_n - 1, f + quick), +1);
                Event(u, "reentry", Ms(at), Ms(at) + 300, std::clamp((after - before) / 30.0, 0.0, 1.0), 0.55, { fmt::format("{:.0f} dB back at once", after - before) });
                f = at + lowFor;
            }
        }
        // A resolution: a held sound that dies away at the end of the music.
        int lastSound = 0;
        for (size_t f = 0; f < _n; ++f) {
            lastSound = level[f] > _floorDb + 3 ? Ms(f) : lastSound;
        }
        for (const SoundEvent& e : std::vector<SoundEvent>(u.events)) {
            if (e.kind == "tail" && (e.startMS >= u.durationMS * 0.75 || e.endMS >= lastSound - 500)) {
                Event(u, "resolution", e.startMS, e.endMS, e.strength, 0.35, { "the last held sound dies away" });
                break;
            }
        }
        std::sort(u.events.begin(), u.events.end(), [](const SoundEvent& a, const SoundEvent& b) { return a.startMS != b.startMS ? a.startMS < b.startMS : a.kind < b.kind; });
    }

    // The frame where the level changes most in one direction (-1 falls, +1 rises).
    size_t Steepest(const std::vector<double>& db, size_t from, size_t to, int sign) const {
        size_t best = from;
        double most = -1e9;
        for (size_t f = from + 1; f <= to && f < db.size(); ++f) {
            const double change = (db[f] - db[f - 1]) * sign;
            if (change > most) {
                most = change;
                best = f;
            }
        }
        return best;
    }

    // The music goes on later: a drop is not the end of the piece.
    bool SoundsAfter(const std::vector<double>& level, size_t from) const {
        for (size_t f = from; f < level.size(); ++f) {
            if (level[f] > _floorDb + 12) {
                return true;
            }
        }
        return false;
    }

    void Curve(AudioUnderstanding& u) {
        Raw& r = _r;
        const size_t step = UNDERSTANDING_CURVE_MS / UNDERSTANDING_HOP_MS;
        const auto mean = [&](const std::vector<double>& v, size_t f) { return static_cast<float>(Round3(Mean(v, f, std::min(_n, f + step)))); };
        std::vector<double> fluxes(r.flux.begin(), r.flux.end());
        const double fluxTop = std::max(1e-9, Percentile(fluxes, 0.98));
        for (size_t f = 0; f < _n; f += step) {
            SoundFrame s;
            s.timeMS = Ms(f);
            s.rms = mean(r.rms, f);
            s.low = mean(r.low, f);
            s.mid = mean(r.mid, f);
            s.high = mean(r.high, f);
            s.flux = static_cast<float>(Round3(std::clamp(Mean(r.flux, f, std::min(_n, f + step)) / fluxTop, 0.0, 1.0)));
            s.centroid = static_cast<float>(std::round(Mean(r.centroid, f, std::min(_n, f + step))));
            s.harmonic = mean(r.harmonic, f);
            s.percussive = mean(r.percussive, f);
            s.f0 = static_cast<float>(std::round(r.f0[f]));
            s.voicing = static_cast<float>(Round3(r.voicing[f]));
            s.voice = mean(_voice, f);
            s.music = mean(_music, f);
            u.curve.push_back(s);
        }
    }

    static void Number(AudioUnderstanding& u) {
        std::sort(u.activities.begin(), u.activities.end(), [](const ActivitySegment& a, const ActivitySegment& b) {
            return a.startMS != b.startMS ? a.startMS < b.startMS : a.kind < b.kind;
        });
        std::map<std::string, int> next;
        for (ActivitySegment& s : u.activities) {
            s.id = fmt::format("{}#{}", s.kind, ++next[s.kind]);
        }
        next.clear();
        for (SoundEvent& e : u.events) {
            e.id = fmt::format("{}#{}", e.kind, ++next[e.kind]);
        }
    }

    const std::vector<float>& _left;
    const std::vector<float>& _right;
    int _rate;
    UnderstandingOptions _o;
    UnderstandingCancel _cancel;
    size_t _hop = 1;
    size_t _n = 0;
    Raw _r;
    std::vector<double> _voice, _music;
    std::vector<bool> _onset;
    std::vector<bool> _struck; // onsets that are not a voice starting a syllable
    double _floorDb = -50;
};

const char* const ACTION_IDS[] = { "move", "split", "merge", "reclassify", "reassign", "remove", "add", "confirm" };

std::string OptionsKey(const UnderstandingOptions& o) {
    return fmt::format("{}|{}|{}|{}|{}|{}", AUDIO_UNDERSTANDING_VERSION, o.silenceDb, o.minSilenceMS, o.minPauseMS, o.minVoiceMS, o.estimatePitch ? 1 : 0);
}

bool Decode(const std::string& path, std::vector<float>& left, std::vector<float>& right, int& rate, int& channels, std::string& problem) {
    ObtainAccessToURL(path, false);
    if (!FileExists(path)) {
        problem = "not-found";
        return false;
    }
    AudioManager audio(path);
    if (!audio.IsOk() || audio.GetTrackSize() <= 0) {
        problem = "unreadable";
        return false;
    }
    // The raw copy of the samples exists only once asked for; until then reading the
    // raw data waits for it.
    (void)audio.EnsureFilteredAudioData(AUDIOSAMPLETYPE::RAW, 0, 0);
    const long n = audio.GetTrackSize();
    const float* l = audio.GetRawLeftDataPtr(0);
    const float* r = audio.GetRawRightDataPtr(0);
    if (l == nullptr) {
        problem = "unreadable";
        return false;
    }
    left.assign(l, l + n);
    right = r != nullptr && r != l ? std::vector<float>(r, r + n) : std::vector<float>();
    rate = static_cast<int>(audio.GetRate());
    channels = audio.GetChannels();
    return true;
}

} // namespace

std::vector<const ActivitySegment*> AudioUnderstanding::Of(const std::string& kind) const {
    std::vector<const ActivitySegment*> out;
    for (const ActivitySegment& s : activities) {
        if (s.kind == kind) {
            out.push_back(&s);
        }
    }
    return out;
}

const ActivitySegment* AudioUnderstanding::Find(const std::string& id) const {
    for (const ActivitySegment& s : activities) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

bool AudioUnderstanding::Has(const std::string& kind, int fromMS, int toMS) const {
    return std::any_of(activities.begin(), activities.end(), [&](const ActivitySegment& s) { return s.kind == kind && s.startMS < toMS && s.endMS > fromMS; });
}

AudioUnderstanding UnderstandSamples(const std::vector<float>& left, const std::vector<float>& right, int sampleRate, const UnderstandingOptions& options, const UnderstandingCancel& cancel) {
    AudioUnderstanding u = Reader(left, right, sampleRate, options, cancel).Run();
    u.optionsKey = OptionsKey(options);
    return u;
}

UnderstandingResult UnderstandAudio(const std::string& mixPath, const std::vector<StemInput>& stems, const UnderstandingOptions& options, const UnderstandingProgress& progress,
                                    const UnderstandingCancel& cancel) {
    UnderstandingResult result;
    const auto say = [&](int p, const char* stage) {
        if (progress) {
            progress(p, stage);
        }
    };
    say(2, "decode");
    std::vector<float> left, right;
    int rate = 0, channels = 0;
    std::string problem;
    if (!Decode(mixPath, left, right, rate, channels, problem)) {
        result.errorCode = problem == "not-found" ? "UNDERSTAND_FILE_NOT_FOUND" : "UNDERSTAND_UNREADABLE";
        result.errorDetail = mixPath;
        return result;
    }
    if (static_cast<long long>(left.size()) * 1000 / std::max(1, rate) < 1000) {
        result.errorCode = "UNDERSTAND_TOO_SHORT";
        result.errorDetail = mixPath;
        return result;
    }
    say(10, "analyse");
    result.understanding = UnderstandSamples(left, right, rate, options, cancel);
    if (cancel && cancel()) {
        result.cancelled = true;
        return result;
    }
    AudioUnderstanding& u = result.understanding;
    u.audioHash = Sha256OfFile(mixPath);
    u.channels = channels;
    // Stems: a voice on its own track is a voice for sure, where it sounds.
    int index = 0;
    for (const StemInput& stem : stems) {
        say(50 + 40 * index++ / std::max<size_t>(1, stems.size()), "stems");
        StemInfo info;
        info.role = stem.role;
        info.name = stem.name;
        info.file = fs::path(stem.path).filename().string();
        std::vector<float> sl, sr;
        int srate = 0, sch = 0;
        if (!Decode(stem.path, sl, sr, srate, sch, info.problem)) {
            u.stems.push_back(info);
            u.warnings.push_back("stem-unusable:" + info.file);
            continue;
        }
        info.hash = Sha256OfFile(stem.path);
        info.durationMS = static_cast<int>(static_cast<long long>(sl.size()) * 1000 / std::max(1, srate));
        info.usable = true;
        u.stems.push_back(info);
        if (stem.role != "voice" && stem.role != "chorus" && stem.role != "narration") {
            continue;
        }
        const AudioUnderstanding own = UnderstandSamples(sl, sr, srate, options, cancel);
        const std::string track = stem.role == "chorus" ? "chorus" : "stem:" + (stem.name.empty() ? info.file : stem.name);
        for (const ActivitySegment& s : own.activities) {
            if (s.kind != "speech" && s.kind != "singing" && s.kind != "chorus") {
                continue;
            }
            ActivitySegment copy = s;
            copy.kind = stem.role == "narration" ? "speech" : stem.role == "chorus" ? "chorus" : s.kind;
            copy.provenance = "stem:" + stem.role;
            copy.track = track;
            copy.confidence = std::max(copy.confidence, 0.85);
            copy.evidence.push_back("heard on its own track");
            u.activities.push_back(copy);
        }
    }
    if (std::any_of(u.stems.begin(), u.stems.end(), [](const StemInfo& s) { return s.usable && (s.role == "voice" || s.role == "chorus" || s.role == "narration"); })) {
        // The voices of the mix give way to the ones heard alone.
        std::erase_if(u.activities, [](const ActivitySegment& s) {
            return (s.kind == "speech" || s.kind == "singing" || s.kind == "chorus") && s.provenance == "detected";
        });
    }
    std::sort(u.activities.begin(), u.activities.end(), [](const ActivitySegment& a, const ActivitySegment& b) { return a.startMS != b.startMS ? a.startMS < b.startMS : a.kind < b.kind; });
    std::map<std::string, int> next;
    for (ActivitySegment& s : u.activities) {
        s.id = fmt::format("{}#{}", s.kind, ++next[s.kind]);
    }
    say(100, "done");
    result.ok = true;
    return result;
}

namespace {

// The thing a correction names: by its id while it is near where it was (or a person
// already moved it), else, renumbered by a new analysis, the nearest of the same kind.
template <typename T>
T* Locate(std::vector<T>& items, const UnderstandingCorrection& c) {
    for (T& s : items) {
        if (s.id == c.targetId && (c.anchorMS < 0 || std::abs(s.startMS - c.anchorMS) <= 1500 || s.provenance == "corrected")) {
            return &s;
        }
    }
    const std::string kind = c.targetId.substr(0, c.targetId.find('#'));
    T* best = nullptr;
    int distance = 1501;
    for (T& s : items) {
        if (s.kind == kind && c.anchorMS >= 0 && std::abs(s.startMS - c.anchorMS) < distance) {
            distance = std::abs(s.startMS - c.anchorMS);
            best = &s;
        }
    }
    return best;
}

void ApplyToEvent(AudioUnderstanding& u, const UnderstandingCorrection& c) {
    if (c.action == UnderstandingCorrection::Action::Add) {
        const bool there = std::any_of(u.events.begin(), u.events.end(), [&](const SoundEvent& e) {
            return e.provenance == "added" && e.kind == c.kind && e.startMS == c.startMS;
        });
        if (!there && c.startMS >= 0 && !c.kind.empty()) {
            SoundEvent e;
            e.id = fmt::format("{}#a{}", c.kind, c.startMS);
            e.kind = c.kind;
            e.startMS = c.startMS;
            e.endMS = std::max(c.endMS, c.startMS);
            e.strength = 0.5;
            e.confidence = 1;
            e.provenance = "added";
            e.evidence = { "added by a person" };
            u.events.push_back(e);
        }
        return;
    }
    SoundEvent* e = Locate(u.events, c);
    if (e == nullptr) {
        return;
    }
    switch (c.action) {
    case UnderstandingCorrection::Action::Move:
        if (c.startMS >= 0 && c.endMS >= c.startMS) {
            e->startMS = c.startMS;
            e->endMS = c.endMS;
            e->provenance = "corrected";
        }
        break;
    case UnderstandingCorrection::Action::Reclassify:
        if (!c.kind.empty()) {
            e->kind = c.kind;
            e->provenance = "corrected";
        }
        break;
    case UnderstandingCorrection::Action::Remove:
        e->kind = "removed";
        break;
    case UnderstandingCorrection::Action::Confirm:
        e->confidence = 1;
        if (e->provenance == "detected") {
            e->provenance = "confirmed";
        }
        break;
    default: // a moment is not split, merged or given to a voice
        break;
    }
}

} // namespace

void ApplyUnderstandingCorrections(AudioUnderstanding& u) {
    for (const UnderstandingCorrection& c : u.corrections) {
        if (c.on == "event") {
            ApplyToEvent(u, c);
            continue;
        }
        if (c.action == UnderstandingCorrection::Action::Add) {
            const bool there = std::any_of(u.activities.begin(), u.activities.end(), [&](const ActivitySegment& s) {
                return s.provenance == "added" && s.kind == c.kind && s.startMS == c.startMS && s.endMS == c.endMS && s.track == c.track;
            });
            if (!there && c.endMS > c.startMS) {
                ActivitySegment s;
                s.id = fmt::format("{}#a{}", c.kind, c.startMS);
                s.kind = c.kind;
                s.startMS = c.startMS;
                s.endMS = c.endMS;
                s.track = c.track;
                s.confidence = 1;
                s.provenance = "added";
                s.evidence = { "added by a person" };
                u.activities.push_back(s);
            }
            continue;
        }
        ActivitySegment* s = Locate(u.activities, c);
        if (s == nullptr) {
            continue;
        }
        switch (c.action) {
        case UnderstandingCorrection::Action::Move:
            if (c.endMS > c.startMS) {
                s->startMS = c.startMS;
                s->endMS = c.endMS;
                s->provenance = "corrected";
            }
            break;
        case UnderstandingCorrection::Action::Split:
            if (c.atMS > s->startMS && c.atMS < s->endMS) {
                ActivitySegment right = *s;
                right.startMS = c.atMS;
                right.id = s->id + "/b";
                right.provenance = "corrected";
                s->endMS = c.atMS;
                s->provenance = "corrected";
                u.activities.push_back(right);
            }
            break;
        case UnderstandingCorrection::Action::Merge: {
            ActivitySegment* other = nullptr;
            for (ActivitySegment& o : u.activities) {
                other = o.id == c.otherId && &o != s ? &o : other;
            }
            if (other != nullptr) {
                s->startMS = std::min(s->startMS, other->startMS);
                s->endMS = std::max(s->endMS, other->endMS);
                s->provenance = "corrected";
                other->kind = "removed";
            }
            break;
        }
        case UnderstandingCorrection::Action::Reclassify:
            if (!c.kind.empty()) {
                // A person said what it is: no longer a guess.
                s->kind = c.kind;
                s->provenance = "corrected";
                s->confidence = 1;
                std::erase_if(s->evidence, [](const std::string& e) { return e.find("uncertain") != std::string::npos; });
            }
            break;
        case UnderstandingCorrection::Action::Reassign:
            s->track = c.track;
            s->provenance = "corrected";
            break;
        case UnderstandingCorrection::Action::Remove:
            s->kind = "removed";
            break;
        case UnderstandingCorrection::Action::Confirm:
            s->confidence = 1;
            std::erase_if(s->evidence, [](const std::string& e) { return e.find("uncertain") != std::string::npos; });
            if (s->provenance == "detected") {
                s->provenance = "confirmed";
            }
            break;
        default:
            break;
        }
    }
    std::erase_if(u.activities, [](const ActivitySegment& s) { return s.kind == "removed"; });
    std::erase_if(u.events, [](const SoundEvent& e) { return e.kind == "removed"; });
    std::sort(u.events.begin(), u.events.end(), [](const SoundEvent& a, const SoundEvent& b) { return a.startMS != b.startMS ? a.startMS < b.startMS : a.id < b.id; });
    std::sort(u.activities.begin(), u.activities.end(), [](const ActivitySegment& a, const ActivitySegment& b) { return a.startMS != b.startMS ? a.startMS < b.startMS : a.id < b.id; });
}

void ResetUnderstandingCorrections(AudioUnderstanding& u) {
    u.corrections.clear();
}

const char* CorrectionActionId(UnderstandingCorrection::Action action) {
    return ACTION_IDS[static_cast<int>(action)];
}

std::string ActivityPhrase(const std::string& kind) {
    static const std::map<std::string, const char*> P = {
        { "speech", COMPOSER_TEXT("Someone speaks") }, { "singing", COMPOSER_TEXT("Someone sings") }, { "chorus", COMPOSER_TEXT("The choir comes in") },
        { "a-cappella", COMPOSER_TEXT("A voice with no music") }, { "music", COMPOSER_TEXT("The music plays") }, { "instrumental", COMPOSER_TEXT("An instrument plays") },
        { "percussion", COMPOSER_TEXT("Drums and percussion") }, { "bass", COMPOSER_TEXT("The bass") }, { "silence", COMPOSER_TEXT("Silence") },
        { "pause", COMPOSER_TEXT("The voice pauses") }, { "noise", COMPOSER_TEXT("Noise or ambience") }
    };
    const auto it = P.find(kind);
    return it != P.end() ? it->second : kind;
}

std::string EventPhrase(const std::string& kind) {
    static const std::map<std::string, const char*> P = {
        { "attack", COMPOSER_TEXT("A sound starts") }, { "accent", COMPOSER_TEXT("A strong hit") }, { "sustain", COMPOSER_TEXT("The sound holds") },
        { "decay", COMPOSER_TEXT("The sound dies down") }, { "tail", COMPOSER_TEXT("The sound fades into silence") }, { "fade-in", COMPOSER_TEXT("The music fades in") },
        { "fade-out", COMPOSER_TEXT("The music fades out") }, { "crescendo", COMPOSER_TEXT("The music grows") }, { "diminuendo", COMPOSER_TEXT("The music goes down") },
        { "silence", COMPOSER_TEXT("Silence") }, { "pause", COMPOSER_TEXT("The voice pauses") }, { "drop", COMPOSER_TEXT("The music drops suddenly") },
        { "reentry", COMPOSER_TEXT("The music comes back") }, { "resolution", COMPOSER_TEXT("The music resolves") }
    };
    const auto it = P.find(kind);
    return it != P.end() ? it->second : kind;
}

std::string ToJson(const AudioUnderstanding& u) {
    json activities = json::array();
    for (const ActivitySegment& s : u.activities) {
        activities.push_back({ { "id", s.id }, { "kind", s.kind }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "confidence", s.confidence }, { "track", s.track },
                               { "provenance", s.provenance }, { "evidence", s.evidence } });
    }
    json events = json::array();
    for (const SoundEvent& e : u.events) {
        events.push_back({ { "id", e.id }, { "kind", e.kind }, { "startMS", e.startMS }, { "endMS", e.endMS }, { "strength", e.strength }, { "confidence", e.confidence },
                           { "provenance", e.provenance }, { "evidence", e.evidence } });
    }
    json curve = json::array();
    for (const SoundFrame& f : u.curve) {
        curve.push_back(json::array({ f.timeMS, f.rms, f.low, f.mid, f.high, f.flux, f.centroid, f.harmonic, f.percussive, f.f0, f.voicing, f.voice, f.music }));
    }
    json stems = json::array();
    for (const StemInfo& s : u.stems) {
        stems.push_back({ { "role", s.role }, { "name", s.name }, { "file", s.file }, { "hash", s.hash }, { "durationMS", s.durationMS }, { "usable", s.usable }, { "problem", s.problem } });
    }
    json corrections = json::array();
    for (const UnderstandingCorrection& c : u.corrections) {
        corrections.push_back({ { "action", CorrectionActionId(c.action) }, { "on", c.on }, { "target", c.targetId }, { "anchorMS", c.anchorMS }, { "startMS", c.startMS }, { "endMS", c.endMS },
                                { "atMS", c.atMS }, { "other", c.otherId }, { "kind", c.kind }, { "track", c.track }, { "note", c.note } });
    }
    const json root = { { "kind", "composer-audio-understanding" }, { "schemaVersion", u.schemaVersion }, { "version", u.version }, { "audioHash", u.audioHash }, { "optionsKey", u.optionsKey },
                        { "durationMS", u.durationMS }, { "channels", u.channels }, { "stereo", u.stereo }, { "stems", stems },
                        { "curveFields", { "timeMS", "rms", "low", "mid", "high", "flux", "centroid", "harmonic", "percussive", "f0", "voicing", "voice", "music" } },
                        { "curve", curve }, { "activities", activities }, { "events", events }, { "warnings", u.warnings }, { "corrections", corrections } };
    return root.dump(1);
}

bool FromJson(const std::string& text, AudioUnderstanding& out, std::string& error) {
    try {
        const json root = json::parse(text);
        const int version = root.value("schemaVersion", 0);
        if (version < 1 || version > AUDIO_UNDERSTANDING_SCHEMA_VERSION) {
            error = fmt::format("understanding: schema {} is not one this program reads", version);
            return false;
        }
        AudioUnderstanding u;
        u.version = root.value("version", std::string());
        u.audioHash = root.value("audioHash", std::string());
        u.optionsKey = root.value("optionsKey", std::string());
        u.durationMS = root.value("durationMS", 0);
        u.channels = root.value("channels", 0);
        u.stereo = root.value("stereo", false);
        for (const json& s : root.value("stems", json::array())) {
            u.stems.push_back({ s.value("role", std::string()), s.value("name", std::string()), s.value("file", std::string()), s.value("hash", std::string()), s.value("durationMS", 0),
                                s.value("usable", false), s.value("problem", std::string()) });
        }
        for (const json& f : root.value("curve", json::array())) {
            if (!f.is_array() || f.size() < 13) {
                continue;
            }
            SoundFrame s;
            s.timeMS = f[0].get<int>();
            float* fields[] = { &s.rms, &s.low, &s.mid, &s.high, &s.flux, &s.centroid, &s.harmonic, &s.percussive, &s.f0, &s.voicing, &s.voice, &s.music };
            for (size_t k = 0; k < 12; ++k) {
                *fields[k] = f[k + 1].get<float>();
            }
            u.curve.push_back(s);
        }
        for (const json& s : root.value("activities", json::array())) {
            ActivitySegment a;
            a.id = s.at("id").get<std::string>();
            a.kind = s.at("kind").get<std::string>();
            a.startMS = s.at("startMS").get<int>();
            a.endMS = s.at("endMS").get<int>();
            a.confidence = s.value("confidence", 0.0);
            a.track = s.value("track", std::string());
            a.provenance = s.value("provenance", std::string("detected"));
            a.evidence = s.value("evidence", std::vector<std::string>());
            u.activities.push_back(std::move(a));
        }
        for (const json& s : root.value("events", json::array())) {
            SoundEvent e;
            e.id = s.at("id").get<std::string>();
            e.kind = s.at("kind").get<std::string>();
            e.startMS = s.at("startMS").get<int>();
            e.endMS = s.at("endMS").get<int>();
            e.strength = s.value("strength", 0.0);
            e.confidence = s.value("confidence", 0.0);
            e.provenance = s.value("provenance", std::string("detected"));
            e.evidence = s.value("evidence", std::vector<std::string>());
            u.events.push_back(std::move(e));
        }
        u.warnings = root.value("warnings", std::vector<std::string>());
        for (const json& c : root.value("corrections", json::array())) {
            UnderstandingCorrection k;
            const std::string action = c.value("action", std::string("confirm"));
            bool known = false;
            for (int i = 0; i < 8; ++i) {
                if (action == ACTION_IDS[i]) {
                    k.action = static_cast<UnderstandingCorrection::Action>(i);
                    known = true;
                }
            }
            if (!known) {
                error = "understanding: unknown correction " + action;
                return false;
            }
            k.on = c.value("on", std::string("activity"));
            k.targetId = c.value("target", std::string());
            k.anchorMS = c.value("anchorMS", -1);
            k.startMS = c.value("startMS", -1);
            k.endMS = c.value("endMS", -1);
            k.atMS = c.value("atMS", -1);
            k.otherId = c.value("other", std::string());
            k.kind = c.value("kind", std::string());
            k.track = c.value("track", std::string());
            k.note = c.value("note", std::string());
            u.corrections.push_back(std::move(k));
        }
        out = std::move(u);
    } catch (const json::exception& e) {
        error = std::string("understanding: ") + e.what();
        return false;
    }
    return true;
}

std::string UnderstandingOptionsKey(const UnderstandingOptions& options) {
    return OptionsKey(options);
}

} // namespace composer
