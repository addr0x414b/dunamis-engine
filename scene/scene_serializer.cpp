#include "scene_serializer.h"

#include "../assets/model_import_policy.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace {

// Scene versions 1 and 2 stored legacy world distances in Dunamis units,
// where 100 units represented one meter. Keep this conversion exclusively in
// the versioned document migration; runtime systems use the current meter
// contract and must not carry legacy '/100' conversions.
constexpr float legacyDunamisUnitsPerMeter = 100.0f;
constexpr float legacyMetersPerDunamisUnit =
    1.0f / legacyDunamisUnitsPerMeter;

Result migrateLegacyVectorProperty(nlohmann::json& properties,
                                   const char* propertyName,
                                   const std::string& context) {
    const auto property = properties.find(propertyName);
    if (property == properties.end()) return Result::success();

    glm::vec3 value{};
    Result result = authored_property::decode(property.value(), value);
    if (!result) {
        return Result::failure("Cannot migrate " + context + ": " +
                               result.error());
    }
    value *= legacyMetersPerDunamisUnit;
    result = authored_property::encode(value, property.value());
    if (!result) {
        return Result::failure("Cannot migrate " + context + ": " +
                               result.error());
    }
    return Result::success();
}

Result migrateLegacyScalarProperty(nlohmann::json& properties,
                                   const char* propertyName,
                                   const std::string& context) {
    const auto property = properties.find(propertyName);
    if (property == properties.end()) return Result::success();

    float value = 0.0f;
    Result result = authored_property::decode(property.value(), value);
    if (!result) {
        return Result::failure("Cannot migrate " + context + ": " +
                               result.error());
    }
    value *= legacyMetersPerDunamisUnit;
    result = authored_property::encode(value, property.value());
    if (!result) {
        return Result::failure("Cannot migrate " + context + ": " +
                               result.error());
    }
    return Result::success();
}

Result migrateLegacyWorldObjectRecord(nlohmann::json& record,
                                      const TypeRegistry& registry) {
    if (!record.is_object() || !record.contains("type") ||
        !record.at("type").is_string()) {
        return Result::success();
    }

    std::string objectId = "<unknown>";
    if (record.contains("id") && record.at("id").is_string()) {
        objectId = record.at("id").get<std::string>();
    }
    const std::string typeName = record.at("type").get<std::string>();
    const TypeDescriptor* type = registry.find(typeName);
    if (!type) {
        return Result::success();
    }

    if (record.contains("properties") && record.at("properties").is_object()) {
        auto& properties = record.at("properties");
        if (registry.isA(*type, "GameObject") &&
            registry.findProperty(*type, "position") != nullptr) {
            Result result = migrateLegacyVectorProperty(
                properties, "position", "position on object '" + objectId + "'");
            if (!result) return result;
        }
        if (registry.isA(*type, "Character")) {
            Result result = migrateLegacyScalarProperty(
                properties, "capsuleHeight",
                "capsuleHeight on object '" + objectId + "'");
            if (!result) return result;
            result = migrateLegacyScalarProperty(
                properties, "capsuleRadius",
                "capsuleRadius on object '" + objectId + "'");
            if (!result) return result;
        }

        // Directional shadow volume settings are persisted world distances and
        // were consumed directly by the renderer in the old DU convention.
        if (registry.findProperty(*type, "shadowFocus") != nullptr) {
            Result result = migrateLegacyVectorProperty(
                properties, "shadowFocus",
                "shadowFocus on object '" + objectId + "'");
            if (!result) return result;
            for (const char* propertyName : {"shadowHalfExtent",
                                             "shadowLightDistance",
                                             "shadowNearPlane",
                                             "shadowFarPlane"}) {
                result = migrateLegacyScalarProperty(
                    properties, propertyName,
                    std::string(propertyName) + " on object '" + objectId + "'");
                if (!result) return result;
            }
        }

        // PhysicsBodySettings::sphereRadius is deliberately not converted:
        // its persisted default is 0.5 and its existing consumers indicate
        // that this field was already authored with meter semantics.
    }

    if (record.contains("attachedCamera") &&
        record.at("attachedCamera").is_object()) {
        Result result = migrateLegacyVectorProperty(
            record.at("attachedCamera"), "position",
            "attached camera position on object '" + objectId + "'");
        if (!result) return result;
    }
    return Result::success();
}

