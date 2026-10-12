#version 450

// One textured quad per draw, generated from gl_VertexIndex (4-vertex
// triangle strip): no vertex buffers.

layout(push_constant) uniform Push {
    vec4 rect;    // x, y, width, height in normalized device coordinates
    vec4 uv;      // u0, v0, u1, v1
    float alpha;
    uint opaque;  // 1: ignore the texture's alpha (X* formats)
} pc;

layout(location = 0) out vec2 vUV;

void main()
{
    // 0:(0,0) 1:(1,0) 2:(0,1) 3:(1,1)
    vec2 corner = vec2(gl_VertexIndex & 1, gl_VertexIndex >> 1);
    gl_Position = vec4(pc.rect.xy + corner * pc.rect.zw, 0.0, 1.0);
    vUV = mix(pc.uv.xy, pc.uv.zw, corner);
}
