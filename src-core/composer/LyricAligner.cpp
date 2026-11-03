#include "composer/LyricAligner.h"

#include "composer/ComposerText.h"
#include "utils/ExternalHooks.h"
#include "xLightsVersion.h"

#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace composer {

namespace {

const char* const PROVIDER_IDS[] = { "auto", "existing-track", "manual-phrases", "line-times", "experimental", "none" };
const char* const LEVEL_IDS[] = { "phrase", "word", "phoneme" };
const char* const STATE_IDS[] = { "detected", "corrected", "locked", "confirmed" };
const std::set<std::string> SHAPES = { "AI", "E", "FV", "L", "MBP", "O", "U", "WQ", "etc", "rest" };

constexpr double TRACK_CONFIDENCE = 1.0;
constexpr double MANUAL_CONFIDENCE = 1.0;
constexpr double SRT_CONFIDENCE = 0.7;
// Starts only: every LRC line ends where the estimate says, so it is never trusted unseen.
constexpr double LRC_CONFIDENCE = 0.4;
constexpr double EXPERIMENTAL_CONFIDENCE = 0.1;
// Inside a phrase, words and shapes are spread by syllables: an estimate whatever the
// phrase's own confidence, and phonemes more so than words.
constexpr double WORD_SHARE = 0.5;
constexpr double PHONEME_SHARE = 0.4;

double Round3(double value) {
    return std::round(value * 1000.0) / 1000.0;
}

int ToFrame(double ms, int frame) {
    return static_cast<int>(std::lround(ms / frame)) * frame;
}

bool IsUserState(Provenance state) {
    return state != Provenance::Detected;
}

struct WordPlan {
    AlignedWord word;
    Pronunciation pronunciation;
    double weight = 1;
    bool fixed = false;
};

double WeightOf(const Pronunciation& p, bool phraseFinal, double hold) {
    if (!p.Ok()) {
        return 1.0; // one syllable's worth: its time is still sung, if not shaped
    }
    double weight = 0;
    for (size_t s = 0; s < p.syllables.size(); ++s) {
        double w = p.syllables[s].weight;
        if (phraseFinal && s + 1 == p.syllables.size()) {
            w *= hold;
        }
        weight += w;
    }
    return std::max(0.2, weight);
}

double SyllableWeightOf(const LyricLine& line, const PhonemizerOptions& options) {
    double weight = 0;
    for (const LyricWord& word : line.words) {
        weight += WeightOf(PhonemizeSpanish(word.key, options), false, 1.0);
    }
    return weight;
}

// Spreads `items` over [from, to] in proportion to their weights, on the frame grid.
// Returns false when there are not enough frames for every item to get one.
bool Spread(const std::vector<double>& weights, int from, int to, int frame, std::vector<std::pair<int, int>>& out) {
    out.clear();
    const int frames = (to - from) / frame;
    if (weights.empty() || frames < static_cast<int>(weights.size())) {
        return false;
    }
    const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
    double cumulative = 0;
    int start = from;
    for (size_t i = 0; i < weights.size(); ++i) {
        cumulative += weights[i];
        const int remaining = static_cast<int>(weights.size() - i - 1);
        int end = i + 1 == weights.size() ? to : ToFrame(from + (to - from) * cumulative / total, frame);
        end = std::max(end, start + frame);
        end = std::min(end, to - remaining * frame);
        out.push_back({ start, end });
        start = end;
    }
    return true;
}

struct Segment {
    std::string shape;
    std::string phoneme;
    double start = 0;
    double end = 0;
};

// Shapes for one word: each syllable gets its share of the word, consonants a short
// fixed time inside it and the vowel the rest, so a held vowel stays open.
std::vector<Segment> ShapeWord(const Pronunciation& p, int from, int to, const AlignerOptions& options, bool phraseFinal) {
    std::vector<Segment> segments;
    std::vector<double> weights;
    for (size_t s = 0; s < p.syllables.size(); ++s) {
        double w = p.syllables[s].weight;
        if (phraseFinal && s + 1 == p.syllables.size()) {
            w *= options.phraseFinalHold;
        }
        weights.push_back(w);
    }
    const double total = std::accumulate(weights.begin(), weights.end(), 0.0);
    double at = from;
    for (size_t s = 0; s < p.syllables.size(); ++s) {
        const Syllable& syllable = p.syllables[s];
        const double length = (to - from) * weights[s] / total;
        const double end = at + length;
        int consonants = 0;
        for (int i = syllable.firstPhone; i < syllable.firstPhone + syllable.phoneCount; ++i) {
            consonants += i == syllable.nucleus ? 0 : 1;
        }
        const double each = consonants == 0 ? 0 : std::min<double>(options.consonantMS, length * 0.4 / consonants);
        double cursor = at;
        const double nucleusLength = length - each * consonants;
        for (int i = syllable.firstPhone; i < syllable.firstPhone + syllable.phoneCount; ++i) {
            const std::string& id = p.phones[i].id;
            const double size = i == syllable.nucleus ? nucleusLength : each;
            segments.push_back({ VisemeOf(id), id, cursor, cursor + size });
            cursor += size;
        }
        at = end;
    }
    return segments;
}

// The same shape twice in a row is one shape, and nothing is shorter than a frame:
// a mouth that opens and closes within 25 ms only flickers.
std::vector<Segment> Settle(std::vector<Segment> segments, int from, int to, int frame) {
    std::vector<Segment> merged;
    for (Segment& s : segments) {
        if (!merged.empty() && merged.back().shape == s.shape) {
            merged.back().end = s.end;
            merged.back().phoneme += "+" + s.phoneme;
        } else {
            merged.push_back(std::move(s));
        }
    }
    std::vector<Segment> settled;
    for (size_t i = 0; i < merged.size(); ++i) {
        Segment s = merged[i];
        s.start = i == 0 ? from : settled.empty() ? from : settled.back().end;
        s.end = i + 1 == merged.size() ? to : std::min<double>(to, ToFrame(s.end, frame));
        if (s.end - s.start < frame) {
            // A closed mouth is worth a frame: "amor" without its "m" looks like "aor".
            const bool canTake = s.shape == "MBP" && i + 1 < merged.size() && ToFrame(merged[i + 1].end, frame) - s.start >= 2 * frame;
            if (canTake) {
                s.end = s.start + frame;
            } else {
                if (!settled.empty()) {
                    settled.back().phoneme += "+" + s.phoneme;
                } else if (i + 1 < merged.size()) {
                    merged[i + 1].phoneme = s.phoneme + "+" + merged[i + 1].phoneme;
                }
                continue;
            }
        }
        if (!settled.empty() && settled.back().shape == s.shape) {
            settled.back().end = s.end;
            settled.back().phoneme += "+" + s.phoneme;
        } else {
            settled.push_back(std::move(s));
        }
    }
    if (!settled.empty()) {
        settled.back().end = to;
        if (settled.back().end <= settled.back().start) {
            settled.pop_back();
            if (!settled.empty()) {
                settled.back().end = to;
            }
        }
    }
    return settled;
}

const LyricCorrection* FindCorrection(const std::vector<LyricCorrection>& corrections, LyricCorrection::Level level, const std::string& id) {
    for (auto it = corrections.rbegin(); it != corrections.rend(); ++it) {
        if (it->level == level && it->targetId == id) {
            return &*it;
        }
    }
    return nullptr;
}

class Aligner {
public:
    Aligner(const AlignerInput& input, const AlignerOptions& options, LyricAlignment& out) :
        _in(input),
        _opt(options),
        _a(out),
        _frame(std::max(1, options.frameMS)) {}

