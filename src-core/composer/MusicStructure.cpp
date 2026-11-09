#include "composer/MusicStructure.h"

#include "composer/LyricAligner.h"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <numeric>
#include <set>
#include <tuple>

namespace composer {

namespace {

constexpr int SPAN_GAP_MS = 6000;      // a longer pause between two lines starts another part
constexpr int INTERLUDE_MIN_MS = 6000; // a pause this long without words is a part of its own
constexpr int EDGE_MIN_MS = 2000;      // music before the first line or after the last
constexpr int MIN_SECTION_MS = 3000;
constexpr int SAME_PLACE_LINES = 2;    // repeats this close are one occurrence, "x, y, x, y"
constexpr int SAME_PLACE_MS = 12000;
constexpr double SAME_LINE = 0.6;

// Two bytes of UTF-8 for the accented letters of Spanish, Portuguese, French and
// Catalan: enough to compare what Whisper and a person write.
char Fold(unsigned char lead, unsigned char next) {
    if (lead != 0xC3) {
        return 0;
    }
    static const std::map<unsigned char, char> letters = {
        { 0xA0, 'a' }, { 0xA1, 'a' }, { 0xA2, 'a' }, { 0xA4, 'a' }, { 0x80, 'a' }, { 0x81, 'a' }, { 0x82, 'a' }, { 0x84, 'a' },
        { 0xA8, 'e' }, { 0xA9, 'e' }, { 0xAA, 'e' }, { 0xAB, 'e' }, { 0x88, 'e' }, { 0x89, 'e' }, { 0x8A, 'e' }, { 0x8B, 'e' },
        { 0xAC, 'i' }, { 0xAD, 'i' }, { 0xAE, 'i' }, { 0xAF, 'i' }, { 0x8C, 'i' }, { 0x8D, 'i' }, { 0x8E, 'i' }, { 0x8F, 'i' },
        { 0xB2, 'o' }, { 0xB3, 'o' }, { 0xB4, 'o' }, { 0xB6, 'o' }, { 0x92, 'o' }, { 0x93, 'o' }, { 0x94, 'o' }, { 0x96, 'o' },
        { 0xB9, 'u' }, { 0xBA, 'u' }, { 0xBB, 'u' }, { 0xBC, 'u' }, { 0x99, 'u' }, { 0x9A, 'u' }, { 0x9B, 'u' }, { 0x9C, 'u' },
        { 0xB1, 'n' }, { 0x91, 'n' }, { 0xA7, 'c' }, { 0x87, 'c' }
    };
    const auto it = letters.find(next);
    return it == letters.end() ? 0 : it->second;
}

double Similarity(const std::vector<std::string>& a, const std::vector<std::string>& b) {
    if (a.empty() || b.empty()) {
        return 0;
    }
    const std::set<std::string> left(a.begin(), a.end());
    const std::set<std::string> right(b.begin(), b.end());
    if (std::min(left.size(), right.size()) < 2) {
        return a == b ? 1.0 : 0.0;
    }
    int shared = 0;
    for (const std::string& w : left) {
        shared += right.contains(w) ? 1 : 0;
    }
    return static_cast<double>(shared) / static_cast<double>(left.size() + right.size() - shared);
}

double Round3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

struct Span {
    int first = 0; // line indexes
    int last = 0;
    bool repeated = false;
    std::set<int> groups;
    int family = -1;
    SectionKind kind = SectionKind::Verse;
    double confidence = 0.5;
    std::vector<std::string> evidence;
};

class Builder {
public:
    Builder(SongAnalysis& analysis, std::vector<SungLine> lines) :
        _a(analysis), _lines(std::move(lines)) {}

