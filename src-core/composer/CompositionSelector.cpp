#include "composer/CompositionSelector.h"

#include <algorithm>
#include <cmath>
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
        const auto has = [&](const std::string& t) { return !t.empty() && std::find(e.contexts.begin(), e.contexts.end(), t) != e.contexts.end(); };
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
    if (!out.fallback.empty() && !s.personChoice.empty()) {
        out.reasons.push_back(out.fallback); // the person's choice could not be done: said with the one used
        out.fallback.clear();
    }
    return out;
}

} // namespace composer
