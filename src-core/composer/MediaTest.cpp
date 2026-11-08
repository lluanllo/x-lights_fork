#include "composer/MediaTest.h"

#include "composer/MediaLibrary.h"
#include "media/VideoWriter.h"
#include "utils/ExternalHooks.h"
#include "utils/xlImage.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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

bool WriteText(const fs::path& p, const std::string& text) {
    return WriteFile(p, std::vector<uint8_t>(text.begin(), text.end()));
}

std::string Contents(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

uint8_t Clamp(double v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
}

// Pictures drawn here, from nothing: no one else's work is in them.
xlImage Draw(int w, int h, const std::function<void(int, int, double&, double&, double&)>& pixel) {
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

double Noise(int x, int y, int seed) {
    uint32_t n = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(seed) * 2246822519u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return ((n ^ (n >> 16)) & 0xffff) / 65535.0;
}

xlImage SnowyForest(int w, int h) {
    return Draw(w, h, [&](int x, int y, double& r, double& g, double& b) {
        const double t = static_cast<double>(y) / h;
        r = 10 + 30 * t;
        g = 20 + 40 * t;
        b = 70 + 80 * t;
        for (int k = 0; k < 7; ++k) {
            const int cx = (k * 97 + 40) % w;
            const int base = h - 20;
            const int top = base - 90 - (k % 3) * 25;
            const double half = (y - top) * 0.45;
            if (y >= top && y <= base && std::abs(x - cx) <= half) {
                r = 20;
                g = 110 + 40 * Noise(x, y, 3);
                b = 50;
            }
        }
        if (y > h - 24 || Noise(x, y, 1) > 0.996) {
            r = g = b = 240;
        }
    });
}

xlImage Bokeh(int w, int h) {
    return Draw(w, h, [&](int x, int y, double& r, double& g, double& b) {
        r = g = b = 8;
        for (int k = 0; k < 24; ++k) {
            const double cx = (k * 53.0 + 17) * w / 1300.0;
            const double cy = h * (0.25 + 0.5 * Noise(k, 0, 9));
            const double d = std::hypot(x - cx, y - cy);
            if (d < 30) {
                const double a = 1.0 - d / 30;
                r += a * (k % 3 == 0 ? 255 : 90);
                g += a * (k % 3 == 1 ? 220 : 60);
                b += a * (k % 3 == 2 ? 255 : 40);
            }
        }
    });
}

xlImage Penguin(int w, int h) {
    return Draw(w, h, [&](int x, int y, double& r, double& g, double& b) {
        r = 150;
        g = 200;
        b = 240;
        const double bx = (x - w / 2.0) / (w * 0.32);
        const double by = (y - h * 0.55) / (h * 0.36);
        const double hx = (x - w / 2.0) / (w * 0.22);
        const double hy = (y - h * 0.2) / (h * 0.1);
        if (bx * bx + by * by < 1 || hx * hx + hy * hy < 1) {
            r = g = b = 20;
        }
        const double fx = (x - w / 2.0) / (w * 0.2);
        const double fy = (y - h * 0.58) / (h * 0.28);
        if (fx * fx + fy * fy < 1) {
            r = g = b = 245;
        }
    });
}

xlImage Fireworks(int w, int h) {
    return Draw(w, h, [&](int x, int y, double& r, double& g, double& b) {
        r = 5;
        g = 5;
        b = 25;
        const double cx = w / 2.0, cy = h / 2.0;
        const double a = std::atan2(y - cy, x - cx);
        const double d = std::hypot(x - cx, y - cy);
        if (d < w * 0.45 && std::fmod(a + 3.2, 0.26) < 0.05) {
            const double f = 1.0 - d / (w * 0.45);
            r += 255 * f;
            g += 200 * f;
            b += 60 * f;
        }
    });
}

xlImage NightSky(int w, int h) {
    return Draw(w, h, [&](int x, int y, double& r, double& g, double& b) {
        r = 2;
        g = 4;
        b = 30 + 20.0 * y / h;
        if (Noise(x, y, 7) > 0.992) {
            r = g = b = 255;
        }
    });
}

xlImage Flame(int w, int h, int frame) {
    return Draw(w, h, [&](int x, int y, double& r, double& g, double& b) {
        const double t = 1.0 - static_cast<double>(y) / h;
        const double wave = std::sin(x * 0.3 + frame * 1.7) * 4;
        const double edge = h * (0.25 + 0.5 * std::abs(std::sin((x + wave) * 3.14159 / w)));
        const bool lit = y > h - edge;
        r = lit ? 255 : 10;
        g = lit ? 80 + 140 * (1 - t) * Noise(x, y, frame) : 5;
        b = lit ? 10 : 5;
    });
}

// A GIF whose picture data is written without compression: each pixel is one 9-bit
// code, with a clear code before the code table could grow. Any decoder reads it.
std::vector<uint8_t> GifOf(const std::vector<xlImage>& frames, int delayCS) {
    const int w = frames[0].GetWidth();
    const int h = frames[0].GetHeight();
    std::vector<uint8_t> out = { 'G', 'I', 'F', '8', '9', 'a', static_cast<uint8_t>(w & 255), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(h & 255), static_cast<uint8_t>(h >> 8), 0xF7, 0, 0 };
    for (int i = 0; i < 256; ++i) {
        out.push_back(static_cast<uint8_t>(((i >> 5) & 7) * 255 / 7));
        out.push_back(static_cast<uint8_t>(((i >> 2) & 7) * 255 / 7));
        out.push_back(static_cast<uint8_t>((i & 3) * 255 / 3));
    }
    const uint8_t loop[] = { 0x21, 0xFF, 0x0B, 'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0', 0x03, 0x01, 0x00, 0x00, 0x00 };
    out.insert(out.end(), std::begin(loop), std::end(loop));
    for (const xlImage& f : frames) {
        const uint8_t gce[] = { 0x21, 0xF9, 0x04, 0x00, static_cast<uint8_t>(delayCS & 255), static_cast<uint8_t>(delayCS >> 8), 0x00, 0x00 };
        out.insert(out.end(), std::begin(gce), std::end(gce));
        const uint8_t desc[] = { 0x2C, 0, 0, 0, 0, static_cast<uint8_t>(w & 255), static_cast<uint8_t>(w >> 8), static_cast<uint8_t>(h & 255), static_cast<uint8_t>(h >> 8), 0x00, 0x08 };
        out.insert(out.end(), std::begin(desc), std::end(desc));
        std::vector<uint8_t> bits;
        uint32_t acc = 0;
        int nbits = 0;
        const auto put = [&](int code) {
            acc |= static_cast<uint32_t>(code) << nbits;
            nbits += 9;
            while (nbits >= 8) {
                bits.push_back(static_cast<uint8_t>(acc & 255));
                acc >>= 8;
                nbits -= 8;
            }
        };
        put(256);
        int run = 0;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const int idx = (f.GetRed(x, y) >> 5) << 5 | (f.GetGreen(x, y) >> 5) << 2 | (f.GetBlue(x, y) >> 6);
                put(idx);
                if (++run == 254) {
                    put(256);
                    run = 0;
                }
            }
        }
        put(257);
        if (nbits > 0) {
            bits.push_back(static_cast<uint8_t>(acc & 255));
        }
        for (size_t i = 0; i < bits.size(); i += 255) {
            const size_t n = std::min<size_t>(255, bits.size() - i);
            out.push_back(static_cast<uint8_t>(n));
            out.insert(out.end(), bits.begin() + static_cast<std::ptrdiff_t>(i), bits.begin() + static_cast<std::ptrdiff_t>(i + n));
        }
        out.push_back(0);
    }
    out.push_back(0x3B);
    return out;
}

