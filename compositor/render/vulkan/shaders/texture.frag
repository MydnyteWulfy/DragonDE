#version 450

// Everything the compositor draws (`mode & 7`):
//   0 texture: a client surface or title bar, optionally clipped to a rounded
//     rectangle and faded by alpha
//   1 shadow: a soft drop shadow (or glow) falling off from shapeRect;
//     `angle` is the falloff power
//   2 border: a ring of width shapeSize outside shapeRect, solid or a
//     gradient from color1 to color2 along `angle`
//   3 fill: shapeRect filled with color1 (dimming)
//   4 / 5 blur down / up: one dual-Kawase pass; color1 = (texel x, y, spread)
//   6 blur finish: the blurred backdrop with color1 = (noise, contrast,
//     brightness, vibrancy); with a mask (bit 16), only where the mask
//     texture's alpha (over shapeRect) is at least shapeSize
// Bit 8 inverts the clip (draw only outside clipRect: shadows don't show
// through translucent windows).
// Shapes are signed distance fields: edges come out antialiased.

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv;
    vec4 clipRect;
    vec4 shapeRect;
    vec4 color1;
    vec4 color2;
    float clipRadius;
    float alpha;
    float shapeRadius;
    float shapeSize;
    float angle;
    uint mode;
    uint opaque;
    float pad;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;
layout(set = 0, binding = 1) uniform sampler2D mask;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// Distance from p to a rectangle (x, y, w, h) with corner radius r; < 0 inside.
float roundedBox(vec2 p, vec4 box, float r)
{
    vec2 half_ = box.zw * 0.5;
    r = min(r, min(half_.x, half_.y));
    vec2 q = abs(p - (box.xy + half_)) - half_ + r;
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

// 1 inside, 0 outside, a one-pixel ramp across the edge.
float coverage(float distance)
{
    return clamp(0.5 - distance, 0.0, 1.0);
}

vec4 premultiply(vec4 c)
{
    return vec4(c.rgb * c.a, c.a);
}

float hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

vec4 blurDown(vec2 uv)
{
    vec2 o = pc.color1.xy * pc.color1.z * 0.5;
    vec4 sum = texture(tex, uv) * 4.0;
    sum += texture(tex, uv - o);
    sum += texture(tex, uv + o);
    sum += texture(tex, uv + vec2(o.x, -o.y));
    sum += texture(tex, uv - vec2(o.x, -o.y));
    return sum / 8.0;
}

vec4 blurUp(vec2 uv)
{
    vec2 o = pc.color1.xy * pc.color1.z * 0.5;
    vec4 sum = texture(tex, uv + vec2(-o.x * 2.0, 0.0));
    sum += texture(tex, uv + vec2(-o.x, o.y)) * 2.0;
    sum += texture(tex, uv + vec2(0.0, o.y * 2.0));
    sum += texture(tex, uv + vec2(o.x, o.y)) * 2.0;
    sum += texture(tex, uv + vec2(o.x * 2.0, 0.0));
    sum += texture(tex, uv + vec2(o.x, -o.y)) * 2.0;
    sum += texture(tex, uv + vec2(0.0, -o.y * 2.0));
    sum += texture(tex, uv + vec2(-o.x, -o.y)) * 2.0;
    return sum / 12.0;
}

void main()
{
    vec2 p = gl_FragCoord.xy;
    uint kind = pc.mode & 7u;

    vec4 color;
    if (kind == 0u) {
        color = texture(tex, vUV);
        if (pc.opaque != 0u)
            color.a = 1.0;
    } else if (kind == 1u) {
        float d = roundedBox(p, pc.shapeRect, pc.shapeRadius);
        float t = clamp(d / pc.shapeSize, 0.0, 1.0);
        float falloff = pow(1.0 - t, max(pc.angle, 0.01));
        color = premultiply(vec4(pc.color1.rgb, pc.color1.a * falloff));
    } else if (kind == 2u) {
        float w = pc.shapeSize;
        vec4 outer = vec4(pc.shapeRect.xy - w, pc.shapeRect.zw + 2.0 * w);
        float ring = coverage(roundedBox(p, outer, pc.shapeRadius + w)) *
                     (1.0 - coverage(roundedBox(p, pc.shapeRect, pc.shapeRadius)));
        vec2 dir = vec2(cos(pc.angle), sin(pc.angle));
        vec2 center = outer.xy + outer.zw * 0.5;
        float extent = abs(dir.x) * outer.z + abs(dir.y) * outer.w;
        float g = clamp(dot(p - center, dir) / max(extent, 1.0) + 0.5, 0.0, 1.0);
        color = premultiply(mix(pc.color1, pc.color2, g)) * ring;
    } else if (kind == 3u) {
        color = premultiply(pc.color1) * coverage(roundedBox(p, pc.shapeRect, pc.shapeRadius));
    } else if (kind == 4u) {
        color = blurDown(vUV);
    } else if (kind == 5u) {
        color = blurUp(vUV);
    } else {
        float keep = 1.0; // (no `discard`: it needs an optional device feature)
        if ((pc.mode & 16u) != 0u) {
            vec2 m = (p - pc.shapeRect.xy) / max(pc.shapeRect.zw, vec2(1.0));
            keep = texture(mask, m).a >= pc.shapeSize ? 1.0 : 0.0;
        }
        vec3 c = texture(tex, vUV).rgb;
        c = (c - 0.5) * pc.color1.y + 0.5;
        c *= pc.color1.z;
        float luma = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = mix(vec3(luma), c, 1.0 + pc.color1.w);
        c += (hash(p) - 0.5) * pc.color1.x;
        color = vec4(clamp(c, 0.0, 1.0), 1.0) * keep;
    }

    if (pc.clipRect.z > 0.0) {
        float c = coverage(roundedBox(p, pc.clipRect, pc.clipRadius));
        color *= (pc.mode & 8u) != 0u ? 1.0 - c : c;
    }
    // Premultiplied throughout, so alpha scales every channel.
    outColor = color * pc.alpha;
}
