#version 460

layout(location = 0) out vec2 vNdc;

// Fullscreen triangle at reversed-Z far (z = 0): the depth test passes only
// where the scene left the clear value.
void main()
{
    const vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    vNdc = positions[gl_VertexIndex];
    gl_Position = vec4(vNdc, 0.0, 1.0);
}
