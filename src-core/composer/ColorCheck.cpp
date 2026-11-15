#include "composer/ColorCheck.h"

#include "composer/ColorSpace.h"
#include "render/FSEQFile.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <memory>

namespace composer {

namespace {

constexpr int LIT = 24;            // a light counts as lit above this on any channel
constexpr double MIN_LIT = 0.01;   // a frame with less of the house lit is dark, not counted
constexpr double FAINT = 15;       // a gesture under this ΔE against what was there blends in
constexpr int BITS = 6;            // colours are looked up at 6 bits a channel
constexpr int LEVELS = 1 << BITS;

using Histogram = std::array<float, FAMILY_COUNT>;

struct Swatch {
    float L = 0, a = 0, b = 0;
    uint8_t family = FAMILY_NEUTRAL;
};

// Every colour at 6 bits a channel, converted once.
const std::vector<Swatch>& Table() {
    static const std::vector<Swatch> table = [] {
        std::vector<Swatch> t(static_cast<size_t>(LEVELS) * LEVELS * LEVELS);
        for (int r = 0; r < LEVELS; ++r) {
            for (int g = 0; g < LEVELS; ++g) {
                for (int b = 0; b < LEVELS; ++b) {
                    const double s = 1.0 / (LEVELS - 1);
                    const Oklab lab = ToOklab(Rgb { r * s, g * s, b * s });
                    Swatch& w = t[(static_cast<size_t>(r) * LEVELS + g) * LEVELS + b];
                    w.L = static_cast<float>(lab.L);
                    w.a = static_cast<float>(lab.a);
                    w.b = static_cast<float>(lab.b);
                    w.family = static_cast<uint8_t>(ColorFamily(ToOklch(lab)));
                }
            }
        }
        return t;
    }();
    return table;
}

const Swatch& Look(uint8_t r, uint8_t g, uint8_t b) {
    return Table()[(static_cast<size_t>(r >> (8 - BITS)) * LEVELS + (g >> (8 - BITS))) * LEVELS + (b >> (8 - BITS))];
}

struct Measured {
    std::string name;
    std::vector<uint32_t> channels;
    int ro = 0, go = 1, bo = 2; // where red, green and blue are in a light's channels
    bool single = false;        // a single-colour string: one channel, read as white
};

struct Accum {
    double L = 0, a = 0, b = 0, w = 0;
    void Add(const Swatch& s, double weight) {
        L += s.L * weight;
        a += s.a * weight;
        b += s.b * weight;
        w += weight;
    }
    [[nodiscard]] Oklab Mean() const {
        return w > 0 ? Oklab { L / w, a / w, b / w } : Oklab();
    }
};

// What a figure showed in a frame.
struct FigureFrame {
    float L = 0, a = 0, b = 0; // mean colour of its lit lights, by brightness
    float weight = 0;          // its light: sum of brightness
    uint8_t family = FAMILY_NEUTRAL;
    bool lit = false;
};

double Entropy(const Histogram& h, double& effective) {
    double total = 0;
    for (float v : h) {
        total += v;
    }
    double e = 0;
    if (total > 0) {
        for (float v : h) {
            if (v > 0) {
                const double p = v / total;
                e -= p * std::log(p);
            }
        }
    }
    effective = std::exp(e);
    return e / std::log(static_cast<double>(FAMILY_COUNT));
}

int Dominant(const Histogram& h, double& share) {
    double total = 0;
    int best = FAMILY_NEUTRAL;
    for (int k = 0; k < FAMILY_COUNT; ++k) {
        total += h[k];
        if (h[k] > h[best]) {
            best = k;
        }
    }
    share = total > 0 ? h[best] / total : 0;
    return best;
}

std::vector<ColorShare> Shares(const Histogram& h) {
    double total = 0;
    for (float v : h) {
        total += v;
    }
    std::vector<ColorShare> out;
    for (int k = 0; k < FAMILY_COUNT; ++k) {
        if (total > 0 && h[k] / total >= 0.01) {
            out.push_back({ ColorFamilyId(k), std::round(h[k] / total * 1000.0) / 1000.0 });
        }
    }
    std::sort(out.begin(), out.end(), [](const ColorShare& x, const ColorShare& y) { return x.share > y.share; });
    return out;
}

double Variation(const Histogram& x, const Histogram& y) {
    double tx = 0, ty = 0;
    for (int k = 0; k < FAMILY_COUNT; ++k) {
        tx += x[k];
        ty += y[k];
    }
    if (tx <= 0 || ty <= 0) {
        return 0;
    }
    double d = 0;
    for (int k = 0; k < FAMILY_COUNT; ++k) {
        d += std::fabs(x[k] / tx - y[k] / ty);
    }
    return 0.5 * d;
}

std::string HexOf(const Oklab& lab) {
    return ToHex(ToRgbInGamut(ToOklch(lab)));
}

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

// A probe of one gesture on one figure: its lights just before and during its part.
struct Probe {
    size_t gesture = 0;
    size_t figure = 0;
    int atMS = 0;
    int beforeFrame = 0;
    int duringFrame = 0;
    std::vector<uint8_t> before; // RGB per light
    std::vector<uint8_t> during;
    Accum house; // the rest of the lit house during
};

} // namespace

ColorCheck CheckColors(const std::string& fseqPath, const FigureCatalog& geometry, const StageSpace& space, const GlobalStageScore* score, const std::set<std::string>* intended) {
    ColorCheck check;
    std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(fseqPath));
    if (file == nullptr) {
        check.problem = "fseq-unreadable";
        return check;
    }
    const uint32_t channels = static_cast<uint32_t>(file->getMaxChannel());
    check.frameMS = std::max(1, static_cast<int>(file->getStepTime()));
    check.frames = static_cast<int>(file->getNumFrames());
    std::vector<Measured> figures;
    std::map<std::string, size_t> index;
    for (const StageFigure& f : space.figures) {
        const FigureGeometry* g = geometry.Find(f.name);
        if (!f.placed || f.noController || g == nullptr || g->nodeMap.empty()) {
            continue;
        }
        Measured m;
        m.name = f.name;
        m.single = g->channelsPerNode > 0 && g->channelsPerNode < 3;
        if (!m.single && g->colorOrder.size() >= 3) {
            const auto at = [&](char c) {
                const size_t p = g->colorOrder.find(c);
                return p == std::string::npos ? 0 : static_cast<int>(p);
            };
            m.ro = at('R');
            m.go = at('G');
            m.bo = at('B');
        }
        for (const FigureNode& n : g->nodeMap) {
            if (n.channel >= 0 && static_cast<uint32_t>(n.channel) + (m.single ? 0 : 2) < channels) {
                m.channels.push_back(static_cast<uint32_t>(n.channel));
            }
        }
        if (!m.channels.empty()) {
            index[m.name] = figures.size();
            check.lights += static_cast<int>(m.channels.size());
            figures.push_back(std::move(m));
        }
    }
    check.figures = static_cast<int>(figures.size());
    if (figures.empty() || check.frames == 0) {
        check.problem = "nothing-to-measure";
        return check;
    }
    // The scenes: the score's, or ten-second stretches.
    for (int a = 0; score == nullptr && a < check.frames * check.frameMS; a += 10000) {
        check.scenes.push_back({ fmt::format("{}-{}", a, a + 10000), "", a, std::min(a + 10000, check.frames * check.frameMS) });
    }
    if (score != nullptr) {
        for (const StageScene& s : score->scenes) {
            SceneColor sc;
            sc.id = s.id;
            sc.mode = StageModeId(s.mode);
            sc.startMS = s.startMS;
            sc.endMS = s.endMS;
            sc.intended = intended != nullptr && intended->contains(s.id);
            check.scenes.push_back(sc);
        }
    }
    // The gestures' probes.
    std::vector<Probe> probes;
    std::map<int, std::vector<size_t>> probesAt; // frame -> probes reading it
    if (score != nullptr) {
        for (size_t gi = 0; gi < score->gestures.size(); ++gi) {
            const GlobalGesture& g = score->gestures[gi];
            for (const GestureParticipant& p : g.participants) {
                const auto it = index.find(p.figure);
                if (it == index.end()) {
                    continue;
                }
                Probe pr;
                pr.gesture = gi;
                pr.figure = it->second;
                pr.atMS = g.startMS + p.delayMS;
                pr.beforeFrame = std::clamp((pr.atMS - 100) / check.frameMS, 0, check.frames - 1);
                pr.duringFrame = std::clamp(std::min(pr.atMS + 200, std::max(pr.atMS, g.endMS - check.frameMS)) / check.frameMS, 0, check.frames - 1);
                probesAt[pr.beforeFrame].push_back(probes.size());
                probesAt[pr.duringFrame].push_back(probes.size());
                probes.push_back(std::move(pr));
            }
        }
    }

