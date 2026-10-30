#pragma once

#include "wsi/json.hpp"

#include <string>
#include <vector>

namespace wsi {

enum class ArgKind { Any, Integer, Number, String, Boolean, Array, Object, Enum };

/// One action argument. The kind is the JSON Schema the model sees, and the
/// check that runs before the method.
struct ArgSpec {
    std::string name;
    ArgKind kind = ArgKind::Any;
    bool required = true;
    std::vector<Json> enum_values;
    ArgKind item_kind = ArgKind::Any;
    std::vector<Json> item_enum;
};

inline ArgSpec any(std::string name, bool required = true) {
    ArgSpec spec;
    spec.name = std::move(name);
    spec.kind = ArgKind::Any;
    spec.required = required;
    return spec;
}

inline ArgSpec integer(std::string name, bool required = true) {
    ArgSpec spec;
    spec.name = std::move(name);
    spec.kind = ArgKind::Integer;
    spec.required = required;
    return spec;
}

inline ArgSpec number(std::string name, bool required = true) {
    ArgSpec spec;
    spec.name = std::move(name);
    spec.kind = ArgKind::Number;
    spec.required = required;
    return spec;
}

inline ArgSpec string(std::string name, bool required = true) {
    ArgSpec spec;
    spec.name = std::move(name);
    spec.kind = ArgKind::String;
    spec.required = required;
    return spec;
}

inline ArgSpec boolean(std::string name, bool required = true) {
    ArgSpec spec;
    spec.name = std::move(name);
    spec.kind = ArgKind::Boolean;
    spec.required = required;
    return spec;
}

inline ArgSpec object(std::string name, bool required = true) {
    ArgSpec spec;
    spec.name = std::move(name);
    spec.kind = ArgKind::Object;
    spec.required = required;
    return spec;
}

inline ArgSpec array_of(std::string name, ArgKind item_kind, bool required = true) {
    ArgSpec spec;
    spec.name = std::move(name);
    spec.kind = ArgKind::Array;
    spec.item_kind = item_kind;
    spec.required = required;
    return spec;
}

inline ArgSpec enumeration(std::string name, std::vector<std::string> values, bool required = true) {
    ArgSpec spec;
    spec.name = std::move(name);
    spec.kind = ArgKind::Enum;
    spec.required = required;
    spec.enum_values.reserve(values.size());
    for (auto& value : values) {
        spec.enum_values.emplace_back(std::move(value));
    }
    return spec;
}

inline ArgSpec optional(ArgSpec spec) {
    spec.required = false;
    return spec;
}

namespace detail {

std::string check_args(const Json& args, const std::vector<ArgSpec>& specs);
Json input_schema(const std::vector<ArgSpec>& specs);
Json tool_descriptor(const std::string& name, const std::string& description, const std::vector<ArgSpec>& specs);

}  // namespace detail
}  // namespace wsi
