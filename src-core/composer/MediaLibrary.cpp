#include "composer/MediaLibrary.h"

#include "composer/ComposerText.h"
#include "composer/MediaAnalysis.h"
#include "media/VideoFrame.h"
#include "media/VideoReader.h"
#include "utils/ExternalHooks.h"
#include "utils/nanosvg_xl.h"
#include "utils/xlImage.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace composer {

namespace {

constexpr int THUMB_W = 160;
constexpr int THUMB_H = 120;
constexpr uint64_t HASH_SPAN = 64 * 1024;
// A picture larger than this is not decoded: it would not be a picture for lights.
constexpr uint64_t MAX_IMAGE_BYTES = 256ull * 1024 * 1024;

fs::path P(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string S(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string G(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}

std::string Native(const std::string& utf8) {
    fs::path p = P(utf8);
    p.make_preferred();
    return S(p);
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ReadBytes(const fs::path& p, std::vector<uint8_t>& out, uint64_t limit) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) {
        return false;
    }
    const std::streamoff size = f.tellg();
    if (size <= 0 || static_cast<uint64_t>(size) > limit) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), size);
    return static_cast<bool>(f);
}

std::string HashFile(const fs::path& p, uint64_t bytes) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const char* data, size_t n) {
        for (size_t i = 0; i < n; ++i) {
            h ^= static_cast<unsigned char>(data[i]);
            h *= 1099511628211ull;
        }
    };
    const std::string size = std::to_string(bytes);
    mix(size.data(), size.size());
    std::ifstream f(p, std::ios::binary);
    if (!f) {
        return {};
    }
    std::vector<char> buf(static_cast<size_t>(std::min<uint64_t>(bytes, HASH_SPAN)));
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    mix(buf.data(), static_cast<size_t>(f.gcount()));
    if (bytes > HASH_SPAN) {
        f.clear();
        f.seekg(static_cast<std::streamoff>(bytes - std::min<uint64_t>(bytes - HASH_SPAN, HASH_SPAN)));
        f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        mix(buf.data(), static_cast<size_t>(f.gcount()));
    }
    return fmt::format("{:016x}", h);
}

bool Hidden(const fs::path& p) {
    const std::string name = S(p.filename());
    if (name.empty() || name[0] == '.' || name[0] == '$' || name == "System Volume Information") {
        return true;
    }
#ifdef _WIN32
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
#else
    return false;
#endif
}

// Media formats a person may have that Show Composer cannot read: listed, never used.
const std::set<std::string>& KnownUnsupported() {
    static const std::set<std::string> s = { "webp", "tif", "tiff", "heic", "heif", "psd", "webm", "ogv", "3gp", "avif", "jxl", "ico", "raw", "cr2", "nef", "dng" };
    return s;
}

bool SamePath(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    return fs::equivalent(a, b, ec);
}

bool Under(const std::string& path, const std::string& folder) {
    const std::string p = Lower(path);
    std::string f = Lower(folder);
    while (!f.empty() && f.back() == '/') {
        f.pop_back();
    }
    return !f.empty() && (p == f || p.rfind(f + "/", 0) == 0);
}

std::string Canonical(const std::string& path) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(P(path), ec);
    if (ec) {
        c = fs::absolute(P(path), ec);
    }
    return G(c);
}

const MediaItem* FindIn(const std::vector<MediaItem>& items, const std::string& id) {
    const auto it = std::find_if(items.begin(), items.end(), [&](const MediaItem& m) { return m.id == id; });
    return it != items.end() ? &*it : nullptr;
}

