#ifndef TYPE_REGISTRY_H
#define TYPE_REGISTRY_H

#include <cmath>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <type_traits>
#include <typeindex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include "../core/result.h"
#include "game_object.h"

namespace authored_property {

template <typename T>
inline constexpr bool is_supported_v =
    std::is_same_v<T, bool> || std::is_same_v<T, std::string> ||
    std::is_integral_v<T> || std::is_floating_point_v<T> ||
    std::is_same_v<T, glm::vec2> || std::is_same_v<T, glm::vec3> ||
    std::is_same_v<T, glm::vec4>;

template <typename T>
Result encode(const T& value, nlohmann::json& output) {
    if constexpr (std::is_same_v<T, bool> || std::is_same_v<T, std::string>) {
        output = value;
        return Result::success();
    } else if constexpr (std::is_integral_v<T>) {
        output = value;
        return Result::success();
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!std::isfinite(value)) {
            return Result::failure("expected a finite number");
        }
        output = value;
        return Result::success();
    } else if constexpr (std::is_same_v<T, glm::vec2> ||
                         std::is_same_v<T, glm::vec3> ||
                         std::is_same_v<T, glm::vec4>) {
        output = nlohmann::json::array();
        for (glm::length_t i = 0; i < value.length(); ++i) {
            if (!std::isfinite(value[i])) {
                return Result::failure("expected finite vector components");
            }
            output.push_back(value[i]);
        }
        return Result::success();
    } else {
        static_assert(!sizeof(T), "Unsupported authored property type");
    }
}

template <typename T>
Result decode(const nlohmann::json& input, T& value) {
    try {
        if constexpr (std::is_same_v<T, bool>) {
            if (!input.is_boolean()) return Result::failure("expected bool");
            value = input.get<bool>();
        } else if constexpr (std::is_same_v<T, std::string>) {
            if (!input.is_string()) return Result::failure("expected string");
            value = input.get<std::string>();
        } else if constexpr (std::is_integral_v<T> && std::is_unsigned_v<T>) {
            if (!input.is_number_unsigned()) {
                return Result::failure("expected unsigned integer");
            }
            const auto decoded = input.get<std::uint64_t>();
            if (decoded > static_cast<std::uint64_t>(
                              std::numeric_limits<T>::max())) {
                return Result::failure("unsigned integer is out of range");
            }
            value = static_cast<T>(decoded);
        } else if constexpr (std::is_integral_v<T>) {
            if (!input.is_number_integer()) {
                return Result::failure("expected integer");
            }
            const auto decoded = input.get<std::int64_t>();
            if (decoded < static_cast<std::int64_t>(
                              std::numeric_limits<T>::min()) ||
                decoded > static_cast<std::int64_t>(
                              std::numeric_limits<T>::max())) {
                return Result::failure("integer is out of range");
            }
            value = static_cast<T>(decoded);
        } else if constexpr (std::is_floating_point_v<T>) {
            if (!input.is_number()) return Result::failure("expected number");
            const double decoded = input.get<double>();
            if (!std::isfinite(decoded) ||
                decoded < -static_cast<double>(std::numeric_limits<T>::max()) ||
                decoded > static_cast<double>(std::numeric_limits<T>::max())) {
                return Result::failure("expected a finite in-range number");
            }
            value = static_cast<T>(decoded);
        } else if constexpr (std::is_same_v<T, glm::vec2> ||
                             std::is_same_v<T, glm::vec3> ||
                             std::is_same_v<T, glm::vec4>) {
            T decoded{};
            if (!input.is_array() || input.size() != decoded.length()) {
                return Result::failure("expected a vector array of the correct size");
            }
            for (glm::length_t i = 0; i < decoded.length(); ++i) {
                Result result = decode(input.at(i), decoded[i]);
                if (!result) return result;
            }
            value = decoded;
        } else {
            static_assert(!sizeof(T), "Unsupported authored property type");
        }
    } catch (const std::exception& exception) {
        return Result::failure(exception.what());
    }
    return Result::success();
}

}  // namespace authored_property

enum class PropertyLifecycle {
    Persisted,
    RuntimeTransferOnly,
    Transient,
};

struct PropertyDescriptor {
    std::string name;
    PropertyLifecycle lifecycle = PropertyLifecycle::Persisted;
    std::function<Result(const GameObject&, nlohmann::json&)> read;
    std::function<Result(GameObject&, const nlohmann::json&)> write;
    std::function<Result(const GameObject&, GameObject&)> copy;
};

struct TypeDescriptor {
    TypeDescriptor(std::string stableName, std::type_index cppType)
        : name(std::move(stableName)), type(cppType) {}

    std::string name;
    std::string parentName;
    std::type_index type;
    std::function<std::unique_ptr<GameObject>()> factory;
    std::vector<PropertyDescriptor> properties;
};

