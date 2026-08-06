#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 cameraPosition;
} objectUbo;

layout(std140, set = 1, binding = 0) uniform DirectionalShadowUBO {
    mat4 lightViewProjection;
} directionalShadow;

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inTexCoord;
layout(location = 0) out vec2 fragTexCoord;

void main() {
    gl_Position = directionalShadow.lightViewProjection * objectUbo.model *
                  vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
}