    void Run() {
        for (const LyricCorrection& c : _in.corrections) {
            if (IsUserState(c.state)) {
                _a.corrections.push_back(c);
            }
        }
        const LyricDocument* document = _in.document;
        if (document == nullptr || document->Empty()) {
            _a.warnings.push_back("no-lyrics");
            _a.quality = "none";
            _a.provider = AlignmentProvider::None;
            return;
        }
        _a.language = document->language;
        if (document->language == "en") {
            _a.warnings.push_back("language-unsupported");
        } else if (document->language != "es") {
            _a.warnings.push_back("language-undetermined");
        }
        _a.provider = ChooseProvider(*document);
        BuildPhrases(*document);
        TimePhrases(*document);
        ApplyPhraseCorrections();
        TimeWords();
        ShapeWords();
        ApplyPhonemeCorrections();
        AddRests();
        Summarize();
    }

private:
    AlignmentProvider ChooseProvider(const LyricDocument& document) const {
        if (_in.provider != AlignmentProvider::Auto) {
            return _in.provider;
        }
        if (_in.existingTrack != nullptr && !_in.existingTrack->layers.empty() && !_in.existingTrack->layers[0].empty()) {
            return AlignmentProvider::ExistingTrack;
        }
        const bool marks = std::any_of(_a.corrections.begin(), _a.corrections.end(), [](const LyricCorrection& c) { return c.level == LyricCorrection::Level::Phrase && c.startMS >= 0; });
        if (marks) {
            return AlignmentProvider::ManualPhrases;
        }
        if (document.HasTimes()) {
            return AlignmentProvider::LineTimes;
        }
        if (_opt.allowExperimental && _in.analysis != nullptr) {
            return AlignmentProvider::Experimental;
        }
        return AlignmentProvider::None;
    }

    void BuildPhrases(const LyricDocument& document) {
        for (const LyricLine* line : document.Lines()) {
            AlignedPhrase phrase;
            phrase.id = line->id;
            phrase.text = line->text;
            phrase.voice = line->voice;
            phrase.instrumental = line->instrumental;
            _a.phrases.push_back(std::move(phrase));
            _lines.push_back(line);
        }
    }

    void TimePhrases(const LyricDocument& document) {
        switch (_a.provider) {
        case AlignmentProvider::ExistingTrack:
        case AlignmentProvider::LineTimes:
        case AlignmentProvider::ManualPhrases:
            // Line times from the source are used whatever the provider: a person's
            // marks then replace them phrase by phrase.
            TimeFromLines();
            break;
        case AlignmentProvider::Experimental:
            TimeExperimentally();
            break;
        default:
            break;
        }
        (void)document;
    }