Result migrateLegacyWorldDocument(nlohmann::json& document,
                                  const TypeRegistry& registry) {
    if (document.contains("objects") && document.at("objects").is_array()) {
        for (auto& record : document.at("objects")) {
            Result result = migrateLegacyWorldObjectRecord(record, registry);
            if (!result) return result;
        }
    }
    if (document.contains("editor") && document.at("editor").is_object() &&
        document.at("editor").contains("camera") &&
        document.at("editor").at("camera").is_object()) {
        Result result = migrateLegacyVectorProperty(
            document.at("editor").at("camera"), "position",
            "editor camera position");
        if (!result) return result;
    }
    return Result::success();
}

Result migrateLegacyAssetScaleObjectRecord(
    nlohmann::json& record, const TypeRegistry& registry,
    std::vector<std::string>& warnings) {
    if (!record.is_object() || !record.contains("type") ||
        !record.at("type").is_string() ||
        !record.contains("properties") ||
        !record.at("properties").is_object()) {
        return Result::success();
    }

    std::string objectId = "<unknown>";
    if (record.contains("id") && record.at("id").is_string()) {
        objectId = record.at("id").get<std::string>();
    }
    const TypeDescriptor* type =
        registry.find(record.at("type").get<std::string>());
    if (!type || !registry.isA(*type, "GameObject") ||
        registry.findProperty(*type, "scale") == nullptr) {
        return Result::success();
    }

    auto& properties = record.at("properties");
    const auto modelPath = properties.find("modelPath");
    const auto scale = properties.find("scale");
    if (modelPath == properties.end() || !modelPath->is_string() ||
        modelPath->get<std::string>().empty() || scale == properties.end()) {
        return Result::success();
    }

    glm::vec3 value{};
    Result result = authored_property::decode(scale.value(), value);
    if (!result) {
        return Result::failure("Cannot migrate scale on object '" + objectId +
                               "': " + result.error());
    }

    const std::string modelPathString = modelPath->get<std::string>();
    const model_loading::LegacyAssetScaleMigration migration =
        model_loading::inspectLegacyAssetScaleMigration(modelPathString);
    if (!migration.known) {
        warnings.push_back(
            "Preserving legacy scale on object '" + objectId +
            "' for asset '" + modelPathString + "': " + migration.evidence);
        return Result::success();
    }

    value *= migration.scaleFactor;
    result = authored_property::encode(value, scale.value());
    if (!result) {
        return Result::failure("Cannot migrate scale on object '" + objectId +
                               "': " + result.error());
    }
    return Result::success();
}

Result migrateLegacyAssetScaleDocument(
    nlohmann::json& document, const TypeRegistry& registry,
    std::vector<std::string>& warnings) {
    if (!document.contains("objects") || !document.at("objects").is_array()) {
        return Result::success();
    }
    for (auto& record : document.at("objects")) {
        Result result = migrateLegacyAssetScaleObjectRecord(
            record, registry, warnings);
        if (!result) return result;
    }
    return Result::success();
}

Result encodeCamera(const Camera& camera, nlohmann::json& output) {
    Result result = authored_property::encode(camera.position, output["position"]);
    if (!result) return result;
    result = authored_property::encode(camera.front, output["front"]);
    if (!result) return result;
    return authored_property::encode(camera.up, output["up"]);
}

Result encodeAttachedCamera(const Camera& camera, nlohmann::json& output) {
    Result result = encodeCamera(camera, output);
    if (!result) return result;
    return authored_property::encode(camera.fov(), output["fov"]);
}

Result decodeCamera(const nlohmann::json& input, EditorCameraState& state) {
    if (!input.is_object()) return Result::failure("expected camera object");
    for (const char* field : {"position", "front", "up"}) {
        if (!input.contains(field)) {
            return Result::failure(std::string("missing camera field '") + field + "'");
        }
    }
    Result result = authored_property::decode(input.at("position"), state.position);
    if (!result) return result;
    result = authored_property::decode(input.at("front"), state.front);
    if (!result) return result;
    result = authored_property::decode(input.at("up"), state.up);
    if (!result) return result;
    Camera validation;
    validation.position = state.position;
    validation.front = state.front;
    validation.up = state.up;
    double yaw = 0.0;
    double pitch = 0.0;
    if (!validation.deriveYawPitchDegrees(yaw, pitch) ||
        glm::dot(glm::cross(validation.front, validation.up),
                 glm::cross(validation.front, validation.up)) <= 1.0e-8f) {
        return Result::failure("camera orientation is invalid");
    }
    return Result::success();
}