    std::vector<std::vector<FigureFrame>> seen(static_cast<size_t>(check.frames), std::vector<FigureFrame>(figures.size()));
    std::vector<Histogram> house(static_cast<size_t>(check.frames));
    std::vector<char> lit(static_cast<size_t>(check.frames), 0);
    std::vector<float> spread(static_cast<size_t>(check.frames), 0);   // 1 - |mean hue vector|
    std::vector<float> perFigure(static_cast<size_t>(check.frames), 0); // families over 10% in a lit figure
    file->prepareRead({ { 0, channels } });
    std::vector<uint8_t> data(channels, 0);
    for (int f = 0; f < check.frames; ++f) {
        std::fill(data.begin(), data.end(), 0);
        std::unique_ptr<FSEQFile::FrameData> frame(file->getFrame(static_cast<uint32_t>(f)));
        if (frame == nullptr || !frame->readFrame(data.data(), channels)) {
            continue;
        }
        Histogram& h = house[f];
        h.fill(0);
        int litLights = 0;
        double va = 0, vb = 0, vc = 0;
        double figureColours = 0;
        int litFigures = 0;
        for (size_t k = 0; k < figures.size(); ++k) {
            const Measured& m = figures[k];
            Accum acc;
            Histogram fh {};
            for (uint32_t c : m.channels) {
                const uint8_t r = m.single ? data[c] : data[c + m.ro];
                const uint8_t g = m.single ? data[c] : data[c + m.go];
                const uint8_t b = m.single ? data[c] : data[c + m.bo];
                const int top = std::max({ r, g, b });
                if (top <= LIT) {
                    continue;
                }
                ++litLights;
                const Swatch& s = Look(r, g, b);
                const double w = top / 255.0;
                acc.Add(s, w);
                fh[s.family] += static_cast<float>(w);
                const double C = std::sqrt(static_cast<double>(s.a) * s.a + static_cast<double>(s.b) * s.b);
                if (C >= NEUTRAL_CHROMA) {
                    va += w * s.a;
                    vb += w * s.b;
                    vc += w * C;
                }
            }
            FigureFrame& ff = seen[f][k];
            if (acc.w > 0) {
                const Oklab mean = acc.Mean();
                ff = { static_cast<float>(mean.L), static_cast<float>(mean.a), static_cast<float>(mean.b), static_cast<float>(acc.w), 0, true };
                double share = 0;
                ff.family = static_cast<uint8_t>(Dominant(fh, share));
                for (int q = 0; q < FAMILY_COUNT; ++q) {
                    h[q] += fh[q];
                }
                int inside = 0;
                for (int q = 0; q < FAMILY_COUNT; ++q) {
                    inside += fh[q] >= 0.1 * acc.w ? 1 : 0;
                }
                figureColours += inside;
                ++litFigures;
            }
        }
        lit[f] = litLights >= std::max(1, static_cast<int>(check.lights * MIN_LIT)) ? 1 : 0;
        spread[f] = vc > 0 ? static_cast<float>(1.0 - std::sqrt(va * va + vb * vb) / vc) : 0.0F;
        perFigure[f] = litFigures > 0 ? static_cast<float>(figureColours / litFigures) : 0.0F;
        // The probes that read this frame.
        if (const auto pa = probesAt.find(f); pa != probesAt.end()) {
            for (size_t pi : pa->second) {
                Probe& pr = probes[pi];
                const Measured& m = figures[pr.figure];
                std::vector<uint8_t> rgb;
                rgb.reserve(m.channels.size() * 3);
                for (uint32_t c : m.channels) {
                    rgb.push_back(m.single ? data[c] : data[c + m.ro]);
                    rgb.push_back(m.single ? data[c] : data[c + m.go]);
                    rgb.push_back(m.single ? data[c] : data[c + m.bo]);
                }
                if (f == pr.beforeFrame && pr.before.empty()) {
                    pr.before = std::move(rgb);
                } else {
                    pr.during = std::move(rgb);
                    for (size_t k = 0; k < figures.size(); ++k) {
                        const FigureFrame& o = seen[f][k];
                        if (k != pr.figure && o.lit) {
                            pr.house.Add(Swatch { o.L, o.a, o.b, 0 }, o.weight);
                        }
                    }
                }
            }
        }
    }