bool WriteVideo(const fs::path& p, int w, int h, int fps, int frames, std::string& error) {
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
                    uint8_t* px = vf.rgbBuffer + (static_cast<size_t>(y) * w + x) * 3;
                    const bool star = Noise(x, y, 11) > 0.99 && ((x + i) / 3) % 2 == 0;
                    const bool bar = std::abs(x - (i * w / frames)) < 12;
                    px[0] = star ? 255 : bar ? 200 : 5;
                    px[1] = star ? 255 : bar ? 30 : 10;
                    px[2] = star ? 255 : bar ? 30 : static_cast<uint8_t>(40 + 60 * y / h);
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

constexpr const char* SVG_CASTLE = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="160" viewBox="0 0 240 160">
<rect x="0" y="0" width="240" height="160" fill="#101040"/>
<rect x="40" y="60" width="160" height="100" fill="#8080a0"/>
<rect x="30" y="30" width="40" height="130" fill="#9090b0"/>
<rect x="170" y="30" width="40" height="130" fill="#9090b0"/>
<rect x="105" y="110" width="30" height="50" fill="#302010"/>
</svg>
)svg";

} // namespace

bool WriteMediaFixtures(const std::string& folder, std::string& error) {
    const fs::path f = P(folder);
    bool ok = SavePNG(SnowyForest(640, 360), f / "nieve_bosque 16x9.png");
    ok = ok && SavePNG(Bokeh(1200, 200), f / "panorama luces.png");
    ok = ok && SavePNG(Penguin(300, 800), f / P("vertical pingüino.png"));
    ok = ok && SavePNG(Fireworks(400, 400), f / P("Año nuevo — fuegos.png"));
    std::vector<xlImage> flames;
    for (int i = 0; i < 4; ++i) {
        flames.push_back(Flame(64, 48, i));
    }
    ok = ok && WriteFile(f / "bucle fuego.gif", GifOf(flames, 10));
    ok = ok && WriteText(f / "dibujo castillo.svg", SVG_CASTLE);
    if (!ok) {
        error = "pictures";
        return false;
    }
    if (!WriteVideo(f / "video estrellas.mp4", 320, 180, 20, 40, error)) {
        error = "video: " + error;
        return false;
    }
    return true;
}

