#ifndef CAMERA_PROJECTION_H
#define CAMERA_PROJECTION_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace camera_projection {

inline glm::mat4 makeVulkanPerspective(float verticalFovDegrees,
                                       float aspectRatio,
                                       float nearClipMeters,
                                       float farClipMeters) noexcept {
    glm::mat4 projection = glm::perspective(
        glm::radians(verticalFovDegrees), aspectRatio, nearClipMeters,
        farClipMeters);
    projection[1][1] *= -1.0f;
    return projection;
}

}  // namespace camera_projection

#endif