void Shade(MediaItem& item, const xlImage& img) {
    double r = 0, g = 0, b = 0, w = 0;
    for (int y = 0; y < img.GetHeight(); ++y) {
        for (int x = 0; x < img.GetWidth(); ++x) {
            const double a = img.GetAlpha(x, y) / 255.0;
            r += img.GetRed(x, y) * a;
            g += img.GetGreen(x, y) * a;
            b += img.GetBlue(x, y) * a;
            w += 1.0;
        }
    }
    if (w <= 0) {
        return;
    }
    r /= w;
    g /= w;
    b /= w;
    item.brightness = std::round((0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0 * 1000.0) / 1000.0;
    item.color = fmt::format("#{:02x}{:02x}{:02x}", static_cast<int>(r), static_cast<int>(g), static_cast<int>(b));
}

void FitInto(int w, int h, int maxW, int maxH, int& outW, int& outH) {
    const double s = std::min(1.0, std::min(static_cast<double>(maxW) / std::max(1, w), static_cast<double>(maxH) / std::max(1, h)));
    outW = std::max(1, static_cast<int>(std::lround(w * s)));
    outH = std::max(1, static_cast<int>(std::lround(h * s)));
}

xlImage RasterSVG(std::vector<uint8_t> bytes, int maxW, int maxH, int* width = nullptr, int* height = nullptr) {
    bytes.push_back(0);
    NSVGimage* svg = nsvgParse(reinterpret_cast<char*>(bytes.data()), "px", 96);
    if (svg == nullptr || svg->width <= 0 || svg->height <= 0) {
        if (svg != nullptr) {
            nsvgDelete(svg);
        }
        return {};
    }
    const int w = static_cast<int>(std::lround(svg->width));
    const int h = static_cast<int>(std::lround(svg->height));
    if (width != nullptr) {
        *width = w;
        *height = h;
    }
    int tw = 0, th = 0;
    FitInto(w, h, maxW, maxH, tw, th);
    xlImage out(tw, th);
    NSVGrasterizer* rast = nsvgCreateRasterizer();
    nsvgRasterize(rast, svg, 0, 0, static_cast<float>(tw) / svg->width, out.GetData(), tw, th, tw * 4);
    nsvgDeleteRasterizer(rast);
    nsvgDelete(svg);
    return out;
}

xlImage FrameImage(const VideoFrame& f) {
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

struct Keyword {
    const char* category;
    std::vector<std::string> words;
};

const std::vector<Keyword>& Keywords() {
    // Folded (lowercase, no accents) words in Spanish and English. A word of four
    // letters or more also matches as the start of a longer word ("estrella" in
    // "estrellas"); shorter ones must match whole.
    static const std::vector<Keyword> k = {
        { "christmas", { "navidad", "navideno", "christmas", "xmas", "santa", "papa noel", "belen", "nacimiento", "regalo", "gift", "elfo", "elf", "duende", "campana", "jingle", "reyes magos", "villancico", "noel" } },
        { "snow", { "nieve", "nevado", "snow", "copo", "snowflake", "invierno", "winter", "hielo", "ice", "frozen", "helado", "escarcha", "frost", "blizzard", "ventisca" } },
        { "fire", { "fuego", "fire", "llama", "flame", "hoguera", "bonfire", "chimenea", "fireplace", "brasa", "ember", "candle", "vela", "antorcha", "torch" } },
        { "stars", { "estrella", "star", "galaxia", "galaxy", "cosmos", "luna", "moon", "noche", "night", "cielo", "sky", "constelacion", "nebula", "nebulosa", "planeta", "planet" } },
        { "forest", { "bosque", "forest", "arbol", "tree", "pino", "pine", "hoja", "leaf", "leaves", "selva", "jungle", "naturaleza", "nature", "montana", "mountain", "jardin", "garden" } },
        { "animals", { "animal", "perro", "dog", "gato", "cat", "pinguino", "penguin", "oso", "bear", "reno", "reindeer", "pajaro", "bird", "leon", "lion", "buho", "owl", "zorro", "fox", "caballo", "horse", "conejo", "rabbit", "ciervo", "deer" } },
        { "cinema", { "cine", "cinema", "movie", "pelicula", "film", "trailer", "claqueta", "clapper", "palomitas", "popcorn", "hollywood" } },
        { "adventure", { "aventura", "adventure", "pirata", "pirate", "mapa", "map", "tesoro", "treasure", "explorador", "explorer", "barco", "ship", "isla", "island", "viaje", "journey" } },
        { "fantasy", { "fantasia", "fantasy", "magia", "magic", "hada", "fairy", "castillo", "castle", "dragon", "unicornio", "unicorn", "mago", "wizard", "princesa", "princess", "encantado", "enchanted" } },
        { "halloween", { "halloween", "calabaza", "pumpkin", "fantasma", "ghost", "bruja", "witch", "murcielago", "bat", "calavera", "skull", "zombie", "arana", "spider", "monstruo", "monster", "terror", "spooky" } },
        { "new-year", { "año nuevo", "ano nuevo", "new year", "nochevieja", "fireworks", "fuegos artificiales", "countdown", "cuenta atras", "champan", "champagne", "uvas", "campanadas", "brindis" } },
        { "lights", { "luces", "luz", "lights", "light", "bokeh", "neon", "laser", "glow", "brillo", "destello", "sparkle", "glitter", "purpurina", "led" } },
        { "backgrounds", { "fondo", "background", "textura", "texture", "loop", "pattern", "patron", "gradient", "degradado", "abstracto", "abstract", "wallpaper" } },
    };
    return k;
}

} // namespace

const MediaItem* MediaLibrary::Find(const std::string& id) const {
    return FindIn(items, id);
}

MediaItem* MediaLibrary::Find(const std::string& id) {
    const auto it = std::find_if(items.begin(), items.end(), [&](const MediaItem& m) { return m.id == id; });
    return it != items.end() ? &*it : nullptr;
}

const MediaItem* MediaLibrary::FindByPath(const std::string& path) const {
    const std::string want = Lower(G(P(path)));
    const auto it = std::find_if(items.begin(), items.end(), [&](const MediaItem& m) { return Lower(G(P(m.path))) == want; });
    return it != items.end() ? &*it : nullptr;
}

const std::vector<std::string>& MediaCategories() {
    static const std::vector<std::string> c = { "christmas", "snow", "fire", "stars", "forest", "animals", "cinema", "adventure", "fantasy", "halloween", "new-year", "lights", "backgrounds", "other" };
    return c;
}

std::vector<std::string> SuggestMediaCategories(const std::string& pathText) {
    // Only the file name and its folders' names: never the contents.
    std::string folded = " ";
    for (char32_t c : text::Decode(text::Fold(pathText))) {
        folded += text::IsLetter(c) || text::IsDigit(c) ? text::Encode(c) : std::string(" ");
    }
    folded += " ";
    std::vector<std::string> words;
    {
        std::string w;
        for (char c : folded) {
            if (c == ' ') {
                if (!w.empty()) {
                    words.push_back(w);
                }
                w.clear();
            } else {
                w += c;
            }
        }
    }
    std::vector<std::string> out;
    for (const Keyword& k : Keywords()) {
        bool hit = false;
        for (const std::string& key : k.words) {
            if (key.find(' ') != std::string::npos) {
                hit = folded.find(" " + key + " ") != std::string::npos;
            } else {
                hit = std::any_of(words.begin(), words.end(), [&](const std::string& w) { return w == key || (key.size() >= 4 && w.rfind(key, 0) == 0); });
            }
            if (hit) {
                break;
            }
        }
        if (hit) {
            out.push_back(k.category);
        }
    }
    return out;
}

const char* MediaKindId(MediaKind kind) {
    switch (kind) {
    case MediaKind::Image: return "image";
    case MediaKind::Animation: return "animation";
    case MediaKind::Video: return "video";
    default: return "unsupported";
    }
}

MediaKind MediaKindFromId(const std::string& id) {
    return id == "image" ? MediaKind::Image : id == "animation" ? MediaKind::Animation : id == "video" ? MediaKind::Video : MediaKind::Unsupported;
}

const char* MediaStatusId(MediaStatus status) {
    switch (status) {
    case MediaStatus::Ok: return "ok";
    case MediaStatus::Missing: return "missing";
    case MediaStatus::Unreadable: return "unreadable";
    default: return "unsupported";
    }
}

MediaStatus MediaStatusFromId(const std::string& id) {
    return id == "ok" ? MediaStatus::Ok : id == "missing" ? MediaStatus::Missing : id == "unreadable" ? MediaStatus::Unreadable : MediaStatus::Unsupported;
}

MediaKind MediaKindOfName(const std::string& path) {
    std::string ext = Lower(S(P(path).extension()));
    if (!ext.empty() && ext[0] == '.') {
        ext.erase(0, 1);
    }
    if (ext == "gif") {
        return MediaKind::Animation;
    }
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "bmp" || ext == "svg") {
        return MediaKind::Image;
    }
    if (!ext.empty() && VideoReader::IsVideoFile(path)) {
        return MediaKind::Video;
    }
    return MediaKind::Unsupported;
}

