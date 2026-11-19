#include "composer/CompositionSelector.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#define COMPOSER_TEXT(text) text

namespace composer {

namespace {

// What kind of part a scene is, in the words the compositions' contexts use.
std::set<std::string> Tags(const SelectorScene& s, std::string& fromContext, std::string& fromSection, std::string& fromNarrative) {
    std::set<std::string> tags;
    fromContext = s.context == "silence" ? "pause" : s.context == "duet" ? "solo" : s.context;
    const std::string& k = s.sectionKind;
    fromSection = k == "intro" || k == "verse" || k == "chorus" ? k
                : k == "outro"                                  ? "ending"
                : k == "bridge" || k == "pre-chorus"            ? "transition"
                : k == "interlude" || k == "solo" || k == "breakdown" ? "instrumental"
                                                                       : std::string();
    const std::string& n = s.narrative;
    fromNarrative = n == "presentation"                         ? "intro"
                  : n == "preparation"                          ? "transition"
                  : n == "climax" || n == "intensification"     ? "climax"
                  : n == "resolution"                           ? "ending"
                  : n == "rest"                                 ? "pause"
                                                                : std::string();
    for (const std::string* t : { &fromContext, &fromSection, &fromNarrative }) {
        if (!t->empty()) {
            tags.insert(*t);
        }
    }
    if (s.accentRun || s.strongHits >= 2) {
        tags.insert("accent");
    }
    return tags;
}

// The level of attention the scene asks for: one figure, a group or the whole house.
std::string WantedLevel(const SelectorScene& s) {
    if (s.modeByPerson) {
        switch (s.mode) {
        case StageMode::Protagonist:
        case StageMode::Duo: return "local";
        case StageMode::Propagation:
        case StageMode::FullEnsemble: return "global";
        default: return "group";
        }
    }
    const std::string& n = s.narrative;
    if (s.context == "narration" || n == "presentation") {
        return "local";
    }
    if (n == "climax" || n == "intensification" || n == "transformation" || n == "preparation" || n == "resolution" || s.context == "climax") {
        return "global";
    }
    return "group";
}

int LevelIndex(const std::string& level) {
    return level == "local" ? 0 : level == "global" ? 2 : 1;
}

bool HitDriven(GestureKind k) {
    return k != GestureKind::SpatialSweep && k != GestureKind::Expansion && k != GestureKind::Contraction && k != GestureKind::Echo;
}

} // namespace

int CompositionNeedsMS(const StageLibraryEntry& e, double beat, int figures) {
    const int b = static_cast<int>(beat * 1000);
    if (e.beats > 0) {
        return e.beats * b;
    }
    GestureKind k = GestureKind::JointPulse;
    (void)GestureKindFromId(e.gesture, k);
    switch (k) {
    case GestureKind::PropagatedWave:
    case GestureKind::Expansion:
    case GestureKind::Contraction: return 2 * b + 600;
    case GestureKind::Echo: return 4 * b;
    case GestureKind::CallResponse: return (std::min(figures, 5) + 1) * b + 600;
    case GestureKind::Cascade: return 2000;
    case GestureKind::SpatialSweep: return 4 * b + 800;
    case GestureKind::Counterpoint: return 4 * b;
    case GestureKind::AmbientField: return 4 * b;
    case GestureKind::LocalWaves:
    case GestureKind::JointPulse: return 2 * b;
    }
    return 2 * b;
}

CompositionChoice SelectComposition(const StageLibrary& library, const SelectorScene& s) {
    CompositionChoice out;
    std::string fromContext, fromSection, fromNarrative;
    const std::set<std::string> tags = Tags(s, fromContext, fromSection, fromNarrative);
    const std::string wanted = WantedLevel(s);
    const CompositionCandidate* best = nullptr;
    const StageLibraryEntry* bestEntry = nullptr;
    std::vector<const StageLibraryEntry*> entries;
    for (const StageLibraryEntry& e : library.entries) {
        if (e.kind == "composition") {
            entries.push_back(&e);
        }
    }
    for (const StageLibraryEntry* ep : entries) {
        const StageLibraryEntry& e = *ep;
        CompositionCandidate c;
        c.id = e.id;
        std::vector<std::string> out_; // ruling it out
        const auto plus = [&](double v, const char* why) {
            c.score += v;
            c.reasons.push_back(std::string("+") + why);
        };
        const auto minus = [&](double v, const char* why) {
            c.score -= v;
            c.reasons.push_back(std::string("-") + why);
        };
        const auto rule = [&](const char* why) { out_.push_back(std::string("x") + why); };
        GestureKind kind = GestureKind::JointPulse;
        const bool known = GestureKindFromId(e.gesture, kind);
        // How many figures it would use here: what the scene asks, within what it allows.
        const int most = e.maxFigures > 0 ? std::min(e.maxFigures, s.available) : s.available;
        int figures = s.target > 0 ? s.target : e.level == "local" ? 1 : e.level == "global" ? s.available : (e.minFigures + std::max(e.minFigures, most)) / 2;
        if (known && kind == GestureKind::CallResponse) {
            figures = std::max(figures, s.faces + 3); // the voices that call and a few to answer them
        } else if (known && e.temporal == "answer") {
            figures = std::max(figures, 3); // one starts, the others answer
        }
        figures = std::clamp(figures, std::min(e.minFigures, most), std::max(1, most));
        c.figures = figures;
        // What rules it out.
        if (e.excluded) {
            rule(COMPOSER_TEXT("a person excluded it"));
        }
        if (!known) {
            rule(COMPOSER_TEXT("its gesture is not one the director can make"));
        }
        if (s.context == "silence" || s.mode == StageMode::Rest) {
            rule(COMPOSER_TEXT("the music rests here"));
        }
        if (s.available < e.minFigures) {
            rule(COMPOSER_TEXT("not enough figures can take part"));
        }
        if (!e.fits.empty()) {
            bool any = false;
            for (const std::string& t : e.fits) {
                any = any || (s.topologies.contains(t) && s.topologies.at(t) > 0);
            }
            if (!any) {
                rule(COMPOSER_TEXT("no figure of the shape it needs"));
            }
        }
        if (s.durationMS - 300 < CompositionNeedsMS(e, s.beat, figures)) {
            rule(COMPOSER_TEXT("the scene is too short for it"));
        }
        if (known) {
            if (HitDriven(kind) && s.strongHits == 0 && !(kind == GestureKind::Cascade && s.accentRun)) {
                rule(COMPOSER_TEXT("no hit strong enough to start it"));
            }
            if (kind == GestureKind::Cascade && !s.accentRun) {
                rule(COMPOSER_TEXT("it needs a few hits close together"));
            }
            if (kind == GestureKind::CallResponse && s.faces == 0 && s.context != "choir") {
                rule(COMPOSER_TEXT("no voice calls here"));
            }
            if (kind == GestureKind::SpatialSweep && !s.beforePeak && s.narrative != "preparation") {
                rule(COMPOSER_TEXT("nothing grows towards a peak here"));
            }
            if (kind == GestureKind::Expansion && !s.peakBegins) {
                rule(COMPOSER_TEXT("no peak opens here"));
            }
            if (kind == GestureKind::Contraction && s.narrative != "resolution" && !s.lastScene && !s.drop) {
                rule(COMPOSER_TEXT("nothing closes or drops here"));
            }
            if (kind == GestureKind::Echo && !s.tail) {
                rule(COMPOSER_TEXT("no sound dies away here"));
            }
            if (kind == GestureKind::AmbientField && s.available < 2) {
                rule(COMPOSER_TEXT("no figure to keep the protagonist company"));
            }
        }
        if (s.modeByPerson && LevelIndex(e.level) != LevelIndex(wanted)) {
            rule(COMPOSER_TEXT("a person chose another way to use the house"));
        }
        // What counts for it.
        const auto has = [&](const std::string& t) {
            if (t.empty()) {
                return false;
            }
            if (std::find(e.contexts.begin(), e.contexts.end(), t) != e.contexts.end()) {
                return true;
            }
            return s.calibration >= 1 && e.source == "built-in" && e.id == "composition:neighbours-answer" && t == "solo";
        };
        if (has(fromContext)) {
            plus(3, COMPOSER_TEXT("it suits what the voice or the instruments do here"));
        }
        if (has(fromSection)) {
            plus(2, COMPOSER_TEXT("it suits this kind of part of the song"));
        }
        if (has(fromNarrative)) {
            plus(2, COMPOSER_TEXT("it suits the scene's role in the song"));
        }
        if (tags.contains("accent") && has("accent")) {
            plus(1, COMPOSER_TEXT("the scene has hits to mark"));
        }
        const int distance = std::abs(LevelIndex(e.level) - LevelIndex(wanted));
        if (distance == 0) {
            plus(3, COMPOSER_TEXT("it uses as much of the house as the scene asks"));
        } else if (distance == 2) {
            minus(3, COMPOSER_TEXT("it uses far more or far less of the house than the scene asks"));
        }
        if (s.priority >= 3 && e.level == "global") {
            plus(1, COMPOSER_TEXT("a strong part: the whole house can carry it"));
        } else if (s.priority <= 1 && e.level == "local") {
            plus(1, COMPOSER_TEXT("a quiet part: one figure is enough"));
        }
        // The idea's own composition, and what the scene does with the idea: brought back as it
        // was, grown to more figures, made stronger, gathered in.
        const bool continues = !s.motifComposition.empty() && s.motifOperation != "present";
        const std::string& op = s.motifOperation;
        const StageLibraryEntry* born = continues ? library.Find(s.motifComposition) : nullptr;
        const bool same = continues && s.motifComposition == e.id;
        const bool variant = continues && ((!e.variantOf.empty() && e.variantOf == s.motifComposition) || (born != nullptr && !born->variantOf.empty() && born->variantOf == e.id));
        if (op == "recall" || op == "hold" || op == "split") {
            if (same) {
                plus(4, COMPOSER_TEXT("it is the composition the scene's idea was born with"));
            } else if (variant) {
                plus(3, COMPOSER_TEXT("it varies the composition the scene's idea was born with"));
            }
        } else if (op == "develop" && born != nullptr) {
            if (LevelIndex(e.level) == LevelIndex(born->level) + 1 || (born->level == "global" && e.level == "global" && !same)) {
                plus(3, COMPOSER_TEXT("the idea grows: more figures take it up"));
            } else if (same || variant) {
                plus(1, COMPOSER_TEXT("it keeps the idea's own composition"));
            }
        } else if (op == "intensify" && e.level == "global") {
            plus(3, COMPOSER_TEXT("the idea comes back stronger: the whole house carries it"));
        } else if ((op == "reduce" || op == "resolve") && born != nullptr && LevelIndex(e.level) < LevelIndex(born->level)) {
            plus(2, COMPOSER_TEXT("the idea gathers onto fewer figures"));
        } else if ((op == "reduce" || op == "resolve") && same) {
            plus(1, COMPOSER_TEXT("it keeps the idea's own composition"));
        }
        if (!s.motifGesture.empty() && s.motifGesture == e.gesture) {
            plus(2, COMPOSER_TEXT("it moves the way the scene's idea moves here"));
        }
        if (!s.previous.empty() && s.previous == e.id && !same) {
            minus(2, COMPOSER_TEXT("the scene before used it: the song needs variety"));
        } else if (!s.previous.empty() && s.previous == e.id && s.previousRun >= 2) {
            minus(3, COMPOSER_TEXT("the same composition several scenes running: the idea needs another shape"));
        }
        if (s.used.contains(e.id) && s.used.at(e.id) >= 3 && s.motifComposition != e.id) {
            minus(1, COMPOSER_TEXT("already used many times in the song"));
        }
        if (e.favorite) {
            plus(2, COMPOSER_TEXT("a person's favourite"));
        }
        if (!e.fits.empty() && out_.empty()) {
            plus(2, COMPOSER_TEXT("a figure here has the shape it is made for"));
        }
        if (e.temporal == "answer" && (s.context == "choir" || s.voices >= 2)) {
            plus(1, COMPOSER_TEXT("voices answer each other here"));
        }
        if (known && kind == GestureKind::Cascade && s.accentRun) {
            plus(3, COMPOSER_TEXT("a few hits close together, one for each figure"));
        }
        // The song's own turning points ask for their composition.
        if (known && kind == GestureKind::Expansion && s.peakBegins) {
            plus(3, COMPOSER_TEXT("the peak begins here"));
        } else if (known && kind == GestureKind::SpatialSweep && s.narrative == "preparation") {
            plus(3, COMPOSER_TEXT("the peak comes next: the light grows towards it"));
        } else if (known && kind == GestureKind::Contraction && (s.lastScene || s.narrative == "resolution")) {
            plus(3, COMPOSER_TEXT("the song closes here"));
        }
        if (e.custom && !e.figures.empty()) {
            plus(1, COMPOSER_TEXT("a person's own composition"));
        }
        // The plan for the whole song, when there is one: its choice comes before the scene's own
        // best (a person's choice still comes first).
        if (!s.planned.empty() && s.planned == e.id && out_.empty()) {
            c.score += 50;
            c.reasons.insert(c.reasons.begin(), std::string("+") + COMPOSER_TEXT("chosen in the plan for the whole song"));
        }
        // A person's choice for this scene wins, unless it cannot be done here.
        if (!s.personChoice.empty() && s.personChoice == e.id) {
            if (out_.empty()) {
                c.score += 100;
                c.reasons.insert(c.reasons.begin(), std::string("+") + COMPOSER_TEXT("chosen by a person for this scene"));
            } else {
                out.fallback = COMPOSER_TEXT("the composition a person chose cannot be done here");
            }
        }
        if (!out_.empty()) {
            c.verdict = "rejected";
            c.reasons.insert(c.reasons.begin(), out_.begin(), out_.end());
        } else {
            c.verdict = "possible";
        }
        out.candidates.push_back(std::move(c));
    }
    // The highest score among the possible ones; a tie goes to the one closest to how much of
    // the house the scene asks for, then to the library's order.
    for (size_t i = 0; i < out.candidates.size(); ++i) {
        const CompositionCandidate& c = out.candidates[i];
        if (c.verdict != "possible") {
            continue;
        }
        const bool higher = best == nullptr || c.score > best->score + 1e-9;
        const bool tie = best != nullptr && std::abs(c.score - best->score) <= 1e-9;
        if (higher || (tie && std::abs(LevelIndex(entries[i]->level) - LevelIndex(wanted)) < std::abs(LevelIndex(bestEntry->level) - LevelIndex(wanted)))) {
            best = &c;
            bestEntry = entries[i];
        }
    }
    if (s.personChoice == "none") {
        out.fallback = COMPOSER_TEXT("a person asked for the director's own way here");
        return out;
    }
    if (best == nullptr) {
        if (out.fallback.empty()) {
            out.fallback = s.context == "silence" || s.mode == StageMode::Rest ? COMPOSER_TEXT("the music rests here") : COMPOSER_TEXT("no composition can be done here");
        }
        return out;
    }
    constexpr double enough = 4;
    if (best->score < enough) {
        if (out.fallback.empty()) {
            out.fallback = COMPOSER_TEXT("no composition fits the scene well enough");
        }
        return out;
    }
    const size_t at = static_cast<size_t>(best - out.candidates.data());
    out.candidates[at].verdict = "chosen";
    out.entry = bestEntry;
    out.figures = out.candidates[at].figures;
    for (const std::string& r : out.candidates[at].reasons) {
        if (!r.empty() && r[0] == '+') {
            out.reasons.push_back(r.substr(1));
        }
    }
    if (s.target > 0 && out.figures != s.target) {
        out.adapted = COMPOSER_TEXT("fitted to the number of figures it allows");
    } else if (bestEntry->maxFigures > 0 && s.available < bestEntry->maxFigures && out.figures == s.available) {
        out.adapted = COMPOSER_TEXT("fitted to the figures this house has");
    }
    if (!s.planned.empty() && bestEntry->id == s.planned) {
        for (const std::string& r : s.plannedReasons) {
            out.reasons.push_back(r);
        }
    }
    if (!out.fallback.empty() && !s.personChoice.empty()) {
        out.reasons.push_back(out.fallback); // the person's choice could not be done: said with the one used
        out.fallback.clear();
    }
    return out;
}


namespace {

// A composition's score in one scene on its own: what the scenes around it chose left out.
struct LocalScores {
    std::map<std::string, double> feasible; // id -> score, only those good enough
    std::string best;
    double bestScore = 0;
    bool personNone = false;
};

LocalScores ScoreAlone(const StageLibrary& library, const SelectorScene& facts, const std::string& motifComposition) {
    SelectorScene f = facts;
    f.previous.clear();
    f.previousRun = 0;
    f.used.clear();
    f.planned.clear();
    f.plannedReasons.clear();
    f.motifComposition = motifComposition;
    const CompositionChoice c = SelectComposition(library, f);
    LocalScores out;
    out.personNone = facts.personChoice == "none";
    constexpr double enough = 4;
    for (const CompositionCandidate& x : c.candidates) {
        if (x.verdict == "rejected" || x.score < enough) {
            continue;
        }
        out.feasible[x.id] = x.score;
        if (out.best.empty() || x.score > out.bestScore + 1e-9) {
            out.best = x.id;
            out.bestScore = x.score;
        }
    }
    return out;
}

struct Plan {
    std::vector<std::string> picks;
    std::vector<int> order; // each pick's place in the library (for ties)
    std::vector<double> local, song;
    std::vector<std::vector<std::string>> reasons;
    std::map<std::string, int> loose; // uses that bring no idea back
    double total = 0;
};

bool BringsBack(const std::string& op) {
    return op == "recall" || op == "hold" || op == "split";
}

} // namespace

CompositionPlanResult PlanCompositions(const StageLibrary& library, const std::vector<PlanScene>& scenes) {
    CompositionPlanResult result;
    std::map<std::string, int> place;
    std::map<std::string, std::string> gesture;
    int k = 0;
    for (const StageLibraryEntry& e : library.entries) {
        if (e.kind == "composition") {
            place[e.id] = k++;
            gesture[e.id] = e.gesture;
        }
    }
    std::map<std::pair<size_t, std::string>, LocalScores> cache;
    const auto alone = [&](size_t i, const std::string& motifComposition) -> const LocalScores& {
        const auto key = std::make_pair(i, motifComposition);
        auto it = cache.find(key);
        if (it == cache.end()) {
            it = cache.emplace(key, ScoreAlone(library, scenes[i].facts, motifComposition)).first;
        }
        return it->second;
    };
    constexpr size_t BEAM = 256;
    std::vector<Plan> beams(1);
    // What the song adds to or takes from choosing `c` after the plan `b` so far.
    const auto songTerms = [&](const Plan& b, size_t i, const std::string& c, std::vector<std::string>& why) {
        double adj = 0;
        if (c.empty()) {
            return adj;
        }
        const PlanScene& sc = scenes[i];
        const bool back = sc.origin >= 0 && static_cast<size_t>(sc.origin) < i && b.picks[static_cast<size_t>(sc.origin)] == c && BringsBack(sc.facts.motifOperation);
        if (back) {
            why.push_back(COMPOSER_TEXT("it brings the idea back with the composition it was born with"));
            // Brought back again right away with the same composition: continuity the first time,
            // repetition after; another shape keeps the idea alive.
            int run = 0;
            for (size_t j = i; j-- > 0 && b.picks[j] == c;) {
                ++run;
            }
            if (run >= 1) {
                adj -= 1.5 * run;
                why.push_back(COMPOSER_TEXT("the idea comes back again right away with the same composition: another shape keeps it alive"));
            }
            return adj;
        }
        const std::string last = i > 0 ? b.picks[i - 1] : std::string();
        if (!last.empty() && last == c) {
            adj -= 3;
            why.push_back(COMPOSER_TEXT("the scene before used it: the song needs variety"));
        } else if (!last.empty() && gesture[last] == gesture[c]) {
            adj -= 1;
            why.push_back(COMPOSER_TEXT("the scene before used a composition of the same structure"));
        }
        const auto n = b.loose.find(c);
        if (n != b.loose.end() && n->second > 0) {
            adj -= 1.5 * n->second;
            why.push_back(COMPOSER_TEXT("already used in the song without bringing an idea back"));
        }
        return adj;
    };
    for (size_t i = 0; i < scenes.size(); ++i) {
        std::vector<Plan> next;
        for (const Plan& b : beams) {
            const PlanScene& sc = scenes[i];
            const std::string born = sc.origin >= 0 && static_cast<size_t>(sc.origin) < i ? b.picks[static_cast<size_t>(sc.origin)] : sc.facts.motifComposition;
            const LocalScores& ls = alone(i, born);
            std::vector<std::string> options;
            if (!ls.personNone) {
                for (const auto& [id, score] : ls.feasible) {
                    options.push_back(id);
                }
                std::sort(options.begin(), options.end(), [&](const std::string& a, const std::string& c) { return place[a] < place[c]; });
            }
            if (options.empty()) {
                options.push_back(std::string());
            }
            for (const std::string& c : options) {
                Plan p = b;
                std::vector<std::string> why;
                const double local = c.empty() ? 0.0 : ls.feasible.at(c);
                const double adj = songTerms(b, i, c, why);
                const bool back = std::find(why.begin(), why.end(), std::string(COMPOSER_TEXT("it brings the idea back with the composition it was born with"))) != why.end();
                p.picks.push_back(c);
                p.order.push_back(c.empty() ? 999 : place[c]);
                p.local.push_back(local);
                p.song.push_back(adj);
                p.reasons.push_back(why);
                if (!c.empty() && !back) {
                    p.loose[c]++;
                }
                p.total += local + adj;
                next.push_back(std::move(p));
            }
        }
        result.explored += static_cast<int>(next.size());
        std::stable_sort(next.begin(), next.end(), [](const Plan& a, const Plan& b) { return a.total > b.total + 1e-9 || (std::abs(a.total - b.total) <= 1e-9 && a.order < b.order); });
        if (next.size() > BEAM) {
            next.resize(BEAM);
        }
        beams = std::move(next);
    }
    if (beams.empty()) {
        return result;
    }
    const Plan& best = beams.front();
    result.total = best.total;
    Plan prefix;
    for (size_t i = 0; i < scenes.size(); ++i) {
        const PlanScene& sc = scenes[i];
        const std::string born = sc.origin >= 0 && static_cast<size_t>(sc.origin) < i ? best.picks[static_cast<size_t>(sc.origin)] : sc.facts.motifComposition;
        const LocalScores& ls = alone(i, born);
        PlannedComposition pc;
        pc.scene = sc.scene;
        pc.id = best.picks[i];
        pc.local = best.local[i];
        pc.song = best.song[i];
        pc.reasons = best.reasons[i];
        pc.localBest = ls.best;
        pc.localBestScore = ls.bestScore;
        for (const auto& [id, score] : ls.feasible) {
            if (id == pc.id) {
                continue;
            }
            std::vector<std::string> why;
            pc.alternatives.push_back({ id, score + songTerms(prefix, i, id, why) });
        }
        std::stable_sort(pc.alternatives.begin(), pc.alternatives.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        const bool back = std::find(pc.reasons.begin(), pc.reasons.end(), std::string(COMPOSER_TEXT("it brings the idea back with the composition it was born with"))) != pc.reasons.end();
        pc.kind = pc.id.empty()                                        ? "none"
                : !sc.facts.personChoice.empty() && sc.facts.personChoice == pc.id ? "person"
                : back                                                 ? "recall"
                : ls.feasible.size() == 1                              ? "only"
                : pc.id == ls.best || pc.local >= ls.bestScore - 1e-9  ? "best" // a tie is no sacrifice
                                                                       : "song";
        if (pc.kind == "song") {
            pc.reasons.push_back(COMPOSER_TEXT("another composition scored more in this scene alone, but the song as a whole is better with this one"));
        } else if (pc.kind == "only") {
            pc.reasons.push_back(COMPOSER_TEXT("the only composition that suits this scene"));
        }
        prefix.picks.push_back(pc.id);
        if (!pc.id.empty() && !back) {
            prefix.loose[pc.id]++;
        }
        result.scenes.push_back(std::move(pc));
    }
    return result;
}

} // namespace composer
