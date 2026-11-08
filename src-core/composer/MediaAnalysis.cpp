#include "composer/MediaAnalysis.h"

#include "composer/VisualModel.h"
#include "media/VideoFrame.h"
#include "media/VideoReader.h"
#include "utils/xlImage.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <map>
#include <set>

using nlohmann::json;

namespace composer {

namespace {

double R3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string Hex(double r, double g, double b) {
    const auto c = [](double v) { return static_cast<int>(std::clamp(std::lround(v), 0L, 255L)); };
    return fmt::format("#{:02x}{:02x}{:02x}", c(r), c(g), c(b));
}

double Luma(double r, double g, double b) {
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
}

// Hue in degrees and saturation (HSV) of an RGB colour.
void HueSat(double r, double g, double b, double& hue, double& sat) {
    const double mx = std::max({ r, g, b });
    const double mn = std::min({ r, g, b });
    const double d = mx - mn;
    sat = mx > 0 ? d / mx : 0;
    if (d <= 0) {
        hue = 0;
        return;
    }
    if (mx == r) {
        hue = 60.0 * std::fmod((g - b) / d, 6.0);
    } else if (mx == g) {
        hue = 60.0 * ((b - r) / d + 2.0);
    } else {
        hue = 60.0 * ((r - g) / d + 4.0);
    }
    if (hue < 0) {
        hue += 360.0;
    }
}

// A picture's pixels as linear arrays, over black where transparent.
struct Pixels {
    int w = 0;
    int h = 0;
    std::vector<double> r, g, b, y;

    explicit Pixels(const xlImage& img) : w(img.GetWidth()), h(img.GetHeight()) {
        const size_t n = static_cast<size_t>(w) * h;
        r.resize(n);
        g.resize(n);
        b.resize(n);
        y.resize(n);
        const uint8_t* d = img.GetData();
        for (size_t i = 0; i < n; ++i) {
            const double a = d[i * 4 + 3] / 255.0;
            r[i] = d[i * 4] * a;
            g[i] = d[i * 4 + 1] * a;
            b[i] = d[i * 4 + 2] * a;
            y[i] = Luma(r[i], g[i], b[i]);
        }
    }
};

xlImage Fit(const xlImage& img, int maxSide) {
    const int w = img.GetWidth();
    const int h = img.GetHeight();
    if (w <= maxSide && h <= maxSide) {
        return img;
    }
    const double s = static_cast<double>(maxSide) / std::max(w, h);
    return ResampleImage(img, std::max(1, static_cast<int>(std::lround(w * s))), std::max(1, static_cast<int>(std::lround(h * s))));
}

uint64_t DHash(const xlImage& img) {
    const xlImage s = ResampleImage(img, 9, 8);
    const Pixels p(s);
    uint64_t bits = 0;
    for (int yy = 0; yy < 8; ++yy) {
        for (int x = 0; x < 8; ++x) {
            bits = (bits << 1) | (p.y[yy * 9 + x] < p.y[yy * 9 + x + 1] ? 1u : 0u);
        }
    }
    return bits;
}

int Hamming(uint64_t a, uint64_t b) {
    return std::popcount(a ^ b);
}

// Dominant colours by k-means with a fixed start (luma quantiles), so the result does
// not depend on chance.
std::vector<MediaColor> Dominant(const Pixels& p) {
    const size_t n = p.y.size();
    if (n == 0) {
        return {};
    }
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return p.y[a] < p.y[b]; });
    constexpr int K = 5;
    std::array<std::array<double, 3>, K> c{};
    for (int k = 0; k < K; ++k) {
        const size_t i = order[std::min(n - 1, static_cast<size_t>((k + 0.5) / K * n))];
        c[k] = { p.r[i], p.g[i], p.b[i] };
    }
    std::vector<int> label(n, 0);
    for (int iter = 0; iter < 10; ++iter) {
        std::array<std::array<double, 4>, K> sum{};
        for (size_t i = 0; i < n; ++i) {
            int best = 0;
            double bd = 1e18;
            for (int k = 0; k < K; ++k) {
                const double d = (p.r[i] - c[k][0]) * (p.r[i] - c[k][0]) + (p.g[i] - c[k][1]) * (p.g[i] - c[k][1]) + (p.b[i] - c[k][2]) * (p.b[i] - c[k][2]);
                if (d < bd) {
                    bd = d;
                    best = k;
                }
            }
            label[i] = best;
            sum[best][0] += p.r[i];
            sum[best][1] += p.g[i];
            sum[best][2] += p.b[i];
            sum[best][3] += 1;
        }
        for (int k = 0; k < K; ++k) {
            if (sum[k][3] > 0) {
                c[k] = { sum[k][0] / sum[k][3], sum[k][1] / sum[k][3], sum[k][2] / sum[k][3] };
            }
        }
    }
    std::array<double, K> count{};
    for (int l : label) {
        count[l] += 1;
    }
    // Colours closer than a small step are one colour.
    for (int a = 0; a < K; ++a) {
        for (int b = a + 1; b < K; ++b) {
            if (count[a] <= 0 || count[b] <= 0) {
                continue;
            }
            const double d = std::sqrt(std::pow(c[a][0] - c[b][0], 2) + std::pow(c[a][1] - c[b][1], 2) + std::pow(c[a][2] - c[b][2], 2));
            if (d < 28) {
                const double t = count[a] + count[b];
                for (int i = 0; i < 3; ++i) {
                    c[a][i] = (c[a][i] * count[a] + c[b][i] * count[b]) / t;
                }
                count[a] = t;
                count[b] = 0;
            }
        }
    }
    std::vector<MediaColor> out;
    for (int k = 0; k < K; ++k) {
        if (count[k] / n >= 0.02) {
            out.push_back({ Hex(c[k][0], c[k][1], c[k][2]), R3(count[k] / n) });
        }
    }
    std::sort(out.begin(), out.end(), [](const MediaColor& a, const MediaColor& b) { return a.share != b.share ? a.share > b.share : a.hex < b.hex; });
    return out;
}

void HexRGB(const std::string& hex, double& r, double& g, double& b) {
    const long v = hex.size() == 7 ? std::strtol(hex.c_str() + 1, nullptr, 16) : 0;
    r = (v >> 16) & 255;
    g = (v >> 8) & 255;
    b = v & 255;
}

std::string OrientationOf(double aspect) {
    return aspect >= 2.0 ? "panoramic" : aspect >= 1.2 ? "landscape" : aspect > 0.83 ? "square" : aspect > 0.5 ? "portrait" : "tall";
}

bool Has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

void AddIssue(MediaAnalysis& a, const std::string& issue) {
    if (!Has(a.issues, issue)) {
        a.issues.push_back(issue);
    }
}

