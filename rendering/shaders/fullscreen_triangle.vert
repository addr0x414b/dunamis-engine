#version 450

layout(location = 0) out vec2 outUv;

void main() {
    const vec2 positions[3] = vec2[](vec2(-1.0, -1.0),
                                     vec2(3.0, -1.0),
                                     vec2(-1.0, 3.0));
    vec2 position = positions[gl_VertexIndex];
    gl_Position = vec4(position, 0.0, 1.0);
    // Vulkan's viewport and sampled-image coordinates both use the same
    // top-to-bottom convention here, so this remains pixel aligned.
    outUv = position * 0.5 + 0.5;
}
