#pragma once

#include <set>
#include <stdexcept>
#include <vector>

namespace oreslang::types {

enum class AutoTrait { Send, Sync, Share };
enum class AutoTypeKind {
    Scalar, Nominal, Box, Arc, Weak, Rc, RefCell, Mutex, RwLock, Dyn
};

struct Capabilities {
    bool send = false;
    bool sync = false;
    bool share = false;

    bool has(AutoTrait trait) const noexcept {
        switch (trait) {
        case AutoTrait::Send: return send;
        case AutoTrait::Sync: return sync;
        case AutoTrait::Share: return share;
        }
        return false;
    }
};

struct AutoType {
    AutoTypeKind kind = AutoTypeKind::Scalar;
    std::vector<AutoType> parameters;
    std::set<AutoTrait> dyn_bounds;
    bool shareable_domain = false;
    // Nominal auto traits require a complete instantiated layout proof.
    // Opaque/imported/unresolved nominal provenance fails closed.
    bool complete_nominal_layout = false;
    std::set<AutoTrait> negative;
};

inline Capabilities infer_auto_traits(const AutoType& type);

inline const AutoType& exactly_one(const AutoType& type) {
    if (type.parameters.size() != 1)
        throw std::invalid_argument("ownership auto-trait wrapper requires one type parameter");
    return type.parameters.front();
}

inline bool all_have(const std::vector<AutoType>& values, AutoTrait trait) {
    for (const auto& value : values)
        if (!infer_auto_traits(value).has(trait)) return false;
    return true;
}

inline Capabilities apply_negative(Capabilities caps, const std::set<AutoTrait>& negative) {
    if (negative.count(AutoTrait::Send)) caps.send = false;
    if (negative.count(AutoTrait::Sync)) caps.sync = false;
    if (negative.count(AutoTrait::Share)) caps.share = false;
    return caps;
}

/**
 * Backend-independent Send/Sync/Share composition. Share is intentionally
 * separate from Send: moving a unique owner never grants shared actor-domain
 * aliases to its backing allocation.
 */
inline Capabilities infer_auto_traits(const AutoType& type) {
    Capabilities caps;
    switch (type.kind) {
    case AutoTypeKind::Scalar:
        caps = {true, true, true};
        break;
    case AutoTypeKind::Nominal:
        if (!type.complete_nominal_layout) {
            caps = {false, false, false};
            break;
        }
        caps = {
            all_have(type.parameters, AutoTrait::Send),
            all_have(type.parameters, AutoTrait::Sync),
            type.shareable_domain && all_have(type.parameters, AutoTrait::Share)
        };
        break;
    case AutoTypeKind::Box: {
        const auto nested = infer_auto_traits(exactly_one(type));
        caps = {nested.send, nested.sync, false};
        break;
    }
    case AutoTypeKind::Arc:
    case AutoTypeKind::Weak: {
        const auto nested = infer_auto_traits(exactly_one(type));
        const bool concurrent =
            type.shareable_domain && nested.send && nested.sync;
        caps = {concurrent, concurrent, concurrent && nested.share};
        break;
    }
    case AutoTypeKind::Rc:
        (void)exactly_one(type);
        caps = {false, false, false};
        break;
    case AutoTypeKind::RefCell: {
        const auto nested = infer_auto_traits(exactly_one(type));
        caps = {nested.send, false, false};
        break;
    }
    case AutoTypeKind::Mutex: {
        const auto nested = infer_auto_traits(exactly_one(type));
        caps = {nested.send, nested.send, false};
        break;
    }
    case AutoTypeKind::RwLock: {
        const auto nested = infer_auto_traits(exactly_one(type));
        caps = {nested.send, nested.send && nested.sync, false};
        break;
    }
    case AutoTypeKind::Dyn:
        caps = {
            type.dyn_bounds.count(AutoTrait::Send) != 0,
            type.dyn_bounds.count(AutoTrait::Sync) != 0,
            type.shareable_domain && type.dyn_bounds.count(AutoTrait::Share) != 0
        };
        break;
    }
    return apply_negative(caps, type.negative);
}

} // namespace oreslang::types
