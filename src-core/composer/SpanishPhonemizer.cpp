#include "composer/SpanishPhonemizer.h"

#include "composer/ComposerText.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <sstream>

namespace composer {

namespace {

// ---------------------------------------------------------------- graphemes

struct Grapheme {
    std::string id;      // lowercase spelling: "a", "á", "ch", "qu", "y"...
    std::string written; // as written, case kept
    bool vowel = false;
    bool strong = false;   // a, e, o, and any vowel with a written accent
    bool accented = false; // á é í ó ú
    int elongation = 1;
};

// Read from left to right, longest first. `before` lists what must follow for the
// digraph to apply: "gu" is one sound in "guerra" but two in "agua".
struct DigraphRule {
    const char32_t* letters;
    const char32_t* before;
    const char* id;
};

const DigraphRule DIGRAPHS[] = {
    { U"ch", U"", "ch" },
    { U"ll", U"", "ll" },
    { U"rr", U"", "rr" },
    { U"qu", U"eiéí", "qu" },
    { U"gu", U"eiéí", "gu" },
};

struct VowelInfo {
    char32_t letter;
    const char* base;
    bool strong;
    bool accented;
};

// Grave and circumflex accents are not Spanish; they are read as plain vowels.
const VowelInfo VOWELS[] = {
    { U'a', "a", true, false }, { 0xE1, "a", true, true }, { 0xE0, "a", true, false }, { 0xE2, "a", true, false }, { 0xE4, "a", true, false },
    { U'e', "e", true, false }, { 0xE9, "e", true, true }, { 0xE8, "e", true, false }, { 0xEA, "e", true, false }, { 0xEB, "e", true, false },
    { U'o', "o", true, false }, { 0xF3, "o", true, true }, { 0xF2, "o", true, false }, { 0xF4, "o", true, false }, { 0xF6, "o", true, false },
    { U'i', "i", false, false }, { 0xED, "i", true, true }, { 0xEC, "i", false, false }, { 0xEE, "i", false, false }, { 0xEF, "i", false, false },
    { U'u', "u", false, false }, { 0xFA, "u", true, true }, { 0xF9, "u", false, false }, { 0xFB, "u", false, false }, { 0xFC, "u", false, false },
};

const VowelInfo* FindVowel(char32_t c) {
    for (const VowelInfo& v : VOWELS) {
        if (v.letter == c) {
            return &v;
        }
    }
    return nullptr;
}

// Letters outside Spanish spelling, read as the nearest Spanish one.
const std::map<char32_t, char32_t> FOREIGN_LETTERS = {
    { 0xE7, U's' }, // ç
    { 0xE3, U'a' }, { 0xF5, U'o' }, { 0xE5, U'a' }, { 0xF8, U'o' },
};

bool IsVowelBase(const Grapheme& g, const char* base) {
    if (!g.vowel) {
        return false;
    }
    const std::vector<char32_t> cps = text::Decode(g.id);
    const VowelInfo* v = cps.empty() ? nullptr : FindVowel(cps[0]);
    return v != nullptr ? std::string(v->base) == base : (std::string(base) == "i" && g.id == "y");
}

bool IsFrontVowel(const Grapheme* g) {
    return g != nullptr && (IsVowelBase(*g, "e") || IsVowelBase(*g, "i"));
}

struct Letters {
    std::vector<char32_t> lower;
    std::vector<char32_t> written;
    std::vector<int> elongation;
};

// Lowercase letters only, with any letter repeated three times or more taken as one
// held sound. Two are left alone: Spanish spells "leer", "creer" and "zoo".
Letters ReadLetters(const std::string& word, std::vector<std::string>& warnings) {
    Letters letters;
    bool digits = false;
    for (char32_t c : text::Decode(word)) {
        if (text::IsDigit(c)) {
            digits = true;
            continue;
        }
        if (!text::IsLetter(c)) {
            continue;
        }
        char32_t lower = text::ToLower(c);
        const auto foreign = FOREIGN_LETTERS.find(lower);
        if (foreign != FOREIGN_LETTERS.end()) {
            warnings.push_back("foreign-letter");
            lower = foreign->second;
        } else if (!(lower >= U'a' && lower <= U'z') && lower != 0xF1 && FindVowel(lower) == nullptr) {
            warnings.push_back("unsupported-letter");
            continue;
        }
        letters.lower.push_back(lower);
        letters.written.push_back(c);
        letters.elongation.push_back(1);
    }
    if (digits) {
        warnings.push_back("contains-digits");
    }
    Letters collapsed;
    for (size_t i = 0; i < letters.lower.size();) {
        size_t run = 1;
        while (i + run < letters.lower.size() && letters.lower[i + run] == letters.lower[i]) {
            ++run;
        }
        if (run >= 3) {
            // A held "r" is still the trill it was written as.
            const size_t keep = letters.lower[i] == U'r' ? 2 : 1;
            for (size_t k = 0; k < keep; ++k) {
                collapsed.lower.push_back(letters.lower[i]);
                collapsed.written.push_back(letters.written[i]);
                collapsed.elongation.push_back(k + 1 == keep ? static_cast<int>(run - keep + 1) : 1);
            }
        } else {
            for (size_t k = 0; k < run; ++k) {
                collapsed.lower.push_back(letters.lower[i + k]);
                collapsed.written.push_back(letters.written[i + k]);
                collapsed.elongation.push_back(1);
            }
        }
        i += run;
    }
    return collapsed;
}

std::vector<Grapheme> DetectGraphemes(const Letters& letters) {
    std::vector<Grapheme> graphemes;
    const auto& lower = letters.lower;
    for (size_t i = 0; i < lower.size();) {
        bool matched = false;
        for (const DigraphRule& rule : DIGRAPHS) {
            const std::u32string pattern(rule.letters);
            if (i + pattern.size() > lower.size() || !std::equal(pattern.begin(), pattern.end(), lower.begin() + static_cast<std::ptrdiff_t>(i))) {
                continue;
            }
            const std::u32string before(rule.before);
            const size_t next = i + pattern.size();
            if (!before.empty() && (next >= lower.size() || before.find(lower[next]) == std::u32string::npos)) {
                continue;
            }
            Grapheme g;
            g.id = rule.id;
            g.written = text::Encode(std::vector<char32_t>(letters.written.begin() + static_cast<std::ptrdiff_t>(i), letters.written.begin() + static_cast<std::ptrdiff_t>(next)));
            g.elongation = *std::max_element(letters.elongation.begin() + static_cast<std::ptrdiff_t>(i), letters.elongation.begin() + static_cast<std::ptrdiff_t>(next));
            graphemes.push_back(std::move(g));
            i = next;
            matched = true;
            break;
        }
        if (matched) {
            continue;
        }
        Grapheme g;
        g.id = text::Encode(lower[i]);
        g.written = text::Encode(letters.written[i]);
        g.elongation = letters.elongation[i];
        if (const VowelInfo* v = FindVowel(lower[i])) {
            g.vowel = true;
            g.strong = v->strong;
            g.accented = v->accented;
        }
        graphemes.push_back(std::move(g));
        ++i;
    }
    // "y" is a vowel alone ("y") and closing a word after a vowel ("hoy", "muy").
    for (size_t i = 0; i < graphemes.size(); ++i) {
        if (graphemes[i].id != "y") {
            continue;
        }
        const bool alone = graphemes.size() == 1;
        const bool closing = i + 1 == graphemes.size() && i > 0 && graphemes[i - 1].vowel;
        if (alone || closing) {
            graphemes[i].vowel = true;
        }
    }
    return graphemes;
}

// ------------------------------------------------------------ syllables

// Onsets that never split across syllables: "ma-dre", "ha-blar", "com-pra".
bool IsInseparable(const std::string& first, const std::string& second) {
    static const std::string R_FIRST[] = { "p", "b", "t", "d", "c", "k", "g", "f" };
    static const std::string L_FIRST[] = { "p", "b", "c", "k", "g", "f" };
    if (second == "r") {
        return std::find(std::begin(R_FIRST), std::end(R_FIRST), first) != std::end(R_FIRST);
    }
    if (second == "l") {
        return std::find(std::begin(L_FIRST), std::end(L_FIRST), first) != std::end(L_FIRST);
    }
    return false;
}

struct VowelGroup {
    int first = 0; // grapheme indices, inclusive
    int last = 0;
    int nucleus = 0;
};

// Two strong vowels, or a written accent on "i" or "u", keep vowels apart (hiatus:
// "pa-ís", "rí-o"). Anything else shares a syllable (diphthong: "cie-lo", "hue-vo").
std::vector<VowelGroup> GroupVowels(const std::vector<Grapheme>& g) {
    std::vector<VowelGroup> groups;
    for (int i = 0; i < static_cast<int>(g.size()); ++i) {
        if (!g[i].vowel) {
            continue;
        }
        const bool continues = !groups.empty() && groups.back().last == i - 1;
        const bool accentedWeak = g[i].accented && (IsVowelBase(g[i], "i") || IsVowelBase(g[i], "u"));
        const bool previousAccentedWeak = continues && g[i - 1].accented && (IsVowelBase(g[i - 1], "i") || IsVowelBase(g[i - 1], "u"));
        const bool hiatus = continues && ((g[i].strong && g[i - 1].strong) || accentedWeak || previousAccentedWeak);
        if (continues && !hiatus) {
            groups.back().last = i;
        } else {
            groups.push_back({ i, i, i });
        }
    }
    for (VowelGroup& group : groups) {
        int nucleus = group.last; // two weak vowels: the second carries it ("cui-da-do")
        for (int i = group.first; i <= group.last; ++i) {
            if (g[i].strong) {
                nucleus = i;
                break;
            }
        }
        group.nucleus = nucleus;
    }
    return groups;
}

// Where each syllable starts, as a grapheme index.
std::vector<int> SyllableStarts(const std::vector<Grapheme>& g, const std::vector<VowelGroup>& groups) {
    std::vector<int> starts;
    if (groups.empty()) {
        return starts;
    }
    starts.push_back(0);
    for (size_t k = 1; k < groups.size(); ++k) {
        const int from = groups[k - 1].last + 1;
        const int to = groups[k].first; // exclusive
        const int consonants = to - from;
        int split = to;
        if (consonants == 1) {
            split = from;
        } else if (consonants == 2) {
            split = IsInseparable(g[from].id, g[from + 1].id) ? from : from + 1;
        } else if (consonants == 3) {
            split = IsInseparable(g[from + 1].id, g[from + 2].id) ? from + 1 : from + 2;
        } else if (consonants >= 4) {
            split = to - 2;
        }
        starts.push_back(split);
    }
    return starts;
}

// ------------------------------------------------------------ phonemes

enum class When {
    Always,
    BeforeFrontVowel,
    WordInitial,
    WordInitialOrAfterLNS,
};

// First matching rule wins. "$theta" and "$ll" depend on the dialect.
struct PhoneRule {
    const char* grapheme;
    When when;
    const char* phones;
};

const PhoneRule PHONE_RULES[] = {
    { "c", When::BeforeFrontVowel, "$theta" },
    { "c", When::Always, "k" },
    { "z", When::Always, "$theta" },
    { "g", When::BeforeFrontVowel, "x" },
    { "g", When::Always, "g" },
    { "gu", When::Always, "g" },
    { "qu", When::Always, "k" },
    { "q", When::Always, "k" },
    { "k", When::Always, "k" },
    { "r", When::WordInitialOrAfterLNS, "rr" },
    { "r", When::Always, "r" },
    { "rr", When::Always, "rr" },
    { "ll", When::Always, "$ll" },
    { "y", When::Always, "jj" },
    { "x", When::WordInitial, "s" },
    { "x", When::Always, "k s" },
    { "h", When::Always, "" },
    { "ch", When::Always, "tS" },
    { "\xC3\xB1", When::Always, "J" }, // ñ
    { "j", When::Always, "x" },
    { "v", When::Always, "b" },
    { "w", When::Always, "w" },
    { "b", When::Always, "b" },
    { "d", When::Always, "d" },
    { "f", When::Always, "f" },
    { "l", When::Always, "l" },
    { "m", When::Always, "m" },
    { "n", When::Always, "n" },
    { "p", When::Always, "p" },
    { "s", When::Always, "s" },
    { "t", When::Always, "t" },
};

bool Applies(When when, const std::vector<Grapheme>& g, size_t i) {
    const Grapheme* next = i + 1 < g.size() ? &g[i + 1] : nullptr;
    const Grapheme* previous = i > 0 ? &g[i - 1] : nullptr;
    switch (when) {
    case When::Always:
        return true;
    case When::BeforeFrontVowel:
        return IsFrontVowel(next);
    case When::WordInitial:
        return i == 0;
    case When::WordInitialOrAfterLNS:
        return i == 0 || (previous != nullptr && (previous->id == "l" || previous->id == "n" || previous->id == "s"));
    }
    return false;
}

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    std::string token;
    while (in >> token) {
        out.push_back(token);
    }
    return out;
}

std::vector<std::string> ConsonantPhones(const std::vector<Grapheme>& g, size_t i, const PhonemizerOptions& options) {
    for (const PhoneRule& rule : PHONE_RULES) {
        if (g[i].id != rule.grapheme || !Applies(rule.when, g, i)) {
            continue;
        }
        std::string phones = rule.phones;
        if (phones == "$theta") {
            phones = options.dialect == SpanishDialect::Distinction ? "T" : "s";
        } else if (phones == "$ll") {
            phones = options.yeismo ? "jj" : "L";
        }
        return Split(phones);
    }
    return {};
}

std::string VowelPhone(const Grapheme& g, bool nucleus) {
    for (const char* base : { "a", "e", "o" }) {
        if (IsVowelBase(g, base)) {
            return base;
        }
    }
    if (IsVowelBase(g, "i")) {
        return nucleus ? "i" : "j";
    }
    return nucleus ? "u" : "w";
}

// ------------------------------------------------------------ exceptions

// Words whose sound the spelling does not decide. Keyed by lowercase spelling with
// accents. Bump SPANISH_EXCEPTIONS_VERSION whenever an entry changes.
std::map<std::string, std::string>& Exceptions() {
    static std::map<std::string, std::string> table = {
        { "m\xC3\xA9xico", "'m e . x i . k o" },
        { "mexicano", "m e . x i . 'k a . n o" },
        { "mexicana", "m e . x i . 'k a . n a" },
        { "oaxaca", "w a . 'x a . k a" },
        { "texas", "'t e . x a s" },
        { "xavier", "x a . 'b j e r" },
    };
    return table;
}

std::mutex& ExceptionsLock() {
    static std::mutex lock;
    return lock;
}

bool FromException(const std::string& key, Pronunciation& p) {
    std::string entry;
    {
        std::lock_guard<std::mutex> guard(ExceptionsLock());
        const auto found = Exceptions().find(key);
        if (found == Exceptions().end()) {
            return false;
        }
        entry = found->second;
    }
    int syllable = 0;
    Syllable current;
    const auto close = [&]() {
        if (current.phoneCount > 0) {
            p.syllables.push_back(current);
            ++syllable;
        }
        current = Syllable();
        current.firstPhone = static_cast<int>(p.phones.size());
    };
    current.firstPhone = 0;
    for (std::string token : Split(entry)) {
        if (token == ".") {
            close();
            continue;
        }
        if (token[0] == '\'') {
            current.stressed = true;
            p.stressedSyllable = syllable;
            token = token.substr(1);
        }
        p.phones.push_back({ token, syllable });
        if (token == "a" || token == "e" || token == "i" || token == "o" || token == "u") {
            current.nucleus = static_cast<int>(p.phones.size()) - 1;
        }
        current.text += token;
        ++current.phoneCount;
    }
    close();
    p.source = "exceptions";
    return true;
}

// ------------------------------------------------------------ weights

bool IsVowelPhone(const std::string& id) {
    return id == "a" || id == "e" || id == "i" || id == "o" || id == "u";
}

void Weigh(Pronunciation& p) {
    for (Syllable& s : p.syllables) {
        double weight = 0;
        for (int i = s.firstPhone; i < s.firstPhone + s.phoneCount; ++i) {
            const std::string& id = p.phones[i].id;
            if (i == s.nucleus) {
                weight += 1.0 * s.elongation;
            } else if (id == "j" || id == "w") {
                weight += 0.15;
            } else {
                weight += i < s.nucleus ? 0.25 : 0.3;
            }
        }
        if (s.stressed) {
            weight *= 1.2;
        }
        s.weight = weight;
    }
}

} // namespace

