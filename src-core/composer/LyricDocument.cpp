#include "composer/LyricDocument.h"

#include "composer/ComposerText.h"
#include "lyrics/LRCParser.h"
#include "lyrics/SRTParser.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

namespace composer {

namespace {

const char* const BLOCK_IDS[] = { "unlabeled", "intro", "verse", "pre-chorus", "chorus", "bridge", "instrumental", "outro", "other" };

// Tag words, folded to lowercase without accents. Spanish first, since the lyrics
// are, and the English ones because vendors and LRCLIB use them.
struct TagWord {
    const char* word;
    LyricBlockKind kind;
};

const TagWord TAG_WORDS[] = {
    { "preestribillo", LyricBlockKind::PreChorus }, { "precoro", LyricBlockKind::PreChorus }, { "prechorus", LyricBlockKind::PreChorus },
    { "intro", LyricBlockKind::Intro }, { "introduccion", LyricBlockKind::Intro },
    { "estrofa", LyricBlockKind::Verse }, { "verso", LyricBlockKind::Verse }, { "verse", LyricBlockKind::Verse },
    { "estribillo", LyricBlockKind::Chorus }, { "coro", LyricBlockKind::Chorus }, { "chorus", LyricBlockKind::Chorus }, { "refrain", LyricBlockKind::Chorus },
    { "puente", LyricBlockKind::Bridge }, { "bridge", LyricBlockKind::Bridge },
    { "instrumental", LyricBlockKind::Instrumental }, { "solo", LyricBlockKind::Instrumental }, { "interludio", LyricBlockKind::Instrumental }, { "interlude", LyricBlockKind::Instrumental },
    { "outro", LyricBlockKind::Outro }, { "final", LyricBlockKind::Outro }, { "cierre", LyricBlockKind::Outro }, { "coda", LyricBlockKind::Outro },
};

// What a line that only says "there is music here" looks like, folded.
const std::set<std::string> INSTRUMENTAL_LINES = { "instrumental", "musica", "music", "solo", "interludio", "interlude" };

std::string LettersOnly(const std::string& folded) {
    std::string out;
    for (char32_t c : text::Decode(folded)) {
        if (text::IsLetter(c)) {
            out += text::Encode(c);
        }
    }
    return out;
}

bool IsMusicSymbol(char32_t c) {
    return c == 0x2669 || c == 0x266A || c == 0x266B || c == 0x266C;
}

bool IsInstrumentalLine(const std::string& line) {
    const std::vector<char32_t> cps = text::Decode(line);
    if (!cps.empty() && std::all_of(cps.begin(), cps.end(), [](char32_t c) { return IsMusicSymbol(c) || text::IsSpace(c); })) {
        return true;
    }
    const std::string trimmed = text::Trim(line);
    const bool wrapped = trimmed.size() >= 2 && ((trimmed.front() == '(' && trimmed.back() == ')') || (trimmed.front() == '[' && trimmed.back() == ']'));
    return wrapped && INSTRUMENTAL_LINES.contains(LettersOnly(text::Fold(trimmed)));
}

bool IsTag(const std::string& trimmed) {
    if (trimmed.size() < 3 || trimmed.front() != '[' || trimmed.back() != ']') {
        return false;
    }
    // "[01:23.45]" is an LRC time, not a tag.
    const std::string inner = trimmed.substr(1, trimmed.size() - 2);
    const size_t colon = inner.find(':');
    return colon == std::string::npos || colon == 0 || !std::all_of(inner.begin(), inner.begin() + static_cast<std::ptrdiff_t>(colon), [](char c) { return c >= '0' && c <= '9'; });
}

struct Tag {
    bool isVoice = false;
    std::string voice;
    std::vector<std::string> voices; // "[Voz: Ana y Luis]" is two
    bool spoken = false;             // "[Narrador]", "[Hablado]"
    bool sung = false;               // "[Cantado]" goes back to singing
    LyricBlockKind kind = LyricBlockKind::Other;
    std::string label;
    int number = 0;
};

// "Ana y Luis", "Ana & Luis", "Ana, Luis": the names, as written.
std::vector<std::string> SplitNames(const std::string& names) {
    std::vector<std::string> out;
    std::string rest = names;
    for (const char* sep : { " y ", " and ", " & ", ",", "/" }) {
        std::string joined;
        size_t at = 0;
        std::string s = rest;
        rest.clear();
        while ((at = s.find(sep)) != std::string::npos) {
            joined += s.substr(0, at) + "\x1f";
            s = s.substr(at + std::string(sep).size());
        }
        rest = joined + s;
    }
    std::string name;
    std::istringstream in(rest);
    while (std::getline(in, name, '\x1f')) {
        name = text::Trim(name);
        if (!name.empty()) {
            out.push_back(name);
        }
    }
    return out;
}

Tag ReadTag(const std::string& trimmed) {
    Tag tag;
    tag.label = text::Trim(trimmed.substr(1, trimmed.size() - 2));
    const std::string folded = text::Fold(tag.label);
    for (const char* prefix : { "voces", "voices", "voz", "voice", "duo", "duet" }) {
        const std::string p(prefix);
        if (folded.rfind(p, 0) == 0 && folded.size() > p.size() && (folded[p.size()] == ':' || folded[p.size()] == ' ')) {
            tag.isVoice = true;
            tag.voice = text::Trim(tag.label.substr(p.size() + 1));
            tag.voices = SplitNames(tag.voice);
            return tag;
        }
    }
    // Who speaks rather than sings; a narrator is a voice of its own.
    static const std::set<std::string> NARRATOR = { "narrador", "narradora", "narracion", "narration", "narrator" };
    static const std::set<std::string> SPOKEN = { "hablado", "hablada", "spoken", "recitado" };
    static const std::set<std::string> SUNG = { "cantado", "cantada", "sung" };
    const std::string word = LettersOnly(folded);
    if (NARRATOR.contains(word)) {
        tag.isVoice = true;
        tag.voice = tag.label;
        tag.voices = { tag.label };
        tag.spoken = true;
        return tag;
    }
    if (SPOKEN.contains(word) || SUNG.contains(word)) {
        tag.isVoice = true;
        tag.spoken = SPOKEN.contains(word);
        tag.sung = SUNG.contains(word);
        return tag;
    }
    const std::string letters = LettersOnly(folded);
    for (const TagWord& word : TAG_WORDS) {
        if (letters.rfind(word.word, 0) == 0) {
            tag.kind = word.kind;
            break;
        }
    }
    std::string digits;
    for (char c : folded) {
        if (c >= '0' && c <= '9') {
            digits += c;
        }
    }
    tag.number = digits.empty() ? 0 : static_cast<int>(std::strtol(digits.c_str(), nullptr, 10));
    return tag;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    std::string line;
    std::istringstream in(text);
    while (std::getline(in, line)) {
        while (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }
    return lines;
}

// Line ids come from the text, numbered among identical lines, so a chorus sung
// three times has three ids and editing one verse does not renumber the others.
void AssignIds(LyricDocument& document) {
    std::map<std::string, int> seen;
    for (LyricBlock& block : document.blocks) {
        for (LyricLine& line : block.lines) {
            const std::string key = line.instrumental ? "~instrumental" : LyricKey(line.text);
            line.id = key + "#" + std::to_string(++seen[key]);
        }
    }
}

// "ANA: Hola" in a dialogue: the name in capitals before a colon is who speaks. Only
// capitals, so "Dime: ¿por qué?" in a song stays a line of the song.
bool SpeakerPrefix(const std::string& text, std::string& speaker, std::string& rest) {
    const size_t colon = text.find(':');
    if (colon == std::string::npos || colon == 0 || colon > 24 || colon + 1 >= text.size()) {
        return false;
    }
    const std::string name = text::Trim(text.substr(0, colon));
    int letters = 0;
    for (char32_t c : text::Decode(name)) {
        if (text::IsLetter(c)) {
            if (text::ToLower(c) == c) {
                return false;
            }
            ++letters;
        } else if (!text::IsSpace(c)) {
            return false;
        }
    }
    if (letters < 2) {
        return false;
    }
    speaker = name;
    rest = text::Trim(text.substr(colon + 1));
    return !rest.empty();
}

// The cast from the names on the lines, in order: the first named voice leads, the
// second answers, a choir and a narrator are what their names say.
void BuildCast(LyricDocument& document) {
    for (LyricBlock& block : document.blocks) {
        for (LyricLine& line : block.lines) {
            std::string speaker;
            std::string rest;
            if (!line.instrumental && line.voices.empty() && SpeakerPrefix(line.text, speaker, rest)) {
                line.text = rest;
                line.words = SplitLyricWords(rest);
                line.voices = { speaker };
                line.voice = speaker;
                line.delivery = Delivery::Spoken;
                line.voice += "\x1e"; // marks a dialogue speaker for the cast below
            }
        }
    }
    static const std::set<std::string> CHOIR = { "coro", "coros", "choir", "chorus", "todos", "all", "everyone" };
    static const std::set<std::string> NARRATOR = { "narrador", "narradora", "narracion", "narration", "narrator" };
    int named = 0;
    for (LyricBlock& block : document.blocks) {
        for (LyricLine& line : block.lines) {
            const bool dialogue = !line.voice.empty() && line.voice.back() == '\x1e';
            if (dialogue) {
                line.voice.pop_back();
            }
            std::vector<std::string> ids;
            for (const std::string& name : line.voices) {
                const std::string id = VoiceIdOf(name);
                ids.push_back(id);
                if (document.cast.Find(id) != nullptr) {
                    continue;
                }
                VoiceDef v;
                v.id = id;
                v.name = name;
                const std::string word = LettersOnly(text::Fold(name));
                if (CHOIR.contains(word)) {
                    v.role = VoiceRole::Chorus;
                } else if (NARRATOR.contains(word)) {
                    v.role = VoiceRole::Narrator;
                    v.delivery = Delivery::Spoken;
                } else if (dialogue) {
                    v.role = VoiceRole::Dialogue;
                    v.delivery = Delivery::Spoken;
                    v.source = "speaker-prefix";
                } else {
                    v.role = named == 0 ? VoiceRole::Lead : named == 1 ? VoiceRole::Second : VoiceRole::Extra;
                    ++named;
                }
                document.cast.voices.push_back(std::move(v));
            }
            line.voices = std::move(ids);
        }
    }
}

void Finish(LyricDocument& document, const std::string& source) {
    BuildCast(document);
    AssignIds(document);
    if (text::Encode(text::Decode(source)) != source) {
        document.warnings.push_back("text-not-utf8");
    }
    const LanguageGuess language = DetectLyricLanguage(source);
    document.language = language.code;
    document.languageConfidence = language.confidence;
    if (document.Empty()) {
        document.warnings.push_back("no-lyrics");
    }
}

LyricLine MakeLine(const std::string& raw, const std::string& voice) {
    LyricLine line;
    line.text = text::Trim(raw);
    line.voice = voice;
    line.instrumental = IsInstrumentalLine(line.text);
    if (!line.instrumental) {
        line.words = SplitLyricWords(line.text);
    }
    return line;
}

// Timed sources have one block; a line counts only if it has words or marks a pause.
LyricDocument FromTimedLines(const std::string& format, std::vector<LyricLine> lines, const std::string& source) {
    LyricDocument document;
    document.format = format;
    std::stable_sort(lines.begin(), lines.end(), [](const LyricLine& a, const LyricLine& b) { return a.startMS < b.startMS; });
    LyricBlock block;
    for (LyricLine& line : lines) {
        if (line.words.empty() && !line.instrumental) {
            continue;
        }
        block.lines.push_back(std::move(line));
    }
    if (!block.lines.empty()) {
        document.blocks.push_back(std::move(block));
    }
    Finish(document, source);
    return document;
}

} // namespace

const VoiceDef* VoiceCast::Find(const std::string& id) const {
    for (const VoiceDef& v : voices) {
        if (v.id == id) {
            return &v;
        }
    }
    return nullptr;
}

const char* VoiceRoleId(VoiceRole role) {
    static const char* const IDS[] = { "lead", "second", "chorus", "narrator", "dialogue", "extra", "unknown" };
    return IDS[static_cast<int>(role)];
}

bool VoiceRoleFromId(const std::string& id, VoiceRole& role) {
    for (int i = 0; i <= static_cast<int>(VoiceRole::Unknown); ++i) {
        if (id == VoiceRoleId(static_cast<VoiceRole>(i))) {
            role = static_cast<VoiceRole>(i);
            return true;
        }
    }
    return false;
}

const char* DeliveryId(Delivery delivery) {
    return delivery == Delivery::Spoken ? "spoken" : "sung";
}

std::string VoiceIdOf(const std::string& name) {
    std::string out = "v:";
    for (char32_t c : text::Decode(text::Fold(name))) {
        if (text::IsLetter(c) || text::IsDigit(c)) {
            out += text::Encode(c);
        } else if (out.back() != '-' && out.size() > 2) {
            out += '-';
        }
    }
    while (out.size() > 2 && out.back() == '-') {
        out.pop_back();
    }
    return out.size() > 2 ? out : std::string("v:unknown");
}

std::vector<const LyricLine*> LyricDocument::Lines() const {
    std::vector<const LyricLine*> lines;
    for (const LyricBlock& block : blocks) {
        for (const LyricLine& line : block.lines) {
            lines.push_back(&line);
        }
    }
    return lines;
}

int LyricDocument::ErrorCount() const {
    return static_cast<int>(std::count_if(issues.begin(), issues.end(), [](const LyricIssue& i) { return i.error; }));
}

void CheckLyricsAgainstSong(LyricDocument& document, int durationMS) {
    if (durationMS <= 0) {
        return;
    }
    for (LyricBlock& block : document.blocks) {
        for (auto it = block.lines.begin(); it != block.lines.end();) {
            if (it->startMS < durationMS) {
                ++it;
                continue;
            }
            // A pause after the music adds nothing; words there are a wrong file or a wrong song.
            document.issues.push_back({ "time-past-song-end", 0, it->startMS, it->text, !it->instrumental, it->instrumental });
            it = it->instrumental ? block.lines.erase(it) : it + 1;
        }
    }
}

std::string DescribeLyricIssue(const LyricIssue& issue) {
    std::string out = issue.code;
    if (issue.line > 0) {
        out += " line " + std::to_string(issue.line);
    }
    if (issue.timeMS >= 0) {
        out += " at " + std::to_string(issue.timeMS) + " ms";
    }
    if (!issue.text.empty()) {
        out += ": " + issue.text;
    }
    if (issue.repaired) {
        out += " (repaired)";
    }
    return out;
}

bool LyricDocument::Empty() const {
    for (const LyricBlock& block : blocks) {
        for (const LyricLine& line : block.lines) {
            if (!line.words.empty()) {
                return false;
            }
        }
    }
    return true;
}

bool LyricDocument::HasTimes() const {
    for (const LyricLine* line : Lines()) {
        if (line->startMS >= 0) {
            return true;
        }
    }
    return false;
}

std::vector<LyricWord> SplitLyricWords(const std::string& line) {
    std::vector<LyricWord> words;
    const std::vector<char32_t> cps = text::Decode(line);
    size_t i = 0;
    while (i < cps.size()) {
        while (i < cps.size() && text::IsSpace(cps[i])) {
            ++i;
        }
        const size_t start = i;
        while (i < cps.size() && !text::IsSpace(cps[i])) {
            ++i;
        }
        if (i == start) {
            continue;
        }
        LyricWord word;
        std::vector<char32_t> key;
        for (size_t k = start; k < i; ++k) {
            if (text::IsWordChar(cps[k])) {
                key.push_back(text::ToLower(cps[k] == 0x2019 ? U'\'' : cps[k]));
            }
        }
        // Apostrophes only join letters: "'hola'" is quoted, "pa'l" is one word.
        while (!key.empty() && key.front() == U'\'') {
            key.erase(key.begin());
        }
        while (!key.empty() && key.back() == U'\'') {
            key.pop_back();
        }
        if (key.empty()) {
            continue; // "—", "¡", "...": part of the line, not a word
        }
        word.text = text::Encode(std::vector<char32_t>(cps.begin() + static_cast<std::ptrdiff_t>(start), cps.begin() + static_cast<std::ptrdiff_t>(i)));
        word.key = text::Encode(key);
        words.push_back(std::move(word));
    }
    return words;
}

std::string LyricKey(const std::string& line) {
    std::string key;
    for (const LyricWord& word : SplitLyricWords(line)) {
        if (!key.empty()) {
            key += ' ';
        }
        key += word.key;
    }
    return key;
}

const char* BlockKindId(LyricBlockKind kind) {
    return BLOCK_IDS[static_cast<int>(kind)];
}

LyricDocument ParsePlainLyrics(const std::string& source) {
    LyricDocument document;
    document.format = "text";
    LyricBlock current;
    bool open = false;
    std::string voice;
    std::vector<std::string> voices;
    Delivery delivery = Delivery::Sung;
    const auto close = [&]() {
        if (open && (!current.lines.empty() || current.kind != LyricBlockKind::Unlabeled)) {
            document.blocks.push_back(std::move(current));
        }
        current = LyricBlock();
        open = false;
    };
    for (const std::string& raw : SplitLines(source)) {
        const std::string trimmed = text::Trim(raw);
        if (trimmed.empty()) {
            // A blank line after a tag alone ("[Instrumental]") keeps the tag; after
            // lines it ends the block, so an untagged paragraph never joins a chorus.
            if (open && !current.lines.empty()) {
                close();
            }
            continue;
        }
        if (IsTag(trimmed)) {
            const Tag tag = ReadTag(trimmed);
            if (tag.isVoice) {
                if (!tag.voices.empty()) {
                    voice = tag.voice;
                    voices = tag.voices;
                    // A narrator speaks until a tag says otherwise; a singer sings.
                    delivery = tag.spoken ? Delivery::Spoken : Delivery::Sung;
                } else {
                    delivery = tag.spoken ? Delivery::Spoken : Delivery::Sung;
                }
                continue;
            }
            close();
            current.kind = tag.kind;
            current.label = tag.label;
            current.number = tag.number;
            open = true;
            continue;
        }
        LyricLine line = MakeLine(raw, voice);
        line.voices = voices; // names for now: Finish turns them into ids
        line.delivery = delivery;
        if (line.words.empty() && !line.instrumental) {
            continue;
        }
        open = true;
        current.lines.push_back(std::move(line));
    }
    close();
    Finish(document, source);
    return document;
}

LyricDocument ParseLrcLyrics(const std::string& source) {
    const lrc::ParsedLRC parsed = lrc::ParseLRCDetailed(source);
    std::vector<LyricIssue> issues;
    for (const lrc::ParsedLRC::Issue& i : parsed.issues) {
        // A line with no tag is not lyrics an LRC player would show; the rest is a bad mark.
        issues.push_back({ i.code, i.line, -1, i.text, i.code != "untimed-line", false });
    }
    if (text::Trim(source).empty()) {
        issues.push_back({ "empty-file", 0, -1, "", true, false });
    } else if (parsed.timestampCount == 0) {
        issues.push_back({ "no-timestamps", 0, -1, "", true, false });
    }
    if (parsed.hadOffset && parsed.offsetMS != 0) {
        issues.push_back({ "offset-applied", 0, parsed.offsetMS, std::to_string(parsed.offsetMS) + " ms", false, true });
    }
    if (parsed.hadWordTimes) {
        issues.push_back({ "word-times-ignored", 0, -1, "", false, false });
    }

    struct Timed {
        LyricLine line;
        int number;
    };
    std::vector<Timed> timed;
    for (const lrc::ParsedLRC::Line& l : parsed.lines) {
        LyricLine line = MakeLine(l.text, "");
        if (text::Trim(l.text).empty()) {
            line.instrumental = true; // a blank timed line is where the singing stops
        }
        line.startMS = l.timeMS;
        line.timingSource = "lrc";
        // Several stamps on one line repeat it on purpose; only a stamp earlier than the
        // line before it in the file is out of order.
        if (!timed.empty() && l.number != timed.back().number && l.timeMS < timed.back().line.startMS) {
            issues.push_back({ "out-of-order", l.number, l.timeMS, line.text, false, true });
        }
        timed.push_back({ std::move(line), l.number });
    }
    std::stable_sort(timed.begin(), timed.end(), [](const Timed& a, const Timed& b) { return a.line.startMS < b.line.startMS; });
    // Two lines on the same time would leave the first one no time at all: they are sung
    // together, so they become one line.
    std::vector<LyricLine> lines;
    for (Timed& t : timed) {
        if (!lines.empty() && lines.back().startMS == t.line.startMS) {
            LyricLine& previous = lines.back();
            issues.push_back({ "duplicate-time", t.number, t.line.startMS, t.line.text, false, true });
            if (t.line.instrumental) {
                continue;
            }
            if (previous.instrumental) {
                previous = std::move(t.line);
                continue;
            }
            previous.text += " " + t.line.text;
            previous.words = SplitLyricWords(previous.text);
            continue;
        }
        lines.push_back(std::move(t.line));
    }
    LyricDocument document = FromTimedLines("lrc", std::move(lines), source);
    document.issues = std::move(issues);
    return document;
}

LyricDocument ParseSrtLyrics(const std::string& source) {
    std::vector<LyricLine> lines;
    int skipped = 0;
    for (const srt::Entry& entry : srt::ParseSRT(SplitLines(source))) {
        if (entry.text.empty() || entry.endMS <= entry.startMS) {
            ++skipped;
            continue;
        }
        LyricLine line = MakeLine(entry.text, "");
        line.startMS = static_cast<int>(entry.startMS);
        line.endMS = static_cast<int>(entry.endMS);
        line.timingSource = "srt";
        lines.push_back(std::move(line));
    }
    LyricDocument document = FromTimedLines("srt", std::move(lines), source);
    if (skipped > 0) {
        document.warnings.push_back("srt-entries-skipped");
    }
    return document;
}

LyricDocument LyricsFromTrack(const TimingTrackSpec& track) {
    std::vector<LyricLine> lines;
    std::string all;
    for (const TimingMark& phrase : track.layers.empty() ? std::vector<TimingMark>() : track.layers[0]) {
        LyricLine line = MakeLine(phrase.label, "");
        line.startMS = phrase.startMS;
        line.endMS = phrase.endMS;
        line.timingSource = "track";
        // The track's own words win over splitting the phrase again: they are what
        // the faces already move to.
        if (track.layers.size() > 1 && !line.instrumental) {
            std::vector<LyricWord> words;
            for (const TimingMark& mark : track.layers[1]) {
                if (mark.startMS >= phrase.startMS && mark.startMS < phrase.endMS) {
                    for (LyricWord& word : SplitLyricWords(mark.label)) {
                        words.push_back(std::move(word));
                    }
                }
            }
            if (!words.empty()) {
                line.words = std::move(words);
            }
        }
        all += phrase.label + "\n";
        lines.push_back(std::move(line));
    }
    return FromTimedLines("track", std::move(lines), all);
}

LyricDocument ParseLyrics(const std::string& source) {
    bool lrc = false;
    for (const std::string& line : SplitLines(source)) {
        const std::string trimmed = text::Trim(line);
        if (trimmed.find("-->") != std::string::npos) {
            return ParseSrtLyrics(source);
        }
        if (trimmed.size() > 6 && trimmed[0] == '[' && trimmed[1] >= '0' && trimmed[1] <= '9' && trimmed.find(':') != std::string::npos && trimmed.find(':') < trimmed.find(']')) {
            lrc = true;
        }
    }
    return lrc ? ParseLrcLyrics(source) : ParsePlainLyrics(source);
}

LyricDocument ParseLyricsFile(const std::string& path, const std::string& source) {
    std::string extension = std::filesystem::path(path).extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".lrc" ? ParseLrcLyrics(source) : ParseLyrics(source);
}

LanguageGuess DetectLyricLanguage(const std::string& source) {
    static const std::set<std::string> SPANISH = {
        "de", "la", "que", "el", "en", "y", "los", "se", "del", "las", "un", "por", "con", "no", "una", "su", "para", "es", "al", "lo",
        "como", "mas", "pero", "sus", "le", "ya", "o", "este", "si", "porque", "esta", "cuando", "muy", "sin", "sobre", "me", "hasta",
        "hay", "donde", "todo", "nos", "mi", "tu", "te", "yo", "ti", "estoy", "eres", "soy", "noche", "navidad", "amor", "corazon",
        "cielo", "feliz", "quiero", "vida", "dia", "ven", "vamos", "nunca", "siempre", "contigo", "conmigo",
    };
    static const std::set<std::string> ENGLISH = {
        "the", "and", "you", "to", "of", "a", "i", "it", "in", "is", "that", "me", "my", "your", "be", "on", "for", "this", "we",
        "love", "all", "with", "so", "what", "are", "not", "oh", "do", "know", "just", "can", "christmas", "night", "baby", "want",
        "when", "now", "like", "go", "there", "she", "he", "they", "will", "have", "dont", "im", "its", "snow", "let",
    };
    double spanish = 0;
    double english = 0;
    int words = 0;
    for (char32_t c : text::Decode(source)) {
        // ñ, accented vowels, ¿ and ¡ all but settle it.
        if (c == 0xF1 || c == 0xD1 || c == 0xBF || c == 0xA1 || c == 0xE1 || c == 0xE9 || c == 0xED || c == 0xF3 || c == 0xFA) {
            spanish += 2;
        }
    }
    for (const std::string& raw : SplitLines(source)) {
        for (const LyricWord& word : SplitLyricWords(raw)) {
            const std::string folded = LettersOnly(text::Fold(word.key));
            ++words;
            spanish += SPANISH.contains(folded) ? 1 : 0;
            english += ENGLISH.contains(folded) ? 1 : 0;
        }
    }
    LanguageGuess guess;
    const double total = spanish + english;
    if (words == 0 || total < 2) {
        return guess;
    }
    const double margin = std::abs(spanish - english) / total;
    if (margin < 0.2) {
        return guess;
    }
    guess.code = spanish > english ? "es" : "en";
    guess.confidence = std::min(1.0, margin * std::min(1.0, total / 6.0));
    return guess;
}

} // namespace composer
