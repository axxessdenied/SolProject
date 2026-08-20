#version 460

layout(push_constant) uniform Push
{
    mat4 mvp;
    vec4 modelColumn0; // .w = ambient
    vec4 modelColumn1; // .w = emissive
    vec4 modelColumn2; // .w = sun intensity
    vec4 sunDirection; // .xyz surface-to-sun, world space; .w = alpha
}
pc;

layout(set = 0, binding = 0) uniform sampler2D uAlbedo;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;

layout(location = 0) out vec4 outColor;

void main()
{
    const float ambient = pc.modelColumn0.w;
    const float emissive = pc.modelColumn1.w;
    const float sunIntensity = pc.modelColumn2.w;

    // Phase 12: the alpha lane rides in sunDirection.w, which was the only
    // dead component left in a push block that is exactly at the guaranteed
    // 128-byte minimum. The opaque pipeline passes 1.0 and blending is off for
    // it anyway, so its output is unchanged by construction rather than by
    // inspection - premultiplying by 1.0 is the identity.
    const float alpha = pc.sunDirection.w;

    vec3 normal = normalize(vNormal);
    float diffuse = max(dot(normal, pc.sunDirection.xyz), 0.0);
    vec3 albedo = texture(uAlbedo, vUv).rgb;
    vec3 lit = albedo * (ambient + sunIntensity * diffuse + emissive);

    // BlendMode::Alpha is premultiplied (src.rgb + dst.rgb * (1 - src.a)), so
    // the colour goes out already scaled by its own coverage.
    outColor = vec4(lit * alpha, alpha);
}
