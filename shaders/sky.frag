#version 460

layout(push_constant) uniform Push
{
    vec4 right;   // camera right * tan(fovX/2)
    vec4 up;      // camera up * tan(fovY/2)
    vec4 forward; // camera forward, .w = intensity
}
pc;

layout(set = 0, binding = 0) uniform samplerCube uSky;

layout(location = 0) in vec2 vNdc;

layout(location = 0) out vec4 outColor;

void main()
{
    vec3 direction = normalize(pc.forward.xyz + pc.right.xyz * vNdc.x + pc.up.xyz * vNdc.y);
    outColor = vec4(texture(uSky, direction).rgb * pc.forward.w, 1.0);
}