bool IsSafeMediaFolder(const std::string& folder, std::string& reason) {
    reason.clear();
    if (folder.empty()) {
        reason = "empty";
        return false;
    }
    std::error_code ec;
    const fs::path p = P(folder);
    if (!fs::exists(p, ec)) {
        reason = "not-found";
        return false;
    }
    if (!fs::is_directory(p, ec)) {
        reason = "not-folder";
        return false;
    }
    const std::string c = Canonical(folder);
    const fs::path cp = P(c);
    if (!cp.has_relative_path() || cp.relative_path().empty() || cp == cp.root_path()) {
        reason = "root";
        return false;
    }
    std::vector<std::string> home;
    std::vector<std::string> system;
#ifdef _WIN32
    for (const char* v : { "USERPROFILE" }) {
        if (const char* e = std::getenv(v); e != nullptr && *e != '\0') {
            home.push_back(Canonical(e));
        }
    }
    for (const char* v : { "WINDIR", "SystemRoot", "ProgramFiles", "ProgramFiles(x86)", "ProgramData" }) {
        if (const char* e = std::getenv(v); e != nullptr && *e != '\0') {
            system.push_back(Canonical(e));
        }
    }
#else
    if (const char* e = std::getenv("HOME"); e != nullptr && *e != '\0') {
        home.push_back(Canonical(e));
    }
    for (const char* s : { "/System", "/usr", "/bin", "/sbin", "/etc", "/proc", "/dev", "/sys", "/var", "/Library", "/Applications", "/private" }) {
        system.push_back(s);
    }
#endif
    for (const std::string& h : home) {
        if (Lower(h) == Lower(c)) {
            reason = "home";
            return false;
        }
    }
    for (const std::string& s : system) {
        if (Under(c, s)) {
            reason = "system";
            return false;
        }
    }
    return true;
}

std::string MediaRelativePath(const std::string& path, const std::string& showDir) {
    if (path.empty() || showDir.empty()) {
        return {};
    }
    const std::string c = Canonical(path);
    const std::string show = Canonical(showDir);
    if (!Under(c, show) || Lower(c) == Lower(show)) {
        return {};
    }
    std::string rel = c.substr(show.size());
    while (!rel.empty() && rel[0] == '/') {
        rel.erase(0, 1);
    }
    return rel;
}

