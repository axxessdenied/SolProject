#version 460

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 center; // camera-relative, .w = radius
    vec4 colorA; // .w = mode (0 planet, 1 star)
    vec4 colorB; // .w = quad scale
    vec4 sunDirection;
}
pc;

layout(location = 0) out vec3 vRayTarget; // camera-relative position on the quad

// Camera-facing quad; the camera sits at the origin in camera-relative space.
void main()
{
    const vec2 corners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                   vec2(-1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));

    vec3 toCenter = normalize(pc.center.xyz);
    vec3 reference = abs(toCenter.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(reference, toCenter));
    vec3 up = cross(toCenter, right);

    float halfSize = pc.center.w * pc.colorB.w;
    vec2 corner = corners[gl_VertexIndex];
    vec3 world = pc.center.xyz + (right * corner.x + up * corner.y) * halfSize;

    vRayTarget = world;
    gl_Position = pc.viewProjection * vec4(world, 1.0);
}
