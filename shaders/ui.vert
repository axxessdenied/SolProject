#version 450

// Screen-space UI geometry: positions arrive in pixels with the origin at the
// top-left, which is how layout code thinks, and map straight to clip space.

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inUv;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec2 outUv;
layout(location = 1) out vec4 outColor;

layout(push_constant) uniform Push
{
    vec2 inverseScreenSize; // 1 / (width, height) in pixels
}
push;

void main()
{
    const vec2 normalized = inPosition * push.inverseScreenSize;
    gl_Position = vec4(normalized * 2.0 - 1.0, 0.0, 1.0);
    outUv = inUv;
    outColor = inColor;
}
