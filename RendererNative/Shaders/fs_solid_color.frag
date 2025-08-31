#version 450
layout(push_constant) uniform PC { vec4 color; } pc;
layout(location = 0) out vec4 outCol;
void main()
{
    outCol = pc.color;
}