#include "composer/MultimodalTimeline.h"

#include "composer/LyricAligner.h"
#include "composer/SongAnalysis.h"

#include <algorithm>
#include <map>
#include <set>

#define COMPOSER_TEXT(text) text

namespace composer {

namespace {

const std::set<std::string> ENERGY_EVENTS = { "sustain", "fade-in", "fade-out", "crescendo", "diminuendo" };
const std::set<std::string> MIRRORED_EVENTS = { "silence", "pause" }; // also activities: shown once, as those

std::string SectionPhrase(const std::string& kind) {
    static const std::map<std::string, const char*> P = {
        { "intro", COMPOSER_TEXT("Intro") }, { "verse", COMPOSER_TEXT("Verse") }, { "chorus", COMPOSER_TEXT("Chorus") }, { "bridge", COMPOSER_TEXT("Bridge") },
        { "instrumental", COMPOSER_TEXT("Instrumental") }, { "outro", COMPOSER_TEXT("Ending") }, { "pre-chorus", COMPOSER_TEXT("Before the chorus") },
        { "prechorus", COMPOSER_TEXT("Before the chorus") }, { "interlude", COMPOSER_TEXT("Interlude") }
    };
    const auto it = P.find(kind);
    return it != P.end() ? it->second : COMPOSER_TEXT("Part of the song");
}

std::string EmissionPhrase(const std::string& emission) {
    if (emission == "spoken") {
        return COMPOSER_TEXT("Speaks");
    }
    if (emission == "sung") {
        return COMPOSER_TEXT("Sings");
    }
    if (emission == "choral") {
        return COMPOSER_TEXT("The choir sings");
    }
    return COMPOSER_TEXT("A voice (not sure how)");
}

// Which voice lane a track belongs to.
std::string VoiceLane(const VoiceTrack* t) {
    if (t == nullptr) {
        return "secondary";
    }
    if (t->kind == "narrator") {
        return "narration";
    }
    if (t->kind == "chorus") {
        return "chorus";
    }
    if (t->kind == "lead") {
        return "lead";
    }
    return "secondary";
}

TimelineLane* LaneOf(MultimodalTimeline& t, const std::string& id) {
    for (TimelineLane& l : t.lanes) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
}

} // namespace

const TimelineLane* MultimodalTimeline::Lane(const std::string& id) const {
    for (const TimelineLane& l : lanes) {
        if (l.id == id) {
            return &l;
        }
    }
    return nullptr;
}

const TimelineItem* MultimodalTimeline::Find(const std::string& itemId) const {
    for (const TimelineLane& l : lanes) {
        for (const TimelineItem& i : l.items) {
            if (i.id == itemId) {
                return &i;
            }
        }
    }
    return nullptr;
}

std::vector<const TimelineItem*> MultimodalTimeline::At(int timeMS) const {
    std::vector<const TimelineItem*> out;
    for (const TimelineLane& l : lanes) {
        if (l.id == "beats" || l.id == "bars") {
            continue;
        }
        for (const TimelineItem& i : l.items) {
            if (i.startMS <= timeMS && timeMS < std::max(i.endMS, i.startMS + 1)) {
                out.push_back(&i);
            }
        }
    }
    return out;
}

MultimodalTimeline BuildTimeline(const TimelineInput& in) {
    MultimodalTimeline t;
    for (const char* id : { "structure", "beats", "bars", "energy", "events", "narration", "lead", "chorus", "secondary", "instrumental", "words", "figures" }) {
        t.lanes.push_back({ id, LaneTitle(id), {} });
    }
    const auto add = [&](const std::string& lane, TimelineItem item) {
        item.id = lane + "/" + item.source;
        t.durationMS = std::max(t.durationMS, item.endMS);
        LaneOf(t, lane)->items.push_back(std::move(item));
    };

    if (in.song != nullptr) {
        t.durationMS = std::max(t.durationMS, in.song->durationMS);
        for (size_t i = 0; i < in.song->sections.size(); ++i) {
            const SongSection& s = in.song->sections[i];
            const std::string kind = SectionKindId(s.kind);
            add("structure", { "", "section:" + std::to_string(i), s.startMS, s.endMS, kind, SectionPhrase(kind), s.confidence, s.provenance == Provenance::Detected ? "detected" : "corrected" });
        }
        int beat = 0;
        int bar = 0;
        for (const MusicalEvent& e : in.song->events) {
            if (e.type == MusicalEventType::Beat || e.type == MusicalEventType::Downbeat) {
                add("beats", { "", "beat:" + std::to_string(beat++), e.timeMS, e.timeMS, "beat", COMPOSER_TEXT("Beat"), e.confidence, "detected" });
            }
            if (e.type == MusicalEventType::Downbeat) {
                add("bars", { "", "bar:" + std::to_string(bar++), e.timeMS, e.timeMS, "bar", COMPOSER_TEXT("Bar"), e.confidence, "detected" });
            }
        }
    }

    if (in.audio != nullptr) {
        t.durationMS = std::max(t.durationMS, in.audio->durationMS);
        for (const SoundEvent& e : in.audio->events) {
            if (MIRRORED_EVENTS.contains(e.kind)) {
                continue;
            }
            TimelineItem item{ "", e.id, e.startMS, e.endMS, e.kind, EventPhrase(e.kind), e.confidence, e.provenance };
            item.editable = true;
            add(ENERGY_EVENTS.contains(e.kind) ? "energy" : "events", std::move(item));
        }
        for (const ActivitySegment& s : in.audio->activities) {
            std::string lane;
            if (s.kind == "silence" || s.kind == "pause" || s.kind == "noise") {
                lane = "events";
            } else if (s.kind == "instrumental" || s.kind == "percussion") {
                lane = "instrumental";
            } else {
                continue; // the voices come from the vocal reading, the music runs underneath them all
            }
            TimelineItem item{ "", s.id, s.startMS, s.endMS, s.kind, ActivityPhrase(s.kind), s.confidence, s.provenance };
            item.editable = true;
            add(lane, std::move(item));
        }
    }

    if (in.voices != nullptr) {
        for (const Intervention& i : in.voices->interventions) {
            const VoiceTrack* track = in.voices->Track(i.track);
            TimelineItem item{ "", i.id, i.startMS, i.endMS, i.emission, EmissionPhrase(i.emission), i.confidence, i.provenance };
            item.track = i.track;
            if (track != nullptr) {
                item.who = track->label;
            }
            // Only what came from the sound can be corrected here; a phrase from the
            // lyrics alone is corrected in the lyrics.
            item.editable = !i.activityId.empty();
            add(VoiceLane(track), std::move(item));
        }
    }

    if (in.alignment != nullptr) {
        for (const AlignedWord& w : in.alignment->words) {
            if (w.startMS < 0 || w.endMS <= w.startMS) {
                continue;
            }
            add("words", { "", w.id, w.startMS, w.endMS, "word", w.text, w.confidence, w.source });
        }
    }

    if (in.performers != nullptr && in.audio != nullptr && in.voices != nullptr) {
        for (const PerformerAssignment& a : in.performers->assignments) {
            if (a.figures.empty()) {
                continue;
            }
            int n = 0;
            for (const auto& [from, to] : ActiveWindows(a, *in.audio, *in.voices)) {
                if (from < 0 || to <= from) {
                    continue; // performs nowhere in the show
                }
                std::string names;
                for (const std::string& f : a.figures) {
                    names += (names.empty() ? "" : ", ") + f;
                }
                TimelineItem item{ "", a.source + "@" + std::to_string(n++), from, to, a.function, FunctionPhrase(a.function), a.confidence, a.state };
                item.who = names;
                item.figures = a.figures;
                item.editable = true;
                add("figures", std::move(item));
            }
        }
    }

    for (TimelineLane& l : t.lanes) {
        std::stable_sort(l.items.begin(), l.items.end(), [](const TimelineItem& a, const TimelineItem& b) { return a.startMS < b.startMS; });
    }
    return t;
}

TimelineChange TranslateEdit(const MultimodalTimeline& timeline, const TimelineEdit& edit, const AudioUnderstanding& audio, const VocalUnderstanding& voices, const PerformanceMap* performers) {
    TimelineChange change;
    const TimelineItem* item = timeline.Find(edit.itemId);
    if (item == nullptr) {
        change.error = "item-not-found";
        return change;
    }
    const std::string lane = edit.itemId.substr(0, edit.itemId.find('/'));
    if (lane == "structure" || lane == "beats" || lane == "bars") {
        change.error = "edit-in-music-analysis";
        return change;
    }
    if (lane == "words") {
        change.error = "edit-in-lyrics";
        return change;
    }

    if (lane == "figures") {
        if (edit.action != TimelineEdit::Action::Reassign && edit.action != TimelineEdit::Action::Confirm && edit.action != TimelineEdit::Action::Remove) {
            change.error = "figures-follow-their-voice";
            return change;
        }
        const std::string source = item->source.substr(0, item->source.rfind('@'));
        const PerformerAssignment* current = performers != nullptr ? performers->For(source) : nullptr;
        PerformerAssignment a = current != nullptr ? *current : PerformerAssignment{};
        a.source = source;
        a.state = "manual";
        a.confidence = 1;
        a.conflicts.clear();
        a.reasons = { "chosen by a person" };
        if (edit.action == TimelineEdit::Action::Reassign) {
            a.figures = edit.figures;
        } else if (edit.action == TimelineEdit::Action::Remove) {
            a.figures.clear();
        }
        change.ok = true;
        change.isAssignment = true;
        change.assignment = std::move(a);
        return change;
    }

    // A voice item is corrected through the stretch of sound it came from.
    UnderstandingCorrection c;
    const ActivitySegment* segment = nullptr;
    const SoundEvent* event = nullptr;
    std::string target = item->source;
    if (lane == "narration" || lane == "lead" || lane == "chorus" || lane == "secondary") {
        const auto it = std::find_if(voices.interventions.begin(), voices.interventions.end(), [&](const Intervention& i) { return i.id == item->source; });
        if (it == voices.interventions.end() || it->activityId.empty()) {
            change.error = "edit-in-lyrics";
            return change;
        }
        target = it->activityId;
    }
    segment = audio.Find(target);
    if (segment == nullptr) {
        for (const SoundEvent& e : audio.events) {
            event = e.id == target ? &e : event;
        }
    }
    if (segment == nullptr && event == nullptr) {
        change.error = "item-not-found";
        return change;
    }
    c.on = segment != nullptr ? "activity" : "event";
    c.targetId = target;
    c.anchorMS = segment != nullptr ? segment->startMS : event->startMS;
    switch (edit.action) {
    case TimelineEdit::Action::Move:
        if (edit.startMS < 0 || edit.endMS < edit.startMS || (segment != nullptr && edit.endMS == edit.startMS)) {
            change.error = "times-not-valid";
            return change;
        }
        c.action = UnderstandingCorrection::Action::Move;
        c.startMS = edit.startMS;
        c.endMS = edit.endMS;
        break;
    case TimelineEdit::Action::Split:
        if (event != nullptr) {
            change.error = "moment-cannot-split";
            return change;
        }
        if (edit.atMS <= segment->startMS || edit.atMS >= segment->endMS) {
            change.error = "times-not-valid";
            return change;
        }
        c.action = UnderstandingCorrection::Action::Split;
        c.atMS = edit.atMS;
        break;
    case TimelineEdit::Action::Merge: {
        const TimelineItem* other = timeline.Find(edit.otherItemId);
        if (event != nullptr || other == nullptr) {
            change.error = event != nullptr ? "moment-cannot-merge" : "item-not-found";
            return change;
        }
        std::string otherTarget = other->source;
        const auto it = std::find_if(voices.interventions.begin(), voices.interventions.end(), [&](const Intervention& i) { return i.id == other->source; });
        if (it != voices.interventions.end()) {
            otherTarget = it->activityId;
        }
        if (audio.Find(otherTarget) == nullptr || otherTarget == target) {
            change.error = "item-not-found";
            return change;
        }
        c.action = UnderstandingCorrection::Action::Merge;
        c.otherId = otherTarget;
        break;
    }
    case TimelineEdit::Action::Reclassify:
        if (edit.kind.empty()) {
            change.error = "kind-missing";
            return change;
        }
        c.action = UnderstandingCorrection::Action::Reclassify;
        c.kind = edit.kind;
        break;
    case TimelineEdit::Action::Reassign:
        if (event != nullptr) {
            change.error = "moment-has-no-voice";
            return change;
        }
        c.action = UnderstandingCorrection::Action::Reassign;
        c.track = edit.track;
        break;
    case TimelineEdit::Action::Remove:
        c.action = UnderstandingCorrection::Action::Remove;
        break;
    case TimelineEdit::Action::Confirm:
        c.action = UnderstandingCorrection::Action::Confirm;
        break;
    }
    c.note = "timeline";
    change.ok = true;
    change.isCorrection = true;
    change.correction = std::move(c);
    return change;
}

void ApplyChange(const TimelineChange& change, AudioUnderstanding& audio, std::vector<PerformerAssignment>& manual) {
    if (!change.ok) {
        return;
    }
    if (change.isCorrection) {
        audio.corrections.push_back(change.correction);
        ApplyUnderstandingCorrections(audio);
    }
    if (change.isAssignment) {
        std::erase_if(manual, [&](const PerformerAssignment& a) { return a.source == change.assignment.source; });
        manual.push_back(change.assignment);
    }
}

std::string LaneTitle(const std::string& lane) {
    static const std::map<std::string, const char*> P = {
        { "structure", COMPOSER_TEXT("Parts of the song") }, { "beats", COMPOSER_TEXT("Beats") }, { "bars", COMPOSER_TEXT("Bars of the music") },
        { "energy", COMPOSER_TEXT("How strong the music is") }, { "events", COMPOSER_TEXT("Hits, drops and silences") }, { "narration", COMPOSER_TEXT("Narration") },
        { "lead", COMPOSER_TEXT("Main voice") }, { "chorus", COMPOSER_TEXT("Choir") }, { "secondary", COMPOSER_TEXT("Other voices") },
        { "instrumental", COMPOSER_TEXT("Instruments alone") }, { "words", COMPOSER_TEXT("Words") }, { "figures", COMPOSER_TEXT("Figures") }
    };
    const auto it = P.find(lane);
    return it != P.end() ? it->second : lane;
}

std::string TimelineEditError(const std::string& code) {
    static const std::map<std::string, const char*> P = {
        { "item-not-found", COMPOSER_TEXT("That piece is no longer in the reading. Analyse again and retry.") },
        { "edit-in-music-analysis", COMPOSER_TEXT("Beats, bars and parts of the song are corrected in the music analysis.") },
        { "edit-in-lyrics", COMPOSER_TEXT("This comes from the lyrics: correct it in the lyrics editor.") },
        { "figures-follow-their-voice", COMPOSER_TEXT("A figure follows its voice: move the voice, or choose other figures.") },
        { "times-not-valid", COMPOSER_TEXT("The times are not valid.") },
        { "moment-cannot-split", COMPOSER_TEXT("A single moment cannot be split.") },
        { "moment-cannot-merge", COMPOSER_TEXT("A single moment cannot be joined to another.") },
        { "moment-has-no-voice", COMPOSER_TEXT("A moment of the music belongs to no voice.") },
        { "kind-missing", COMPOSER_TEXT("Choose what it is.") },
    };
    const auto it = P.find(code);
    return it != P.end() ? it->second : code;
}

} // namespace composer