// What a few colour rules suggest without a model; always "weak".
void RuleConcepts(MediaAnalysis& a, const VisualConcepts* concepts) {
    std::vector<std::pair<std::string, double>> found;
    double warm = 0, green = 0, blue = 0;
    for (const MediaColor& c : a.colors) {
        double r, g, b, hue, sat;
        HexRGB(c.hex, r, g, b);
        HueSat(r, g, b, hue, sat);
        if (sat < 0.35 || Luma(r, g, b) < 0.12) {
            continue;
        }
        if (hue < 50 || hue >= 340) {
            warm += c.share;
        } else if (hue >= 70 && hue < 170) {
            green += c.share;
        } else if (hue >= 185 && hue < 260) {
            blue += c.share;
        }
    }
    if (a.dark >= 0.45 && a.brightness < 0.25 && a.highlights >= 0.001 && a.highlights < 0.2 && a.complexity < 0.35 && a.colorfulness < 0.35) {
        found.emplace_back("stars", 0.3);
    }
    if (a.white >= 0.25 && a.saturation < 0.25 && a.contrast >= 0.03) {
        found.emplace_back("snowy-landscape", 0.3);
    }
    if (warm >= 0.45 && a.saturation >= 0.5 && a.contrast >= 0.25) {
        found.emplace_back("fire", 0.25 + 0.2 * std::min(1.0, warm));
    }
    if (green >= 0.35) {
        found.emplace_back("forest", 0.25);
    }
    if (blue >= 0.4 && a.brightness >= 0.35) {
        found.emplace_back("clouds", 0.2);
    }
    if (a.colorfulness >= 0.5 && a.saturation >= 0.35 && a.dark >= 0.3 && a.complexity < 0.3) {
        found.emplace_back("bokeh", 0.25);
    } else if (a.colorfulness >= 0.5 && a.saturation >= 0.35) {
        found.emplace_back("abstract", 0.25);
    }
    if (a.complexity < 0.02 && a.contrast >= 0.05 && a.contrast < 0.35 && a.colorfulness < 0.3) {
        found.emplace_back("gradient", 0.25);
    }
    std::sort(found.begin(), found.end(), [](const auto& x, const auto& y) { return x.second != y.second ? x.second > y.second : x.first < y.first; });
    for (const auto& [id, score] : found) {
        if (concepts == nullptr || concepts->Find(id) != nullptr) {
            a.concepts.push_back({ id, R3(score), 0, "weak", "rules" });
        }
    }
}

// Generic suitability for each kind of figure; the planner refines it with the real
// figure's shape.
void Suitability(MediaAnalysis& a, double aspect, int width, int height, MediaKind kind, const VisualConcepts* concepts) {
    double m = 0.5, t = 0.5;
    std::vector<std::string>& why = a.fitReasons;
    why.clear();
    if (a.orientation == "panoramic" || a.orientation == "landscape") {
        m += 0.2;
        why.push_back("matrix:+shape");
    } else if (a.orientation == "portrait" || a.orientation == "tall") {
        m -= 0.15;
        why.push_back("matrix:-shape");
    }
    if (a.orientation == "portrait" || a.orientation == "tall") {
        t += 0.2;
        why.push_back("tree:+shape");
    } else if (a.orientation == "panoramic") {
        t += 0.1;
        why.push_back("tree:+wraps");
    }
    if (std::min(width, height) < 48) {
        m -= 0.25;
        t -= 0.15;
        why.push_back("both:-small");
    }
    if (a.complexity > 0.45) {
        m -= 0.1;
        t -= 0.25;
        why.push_back("both:-detail");
    } else if (a.complexity < 0.2) {
        t += 0.1;
        why.push_back("tree:+bold");
    }
    if (a.contrast < 0.12) {
        m -= 0.1;
        t -= 0.1;
        why.push_back("both:-flat");
    }
    if (a.saturation >= 0.45) {
        m += 0.05;
        t += 0.1;
        why.push_back("both:+colour");
    }
    if (a.brightness < 0.08 && a.highlights < 0.005) {
        m -= 0.35;
        t -= 0.35;
        why.push_back("both:-dark");
    }
    if (a.white >= 0.75) {
        m -= 0.25;
        t -= 0.25;
        why.push_back("both:-white");
    }
    if (kind == MediaKind::Video || kind == MediaKind::Animation) {
        if (a.video.motion >= 0.08) {
            m += 0.05;
            t += 0.05;
            why.push_back("both:+moves");
        }
        if (Has(a.issues, "static-video")) {
            m -= 0.1;
            t -= 0.1;
        }
    }
    if (concepts != nullptr) {
        double wm = 0, wt = 0, w = 0;
        for (const MediaConcept& c : a.concepts) {
            const VisualConceptDef* d = concepts->Find(c.id);
            if (d == nullptr || c.level == "weak") {
                continue;
            }
            const double k = c.level == "strong" ? 1.0 : 0.5;
            wm += d->matrix * k;
            wt += d->tree * k;
            w += k;
        }
        if (w > 0) {
            m += 0.2 * (wm / w - 0.5);
            t += 0.2 * (wt / w - 0.5);
            why.push_back("both:content");
        }
    }
    (void)aspect;
    a.matrixFit = R3(std::clamp(m, 0.0, 1.0));
    a.treeFit = R3(std::clamp(t, 0.0, 1.0));
}

void Describe(MediaAnalysis& a, const VisualConcepts* concepts) {
    a.description.clear();
    a.categories.clear();
    std::set<std::string> groups;
    for (const MediaConcept& c : a.concepts) {
        if (a.description.size() >= 3) {
            break;
        }
        const VisualConceptDef* d = concepts != nullptr ? concepts->Find(c.id) : nullptr;
        const std::string group = d != nullptr ? d->group : "subject";
        // One concept of each kind reads better than three ways of saying the same.
        if (c.level == "weak" && !a.description.empty()) {
            continue;
        }
        if (groups.insert(group).second || c.level == "strong") {
            a.description.push_back(c.id);
        }
    }
    for (const MediaConcept& c : a.concepts) {
        if (c.level == "weak" && c.source == "model") {
            continue;
        }
        if (const VisualConceptDef* d = concepts != nullptr ? concepts->Find(c.id) : nullptr; d != nullptr) {
            for (const std::string& cat : d->categories) {
                if (!Has(a.categories, cat)) {
                    a.categories.push_back(cat);
                }
            }
        }
    }
}