    void TimeFromLines() {
        bool estimatedEnds = false;
        for (size_t i = 0; i < _lines.size(); ++i) {
            const LyricLine& line = *_lines[i];
            AlignedPhrase& phrase = _a.phrases[i];
            if (line.startMS < 0) {
                continue;
            }
            int end = line.endMS;
            if (line.timingSource == "lrc") {
                // LRC gives starts only: a line ends where the next one begins, but no
                // later than a generous time per syllable, since singers stop between.
                int next = -1;
                for (size_t k = i + 1; k < _lines.size() && next < 0; ++k) {
                    next = _lines[k]->startMS > line.startMS ? _lines[k]->startMS : -1;
                }
                const int syllables = std::max(1, static_cast<int>(std::ceil(SyllableWeightOf(line, _opt.phonemizer))));
                const int cap = line.startMS + std::max(1500, syllables * _opt.lrcMaxSyllableMS);
                end = next >= 0 ? std::min(next, cap) : cap;
                estimatedEnds = true;
            }
            if (_in.durationMS > 0) {
                end = std::min(end, _in.durationMS);
            }
            if (end <= line.startMS) {
                continue;
            }
            phrase.startMS = ToFrame(line.startMS, _frame);
            phrase.endMS = std::max(phrase.startMS + _frame, ToFrame(end, _frame));
            phrase.source = line.timingSource;
            phrase.confidence = line.timingSource == "track" ? TRACK_CONFIDENCE : line.timingSource == "srt" ? SRT_CONFIDENCE : LRC_CONFIDENCE;
        }
        if (estimatedEnds) {
            _a.warnings.push_back("lrc-line-ends-estimated");
        }
    }

    // Lines laid over where a voice was estimated, by syllables. Its errors were
    // measured in seconds, so it is only ever offered as a starting point.
    void TimeExperimentally() {
        _a.warnings.push_back("experimental-alignment");
        std::vector<TimeRange> spans;
        if (_in.analysis != nullptr) {
            for (const VocalSegment& v : _in.analysis->vocalSegments) {
                spans.push_back({ v.startMS, v.endMS });
            }
            if (spans.empty() && _in.analysis->musicEndMS > _in.analysis->musicStartMS) {
                spans.push_back({ _in.analysis->musicStartMS, _in.analysis->musicEndMS });
            }
        }
        long available = 0;
        for (const TimeRange& span : spans) {
            available += span.endMS - span.startMS;
        }
        std::vector<double> weights;
        double total = 0;
        for (const LyricLine* line : _lines) {
            weights.push_back(line->instrumental ? 0 : SyllableWeightOf(*line, _opt.phonemizer));
            total += weights.back();
        }
        if (available <= 0 || total <= 0) {
            _a.warnings.push_back("experimental-no-voice");
            return;
        }
        const auto timeAt = [&](double share) {
            double left = share * available;
            for (const TimeRange& span : spans) {
                const double length = span.endMS - span.startMS;
                if (left <= length) {
                    return span.startMS + left;
                }
                left -= length;
            }
            return static_cast<double>(spans.back().endMS);
        };
        double cumulative = 0;
        for (size_t i = 0; i < _lines.size(); ++i) {
            const double from = timeAt(cumulative / total);
            cumulative += weights[i];
            const double to = timeAt(cumulative / total);
            if (weights[i] <= 0 || to - from < _frame) {
                continue;
            }
            AlignedPhrase& phrase = _a.phrases[i];
            phrase.startMS = ToFrame(from, _frame);
            phrase.endMS = std::max(phrase.startMS + _frame, ToFrame(to, _frame));
            phrase.source = "experimental";
            phrase.confidence = EXPERIMENTAL_CONFIDENCE;
        }
    }

    AlignedPhrase* FindPhrase(const std::string& id) {
        for (AlignedPhrase& phrase : _a.phrases) {
            if (phrase.id == id) {
                return &phrase;
            }
        }
        return nullptr;
    }

    void ApplyPhraseCorrections() {
        std::set<std::string> applied;
        for (auto it = _a.corrections.rbegin(); it != _a.corrections.rend(); ++it) {
            const LyricCorrection& c = *it;
            if (c.level != LyricCorrection::Level::Phrase || !applied.insert(c.targetId).second) {
                continue;
            }
            AlignedPhrase* phrase = FindPhrase(c.targetId);
            if (phrase == nullptr) {
                _a.conflicts.push_back({ c.targetId, "target-missing" });
                continue;
            }
            if (c.startMS < 0 || c.endMS <= c.startMS) {
                phrase->state = c.state; // a confirmation without new times
                continue;
            }
            phrase->originalStartMS = phrase->startMS;
            phrase->originalEndMS = phrase->endMS;
            phrase->startMS = ToFrame(c.startMS, _frame);
            phrase->endMS = std::max(phrase->startMS + _frame, ToFrame(c.endMS, _frame));
            phrase->state = c.state;
            phrase->source = "manual";
            phrase->confidence = MANUAL_CONFIDENCE;
        }
        // Detected phrases make way for corrected ones, never the other way round.
        for (size_t i = 0; i < _a.phrases.size(); ++i) {
            AlignedPhrase& phrase = _a.phrases[i];
            if (phrase.startMS < 0 || IsUserState(phrase.state)) {
                continue;
            }
            for (const AlignedPhrase& other : _a.phrases) {
                if (&other == &phrase || other.startMS < 0 || !IsUserState(other.state)) {
                    continue;
                }
                if (other.startMS < phrase.endMS && other.endMS > phrase.startMS) {
                    if (other.startMS > phrase.startMS) {
                        phrase.endMS = other.startMS;
                    } else {
                        phrase.startMS = other.endMS;
                    }
                }
            }
            if (phrase.endMS - phrase.startMS < _frame) {
                phrase.startMS = phrase.endMS = -1;
                _a.warnings.push_back("phrase-displaced-by-correction");
            }
        }
    }

