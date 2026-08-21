#include "scene_serializer.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <unordered_set>

namespace {

Result encodeCamera(const Camera& camera, nlohmann::json& output) {
    Result result = authored_property::encode(camera.position, output["position"]);
    if (!result) return result;
    result = authored_property::encode(camera.front, output["front"]);
    if (!result) return result;
    return authored_property::encode(camera.up, output["up"]);
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
    output = {{"id", object.persistentId}, {"type", type->name},
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
        Result result = encodeCamera(*attached, camera);
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
        output["objects"].push_back(std::move(record));

        if (scene.activeCamera() == dynamic_cast<const Camera*>(object.get())) {
            output["activeCamera"] = {{"ownerId", object->persistentId},
                                      {"kind", "object"}};
        } else if (scene.activeCamera() == object->attachedCamera()) {
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
        const int version = document.at("formatVersion").get<int>();
        if (version != formatVersion) {
            return Result::failure("Unsupported scene formatVersion " + std::to_string(version));
        }
        if (!document.contains("scene") || !document.at("scene").is_object()) {
            return Result::failure("Scene document requires a scene object");
        }
        const auto& sceneData = document.at("scene");
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

        if (!document.contains("objects") || !document.at("objects").is_array()) {
            return Result::failure("Scene document requires an objects array");
        }
        std::unordered_set<std::string> savedIds;
        for (const auto& record : document.at("objects")) {
            if (!record.is_object() || !record.contains("id") ||
                !record.at("id").is_string() ||
                record.at("id").get<std::string>().empty()) {
                return Result::failure("Every saved object requires a non-empty string ID");
            }
            const std::string id = record.at("id").get<std::string>();
            if (!savedIds.insert(id).second) {
                return Result::failure("Duplicate persistent ID '" + id + "'");
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
                EditorCameraState attachedState;
                result = decodeCamera(record.at("attachedCamera"), attachedState);
                if (!result) return Result::failure("Invalid attached camera on object '" + id + "': " + result.error());
                attached->position = attachedState.position;
                attached->front = attachedState.front;
                attached->up = attachedState.up;
            }
        }

        if (document.contains("activeCamera")) {
            result = resolveActiveCamera(document.at("activeCamera"), candidate);
            if (!result) return result;
        }
        result = candidate.validateForActivation();
        if (!result) return Result::failure("Loaded candidate scene is invalid: " + result.error());

        if (document.contains("editor") && document.at("editor").is_object() &&
            document.at("editor").contains("camera")) {
            EditorCameraState state;
            result = decodeCamera(document.at("editor").at("camera"), state);
            if (result) loadData.editorCamera = state;
            else loadData.warnings.push_back("Ignoring invalid editor camera: " + result.error());
        }
        if (document.contains("editor") && document.at("editor").is_object() &&
            document.at("editor").contains("renderColliders")) {
            const auto& ids = document.at("editor").at("renderColliders");
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
    nlohmann::json document;
    Result result = serializeAuthored(source, registry, document);
    if (!result) return result;
    SceneLoadData ignored;
    result = applyDocument(document, destination, registry, ignored);
    if (!result) return result;
    // FOV is intentionally not part of the persisted scene format yet, but it
    // must survive the in-memory editor-to-runtime transfer.
    return source.copyAuthoringStateTo(destination);
}