    // Figures' differences in a frame: mean pairwise ΔE of the lit figures' colours, and
    // how many families they wear.
    const auto difference = [&](int f, double& families) {
        std::vector<const FigureFrame*> on;
        std::set<int> fams;
        for (const FigureFrame& ff : seen[f]) {
            if (ff.lit && ff.weight >= 1.0F) {
                on.push_back(&ff);
                fams.insert(ff.family);
            }
        }
        families = static_cast<double>(fams.size());
        double sum = 0;
        int pairs = 0;
        for (size_t x = 0; x < on.size(); ++x) {
            for (size_t y = x + 1; y < on.size(); ++y) {
                sum += DeltaE(Oklab { on[x]->L, on[x]->a, on[x]->b }, Oklab { on[y]->L, on[y]->a, on[y]->b });
                ++pairs;
            }
        }
        return pairs > 0 ? sum / pairs : 0.0;
    };

    Histogram song {};
    double sumDiff = 0, sumFams = 0, sumAccent = 0;
    int monotone = 0, monotoneIntended = 0;
    int runStart = -1, runFamily = -1, lastLit = -1;
    const auto closeRun = [&](int endFrame) {
        if (runStart >= 0) {
            const int ms = (endFrame - runStart) * check.frameMS;
            if (ms > check.longestMonotoneMS) {
                check.longestMonotoneMS = ms;
                check.longestMonotoneStartMS = runStart * check.frameMS;
                check.longestMonotoneFamily = ColorFamilyId(runFamily);
            }
        }
        runStart = -1;
        runFamily = -1;
    };
    const auto sceneOf = [&](int ms) -> SceneColor* {
        for (SceneColor& s : check.scenes) {
            if (ms >= s.startMS && ms < s.endMS) {
                return &s;
            }
        }
        return nullptr;
    };
    std::vector<Histogram> sceneHist(check.scenes.size());
    std::vector<int> sceneFrames(check.scenes.size(), 0), sceneMonotone(check.scenes.size(), 0);
    std::vector<double> sceneDiff(check.scenes.size(), 0), sceneFams(check.scenes.size(), 0), sceneAccent(check.scenes.size(), 0);
    std::vector<double> sceneFamilies(check.scenes.size(), 0), sceneTopTwo(check.scenes.size(), 0), sceneSpread(check.scenes.size(), 0), scenePerFigure(check.scenes.size(), 0);
    double sumFamilies = 0, sumTopTwo = 0, sumSpread = 0, sumPerFigure = 0;
    std::vector<Accum> sceneMean(check.scenes.size());
    std::vector<double> sceneChroma(check.scenes.size(), 0);
    for (int f = 0; f < check.frames; ++f) {
        if (!lit[f]) {
            // A dark moment shorter than a second does not end a stretch in one colour.
            if (runStart >= 0 && (f - lastLit) * check.frameMS > 1000) {
                closeRun(lastLit + 1);
            }
            continue;
        }
        ++check.litFrames;
        const Histogram& h = house[f];
        for (int q = 0; q < FAMILY_COUNT; ++q) {
            song[q] += h[q];
        }
        double share = 0;
        const int dom = Dominant(h, share);
        SceneColor* sc = sceneOf(f * check.frameMS);
        const size_t si = sc == nullptr ? 0 : static_cast<size_t>(sc - check.scenes.data());
        const bool inOne = share >= MONOTONE_SHARE;
        if (inOne) {
            if (sc != nullptr && sc->intended) {
                ++monotoneIntended;
            } else {
                ++monotone;
            }
        }
        if (inOne && !(sc != nullptr && sc->intended)) {
            if (runStart >= 0 && dom != runFamily) {
                closeRun(f);
            }
            if (runStart < 0) {
                runStart = f;
                runFamily = dom;
            }
        } else {
            closeRun(f);
        }
        lastLit = f;
        double fams = 0;
        const double diff = difference(f, fams);
        sumDiff += diff;
        sumFams += fams;
        double total = 0;
        for (float v : h) {
            total += v;
        }
        bool accent = false;
        for (int q = 0; q < FAMILY_COUNT && total > 0; ++q) {
            accent |= q != dom && h[q] / total >= 0.03 && h[q] / total <= 0.2;
        }
        sumAccent += accent ? 1 : 0;
        // How many families share the house, and how much the two largest hold.
        std::array<float, FAMILY_COUNT> sorted = h;
        std::sort(sorted.begin(), sorted.end(), std::greater<float>());
        int families = 0;
        for (int q = 0; q < FAMILY_COUNT && total > 0; ++q) {
            families += h[q] / total >= 0.05 ? 1 : 0;
        }
        const double topTwo = total > 0 ? (sorted[0] + sorted[1]) / total : 0;
        sumFamilies += families;
        sumTopTwo += topTwo;
        sumSpread += spread[f];
        sumPerFigure += perFigure[f];
        if (sc != nullptr) {
            sceneFamilies[si] += families;
            sceneTopTwo[si] += topTwo;
            sceneSpread[si] += spread[f];
            scenePerFigure[si] += perFigure[f];
            for (int q = 0; q < FAMILY_COUNT; ++q) {
                sceneHist[si][q] += h[q];
            }
            ++sceneFrames[si];
            sceneMonotone[si] += inOne ? 1 : 0;
            sceneDiff[si] += diff;
            sceneFams[si] += fams;
            sceneAccent[si] += accent ? 1 : 0;
            for (const FigureFrame& ff : seen[f]) {
                if (ff.lit) {
                    sceneMean[si].Add(Swatch { ff.L, ff.a, ff.b, 0 }, ff.weight);
                    sceneChroma[si] += std::sqrt(ff.a * ff.a + ff.b * ff.b) * ff.weight;
                }
            }
        }
    }
    closeRun(lastLit + 1);
    if (check.litFrames > 0) {
        const double n = check.litFrames;
        check.monotoneShare = Round3(monotone / std::max(1.0, n - monotoneIntended));
        check.figureDifference = Round3(sumDiff / n);
        check.figureFamilies = Round3(sumFams / n);
        check.accentFrames = Round3(sumAccent / n);
        check.familiesAtOnce = Round3(sumFamilies / n);
        check.topTwoShare = Round3(sumTopTwo / n);
        check.minorLight = Round3(1.0 - sumTopTwo / n);
        check.hueSpread = Round3(sumSpread / n);
        check.colorsPerFigure = Round3(sumPerFigure / n);
    }
    double share = 0;
    check.dominant = ColorFamilyId(Dominant(song, share));
    check.dominantShare = Round3(share);
    check.entropy = Round3(Entropy(song, check.effectiveColors));
    check.effectiveColors = Round3(check.effectiveColors);
    check.families = Shares(song);