    // Words of a track are kept where they are; otherwise they are spread over the
    // phrase by syllables, around any word a person placed.
    void TimeWords() {
        for (size_t i = 0; i < _a.phrases.size(); ++i) {
            const AlignedPhrase& phrase = _a.phrases[i];
            const LyricLine& line = *_lines[i];
            if (phrase.startMS < 0 || phrase.instrumental || line.words.empty()) {
                continue;
            }
            std::vector<WordPlan> plans;
            for (size_t w = 0; w < line.words.size(); ++w) {
                WordPlan plan;
                plan.word.id = fmt::format("{}/{}:{}", phrase.id, w, line.words[w].key);
                plan.word.phraseId = phrase.id;
                plan.word.text = line.words[w].text;
                plan.word.key = line.words[w].key;
                plan.pronunciation = PhonemizeSpanish(line.words[w].key, _opt.phonemizer);
                plan.word.pronunciation = plan.pronunciation.source;
                plan.word.phonemes = PhonemeString(plan.pronunciation);
                plan.weight = WeightOf(plan.pronunciation, w + 1 == line.words.size(), _opt.phraseFinalHold);
                plans.push_back(std::move(plan));
            }
            if (!TimeWordsFromTrack(phrase, plans)) {
                TimeWordsBySyllables(phrase, plans);
            }
            for (WordPlan& plan : plans) {
                if (plan.word.endMS > plan.word.startMS) {
                    _pronunciations[plan.word.id] = plan.pronunciation;
                    _a.words.push_back(std::move(plan.word));
                }
            }
        }
    }

    bool TimeWordsFromTrack(const AlignedPhrase& phrase, std::vector<WordPlan>& plans) {
        if (_a.provider != AlignmentProvider::ExistingTrack || phrase.source != "track" || _in.existingTrack == nullptr || _in.existingTrack->layers.size() < 2) {
            return false;
        }
        std::vector<const TimingMark*> marks;
        for (const TimingMark& mark : _in.existingTrack->layers[1]) {
            if (mark.startMS >= phrase.startMS && mark.startMS < phrase.endMS) {
                marks.push_back(&mark);
            }
        }
        if (marks.size() != plans.size()) {
            return false; // a mark holding several words: spread them instead
        }
        for (size_t w = 0; w < plans.size(); ++w) {
            plans[w].word.startMS = marks[w]->startMS;
            plans[w].word.endMS = marks[w]->endMS;
            plans[w].word.source = "track";
            plans[w].word.confidence = TRACK_CONFIDENCE;
        }
        ApplyWordCorrections(plans);
        return true;
    }

    void ApplyWordCorrections(std::vector<WordPlan>& plans) {
        for (WordPlan& plan : plans) {
            const LyricCorrection* c = FindCorrection(_a.corrections, LyricCorrection::Level::Word, plan.word.id);
            if (c == nullptr) {
                continue;
            }
            _appliedWords.insert(plan.word.id);
            plan.word.state = c->state;
            if (c->startMS >= 0 && c->endMS > c->startMS) {
                plan.word.originalStartMS = plan.word.startMS;
                plan.word.originalEndMS = plan.word.endMS;
                plan.word.startMS = ToFrame(c->startMS, _frame);
                plan.word.endMS = std::max(plan.word.startMS + _frame, ToFrame(c->endMS, _frame));
                plan.word.source = "manual";
                plan.word.confidence = MANUAL_CONFIDENCE;
                plan.fixed = true;
            }
        }
    }

    void TimeWordsBySyllables(const AlignedPhrase& phrase, std::vector<WordPlan>& plans) {
        // Where the words would go without corrections, to record what they replaced.
        std::vector<double> allWeights;
        for (const WordPlan& plan : plans) {
            allWeights.push_back(plan.weight);
        }
        std::vector<std::pair<int, int>> natural;
        Spread(allWeights, phrase.startMS, phrase.endMS, _frame, natural);
        for (size_t w = 0; w < plans.size() && w < natural.size(); ++w) {
            plans[w].word.startMS = natural[w].first;
            plans[w].word.endMS = natural[w].second;
        }
        ApplyWordCorrections(plans);
        for (WordPlan& plan : plans) {
            if (plan.fixed && (plan.word.startMS < phrase.startMS || plan.word.endMS > phrase.endMS)) {
                _a.conflicts.push_back({ plan.word.id, "outside-phrase" });
            }
        }

        // Runs of detected words between fixed ones share the time those leave.
        size_t w = 0;
        bool tooShort = false;
        while (w < plans.size()) {
            if (plans[w].fixed) {
                ++w;
                continue;
            }
            size_t last = w;
            while (last + 1 < plans.size() && !plans[last + 1].fixed) {
                ++last;
            }
            const int from = w == 0 ? phrase.startMS : plans[w - 1].word.endMS;
            const int to = last + 1 < plans.size() ? plans[last + 1].word.startMS : phrase.endMS;
            std::vector<double> weights;
            for (size_t k = w; k <= last; ++k) {
                weights.push_back(plans[k].weight);
            }
            std::vector<std::pair<int, int>> spans;
            const bool fits = Spread(weights, from, to, _frame, spans);
            for (size_t k = w; k <= last; ++k) {
                WordPlan& plan = plans[k];
                if (fits) {
                    plan.word.startMS = spans[k - w].first;
                    plan.word.endMS = spans[k - w].second;
                } else {
                    plan.word.startMS = plan.word.endMS = 0;
                    tooShort = true;
                }
                plan.word.source = "syllables";
                plan.word.confidence = Round3(phrase.confidence * WORD_SHARE);
            }
            w = last + 1;
        }
        if (tooShort) {
            _a.warnings.push_back("phrase-too-short-for-its-words");
        }
    }