std::string MediaAbsolutePath(const std::string& kept, const std::string& showDir) {
    if (kept.empty()) {
        return {};
    }
    const fs::path p = P(kept);
    if (p.is_absolute() || showDir.empty()) {
        return S(p.lexically_normal());
    }
    return S((P(showDir) / p).lexically_normal());
}

MediaItem ProbeMedia(const std::string& path, const std::string& showDir) {
    MediaItem m;
    const fs::path p = P(path);
    m.path = S(p);
    m.name = S(p.filename());
    m.extension = Lower(S(p.extension()));
    if (!m.extension.empty() && m.extension[0] == '.') {
        m.extension.erase(0, 1);
    }
    m.relative = MediaRelativePath(path, showDir);
    m.kind = MediaKindOfName(path);
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) {
        m.status = MediaStatus::Missing;
        m.problem = "missing";
        return m;
    }
    m.bytes = fs::file_size(p, ec);
    // File-clock seconds: only compared with themselves, to notice a changed file.
    m.modified = std::chrono::duration_cast<std::chrono::seconds>(fs::last_write_time(p, ec).time_since_epoch()).count();
    m.hash = HashFile(p, m.bytes);
    if (m.kind == MediaKind::Unsupported) {
        m.status = MediaStatus::Unsupported;
        m.problem = "format";
        return m;
    }
    if (m.bytes == 0) {
        m.status = MediaStatus::Unreadable;
        m.problem = "empty";
        return m;
    }
    if (m.kind == MediaKind::Video) {
        VideoReader reader(m.path, 64, 64, false, true, false);
        if (!reader.IsValid() || reader.GetWidth() <= 0 || reader.GetHeight() <= 0 || reader.GetLengthMS() <= 0) {
            m.status = MediaStatus::Unreadable;
            m.problem = "decode";
            return m;
        }
        m.width = reader.GetWidth();
        m.height = reader.GetHeight();
        m.durationMS = reader.GetLengthMS();
    } else {
        std::vector<uint8_t> bytes;
        if (!ReadBytes(p, bytes, MAX_IMAGE_BYTES)) {
            m.status = MediaStatus::Unreadable;
            m.problem = m.bytes > MAX_IMAGE_BYTES ? "too-large" : "read";
            return m;
        }
        if (m.extension == "svg") {
            int w = 0, h = 0;
            if (!RasterSVG(bytes, 8, 8, &w, &h).IsOk()) {
                m.status = MediaStatus::Unreadable;
                m.problem = "decode";
                return m;
            }
            m.width = w;
            m.height = h;
        } else if (m.kind == MediaKind::Animation) {
            const AnimatedImageData gif = LoadAnimatedGIFFromMemory(bytes.data(), bytes.size());
            if (gif.frames.empty() || gif.width <= 0 || gif.height <= 0) {
                m.status = MediaStatus::Unreadable;
                m.problem = "decode";
                return m;
            }
            m.width = gif.width;
            m.height = gif.height;
            m.frames = static_cast<int>(gif.frames.size());
            if (m.frames > 1) {
                for (long t : gif.frameTimes) {
                    m.durationMS += static_cast<int>(t);
                }
            } else {
                m.kind = MediaKind::Image;
            }
        } else {
            xlImage img;
            if (!img.LoadFromMemory(bytes.data(), bytes.size()) || !img.IsOk()) {
                m.status = MediaStatus::Unreadable;
                m.problem = "decode";
                return m;
            }
            m.width = img.GetWidth();
            m.height = img.GetHeight();
        }
    }
    m.aspect = m.height > 0 ? std::round(static_cast<double>(m.width) / m.height * 1000.0) / 1000.0 : 0;
    return m;
}

std::string MediaLibraryPath(const std::string& showDir) {
    return S(P(showDir) / "ShowComposer" / "media-library.json");
}

std::string MediaCacheDir(const std::string& showDir) {
    return S(P(showDir) / "ShowComposer" / "media-cache");
}