namespace {

class Suite {
public:
    explicit Suite(fs::path root) : _root(std::move(root)) {}

    int Run() {
        TestNames();
        if (!Prepare()) {
            return Summary();
        }
        TestSafety();
        TestScan();
        TestThumbnails();
        TestPersistence();
        TestMovedAndMissing();
        TestCopy();
        TestLimits();
        TestUntouched();
        return Summary();
    }

private:
    fs::path _root;
    fs::path _show;
    fs::path _media;   // outside the show
    fs::path _inShow;  // a media folder inside the show
    MediaLibrary _lib;
    std::map<std::string, std::string> _before; // fixture path -> contents
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
        printf("composer-media-test: %d passed, %d failed (%s)\n", _passed, _failed, S(_root).c_str());
        return _failed == 0 ? 0 : 1;
    }

    const MediaItem* Named(const MediaLibrary& lib, const std::string& name) {
        const auto it = std::find_if(lib.items.begin(), lib.items.end(), [&](const MediaItem& m) { return m.name == name; });
        return it != lib.items.end() ? &*it : nullptr;
    }

    bool HasWarning(const MediaLibrary& lib, const std::string& prefix) {
        return std::any_of(lib.warnings.begin(), lib.warnings.end(), [&](const std::string& w) { return w.rfind(prefix, 0) == 0; });
    }

    MediaLibrary Scan(const MediaLibrary* previous = nullptr, int maxFiles = 5000) {
        MediaScanOptions o;
        o.showDir = S(_show);
        o.folders = { S(_media), S(_inShow) };
        o.maxFiles = maxFiles;
        return ScanMediaFolders(o, previous);
    }

