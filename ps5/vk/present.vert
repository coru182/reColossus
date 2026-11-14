#version 450
// The picture (or the host's overlay) onto the display's swapchain image, over the viewport it is given. uFlip: the
// renderer's pictures hold clip y = -1 in row 0 (the GL backend's layout), so the display's top row reads v = 1.
layout(push_constant) uniform Push
{
    int uMode; // 0 the picture (opaque), 1 the overlay (straight alpha)
    int uFlip;
};
layout(location = 0) out vec2 vUV;
const vec2 kTriangle[3] = vec2[3](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
void main()
{
    vec2 p = kTriangle[gl_VertexIndex];
    gl_Position = vec4(p, 0.0, 1.0);
    vUV = p * 0.5 + 0.5;
    if (uFlip != 0)
        vUV.y = 1.0 - vUV.y;
}