xlImage FrameToImage(const VideoFrame& f) {
    if (f.data == nullptr || f.width <= 0 || f.height <= 0 || f.format == VideoPixelFormat::PlatformNative) {
        return {};
    }
    const bool four = f.format == VideoPixelFormat::RGBA || f.format == VideoPixelFormat::BGRA;
    const bool bgr = f.format == VideoPixelFormat::BGR24 || f.format == VideoPixelFormat::BGRA;
    xlImage img(f.width, f.height);
    for (int y = 0; y < f.height; ++y) {
        const uint8_t* row = f.data + static_cast<size_t>(y) * f.linesize;
        for (int x = 0; x < f.width; ++x) {
            const uint8_t* px = row + x * (four ? 4 : 3);
            img.SetRGBA(x, y, bgr ? px[2] : px[0], px[1], bgr ? px[0] : px[2], 255);
        }
    }
    return img;
}

struct Sample {
    int ms = 0;
    double brightness = 0;
    std::array<double, 64> hist{};
    std::vector<double> luma;
    xlImage picture;
};

Sample Measure(int ms, xlImage picture) {
    Sample s;
    s.ms = ms;
    const Pixels p(picture);
    s.luma = p.y;
    double sum = 0;
    for (size_t i = 0; i < p.y.size(); ++i) {
        sum += p.y[i];
        const int bin = (static_cast<int>(p.r[i]) >> 6) * 16 + (static_cast<int>(p.g[i]) >> 6) * 4 + (static_cast<int>(p.b[i]) >> 6);
        s.hist[static_cast<size_t>(std::clamp(bin, 0, 63))] += 1;
    }
    const double n = std::max<size_t>(1, p.y.size());
    s.brightness = sum / n;
    for (double& h : s.hist) {
        h /= n;
    }
    s.picture = std::move(picture);
    return s;
}

double LumaDiff(const Sample& a, const Sample& b) {
    if (a.luma.size() != b.luma.size() || a.luma.empty()) {
        return 0;
    }
    double d = 0;
    for (size_t i = 0; i < a.luma.size(); ++i) {
        d += std::fabs(a.luma[i] - b.luma[i]);
    }
    return d / a.luma.size();
}

double HistDiff(const Sample& a, const Sample& b) {
    double d = 0;
    for (size_t i = 0; i < a.hist.size(); ++i) {
        d += std::fabs(a.hist[i] - b.hist[i]);
    }
    return d;
}

// How a sequence of frames moves, cuts, darkens and loops; scenes between cuts.
void Temporal(MediaAnalysis& a, const std::vector<Sample>& samples, int lengthMS) {
    MediaVideoAnalysis& v = a.video;
    v = MediaVideoAnalysis();
    v.samples = static_cast<int>(samples.size());
    if (samples.empty()) {
        return;
    }
    v.sampledMS = samples.back().ms;
    std::vector<double> motion(samples.size(), 0);
    std::vector<size_t> cutsAt;
    for (size_t i = 1; i < samples.size(); ++i) {
        const double dt = std::max(1, samples[i].ms - samples[i - 1].ms);
        const double d = LumaDiff(samples[i], samples[i - 1]);
        const double h = HistDiff(samples[i], samples[i - 1]);
        if (h > 0.6 && d > 0.08) {
            cutsAt.push_back(i);
            motion[i] = motion[i - 1]; // a cut is not motion
        } else {
            motion[i] = std::clamp(d * (250.0 / dt) / 0.06, 0.0, 1.0);
        }
    }
    if (samples.size() > 1) {
        motion[0] = motion[1];
    }
    double msum = 0, bsum = 0, bsq = 0;
    for (size_t i = 0; i < samples.size(); ++i) {
        msum += motion[i];
        bsum += samples[i].brightness;
        bsq += samples[i].brightness * samples[i].brightness;
    }
    const double n = static_cast<double>(samples.size());
    v.motion = R3(msum / n);
    v.cuts = static_cast<int>(cutsAt.size());
    const double bmean = bsum / n;
    const double bstd = std::sqrt(std::max(0.0, bsq / n - bmean * bmean));
    // Scenes: between cuts, a scene shorter than a second joins the one before.
    std::vector<size_t> starts = { 0 };
    for (size_t c : cutsAt) {
        if (samples[c].ms - samples[starts.back()].ms >= 1000) {
            starts.push_back(c);
        }
    }
    for (size_t k = 0; k < starts.size(); ++k) {
        const size_t from = starts[k];
        const size_t to = k + 1 < starts.size() ? starts[k + 1] : samples.size();
        MediaScene sc;
        sc.startMS = samples[from].ms;
        sc.endMS = to < samples.size() ? samples[to].ms : std::max(lengthMS, samples.back().ms);
        double sm = 0, sb = 0;
        for (size_t i = from; i < to; ++i) {
            sm += motion[i];
            sb += samples[i].brightness;
        }
        const double cnt = std::max<size_t>(1, to - from);
        sc.motion = R3(sm / cnt);
        sc.brightness = R3(sb / cnt);
        const Sample& mid = samples[from + (to - from) / 2];
        sc.dhash = DHash(mid.picture);
        const std::vector<MediaColor> dom = Dominant(Pixels(Fit(mid.picture, 32)));
        sc.color = dom.empty() ? std::string("#000000") : dom.front().hex;
        sc.energy = R3(std::clamp(0.7 * sc.motion + 0.3 * sc.brightness, 0.0, 1.0));
        v.scenes.push_back(std::move(sc));
    }
    // Dark stretches of three quarters of a second or more.
    for (size_t i = 0; i < samples.size();) {
        if (samples[i].brightness >= 0.06) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < samples.size() && samples[j].brightness < 0.06) {
            ++j;
        }
        const int from = samples[i].ms;
        const int to = j < samples.size() ? samples[j].ms : std::max(lengthMS, samples.back().ms);
        if (to - from >= 750) {
            v.darkSpans.emplace_back(from, to);
        }
        i = j;
    }
    if (samples.size() >= 2) {
        const double d = LumaDiff(samples.front(), samples.back());
        const double h = HistDiff(samples.front(), samples.back());
        v.loop = R3(std::clamp(1.0 - d * 6.0 - h * 0.5, 0.0, 1.0));
    }
    const double minutes = std::max(1.0, static_cast<double>(lengthMS)) / 60000.0;
    v.energy = R3(std::clamp(0.6 * v.motion + 0.2 * std::min(1.0, v.cuts / minutes / 20.0) + 0.2 * std::min(1.0, bstd * 4.0), 0.0, 1.0));
    v.shortUse = lengthMS >= 1500 && (v.motion >= 0.1 || v.cuts > 0);
    v.longUse = lengthMS >= 8000 || v.loop >= 0.7;
    int dark = 0;
    for (const auto& [f, t] : v.darkSpans) {
        dark += t - f;
    }
    if (lengthMS > 0 && dark > lengthMS / 4) {
        AddIssue(a, "dark-spans");
    }
    if (v.motion < 0.01 && v.cuts == 0 && samples.size() >= 4) {
        AddIssue(a, "static-video");
    }
}