Result decodeAttachedCamera(const nlohmann::json& input, Camera& camera) {
    EditorCameraState state;
    Result result = decodeCamera(input, state);
    if (!result) return result;
    camera.position = state.position;
    camera.front = state.front;
    camera.up = state.up;
    if (input.contains("fov")) {
        float fov = 0.0f;
        result = authored_property::decode(input.at("fov"), fov);
        if (!result) return result;
        if (!camera.setFov(fov)) {
            return Result::failure("camera FOV must be finite and between 0 and 180 degrees");
        }
    }
    return Result::success();
}

Result applyProperties(const nlohmann::json& properties, GameObject& object,
                       const TypeDescriptor& type,
                       const TypeRegistry& registry,
                       std::vector<std::string>& warnings) {
    if (!properties.is_object()) return Result::failure("expected properties object");
    for (const PropertyDescriptor* descriptor :
         registry.persistedProperties(type)) {
        const auto property = properties.find(descriptor->name);
        if (property == properties.end()) continue;
        Result result = descriptor->write(object, property.value());
        if (!result) {
            return Result::failure("Failed to restore property '" + descriptor->name +
                                   "': " + result.error());
        }
    }
    for (auto property = properties.begin(); property != properties.end(); ++property) {
        const PropertyDescriptor* descriptor =
            registry.findProperty(type, property.key());
        if (!descriptor ||
            descriptor->lifecycle != PropertyLifecycle::Persisted) {
            warnings.push_back("Ignoring unknown property '" + property.key() +
                               "' on type '" + type.name + "'");
        }
    }
    return Result::success();
}

Result resolveActiveCamera(const nlohmann::json& reference, Scene& scene) {
    if (reference.is_null()) return Result::success();
    if (!reference.is_object() || !reference.contains("ownerId") ||
        !reference.at("ownerId").is_string() || !reference.contains("kind") ||
        !reference.at("kind").is_string()) {
        return Result::failure("activeCamera must contain string ownerId and kind");
    }
    const std::string ownerId = reference.at("ownerId").get<std::string>();
    const std::string kind = reference.at("kind").get<std::string>();
    GameObject* owner = scene.findGameObject(ownerId);
    if (!owner) return Result::failure("activeCamera owner '" + ownerId + "' was not found");
    Camera* camera = nullptr;
    if (kind == "object") camera = dynamic_cast<Camera*>(owner);
    else if (kind == "attached") camera = owner->attachedCamera();
    else return Result::failure("unknown activeCamera kind '" + kind + "'");
    if (!camera) return Result::failure("activeCamera reference does not resolve to a camera");
    return scene.setActiveCameraReference(camera);
}

Result copyRuntimeTransferProperty(const PropertyDescriptor& descriptor,
                                   const GameObject& source,
                                   GameObject& destination,
                                   const std::string& objectId,
                                   const std::string& typeName) {
    const std::string context = "Failed to transfer runtime-only property '" +
        descriptor.name + "' on object '" + objectId + "' (" + typeName + ")";
    if (!descriptor.copy) {
        return Result::failure(context + ": descriptor has no copy callback");
    }

    try {
        Result result = descriptor.copy(source, destination);
        if (!result) return Result::failure(context + ": " + result.error());
    } catch (const std::exception& exception) {
        return Result::failure(context + ": " + exception.what());
    } catch (...) {
        return Result::failure(context + ": copy callback threw an unknown exception");
    }
    return Result::success();
}

Result copyRuntimeTransferProperties(const GameObject& source,
                                     GameObject& destination,
                                     const TypeDescriptor& type,
                                     const TypeRegistry& registry,
                                     const std::string& objectId) {
    for (const PropertyDescriptor* descriptor :
         registry.runtimeTransferProperties(type)) {
        if (descriptor->lifecycle != PropertyLifecycle::RuntimeTransferOnly) {
            continue;
        }
        Result result = copyRuntimeTransferProperty(
            *descriptor, source, destination, objectId, type.name);
        if (!result) return result;
    }
    return Result::success();
}

Result copyAttachedCameraRuntimeTransferProperties(
    const GameObject& sourceObject, GameObject& destinationObject,
    const TypeRegistry& registry, const std::string& objectId,
    const std::string& ownerTypeName) {
    const Camera* sourceCamera = sourceObject.attachedCamera();
    Camera* destinationCamera = destinationObject.attachedCamera();
    if ((sourceCamera != nullptr) != (destinationCamera != nullptr)) {
        return Result::failure(
            "Attached-camera topology does not match for object '" + objectId +
            "' (" + ownerTypeName + ")");
    }
    if (!sourceCamera) return Result::success();

    const TypeDescriptor* cameraDescriptor = registry.find("Camera");
    if (!cameraDescriptor) {
        return Result::failure(
            "Cannot transfer attached-camera runtime-only properties for object '" +
            objectId + "' (" + ownerTypeName + "): registered type 'Camera' was not found");
    }

    for (const PropertyDescriptor& descriptor : cameraDescriptor->properties) {
        if (descriptor.lifecycle != PropertyLifecycle::RuntimeTransferOnly) {
            continue;
        }
        Result result = copyRuntimeTransferProperty(
            descriptor, *sourceCamera, *destinationCamera, objectId,
            cameraDescriptor->name);
        if (!result) return result;
    }
    return Result::success();
}

}  // namespace

