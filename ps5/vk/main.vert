#version 450
// The native renderer's main vertex stage on Vulkan: native_renderer_gl.cpp's kMainVertex (GLSL 330) line for line.
// The picture has the GL backend's memory layout (row 0 at clip y = -1), so nothing is turned over; Vulkan's depth
// range is 0..1: z = w - 0.1 gives the GL backend's depth 1 - 0.1 / w. SKINNED: the bind pose posed first by up to
// three palette matrices (the GL skin program's prologue), the palette from the frame's storage buffer.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in uvec4 aColor; // 0..255 (0x80 = 1.0)
layout(location = 3) in vec2 aUV;
#ifdef SKINNED
layout(location = 4) in vec3 aBones;
layout(location = 5) in vec3 aWeights;
layout(std430, set = 0, binding = 1) readonly buffer Palettes { vec4 uPalette[]; };
#endif
layout(std140, set = 0, binding = 0) uniform Draw
{
    vec4 uClip[4], uDirections[4], uColours[4]; // matrix columns
    vec4 uMisc;      // lit, opacity, uv offset
    vec4 uFogA;      // start, width, steps, fogged
    vec4 uFogB;      // size, vertex fog, base
    vec4 uFogC;      // scale, lod K, lod scale, max lod
    vec4 uFogColour; // rgb, textured mode
    vec4 uDebug;
};
#ifdef SKINNED
layout(push_constant) uniform Push { uint uPaletteBase; };
#endif
layout(location = 0) out vec4 vUV;     // uv, w, fog x w
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vScreen; // clip x, y, w
void main()
{
#ifdef SKINNED
    vec3 pos = vec3(0.0), normal = vec3(0.0);
    for (int k = 0; k < 3; ++k)
    {
        uint b = uPaletteBase + uint(int(aBones[k] + 0.5));
        float w = aWeights[k];
        pos += w * (uPalette[b].xyz * aPos.x + uPalette[b + 1u].xyz * aPos.y + uPalette[b + 2u].xyz * aPos.z + uPalette[b + 3u].xyz);
        normal += w * (uPalette[b].xyz * aNormal.x + uPalette[b + 1u].xyz * aNormal.y + uPalette[b + 2u].xyz * aNormal.z);
    }
#else
    vec3 pos = aPos, normal = aNormal;
#endif
    vec4 c = uClip[0] * pos.x + uClip[1] * pos.y + uClip[2] * pos.z + uClip[3];
    gl_Position = vec4(c.x, c.y, c.w - 0.1, c.w);
    float fog = uFogB.z > 0.5 ? clamp(uFogB.w + uFogC.x / max(c.w, 1e-3), 0.0, 255.0) / 255.0 : 1.0;
    vUV = vec4(aUV + uMisc.zw, c.w, fog * c.w);
    vColor = vec4(aColor) / 128.0;
    vColor.a *= uMisc.y;
    if (uMisc.x < -0.5) // water (microcode 1): alpha x (0.2 + 0.8 (1 - |n . v|)) in camera space
    {
        vec4 p = uDirections[0] * pos.x + uDirections[1] * pos.y + uDirections[2] * pos.z + uDirections[3];
        float k = dot(normalize(p.xyz), uDirections[1].xyz);
        vColor.a *= 0.2 + 0.8 * (1.0 - abs(k));
    }
    else if (uMisc.x > 0.5)
    {
        vec4 d = max(uDirections[0] * normal.x + uDirections[1] * normal.y + uDirections[2] * normal.z + uDirections[3], 0.0);
        d.w = 1.0;
        vec4 light = uColours[0] * d.x + uColours[1] * d.y + uColours[2] * d.z + uColours[3];
        vColor.rgb = min(vColor.rgb * light.rgb, vec3(255.0 / 128.0));
    }
    vScreen = vec4(c.x, c.y, c.w, 0.0);
}
