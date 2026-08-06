#version 450

layout(set = 0, binding = 1) uniform sampler2D baseColorSampler;
layout(location = 0) in vec2 fragTexCoord;

layout(push_constant) uniform MaterialPushConstants {
    vec4 materialBaseColorFactor;
    float materialMetallicFactor;
    float materialRoughnessFactor;
    int materialAlphaMode;
    float materialAlphaCutoff;
    int materialNormalMapEnabled;
    int materialMetallicRoughnessMapEnabled;
};

const int MASK_MODE = 1;

void main() {
    if (materialAlphaMode == MASK_MODE &&
        texture(baseColorSampler, fragTexCoord).a *
            materialBaseColorFactor.a < materialAlphaCutoff) {
        discard;
    }
}
