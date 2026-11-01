#pragma once

#include <string>
#include <utility>
#include <vector>

// Plain data describing a sequence to write. Holds no xLights objects, so a
// spec can be built and compared without a show loaded.
namespace composer {

struct TimingMark {
    std::string label;
    int startMS = 0;
    int endMS = 0;
};

struct TimingTrackSpec {
    std::string name;
    std::vector<std::vector<TimingMark>> layers;
};

struct EffectSpec {
    std::string target;   // model or group name
    std::string subModel; // empty when the effect goes on the target itself
    int layer = 0;
    std::string effect;
    // Raw values: the writer escapes them through SettingsMap.
    std::vector<std::pair<std::string, std::string>> settings;
    std::string palette;
    int startMS = 0;
    int endMS = 0;
};

struct SequenceSpec {
    int frameMS = 25;
    int durationMS = 0;
    std::vector<TimingTrackSpec> timingTracks;
    std::vector<EffectSpec> effects;
};

struct WriteResult {
    bool ok = false;
    std::string finalPath;
    std::vector<std::string> problems;
    std::vector<std::string> leftoverFiles;
};

} // namespace composer
