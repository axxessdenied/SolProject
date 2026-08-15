#version 460

layout(push_constant) uniform Push
{
    mat4 mvp;
    vec4 modelColumn0; // .w = ambient
    vec4 modelColumn1; // .w = emissive
    vec4 modelColumn2; // .w = sun intensity
    vec4 sunDirection; // .xyz surface-to-sun, world space
}
pc;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUv;

void main()
{
    gl_Position = pc.mvp * vec4(inPosition, 1.0);
    const mat3 normalMatrix =
        mat3(pc.modelColumn0.xyz, pc.modelColumn1.xyz, pc.modelColumn2.xyz);
    vNormal = normalMatrix * inNormal;
    vUv = inUv;
}