    void ShapeWords() {
        for (const AlignedWord& word : _a.words) {
            const AlignedPhrase* phrase = FindPhrase(word.phraseId);
            if (UseTrackPhonemes(word)) {
                continue;
            }
            const Pronunciation& p = _pronunciations[word.id];
            if (!p.Ok()) {
                _a.warnings.push_back("word-without-phonemes");
                continue;
            }
            const bool phraseFinal = std::none_of(_a.words.begin(), _a.words.end(), [&](const AlignedWord& other) { return other.phraseId == word.phraseId && other.startMS > word.startMS; });
            const std::vector<Segment> segments = Settle(ShapeWord(p, word.startMS, word.endMS, _opt, phraseFinal), word.startMS, word.endMS, _frame);
            const double confidence = Round3((phrase != nullptr ? phrase->confidence : 0) * PHONEME_SHARE);
            for (size_t k = 0; k < segments.size(); ++k) {
                AlignedPhoneme phoneme;
                phoneme.id = fmt::format("{}/{}", word.id, k);
                phoneme.wordId = word.id;
                phoneme.shape = segments[k].shape;
                phoneme.phoneme = segments[k].phoneme;
                phoneme.startMS = static_cast<int>(segments[k].start);
                phoneme.endMS = static_cast<int>(segments[k].end);
                phoneme.confidence = word.state != Provenance::Detected ? Round3(MANUAL_CONFIDENCE * PHONEME_SHARE) : confidence;
                _a.phonemes.push_back(std::move(phoneme));
            }
        }
        // A word whose own times a person set has its phonemes laid out again, and that
        // is a correction that was honoured, whatever the provider.
        for (const LyricCorrection& c : _a.corrections) {
            if (c.level == LyricCorrection::Level::Word && !_appliedWords.contains(c.targetId)) {
                _a.conflicts.push_back({ c.targetId, "target-missing" });
            }
        }
    }

    bool UseTrackPhonemes(const AlignedWord& word) {
        if (word.source != "track" || _in.existingTrack == nullptr || _in.existingTrack->layers.size() < 3) {
            return false;
        }
        int k = 0;
        for (const TimingMark& mark : _in.existingTrack->layers[2]) {
            if (mark.startMS >= word.startMS && mark.startMS < word.endMS) {
                AlignedPhoneme phoneme;
                phoneme.id = fmt::format("{}/{}", word.id, k++);
                phoneme.wordId = word.id;
                phoneme.shape = mark.label;
                phoneme.startMS = mark.startMS;
                phoneme.endMS = mark.endMS;
                phoneme.confidence = TRACK_CONFIDENCE;
                _a.phonemes.push_back(std::move(phoneme));
            }
        }
        return k > 0;
    }

    void ApplyPhonemeCorrections() {
        std::set<std::string> applied;
        for (auto it = _a.corrections.rbegin(); it != _a.corrections.rend(); ++it) {
            const LyricCorrection& c = *it;
            if (c.level != LyricCorrection::Level::Phoneme || !applied.insert(c.targetId).second) {
                continue;
            }
            auto found = std::find_if(_a.phonemes.begin(), _a.phonemes.end(), [&](const AlignedPhoneme& p) { return p.id == c.targetId; });
            if (found == _a.phonemes.end()) {
                _a.conflicts.push_back({ c.targetId, "target-missing" });
                continue;
            }
            if (!c.shape.empty() && !SHAPES.contains(c.shape)) {
                _a.conflicts.push_back({ c.targetId, "shape-invalid" });
                continue;
            }
            if (!c.shape.empty()) {
                found->shape = c.shape;
            }
            if (c.startMS >= 0 && c.endMS > c.startMS) {
                found->startMS = ToFrame(c.startMS, _frame);
                found->endMS = std::max(found->startMS + _frame, ToFrame(c.endMS, _frame));
                // Neighbours in the same word give way to the corrected phoneme.
                for (AlignedPhoneme& other : _a.phonemes) {
                    if (&other == &*found || other.wordId != found->wordId) {
                        continue;
                    }
                    if (other.startMS < found->startMS && other.endMS > found->startMS) {
                        other.endMS = found->startMS;
                    } else if (other.startMS >= found->startMS && other.startMS < found->endMS) {
                        other.startMS = std::min(found->endMS, other.endMS);
                    }
                }
                _a.phonemes.erase(std::remove_if(_a.phonemes.begin(), _a.phonemes.end(), [](const AlignedPhoneme& p) { return p.endMS <= p.startMS; }), _a.phonemes.end());
                found = std::find_if(_a.phonemes.begin(), _a.phonemes.end(), [&](const AlignedPhoneme& p) { return p.id == c.targetId; });
            }
            found->state = c.state;
            found->confidence = Round3(MANUAL_CONFIDENCE);
        }
    }

