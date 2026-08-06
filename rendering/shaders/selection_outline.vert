#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 cameraPosition;
} ubo;

layout(push_constant) uniform OutlinePushConstants {
    vec4 color;
    vec4 parameters;
} outline;

layout(location = 0) in vec3 inPosition;
layout(location = 3) in vec3 inNormal;

void main() {
    vec4 clipPosition =
        ubo.proj * ubo.view * ubo.model * vec4(inPosition, 1.0);
    mat3 normalMatrix = transpose(inverse(mat3(ubo.view * ubo.model)));
    vec3 viewNormal = normalize(normalMatrix * inNormal);
    vec2 projectedDirection = (ubo.proj * vec4(viewNormal, 0.0)).xy;
    float directionLength = length(projectedDirection);

    if (directionLength > 0.00001 && outline.parameters.y > 0.0 &&
        outline.parameters.z > 0.0) {
        vec2 direction = projectedDirection / directionLength;
        vec2 ndcPerPixel = vec2(2.0 / outline.parameters.y,
                                 2.0 / outline.parameters.z);
        clipPosition.xy += direction * outline.parameters.x * ndcPerPixel *
                           clipPosition.w;
    }

    gl_Position = clipPosition;
}
