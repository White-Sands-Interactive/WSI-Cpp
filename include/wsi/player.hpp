#pragma once

#include "wsi/protocol.hpp"
#include "wsi/schema.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace wsi {

struct ReflectedAction {
    std::string name;
    std::string description;
    Json descriptor;
    std::function<Json(const Json& args)> invoke;
};

struct ReflectedPlayer {
    std::optional<std::string> description;
    ReflectedAction see;
    std::vector<ReflectedAction> actions;
    Json tools = Json::array();
};

namespace detail {

template <typename T>
struct IsOptional : std::false_type {};

template <typename T>
struct IsOptional<std::optional<T>> : std::true_type {
    using value_type = T;
};

template <typename T>
struct IsVector : std::false_type {};

template <typename T, typename A>
struct IsVector<std::vector<T, A>> : std::true_type {
    using value_type = T;
};

template <typename T>
ArgSpec infer_spec(std::string name) {
    using D = std::decay_t<T>;
    if constexpr (IsOptional<D>::value) {
        auto spec = infer_spec<typename IsOptional<D>::value_type>(std::move(name));
        spec.required = false;
        return spec;
    } else if constexpr (IsVector<D>::value) {
        auto item = infer_spec<typename IsVector<D>::value_type>("item");
        ArgSpec spec;
        spec.name = std::move(name);
        spec.kind = ArgKind::Array;
        spec.item_kind = item.kind;
        spec.item_enum = item.enum_values;
        spec.required = true;
        return spec;
    } else if constexpr (std::is_same_v<D, Json>) {
        return any(std::move(name));
    } else if constexpr (std::is_same_v<D, bool>) {
        return boolean(std::move(name));
    } else if constexpr (std::is_integral_v<D>) {
        return integer(std::move(name));
    } else if constexpr (std::is_floating_point_v<D>) {
        return number(std::move(name));
    } else if constexpr (std::is_same_v<D, std::string>) {
        return string(std::move(name));
    } else {
        static_assert(sizeof(D) == 0, "unsupported action argument type");
    }
}

template <typename T>
T convert_value(const Json& value);

template <typename T>
T convert_arg(const Json& args, const ArgSpec& spec) {
    using D = std::decay_t<T>;
    if constexpr (IsOptional<D>::value) {
        if (!args.is_object() || !args.contains(spec.name) || args.at(spec.name).is_null()) {
            return std::nullopt;
        }
        return convert_value<typename IsOptional<D>::value_type>(args.at(spec.name));
    } else {
        return convert_value<D>(args.at(spec.name));
    }
}

template <typename T>
T convert_value(const Json& value) {
    using D = std::decay_t<T>;
    if constexpr (std::is_same_v<D, Json>) {
        return value;
    } else if constexpr (std::is_same_v<D, bool>) {
        return value.get<bool>();
    } else if constexpr (std::is_integral_v<D>) {
        const auto number = value.get<long long>();
        return static_cast<D>(number);
    } else if constexpr (std::is_floating_point_v<D>) {
        return static_cast<D>(value.get<double>());
    } else if constexpr (std::is_same_v<D, std::string>) {
        return value.get<std::string>();
    } else if constexpr (IsVector<D>::value) {
        D out;
        out.reserve(value.size());
        for (const auto& item : value) {
            out.push_back(convert_value<typename IsVector<D>::value_type>(item));
        }
        return out;
    } else if constexpr (IsOptional<D>::value) {
        if (value.is_null()) {
            return std::nullopt;
        }
        return convert_value<typename IsOptional<D>::value_type>(value);
    } else {
        static_assert(sizeof(D) == 0, "unsupported action argument type");
    }
}

template <typename Method>
struct MethodTraits;

template <class T, class R, class... Args>
struct MethodTraits<R (T::*)(Args...)> {
    using class_type = T;
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
    static constexpr std::size_t arity = sizeof...(Args);

    template <std::size_t... I>
    static Json call(T* self, R (T::*method)(Args...), const Json& args, const std::vector<ArgSpec>& specs,
                     std::index_sequence<I...>) {
        if constexpr (std::is_void_v<R>) {
            (self->*method)(convert_arg<std::decay_t<std::tuple_element_t<I, args_tuple>>>(args, specs[I])...);
            return Json(nullptr);
        } else {
            return Json((self->*method)(convert_arg<std::decay_t<std::tuple_element_t<I, args_tuple>>>(args, specs[I])...));
        }
    }
};

template <class T, class R, class... Args>
struct MethodTraits<R (T::*)(Args...) const> {
    using class_type = T;
    using return_type = R;
    using args_tuple = std::tuple<Args...>;
    static constexpr std::size_t arity = sizeof...(Args);

    template <std::size_t... I>
    static Json call(T* self, R (T::*method)(Args...) const, const Json& args, const std::vector<ArgSpec>& specs,
                     std::index_sequence<I...>) {
        if constexpr (std::is_void_v<R>) {
            (self->*method)(convert_arg<std::decay_t<std::tuple_element_t<I, args_tuple>>>(args, specs[I])...);
            return Json(nullptr);
        } else {
            return Json((self->*method)(convert_arg<std::decay_t<std::tuple_element_t<I, args_tuple>>>(args, specs[I])...));
        }
    }
};

template <typename... Ts>
std::array<std::string, sizeof...(Ts)> make_names(Ts&&... values) {
    return {std::string(std::forward<Ts>(values))...};
}

template <typename... Specs>
std::array<ArgSpec, sizeof...(Specs)> make_args(Specs... specs) {
    return {std::move(specs)...};
}

}  // namespace detail

