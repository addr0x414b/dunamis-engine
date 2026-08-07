#version 450

layout(location = 0) in vec2 fragUv;
layout(location = 0) out float outVisibility;

layout(set = 0, binding = 0) uniform sampler2D screenDepth;
layout(set = 0, binding = 1) uniform sampler2D screenNormal;
layout(set = 0, binding = 2) uniform sampler2D rawAmbientOcclusion;

const float EPSILON = 1.0e-6;

void main() {
    float centerDepth = texture(screenDepth, fragUv).r;
    vec3 centerNormal = texture(screenNormal, fragUv).xyz;
    float centerNormalLength = dot(centerNormal, centerNormal);
    if (centerDepth >= 1.0 - EPSILON || !(centerNormalLength > EPSILON)) {
        outVisibility = 1.0;
        return;
    }
    centerNormal *= inversesqrt(centerNormalLength);
    vec2 texel = 1.0 / vec2(textureSize(rawAmbientOcclusion, 0));
    float weightedVisibility = 0.0;
    float totalWeight = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            vec2 uv = fragUv + vec2(x, y) * texel;
            float neighborDepth = texture(screenDepth, uv).r;
            vec3 neighborNormal = texture(screenNormal, uv).xyz;
            float neighborNormalLength = dot(neighborNormal, neighborNormal);
            if (neighborDepth >= 1.0 - EPSILON ||
                !(neighborNormalLength > EPSILON)) continue;
            neighborNormal *= inversesqrt(neighborNormalLength);
            float spatial = exp(-0.5 * float(x * x + y * y));
            float depthWeight = exp(-abs(neighborDepth - centerDepth) * 120.0);
            float normalWeight = pow(max(dot(centerNormal, neighborNormal), 0.0), 16.0);
            float weight = spatial * depthWeight * normalWeight;
            weightedVisibility += texture(rawAmbientOcclusion, uv).r * weight;
            totalWeight += weight;
        }
    }
    outVisibility = totalWeight > EPSILON ?
        clamp(weightedVisibility / totalWeight, 0.0, 1.0) : 1.0;
}
