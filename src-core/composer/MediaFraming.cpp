#include "composer/MediaFraming.h"

#include "composer/MediaAnalysis.h"
#include "utils/xlImage.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace composer {

namespace {

constexpr double PI = 3.14159265358979323846;

fs::path P(const std::string& utf8) {
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
}

std::string S(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return h;
}

std::string Int(double v) {
    return fmt::format("{}", static_cast<int>(std::lround(v)));
}

// A crop of the given real aspect out of a picture of aspect `ai`, centred as near the
// point (cx, cy) as it can be while staying inside. Fractions of the picture.
void Crop(double ai, double want, double cx, double cy, MediaFraming& f) {
    if (ai > want) {
        f.cropW = want / ai;
        f.cropH = 1;
    } else {
        f.cropW = 1;
        f.cropH = ai / want;
    }
    f.cropX = std::clamp(cx - f.cropW / 2, 0.0, 1.0 - f.cropW);
    f.cropY = std::clamp(cy - f.cropH / 2, 0.0, 1.0 - f.cropH);
    f.kept = f.cropW * f.cropH;
}

bool Inside(const MediaFraming& f, double x, double y) {
    return x >= f.cropX && x <= f.cropX + f.cropW && y >= f.cropY && y <= f.cropY + f.cropH;
}

void Set(MediaFraming& f, const std::string& id, const std::string& value) {
    f.settings.emplace_back(id, value);
}

double Score(const MediaFraming& f, bool wantsMotion) {
    double s = 0.45 * f.coverage + 0.35 * std::min(1.0, f.kept / 0.6) + 0.2 * (f.poiKept ? 1.0 : 0.3);
    if (f.moves && wantsMotion) {
        s += 0.05;
    }
    return std::round(std::clamp(s, 0.0, 1.0) * 1000.0) / 1000.0;
}

} // namespace

FrameTarget FrameTargetOf(const TargetInfo& t) {
    FrameTarget f;
    f.name = t.name;
    f.bufferWi = std::max(1, t.bufferWi);
    f.bufferHt = std::max(1, t.bufferHt);
    const RoleAssignment* role = t.PrimaryRole();
    f.tree = (role != nullptr && role->role == Role::MegaTree) || t.displayAs.find("Tree") != std::string::npos;
    const double bufferAspect = static_cast<double>(f.bufferWi) / f.bufferHt;
    if (f.tree) {
        double diameter = t.width;
        double height = t.height;
        if (diameter <= 0 || height <= 0) {
            // Most mega trees are a little over half as wide at the base as they are tall.
            height = 1;
            diameter = 0.6;
            f.assumptions.push_back("tree-proportions-assumed");
        }
        const double ratio = t.topRatio > 1 ? t.topRatio : 6.0;
        const double average = diameter * (1.0 + 1.0 / ratio) / 2.0;
        const double degrees = t.degrees > 0 ? std::min(360.0, t.degrees) : 360.0;
        if (t.degrees <= 0) {
            f.assumptions.push_back("tree-degrees-assumed");
        }
        const double arc = PI * average * degrees / 360.0;
        f.wraps = degrees >= 300;
        f.wrapAspect = f.wraps ? PI * average / height : 0;
        f.pixelAspect = (arc / f.bufferWi) / (height / f.bufferHt);
        // What one viewer sees of half a turn: the middle columns at their full width,
        // the sides foreshortened; between the arc and its projection.
        f.faceAspect = f.wraps ? ((0.5 + 1.0 / PI) / 2.0) * f.wrapAspect : std::min(arc, average) / height;
    } else {
        if (t.width > 0 && t.height > 0) {
            f.faceAspect = t.width / t.height;
        } else {
            f.faceAspect = bufferAspect;
            f.assumptions.push_back("square-pixels-assumed");
        }
        f.pixelAspect = f.faceAspect / bufferAspect;
    }
    return f;
}

std::string MediaFraming::DerivedName(const MediaItem& item) const {
    const std::string params = fmt::format("{}|{}|{}x{}|{:.4f},{:.4f},{:.4f},{:.4f}|{:.4f}{}", MEDIA_FRAMING_VERSION, layout, derivedWi, derivedHt, cropX, cropY, cropW, cropH, coverage,
                                           bandsAcross ? "a" : "s");
    return fmt::format("{}-{:016x}.png", item.hash.empty() ? std::string("0") : item.hash, Fnv(params));
}

