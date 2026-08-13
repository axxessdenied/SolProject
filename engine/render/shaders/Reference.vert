#version 450

// Camera-relative reference geometry.
//
// Positions arrive already expressed relative to the camera, in metres, as 32-bit floats. That
// is the whole point of the frame model selected in P1a increment A2: the GPU never sees a
// world coordinate, so it never needs the precision a world coordinate would demand. A vertex
// 400 km away is a float with ~30 mm of resolution; the same vertex in Solar System
// barycentric coordinates would not survive being a float at all.

layout(push_constant) uniform Push {
    mat4 viewProjection;
    vec4 colour;
    // xyz = object origin relative to the camera, in metres, already narrowed from double
    // on the CPU after the subtraction. w = object radius in metres.
    vec4 originAndRadius;
} push;

layout(location = 0) in vec3 inPosition;

layout(location = 0) out vec3 outColour;
layout(location = 1) out vec2 outLocal;
layout(location = 2) out float outMarkerMode;

void main()
{
    vec3 cameraRelative = push.originAndRadius.xyz + (inPosition * push.originAndRadius.w);
    gl_Position = push.viewProjection * vec4(cameraRelative, 1.0);
    outColour = push.colour.rgb;
    // Object-space position across the face, for the measurement marker's radial profile.
    outLocal = inPosition.xy;
    outMarkerMode = push.colour.a;
}
