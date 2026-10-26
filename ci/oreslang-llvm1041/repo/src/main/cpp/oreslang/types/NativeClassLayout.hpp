#pragma once
#include "oreslang/frontend/Parser.hpp"
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace oreslang {
// A source place is a nominal path, not a host pointer or a scalar handle.
inline std::pair<std::string, std::string> native_place(const Expression& expression) {
    if (expression.kind == Expression::Kind::Variable) return {expression.name, {}};
    if (expression.kind != Expression::Kind::Field || !expression.left)
        throw std::runtime_error("native projection requires a named owner");
    auto place = native_place(*expression.left);
    place.second += (place.second.empty() ? "" : ".") + expression.name;
    return place;
}
inline std::string native_place_key(const Expression& expression) {
    const auto place = native_place(expression);
    return place.first + (place.second.empty() ? "" : "." + place.second);
}
struct NativeClassLeaf {
    std::string path;
    const ClassField* field = nullptr;
    // Every edge retains its declaring class and visibility, including parents.
    std::vector<std::pair<std::string, const ClassField*>> access;
};
struct NativeClassLayout {
    std::vector<NativeClassLeaf> leaves;
    std::unordered_map<std::string, std::string> subobjects;
    std::unordered_map<std::string, std::size_t> offsets;
    bool owns_arrays = false;
    unsigned depth = 1;
};
class NativeClassLayouts {
    std::unordered_map<std::string, const ClassDecl*> classes_;
    std::unordered_map<std::string, NativeClassLayout> layouts_;
    std::unordered_set<std::string> visiting_;
    const NativeClassLayout& build(const std::string& name, unsigned depth) {
        if (depth > 8) throw std::runtime_error("native nested class depth exceeds 8");
        if (visiting_.count(name)) throw std::runtime_error("native owned class layout cycle");
        if (layouts_.count(name)) return layouts_.at(name);
        if (!classes_.count(name)) throw std::runtime_error("unknown nested class type '" + name + "'");
        visiting_.insert(name);
        NativeClassLayout layout;
        for (const auto& field : classes_.at(name)->fields) {
            if (field.class_name.empty()) {
                layout.leaves.push_back({field.name, &field, {{name, &field}}});
                layout.owns_arrays |= field.int_array;
            } else {
                if (!field.final_field || !field.constructor_argument)
                    throw std::runtime_error("nested owned class field requires a fixed declaration-only slot");
                const auto& child = build(field.class_name, depth + 1);
                layout.depth = std::max(layout.depth, child.depth + 1);
                if (layout.depth > 8) throw std::runtime_error("native nested class depth exceeds 8");
                if (child.leaves.empty()) throw std::runtime_error("nested fieldless class identity requires distinct storage");
                for (const auto& leaf : child.leaves) if (!leaf.field->final_field)
                    throw std::runtime_error("nested class mutable field requires projected writeback lowering");
                layout.subobjects.emplace(field.name, field.class_name);
                layout.offsets.emplace(field.name, layout.leaves.size());
                for (const auto& sub : child.subobjects) {
                    layout.subobjects.emplace(field.name + "." + sub.first, sub.second);
                    layout.offsets.emplace(field.name + "." + sub.first, layout.leaves.size() + child.offsets.at(sub.first));
                }
                for (const auto& leaf : child.leaves) {
                    auto expanded = leaf; expanded.path = field.name + "." + leaf.path;
                    expanded.access.insert(expanded.access.begin(), {name, &field});
                    layout.leaves.push_back(std::move(expanded));
                }
                layout.owns_arrays |= child.owns_arrays;
            }
            if (layout.leaves.size() > 16) throw std::runtime_error("native expanded class field limit exceeds 16");
        }
        visiting_.erase(name);
        return layouts_.emplace(name, std::move(layout)).first->second;
    }
public:
    explicit NativeClassLayouts(const Program& program) {
        for (const auto& cls : program.classes) classes_.emplace(cls.name, &cls);
        for (const auto& cls : program.classes) build(cls.name, 1);
    }
    const NativeClassLayout& at(const std::string& name) const {
        const auto found = layouts_.find(name);
        if (found == layouts_.end())
            throw std::runtime_error("unknown native class type '" + name + "'");
        return found->second;
    }
    const ClassDecl& source(const std::string& name) const { return *classes_.at(name); }
    std::unordered_set<std::string> mutable_paths(const std::string& name) const {
        std::unordered_set<std::string> result;
        for (const auto& leaf : at(name).leaves) {
            bool mutable_parent = true; std::string prefix;
            for (const auto& edge : leaf.access) {
                prefix += (prefix.empty() ? "" : ".") + edge.second->name;
                const bool reference = !edge.second->class_name.empty() || edge.second->int_array;
                if (mutable_parent && (reference ? edge.second->mutable_referent : !edge.second->final_field)) result.insert(prefix);
                mutable_parent &= edge.second->mutable_referent;
            }
        }
        return result;
    }
    std::unordered_set<std::string> accessible(const std::string& name, const std::string& method_owner) const {
        std::unordered_set<std::string> result;
        for (const auto& leaf : at(name).leaves) {
            bool visible = true; std::string prefix;
            for (const auto& edge : leaf.access) {
                prefix += (prefix.empty() ? "" : ".") + edge.second->name;
                visible &= edge.second->public_member || edge.first == method_owner;
                if (visible) result.insert(prefix);
            }
        }
        return result;
    }
};
}
