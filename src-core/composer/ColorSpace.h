#pragma once

#include <string>

// Colour as people see it, for choosing and comparing the colours of a show. OKLab
// (Björn Ottosson, 2020) and its polar form OKLCH: lightness L 0-1, chroma C (0 grey,
// about 0.37 the most saturated sRGB colour) and hue h in degrees. Equal steps in it look
// about equally different, which HSV does not: a yellow and a blue of the same HSV value
// are far apart in lightness.
//
// xLights' values drive LEDs, not a screen; they are read as sRGB here, a convention that
// keeps "the same colour on the screen" and "the same colour in the sequence" together.
// Deterministic and free of exceptions; nothing here depends on NaN or infinity.
namespace composer {

struct Rgb {
    double r = 0, g = 0, b = 0; // 0-1, gamma-encoded sRGB
};
struct Oklab {
    double L = 0, a = 0, b = 0;
};
struct Oklch {
    double L = 0, C = 0, h = 0; // h in degrees, 0-360
};

[[nodiscard]] Oklab ToOklab(const Rgb& c);
// May fall outside 0-1: see InGamut and ToRgbInGamut.
[[nodiscard]] Rgb ToRgb(const Oklab& c);
[[nodiscard]] Oklch ToOklch(const Oklab& c);
[[nodiscard]] Oklab ToOklab(const Oklch& c);
[[nodiscard]] Oklch ToOklch(const Rgb& c);

[[nodiscard]] bool InGamut(const Rgb& c);
// The colour xLights can show nearest to `c`: the same lightness and hue with as much of
// its chroma as fits (found by halving), then clamped.
[[nodiscard]] Rgb ToRgbInGamut(const Oklch& c);
// The largest chroma that fits at this lightness and hue.
[[nodiscard]] double MaxChroma(double L, double h);
// The lightness where a hue can be most saturated (the cusp of the gamut): a deep blue, a
// light yellow. Where an LED shows a hue at its fullest.
[[nodiscard]] double CuspLightness(double h);

// "#RRGGBB" (case-insensitive, the "#" optional) and back.
[[nodiscard]] bool ParseHex(const std::string& hex, Rgb& out);
[[nodiscard]] std::string ToHex(const Rgb& c);
[[nodiscard]] std::string ToHex(const Oklch& c); // through ToRgbInGamut
[[nodiscard]] Oklch OklchOfHex(const std::string& hex); // black when unreadable

// Perceptual difference: distance in OKLab, x100 (1 is about the smallest step people
// notice side by side; 10 an obvious difference; 30 another colour).
[[nodiscard]] double DeltaE(const Oklab& x, const Oklab& y);
[[nodiscard]] double DeltaE(const std::string& hexA, const std::string& hexB);
// Hue difference in degrees, 0-180.
[[nodiscard]] double HueDistance(double h1, double h2);
[[nodiscard]] double WrapHue(double h);

// A mix of two colours in OKLCH, the hue the short way round; t 0-1.
[[nodiscard]] Oklch Mix(const Oklch& a, const Oklch& b, double t);

// Families of hue a person names: twelve of 30 degrees on OKLCH's circle, plus "white"
// and "neutral" for colours with almost no chroma. Names are the colours at their centres.
inline constexpr int HUE_FAMILIES = 12;
inline constexpr int FAMILY_WHITE = 12;
inline constexpr int FAMILY_NEUTRAL = 13;
inline constexpr int FAMILY_COUNT = 14;
inline constexpr double NEUTRAL_CHROMA = 0.045;
[[nodiscard]] int ColorFamily(const Oklch& c);
[[nodiscard]] const char* ColorFamilyId(int family);
// Warm (reds, oranges, yellows) 1, cool (cyans, blues) -1, between 0, by hue.
[[nodiscard]] double Warmth(double hue);

} // namespace composer