class TypeRegistry {
public:
    template <typename T>
    Result registerType(
        std::string name, std::string parentName = {},
        std::function<std::unique_ptr<GameObject>()> factory = {}) {
        static_assert(std::is_base_of_v<GameObject, T>,
                      "Registered types must derive from GameObject");
        if (name.empty()) return Result::failure("Type name cannot be empty");
        if (byName_.count(name) != 0 || byCppType_.count(typeid(T)) != 0) {
            return Result::failure("Duplicate registered type '" + name + "'");
        }
        if (!parentName.empty() && byName_.count(parentName) == 0) {
            return Result::failure("Unknown parent type '" + parentName + "'");
        }
        TypeDescriptor descriptor(std::move(name), typeid(T));
        descriptor.parentName = std::move(parentName);
        descriptor.factory = std::move(factory);
        const std::string stableName = descriptor.name;
        byCppType_.emplace(typeid(T), stableName);
        byName_.emplace(stableName, std::move(descriptor));
        return Result::success();
    }

    template <typename Owner, typename Value>
    Result registerProperty(const std::string& typeName, std::string name,
                            Value Owner::*member,
                            PropertyLifecycle lifecycle =
                                PropertyLifecycle::Persisted) {
        return registerAccessor<Owner, Value>(
            typeName, std::move(name),
            [member](const Owner& object) { return object.*member; },
            [member](Owner& object, const Value& value) {
                object.*member = value;
                return Result::success();
            }, lifecycle);
    }

    template <typename Owner, typename Value, typename Getter, typename Setter>
    Result registerAccessor(const std::string& typeName, std::string name,
                            Getter getter, Setter setter,
                            PropertyLifecycle lifecycle =
                                PropertyLifecycle::Persisted) {
        static_assert(std::is_base_of_v<GameObject, Owner>,
                      "Property owners must derive from GameObject");
        TypeDescriptor* descriptor = findMutable(typeName);
        if (!descriptor || descriptor->type != std::type_index(typeid(Owner))) {
            return Result::failure("Property owner does not match registered type '" +
                                   typeName + "'");
        }
        if (name.empty() || findProperty(*descriptor, name)) {
            return Result::failure("Duplicate or empty property '" + name +
                                   "' on type '" + typeName + "'");
        }
        PropertyDescriptor property;
        property.name = std::move(name);
        property.lifecycle = lifecycle;
        if (lifecycle != PropertyLifecycle::Transient) {
            property.copy = [getter, setter](const GameObject& source,
                                             GameObject& destination) {
                Value value = getter(static_cast<const Owner&>(source));
                return setter(static_cast<Owner&>(destination), value);
            };
        }
        if (lifecycle == PropertyLifecycle::Persisted) {
            if constexpr (!authored_property::is_supported_v<Value>) {
                return Result::failure(
                    "Persisted property '" + property.name +
                    "' has no supported JSON codec");
            } else {
                property.read = [getter](const GameObject& base,
                                         nlohmann::json& output) {
                    return authored_property::encode<Value>(
                        getter(static_cast<const Owner&>(base)), output);
                };
                property.write = [setter](GameObject& base,
                                          const nlohmann::json& input) {
                    Value value{};
                    Result result = authored_property::decode<Value>(input, value);
                    if (!result) return result;
                    return setter(static_cast<Owner&>(base), value);
                };
            }
        }
        descriptor->properties.push_back(std::move(property));
        return Result::success();
    }

    [[nodiscard]] const TypeDescriptor* find(const std::string& name) const;
    [[nodiscard]] const TypeDescriptor* find(const GameObject& object) const;
    [[nodiscard]] const PropertyDescriptor* findProperty(
        const TypeDescriptor& type, const std::string& name) const;
    [[nodiscard]] std::vector<const PropertyDescriptor*> persistedProperties(
        const TypeDescriptor& type) const;
    [[nodiscard]] std::vector<const PropertyDescriptor*> runtimeTransferProperties(
        const TypeDescriptor& type) const;
    [[nodiscard]] bool isA(const TypeDescriptor& type,
                           const std::string& baseName) const;

private:
    [[nodiscard]] TypeDescriptor* findMutable(const std::string& name);
    [[nodiscard]] static const PropertyDescriptor* findOwnProperty(
        const TypeDescriptor& type, const std::string& name);
    void appendProperties(const TypeDescriptor& type,
                          std::vector<const PropertyDescriptor*>& out,
                          bool includeRuntimeTransferOnly) const;

    std::unordered_map<std::string, TypeDescriptor> byName_;
    std::unordered_map<std::type_index, std::string> byCppType_;
};

[[nodiscard]] Result registerEngineTypes(TypeRegistry& registry);

#endif
