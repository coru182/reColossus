#version 450
// Full-screen passes (native_renderer_gl.cpp's kQuadVertex): a triangle over the screen, or the six corners of
// screenBlur's scaled and turned copies; uv 0..1 with v = 0 at clip y = -1, as the GL backend's.
layout(push_constant) uniform Push
{
    vec4 uA, uB;
    vec4 uQuad; // blur copy: scale, angle, offset (NDC); scale 0: plain
    int uMode;
    int uCorners;
};
layout(location = 0) out vec2 vUV;
const vec2 kTriangle[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
const vec2 kCorners[6] = vec2[6](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0), vec2(-1.0, 1.0));
void main()
{
    vec2 aPos = uCorners != 0 ? kCorners[gl_VertexIndex] : kTriangle[gl_VertexIndex];
    vec2 p = aPos;
    if (uQuad.x != 0.0)
    {
        vec2 c = aPos * uQuad.x;
        float s = sin(uQuad.y), k = cos(uQuad.y);
        p = vec2(c.x * k - c.y * s, c.x * s + c.y * k) + uQuad.zw;
    }
    gl_Position = vec4(p, 0.0, 1.0);
    vUV = aPos * 0.5 + 0.5;
}
