#ifdef _MSC_VER
#pragma execution_character_set("utf-8")
#endif

#include "composer/LyricTest.h"

#include "composer/ComposerText.h"
#include "composer/GenerationValidator.h"
#include "composer/LyricAligner.h"
#include "composer/LyricDocument.h"
#include "composer/SequenceWriter.h"
#include "composer/SpanishPhonemizer.h"
#include "composer/WriterTest.h"
#include "lyrics/LRCParser.h"
#include "lyrics/LyricBreakdown.h"
#include "lyrics/PhonemeDictionary.h"
#include "lyrics/SRTParser.h"
#include "models/Model.h"
#include "render/Effect.h"
#include "render/EffectLayer.h"
#include "render/Element.h"
#include "render/FSEQFile.h"
#include "render/HeadlessRenderContext.h"
#include "render/SequenceElements.h"
#include "render/SequenceFile.h"
#include "utils/ExternalHooks.h"
#include "utils/Color.h"
#include "utils/FileUtils.h"
#include "utils/UtilFunctions.h"
#include "utils/xlImage.h"

#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace composer {

namespace {

constexpr int FRAME_MS = 25;
const std::set<std::string> SHAPES = { "AI", "E", "FV", "L", "MBP", "O", "U", "WQ", "etc", "rest" };

std::string Join(const std::vector<std::string>& items) {
    std::string out;
    for (const std::string& item : items) {
        out += (out.empty() ? "" : " ") + item;
    }
    return out;
}

long long ElapsedMS(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

// Same as the analyzer test's: lets xLights' own importer run without a window.
class TimingImportContext : public HeadlessRenderContext {
public:
    TimingElement* AddTimingElement(const std::string& name, const std::string& /*subType*/ = "") override {
        Element* element = GetSequenceElements().AddElement(name, "timing", true, false, false, false, false);
        if (element != nullptr && element->GetEffectLayerCount() == 0) {
            element->AddEffectLayer();
        }
        return dynamic_cast<TimingElement*>(element);
    }
};

std::vector<LyricCorrection> Marks(const LyricDocument& document, const std::vector<std::pair<int, int>>& times, Provenance state = Provenance::Confirmed) {
    std::vector<LyricCorrection> marks;
    size_t t = 0;
    for (const LyricLine* line : document.Lines()) {
        if (line->instrumental || t >= times.size()) {
            continue;
        }
        LyricCorrection c;
        c.level = LyricCorrection::Level::Phrase;
        c.targetId = line->id;
        c.state = state;
        c.startMS = times[t].first;
        c.endMS = times[t].second;
        marks.push_back(c);
        ++t;
    }
    return marks;
}

const AlignedWord* WordOf(const LyricAlignment& a, const std::string& key, int occurrence = 1) {
    int seen = 0;
    for (const AlignedWord& w : a.words) {
        if (w.key == key && ++seen == occurrence) {
            return &w;
        }
    }
    return nullptr;
}

class Suite {
public:
    explicit Suite(const fs::path& root) :
        _root(root) {}

    int Run() {
        TestPhonetics();
        TestVisemes();
        TestDocuments();
        TestLrcFormats();
        TestVoices();
        TestAlignmentProviders();
        TestAlignmentShapes();
        TestCorrections();
        TestValidator();
        TestLegacyBreakdown();
        TestExport();
        TestPerformance();
        printf("composer-lyrics-test: %d checks passed, %d failed (%s)\n", _passed, _failed, _root.string().c_str());
        printf("composer-lyrics-test: real song synchronization: %s\n", MUSIC_QUALITY_NOT_VALIDATED);
        return _failed == 0 ? 0 : 1;
    }

private:
    void Check(bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        fflush(stdout);
        ok ? ++_passed : ++_failed;
    }

    void Title(const char* title) {
        printf("=== %s\n", title);
        fflush(stdout);
    }

    // ------------------------------------------------------------- phonetics

    void TestPhonetics() {
        Title("Spanish phonemes and syllables, peninsular with yeísmo");
        struct Case {
            const char* word;
            const char* phonemes;
            const char* syllables;
        };
        const Case cases[] = {
            { "Navidad", "n a . b i . 'd a d", "na-vi-DAD" },
            { "Año", "'a . J o", "A-ño" },
            { "Señor", "s e . 'J o r", "se-ÑOR" },
            { "Corazón", "k o . r a . 'T o n", "co-ra-ZÓN" },
            { "Noche", "'n o . tS e", "NO-che" },
            { "Lluvia", "'jj u . b j a", "LLU-via" },
            { "Guitarra", "g i . 't a . rr a", "gui-TA-rra" },
            { "Pingüino", "p i n . 'g w i . n o", "pin-GÜI-no" },
            { "Quiero", "'k j e . r o", "QUIE-ro" },
            { "Guerra", "'g e . rr a", "GUE-rra" },
            { "Vergüenza", "b e r . 'g w e n . T a", "ver-GÜEN-za" },
            { "Carro", "'k a . rr o", "CA-rro" },
            { "Pero", "'p e . r o", "PE-ro" },
            { "Calle", "'k a . jj e", "CA-lle" },
            { "Playa", "'p l a . jj a", "PLA-ya" },
            { "Casa", "'k a . s a", "CA-sa" },
            { "Zapato", "T a . 'p a . t o", "za-PA-to" },
            { "Cielo", "'T j e . l o", "CIE-lo" },
            { "Huevo", "'w e . b o", "HUE-vo" },
            { "Ahí", "a . 'i", "a-HÍ" },
            { "País", "p a . 'i s", "pa-ÍS" },
            { "María", "m a . 'r i . a", "ma-RÍ-a" },
            { "Río", "'rr i . o", "RÍ-o" },
            { "Canción", "k a n . 'T j o n", "can-CIÓN" },
            { "Muñeco", "m u . 'J e . k o", "mu-ÑE-co" },
            { "Estrella", "e s . 't r e . jj a", "es-TRE-lla" },
            { "Chocolate", "tS o . k o . 'l a . t e", "cho-co-LA-te" },
            { "Feliz", "f e . 'l i T", "fe-LIZ" },
            { "Amor", "a . 'm o r", "a-MOR" },
            // Beyond the list: contexts the rules must tell apart.
            { "Enrique", "e n . 'rr i . k e", "en-RI-que" },
            { "Israel", "i s . rr a . 'e l", "is-ra-EL" },
            { "alrededor", "a l . rr e . d e . 'd o r", "al-re-de-DOR" },
            { "agua", "'a . g w a", "A-gua" },
            { "ciudad", "T j u . 'd a d", "ciu-DAD" },
            { "cuidado", "k w i . 'd a . d o", "cui-DA-do" },
            { "hoy", "'o j", "HOY" },
            { "muy", "'m w i", "MUY" },
            { "rey", "'rr e j", "REY" },
            { "y", "'i", "Y" },
            { "Uruguay", "u . r u . 'g w a j", "u-ru-GUAY" },
            { "taxi", "'t a . k s i", "TA-xi" },
            { "gente", "'x e n . t e", "GEN-te" },
            { "jamás", "x a . 'm a s", "ja-MÁS" },
            { "México", "'m e . x i . k o", "" },
        };
        for (const Case& c : cases) {
            const Pronunciation p = PhonemizeSpanish(c.word);
            const std::string phonemes = PhonemeString(p);
            const std::string syllables = SyllableString(p);
            const bool syllablesOk = c.syllables[0] == '\0' || syllables == c.syllables;
            Check(phonemes == c.phonemes && syllablesOk, fmt::format("{}: [{}] {} ({}, expected [{}] {})", c.word, phonemes, syllables, p.source, c.phonemes, c.syllables));
        }
        Check(PhonemizeSpanish("México").source == "exceptions" && PhonemizeSpanish("mexicano").source == "exceptions", "the /x/ of México comes from the exception table, not the rules");

        Title("Dialects");
        PhonemizerOptions seseo;
        seseo.dialect = SpanishDialect::Seseo;
        PhonemizerOptions distinguishing;
        distinguishing.yeismo = false;
        const struct {
            const char* word;
            const PhonemizerOptions* options;
            const char* phonemes;
        } dialects[] = {
            { "Corazón", &seseo, "k o . r a . 's o n" },
            { "Zapato", &seseo, "s a . 'p a . t o" },
            { "Cielo", &seseo, "'s j e . l o" },
            { "Canción", &seseo, "k a n . 's j o n" },
            { "Feliz", &seseo, "f e . 'l i s" },
            { "Vergüenza", &seseo, "b e r . 'g w e n . s a" },
            { "Lluvia", &distinguishing, "'L u . b j a" },
            { "Calle", &distinguishing, "'k a . L e" },
            { "Estrella", &distinguishing, "e s . 't r e . L a" },
            { "Playa", &distinguishing, "'p l a . jj a" },
        };
        for (const auto& d : dialects) {
            const std::string phonemes = PhonemeString(PhonemizeSpanish(d.word, *d.options));
            Check(phonemes == d.phonemes, fmt::format("{} ({}): [{}], expected [{}]", d.word, d.options == &seseo ? "seseo" : "ll distinguished from y", phonemes, d.phonemes));
        }
        Check(PhonemeString(PhonemizeSpanish("Casa", seseo)) == PhonemeString(PhonemizeSpanish("Casa")), "seseo changes nothing where there is no z or soft c");

        Title("Spelling that is not a dictionary word");
        const Pronunciation held = PhonemizeSpanish("Navidaaad");
        Check(PhonemeString(held) == "n a . b i . 'd a d" && held.normalized == "navidad", fmt::format("Navidaaad sounds like Navidad: [{}]", PhonemeString(held)));
        Check(held.syllables.size() == 3 && held.syllables[2].elongation == 3, fmt::format("and its last syllable is held three times ({})", held.syllables.empty() ? 0 : held.syllables.back().elongation));
        const Pronunciation normal = PhonemizeSpanish("Navidad");
        // Only the vowel is held, so the syllable gains two stressed vowel lengths (2 x 1.0 x 1.2)
        // while its consonants stay as they were.
        Check(std::abs(held.syllables[2].weight - normal.syllables[2].weight - 2.4) < 1e-9, fmt::format("so it weighs two vowel lengths more: {:.2f} against {:.2f}", held.syllables[2].weight, normal.syllables[2].weight));
        Check(PhonemeString(PhonemizeSpanish("leer")) == "l e . 'e r", "a doubled vowel is a hiatus, not a held note (le-er)");
        Check(PhonemeString(PhonemizeSpanish("¡Señor!")) == "s e . 'J o r", "punctuation around a word is not pronounced");
        Check(PhonemeString(PhonemizeSpanish("SEÑOR")) == "s e . 'J o r", "capitals, Ñ included, read as lowercase");
        Check(PhonemeString(PhonemizeSpanish("pa'l")) == "'p a l", "an apostrophe joins letters (pa'l)");
        const Pronunciation digits = PhonemizeSpanish("2025");
        Check(!digits.Ok() && digits.source == "none" && std::count(digits.warnings.begin(), digits.warnings.end(), "contains-digits") == 1, "digits are not invented into words: no phonemes and a warning");
        Check(!PhonemizeSpanish("mmm").Ok(), "a word with no vowel has no syllable to sing");
        AddSpanishException("Xilófono", "x i . 'l o . f o . n o");
        Check(PhonemizeSpanish("xilófono").source == "exceptions", "the exception table can be extended");
    }

    void TestVisemes() {
        Title("Phonemes to mouth shapes, checked apart from the phonemes");
        const struct {
            const char* word;
            const char* shapes;
        } cases[] = {
            { "amor", "AI MBP O etc" },
            { "pingüino", "MBP E etc etc WQ E etc O" },
            { "feliz", "FV E L E etc" },
            { "huevo", "WQ E MBP O" },
            { "lluvia", "etc U MBP E AI" },
            { "Navidad", "etc AI MBP E etc AI etc" },
            { "chocolate", "etc O etc O L AI etc E" },
        };
        for (const auto& c : cases) {
            const std::string shapes = Join(VisemesOf(PhonemizeSpanish(c.word)));
            Check(shapes == c.shapes, fmt::format("{}: {} (expected {})", c.word, shapes, c.shapes));
        }
        PhonemizerOptions distinguishing;
        distinguishing.yeismo = false;
        Check(Join(VisemesOf(PhonemizeSpanish("lluvia", distinguishing))) == "L U MBP E AI", "/ʎ/ is drawn with the tongue up, like L");
        Check(std::string(VisemeOf("b")) == "MBP" && std::string(VisemeOf("m")) == "MBP" && std::string(VisemeOf("p")) == "MBP", "b, m and p share the closed mouth, as distinct phonemes");
        Check(std::string(VisemeOf("T")) == VisemeOf("s") && PhonemizeSpanish("zapato").phones[0].id != PhonemizeSpanish("zapato", { SpanishDialect::Seseo, true }).phones[0].id,
              "θ and s look the same, but stay different phonemes");
        bool allKnown = true;
        for (const char* word : { "Navidad", "Año", "Señor", "Corazón", "Noche", "Lluvia", "Guitarra", "Pingüino", "Quiero", "Guerra", "Vergüenza", "Carro", "Pero", "Calle", "Playa", "Casa", "Zapato", "Cielo",
                                  "Huevo", "Ahí", "País", "María", "Río", "Canción", "Muñeco", "Estrella", "Chocolate", "Feliz", "Amor" }) {
            for (const std::string& shape : VisemesOf(PhonemizeSpanish(word))) {
                allKnown = allKnown && SHAPES.contains(shape) && shape != "rest";
            }
        }
        Check(allKnown, "every shape of the 29 words is one FacesEffect draws");
    }

    // ------------------------------------------------------------- documents

    void TestDocuments() {
        Title("Lyrics as written");
        const std::string text =
            "[Intro]\n"
            "\n"
            "[Estrofa 1]\n"
            "Una noche de Navidad\n"
            "Las estrellas brillarán\n"
            "\n"
            "[Estribillo]\n"
            "¡Vuelve la Navidad!\n"
            "¡Vuelve la Navidad!\n"
            "\n"
            "[Instrumental]\n"
            "\n"
            "[Voz: María]\n"
            "[Estrofa 2]\n"
            "“Canción” de amor, pa'l corazón\n"
            "(instrumental)\n";
        const LyricDocument d = ParsePlainLyrics(text);
        Check(d.blocks.size() == 5, fmt::format("5 blocks ({})", d.blocks.size()));
        if (d.blocks.size() == 5) {
            Check(d.blocks[0].kind == LyricBlockKind::Intro && d.blocks[0].lines.empty(), "an intro with no lines is kept, with no lines invented");
            Check(d.blocks[1].kind == LyricBlockKind::Verse && d.blocks[1].number == 1 && d.blocks[1].lines.size() == 2, "verse 1 with its two lines");
            Check(d.blocks[2].kind == LyricBlockKind::Chorus && d.blocks[2].label == "Estribillo", "the chorus is a chorus because it was tagged, label kept");
            Check(d.blocks[3].kind == LyricBlockKind::Instrumental && d.blocks[3].lines.empty(), "the instrumental block is kept empty");
            Check(d.blocks[4].kind == LyricBlockKind::Verse && d.blocks[4].number == 2, "verse 2");
            if (d.blocks[2].lines.size() == 2 && d.blocks[4].lines.size() == 2) {
                const LyricLine& first = d.blocks[2].lines[0];
                const LyricLine& second = d.blocks[2].lines[1];
                Check(first.text == "¡Vuelve la Navidad!" && first.id != second.id, fmt::format("repeated lines keep their text and get two ids: {} / {}", first.id, second.id));
                Check(first.words.size() == 3 && first.words[0].text == "¡Vuelve" && first.words[0].key == "vuelve" && first.words[2].key == "navidad", "words keep their text; the key drops ¡ and !");
                const LyricLine& quoted = d.blocks[4].lines[0];
                Check(quoted.text == "“Canción” de amor, pa'l corazón", "typographic quotes, accents and the apostrophe kept literally");
                Check(quoted.words.size() == 5 && quoted.words[0].key == "canción" && quoted.words[3].key == "pa'l", fmt::format("{} words, \"pa'l\" is one", quoted.words.size()));
                Check(quoted.voice == "María", "the voice tag applies to the lines after it");
                Check(d.blocks[4].lines[1].instrumental && d.blocks[4].lines[1].words.empty(), "\"(instrumental)\" marks a pause, not words");
            }
        }
        Check(d.language == "es" && d.languageConfidence > 0.5, fmt::format("language {} ({:.2f})", d.language, d.languageConfidence));
        Check(!d.HasTimes(), "plain text has no times, and none are made up");

        const LyricDocument untagged = ParsePlainLyrics("Una noche de Navidad\nLas estrellas brillarán\n\nVuelve la Navidad\nVuelve la Navidad\n");
        Check(untagged.blocks.size() == 2 && untagged.blocks[0].kind == LyricBlockKind::Unlabeled && untagged.blocks[1].kind == LyricBlockKind::Unlabeled,
              "without tags, paragraphs are neutral blocks: no chorus is guessed from repetition");
        const LyricDocument afterChorus = ParsePlainLyrics("[Estribillo]\nVuelve la Navidad\n\nOtra estrofa sin etiqueta\n");
        Check(afterChorus.blocks.size() == 2 && afterChorus.blocks[1].kind == LyricBlockKind::Unlabeled, "an untagged paragraph after a chorus does not join it");

        Title("Synced formats through xLights' own readers");
        const LyricDocument lrc = ParseLrcLyrics("[ti:Prueba]\n[00:01.50]Una noche de Navidad\n[00:04.00]Las estrellas brillarán\n[00:07.25]\n[00:09.00]Vuelve la Navidad\n");
        const auto lrcLines = lrc.Lines();
        Check(lrc.format == "lrc" && lrcLines.size() == 4, fmt::format("LRC: {} lines, metadata ignored", lrcLines.size()));
        if (lrcLines.size() == 4) {
            Check(lrcLines[0]->startMS == 1500 && lrcLines[0]->endMS == -1 && lrcLines[1]->startMS == 4000, "starts read, ends left unknown as LRC has none");
            Check(lrcLines[2]->instrumental && lrcLines[2]->startMS == 7250, "a blank timed line marks where singing stops");
        }
        const std::string srtText = "1\r\n00:00:01,000 --> 00:00:03,500\r\nUna noche\r\nde Navidad\r\n\r\n2\r\n00:00:04,000 --> 00:00:06,000\r\nLas estrellas\r\n\r\n3\r\n00:00:07,000 --> 00:00:09,000\r\nVuelve la Navidad\r\n";
        const LyricDocument srt = ParseSrtLyrics(srtText);
        const auto srtLines = srt.Lines();
        Check(srtLines.size() == 3, fmt::format("SRT with Windows line ends: {} entries", srtLines.size()));
        if (srtLines.size() == 3) {
            Check(srtLines[0]->text == "Una noche de Navidad" && srtLines[0]->startMS == 1000 && srtLines[0]->endMS == 3500, "two text lines joined, start and end read");
            Check(srtLines[2]->text == "Vuelve la Navidad", "the last subtitle is kept (xLights used to drop it)");
        }
        std::vector<std::string> raw = { "1", "00:00:01,000 --> 00:00:02,000", "uno", "", "2", "00:00:03,000 --> 00:00:04,000", "dos" };
        const std::vector<srt::Entry> entries = srt::ParseSRT(raw);
        Check(entries.size() == 2 && entries[1].text == "dos" && entries[1].startMS == 3000, "the shared SRT reader returns the final entry");
        Check(ParseLyrics(srtText).format == "srt" && ParseLyrics("[00:01.00]hola\n").format == "lrc" && ParseLyrics("hola\n").format == "text", "the format is recognised from the content");

        TimingTrackSpec track{ "Pista", { { { "Vuelve la Navidad", 1000, 3000 } }, { { "Vuelve", 1000, 1500 }, { "la", 1500, 1800 }, { "Navidad", 1800, 3000 } } } };
        const LyricDocument fromTrack = LyricsFromTrack(track);
        Check(fromTrack.Lines().size() == 1 && fromTrack.Lines()[0]->startMS == 1000 && fromTrack.Lines()[0]->words.size() == 3, "an existing track's phrases and words are read");

        const LanguageGuess english = DetectLyricLanguage("Let it snow, let it snow, let it snow\nOh the weather outside is frightful\n");
        Check(english.code == "en", fmt::format("English lyrics are recognised as {}", english.code));
        Check(DetectLyricLanguage("Lorem ipsum dolor").code == "und", "and anything else is \"und\" rather than a guess");
        const LyricDocument nothing = ParsePlainLyrics("");
        Check(nothing.Empty() && std::count(nothing.warnings.begin(), nothing.warnings.end(), "no-lyrics") == 1, "no lyrics: an empty document that says so");
    }

    // ------------------------------------------------------------- alignment

    static int Issues(const LyricDocument& d, const std::string& code) {
        return static_cast<int>(std::count_if(d.issues.begin(), d.issues.end(), [&](const LyricIssue& i) { return i.code == code; }));
    }

    static std::string Times(const LyricDocument& d) {
        std::string out;
        for (const LyricLine* l : d.Lines()) {
            out += (out.empty() ? "" : " ") + std::to_string(l->startMS);
        }
        return out;
    }

    void TestLrcFormats() {
        Title("LRC as people write it, and what is wrong in it");
        // A BOM, Windows line ends, metadata, every fraction width, and a chorus written once with two times.
        const std::string written = "\xEF\xBB\xBF[ti:Chiquetere]\r\n[ar:Seba]\r\n[al:Álbum]\r\n[length:02:14]\r\n[00:01.5]Una\r\n[00:02.25]noche de Navidad\r\n"
                                    "[00:03.125]¡Año nuevo, señor!\r\n[00:05]Cigüeña\r\n[00:08.00][00:20.00]Vuelve la canción\r\n";
        const LyricDocument d = ParseLyrics(written);
        const std::vector<const LyricLine*> lines = d.Lines();
        Check(d.format == "lrc" && Times(d) == "1500 2250 3125 5000 8000 20000", fmt::format("BOM, CRLF and tags: {} lines at {}", lines.size(), Times(d)));
        Check(d.issues.empty() && d.ErrorCount() == 0, fmt::format("a correct file has nothing to report ({} issues)", d.issues.size()));
        if (lines.size() == 6) {
            Check(lines[0]->text == "Una" && lines[2]->text == "¡Año nuevo, señor!" && lines[3]->text == "Cigüeña", "the first line survives the BOM; Spanish letters kept");
            Check(lines[4]->text == "Vuelve la canción" && lines[5]->text == "Vuelve la canción" && lines[4]->id != lines[5]->id, "two times on one line: the line twice, with two ids");
            Check(lines[2]->words.size() == 3 && lines[2]->words[0].key == "año", "words split as in any other line");
        }
        const auto shared = lrc::ParseLRC("[00:01.00][00:03.00]x\n[00:02.00]y\n");
        Check(shared.size() == 3 && shared[1].first == 3000 && shared[1].second == "x", "the reader shared with the desktop and the iPad expands the times too");

        const LyricDocument later = ParseLrcLyrics("[offset:+500]\n[00:02.00]hola\n");
        Check(Times(later) == "1500" && Issues(later, "offset-applied") == 1 && later.ErrorCount() == 0, fmt::format("[offset:+500] plays every line 500 ms earlier: {}", Times(later)));
        Check(Times(ParseLrcLyrics("[offset:-250]\r\n[00:02.00]hola\r\n")) == "2250", "and a negative offset later");
        const LyricDocument pushed = ParseLrcLyrics("[offset:3000]\n[00:01.00]antes\n[00:05.00]después\n");
        Check(Issues(pushed, "negative-time") == 1 && pushed.ErrorCount() == 1 && Times(pushed) == "2000", "an offset that leaves a line before the song: an error, not a line at 0");

        const LyricDocument negative = ParseLrcLyrics("[-00:01.00]hola\n[00:02.00]adiós\n");
        Check(Issues(negative, "negative-time") == 1 && negative.ErrorCount() == 1 && negative.issues[0].line == 1, "a negative time is an error, with its line");
        const LyricDocument malformed = ParseLrcLyrics("[00:01.00]bien\n[00:1x.00]letra\n[00:75.00]segundos\n[00:03.0000]fracción\n[00:04.00 sin cerrar\n");
        Check(Issues(malformed, "malformed-time") == 4 && malformed.ErrorCount() == 4 && Times(malformed) == "1000", fmt::format("letters, 75 seconds, four decimals and an open bracket: {} malformed", Issues(malformed, "malformed-time")));
        if (Issues(malformed, "malformed-time") == 4) {
            Check(malformed.issues[0].line == 2 && malformed.issues[3].line == 5, "each with the line it is on");
        }

        const LyricDocument unordered = ParseLrcLyrics("[00:05.00]segunda\n[00:02.00]primera\n[00:09.00]tercera\n");
        Check(Times(unordered) == "2000 5000 9000" && Issues(unordered, "out-of-order") == 1 && unordered.ErrorCount() == 0 && unordered.issues[0].repaired,
              "lines out of order: sorted, and the repair reported");
        Check(unordered.Lines()[0]->text == "primera", "the text goes with its time");
        const LyricDocument together = ParseLrcLyrics("[00:02.00]una\n[00:02.00]dos\n[00:04.00]tres\n");
        Check(Times(together) == "2000 4000" && together.Lines()[0]->text == "una dos" && Issues(together, "duplicate-time") == 1, "two lines on the same time: one line, nothing lost, reported");
        const LyricDocument pause = ParseLrcLyrics("[00:02.00]una\n[00:04.00]\n[00:04.00]dos\n");
        Check(Times(pause) == "2000 4000" && !pause.Lines()[1]->instrumental && pause.Lines()[1]->text == "dos", "a pause and a line on the same time: the line wins");

        const LyricDocument empty = ParseLrcLyrics("");
        Check(Issues(empty, "empty-file") == 1 && empty.ErrorCount() == 1, "an empty file says so");
        Check(Issues(ParseLrcLyrics(" \r\n\r\n"), "empty-file") == 1, "and so does one with only blank lines");
        const LyricDocument untimed = ParseLrcLyrics("[ti:Sin tiempos]\nhola\nadiós\n");
        Check(Issues(untimed, "no-timestamps") == 1 && untimed.ErrorCount() == 1 && Issues(untimed, "untimed-line") == 2, "an LRC with no time at all is an error, and its lines are named");
        const LyricDocument stray = ParseLrcLyrics("Créditos: alguien\n[00:01.00]hola\n");
        Check(stray.ErrorCount() == 0 && Issues(stray, "untimed-line") == 1 && stray.Lines().size() == 1, "a stray line without a time is left out, and said");
        const LyricDocument enhanced = ParseLrcLyrics("[00:01.00]<00:01.00>Hola <00:01.50>mundo\n");
        Check(enhanced.Lines().size() == 1 && enhanced.Lines()[0]->text == "Hola mundo" && Issues(enhanced, "word-times-ignored") == 1, "word times <mm:ss.xx> out of the text, and said");

        LyricDocument past = ParseLrcLyrics("[00:01.00]dentro\n[00:11.00]fuera\n[00:12.00]\n");
        CheckLyricsAgainstSong(past, 10000);
        Check(Issues(past, "time-past-song-end") == 2 && past.ErrorCount() == 1, "a line after the end of the song is an error; a pause there is not");
        Check(past.Lines().size() == 2, "the pause after the music is dropped, the line is kept for the person to see");

        // Chiquetere: a song that does not end on a frame, and a last line close to its end.
        const LyricDocument song = ParseLrcLyrics("[00:01.00]Chiquetere\n[02:06.30]Una musiquilla que suena tal que así\n");
        AlignerInput in;
        in.document = &song;
        in.durationMS = 134072;
        const LyricAlignment aligned = AlignLyrics(in);
        const TimingTrackSpec track = BuildLyricTrack(aligned, "Letra");
        const ValidationReport report = ValidateLyrics(aligned, track, in.durationMS, FRAME_MS);
        int lastEnd = 0;
        bool grid = true;
        for (const auto& layer : track.layers) {
            for (const TimingMark& m : layer) {
                lastEnd = std::max(lastEnd, m.endMS);
                grid = grid && m.endMS % FRAME_MS == 0;
            }
        }
        Check(report.ErrorCount() == 0 && lastEnd == 134050 && grid, fmt::format("song of 134072 ms: the last phrase, word and mouth shape end on the last frame ({} ms), {} errors", lastEnd, report.ErrorCount()));
    }

    static const AlignedPhrase* PhraseWith(const LyricAlignment& a, const std::string& text) {
        for (const AlignedPhrase& p : a.phrases) {
            if (p.text == text) {
                return &p;
            }
        }
        return nullptr;
    }

    void TestVoices() {
        Title("Voices: who sings or speaks each line, and only that");
        const char* PIECE = "[Narrador]\nHabía una vez una casa encendida\n\n[Voz: Ana]\nUna noche de Navidad\n\n[Voz: Luis]\nLas estrellas brillan\n\n"
                            "[Voz: Ana y Luis]\nVuelve la Navidad\n\n[Voz: Coro]\nCanta la Navidad\n";
        const LyricDocument d = ParsePlainLyrics(PIECE);
        const auto roleOf = [&](const char* id) { const VoiceDef* v = d.cast.Find(id); return v != nullptr ? std::string(VoiceRoleId(v->role)) : std::string("-"); };
        Check(d.cast.voices.size() == 4 && roleOf("v:narrador") == "narrator" && roleOf("v:ana") == "lead" && roleOf("v:luis") == "second" && roleOf("v:coro") == "chorus",
              fmt::format("four voices from the tags: narrator, lead, second and choir ({})", d.cast.voices.size()));
        const std::vector<const LyricLine*> lines = d.Lines();
        if (lines.size() == 5) {
            Check(lines[0]->delivery == Delivery::Spoken && lines[1]->delivery == Delivery::Sung, "the narrator speaks; the singers sing");
            Check(lines[3]->voices == std::vector<std::string>({ "v:ana", "v:luis" }), "\"Ana y Luis\" is a duet: both voices on the line");
            Check(lines[1]->text == "Una noche de Navidad", "the text is kept as written");
        }
        const LyricDocument talk = ParsePlainLyrics("ANA: Hola, ¿qué tal?\nLUIS: Muy bien, gracias\nDime: por qué brillas\n");
        Check(talk.cast.voices.size() == 2 && talk.Lines().size() == 3 && talk.Lines()[0]->text == "Hola, ¿qué tal?" && talk.Lines()[0]->delivery == Delivery::Spoken &&
                  talk.cast.voices[0].role == VoiceRole::Dialogue,
              "a dialogue in capitals: who speaks, what they say, spoken");
        Check(talk.Lines().size() == 3 && talk.Lines()[2]->voices.empty() && talk.Lines()[2]->text == "Dime: por qué brillas", "\"Dime: ...\" in a song is not a speaker");
        Check(ParsePlainLyrics("Una noche de Navidad\n").cast.Empty(), "lyrics with no voices: no cast, everything as before");

        AlignerInput in;
        in.document = &d;
        in.corrections = Marks(d, { { 1000, 5000 }, { 6000, 9000 }, { 10000, 13000 }, { 14000, 17000 }, { 18000, 21000 } });
        in.durationMS = 30000;
        const LyricAlignment a = AlignLyrics(in);
        const AlignedPhrase* duet = PhraseWith(a, "Vuelve la Navidad");
        Check(duet != nullptr && duet->voices.size() == 2 && duet->voiceSource == "lyrics", "the voices reach the aligned phrases");
        const AlignedPhrase* spoken = PhraseWith(a, "Había una vez una casa encendida");
        Check(spoken != nullptr && spoken->delivery == "spoken", "and so does the way they are delivered");
        // The same words sung and spoken, in phrases of the same length.
        const LyricDocument same = ParsePlainLyrics("[Narrador]\nla Navidad\n\n[Voz: Ana]\nla Navidad\n");
        AlignerInput both;
        both.document = &same;
        both.corrections = Marks(same, { { 1000, 3000 }, { 4000, 6000 } });
        const LyricAlignment sb = AlignLyrics(both);
        const AlignedWord* spokenLast = nullptr;
        const AlignedWord* sungLast = nullptr;
        for (const AlignedWord& w : sb.words) {
            if (w.key == "navidad") {
                (w.startMS < 3500 ? spokenLast : sungLast) = &w;
            }
        }
        Check(spokenLast != nullptr && sungLast != nullptr && (sungLast->endMS - sungLast->startMS) > (spokenLast->endMS - spokenLast->startMS),
              "a sung line holds its last word; a spoken one does not");

        const std::vector<TimingTrackSpec> tracks = BuildVoiceTracks(a, d.cast, "Letra");
        const auto trackOf = [&](const std::string& name) -> const TimingTrackSpec* {
            for (const TimingTrackSpec& t : tracks) {
                if (t.name == name) {
                    return &t;
                }
            }
            return nullptr;
        };
        const TimingTrackSpec* ana = trackOf("Letra - Ana");
        const TimingTrackSpec* luis = trackOf("Letra - Luis");
        Check(tracks.size() == 4 && ana != nullptr && luis != nullptr, fmt::format("one lyrics track per voice ({})", tracks.size()));
        if (ana != nullptr && luis != nullptr) {
            const auto has = [](const TimingTrackSpec& t, const std::string& label) { return std::any_of(t.layers[0].begin(), t.layers[0].end(), [&](const TimingMark& m) { return m.label == label; }); };
            Check(ana->layers[0].size() == 2 && has(*ana, "Una noche de Navidad") && has(*ana, "Vuelve la Navidad") && !has(*ana, "Las estrellas brillan"), "Ana's track: her line and the duet, nothing of Luis");
            Check(!ana->layers[2].empty() && std::all_of(ana->layers[2].begin(), ana->layers[2].end(), [&](const TimingMark& m) {
                      return std::any_of(ana->layers[0].begin(), ana->layers[0].end(), [&](const TimingMark& p) { return m.startMS >= p.startMS && m.endMS <= p.endMS; });
                  }),
                  "and her mouth shapes only inside her phrases");
            Check(luis->layers[0].size() == 2 && has(*luis, "Vuelve la Navidad"), "the duet is in both tracks");
        }

        // A person's corrections: who sings, split, join.
        AlignerInput fixed = in;
        LyricCorrection voice;
        voice.level = LyricCorrection::Level::Voice;
        voice.targetId = lines.size() == 5 ? lines[0]->id : "";
        voice.voices = { "v:ana" };
        voice.delivery = "sung";
        LyricCorrection split;
        split.level = LyricCorrection::Level::Split;
        split.targetId = lines.size() == 5 ? lines[1]->id : "";
        split.wordIndex = 2;
        LyricCorrection merge;
        merge.level = LyricCorrection::Level::Merge;
        merge.targetId = lines.size() == 5 ? lines[2]->id : "";
        fixed.corrections.push_back(voice);
        fixed.corrections.push_back(split);
        fixed.corrections.push_back(merge);
        const LyricAlignment f = AlignLyrics(fixed);
        const AlignedPhrase* given = PhraseWith(f, "Había una vez una casa encendida");
        Check(given != nullptr && given->voices == std::vector<std::string>({ "v:ana" }) && given->voiceSource == "manual" && given->delivery == "sung", "a phrase given to another voice, sung: the person wins");
        const AlignedPhrase* first = PhraseWith(f, "Una noche");
        const AlignedPhrase* second = PhraseWith(f, "de Navidad");
        Check(first != nullptr && second != nullptr && first->endMS == second->startMS && second->id == split.targetId + "~2" && first->voices == second->voices,
              "a phrase split in two before its third word: two phrases, back to back, same voice");
        const AlignedPhrase* joined = PhraseWith(f, "Las estrellas brillan Vuelve la Navidad");
        Check(joined != nullptr && joined->voices.size() == 2 && f.conflicts.empty(), "two phrases joined into one, with both voices");
        LyricSession session;
        session.corrections = fixed.corrections;
        VoiceDef renamed = d.cast.voices[1];
        renamed.name = "Ana María";
        renamed.faces = { "Carita verde" };
        VoiceDef silent = d.cast.voices[0];
        silent.facesOff = true;
        session.voices = { renamed, silent };
        LyricSession back;
        std::string error;
        Check(FromJson(ToJson(session), back, error) && back.corrections.size() == fixed.corrections.size() && back.voices.size() == 2 && back.voices[0].faces == renamed.faces &&
                  back.corrections.back().level == LyricCorrection::Level::Merge && back.corrections[back.corrections.size() - 2].wordIndex == 2,
              "voices and the new corrections are kept with the marks " + error);
        const VoiceCast cast = EffectiveCast(d.cast, back.voices);
        Check(cast.Find("v:ana")->name == "Ana María" && cast.Find("v:narrador")->facesOff && cast.voices.size() == 4, "a person's names and faces win over the tags");
        Check(VoiceTrackName("Letra", *cast.Find("v:ana")) == "Letra - Ana María", "the track of a voice is named after it");
    }

    void TestAlignmentProviders() {
        Title("Without lyrics, or without times");
        AlignerInput none;
        const LyricDocument empty = ParsePlainLyrics("");
        none.document = &empty;
        const LyricAlignment nothing = AlignLyrics(none);
        Check(nothing.quality == "none" && nothing.phonemes.empty() && nothing.words.empty(), "no lyrics: no words, no phonemes, no mouth movement");
        const TimingTrackSpec emptyTrack = BuildLyricTrack(nothing, "vacía");
        Check(emptyTrack.layers.size() == 3 && emptyTrack.layers[0].empty() && emptyTrack.layers[2].empty(), "and an empty track the rest of the show can live with");

        const LyricDocument text = ParsePlainLyrics("Una noche de Navidad\nLas estrellas brillarán\n");
        AlignerInput plain;
        plain.document = &text;
        const LyricAlignment unsynced = AlignLyrics(plain);
        Check(unsynced.quality == "unsynchronized" && unsynced.words.empty() && unsynced.phonemes.empty(), "text without times stays unsynchronized: nothing spread evenly and called right");
        Check(unsynced.phrases.size() == 2 && unsynced.phrases[0].startMS == -1, "its phrases are kept, ready to be marked");

        Title("A: an existing synced track is kept");
        TimingTrackSpec synced{ "Vendor", { { { "Vuelve la Navidad", 1000, 3000 } }, { { "Vuelve", 1000, 1475 }, { "la", 1475, 1800 }, { "Navidad", 1800, 3000 } }, { { "WQ", 1000, 1100 }, { "E", 1100, 1475 }, { "L", 1475, 1600 }, { "AI", 1600, 1800 }, { "etc", 1800, 1900 }, { "AI", 1900, 3000 } } } };
        const LyricDocument trackDoc = LyricsFromTrack(synced);
        AlignerInput a;
        a.document = &trackDoc;
        a.existingTrack = &synced;
        const LyricAlignment kept = AlignLyrics(a);
        const TimingTrackSpec rebuilt = BuildLyricTrack(kept, "Vendor");
        bool same = rebuilt.layers.size() == 3 && rebuilt.layers[0].size() == 1 && rebuilt.layers[1].size() == 3;
        for (size_t w = 0; same && w < 3; ++w) {
            same = rebuilt.layers[1][w].startMS == synced.layers[1][w].startMS && rebuilt.layers[1][w].endMS == synced.layers[1][w].endMS;
        }
        const auto shaped = std::count_if(rebuilt.layers[2].begin(), rebuilt.layers[2].end(), [](const TimingMark& m) { return m.label != "rest"; });
        Check(kept.provider == AlignmentProvider::ExistingTrack && kept.quality == "synced" && same, "words keep their times to the millisecond");
        Check(shaped == 6 && rebuilt.layers[2][0].label == "WQ", "and its own phonemes, untouched");
        Check(kept.words.size() == 3 && kept.words[0].source == "track" && kept.words[0].confidence == 1.0, "with the track as their source");

        Title("C: LRC and SRT line times as anchors, and no more");
        const LyricDocument lrc = ParseLrcLyrics("[00:01.00]Una noche de Navidad\n[00:04.00]Las estrellas brillarán\n[00:20.00]Vuelve la Navidad\n");
        AlignerInput c;
        c.document = &lrc;
        c.durationMS = 30000;
        const LyricAlignment fromLrc = AlignLyrics(c);
        Check(fromLrc.provider == AlignmentProvider::LineTimes && fromLrc.quality == "approximate", fmt::format("provider {}, quality {}", ProviderId(fromLrc.provider), fromLrc.quality));
        if (fromLrc.phrases.size() == 3) {
            Check(fromLrc.phrases[0].startMS == 1000 && fromLrc.phrases[0].endMS == 4000, "a line ends where the next begins");
            Check(fromLrc.phrases[1].endMS < 20000 && fromLrc.phrases[1].endMS > 4000, fmt::format("but not 16 s later: it ends at {} ms", fromLrc.phrases[1].endMS));
            Check(fromLrc.phrases[0].confidence <= 0.5 && fromLrc.words[0].confidence < fromLrc.phrases[0].confidence, "line confidence 0.5 at most; words lower");
        }
        Check(std::count(fromLrc.warnings.begin(), fromLrc.warnings.end(), "lrc-line-ends-estimated") == 1, "the estimated ends are reported");

        const LyricDocument srt = ParseSrtLyrics("1\n00:00:01,000 --> 00:00:03,000\nUna noche de Navidad\n\n2\n00:00:05,000 --> 00:00:07,500\nVuelve la Navidad\n");
        AlignerInput s;
        s.document = &srt;
        const LyricAlignment fromSrt = AlignLyrics(s);
        Check(fromSrt.phrases.size() == 2 && fromSrt.phrases[1].endMS == 7500 && fromSrt.phrases[1].confidence == 0.7, "SRT ends are used, at confidence 0.7");

        Title("D: experimental, and never by default");
        SongAnalysis analysis;
        analysis.durationMS = 20000;
        analysis.musicStartMS = 0;
        analysis.musicEndMS = 20000;
        analysis.vocalSegments.push_back({ 2000, 8000, 0.3, "filter:VOCALS-center-channel" });
        AlignerInput d;
        d.document = &text;
        d.analysis = &analysis;
        const LyricAlignment byDefault = AlignLyrics(d);
        Check(byDefault.quality == "unsynchronized", "with a song analysis but no request, nothing is aligned automatically");
        AlignerOptions experimental;
        experimental.allowExperimental = true;
        const LyricAlignment guessed = AlignLyrics(d, experimental);
        const bool inside = !guessed.phrases.empty() && guessed.phrases.front().startMS >= 2000 && guessed.phrases.back().endMS <= 8000;
        Check(guessed.quality == "experimental" && inside && guessed.phrases[0].confidence <= 0.1, fmt::format("on request: inside the voice, confidence {}", guessed.phrases.empty() ? 0.0 : guessed.phrases[0].confidence));
    }

    void TestAlignmentShapes() {
        Title("B: phrases marked by hand; words and shapes inside them");
        const LyricDocument doc = ParsePlainLyrics("Una canción de amor\nla Navidad\nla Navidaaad\n");
        AlignerInput input;
        input.document = &doc;
        input.corrections = Marks(doc, { { 1013, 3990 }, { 5000, 6500 }, { 8000, 9500 } });
        const LyricAlignment a = AlignLyrics(input);
        Check(a.provider == AlignmentProvider::ManualPhrases && a.quality == "manual", fmt::format("provider {}, quality {}", ProviderId(a.provider), a.quality));
        {
            std::ofstream dump(_root / "manual_alignment.json", std::ios::binary);
            dump << ToJson(a);
        }
        Check(a.phrases.size() == 3 && a.phrases[0].startMS == 1025 && a.phrases[0].endMS == 4000 && a.phrases[0].state == Provenance::Confirmed,
              fmt::format("marks land on the nearest frame (1013 -> 1025, 3990 -> 4000), confirmed: {} phrases, first {}-{} ms, {}", a.phrases.size(), a.phrases.empty() ? -1 : a.phrases[0].startMS, a.phrases.empty() ? -1 : a.phrases[0].endMS, a.phrases.empty() ? "" : ProvenanceId(a.phrases[0].state)));
        const AlignedWord* de = WordOf(a, "de");
        const AlignedWord* cancion = WordOf(a, "canción");
        Check(de != nullptr && cancion != nullptr && (cancion->endMS - cancion->startMS) > 1.8 * (de->endMS - de->startMS), "words are spread by syllables: \"canción\" gets more time than \"de\"");
        const AlignedWord* plainWord = WordOf(a, "navidad", 1);
        const AlignedWord* heldWord = WordOf(a, "navidaaad", 1);
        Check(plainWord != nullptr && heldWord != nullptr && (heldWord->endMS - heldWord->startMS) > (plainWord->endMS - plainWord->startMS), "in two equal phrases, the held \"Navidaaad\" lasts longer than \"Navidad\"");
        Check(a.words.size() == 8 && std::all_of(a.words.begin(), a.words.end(), [](const AlignedWord& w) { return w.source == "syllables" && w.confidence <= 0.5; }), "word times are marked as an estimate");

        const TimingTrackSpec track = BuildLyricTrack(a, "Letra");
        bool grid = true;
        bool shortOnes = false;
        for (const auto& layer : track.layers) {
            for (const TimingMark& m : layer) {
                grid = grid && m.startMS % FRAME_MS == 0 && m.endMS % FRAME_MS == 0;
                shortOnes = shortOnes || m.endMS - m.startMS < FRAME_MS;
            }
        }
        Check(grid && !shortOnes, "every mark on a frame and at least a frame long");
        bool shapesKnown = std::all_of(track.layers[2].begin(), track.layers[2].end(), [](const TimingMark& m) { return SHAPES.contains(m.label); });
        Check(shapesKnown, "only shapes FacesEffect draws");
        // Within a word: across two words the same shape twice is already a still mouth.
        bool noRepeats = true;
        for (size_t i = 1; i < a.phonemes.size(); ++i) {
            const AlignedPhoneme& previous = a.phonemes[i - 1];
            const AlignedPhoneme& current = a.phonemes[i];
            noRepeats = noRepeats && !(current.wordId == previous.wordId && current.shape == previous.shape && current.startMS == previous.endMS);
        }
        Check(noRepeats, "within a word the same shape twice in a row is merged, so the mouth does not flicker");
        const auto rests = std::count_if(track.layers[2].begin(), track.layers[2].end(), [](const TimingMark& m) { return m.label == "rest"; });
        const bool restInPause = std::any_of(track.layers[2].begin(), track.layers[2].end(), [](const TimingMark& m) { return m.label == "rest" && m.startMS == 4000 && m.endMS == 5000; });
        Check(rests >= 2 && restInPause, fmt::format("pauses between phrases are rests ({} rests)", rests));
        if (heldWord != nullptr) {
            int longest = 0;
            std::string shape;
            for (const AlignedPhoneme& p : a.phonemes) {
                if (p.wordId == heldWord->id && p.endMS - p.startMS > longest) {
                    longest = p.endMS - p.startMS;
                    shape = p.shape;
                }
            }
            Check(shape == "AI" && longest >= 400, fmt::format("the held vowel keeps the mouth open in one shape: {} for {} ms", shape, longest));
        }
        bool mbpKept = false;
        if (const AlignedWord* amor = WordOf(a, "amor")) {
            for (const AlignedPhoneme& p : a.phonemes) {
                mbpKept = mbpKept || (p.wordId == amor->id && p.shape == "MBP");
            }
        }
        Check(mbpKept, "the closed mouth of \"amor\" survives the merging");
        const ValidationReport report = ValidateLyrics(a, track, 12000, FRAME_MS);
        Check(report.ErrorCount() == 0, fmt::format("GenerationValidator: {} errors, {} warnings", report.ErrorCount(), report.WarningCount()));

        Title("Edge cases");
        const LyricDocument mono = ParsePlainLyrics("y yo sé\n");
        AlignerInput monoInput;
        monoInput.document = &mono;
        monoInput.corrections = Marks(mono, { { 1000, 1600 } });
        const LyricAlignment monoAligned = AlignLyrics(monoInput);
        Check(monoAligned.words.size() == 3 && monoAligned.words[0].endMS > monoAligned.words[0].startMS, "monosyllables each get their own time");
        const LyricDocument longWord = ParsePlainLyrics("extraordinariamente\n");
        AlignerInput longInput;
        longInput.document = &longWord;
        longInput.corrections = Marks(longWord, { { 0, 2000 } });
        const LyricAlignment longAligned = AlignLyrics(longInput);
        Check(longAligned.phonemes.size() >= 8, fmt::format("a long word gets many shapes ({})", longAligned.phonemes.size()));
        const LyricDocument crowded = ParsePlainLyrics("una noche de Navidad las estrellas\n");
        AlignerInput crowdedInput;
        crowdedInput.document = &crowded;
        crowdedInput.corrections = Marks(crowded, { { 1000, 1100 } });
        const LyricAlignment tooShort = AlignLyrics(crowdedInput);
        Check(std::count(tooShort.warnings.begin(), tooShort.warnings.end(), "phrase-too-short-for-its-words") == 1, "a phrase too short for its words is reported, not squeezed into nothing");

        Title("A repeated chorus");
        const LyricDocument chorus = ParsePlainLyrics("[Estribillo]\nVuelve la Navidad\nVuelve la Navidad\n\n[Estribillo]\nVuelve la Navidad\n");
        AlignerInput chorusInput;
        chorusInput.document = &chorus;
        chorusInput.corrections = Marks(chorus, { { 1000, 3000 }, { 3500, 5500 }, { 9000, 11000 } });
        const LyricAlignment repeated = AlignLyrics(chorusInput);
        std::set<std::string> ids;
        for (const AlignedPhrase& p : repeated.phrases) {
            ids.insert(p.id);
        }
        Check(ids.size() == 3 && repeated.phrases[2].startMS == 9000, "three occurrences, three ids, each at its own time");
    }

    void TestCorrections() {
        Title("Manual corrections survive regeneration");
        const std::string lyrics = "Una noche de Navidad\nLas estrellas brillarán\n";
        LyricDocument doc = ParsePlainLyrics(lyrics);
        AlignerInput input;
        input.document = &doc;
        input.corrections = Marks(doc, { { 1000, 4000 }, { 5000, 8000 } });
        const LyricAlignment first = AlignLyrics(input);
        const AlignedWord* navidad = WordOf(first, "navidad");
        const AlignedWord* estrellas = WordOf(first, "estrellas");
        if (navidad == nullptr || estrellas == nullptr) {
            Check(false, "words to correct");
            return;
        }
        LyricCorrection lockedWord;
        lockedWord.level = LyricCorrection::Level::Word;
        lockedWord.targetId = navidad->id;
        lockedWord.state = Provenance::Locked;
        lockedWord.startMS = 2500;
        lockedWord.endMS = 3900;
        lockedWord.originalStartMS = navidad->startMS;
        lockedWord.originalEndMS = navidad->endMS;
        std::string phonemeId;
        for (const AlignedPhoneme& p : first.phonemes) {
            if (p.wordId == estrellas->id) {
                phonemeId = p.id;
                break;
            }
        }
        LyricCorrection shape;
        shape.level = LyricCorrection::Level::Phoneme;
        shape.targetId = phonemeId;
        shape.state = Provenance::Corrected;
        shape.shape = "O";
        input.corrections.push_back(lockedWord);
        input.corrections.push_back(shape);

        // Regenerate with another dialect and a phrase moved: what was not corrected may
        // change, what was must not.
        AlignerOptions seseo;
        seseo.phonemizer.dialect = SpanishDialect::Seseo;
        input.corrections[0].startMS = 1200;
        const LyricAlignment second = AlignLyrics(input, seseo);
        const AlignedWord* again = WordOf(second, "navidad");
        Check(again != nullptr && again->startMS == 2500 && again->endMS == 3900 && again->state == Provenance::Locked, "the locked word keeps its times after regenerating");
        Check(again != nullptr && again->originalStartMS >= 0, "and remembers what it replaced");
        const auto corrected = std::find_if(second.phonemes.begin(), second.phonemes.end(), [&](const AlignedPhoneme& p) { return p.id == phonemeId; });
        Check(corrected != second.phonemes.end() && corrected->shape == "O" && corrected->state == Provenance::Corrected, "the corrected mouth shape is kept");
        Check(second.phrases[0].startMS == 1200 && second.phrases[0].state == Provenance::Confirmed, "the moved phrase mark is kept, confirmed");
        const AlignedWord* una = WordOf(second, "una");
        Check(una != nullptr && una->startMS >= 1200 && una->endMS <= 2500, "detected words make way around the locked one");
        const TimingTrackSpec track = BuildLyricTrack(second, "Letra");
        const ValidationReport kept = ValidateLyrics(second, track, 10000, FRAME_MS);
        Check(!kept.Has(issue::LyricsCorrectionLost) && !kept.Has(issue::LyricsCorrectionConflict), "the validator confirms every correction was kept");

        LyricAlignment tampered = second;
        for (AlignedWord& w : tampered.words) {
            if (w.id == lockedWord.targetId) {
                w.startMS = 2000;
            }
        }
        Check(ValidateLyrics(tampered, track, 10000, FRAME_MS).Has(issue::LyricsCorrectionLost), "and catches one that was not");

        Title("Lyrics edited after correcting");
        const LyricDocument edited = ParsePlainLyrics("Una noche de invierno\nLas estrellas brillarán\n");
        AlignerInput editedInput = input;
        editedInput.document = &edited;
        const LyricAlignment afterEdit = AlignLyrics(editedInput);
        const bool reported = std::any_of(afterEdit.conflicts.begin(), afterEdit.conflicts.end(), [&](const AlignmentConflict& c) { return c.targetId == lockedWord.targetId; });
        const bool stillThere = std::any_of(afterEdit.corrections.begin(), afterEdit.corrections.end(), [&](const LyricCorrection& c) { return c.targetId == lockedWord.targetId; });
        Check(reported && stillThere, "a correction whose word disappeared is reported as a conflict and kept, not deleted");
        Check(ValidateLyrics(afterEdit, BuildLyricTrack(afterEdit, "Letra"), 10000, FRAME_MS).Has(issue::LyricsCorrectionConflict), "the validator reports the conflict");

        Title("Sessions on disk");
        LyricSession session;
        session.trackName = "Letra, prueba";
        session.lyrics = lyrics;
        session.corrections = input.corrections;
        const std::string path = (_root / "session.composer-lyrics.json").string();
        std::string error;
        Check(SaveLyricSession(path, session, error), fmt::format("saved {}", error));
        LyricSession loaded;
        Check(LoadLyricSession(path, loaded, error) && ToJson(loaded) == ToJson(session), "read back identical");
        Check(!fs::exists(path + ".composer-tmp"), "no temporary left");
        LyricSession broken;
        Check(!FromJson("{\"schemaVersion\":\"x\"}", broken, error) && !FromJson("not json", broken, error), "a damaged session is an error, not a crash");
        std::vector<LyricCorrection> list;
        LyricCorrection mark;
        mark.targetId = "a#1";
        mark.startMS = 1000;
        mark.endMS = 2000;
        mark.originalStartMS = 900;
        UpsertCorrection(list, mark);
        mark.startMS = 1100;
        mark.originalStartMS = 1000;
        UpsertCorrection(list, mark);
        Check(list.size() == 1 && list[0].startMS == 1100 && list[0].originalStartMS == 900, "correcting twice keeps one correction and the first original time");
    }

    void TestValidator() {
        Title("GenerationValidator on lyrics tracks");
        TimingTrackSpec good{ "Letra", { { { "la Navidad", 1000, 2000 } }, { { "la", 1000, 1300 }, { "Navidad", 1400, 2000 } }, { { "L", 1000, 1100 }, { "AI", 1100, 1300 }, { "rest", 1300, 1400 }, { "etc", 1400, 1500 }, { "AI", 1500, 2000 } } } };
        Check(ValidateLyricTrack(good, 5000, FRAME_MS).ErrorCount() == 0, "a correct track passes");
        TimingTrackSpec twoLayers{ "Letra", { good.layers[0], good.layers[1] } };
        Check(ValidateLyricTrack(twoLayers, 5000, FRAME_MS).Has(issue::LyricsTrackIncomplete), "two layers: incomplete");
        TimingTrackSpec swapped{ "Letra", { good.layers[0], good.layers[2], good.layers[1] } };
        Check(ValidateLyricTrack(swapped, 5000, FRAME_MS).Has(issue::LyricsLayerOrder), "words and phonemes swapped: wrong order");
        TimingTrackSpec unknown = good;
        unknown.layers[2][1].label = "A";
        Check(ValidateLyricTrack(unknown, 5000, FRAME_MS).Has(issue::FacePhonemeUnknown), "\"A\" is not a shape the face knows");
        TimingTrackSpec moving = good;
        moving.layers[2] = { { "L", 1000, 1100 }, { "AI", 1100, 1450 }, { "etc", 1450, 1500 }, { "AI", 1500, 2000 } };
        const ValidationReport noRest = ValidateLyricTrack(moving, 5000, FRAME_MS);
        Check(noRest.Has(issue::LyricsRestMissing) && noRest.Has(issue::LyricsOverlap), "a shape across a pause: no rest, and outside its word");
        TimingTrackSpec bad = good;
        bad.layers[1][1].endMS = 2500;
        Check(ValidateLyricTrack(bad, 5000, FRAME_MS).Has(issue::LyricsOverlap), "a word past its phrase");
        TimingTrackSpec reversed = good;
        reversed.layers[0][0].endMS = 900;
        Check(ValidateLyricTrack(reversed, 5000, FRAME_MS).Has(issue::LyricsTimeInvalid), "a phrase ending before it starts");
        Check(ValidateLyricTrack(good, 1500, FRAME_MS).Has(issue::LyricsTimeInvalid), "marks past the end of the song");

        const LyricDocument english = ParsePlainLyrics("Let it snow, let it snow\n");
        AlignerInput in;
        in.document = &english;
        in.corrections = Marks(english, { { 1000, 3000 } });
        const LyricAlignment en = AlignLyrics(in);
        Check(ValidateLyrics(en, BuildLyricTrack(en, "Snow"), 5000, FRAME_MS).Has(issue::LyricsLanguageUnsupported), "English lyrics: language not supported is reported");
        const LyricDocument year = ParsePlainLyrics("Feliz 2025\n");
        in.document = &year;
        in.corrections = Marks(year, { { 1000, 3000 } });
        const LyricAlignment numbers = AlignLyrics(in);
        Check(ValidateLyrics(numbers, BuildLyricTrack(numbers, "Año"), 5000, FRAME_MS).Has(issue::LyricsWordNoPhonemes), "\"2025\" has no mouth shapes, and that is reported");
        const LyricDocument plain = ParsePlainLyrics("Una noche\nOtra noche\n");
        in.document = &plain;
        in.corrections = Marks(plain, { { 1000, 2000 } });
        const LyricAlignment half = AlignLyrics(in);
        Check(ValidateLyrics(half, BuildLyricTrack(half, "Mitad"), 5000, FRAME_MS).Has(issue::LyricsNotSynchronized), "a phrase without times is reported");
        const LyricDocument lrc = ParseLrcLyrics("[00:01.00]Una noche\n[00:03.00]Otra noche\n");
        in.document = &lrc;
        in.corrections.clear();
        const LyricAlignment approx = AlignLyrics(in);
        Check(ValidateLyrics(approx, BuildLyricTrack(approx, "LRC"), 5000, FRAME_MS).Has(issue::LyricsLowConfidence), "LRC times are reported as uncertain");
        ValidationIssue sample;
        sample.code = issue::LyricsRestMissing;
        sample.args = { { "track", "Letra" }, { "at", "1300" } };
        Check(DescribeIssue(sample).find("1300") != std::string::npos && !DescribeSuggestion(sample).empty(), "every new code has a message and a suggestion");
    }

    // The old path is untouched: English dictionary words, uniform phrase breakdown.
    void TestLegacyBreakdown() {
        Title("Regression: PhonemeDictionary and LyricBreakdown");
        std::vector<std::string> dirs = { FileUtils::GetResourcesDir() + "/dictionaries", FileUtils::GetResourcesDir(), (fs::current_path() / "dictionaries").string(), (fs::current_path() / "resources" / "dictionaries").string() };
        PhonemeDictionary dictionary;
        dictionary.LoadDictionaries(dirs);
        std::vector<std::string> phonemes;
        dictionary.BreakdownWord("hello", phonemes);
        Check(Join(phonemes) == "etc AI L O", fmt::format("HELLO: {}", Join(phonemes)));
        dictionary.BreakdownWord("amor", phonemes);
        Check(Join(phonemes) == "AI MBP E", fmt::format("AMOR through the English dictionary: {} (as before)", Join(phonemes)));
        dictionary.BreakdownWord("corazón", phonemes);
        Check(phonemes.empty(), "\"corazón\" with its accent is still not in the English dictionary: the gap this phase fills");

        const fs::path show = _root / "legacy_show";
        std::error_code ec;
        fs::create_directories(show, ec);
        pugi::xml_document doc;
        pugi::xml_node root = doc.append_child("xrgb");
        root.append_child("models");
        root.append_child("modelGroups");
        (void)doc.save_file((show / "xlights_rgbeffects.xml").string().c_str());
        TimingImportContext ctx;
        if (!ctx.LoadShowFolder(show.string())) {
            Check(false, "loading an empty show");
            return;
        }
        SequenceElements& elements = ctx.GetSequenceElements();
        elements.SetFrequency(1000.0 / FRAME_MS);
        TimingElement* element = ctx.AddTimingElement("Legacy");
        element->AddEffectLayer();
        element->AddEffectLayer();
        BreakdownPhrase(element->GetEffectLayer(1), 0, 3000, "uno dos tres", elements.GetFrequency(), elements.get_undo_mgr());
        EffectLayer* words = element->GetEffectLayer(1);
        bool uniform = words->GetEffectCount() == 3;
        for (int w = 0; uniform && w < 3; ++w) {
            uniform = words->GetEffect(w)->GetStartTimeMS() == w * 1000 && words->GetEffect(w)->GetEndTimeMS() == (w + 1) * 1000;
        }
        Check(uniform, "BreakdownPhrase still splits words evenly");
        BreakdownWord(element->GetEffectLayer(2), 0, 1000, "hello", elements.GetFrequency(), dictionary, elements.get_undo_mgr());
        Check(element->GetEffectLayer(2)->GetEffectCount() == 4, fmt::format("BreakdownWord still gives HELLO {} phonemes", element->GetEffectLayer(2)->GetEffectCount()));
    }

    void TestExport() {
        Title("Three layers through xLights' own timing import");
        const LyricDocument doc = ParsePlainLyrics("¡Feliz Navidad, señor!\nUna canción de amor\n");
        AlignerInput input;
        input.document = &doc;
        input.corrections = Marks(doc, { { 1000, 3500 }, { 4200, 7000 } });
        const LyricAlignment a = AlignLyrics(input);
        const TimingTrackSpec track = BuildLyricTrack(a, "Letra, prueba española");
        const std::string path = (_root / "lyrics.xtiming").string();
        std::string error;
        Check(WriteTimingTracks(path, { track }, error), fmt::format("written {}", error));
        const fs::path show = _root / "import_show";
        std::error_code ec;
        fs::create_directories(show, ec);
        {
            pugi::xml_document doc2;
            pugi::xml_node root = doc2.append_child("xrgb");
            root.append_child("models");
            root.append_child("modelGroups");
            (void)doc2.save_file((show / "xlights_rgbeffects.xml").string().c_str());
        }
        const std::string sequence = (show / "imported.xsq").string();
        {
            TimingImportContext ctx;
            if (!ctx.LoadShowFolder(show.string())) {
                Check(false, "loading the test show");
                return;
            }
            SequenceElements& elements = ctx.GetSequenceElements();
            elements.SetFrequency(1000.0 / FRAME_MS);
            elements.SetViewsManager(&ctx._sequenceViewManager);
            elements.SetSequenceEnd(10000);
            SequenceFile file(sequence, FRAME_MS);
            file.SetSequenceType("Animation");
            file.SetSequenceDurationMS(10000);
            file.SetSequenceTiming(std::to_string(FRAME_MS) + " ms");
            file.ProcessXTiming(std::vector<std::string>{ path }, &ctx);
            Check(file.Save(elements), "saved through SequenceFile");
            (void)ctx.CloseSequence();
        }
        HeadlessRenderContext reopened;
        if (!reopened.LoadShowFolder(show.string()) || !reopened.OpenSequence(sequence)) {
            Check(false, "reopening the imported sequence");
            return;
        }
        // "Import Timing Track" passes names through RemoveUnsafeXmlChars, which drops every
        // non-ASCII byte; marks are not touched. SequenceWriter keeps the full name.
        const std::string importedName = RemoveUnsafeXmlChars(track.name);
        Check(importedName == "Letra, prueba espaola", fmt::format("xLights' importer strips the n-tilde from the track name: \"{}\" (known, outside the Composer)", importedName));
        TimingElement* element = reopened.GetSequenceElements().GetTimingElement(importedName);
        if (element == nullptr || element->GetEffectLayerCount() != 3) {
            std::string names;
            for (int e = 0; e < reopened.GetSequenceElements().GetElementCount(); ++e) {
                names += " [" + reopened.GetSequenceElements().GetElement(e)->GetName() + "]";
            }
            Check(false, fmt::format("track imported with 3 layers ({} layers; elements:{})", element == nullptr ? -1 : static_cast<int>(element->GetEffectLayerCount()), names));
            return;
        }
        for (int l = 0; l < 3; ++l) {
            EffectLayer* layer = element->GetEffectLayer(l);
            bool same = layer->GetEffectCount() == static_cast<int>(track.layers[l].size());
            for (size_t m = 0; same && m < track.layers[l].size(); ++m) {
                const Effect* mark = layer->GetEffect(static_cast<int>(m));
                same = mark->GetStartTimeMS() == track.layers[l][m].startMS && mark->GetEndTimeMS() == track.layers[l][m].endMS && mark->GetEffectName() == track.layers[l][m].label;
            }
            Check(same, fmt::format("layer {}: {} marks identical after import, save and reopen", l, track.layers[l].size()));
        }
        Check(element->GetEffectLayer(0)->GetEffect(0)->GetEffectName() == "¡Feliz Navidad, señor!", "accents, ñ and ¡ survive the round trip");
        (void)reopened.CloseSequence();
    }

    void TestPerformance() {
        Title("Timings (Release figures are the ones to quote)");
        const char* words[] = { "Navidad", "corazón", "pingüino", "estrella", "vergüenza", "chocolate", "lluvia", "canción", "muñeco", "guitarra" };
        constexpr int COUNT = 20000;
        auto start = std::chrono::steady_clock::now();
        size_t phones = 0;
        for (int i = 0; i < COUNT; ++i) {
            phones += PhonemizeSpanish(words[i % 10]).phones.size();
        }
        const long long phonemize = ElapsedMS(start);
        printf("  phonemize+syllabify: %d words in %lld ms (%zu phonemes)\n", COUNT, phonemize, phones);
        std::string song;
        std::vector<std::pair<int, int>> times;
        for (int line = 0; line < 120; ++line) {
            song += fmt::format("Vuelve la Navidad con su canción de amor número {}\n", line % 7 == 0 ? std::string("siete") : std::string("uno"));
            times.push_back({ line * 2500, line * 2500 + 2200 });
        }
        start = std::chrono::steady_clock::now();
        const LyricDocument doc = ParsePlainLyrics(song);
        const long long parse = ElapsedMS(start);
        AlignerInput input;
        input.document = &doc;
        input.corrections = Marks(doc, times);
        start = std::chrono::steady_clock::now();
        const LyricAlignment a = AlignLyrics(input);
        const long long align = ElapsedMS(start);
        start = std::chrono::steady_clock::now();
        const TimingTrackSpec track = BuildLyricTrack(a, "Bench");
        const long long build = ElapsedMS(start);
        start = std::chrono::steady_clock::now();
        const ValidationReport report = ValidateLyrics(a, track, 300000, FRAME_MS);
        const long long validate = ElapsedMS(start);
        printf("  120 lines, %zu words, %zu shapes: parse %lld ms, align %lld ms, track %lld ms, validate %lld ms\n", a.words.size(), a.phonemes.size(), parse, align, build, validate);
        Check(report.ErrorCount() == 0 && a.words.size() == 120 * 10, fmt::format("a five-minute song aligns cleanly ({} errors)", report.ErrorCount()));
    }

    fs::path _root;
    int _passed = 0;
    int _failed = 0;
};

// ---------------------------------------------------------------- faces

constexpr const char* FACE_MODEL = "Carita rosa chica";
constexpr const char* FACE_DEFINITION = "Upright Bulb";
constexpr const char* FACE_TRACK = "Letra, prueba española";
constexpr int FACE_SONG_MS = 18000;

const std::string FACE_LYRICS =
    "[Estrofa 1]\n"
    "¡Feliz Navidad, señor!\n"
    "Una canción de amor\n"
    "\n"
    "[Estribillo]\n"
    "Navidaaad, Navidad\n";

class FrameReader {
public:
    explicit FrameReader(const std::string& path) {
        std::unique_ptr<FSEQFile> file(FSEQFile::openFSEQFile(path));
        if (file == nullptr) {
            return;
        }
        _channels = file->getMaxChannel();
        _frameMS = file->getStepTime();
        std::vector<std::pair<uint32_t, uint32_t>> ranges{ { 0, _channels } };
        file->prepareRead(ranges);
        for (uint64_t f = 0; f < file->getNumFrames(); ++f) {
            std::vector<uint8_t> data(_channels, 0);
            std::unique_ptr<FSEQFile::FrameData> frame(file->getFrame(static_cast<uint32_t>(f)));
            if (frame != nullptr) {
                frame->readFrame(data.data(), _channels);
            }
            _frames.push_back(std::move(data));
        }
    }

    [[nodiscard]] bool Valid() const { return !_frames.empty(); }

    [[nodiscard]] xlColor Node(int ms, uint32_t channel) const {
        const std::vector<uint8_t>& frame = _frames.at(std::min<size_t>(_frames.size() - 1, ms / _frameMS));
        return channel + 2 < frame.size() ? xlColor(frame[channel], frame[channel + 1], frame[channel + 2]) : xlBLACK;
    }

private:
    uint32_t _channels = 0;
    int _frameMS = FRAME_MS;
    std::vector<std::vector<uint8_t>> _frames;
};

// One picture of the model's nodes as the render left them, laid out on its buffer.
bool SaveFrame(Model& model, const FrameReader& frames, int ms, const std::string& path) {
    constexpr int CELL = 12;
    int maxX = 0;
    int maxY = 0;
    std::vector<std::vector<xlPoint>> coords(model.GetNodeCount());
    for (size_t n = 0; n < model.GetNodeCount(); ++n) {
        model.GetNodeCoords(static_cast<int>(n), coords[n]);
        for (const xlPoint& p : coords[n]) {
            maxX = std::max(maxX, static_cast<int>(p.x));
            maxY = std::max(maxY, static_cast<int>(p.y));
        }
    }
    const int width = (maxX + 1) * CELL;
    const int height = (maxY + 1) * CELL;
    xlImage image(width, height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            image.SetRGB(x, y, 16, 16, 16);
            image.SetAlpha(x, y, 255);
        }
    }
    for (size_t n = 0; n < model.GetNodeCount(); ++n) {
        const xlColor c = frames.Node(ms, static_cast<uint32_t>(model.NodeStartChannel(n)));
        for (const xlPoint& p : coords[n]) {
            // Buffer rows count up from the bottom.
            const int left = static_cast<int>(p.x) * CELL;
            const int top = (maxY - static_cast<int>(p.y)) * CELL;
            for (int y = 2; y < CELL - 2; ++y) {
                for (int x = 2; x < CELL - 2; ++x) {
                    const bool lit = c.red || c.green || c.blue;
                    image.SetRGB(left + x, top + y, lit ? c.red : 48, lit ? c.green : 48, lit ? c.blue : 48);
                }
            }
        }
    }
    std::vector<uint8_t> png;
    if (!image.SaveAsPNG(png)) {
        return false;
    }
    std::ofstream out(fs::path(path), std::ios::binary);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return static_cast<bool>(out);
}

} // namespace

