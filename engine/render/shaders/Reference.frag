#version 450

layout(location = 0) in vec3 inColour;
layout(location = 1) in vec2 inLocal;
layout(location = 2) in float inMarkerMode;

layout(location = 0) out vec4 outColour;

void main()
{
    if (inMarkerMode < 0.5) {
        outColour = vec4(inColour, 1.0);
        return;
    }

    // Measurement marker: a smooth radial intensity profile rather than a hard-edged shape.
    //
    // This is not cosmetic. The screen-space jitter gate computes the marker's centroid to
    // sub-pixel accuracy, and a hard-edged marker cannot support that: with no antialiasing,
    // pixel coverage is binary, so the rasterised footprint changes in whole-pixel steps and
    // the centroid quantises to roughly a pixel. A gate of 0.25 px would then be unmeasurable
    // — a zero reading would mean "below the quantisation floor", not "stable".
    //
    // A smooth profile puts the sub-pixel information into intensity, where the luminance
    // weighted centroid can read it. This is the standard centroiding approach in astrometry,
    // and it is what makes the measured number mean what the gate says it means.
    // No discard, and a falloff steep enough that the profile is already below the harness's
    // detection threshold before it reaches the edge of the face it is drawn on.
    //
    // Both details matter for the same reason. Any hard boundary — a discard circle, or the
    // square edge of the geometry — makes pixels pop in and out discretely as the marker moves
    // sub-pixel, which puts whole-pixel quantisation straight back into the centroid. A profile
    // that decays smoothly to below threshold on its own has no such boundary anywhere.
    float r = length(inLocal);
    float intensity = exp(-12.0 * r * r);
    outColour = vec4(inColour * intensity, 1.0);
}
