#version 460

// The gate membrane's own fragment stage (engine plan Phase 25 stage B), and
// the first shader in this engine that reaches the game as DATA: it is named
// by `sol.gate_membrane` in game/data/materials.toml and by nothing in C++.
//
// ⚑⚑ IT EXISTS TO FIX SOMETHING models.toml ALREADY WORRIED ABOUT OUT LOUD.
// The shared `mesh.frag` does not flip the normal for back faces, so lambert
// is all-or-nothing across BOTH faces of a one-sided film at once. The
// membrane survives that only by accident of authoring: its profile normal is
// -Z, which happens to face the star, so it is lit. The def row says as much -
// "wind that profile the other way and it goes black from every angle at the
// same time" - and carries an `emissive` floor as insurance against exactly
// that. A two-sided surface should be lit from whichever side you are looking
// at, and one `gl_FrontFacing` is the whole fix.
//
// ⚑ There is no vertex stage beside this file. The material names
// `vertex_shader = "mesh"`, because the vertex work is identical and a
// byte-identical copy of mesh.vert would be a file nothing binds - which is
// precisely why the def carries two shader keys rather than one stem.
//
// ⚑ Nothing here varies with time. The push block is exactly at the guaranteed
// 128-byte minimum with every .w lane already spent, so a material has nowhere
// to put a parameter of its own until Phase 25 stage C adds a uniform buffer.
// A shimmer or a scanline is stage C's to make possible, not this stage's.

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
    const float alpha = pc.sunDirection.w;

    // The one line this shader exists for. The film has no inside, so a face
    // pointing away from the camera is the same surface seen from behind
    // rather than a hidden one - and it should catch the light the same way.
    vec3 normal = normalize(vNormal);
    if (!gl_FrontFacing) {
        normal = -normal;
    }

    const float diffuse = max(dot(normal, pc.sunDirection.xyz), 0.0);
    const vec3 albedo = texture(uAlbedo, vUv).rgb;
    const vec3 lit = albedo * (ambient + sunIntensity * diffuse + emissive);

    // BlendMode::Alpha is premultiplied (src.rgb + dst.rgb * (1 - src.a)), so
    // the colour goes out already scaled by its own coverage - identical to
    // mesh.frag, because the blend state is the material's business and not
    // the shader's.
    outColor = vec4(lit * alpha, alpha);
}
