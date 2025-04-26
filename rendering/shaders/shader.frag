#version 450

layout(location = 0) in vec3 fragColor;
layout(location = 1) in vec2 fragTexCoord;
layout(location = 0) out vec4 outColor;
layout(location = 3) in vec3 cameraPos;
layout(location = 4) in vec3 fragPos;

layout(set = 0, binding = 1) uniform sampler2D texSampler;

struct LightData {
    vec3 position;
    float padding;
    vec3 color;
    float intensity;
};

layout(set = 1, binding = 0) uniform LightsUBO {
    LightData lights[16];
    int numLights;
};

float BurleyDiffuse(float NdotL, float NdotV, float roughness) {
    float fd90 = 0.5 + 2.0 * pow(NdotL, 2.0) * roughness;
    float lightScatter = 1.0 + (fd90 - 1.0) * pow(1.0 - NdotL, 5.0);
    float viewScatter  = 1.0 + (fd90 - 1.0) * pow(1.0 - NdotV, 5.0);
    return lightScatter * viewScatter;
}

float calculateAttenuation(vec3 fragPos, vec3 lightPos) {
    float distance = length(fragPos - lightPos);  // Distance between the fragment and the light
    float attenuation = 1.0 / (distance * distance);  // Simple inverse square attenuation
    return attenuation;
}

void main() {
        // Sample the texture (albedo)
    vec3 albedo = texture(texSampler, fragTexCoord).rgb;

    // Initialize the final color to black
    vec3 finalColor = vec3(0.0);

    // Loop over all lights
    for (int i = 0; i < numLights; ++i) {
        // Get light position and color
        vec3 lightPos = lights[i].position;
        vec3 lightColor = lights[i].color;
        float lightIntensity = lights[i].intensity * 100.0;

        // Calculate the direction from the fragment to the light
        vec3 lightDir = normalize(lightPos - fragPos);  // Direction to the light
        vec3 normal = normalize(vec3(0.0, 0.0, 1.0));  // Fake normal (for simplicity)

        // Diffuse lighting (dot product of normal and light direction)
        float NdotL = max(dot(normal, lightDir), 0.0);

        // Calculate the attenuation based on the distance between the fragment and the light
        float attenuation = calculateAttenuation(fragPos, lightPos);

        // Apply lighting: diffuse * color * intensity * attenuation
        vec3 diffuse = albedo * lightColor * lightIntensity * attenuation;

        // Add the light's contribution to the final color
        finalColor += diffuse;
    }

    // Output the final color with full alpha (1.0)
    outColor = vec4(finalColor, 1.0);
}
