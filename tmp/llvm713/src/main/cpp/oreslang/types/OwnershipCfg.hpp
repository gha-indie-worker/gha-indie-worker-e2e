#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oreslang::ownership {

// Powerset lattice: a join preserves *every* possible incoming state.
// In particular {Owned, Moved} is NOT definitely initialized or movable.
using ValueMask = std::uint8_t;
constexpr ValueMask kUninitialized = 1;
constexpr ValueMask kOwned = 2;
constexpr ValueMask kMoved = 4;
constexpr ValueMask kDropped = 8;

struct PlaceState {
    ValueMask possible = kUninitialized;
    unsigned shared_loans = 0;
    bool exclusive_loan = false;
    // Identity, not merely the number of borrows, must survive a CFG join.
    // Token zero is reserved for a legacy loan already live at graph entry.
    std::set<std::uint64_t> shared_loan_ids;
    std::optional<std::uint64_t> exclusive_loan_id;

    // Preserve legacy three-field construction under -Werror while making
    // the new identity fields explicit and safely default-initialized.
    PlaceState(ValueMask state = kUninitialized, unsigned shared = 0,
            bool exclusive = false, std::set<std::uint64_t> sharedIds = {},
            std::optional<std::uint64_t> exclusiveId = std::nullopt)
        : possible(state), shared_loans(shared), exclusive_loan(exclusive),
          shared_loan_ids(std::move(sharedIds)), exclusive_loan_id(exclusiveId) {}

    bool operator==(const PlaceState& rhs) const {
        return possible == rhs.possible &&
            shared_loans == rhs.shared_loans &&
            exclusive_loan == rhs.exclusive_loan &&
            shared_loan_ids == rhs.shared_loan_ids &&
            exclusive_loan_id == rhs.exclusive_loan_id;
    }
};
using State = std::map<std::string, PlaceState>;

struct Operation {
    enum class Kind {
        Initialize, Read, Move, Drop, DropIfOwned,
        ReadObject, MoveObject, BorrowShared, BorrowExclusive,
        ReleaseShared, ReleaseExclusive
    };
    Kind kind;
    std::string place;
    // Optional explicit lexical loan identity; absent borrow IDs derive from
    // the original CFG operation site, not the active traversal order.
    std::optional<std::uint64_t> loan_id;

    Operation() = default;
    Operation(Kind operation, std::string key,
            std::optional<std::uint64_t> provenance = std::nullopt)
        : kind(operation), place(std::move(key)), loan_id(provenance) {}
};

struct BasicBlock {
    std::vector<Operation> operations;
    std::vector<std::size_t> successors;
};

struct Graph {
    State entry;
    std::vector<BasicBlock> blocks;
};

struct Analysis {
    std::vector<std::optional<State>> incoming;
    std::vector<std::optional<State>> outgoing;
};

[[noreturn]] inline void violation(const std::string& name, const std::string& why) {
    throw std::runtime_error("ownership violation: " + why + " '" + name + "'");
}

inline PlaceState& place(State& state, const std::string& key) {
    const auto found = state.find(key);
    if (found == state.end())
        violation(key, "unknown CFG place");
    return found->second;
}

// Flat CFG entries do not encode the alias relationship between a whole
// object and a simultaneously tracked projection of that same object.
// In particular, checking a loan on "p" does not protect "p.field" in
// apply(), and vice versa. Until this verifier has a proven projection tree,
// reject those mixed layouts instead of allowing a loan to be bypassed.
// Siblings (p.a and p.b) remain independent places.
inline void validate_disjoint_place_layout(const State& state) {
    for (const auto& item : state) {
        const std::string& key = item.first;
        if (key.empty() || key.front() == '.' || key.back() == '.' ||
                key.find("..") != std::string::npos)
            violation(key, "invalid CFG place path");
        for (std::size_t dot = key.find('.'); dot != std::string::npos;
                dot = key.find('.', dot + 1)) {
            if (state.count(key.substr(0, dot)))
                violation(key, "whole-object and projected-field places overlap without alias proof");
        }
    }
}

inline void require_owned(const std::string& key, const PlaceState& value) {
    if (value.possible != kOwned)
        violation(key, "not definitely initialized on every CFG predecessor");
}

