#version 460

// The cabin the player sits in (engine plan Phase 25 stage C), and the first
// shader in this engine to take anything a material DECLARED.
//
// ⚑ IT IS mesh.frag PLUS ONE TERM, exactly as membrane.frag is mesh.frag plus
// one flip. That is the shape a material shader wants to have here: the
// lighting model is the engine's - lambert plus ambient plus emissive, one
// albedo, no PBR (Phase 25 decision 6) - and what a material brings is the one
// thing it needs that the model cannot say.
//
// ⚑⚑ SET 0 IS THE ENGINE'S, SET 1 IS THE MATERIAL'S, and this file is the only
// place that fact is visible to somebody writing GLSL. uAlbedo at set 0 is what
// every mesh shader has had since Phase 3 and is bound per draw. Everything at
// set 1 is named in a [[material]] row - IN THIS ORDER, because a binding is a
// number - and the engine refuses at load if the row and this file disagree,
// naming the slot.
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

// [material.textures] glow = "cockpit_glow"
layout(set = 1, binding = 0) uniform sampler2D uGlow;

// [material.params] glow_strength = ...
//
// ⚑ The member name is the contract. Params are matched into this block BY
// NAME rather than by position, so renaming `glow_strength` here without
// renaming it in materials.toml is a refusal at load and not a value that
// quietly lands in the wrong float.
layout(set = 1, binding = 1) uniform Params
{
    float glow_strength;
}
params;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;

layout(location = 0) out vec4 outColor;

void main()
{
    const float ambient = pc.modelColumn0.w;
    const float emissive = pc.modelColumn1.w;
    const float sunIntensity = pc.modelColumn2.w;
    const float alpha = pc.sunDirection.w;

    vec3 normal = normalize(vNormal);
    float diffuse = max(dot(normal, pc.sunDirection.xyz), 0.0);
    vec3 albedo = texture(uAlbedo, vUv).rgb;

    // ⚑ The glow MULTIPLIES the albedo, like every other term here, which is
    // what keeps the mask cheap to author: a strip that bleeds a few pixels off
    // the amber and onto the near-black panelling beside it lights almost
    // nothing, because that panelling has almost no albedo to light.
    float glow = texture(uGlow, vUv).r * params.glow_strength;

    vec3 lit = albedo * (ambient + sunIntensity * diffuse + emissive + glow);
    outColor = vec4(lit * alpha, alpha);
}