    std::set<std::string> dominants;
    double variation = 0;
    int pairs = 0;
    int previous = -1;
    for (size_t i = 0; i < check.scenes.size(); ++i) {
        SceneColor& s = check.scenes[i];
        if (sceneFrames[i] == 0) {
            continue;
        }
        const double n = sceneFrames[i];
        double ds = 0;
        s.dominant = ColorFamilyId(Dominant(sceneHist[i], ds));
        s.dominantShare = Round3(ds);
        s.monotoneShare = Round3(sceneMonotone[i] / n);
        s.entropy = Round3(Entropy(sceneHist[i], s.effectiveColors));
        s.effectiveColors = Round3(s.effectiveColors);
        s.figureDifference = Round3(sceneDiff[i] / n);
        s.figureFamilies = Round3(sceneFams[i] / n);
        s.accentFrames = Round3(sceneAccent[i] / n);
        s.familiesAtOnce = Round3(sceneFamilies[i] / n);
        s.topTwoShare = Round3(sceneTopTwo[i] / n);
        s.minorLight = Round3(1.0 - sceneTopTwo[i] / n);
        s.hueSpread = Round3(sceneSpread[i] / n);
        s.colorsPerFigure = Round3(scenePerFigure[i] / n);
        check.overloadedScenes += s.familiesAtOnce > 4.0 && !s.intended ? 1 : 0;
        s.chroma = Round3(sceneMean[i].w > 0 ? sceneChroma[i] / sceneMean[i].w : 0);
        s.lightness = Round3(sceneMean[i].Mean().L);
        s.meanHex = HexOf(sceneMean[i].Mean());
        s.families = Shares(sceneHist[i]);
        dominants.insert(s.dominant);
        if (previous >= 0) {
            variation += Variation(sceneHist[static_cast<size_t>(previous)], sceneHist[i]);
            ++pairs;
        }
        previous = static_cast<int>(i);
    }
    check.sceneDominants = static_cast<int>(dominants.size());
    check.sceneVariation = Round3(pairs > 0 ? variation / pairs : 0);