    StructureResult Run() {
        _r.lines = static_cast<int>(_lines.size());
        if (_lines.size() < 6) {
            _r.reason = "too-few-lines";
            return _r;
        }
        if (_a.musicEndMS <= _a.musicStartMS) {
            _r.reason = "no-music";
            return _r;
        }
        GroupLines();
        MakeSpans();
        if (!NameSpans()) {
            return _r;
        }
        Lay();
        _r.applied = true;
        return _r;
    }

private:
    void GroupLines() {
        const int n = static_cast<int>(_lines.size());
        std::vector<std::vector<std::string>> words(n);
        for (int i = 0; i < n; ++i) {
            words[i] = LineWords(_lines[i].text);
        }
        _group.resize(n);
        std::iota(_group.begin(), _group.end(), 0);
        const auto root = [&](int i) {
            while (_group[i] != i) {
                i = _group[i];
            }
            return i;
        };
        for (int i = 0; i < n; ++i) {
            for (int j = i + 1; j < n; ++j) {
                if (Similarity(words[i], words[j]) >= SAME_LINE) {
                    _group[root(j)] = root(i);
                }
            }
        }
        for (int i = 0; i < n; ++i) {
            _group[i] = root(i);
        }
        // A group recurs when it comes back after something else was sung: a line
        // repeated straight away is one occurrence, as "x, y, x, y" is.
        std::map<int, std::vector<int>> members;
        for (int i = 0; i < n; ++i) {
            members[_group[i]].push_back(i);
        }
        for (const auto& [g, list] : members) {
            int places = 1;
            for (size_t k = 1; k < list.size(); ++k) {
                const bool samePlace = list[k] - list[k - 1] <= SAME_PLACE_LINES && _lines[list[k]].startMS - _lines[list[k - 1]].endMS <= SAME_PLACE_MS;
                places += samePlace ? 0 : 1;
            }
            if (places >= 2) {
                _recurring.insert(g);
            }
        }
    }

    void MakeSpans() {
        for (int i = 0; i < static_cast<int>(_lines.size()); ++i) {
            const bool repeated = _recurring.contains(_group[i]);
            const bool pause = i > 0 && _lines[i].startMS - _lines[i - 1].endMS > SPAN_GAP_MS;
            if (_spans.empty() || pause || _spans.back().repeated != repeated) {
                Span s;
                s.first = i;
                s.last = i;
                s.repeated = repeated;
                _spans.push_back(s);
            }
            _spans.back().last = i;
            if (repeated) {
                _spans.back().groups.insert(_group[i]);
            }
        }
    }