Result SceneSerializer::serializeObject(const GameObject& object,
                                        const TypeRegistry& registry,
                                        nlohmann::json& output) {
    if (object.persistentId.empty()) {
        return Result::failure("GameObject '" + object.name +
                               "' has no persistent ID");
    }
    const TypeDescriptor* type = registry.find(object);
    if (!type) {
        return Result::failure("Object '" + object.persistentId +
                               "' has an unregistered C++ type");
    }
    const GameObject* parent = object.parent();
    if (parent != nullptr && parent->persistentId.empty()) {
        return Result::failure("Object '" + object.persistentId +
                               "' has a parent without a persistent ID");
    }
    output = {{"id", object.persistentId},
              {"parentId", parent == nullptr
                               ? nlohmann::json(nullptr)
                               : nlohmann::json(parent->persistentId)},
              {"type", type->name},
              {"properties", nlohmann::json::object()}};
    for (const PropertyDescriptor* property : registry.persistedProperties(*type)) {
        nlohmann::json value;
        Result result = property->read(object, value);
        if (!result) {
            return Result::failure("Failed to serialize property '" + property->name +
                                   "' on object '" + object.persistentId +
                                   "': " + result.error());
        }
        output["properties"][property->name] = std::move(value);
    }
    if (const Camera* attached = object.attachedCamera()) {
        nlohmann::json camera = nlohmann::json::object();
        Result result = encodeAttachedCamera(*attached, camera);
        if (!result) return Result::failure("Invalid attached camera on object '" +
                                            object.persistentId + "': " + result.error());
        output["attachedCamera"] = std::move(camera);
    }
    return Result::success();
}

Result SceneSerializer::serializeAuthored(const Scene& scene,
                                          const TypeRegistry& registry,
                                          nlohmann::json& output) {
    Result validation = scene.validateAuthoredState();
    if (!validation) {
        return Result::failure("Scene authored state is invalid: " +
                               validation.error());
    }
    output = {{"formatVersion", formatVersion},
              {"scene", {{"name", scene.name}}},
              {"activeCamera", nullptr},
              {"objects", nlohmann::json::array()}};
    Result result = authored_property::encode(scene.backgroundColor(),
                                               output["scene"]["backgroundColor"]);
    if (!result) return result;
    result = authored_property::encode(scene.ambientColor(),
                                       output["scene"]["ambientColor"]);
    if (!result) return result;
    result = authored_property::encode(scene.ambientIntensity(),
                                       output["scene"]["ambientIntensity"]);
    if (!result) return result;

    std::unordered_set<std::string> ids;
    for (const auto& object : scene.gameObjects()) {
        if (!ids.insert(object->persistentId).second) {
            return Result::failure("Duplicate persistent ID '" +
                                   object->persistentId + "'");
        }
        nlohmann::json record;
        result = serializeObject(*object, registry, record);
        if (!result) return result;

        const GameObject* parent = object->parent();
        const auto& siblings = parent == nullptr
                                   ? scene.rootObjects()
                                   : parent->children();
        const auto sibling = std::find(siblings.begin(), siblings.end(),
                                       object.get());
        if (sibling == siblings.end()) {
            return Result::failure(
                "Object '" + object->persistentId +
                "' is missing from its authored sibling list");
        }
        record["siblingIndex"] = static_cast<std::size_t>(
            std::distance(siblings.begin(), sibling));
        output["objects"].push_back(std::move(record));

        if (scene.activeCamera() != nullptr &&
            scene.activeCamera() == dynamic_cast<const Camera*>(object.get())) {
            output["activeCamera"] = {{"ownerId", object->persistentId},
                                      {"kind", "object"}};
        } else if (scene.activeCamera() != nullptr &&
                   scene.activeCamera() == object->attachedCamera()) {
            output["activeCamera"] = {{"ownerId", object->persistentId},
                                      {"kind", "attached"}};
        }
    }
    return Result::success();
}