    // Each figure over the song.
    for (size_t k = 0; k < figures.size(); ++k) {
        FigureColorCheck fc;
        fc.name = figures[k].name;
        Histogram fh {};
        int litFrames = 0;
        std::vector<Histogram> perScene(check.scenes.size());
        for (int f = 0; f < check.frames; ++f) {
            const FigureFrame& ff = seen[f][k];
            if (!ff.lit) {
                continue;
            }
            ++litFrames;
            fh[ff.family] += ff.weight;
            if (SceneColor* sc = sceneOf(f * check.frameMS); sc != nullptr) {
                perScene[static_cast<size_t>(sc - check.scenes.data())][ff.family] += ff.weight;
            }
        }
        double fs = 0;
        fc.dominant = ColorFamilyId(Dominant(fh, fs));
        fc.dominantShare = Round3(fs);
        fc.litShare = Round3(static_cast<double>(litFrames) / check.frames);
        std::set<int> doms;
        for (const Histogram& ph : perScene) {
            double total = 0;
            for (float v : ph) {
                total += v;
            }
            if (total > 0) {
                double s2 = 0;
                doms.insert(Dominant(ph, s2));
            }
        }
        fc.sceneDominants = static_cast<int>(doms.size());
        fc.families = Shares(fh);
        check.figureChecks.push_back(std::move(fc));
    }