bool MakeMediaThumbnail(MediaItem& item, const std::string& showDir) {
    if (!item.Usable() || item.hash.empty() || showDir.empty()) {
        return false;
    }
    const fs::path dir = P(MediaCacheDir(showDir));
    const fs::path file = dir / (item.hash + ".png");
    item.thumbnail = "ShowComposer/media-cache/" + item.hash + ".png";
    std::error_code ec;
    std::vector<uint8_t> bytes;
    if (fs::is_regular_file(file, ec) && ReadBytes(file, bytes, MAX_IMAGE_BYTES)) {
        xlImage cached;
        if (cached.LoadFromMemory(bytes.data(), bytes.size()) && cached.IsOk()) {
            Shade(item, cached);
            return true;
        }
    }
    xlImage thumb;
    if (item.kind == MediaKind::Video) {
        int tw = 0, th = 0;
        FitInto(item.width, item.height, THUMB_W, THUMB_H, tw, th);
        VideoReader reader(item.path, tw, th, true, false, false);
        if (reader.IsValid()) {
            // A third of the way in, but not past two seconds: past any fade from black.
            const int at = std::min(2000, std::max(0, reader.GetLengthMS() / 3));
            if (const VideoFrame* f = reader.GetNextFrame(at); f != nullptr) {
                thumb = FrameImage(*f);
            }
        }
    } else if (ReadBytes(P(item.path), bytes, MAX_IMAGE_BYTES)) {
        if (item.extension == "svg") {
            thumb = RasterSVG(bytes, THUMB_W, THUMB_H);
        } else if (item.kind == MediaKind::Animation) {
            AnimatedImageData gif = LoadAnimatedGIFFromMemory(bytes.data(), bytes.size());
            if (!gif.frames.empty()) {
                thumb = std::move(gif.frames[0]);
            }
        } else {
            thumb.LoadFromMemory(bytes.data(), bytes.size());
        }
    }
    if (!thumb.IsOk()) {
        item.thumbnail.clear();
        return false;
    }
    int tw = 0, th = 0;
    FitInto(thumb.GetWidth(), thumb.GetHeight(), THUMB_W, THUMB_H, tw, th);
    if (tw != thumb.GetWidth() || th != thumb.GetHeight()) {
        thumb.Rescale(tw, th);
    }
    Shade(item, thumb);
    std::vector<uint8_t> png;
    fs::create_directories(dir, ec);
    if (!thumb.SaveAsPNG(png)) {
        item.thumbnail.clear();
        return false;
    }
    const fs::path tmp = dir / (item.hash + ".png.tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!out) {
            fs::remove(tmp, ec);
            item.thumbnail.clear();
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        item.thumbnail.clear();
        return false;
    }
    return true;
}

bool LoadMediaPicture(const MediaItem& item, int maxSide, xlImage& out) {
    out = xlImage();
    if (!item.Usable() || item.kind == MediaKind::Video) {
        return false;
    }
    std::vector<uint8_t> bytes;
    if (!ReadBytes(P(item.path), bytes, MAX_IMAGE_BYTES)) {
        return false;
    }
    if (item.extension == "svg") {
        int w = 0, h = 0;
        if (!RasterSVG(bytes, 8, 8, &w, &h).IsOk() || w <= 0 || h <= 0) {
            return false;
        }
        const double s = static_cast<double>(maxSide) / std::max(w, h);
        out = RasterSVG(std::move(bytes), std::max(1, static_cast<int>(std::lround(w * s))), std::max(1, static_cast<int>(std::lround(h * s))));
        return out.IsOk();
    }
    if (item.kind == MediaKind::Animation) {
        AnimatedImageData gif = LoadAnimatedGIFFromMemory(bytes.data(), bytes.size());
        if (gif.frames.empty()) {
            return false;
        }
        out = std::move(gif.frames[0]);
    } else if (!out.LoadFromMemory(bytes.data(), bytes.size())) {
        return false;
    }
    if (!out.IsOk()) {
        return false;
    }
    int tw = 0, th = 0;
    FitInto(out.GetWidth(), out.GetHeight(), maxSide, maxSide, tw, th);
    if (tw != out.GetWidth() || th != out.GetHeight()) {
        out = ResampleImage(out, tw, th);
    }
    return true;
}

bool LoadMediaAnimation(const MediaItem& item, int maxSide, int limit, std::vector<xlImage>& frames, std::vector<long>& times) {
    frames.clear();
    times.clear();
    std::vector<uint8_t> bytes;
    if (!item.Usable() || item.kind != MediaKind::Animation || !ReadBytes(P(item.path), bytes, MAX_IMAGE_BYTES)) {
        return false;
    }
    AnimatedImageData gif = LoadAnimatedGIFFromMemory(bytes.data(), bytes.size());
    const size_t n = std::min(gif.frames.size(), static_cast<size_t>(std::max(1, limit)));
    for (size_t i = 0; i < n; ++i) {
        const size_t at = gif.frames.size() <= n ? i : i * gif.frames.size() / n;
        int tw = 0, th = 0;
        FitInto(gif.frames[at].GetWidth(), gif.frames[at].GetHeight(), maxSide, maxSide, tw, th);
        frames.push_back(ResampleImage(gif.frames[at], tw, th));
        times.push_back(at < gif.frameTimes.size() ? gif.frameTimes[at] : 100);
    }
    return !frames.empty();
}

namespace {

struct Walker {
    const MediaScanOptions& options;
    std::vector<std::string>& warnings;
    std::vector<std::pair<std::string, std::string>> files; // path, folder
    std::set<std::string> seen;
    std::set<std::string> skipped; // the show's own folders, never media
    int ignored = 0;
    int links = 0;
    int hidden = 0;
    bool full = false;
    bool deep = false;

