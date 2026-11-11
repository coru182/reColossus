#version 450
// Full-screen passes (native_renderer_gl.cpp's kQuadFragment, every mode, line for line). gl_FragCoord counts rows
// from row 0 in memory as the GL backend's does (its origin at the bottom, which is row 0 there), so the GS chain's
// row arithmetic stays as it is.
layout(location = 0) in vec2 vUV;
layout(set = 0, binding = 0) uniform sampler2D uSrc;
layout(set = 0, binding = 1) uniform sampler2D uSrc2;
layout(set = 0, binding = 2) uniform sampler2D uDepth;
layout(push_constant) uniform Push
{
    vec4 uA, uB;
    vec4 uQuad;
    int uMode;
    int uCorners;
};
layout(location = 0) out vec4 oColor;
float gsZ(float d, float zA, float zB) { return d >= 1.0 ? 0.0 : zA + zB * (1.0 - d) / 0.1; }
void main()
{
    if (uMode == 0) // copy
        oColor = vec4(texture(uSrc, vUV).rgb, 1.0);
    else if (uMode == 1) // blur: uA.xy step
    {
        float w0 = 0.30, w1 = 0.235, w2 = 0.12, w3 = 0.05;
        vec4 c = texture(uSrc, vUV) * w0;
        c += (texture(uSrc, vUV + uA.xy) + texture(uSrc, vUV - uA.xy)) * w1;
        c += (texture(uSrc, vUV + uA.xy * 2.0) + texture(uSrc, vUV - uA.xy * 2.0)) * w2;
        c += (texture(uSrc, vUV + uA.xy * 3.0) + texture(uSrc, vUV - uA.xy * 3.0)) * w3;
        oColor = c;
    }
    else if (uMode == 2) // temporal: mix(previous, current, uA.x)
        oColor = mix(texture(uSrc2, vUV), texture(uSrc, vUV), uA.x);
    else if (uMode == 3) // blur copy: alpha uA.x
        oColor = vec4(texture(uSrc, vUV).rgb, uA.x);
    else if (uMode == 4) // tint: rgb x uA.rgb, alpha uA.a
        oColor = vec4(clamp(texture(uSrc, vUV).rgb * uA.rgb, 0.0, 1.0), uA.a);
    else if (uMode == 13) // plain copy, alpha kept, shifted by uA.xy
        oColor = texture(uSrc, vUV + uA.xy);
    else if (uMode == 14) // glow: rgb x uA.rgb, alpha x uA.a
    {
        vec4 c = texture(uSrc, vUV);
        oColor = clamp(vec4(c.rgb * uA.rgb, c.a * uA.a), 0.0, 1.0);
    }
    else if (uMode == 5) // shadow darken: alpha uA.x
        oColor = vec4(0.0, 0.0, 0.0, uA.x);
    else if (uMode == 6) // shadow mask write
        oColor = vec4(1.0);
    else if (uMode == 7 || uMode == 8) // 4 x 4 reduction: shadow mask (7) or bloom mask from depth (8)
    {
        vec2 size = uMode == 7 ? vec2(textureSize(uSrc, 0)) : vec2(textureSize(uDepth, 0));
        ivec2 base = ivec2(vUV * size) & ~3;
        float sum = 0.0;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
            {
                ivec2 at = min(base + ivec2(x, y), ivec2(size) - 1);
                if (uMode == 7)
                    sum += texelFetch(uSrc, at, 0).r;
                else
                    sum += gsZ(texelFetch(uDepth, at, 0).r, uA.y, uA.z) <= uA.x + 1.0 ? 1.0 : 0.0;
            }
        oColor = uMode == 7 ? vec4(vec3(sum / 16.0), 1.0) : vec4(uB.rgb * (sum / 16.0), 1.0);
    }
    else if (uMode == 9) // soft shadow: alpha mask x uA.x
        oColor = vec4(0.0, 0.0, 0.0, clamp(texture(uSrc, vUV).r * uA.x, 0.0, 1.0));
    else if (uMode == 10) // bloom composite: rgb x uB.rgb x uB.a
        oColor = vec4(texture(uSrc, vUV).rgb * uB.rgb * uB.a, 1.0);
    else if (uMode == 15) // the GS chain's mask at 256 x 256 (rows from the top)
    {
        vec2 size = vec2(textureSize(uDepth, 0));
        vec2 p = vec2(floor(gl_FragCoord.x), 255.0 - floor(gl_FragCoord.y));
        vec2 t = (p + 0.5) * vec2(2.0, 1.75) - 0.5;
        vec2 i0 = floor(t), f = t - i0;
        float m[4];
        for (int k = 0; k < 4; ++k)
        {
            vec2 at = clamp(i0 + vec2(k & 1, k >> 1), vec2(0.0), vec2(511.0, 447.0));
            vec2 px = (at + 0.5) * size / vec2(512.0, 448.0);
            ivec2 ip = ivec2(min(px.x, size.x - 1.0), min(size.y - 1.0 - px.y, size.y - 1.0));
            m[k] = gsZ(texelFetch(uDepth, max(ip, ivec2(0)), 0).r, uA.y, uA.z) <= uA.x + 1.0 ? 1.0 : 0.0;
        }
        oColor = vec4(uB.rgb * mix(mix(m[0], m[1], f.x), mix(m[2], m[3], f.x), f.y), 1.0);
    }
    else if (uMode == 16) // the GS chain's blur: the 64 x 64 source onto uA.x pixels, 12 shifted lerps (8-bit, truncated)
    {
        float r = uA.x, k = 64.0 / r;
        vec2 p = vec2(floor(gl_FragCoord.x), r - 1.0 - floor(gl_FragCoord.y));
        vec2 t0 = (p + 0.5) * k;
        vec3 acc = floor(texture(uSrc, vec2(t0.x, 64.0 - t0.y) / 64.0).rgb * 255.0 + 0.001);
        vec2 d[12] = vec2[12](vec2(-1, 0), vec2(1, 0), vec2(0, -1), vec2(0, 1), vec2(-2, 0), vec2(2, 0),
                              vec2(0, -2), vec2(0, 2), vec2(-3, 0), vec2(3, 0), vec2(0, -3), vec2(0, 3));
        for (int i = 0; i < 12; ++i)
        {
            float a = i < 4 ? 0.5 : (i < 8 ? 0.25 : 0.125);
            vec2 q = p - d[i];
            if (any(lessThan(q, vec2(0.0))) || any(greaterThanEqual(q, vec2(r))))
                continue;
            vec2 t = 0.5 + q * k;
            vec3 c = floor(texture(uSrc, vec2(t.x, 64.0 - t.y) / 64.0).rgb * 255.0 + 0.001);
            acc += floor((c - acc) * a);
        }
        oColor = vec4(acc / 255.0, 1.0);
    }
    else if (uMode == 17) // the GS chain's temporal step: Cd + ((Cs - Cd) x uA.x) truncated, Cs the history (uSrc2)
    {
        vec3 cd = floor(texture(uSrc, vUV).rgb * 255.0 + 0.001), cs = floor(texture(uSrc2, vUV).rgb * 255.0 + 0.001);
        oColor = vec4((cd + floor((cs - cd) * uA.x)) / 255.0, 1.0);
    }
    else if (uMode == 18) // the GS chain's composite: UV 0.5..256.5 over the PS2 frame; uA.xy render pixels per PS2 pixel
    {
        vec2 size = vec2(textureSize(uDepth, 0));
        vec2 x = vec2(gl_FragCoord.x, size.y - gl_FragCoord.y) / uA.xy - 0.5;
        vec2 uv = (0.5 + x * vec2(256.0 / 512.0, 256.0 / 448.0)) / 256.0;
        oColor = vec4(texture(uSrc, vec2(uv.x, 1.0 - uv.y)).rgb * uB.rgb * uB.a, 1.0);
    }
    else if (uMode == 12) // zFog: background (uSrc) by the depth's slice; uA = start, width, steps
    {
        float d = texelFetch(uDepth, ivec2(vUV * vec2(textureSize(uDepth, 0))), 0).r;
        float w = d >= 1.0 ? 1e30 : 0.1 / (1.0 - d);
        int steps = int(uA.z);
        float a = 0.0;
        for (int i = 0; i <= steps; ++i)
            if (w >= uA.x + uA.y * float(i) / float(steps))
                a = float((i * 128) / steps) / 128.0;
        oColor = vec4(texture(uSrc, vUV).rgb, a);
    }
    else // 11: depth of field: uA = zA, zB, focusZ, slices
    {
        float d = texelFetch(uDepth, ivec2(vUV * vec2(textureSize(uDepth, 0))), 0).r;
        float z = gsZ(d, uA.x, uA.y);
        float keep = 1.0;
        int slices = int(uA.w);
        for (int i = 0; i < slices; ++i)
            if (z <= float(uint(uA.z) >> uint(i)))
                keep *= 1.0 - float(i + 1) / float(slices);
        oColor = vec4(texture(uSrc, vUV).rgb, 1.0 - keep);
    }
}
