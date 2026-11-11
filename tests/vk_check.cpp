// reColossus: the Vulkan renderer (ps5/vk/native_renderer_vk.cpp) against Celosia's OpenGL one, on Linux: the same
// frames (a synthetic scene that uses every kind of draw and every effect the game's override hands the renderer)
// drawn by both, on Mesa's CPU drivers (llvmpipe through EGL, lavapipe), and their pictures compared.
//
//   tests/vk_check.sh            builds this against the pinned Celosia tree and runs it (validation layers on)
//   vk_check <out dir> [frames]  writes <out>/gl_<n>.ppm, vk_<n>.ppm, diff_<n>.ppm and prints the differences
#include "native_backend.h"
#include "runtime/native_tuning.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace native_renderer::detail
{
    Backend *createVkBackend();
}
using namespace native_renderer;
using detail::Backend;

namespace
{
    struct Mat
    {
        float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // column-major
    };
    Mat mul(const Mat &a, const Mat &b)
    {
        Mat r;
        for (int c = 0; c < 4; ++c)
            for (int row = 0; row < 4; ++row)
            {
                float s = 0;
                for (int k = 0; k < 4; ++k)
                    s += a.m[k * 4 + row] * b.m[c * 4 + k];
                r.m[c * 4 + row] = s;
            }
        return r;
    }
    Mat translate(float x, float y, float z)
    {
        Mat r;
        r.m[12] = x;
        r.m[13] = y;
        r.m[14] = z;
        return r;
    }
    Mat rotateY(float a)
    {
        Mat r;
        r.m[0] = std::cos(a);
        r.m[2] = -std::sin(a);
        r.m[8] = std::sin(a);
        r.m[10] = std::cos(a);
        return r;
    }
    // The game's projection as the renderer takes it: x/w, y/w in -1..1 (y up), w the view depth.
    Mat projection()
    {
        Mat p;
        const float f = 1.6f;
        p.m[0] = f / (512.0f / 448.0f);
        p.m[5] = f;
        p.m[10] = 0;
        p.m[11] = 1; // w = z
        p.m[14] = 0;
        p.m[15] = 0;
        return p;
    }
    Vertex vtx(float x, float y, float z, float u, float v, uint8_t r, uint8_t g, uint8_t b, uint8_t a, float nx = 0, float ny = 1, float nz = 0)
    {
        Vertex o{};
        o.x = x;
        o.y = y;
        o.z = z;
        o.nx = nx;
        o.ny = ny;
        o.nz = nz;
        o.u = u;
        o.v = v;
        o.r = r;
        o.g = g;
        o.b = b;
        o.a = a;
        return o;
    }
    void quadTris(std::vector<Vertex> &out, const Vertex &a, const Vertex &b, const Vertex &c, const Vertex &d)
    {
        out.insert(out.end(), {a, b, c, a, c, d});
    }
    std::vector<Vertex> ground()
    {
        std::vector<Vertex> v;
        const int n = 24;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n; ++j)
            {
                auto at = [&](int x, int z)
                {
                    const float fx = (x - n / 2) * 2.0f, fz = z * 2.0f + 2.0f;
                    const float y = -1.5f + 0.4f * std::sin(x * 0.7f) * std::cos(z * 0.5f);
                    return vtx(fx, y, fz, x * 0.5f, z * 0.5f, static_cast<uint8_t>(90 + 3 * x), 120, static_cast<uint8_t>(80 + 2 * z), 0x80, 0.1f, 1.0f, 0.05f);
                };
                quadTris(v, at(i, j), at(i + 1, j), at(i + 1, j + 1), at(i, j + 1));
            }
        return v;
    }
    std::vector<Vertex> box(float s, uint8_t alpha)
    {
        std::vector<Vertex> v;
        const float p[8][3] = {{-s, -s, -s}, {s, -s, -s}, {s, s, -s}, {-s, s, -s}, {-s, -s, s}, {s, -s, s}, {s, s, s}, {-s, s, s}};
        const int f[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
        const float nrm[6][3] = {{0, 0, -1}, {0, 0, 1}, {-1, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
        for (int k = 0; k < 6; ++k)
        {
            Vertex q[4];
            const float uv[4][2] = {{0, 0}, {2, 0}, {2, 2}, {0, 2}};
            for (int c = 0; c < 4; ++c)
                q[c] = vtx(p[f[k][c]][0], p[f[k][c]][1], p[f[k][c]][2], uv[c][0], uv[c][1], static_cast<uint8_t>(60 + 30 * k), 128, static_cast<uint8_t>(200 - 25 * k),
                           alpha, nrm[k][0], nrm[k][1], nrm[k][2]);
            quadTris(v, q[0], q[1], q[2], q[3]);
        }
        return v;
    }
    std::vector<Vertex> rect(float x0, float y0, float x1, float y1, float z, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
    {
        std::vector<Vertex> v;
        quadTris(v, vtx(x0, y0, z, 0, 1, r, g, b, a), vtx(x1, y0, z, 1, 1, r, g, b, a), vtx(x1, y1, z, 1, 0, r, g, b, a), vtx(x0, y1, z, 0, 0, r, g, b, a));
        return v;
    }
    // A texture with its mip levels (alpha 0x80 = opaque, as the PS2's); `holes`: transparent cells.
    std::vector<uint8_t> checker(uint32_t size, uint32_t levels, uint8_t r, uint8_t g, uint8_t b, bool holes)
    {
        std::vector<uint8_t> out;
        for (uint32_t l = 0, s = size; l < levels; ++l, s = std::max(1u, s / 2))
            for (uint32_t y = 0; y < s; ++y)
                for (uint32_t x = 0; x < s; ++x)
                {
                    const bool on = (((x * 8) / s) ^ ((y * 8) / s)) & 1;
                    const float k = on ? 1.0f : 0.45f;
                    out.push_back(static_cast<uint8_t>(r * k));
                    out.push_back(static_cast<uint8_t>(g * k * (1.0f - 0.15f * l)));
                    out.push_back(static_cast<uint8_t>(b * k));
                    out.push_back(holes && !on && ((x * 4 / s + y * 4 / s) % 3 == 0) ? 0 : 0x80);
                }
        return out;
    }

    struct Scene
    {
        Mat view, proj;
        void build(Backend &b, int frame)
        {
            const float t = frame * 0.11f;
            view = mul(translate(0.3f * std::sin(t), -0.2f, 3.0f), rotateY(0.15f * std::sin(t * 0.7f)));
            proj = projection();
            if (!b.hasMesh(1))
            {
                auto g = ground();
                b.uploadMesh(1, g.data(), static_cast<uint32_t>(g.size()), false);
                auto c = box(0.8f, 0x80);
                b.uploadMesh(2, c.data(), static_cast<uint32_t>(c.size()), false);
                auto w = rect(-30, 0, 30, 60, 0, 120, 160, 220, 0x50); // water plane (y = 0 in model space, turned below)
                b.uploadMesh(3, w.data(), static_cast<uint32_t>(w.size()), false);
                auto sky = rect(-1, 0.0f, 1, 1, 0.5f, 150, 170, 220, 0x80);
                b.uploadMesh(4, sky.data(), static_cast<uint32_t>(sky.size()), false);
                auto vol = box(1.2f, 0x80);
                b.uploadMesh(5, vol.data(), static_cast<uint32_t>(vol.size()), false);
                // A skinned bar: two bones, the top half following the second.
                std::vector<SkinVertex> sv;
                std::vector<uint32_t> si;
                for (int ring = 0; ring <= 4; ++ring)
                    for (int k = 0; k < 8; ++k)
                    {
                        const float a = k * 0.785398f, y = ring * 0.5f;
                        SkinVertex s{};
                        s.x = 0.3f * std::cos(a);
                        s.y = y;
                        s.z = 0.3f * std::sin(a);
                        s.nx = std::cos(a);
                        s.nz = std::sin(a);
                        s.u = k / 8.0f;
                        s.v = ring / 4.0f;
                        s.r = 200;
                        s.g = 90;
                        s.b = 60;
                        s.a = 0x80;
                        const float wTop = ring / 4.0f;
                        s.bone[0] = 0;
                        s.bone[1] = 4;
                        s.bone[2] = 0;
                        s.weight[0] = 1.0f - wTop;
                        s.weight[1] = wTop;
                        s.weight[2] = 0;
                        sv.push_back(s);
                    }
                for (int ring = 0; ring < 4; ++ring)
                    for (int k = 0; k < 8; ++k)
                    {
                        const uint32_t a = ring * 8 + k, b2 = ring * 8 + (k + 1) % 8, c = a + 8, d = b2 + 8;
                        si.insert(si.end(), {a, b2, d, a, d, c});
                    }
                if (b.canSkin())
                    b.uploadSkinnedMesh(6, sv.data(), static_cast<uint32_t>(sv.size()), si.data(), static_cast<uint32_t>(si.size()));
            }
            if (!b.hasTexture(100))
            {
                auto tex = checker(64, 7, 200, 180, 120, false);
                b.uploadTexture(100, 64, 64, tex.data(), 7);
                auto t2 = checker(32, 1, 90, 200, 240, true);
                b.uploadTexture(101, 32, 32, t2.data(), 1);
            }
            b.beginFrame();
            Fog fog;
            fog.enabled = true;
            fog.start = 10.0f;
            fog.width = 30.0f;
            fog.steps = 8;
            fog.order = 6;
            fog.vertexFog = true;
            fog.base = 255.0f;
            fog.scale = -900.0f;
            fog.colour[0] = 0.6f;
            fog.colour[1] = 0.65f;
            fog.colour[2] = 0.75f;
            b.setFog(fog);
            const Mat pv = mul(proj, view);
            auto draw = [&](uint64_t mesh, const Mat &model, DrawState s)
            {
                const Mat clip = mul(pv, model);
                b.draw(mesh, clip.m, s);
            };
            // The sky behind it all (layer 1, a background draw: what fog fades to).
            {
                DrawState s;
                s.order = 1;
                s.screen = true;
                s.background = true;
                s.blend = Blend::Opaque;
                Mat id;
                b.draw(4, id.m, s);
                auto low = rect(-1, -1, 1, 0.0f, 0.5f, 110, 120, 140, 0x80);
                Vertex *room = b.frameMesh(static_cast<uint32_t>(low.size()));
                std::memcpy(room, low.data(), low.size() * sizeof(Vertex));
                b.commitFrameMesh(50, static_cast<uint32_t>(low.size()), 0);
                b.draw(50, id.m, s);
            }
            {
                DrawState s; // the ground: textured, lit, mipmapped at the GS's level
                s.order = 3;
                s.texture = 100;
                s.lit = true;
                s.lodK = -1.0f;
                s.lodScale = 1.0f;
                s.maxLod = 6.0f;
                s.fogged = true;
                s.background = false;
                for (int i = 0; i < 4; ++i)
                {
                    s.light[i * 4 + 0] = i == 0 ? 0.6f : 0.0f;
                    s.light[i * 4 + 1] = i == 1 ? 0.8f : 0.0f;
                    s.light[i * 4 + 2] = i == 2 ? 0.3f : 0.0f;
                }
                for (int i = 0; i < 16; ++i)
                    s.light[16 + i] = (i % 4 == 3) ? 0.0f : (i < 12 ? 0.5f : 0.35f);
                draw(1, Mat{}, s);
            }
            {
                DrawState s; // boxes: opaque, alpha-tested, additive, subtract
                s.order = 4;
                s.texture = 101;
                s.clamp = 1;
                s.blend = Blend::Normal;
                draw(2, mul(translate(-2.0f, 0.0f, 4.0f), rotateY(t)), s);
                s.texture = 100;
                s.clamp = 0;
                s.blend = Blend::Opaque;
                draw(2, mul(translate(1.5f, -0.4f, 6.0f), rotateY(-t * 0.5f)), s);
                s.blend = Blend::Additive;
                s.opacity = 0.6f;
                s.order = 5;
                draw(2, mul(translate(0.0f, 0.8f, 9.0f), rotateY(t * 0.3f)), s);
                s.blend = Blend::Subtract;
                s.opacity = 0.4f;
                draw(2, mul(translate(3.0f, 0.3f, 12.0f), rotateY(t)), s);
                s.blend = Blend::Normal;
                s.opacity = 1.0f;
                s.overlay = true; // a second pass over the first box
                s.texture = 0;
                s.debugColour = 0;
                s.opacity = 0.3f;
                draw(2, mul(translate(-2.0f, 0.0f, 4.0f), rotateY(t)), s);
            }
            if (b.canSkin())
            {
                DrawState s; // the skinned bar, its top bone turned
                s.order = 4;
                s.lit = true;
                for (int i = 0; i < 32; ++i)
                    s.light[i] = (i == 1 || i == 16 || i == 21 || i == 26) ? 0.9f : (i == 28 || i == 29 || i == 30 ? 0.3f : 0.0f);
                const Mat a = translate(0, -1.0f, 0), bm = mul(translate(0, -0.2f, 0), rotateY(t * 2.0f));
                float pal[8 * 4];
                for (int k = 0; k < 4; ++k)
                    for (int c = 0; c < 4; ++c)
                    {
                        pal[k * 4 + c] = a.m[k * 4 + c];
                        pal[16 + k * 4 + c] = bm.m[k * 4 + c];
                    }
                s.skinPalette = b.framePalette(pal, 8);
                s.skinPaletteCount = 8;
                draw(6, translate(-0.6f, 0.0f, 5.0f), s);
            }
            {
                DrawState s; // a shadow volume around the second box (counted on the stencil, darkened after its layer)
                s.order = 4;
                s.shadowVolume = true;
                s.shadow = 0.45f;
                draw(5, translate(1.5f, -1.2f, 6.0f), s);
            }
            {
                DrawState s; // the mirrored world (reflection source) and the water showing it, with refraction
                s.order = 3;
                s.reflectionSource = true;
                s.texture = 100;
                Mat flip;
                flip.m[5] = -1;
                draw(1, mul(translate(0, -3.0f, 0), flip), s);
                DrawState w;
                w.order = 9;
                w.reflectionSample = true;
                w.reflectionTint[0] = 0.8f;
                w.reflectionTint[1] = 0.9f;
                w.reflectionTint[2] = 1.0f;
                w.reflectionTint[3] = 0.7f;
                w.mirrorRipple[0] = frame * 1.0f;
                w.mirrorRipple[1] = 60.0f;
                w.noDepthWrite = true;
                Mat lay;
                lay.m[5] = 0;
                lay.m[6] = 1;
                lay.m[9] = 1;
                lay.m[10] = 0; // the rect's y becomes z
                draw(3, mul(translate(0, -1.6f, 8.0f), lay), w);
                DrawState rf = w;
                rf.reflectionSample = false;
                rf.refractionSample = true;
                rf.reflectionTint[3] = 0.5f;
                draw(3, mul(translate(0, -1.62f, 30.0f), lay), rf);
            }
            {
                DrawState s; // layer 8: the glow layer (bossDeathEffect), drawn apart
                s.order = 8;
                s.texture = 100;
                s.blend = Blend::Additive;
                draw(2, mul(translate(-1.0f, 1.2f, 7.0f), rotateY(-t)), s);
            }
            {
                DrawState s; // the heat haze: a copy of the picture so far at layer 23, tested against its depth
                s.order = 23;
                s.screenCopy = true;
                s.screen = true;
                auto haze = rect(-0.3f, -0.3f, 0.3f, 0.3f, 0.0f, 128, 128, 128, 0x60);
                for (Vertex &v : haze)
                {
                    v.u = v.u * 0.5f + 0.2f;
                    v.v = v.v * 0.5f + 0.25f;
                }
                Vertex *room = b.frameMesh(4);
                uint32_t base = 0;
                const Vertex corners[4] = {haze[0], haze[1], haze[2], haze[5]};
                std::memcpy(room, corners, sizeof(corners));
                uint32_t *ix = b.frameMeshIndices(6, base);
                const uint32_t q[6] = {0, 1, 2, 0, 2, 3};
                for (int i = 0; i < 6; ++i)
                    ix[i] = base + q[i];
                b.commitFrameMesh(51, 4, 6);
                Mat id;
                b.draw(51, id.m, s);
            }
            {
                DrawState s; // the last frame shown over this one (feedbackBlur), then 2D on top
                s.order = 29;
                s.previousFrame = true;
                s.screen = true;
                auto full = rect(-1, -1, 1, 1, 0, 128, 128, 128, 0x20);
                Vertex *room = b.frameMesh(6);
                std::memcpy(room, full.data(), 6 * sizeof(Vertex));
                b.commitFrameMesh(52, 6, 0);
                Mat id;
                b.draw(52, id.m, s);
                DrawState h;
                h.order = 30;
                h.screen = true;
                h.texture = 101;
                auto hud = rect(-0.9f, 0.6f, -0.5f, 0.9f, 0, 128, 128, 128, 0x80);
                room = b.frameMesh(6);
                std::memcpy(room, hud.data(), 6 * sizeof(Vertex));
                b.commitFrameMesh(53, 6, 0);
                b.draw(53, id.m, h);
            }
            Bloom bl;
            bl.enabled = true;
            bl.order = 20;
            bl.threshold = 200.0f;
            bl.zA = 0.0f;
            bl.zB = 2000.0f;
            bl.maskColor[0] = 0.5f;
            bl.maskColor[1] = 0.45f;
            bl.maskColor[2] = 0.4f;
            bl.color[0] = bl.color[1] = bl.color[2] = 1.0f;
            bl.color[3] = 0.6f;
            bl.temporal = 0.9f;
            b.setBloom(bl);
            GlowComposite gl;
            gl.enabled = true;
            gl.order = 8;
            gl.rounds = 3;
            gl.amountA = 0.5f;
            gl.amountB = 0.8f;
            b.setGlowComposite(gl);
            DepthOfField dof;
            dof.enabled = true;
            dof.order = 21;
            dof.zA = 0.0f;
            dof.zB = 2000.0f;
            dof.focusZ = 120;
            dof.slices = 3;
            dof.rounds = 1;
            b.setDepthOfField(dof);
            ScreenBlur sb;
            sb.order = 25;
            sb.angle = 0.03f;
            sb.scale = 1.03f;
            sb.dx = 2.0f;
            sb.dy = 1.0f;
            sb.steps = 3;
            b.addScreenBlur(sb);
            b.endFrame(512, 448);
            b.finishFrames();
        }
    };

    bool writePpm(const std::string &path, const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
    {
        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f)
            return false;
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
            std::fwrite(&rgba[i * 4], 1, 3, f);
        std::fclose(f);
        return true;
    }

    EGLDisplay g_display = EGL_NO_DISPLAY;
    EGLContext g_context = EGL_NO_CONTEXT;
    bool makeGl()
    {
        auto getPlatformDisplay = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(eglGetProcAddress("eglGetPlatformDisplayEXT"));
        g_display = getPlatformDisplay ? getPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr) : EGL_NO_DISPLAY;
        if (g_display == EGL_NO_DISPLAY || !eglInitialize(g_display, nullptr, nullptr))
            return false;
        eglBindAPI(EGL_OPENGL_API);
        const EGLint cfgAttrs[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE};
        EGLConfig cfg;
        EGLint n = 0;
        if (!eglChooseConfig(g_display, cfgAttrs, &cfg, 1, &n) || n < 1)
            return false;
        const EGLint ctxAttrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                   EGL_NONE};
        g_context = eglCreateContext(g_display, cfg, EGL_NO_CONTEXT, ctxAttrs);
        return g_context != EGL_NO_CONTEXT;
    }
}

int main(int argc, char **argv)
{
    const std::string out = argc > 1 ? argv[1] : ".";
    const int frames = argc > 2 ? std::atoi(argv[2]) : 6;
    tuning().renderThread = false;
    if (!makeGl())
    {
        std::fprintf(stderr, "check: no OpenGL context (EGL surfaceless)\n");
        return 2;
    }
    Backend *gl = detail::createGlBackend([] { eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, g_context); });
    Backend *vk = detail::createVkBackend();
    if (!gl || !vk)
    {
        std::fprintf(stderr, "check: backend missing (gl %p vk %p)\n", static_cast<void *>(gl), static_cast<void *>(vk));
        return 2;
    }
    std::printf("check: skinning gl %d vk %d\n", gl->canSkin() ? 1 : 0, vk->canSkin() ? 1 : 0);
    Scene sgl, svk;
    int worst = 0;
    for (int f = 0; f < frames; ++f)
    {
        sgl.build(*gl, f);
        svk.build(*vk, f);
        std::vector<uint8_t> a, b;
        uint32_t aw = 0, ah = 0, bw = 0, bh = 0;
        if (!gl->latestFrame(a, aw, ah) || !vk->latestFrame(b, bw, bh) || aw != bw || ah != bh)
        {
            std::fprintf(stderr, "check: frame %d: no picture (gl %ux%u vk %ux%u)\n", f, aw, ah, bw, bh);
            return 1;
        }
        std::vector<uint8_t> diff(a.size());
        size_t over8 = 0, over32 = 0;
        int maxd = 0;
        double sum = 0;
        for (size_t i = 0; i < static_cast<size_t>(aw) * ah; ++i)
        {
            int d = 0;
            for (int c = 0; c < 3; ++c)
                d = std::max(d, std::abs(int(a[i * 4 + c]) - int(b[i * 4 + c])));
            maxd = std::max(maxd, d);
            sum += d;
            over8 += d > 8;
            over32 += d > 32;
            const uint8_t v = static_cast<uint8_t>(std::min(255, d * 8));
            diff[i * 4] = diff[i * 4 + 1] = diff[i * 4 + 2] = v;
            diff[i * 4 + 3] = 255;
        }
        const double pixels = static_cast<double>(aw) * ah;
        std::printf("frame %d: %ux%u mean %.3f max %d, >8: %.3f%%, >32: %.3f%%\n", f, aw, ah, sum / pixels, maxd, 100.0 * over8 / pixels, 100.0 * over32 / pixels);
        writePpm(out + "/gl_" + std::to_string(f) + ".ppm", a, aw, ah);
        writePpm(out + "/vk_" + std::to_string(f) + ".ppm", b, bw, bh);
        writePpm(out + "/diff_" + std::to_string(f) + ".ppm", diff, aw, ah);
        if (100.0 * over8 / pixels > 0.5)
            worst = 1;
    }
    std::printf("check: %s\n", worst ? "the pictures differ" : "the pictures match");
    std::fflush(stdout);
    std::_Exit(worst); // the backends' threads are left running
}