    // Each gesture's light against what was there.
    std::vector<double> contrasts, houseContrasts;
    for (const Probe& pr : probes) {
        if (pr.before.size() != pr.during.size() || pr.during.empty()) {
            continue;
        }
        Accum now, then;
        int changed = 0;
        for (size_t i = 0; i + 2 < pr.during.size(); i += 3) {
            const uint8_t r1 = pr.during[i], g1 = pr.during[i + 1], b1 = pr.during[i + 2];
            const uint8_t r0 = pr.before[i], g0 = pr.before[i + 1], b0 = pr.before[i + 2];
            const int top1 = std::max({ r1, g1, b1 });
            if (top1 <= LIT) {
                continue;
            }
            const Swatch& s1 = Look(r1, g1, b1);
            const Swatch& s0 = Look(r0, g0, b0);
            const int top0 = std::max({ r0, g0, b0 });
            const bool rose = top1 - top0 >= 30;
            const bool turned = DeltaE(Oklab { s1.L, s1.a, s1.b }, Oklab { s0.L, s0.a, s0.b }) >= 8;
            if (!rose && !turned) {
                continue;
            }
            ++changed;
            now.Add(s1, top1 / 255.0);
            // A dark light before: the gesture is compared with black, which is what was there.
            then.Add(top0 <= LIT ? Swatch() : s0, 1.0);
        }
        if (changed == 0) {
            // Nothing changed on a lit figure: the gesture's light is the same as what was
            // there, which is exactly what is worth saying.
            for (size_t i = 0; i + 2 < pr.during.size(); i += 3) {
                const int top1 = std::max({ pr.during[i], pr.during[i + 1], pr.during[i + 2] });
                if (top1 > LIT) {
                    ++changed;
                    const Swatch& s1 = Look(pr.during[i], pr.during[i + 1], pr.during[i + 2]);
                    now.Add(s1, top1 / 255.0);
                    then.Add(Look(pr.before[i], pr.before[i + 1], pr.before[i + 2]), 1.0);
                }
            }
            if (changed == 0) {
                continue;
            }
        }
        GestureColorCheck gc;
        gc.gesture = score->gestures[pr.gesture].id;
        gc.figure = figures[pr.figure].name;
        gc.atMS = pr.atMS;
        gc.lights = changed;
        gc.againstBefore = std::round(DeltaE(now.Mean(), then.Mean()) * 10.0) / 10.0;
        gc.gestureHex = HexOf(now.Mean());
        gc.beforeHex = HexOf(then.Mean());
        if (pr.house.w > 0) {
            gc.againstHouse = std::round(DeltaE(now.Mean(), pr.house.Mean()) * 10.0) / 10.0;
            gc.houseHex = HexOf(pr.house.Mean());
            houseContrasts.push_back(gc.againstHouse);
        }
        contrasts.push_back(gc.againstBefore);
        check.gesturesFaint += gc.againstBefore < FAINT ? 1 : 0;
        check.gestures.push_back(std::move(gc));
    }
    const auto median = [](std::vector<double> v) {
        if (v.empty()) {
            return 0.0;
        }
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    };
    check.gestureContrast = median(contrasts);
    check.gestureHouseContrast = median(houseContrasts);