    void Walk(const fs::path& dir, const std::string& folder, int depth) {
        std::error_code ec;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
        if (ec) {
            warnings.push_back(fmt::format("media-folder-unreadable:{}", S(dir)));
            return;
        }
        std::vector<fs::directory_entry> entries;
        for (; it != fs::directory_iterator(); it.increment(ec)) {
            if (ec) {
                break;
            }
            entries.push_back(*it);
        }
        std::sort(entries.begin(), entries.end(), [](const fs::directory_entry& a, const fs::directory_entry& b) { return a.path() < b.path(); });
        for (const fs::directory_entry& e : entries) {
            if (full || (options.cancelled && options.cancelled())) {
                return;
            }
            if (e.is_symlink(ec)) {
                ++links;
                continue;
            }
            if (Hidden(e.path())) {
                ++hidden;
                continue;
            }
            if (e.is_directory(ec)) {
                if (skipped.contains(Lower(G(e.path())))) {
                    continue;
                }
                if (depth >= options.maxDepth) {
                    deep = true;
                    continue;
                }
                Walk(e.path(), folder, depth + 1);
                continue;
            }
            if (!e.is_regular_file(ec)) {
                continue;
            }
            const std::string path = S(e.path());
            std::string ext = Lower(S(e.path().extension()));
            ext = !ext.empty() && ext[0] == '.' ? ext.substr(1) : ext;
            if (MediaKindOfName(path) == MediaKind::Unsupported && !KnownUnsupported().contains(ext)) {
                ++ignored;
                continue;
            }
            if (!seen.insert(Lower(Canonical(path))).second) {
                continue;
            }
            if (static_cast<int>(files.size()) >= options.maxFiles) {
                full = true;
                return;
            }
            files.emplace_back(path, folder);
        }
    }
};

void Carry(MediaItem& to, const MediaItem& from) {
    to.categories = from.categories;
    to.tags = from.tags;
    to.categorySource = from.categorySource;
    to.disabled = from.disabled;
    to.reviewed = from.reviewed;
    to.human = from.human;
    // Its key says which file version it describes: a changed file is analysed again.
    to.analysis = from.analysis;
}

void AssignIds(std::vector<MediaItem>& items) {
    std::map<std::string, int> uses;
    for (MediaItem& m : items) {
        const std::string base = "m:" + (m.hash.empty() ? std::string("0000000000000000") : m.hash);
        const int n = ++uses[base];
        m.id = n == 1 ? base : fmt::format("{}-{}", base, n);
    }
}

} // namespace

MediaLibrary ScanMediaFolders(const MediaScanOptions& options, const MediaLibrary* previous) {
    MediaLibrary lib;
    Walker w{ options, lib.warnings };
    if (!options.showDir.empty()) {
        w.skipped.insert(Lower(G(P(options.showDir) / "ShowComposer")));
        w.skipped.insert(Lower(G(P(options.showDir) / "Backup")));
    }
    std::set<std::string> folderKeys;
    for (const std::string& f : options.folders) {
        std::string reason;
        if (!IsSafeMediaFolder(f, reason)) {
            lib.warnings.push_back(fmt::format("media-folder-refused:{}:{}", reason, f));
            continue;
        }
        const std::string c = Native(Canonical(f));
        if (!folderKeys.insert(Lower(c)).second) {
            continue;
        }
        lib.folders.push_back({ c, MediaRelativePath(c, options.showDir) });
    }
    for (const MediaFolder& f : lib.folders) {
        w.Walk(P(f.path), f.path, 1);
    }
    if (w.full) {
        lib.warnings.push_back(fmt::format("media-file-limit:{}", options.maxFiles));
    }
    if (w.deep) {
        lib.warnings.push_back(fmt::format("media-depth-limit:{}", options.maxDepth));
    }
    if (w.links > 0) {
        lib.warnings.push_back(fmt::format("media-links-skipped:{}", w.links));
    }
    if (w.hidden > 0) {
        lib.warnings.push_back(fmt::format("media-hidden-skipped:{}", w.hidden));
    }
    if (w.ignored > 0) {
        lib.warnings.push_back(fmt::format("media-other-files:{}", w.ignored));
    }
    int done = 0;
    for (const auto& [path, folder] : w.files) {
        if (options.cancelled && options.cancelled()) {
            lib.warnings.push_back("media-scan-cancelled");
            break;
        }
        MediaItem m = ProbeMedia(path, options.showDir);
        m.folder = folder;
        lib.items.push_back(std::move(m));
        if (options.progress) {
            options.progress(++done, lib.items.back().name);
        }
    }
    AssignIds(lib.items);
    std::set<std::string> carried;
    for (MediaItem& m : lib.items) {
        const MediaItem* before = nullptr;
        if (previous != nullptr) {
            // At the same path it is the same file, even edited; elsewhere, the same
            // contents at a path that is gone means it was moved.
            before = previous->FindByPath(m.path);
            if (before == nullptr) {
                const auto it = std::find_if(previous->items.begin(), previous->items.end(), [&](const MediaItem& p) {
                    return !p.hash.empty() && p.hash == m.hash && p.bytes == m.bytes && !carried.contains(p.id) && !FileExists(p.path);
                });
                if (it != previous->items.end()) {
                    before = &*it;
                    lib.moved[before->path] = m.path;
                }
            }
        }
        if (before != nullptr) {
            carried.insert(before->id);
            Carry(m, *before);
        }
        if (m.categories.empty() && m.categorySource != "manual") {
            const std::string where = m.relative.empty() ? S(P(m.path).lexically_relative(P(m.folder))) : m.relative;
            m.categories = SuggestMediaCategories(where);
            m.categorySource = m.categories.empty() ? std::string() : std::string("name");
        }
        if (options.thumbnails) {
            MakeMediaThumbnail(m, options.showDir);
        }
    }
    if (previous != nullptr) {
        // A file that was in a folder still searched, and is nowhere now, stays listed so
        // what used it can say so and a person can find it.
        for (const MediaItem& p : previous->items) {
            if (carried.contains(p.id) || lib.FindByPath(p.path) != nullptr || FileExists(p.path)) {
                continue;
            }
            const bool inFolder = std::any_of(lib.folders.begin(), lib.folders.end(), [&](const MediaFolder& f) { return Under(G(P(p.path)), G(P(f.path))); });
            if (!inFolder) {
                continue;
            }
            MediaItem gone = p;
            gone.status = MediaStatus::Missing;
            gone.problem = "missing";
            if (lib.Find(gone.id) != nullptr) {
                gone.id += "-missing";
            }
            lib.items.push_back(std::move(gone));
        }
    }
    std::sort(lib.items.begin(), lib.items.end(), [](const MediaItem& a, const MediaItem& b) { return a.path < b.path; });
    return lib;
}

