#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
    vec3 cameraPosition;
} ubo;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;
layout(location = 3) in vec3 inNormal;
layout(location = 4) in vec4 inTangent;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;
layout(location = 3) out vec3 cameraPos;
layout(location = 4) out vec3 fragPos;
layout(location = 5) out vec3 fragNormal;
layout(location = 6) out vec4 fragTangent;

void main() {
    gl_Position = ubo.proj * ubo.view * ubo.model * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
    cameraPos = ubo.cameraPosition;
    fragPos = vec3(ubo.model * vec4(inPosition, 1.0));
    mat3 normalMatrix = transpose(inverse(mat3(ubo.model)));
    vec3 normal = normalize(normalMatrix * inNormal);
    vec3 tangent = mat3(ubo.model) * inTangent.xyz;
    tangent = tangent - dot(tangent, normal) * normal;
    if (dot(tangent, tangent) > 0.0) {
        tangent = normalize(tangent);
    }
    fragNormal = normal;
    fragTangent = vec4(tangent, inTangent.w);
}
