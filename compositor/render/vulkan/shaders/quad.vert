#version 450

// One quad per draw, generated from gl_VertexIndex (4-vertex triangle
// strip): no vertex buffers. Push constants: see texture.frag.

layout(push_constant) uniform Push {
    vec4 rect;       // x, y, width, height in normalized device coordinates
    vec4 uv;         // u0, v0, u1, v1
    vec4 clipRect;   // framebuffer pixels; width 0 = no clip
    vec4 shapeRect;  // framebuffer pixels
    vec4 color1;     // straight (not premultiplied) RGBA
    vec4 color2;
    float clipRadius;
    float alpha;
    float shapeRadius;
    float shapeSize;  // border width / shadow blur
    float angle;      // gradient direction, radians
    uint mode;        // see texture.frag
    uint opaque;
    float pad;
} pc;

layout(location = 0) out vec2 vUV;

void main()
{
    // 0:(0,0) 1:(1,0) 2:(0,1) 3:(1,1)
    vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(pc.rect.xy + corner * pc.rect.zw, 0.0, 1.0);
    vUV = mix(pc.uv.xy, pc.uv.zw, corner);
}