std::string MediaDerivedDir(const std::string& showDir) {
    return S(P(showDir) / "ShowComposer" / "media-derived");
}

MediaFraming PlanFraming(const MediaItem& item, const FrameTarget& t, int durationMS, double energy, const std::string& role, uint64_t seed) {
    MediaFraming f;
    const double ai = item.aspect > 0 ? item.aspect : 1.0;
    const double px = item.analysis.Done() ? item.analysis.poiX : 0.5;
    const double py = item.analysis.Done() ? item.analysis.poiY : 0.5;
    const int W = t.bufferWi;
    const int H = t.bufferHt;
    // A slow camera over a still picture suits the calm parts; the strong parts cut on
    // the beat and hold the picture still.
    const bool calmRole = role == "establish" || role == "verse" || role == "bridge" || role == "ending" || role == "breath";
    const bool wantsMotion = durationMS >= 3000 && (calmRole || energy < 0.6);

    if (item.kind == MediaKind::Video) {
        const double want = t.wraps ? t.wrapAspect : t.faceAspect;
        const double r = ai / want;
        if (std::fabs(std::log(r)) < 0.08) {
            f.strategy = t.wraps ? "video-wrap" : "video-fill";
            Set(f, "Video_AspectRatio", "0");
        } else {
            Crop(ai, want, px, py, f);
            if (f.kept >= 0.5) {
                f.strategy = t.wraps ? "video-wrap" : "video-crop";
                f.constraints.push_back("cropped");
                Set(f, "Video_AspectRatio", "0");
                Set(f, "Video_CropLeft", Int(f.cropX * 100));
                Set(f, "Video_CropRight", Int((f.cropX + f.cropW) * 100));
                // Top and bottom are measured from the bottom of the frame.
                Set(f, "Video_CropTop", Int((1.0 - f.cropY) * 100));
                Set(f, "Video_CropBottom", Int((1.0 - f.cropY - f.cropH) * 100));
                f.poiKept = Inside(f, px, py);
            } else {
                f.strategy = "video-fit";
                f.kept = 1;
                f.coverage = std::min(r, 1.0 / r);
                f.cropX = f.cropY = 0;
                f.cropW = f.cropH = 1;
                f.constraints.push_back("letterboxed");
                Set(f, "Video_AspectRatio", "1");
            }
        }
        f.score = Score(f, false);
        if (t.wraps && ai < want * 0.6) {
            // A narrow video round a whole tree is stretched sideways.
            f.score = std::round(f.score * 0.6 * 1000.0) / 1000.0;
            f.constraints.push_back("stretched");
        }
        f.why = fmt::format("{} {:.2f} on {:.2f}", f.strategy, ai, want);
        return f;
    }

    if (item.kind == MediaKind::Animation) {
        const double r = ai / t.faceAspect;
        const double kept = std::min(r, 1.0 / r);
        Set(f, "LoopGIF", "1");
        if (kept >= 0.55) {
            f.strategy = "gif-fill";
            f.kept = kept;
            Set(f, "Scaling", std::fabs(std::log(r)) < 0.08 ? "Scale To Fit" : "Scale Keep Aspect Ratio Crop");
            if (std::fabs(std::log(r)) >= 0.08) {
                f.constraints.push_back("cropped");
            }
        } else {
            f.strategy = "gif-fit";
            f.coverage = kept;
            Set(f, "Scaling", "Scale Keep Aspect Ratio");
            f.constraints.push_back("letterboxed");
        }
        f.constraints.push_back("loops");
        f.score = Score(f, false);
        f.why = fmt::format("{} {:.2f} on {:.2f}", f.strategy, ai, t.faceAspect);
        return f;
    }

    // Still pictures: always through a derived picture, sized for the figure.
    f.derived = true;
    f.constraints.push_back("derived");
    Set(f, "Scaling", "No Scaling");
    if (t.tree && t.wraps) {
        if (ai >= t.wrapAspect * 0.8) {
            // A panorama goes all the way round and turns slowly.
            f.strategy = "tree-wrap";
            f.layout = "wrap";
            Crop(ai, t.wrapAspect, px, py, f);
            f.derivedWi = W;
            f.derivedHt = H;
            f.moves = durationMS >= 4000;
            Set(f, "Pictures_WrapX", "1");
            if (f.moves) {
                const double turn = std::min(1.0, durationMS / 20000.0);
                Set(f, "Pictures_Direction", "vector");
                Set(f, "PicturesXC", "0");
                Set(f, "PicturesEndXC", Int((seed % 2 == 0 ? -100.0 : 100.0) * turn));
                f.constraints.push_back("turns");
            }
        } else {
            // The same picture on two faces, so it is seen from any side; at the strong
            // moments the tree turns half way round.
            f.layout = "faces";
            Crop(ai, t.faceAspect, px, py, f);
            f.derivedWi = W;
            f.derivedHt = H;
            Set(f, "Pictures_WrapX", "1");
            if (energy >= 0.6 && durationMS >= 4000) {
                f.strategy = "tree-turn";
                f.moves = true;
                Set(f, "Pictures_Direction", "vector");
                Set(f, "PicturesXC", "0");
                Set(f, "PicturesEndXC", Int(-100.0 * std::min(0.5, durationMS / 16000.0)));
                f.constraints.push_back("turns");
            } else {
                f.strategy = "tree-faces";
            }
        }
        f.poiKept = Inside(f, px, py);
        if (f.kept < 0.999) {
            f.constraints.push_back("cropped");
        }
        f.score = Score(f, wantsMotion);
        f.why = fmt::format("{} {:.2f} on {:.2f} round the tree", f.strategy, ai, f.layout == "wrap" ? t.wrapAspect : t.faceAspect);
        return f;
    }

    const double A = t.faceAspect;
    const double r = ai / A;
    const double pa = t.pixelAspect > 0 ? t.pixelAspect : 1.0;
    // The whole picture covering the figure, in buffer pixels.
    const auto cover = [&](double& dw, double& dh) {
        if (ai >= A) {
            dh = H;
            dw = H * ai / pa;
        } else {
            dw = W;
            dh = W * pa / ai;
        }
    };
    const bool wide = r > 1.25;
    const bool tall = r < 0.8;
    std::string pick;
    if (wantsMotion && (wide || tall) && r >= 0.3 && r <= 3.5) {
        pick = wide ? "pan-x" : "pan-y";
    } else if (wantsMotion && !wide && !tall) {
        pick = role == "ending" || (seed % 3 == 2 && role != "establish") ? "zoom-out" : "zoom-in";
    } else {
        Crop(ai, A, px, py, f);
        pick = f.kept >= 0.45 ? (std::fabs(px - 0.5) > 0.1 || std::fabs(py - 0.5) > 0.1 ? "subject" : "fill") : "fit";
    }
    f.strategy = pick;
    if (pick == "fill" || pick == "subject") {
        f.layout = "cover";
        Crop(ai, A, pick == "fill" ? 0.5 : px, pick == "fill" ? 0.5 : py, f);
        f.derivedWi = W;
        f.derivedHt = H;
        f.poiKept = Inside(f, px, py);
        if (f.kept < 0.999) {
            f.constraints.push_back("cropped");
        }
    } else if (pick == "fit") {
        f.layout = "fit";
        f.cropX = f.cropY = 0;
        f.cropW = f.cropH = 1;
        f.derivedWi = W;
        f.derivedHt = H;
        f.kept = 1;
        f.coverage = std::min(r, 1.0 / r);
        f.bandsAcross = r > 1.0; // a wider picture leaves bands above and below
        f.constraints.push_back("letterboxed");
    } else {
        f.layout = "cover";
        f.moves = true;
        double dw = 0, dh = 0;
        cover(dw, dh);
        // The "vector" offsets reach a whole buffer either way: a longer panorama is
        // cut round its point of interest to three buffers.
        f.cropX = f.cropY = 0;
        f.cropW = f.cropH = 1;
        if (dw > 3.0 * W) {
            f.cropW = 3.0 * W / dw;
            f.cropX = std::clamp(px - f.cropW / 2, 0.0, 1.0 - f.cropW);
            dw = 3.0 * W;
        }
        if (dh > 3.0 * H) {
            f.cropH = 3.0 * H / dh;
            f.cropY = std::clamp(py - f.cropH / 2, 0.0, 1.0 - f.cropH);
            dh = 3.0 * H;
        }
        f.kept = f.cropW * f.cropH;
        f.derivedWi = std::max(W, static_cast<int>(std::lround(dw)));
        f.derivedHt = std::max(H, static_cast<int>(std::lround(dh)));
        const double DW = f.derivedWi, DH = f.derivedHt;
        // Where the point of interest is in the derived picture.
        const double u = std::clamp((px - f.cropX) / f.cropW, 0.0, 1.0) * DW;
        const double v = std::clamp((py - f.cropY) / f.cropH, 0.0, 1.0) * DH;
        Set(f, "Pictures_Direction", "vector");
        if (pick == "pan-x" || pick == "pan-y") {
            const bool x = pick == "pan-x";
            const double excess = (x ? DW - W : DH - H) / 2.0;
            const double span = x ? W : H;
            // Ends on the point of interest, starts from the far side, no faster than a
            // buffer every five seconds.
            double end = x ? std::clamp(DW / 2 - u, -excess, excess) : std::clamp(v - DH / 2, -excess, excess);
            double start = end >= 0 ? -excess : excess;
            if (std::fabs(end - start) < excess * 0.5) {
                start = -start;
            }
            const double travel = span * std::max(1.0, durationMS / 5000.0);
            if (std::fabs(end - start) > travel) {
                start = end + (start > end ? travel : -travel);
            }
            Set(f, x ? "PicturesXC" : "PicturesYC", Int(start / span * 100));
            Set(f, x ? "PicturesEndXC" : "PicturesEndYC", Int(end / span * 100));
            f.constraints.push_back("pans");
        } else {
            const double z = 1.0 + std::clamp(0.12 + durationMS / 60000.0, 0.12, 0.3);
            const double xz = std::clamp(z * (DW / 2 - u), -(DW * z - W) / 2, (DW * z - W) / 2);
            const double yz = std::clamp(z * (v - DH / 2), -(DH * z - H) / 2, (DH * z - H) / 2);
            const bool in = pick == "zoom-in";
            Set(f, "PicturesXC", Int(in ? 0 : xz / W * 100));
            Set(f, "PicturesYC", Int(in ? 0 : yz / H * 100));
            Set(f, "PicturesEndXC", Int(in ? xz / W * 100 : 0));
            Set(f, "PicturesEndYC", Int(in ? yz / H * 100 : 0));
            Set(f, "Pictures_StartScale", Int(in ? 100 : z * 100));
            Set(f, "Pictures_EndScale", Int(in ? z * 100 : 100));
            f.constraints.push_back("zooms");
        }
        f.poiKept = Inside(f, px, py);
    }
    // A plain fill or fit on a figure with square lights needs nothing derived: the
    // effect reads the person's own file and scales it itself, unless the file is
    // huge (every render would scale it down again).
    if ((pick == "fill" || pick == "fit") && std::fabs(std::log(pa)) < 0.08 && std::max(item.width, item.height) <= 4096) {
        f.derived = false;
        f.layout = "native";
        f.derivedWi = f.derivedHt = 0;
        std::erase(f.constraints, std::string("derived"));
        f.settings.clear();
        Set(f, "Scaling", pick == "fit" ? "Scale Keep Aspect Ratio" : std::fabs(std::log(r)) < 0.08 ? "Scale To Fit" : "Scale Keep Aspect Ratio Crop");
    }
    f.score = Score(f, wantsMotion);
    f.why = fmt::format("{} {:.2f} on {:.2f}", f.strategy, ai, A);
    return f;
}