void MergeConcepts(std::vector<MediaConcept>& into, const std::vector<MediaConcept>& from) {
    const auto rank = [](const std::string& l) { return l == "strong" ? 3 : l == "probable" ? 2 : 1; };
    for (const MediaConcept& c : from) {
        auto it = std::find_if(into.begin(), into.end(), [&](const MediaConcept& x) { return x.id == c.id; });
        if (it == into.end()) {
            into.push_back(c);
        } else {
            if (c.score > it->score) {
                it->score = c.score;
                it->similarity = c.similarity;
            }
            if (rank(c.level) > rank(it->level)) {
                it->level = c.level;
            }
        }
    }
    std::sort(into.begin(), into.end(), [&](const MediaConcept& a, const MediaConcept& b) {
        if (rank(a.level) != rank(b.level)) {
            return rank(a.level) > rank(b.level);
        }
        return a.similarity != b.similarity ? a.similarity > b.similarity : a.score != b.score ? a.score > b.score : a.id < b.id;
    });
}

bool Semantic(const MediaAnalysisOptions& o, const xlImage& picture, std::vector<MediaConcept>& out, std::string& error) {
    if (o.model == nullptr || o.index == nullptr) {
        return false;
    }
    std::vector<float> e;
    if (!o.model->EmbedImage(picture, e, error)) {
        return false;
    }
    out = ScoreConcepts(*o.model, *o.index, e);
    return true;
}

std::string ParamsKey(const MediaAnalysisOptions& o, bool withModel) {
    return fmt::format("{:08x}", Fnv(fmt::format("{}/{}/{}", o.videoSampleMS, o.maxVideoSamples, withModel && o.concepts != nullptr ? o.concepts->hash : std::string())) & 0xffffffffull);
}

std::string FilePart(const MediaItem& item) {
    return fmt::format("{}:{}", item.hash, item.bytes);
}

} // namespace

xlImage ResampleImage(const xlImage& src, int width, int height) {
    width = std::max(1, width);
    height = std::max(1, height);
    xlImage out(width, height);
    const int sw = src.GetWidth();
    const int sh = src.GetHeight();
    if (!src.IsOk() || sw <= 0 || sh <= 0) {
        return out;
    }
    const uint8_t* s = src.GetData();
    uint8_t* d = out.GetData();
    const double fx = static_cast<double>(sw) / width;
    const double fy = static_cast<double>(sh) / height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            double acc[4] = { 0, 0, 0, 0 };
            if (fx >= 1.0 && fy >= 1.0) {
                // Area average: every source pixel the target pixel covers, weighted by how much.
                const double x0 = x * fx, x1 = (x + 1) * fx, y0 = y * fy, y1 = (y + 1) * fy;
                double wsum = 0;
                for (int sy = static_cast<int>(y0); sy < std::min(sh, static_cast<int>(std::ceil(y1))); ++sy) {
                    const double wy = std::min(y1, sy + 1.0) - std::max(y0, static_cast<double>(sy));
                    for (int sx = static_cast<int>(x0); sx < std::min(sw, static_cast<int>(std::ceil(x1))); ++sx) {
                        const double w = wy * (std::min(x1, sx + 1.0) - std::max(x0, static_cast<double>(sx)));
                        const uint8_t* p = s + (static_cast<size_t>(sy) * sw + sx) * 4;
                        for (int c = 0; c < 4; ++c) {
                            acc[c] += p[c] * w;
                        }
                        wsum += w;
                    }
                }
                for (double& v : acc) {
                    v /= std::max(1e-9, wsum);
                }
            } else {
                const double gx = std::clamp((x + 0.5) * fx - 0.5, 0.0, sw - 1.0);
                const double gy = std::clamp((y + 0.5) * fy - 0.5, 0.0, sh - 1.0);
                const int ix = static_cast<int>(gx), iy = static_cast<int>(gy);
                const int jx = std::min(sw - 1, ix + 1), jy = std::min(sh - 1, iy + 1);
                const double tx = gx - ix, ty = gy - iy;
                for (int c = 0; c < 4; ++c) {
                    const double a = s[(static_cast<size_t>(iy) * sw + ix) * 4 + c] * (1 - tx) + s[(static_cast<size_t>(iy) * sw + jx) * 4 + c] * tx;
                    const double b = s[(static_cast<size_t>(jy) * sw + ix) * 4 + c] * (1 - tx) + s[(static_cast<size_t>(jy) * sw + jx) * 4 + c] * tx;
                    acc[c] = a * (1 - ty) + b * ty;
                }
            }
            uint8_t* q = d + (static_cast<size_t>(y) * width + x) * 4;
            for (int c = 0; c < 4; ++c) {
                q[c] = static_cast<uint8_t>(std::clamp(std::lround(acc[c]), 0L, 255L));
            }
        }
    }
    return out;
}