    // What a person should look at, with why.
    if (check.litFrames > 0 && check.monotoneShare >= 0.5) {
        check.warnings.push_back(fmt::format("house-in-one-colour: for {:.0f}% of the lit time one family of colour ({} most often) held at least {:.0f}% of the house's light", check.monotoneShare * 100,
                                             check.dominant, MONOTONE_SHARE * 100));
    }
    if (check.longestMonotoneMS >= 30000) {
        check.warnings.push_back(fmt::format("long-stretch-in-one-colour: {:.0f} s in {} from {:.1f} s without the house changing its main colour", check.longestMonotoneMS / 1000.0,
                                             check.longestMonotoneFamily, check.longestMonotoneStartMS / 1000.0));
    }
    if (check.scenes.size() >= 4 && check.sceneDominants <= 1) {
        check.warnings.push_back(fmt::format("main-colour-never-changes: every scene's main colour is {}", check.dominant));
    }
    if (check.scenes.size() >= 4 && check.sceneVariation < 0.15) {
        check.warnings.push_back(fmt::format("scenes-repeat-their-colours: from one scene to the next the colour mix changes {:.0f}% on average", check.sceneVariation * 100));
    }
    if (check.litFrames > 0 && check.figureDifference < 10) {
        check.warnings.push_back(fmt::format("figures-alike: the lit figures differ by {:.1f} ΔE on average, about the same colour", check.figureDifference));
    }
    if (check.litFrames > 0 && check.accentFrames < 0.1) {
        check.warnings.push_back(fmt::format("few-accents: a minority colour appears in {:.0f}% of the lit time", check.accentFrames * 100));
    }
    if (check.overloadedScenes > 0) {
        check.warnings.push_back(fmt::format("too-many-colours-at-once: in {} scenes more than four families of colour share the house at once, on average", check.overloadedScenes));
    }
    if (check.litFrames > 0 && check.minorLight > 0.35) {
        check.warnings.push_back(fmt::format("colours-scattered: {:.0f}% of the light is outside the two main families of colour", check.minorLight * 100));
    }
    if (check.litFrames > 0 && check.colorsPerFigure > 2.2) {
        check.warnings.push_back(fmt::format("figures-in-many-colours: a lit figure wears {:.1f} families of colour at once on average", check.colorsPerFigure));
    }
    if (!contrasts.empty() && check.gestureContrast < FAINT) {
        check.warnings.push_back(fmt::format("gestures-blend-in: {} of {} gesture parts changed their lights by under {:.0f} ΔE (median {:.1f})", check.gesturesFaint, contrasts.size(), FAINT,
                                             check.gestureContrast));
    }
    check.ran = true;
    return check;
}

std::string ToJson(const ColorCheck& c) {
    using json = nlohmann::json;
    const auto shares = [](const std::vector<ColorShare>& v) {
        json a = json::array();
        for (const ColorShare& s : v) {
            a.push_back({ { "family", s.family }, { "share", s.share } });
        }
        return a;
    };
    json scenes = json::array();
    for (const SceneColor& s : c.scenes) {
        scenes.push_back({ { "id", s.id }, { "mode", s.mode }, { "startMS", s.startMS }, { "endMS", s.endMS }, { "dominant", s.dominant }, { "dominantShare", s.dominantShare },
                           { "monotoneShare", s.monotoneShare }, { "entropy", s.entropy }, { "effectiveColors", s.effectiveColors }, { "figureDifference", s.figureDifference },
                           { "figureFamilies", s.figureFamilies }, { "accentFrames", s.accentFrames }, { "familiesAtOnce", s.familiesAtOnce },
                           { "topTwoShare", s.topTwoShare }, { "minorLight", s.minorLight }, { "hueSpread", s.hueSpread }, { "colorsPerFigure", s.colorsPerFigure }, { "chroma", s.chroma }, { "lightness", s.lightness }, { "meanHex", s.meanHex },
                           { "intended", s.intended }, { "families", shares(s.families) } });
    }
    json figures = json::array();
    for (const FigureColorCheck& f : c.figureChecks) {
        figures.push_back({ { "name", f.name }, { "dominant", f.dominant }, { "dominantShare", f.dominantShare }, { "litShare", f.litShare }, { "sceneDominants", f.sceneDominants },
                            { "families", shares(f.families) } });
    }
    json gestures = json::array();
    for (const GestureColorCheck& g : c.gestures) {
        gestures.push_back({ { "gesture", g.gesture }, { "figure", g.figure }, { "atMS", g.atMS }, { "againstBefore", g.againstBefore }, { "againstHouse", g.againstHouse },
                             { "gestureHex", g.gestureHex }, { "beforeHex", g.beforeHex }, { "houseHex", g.houseHex }, { "lights", g.lights } });
    }
    return json({ { "kind", "composer-color-check" }, { "schemaVersion", c.schemaVersion }, { "ran", c.ran }, { "problem", c.problem }, { "frames", c.frames }, { "frameMS", c.frameMS },
                  { "figures", c.figures }, { "lights", c.lights }, { "litFrames", c.litFrames }, { "dominant", c.dominant }, { "dominantShare", c.dominantShare }, { "entropy", c.entropy },
                  { "effectiveColors", c.effectiveColors }, { "monotoneShare", c.monotoneShare }, { "monotoneThreshold", MONOTONE_SHARE }, { "longestMonotoneMS", c.longestMonotoneMS },
                  { "longestMonotoneStartMS", c.longestMonotoneStartMS }, { "longestMonotoneFamily", c.longestMonotoneFamily }, { "sceneDominants", c.sceneDominants },
                  { "sceneVariation", c.sceneVariation }, { "figureDifference", c.figureDifference }, { "figureFamilies", c.figureFamilies }, { "accentFrames", c.accentFrames },
                  { "familiesAtOnce", c.familiesAtOnce }, { "topTwoShare", c.topTwoShare }, { "minorLight", c.minorLight }, { "hueSpread", c.hueSpread },
                  { "colorsPerFigure", c.colorsPerFigure }, { "overloadedScenes", c.overloadedScenes },
                  { "gestureContrast", c.gestureContrast }, { "gestureHouseContrast", c.gestureHouseContrast }, { "gesturesFaint", c.gesturesFaint }, { "families", shares(c.families) },
                  { "scenes", scenes }, { "figureColors", figures }, { "gestures", gestures }, { "warnings", c.warnings }, { "review", "not-reviewed" } })
        .dump(1);
}