bool MakeDerivedPicture(const MediaItem& item, const MediaFraming& f, const std::string& showDir, std::string& relative, std::string& error) {
    relative.clear();
    if (!f.derived || f.derivedWi <= 0 || f.derivedHt <= 0) {
        error = "not-derived";
        return false;
    }
    const std::string name = f.DerivedName(item);
    const fs::path dir = P(MediaDerivedDir(showDir));
    const fs::path file = dir / name;
    relative = "ShowComposer/media-derived/" + name;
    std::error_code ec;
    if (fs::is_regular_file(file, ec) && fs::file_size(file, ec) > 0) {
        return true;
    }
    const int need = std::max(f.derivedWi, f.derivedHt);
    xlImage original;
    if (!LoadMediaPicture(item, need > 400 ? 2048 : 1024, original) || !original.IsOk()) {
        error = "original-unreadable";
        return false;
    }
    const int ow = original.GetWidth();
    const int oh = original.GetHeight();
    const auto cropped = [&]() {
        const int x0 = std::clamp(static_cast<int>(std::lround(f.cropX * ow)), 0, ow - 1);
        const int y0 = std::clamp(static_cast<int>(std::lround(f.cropY * oh)), 0, oh - 1);
        const int cw = std::clamp(static_cast<int>(std::lround(f.cropW * ow)), 1, ow - x0);
        const int ch = std::clamp(static_cast<int>(std::lround(f.cropH * oh)), 1, oh - y0);
        xlImage c(cw, ch);
        for (int y = 0; y < ch; ++y) {
            std::copy_n(original.GetData() + (static_cast<size_t>(y0 + y) * ow + x0) * 4, static_cast<size_t>(cw) * 4, c.GetData() + static_cast<size_t>(y) * cw * 4);
        }
        return c;
    };
    const int W = f.derivedWi;
    const int H = f.derivedHt;
    xlImage out(W, H);
    std::fill_n(out.GetData(), static_cast<size_t>(W) * H * 4, 0);
    for (int i = 0; i < W * H; ++i) {
        out.GetData()[i * 4 + 3] = 255;
    }
    const auto paste = [&](const xlImage& src, int x0, int y0) {
        for (int y = 0; y < src.GetHeight(); ++y) {
            for (int x = 0; x < src.GetWidth(); ++x) {
                const int tx = x0 + x, ty = y0 + y;
                if (tx < 0 || ty < 0 || tx >= W || ty >= H) {
                    continue;
                }
                const uint8_t* s = src.GetData() + (static_cast<size_t>(y) * src.GetWidth() + x) * 4;
                uint8_t* d = out.GetData() + (static_cast<size_t>(ty) * W + tx) * 4;
                const double a = s[3] / 255.0;
                for (int c = 0; c < 3; ++c) {
                    d[c] = static_cast<uint8_t>(std::lround(s[c] * a));
                }
                d[3] = 255;
            }
        }
    };
    if (f.layout == "cover" || f.layout == "wrap") {
        paste(ResampleImage(cropped(), W, H), 0, 0);
        if (f.layout == "wrap") {
            // The two ends meet at the back of the tree: blend them so the seam is soft.
            const int band = std::max(1, W / 10);
            xlImage copy = out;
            for (int i = 0; i < band; ++i) {
                const double t = (i + 1.0) / (band + 1.0);
                for (int y = 0; y < H; ++y) {
                    uint8_t* d = out.GetData() + (static_cast<size_t>(y) * W + (W - band + i)) * 4;
                    const uint8_t* s = copy.GetData() + (static_cast<size_t>(y) * W + i) * 4;
                    for (int c = 0; c < 3; ++c) {
                        d[c] = static_cast<uint8_t>(std::lround(d[c] * (1 - t) + s[c] * t));
                    }
                }
            }
        }
    } else if (f.layout == "faces") {
        const xlImage face = cropped();
        const int half = W / 2;
        paste(ResampleImage(face, half, H), 0, 0);
        paste(ResampleImage(face, W - half, H), half, 0);
    } else if (f.layout == "fit") {
        // The whole picture with bands: `coverage` is the share of the figure it fills,
        // across the narrower way.
        int iw = W, ih = H;
        if (f.coverage < 1) {
            if (f.bandsAcross) {
                ih = std::max(1, static_cast<int>(std::lround(H * f.coverage)));
            } else {
                iw = std::max(1, static_cast<int>(std::lround(W * f.coverage)));
            }
        }
        paste(ResampleImage(original, iw, ih), (W - iw) / 2, (H - ih) / 2);
    } else {
        error = "layout";
        return false;
    }
    std::vector<uint8_t> png;
    if (!out.SaveAsPNG(png)) {
        error = "encode";
        return false;
    }
    fs::create_directories(dir, ec);
    const fs::path tmp = dir / (name + ".tmp");
    {
        std::ofstream o(tmp, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
        if (!o) {
            fs::remove(tmp, ec);
            error = "write";
            return false;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(tmp, ec);
        error = "write";
        return false;
    }
    return true;
}

} // namespace composer