void AnalyzePicture(const xlImage& picture, MediaAnalysis& a) {
    const xlImage reduced = Fit(picture, 128);
    const Pixels p(reduced);
    const size_t n = p.y.size();
    if (n == 0) {
        return;
    }
    double ysum = 0, ysq = 0, ssum = 0, dark = 0, white = 0, lit = 0;
    double rgm = 0, ybm = 0, rgs = 0, ybs = 0;
    for (size_t i = 0; i < n; ++i) {
        double hue, sat;
        HueSat(p.r[i], p.g[i], p.b[i], hue, sat);
        ysum += p.y[i];
        ysq += p.y[i] * p.y[i];
        ssum += sat;
        dark += p.y[i] < 0.06 ? 1 : 0;
        white += p.y[i] > 0.9 && sat < 0.12 ? 1 : 0;
        lit += p.y[i] > 0.18 ? 1 : 0;
        const double rg = p.r[i] - p.g[i];
        const double yb = 0.5 * (p.r[i] + p.g[i]) - p.b[i];
        rgm += rg;
        ybm += yb;
        rgs += rg * rg;
        ybs += yb * yb;
    }
    const double nn = static_cast<double>(n);
    const double mean = ysum / nn;
    a.brightness = R3(mean);
    a.contrast = R3(std::clamp(std::sqrt(std::max(0.0, ysq / nn - mean * mean)) * 2.5, 0.0, 1.0));
    a.saturation = R3(ssum / nn);
    a.dark = R3(dark / nn);
    a.white = R3(white / nn);
    a.highlights = R3(lit / nn);
    rgm /= nn;
    ybm /= nn;
    const double sigma = std::sqrt(std::max(0.0, rgs / nn - rgm * rgm) + std::max(0.0, ybs / nn - ybm * ybm));
    const double mu = std::sqrt(rgm * rgm + ybm * ybm);
    a.colorfulness = R3(std::clamp((sigma + 0.3 * mu) / 150.0, 0.0, 1.0));
    // Detail: the share of pixels on an edge (Sobel on the light).
    const int w = p.w, h = p.h;
    std::vector<double> edge(n, 0);
    int edges = 0;
    for (int y = 1; y + 1 < h; ++y) {
        for (int x = 1; x + 1 < w; ++x) {
            const auto Y = [&](int xx, int yy) { return p.y[static_cast<size_t>(yy) * w + xx]; };
            const double gx = (Y(x + 1, y - 1) + 2 * Y(x + 1, y) + Y(x + 1, y + 1)) - (Y(x - 1, y - 1) + 2 * Y(x - 1, y) + Y(x - 1, y + 1));
            const double gy = (Y(x - 1, y + 1) + 2 * Y(x, y + 1) + Y(x + 1, y + 1)) - (Y(x - 1, y - 1) + 2 * Y(x, y - 1) + Y(x + 1, y - 1));
            const double m = std::sqrt(gx * gx + gy * gy) / 4.0;
            edge[static_cast<size_t>(y) * w + x] = m;
            edges += m > 0.12 ? 1 : 0;
        }
    }
    a.complexity = R3(std::clamp(edges / std::max(1.0, (w - 2.0) * (h - 2.0)) * 2.0, 0.0, 1.0));
    a.colors = Dominant(Pixels(Fit(reduced, 64)));
    // Point of interest: what differs most from the picture's average colour, or is
    // full of edges, with a slight pull to the centre where subjects usually are.
    double mr = 0, mg = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) {
        mr += p.r[i];
        mg += p.g[i];
        mb += p.b[i];
    }
    mr /= nn;
    mg /= nn;
    mb /= nn;
    std::vector<double> sal(n);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = static_cast<size_t>(y) * w + x;
            const double cd = std::sqrt(std::pow(p.r[i] - mr, 2) + std::pow(p.g[i] - mg, 2) + std::pow(p.b[i] - mb, 2)) / 441.7;
            const double dx = (x + 0.5) / w - 0.5, dy = (y + 0.5) / h - 0.5;
            sal[i] = (0.65 * cd + 0.35 * edge[i]) * (1.0 - 0.6 * std::sqrt(dx * dx + dy * dy));
        }
    }
    std::vector<double> sorted = sal;
    std::sort(sorted.begin(), sorted.end());
    const double cut = sorted[static_cast<size_t>(0.9 * (n - 1))];
    double sx = 0, sy = 0, sw = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const double v = sal[static_cast<size_t>(y) * w + x];
            if (v >= cut && v > 0) {
                sx += (x + 0.5) * v;
                sy += (y + 0.5) * v;
                sw += v;
            }
        }
    }
    a.poiX = sw > 0 ? R3(sx / sw / w) : 0.5;
    a.poiY = sw > 0 ? R3(sy / sw / h) : 0.5;
    const double ox = a.poiX - 0.5, oy = a.poiY - 0.5;
    a.subject = std::max(std::fabs(ox), std::fabs(oy)) < 0.12 ? "center" : std::fabs(ox) >= std::fabs(oy) ? (ox < 0 ? "left" : "right") : (oy < 0 ? "top" : "bottom");
    a.dhash = DHash(picture);
}

std::string MediaAnalysisKey(const MediaItem& item, const std::string& modelId, const MediaAnalysisOptions& o) {
    return fmt::format("{}|{}|{}|{}", FilePart(item), MEDIA_ANALYZER_VERSION, modelId.empty() ? std::string("basic") : modelId, ParamsKey(o, !modelId.empty()));
}

