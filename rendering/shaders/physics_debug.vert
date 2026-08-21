#version 450
layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 ignoredModel;
    mat4 view;
    mat4 proj;
    vec3 cameraPosition;
} ubo;
layout(push_constant) uniform DebugPush { mat4 model; } pushData;
layout(location = 0) in vec3 inPosition;
void main() { gl_Position = ubo.proj * ubo.view * pushData.model * vec4(inPosition, 1.0); }
