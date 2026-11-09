#pragma once

#include "composer/MediaLibrary.h"
#include "composer/VisualReview.h"

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <string>
#include <vector>

// What a show's maker likes, learned from their reviews and set by hand. Local, plain and
// reversible: every learned value comes from signals that name the review decision they
// came from, so a value can be explained, a review forgotten, a preference changed,
// turned off or reset. Nothing here is a model: a value is a bounded weighted mean.
//
// Keys and what they mean (a positive value is "more of it"):
//   kind.video                        videos rather than pictures
//   light.dark                        dark pictures and videos
//   motion.moving                     slow camera movements (pans, zooms, turns)
//   transition.<kind>                 that way in ("cut", "fade", "crossfade", "zoom", "wipe")
//   repetition                        files coming back in the song
//   amount                            how much of the song has pictures (set by hand)
//   concept.<id>                      files showing that concept
//   screen.<screen>.presence          pictures on that screen
//   screen.<screen>.concept.<id>      that concept on that screen
//   scene.<role>.concept.<id>         that concept in that part of a song
//   scene.<role>.motion.moving        movement in that part of a song
//   profile.<id>.kind.video, profile.<id>.motion.moving   the same, within one artistic profile
//   file.<media>@<screen>             one file on one screen
//   director.variety                  recipes changing more from scene to scene
//   director.christmas                colours nearer the Christmas ones than the song's
namespace composer {

inline constexpr int PREFERENCES_VERSION = 1;
// Learned values need this much backing before they act, from at least two reviews: two
// full decisions (an older one weighs 0.85), never one, and never one show alone.
inline constexpr double PREFERENCE_MIN_SUPPORT = 1.75;
inline constexpr int PREFERENCE_MIN_REVIEWS = 2;

struct PreferenceSignal {
    std::string key;
    double sign = 0;   // -1 .. 1
    double weight = 1; // 0 .. 1: no single decision weighs more than one
    int session = 0;   // which review session it came from: older ones weigh less
    std::string origin; // "<sequence>|<decision>"
    std::string what;   // the decision in short: "approved", "rejected", "wrong-motion"...
};

struct PreferenceOverride {
    bool hasValue = false; // a person's own value replaces the learned one
    double value = 0;
    bool disabled = false; // the preference is ignored
};

struct ArtisticPreferences {
    int version = PREFERENCES_VERSION;
    bool learning = true; // false: learned values are kept but not used
    int session = 0;
    std::vector<PreferenceSignal> signals;
    std::map<std::string, PreferenceOverride> overrides;
};

struct Preference {
    std::string key;
    std::string scope; // "global", "screen", "scene", "profile", "file"
    double learned = 0;
    double support = 0;
    int decisions = 0;
    int reviews = 0;     // the different shows' reviews behind it: a taste needs two
    double value = 0;    // what generation uses (0 when off or not backed enough)
    std::string source;  // "learned", "person", "learned-not-enough"
    bool enabled = true;
    bool active = false;
    std::vector<std::string> origins; // the decisions behind it, newest first
};

// Every preference with signals or a person's value, by key.
[[nodiscard]] std::vector<Preference> ComputePreferences(const ArtisticPreferences& prefs);
// The values generation uses: a person's own values, and (when learning is on) the
// learned ones backed enough. Keys with no value are absent.
[[nodiscard]] std::map<std::string, double> EffectivePreferences(const ArtisticPreferences& prefs);

// The signals a review gives, replacing the ones it gave before (a changed decision
// changes its signal; a decision back to pending takes its signal away). Returns how
// many signals changed; a change opens a new session.
int LearnFromReview(ArtisticPreferences& prefs, const VisualReview& review, const MediaLibrary* library, const std::string& profileId);
// Takes back everything learned from one sequence's review.
int ForgetReview(ArtisticPreferences& prefs, const std::string& sequence);
void SetPreference(ArtisticPreferences& prefs, const std::string& key, double value);
void EnablePreference(ArtisticPreferences& prefs, const std::string& key, bool enabled);
// Forgets one preference: its signals and the person's value.
void ResetPreference(ArtisticPreferences& prefs, const std::string& key);
void ResetAllPreferences(ArtisticPreferences& prefs);

[[nodiscard]] std::string PreferencesPath(const std::string& showDir);
[[nodiscard]] nlohmann::json PreferencesJson(const ArtisticPreferences& prefs);
[[nodiscard]] bool PreferencesFrom(const nlohmann::json& object, ArtisticPreferences& prefs, std::string& error);
// A missing file is an empty profile (true); a damaged or newer one is an error.
bool ReadPreferences(const std::string& path, ArtisticPreferences& prefs, std::string& error);
bool SavePreferences(const std::string& path, const ArtisticPreferences& prefs, std::string& error);

} // namespace composer