Pronunciation PhonemizeSpanish(const std::string& word, const PhonemizerOptions& options) {
    Pronunciation p;
    p.word = word;
    const Letters letters = ReadLetters(word, p.warnings);
    p.normalized = text::Encode(letters.lower);
    if (letters.lower.empty()) {
        p.source = "none";
        return p;
    }
    if (FromException(p.normalized, p)) {
        Weigh(p);
        return p;
    }

    const std::vector<Grapheme> g = DetectGraphemes(letters);
    const std::vector<VowelGroup> groups = GroupVowels(g);
    if (groups.empty()) {
        // "pss", "mmm": letters but no syllable to sing.
        p.warnings.push_back("no-vowel");
        p.source = "none";
        return p;
    }
    const std::vector<int> starts = SyllableStarts(g, groups);
    std::vector<int> syllableOf(g.size(), 0);
    for (size_t s = 0; s < starts.size(); ++s) {
        const int end = s + 1 < starts.size() ? starts[s + 1] : static_cast<int>(g.size());
        for (int i = starts[s]; i < end; ++i) {
            syllableOf[i] = static_cast<int>(s);
        }
    }

    // Written accent first; otherwise words ending in a vowel, "n" or "s" stress the
    // second to last syllable, and the rest the last one.
    int stressed = -1;
    for (size_t i = 0; i < g.size() && stressed < 0; ++i) {
        if (g[i].accented) {
            stressed = syllableOf[i];
        }
    }
    if (stressed < 0) {
        const Grapheme& last = g.back();
        const bool penultimate = (last.vowel && last.id != "y") || last.id == "n" || last.id == "s";
        stressed = groups.size() >= 2 && penultimate ? static_cast<int>(groups.size()) - 2 : static_cast<int>(groups.size()) - 1;
    }
    p.stressedSyllable = stressed;

    p.syllables.resize(groups.size());
    for (size_t s = 0; s < groups.size(); ++s) {
        p.syllables[s].stressed = static_cast<int>(s) == stressed;
    }
    for (size_t i = 0; i < g.size(); ++i) {
        Syllable& syllable = p.syllables[syllableOf[i]];
        if (syllable.phoneCount == 0 && syllable.text.empty()) {
            syllable.firstPhone = static_cast<int>(p.phones.size());
        }
        syllable.text += g[i].written;
        syllable.elongation = std::max(syllable.elongation, g[i].elongation);
        std::vector<std::string> phones;
        if (g[i].vowel) {
            const bool nucleus = static_cast<int>(i) == groups[syllableOf[i]].nucleus;
            phones.push_back(VowelPhone(g[i], nucleus));
        } else {
            phones = ConsonantPhones(g, i, options);
        }
        for (const std::string& id : phones) {
            if (g[i].vowel && static_cast<int>(i) == groups[syllableOf[i]].nucleus) {
                syllable.nucleus = static_cast<int>(p.phones.size());
            }
            p.phones.push_back({ id, syllableOf[i] });
            ++syllable.phoneCount;
        }
    }
    p.source = "rules";
    Weigh(p);
    return p;
}

