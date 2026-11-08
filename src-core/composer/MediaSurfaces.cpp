#include "composer/MediaSurfaces.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <functional>
#include <cmath>
#include <map>

using nlohmann::json;

namespace composer {

namespace {

const char* const ROLE_IDS[] = { "main", "secondary", "tree", "lyrics", "narration", "media", "support", "off" };
const char* const MODE_IDS[] = { "lyrics-first", "smart", "free" };

double R3(double v) {
    return std::round(v * 1000.0) / 1000.0;
}

bool Has(const std::vector<std::string>& v, const std::string& x) {
    return std::find(v.begin(), v.end(), x) != v.end();
}

std::string OrientationOf(double aspect, bool round) {
    if (round) {
        return "round";
    }
    return aspect >= 2.0 ? "panoramic" : aspect >= 1.2 ? "landscape" : aspect > 0.83 ? "square" : aspect > 0.5 ? "portrait" : "tall";
}

} // namespace

const ScreenChoice* ScreenSettings::Find(const std::string& target) const {
    const auto it = std::find_if(screens.begin(), screens.end(), [&](const ScreenChoice& c) { return c.target == target; });
    return it != screens.end() ? &*it : nullptr;
}

const Surface* SurfacePlan::Find(const std::string& target) const {
    const auto it = std::find_if(surfaces.begin(), surfaces.end(), [&](const Surface& s) { return s.target == target; });
    return it != surfaces.end() ? &*it : nullptr;
}

const char* SurfaceRoleId(SurfaceRole role) {
    return ROLE_IDS[static_cast<int>(role)];
}

bool SurfaceRoleFromId(const std::string& id, SurfaceRole& role) {
    for (int i = 0; i < 8; ++i) {
        if (id == ROLE_IDS[i]) {
            role = static_cast<SurfaceRole>(i);
            return true;
        }
    }
    return false;
}

const char* LyricsModeId(LyricsMode mode) {
    return MODE_IDS[static_cast<int>(mode)];
}

bool LyricsModeFromId(const std::string& id, LyricsMode& mode) {
    for (int i = 0; i < 3; ++i) {
        if (id == MODE_IDS[i]) {
            mode = static_cast<LyricsMode>(i);
            return true;
        }
    }
    return false;
}

SurfacePlan PlanSurfaces(const ModelCapabilities& caps, const CompositionPlan* plan, const ScreenSettings& settings) {
    SurfacePlan out;
    out.mode = settings.mode;
    out.preview = settings.preview;
    const double duration = plan != nullptr ? std::max(1, plan->durationMS) : 1.0;
    // Channels of every model, to find figures that share lights.
    struct Range {
        const TargetInfo* t;
        uint32_t first, last;
    };
    std::vector<Range> ranges;
    for (const TargetInfo& t : caps.targets) {
        if (t.type == TargetType::Model && t.lastChannel >= t.firstChannel && t.lastChannel > 0) {
            ranges.push_back({ &t, t.firstChannel, t.lastChannel });
        }
    }
    double maxArea = 0;
    int maxPixels = 1;
    for (const TargetInfo& t : caps.targets) {
        const RoleAssignment* role = t.PrimaryRole();
        const bool matrixRole = role != nullptr && role->role == Role::Matrix;
        const bool treeRole = role != nullptr && (role->role == Role::MegaTree || role->role == Role::SmallTree);
        const double density = t.bufferWi > 0 && t.bufferHt > 0 ? static_cast<double>(t.nodes) / (static_cast<double>(t.bufferWi) * t.bufferHt) : 0;
        const bool tall = t.bufferWi >= 8 && t.bufferHt >= 16;
        const bool square = t.bufferWi >= 16 && t.bufferHt >= 16;
        std::string kind;
        if (t.type == TargetType::Model && matrixRole) {
            kind = "matrix";
        } else if (t.type == TargetType::Model && treeRole && tall) {
            kind = role->role == Role::MegaTree ? "mega-tree" : "tree";
        } else if (t.type == TargetType::Model && square && density >= 0.6 && t.displayAs != "ModelGroup") {
            kind = "grid";
        } else if (t.type == TargetType::SubModel && square && density >= 0.6) {
            const TargetInfo* parent = caps.Find(t.parent);
            const RoleAssignment* pr = parent != nullptr ? parent->PrimaryRole() : nullptr;
            if (pr != nullptr && pr->role == Role::Matrix) {
                kind = "submodel";
            }
        }
        if (kind.empty() || t.Excluded() || t.nodes <= 0 || t.channelsPerNode < 3) {
            continue;
        }
        Surface s;
        s.target = t.name;
        s.kind = kind;
        s.bufferWi = t.bufferWi;
        s.bufferHt = t.bufferHt;
        s.pixels = t.nodes;
        s.density = R3(std::min(1.0, density));
        s.degrees = t.degrees;
        const bool round = (kind == "mega-tree" || kind == "tree") && t.degrees >= 300;
        s.aspect = R3(t.width > 0 && t.height > 0 ? t.width / t.height : static_cast<double>(t.bufferWi) / std::max(1, t.bufferHt));
        s.orientation = OrientationOf(s.aspect, round);
        s.image = t.Can(Capability::Image).support;
        s.video = t.Can(Capability::Video).support;
        if ((kind == "mega-tree" || kind == "tree") && s.image == Support::Unknown) {
            // A tree's buffer is its strings side by side: a picture framed for it is drawn.
            s.image = Support::Yes;
            s.video = Support::Yes;
            s.reasons.push_back("tree-buffer-wraps");
        }
        s.nativeOk = s.image == Support::Yes && s.density >= 0.5;
        s.noController = t.noController;
        for (const Range& r : ranges) {
            if (r.t == &t || r.t->name == t.name || t.type != TargetType::Model) {
                continue;
            }
            if (r.first <= t.lastChannel && t.firstChannel <= r.last && s.conflicts.size() < 5) {
                s.conflicts.push_back(r.t->name);
            }
        }
        // How much of the song it shows words, or a face sings on it, or a group lights it.
        if (plan != nullptr) {
            double lyric = 0, busy = 0;
            for (const PlanSegment& seg : plan->segments) {
                const int len = seg.endMS - seg.startMS;
                bool words = false, taken = false;
                for (const PlanAssignment& a : seg.assignments) {
                    if (a.prominence == Prominence::Rest) {
                        continue;
                    }
                    if (a.target == t.name) {
                        words = words || a.intent == "lyric-text";
                        taken = taken || a.intent == "face-sing";
                        continue;
                    }
                    const TargetInfo* other = caps.Find(a.target);
                    if (other != nullptr && ((other->type == TargetType::Group && Has(other->flatModels, t.name)) || (other->type == TargetType::SubModel && other->parent == t.name))) {
                        taken = true;
                    }
                }
                lyric += words ? len : 0;
                busy += words || taken ? len : 0;
            }
            s.lyricShare = R3(lyric / duration);
            s.freeShare = R3(std::max(0.0, 1.0 - busy / duration));
        } else {
            s.freeShare = 1;
        }
        maxArea = std::max(maxArea, t.width * t.height);
        maxPixels = std::max(maxPixels, t.nodes);
        out.surfaces.push_back(std::move(s));
    }
    for (Surface& s : out.surfaces) {
        const TargetInfo* t = caps.Find(s.target);
        const double area = t != nullptr ? t->width * t->height : 0;
        s.visibility = R3(maxArea > 0 && area > 0 ? area / maxArea : static_cast<double>(s.pixels) / maxPixels);
        // A main screen must be big and seen, free to show pictures, wired, and own its lights.
        const double landscape = s.aspect >= 1.1 && s.aspect <= 3.5 ? 1.0 : 0.5;
        s.score = R3(0.35 * s.visibility + 0.25 * static_cast<double>(s.pixels) / maxPixels + 0.25 * s.freeShare + 0.1 * landscape + (s.noController ? 0 : 0.05) - (s.conflicts.empty() ? 0 : 0.3) -
                     (s.nativeOk ? 0 : 0.5));
    }
    std::stable_sort(out.surfaces.begin(), out.surfaces.end(), [](const Surface& a, const Surface& b) { return a.score != b.score ? a.score > b.score : a.target < b.target; });
    // A person's roles first.
    for (Surface& s : out.surfaces) {
        if (const ScreenChoice* c = settings.Find(s.target); c != nullptr && c->roleSet) {
            s.role = c->role;
            s.roleSource = "person";
            s.reasons.push_back("chosen-by-person");
            // A person may put pictures on a figure the program would not choose, as long
            // as it can draw them at all; the result is said to be coarse.
            if (!s.nativeOk && (s.image == Support::Limited || s.image == Support::Unknown) && s.density >= 0.5 && s.role != SurfaceRole::Off && s.role != SurfaceRole::Support) {
                s.nativeOk = true;
                if (s.image == Support::Limited) {
                    s.reasons.push_back("small-buffer-chosen-by-person");
                    out.warnings.push_back(fmt::format("surface-coarse:{}:{}x{}", s.target, s.bufferWi, s.bufferHt));
                } else {
                    s.reasons.push_back("unchecked-shape-chosen-by-person");
                    out.warnings.push_back(fmt::format("surface-unchecked:{}", s.target));
                }
            }
        }
    }
    const auto taken = [&](SurfaceRole r) { return std::any_of(out.surfaces.begin(), out.surfaces.end(), [&](const Surface& s) { return s.role == r && !s.roleSource.empty(); }); };
    // Screens showing words keep them, except in free mode where only a person decides.
    if (settings.mode != LyricsMode::Free) {
        for (Surface& s : out.surfaces) {
            if (s.roleSource.empty() && s.lyricShare >= 0.15) {
                s.role = SurfaceRole::Lyrics;
                s.roleSource = "auto";
                s.reasons.push_back(fmt::format("shows-lyrics:{:.0f}%", s.lyricShare * 100));
            }
        }
    }
    // A screen nothing drives is never the program's choice for the real installation:
    // its pictures would only exist on the computer.
    const auto usable = [&](const Surface& s) { return s.nativeOk && (settings.preview || !s.noController); };
    const auto pick = [&](SurfaceRole role, const std::function<bool(const Surface&)>& ok, const std::string& why) {
        if (taken(role)) {
            return;
        }
        for (Surface& s : out.surfaces) {
            if (s.roleSource.empty() && usable(s) && ok(s)) {
                s.role = role;
                s.roleSource = "auto";
                s.reasons.push_back(why);
                return;
            }
        }
    };
    pick(SurfaceRole::Main, [](const Surface& s) { return s.kind == "matrix" || s.kind == "grid"; }, "main:largest-free-visible");
    pick(SurfaceRole::Tree, [](const Surface& s) { return s.kind == "mega-tree"; }, "tree:the-mega-tree");
    const Surface* main = nullptr;
    for (const Surface& s : out.surfaces) {
        main = s.role == SurfaceRole::Main ? &s : main;
    }
    pick(SurfaceRole::Secondary, [&](const Surface& s) {
        return (s.kind == "matrix" || s.kind == "grid") && (main == nullptr || (s.pixels * 4 >= main->pixels && s.visibility >= 0.15));
    }, "secondary:second-matrix-worth-it");
    for (Surface& s : out.surfaces) {
        if (s.roleSource.empty()) {
            s.role = s.kind == "submodel" ? SurfaceRole::Off : SurfaceRole::Support;
            s.roleSource = "auto";
            s.reasons.push_back(s.kind == "submodel"                         ? "submodel:only-if-chosen"
                                : s.nativeOk && s.noController && !settings.preview ? "support:no-controller"
                                : s.nativeOk                                       ? "support:lights"
                                : s.image == Support::Unknown                      ? "support:shape-unknown"
                                                                                   : "support:not-a-picture");
        }
        s.availability = !s.nativeOk ? "layout" : s.noController ? "preview" : "installation";
        if (!s.conflicts.empty()) {
            out.warnings.push_back(fmt::format("surface-shares-channels:{}:{}", s.target, s.conflicts.front()));
        }
        // A screen showing pictures that no controller drives shows them only once wired.
        if (s.noController && s.role != SurfaceRole::Support && s.role != SurfaceRole::Off) {
            out.warnings.push_back(fmt::format("surface-no-controller:{}", s.target));
        }
    }
    return out;
}

void MergeScreenSettings(ScreenSettings& into, const ScreenSettings& extra) {
    if (!extra.set) {
        return;
    }
    into.set = true;
    into.mode = extra.mode;
    into.preview = extra.preview;
    for (const ScreenChoice& c : extra.screens) {
        std::erase_if(into.screens, [&](const ScreenChoice& o) { return o.target == c.target; });
        into.screens.push_back(c);
    }
    std::sort(into.screens.begin(), into.screens.end(), [](const ScreenChoice& a, const ScreenChoice& b) { return a.target < b.target; });
}

json ScreenSettingsJson(const ScreenSettings& s) {
    json screens = json::array();
    for (const ScreenChoice& c : s.screens) {
        json j = { { "target", c.target } };
        if (c.roleSet) {
            j["role"] = SurfaceRoleId(c.role);
        }
        if (c.frontOffset >= 0) {
            j["frontOffset"] = c.frontOffset;
        }
        if (c.turn != 0) {
            j["turn"] = c.turn;
        }
        screens.push_back(std::move(j));
    }
    json out = { { "lyricsMode", LyricsModeId(s.mode) }, { "screens", std::move(screens) } };
    if (s.preview) {
        out["preview"] = true;
    }
    return out;
}

bool ScreenSettingsFrom(const json& j, ScreenSettings& s, std::string& error) {
    s = ScreenSettings();
    if (!j.is_object()) {
        error = "screens: not an object";
        return false;
    }
    s.set = true;
    if (!LyricsModeFromId(j.value("lyricsMode", std::string("smart")), s.mode)) {
        error = "screens: unknown lyrics mode";
        return false;
    }
    s.preview = j.value("preview", false);
    for (const json& c : j.value("screens", json::array())) {
        ScreenChoice choice;
        choice.target = c.value("target", std::string());
        if (choice.target.empty()) {
            continue;
        }
        if (c.contains("role")) {
            if (!SurfaceRoleFromId(c.value("role", std::string()), choice.role)) {
                error = "screens: unknown role";
                return false;
            }
            choice.roleSet = true;
        }
        choice.frontOffset = c.value("frontOffset", -1.0);
        choice.turn = c.value("turn", 0);
        s.screens.push_back(std::move(choice));
    }
    return true;
}

json SurfacePlanJson(const SurfacePlan& p) {
    json list = json::array();
    for (const Surface& s : p.surfaces) {
        list.push_back({ { "target", s.target }, { "kind", s.kind }, { "buffer", { s.bufferWi, s.bufferHt } }, { "pixels", s.pixels }, { "density", s.density }, { "aspect", s.aspect },
                         { "orientation", s.orientation }, { "degrees", s.degrees }, { "visibility", s.visibility }, { "image", SupportId(s.image) }, { "video", SupportId(s.video) },
                         { "nativeOk", s.nativeOk }, { "noController", s.noController }, { "availability", s.availability }, { "conflicts", s.conflicts }, { "lyricShare", s.lyricShare }, { "freeShare", s.freeShare },
                         { "role", SurfaceRoleId(s.role) }, { "roleSource", s.roleSource }, { "score", s.score }, { "reasons", s.reasons } });
    }
    return { { "lyricsMode", LyricsModeId(p.mode) }, { "preview", p.preview }, { "surfaces", std::move(list) }, { "warnings", p.warnings } };
}

std::string SurfaceReport(const SurfacePlan& p) {
    std::string out = fmt::format("Screens ({} lyrics mode, for the {}): {} surfaces\n", LyricsModeId(p.mode), p.preview ? "preview" : "installation", p.surfaces.size());
    for (const Surface& s : p.surfaces) {
        std::string reasons;
        for (const std::string& r : s.reasons) {
            reasons += (reasons.empty() ? "" : ", ") + r;
        }
        out += fmt::format("  {} [{}] {}: {}x{} buffer, {} lights, real shape {:.2f} {}{}, seen {:.0f}%, words {:.0f}%, free {:.0f}%{}{}, score {:.2f}; {}\n", s.target, SurfaceRoleId(s.role), s.kind, s.bufferWi,
                           s.bufferHt, s.pixels, s.aspect, s.orientation, s.degrees > 0 ? fmt::format(" {:.0f} degrees", s.degrees) : std::string(), s.visibility * 100, s.lyricShare * 100,
                           s.freeShare * 100, s.noController ? ", no controller (" + s.availability + ")" : std::string(), s.conflicts.empty() ? std::string() : ", shares channels with " + s.conflicts.front(), s.score, reasons);
    }
    for (const std::string& w : p.warnings) {
        out += "  warning: " + w + "\n";
    }
    return out;
}

} // namespace composer