    bool NameSpans() {
        // Spans that share a group of lines are the same part sung again.
        std::vector<int> family(_spans.size(), -1);
        int families = 0;
        for (size_t i = 0; i < _spans.size(); ++i) {
            if (!_spans[i].repeated || family[i] >= 0) {
                continue;
            }
            family[i] = families;
            std::set<int> groups = _spans[i].groups;
            bool grew = true;
            while (grew) {
                grew = false;
                for (size_t j = 0; j < _spans.size(); ++j) {
                    if (!_spans[j].repeated || family[j] >= 0) {
                        continue;
                    }
                    const bool shares = std::any_of(_spans[j].groups.begin(), _spans[j].groups.end(), [&](int g) { return groups.contains(g); });
                    if (shares) {
                        family[j] = families;
                        groups.insert(_spans[j].groups.begin(), _spans[j].groups.end());
                        grew = true;
                    }
                }
            }
            ++families;
        }
        for (size_t i = 0; i < _spans.size(); ++i) {
            _spans[i].family = family[i];
        }
        _r.families = families;
        if (families == 0) {
            _r.reason = "no-line-sung-again";
            return false;
        }
        // The chorus is the part that comes back most often, then the one with more
        // lines, then the louder: a chorus is sung again, and usually the fullest.
        int chorus = -1;
        std::tuple<int, int, double> best{ -1, -1, -1.0 };
        for (int f = 0; f < families; ++f) {
            int count = 0;
            int lines = 0;
            double energy = 0;
            for (const Span& s : _spans) {
                if (s.family == f) {
                    ++count;
                    lines += s.last - s.first + 1;
                    energy += MeanIntensity(_lines[s.first].startMS, _lines[s.last].endMS);
                }
            }
            const std::tuple<int, int, double> score{ count, lines, energy / std::max(1, count) };
            if (score > best) {
                best = score;
                chorus = f;
            }
        }
        if (std::get<0>(best) < 2) {
            _r.reason = "no-part-sung-again";
            return false;
        }
        std::vector<int> chorusStarts;
        for (const Span& s : _spans) {
            if (s.family == chorus) {
                chorusStarts.push_back(_lines[s.first].startMS);
            }
        }
        // A bridge is the one part sung once, after the chorus came twice and with the
        // chorus still to come: the last such part, the others being verses.
        int bridge = -1;
        for (size_t i = 0; i < _spans.size(); ++i) {
            const int start = _lines[_spans[i].first].startMS;
            const int before = static_cast<int>(std::count_if(chorusStarts.begin(), chorusStarts.end(), [&](int c) { return c < start; }));
            const bool after = std::any_of(chorusStarts.begin(), chorusStarts.end(), [&](int c) { return c > start; });
            if (!_spans[i].repeated && before >= 2 && after) {
                bridge = static_cast<int>(i);
            }
        }
        for (size_t i = 0; i < _spans.size(); ++i) {
            Span& s = _spans[i];
            if (s.family == chorus) {
                s.kind = SectionKind::Chorus;
                s.confidence = 0.7;
                s.evidence = { "lyrics-sung-again", "most-repeated-part" };
                ++_r.choruses;
                continue;
            }
            const bool beforeChorus = i + 1 < _spans.size() && _spans[i + 1].family == chorus && _lines[_spans[i + 1].first].startMS - _lines[s.last].endMS <= 3000;
            if (s.repeated && beforeChorus) {
                s.kind = SectionKind::PreChorus;
                s.confidence = 0.45;
                s.evidence = { "lyrics-sung-again", "leads-into-chorus" };
                continue;
            }
            if (static_cast<int>(i) == bridge) {
                s.kind = SectionKind::Bridge;
                s.confidence = 0.35;
                s.evidence = { "lyrics-sung-once", "between-late-choruses" };
                continue;
            }
            s.kind = SectionKind::Verse;
            s.confidence = s.repeated ? 0.5 : 0.55;
            s.evidence = { s.repeated ? "lyrics-other-part-sung-again" : "lyrics-sung-once" };
            ++_r.verses;
        }
        _chorusFamily = chorus;
        return true;
    }

    double MeanIntensity(int from, int to) const {
        double sum = 0;
        int count = 0;
        for (const EnergyPoint& p : _a.energy) {
            if (p.timeMS >= from && p.timeMS < to) {
                sum += p.intensity;
                ++count;
            }
        }
        return count == 0 ? 0 : sum / count;
    }

    // The bar line just before a part starts, if one is close; else the beat; else a
    // little before the first word, never earlier than `floor`.
    int StartBefore(int timeMS, int floor) const {
        int best = -1;
        for (const MusicalEvent& e : _a.events) {
            if (e.type == MusicalEventType::Downbeat && e.timeMS <= timeMS + 150 && e.timeMS >= std::max(floor, timeMS - 2500) && e.timeMS > best) {
                best = e.timeMS;
            }
        }
        if (best >= 0) {
            return best;
        }
        for (const MusicalEvent& e : _a.events) {
            if (e.type == MusicalEventType::Beat && e.timeMS <= timeMS + 100 && e.timeMS >= std::max(floor, timeMS - 1000) && e.timeMS > best) {
                best = e.timeMS;
            }
        }
        return best >= 0 ? best : std::max(floor, timeMS - std::min(500, std::max(0, timeMS - floor) / 2));
    }

    void Add(std::vector<SongSection>& out, int start, int end, SectionKind kind, double confidence, std::vector<std::string> evidence, int repeatGroup) const {
        if (end <= start) {
            return;
        }
        SongSection s;
        s.startMS = start;
        s.endMS = end;
        s.kind = kind;
        s.confidence = Round3(confidence);
        s.evidence = std::move(evidence);
        s.repeatGroup = repeatGroup;
        out.push_back(std::move(s));
    }

