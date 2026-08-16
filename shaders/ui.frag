#version 450

layout(location = 0) in vec2 inUv;
layout(location = 1) in vec4 inColor;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

void main()
{
    // Glyph atlases are single-channel and viewed with a (1,1,1,R) swizzle, so
    // coverage arrives in alpha and this one multiply serves both text and
    // ordinary color textures.
    vec4 texel = texture(uTexture, inUv);
    vec4 color = inColor * texel;

    // The alpha blend mode is premultiplied (src.rgb + dst.rgb * (1 - src.a)).
    outColor = vec4(color.rgb * color.a, color.a);
}