int RelocateMissingMedia(MediaLibrary& library, const std::vector<std::string>& folders, const std::string& showDir) {
    std::vector<MediaItem*> missing;
    for (MediaItem& m : library.items) {
        if (m.status == MediaStatus::Missing && !m.hash.empty()) {
            missing.push_back(&m);
        }
    }
    if (missing.empty()) {
        return 0;
    }
    MediaScanOptions o;
    o.showDir = showDir;
    std::vector<std::string> warnings;
    Walker w{ o, warnings };
    for (const std::string& f : folders) {
        std::string reason;
        if (IsSafeMediaFolder(f, reason)) {
            const std::string c = Native(Canonical(f));
            w.Walk(P(c), c, 1);
        }
    }
    int found = 0;
    for (const auto& [path, folder] : w.files) {
        if (library.FindByPath(path) != nullptr) {
            continue;
        }
        std::error_code ec;
        const uint64_t bytes = fs::file_size(P(path), ec);
        for (MediaItem* m : missing) {
            if (m->status != MediaStatus::Missing || m->bytes != bytes || HashFile(P(path), bytes) != m->hash) {
                continue;
            }
            library.moved[m->path] = path;
            MediaItem again = ProbeMedia(path, showDir);
            again.id = m->id;
            again.folder = folder;
            Carry(again, *m);
            again.thumbnail = m->thumbnail;
            again.brightness = m->brightness;
            again.color = m->color;
            again.analysis = m->analysis;
            *m = std::move(again);
            ++found;
            break;
        }
    }
    return found;
}

bool CopyMediaIntoShow(const MediaItem& item, const std::string& showDir, const std::string& subfolder, std::string& copied, std::string& error) {
    copied.clear();
    error.clear();
    std::error_code ec;
    if (!fs::is_regular_file(P(item.path), ec)) {
        error = "missing";
        return false;
    }
    const fs::path dir = P(showDir) / P(subfolder);
    fs::create_directories(dir, ec);
    if (ec) {
        error = "folder";
        return false;
    }
    const fs::path stem = P(item.name).stem();
    const fs::path ext = P(item.name).extension();
    for (int n = 1; n < 1000; ++n) {
        const fs::path target = dir / (n == 1 ? P(item.name) : P(S(stem) + fmt::format(" ({})", n) + S(ext)));
        if (fs::exists(target, ec)) {
            if (SamePath(target, P(item.path))) {
                copied = S(target);
                return true;
            }
            const uint64_t bytes = fs::file_size(target, ec);
            if (bytes == item.bytes && HashFile(target, bytes) == item.hash) {
                copied = S(target); // already copied once
                return true;
            }
            continue;
        }
        if (!fs::copy_file(P(item.path), target, fs::copy_options::none, ec) || ec) {
            error = "copy";
            return false;
        }
        copied = S(target);
        return true;
    }
    error = "names";
    return false;
}