    void TestNames() {
        Title("Kinds and categories from a file's name alone");
        Check(MediaKindOfName("a.PNG") == MediaKind::Image && MediaKindOfName("a.jpeg") == MediaKind::Image && MediaKindOfName("a.svg") == MediaKind::Image, "pictures by their extension, in any case");
        Check(MediaKindOfName("a.gif") == MediaKind::Animation && MediaKindOfName("a.mp4") == MediaKind::Video && MediaKindOfName("a.MOV") == MediaKind::Video, "animations and videos");
        Check(MediaKindOfName("a.webp") == MediaKind::Unsupported && MediaKindOfName("a.txt") == MediaKind::Unsupported && MediaKindOfName("sin extension") == MediaKind::Unsupported, "and what Show Composer cannot read");
        const auto has = [](const std::vector<std::string>& v, const char* c) { return std::find(v.begin(), v.end(), c) != v.end(); };
        const std::vector<std::string> a = SuggestMediaCategories("Invierno/Nieve en el bosque.png");
        Check(has(a, "snow") && has(a, "forest") && !has(a, "fire"), "\"Nieve en el bosque\" in \"Invierno\": snow and forest");
        Check(has(SuggestMediaCategories("Año Nuevo - cuenta atrás.mp4"), "new-year") && has(SuggestMediaCategories("Ano nuevo.png"), "new-year") && has(SuggestMediaCategories("Año nuevo — fuegos.png"), "new-year"),
              "\"Año Nuevo\": new year, written with ñ or without, next to a long dash");
        Check(has(SuggestMediaCategories("pingüinos.png"), "animals") && has(SuggestMediaCategories("Estrellas fugaces.jpg"), "stars"), "plurals and diaeresis: penguins are animals, \"estrellas\" are stars");
        Check(SuggestMediaCategories("IMG_2041.jpg").empty() && !has(SuggestMediaCategories("batería.png"), "halloween"), "nothing from a camera's number, and \"batería\" is not a bat");
        Check(MediaCategories().size() == 14 && MediaCategories().back() == "other", "fourteen fixed categories, ending in other");
    }