Result SceneSerializer::serializeFull(const Scene& scene,
                                      const TypeRegistry& registry,
                                      const Camera& editorCamera,
                                      nlohmann::json& output) {
    return serializeFull(scene, registry, editorCamera, {}, output);
}

Result SceneSerializer::serializeFull(const Scene& scene,
                                      const TypeRegistry& registry,
                                      const Camera& editorCamera,
                                      const std::vector<std::string>& renderColliders,
                                      nlohmann::json& output) {
    Result result = serializeAuthored(scene, registry, output);
    if (!result) return result;
    nlohmann::json camera = nlohmann::json::object();
    result = encodeCamera(editorCamera, camera);
    if (!result) return Result::failure("Invalid editor camera: " + result.error());
    std::vector<std::string> ids = renderColliders;
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    output["editor"] = {{"camera", std::move(camera)}, {"renderColliders", ids}};
    return Result::success();
}

Result SceneSerializer::applyDocument(const nlohmann::json& document,
                                      Scene& candidate,
                                      const TypeRegistry& registry,
                                      SceneLoadData& loadData) {
    try {
        if (!document.is_object()) return Result::failure("Scene document root must be an object");
        if (!document.contains("formatVersion") ||
            !document.at("formatVersion").is_number_integer()) {
            return Result::failure("Scene document requires integer formatVersion");
        }
        const int sourceVersion = document.at("formatVersion").get<int>();
        if (sourceVersion < 1 || sourceVersion > formatVersion) {
            return Result::failure("Unsupported scene formatVersion " +
                                   std::to_string(sourceVersion));
        }

        nlohmann::json migratedDocument = document;
        if (sourceVersion == 1 || sourceVersion == 2) {
            Result migration = migrateLegacyWorldDocument(
                migratedDocument, registry);
            if (!migration) {
                return Result::failure("Scene migration failed: " +
                                       migration.error());
            }
        }
        if (sourceVersion != formatVersion) {
            Result migration = migrateLegacyAssetScaleDocument(
                migratedDocument, registry, loadData.warnings);
            if (!migration) {
                return Result::failure("Scene asset-basis migration failed: " +
                                       migration.error());
            }
            migratedDocument["formatVersion"] = formatVersion;
        }

        const nlohmann::json& input = migratedDocument;
        const bool hasSavedHierarchy = sourceVersion == 2 || sourceVersion == 3 ||
                                       sourceVersion == formatVersion;
        if (!input.contains("scene") || !input.at("scene").is_object()) {
            return Result::failure("Scene document requires a scene object");
        }
        const auto& sceneData = input.at("scene");
        for (const char* field : {"name", "backgroundColor", "ambientColor",
                                  "ambientIntensity"}) {
            if (!sceneData.contains(field)) {
                return Result::failure(std::string("Scene section is missing '") + field + "'");
            }
        }
        if (!sceneData.at("name").is_string()) return Result::failure("Scene name must be a string");
        glm::vec4 background;
        glm::vec3 ambient;
        float intensity = 0.0f;
        Result result = authored_property::decode(sceneData.at("backgroundColor"), background);
        if (!result) return Result::failure("Invalid backgroundColor: " + result.error());
        result = authored_property::decode(sceneData.at("ambientColor"), ambient);
        if (!result) return Result::failure("Invalid ambientColor: " + result.error());
        result = authored_property::decode(sceneData.at("ambientIntensity"), intensity);
        if (!result) return Result::failure("Invalid ambientIntensity: " + result.error());
        result = candidate.setBackgroundColor(background);
        if (!result) return result;
        result = candidate.setAmbientLight(ambient, intensity);
        if (!result) return result;
        candidate.name = sceneData.at("name").get<std::string>();

        if (!input.contains("objects") || !input.at("objects").is_array()) {
            return Result::failure("Scene document requires an objects array");
        }
        struct PendingHierarchy {
            GameObject* object = nullptr;
            std::optional<std::string> parentId;
            std::optional<std::size_t> siblingIndex;
        };

        std::unordered_set<std::string> savedIds;
        std::unordered_map<std::string, GameObject*> savedObjects;
        std::vector<PendingHierarchy> pendingHierarchy;
        pendingHierarchy.reserve(input.at("objects").size());
        for (const auto& record : input.at("objects")) {
            if (!record.is_object() || !record.contains("id") ||
                !record.at("id").is_string() ||
                record.at("id").get<std::string>().empty()) {
                return Result::failure("Every saved object requires a non-empty string ID");
            }
            const std::string id = record.at("id").get<std::string>();
            if (!savedIds.insert(id).second) {
                return Result::failure("Duplicate persistent ID '" + id + "'");
            }

            std::optional<std::string> parentId;
            std::optional<std::size_t> siblingIndex;
            if (hasSavedHierarchy) {
                if (!record.contains("parentId")) {
                    return Result::failure("Object '" + id +
                                           "' is missing parentId");
                }
                const auto& parent = record.at("parentId");
                if (!parent.is_null()) {
                    if (!parent.is_string() || parent.get<std::string>().empty()) {
                        return Result::failure(
                            "Object '" + id +
                            "' parentId must be null or a non-empty string");
                    }
                    parentId = parent.get<std::string>();
                }
                if (record.contains("siblingIndex")) {
                    const auto& siblingOrder = record.at("siblingIndex");
                    if (!siblingOrder.is_number_unsigned()) {
                        return Result::failure(
                            "Object '" + id +
                            "' siblingIndex must be an unsigned integer");
                    }
                    const std::uint64_t decoded =
                        siblingOrder.get<std::uint64_t>();
                    if (decoded >
                        static_cast<std::uint64_t>(
                            std::numeric_limits<std::size_t>::max())) {
                        return Result::failure(
                            "Object '" + id + "' siblingIndex is out of range");
                    }
                    // Keep presentation metadata separate from parentId: the
                    // latter remains the sole serialized hierarchy truth.
                    siblingIndex = static_cast<std::size_t>(decoded);
                }
            }

            if (!record.contains("type") || !record.at("type").is_string()) {
                return Result::failure("Object '" + id + "' requires a string type");
            }
            const std::string typeName = record.at("type").get<std::string>();
            const TypeDescriptor* savedType = registry.find(typeName);
            if (!savedType) return Result::failure("Object '" + id +
                                                   "' has unknown registered type '" + typeName + "'");
            GameObject* object = candidate.findGameObject(id);
            if (object) {
                const TypeDescriptor* actualType = registry.find(*object);
                if (!actualType || actualType->name != savedType->name) {
                    return Result::failure("Object '" + id + "' type is incompatible with '" +
                                           typeName + "'");
                }
            } else {
                if (!savedType->factory) {
                    return Result::failure("Object '" + id + "' type '" + typeName +
                                           "' has no factory");
                }
                std::unique_ptr<GameObject> created = savedType->factory();
                if (!created) return Result::failure("Factory for type '" + typeName + "' returned null");
                created->persistentId = id;
                object = created.get();
                result = candidate.addGameObject(std::move(created));
                if (!result) return Result::failure("Failed to add object '" + id + "': " + result.error());
            }
            if (!record.contains("properties")) {
                return Result::failure("Object '" + id + "' is missing properties");
            }
            result = applyProperties(record.at("properties"), *object, *savedType,
                                     registry, loadData.warnings);
            if (!result) return Result::failure("Failed to load object '" + id +
                                                "' (" + typeName + "): " + result.error());
            if (record.contains("attachedCamera")) {
                Camera* attached = object->attachedCamera();
                if (!attached) return Result::failure("Object '" + id +
                                                      "' has saved attached-camera data but no attached camera");
                result = decodeAttachedCamera(record.at("attachedCamera"), *attached);
                if (!result) return Result::failure("Invalid attached camera on object '" + id + "': " + result.error());
            }

            savedObjects.emplace(id, object);
            pendingHierarchy.push_back(
                {object, std::move(parentId), siblingIndex});
        }

        if (sourceVersion == 1 || hasSavedHierarchy) {
            for (const PendingHierarchy& pending : pendingHierarchy) {
                if (pending.object == nullptr || pending.object->parent() == nullptr) {
                    continue;
                }
                result = candidate.reparentGameObject(
                    *pending.object, nullptr, Scene::ReparentMode::PreserveLocal);
                if (!result) {
                    return Result::failure(
                        "Failed to clear existing hierarchy for object '" +
                        pending.object->persistentId + "': " + result.error());
                }
            }

        }

        if (hasSavedHierarchy) {
            for (const PendingHierarchy& pending : pendingHierarchy) {
                if (pending.object == nullptr) {
                    return Result::failure("Loaded object hierarchy contains a null object");
                }

                const std::string& childId = pending.object->persistentId;
                GameObject* parent = nullptr;
                if (pending.parentId.has_value()) {
                    const auto parentIt = savedObjects.find(*pending.parentId);
                    if (parentIt == savedObjects.end()) {
                        return Result::failure(
                            "Object '" + childId + "' references missing parent '" +
                            *pending.parentId + "'");
                    }
                    parent = parentIt->second;
                }

                if (pending.object->parent() != parent) {
                    result = candidate.reparentGameObject(
                        *pending.object, parent, Scene::ReparentMode::PreserveLocal);
                    if (!result) {
                        return Result::failure(
                            "Failed to restore hierarchy for object '" + childId +
                            "': " + result.error());
                    }
                }
            }

            bool hasSiblingOrder = false;
            bool hasMissingSiblingOrder = false;
            for (const PendingHierarchy& pending : pendingHierarchy) {
                hasSiblingOrder |= pending.siblingIndex.has_value();
                hasMissingSiblingOrder |= !pending.siblingIndex.has_value();
            }
            bool validSiblingOrder = hasSiblingOrder && !hasMissingSiblingOrder;
            std::vector<GameObject*> orderParents;
            if (validSiblingOrder) {
                orderParents.reserve(pendingHierarchy.size());
                for (const PendingHierarchy& pending : pendingHierarchy) {
                    GameObject* parent = pending.object->parent();
                    if (std::find(orderParents.begin(), orderParents.end(),
                                  parent) == orderParents.end()) {
                        orderParents.push_back(parent);
                    }
                }

                for (GameObject* parent : orderParents) {
                    std::vector<const PendingHierarchy*> siblings;
                    for (const PendingHierarchy& pending : pendingHierarchy) {
                        if (pending.object->parent() == parent) {
                            siblings.push_back(&pending);
                        }
                    }
                    std::sort(
                        siblings.begin(), siblings.end(),
                        [](const PendingHierarchy* left,
                           const PendingHierarchy* right) {
                            return *left->siblingIndex < *right->siblingIndex;
                        });
                    for (std::size_t index = 0; index < siblings.size(); ++index) {
                        if (!siblings[index]->siblingIndex.has_value() ||
                            *siblings[index]->siblingIndex != index) {
                            validSiblingOrder = false;
                            break;
                        }
                    }
                    if (!validSiblingOrder) break;
                }
            }

            if (hasSiblingOrder && !validSiblingOrder) {
                loadData.warnings.push_back(
                    "Ignoring incomplete or conflicting scene hierarchy order metadata");
            } else if (validSiblingOrder) {
                for (GameObject* parent : orderParents) {
                    std::vector<const PendingHierarchy*> siblings;
                    for (const PendingHierarchy& pending : pendingHierarchy) {
                        if (pending.object->parent() == parent) {
                            siblings.push_back(&pending);
                        }
                    }
                    std::sort(
                        siblings.begin(), siblings.end(),
                        [](const PendingHierarchy* left,
                           const PendingHierarchy* right) {
                            return *left->siblingIndex < *right->siblingIndex;
                        });
                    for (std::size_t index = 0; index < siblings.size(); ++index) {
                        Result reorder = candidate.reorderGameObject(
                            *siblings[index]->object, parent, index);
                        if (!reorder) {
                            return Result::failure(
                                "Failed to restore sibling order for object '" +
                                siblings[index]->object->persistentId + "': " +
                                reorder.error());
                        }
                    }
                }
            }
        }

        if (input.contains("activeCamera")) {
            result = resolveActiveCamera(input.at("activeCamera"), candidate);
            if (!result) return result;
        }
        result = candidate.validateForActivation();
        if (!result) return Result::failure("Loaded candidate scene is invalid: " + result.error());

        if (input.contains("editor") && input.at("editor").is_object() &&
            input.at("editor").contains("camera")) {
            EditorCameraState state;
            result = decodeCamera(input.at("editor").at("camera"), state);
            if (result) loadData.editorCamera = state;
            else loadData.warnings.push_back("Ignoring invalid editor camera: " + result.error());
        }
        if (input.contains("editor") && input.at("editor").is_object() &&
            input.at("editor").contains("renderColliders")) {
            const auto& ids = input.at("editor").at("renderColliders");
            if (!ids.is_array()) {
                loadData.warnings.push_back("Ignoring editor.renderColliders: expected an array");
            } else {
                std::unordered_set<std::string> uniqueIds;
                for (const auto& id : ids) {
                    if (!id.is_string() || id.get<std::string>().empty()) {
                        loadData.warnings.push_back("Ignoring invalid editor.renderColliders ID");
                    } else if (uniqueIds.insert(id.get<std::string>()).second) {
                        loadData.renderColliders.push_back(id.get<std::string>());
                    }
                }
            }
        }

        result = serializeAuthored(candidate, registry, loadData.authoredBaseline);
        if (!result) return result;
        auto& baselineObjects = loadData.authoredBaseline["objects"];
        baselineObjects.erase(
            std::remove_if(baselineObjects.begin(), baselineObjects.end(),
                           [&savedIds](const nlohmann::json& object) {
                               return savedIds.count(object.at("id").get<std::string>()) == 0;
                           }), baselineObjects.end());
        return Result::success();
    } catch (const nlohmann::json::exception& exception) {
        return Result::failure(std::string("Invalid scene JSON: ") + exception.what());
    } catch (const std::exception& exception) {
        return Result::failure(exception.what());
    }
}

