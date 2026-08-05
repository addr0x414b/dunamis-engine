#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;
layout(location = 3) in vec3 cameraPos;
layout(location = 4) in vec3 fragPos;
layout(location = 5) in vec3 fragNormal;
layout(location = 6) in vec4 fragTangent;

layout(set = 0, binding = 1) uniform sampler2D baseColorSampler;
layout(set = 0, binding = 2) uniform sampler2D normalSampler;
layout(set = 0, binding = 3) uniform sampler2D metallicRoughnessSampler;

struct LightData {
    vec3 position;
    float padding;
    vec3 color;
    float intensity;
};

layout(set = 1, binding = 0) uniform LightsUBO {
    LightData lights[16];
    vec4 ambientColorIntensity;
    int numLights;
};

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
const float PI = 3.14159265359;
const float EPSILON = 0.0001;

vec3 safeNormalize(vec3 value) {
    float lengthSquared = dot(value, value);
    if (!(lengthSquared > EPSILON * EPSILON)) {
        return vec3(0.0);
    }
    return value * inversesqrt(lengthSquared);
}

float distributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denominatorTerm = NdotH2 * (a2 - 1.0) + 1.0;
    float denominator = PI * denominatorTerm * denominatorTerm;
    return a2 / max(denominator, EPSILON);
}

float geometrySchlickGGX(float NdotX, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotX / max(NdotX * (1.0 - k) + k, EPSILON);
}

float geometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float viewGeometry = geometrySchlickGGX(NdotV, roughness);
    float lightGeometry = geometrySchlickGGX(NdotL, roughness);
    return viewGeometry * lightGeometry;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    float clampedCosTheta = clamp(cosTheta, 0.0, 1.0);
    return F0 + (vec3(1.0) - F0) *
                   pow(1.0 - clampedCosTheta, 5.0);
}

float calculateAttenuation(vec3 fragPosition, vec3 lightPosition) {
    vec3 toLight = lightPosition - fragPosition;
    float distanceSquared = dot(toLight, toLight);
    return 1.0 / max(distanceSquared, EPSILON);
}

void main() {
    vec4 baseColor = texture(baseColorSampler, fragTexCoord) *
                     materialBaseColorFactor;
    if (materialAlphaMode == MASK_MODE &&
        baseColor.a < materialAlphaCutoff) {
        discard;
    }
    vec3 albedo = baseColor.rgb;

    vec3 metallicRoughnessSample = vec3(1.0);
    if (materialMetallicRoughnessMapEnabled != 0) {
        metallicRoughnessSample =
            texture(metallicRoughnessSampler, fragTexCoord).rgb;
    }
    float metallicFactor = clamp(materialMetallicFactor, 0.0, 1.0);
    float roughnessFactor = clamp(materialRoughnessFactor, 0.0, 1.0);
    float metallic = clamp(metallicRoughnessSample.b * metallicFactor,
                           0.0, 1.0);
    float roughness = max(
        clamp(metallicRoughnessSample.g * roughnessFactor, 0.0, 1.0), 0.04);

    vec3 ambient = albedo * ambientColorIntensity.rgb *
                   ambientColorIntensity.a;
    vec3 finalColor = ambient;
    vec3 normal = safeNormalize(fragNormal);
    if (!(dot(normal, normal) > EPSILON)) {
        normal = vec3(0.0, 0.0, 1.0);
    }
    if (materialNormalMapEnabled != 0) {
        vec3 tangentNormal = texture(normalSampler, fragTexCoord).xyz * 2.0 - 1.0;
        vec3 tangent = safeNormalize(fragTangent.xyz);
        if (dot(tangent, tangent) > EPSILON) {
            tangent = safeNormalize(tangent - dot(tangent, normal) * normal);
            vec3 bitangent = safeNormalize(cross(normal, tangent)) *
                             fragTangent.w;
            mat3 TBN = mat3(tangent, bitangent, normal);
            normal = safeNormalize(TBN * safeNormalize(tangentNormal));
            if (!(dot(normal, normal) > EPSILON)) {
                normal = safeNormalize(fragNormal);
            }
        }
    }

    vec3 viewDirection = safeNormalize(cameraPos - fragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);
    int lightCount = min(numLights, 16);
    for (int i = 0; i < lightCount; ++i) {
        vec3 lightPos = lights[i].position;
        vec3 lightColor = lights[i].color;
        float lightIntensity = lights[i].intensity * 100.0;

        vec3 lightVector = lightPos - fragPos;
        float distanceSquared = dot(lightVector, lightVector);
        if (!(distanceSquared > EPSILON)) {
            continue;
        }

        vec3 lightDirection = lightVector * inversesqrt(distanceSquared);
        vec3 halfwayDirection = safeNormalize(viewDirection + lightDirection);
        float NdotL = max(dot(normal, lightDirection), 0.0);
        float NdotV = max(dot(normal, viewDirection), 0.0);
        if (NdotL <= 0.0 || NdotV <= 0.0 ||
            !(dot(halfwayDirection, halfwayDirection) > EPSILON)) {
            continue;
        }

        float attenuation = calculateAttenuation(fragPos, lightPos);
        vec3 radiance = lightColor * lightIntensity * attenuation;
        vec3 F = fresnelSchlick(dot(halfwayDirection, viewDirection), F0);
        float D = distributionGGX(normal, halfwayDirection, roughness);
        float G = geometrySmith(normal, viewDirection, lightDirection,
                                roughness);

        vec3 numerator = D * G * F;
        float denominator = max(4.0 * NdotV * NdotL, EPSILON);
        vec3 specular = numerator / denominator;

        vec3 kS = F;
        vec3 kD = (vec3(1.0) - kS) * (1.0 - metallic);
        vec3 direct = (kD * albedo / PI + specular) * radiance * NdotL;
        finalColor += direct;
    }

    outColor = vec4(finalColor, 1.0);
}
