#version 450

layout(location = 0) in vec3 inColour;
layout(location = 1) in vec3 inViewPosition;

layout(location = 0) out vec4 outColour;

void main()
{
    // Flat shading from the surface's own orientation, recovered from screen-space derivatives
    // of the camera-relative position. No CPU-side normals and no extra vertex attribute: the
    // derivative of the interpolated position across a triangle *is* its plane.
    //
    // Lighting is not decoration here. Colouring by height alone mapped the relief onto about
    // 68 luminance levels, so the geometry a LOD transition introduces changed the image by
    // roughly a luminance level — invisible, and unmeasurable. Facet orientation changes
    // sharply when tessellation changes even where height barely moves, which is why popping
    // is visible in real renderers and why it must be visible here for the gate to test
    // anything.
    //
    // The known weakness of the derivative form is that at full morph half a patch's triangles
    // are exactly degenerate and their derivative is undefined. Replacing this with morphed
    // per-vertex normals was tried and measured *worse* — see the note in TerrainLod.h — so
    // the weaker-looking version is the one that stays until a patch skirt makes the
    // alternative correct at edges.
    vec3 normal = normalize(cross(dFdx(inViewPosition), dFdy(inViewPosition)));

    vec3 lightDirection = normalize(vec3(0.4, 0.7, 0.6));
    float diffuse = max(dot(normal, lightDirection), 0.0);

    float ambient = 0.25;
    outColour = vec4(inColour * (ambient + (0.75 * diffuse)), 1.0);
}
