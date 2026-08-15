#version 460

layout(push_constant) uniform Push
{
    vec4 params; // .x = exposure
}
pc;

layout(set = 0, binding = 0) uniform sampler2D uHdr;

layout(location = 0) out vec4 outColor;

// ACES filmic fit (Narkowicz).
vec3 acesTonemap(vec3 x)
{
    return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0);
}

void main()
{
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(uHdr, 0));
    vec3 hdr = texture(uHdr, uv).rgb;
    outColor = vec4(acesTonemap(hdr * pc.params.x), 1.0);
}
