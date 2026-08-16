#version 450

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

// UI colors are authored as sRGB hex, and the swapchain is an _SRGB format
// that encodes on write. Without this the whole palette writes out far too
// bright - a dark panel comes back as mid grey.
vec3 srgbToLinear(vec3 color)
{
    bvec3 cutoff = lessThanEqual(color, vec3(0.04045));
    vec3 low = color / 12.92;
    vec3 high = pow((color + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, vec3(cutoff));
}

void main()
{
    // Glyph atlases are single-channel and viewed with a (1,1,1,R) swizzle, so
    // coverage arrives in alpha and this one multiply serves both text and
    // ordinary color textures.
    vec4 texel = texture(uTexture, inUv);
    vec4 color = vec4(srgbToLinear(inColor.rgb) * texel.rgb, inColor.a * texel.a);

    // The alpha blend mode is premultiplied (src.rgb + dst.rgb * (1 - src.a)).
    outColor = vec4(color.rgb * color.a, color.a);
}