bool AnalyzeMediaItem(MediaItem& item, const MediaAnalysisOptions& o) {
    MediaAnalysis a;
    a.analyzer = MEDIA_ANALYZER_VERSION;
    const bool withModel = o.model != nullptr && o.index != nullptr;
    a.model = withModel ? o.model->Id() : std::string();
    a.key = MediaAnalysisKey(item, a.model, o);
    if (!item.Usable()) {
        return false;
    }
    std::string error;
    std::vector<MediaConcept> concepts;
    bool semantic = false;
    if (item.kind == MediaKind::Video) {
        const auto start = std::chrono::steady_clock::now();
        int sw = 96, sh = 96;
        if (item.width > 0 && item.height > 0) {
            const double s = 96.0 / std::max(item.width, item.height);
            sw = std::max(8, static_cast<int>(std::lround(item.width * s)));
            sh = std::max(8, static_cast<int>(std::lround(item.height * s)));
        }
        std::vector<Sample> samples;
        {
            VideoReader reader(item.path, sw, sh, false, false, false);
            if (!reader.IsValid()) {
                a.error = "decode";
                AddIssue(a, "damaged");
                a.mode = "basic";
                item.analysis = std::move(a);
                return false;
            }
            const int length = std::max(1, reader.GetLengthMS());
            const int step = std::max(o.videoSampleMS, length / std::max(1, o.maxVideoSamples));
            for (int t = 0; t < length; t += step) {
                if (o.cancelled && o.cancelled()) {
                    return false;
                }
                if (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count() > o.videoTimeLimitMS) {
                    AddIssue(a, "video-partial");
                    break;
                }
                const VideoFrame* f = reader.GetNextFrame(t);
                if (f == nullptr) {
                    break;
                }
                xlImage img = FrameToImage(*f);
                if (!img.IsOk()) {
                    break;
                }
                samples.push_back(Measure(t, std::move(img)));
            }
        }
        if (samples.empty()) {
            a.error = "decode";
            AddIssue(a, "damaged");
            a.mode = "basic";
            item.analysis = std::move(a);
            return false;
        }
        Temporal(a, samples, item.durationMS);
        // The picture's measures come from the middle of its longest scene.
        size_t longest = 0;
        for (size_t i = 1; i < a.video.scenes.size(); ++i) {
            if (a.video.scenes[i].endMS - a.video.scenes[i].startMS > a.video.scenes[longest].endMS - a.video.scenes[longest].startMS) {
                longest = i;
            }
        }
        std::vector<int> keyTimes;
        for (const MediaScene& sc : a.video.scenes) {
            keyTimes.push_back((sc.startMS + sc.endMS) / 2);
        }
        // Key frames, sharper, in time order: the representative one and, with the
        // model, those of the longest scenes.
        std::vector<size_t> byLength(a.video.scenes.size());
        for (size_t i = 0; i < byLength.size(); ++i) {
            byLength[i] = i;
        }
        std::stable_sort(byLength.begin(), byLength.end(), [&](size_t x, size_t y) {
            return a.video.scenes[x].endMS - a.video.scenes[x].startMS > a.video.scenes[y].endMS - a.video.scenes[y].startMS;
        });
        std::set<size_t> wanted = { longest };
        if (withModel) {
            for (size_t i = 0; i < byLength.size() && static_cast<int>(wanted.size()) < o.maxVideoScenes; ++i) {
                wanted.insert(byLength[i]);
            }
        }
        int kw = 256, kh = 256;
        if (item.width > 0 && item.height > 0) {
            const double s = std::min(1.0, 256.0 / std::max(item.width, item.height));
            kw = std::max(8, static_cast<int>(std::lround(item.width * s)));
            kh = std::max(8, static_cast<int>(std::lround(item.height * s)));
        }
        VideoReader sharp(item.path, kw, kh, false, false, false);
        for (size_t idx : wanted) {
            xlImage key;
            if (sharp.IsValid()) {
                if (const VideoFrame* f = sharp.GetNextFrame(keyTimes[idx]); f != nullptr) {
                    key = FrameToImage(*f);
                }
            }
            if (!key.IsOk()) {
                key = samples[std::min(samples.size() - 1, static_cast<size_t>(std::lower_bound(samples.begin(), samples.end(), keyTimes[idx], [](const Sample& s, int t) { return s.ms < t; }) - samples.begin()))].picture;
            }
            if (idx == longest) {
                AnalyzePicture(key, a);
            }
            std::vector<MediaConcept> sc;
            if (withModel && Semantic(o, key, sc, error)) {
                semantic = true;
                a.video.scenes[idx].concepts = sc;
                MergeConcepts(concepts, sc);
            }
        }
        if (item.durationMS > 0 && item.durationMS < 1500) {
            AddIssue(a, "too-short");
        }
    } else {
        xlImage picture;
        if (!LoadMediaPicture(item, 512, picture)) {
            a.error = "decode";
            AddIssue(a, "damaged");
            a.mode = "basic";
            item.analysis = std::move(a);
            return false;
        }
        AnalyzePicture(picture, a);
        if (item.kind == MediaKind::Animation) {
            std::vector<xlImage> frames;
            std::vector<long> times;
            if (LoadMediaAnimation(item, 64, 120, frames, times)) {
                std::vector<Sample> samples;
                int t = 0;
                for (size_t i = 0; i < frames.size(); ++i) {
                    samples.push_back(Measure(t, frames[i]));
                    t += static_cast<int>(std::max(20L, times[i]));
                }
                Temporal(a, samples, std::max(t, item.durationMS));
            }
        }
        if (withModel && Semantic(o, picture, concepts, error)) {
            semantic = true;
        }
    }
    if (!error.empty()) {
        a.error = error;
    }
    a.orientation = OrientationOf(item.aspect);
    if (semantic) {
        a.concepts = concepts;
        a.mode = "semantic";
    } else {
        RuleConcepts(a, o.concepts);
        a.mode = "basic";
        a.model.clear();
        a.key = MediaAnalysisKey(item, "", o);
    }
    // Black is the lights being off, so a dark picture is fine while something in it
    // is lit; nearly nothing lit is a problem.
    if (a.brightness < 0.08 && a.highlights < 0.005) {
        AddIssue(a, "very-dark");
    }
    if (a.white >= 0.75 || a.brightness > 0.92) {
        AddIssue(a, "very-bright");
    }
    if (std::max(item.width, item.height) < 64 || std::min(item.width, item.height) < 24) {
        AddIssue(a, "low-resolution");
    }
    if (item.aspect > 4.0 || (item.aspect > 0 && item.aspect < 0.25)) {
        AddIssue(a, "extreme-aspect");
    }
    Suitability(a, item.aspect, item.width, item.height, item.kind, o.concepts);
    Describe(a, o.concepts);
    item.analysis = std::move(a);
    return true;
}

void FindDuplicateMedia(MediaLibrary& library) {
    for (MediaItem& m : library.items) {
        std::erase_if(m.analysis.issues, [](const std::string& i) { return i.rfind("duplicate-of:", 0) == 0 || i.rfind("similar-to:", 0) == 0; });
    }
    for (size_t i = 0; i < library.items.size(); ++i) {
        MediaItem& m = library.items[i];
        if (!m.Usable() || !m.analysis.Done()) {
            continue;
        }
        for (size_t k = 0; k < i; ++k) {
            const MediaItem& o = library.items[k];
            if (!o.Usable() || !o.analysis.Done() || (o.kind == MediaKind::Video) != (m.kind == MediaKind::Video)) {
                continue;
            }
            const bool sameFile = !m.hash.empty() && m.hash == o.hash && m.bytes == o.bytes;
            const int d = Hamming(m.analysis.dhash, o.analysis.dhash);
            // On a nearly flat picture the perceptual hash is only noise: the light and
            // the main colour must agree too.
            double r1 = 0, g1 = 0, b1 = 0, r2 = 0, g2 = 0, b2 = 0;
            if (!m.analysis.colors.empty() && !o.analysis.colors.empty()) {
                HexRGB(m.analysis.colors.front().hex, r1, g1, b1);
                HexRGB(o.analysis.colors.front().hex, r2, g2, b2);
            }
            const bool looksAlike = std::fabs(m.analysis.brightness - o.analysis.brightness) < 0.06 && std::sqrt((r1 - r2) * (r1 - r2) + (g1 - g2) * (g1 - g2) + (b1 - b2) * (b1 - b2)) < 40 &&
                                    std::fabs(m.analysis.contrast - o.analysis.contrast) < 0.1;
            const bool sameShape = m.aspect > 0 && o.aspect > 0 && std::fabs(std::log(m.aspect / o.aspect)) < 0.05;
            const bool sameLength = m.kind != MediaKind::Video || std::abs(m.durationMS - o.durationMS) <= std::max(500, o.durationMS / 20);
            if (sameFile || (sameShape && sameLength && looksAlike && d <= 5)) {
                AddIssue(m.analysis, "duplicate-of:" + o.id);
                break;
            }
            if (sameShape && looksAlike && d <= 12 && m.analysis.contrast >= 0.05) {
                AddIssue(m.analysis, "similar-to:" + o.id);
            }
        }
    }
}

std::vector<std::string> EffectiveCategories(const MediaItem& m) {
    if (m.categorySource == "manual") {
        return m.categories;
    }
    std::vector<std::string> out = m.categories;
    for (const std::string& c : m.analysis.categories) {
        if (!Has(out, c)) {
            out.push_back(c);
        }
    }
    return out;
}

