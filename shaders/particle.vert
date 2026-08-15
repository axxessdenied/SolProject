#version 460

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 cameraRight;
    vec4 cameraUp;
}
pc;

layout(location = 0) in vec3 inPosition; // camera-relative billboard center
layout(location = 1) in vec2 inCorner;   // [-1,1] quad corner
layout(location = 2) in vec4 inColor;
layout(location = 3) in float inSize;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vCorner;

void main()
{
    vec3 world = inPosition + (pc.cameraRight.xyz * inCorner.x + pc.cameraUp.xyz * inCorner.y) * inSize;
    gl_Position = pc.viewProjection * vec4(world, 1.0);
    vColor = inColor;
    // Fade out as the camera flies through the exhaust stream.
    vColor.a *= smoothstep(1.5, 10.0, length(inPosition));
    vCorner = inCorner;
}
