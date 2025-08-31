#version 450
layout(location = 0) in vec2 in_pos;   // input vertex position in NDC
void main()
{
    gl_Position = vec4(in_pos, 0.0, 1.0);
}