MediaAnalysis EffectiveAnalysis(const MediaItem& m, const VisualConcepts* concepts) {
    MediaAnalysis a = m.analysis;
    const MediaHumanLabels& h = m.human;
    if (h.Empty()) {
        return a;
    }
    const auto gone = [&](const std::string& id) { return Has(h.rejected, id) || Has(h.excluded, id); };
    std::vector<MediaConcept> person, rest;
    for (const MediaConcept& c : a.concepts) {
        if (gone(c.id)) {
            continue;
        }
        if (Has(h.confirmed, c.id) || Has(h.added, c.id)) {
            MediaConcept p = c;
            p.level = "strong";
            p.score = std::max(p.score, 0.5);
            p.source = "person";
            person.push_back(p);
        } else {
            rest.push_back(c);
        }
    }
    for (const std::vector<std::string>* list : { &h.confirmed, &h.added }) {
        for (const std::string& id : *list) {
            if (!gone(id) && std::none_of(person.begin(), person.end(), [&](const MediaConcept& c) { return c.id == id; })) {
                person.push_back({ id, 0.5, 0, "strong", "person" });
            }
        }
    }
    a.concepts = person;
    a.concepts.insert(a.concepts.end(), rest.begin(), rest.end());
    std::vector<std::string> description;
    for (const MediaConcept& c : person) {
        description.push_back(c.id);
    }
    for (const std::string& id : a.description) {
        if (!gone(id) && !Has(description, id) && description.size() < 3) {
            description.push_back(id);
        }
    }
    a.description = description;
    if (concepts != nullptr) {
        for (const MediaConcept& c : person) {
            if (const VisualConceptDef* d = concepts->Find(c.id); d != nullptr) {
                for (const std::string& cat : d->categories) {
                    if (!Has(a.categories, cat)) {
                        a.categories.push_back(cat);
                    }
                }
            }
        }
    }
    return a;
}

void ApplyHumanLabels(MediaLibrary& library, const VisualConcepts* concepts) {
    for (MediaItem& m : library.items) {
        m.analysis = EffectiveAnalysis(m, concepts);
    }
}

MediaAnalysisSummary AnalyzeMediaLibrary(MediaLibrary& library, const MediaAnalysisOptions& o) {
    MediaAnalysisSummary s;
    const auto start = std::chrono::steady_clock::now();
    const bool withModel = o.model != nullptr && o.index != nullptr;
    const std::string modelId = withModel ? o.model->Id() : std::string();
    int todo = 0;
    for (const MediaItem& m : library.items) {
        todo += m.Usable() ? 1 : 0;
    }
    int done = 0;
    for (MediaItem& m : library.items) {
        ++s.total;
        s.images += m.kind == MediaKind::Image ? 1 : 0;
        s.animations += m.kind == MediaKind::Animation ? 1 : 0;
        s.videos += m.kind == MediaKind::Video ? 1 : 0;
        if (m.status == MediaStatus::Unsupported || m.kind == MediaKind::Unsupported) {
            ++s.unsupported;
            continue;
        }
        if (m.status == MediaStatus::Missing) {
            ++s.missing;
            continue;
        }
        if (m.status == MediaStatus::Unreadable) {
            ++s.damaged;
            continue;
        }
        ++s.usable;
        if (o.cancelled && o.cancelled()) {
            s.cancelled = true;
            break;
        }
        const std::string want = MediaAnalysisKey(m, modelId, o);
        // An analysis with the model is kept when this run has none: it is better, and
        // the file has not changed.
        const std::string keptFile = m.analysis.key.substr(0, m.analysis.key.find('|'));
        const bool betterKept = !withModel && m.analysis.mode == "semantic" && keptFile == FilePart(m) && m.analysis.analyzer == MEDIA_ANALYZER_VERSION;
        if (!o.force && m.analysis.Done() && (m.analysis.key == want || betterKept)) {
            ++s.cached;
            s.semantic += m.analysis.mode == "semantic" ? 1 : 0;
        } else {
            if (o.progress) {
                o.progress(done, todo, m.name);
            }
            if (AnalyzeMediaItem(m, o)) {
                ++s.analysed;
                s.semantic += m.analysis.mode == "semantic" ? 1 : 0;
            } else if (o.cancelled && o.cancelled()) {
                s.cancelled = true;
                break;
            } else {
                ++s.failed;
            }
        }
        ++done;
        if (o.progress) {
            o.progress(done, todo, m.name);
        }
    }
    FindDuplicateMedia(library);
    for (const MediaItem& m : library.items) {
        s.duplicates += std::any_of(m.analysis.issues.begin(), m.analysis.issues.end(), [](const std::string& i) { return i.rfind("duplicate-of:", 0) == 0; }) ? 1 : 0;
        s.similar += std::any_of(m.analysis.issues.begin(), m.analysis.issues.end(), [](const std::string& i) { return i.rfind("similar-to:", 0) == 0; }) ? 1 : 0;
    }
    s.mode = s.semantic == 0 ? "basic" : s.semantic >= s.usable - s.failed ? "semantic" : "mixed";
    s.elapsedMS = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    return s;
}

json MediaAnalysisJson(const MediaAnalysis& a) {
    json colors = json::array();
    for (const MediaColor& c : a.colors) {
        colors.push_back({ { "hex", c.hex }, { "share", c.share } });
    }
    const auto conceptsJson = [](const std::vector<MediaConcept>& list) {
        json out = json::array();
        for (const MediaConcept& c : list) {
            out.push_back({ { "id", c.id }, { "score", c.score }, { "similarity", c.similarity }, { "level", c.level }, { "source", c.source } });
        }
        return out;
    };
    json j = { { "key", a.key },
               { "analyzer", a.analyzer },
               { "mode", a.mode },
               { "colors", std::move(colors) },
               { "brightness", a.brightness },
               { "contrast", a.contrast },
               { "saturation", a.saturation },
               { "colorfulness", a.colorfulness },
               { "complexity", a.complexity },
               { "dark", a.dark },
               { "white", a.white },
               { "highlights", a.highlights },
               { "orientation", a.orientation },
               { "poi", { a.poiX, a.poiY } },
               { "subject", a.subject },
               { "dhash", fmt::format("{:016x}", a.dhash) },
               { "concepts", conceptsJson(a.concepts) },
               { "description", a.description },
               { "categories", a.categories },
               { "fit", { { "matrix", a.matrixFit }, { "megaTree", a.treeFit }, { "reasons", a.fitReasons } } },
               { "issues", a.issues } };
    if (!a.model.empty()) {
        j["model"] = a.model;
    }
    if (!a.error.empty()) {
        j["error"] = a.error;
    }
    if (a.video.samples > 0) {
        json scenes = json::array();
        for (const MediaScene& s : a.video.scenes) {
            scenes.push_back({ { "startMS", s.startMS }, { "endMS", s.endMS }, { "brightness", s.brightness }, { "motion", s.motion }, { "energy", s.energy }, { "color", s.color },
                               { "dhash", fmt::format("{:016x}", s.dhash) }, { "concepts", conceptsJson(s.concepts) } });
        }
        json dark = json::array();
        for (const auto& [f, t] : a.video.darkSpans) {
            dark.push_back({ f, t });
        }
        j["video"] = { { "samples", a.video.samples }, { "sampledMS", a.video.sampledMS }, { "scenes", std::move(scenes) }, { "motion", a.video.motion }, { "cuts", a.video.cuts },
                       { "darkSpans", std::move(dark) }, { "loop", a.video.loop }, { "energy", a.video.energy }, { "short", a.video.shortUse }, { "long", a.video.longUse } };
    }
    return j;
}

