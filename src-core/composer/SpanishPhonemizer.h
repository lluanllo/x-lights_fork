#pragma once

#include <string>
#include <vector>

// Spanish words to sounds, and sounds to the mouth shapes FacesEffect draws.
//
// Letters, phonemes and visemes are kept apart: "v" and "b" are one phoneme, and
// /t/, /s/ and /k/ are three phonemes that share one mouth shape. The phonemes keep
// every distinction the language makes; only VisemeOf merges them.
//
// Rules cover what the spelling decides. What it does not decide, such as the /x/
// of "México", comes from a small versioned exception table.
namespace composer {

inline constexpr int SPANISH_RULES_VERSION = 1;
inline constexpr int SPANISH_EXCEPTIONS_VERSION = 1;

enum class SpanishDialect {
    Distinction, // peninsular: "z" and "ce", "ci" are /θ/
    Seseo        // they are /s/
};

struct PhonemizerOptions {
    SpanishDialect dialect = SpanishDialect::Distinction;
    bool yeismo = true; // "ll" sounds as "y"; false keeps /ʎ/
};

// Phoneme ids are ASCII and stable:
//   vowels a e i o u, glides j w,
//   p b t d k g f T(θ) s x tS(ch) jj(ʝ, y) L(ʎ, ll) m n J(ɲ, ñ) l r(tap) rr(trill)
struct Phone {
    std::string id;
    int syllable = 0;
};

struct Syllable {
    std::string text;   // the letters as written, accents kept
    int firstPhone = 0;
    int phoneCount = 0;
    int nucleus = -1;   // index of its vowel in `phones`
    bool stressed = false;
    int elongation = 1; // "Navidaaad": the last syllable is held three times
    double weight = 0;  // relative length; only an estimate, never a duration
};

struct Pronunciation {
    std::string word;       // as given
    std::string normalized; // lowercase letters, elongations collapsed
    std::vector<Phone> phones;
    std::vector<Syllable> syllables;
    int stressedSyllable = -1;
    std::string source; // "rules", "exceptions" or "none"
    std::vector<std::string> warnings;

    [[nodiscard]] bool Ok() const { return !phones.empty(); }
};

[[nodiscard]] Pronunciation PhonemizeSpanish(const std::string& word, const PhonemizerOptions& options = {});

// The Preston Blair shape xLights draws for a phoneme: AI E FV L MBP O U WQ etc.
[[nodiscard]] const char* VisemeOf(const std::string& phoneId);

// One shape per phoneme, in order, before any timing merges them.
[[nodiscard]] std::vector<std::string> VisemesOf(const Pronunciation& pronunciation);

// "n a . b i . 'd a d": phonemes, syllables and stress, for tests and reports.
[[nodiscard]] std::string PhonemeString(const Pronunciation& pronunciation);
// "na-vi-DAD", with the stressed syllable in capitals.
[[nodiscard]] std::string SyllableString(const Pronunciation& pronunciation);

// Adds or replaces an exception for this process, as space-separated phoneme ids
// with "." between syllables and "'" before the stressed one: "'m e . x i . k o".
void AddSpanishException(const std::string& word, const std::string& phonemes);

} // namespace composer
