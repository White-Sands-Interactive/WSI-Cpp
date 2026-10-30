#include "wsi/schema.hpp"

#include <sstream>

namespace wsi {
namespace {

std::string type_name(const Json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "bool";
    if (value.is_number_integer()) return "int";
    if (value.is_number()) return "float";
    if (value.is_string()) return "str";
    if (value.is_array()) return "list";
    if (value.is_object()) return "dict";
    return "value";
}

std::string render_choice(const Json& value) {
    if (value.is_string()) {
        std::string out = "'";
        for (const char ch : value.get<std::string>()) {
            if (ch == '\\' || ch == '\'') out.push_back('\\');
            out.push_back(ch);
        }
        out.push_back('\'');
        return out;
    }
    if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
    if (value.is_null()) return "None";
    return value.dump();
}

std::string render_choices(const std::vector<Json>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ", ";
        out += render_choice(values[i]);
    }
    return out;
}

bool all_of(const std::vector<Json>& values, bool (*pred)(const Json&)) {
    if (values.empty()) return false;
    for (const auto& value : values) {
        if (!pred(value)) return false;
    }
    return true;
}

Json property_schema(const ArgSpec& spec) {
    switch (spec.kind) {
        case ArgKind::Any:
            return Json::object();
        case ArgKind::Integer:
            return Json{{"type", "integer"}};
        case ArgKind::Number:
            return Json{{"type", "number"}};
        case ArgKind::String:
            return Json{{"type", "string"}};
        case ArgKind::Boolean:
            return Json{{"type", "boolean"}};
        case ArgKind::Object:
            return Json{{"type", "object"}};
        case ArgKind::Array: {
            Json schema = Json{{"type", "array"}};
            if (spec.item_kind != ArgKind::Any || !spec.item_enum.empty()) {
                ArgSpec item;
                item.kind = spec.item_kind == ArgKind::Any && !spec.item_enum.empty() ? ArgKind::Enum : spec.item_kind;
                item.enum_values = spec.item_enum;
                if (item.kind != ArgKind::Any) schema["items"] = property_schema(item);
            }
            return schema;
        }
        case ArgKind::Enum: {
            Json schema = Json{{"enum", spec.enum_values}};
            if (all_of(spec.enum_values, [](const Json& value) { return value.is_string(); })) {
                schema["type"] = "string";
            } else if (all_of(spec.enum_values, [](const Json& value) { return value.is_boolean(); })) {
                schema["type"] = "boolean";
            } else if (all_of(spec.enum_values, [](const Json& value) {
                           return value.is_number_integer() && !value.is_boolean();
                       })) {
                schema["type"] = "integer";
            }
            return schema;
        }
    }
    return Json::object();
}

std::string check_one(const std::string& path, const Json& value, const ArgSpec& spec) {
    switch (spec.kind) {
        case ArgKind::Any:
            return {};
        case ArgKind::Integer:
            if (value.is_boolean() || !value.is_number_integer()) {
                return path + ": expected integer, received " + type_name(value);
            }
            return {};
        case ArgKind::Number:
            if (value.is_boolean() || !value.is_number()) {
                return path + ": expected number, received " + type_name(value);
            }
            return {};
        case ArgKind::String:
            if (!value.is_string()) return path + ": expected string, received " + type_name(value);
            return {};
        case ArgKind::Boolean:
            if (!value.is_boolean()) return path + ": expected boolean, received " + type_name(value);
            return {};
        case ArgKind::Object:
            if (!value.is_object()) return path + ": expected object, received " + type_name(value);
            return {};
        case ArgKind::Enum: {
            for (const auto& choice : spec.enum_values) {
                if (choice == value) return {};
            }
            return path + ": expected one of " + render_choices(spec.enum_values);
        }
        case ArgKind::Array: {
            if (!value.is_array()) return path + ": expected array, received " + type_name(value);
            if (spec.item_kind == ArgKind::Any && spec.item_enum.empty()) return {};
            ArgSpec item;
            item.kind = spec.item_kind == ArgKind::Any && !spec.item_enum.empty() ? ArgKind::Enum : spec.item_kind;
            item.enum_values = spec.item_enum;
            std::string messages;
            std::size_t index = 0;
            for (const auto& entry : value) {
                auto message = check_one(path + "." + std::to_string(index), entry, item);
                if (!message.empty()) {
                    if (!messages.empty()) messages += "; ";
                    messages += message;
                }
                ++index;
            }
            return messages;
        }
    }
    return {};
}

std::string join_messages(const std::vector<std::string>& messages) {
    std::ostringstream out;
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) out << "; ";
        out << messages[i];
    }
    return out.str();
}

}  // namespace

namespace detail {

std::string check_args(const Json& args, const std::vector<ArgSpec>& specs) {
    const Json object = args.is_object() ? args : Json::object();
    std::vector<std::string> messages;
    for (const auto& spec : specs) {
        const bool present = object.contains(spec.name);
        if (!present || object.at(spec.name).is_null()) {
            if (!spec.required) continue;
            if (!present) {
                messages.push_back(spec.name + ": required");
                continue;
            }
        }
        auto message = check_one(spec.name, object.at(spec.name), spec);
        if (!message.empty()) messages.push_back(std::move(message));
    }
    return join_messages(messages);
}

Json input_schema(const std::vector<ArgSpec>& specs) {
    Json properties = Json::object();
    Json required = Json::array();
    for (const auto& spec : specs) {
        properties[spec.name] = property_schema(spec);
        if (spec.required) required.push_back(spec.name);
    }
    Json schema = Json{{"type", "object"}, {"properties", std::move(properties)}};
    if (!required.empty()) schema["required"] = std::move(required);
    return schema;
}

Json tool_descriptor(const std::string& name, const std::string& description, const std::vector<ArgSpec>& specs) {
    return Json{{"name", name}, {"description", description}, {"inputSchema", input_schema(specs)}};
}

}  // namespace detail
}  // namespace wsi
