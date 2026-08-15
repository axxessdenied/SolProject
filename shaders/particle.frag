#version 460

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vCorner;

layout(location = 0) out vec4 outColor;

void main()
{
    // Soft radial falloff; additive blending, so alpha rides in the color.
    float r2 = dot(vCorner, vCorner);
    float falloff = exp(-r2 * 3.5) * max(1.0 - r2, 0.0);
    outColor = vec4(vColor.rgb * (vColor.a * falloff), 0.0);
}
