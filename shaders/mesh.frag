#version 460

layout(push_constant) uniform Push
{
    mat4 mvp;
    vec4 modelColumn0; // .w = ambient
    vec4 modelColumn1; // .w = emissive
    vec4 modelColumn2; // .w = sun intensity
    vec4 sunDirection; // .xyz surface-to-sun, world space
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

    vec3 normal = normalize(vNormal);
    float diffuse = max(dot(normal, pc.sunDirection.xyz), 0.0);
    vec3 albedo = texture(uAlbedo, vUv).rgb;
    outColor = vec4(albedo * (ambient + sunIntensity * diffuse + emissive), 1.0);
}