Result SceneSerializer::copyAuthoredState(const Scene& source,
                                          Scene& destination,
                                          const TypeRegistry& registry) {
    if (&source == &destination) {
        return Result::failure(
            "Authoring state destination must be a different scene");
    }

    nlohmann::json document;
    Result result = serializeAuthored(source, registry, document);
    if (!result) return result;
    SceneLoadData ignored;
    result = applyDocument(document, destination, registry, ignored);
    if (!result) return result;

    for (const auto& sourceObject : source.gameObjects()) {
        if (!sourceObject) {
            return Result::failure(
                "Cannot transfer runtime-only properties from a null source object");
        }
        if (sourceObject->persistentId.empty()) {
            return Result::failure(
                "Cannot transfer runtime-only properties for an object without a persistent ID");
        }

        GameObject* destinationObject =
            destination.findGameObject(sourceObject->persistentId);
        if (!destinationObject) {
            return Result::failure(
                "Runtime destination is missing object '" +
                sourceObject->persistentId + "'");
        }

        const TypeDescriptor* sourceType = registry.find(*sourceObject);
        if (!sourceType) {
            return Result::failure(
                "Object '" + sourceObject->persistentId +
                "' has an unregistered source C++ type");
        }
        const TypeDescriptor* destinationType = registry.find(*destinationObject);
        if (!destinationType) {
            return Result::failure(
                "Runtime object '" + sourceObject->persistentId +
                "' has an unregistered destination C++ type");
        }
        if (sourceType->name != destinationType->name ||
            sourceType->type != destinationType->type) {
            return Result::failure(
                "Object '" + sourceObject->persistentId +
                "' registered type mismatch: source '" + sourceType->name +
                "', destination '" + destinationType->name + "'");
        }

        result = copyRuntimeTransferProperties(
            *sourceObject, *destinationObject, *sourceType, registry,
            sourceObject->persistentId);
        if (!result) return result;

        result = copyAttachedCameraRuntimeTransferProperties(
            *sourceObject, *destinationObject, registry,
            sourceObject->persistentId, sourceType->name);
        if (!result) return result;
    }

    return Result::success();
}

