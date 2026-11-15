#include "composer/ColorSpace.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace composer {

namespace {

constexpr double PI = 3.14159265358979323846;

double ToLinear(double c) {
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double ToGamma(double c) {
    return c <= 0.0031308 ? 12.92 * c : 1.055 * std::pow(std::max(0.0, c), 1.0 / 2.4) - 0.055;
}

} // namespace

Oklab ToOklab(const Rgb& c) {
    const double r = ToLinear(c.r), g = ToLinear(c.g), b = ToLinear(c.b);
    const double l = std::cbrt(0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b);
    const double m = std::cbrt(0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b);
    const double s = std::cbrt(0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b);
    return { 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s, 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s,
             0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s };
}

Rgb ToRgb(const Oklab& c) {
    const double l1 = c.L + 0.3963377774 * c.a + 0.2158037573 * c.b;
    const double m1 = c.L - 0.1055613458 * c.a - 0.0638541728 * c.b;
    const double s1 = c.L - 0.0894841775 * c.a - 1.2914855480 * c.b;
    const double l = l1 * l1 * l1, m = m1 * m1 * m1, s = s1 * s1 * s1;
    const double r = 4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
    const double g = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
    const double b = -0.0041960863 * l - 0.7034186147 * m + 1.7076147010 * s;
    const auto gamma = [](double v) { return v < 0 ? -ToGamma(-v) : ToGamma(v); };
    return { gamma(r), gamma(g), gamma(b) };
}

double WrapHue(double h) {
    double w = std::fmod(h, 360.0);
    if (w < 0) {
        w += 360.0;
    }
    return w;
}

Oklch ToOklch(const Oklab& c) {
    const double C = std::sqrt(c.a * c.a + c.b * c.b);
    const double h = C < 1e-7 ? 0.0 : WrapHue(std::atan2(c.b, c.a) * 180.0 / PI);
    return { c.L, C, h };
}

Oklab ToOklab(const Oklch& c) {
    const double r = c.h * PI / 180.0;
    return { c.L, c.C * std::cos(r), c.C * std::sin(r) };
}

Oklch ToOklch(const Rgb& c) {
    return ToOklch(ToOklab(c));
}

bool InGamut(const Rgb& c) {
    constexpr double E = 1e-4;
    return c.r >= -E && c.r <= 1 + E && c.g >= -E && c.g <= 1 + E && c.b >= -E && c.b <= 1 + E;
}

double MaxChroma(double L, double h) {
    double lo = 0, hi = 0.4;
    if (InGamut(ToRgb(ToOklab(Oklch { L, hi, h })))) {
        return hi;
    }
    for (int k = 0; k < 24; ++k) {
        const double mid = 0.5 * (lo + hi);
        if (InGamut(ToRgb(ToOklab(Oklch { L, mid, h })))) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return lo;
}

double CuspLightness(double h) {
    static double cache[360] = {};
    static bool filled[360] = {};
    const int k = std::clamp(static_cast<int>(std::lround(WrapHue(h))) % 360, 0, 359);
    if (!filled[k]) {
        double bestL = 0.6, bestC = -1;
        for (int i = 0; i <= 72; ++i) {
            const double L = 0.25 + i * 0.01;
            const double c = MaxChroma(L, k);
            if (c > bestC) {
                bestC = c;
                bestL = L;
            }
        }
        cache[k] = bestL;
        filled[k] = true;
    }
    return cache[k];
}

Rgb ToRgbInGamut(const Oklch& c) {
    Oklch k { std::clamp(c.L, 0.0, 1.0), std::max(0.0, c.C), WrapHue(c.h) };
    Rgb out = ToRgb(ToOklab(k));
    if (!InGamut(out)) {
        k.C = std::min(k.C, MaxChroma(k.L, k.h));
        out = ToRgb(ToOklab(k));
    }
    out.r = std::clamp(out.r, 0.0, 1.0);
    out.g = std::clamp(out.g, 0.0, 1.0);
    out.b = std::clamp(out.b, 0.0, 1.0);
    return out;
}

bool ParseHex(const std::string& hex, Rgb& out) {
    std::string h = hex;
    if (!h.empty() && h[0] == '#') {
        h = h.substr(1);
    }
    if (h.size() != 6) {
        return false;
    }
    for (char ch : h) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            return false;
        }
    }
    const long v = std::strtol(h.c_str(), nullptr, 16);
    out = { ((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0, (v & 0xFF) / 255.0 };
    return true;
}

std::string ToHex(const Rgb& c) {
    const auto byte = [](double v) { return static_cast<int>(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    char text[8];
    std::snprintf(text, sizeof(text), "#%02X%02X%02X", byte(c.r), byte(c.g), byte(c.b));
    return text;
}

std::string ToHex(const Oklch& c) {
    return ToHex(ToRgbInGamut(c));
}

Oklch OklchOfHex(const std::string& hex) {
    Rgb c;
    return ParseHex(hex, c) ? ToOklch(c) : Oklch();
}

double DeltaE(const Oklab& x, const Oklab& y) {
    const double dl = x.L - y.L, da = x.a - y.a, db = x.b - y.b;
    return 100.0 * std::sqrt(dl * dl + da * da + db * db);
}

double DeltaE(const std::string& hexA, const std::string& hexB) {
    Rgb a, b;
    if (!ParseHex(hexA, a) || !ParseHex(hexB, b)) {
        return 0;
    }
    return DeltaE(ToOklab(a), ToOklab(b));
}

double HueDistance(double h1, double h2) {
    const double d = std::fabs(WrapHue(h1) - WrapHue(h2));
    return d > 180 ? 360 - d : d;
}

Oklch Mix(const Oklch& a, const Oklch& b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    // A grey has no hue of its own: it takes the other's, so a mix with white does not
    // swing round the circle.
    double ha = a.h, hb = b.h;
    if (a.C < NEUTRAL_CHROMA * 0.5) {
        ha = hb;
    }
    if (b.C < NEUTRAL_CHROMA * 0.5) {
        hb = ha;
    }
    double d = WrapHue(hb - ha);
    if (d > 180) {
        d -= 360;
    }
    return { a.L + (b.L - a.L) * t, a.C + (b.C - a.C) * t, WrapHue(ha + d * t) };
}

int ColorFamily(const Oklch& c) {
    if (c.C < NEUTRAL_CHROMA) {
        return c.L >= 0.8 ? FAMILY_WHITE : FAMILY_NEUTRAL;
    }
    return std::clamp(static_cast<int>(WrapHue(c.h) / 30.0), 0, HUE_FAMILIES - 1);
}

const char* ColorFamilyId(int family) {
    static const char* const IDS[FAMILY_COUNT] = { "red", "orange", "amber", "yellow", "green", "emerald", "cyan", "azure", "blue", "violet", "magenta", "pink", "white", "neutral" };
    return family >= 0 && family < FAMILY_COUNT ? IDS[family] : "unknown";
}

double Warmth(double hue) {
    return std::cos((WrapHue(hue) - 50.0) * PI / 180.0);
}

} // namespace composer
