#version 450

layout(location = 0) in vec2 fragUv;
layout(location = 0) out float outVisibility;

layout(set = 0, binding = 0) uniform sampler2D screenDepth;
layout(set = 0, binding = 1) uniform sampler2D screenNormal;

layout(std140, set = 0, binding = 4) uniform AmbientOcclusionUBO {
    mat4 projection;
    mat4 inverseProjection;
    vec4 samples[32];
    vec4 parameters;
    vec4 viewport;
} ao;

const float EPSILON = 1.0e-6;

bool finiteFloat(float value) { return !isnan(value) && !isinf(value); }
bool finiteVec3(vec3 value) {
    return finiteFloat(value.x) && finiteFloat(value.y) && finiteFloat(value.z);
}

vec3 reconstructViewPosition(vec2 uv, float depth) {
    vec4 view = ao.inverseProjection * vec4(uv * 2.0 - 1.0, depth, 1.0);
    if (!finiteFloat(view.w) || abs(view.w) < EPSILON) return vec3(0.0);
    return view.xyz / view.w;
}

vec2 rotationForPixel(vec2 pixel) {
    // Isolated deterministic hash: replace with blue noise later without
    // changing descriptors or the AO resource layout.
    float angle = fract(sin(dot(pixel, vec2(12.9898, 78.233))) * 43758.5453) *
                  6.28318530718;
    return vec2(cos(angle), sin(angle));
}

void main() {
    if (ao.parameters.w < 0.5) {
        outVisibility = 1.0;
        return;
    }
    float depth = texture(screenDepth, fragUv).r;
    if (!finiteFloat(depth) || depth >= 1.0 - EPSILON) {
        outVisibility = 1.0;
        return;
    }
    vec3 viewPosition = reconstructViewPosition(fragUv, depth);
    vec3 normal = texture(screenNormal, fragUv).xyz;
    float normalLengthSquared = dot(normal, normal);
    if (!finiteVec3(viewPosition) || !finiteVec3(normal) ||
        !(normalLengthSquared > EPSILON)) {
        outVisibility = 1.0;
        return;
    }
    normal *= inversesqrt(normalLengthSquared);
    vec2 rotation = rotationForPixel(gl_FragCoord.xy);
    vec3 randomVector = vec3(rotation, 0.0);
    vec3 tangent = randomVector - normal * dot(randomVector, normal);
    float tangentLengthSquared = dot(tangent, tangent);
    if (!(tangentLengthSquared > EPSILON)) {
        tangent = abs(normal.z) < 0.9 ? cross(normal, vec3(0.0, 0.0, 1.0))
                                     : cross(normal, vec3(0.0, 1.0, 0.0));
        tangentLengthSquared = dot(tangent, tangent);
    }
    if (!(tangentLengthSquared > EPSILON)) {
        outVisibility = 1.0;
        return;
    }
    tangent *= inversesqrt(tangentLengthSquared);
    vec3 bitangent = cross(normal, tangent);
    mat3 tbn = mat3(tangent, bitangent, normal);
    float occlusion = 0.0;
    const float radius = ao.parameters.x;
    const float bias = ao.parameters.y;
    for (int index = 0; index < 32; ++index) {
        vec3 samplePosition = viewPosition + tbn * ao.samples[index].xyz * radius;
        vec4 clip = ao.projection * vec4(samplePosition, 1.0);
        if (!finiteFloat(clip.w) || abs(clip.w) < EPSILON) continue;
        vec3 ndc = clip.xyz / clip.w;
        if (!finiteVec3(ndc)) continue;
        vec2 sampleUv = ndc.xy * 0.5 + 0.5;
        if (any(lessThan(sampleUv, vec2(0.0))) ||
            any(greaterThan(sampleUv, vec2(1.0)))) continue;
        float sampledDepth = texture(screenDepth, sampleUv).r;
        if (!finiteFloat(sampledDepth) || sampledDepth >= 1.0 - EPSILON) continue;
        vec3 sampledPosition = reconstructViewPosition(sampleUv, sampledDepth);
        if (!finiteVec3(sampledPosition)) continue;
        float range = smoothstep(0.0, 1.0, radius /
            max(abs(viewPosition.z - sampledPosition.z), EPSILON));
        // View-space camera looks down -Z: a larger Z is closer to the camera.
        occlusion += sampledPosition.z > samplePosition.z + bias ? range : 0.0;
    }
    float visibility = 1.0 - occlusion / 32.0;
    outVisibility = clamp(pow(clamp(visibility, 0.0, 1.0), ao.parameters.z),
                          0.0, 1.0);
}