inline void apply(Operation op, State& state, std::uint64_t site_id = 0) {
    // Keep direct helper callers as safe as verify(Graph), not just the CFG path.
    validate_disjoint_place_layout(state);
    if (op.kind == Operation::Kind::ReadObject ||
            op.kind == Operation::Kind::MoveObject) {
        // A non-Copy object requires *all* its fields. A moved field does
        // not prevent reading a different owned field, but prevents moving
        // or borrowing the complete object again.
        const std::string prefix = op.place + ".";
        bool saw_field = false;
        for (auto& item : state) {
            if (item.first.compare(0, prefix.size(), prefix) != 0) continue;
            saw_field = true;
            require_owned(item.first, item.second);
            // Reading an object aliases every field. An active exclusive
            // field loan forbids a whole-object read; shared field loans
            // permit reads but prohibit moving/dropping the complete owner.
            if (op.kind == Operation::Kind::ReadObject &&
                    item.second.exclusive_loan)
                violation(op.place, "whole-object read overlaps exclusive field loan");
            if (op.kind == Operation::Kind::MoveObject &&
                    (item.second.shared_loans || item.second.exclusive_loan))
                violation(op.place, "move while any field is borrowed");
        }
        if (!saw_field) violation(op.place, "object has no tracked fields");
        if (op.kind == Operation::Kind::MoveObject)
            for (auto& item : state)
                if (item.first.compare(0, prefix.size(), prefix) == 0)
                    item.second.possible = kMoved;
        return;
    }
    PlaceState& value = place(state, op.place);
    switch (op.kind) {
        case Operation::Kind::Initialize:
            if (value.possible != kUninitialized || value.shared_loans || value.exclusive_loan)
                violation(op.place, "initializing a live or ambiguous owner");
            value.possible = kOwned;
            return;
        case Operation::Kind::Read:
            require_owned(op.place, value);
            if (value.exclusive_loan) violation(op.place, "read overlaps mutable loan");
            return;
        case Operation::Kind::Move:
        case Operation::Kind::Drop:
            require_owned(op.place, value);
            if (value.shared_loans || value.exclusive_loan)
                violation(op.place, "moving/dropping a borrowed owner");
            value.possible = op.kind == Operation::Kind::Move ? kMoved : kDropped;
            return;
        case Operation::Kind::DropIfOwned:
            if (value.shared_loans || value.exclusive_loan)
                violation(op.place, "conditional drop with a live loan");
            // Runtime lowering MUST generate a corresponding ownership flag.
            if (value.possible & kOwned)
                value.possible = static_cast<ValueMask>(
                    (value.possible & static_cast<ValueMask>(~kOwned)) | kDropped);
            return;
        case Operation::Kind::BorrowShared:
            // The zero token denotes a pre-existing entry loan. A new CFG
            // borrow must never impersonate that identity.
            if (op.loan_id == 0) violation(op.place, "reserved entry loan identity");
            if (!op.loan_id && !site_id)
                violation(op.place, "borrow requires compiler site provenance");
            require_owned(op.place, value);
            if (value.exclusive_loan) violation(op.place, "shared borrow overlaps mutable loan");
            if (value.shared_loans == 65535) violation(op.place, "shared loan count exceeds bound");
            if (!value.shared_loan_ids.insert(op.loan_id.value_or(site_id)).second)
                violation(op.place, "shared loan identity already live");
            ++value.shared_loans;
            return;
        case Operation::Kind::BorrowExclusive:
            if (op.loan_id == 0) violation(op.place, "reserved entry loan identity");
            if (!op.loan_id && !site_id)
                violation(op.place, "borrow requires compiler site provenance");
            require_owned(op.place, value);
            if (value.exclusive_loan || value.shared_loans)
                violation(op.place, "overlapping exclusive loans");
            value.exclusive_loan = true;
            value.exclusive_loan_id = op.loan_id.value_or(site_id);
            return;
        case Operation::Kind::ReleaseShared: {
            if (!value.shared_loans) violation(op.place, "unmatched shared loan release");
            if (!op.loan_id && value.shared_loan_ids.size() != 1)
                violation(op.place, "shared loan release requires unambiguous provenance");
            const auto to_release = op.loan_id.value_or(*value.shared_loan_ids.begin());
            if (!value.shared_loan_ids.erase(to_release))
                violation(op.place, "shared loan release has foreign provenance");
            --value.shared_loans;
            return;
        }
        case Operation::Kind::ReleaseExclusive:
            if (!value.exclusive_loan) violation(op.place, "unmatched exclusive loan release");
            if (op.loan_id && op.loan_id != value.exclusive_loan_id)
                violation(op.place, "exclusive loan release has foreign provenance");
            value.exclusive_loan = false;
            value.exclusive_loan_id.reset();
            return;
        case Operation::Kind::ReadObject:
        case Operation::Kind::MoveObject:
            break;
    }
    violation(op.place, "invalid CFG operation");
}

