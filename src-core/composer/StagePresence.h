#pragma once

#include "composer/StageDirector.h"

#include <string>
#include <vector>

// The score made the show's behaviour (phase 21). The director says, scene by scene, which
// figure leads, accompanies, answers, keeps a quiet glow or rests; here every effect the
// other engines made is made to follow it: the effects of a resting figure stop for the
// scene, a glowing figure's are dimmed, and an effect on a group of xLights that holds
// figures asked to do different things is split into one effect per figure that keeps
// playing, so a group no longer lights the figures the score rests. Faces, words, pictures,
// videos and a person's locked or edited effects are never touched. Figures wired to the same
// lights are kept from contradicting each other. The gestures and identity layers of the
// score come after this (RealizeStage) and are the score's own.
namespace composer {

// ArtLevel Presence and above: applies every scene's presence to the choreography's own
// effects and records the shared lights and what was done in the score. Returns notes.
std::vector<std::string> ApplyStagePresence(GlobalStageScore& score, const StageDirectorInput& input, Choreography& choreography);

// ArtLevel Orchestra: links each scene's composition to the next: figures that come in are
// staggered from where the next idea starts and fade in, figures that go out fade (and, where
// the light gathers, leave from the farthest first), so the house does not cut from one idea
// to the next. Records each join and what it was checked for. Returns notes.
std::vector<std::string> LinkCompositions(GlobalStageScore& score, const StageDirectorInput& input, Choreography& choreography);

// The pairs of stage figures wired to the same lights, from the layout's channels.
[[nodiscard]] std::vector<SharedLights> FindSharedLights(const ModelCapabilities& capabilities, const std::vector<std::string>& figures);

[[nodiscard]] std::string PresenceStatePhrase(const std::string& state);   // plain words (to translate)
[[nodiscard]] std::string TransitionKindPhrase(const std::string& kind);   // plain words (to translate)

} // namespace composer
