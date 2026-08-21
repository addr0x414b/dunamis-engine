#include "type_registry.h"

#include "camera.h"
#include "directional_light.h"
#include "point_light.h"

const TypeDescriptor* TypeRegistry::find(const std::string& name) const {
    const auto found = byName_.find(name);
    return found == byName_.end() ? nullptr : &found->second;
}

const TypeDescriptor* TypeRegistry::find(const GameObject& object) const {
    const auto found = byCppType_.find(typeid(object));
    return found == byCppType_.end() ? nullptr : find(found->second);
}

TypeDescriptor* TypeRegistry::findMutable(const std::string& name) {
    const auto found = byName_.find(name);
    return found == byName_.end() ? nullptr : &found->second;
}

const PropertyDescriptor* TypeRegistry::findOwnProperty(
    const TypeDescriptor& type, const std::string& name) {
    for (const PropertyDescriptor& property : type.properties) {
        if (property.name == name) return &property;
    }
    return nullptr;
}

const PropertyDescriptor* TypeRegistry::findProperty(
    const TypeDescriptor& type, const std::string& name) const {
    if (const PropertyDescriptor* own = findOwnProperty(type, name)) return own;
    const TypeDescriptor* current = &type;
    while (!current->parentName.empty()) {
        current = find(current->parentName);
        if (!current) return nullptr;
        if (const PropertyDescriptor* inherited = findOwnProperty(*current, name)) {
            return inherited;
        }
    }
    return nullptr;
}

void TypeRegistry::appendProperties(
    const TypeDescriptor& type,
    std::vector<const PropertyDescriptor*>& out,
    bool includeRuntimeTransferOnly) const {
    if (!type.parentName.empty()) {
        if (const TypeDescriptor* parent = find(type.parentName)) {
            appendProperties(*parent, out, includeRuntimeTransferOnly);
        }
    }
    for (const PropertyDescriptor& property : type.properties) {
        if (property.lifecycle == PropertyLifecycle::Persisted ||
            (includeRuntimeTransferOnly &&
             property.lifecycle == PropertyLifecycle::RuntimeTransferOnly)) {
            out.push_back(&property);
        }
    }
}

std::vector<const PropertyDescriptor*> TypeRegistry::persistedProperties(
    const TypeDescriptor& type) const {
    std::vector<const PropertyDescriptor*> properties;
    appendProperties(type, properties, false);
    return properties;
}

std::vector<const PropertyDescriptor*> TypeRegistry::runtimeTransferProperties(
    const TypeDescriptor& type) const {
    std::vector<const PropertyDescriptor*> properties;
    appendProperties(type, properties, true);
    return properties;
}

bool TypeRegistry::isA(const TypeDescriptor& type,
                       const std::string& baseName) const {
    const TypeDescriptor* current = &type;
    while (current) {
        if (current->name == baseName) return true;
        current = current->parentName.empty() ? nullptr : find(current->parentName);
    }
    return false;
}

Result registerEngineTypes(TypeRegistry& registry) {
    Result result = registry.registerType<GameObject>(
        "GameObject", {}, [] { return std::make_unique<GameObject>(); });
    if (!result) return result;
    result = registry.registerProperty("GameObject", "name", &GameObject::name);
    if (!result) return result;
    result = registry.registerProperty("GameObject", "position", &GameObject::position);
    if (!result) return result;
    result = registry.registerProperty("GameObject", "rotation", &GameObject::rotation);
    if (!result) return result;
    result = registry.registerProperty("GameObject", "scale", &GameObject::scale);
    if (!result) return result;
    result = registry.registerProperty(
        "GameObject", "physics", &GameObject::physics,
        PropertyLifecycle::RuntimeTransferOnly);
    if (!result) return result;
    result = registry.registerAccessor<GameObject, std::string>(
        "GameObject", "texturePath",
        [](const GameObject& object) { return object.authoredTexturePath(); },
        [](GameObject& object, const std::string& path) {
            return object.setAuthoredTexturePath(path);
        });
    if (!result) return result;
    result = registry.registerAccessor<GameObject, std::string>(
        "GameObject", "modelPath",
        [](const GameObject& object) { return object.authoredModelPath(); },
        [](GameObject& object, const std::string& path) {
            return object.setAuthoredModelPath(path);
        });
    if (!result) return result;

    result = registry.registerType<Camera>(
        "Camera", "GameObject", [] { return std::make_unique<Camera>(); });
    if (!result) return result;
    result = registry.registerProperty("Camera", "front", &Camera::front);
    if (!result) return result;
    result = registry.registerProperty("Camera", "up", &Camera::up);
    if (!result) return result;
    result = registry.registerAccessor<Camera, float>(
        "Camera", "fov",
        [](const Camera& camera) { return camera.fov(); },
        [](Camera& camera, const float value) {
            return camera.setFov(value)
                ? Result::success()
                : Result::failure(
                      "Camera FOV must be finite and between 0 and 180 degrees");
        },
        PropertyLifecycle::RuntimeTransferOnly);
    if (!result) return result;

    result = registry.registerType<PointLight>(
        "PointLight", "GameObject", [] { return std::make_unique<PointLight>(); });
    if (!result) return result;
    result = registry.registerProperty("PointLight", "color", &PointLight::color);
    if (!result) return result;
    result = registry.registerProperty("PointLight", "intensity", &PointLight::intensity);
    if (!result) return result;

    result = registry.registerType<DirectionalLight>(
        "DirectionalLight", "GameObject",
        [] { return std::make_unique<DirectionalLight>(); });
    if (!result) return result;
    result = registry.registerProperty("DirectionalLight", "color", &DirectionalLight::color);
    if (!result) return result;
    result = registry.registerProperty("DirectionalLight", "intensity", &DirectionalLight::intensity);
    if (!result) return result;
    result = registry.registerAccessor<DirectionalLight, glm::vec3>(
        "DirectionalLight", "shadowFocus",
        [](const DirectionalLight& light) { return light.shadow.focus; },
        [](DirectionalLight& light, const glm::vec3& value) {
            light.shadow.focus = value; return Result::success();
        });
    if (!result) return result;
#define DUNAMIS_SHADOW_PROPERTY(jsonName, memberName) \
    result = registry.registerAccessor<DirectionalLight, float>( \
        "DirectionalLight", jsonName, \
        [](const DirectionalLight& light) { return light.shadow.memberName; }, \
        [](DirectionalLight& light, const float value) { \
            light.shadow.memberName = value; return Result::success(); \
        }); \
    if (!result) return result
    DUNAMIS_SHADOW_PROPERTY("shadowHalfExtent", halfExtent);
    DUNAMIS_SHADOW_PROPERTY("shadowLightDistance", lightDistance);
    DUNAMIS_SHADOW_PROPERTY("shadowNearPlane", nearPlane);
    DUNAMIS_SHADOW_PROPERTY("shadowFarPlane", farPlane);
#undef DUNAMIS_SHADOW_PROPERTY
    return Result::success();
}
