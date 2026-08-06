#version 450

layout(push_constant) uniform OutlinePushConstants {
    vec4 color;
    vec4 parameters;
} outline;

layout(location = 0) out vec4 outColor;

void main() {
    outColor = outline.color;
}