Result SceneSerializer::copyAuthoredAttachedCameraState(
    const Camera& source, Camera& destination, const TypeRegistry& registry) {
    const TypeDescriptor* sourceType = registry.find(source);
    if (!sourceType || !registry.isA(*sourceType, "Camera")) {
        return Result::failure(
            "Attached source camera has no registered Camera type");
    }
    const TypeDescriptor* destinationType = registry.find(destination);
    if (!destinationType || !registry.isA(*destinationType, "Camera")) {
        return Result::failure(
            "Attached destination camera has no registered Camera type");
    }
    if (sourceType->name != destinationType->name ||
        sourceType->type != destinationType->type) {
        return Result::failure(
            "Attached-camera registered concrete types do not match");
    }

    // Attached cameras are authored topology owned by their GameObject, so
    // only the camera fields represented by the attached-camera JSON record
    // are copied here. Runtime camera state remains untouched.
    for (const char* propertyName : {"position", "front", "up", "fov"}) {
        const PropertyDescriptor* property =
            registry.findProperty(*sourceType, propertyName);
        if (!property || property->lifecycle != PropertyLifecycle::Persisted ||
            !property->copy) {
            return Result::failure(
                "Attached Camera persisted property '" +
                std::string(propertyName) + "' has no copy callback");
        }
        try {
            const Result result = property->copy(source, destination);
            if (!result) {
                return Result::failure(
                    "Failed to copy attached Camera property '" +
                    std::string(propertyName) + "': " + result.error());
            }
        } catch (const std::exception& exception) {
            return Result::failure(
                "Failed to copy attached Camera property '" +
                std::string(propertyName) + "': " + exception.what());
        } catch (...) {
            return Result::failure(
                "Failed to copy attached Camera property '" +
                std::string(propertyName) +
                "': copy callback threw an unknown exception");
        }
    }
    return Result::success();
}