    // Pauses close the mouth explicitly, between words and between phrases.
    void AddRests() {
        std::sort(_a.phonemes.begin(), _a.phonemes.end(), [](const AlignedPhoneme& a, const AlignedPhoneme& b) { return a.startMS < b.startMS; });
        std::vector<AlignedPhoneme> rests;
        for (size_t i = 0; i + 1 < _a.phonemes.size(); ++i) {
            const int gapStart = _a.phonemes[i].endMS;
            const int gapEnd = _a.phonemes[i + 1].startMS;
            if (gapEnd - gapStart >= _frame) {
                AlignedPhoneme rest;
                rest.id = fmt::format("rest:{}", gapStart);
                rest.shape = "rest";
                rest.startMS = gapStart;
                rest.endMS = gapEnd;
                rest.confidence = 1;
                rests.push_back(std::move(rest));
            }
        }
        _a.phonemes.insert(_a.phonemes.end(), rests.begin(), rests.end());
        std::sort(_a.phonemes.begin(), _a.phonemes.end(), [](const AlignedPhoneme& a, const AlignedPhoneme& b) { return a.startMS < b.startMS; });
    }

    void Summarize() {
        std::sort(_a.warnings.begin(), _a.warnings.end());
        _a.warnings.erase(std::unique(_a.warnings.begin(), _a.warnings.end()), _a.warnings.end());
        int timed = 0;
        int untimed = 0;
        std::set<std::string> sources;
        for (const AlignedPhrase& phrase : _a.phrases) {
            if (phrase.instrumental) {
                continue;
            }
            if (phrase.startMS < 0) {
                ++untimed;
            } else {
                ++timed;
                sources.insert(phrase.source);
            }
        }
        if (timed == 0) {
            _a.quality = "unsynchronized";
            _a.warnings.push_back("lyrics-not-synchronized");
        } else if (sources.contains("experimental")) {
            _a.quality = "experimental";
        } else if (sources.contains("lrc") || sources.contains("srt")) {
            _a.quality = "approximate";
        } else if (sources.contains("track")) {
            _a.quality = "synced";
        } else {
            _a.quality = "manual";
        }
        if (timed > 0 && untimed > 0) {
            _a.warnings.push_back("some-phrases-not-synchronized");
        }
    }

    const AlignerInput& _in;
    const AlignerOptions& _opt;
    LyricAlignment& _a;
    int _frame;
    std::vector<const LyricLine*> _lines;
    std::map<std::string, Pronunciation> _pronunciations;
    std::set<std::string> _appliedWords;
};

Provenance StateFromId(const std::string& id) {
    for (int i = 0; i < 4; ++i) {
        if (id == STATE_IDS[i]) {
            return static_cast<Provenance>(i);
        }
    }
    return Provenance::Detected;
}

json CorrectionToJson(const LyricCorrection& c) {
    return { { "level", LEVEL_IDS[static_cast<int>(c.level)] }, { "targetId", c.targetId }, { "state", STATE_IDS[static_cast<int>(c.state)] }, { "startMS", c.startMS }, { "endMS", c.endMS }, { "shape", c.shape }, { "originalStartMS", c.originalStartMS }, { "originalEndMS", c.originalEndMS } };
}

} // namespace

LyricAlignment AlignLyrics(const AlignerInput& input, const AlignerOptions& options) {
    LyricAlignment alignment;
    Aligner aligner(input, options, alignment);
    aligner.Run();
    return alignment;
}

TimingTrackSpec BuildLyricTrack(const LyricAlignment& alignment, const std::string& trackName) {
    TimingTrackSpec track;
    track.name = trackName;
    track.layers.resize(3);
    for (const AlignedPhrase& phrase : alignment.phrases) {
        if (phrase.startMS >= 0 && !phrase.instrumental) {
            // xLights strips these from phrase labels when it imports lyrics itself;
            // the text kept in the document is unchanged.
            std::string label = phrase.text;
            label.erase(std::remove_if(label.begin(), label.end(), [](char c) { return c == '"' || c == '<' || c == '>'; }), label.end());
            track.layers[0].push_back({ label, phrase.startMS, phrase.endMS });
        }
    }
    for (const AlignedWord& word : alignment.words) {
        std::string label = word.text;
        label.erase(std::remove_if(label.begin(), label.end(), [](char c) { return c == '"' || c == '<' || c == '>'; }), label.end());
        track.layers[1].push_back({ label, word.startMS, word.endMS });
    }
    for (const AlignedPhoneme& phoneme : alignment.phonemes) {
        track.layers[2].push_back({ phoneme.shape, phoneme.startMS, phoneme.endMS });
    }
    for (auto& layer : track.layers) {
        std::stable_sort(layer.begin(), layer.end(), [](const TimingMark& a, const TimingMark& b) { return a.startMS < b.startMS; });
    }
    return track;
}

