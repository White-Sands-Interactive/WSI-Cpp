#include "wsi/player.hpp"

#include <algorithm>
#include <stdexcept>

namespace wsi {

ReflectedPlayer Player::reflect() {
    const Bound* see = nullptr;
    for (const auto& bound : bound_) {
        if (bound.name == "see") see = &bound;
    }
    if (see == nullptr) throw std::runtime_error("Player must implement a public see() method");

    for (const auto& [name, description] : declared_) {
        (void)description;
        const bool published = std::any_of(bound_.begin(), bound_.end(), [&](const Bound& bound) {
            return bound.name == name && name != "see";
        });
        if (!published) {
            throw std::runtime_error("Declared action \"" + name + "\" is not a public method on " + type_name());
        }
    }

    ReflectedPlayer reflected;
    if (!description_.empty()) reflected.description = description_;

    auto publish = [](const Bound& bound) {
        ReflectedAction action;
        action.name = bound.name;
        action.description = bound.name == "see" ? std::string(kSeeDescription) : bound.description;
        action.descriptor = detail::tool_descriptor(action.name, action.description, bound.specs);
        action.invoke = bound.invoke;
        return action;
    };

    reflected.see = publish(*see);
    std::vector<const Bound*> actions;
    for (const auto& bound : bound_) {
        if (bound.name != "see") actions.push_back(&bound);
    }
    std::sort(actions.begin(), actions.end(), [](const Bound* left, const Bound* right) { return left->name < right->name; });
    reflected.tools = Json::array();
    reflected.tools.push_back(reflected.see.descriptor);
    for (const Bound* action : actions) {
        reflected.actions.push_back(publish(*action));
        reflected.tools.push_back(reflected.actions.back().descriptor);
    }
    reflected.tools.push_back(detail::tool_descriptor("wait", kWaitDescription, {}));
    return reflected;
}

}  // namespace wsi
