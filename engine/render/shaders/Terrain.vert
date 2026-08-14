#version 450

// Terrain vertex with continuous LOD morphing.
//
// Both positions arrive already camera-relative, in metres, narrowed from double on the CPU.
// The GPU never sees a planet-relative coordinate: at ~6.4e6 m a float quantises to half a
// metre, and subtracting the camera here would be catastrophic cancellation of two large
// nearly-equal values.
//
// The mix below is the whole LOD continuity claim. As a patch approaches the distance at which
// its parent takes over, `morph` runs to 1 and the fine mesh continuously becomes the coarse
// one — so the switch happens when the two geometries are already identical, and there is
// nothing left to pop. With morph forced to 0 (the negative control) the switch is abrupt.

layout(push_constant) uniform Push {
    mat4 viewProjection;
    // x = morph band start distance, y = morph band end distance,
    // z = 1 when morphing is enabled and 0 for the negative control, w = LOD level.
    vec4 params;
    vec4 unused;
} push;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inCoarsePosition;
layout(location = 2) in float inHeightUnit;
layout(location = 3) in float inCoarseHeightUnit;

layout(location = 0) out vec3 outColour;
layout(location = 1) out vec3 outViewPosition;

void main()
{
    // Morph evaluated per vertex, from this vertex's own distance to the camera.
    //
    // Positions arrive camera-relative, so `length(inPosition)` *is* that distance and no
    // extra data is needed. This is what makes the factor a continuous function of position:
    // two adjacent patches evaluate the same value at a shared vertex, so their common edge is
    // one polyline rather than two, and there is no crack. A single per-patch factor cannot
    // have that property, because neighbouring patch centres are a patch width apart.
    float distance = length(inPosition);
    float bandStart = push.params.x;
    float bandEnd = push.params.y;
    float band = max(bandEnd - bandStart, 1e-6);
    float morph = push.params.z * clamp((distance - bandStart) / band, 0.0, 1.0);

    vec3 position = mix(inPosition, inCoarsePosition, morph);
    float height = mix(inHeightUnit, inCoarseHeightUnit, morph);

    gl_Position = push.viewProjection * vec4(position, 1.0);
    outViewPosition = position;

    // Shading is a smooth function of terrain height only. Deliberately not of the LOD level:
    // colouring by level would paint a hard seam at exactly the transitions the gate measures,
    // and the popping detector would then be measuring the debug colouring rather than the
    // geometry it is supposed to be testing.
    vec3 low = vec3(0.18, 0.24, 0.35);
    vec3 high = vec3(0.62, 0.58, 0.50);
    outColour = mix(low, high, clamp(height, 0.0, 1.0));
}
