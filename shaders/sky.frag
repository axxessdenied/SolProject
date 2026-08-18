#version 460

layout(push_constant) uniform Push
{
    vec4 right;   // camera right * tan(fovX/2)
    vec4 up;      // camera up * tan(fovY/2)
    vec4 forward; // camera forward, .w = intensity
    // Phase 8v jump tunnel: .xyz = world-space travel axis the streaks
    // converge on, .w = strength (0 at rest, 1 at the swap).
    vec4 warp;
}
pc;

layout(set = 0, binding = 0) uniform samplerCube uSky;

layout(location = 0) in vec2 vNdc;

layout(location = 0) out vec4 outColor;

const int kWarpTaps = 12;

void main()
{
    vec3 direction = normalize(pc.forward.xyz + pc.right.xyz * vNdc.x + pc.up.xyz * vNdc.y);

    vec3 sky;
    // The component of the travel axis perpendicular to this ray, which is the
    // direction "back toward the middle of the tunnel" along the great circle.
    // Its length is sin(angle from the axis): zero dead ahead and growing to
    // the edges, so the still point at the centre and the long streaks at the
    // rim both fall out of the geometry rather than being faked.
    vec3 perpendicular = pc.warp.xyz - direction * dot(direction, pc.warp.xyz);
    float spread = length(perpendicular);
    if (pc.warp.w <= 0.0 || spread < 1.0e-4) {
        sky = texture(uSky, direction).rgb;
    } else {
        vec3 tangent = perpendicular / spread;
        // Smear TOWARD the axis: flying forward, a star's trail lies behind it
        // on the way out from the centre.
        float reach = pc.warp.w * 1.2 * spread;
        vec3 sum = vec3(0.0);
        float weightSum = 0.0;
        for (int i = 0; i < kWarpTaps; ++i) {
            float t = float(i) / float(kWarpTaps - 1);
            vec3 tap = normalize(direction + tangent * (t * reach));
            // The head of the streak stays brightest, so a star still reads as
            // a star being stretched rather than as a uniform grey dash.
            float weight = 1.0 - 0.65 * t;
            sum += texture(uSky, tap).rgb * weight;
            weightSum += weight;
        }
        sky = sum / weightSum;
    }

    outColor = vec4(sky * pc.forward.w, 1.0);
}