    void Lay() {
        std::vector<SongSection> laid;
        const int musicStart = _a.musicStartMS;
        const int musicEnd = _a.musicEndMS;
        std::map<int, int> groupOf;
        int nextGroup = 1;
        int cursor = musicStart;
        for (size_t i = 0; i < _spans.size(); ++i) {
            const Span& s = _spans[i];
            const int firstWord = std::max(musicStart, _lines[s.first].startMS);
            int start = i == 0 && firstWord - musicStart < EDGE_MIN_MS ? musicStart : StartBefore(firstWord, cursor);
            if (i == 0 && start > musicStart) {
                Add(laid, musicStart, start, SectionKind::Intro, 0.6, { "before-first-line" }, -1);
            } else if (i > 0) {
                const int previousEnd = _lines[_spans[i - 1].last].endMS;
                if (firstWord - previousEnd >= INTERLUDE_MIN_MS) {
                    const int pauseStart = std::max(cursor, StartBefore(previousEnd + 500, previousEnd));
                    laid.back().endMS = pauseStart;
                    Add(laid, pauseStart, start, SectionKind::Interlude, 0.5, { "no-lyrics-between-parts" }, -1);
                } else if (!laid.empty()) {
                    laid.back().endMS = start;
                }
            }
            int group = -1;
            if (s.family >= 0) {
                if (s.family == _chorusFamily) {
                    group = 0;
                } else {
                    const auto [it, added] = groupOf.try_emplace(s.family, nextGroup);
                    nextGroup += added ? 1 : 0;
                    group = it->second;
                }
            }
            Add(laid, start, std::max(start + 1, _lines[s.last].endMS), s.kind, s.confidence, s.evidence, group);
            cursor = laid.back().endMS;
        }
        const int lastWord = _lines[_spans.back().last].endMS;
        if (musicEnd - lastWord >= EDGE_MIN_MS) {
            const int outroStart = std::max(cursor, StartBefore(lastWord + 500, lastWord));
            laid.back().endMS = outroStart;
            Add(laid, outroStart, musicEnd, SectionKind::Outro, 0.6, { "after-last-line" }, -1);
        } else {
            laid.back().endMS = musicEnd;
        }

        // Too short to be a part of its own: it joins the one before, or the next.
        for (size_t i = 0; i < laid.size();) {
            if (laid.size() > 1 && laid[i].endMS - laid[i].startMS < MIN_SECTION_MS) {
                if (i > 0) {
                    laid[i - 1].endMS = laid[i].endMS;
                } else {
                    laid[i + 1].startMS = laid[i].startMS;
                }
                laid.erase(laid.begin() + static_cast<long>(i));
                continue;
            }
            ++i;
        }
        // Parts of the same kind side by side, when neither repeats, are one part.
        for (size_t i = 1; i < laid.size();) {
            if (laid[i].kind == laid[i - 1].kind && laid[i].repeatGroup == laid[i - 1].repeatGroup && laid[i].kind != SectionKind::Chorus) {
                laid[i - 1].endMS = laid[i].endMS;
                laid.erase(laid.begin() + static_cast<long>(i));
                continue;
            }
            ++i;
        }

        std::vector<int> audioStarts;
        for (const SongSection& old : _a.sections) {
            audioStarts.push_back(old.startMS);
        }
        for (SongSection& s : laid) {
            s.energy = Round3(MeanIntensity(s.startMS, s.endMS));
            const bool agrees = std::any_of(audioStarts.begin(), audioStarts.end(), [&](int t) { return t > _a.musicStartMS && std::abs(t - s.startMS) <= 2500; });
            if (agrees) {
                s.evidence.push_back("audio-change-near");
                s.confidence = Round3(std::min(0.9, s.confidence + 0.1));
            }
        }

        // The silences the audio found stay as they were, at the edges and inside.
        std::vector<SongSection> result;
        for (const SongSection& old : _a.sections) {
            if (std::find(old.evidence.begin(), old.evidence.end(), "silence") != old.evidence.end()) {
                result.push_back(old);
            }
        }
        for (const SongSection& s : laid) {
            std::vector<std::pair<int, int>> pieces = { { s.startMS, s.endMS } };
            for (const SongSection& silent : result) {
                std::vector<std::pair<int, int>> next;
                for (const auto& [from, to] : pieces) {
                    if (silent.endMS <= from || silent.startMS >= to) {
                        next.push_back({ from, to });
                        continue;
                    }
                    if (silent.startMS > from) {
                        next.push_back({ from, silent.startMS });
                    }
                    if (silent.endMS < to) {
                        next.push_back({ silent.endMS, to });
                    }
                }
                pieces = std::move(next);
            }
            for (const auto& [from, to] : pieces) {
                if (to - from >= 1000) {
                    SongSection piece = s;
                    piece.startMS = from;
                    piece.endMS = to;
                    result.push_back(std::move(piece));
                }
            }
        }
        std::sort(result.begin(), result.end(), [](const SongSection& a, const SongSection& b) { return a.startMS < b.startMS; });
        // No gap and no overlap: each part ends where the next starts.
        for (size_t i = 0; i + 1 < result.size(); ++i) {
            result[i].endMS = result[i + 1].startMS;
        }
        if (!result.empty()) {
            result.front().startMS = 0;
            result.back().endMS = _a.durationMS;
        }
        for (const SongSection& s : result) {
            std::string evidence;
            for (const std::string& e : s.evidence) {
                evidence += (evidence.empty() ? "" : ", ") + e;
            }
            _r.notes.push_back(fmt::format("{}-{} {} ({})", s.startMS, s.endMS, SectionKindId(s.kind), evidence));
        }
        _a.sections = std::move(result);
        ApplySectionCorrections(_a);
    }

