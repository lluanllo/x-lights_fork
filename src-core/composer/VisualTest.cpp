#include "composer/VisualTest.h"

#include "composer/MediaAnalysis.h"
#include "composer/MediaLibrary.h"
#include "composer/MediaTest.h"
#include "composer/VisualModel.h"
#include "media/VideoWriter.h"
#include "utils/ExternalHooks.h"
#include "utils/xlImage.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace composer {

namespace {

fs::path P(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string S(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool WriteFile(const fs::path& p, const std::vector<uint8_t>& bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

uint8_t Clamp(double v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
}

double Noise(int x, int y, int seed) {
    uint32_t n = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(seed) * 2246822519u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return ((n ^ (n >> 16)) & 0xffff) / 65535.0;
}

using PixelFn = std::function<void(int x, int y, double& r, double& g, double& b)>;

// Pictures drawn here, from nothing: no one else's work is in them.
xlImage Draw(int w, int h, const PixelFn& pixel) {
    xlImage img(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double r = 0, g = 0, b = 0;
            pixel(x, y, r, g, b);
            img.SetRGBA(x, y, Clamp(r), Clamp(g), Clamp(b), 255);
        }
    }
    return img;
}

bool SavePNG(const xlImage& img, const fs::path& p) {
    std::vector<uint8_t> png;
    return img.SaveAsPNG(png) && WriteFile(p, png);
}

// Mountains under a blue sky, wide: a panorama.
xlImage Mountains(int w, int h) {
    return Draw(w, h, [w, h](int x, int y, double& r, double& g, double& b) {
        const double u = static_cast<double>(x) / w, v = static_cast<double>(y) / h;
        const double ridge = 0.45 + 0.18 * std::sin(u * 17.0) * std::sin(u * 5.3 + 1.0) + 0.08 * std::sin(u * 41.0);
        if (v < ridge) {
            r = 90 + 80 * v;
            g = 150 + 70 * v;
            b = 235;
        } else if (v < ridge + 0.06) {
            r = g = b = 235; // snow on the peaks
        } else if (v < 0.82) {
            r = 95 - 30 * v;
            g = 100 - 30 * v;
            b = 110 - 30 * v;
        } else {
            r = 40;
            g = 120 + 30 * Noise(x, y, 3);
            b = 40;
        }
    });
}

// A Christmas tree with lights and a star, tall.
xlImage ChristmasTree(int w, int h, double hueShift) {
    return Draw(w, h, [=](int x, int y, double& r, double& g, double& b) {
        const double u = static_cast<double>(x) / w - 0.5, v = static_cast<double>(y) / h;
        r = 8;
        g = 12;
        b = 40;
        const double half = (v - 0.12) * 0.45;
        if (v > 0.12 && v < 0.88 && std::fabs(u) < half) {
            r = 10;
            g = 110 + 40 * Noise(x / 3, y / 3, 5);
            b = 30;
            if (Noise(x / 6, y / 6, 9) > 0.93) {
                const int k = static_cast<int>(Noise(x / 6, y / 6, 11) * 3 + hueShift) % 3;
                r = k == 0 ? 255 : k == 1 ? 255 : 80;
                g = k == 0 ? 40 : k == 1 ? 210 : 120;
                b = k == 0 ? 40 : k == 1 ? 40 : 255;
            }
        } else if (v >= 0.88 && v < 0.96 && std::fabs(u) < 0.05) {
            r = 100;
            g = 60;
            b = 20;
        }
        const double sx = u, sy = v - 0.09;
        if (std::fabs(sx) + std::fabs(sy) * 1.3 < 0.06) {
            r = 255;
            g = 220;
            b = 40;
        }
    });
}

// A dark street with a lantern well off to the right: the subject is not centred.
xlImage Lantern(int w, int h) {
    return Draw(w, h, [w, h](int x, int y, double& r, double& g, double& b) {
        const double dx = static_cast<double>(x) / w - 0.8, dy = static_cast<double>(y) / h - 0.4;
        const double d = std::sqrt(dx * dx * 3.2 + dy * dy);
        r = 20 + 10 * Noise(x / 4, y / 4, 1);
        g = 22;
        b = 35;
        if (d < 0.09) {
            r = 255;
            g = 150;
            b = 40;
        } else if (d < 0.3) {
            const double k = 1 - d / 0.3;
            r += 160 * k * k;
            g += 80 * k * k;
            b += 10 * k * k;
        }
    });
}

xlImage Bokeh(int w, int h) {
    return Draw(w, h, [w, h](int x, int y, double& r, double& g, double& b) {
        r = 10;
        g = 5;
        b = 25;
        for (int i = 0; i < 24; ++i) {
            const double cx = Noise(i, 1, 21) * w, cy = Noise(i, 2, 21) * h, rad = 12 + Noise(i, 3, 21) * 40;
            const double d = std::hypot(x - cx, y - cy);
            if (d < rad) {
                const double k = 0.6 + 0.4 * (1 - d / rad);
                const int c = i % 4;
                r += k * (c == 0 ? 255 : c == 1 ? 40 : c == 2 ? 255 : 120);
                g += k * (c == 0 ? 60 : c == 1 ? 200 : c == 2 ? 200 : 40);
                b += k * (c == 0 ? 160 : c == 1 ? 255 : c == 2 ? 30 : 255);
            }
        }
    });
}

xlImage Snow(int w, int h) {
    return Draw(w, h, [w, h](int x, int y, double& r, double& g, double& b) {
        const double v = static_cast<double>(y) / h;
        if (v < 0.55) {
            r = 200 + 30 * v;
            g = 215 + 20 * v;
            b = 235;
        } else {
            r = g = 240 - 20 * Noise(x / 8, y / 8, 4);
            b = 250;
        }
        if (Noise(x / 2, y / 2, 17) > 0.985) {
            r = g = b = 255;
        }
        (void)w;
    });
}

xlImage Fire(int w, int h, int seed) {
    return Draw(w, h, [w, h, seed](int x, int y, double& r, double& g, double& b) {
        const double u = static_cast<double>(x) / w, v = static_cast<double>(y) / h;
        const double flame = std::clamp((v - 0.25) * 1.4 + 0.35 * std::sin(u * 18.0 + Noise(x / 16, 0, seed) * 3) * (1 - v) - 0.2 * Noise(x / 5, y / 5, seed), 0.0, 1.0);
        r = 40 + 215 * flame;
        g = 10 + 170 * flame * flame;
        b = 5 + 40 * flame * flame * flame;
    });
}

xlImage Stars(int w, int h) {
    return Draw(w, h, [w, h](int x, int y, double& r, double& g, double& b) {
        r = 2;
        g = 4;
        b = 18 + 20.0 * y / h;
        if (Noise(x, y, 7) > 0.995) {
            r = g = b = 240;
        }
        const double d = std::hypot(x - w * 0.75, y - h * 0.3);
        if (d < h * 0.08) {
            r = g = 230;
            b = 200;
        }
    });
}

xlImage Flat(int w, int h, double level) {
    return Draw(w, h, [level](int x, int y, double& r, double& g, double& b) { r = g = b = level + 3 * Noise(x, y, 2); });
}

using FrameFn = std::function<void(int frame, int x, int y, double& r, double& g, double& b)>;

bool WriteVideo(const fs::path& p, int w, int h, int fps, int frames, const FrameFn& pixel, std::string& error) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    VideoWriterParams params;
    params.width = w;
    params.height = h;
    params.fps = fps;
    params.audioSampleRate = 0;
    params.inputChannels = 3;
    params.cpuFrames = true;
    params.videoCodec = "H.264";
    try {
        VideoWriter writer(S(p), params, true);
        writer.setGetVideoCallback([&](VideoWriterFrame& vf, unsigned index) {
            const int i = static_cast<int>(std::min<unsigned>(index, static_cast<unsigned>(frames - 1)));
            for (int y = 0; y < h; ++y) {
                for (int x = 0; x < w; ++x) {
                    double r = 0, g = 0, b = 0;
                    pixel(i, x, y, r, g, b);
                    uint8_t* px = vf.rgbBuffer + (static_cast<size_t>(y) * w + x) * 3;
                    px[0] = Clamp(r);
                    px[1] = Clamp(g);
                    px[2] = Clamp(b);
                }
            }
            return true;
        });
        writer.initialize();
        writer.exportFrames(frames);
        writer.completeExport();
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return FileExists(S(p));
}

constexpr int VW = 192;
constexpr int VH = 108;
constexpr int FPS = 20;

} // namespace

bool WriteVisualFixtures(const std::string& folder, std::string& error) {
    const fs::path f = P(folder);
    std::error_code ec;
    fs::create_directories(f, ec);
    bool ok = SavePNG(Mountains(1500, 300), f / P("paisaje montañas panorámico.png"));
    ok = ok && SavePNG(ChristmasTree(360, 900, 0), f / P("árbol de navidad vertical.png"));
    ok = ok && SavePNG(Lantern(800, 450), f / P("farol a la derecha.png"));
    ok = ok && SavePNG(Bokeh(640, 360), f / P("abstracto colores.png"));
    ok = ok && SavePNG(Snow(640, 360), f / P("nieve.png"));
    ok = ok && SavePNG(Fire(480, 480, 1), f / P("fuego.png"));
    ok = ok && SavePNG(Stars(800, 400), f / P("noche estrellada.png"));
    ok = ok && SavePNG(Flat(400, 300, 3), f / P("casi negro.png"));
    ok = ok && SavePNG(Flat(400, 300, 250), f / P("casi blanco.png"));
    ok = ok && SavePNG(Mountains(20, 12), f / P("diminuta.png"));
    ok = ok && SavePNG(Fire(480, 480, 1), f / P("duplicados/fuego copia.png"));
    ok = ok && SavePNG(ResampleImage(Fire(480, 480, 1), 432, 432), f / P("duplicados/fuego reducido.png"));
    ok = ok && SavePNG(ChristmasTree(300, 750, 1), f / P("Árbol — ñandú 星.png"));
    ok = ok && WriteFile(f / P("roto.png"), { 0x89, 'P', 'N', 'G', 1, 2, 3, 4, 5, 6, 7, 8 });
    if (!ok) {
        error = "pictures";
        return false;
    }
    // Three scenes cut sharply: red, green, blue, each with something moving.
    ok = WriteVideo(f / P("vídeos/tres escenas.mp4"), VW, VH, FPS, 6 * FPS, [](int i, int x, int y, double& r, double& g, double& b) {
        const int scene = i / (2 * FPS);
        const double cx = 30 + (i % (2 * FPS)) * 3.0;
        const bool dot = std::hypot(x - cx, y - VH / 2.0) < 14;
        r = scene == 0 ? 200 : 20;
        g = scene == 1 ? 180 : 20;
        b = scene == 2 ? 210 : 30;
        if (dot) {
            r = g = b = 250;
        }
    }, error);
    // Fast stripes and flashes.
    ok = ok && WriteVideo(f / P("vídeos/mucho movimiento.mp4"), VW, VH, FPS, 4 * FPS, [](int i, int x, int y, double& r, double& g, double& b) {
        const bool stripe = ((x + i * 9) / 12) % 2 == 0;
        r = stripe ? 250 : 30;
        g = stripe ? 120 : 10;
        b = ((y + i * 7) / 10) % 2 == 0 ? 200 : 20;
    }, error);
    // A slow drift of colour.
    ok = ok && WriteVideo(f / P("vídeos/poco movimiento.mp4"), VW, VH, FPS, 4 * FPS, [](int i, int x, int y, double& r, double& g, double& b) {
        r = 40 + 0.3 * x + 0.2 * i;
        g = 60 + 0.5 * y;
        b = 140;
    }, error);
    ok = ok && WriteVideo(f / P("vídeos/estático.mp4"), VW, VH, FPS, 3 * FPS, [](int, int x, int y, double& r, double& g, double& b) {
        r = 30 + x / 2.0;
        g = 80;
        b = 30 + y;
    }, error);
    ok = ok && WriteVideo(f / P("vídeos/intro oscura.mp4"), VW, VH, FPS, 4 * FPS, [](int i, int x, int y, double& r, double& g, double& b) {
        if (i < FPS * 3 / 2) {
            r = g = b = 0;
            return;
        }
        r = 220;
        g = 120 + 60 * std::sin((x + i * 4) / 15.0);
        b = 40 + y;
    }, error);
    // One full turn of a light around the centre: the last frame runs into the first.
    ok = ok && WriteVideo(f / P("vídeos/bucle.mp4"), VW, VH, FPS, 4 * FPS, [](int i, int x, int y, double& r, double& g, double& b) {
        const double a = 2 * 3.14159265 * i / (4 * FPS);
        const double cx = VW / 2.0 + 50 * std::cos(a), cy = VH / 2.0 + 30 * std::sin(a);
        const double d = std::hypot(x - cx, y - cy);
        r = 10 + 240 * std::max(0.0, 1 - d / 22);
        g = 10 + 200 * std::max(0.0, 1 - d / 22);
        b = 60;
    }, error);
    // Snow in three scenes: a few slow flakes, a blizzard, and a green glow in the sky.
    ok = ok && WriteVideo(f / P("vídeos/nevada en tres escenas.mp4"), VW, VH, FPS, 9 * FPS, [](int i, int x, int y, double& r, double& g, double& b) {
        const int scene = i / (3 * FPS);
        const int t = i % (3 * FPS);
        r = 5;
        g = 10;
        b = 50;
        if (scene == 2) {
            const double band = std::exp(-std::pow((y - 25.0 - 8 * std::sin((x + t * 2) / 20.0)) / 9.0, 2));
            g += 180 * band;
            b += 60 * band;
        }
        const int speed = scene == 1 ? 6 : 1;
        const int spacing = scene == 1 ? 7 : 19;
        if (Noise(x / 2, (y - t * speed + 4000) / 2, 31) > (scene == 1 ? 0.85 : 0.94) && (x + y) % spacing < 3) {
            r = g = b = 245;
        }
    }, error);
    ok = ok && WriteVideo(f / P("vídeos/estrellas fugaces.mp4"), VW, VH, FPS, 5 * FPS, [](int i, int x, int y, double& r, double& g, double& b) {
        r = 2;
        g = 3;
        b = 20;
        if (Noise(x, y, 41) > 0.993) {
            r = g = b = 200;
        }
        for (int k = 0; k < 3; ++k) {
            const double hx = std::fmod(i * 7.0 + k * 70, VW + 60) - 30, hy = 15 + k * 30 + (i * 7.0 + k * 70) * 0.25 - std::floor((i * 7.0 + k * 70) / (VW + 60)) * 0;
            const double d = std::fabs((y - hy) - 0.25 * (x - hx));
            if (x <= hx && x > hx - 25 && d < 1.5) {
                const double k2 = 1.0 - (hx - x) / 25.0;
                r = g = b = 255 * k2;
            }
        }
    }, error);
    ok = ok && WriteVideo(f / P("vídeos/corto.mp4"), VW, VH, FPS, FPS, [](int i, int x, int, double& r, double& g, double& b) {
        r = 100 + i * 5;
        g = 50 + x / 2.0;
        b = 90;
    }, error);
    if (!ok) {
        error = "videos: " + error;
        return false;
    }
    return WriteMediaFixtures(S(f / "fase10"), error);
}

namespace {

class Suite {
public:
    Suite(fs::path root, std::string modelDir) : _root(std::move(root)), _modelDir(std::move(modelDir)) {}

    int Run() {
        if (!Prepare()) {
            return Summary();
        }
        TestBasicAnalysis();
        TestVideos();
        TestDuplicatesAndIssues();
        TestCache();
        TestModel();
        return Summary();
    }

private:
    fs::path _root;
    std::string _modelDir;
    fs::path _show;
    fs::path _media;
    MediaLibrary _lib;
    MediaAnalysisSummary _summary;
    VisualConcepts _concepts;
    int _passed = 0;
    int _failed = 0;

    void Check(bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        fflush(stdout);
        ok ? ++_passed : ++_failed;
    }

    void Title(const char* title) {
        printf("=== %s\n", title);
        fflush(stdout);
    }

    int Summary() {
        printf("composer-visual-test: %d passed, %d failed (%s)\n", _passed, _failed, S(_root).c_str());
        return _failed == 0 ? 0 : 1;
    }

    const MediaItem* Item(const std::string& name) const {
        for (const MediaItem& m : _lib.items) {
            if (m.name == name) {
                return &m;
            }
        }
        return nullptr;
    }

    bool HasIssue(const MediaItem* m, const std::string& prefix) const {
        return m != nullptr && std::any_of(m->analysis.issues.begin(), m->analysis.issues.end(), [&](const std::string& i) { return i.rfind(prefix, 0) == 0; });
    }

    bool HasConcept(const MediaItem* m, const std::string& id, size_t within) const {
        if (m == nullptr) {
            return false;
        }
        for (size_t i = 0; i < m->analysis.concepts.size() && i < within; ++i) {
            if (m->analysis.concepts[i].id == id) {
                return true;
            }
        }
        return false;
    }

    MediaAnalysisOptions Options() const {
        MediaAnalysisOptions o;
        o.showDir = S(_show);
        o.concepts = &_concepts;
        return o;
    }

    bool Prepare() {
        Title("fixtures");
        _show = _root / "show";
        _media = _root / P("Mis imágenes y vídeos");
        std::error_code ec;
        fs::create_directories(_show, ec);
        std::string error;
        const auto t0 = std::chrono::steady_clock::now();
        const bool made = WriteVisualFixtures(S(_media), error);
        Check(made, fmt::format("original pictures and videos drawn by the test ({} ms){}", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(), error.empty() ? "" : ": " + error));
        std::string cerr;
        Check(ReadVisualConcepts(DefaultVisualConceptsPath(), _concepts, cerr), fmt::format("the concept vocabulary reads ({} concepts){}", _concepts.concepts.size(), cerr));
        if (!made) {
            return false;
        }
        MediaScanOptions so;
        so.showDir = S(_show);
        so.folders = { S(_media) };
        _lib = ScanMediaFolders(so);
        Check(_lib.items.size() >= 25, fmt::format("the library finds the fixtures ({} files)", _lib.items.size()));
        return true;
    }

    void TestBasicAnalysis() {
        Title("analysis without a model");
        _summary = AnalyzeMediaLibrary(_lib, Options());
        printf("%s", MediaAnalysisReport(_lib, _summary).c_str());
        Check(_summary.mode == "basic" && _summary.semantic == 0, "without a model the analysis says it is basic");
        Check(_summary.analysed == _summary.usable && _summary.failed == 0, fmt::format("every usable file is analysed ({} of {})", _summary.analysed, _summary.usable));
        Check(_summary.damaged >= 1, "the damaged file is counted as damaged, not analysed");
        const MediaItem* pano = Item("paisaje montañas panorámico.png");
        const MediaItem* tree = Item("árbol de navidad vertical.png");
        Check(pano != nullptr && pano->analysis.orientation == "panoramic", "a 5:1 picture is panoramic");
        Check(tree != nullptr && tree->analysis.orientation == "tall", "a 2:5 picture is tall");
        Check(pano != nullptr && pano->analysis.matrixFit > pano->analysis.treeFit, "the panorama suits the matrix better than the mega tree");
        Check(tree != nullptr && tree->analysis.treeFit > tree->analysis.matrixFit, "the tall picture suits the mega tree better than the matrix");
        const MediaItem* lantern = Item("farol a la derecha.png");
        Check(lantern != nullptr && lantern->analysis.poiX > 0.65 && std::fabs(lantern->analysis.poiY - 0.4) < 0.2,
              fmt::format("the point of interest is the lantern on the right ({:.2f}, {:.2f})", lantern ? lantern->analysis.poiX : 0, lantern ? lantern->analysis.poiY : 0));
        Check(lantern != nullptr && lantern->analysis.subject == "right", "the subject is said to be on the right");
        const MediaItem* bokeh = Item("abstracto colores.png");
        const MediaItem* snow = Item("nieve.png");
        Check(bokeh != nullptr && snow != nullptr && bokeh->analysis.colorfulness > snow->analysis.colorfulness + 0.2, "the abstract picture is far more colourful than the snow");
        Check(snow != nullptr && snow->analysis.brightness > 0.75 && snow->analysis.saturation < 0.2, "the snow is bright and pale");
        const MediaItem* fire = Item("fuego.png");
        bool warm = false;
        if (fire != nullptr && !fire->analysis.colors.empty()) {
            const std::string& hex = fire->analysis.colors.front().hex;
            const long v = std::strtol(hex.c_str() + 1, nullptr, 16);
            const long r = (v >> 16) & 255, g = (v >> 8) & 255, b = v & 255;
            warm = r > 2 * b && r >= g;
        }
        Check(warm, "the fire's main colour is warm");
        Check(fire != nullptr && fire->analysis.colors.size() >= 2 && fire->analysis.colors.size() <= 5, "a few dominant colours, largest first");
        const MediaItem* stars = Item("noche estrellada.png");
        Check(HasConcept(stars, "stars", 3), "the colour rules guess a starry sky");
        Check(HasConcept(fire, "fire", 3), "the colour rules guess fire");
        Check(stars != nullptr && !stars->analysis.concepts.empty() && stars->analysis.concepts.front().level == "weak" && stars->analysis.concepts.front().source == "rules",
              "a guess from the rules is only ever weak");
        const std::vector<std::string> effective = stars != nullptr ? EffectiveCategories(*stars) : std::vector<std::string>();
        Check(stars != nullptr && std::find(stars->analysis.categories.begin(), stars->analysis.categories.end(), "stars") != stars->analysis.categories.end() &&
                  std::find(effective.begin(), effective.end(), "stars") != effective.end(),
              "the guessed concept suggests its category");
        const MediaItem* unicode = Item("Árbol — ñandú 星.png");
        Check(unicode != nullptr && unicode->analysis.Done(), "a file with accents, a dash and a Chinese character is analysed");
        Check(pano != nullptr && !pano->analysis.key.empty() && pano->analysis.key.find("|basic|") != std::string::npos, "the cache key names the file version, analyser and model");
    }

    void TestVideos() {
        Title("videos over time");
        const MediaItem* three = Item("tres escenas.mp4");
        Check(three != nullptr && three->analysis.video.cuts >= 2 && three->analysis.video.scenes.size() == 3,
              fmt::format("three scenes cut sharply are found ({} cuts, {} scenes)", three ? three->analysis.video.cuts : -1, three ? three->analysis.video.scenes.size() : 0));
        if (three != nullptr && three->analysis.video.scenes.size() == 3) {
            const auto& sc = three->analysis.video.scenes;
            Check(std::abs(sc[1].startMS - 2000) <= 300 && std::abs(sc[2].startMS - 4000) <= 300, fmt::format("the cuts are at 2 s and 4 s ({} and {})", sc[1].startMS, sc[2].startMS));
            Check(sc[0].color != sc[1].color && sc[1].color != sc[2].color, "each scene keeps its own colour");
        }
        const MediaItem* fast = Item("mucho movimiento.mp4");
        const MediaItem* slow = Item("poco movimiento.mp4");
        Check(fast != nullptr && slow != nullptr && fast->analysis.video.motion > slow->analysis.video.motion + 0.3,
              fmt::format("fast stripes move much more than a slow drift ({:.2f} against {:.2f})", fast ? fast->analysis.video.motion : 0, slow ? slow->analysis.video.motion : 0));
        Check(fast != nullptr && slow != nullptr && fast->analysis.video.energy > slow->analysis.video.energy, "and have more energy");
        Check(HasIssue(Item("estático.mp4"), "static-video"), "a video whose frames never change is marked as static");
        const MediaItem* dark = Item("intro oscura.mp4");
        Check(dark != nullptr && !dark->analysis.video.darkSpans.empty() && dark->analysis.video.darkSpans.front().first == 0 && dark->analysis.video.darkSpans.front().second >= 1250,
              "the dark start of a video is found");
        const MediaItem* loop = Item("bucle.mp4");
        Check(loop != nullptr && fast != nullptr && loop->analysis.video.loop >= 0.7 && loop->analysis.video.loop > Item("tres escenas.mp4")->analysis.video.loop,
              fmt::format("a video that turns once loops well ({:.2f})", loop ? loop->analysis.video.loop : 0));
        Check(HasIssue(Item("corto.mp4"), "too-short"), "a one-second video is marked as too short");
        const MediaItem* gif = Item("bucle fuego.gif");
        Check(gif != nullptr && gif->analysis.video.samples >= 4, "an animated GIF is looked at frame by frame");
        Check(fast != nullptr && fast->analysis.video.shortUse && loop != nullptr && loop->analysis.video.longUse, "short and long uses are told apart");
    }

    void TestDuplicatesAndIssues() {
        Title("problems and duplicates");
        Check(HasIssue(Item("casi negro.png"), "very-dark"), "a nearly black picture is marked very dark");
        Check(HasIssue(Item("casi blanco.png"), "very-bright"), "a nearly white picture is marked very bright");
        Check(HasIssue(Item("diminuta.png"), "low-resolution"), "a 20 x 12 picture is marked as too small");
        Check(HasIssue(Item("paisaje montañas panorámico.png"), "extreme-aspect"), "a 5:1 picture is marked as very wide");
        const MediaItem* copy = Item("fuego copia.png");
        const MediaItem* fire = Item("fuego.png");
        // The library is sorted by path: the copy in its subfolder comes first.
        const bool copyFirst = copy != nullptr && fire != nullptr && copy->path < fire->path;
        Check(copyFirst ? HasIssue(fire, "duplicate-of:" + copy->id) : HasIssue(copy, "duplicate-of:"), "an exact copy is marked as a duplicate of the earlier one");
        const MediaItem* reduced = Item("fuego reducido.png");
        Check(HasIssue(reduced, "duplicate-of:") || HasIssue(reduced, "similar-to:"), "the same picture at another size is marked too");
        Check(!HasIssue(Item("nieve.png"), "duplicate-of:") && !HasIssue(Item("nieve.png"), "similar-to:"), "a different picture is not");
        Check(fs::exists(P(Item("casi negro.png")->path)) && fs::exists(P(copy->path)), "nothing marked is removed or moved");
    }

    void TestCache() {
        Title("cache");
        const std::string before = nlohmann::json(MediaAnalysisJson(Item("tres escenas.mp4")->analysis)).dump();
        const MediaAnalysisSummary again = AnalyzeMediaLibrary(_lib, Options());
        Check(again.analysed == 0 && again.cached == _summary.usable, fmt::format("a second analysis reuses everything ({} analysed, {} from the cache)", again.analysed, again.cached));
        std::string error;
        const std::string path = MediaLibraryPath(S(_show));
        Check(SaveMediaLibrary(path, _lib, error), "the library saves with its analysis");
        MediaLibrary read;
        Check(ReadMediaLibrary(path, read, error) && read.items.size() == _lib.items.size(), "and reads back");
        const MediaItem* r = read.Find(Item("tres escenas.mp4")->id);
        Check(r != nullptr && nlohmann::json(MediaAnalysisJson(r->analysis)).dump() == before, "the analysis reads back exactly");
        // A changed file is analysed again; the others still come from the cache.
        SavePNG(Snow(640, 360).Copy(), P(Item("nieve.png")->path));
        {
            xlImage changed = Snow(640, 360);
            for (int x = 0; x < 640; ++x) {
                changed.SetRGBA(x, 10, 255, 0, 0, 255);
            }
            SavePNG(changed, P(Item("nieve.png")->path));
        }
        MediaScanOptions so;
        so.showDir = S(_show);
        so.folders = { S(_media) };
        MediaLibrary rescanned = ScanMediaFolders(so, &read);
        const MediaAnalysisSummary third = AnalyzeMediaLibrary(rescanned, Options());
        Check(third.analysed == 1 && third.cached == _summary.usable - 1, fmt::format("after one file changed only it is analysed again ({} analysed)", third.analysed));
        MediaLibrary fresh = ScanMediaFolders(so);
        AnalyzeMediaLibrary(fresh, Options());
        bool same = fresh.items.size() == rescanned.items.size();
        for (size_t i = 0; same && i < fresh.items.size(); ++i) {
            same = nlohmann::json(MediaAnalysisJson(fresh.items[i].analysis)).dump() == nlohmann::json(MediaAnalysisJson(rescanned.items[i].analysis)).dump();
        }
        Check(same, "analysing from scratch gives exactly the cached analysis: it is deterministic");
        _lib = std::move(rescanned);
        MediaAnalysisOptions cancel = Options();
        cancel.force = true;
        int seen = 0;
        cancel.cancelled = [&]() { return seen >= 3; };
        cancel.progress = [&](int, int, const std::string&) { ++seen; };
        MediaLibrary copy = _lib;
        const MediaAnalysisSummary stopped = AnalyzeMediaLibrary(copy, cancel);
        Check(stopped.cancelled && stopped.analysed < _summary.usable, "an analysis can be cancelled");
    }

    void TestModel() {
        Title("image-text model");
        if (_modelDir.empty()) {
            printf("  no --vision-model given: the semantic analysis was not checked (only the basic analysis was)\n");
            return;
        }
        Check(VisualModelSupported(), "this build can run the image-text model");
        Check(VisualModelComplete(_modelDir), "the model folder is complete");
        UnigramTokenizer tok;
        std::string error;
        Check(tok.Load(S(P(_modelDir) / "tokenizer.json"), error), "the tokenizer reads");
        // Ids from Hugging Face's tokenizers library for the same text.
        const std::vector<std::pair<std::string, std::vector<int64_t>>> refs = {
            { "a photo of snow falling", { 262, 266, 1304, 267, 2468, 5027, 1 } },
            { "a christmas tree with lights", { 262, 266, 15508, 1921, 275, 2983, 1 } },
            { "fire", { 1422, 1 } },
            { "a lion in the savanna", { 262, 266, 12366, 268, 260, 262, 264, 266, 484, 7632, 1 } },
            { "starry night sky", { 12758, 5165, 766, 4333, 1 } },
            { "a city at night", { 262, 266, 741, 289, 766, 1 } },
            { "año nuevo fuegos artificiales", { 262, 266, 2, 298, 262, 296, 432, 288, 484, 298, 262, 411, 31695, 264, 5707, 288, 264, 1 } },
        };
        int same = 0;
        for (const auto& [text, ids] : refs) {
            same += tok.Encode(text, 0, false) == ids ? 1 : 0;
        }
        Check(same == static_cast<int>(refs.size()), fmt::format("the tokenizer gives the reference ids ({} of {})", same, refs.size()));
        Check(tok.Encode("fire", 64, true).size() == 64 && tok.Encode("fire", 64, true)[2] == 1, "texts are padded to 64 with the end token");
        Check(UnigramTokenizer::Canonical("A Photo, of SNOW!") == "a photo of snow", "texts are lowercased and stripped of punctuation first");
        if (!VisualModelSupported() || !VisualModelComplete(_modelDir)) {
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        std::unique_ptr<VisualModel> model = OpenVisualModel(_modelDir, error);
        Check(model != nullptr, fmt::format("the model opens ({} ms){}", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count(), error));
        if (model == nullptr) {
            return;
        }
        ConceptIndex index;
        const std::string cache = MediaCacheDir(S(_show));
        auto t1 = std::chrono::steady_clock::now();
        const bool built = BuildConceptIndex(*model, _concepts, cache, index, error) && index.ids.size() == _concepts.concepts.size();
        auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
        Check(built, fmt::format("the concepts' text embeddings are computed ({} in {} ms)", index.ids.size(), took));
        ConceptIndex again;
        t1 = std::chrono::steady_clock::now();
        const bool reread = BuildConceptIndex(*model, _concepts, cache, again, error) && again.vectors == index.vectors;
        took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
        Check(reread, fmt::format("and read back from the cache the second time ({} ms)", took));
        MediaAnalysisOptions o = Options();
        o.model = model.get();
        o.index = &again;
        t1 = std::chrono::steady_clock::now();
        const MediaAnalysisSummary s = AnalyzeMediaLibrary(_lib, o);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1).count();
        printf("%s", MediaAnalysisReport(_lib, s).c_str());
        Check(s.semantic == s.usable - s.failed && s.analysed == s.usable, fmt::format("with the model every file is analysed again, semantically ({} in {} ms)", s.semantic, ms));
        const auto top = [&](const char* name, std::initializer_list<const char*> ids, size_t within) {
            const MediaItem* m = Item(name);
            for (const char* id : ids) {
                if (HasConcept(m, id, within)) {
                    return true;
                }
            }
            return false;
        };
        Check(top("fuego.png", { "fire" }, 3), "the model sees fire in the fire");
        Check(top("noche estrellada.png", { "stars", "moon", "galaxy" }, 3), "a starry sky in the night picture");
        Check(top("árbol de navidad vertical.png", { "christmas-tree" }, 3), "a Christmas tree in the tree");
        Check(top("nieve.png", { "snowy-landscape", "snowfall", "snowflakes" }, 4), "snow in the snow");
        Check(top("paisaje montañas panorámico.png", { "mountains", "snowy-landscape" }, 3), "mountains in the panorama");
        Check(top("abstracto colores.png", { "bokeh", "abstract", "particles", "light-beams", "neon" }, 3), "lights or an abstract pattern in the bokeh");
        const MediaItem* three = Item("tres escenas.mp4");
        Check(three != nullptr && std::all_of(three->analysis.video.scenes.begin(), three->analysis.video.scenes.end(), [](const MediaScene& sc) { return !sc.concepts.empty(); }),
              "each scene of a video is looked at by the model");
        const MediaItem* fire = Item("fuego.png");
        Check(fire != nullptr && fire->analysis.key.find(model->Id()) != std::string::npos && fire->analysis.mode == "semantic", "the cache key names the model");
        Check(fire != nullptr && std::all_of(fire->analysis.concepts.begin(), fire->analysis.concepts.end(), [](const MediaConcept& c) { return c.score < 1.0 && !c.level.empty(); }),
              "every concept has a level and none is a certainty");
        // Without the model later, the better analysis is kept rather than lost.
        const MediaAnalysisSummary kept = AnalyzeMediaLibrary(_lib, Options());
        Check(kept.analysed == 0 && kept.semantic == s.semantic, "an analysis without the model keeps the semantic one already made");
    }
};

} // namespace

int RunVisualTest(const std::string& workDir, const std::string& modelDir) {
    fs::path root;
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        root = P(workDir) / fmt::format("composer-visual-test-{}", i);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-visual-test: cannot create %s\n", S(root).c_str());
        return 1;
    }
    Suite suite(root, modelDir);
    return suite.Run();
}

} // namespace composer
