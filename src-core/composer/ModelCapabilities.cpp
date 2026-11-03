#include "composer/ModelCapabilities.h"

#include "composer/ComposerText.h"
#include "composer/GenerationValidator.h"
#include "models/DisplayAsType.h"
#include "models/Model.h"
#include "models/ModelGroup.h"
#include "render/xLightsShowContext.h"
#include "utils/ExternalHooks.h"
#include "utils/FileUtils.h"

#include <nlohmann/json.hpp>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <functional>
#include <set>

using json = nlohmann::json;

namespace composer {

namespace {

const char* const ROLE_IDS[ROLE_COUNT] = { "matrix", "mega-tree", "small-tree", "window", "arch", "line", "star", "face", "figure", "prop", "group", "excluded" };
const char* const ROLE_STATUS_IDS[] = { "confirmed", "suggested", "unknown", "excluded-manually" };
const char* const CAPABILITY_IDS[CAPABILITY_COUNT] = { "text", "lyric-track", "image", "video", "face", "geometric", "color", "group-delivery", "states" };
const char* const SUPPORT_IDS[] = { "yes", "limited", "no", "unknown" };
const char* const TARGET_TYPE_IDS[] = { "model", "group", "submodel" };

// What the model type itself says. The geometry rules below refine trees; nothing
// here looks at the name.
struct TypeRule {
    DisplayAsType type;
    Role role;
    double confidence;
};

const TypeRule TYPE_RULES[] = {
    { DisplayAsType::Matrix, Role::Matrix, 0.9 },
    { DisplayAsType::Arches, Role::Arch, 0.9 },
    { DisplayAsType::Star, Role::Star, 0.9 },
    { DisplayAsType::WindowFrame, Role::Window, 0.9 },
    { DisplayAsType::SingleLine, Role::Line, 0.9 },
    { DisplayAsType::PolyLine, Role::Line, 0.8 },
    { DisplayAsType::Icicles, Role::Line, 0.7 },
    { DisplayAsType::CandyCanes, Role::Prop, 0.6 },
    { DisplayAsType::Circle, Role::Prop, 0.6 },
    { DisplayAsType::Wreath, Role::Prop, 0.6 },
    { DisplayAsType::Sphere, Role::Prop, 0.6 },
    { DisplayAsType::Spinner, Role::Prop, 0.6 },
    { DisplayAsType::Cube, Role::Prop, 0.5 },
    { DisplayAsType::Image, Role::Prop, 0.5 },
    { DisplayAsType::MultiPoint, Role::Prop, 0.5 },
    // A custom model is drawn by hand, most often a figure; little more can be said.
    { DisplayAsType::Custom, Role::Figure, 0.4 },
};

// Only when the model type says nothing better, and never for Matrix or MegaTree:
// "Ventana Matrix 1" is a window frame, and a "tree" can be any size.
struct NameHint {
    const char* word;
    Role role;
};

const NameHint NAME_HINTS[] = {
    { "ventana", Role::Window }, { "window", Role::Window },
    { "arco", Role::Arch }, { "arch", Role::Arch },
    { "estrella", Role::Star }, { "star", Role::Star },
    { "linea", Role::Line }, { "line", Role::Line },
};
constexpr double NAME_HINT_CONFIDENCE = 0.3;

// A tree big enough to carry a show: either by pixels or by height.
constexpr int MEGA_TREE_NODES = 500;
constexpr int MEGA_TREE_ROWS = 40;
// Below these, text and pictures cannot be read whatever the model.
constexpr int TEXT_ROWS = 7;
constexpr int TEXT_COLUMNS = 20;
constexpr int PICTURE_SIZE = 16;
constexpr int GEOMETRIC_NODES = 10;

void AddRole(TargetInfo& t, Role role, RoleStatus status, const char* source, double confidence) {
    for (RoleAssignment& r : t.roles) {
        if (r.role == role) {
            if (confidence > r.confidence) {
                r = { role, status, source, confidence };
            }
            return;
        }
    }
    t.roles.push_back({ role, status, source, confidence });
}

void Set(TargetInfo& t, Capability c, Support s, std::string reason) {
    t.capabilities[static_cast<int>(c)] = { s, std::move(reason) };
}

bool IsDmx(DisplayAsType type) {
    return IsDmxDisplayType(type) || type == DisplayAsType::ChannelBlock;
}

std::string Signature(const TargetInfo& t) {
    return fmt::format("{}:{}:{}x{}", t.displayAs, t.nodes, t.bufferWi, t.bufferHt);
}

std::vector<uint64_t> Bits(size_t size) {
    return std::vector<uint64_t>((size + 63) / 64, 0);
}

} // namespace

bool TargetInfo::HasRole(Role role) const {
    return std::any_of(roles.begin(), roles.end(), [&](const RoleAssignment& r) { return r.role == role && r.status != RoleStatus::ExcludedManually; });
}

const RoleAssignment* TargetInfo::PrimaryRole() const {
    return roles.empty() ? nullptr : &roles.front();
}

bool TargetInfo::Excluded() const {
    return std::any_of(roles.begin(), roles.end(), [](const RoleAssignment& r) { return r.role == Role::Excluded || r.status == RoleStatus::ExcludedManually; });
}

void ClassifyTarget(TargetInfo& t) {
    t.roles.clear();
    const DisplayAsType type = DisplayAsTypeFromString(t.displayAs);
    if (t.type == TargetType::Group) {
        AddRole(t, Role::Group, RoleStatus::Suggested, "display-type", 1.0);
    } else if (IsDmx(type)) {
        AddRole(t, Role::Excluded, RoleStatus::Suggested, "display-type", 0.7);
    } else if (type == DisplayAsType::Tree) {
        const bool mega = t.nodes >= MEGA_TREE_NODES || t.bufferHt >= MEGA_TREE_ROWS;
        AddRole(t, mega ? Role::MegaTree : Role::SmallTree, RoleStatus::Suggested, "geometry", 0.7);
    } else {
        for (const TypeRule& rule : TYPE_RULES) {
            if (rule.type == type) {
                AddRole(t, rule.role, RoleStatus::Suggested, "display-type", rule.confidence);
            }
        }
    }
    if (!t.faces.empty() && t.type != TargetType::Group) {
        AddRole(t, Role::Face, RoleStatus::Suggested, "face-definition", 0.9);
    }
    const bool weak = t.roles.empty() || (t.roles.size() == 1 && t.roles[0].confidence < 0.5 && t.roles[0].role != Role::Face);
    if (weak && t.type != TargetType::Group) {
        const std::string folded = text::Fold(t.type == TargetType::SubModel ? t.name.substr(t.name.find('/') + 1) : t.name);
        for (const NameHint& hint : NAME_HINTS) {
            if (folded.find(hint.word) != std::string::npos) {
                AddRole(t, hint.role, RoleStatus::Suggested, "name-hint", NAME_HINT_CONFIDENCE);
                break;
            }
        }
    }
    std::stable_sort(t.roles.begin(), t.roles.end(), [](const RoleAssignment& a, const RoleAssignment& b) { return a.confidence > b.confidence; });

    // Capabilities: what can be said without rendering, and why.
    const bool pixels = !IsDmx(type) && t.nodes > 0;
    const bool matrixLike = type == DisplayAsType::Matrix;
    if (!pixels) {
        for (int c = 0; c < CAPABILITY_COUNT; ++c) {
            t.capabilities[c] = { Support::No, "not-a-pixel-model" };
        }
        return;
    }
    if (t.bufferHt >= TEXT_ROWS && t.bufferWi >= TEXT_COLUMNS) {
        Set(t, Capability::Text, matrixLike ? Support::Yes : Support::Unknown, matrixLike ? "buffer-readable" : "legibility-depends-on-node-layout");
    } else if (t.bufferHt >= 3 && t.bufferWi >= 3) {
        Set(t, Capability::Text, Support::Limited, "buffer-too-small-for-words");
    } else {
        Set(t, Capability::Text, Support::No, "no-rows-to-write-on");
    }
    if (t.bufferHt >= PICTURE_SIZE && t.bufferWi >= PICTURE_SIZE) {
        Set(t, Capability::Image, matrixLike ? Support::Yes : Support::Unknown, matrixLike ? "buffer-sufficient" : "depends-on-node-layout");
        // Whether a given file decodes is only known when it is chosen.
        Set(t, Capability::Video, matrixLike ? Support::Yes : Support::Unknown, matrixLike ? "buffer-sufficient-file-checked-at-generation" : "depends-on-node-layout");
    } else {
        Set(t, Capability::Image, Support::Limited, "buffer-too-small");
        Set(t, Capability::Video, Support::Limited, "buffer-too-small");
    }
    if (t.type == TargetType::Group) {
        // FacesEffect only spreads an image face over a group; a node face does nothing.
        Set(t, Capability::Face, Support::No, "faces-render-on-models");
    } else if (!t.faces.empty()) {
        Set(t, Capability::Face, Support::Yes, "face-definition");
    } else {
        Set(t, Capability::Face, Support::No, "no-face-definition");
    }
    const Support text = t.Can(Capability::Text).support;
    if (t.Can(Capability::Face).support == Support::Yes || text == Support::Yes) {
        Set(t, Capability::LyricTrack, Support::Yes, t.Can(Capability::Face).support == Support::Yes ? "face" : "text");
    } else {
        Set(t, Capability::LyricTrack, text == Support::Unknown ? Support::Unknown : Support::No, text == Support::Unknown ? "text-legibility-unknown" : "no-face-or-text");
    }
    Set(t, Capability::Geometric, t.nodes >= GEOMETRIC_NODES ? Support::Yes : (t.nodes > 1 ? Support::Limited : Support::No), t.nodes >= GEOMETRIC_NODES ? "enough-nodes" : "few-nodes");
    if (t.type == TargetType::Group || t.channelsPerNode >= 3) {
        Set(t, Capability::Color, Support::Yes, "rgb");
    } else if (t.channelsPerNode == 1) {
        Set(t, Capability::Color, Support::Limited, "single-colour");
    } else {
        Set(t, Capability::Color, Support::Unknown, "channels-per-node-unknown");
    }
    if (t.type == TargetType::Group) {
        Set(t, Capability::GroupDelivery, Support::Yes, "is-a-group");
    } else {
        Set(t, Capability::GroupDelivery, t.groups.empty() ? Support::No : Support::Yes, t.groups.empty() ? "in-no-group" : "member-of-groups");
    }
    Set(t, Capability::States, t.states.empty() ? Support::No : Support::Yes, t.states.empty() ? "no-state-definitions" : "state-definitions");
}

namespace {

// Models on a controller whose channel ranges cross: lighting one lights part of the
// other, whatever the groups say. Models with no controller have no real channels.
std::map<std::string, std::vector<std::string>> SharedChannelPartners(const std::vector<TargetInfo>& targets) {
    std::vector<const TargetInfo*> models;
    for (const TargetInfo& t : targets) {
        if (t.type == TargetType::Model && !t.noController && t.nodes > 0 && t.lastChannel >= t.firstChannel) {
            models.push_back(&t);
        }
    }
    std::sort(models.begin(), models.end(), [](const TargetInfo* a, const TargetInfo* b) { return a->firstChannel != b->firstChannel ? a->firstChannel < b->firstChannel : a->name < b->name; });
    std::map<std::string, std::vector<std::string>> partners;
    for (size_t i = 0; i < models.size(); ++i) {
        for (size_t j = i + 1; j < models.size() && models[j]->firstChannel <= models[i]->lastChannel; ++j) {
            partners[models[i]->name].push_back(models[j]->name);
            partners[models[j]->name].push_back(models[i]->name);
        }
    }
    return partners;
}

} // namespace

ModelCapabilities ExtractCapabilities(const xLightsShowContext& ctx, const RoleOverrides* overrides) {
    ModelCapabilities caps;
    caps.layoutFingerprint = LayoutFingerprint(ctx);
    std::vector<TargetInfo> models;
    std::vector<TargetInfo> groups;
    std::vector<TargetInfo> subs;
    std::map<std::string, const ModelGroup*> groupObjects;

    const auto fill = [](TargetInfo& t, const Model& m) {
        t.displayAs = m.GetDisplayAsString();
        t.bufferWi = m.GetDefaultBufferWi();
        t.bufferHt = m.GetDefaultBufferHt();
        t.nodes = static_cast<int>(m.GetNodeCount());
        t.strands = m.GetNumStrands();
        t.stringType = m.GetStringType();
        t.channelsPerNode = static_cast<int>(m.GetNodeChannelCount(t.stringType));
        t.firstChannel = m.GetFirstChannel();
        t.lastChannel = m.GetLastChannel();
    };

    for (const auto& [name, model] : ctx.AllModels) {
        if (model == nullptr) {
            continue;
        }
        TargetInfo t;
        t.name = name;
        fill(t, *model);
        if (const auto* group = dynamic_cast<const ModelGroup*>(model)) {
            t.type = TargetType::Group;
            t.members = group->ModelNames();
            groupObjects[name] = group;
            groups.push_back(std::move(t));
            continue;
        }
        t.controller = model->GetControllerName();
        t.noController = t.controller == "No Controller";
        for (const auto& [face, info] : model->GetFaceInfo()) {
            const bool drawable = std::any_of(info.begin(), info.end(), [](const auto& kv) { return kv.first.rfind("Mouth-", 0) == 0 && kv.first.find("-Color") == std::string::npos && !kv.second.empty(); });
            // An image face whose files are not on this machine paints every node red,
            // as the validator says: it is not a face that can be drawn here.
            const auto type = info.find("Type");
            const bool images = type != info.end() && type->second == "Matrix";
            const bool missing = images && std::any_of(info.begin(), info.end(), [&](const auto& kv) {
                return kv.first != "CustomColors" && kv.first != "ImagePlacement" && kv.first != "Type" && kv.first != "Name" && !kv.second.empty() && !FileExists(FileUtils::FixFile(ctx.GetShowDirectory(), kv.second));
            });
            if (!face.empty() && drawable && missing) {
                t.warnings.push_back("face-images-missing:" + face);
            } else if (!face.empty() && drawable) {
                t.faces.push_back(face);
            }
        }
        for (const auto& [state, info] : model->GetStateInfo()) {
            if (!state.empty()) {
                t.states.push_back(state);
            }
        }
        for (int s = 0; s < model->GetNumSubModels(); ++s) {
            const Model* sub = model->GetSubModel(s);
            if (sub == nullptr) {
                continue;
            }
            TargetInfo st;
            st.name = name + "/" + sub->GetName();
            st.type = TargetType::SubModel;
            st.parent = name;
            fill(st, *sub);
            st.displayAs = t.displayAs; // a submodel draws like its parent
            st.controller = t.controller;
            st.noController = t.noController;
            t.subModels.push_back(sub->GetName());
            subs.push_back(std::move(st));
        }
        models.push_back(std::move(t));
    }

    std::map<std::string, size_t> modelIndex;
    for (size_t i = 0; i < models.size(); ++i) {
        modelIndex[models[i].name] = i;
    }
    std::map<std::string, size_t> groupIndex;
    for (size_t i = 0; i < groups.size(); ++i) {
        groupIndex[groups[i].name] = i;
    }
    // Every model a group reaches, through nested groups and submodels, guarded
    // against a group that ends up containing itself.
    std::set<std::string> cycles;
    std::function<void(const std::string&, std::set<std::string>&, std::set<std::string>&)> flatten = [&](const std::string& group, std::set<std::string>& path, std::set<std::string>& out) {
        if (!path.insert(group).second) {
            cycles.insert(group);
            return;
        }
        for (const std::string& member : groups[groupIndex[group]].members) {
            const std::string base = member.substr(0, member.find('/'));
            if (groupIndex.contains(member)) {
                flatten(member, path, out);
            } else if (modelIndex.contains(base)) {
                out.insert(base);
            }
        }
        path.erase(group);
    };
    for (TargetInfo& g : groups) {
        std::set<std::string> path;
        std::set<std::string> flat;
        flatten(g.name, path, flat);
        g.flatModels.assign(flat.begin(), flat.end());
        g.nodes = 0;
        for (const std::string& m : g.flatModels) {
            g.nodes += models[modelIndex[m]].nodes;
        }
        for (const std::string& member : g.members) {
            const std::string base = member.substr(0, member.find('/'));
            if (modelIndex.contains(base)) {
                auto& list = models[modelIndex[base]].groups;
                if (std::find(list.begin(), list.end(), g.name) == list.end()) {
                    list.push_back(g.name);
                }
            } else if (!groupIndex.contains(member)) {
                g.warnings.push_back("member-missing:" + member);
            }
        }
    }
    for (const std::string& g : cycles) {
        caps.warnings.push_back("group-cycle:" + g);
    }
    for (TargetInfo& m : models) {
        std::sort(m.groups.begin(), m.groups.end());
    }
    for (std::vector<TargetInfo>* list : { &models, &groups, &subs }) {
        for (TargetInfo& t : *list) {
            t.signature = Signature(t);
            t.identity = t.name + "|" + t.signature;
            ClassifyTarget(t);
            caps.targets.push_back(std::move(t));
        }
    }
    caps.Index();
    for (const auto& [name, others] : SharedChannelPartners(caps.targets)) {
        TargetInfo& t = caps.targets[caps.IndexOf(name)];
        for (const std::string& other : others) {
            t.warnings.push_back("shares-channels:" + other);
        }
        caps.warnings.push_back(fmt::format("{} shares channels with {} other models: they never light at the same time", name, others.size()));
    }
    if (overrides != nullptr) {
        ApplyRoleOverrides(caps, *overrides);
    }
    for (const TargetInfo& t : caps.targets) {
        if (t.type == TargetType::Model && t.roles.empty()) {
            caps.unresolved.push_back(t.name);
        }
    }
    return caps;
}

void ModelCapabilities::Index() {
    index.clear();
    for (size_t i = 0; i < targets.size(); ++i) {
        index[targets[i].name] = static_cast<int>(i);
    }
    std::map<std::string, size_t> modelOrdinal;
    for (const TargetInfo& t : targets) {
        if (t.type == TargetType::Model) {
            modelOrdinal.emplace(t.name, modelOrdinal.size());
        }
    }
    modelBits.assign(targets.size(), Bits(modelOrdinal.size()));
    const std::map<std::string, std::vector<std::string>> partners = SharedChannelPartners(targets);
    for (size_t i = 0; i < targets.size(); ++i) {
        const TargetInfo& t = targets[i];
        const auto markOne = [&](const std::string& model) {
            const auto found = modelOrdinal.find(model);
            if (found != modelOrdinal.end()) {
                modelBits[i][found->second / 64] |= 1ULL << (found->second % 64);
            }
        };
        const auto mark = [&](const std::string& model) {
            markOne(model);
            if (const auto shared = partners.find(model); shared != partners.end()) {
                for (const std::string& other : shared->second) {
                    markOne(other);
                }
            }
        };
        if (t.type == TargetType::Model) {
            mark(t.name);
        } else if (t.type == TargetType::SubModel) {
            mark(t.parent);
        } else {
            for (const std::string& m : t.flatModels) {
                mark(m);
            }
        }
    }
}

const TargetInfo* ModelCapabilities::Find(const std::string& name) const {
    const int i = IndexOf(name);
    return i < 0 ? nullptr : &targets[i];
}

int ModelCapabilities::IndexOf(const std::string& name) const {
    const auto found = index.find(name);
    return found == index.end() ? -1 : found->second;
}

bool ModelCapabilities::Overlap(const std::string& a, const std::string& b) const {
    const int i = IndexOf(a);
    const int j = IndexOf(b);
    if (i < 0 || j < 0 || i == j) {
        return i == j && i >= 0;
    }
    // Two submodels of one model may light different nodes; whether they do is a
    // node-level question this level does not answer.
    if (targets[i].type == TargetType::SubModel && targets[j].type == TargetType::SubModel) {
        return false;
    }
    for (size_t w = 0; w < modelBits[i].size(); ++w) {
        if (modelBits[i][w] & modelBits[j][w]) {
            return true;
        }
    }
    return false;
}

bool ModelCapabilities::LightsExcluded(const std::string& name) const {
    const TargetInfo* t = Find(name);
    if (t == nullptr) {
        return false;
    }
    if (t->Excluded()) {
        return true;
    }
    if (t->type == TargetType::SubModel) {
        const TargetInfo* parent = Find(t->parent);
        return parent != nullptr && parent->Excluded();
    }
    return std::any_of(t->flatModels.begin(), t->flatModels.end(), [&](const std::string& m) {
        const TargetInfo* model = Find(m);
        return model != nullptr && model->Excluded();
    });
}

int ModelCapabilities::Count(TargetType type) const {
    return static_cast<int>(std::count_if(targets.begin(), targets.end(), [&](const TargetInfo& t) { return t.type == type; }));
}

void ApplyRoleOverrides(ModelCapabilities& caps, const RoleOverrides& overrides) {
    if (!overrides.layoutFingerprint.empty() && overrides.layoutFingerprint != caps.layoutFingerprint) {
        caps.warnings.push_back("roles-from-other-layout");
    }
    std::set<std::string> listed;
    for (const RoleOverride& o : overrides.models) {
        listed.insert(o.name);
        TargetInfo* t = nullptr;
        const int i = caps.IndexOf(o.name);
        if (i >= 0) {
            t = &caps.targets[i];
        }
        if (t == nullptr) {
            // The same shape under another name may be a rename; it is reported, never
            // assumed.
            std::string candidate;
            int matches = 0;
            for (const TargetInfo& other : caps.targets) {
                if (!o.signature.empty() && other.signature == o.signature && other.type == TargetType::Model) {
                    candidate = other.name;
                    ++matches;
                }
            }
            caps.warnings.push_back(matches == 1 ? fmt::format("role-model-missing:{} (possibly renamed to {})", o.name, candidate) : "role-model-missing:" + o.name);
            continue;
        }
        if (!o.signature.empty() && o.signature != t->signature) {
            caps.warnings.push_back(fmt::format("role-identity-changed:{} ({} -> {})", o.name, o.signature, t->signature));
            continue;
        }
        if (o.signature.empty()) {
            caps.warnings.push_back("role-without-signature:" + o.name);
        }
        if (o.excluded) {
            for (RoleAssignment& r : t->roles) {
                r.status = RoleStatus::ExcludedManually;
            }
            t->roles.insert(t->roles.begin(), { Role::Excluded, RoleStatus::ExcludedManually, "manual", 1.0 });
            continue;
        }
        if (!o.roles.empty()) {
            std::vector<RoleAssignment> manual;
            for (Role r : o.roles) {
                manual.push_back({ r, RoleStatus::Confirmed, "manual", 1.0 });
            }
            t->roles = std::move(manual);
        }
    }
}

const char* RoleId(Role role) {
    return ROLE_IDS[static_cast<int>(role)];
}

bool RoleFromId(const std::string& id, Role& role) {
    for (int i = 0; i < ROLE_COUNT; ++i) {
        if (id == ROLE_IDS[i]) {
            role = static_cast<Role>(i);
            return true;
        }
    }
    return false;
}

const char* RoleStatusId(RoleStatus status) {
    return ROLE_STATUS_IDS[static_cast<int>(status)];
}

const char* CapabilityId(Capability capability) {
    return CAPABILITY_IDS[static_cast<int>(capability)];
}

const char* SupportId(Support support) {
    return SUPPORT_IDS[static_cast<int>(support)];
}

const char* TargetTypeId(TargetType type) {
    return TARGET_TYPE_IDS[static_cast<int>(type)];
}

std::string ToJson(const ModelCapabilities& caps) {
    json root;
    root["schemaVersion"] = caps.schemaVersion;
    root["layoutFingerprint"] = caps.layoutFingerprint;
    root["counts"] = { { "models", caps.Count(TargetType::Model) }, { "groups", caps.Count(TargetType::Group) }, { "submodels", caps.Count(TargetType::SubModel) } };
    root["warnings"] = caps.warnings;
    root["unresolved"] = caps.unresolved;
    json targets = json::array();
    for (const TargetInfo& t : caps.targets) {
        json roles = json::array();
        for (const RoleAssignment& r : t.roles) {
            roles.push_back({ { "role", RoleId(r.role) }, { "status", RoleStatusId(r.status) }, { "source", r.source }, { "confidence", r.confidence } });
        }
        json capabilities = json::object();
        for (int c = 0; c < CAPABILITY_COUNT; ++c) {
            capabilities[CAPABILITY_IDS[c]] = { { "support", SupportId(t.capabilities[c].support) }, { "reason", t.capabilities[c].reason } };
        }
        json item = { { "name", t.name }, { "identity", t.identity }, { "type", TargetTypeId(t.type) }, { "displayAs", t.displayAs }, { "buffer", { t.bufferWi, t.bufferHt } }, { "nodes", t.nodes }, { "strands", t.strands },
                      { "channelsPerNode", t.channelsPerNode }, { "controller", t.controller }, { "noController", t.noController }, { "channels", { t.firstChannel, t.lastChannel } },
                      { "roles", roles }, { "capabilities", capabilities } };
        if (!t.parent.empty()) {
            item["parent"] = t.parent;
        }
        for (const auto& [key, list] : { std::pair<const char*, const std::vector<std::string>*>{ "faces", &t.faces }, { "states", &t.states }, { "subModels", &t.subModels }, { "groups", &t.groups }, { "members", &t.members }, { "flatModels", &t.flatModels }, { "warnings", &t.warnings } }) {
            if (!list->empty()) {
                item[key] = *list;
            }
        }
        targets.push_back(std::move(item));
    }
    root["targets"] = std::move(targets);
    return root.dump(1);
}

bool FromJson(const std::string& text, ModelCapabilities& caps, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    const auto idOf = [](const auto& ids, int count, const std::string& id, int fallback) {
        for (int i = 0; i < count; ++i) {
            if (id == ids[i]) {
                return i;
            }
        }
        return fallback;
    };
    try {
        ModelCapabilities out;
        out.schemaVersion = root.at("schemaVersion").get<int>();
        if (out.schemaVersion > CAPABILITIES_SCHEMA_VERSION) {
            error = "written by a newer version";
            return false;
        }
        out.layoutFingerprint = root.value("layoutFingerprint", std::string());
        out.warnings = root.value("warnings", std::vector<std::string>());
        out.unresolved = root.value("unresolved", std::vector<std::string>());
        for (const json& item : root.at("targets")) {
            TargetInfo t;
            t.name = item.at("name").get<std::string>();
            t.identity = item.value("identity", std::string());
            t.type = static_cast<TargetType>(idOf(TARGET_TYPE_IDS, 3, item.value("type", std::string("model")), 0));
            t.displayAs = item.value("displayAs", std::string());
            if (item.contains("buffer") && item["buffer"].size() == 2) {
                t.bufferWi = item["buffer"][0].get<int>();
                t.bufferHt = item["buffer"][1].get<int>();
            }
            t.nodes = item.value("nodes", 0);
            t.strands = item.value("strands", 0);
            t.channelsPerNode = item.value("channelsPerNode", 0);
            t.controller = item.value("controller", std::string());
            t.noController = item.value("noController", false);
            if (item.contains("channels") && item["channels"].size() == 2) {
                t.firstChannel = item["channels"][0].get<uint32_t>();
                t.lastChannel = item["channels"][1].get<uint32_t>();
            }
            t.parent = item.value("parent", std::string());
            for (const json& r : item.value("roles", json::array())) {
                t.roles.push_back({ static_cast<Role>(idOf(ROLE_IDS, ROLE_COUNT, r.value("role", std::string("prop")), static_cast<int>(Role::Prop))),
                                    static_cast<RoleStatus>(idOf(ROLE_STATUS_IDS, 4, r.value("status", std::string("suggested")), static_cast<int>(RoleStatus::Suggested))), r.value("source", std::string()), r.value("confidence", 0.0) });
            }
            const json capabilities = item.value("capabilities", json::object());
            for (int c = 0; c < CAPABILITY_COUNT; ++c) {
                if (capabilities.contains(CAPABILITY_IDS[c])) {
                    const json& v = capabilities[CAPABILITY_IDS[c]];
                    t.capabilities[c] = { static_cast<Support>(idOf(SUPPORT_IDS, 4, v.value("support", std::string("unknown")), static_cast<int>(Support::Unknown))), v.value("reason", std::string()) };
                }
            }
            t.faces = item.value("faces", std::vector<std::string>());
            t.states = item.value("states", std::vector<std::string>());
            t.subModels = item.value("subModels", std::vector<std::string>());
            t.groups = item.value("groups", std::vector<std::string>());
            t.members = item.value("members", std::vector<std::string>());
            t.flatModels = item.value("flatModels", std::vector<std::string>());
            t.warnings = item.value("warnings", std::vector<std::string>());
            const size_t bar = t.identity.find('|');
            t.signature = bar == std::string::npos ? std::string() : t.identity.substr(bar + 1);
            out.targets.push_back(std::move(t));
        }
        out.Index();
        caps = std::move(out);
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::string ToJson(const RoleOverrides& overrides) {
    json root;
    root["schemaVersion"] = overrides.schemaVersion;
    root["layoutFingerprint"] = overrides.layoutFingerprint;
    root["help"] = "Set \"roles\" to any of: matrix, mega-tree, small-tree, window, arch, line, star, face, figure, prop. Set \"excluded\": true to keep a model out of the show. A model whose \"signature\" no longer matches is reported and left as inferred.";
    json models = json::array();
    for (const RoleOverride& o : overrides.models) {
        json roles = json::array();
        for (Role r : o.roles) {
            roles.push_back(RoleId(r));
        }
        models.push_back({ { "name", o.name }, { "signature", o.signature }, { "roles", roles }, { "excluded", o.excluded }, { "note", o.note } });
    }
    root["models"] = std::move(models);
    return root.dump(1);
}

bool FromJson(const std::string& text, RoleOverrides& overrides, std::string& error) {
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        error = "not a JSON object";
        return false;
    }
    try {
        RoleOverrides out;
        out.schemaVersion = root.value("schemaVersion", 1);
        if (out.schemaVersion > ROLES_SCHEMA_VERSION) {
            error = "written by a newer version";
            return false;
        }
        out.layoutFingerprint = root.value("layoutFingerprint", std::string());
        for (const json& m : root.value("models", json::array())) {
            RoleOverride o;
            o.name = m.at("name").get<std::string>();
            o.signature = m.value("signature", std::string());
            o.excluded = m.value("excluded", false);
            o.note = m.value("note", std::string());
            for (const json& r : m.value("roles", json::array())) {
                Role role;
                if (!RoleFromId(r.get<std::string>(), role)) {
                    error = fmt::format("unknown role '{}' for '{}'", r.get<std::string>(), o.name);
                    return false;
                }
                o.roles.push_back(role);
            }
            out.models.push_back(std::move(o));
        }
        overrides = std::move(out);
    } catch (const json::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

RoleOverrides RoleTemplate(const ModelCapabilities& caps) {
    RoleOverrides out;
    out.layoutFingerprint = caps.layoutFingerprint;
    for (const TargetInfo& t : caps.targets) {
        if (t.type != TargetType::Model) {
            continue;
        }
        RoleOverride o;
        o.name = t.name;
        o.signature = t.signature;
        for (const RoleAssignment& r : t.roles) {
            if (r.status == RoleStatus::Confirmed) {
                o.roles.push_back(r.role);
            }
        }
        o.excluded = std::any_of(t.roles.begin(), t.roles.end(), [](const RoleAssignment& r) { return r.status == RoleStatus::ExcludedManually; });
        std::string suggested;
        for (const RoleAssignment& r : t.roles) {
            suggested += fmt::format("{}{} ({}, {:.1f})", suggested.empty() ? "" : ", ", RoleId(r.role), r.source, r.confidence);
        }
        o.note = suggested.empty() ? "suggested: none" : "suggested: " + suggested;
        out.models.push_back(std::move(o));
    }
    return out;
}

std::string CapabilitiesReport(const ModelCapabilities& caps) {
    std::string out = fmt::format("Layout {}: {} models, {} groups, {} submodels\n", caps.layoutFingerprint, caps.Count(TargetType::Model), caps.Count(TargetType::Group), caps.Count(TargetType::SubModel));
    for (const std::string& w : caps.warnings) {
        out += "warning: " + w + "\n";
    }
    if (!caps.unresolved.empty()) {
        out += "unresolved roles:";
        for (const std::string& name : caps.unresolved) {
            out += " [" + name + "]";
        }
        out += "\n";
    }
    out += "\nmodels\n";
    for (const TargetInfo& t : caps.targets) {
        if (t.type != TargetType::Model) {
            continue;
        }
        std::string roles;
        for (const RoleAssignment& r : t.roles) {
            roles += fmt::format("{}{}:{}({},{:.1f})", roles.empty() ? "" : " ", RoleId(r.role), RoleStatusId(r.status), r.source, r.confidence);
        }
        std::string known;
        std::string unknown;
        for (int c = 0; c < CAPABILITY_COUNT; ++c) {
            const Support s = t.capabilities[c].support;
            if (s == Support::Yes || s == Support::Limited) {
                known += fmt::format("{}{}{}", known.empty() ? "" : ",", CAPABILITY_IDS[c], s == Support::Limited ? "(limited)" : "");
            } else if (s == Support::Unknown) {
                unknown += fmt::format("{}{}", unknown.empty() ? "" : ",", CAPABILITY_IDS[c]);
            }
        }
        out += fmt::format("  {:<32} {:<12} {:>5} nodes {:>3}x{:<3} {}{}\n      roles: {}\n      can: {}{}{}\n", t.name, t.displayAs, t.nodes, t.bufferWi, t.bufferHt, t.noController ? "[no controller] " : "",
                           t.faces.empty() ? "" : "faces: " + std::to_string(t.faces.size()), roles.empty() ? "unknown" : roles, known, unknown.empty() ? "" : "  unknown: ", unknown);
    }
    out += "\ngroups\n";
    for (const TargetInfo& t : caps.targets) {
        if (t.type == TargetType::Group) {
            out += fmt::format("  {:<40} {} members, {} models{}\n", t.name, t.members.size(), t.flatModels.size(), t.warnings.empty() ? "" : "  (" + std::to_string(t.warnings.size()) + " missing members)");
        }
    }
    return out;
}

} // namespace composer