bool WriteTimingTracks(const std::string& path, const std::vector<TimingTrackSpec>& tracks, std::string& error) {
    pugi::xml_document doc;
    pugi::xml_node declaration = doc.append_child(pugi::node_declaration);
    declaration.append_attribute("version") = "1.0";
    declaration.append_attribute("encoding") = "UTF-8";
    pugi::xml_node root = doc.append_child("timings");
    root.append_attribute("SourceVersion") = xlights_version_string.c_str();
    for (const TimingTrackSpec& track : tracks) {
        pugi::xml_node timing = root.append_child("timing");
        timing.append_attribute("name") = track.name.c_str();
        timing.append_attribute("subType") = "";
        timing.append_attribute("SourceVersion") = xlights_version_string.c_str();
        for (const auto& layer : track.layers) {
            pugi::xml_node node = timing.append_child("EffectLayer");
            for (const TimingMark& mark : layer) {
                pugi::xml_node effect = node.append_child("Effect");
                effect.append_attribute("label") = mark.label.c_str();
                effect.append_attribute("starttime") = mark.startMS;
                effect.append_attribute("endtime") = mark.endMS;
            }
        }
    }
    ObtainAccessToURL(path, true);
    const std::string temporary = path + ".composer-tmp";
    if (!doc.save_file(temporary.c_str(), "  ")) {
        error = fmt::format("cannot write '{}'", temporary);
        return false;
    }
    std::error_code ec;
    fs::rename(fs::path(temporary), fs::path(path), ec);
    if (ec) {
        error = fmt::format("cannot replace '{}': {}", path, ec.message());
        std::error_code ignored;
        fs::remove(fs::path(temporary), ignored);
        return false;
    }
    return true;
}

const char* ProviderId(AlignmentProvider provider) {
    return PROVIDER_IDS[static_cast<int>(provider)];
}

const char* CorrectionLevelId(LyricCorrection::Level level) {
    return LEVEL_IDS[static_cast<int>(level)];
}

std::string ToJson(const LyricAlignment& a) {
    json root;
    root["schemaVersion"] = a.schemaVersion;
    root["provider"] = ProviderId(a.provider);
    root["quality"] = a.quality;
    root["musicQuality"] = a.musicQuality;
    root["language"] = a.language;
    root["warnings"] = a.warnings;
    json phrases = json::array();
    for (const AlignedPhrase& p : a.phrases) {
        phrases.push_back({ { "id", p.id }, { "text", p.text }, { "voice", p.voice }, { "startMS", p.startMS }, { "endMS", p.endMS }, { "instrumental", p.instrumental }, { "confidence", p.confidence }, { "source", p.source }, { "state", STATE_IDS[static_cast<int>(p.state)] }, { "originalStartMS", p.originalStartMS }, { "originalEndMS", p.originalEndMS } });
    }
    root["phrases"] = std::move(phrases);
    json words = json::array();
    for (const AlignedWord& w : a.words) {
        words.push_back({ { "id", w.id }, { "text", w.text }, { "startMS", w.startMS }, { "endMS", w.endMS }, { "confidence", w.confidence }, { "source", w.source }, { "state", STATE_IDS[static_cast<int>(w.state)] }, { "pronunciation", w.pronunciation }, { "phonemes", w.phonemes }, { "originalStartMS", w.originalStartMS }, { "originalEndMS", w.originalEndMS } });
    }
    root["words"] = std::move(words);
    json phonemes = json::array();
    for (const AlignedPhoneme& p : a.phonemes) {
        phonemes.push_back({ { "id", p.id }, { "shape", p.shape }, { "phoneme", p.phoneme }, { "startMS", p.startMS }, { "endMS", p.endMS }, { "confidence", p.confidence }, { "state", STATE_IDS[static_cast<int>(p.state)] } });
    }
    root["phonemes"] = std::move(phonemes);
    json corrections = json::array();
    for (const LyricCorrection& c : a.corrections) {
        corrections.push_back(CorrectionToJson(c));
    }
    root["corrections"] = std::move(corrections);
    json conflicts = json::array();
    for (const AlignmentConflict& c : a.conflicts) {
        conflicts.push_back({ { "targetId", c.targetId }, { "reason", c.reason } });
    }
    root["conflicts"] = std::move(conflicts);
    return root.dump(1);
}