namespace {

json ItemJson(const MediaItem& m) {
    json j = { { "id", m.id }, { "path", m.path }, { "relative", m.relative }, { "folder", m.folder }, { "name", m.name }, { "extension", m.extension },
               { "kind", MediaKindId(m.kind) }, { "status", MediaStatusId(m.status) }, { "bytes", m.bytes }, { "modified", m.modified },
               { "width", m.width }, { "height", m.height }, { "aspect", m.aspect }, { "hash", m.hash }, { "categories", m.categories }, { "tags", m.tags },
               { "brightness", m.brightness } };
    const auto opt = [&](const char* key, const std::string& v) {
        if (!v.empty()) {
            j[key] = v;
        }
    };
    opt("problem", m.problem);
    opt("categorySource", m.categorySource);
    opt("thumbnail", m.thumbnail);
    opt("color", m.color);
    if (m.durationMS > 0) {
        j["durationMS"] = m.durationMS;
    }
    if (m.frames > 0) {
        j["frames"] = m.frames;
    }
    if (m.disabled) {
        j["disabled"] = true;
    }
    if (m.reviewed) {
        j["reviewed"] = true;
    }
    if (m.analysis.Done() || !m.analysis.error.empty()) {
        j["analysis"] = MediaAnalysisJson(m.analysis);
    }
    if (!m.human.Empty()) {
        const MediaHumanLabels& h = m.human;
        j["human"] = { { "confirmed", h.confirmed }, { "rejected", h.rejected }, { "added", h.added }, { "excluded", h.excluded }, { "notFor", h.notFor },
                       { "complete", h.complete }, { "reviewer", h.reviewer }, { "reviewedAt", h.reviewedAt } };
    }
    return j;
}

MediaItem ItemFrom(const json& j) {
    MediaItem m;
    m.id = j.value("id", std::string());
    m.path = j.value("path", std::string());
    m.relative = j.value("relative", std::string());
    m.folder = j.value("folder", std::string());
    m.name = j.value("name", std::string());
    m.extension = j.value("extension", std::string());
    m.kind = MediaKindFromId(j.value("kind", std::string()));
    m.status = MediaStatusFromId(j.value("status", std::string()));
    m.problem = j.value("problem", std::string());
    m.bytes = j.value("bytes", uint64_t(0));
    m.modified = j.value("modified", int64_t(0));
    m.width = j.value("width", 0);
    m.height = j.value("height", 0);
    m.durationMS = j.value("durationMS", 0);
    m.frames = j.value("frames", 0);
    m.aspect = j.value("aspect", 0.0);
    m.hash = j.value("hash", std::string());
    m.categories = j.value("categories", std::vector<std::string>());
    m.tags = j.value("tags", std::vector<std::string>());
    m.categorySource = j.value("categorySource", std::string());
    m.thumbnail = j.value("thumbnail", std::string());
    m.brightness = j.value("brightness", 0.0);
    m.color = j.value("color", std::string());
    m.disabled = j.value("disabled", false);
    m.reviewed = j.value("reviewed", false);
    if (j.contains("analysis") && j["analysis"].is_object()) {
        m.analysis = MediaAnalysisFrom(j["analysis"]);
    }
    if (j.contains("human") && j["human"].is_object()) {
        const json& h = j["human"];
        m.human.confirmed = h.value("confirmed", std::vector<std::string>());
        m.human.rejected = h.value("rejected", std::vector<std::string>());
        m.human.added = h.value("added", std::vector<std::string>());
        m.human.excluded = h.value("excluded", std::vector<std::string>());
        m.human.notFor = h.value("notFor", std::vector<std::string>());
        m.human.complete = h.value("complete", false);
        m.human.reviewer = h.value("reviewer", std::string());
        m.human.reviewedAt = h.value("reviewedAt", std::string());
    }
    return m;
}

} // namespace

bool SaveMediaLibrary(const std::string& path, const MediaLibrary& library, std::string& error) {
    // Always written as the current version: what it holds now is read as such.
    json root = { { "format", "xjuanjilights-media-library" }, { "version", MediaLibrary().version } };
    json folders = json::array();
    for (const MediaFolder& f : library.folders) {
        folders.push_back({ { "path", f.path }, { "relative", f.relative } });
    }
    root["folders"] = std::move(folders);
    json items = json::array();
    for (const MediaItem& m : library.items) {
        items.push_back(ItemJson(m));
    }
    root["items"] = std::move(items);
    root["warnings"] = library.warnings;
    if (!library.moved.empty()) {
        root["moved"] = library.moved;
    }
    std::error_code ec;
    const fs::path p = P(path);
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = P(path + ".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << root.dump(2);
        if (!out) {
            error = "write";
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        error = "rename";
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

bool ReadMediaLibrary(const std::string& path, MediaLibrary& library, std::string& error) {
    library = MediaLibrary();
    std::ifstream in(P(path), std::ios::binary);
    if (!in) {
        error = "missing";
        return false;
    }
    const json root = json::parse(in, nullptr, false);
    if (root.is_discarded() || !root.is_object() || root.value("format", std::string()) != "xjuanjilights-media-library") {
        error = "format";
        return false;
    }
    library.version = root.value("version", 1);
    if (library.version > 3) {
        error = "newer";
        return false;
    }
    for (const json& f : root.value("folders", json::array())) {
        library.folders.push_back({ f.value("path", std::string()), f.value("relative", std::string()) });
    }
    for (const json& j : root.value("items", json::array())) {
        if (j.is_object()) {
            library.items.push_back(ItemFrom(j));
        }
    }
    library.warnings = root.value("warnings", std::vector<std::string>());
    library.moved = root.value("moved", std::map<std::string, std::string>());
    return true;
}

} // namespace composer