    SongAnalysis& _a;
    std::vector<SungLine> _lines;
    std::vector<int> _group;
    std::set<int> _recurring;
    std::vector<Span> _spans;
    int _chorusFamily = -1;
    StructureResult _r;
};

} // namespace

std::vector<std::string> LineWords(const std::string& text) {
    std::vector<std::string> words;
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        char letter = 0;
        if (c < 0x80) {
            letter = std::isalnum(c) ? static_cast<char>(std::tolower(c)) : 0;
        } else if (i + 1 < text.size()) {
            letter = Fold(c, static_cast<unsigned char>(text[i + 1]));
            if (letter != 0) {
                ++i;
            }
        }
        if (letter != 0) {
            current.push_back(letter);
        } else if (!current.empty()) {
            words.push_back(std::move(current));
            current.clear();
        }
    }
    if (!current.empty()) {
        words.push_back(std::move(current));
    }
    return words;
}

std::vector<SungLine> TimedLinesFrom(const LyricAlignment& alignment) {
    std::vector<SungLine> lines;
    for (const AlignedPhrase& p : alignment.phrases) {
        if (p.instrumental || p.startMS < 0 || p.endMS <= p.startMS) {
            continue;
        }
        // The second half of a split phrase continues the line it came from.
        if (p.id.find('~') != std::string::npos && !lines.empty()) {
            lines.back().endMS = std::max(lines.back().endMS, p.endMS);
            lines.back().text += " " + p.text;
            continue;
        }
        lines.push_back({ p.startMS, p.endMS, p.text });
    }
    std::sort(lines.begin(), lines.end(), [](const SungLine& a, const SungLine& b) { return a.startMS < b.startMS; });
    return lines;
}

StructureResult RefineSectionsWithLyrics(SongAnalysis& analysis, const std::vector<SungLine>& lines) {
    std::vector<SungLine> inside;
    for (const SungLine& l : lines) {
        if (l.endMS > l.startMS && l.startMS < analysis.durationMS && !LineWords(l.text).empty()) {
            inside.push_back(l);
        }
    }
    std::sort(inside.begin(), inside.end(), [](const SungLine& a, const SungLine& b) { return a.startMS < b.startMS; });
    return Builder(analysis, std::move(inside)).Run();
}

} // namespace composer
