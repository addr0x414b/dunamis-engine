#include "editor_duplication.h"

#include "../scene/camera.h"
#include "../scene/game_object.h"
#include "../scene/scene.h"
#include "../scene/scene_serializer.h"
#include "../scene/type_registry.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <limits>
#include <system_error>
#include <unordered_set>
#include <utility>

namespace {

bool parsePositiveDuplicateSuffix(const std::string& name,
                                  std::size_t& suffixStart) {
    suffixStart = std::string::npos;
    if (name.size() < 4 || name.back() != ')') {
        return false;
    }

    const std::size_t opening = name.rfind(" (");
    if (opening == std::string::npos || opening + 3 > name.size()) {
        return false;
    }

    const char* begin = name.data() + opening + 2;
    const char* end = name.data() + name.size() - 1;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0) {
        return false;
    }

    suffixStart = opening;
    return true;
}

std::string makeNumberedName(const std::string& base,
                             std::uint64_t index) {
    return base + " (" + std::to_string(index) + ")";
}

Result copyAttachedCameraIfPresent(const GameObject& source,
                                   GameObject& duplicate,
                                   const TypeRegistry& registry) {
    const Camera* sourceCamera = source.attachedCamera();
    Camera* duplicateCamera = duplicate.attachedCamera();
    if ((sourceCamera != nullptr) != (duplicateCamera != nullptr)) {
        return Result::failure(
            "Attached-camera topology does not match the source object");
    }
    if (!sourceCamera) {
        return Result::success();
    }

    return SceneSerializer::copyAuthoredAttachedCameraState(
        *sourceCamera, *duplicateCamera, registry);
}

}  // namespace

namespace editor {

std::string makeDuplicateName(const std::string& sourceName,
                              const std::vector<std::string>& existingNames) {
    if (sourceName.empty()) {
        return {};
    }

    std::string baseName = sourceName;
    std::size_t suffixStart = std::string::npos;
    if (parsePositiveDuplicateSuffix(sourceName, suffixStart)) {
        baseName = sourceName.substr(0, suffixStart);
    }

    std::unordered_set<std::string> names;
    names.reserve(existingNames.size());
    for (const std::string& name : existingNames) {
        names.insert(name);
    }

    for (std::uint64_t index = 1;; ++index) {
        const std::string candidate = makeNumberedName(baseName, index);
        if (names.count(candidate) == 0) {
            return candidate;
        }
        if (index == std::numeric_limits<std::uint64_t>::max()) {
            // A Scene cannot realistically contain this many names, but keep
            // the loop defined if an adversarial test supplies that input.
            return sourceName;
        }
    }
}

Result duplicateAuthoredGameObject(
    const GameObject& source, const TypeRegistry& registry,
    std::unique_ptr<GameObject>& duplicate) {
    duplicate.reset();

    const TypeDescriptor* sourceType = registry.find(source);
    if (!sourceType) {
        return Result::failure(
            "Cannot duplicate an object with an unregistered C++ type");
    }
    if (!sourceType->factory) {
        return Result::failure("Registered type '" + sourceType->name +
                               "' has no factory");
    }

    try {
        duplicate = sourceType->factory();
    } catch (const std::exception& exception) {
        return Result::failure("Factory for type '" + sourceType->name +
                               "' threw: " + exception.what());
    } catch (...) {
        return Result::failure("Factory for type '" + sourceType->name +
                               "' threw an unknown exception");
    }
    if (!duplicate) {
        return Result::failure("Factory for type '" + sourceType->name +
                               "' returned null");
    }

    const TypeDescriptor* duplicateType = registry.find(*duplicate);
    if (!duplicateType || duplicateType->name != sourceType->name ||
        duplicateType->type != sourceType->type) {
        duplicate.reset();
        return Result::failure(
            "Factory for type '" + sourceType->name +
            "' returned a different registered concrete type");
    }

    try {
        const Result topologyResult =
            copyAttachedCameraIfPresent(source, *duplicate, registry);
        if (!topologyResult) {
            duplicate.reset();
            return topologyResult;
        }

        Result result = registry.copyAuthoredProperties(source, *duplicate);
        if (!result) {
            duplicate.reset();
            return result;
        }
    } catch (const std::exception& exception) {
        duplicate.reset();
        return Result::failure(
            "Authored GameObject copy threw: " + std::string(exception.what()));
    } catch (...) {
        duplicate.reset();
        return Result::failure(
            "Authored GameObject copy threw an unknown exception");
    }

    // Persistent identity is intentionally outside TypeRegistry authored
    // properties. Clear any factory default so Scene's centralized insertion
    // policy assigns the identity at commit time.
    duplicate->persistentId.clear();
    return Result::success();
}

Result EditorObjectCoordinator::duplicateIntoScene(
    Scene& scene, const GameObject& source, const TypeRegistry& registry,
    const RendererAttachment& attachRenderer, GameObject*& duplicate) {
    duplicate = nullptr;
    if (!attachRenderer) {
        return Result::failure(
            "Cannot duplicate an object without a renderer attachment path");
    }

    bool sourceBelongsToScene = false;
    for (const auto& owner : scene.gameObjects()) {
        if (owner.get() == &source) {
            sourceBelongsToScene = true;
            break;
        }
    }
    if (!sourceBelongsToScene) {
        return Result::failure(
            "Cannot duplicate an object that is not owned by the Scene");
    }

    std::unique_ptr<GameObject> detachedDuplicate;
    Result result = duplicateAuthoredGameObject(
        source, registry, detachedDuplicate);
    if (!result) {
        return result;
    }

    std::vector<std::string> existingNames;
    try {
        existingNames.reserve(scene.gameObjects().size());
        for (const auto& owner : scene.gameObjects()) {
            if (owner) {
                existingNames.push_back(owner->name);
            }
        }
        detachedDuplicate->name =
            makeDuplicateName(source.name, existingNames);
    } catch (const std::exception& exception) {
        return Result::failure("Failed to generate duplicate name: " +
                               std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to generate duplicate name with an unknown exception");
    }

    result = scene.validateEditorGameObjectInsertion(*detachedDuplicate);
    if (!result) {
        return result;
    }

    GameObject* inserted = nullptr;
    result = scene.addGameObjectForEditor(std::move(detachedDuplicate),
                                          inserted);
    if (!result) {
        return result;
    }

    try {
        result = attachRenderer(scene, *inserted);
    } catch (const std::exception& exception) {
        result = Result::failure("Renderer attachment threw: " +
                                 std::string(exception.what()));
    } catch (...) {
        result = Result::failure(
            "Renderer attachment threw an unknown exception");
    }
    if (!result) {
        std::unique_ptr<GameObject> removed;
        const Result rollback =
            scene.removeGameObjectForEditor(inserted, removed);
        if (!rollback) {
            return Result::failure(
                result.error() + "; Scene rollback failed: " +
                rollback.error());
        }
        return result;
    }

    duplicate = inserted;
    return Result::success();
}

}  // namespace editor
