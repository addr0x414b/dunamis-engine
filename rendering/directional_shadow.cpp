#include "directional_shadow.h"

#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

namespace directional_shadow {
namespace {

constexpr float parallelUpThreshold = 0.99f;

bool isFiniteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

Result validateSettings(const DirectionalLight& light) {
    const DirectionalShadowSettings& settings = light.shadow;
    if (!isFiniteVector(settings.focus)) {
        return Result::failure("Directional shadow focus must be finite");
    }
    if (!std::isfinite(settings.halfExtent) ||
        !std::isfinite(settings.lightDistance) ||
        !std::isfinite(settings.nearPlane) ||
        !std::isfinite(settings.farPlane) || settings.halfExtent <= 0.0f ||
        settings.lightDistance <= 0.0f || settings.nearPlane < 0.0f ||
        settings.farPlane <= settings.nearPlane) {
        return Result::failure("Directional shadow settings are invalid");
    }
    return Result::success();
}

}  // namespace

bool isFiniteMatrix(const glm::mat4& matrix) noexcept {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) return false;
        }
    }
    return true;
}

Result calculateLightMatrices(const DirectionalLight& light,
                              LightMatrices& matrices) {
    Result validation = validateSettings(light);
    if (!validation) return validation;

    glm::vec3 direction;
    if (!light.calculateWorldDirection(direction)) {
        return Result::failure(
            "Directional light rotation does not produce a valid direction");
    }

    const DirectionalShadowSettings& settings = light.shadow;
    const glm::vec3 lightPosition = settings.focus - direction *
                                                    settings.lightDistance;
    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, up)) > parallelUpThreshold) {
        up = glm::vec3(1.0f, 0.0f, 0.0f);
    }

    glm::mat4 projection = glm::ortho(-settings.halfExtent,
                                      settings.halfExtent,
                                      -settings.halfExtent,
                                      settings.halfExtent,
                                      settings.nearPlane,
                                      settings.farPlane);
    // GLM produces mathematical Y-up clip coordinates; Vulkan's viewport
    // convention needs this deliberate projection-space Y inversion.
    projection[1][1] *= -1.0f;
    const glm::mat4 view = glm::lookAt(lightPosition, settings.focus, up);
    const glm::mat4 viewProjection = projection * view;
    if (!isFiniteMatrix(view) || !isFiniteMatrix(projection) ||
        !isFiniteMatrix(viewProjection)) {
        return Result::failure("Directional shadow matrices are not finite");
    }

    matrices.normalizedDirection = direction;
    matrices.view = view;
    matrices.projection = projection;
    matrices.viewProjection = viewProjection;
    return Result::success();
}

bool projectWorldPoint(const glm::mat4& lightViewProjection,
                       const glm::vec3& worldPoint,
                       glm::vec3& textureCoordinates) noexcept {
    textureCoordinates = {};
    if (!isFiniteMatrix(lightViewProjection) || !isFiniteVector(worldPoint)) {
        return false;
    }
    const glm::vec4 clip = lightViewProjection * glm::vec4(worldPoint, 1.0f);
    if (!std::isfinite(clip.w) || std::abs(clip.w) < 1.0e-6f) return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (!isFiniteVector(ndc)) return false;
    textureCoordinates = glm::vec3(ndc.x * 0.5f + 0.5f,
                                   ndc.y * 0.5f + 0.5f, ndc.z);
    return textureCoordinates.x >= 0.0f && textureCoordinates.x <= 1.0f &&
           textureCoordinates.y >= 0.0f && textureCoordinates.y <= 1.0f &&
           textureCoordinates.z >= 0.0f && textureCoordinates.z <= 1.0f;
}

}  // namespace directional_shadow