bool FromJson(const std::string& text, LyricAlignment& alignment, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    const auto parentOf = [](const std::string& id) {
        const size_t slash = id.rfind('/');
        return slash == std::string::npos ? std::string() : id.substr(0, slash);
    };
    try {
        LyricAlignment a;
        a.schemaVersion = root.at("schemaVersion").get<int>();
        if (a.schemaVersion > LYRIC_ALIGNMENT_SCHEMA_VERSION) {
            error = "written by a newer version";
            return false;
        }
        const std::string provider = root.value("provider", std::string("none"));
        for (int i = 0; i < static_cast<int>(std::size(PROVIDER_IDS)); ++i) {
            if (provider == PROVIDER_IDS[i]) {
                a.provider = static_cast<AlignmentProvider>(i);
            }
        }
        a.quality = root.value("quality", std::string("none"));
        a.musicQuality = root.value("musicQuality", std::string(MUSIC_QUALITY_NOT_VALIDATED));
        a.language = root.value("language", std::string());
        a.warnings = root.value("warnings", std::vector<std::string>());
        for (const json& p : root.value("phrases", json::array())) {
            AlignedPhrase phrase;
            phrase.id = p.at("id").get<std::string>();
            phrase.text = p.value("text", std::string());
            phrase.voice = p.value("voice", std::string());
            phrase.startMS = p.value("startMS", -1);
            phrase.endMS = p.value("endMS", -1);
            phrase.instrumental = p.value("instrumental", false);
            phrase.confidence = p.value("confidence", 0.0);
            phrase.source = p.value("source", std::string());
            phrase.state = StateFromId(p.value("state", std::string("detected")));
            phrase.originalStartMS = p.value("originalStartMS", -1);
            phrase.originalEndMS = p.value("originalEndMS", -1);
            a.phrases.push_back(std::move(phrase));
        }
        for (const json& w : root.value("words", json::array())) {
            AlignedWord word;
            word.id = w.at("id").get<std::string>();
            word.phraseId = parentOf(word.id);
            word.text = w.value("text", std::string());
            const size_t colon = word.id.rfind(':');
            word.key = colon == std::string::npos ? std::string() : word.id.substr(colon + 1);
            word.startMS = w.value("startMS", 0);
            word.endMS = w.value("endMS", 0);
            word.confidence = w.value("confidence", 0.0);
            word.source = w.value("source", std::string());
            word.state = StateFromId(w.value("state", std::string("detected")));
            word.pronunciation = w.value("pronunciation", std::string());
            word.phonemes = w.value("phonemes", std::string());
            word.originalStartMS = w.value("originalStartMS", -1);
            word.originalEndMS = w.value("originalEndMS", -1);
            a.words.push_back(std::move(word));
        }
        for (const json& p : root.value("phonemes", json::array())) {
            AlignedPhoneme phoneme;
            phoneme.id = p.at("id").get<std::string>();
            phoneme.wordId = phoneme.id.rfind("rest:", 0) == 0 ? std::string() : parentOf(phoneme.id);
            phoneme.shape = p.value("shape", std::string());
            phoneme.phoneme = p.value("phoneme", std::string());
            phoneme.startMS = p.value("startMS", 0);
            phoneme.endMS = p.value("endMS", 0);
            phoneme.confidence = p.value("confidence", 0.0);
            phoneme.state = StateFromId(p.value("state", std::string("detected")));
            a.phonemes.push_back(std::move(phoneme));
        }
        LyricSession corrections;
        if (!FromJson(json{ { "schemaVersion", LYRIC_ALIGNMENT_SCHEMA_VERSION }, { "corrections", root.value("corrections", json::array()) } }.dump(), corrections, error)) {
            return false;
        }
        a.corrections = std::move(corrections.corrections);
        for (const json& c : root.value("conflicts", json::array())) {
            a.conflicts.push_back({ c.value("targetId", std::string()), c.value("reason", std::string()) });
        }
        alignment = std::move(a);
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::string ToJson(const LyricSession& session) {
    json root;
    root["schemaVersion"] = session.schemaVersion;
    root["trackName"] = session.trackName;
    root["lyrics"] = session.lyrics;
    json corrections = json::array();
    for (const LyricCorrection& c : session.corrections) {
        corrections.push_back(CorrectionToJson(c));
    }
    root["corrections"] = std::move(corrections);
    return root.dump(1);
}

bool FromJson(const std::string& text, LyricSession& session, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    try {
        LyricSession out;
        out.schemaVersion = root.at("schemaVersion").get<int>();
        if (out.schemaVersion > LYRIC_ALIGNMENT_SCHEMA_VERSION) {
            error = "written by a newer version";
            return false;
        }
        out.trackName = root.value("trackName", std::string());
        out.lyrics = root.value("lyrics", std::string());
        for (const json& c : root.value("corrections", json::array())) {
            LyricCorrection correction;
            const std::string level = c.at("level").get<std::string>();
            correction.level = level == "word" ? LyricCorrection::Level::Word : level == "phoneme" ? LyricCorrection::Level::Phoneme : LyricCorrection::Level::Phrase;
            correction.targetId = c.at("targetId").get<std::string>();
            correction.state = StateFromId(c.value("state", std::string("confirmed")));
            correction.startMS = c.value("startMS", -1);
            correction.endMS = c.value("endMS", -1);
            correction.shape = c.value("shape", std::string());
            correction.originalStartMS = c.value("originalStartMS", -1);
            correction.originalEndMS = c.value("originalEndMS", -1);
            if (correction.state != Provenance::Detected) {
                out.corrections.push_back(std::move(correction));
            }
        }
        session = std::move(out);
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

bool SaveLyricSession(const std::string& path, const LyricSession& session, std::string& error) {
    ObtainAccessToURL(path, true);
    const std::string temporary = path + ".composer-tmp";
    {
        std::ofstream out(fs::path(temporary), std::ios::binary | std::ios::trunc);
        out << ToJson(session);
        if (!out) {
            error = fmt::format("cannot write '{}'", temporary);
            return false;
        }
    }
    std::error_code ec;
    fs::rename(fs::path(temporary), fs::path(path), ec);
    if (ec) {
        error = fmt::format("cannot replace '{}': {}", path, ec.message());
        std::error_code ignored;
        fs::remove(fs::path(temporary), ignored);
        return false;
    }
    return true;
}

bool LoadLyricSession(const std::string& path, LyricSession& session, std::string& error) {
    ObtainAccessToURL(path, false);
    std::ifstream in(fs::path(path), std::ios::binary);
    if (!in) {
        error = fmt::format("cannot read '{}'", path);
        return false;
    }
    std::ostringstream content;
    content << in.rdbuf();
    return FromJson(content.str(), session, error);
}

void UpsertCorrection(std::vector<LyricCorrection>& corrections, const LyricCorrection& correction) {
    for (LyricCorrection& existing : corrections) {
        if (existing.level == correction.level && existing.targetId == correction.targetId) {
            const int originalStart = existing.originalStartMS;
            const int originalEnd = existing.originalEndMS;
            existing = correction;
            if (originalStart >= 0 || originalEnd >= 0) {
                existing.originalStartMS = originalStart;
                existing.originalEndMS = originalEnd;
            }
            return;
        }
    }
    corrections.push_back(correction);
}

} // namespace composer
