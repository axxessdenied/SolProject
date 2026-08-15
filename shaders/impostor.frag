#version 460

layout(push_constant) uniform Push
{
    mat4 viewProjection;
    vec4 center; // camera-relative, .w = radius
    vec4 colorA; // .w = mode (0 planet, 1 star)
    vec4 colorB; // .w = quad scale
    vec4 sunDirection;
}
pc;

layout(location = 0) in vec3 vRayTarget;

layout(location = 0) out vec4 outColor;

float hash3(vec3 p)
{
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}

float valueNoise(vec3 p)
{
    vec3 cell = floor(p);
    vec3 t = fract(p);
    t = t * t * (3.0 - 2.0 * t);
    float c000 = hash3(cell + vec3(0, 0, 0));
    float c100 = hash3(cell + vec3(1, 0, 0));
    float c010 = hash3(cell + vec3(0, 1, 0));
    float c110 = hash3(cell + vec3(1, 1, 0));
    float c001 = hash3(cell + vec3(0, 0, 1));
    float c101 = hash3(cell + vec3(1, 0, 1));
    float c011 = hash3(cell + vec3(0, 1, 1));
    float c111 = hash3(cell + vec3(1, 1, 1));
    return mix(mix(mix(c000, c100, t.x), mix(c010, c110, t.x), t.y),
               mix(mix(c001, c101, t.x), mix(c011, c111, t.x), t.y), t.z);
}

void main()
{
    vec3 ray = normalize(vRayTarget);
    vec3 center = pc.center.xyz;
    float radius = pc.center.w;

    if (pc.colorA.w > 0.5) {
        // Star: emissive disc + additive glow halo, no depth interaction.
        float distanceToCenter = length(center);
        float angularRadius = radius / distanceToCenter;
        vec3 centerDirection = center / distanceToCenter;
        float angle = acos(clamp(dot(ray, centerDirection), -1.0, 1.0));
        float x = angle / angularRadius;
        float disc = 1.0 - smoothstep(0.9, 1.0, x);
        float glow = exp(-x * 0.9) * 0.35;
        outColor = vec4(pc.colorA.rgb * disc + pc.colorB.rgb * glow, 0.0);
        gl_FragDepth = 0.0; // reversed-Z: as far as the sky
        return;
    }

    // Planet: analytic ray-sphere intersection from the camera at the origin.
    float b = dot(ray, center);
    float discriminant = b * b - dot(center, center) + radius * radius;
    if (discriminant < 0.0) {
        discard;
    }
    float t = b - sqrt(discriminant);
    if (t < 0.0) {
        discard;
    }
    vec3 hit = ray * t;
    vec3 normal = normalize(hit - center);

    // Procedural surface: banded continents from low-frequency noise.
    float continents = valueNoise(normal * 4.0) * 0.65 + valueNoise(normal * 11.0) * 0.35;
    vec3 albedo = mix(pc.colorA.rgb, pc.colorB.rgb, smoothstep(0.42, 0.62, continents));

    float light = max(dot(normal, pc.sunDirection.xyz), 0.0);
    // Slight rim darkening sells the sphere even at partial phase.
    float rim = max(dot(normal, -ray), 0.0);
    vec3 lit = albedo * (0.004 + light * (0.75 + 0.25 * rim));
    outColor = vec4(lit, 1.0);

    vec4 clip = pc.viewProjection * vec4(hit, 1.0);
    gl_FragDepth = clip.z / clip.w;
}