MediaAnalysis MediaAnalysisFrom(const json& j) {
    MediaAnalysis a;
    const auto conceptsFrom = [](const json& list) {
        std::vector<MediaConcept> out;
        for (const json& c : list) {
            out.push_back({ c.value("id", std::string()), c.value("score", 0.0), c.value("similarity", 0.0), c.value("level", std::string("weak")), c.value("source", std::string("model")) });
        }
        return out;
    };
    const auto hash = [](const json& v) { return v.is_string() ? std::strtoull(v.get<std::string>().c_str(), nullptr, 16) : 0ull; };
    a.key = j.value("key", std::string());
    a.analyzer = j.value("analyzer", std::string());
    a.model = j.value("model", std::string());
    a.mode = j.value("mode", std::string());
    a.error = j.value("error", std::string());
    for (const json& c : j.value("colors", json::array())) {
        a.colors.push_back({ c.value("hex", std::string()), c.value("share", 0.0) });
    }
    a.brightness = j.value("brightness", 0.0);
    a.contrast = j.value("contrast", 0.0);
    a.saturation = j.value("saturation", 0.0);
    a.colorfulness = j.value("colorfulness", 0.0);
    a.complexity = j.value("complexity", 0.0);
    a.dark = j.value("dark", 0.0);
    a.white = j.value("white", 0.0);
    a.highlights = j.value("highlights", 0.0);
    a.orientation = j.value("orientation", std::string());
    if (j.contains("poi") && j["poi"].is_array() && j["poi"].size() == 2) {
        a.poiX = j["poi"][0].get<double>();
        a.poiY = j["poi"][1].get<double>();
    }
    a.subject = j.value("subject", std::string());
    a.dhash = j.contains("dhash") ? hash(j["dhash"]) : 0;
    a.concepts = conceptsFrom(j.value("concepts", json::array()));
    a.description = j.value("description", std::vector<std::string>());
    a.categories = j.value("categories", std::vector<std::string>());
    if (j.contains("fit") && j["fit"].is_object()) {
        a.matrixFit = j["fit"].value("matrix", 0.0);
        a.treeFit = j["fit"].value("megaTree", 0.0);
        a.fitReasons = j["fit"].value("reasons", std::vector<std::string>());
    }
    a.issues = j.value("issues", std::vector<std::string>());
    if (j.contains("video") && j["video"].is_object()) {
        const json& v = j["video"];
        a.video.samples = v.value("samples", 0);
        a.video.sampledMS = v.value("sampledMS", 0);
        for (const json& s : v.value("scenes", json::array())) {
            MediaScene sc;
            sc.startMS = s.value("startMS", 0);
            sc.endMS = s.value("endMS", 0);
            sc.brightness = s.value("brightness", 0.0);
            sc.motion = s.value("motion", 0.0);
            sc.energy = s.value("energy", 0.0);
            sc.color = s.value("color", std::string());
            sc.dhash = s.contains("dhash") ? hash(s["dhash"]) : 0;
            sc.concepts = conceptsFrom(s.value("concepts", json::array()));
            a.video.scenes.push_back(std::move(sc));
        }
        a.video.motion = v.value("motion", 0.0);
        a.video.cuts = v.value("cuts", 0);
        for (const json& d : v.value("darkSpans", json::array())) {
            if (d.is_array() && d.size() == 2) {
                a.video.darkSpans.emplace_back(d[0].get<int>(), d[1].get<int>());
            }
        }
        a.video.loop = v.value("loop", 0.0);
        a.video.energy = v.value("energy", 0.0);
        a.video.shortUse = v.value("short", false);
        a.video.longUse = v.value("long", false);
    }
    return a;
}

std::string MediaAnalysisReport(const MediaLibrary& library, const MediaAnalysisSummary& s) {
    std::string out = fmt::format("Media analysis ({}): {} files, {} images, {} animations, {} videos; {} usable, {} not supported, {} damaged, {} missing; {} analysed, {} from the cache, {} with the model, {} failed; {} duplicates, {} similar; {} ms{}\n",
                                  s.mode, s.total, s.images, s.animations, s.videos, s.usable, s.unsupported, s.damaged, s.missing, s.analysed, s.cached, s.semantic, s.failed, s.duplicates, s.similar,
                                  s.elapsedMS, s.cancelled ? ", cancelled" : "");
    if (s.semantic == 0) {
        out += "  no image-text model was used: the concepts below come from colour rules and are only guesses\n";
    }
    for (const MediaItem& m : library.items) {
        const MediaAnalysis& a = m.analysis;
        if (!a.Done()) {
            out += fmt::format("  {}: {}{}\n", m.name, MediaStatusId(m.status), m.problem.empty() ? std::string() : " (" + m.problem + ")");
            continue;
        }
        std::string concepts;
        for (const MediaConcept& c : a.concepts) {
            concepts += fmt::format("{}{} {:.2f} {}", concepts.empty() ? "" : ", ", c.id, c.score, c.level);
        }
        std::string colors;
        for (const MediaColor& c : a.colors) {
            colors += fmt::format("{}{} {:.0f}%", colors.empty() ? "" : " ", c.hex, c.share * 100);
        }
        out += fmt::format("  {} [{}] {} {}x{}: light {:.2f}, contrast {:.2f}, detail {:.2f}, poi ({:.2f}, {:.2f}) {}, matrix {:.2f}, mega tree {:.2f}; {}; colours {}", m.name, a.mode, a.orientation, m.width,
                           m.height, a.brightness, a.contrast, a.complexity, a.poiX, a.poiY, a.subject, a.matrixFit, a.treeFit, concepts.empty() ? std::string("no concept") : concepts, colors);
        if (a.video.samples > 0) {
            out += fmt::format("; video: {} scenes, {} cuts, motion {:.2f}, energy {:.2f}, loop {:.2f}", a.video.scenes.size(), a.video.cuts, a.video.motion, a.video.energy, a.video.loop);
        }
        if (!a.issues.empty()) {
            std::string issues;
            for (const std::string& i : a.issues) {
                issues += (issues.empty() ? "" : ", ") + i;
            }
            out += " [" + issues + "]";
        }
        out += "\n";
    }
    return out;
}

} // namespace composer
