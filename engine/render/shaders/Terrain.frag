#version 450

layout(location = 0) in vec3 inColour;
layout(location = 1) in vec3 inViewPosition;

layout(location = 0) out vec4 outColour;

void main()
{
    // Flat shading from the surface's own orientation, recovered from screen-space derivatives
    // of the camera-relative position. No CPU-side normals are needed, and no extra vertex
    // attribute: the derivative of the interpolated position across a triangle *is* its plane.
    //
    // This is not decoration, and it is what an earlier height-only shading scheme got wrong.
    // Colouring by height alone mapped 8 km of relief onto about 68 luminance levels, so the
    // ~180 m of geometry a LOD transition introduces changed the image by roughly 1.5 levels —
    // invisible, and unmeasurable. Facet *orientation* changes sharply when tessellation
    // changes even where height barely moves, which is exactly why popping is visible in real
    // renderers and why it must be visible here for the gate to test anything.
    vec3 normal = normalize(cross(dFdx(inViewPosition), dFdy(inViewPosition)));

    // A fixed light in view space. The camera-relative frame means this follows the viewer,
    // which is fine: the gate compares consecutive frames of the same path, not absolute
    // appearance.
    vec3 lightDirection = normalize(vec3(0.4, 0.7, 0.6));
    float diffuse = max(dot(normal, lightDirection), 0.0);

    float ambient = 0.25;
    outColour = vec4(inColour * (ambient + (0.75 * diffuse)), 1.0);
}