const char* VisemeOf(const std::string& id) {
    // Grouped as xLights' own phoneme_mapping groups the English phonemes, so a
    // Spanish and an English track move the same face the same way.
    static const std::map<std::string, const char*> VISEMES = {
        { "a", "AI" }, { "e", "E" }, { "i", "E" }, { "o", "O" }, { "u", "U" },
        { "j", "E" }, { "w", "WQ" },
        { "p", "MBP" }, { "b", "MBP" }, { "m", "MBP" },
        { "f", "FV" },
        { "l", "L" }, { "L", "L" },
        { "t", "etc" }, { "d", "etc" }, { "k", "etc" }, { "g", "etc" }, { "T", "etc" }, { "s", "etc" }, { "x", "etc" },
        { "tS", "etc" }, { "jj", "etc" }, { "n", "etc" }, { "J", "etc" }, { "r", "etc" }, { "rr", "etc" },
    };
    const auto found = VISEMES.find(id);
    return found == VISEMES.end() ? "etc" : found->second;
}

std::vector<std::string> VisemesOf(const Pronunciation& p) {
    std::vector<std::string> shapes;
    for (const Phone& phone : p.phones) {
        shapes.push_back(VisemeOf(phone.id));
    }
    return shapes;
}

std::string PhonemeString(const Pronunciation& p) {
    std::string out;
    for (size_t s = 0; s < p.syllables.size(); ++s) {
        const Syllable& syllable = p.syllables[s];
        if (s > 0) {
            out += " . ";
        }
        if (syllable.stressed) {
            out += "'";
        }
        for (int i = syllable.firstPhone; i < syllable.firstPhone + syllable.phoneCount; ++i) {
            out += p.phones[i].id;
            if (i + 1 < syllable.firstPhone + syllable.phoneCount) {
                out += " ";
            }
        }
    }
    return out;
}

std::string SyllableString(const Pronunciation& p) {
    std::string out;
    for (size_t s = 0; s < p.syllables.size(); ++s) {
        if (s > 0) {
            out += "-";
        }
        std::string piece = text::LowerKey(p.syllables[s].text);
        if (p.syllables[s].stressed) {
            std::vector<char32_t> cps = text::Decode(piece);
            for (char32_t& c : cps) {
                if (c >= U'a' && c <= U'z') {
                    c -= 32;
                } else if ((c >= 0xE0 && c <= 0xFE && c != 0xF7)) {
                    c -= 32;
                }
            }
            piece = text::Encode(cps);
        }
        out += piece;
    }
    return out;
}

void AddSpanishException(const std::string& word, const std::string& phonemes) {
    std::lock_guard<std::mutex> guard(ExceptionsLock());
    Exceptions()[text::LowerKey(word)] = phonemes;
}

} // namespace composer
