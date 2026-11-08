#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class xLightsShowContext;

// What every part of the installation is and can do, as plain data the planner reads
// without touching xLights' objects. Extracted once from a loaded layout, on whatever
// thread owns it; everything after that is independent of the UI thread.
//
// Roles say what a target is (a mega tree, a window); capabilities say what it can
// show (text, a face). Neither comes from the name alone: names are only a hint.
namespace composer {

inline constexpr int CAPABILITIES_SCHEMA_VERSION = 1;
inline constexpr int ROLES_SCHEMA_VERSION = 1;

enum class Role {
    Matrix,
    MegaTree,
    SmallTree,
    Window,
    Arch,
    Line,
    Star,
    Face,
    Figure,
    Prop,
    Group,
    Excluded
};
inline constexpr int ROLE_COUNT = 12;

enum class RoleStatus {
    Confirmed,       // set by hand in roles.json
    Suggested,       // inferred, with a source and a confidence
    Unknown,         // nothing to infer it from
    ExcludedManually // a person took it out of the show
};

struct RoleAssignment {
    Role role = Role::Prop;
    RoleStatus status = RoleStatus::Suggested;
    std::string source; // "manual", "display-type", "face-definition", "geometry", "name-hint"
    double confidence = 0;
};

enum class Capability {
    Text,
    LyricTrack,
    Image,
    Video,
    Face,
    Geometric,
    Color,
    GroupDelivery,
    States
};
inline constexpr int CAPABILITY_COUNT = 9;

// Unknown is an answer: some things are only known once rendered, and an unknown is
// never treated as a confirmed incompatibility.
enum class Support {
    Yes,
    Limited,
    No,
    Unknown
};

struct CapabilityValue {
    Support support = Support::Unknown;
    std::string reason;
};

enum class TargetType {
    Model,
    Group,
    SubModel
};

struct TargetInfo {
    std::string name;     // as xLights knows it; "Parent/Sub" for a submodel
    std::string identity; // name + "|" + signature: what roles.json is matched against
    std::string signature; // type, nodes and buffer: survives rewiring, not a change of model
    TargetType type = TargetType::Model;
    std::string displayAs;
    std::string parent; // submodels only
    int bufferWi = 0;
    int bufferHt = 0;
    double width = 0;   // in the layout's world units: the figure's real proportions,
    double height = 0;  // which its buffer's need not share
    double degrees = 0; // trees: how far round the strings go (360: all the way)
    double topRatio = 0; // trees: bottom diameter over top diameter
    int nodes = 0;
    int strands = 0;
    int channelsPerNode = 0;
    std::string stringType;
    std::string controller;
    bool noController = false;
    uint32_t firstChannel = 0;
    uint32_t lastChannel = 0;
    std::vector<std::string> faces;     // definitions with something to draw
    std::vector<std::string> states;
    std::vector<std::string> subModels;
    std::vector<std::string> groups;     // groups that list it directly
    std::vector<std::string> members;    // groups: their direct members
    std::vector<std::string> flatModels; // groups: every model reached, nested groups resolved
    std::vector<RoleAssignment> roles;   // best first
    std::array<CapabilityValue, CAPABILITY_COUNT> capabilities{};
    std::vector<std::string> warnings;

    [[nodiscard]] bool HasRole(Role role) const;
    [[nodiscard]] const RoleAssignment* PrimaryRole() const;
    [[nodiscard]] bool Excluded() const;
    [[nodiscard]] const CapabilityValue& Can(Capability c) const { return capabilities[static_cast<int>(c)]; }
};

// A person's role choices, kept in <show>/ShowComposer/roles.json.
struct RoleOverride {
    std::string name;
    std::string signature; // empty: applies to whatever has that name, with a warning
    std::vector<Role> roles;
    bool excluded = false;
    std::string note;
};

struct RoleOverrides {
    int schemaVersion = ROLES_SCHEMA_VERSION;
    std::string layoutFingerprint;
    std::vector<RoleOverride> models;
};

struct ModelCapabilities {
    int schemaVersion = CAPABILITIES_SCHEMA_VERSION;
    std::string layoutFingerprint;
    std::vector<TargetInfo> targets; // models, then groups, then submodels; each by name
    std::vector<std::string> warnings;
    std::vector<std::string> unresolved; // models nothing could classify

    [[nodiscard]] const TargetInfo* Find(const std::string& name) const;
    [[nodiscard]] int IndexOf(const std::string& name) const;
    // Whether lighting one would light part of the other: a model and a group holding
    // it, two groups sharing a member, a submodel and its parent.
    [[nodiscard]] bool Overlap(const std::string& a, const std::string& b) const;
    [[nodiscard]] int Count(TargetType type) const;
    // Excluded itself, or a group or submodel that would light an excluded model.
    [[nodiscard]] bool LightsExcluded(const std::string& name) const;

    // Built by Index(); the planner's fast path.
    std::map<std::string, int> index;
    std::vector<std::vector<uint64_t>> modelBits; // per target: which models it lights
    void Index();
};

// Reads the layout loaded in `ctx`. `overrides` may be null.
[[nodiscard]] ModelCapabilities ExtractCapabilities(const xLightsShowContext& ctx, const RoleOverrides* overrides);

// Applies roles.json to capabilities extracted earlier. Matched by name and signature:
// a model that kept its name but changed shape is reported, not given the old role.
void ApplyRoleOverrides(ModelCapabilities& caps, const RoleOverrides& overrides);

// Infers roles and capabilities for a target whose plain fields are already filled.
void ClassifyTarget(TargetInfo& target);

[[nodiscard]] const char* RoleId(Role role);
[[nodiscard]] bool RoleFromId(const std::string& id, Role& role);
[[nodiscard]] const char* RoleStatusId(RoleStatus status);
[[nodiscard]] const char* CapabilityId(Capability capability);
[[nodiscard]] const char* SupportId(Support support);
[[nodiscard]] const char* TargetTypeId(TargetType type);

[[nodiscard]] std::string ToJson(const ModelCapabilities& caps);
// The JSON ToJson writes, read back with its index rebuilt.
[[nodiscard]] bool FromJson(const std::string& text, ModelCapabilities& caps, std::string& error);
[[nodiscard]] std::string ToJson(const RoleOverrides& overrides);
[[nodiscard]] bool FromJson(const std::string& text, RoleOverrides& overrides, std::string& error);
// A roles.json with every model and its current suggestion, ready to edit by hand.
[[nodiscard]] RoleOverrides RoleTemplate(const ModelCapabilities& caps);
[[nodiscard]] std::string CapabilitiesReport(const ModelCapabilities& caps);

} // namespace composer
