#include "model_importer.h"

#include <cmath>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "../assets/imported_model_asset.h"
#include "../math/transform_math.h"
#include "game_object.h"
#include "group.h"
#include "scene.h"

namespace model_import {

Result HierarchyImporter::import(Scene& scene,
                                 const std::filesystem::path& sourcePath,
                                 const Options& options, ImportResult& output) {
    output = {};
    try {
        if (sourcePath.empty()) {
            return Result::failure("Hierarchical model source path is empty");
        }
        if (scene.isActive()) {
            return Result::failure(
                "Hierarchical model importing is supported only "
                "while a scene is inactive");
        }
        if (!transform_math::isFiniteVector(options.position) ||
            !transform_math::isFiniteVector(options.rotation)) {
            return Result::failure(
                "Hierarchical model placement must contain finite transforms");
        }

        std::shared_ptr<const model_loading::ImportedModelAsset> asset;
        const Result loadResult = model_loading::loadImportedModelAsset(
            sourcePath, options.textureFallbackPath, asset);
        if (!loadResult)
            return loadResult;
        if (!asset || asset->nodes.empty() ||
            asset->nodes.front().parentIndex !=
                model_loading::ImportedModelNode::noParent) {
            return Result::failure(
                "Hierarchical model importer returned no valid source root");
        }

        if (options.generateStaticColliders) {
            std::vector<bool> hasMeshDescendant(asset->nodes.size(), false);
            for (std::size_t index = asset->nodes.size(); index > 0; --index) {
                const std::size_t nodeIndex = index - 1;
                for (const std::size_t childIndex :
                     asset->nodes[nodeIndex].children) {
                    if (childIndex >= asset->nodes.size()) {
                        return Result::failure(
                            "Imported source hierarchy contains an invalid "
                            "child index");
                    }
                    hasMeshDescendant[nodeIndex] =
                        hasMeshDescendant[nodeIndex] ||
                        !asset->nodes[childIndex].meshes.empty() ||
                        hasMeshDescendant[childIndex];
                }
                if (!asset->nodes[nodeIndex].meshes.empty() &&
                    hasMeshDescendant[nodeIndex]) {
                    return Result::failure(
                        "Static collider generation cannot place a collider on "
                        "source "
                        "node '" +
                        asset->nodes[nodeIndex].identity +
                        "' because it has mesh-bearing descendants; Dunamis "
                        "requires "
                        "physics bodies to have no physics-enabled ancestors");
                }
            }
        }

        std::vector<std::unique_ptr<GameObject>> objects;
        std::vector<std::size_t> parentIndices;
        const std::size_t noParent =
            model_loading::ImportedModelNode::noParent;
        std::vector<bool> pruned(asset->nodes.size(), false);
        std::vector<std::size_t> nearestRetainedSource(
            asset->nodes.size(), noParent);
        std::vector<std::size_t> sourceToGameIndex(
            asset->nodes.size(), noParent);
        std::vector<glm::mat4> sourceRigidWorlds(
            asset->nodes.size(), glm::mat4(1.0f));
        objects.reserve(asset->nodes.size());
        parentIndices.reserve(asset->nodes.size());

        // Empty, unnamed one-child wrapper nodes do not represent an authored
        // object. Collapse them into their retained descendant while composing
        // their rigid transforms so descendant pivots and world placement stay
        // unchanged. Named or branching organizational nodes remain intact.
        for (std::size_t index = 0; index < asset->nodes.size(); ++index) {
            const model_loading::ImportedModelNode& node = asset->nodes[index];
            pruned[index] = index != 0 && node.nameWasEmpty &&
                            node.meshes.empty() && node.children.size() == 1;

            const glm::mat4 local = transform_math::makeModelMatrix(
                node.position, node.rotation, glm::vec3(1.0f));
            if (node.parentIndex == noParent) {
                if (index != 0) {
                    return Result::failure(
                        "Imported source hierarchy contains multiple roots");
                }
                sourceRigidWorlds[index] = local;
                nearestRetainedSource[index] = index;
            } else {
                if (node.parentIndex >= index) {
                    return Result::failure(
                        "Imported source hierarchy is not in parent-first order");
                }
                sourceRigidWorlds[index] =
                    sourceRigidWorlds[node.parentIndex] * local;
                nearestRetainedSource[index] =
                    pruned[index] ? nearestRetainedSource[node.parentIndex]
                                  : index;
            }
            if (!transform_math::isFiniteMatrix(sourceRigidWorlds[index])) {
                return Result::failure(
                    "Imported source hierarchy contains a non-finite composed transform");
            }
        }

        const glm::mat4 placement = transform_math::makeModelMatrix(
            options.position, options.rotation, glm::vec3(1.0f));
        transform_math::DecomposedTransform placedRoot;
        const Result rootDecomposition = transform_math::decomposeModelMatrix(
            placement * sourceRigidWorlds.front(), placedRoot);
        if (!rootDecomposition) {
            return Result::failure(
                "Initial model placement cannot be represented: " +
                rootDecomposition.error());
        }

        ImportedAssetReference reference;
        reference.sourcePath = sourcePath.lexically_normal().generic_string();
        reference.textureFallbackPath =
            options.textureFallbackPath.lexically_normal().generic_string();

        for (std::size_t index = 0; index < asset->nodes.size(); ++index) {
            const model_loading::ImportedModelNode& importedNode =
                asset->nodes[index];
            if (pruned[index]) continue;

            std::unique_ptr<GameObject> object;
            if (index == 0 || importedNode.meshes.empty()) {
                object = std::make_unique<Group>();
            } else {
                object = std::make_unique<GameObject>();
            }
            object->name = index == 0 && !options.rootName.empty()
                               ? options.rootName
                               : importedNode.name;
            if (index == 0 && options.rootName.empty() &&
                importedNode.nameWasEmpty) {
                object->name = sourcePath.stem().string();
            }
            if (index == 0) {
                object->position = placedRoot.position;
                object->rotation = placedRoot.rotation;
            } else {
                const std::size_t parentSourceIndex =
                    nearestRetainedSource[importedNode.parentIndex];
                if (parentSourceIndex >= index ||
                    sourceToGameIndex[parentSourceIndex] == noParent) {
                    return Result::failure(
                        "Collapsed source hierarchy has an invalid retained parent");
                }
                const glm::mat4 localMatrix =
                    glm::inverse(sourceRigidWorlds[parentSourceIndex]) *
                    sourceRigidWorlds[index];
                transform_math::DecomposedTransform localTransform;
                const Result localDecomposition =
                    transform_math::decomposeModelMatrix(
                        localMatrix, localTransform);
                if (!localDecomposition ||
                    std::fabs(localTransform.scale.x - 1.0f) > 1.0e-3f ||
                    std::fabs(localTransform.scale.y - 1.0f) > 1.0e-3f ||
                    std::fabs(localTransform.scale.z - 1.0f) > 1.0e-3f) {
                    return Result::failure(
                        "Collapsed source hierarchy transform cannot be represented for node '" +
                        importedNode.identity + "'");
                }
                object->position = localTransform.position;
                object->rotation = localTransform.rotation;
                parentIndices.push_back(sourceToGameIndex[parentSourceIndex]);
            }
            // All imported source scale, reflection, and shear has already
            // been preserved in the immutable per-node mesh geometry. Scene
            // node transforms therefore remain rigid, which is required for
            // static Jolt bodies beneath organizational parents.
            object->scale = glm::vec3(1.0f);
            if (options.generateStaticColliders &&
                !importedNode.meshes.empty()) {
                object->physics.enabled = true;
                object->physics.motionType =
                    GameObject::PhysicsMotionType::Static;
                object->physics.colliderType =
                    GameObject::PhysicsColliderType::Mesh;
            }

            reference.nodeIdentity = importedNode.identity;
            const Result referenceResult =
                object->setImportedAssetReference(reference);
            if (!referenceResult) {
                return Result::failure("Failed to instantiate source node '" +
                                       importedNode.identity +
                                       "': " + referenceResult.error());
            }
            if (index == 0) parentIndices.push_back(noParent);
            sourceToGameIndex[index] = objects.size();
            objects.push_back(std::move(object));
        }

        GameObject* root = nullptr;
        const std::size_t instantiatedObjectCount = objects.size();
        const Result insertion =
            scene.addImportedHierarchy(std::move(objects), parentIndices, root);
        if (!insertion)
            return insertion;

        output.root = root;
        output.objectCount = instantiatedObjectCount;
        output.sourceNodeCount = asset->nodes.size();
        output.uniqueSourceMeshCount = asset->uniqueSourceMeshes;
        output.meshReferenceCount = asset->meshReferences;
        output.uniqueGeometryVariantCount = asset->uniqueGeometryVariants;
        try {
            spdlog::info(
                "Imported model hierarchy '{}': nodes={} objects={} "
                "uniqueMeshes={} "
                "meshReferences={} geometryVariants={} staticColliders={}",
                reference.sourcePath, output.sourceNodeCount,
                output.objectCount, output.uniqueSourceMeshCount,
                output.meshReferenceCount, output.uniqueGeometryVariantCount,
                options.generateStaticColliders ? "enabled" : "disabled");
        } catch (...) {
            // Logging is diagnostic only; it cannot turn a committed import
            // into a reported failure.
        }
        return Result::success();
    } catch (const std::exception& exception) {
        output = {};
        return Result::failure("Failed to instantiate model hierarchy '" +
                               sourcePath.string() + "': " + exception.what());
    } catch (...) {
        output = {};
        return Result::failure(
            "Failed to instantiate model hierarchy: unknown error");
    }
}

} // namespace model_import
