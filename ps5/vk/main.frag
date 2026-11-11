#version 450
// The native renderer's main fragment stage on Vulkan: native_renderer_gl.cpp's kMainFragment line for line (its
// pictures are laid out as the GL backend's, so its turned-over v for the renderer's own pictures stays).
layout(location = 0) in vec4 vUV;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vScreen;
layout(set = 1, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 1) uniform sampler2D uBackground;
layout(set = 1, binding = 2) uniform sampler2D uReflection;
layout(set = 1, binding = 3) uniform sampler2D uRefraction;
layout(std140, set = 0, binding = 0) uniform Draw
{
    vec4 uClip[4], uDirections[4], uColours[4];
    vec4 uMisc;
    vec4 uFogA;      // start, width, steps, fogged
    vec4 uFogB;
    vec4 uFogC;      // scale, lod K, lod scale, max lod
    vec4 uFogColour; // rgb, textured mode
    vec4 uDebug;
};
layout(location = 0) out vec4 oColor;
void main()
{
    float textured = uFogColour.w;
    vec2 screen = vScreen.xy / vScreen.z * 0.5 + 0.5;
    float depth = vUV.z;
    if (textured > 4.5) // the last frame shown, at the draw's UV
    {
        oColor = vec4(texture(uTex, vec2(vUV.x, 1.0 - vUV.y)).rgb, 1.0) * vColor;
        return;
    }
    if (textured > 3.5)
    {
        vec4 c = vec4(texture(uRefraction, vec2(vUV.x, 1.0 - vUV.y)).rgb, 1.0) * vColor;
        if (c.a < 0.02)
            discard;
        oColor = c;
        return;
    }
    if (textured > 2.5)
    {
        oColor = vec4(clamp(texture(uRefraction, screen).rgb * uDebug.rgb, 0.0, 1.0), -uDebug.w);
        return;
    }
    if (textured > 1.5)
    {
        vec2 uv = screen;
        if (uFogC.z > 0.0)
        {
            float band = floor(clamp(1.0 - uv.y, 0.0, 0.999) * 32.0);
            uv.x += (sin(2.0 * (12.0 * band + uFogC.y) * 75.39822 / uFogC.z) + 1.0) * (6.4 * band / uFogC.z + 0.2) / 256.0;
        }
        oColor = vec4(clamp(texture(uReflection, uv).rgb * uDebug.rgb * vColor.rgb, 0.0, 1.0), clamp(vColor.a * -uDebug.w, 0.0, 1.0));
        return;
    }
    float lod = uFogC.w > 0.0 ? clamp(log2(max(depth, 1e-3)) * uFogC.z + uFogC.y, 0.0, uFogC.w) : 0.0;
    vec4 t = textured > 0.5 ? textureLod(uTex, vUV.xy, lod) : vec4(1.0);
    float alpha = clamp(t.a * (textured > 0.5 ? 2.0 : 1.0) * vColor.a, 0.0, 1.0);
    if (alpha < 0.02)
        discard;
    if (uDebug.w > 0.0)
    {
        oColor = vec4(uDebug.rgb, 1.0);
        return;
    }
    vec3 rgb = clamp(t.rgb * vColor.rgb, 0.0, 1.0);
    rgb = mix(uFogColour.rgb, rgb, vUV.w / vUV.z);
    if (uFogA.w > 0.5)
    {
        float f = clamp((depth - uFogA.x) / uFogA.y, 0.0, 1.0);
        if (uFogA.z > 0.0)
            f = floor(f * uFogA.z) / uFogA.z;
        rgb = mix(rgb, texture(uBackground, screen).rgb, f);
    }
    oColor = vec4(rgb, alpha);
}
