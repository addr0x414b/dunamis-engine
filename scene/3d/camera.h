#ifndef CAMERA_H
#define CAMERA_H

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>

class Camera {
public:
    //glm::vec3 position = glm::vec3(0.0f, 0.0f, 3.0f);
    //glm::vec3 front = glm::vec3(0.0f, 0.0f, -3.0f);
    //glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    //glm::vec3 right = glm::vec3(1.0f, 0.0f, 0.0f);

    glm::vec3 position = glm::vec3(0.0f, 0.0f, 3.0f);
    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

    const char* name = "Camera";


private:
};

#endif  // CAMERA_H