    bool Prepare() {
        Title("Original test media: pictures, an animation, a video, and files that should fail");
        _show = _root / "show";
        _media = _root / "Mis medios de prueba";
        _inShow = _show / "Medios del show";
        const fs::path spaced = _media / "Carpeta con espacios";
        std::string error;
        const bool made = WriteMediaFixtures(S(spaced), error);
        Check(made, "made by the test: three pictures in three shapes, a Unicode name, a GIF, an SVG and an H.264 video " + error);
        if (!made) {
            return false;
        }
        SavePNG(NightSky(320, 240), _inShow / "estrellas noche.png");
        SavePNG(Draw(32, 32, [](int, int, double& r, double& g, double& b) { r = g = b = 255; }), _inShow / "blanco.png");
        SavePNG(Draw(32, 32, [](int, int, double& r, double& g, double& b) { r = g = b = 0; }), _inShow / "negro.png");
        std::vector<uint8_t> broken = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n' };
        for (int i = 0; i < 300; ++i) {
            broken.push_back(static_cast<uint8_t>(i * 37));
        }
        WriteFile(spaced / "roto.png", broken);
        std::vector<uint8_t> junk(4096);
        for (size_t i = 0; i < junk.size(); ++i) {
            junk[i] = static_cast<uint8_t>(i * 131 + 7);
        }
        WriteFile(spaced / "roto.mp4", junk);
        WriteFile(spaced / "textura.webp", junk);
        WriteText(spaced / "notas.txt", "no es un medio");
        WriteFile(spaced / P("vacío.jpg"), {});
        SavePNG(NightSky(16, 16), _media / ".oculto" / "secreto.png");
        SavePNG(NightSky(16, 16), _media / "n1" / "n2" / "n3" / "medio.png");
        SavePNG(NightSky(16, 16), _media / "n1" / "n2" / "n3" / "n4" / "n5" / "n6" / "n7" / "hondo.png");
        SavePNG(NightSky(16, 16), _show / "Backup" / "copia.png");
        std::error_code ec;
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(_root, ec)) {
            if (e.is_regular_file()) {
                _before[S(e.path())] = Contents(e.path());
            }
        }
        Check(_before.size() >= 18, fmt::format("{} files written, in a path with spaces", _before.size()));
        return true;
    }

    void TestSafety() {
        Title("Folders that are not searched");
        std::string reason;
        Check(IsSafeMediaFolder(S(_media), reason), "a folder of pictures is searched");
#ifdef _WIN32
        const char* drive = std::getenv("SystemDrive");
        const std::string root = std::string(drive != nullptr ? drive : "C:") + "\\";
        const char* home = std::getenv("USERPROFILE");
        const char* system = std::getenv("WINDIR");
#else
        const std::string root = "/";
        const char* home = std::getenv("HOME");
        const char* system = "/usr";
#endif
        bool refused = !IsSafeMediaFolder(root, reason) && reason == "root";
        Check(refused, "the root of a disk is refused (" + reason + ")");
        if (home != nullptr) {
            refused = !IsSafeMediaFolder(home, reason) && reason == "home";
            Check(refused, "the whole home folder is refused (" + reason + ")");
        }
        if (system != nullptr) {
            refused = !IsSafeMediaFolder(system, reason) && reason == "system";
            Check(refused, "a system folder is refused (" + reason + ")");
        }
        refused = !IsSafeMediaFolder(S(_root / "no existe"), reason) && reason == "not-found";
        Check(refused, "a folder that is not there (" + reason + ")");
        refused = !IsSafeMediaFolder(S(_inShow / "blanco.png"), reason) && reason == "not-folder";
        Check(refused, "a file is not a folder (" + reason + ")");
        MediaScanOptions o;
        o.showDir = S(_show);
        o.folders = { root };
        const MediaLibrary lib = ScanMediaFolders(o);
        Check(lib.items.empty() && lib.folders.empty() && HasWarning(lib, "media-folder-refused:root"), "a scan asked for a disk root reads nothing and says why");
    }

    void TestScan() {
        Title("What the search finds, and what it makes of each file");
        _lib = Scan();
        const MediaItem* land = Named(_lib, "nieve_bosque 16x9.png");
        const MediaItem* pano = Named(_lib, "panorama luces.png");
        const MediaItem* tall = Named(_lib, "vertical pingüino.png");
        const MediaItem* uni = Named(_lib, "Año nuevo — fuegos.png");
        const MediaItem* gif = Named(_lib, "bucle fuego.gif");
        const MediaItem* svg = Named(_lib, "dibujo castillo.svg");
        const MediaItem* video = Named(_lib, "video estrellas.mp4");
        Check(land != nullptr && land->Usable() && land->kind == MediaKind::Image && land->width == 640 && land->height == 360 && std::abs(land->aspect - 1.778) < 0.01, "a landscape picture, 640x360, aspect 1.78");
        Check(pano != nullptr && pano->Usable() && pano->aspect >= 5.9, "a panorama, aspect 6");
        Check(tall != nullptr && tall->Usable() && tall->aspect < 0.4, "a tall picture with a Unicode name, aspect 0.38");
        Check(uni != nullptr && uni->Usable() && uni->width == 400, "a name with ñ and a long dash reads like any other");
        Check(gif != nullptr && gif->Usable() && gif->kind == MediaKind::Animation && gif->frames == 4 && gif->durationMS == 400, fmt::format("an animated GIF: {} frames, {} ms", gif ? gif->frames : 0, gif ? gif->durationMS : 0));
        Check(svg != nullptr && svg->Usable() && svg->width == 240 && svg->height == 160, "a drawing (SVG) with its own size");
        Check(video != nullptr && video->Usable() && video->kind == MediaKind::Video && video->width == 320 && video->height == 180 && std::abs(video->durationMS - 2000) <= 150,
              fmt::format("a video: {}x{}, {} ms", video ? video->width : 0, video ? video->height : 0, video ? video->durationMS : 0));
        const MediaItem* brokenPng = Named(_lib, "roto.png");
        const MediaItem* brokenMp4 = Named(_lib, "roto.mp4");
        const MediaItem* webp = Named(_lib, "textura.webp");
        const MediaItem* empty = Named(_lib, "vacío.jpg");
        Check(brokenPng != nullptr && brokenPng->status == MediaStatus::Unreadable && brokenPng->problem == "decode" && !brokenPng->Usable(), "a damaged picture is listed, unreadable, and never used");
        Check(brokenMp4 != nullptr && brokenMp4->status == MediaStatus::Unreadable && !brokenMp4->Usable(), "so is a damaged video");
        Check(webp != nullptr && webp->status == MediaStatus::Unsupported && webp->problem == "format", "a format Show Composer cannot read says so");
        Check(empty != nullptr && empty->status == MediaStatus::Unreadable && empty->problem == "empty", "an empty file is unreadable");
        Check(Named(_lib, "notas.txt") == nullptr && HasWarning(_lib, "media-other-files:"), "a text file is not media: counted, not listed");
        Check(Named(_lib, "secreto.png") == nullptr && HasWarning(_lib, "media-hidden-skipped:"), "a hidden folder is not entered");
        Check(Named(_lib, "medio.png") != nullptr && Named(_lib, "hondo.png") == nullptr && HasWarning(_lib, "media-depth-limit:6"), "four folders down is searched; eight is past the limit, which is said");
        Check(Named(_lib, "copia.png") == nullptr, "the show's backups are not media");
        const MediaItem* inShow = Named(_lib, "estrellas noche.png");
        Check(inShow != nullptr && inShow->relative == "Medios del show/estrellas noche.png" && land->relative.empty(), "inside the show a file has a relative path; outside, none");
        Check(MediaAbsolutePath(inShow->relative, S(_show)) == S(P(inShow->path).lexically_normal()), "and the relative path leads back to it");
        const auto has = [](const MediaItem* m, const char* c) { return m != nullptr && std::find(m->categories.begin(), m->categories.end(), c) != m->categories.end(); };
        Check(has(land, "snow") && has(land, "forest") && land->categorySource == "name", "categories suggested from the name: snow, forest");
        Check(has(uni, "new-year") && has(tall, "animals") && has(gif, "fire") && has(inShow, "stars") && has(svg, "fantasy"), "new year, animals, fire, stars, fantasy");
        Check(Named(_lib, "blanco.png")->categories.empty() && !has(Named(_lib, "roto.png"), "stars"), "nothing from a folder called \"Carpeta con espacios\", nor from \"blanco\"");
        std::map<std::string, int> ids;
        for (const MediaItem& m : _lib.items) {
            ++ids[m.id];
        }
        Check(std::all_of(ids.begin(), ids.end(), [](const auto& kv) { return kv.second == 1; }) && land->id.rfind("m:", 0) == 0, "every file has its own id");
    }

    void TestThumbnails() {
        Title("Thumbnails, made once in the show's cache");
        int made = 0, small = 0;
        for (const MediaItem& m : _lib.items) {
            if (!m.Usable()) {
                Check(m.thumbnail.empty(), m.name + ": no thumbnail for what cannot be read");
                continue;
            }
            const fs::path t = _show / P(m.thumbnail);
            xlImage img;
            const std::string bytes = Contents(t);
            if (img.LoadFromMemory(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()) && img.IsOk()) {
                ++made;
                small += img.GetWidth() <= 160 && img.GetHeight() <= 120 ? 1 : 0;
            }
        }
        const int usable = static_cast<int>(std::count_if(_lib.items.begin(), _lib.items.end(), [](const MediaItem& m) { return m.Usable(); }));
        Check(made == usable && small == usable, fmt::format("{} of {} readable files have a thumbnail of at most 160x120, the video's from a frame", made, usable));
        const MediaItem* white = Named(_lib, "blanco.png");
        const MediaItem* black = Named(_lib, "negro.png");
        const MediaItem* video = Named(_lib, "video estrellas.mp4");
        Check(white && black && white->brightness > 0.95 && black->brightness < 0.05 && white->color == "#ffffff", "its light is measured: white 1, black 0");
        Check(video && video->brightness > 0.02 && video->brightness < 0.5, fmt::format("the night video is dark ({:.2f})", video ? video->brightness : -1));
        const fs::path cache = P(MediaCacheDir(S(_show)));
        std::error_code ec;
        const auto stamp = fs::last_write_time(cache / P(white->hash + ".png"), ec);
        MediaItem again = *white;
        MakeMediaThumbnail(again, S(_show));
        Check(fs::last_write_time(cache / P(white->hash + ".png"), ec) == stamp && again.brightness == white->brightness, "a thumbnail already made is read, not made again");
    }

    void TestPersistence() {
        Title("The library kept in the show, and a person's words kept with it");
        const std::string path = MediaLibraryPath(S(_show));
        Check(path.find("ShowComposer") != std::string::npos && P(path).filename() == "media-library.json", "in the show's Show Composer folder");
        MediaItem* land = const_cast<MediaItem*>(Named(_lib, "nieve_bosque 16x9.png"));
        land->tags = { "mi bosque", "invierno" };
        land->categories = { "snow" };
        land->categorySource = "manual";
        land->reviewed = true;
        MediaItem* pano = const_cast<MediaItem*>(Named(_lib, "panorama luces.png"));
        pano->disabled = true;
        std::string error;
        Check(SaveMediaLibrary(path, _lib, error), "saved " + error);
        MediaLibrary read;
        Check(ReadMediaLibrary(path, read, error) && read.items.size() == _lib.items.size() && read.folders.size() == 2, "read back, every file and both folders " + error);
        const MediaItem* l = read.Find(land->id);
        Check(l && l->tags == land->tags && l->categories == std::vector<std::string>{ "snow" } && l->reviewed && l->categorySource == "manual" && l->width == 640 && l->hash == land->hash && l->thumbnail == land->thumbnail, "with its tags, categories, size and thumbnail");
        const MediaItem* v = Named(read, "video estrellas.mp4");
        Check(v && v->durationMS == Named(_lib, "video estrellas.mp4")->durationMS && v->kind == MediaKind::Video, "and the video's length");
        Check(Named(read, "roto.png") && Named(read, "roto.png")->status == MediaStatus::Unreadable && Named(read, "roto.png")->problem == "decode", "and why a file cannot be used");
        const MediaLibrary rescanned = Scan(&read);
        const MediaItem* r = rescanned.Find(land->id);
        Check(r && r->tags == land->tags && r->categories == std::vector<std::string>{ "snow" } && r->categorySource == "manual" && rescanned.Find(pano->id)->disabled, "searched again: a person's tags, categories and choices stay");
        MediaLibrary garbage;
        WriteText(_root / "no-es-biblioteca.json", "{\"format\":\"otra cosa\"}");
        Check(!ReadMediaLibrary(S(_root / "no-es-biblioteca.json"), garbage, error) && error == "format", "a file that is not a library is not read as one");
        _lib = rescanned;
    }

    void TestMovedAndMissing() {
        Title("A file moved, a file gone, and finding it again");
        const MediaItem land = *Named(_lib, "nieve_bosque 16x9.png");
        const MediaItem pano = *Named(_lib, "panorama luces.png");
        std::error_code ec;
        const fs::path movedTo = _media / "movido" / "bosque renombrado.png";
        fs::create_directories(movedTo.parent_path(), ec);
        fs::rename(P(land.path), movedTo, ec);
        const fs::path outside = _root / "fuera" / "panorama luces.png";
        fs::create_directories(outside.parent_path(), ec);
        fs::rename(P(pano.path), outside, ec);
        _before.erase(land.path);
        _before.erase(pano.path);
        MediaLibrary after = Scan(&_lib);
        const MediaItem* m = after.Find(land.id);
        Check(m && m->name == "bosque renombrado.png" && m->Usable() && m->tags == land.tags && after.moved.contains(land.path), "moved and renamed: found by its contents, same id, same tags, the move written down");
        const MediaItem* gone = after.Find(pano.id);
        Check(gone && gone->status == MediaStatus::Missing && gone->problem == "missing" && gone->disabled && !gone->Usable(), "gone: still listed as missing, with what a person chose for it");
        const int found = RelocateMissingMedia(after, { S(_root / "fuera") }, S(_show));
        const MediaItem* back = after.Find(pano.id);
        Check(found == 1 && back && back->Usable() && back->path == S(outside) && back->disabled && after.moved.contains(pano.path), "looked for in another folder: found by its contents, choices kept");
        Check(RelocateMissingMedia(after, { S(_root / "fuera") }, S(_show)) == 0, "nothing more to find");
        _lib = after;
    }

    void TestCopy() {
        Title("Copying into the show, only when asked, never over a file");
        const MediaItem video = *Named(_lib, "video estrellas.mp4");
        std::string copied, error;
        Check(CopyMediaIntoShow(video, S(_show), "Medios Show Composer", copied, error) && FileExists(copied) && FileExists(video.path), "copied into the show; the original is still where it was " + error);
        Check(Contents(P(copied)) == Contents(P(video.path)) && MediaRelativePath(copied, S(_show)) == "Medios Show Composer/video estrellas.mp4", "the same bytes, at a path relative to the show");
        std::string second;
        Check(CopyMediaIntoShow(video, S(_show), "Medios Show Composer", second, error) && second == copied, "copied again: the copy already there is used, not a second one");
        const MediaItem tall = *Named(_lib, "vertical pingüino.png");
        WriteText(_show / "Medios Show Composer" / P("vertical pingüino.png"), "otro archivo de la persona");
        std::string third;
        Check(CopyMediaIntoShow(tall, S(_show), "Medios Show Composer", third, error) && P(third).filename() == P("vertical pingüino (2).png") &&
                  Contents(_show / "Medios Show Composer" / P("vertical pingüino.png")) == "otro archivo de la persona",
              "a different file with the same name is not overwritten: the copy gets a new name");
        MediaItem gone = video;
        gone.path = S(_root / "no existe.mp4");
        Check(!CopyMediaIntoShow(gone, S(_show), "Medios Show Composer", third, error) && error == "missing", "a missing file is not copied");
    }

    void TestLimits() {
        Title("Limits");
        const MediaLibrary few = Scan(nullptr, 3);
        Check(few.items.size() == 3 && HasWarning(few, "media-file-limit:3"), "no more files than the limit, and the limit is said");
        int seen = 0;
        MediaScanOptions o;
        o.showDir = S(_show);
        o.folders = { S(_media) };
        o.thumbnails = false;
        o.cancelled = [&]() { return seen >= 2; };
        o.progress = [&](int done, const std::string&) { seen = done; };
        const MediaLibrary cut = ScanMediaFolders(o);
        Check(cut.items.size() == 2 && HasWarning(cut, "media-scan-cancelled"), "a person can stop a search");
        MediaScanOptions twice;
        twice.showDir = S(_show);
        twice.folders = { S(_media), S(_media / "Carpeta con espacios"), S(_media) };
        twice.thumbnails = false;
        const MediaLibrary nested = ScanMediaFolders(twice);
        std::map<std::string, int> paths;
        for (const MediaItem& m : nested.items) {
            ++paths[m.path];
        }
        Check(nested.folders.size() == 2 && std::all_of(paths.begin(), paths.end(), [](const auto& kv) { return kv.second == 1; }), "a folder given twice, or inside another, lists each file once");
    }

    void TestUntouched() {
        Title("Nothing a person has was changed");
        int same = 0, changed = 0;
        for (const auto& [path, contents] : _before) {
            (FileExists(path) && Contents(P(path)) == contents) ? ++same : ++changed;
        }
        Check(changed == 0, fmt::format("{} files read, {} changed", same, changed));
    }
};

} // namespace

int RunMediaTest(const std::string& workDir) {
    fs::path root;
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        root = P(workDir) / fmt::format("composer-media-test-{}", i);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-media-test: cannot create %s\n", S(root).c_str());
        return 1;
    }
    Suite suite(root);
    return suite.Run();
}

} // namespace composer
