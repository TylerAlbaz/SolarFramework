#version 450
layout(location = 0) in vec3 in_pos;

layout(push_constant) uniform Push {
    mat4 uMVP;
    vec4 uColor;
} pc;

layout(location = 0) out vec4 vColor;

void main() {
    gl_Position = pc.uMVP * vec4(in_pos, 1.0);
    vColor = pc.uColor;
}