/// Build the argument-name list for bind(). Types come from the method.
template <typename... Ts>
std::array<std::string, sizeof...(Ts)> names(Ts&&... values) {
    return detail::make_names(std::forward<Ts>(values)...);
}

/// Build an explicit argument schema for bind().
template <typename... Specs>
std::array<ArgSpec, sizeof...(Specs)> args(Specs... specs) {
    return detail::make_args(std::move(specs)...);
}

/// A game character. Bind `see` and each action in the constructor.
/// C++ has no runtime method list, so bind() is the tool list the other SDKs
/// build by reflecting public methods. `_` names and `wait` are left off.
class Player {
 public:
    virtual ~Player() = default;

    virtual std::string type_name() const { return "Player"; }

    void set_description(std::string description) { description_ = std::move(description); }

    const std::string& description() const { return description_; }

    /// Record an action name that must exist when the player is published.
    void declare(std::string name, std::string description = {}) {
        declared_.emplace_back(std::move(name), std::move(description));
    }

    template <class Method>
    void bind_see(Method method) {
        using Traits = detail::MethodTraits<Method>;
        static_assert(Traits::arity == 0, "see() takes no arguments");
        bind(method, "see", kSeeDescription, std::array<ArgSpec, 0>{});
    }

    template <class Method>
    void bind(Method method, std::string name, std::string description) {
        using Traits = detail::MethodTraits<Method>;
        static_assert(Traits::arity == 0, "pass argument names or schemas for this action");
        bind(method, std::move(name), std::move(description), std::array<ArgSpec, 0>{});
    }

    template <class Method, std::size_t N>
    void bind(Method method, std::string name, std::string description, const std::array<std::string, N>& arg_names) {
        using Traits = detail::MethodTraits<Method>;
        static_assert(N == Traits::arity, "argument name count must match the method");
        bind(method, std::move(name), std::move(description), infer_names<Traits>(arg_names, std::make_index_sequence<N>{}));
    }

    template <class Method, std::size_t N>
    void bind(Method method, std::string name, std::string description, const std::array<ArgSpec, N>& specs) {
        using Traits = detail::MethodTraits<Method>;
        static_assert(N == Traits::arity, "argument schema count must match the method");
        auto* self = dynamic_cast<typename Traits::class_type*>(this);
        if (self == nullptr) {
            throw std::runtime_error("bind() method is not a member of this player");
        }
        if (name.empty() || name == "wait" || name.front() == '_') {
            throw std::runtime_error("cannot publish action \"" + name + "\"");
        }
        std::vector<ArgSpec> spec_vec(specs.begin(), specs.end());
        Bound bound;
        bound.name = std::move(name);
        bound.description = description.empty() ? ("Action: " + bound.name) : std::move(description);
        bound.specs = spec_vec;
        bound.invoke = [self, method, spec_vec](const Json& raw) -> Json {
            const auto error = detail::check_args(raw, spec_vec);
            if (!error.empty()) {
                throw std::runtime_error(error);
            }
            if (!raw.is_object()) {
                return Traits::call(self, method, Json::object(), spec_vec, std::make_index_sequence<Traits::arity>{});
            }
            return Traits::call(self, method, raw, spec_vec, std::make_index_sequence<Traits::arity>{});
        };
        upsert(std::move(bound));
    }

    /// Register an action from a callable. Scripting hosts use this when the
    /// tool list is data rather than a set of C++ methods. `see` is allowed.
    /// `wait` and names that start with `_` are not.
    void bind_dynamic(std::string name, std::string description, std::vector<ArgSpec> specs,
                      std::function<Json(const Json&)> invoke) {
        if (name.empty() || name == "wait" || name.front() == '_') {
            throw std::runtime_error("cannot publish action \"" + name + "\"");
        }
        if (!invoke) {
            throw std::runtime_error("cannot publish action \"" + name + "\"");
        }
        std::vector<ArgSpec> checked = specs;
        Bound bound;
        bound.name = std::move(name);
        bound.description = description.empty() ? ("Action: " + bound.name) : std::move(description);
        bound.specs = std::move(specs);
        bound.invoke = [checked = std::move(checked), invoke = std::move(invoke)](const Json& raw) -> Json {
            const auto error = detail::check_args(raw, checked);
            if (!error.empty()) {
                throw std::runtime_error(error);
            }
            return invoke(raw.is_object() ? raw : Json::object());
        };
        upsert(std::move(bound));
    }

    ReflectedPlayer reflect();

 private:
    struct Bound {
        std::string name;
        std::string description;
        std::vector<ArgSpec> specs;
        std::function<Json(const Json&)> invoke;
    };

    template <class Traits, std::size_t N, std::size_t... I>
    static std::array<ArgSpec, N> infer_names(const std::array<std::string, N>& arg_names, std::index_sequence<I...>) {
        return {detail::infer_spec<std::tuple_element_t<I, typename Traits::args_tuple>>(arg_names[I])...};
    }

    void upsert(Bound bound) {
        for (auto& existing : bound_) {
            if (existing.name == bound.name) {
                existing = std::move(bound);
                return;
            }
        }
        bound_.push_back(std::move(bound));
    }

    std::string description_;
    std::vector<Bound> bound_;
    std::vector<std::pair<std::string, std::string>> declared_;
};

#define WSI_TYPE_NAME(Type) \
    std::string type_name() const override { return #Type; }

}  // namespace wsi