std::string ColorCheckReport(const ColorCheck& c) {
    if (!c.ran) {
        return "colour check not run: " + c.problem + "\n";
    }
    std::string out = fmt::format("{} figures, {} lights, {} frames of {} ms ({} lit)\n", c.figures, c.lights, c.frames, c.frameMS, c.litFrames);
    std::string mix;
    for (size_t i = 0; i < c.families.size() && i < 6; ++i) {
        mix += fmt::format("{}{} {:.0f}%", mix.empty() ? "" : ", ", c.families[i].family, c.families[i].share * 100);
    }
    out += fmt::format("colour: {}; about {:.1f} families share the light (entropy {:.2f})\n", mix, c.effectiveColors, c.entropy);
    out += fmt::format("one colour: {:.0f}% of the lit time with one family over {:.0f}% of the house; longest {:.1f} s ({}, from {:.1f} s)\n", c.monotoneShare * 100, MONOTONE_SHARE * 100,
                       c.longestMonotoneMS / 1000.0, c.longestMonotoneFamily.empty() ? "-" : c.longestMonotoneFamily, c.longestMonotoneStartMS / 1000.0);
    out += fmt::format("scenes: {} main colours over {} scenes; the mix changes {:.0f}% from one to the next\n", c.sceneDominants, c.scenes.size(), c.sceneVariation * 100);
    out += fmt::format("figures: differ by {:.1f} ΔE, wear {:.1f} families at once; a minority accent in {:.0f}% of the time\n", c.figureDifference, c.figureFamilies, c.accentFrames * 100);
    out += fmt::format("cohesion: {:.1f} families over 5% of the house at once, the two largest hold {:.0f}% of the light, hue spread {:.2f}, {:.1f} colours inside a lit figure; {} scenes over four families\n",
                       c.familiesAtOnce, c.topTwoShare * 100, c.hueSpread, c.colorsPerFigure, c.overloadedScenes);
    if (!c.gestures.empty()) {
        out += fmt::format("gestures: {} parts measured, median {:.1f} ΔE against what was lit before and {:.1f} against the rest of the house; {} under 15\n", c.gestures.size(),
                           c.gestureContrast, c.gestureHouseContrast, c.gesturesFaint);
    }
    for (const SceneColor& s : c.scenes) {
        if (s.dominant.empty()) {
            continue;
        }
        out += fmt::format("  {:>7.1f}-{:<7.1f} s {:<14} {:<8} {:3.0f}%  one colour {:3.0f}%  ~{:.1f} families  figures {:4.1f} ΔE  {}{}\n", s.startMS / 1000.0, s.endMS / 1000.0, s.mode, s.dominant,
                           s.dominantShare * 100, s.monotoneShare * 100, s.effectiveColors, s.figureDifference, s.meanHex, s.intended ? "  (one colour on purpose)" : "");
    }
    for (const std::string& w : c.warnings) {
        out += "! " + w + "\n";
    }
    out += "These are measurements of the render, not a judgement of the show.\n";
    return out;
}

} // namespace composer