inline bool join_into(State& target, const State& incoming) {
    if (target.size() != incoming.size())
        throw std::runtime_error("ownership violation: inconsistent CFG place sets");
    bool changed = false;
    for (auto& pair : target) {
        const auto other = incoming.find(pair.first);
        if (other == incoming.end())
            violation(pair.first, "missing CFG predecessor place");
        // Do not invent a released loan by joining distinct loan histories.
        // A future provenance-aware pass may make this more precise.
        if (pair.second.shared_loans != other->second.shared_loans ||
                pair.second.exclusive_loan != other->second.exclusive_loan ||
                pair.second.shared_loan_ids != other->second.shared_loan_ids ||
                pair.second.exclusive_loan_id != other->second.exclusive_loan_id)
            violation(pair.first, "different live loan provenance at CFG join");
        ValueMask merged = static_cast<ValueMask>(pair.second.possible | other->second.possible);
        if (merged != pair.second.possible) {
            pair.second.possible = merged;
            changed = true;
        }
    }
    return changed;
}

inline Analysis verify(const Graph& graph) {
    if (graph.blocks.empty())
        throw std::runtime_error("ownership violation: empty CFG");
    // Each lexical borrow must own a globally unique provenance token,
    // including across mutually exclusive CFG predecessors. Otherwise two
    // different borrows with the same count and token could spuriously join,
    // allowing a single release to discharge the wrong branch's loan.
    // The implicit ID is a stable encoding of the borrow's CFG site.
    std::map<std::uint64_t, std::pair<std::size_t, std::size_t>> borrow_sites;
    for (std::size_t block_id = 0; block_id < graph.blocks.size(); ++block_id) {
        const auto& block = graph.blocks[block_id];
        // Malformed successors are invalid even in presently unreachable
        // blocks: optimizer or lowering changes must not expose bad edges.
        for (const auto next : block.successors)
            if (next >= graph.blocks.size())
                violation(std::to_string(block_id), "CFG edge outside graph");
        for (std::size_t op_id = 0; op_id < block.operations.size(); ++op_id) {
            const auto& op = block.operations[op_id];
            if (op.kind != Operation::Kind::BorrowShared &&
                    op.kind != Operation::Kind::BorrowExclusive) continue;
            if (block_id >= UINT32_MAX || op_id >= UINT32_MAX)
                violation(op.place, "CFG too large for loan identity");
            const auto derived = ((static_cast<std::uint64_t>(block_id) + 1) << 32) |
                    (static_cast<std::uint64_t>(op_id) + 1);
            const auto token = op.loan_id.value_or(derived);
            if (!token) violation(op.place, "entry loan identity cannot be issued");
            if (!borrow_sites.emplace(token, std::make_pair(block_id, op_id)).second)
                violation(op.place, "duplicate lexical loan identity");
        }
    }
    // Reject malformed initial abstract states before any CFG edge is
    // processed. Unknown bits and contradictory live-loan provenance
    // must not silently enter the fixed-point lattice.
    State entry = graph.entry;
    validate_disjoint_place_layout(entry);
    for (auto& [name, place] : entry) {
        if (!place.possible || (place.possible &
                static_cast<ValueMask>(~(kOwned | kMoved | kUninitialized | kDropped))))
            violation(name, "invalid CFG entry ownership mask");
        if (place.exclusive_loan && place.shared_loans)
            violation(name, "conflicting initial borrow states");
        // A live loan must refer to a definitely owned place, not a
        // possibly moved/dropped/uninitialized predecessor. Otherwise a
        // matching Release can erase the impossible loan and admit a path.
        if ((place.exclusive_loan || place.shared_loans) &&
                place.possible != kOwned)
            violation(name, "live loan on non-owned CFG entry place");
        if (place.shared_loans > 65535)
            violation(name, "shared loan count exceeds bound");
        // Backwards-compatible entry loan identity: an entry with one live
        // loan can be named unambiguously; multiple anonymous entry loans
        // would make release ownership impossible to prove.
        if (place.shared_loans == 1 && place.shared_loan_ids.empty())
            place.shared_loan_ids.insert(0);
        if (place.exclusive_loan && !place.exclusive_loan_id)
            place.exclusive_loan_id = 0;
        if (place.shared_loans != place.shared_loan_ids.size() ||
                place.exclusive_loan != place.exclusive_loan_id.has_value())
            violation(name, "unproven initial loan identity");
    }
    // Entry identities are just as authoritative as lexical CFG borrow
    // sites. A borrowed parameter and a local borrow must not reuse an ID
    // (even on disjoint places); two independently borrowed entry places
    // also cannot claim the same nonzero origin. Zero is reserved only for
    // the backward-compatible anonymous single-loan entry state.
    std::map<std::uint64_t, std::string> entry_loan_owners;
    // Zero is an anonymous *single entry loan*, not a globally reusable
    // identity. Multiple parameter loans need distinct explicit tokens.
    bool anonymous_entry_claimed = false;
    for (const auto& entry_item : entry) {
        // C++17: capture ordinary references, not structured bindings, in
        // the nested provenance validator lambda.
        const auto& name = entry_item.first;
        const auto& value = entry_item.second;
        if (value.shared_loans > 1 && value.shared_loan_ids.count(0))
            violation(name, "anonymous entry loan mixed with named loans");
        if (value.shared_loan_ids.count(0) ||
                (value.exclusive_loan_id && *value.exclusive_loan_id == 0)) {
            if (anonymous_entry_claimed)
                violation(name, "duplicate anonymous entry loan identity");
            anonymous_entry_claimed = true;
        }
        auto record_entry_token = [&](std::uint64_t token) {
            if (!token) return;
            if (borrow_sites.count(token) ||
                    !entry_loan_owners.emplace(token, name).second)
                violation(name, "duplicate entry or lexical loan identity");
        };
        for (const auto token : value.shared_loan_ids)
            record_entry_token(token);
        if (value.exclusive_loan_id)
            record_entry_token(*value.exclusive_loan_id);
    }
    Analysis result;
    result.incoming.resize(graph.blocks.size());
    result.outgoing.resize(graph.blocks.size());
    result.incoming[0] = entry;
    std::deque<std::size_t> queue{0};
    std::vector<bool> queued(graph.blocks.size(), false);
    queued[0] = true;
    while (!queue.empty()) {
        const auto index = queue.front();
        queue.pop_front();
        queued[index] = false;
        State current = *result.incoming[index];
        const BasicBlock& block = graph.blocks[index];
        for (std::size_t site = 0; site < block.operations.size(); ++site) {
            // Reserve zero for an explicitly known loan at entry. Source
            // position gives stable IDs across fixed-point reprocessing.
            if (index >= UINT32_MAX || site >= UINT32_MAX)
                throw std::runtime_error("ownership violation: CFG too large for loan identity");
            const auto site_id = ((static_cast<std::uint64_t>(index) + 1) << 32)
                    | (static_cast<std::uint64_t>(site) + 1);
            apply(block.operations[site], current, site_id);
        }
        if (block.successors.empty())
            for (const auto& item : current)
                if (item.second.shared_loans || item.second.exclusive_loan)
                    violation(item.first, "loan escapes CFG exit");
        result.outgoing[index] = current;
        for (std::size_t next : block.successors) {
            if (next >= graph.blocks.size())
                throw std::runtime_error("ownership violation: CFG edge outside graph");
            bool changed = false;
            if (!result.incoming[next]) {
                result.incoming[next] = current;
                changed = true;
            } else {
                changed = join_into(*result.incoming[next], current);
            }
            if (changed && !queued[next]) {
                queue.push_back(next);
                queued[next] = true;
            }
        }
    }
    return result;
}

} // namespace oreslang::ownership