int RunLyricsTest(const std::string& workDir) {
    fs::path root;
    std::error_code ec;
    for (int i = 0; i < 1000; ++i) {
        root = fs::path(workDir) / fmt::format("composer-lyrics-test-{}", i);
        if (!fs::exists(root, ec)) {
            break;
        }
    }
    fs::create_directories(root, ec);
    if (ec) {
        printf("composer-lyrics-test: cannot create %s\n", root.string().c_str());
        return 1;
    }
    Suite suite(root);
    return suite.Run();
}

int RunFacesTest(const std::string& showDir, const std::string& destinationName) {
    std::string refused;
    if (!ControllersAreInactive(showDir, refused)) {
        printf("composer-faces-test: RECHAZADO: %s\n", refused.c_str());
        return 2;
    }
    int failed = 0;
    const auto check = [&](bool ok, const std::string& what) {
        printf("  %s %s\n", ok ? "OK   " : "FAILED", what.c_str());
        failed += ok ? 0 : 1;
    };

    auto start = std::chrono::steady_clock::now();
    const LyricDocument doc = ParsePlainLyrics(FACE_LYRICS);
    AlignerInput input;
    input.document = &doc;
    input.durationMS = FACE_SONG_MS;
    input.corrections = Marks(doc, { { 2000, 5000 }, { 5600, 8400 }, { 10000, 14500 } });
    const LyricAlignment alignment = AlignLyrics(input);
    const TimingTrackSpec track = BuildLyricTrack(alignment, FACE_TRACK);
    printf("composer-faces-test: aligned %zu phrases, %zu words, %zu shapes in %lld ms (%s)\n", alignment.phrases.size(), alignment.words.size(), alignment.phonemes.size(), ElapsedMS(start), alignment.quality.c_str());
    for (const AlignedWord& w : alignment.words) {
        printf("  %5d-%5d  %-12s [%s]\n", w.startMS, w.endMS, w.text.c_str(), w.phonemes.c_str());
    }
    const ValidationReport lyrics = ValidateLyrics(alignment, track, FACE_SONG_MS, FRAME_MS);
    PrintValidationSummary("composer-faces-test lyrics", lyrics);
    check(lyrics.ErrorCount() == 0, "lyrics track passes GenerationValidator");

    SequenceSpec spec;
    spec.frameMS = FRAME_MS;
    spec.durationMS = FACE_SONG_MS;
    spec.timingTracks.push_back(track);
    spec.effects.push_back({ FACE_MODEL, "", 0, "Faces",
                             { { "E_CHOICE_Faces_FaceDefinition", FACE_DEFINITION }, { "E_CHOICE_Faces_Eyes", "Open" }, { "E_CHECKBOX_Faces_Outline", "1" }, { "E_CHOICE_Faces_TimingTrack", FACE_TRACK } },
                             "C_BUTTON_Palette1=#FF66CC,C_BUTTON_Palette2=#FFFFFF,C_CHECKBOX_Palette1=1,C_CHECKBOX_Palette2=1", 1000, 17000 });
    start = std::chrono::steady_clock::now();
    const WriteResult written = WriteSequenceSafely(showDir, spec, destinationName);
    for (const std::string& problem : written.problems) {
        printf("composer-faces-test: ERROR: %s\n", problem.c_str());
    }
    PrintValidationSummary("composer-faces-test sequence", written.validation);
    check(written.ok, fmt::format("sequence written, reopened and validated in {} ms: {}", ElapsedMS(start), written.finalPath));
    if (!written.ok) {
        return 1;
    }

    const std::string fseq = (fs::path(written.finalPath).replace_extension(".fseq")).string();
    HeadlessRenderContext ctx;
    if (!ctx.LoadShowFolder(showDir) || !ctx.OpenSequence(written.finalPath)) {
        check(false, "opening the written sequence");
        return 1;
    }
    start = std::chrono::steady_clock::now();
    const bool rendered = ctx.RenderAndWait() && ctx.WriteFseq(fseq);
    check(rendered, fmt::format("rendered in {} ms: {}", ElapsedMS(start), fseq));
    if (!rendered) {
        return 1;
    }
    const ValidationReport render = ValidateRender(ctx, fseq, spec, Strictness::Generated);
    PrintValidationSummary("composer-faces-test render", render);
    check(render.ErrorCount() == 0 && render.Status(IssueCategory::Render) == CheckStatus::Passed, "render passes GenerationValidator: the mouth matches the definition for each shape and rests in pauses");

    Model* model = ctx.AllModels[FACE_MODEL];
    FrameReader frames(fseq);
    if (model == nullptr || !frames.Valid()) {
        check(false, "reading the face's rendered nodes");
        return 1;
    }
    const auto& definitions = model->GetFaceInfoNodes();
    const auto definition = definitions.find(FACE_DEFINITION);
    if (definition == definitions.end()) {
        check(false, "the face definition has nodes");
        return 1;
    }
    std::set<int> mouthNodes;
    std::map<std::string, std::set<int>> shapes;
    for (const auto& [key, nodes] : definition->second) {
        if (key.rfind("Mouth-", 0) == 0 && key.find("-Color") == std::string::npos) {
            shapes[key.substr(6)] = std::set<int>(nodes.begin(), nodes.end());
            mouthNodes.insert(nodes.begin(), nodes.end());
        }
    }
    const auto litMouth = [&](int ms) {
        std::set<int> lit;
        for (int n : mouthNodes) {
            const xlColor c = frames.Node(ms, static_cast<uint32_t>(model->NodeStartChannel(n)));
            if (c.red || c.green || c.blue) {
                lit.insert(n);
            }
        }
        return lit;
    };
    int samples = 0;
    int matching = 0;
    std::set<std::set<int>> distinct;
    int restSamples = 0;
    int restMatching = 0;
    for (const TimingMark& mark : track.layers[2]) {
        if (mark.endMS - mark.startMS < 2 * FRAME_MS) {
            continue;
        }
        const int middle = (mark.startMS + mark.endMS) / 2 / FRAME_MS * FRAME_MS;
        const std::set<int> lit = litMouth(middle);
        distinct.insert(lit);
        const bool same = lit == shapes[mark.label];
        if (mark.label == "rest") {
            ++restSamples;
            restMatching += same ? 1 : 0;
        } else {
            ++samples;
            matching += same ? 1 : 0;
        }
    }
    check(samples > 0 && matching == samples, fmt::format("mouth nodes equal the definition of the shape in {} of {} sung marks", matching, samples));
    check(restSamples > 0 && restMatching == restSamples, fmt::format("and of \"rest\" in {} of {} pauses", restMatching, restSamples));
    check(distinct.size() >= 5, fmt::format("{} different mouths over the song", distinct.size()));
    check(litMouth(500) == std::set<int>() || litMouth(500) == shapes["rest"], "before the effect starts the mouth is dark or at rest");
    const auto openVowel = std::find_if(alignment.phonemes.begin(), alignment.phonemes.end(), [](const AlignedPhoneme& p) { return p.shape == "AI" && p.endMS - p.startMS >= 300; });
    check(openVowel != alignment.phonemes.end() && litMouth((openVowel->startMS + openVowel->endMS) / 2 / FRAME_MS * FRAME_MS).size() > shapes["rest"].size(),
          "a held open vowel lights more of the mouth than rest");

    // Pictures for a person to judge; nothing here can say a mouth looks natural.
    const fs::path pictures = fs::path(written.finalPath).parent_path() / (fs::path(written.finalPath).stem().string() + "_frames");
    std::error_code ec;
    fs::create_directories(pictures, ec);
    std::vector<std::pair<std::string, int>> moments = { { "00_before", 500 } };
    std::set<std::string> taken;
    for (const AlignedPhoneme& p : alignment.phonemes) {
        if (p.endMS - p.startMS >= 2 * FRAME_MS && taken.insert(p.shape).second) {
            moments.push_back({ fmt::format("{:02}_{}", moments.size(), p.shape), (p.startMS + p.endMS) / 2 / FRAME_MS * FRAME_MS });
        }
    }
    if (openVowel != alignment.phonemes.end()) {
        moments.push_back({ fmt::format("{:02}_held_vowel", moments.size()), (openVowel->startMS + openVowel->endMS) / 2 / FRAME_MS * FRAME_MS });
    }
    int saved = 0;
    for (const auto& [name, ms] : moments) {
        const std::string path = (pictures / fmt::format("{}_{}ms.png", name, ms)).string();
        saved += SaveFrame(*model, frames, ms, path) ? 1 : 0;
    }
    check(saved == static_cast<int>(moments.size()), fmt::format("{} frames saved to {}", saved, pictures.string()));
    (void)ctx.CloseSequence();

    printf("composer-faces-test: real song synchronization: %s; how natural the mouth looks needs a person\n", MUSIC_QUALITY_NOT_VALIDATED);
    printf("composer-faces-test: %s\n", failed == 0 ? "OK" : "FALLO");
    return failed == 0 ? 0 : 1;
}

} // namespace composer
