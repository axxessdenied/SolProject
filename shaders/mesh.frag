#version 460

layout(set = 0, binding = 0) uniform sampler2D uAlbedo;

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUv;

layout(location = 0) out vec4 outColor;

const vec3 kSunDirection = normalize(vec3(0.45, 0.8, 0.3));
const float kAmbient = 0.10;

void main()
{
    vec3 normal = normalize(vNormal);
    float diffuse = max(dot(normal, kSunDirection), 0.0);
    vec3 albedo = texture(uAlbedo, vUv).rgb;
    outColor = vec4(albedo * (kAmbient + (1.0 - kAmbient) * diffuse), 1.0);
}
