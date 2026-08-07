#version 450

layout(set = 0, binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 cameraPosition;
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;

layout(location = 0) out vec2 fragTexCoord;
layout(location = 1) out vec3 viewNormal;

void main() {
    mat4 viewModel = ubo.view * ubo.model;
    gl_Position = ubo.proj * viewModel * vec4(inPosition, 1.0);
    fragTexCoord = inTexCoord;
    mat3 normalMatrix = transpose(inverse(mat3(viewModel)));
    viewNormal = normalize(normalMatrix * inNormal);
}
