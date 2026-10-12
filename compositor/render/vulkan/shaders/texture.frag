#version 450

layout(push_constant) uniform Push {
    vec4 rect;
    vec4 uv;
    float alpha;
    uint opaque;
} pc;

layout(set = 0, binding = 0) uniform sampler2D tex;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 color = texture(tex, vUV);
    if (pc.opaque != 0u)
        color.a = 1.0;
    // Wayland buffers are premultiplied, so alpha scales every channel.
    outColor = color * pc.alpha;
}
