// reColossus: the raylib subset the PS2Recomp runtime uses (raylib.h here), on the PS5's own system libraries.
//
//   Picture  The runtime draws one host frame at a time (BeginDrawing ... EndDrawing): the game's picture as a
//            texture scaled to the screen, and now and then a menu's rectangles and text. Here that is drawn by the
//            CPU into a 1920x1080 frame, which EndDrawing copies into one of VideoOut's scan-out buffers in the
//            display's tiled layout and flips at the next vblank. The VideoOut set-up (five 1080p B8G8R8A8 buffers
//            in one CPU direct-memory allocation, the 128x128 tiling, the cache flush) is reUndr-ps5's
//            screen_ps5.c (MIT, bystritskiy), as reKing's first-start screen uses it.
//   Input    The DualSense of the user who started the app, through libScePad, as raylib's gamepad 0 (the button
//            and axis numbering raylib gives a PlayStation controller). No keyboard.
//   Sound    The runtime's stream callback (the SPU2's mix, 48 kHz 16-bit stereo) played through libSceAudioOut,
//            256 frames per call: the output call returns once the previous ones have been taken, which paces it.
//
// The CPU work per frame (scaling the picture to 1080p, the tiling copy) is split over a few worker threads.
#include "raylib.h"

#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "ps5/sys_ps5.h"
#include "ps5/rc_screen.h"

namespace
{
#include "default_font.inc"

    constexpr int kWidth = 1920, kHeight = 1080;
    constexpr size_t kBufferBytes = 0xa00000; // one 1080p buffer as PS5_Mesa's VideoOut WSI lays it out
    constexpr int kBuffers = 5;               // what VideoOut accepted from this app (fewer were refused)
    constexpr size_t kVideoBytes = kBufferBytes * kBuffers;
    constexpr uint64_t kFormatB8G8R8A8Sdr = 0x8000000000000000ull;
    constexpr int kDirectCpu = 12;
    constexpr uint32_t kFlipVsync = 1;

    using Clock = std::chrono::steady_clock;
    const Clock::time_point g_start = Clock::now();

    double secondsSinceStart() { return std::chrono::duration<double>(Clock::now() - g_start).count(); }

    // ------------------------------------------------------------------------------------------------------------
    // Worker threads for the per-frame pixel work: run(n, fn) calls fn(i, n) for i in 0..n-1, one part each.
    class Workers
    {
    public:
        void run(const std::function<void(int, int)> &fn)
        {
            start();
            std::unique_lock<std::mutex> lock(m_lock);
            m_job = &fn;
            m_pending = m_count;
            ++m_generation;
            m_wake.notify_all();
            lock.unlock();
            fn(0, m_count + 1); // this thread takes part 0
            lock.lock();
            m_done.wait(lock, [&] { return m_pending == 0; });
            m_job = nullptr;
        }

    private:
        void start()
        {
            if (m_started)
                return;
            m_started = true;
            const char *env = std::getenv("RECOLOSSUS_DRAW_THREADS");
            m_count = env ? std::clamp(std::atoi(env), 0, 7) : 3;
            for (int i = 0; i < m_count; ++i)
                std::thread([this, i] { loop(i + 1); }).detach();
        }
        void loop(int part)
        {
            uint64_t seen = 0;
            for (;;)
            {
                std::unique_lock<std::mutex> lock(m_lock);
                m_wake.wait(lock, [&] { return m_generation != seen; });
                seen = m_generation;
                const std::function<void(int, int)> *job = m_job;
                lock.unlock();
                (*job)(part, m_count + 1);
                lock.lock();
                if (--m_pending == 0)
                    m_done.notify_one();
            }
        }

        bool m_started = false;
        int m_count = 0, m_pending = 0;
        uint64_t m_generation = 0;
        const std::function<void(int, int)> *m_job = nullptr;
        std::mutex m_lock;
        std::condition_variable m_wake, m_done;
    };
    Workers g_workers;

    // rows [y0, y1) of part i of n
    void partRows(int i, int n, int y0, int y1, int &a, int &b)
    {
        const int rows = std::max(0, y1 - y0);
        a = y0 + rows * i / n;
        b = y0 + rows * (i + 1) / n;
    }

    // ------------------------------------------------------------------------------------------------------------
    // VideoOut
    struct Video
    {
        int handle = -1;
        int64_t physical = 0;
        uint8_t *buffers = nullptr;
        int next = 0;
    };
    Video g_video;
    bool g_windowReady = false;
    std::vector<uint32_t> g_frame; // the host frame, 0xAARRGGBB (the scan-out's B8G8R8A8 in memory)

    // The GPU's screen (rc_screen_gpu_owns): the Vulkan renderer presents to the display itself, and the host frame
    // is only an overlay over its picture (the fps line, the settings menu), cleared to transparent; what was drawn
    // in it since the last clear is published for the renderer at the end of a frame (the region that changed).
    std::atomic<bool> g_gpuOwnsScreen{false};
    bool g_videoTried = false;
    int g_dirtyX0 = kWidth, g_dirtyY0 = kHeight, g_dirtyX1 = 0, g_dirtyY1 = 0;
    std::mutex g_overlayLock;
    std::vector<uint32_t> g_overlay;               // the published overlay, kWidth x kHeight
    int g_pubX0 = 0, g_pubY0 = 0, g_pubX1 = 0, g_pubY1 = 0; // what changed since the renderer last took it
    uint64_t g_overlaySerial = 0;
    int g_lastX0 = kWidth, g_lastY0 = kHeight, g_lastX1 = 0, g_lastY1 = 0; // the last frame's drawn region

    inline void touch(int x0, int y0, int x1, int y1)
    {
        g_dirtyX0 = std::min(g_dirtyX0, x0);
        g_dirtyY0 = std::min(g_dirtyY0, y0);
        g_dirtyX1 = std::max(g_dirtyX1, x1);
        g_dirtyY1 = std::max(g_dirtyY1, y1);
    }

    // The display's tiling (screen_ps5.c's tiled_offset): 64 KiB tiles of 128x128 pixels, 15 to a row of 1920;
    // inside a tile the byte offset is the XOR of a part from x and a part from y.
    uint32_t g_tileX[kWidth], g_inX[kWidth], g_tileY[kHeight], g_inY[kHeight];

    void buildTiling()
    {
        constexpr uint32_t tilesPerRow = (kWidth + 127) / 128;
        for (uint32_t x = 0; x < kWidth; ++x)
        {
            g_tileX[x] = (x >> 7) << 16;
            g_inX[x] = ((x << 2) & 0xcu) ^ ((x << 5) & 0x380u) ^ ((x << 4) & 0x400u) ^ ((x << 6) & 0x800u) ^ ((x << 9) & 0xa000u);
        }
        for (uint32_t y = 0; y < kHeight; ++y)
        {
            g_tileY[y] = ((y >> 7) * tilesPerRow) << 16;
            g_inY[y] = ((y << 4) & 0x70u) ^ ((y << 5) & 0xf00u) ^ ((y << 9) & 0x1000u) ^ ((y << 8) & 0x4000u);
        }
    }

    bool openVideo()
    {
        if (g_video.handle >= 0)
            return true;
        sceSystemServiceHideSplashScreen();
        std::fprintf(stderr, "[ps5] VideoOut: opening\n");
        int handle = sceVideoOutOpen(255, 0, 0, nullptr);
        if (handle < 0)
        {
            std::fprintf(stderr, "[ps5] sceVideoOutOpen failed %#x\n", unsigned(handle));
            return false;
        }
        sceVideoOutSetFlipRate(handle, 0);
        int64_t physical = 0;
        int rc = sceKernelAllocateDirectMemory(0, int64_t(sceKernelGetDirectMemorySize()), kVideoBytes, 0x200000, kDirectCpu, &physical);
        if (rc < 0)
        {
            std::fprintf(stderr, "[ps5] sceKernelAllocateDirectMemory failed %#x\n", unsigned(rc));
            sceVideoOutClose(handle);
            return false;
        }
        void *mapped = nullptr;
        rc = sceKernelMapDirectMemory(&mapped, kVideoBytes, 0x33, 0, physical, 0x200000);
        if (rc < 0)
        {
            std::fprintf(stderr, "[ps5] sceKernelMapDirectMemory failed %#x\n", unsigned(rc));
            sceKernelReleaseDirectMemory(physical, kVideoBytes);
            sceVideoOutClose(handle);
            return false;
        }
        std::memset(mapped, 0, kVideoBytes);
        SceVideoOutBuffer buffers[kBuffers] = {};
        for (int i = 0; i < kBuffers; ++i)
            buffers[i].address = static_cast<uint8_t *>(mapped) + size_t(i) * kBufferBytes;
        SceVideoOutAttribute attribute = {};
        sceVideoOutSetBufferAttribute2(&attribute, kFormatB8G8R8A8Sdr, 0, kWidth, kHeight, 0, 0, 0);
        rc = sceVideoOutRegisterBuffers2(handle, 0, 0, buffers, kBuffers, &attribute, 0, nullptr);
        if (rc < 0)
        {
            std::fprintf(stderr, "[ps5] sceVideoOutRegisterBuffers2 failed %#x\n", unsigned(rc));
            sceVideoOutClose(handle);
            return false;
        }
        g_video = Video{handle, physical, static_cast<uint8_t *>(mapped), 0};
        buildTiling();
        return true;
    }

    // ------------------------------------------------------------------------------------------------------------
    // Pixels
    inline uint32_t pack(Color c) { return (uint32_t(c.a) << 24) | (uint32_t(c.r) << 16) | (uint32_t(c.g) << 8) | c.b; }

    // src over dst, 8-bit channels; src is 0xAARRGGBB
    inline uint32_t blend(uint32_t dst, uint32_t src)
    {
        const uint32_t a = src >> 24;
        if (a == 255u)
            return src;
        if (a == 0u)
            return dst;
        const uint32_t da = dst >> 24;
        if (da != 255u)
        {
            // over a transparent overlay: straight alpha, out = src + dst x (1 - a)
            const float sa = a / 255.0f, dA = da / 255.0f, oa = sa + dA * (1.0f - sa);
            auto ch = [&](int shift)
            {
                const float sc = float((src >> shift) & 0xffu), dc = float((dst >> shift) & 0xffu);
                return uint32_t(std::lround((sc * sa + dc * dA * (1.0f - sa)) / oa)) << shift;
            };
            return (uint32_t(std::lround(oa * 255.0f)) << 24) | ch(16) | ch(8) | ch(0);
        }
        const uint32_t ia = 255u - a;
        const uint32_t rb = (((src & 0xff00ffu) * a + (dst & 0xff00ffu) * ia + 0x800080u) >> 8) & 0xff00ffu;
        const uint32_t g = (((src & 0x00ff00u) * a + (dst & 0x00ff00u) * ia + 0x008000u) >> 8) & 0x00ff00u;
        return 0xff000000u | rb | g;
    }

    void fillRect(int x, int y, int w, int h, uint32_t color)
    {
        int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(kWidth, x + w), y1 = std::min(kHeight, y + h);
        if (x0 >= x1 || y0 >= y1 || g_frame.empty())
            return;
        touch(x0, y0, x1, y1);
        for (int j = y0; j < y1; ++j)
        {
            uint32_t *row = g_frame.data() + size_t(j) * kWidth;
            if ((color >> 24) == 255u)
                std::fill(row + x0, row + x1, color);
            else
                for (int i = x0; i < x1; ++i)
                    row[i] = blend(row[i], color);
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Textures: RGBA8 kept in memory, drawn by the CPU
    struct Tex
    {
        int width = 0, height = 0, filter = TEXTURE_FILTER_POINT;
        std::vector<uint8_t> rgba;
    };
    std::mutex g_texLock;
    std::unordered_map<unsigned, Tex> g_textures;
    // the last picture the host drew while the screen is the GPU's (R8G8B8A8, rows top down)
    std::mutex g_pictureLock;
    std::vector<uint8_t> g_picture;
    int g_pictureW = 0, g_pictureH = 0;
    uint64_t g_pictureSerial = 0;
    unsigned g_nextTexture = 1;

    Tex *findTexture(unsigned id)
    {
        auto it = g_textures.find(id);
        return it == g_textures.end() ? nullptr : &it->second;
    }

    // ------------------------------------------------------------------------------------------------------------
    // The DualSense (libScePad's public state prefix and button bits, as reUndr's input_ps5.c reads them)
    struct PadData
    {
        uint32_t buttons;
        uint8_t lx, ly, rx, ry, l2, r2;
        uint8_t reserved[66];
        uint8_t connected;
        uint8_t tail[179];
    };
    static_assert(offsetof(PadData, connected) == 76, "libScePad connected ABI");
    constexpr uint32_t kL3 = 0x2, kR3 = 0x4, kOptions = 0x8, kUp = 0x10, kRight = 0x20, kDown = 0x40, kLeft = 0x80,
                       kL2 = 0x100, kR2 = 0x200, kL1 = 0x400, kR1 = 0x800, kTriangle = 0x1000, kCircle = 0x2000,
                       kCross = 0x4000, kSquare = 0x8000, kTouchpad = 0x100000;

    struct Pad
    {
        bool init = false;
        int handle = -1;
        double nextTry = 0.0;
        PadData now{}, before{};
        bool connected = false;
    };
    Pad g_pad;
    std::mutex g_padLock;

    void pollPad()
    {
        std::lock_guard<std::mutex> lock(g_padLock);
        if (!g_pad.init)
        {
            g_pad.init = true;
            sceUserServiceInitialize(nullptr);
            scePadInit();
        }
        g_pad.before = g_pad.now;
        if (g_pad.handle < 0)
        {
            const double t = secondsSinceStart();
            if (t < g_pad.nextTry)
                return;
            g_pad.nextTry = t + 1.0;
            int user = -1;
            if (sceUserServiceGetInitialUser(&user) < 0 || user < 0)
                return;
            g_pad.handle = scePadOpen(user, 0, 0, nullptr);
            if (g_pad.handle < 0)
                g_pad.handle = scePadGetHandle(user, 0, 0);
            if (g_pad.handle >= 0)
                scePadSetVibrationMode(g_pad.handle, 2);
        }
        if (g_pad.handle < 0)
            return;
        PadData d{};
        if (scePadReadState(g_pad.handle, &d) < 0)
        {
            g_pad.handle = -1;
            g_pad.connected = false;
            std::memset(&g_pad.now, 0, sizeof g_pad.now);
            return;
        }
        g_pad.connected = d.connected != 0;
        g_pad.now = g_pad.connected ? d : PadData{};
        if (!g_pad.connected)
            g_pad.now.lx = g_pad.now.ly = g_pad.now.rx = g_pad.now.ry = 128;
    }

    uint32_t buttonBit(int button)
    {
        switch (button)
        {
        case GAMEPAD_BUTTON_LEFT_FACE_UP: return kUp;
        case GAMEPAD_BUTTON_LEFT_FACE_RIGHT: return kRight;
        case GAMEPAD_BUTTON_LEFT_FACE_DOWN: return kDown;
        case GAMEPAD_BUTTON_LEFT_FACE_LEFT: return kLeft;
        case GAMEPAD_BUTTON_RIGHT_FACE_UP: return kTriangle;
        case GAMEPAD_BUTTON_RIGHT_FACE_RIGHT: return kCircle;
        case GAMEPAD_BUTTON_RIGHT_FACE_DOWN: return kCross;
        case GAMEPAD_BUTTON_RIGHT_FACE_LEFT: return kSquare;
        case GAMEPAD_BUTTON_LEFT_TRIGGER_1: return kL1;
        case GAMEPAD_BUTTON_LEFT_TRIGGER_2: return kL2;
        case GAMEPAD_BUTTON_RIGHT_TRIGGER_1: return kR1;
        case GAMEPAD_BUTTON_RIGHT_TRIGGER_2: return kR2;
        case GAMEPAD_BUTTON_MIDDLE_LEFT: return kTouchpad; // the PS2's Select
        case GAMEPAD_BUTTON_MIDDLE_RIGHT: return kOptions; // Start
        case GAMEPAD_BUTTON_LEFT_THUMB: return kL3;
        case GAMEPAD_BUTTON_RIGHT_THUMB: return kR3;
        default: return 0;
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Sound
    struct Stream
    {
        unsigned rate = 48000, bits = 16, channels = 2;
        AudioCallback callback = nullptr;
        std::atomic<bool> playing{false};
        std::atomic<bool> started{false};
        std::atomic<float> volume{1.0f};
        int port = -1;
    };
    bool g_audioReady = false;
    std::atomic<float> g_masterVolume{1.0f};
    constexpr unsigned kGrain = 256;
    constexpr unsigned kS16Stereo = 1; // SCE_AUDIO_OUT_PARAM_FORMAT_S16_STEREO
    constexpr int kSystemUser = 255;   // SCE_USER_SERVICE_USER_ID_SYSTEM
    constexpr int kMainPort = 0;       // SCE_AUDIO_OUT_PORT_TYPE_MAIN

    void *runStream(void *p)
    {
        auto *s = static_cast<Stream *>(p);
        std::vector<uint8_t> raw(kGrain * 2 * 4);
        std::vector<int16_t> out(kGrain * 2);
        for (;;)
        {
            const float gain = s->volume.load(std::memory_order_relaxed) * g_masterVolume.load(std::memory_order_relaxed);
            if (s->playing.load(std::memory_order_acquire) && s->callback)
            {
                std::memset(raw.data(), 0, raw.size());
                s->callback(raw.data(), kGrain);
                for (unsigned i = 0; i < kGrain; ++i)
                    for (unsigned c = 0; c < 2; ++c)
                    {
                        const unsigned from = s->channels == 1 ? i : i * s->channels + c;
                        float v;
                        if (s->bits == 32)
                            std::memcpy(&v, raw.data() + size_t(from) * 4, 4);
                        else
                        {
                            int16_t x;
                            std::memcpy(&x, raw.data() + size_t(from) * 2, 2);
                            v = x / 32768.0f;
                        }
                        v = std::clamp(v * gain, -1.0f, 1.0f);
                        out[i * 2 + c] = int16_t(std::lrint(v * 32767.0f));
                    }
            }
            else
                std::fill(out.begin(), out.end(), int16_t(0));
            int rc = sceAudioOutOutput(s->port, out.data());
            if (rc < 0)
            {
                std::fprintf(stderr, "[ps5] audio: sceAudioOutOutput failed %#x, the sound stops\n", unsigned(rc));
                return nullptr;
            }
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // The default font (raylib's, 10 px high), as raylib's LoadFontDefault lays it out in a 128x128 atlas
    struct GlyphRect
    {
        int x, y, w, h;
    };
    bool g_fontReady = false;
    GlyphRect g_glyphs[224];
    uint8_t g_fontBits[128 * 128];

    void ensureFont()
    {
        if (g_fontReady)
            return;
        g_fontReady = true;
        for (int i = 0, counter = 0; i < 128 * 128; i += 32, ++counter)
            for (int j = 31; j >= 0; --j)
                g_fontBits[i + j] = (kDefaultFontBits[counter] & (1u << j)) ? 1 : 0;
        constexpr int kGlyphHeight = 10, kDivisor = 1;
        int line = 0, posX = kDivisor, testX = kDivisor;
        for (int i = 0; i < 224; ++i)
        {
            const int w = kDefaultFontWidths[i];
            g_glyphs[i] = GlyphRect{posX, kDivisor + line * (kGlyphHeight + kDivisor), w, kGlyphHeight};
            testX += w + kDivisor;
            if (testX >= 128)
            {
                ++line;
                posX = 2 * kDivisor + w;
                testX = posX;
                g_glyphs[i].x = kDivisor;
                g_glyphs[i].y = kDivisor + line * (kGlyphHeight + kDivisor);
            }
            else
                posX = testX;
        }
    }

    int nextCodepoint(const char *&p)
    {
        const unsigned char c = static_cast<unsigned char>(*p++);
        if (c < 0x80)
            return c;
        if ((c & 0xE0) == 0xC0 && (static_cast<unsigned char>(*p) & 0xC0) == 0x80)
            return ((c & 0x1F) << 6) | (static_cast<unsigned char>(*p++) & 0x3F);
        while ((static_cast<unsigned char>(*p) & 0xC0) == 0x80)
            ++p;
        return '?';
    }

    int glyphIndex(int codepoint) { return (codepoint >= 32 && codepoint < 256) ? codepoint - 32 : '?' - 32; }

    // pacing
    int g_targetFps = 0;
    Clock::time_point g_nextFrame = Clock::now();
    double g_lastFrameTime = 1.0 / 60.0;
    Clock::time_point g_lastEnd = Clock::now();
    int g_fps = 0, g_fpsFrames = 0;
    double g_fpsSince = 0.0;
}

// ----------------------------------------------------------------------------------------------------------------
// Window and drawing

void InitWindow(int, int, const char *title)
{
    if (g_windowReady)
        return;
    std::fprintf(stderr, "[ps5] InitWindow\n");
    g_frame.assign(size_t(kWidth) * kHeight, 0xff000000u);
    // VideoOut is opened at the first frame shown, unless the Vulkan renderer has taken the screen by then (a
    // process opens VideoOut once: the renderer's swapchain opens it in the driver, rc_screen_gpu_owns)
    g_windowReady = true;
    std::fprintf(stderr, "[ps5] window \"%s\": VideoOut 1920x1080 at the first frame\n", title ? title : "");
    pollPad();
}

void CloseWindow(void)
{
    if (g_video.handle >= 0)
        sceVideoOutClose(g_video.handle);
    g_video.handle = -1;
    g_windowReady = false;
}

bool WindowShouldClose(void) { return false; }
bool IsWindowReady(void) { return g_windowReady; }
void SetConfigFlags(unsigned int) {}
void SetTraceLogLevel(int) {}
void SetWindowTitle(const char *) {}

void SetTargetFPS(int fps)
{
    g_targetFps = fps > 0 ? fps : 0;
    g_nextFrame = Clock::now();
}

int GetFPS(void) { return g_fps; }
double GetTime(void) { return secondsSinceStart(); }
float GetFrameTime(void) { return static_cast<float>(g_lastFrameTime); }
int GetScreenWidth(void) { return kWidth; }
int GetScreenHeight(void) { return kHeight; }
int GetRenderWidth(void) { return kWidth; }
int GetRenderHeight(void) { return kHeight; }

void BeginDrawing(void) {}

void PollInputEvents(void) { pollPad(); }

void EndDrawing(void)
{
    if (g_gpuOwnsScreen.load(std::memory_order_acquire))
    {
        // the overlay for the renderer: what this frame drew and what the last one did (now cleared)
        const int x0 = std::min(g_dirtyX0, g_lastX0), y0 = std::min(g_dirtyY0, g_lastY0);
        const int x1 = std::max(g_dirtyX1, g_lastX1), y1 = std::max(g_dirtyY1, g_lastY1);
        if (x0 < x1 && y0 < y1)
        {
            std::lock_guard<std::mutex> lock(g_overlayLock);
            if (g_overlay.size() != g_frame.size())
                g_overlay.assign(g_frame.size(), 0u);
            for (int y = y0; y < y1; ++y)
                std::memcpy(&g_overlay[size_t(y) * kWidth + x0], &g_frame[size_t(y) * kWidth + x0], size_t(x1 - x0) * 4u);
            if (g_pubX0 < g_pubX1)
            {
                g_pubX0 = std::min(g_pubX0, x0);
                g_pubY0 = std::min(g_pubY0, y0);
                g_pubX1 = std::max(g_pubX1, x1);
                g_pubY1 = std::max(g_pubY1, y1);
            }
            else
            {
                g_pubX0 = x0;
                g_pubY0 = y0;
                g_pubX1 = x1;
                g_pubY1 = y1;
            }
            ++g_overlaySerial;
        }
        g_lastX0 = g_dirtyX0;
        g_lastY0 = g_dirtyY0;
        g_lastX1 = g_dirtyX1;
        g_lastY1 = g_dirtyY1;
    }
    else if (g_windowReady && !g_frame.empty() && g_video.handle < 0 && !g_videoTried)
    {
        g_videoTried = true;
        if (!openVideo())
            std::fprintf(stderr, "[ps5] VideoOut could not be opened: nothing will be shown\n");
        else
            std::fprintf(stderr, "[ps5] VideoOut 1920x1080 open (the host draws the picture)\n");
    }
    if (!g_gpuOwnsScreen.load(std::memory_order_relaxed) && g_video.handle >= 0 && !g_frame.empty())
    {
        // the frame into the next scan-out buffer, in the display's tiling, then out of the CPU's caches
        uint8_t *dst = g_video.buffers + size_t(g_video.next) * kBufferBytes;
        g_workers.run([&](int part, int parts)
                      {
            int y0, y1;
            partRows(part, parts, 0, kHeight, y0, y1);
            for (int y = y0; y < y1; ++y)
            {
                const uint32_t *src = g_frame.data() + size_t(y) * kWidth;
                const uint32_t tileY = g_tileY[y], inY = g_inY[y];
                for (int x = 0; x < kWidth; ++x)
                    *reinterpret_cast<uint32_t *>(dst + tileY + g_tileX[x] + (inY ^ g_inX[x])) = src[x];
            } });
        g_workers.run([&](int part, int parts)
                      {
            const size_t from = (kBufferBytes / 64) * part / parts * 64, to = (kBufferBytes / 64) * (part + 1) / parts * 64;
            for (size_t i = from; i < to; i += 64)
                __asm__ volatile("clflush (%0)" ::"r"(dst + i) : "memory"); });
        __asm__ volatile("mfence" ::: "memory");
        int rc = sceVideoOutSubmitFlip(g_video.handle, g_video.next, kFlipVsync, 0);
        if (rc < 0)
        {
            static int told = 0;
            if (told++ < 5)
                std::fprintf(stderr, "[ps5] sceVideoOutSubmitFlip failed %#x\n", unsigned(rc));
        }
        g_video.next = (g_video.next + 1) % kBuffers;
    }

    // raylib's frame pacing: at most the target frame rate
    if (g_targetFps > 0)
    {
        const auto period = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(1.0 / g_targetFps));
        g_nextFrame += period;
        const auto now = Clock::now();
        if (g_nextFrame > now)
            std::this_thread::sleep_until(g_nextFrame);
        else if (now - g_nextFrame > period * 4)
            g_nextFrame = now; // far behind: start counting again from now
    }
    const auto end = Clock::now();
    g_lastFrameTime = std::chrono::duration<double>(end - g_lastEnd).count();
    g_lastEnd = end;
    ++g_fpsFrames;
    const double t = secondsSinceStart();
    if (t - g_fpsSince >= 1.0)
    {
        g_fps = int(std::lround(g_fpsFrames / (t - g_fpsSince)));
        g_fpsFrames = 0;
        g_fpsSince = t;
    }
    pollPad();
}

void ClearBackground(Color color)
{
    if (g_frame.empty())
        return;
    if (g_gpuOwnsScreen.load(std::memory_order_relaxed))
    {
        // the overlay: transparent where nothing is drawn (only the last frame's region needs clearing)
        for (int y = g_dirtyY0; y < g_dirtyY1; ++y)
            std::fill_n(&g_frame[size_t(y) * kWidth + g_dirtyX0], g_dirtyX1 - g_dirtyX0, 0u);
        g_dirtyX0 = kWidth;
        g_dirtyY0 = kHeight;
        g_dirtyX1 = g_dirtyY1 = 0;
        return;
    }
    std::fill(g_frame.begin(), g_frame.end(), pack(color) | 0xff000000u);
}

void DrawRectangle(int posX, int posY, int width, int height, Color color) { fillRect(posX, posY, width, height, pack(color)); }

void DrawRectangleRec(Rectangle rec, Color color)
{
    DrawRectangle(int(std::lround(rec.x)), int(std::lround(rec.y)), int(std::lround(rec.width)), int(std::lround(rec.height)), color);
}

void DrawRectangleLines(int posX, int posY, int width, int height, Color color)
{
    const uint32_t c = pack(color);
    fillRect(posX, posY, width, 1, c);
    fillRect(posX, posY + height - 1, width, 1, c);
    fillRect(posX, posY + 1, 1, height - 2, c);
    fillRect(posX + width - 1, posY + 1, 1, height - 2, c);
}

void DrawTexture(Texture2D texture, int posX, int posY, Color tint)
{
    DrawTexturePro(texture, Rectangle{0, 0, float(texture.width), float(texture.height)},
                   Rectangle{float(posX), float(posY), float(texture.width), float(texture.height)}, Vector2{0, 0}, 0.0f, tint);
}

// The source rectangle of the texture scaled into dest (origin and rotation are always 0 in the runtime). A negative
// source width or height flips that way, as in raylib. Bilinear for a texture set to TEXTURE_FILTER_BILINEAR.
void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest, Vector2 origin, float, Color tint)
{
    // the GPU's screen: the picture goes to the renderer, which shows it when it has no picture of its own (the
    // emulated GS's frames: the boot, the films)
    if (g_gpuOwnsScreen.load(std::memory_order_relaxed))
    {
        std::lock_guard<std::mutex> lock(g_texLock);
        Tex *tex = findTexture(texture.id);
        if (!tex || tex->rgba.empty())
            return;
        const int sx = std::clamp(int(std::lround(std::min(source.x, source.x + source.width))), 0, tex->width);
        const int sy = std::clamp(int(std::lround(std::min(source.y, source.y + source.height))), 0, tex->height);
        const int sw = std::min(tex->width - sx, int(std::lround(std::fabs(source.width))));
        const int sh = std::min(tex->height - sy, int(std::lround(std::fabs(source.height))));
        if (sw <= 0 || sh <= 0)
            return;
        std::lock_guard<std::mutex> plock(g_pictureLock);
        g_picture.resize(size_t(sw) * sh * 4u);
        const bool flipY = source.height < 0.0f;
        for (int y = 0; y < sh; ++y)
        {
            const int from = sy + (flipY ? sh - 1 - y : y);
            std::memcpy(&g_picture[size_t(y) * sw * 4u], &tex->rgba[(size_t(from) * tex->width + sx) * 4u], size_t(sw) * 4u);
        }
        g_pictureW = sw;
        g_pictureH = sh;
        ++g_pictureSerial;
        return;
    }
    if (g_frame.empty() || dest.width <= 0.0f || dest.height <= 0.0f)
        return;
    std::lock_guard<std::mutex> lock(g_texLock);
    Tex *tex = findTexture(texture.id);
    if (!tex || tex->rgba.empty() || tex->width <= 0 || tex->height <= 0)
        return;
    const bool flipX = source.width < 0.0f, flipY = source.height < 0.0f;
    const float sw = std::fabs(source.width), sh = std::fabs(source.height);
    if (sw <= 0.0f || sh <= 0.0f)
        return;
    const float dx0 = dest.x - origin.x, dy0 = dest.y - origin.y;
    const int x0 = std::max(0, int(std::floor(dx0))), x1 = std::min(kWidth, int(std::ceil(dx0 + dest.width)));
    const int y0 = std::max(0, int(std::floor(dy0))), y1 = std::min(kHeight, int(std::ceil(dy0 + dest.height)));
    if (x0 >= x1 || y0 >= y1)
        return;
    const int tw = tex->width, th = tex->height;
    const bool bilinear = tex->filter != TEXTURE_FILTER_POINT;
    const bool plainTint = tint.r == 255 && tint.g == 255 && tint.b == 255 && tint.a == 255;

    // per column: the source texel and its fraction (8 bits), in 1/256 texel steps
    struct Tap
    {
        int a, b, f;
    };
    std::vector<Tap> cols(size_t(x1 - x0)), rows(size_t(y1 - y0));
    auto taps = [&](std::vector<Tap> &out, int from, float d0, float dlen, float s0, float slen, bool flip, int limit)
    {
        for (size_t i = 0; i < out.size(); ++i)
        {
            float t = (float(from + int(i)) + 0.5f - d0) / dlen; // 0..1 across dest
            t = std::clamp(t, 0.0f, 1.0f);
            if (flip)
                t = 1.0f - t;
            float s = s0 + t * slen - (bilinear ? 0.5f : 0.0f); // texel coordinate
            if (!bilinear)
            {
                const int p = std::clamp(int(std::floor(s)), 0, limit - 1);
                out[i] = Tap{p, p, 0};
                continue;
            }
            s = std::clamp(s, 0.0f, float(limit - 1));
            const int p = int(std::floor(s));
            out[i] = Tap{p, std::min(p + 1, limit - 1), int((s - float(p)) * 256.0f)};
        }
    };
    taps(cols, x0, dx0, dest.width, source.x, sw, flipX, tw);
    taps(rows, y0, dy0, dest.height, source.y, sh, flipY, th);

    const uint32_t *texels = reinterpret_cast<const uint32_t *>(tex->rgba.data()); // R G B A bytes
    auto fetch = [&](int x, int y) -> uint32_t
    {
        const uint32_t v = texels[size_t(y) * tw + x]; // 0xAABBGGRR
        return (v & 0xff00ff00u) | ((v & 0xffu) << 16) | ((v >> 16) & 0xffu); // to 0xAARRGGBB
    };
    auto lerp = [](uint32_t p, uint32_t q, int f) -> uint32_t
    {
        const uint32_t g = 256u - uint32_t(f);
        const uint32_t rb = (((p & 0x00ff00ffu) * g + (q & 0x00ff00ffu) * uint32_t(f)) >> 8) & 0x00ff00ffu;
        const uint32_t ag = (((p >> 8) & 0x00ff00ffu) * g + ((q >> 8) & 0x00ff00ffu) * uint32_t(f)) & 0xff00ff00u;
        return rb | ag;
    };
    g_workers.run([&](int part, int parts)
                  {
        int ya, yb;
        partRows(part, parts, y0, y1, ya, yb);
        for (int y = ya; y < yb; ++y)
        {
            const Tap &ty = rows[size_t(y - y0)];
            uint32_t *out = g_frame.data() + size_t(y) * kWidth;
            for (int x = x0; x < x1; ++x)
            {
                const Tap &tx = cols[size_t(x - x0)];
                uint32_t c;
                if (!bilinear)
                    c = fetch(tx.a, ty.a);
                else
                    c = lerp(lerp(fetch(tx.a, ty.a), fetch(tx.b, ty.a), tx.f), lerp(fetch(tx.a, ty.b), fetch(tx.b, ty.b), tx.f), ty.f);
                if (!plainTint)
                {
                    const uint32_t a = ((c >> 24) * tint.a) / 255u, r = (((c >> 16) & 255u) * tint.r) / 255u,
                                   g = (((c >> 8) & 255u) * tint.g) / 255u, b = ((c & 255u) * tint.b) / 255u;
                    c = (a << 24) | (r << 16) | (g << 8) | b;
                }
                out[x] = blend(out[x], c);
            }
        } });
}

// raylib's DrawText: the default font scaled to fontSize, fontSize / 10 spacing
void DrawText(const char *text, int posX, int posY, int fontSize, Color color)
{
    if (!text || g_frame.empty())
        return;
    ensureFont();
    fontSize = std::max(fontSize, 10);
    const float scale = fontSize / 10.0f;
    const int spacing = fontSize / 10;
    const uint32_t c = pack(color);
    float x = float(posX);
    int y = posY;
    for (const char *p = text; *p;)
    {
        const int cp = nextCodepoint(p);
        if (cp == '\n')
        {
            x = float(posX);
            y += fontSize + fontSize / 2;
            continue;
        }
        const GlyphRect &g = g_glyphs[glyphIndex(cp)];
        if (cp != ' ')
        {
            const int gw = int(std::lround(g.w * scale)), gh = fontSize;
            touch(std::max(0, int(x)), std::max(0, y), std::min(kWidth, int(x) + gw), std::min(kHeight, y + gh));
            for (int j = 0; j < gh; ++j)
            {
                const int py = y + j;
                if (py < 0 || py >= kHeight)
                    continue;
                const int sy = g.y + std::min(g.h - 1, int(j / scale));
                for (int i = 0; i < gw; ++i)
                {
                    const int px = int(x) + i;
                    if (px < 0 || px >= kWidth)
                        continue;
                    const int sx = g.x + std::min(g.w - 1, int(i / scale));
                    if (g_fontBits[sy * 128 + sx])
                    {
                        uint32_t &d = g_frame[size_t(py) * kWidth + px];
                        d = blend(d, c);
                    }
                }
            }
        }
        x += g.w * scale + spacing;
    }
}

int MeasureText(const char *text, int fontSize)
{
    if (!text)
        return 0;
    ensureFont();
    fontSize = std::max(fontSize, 10);
    const float scale = fontSize / 10.0f;
    const int spacing = fontSize / 10;
    float width = 0.0f, best = 0.0f;
    int glyphs = 0;
    for (const char *p = text; *p;)
    {
        const int cp = nextCodepoint(p);
        if (cp == '\n')
        {
            best = std::max(best, width);
            width = 0.0f;
            glyphs = 0;
            continue;
        }
        width += g_glyphs[glyphIndex(cp)].w * scale;
        ++glyphs;
    }
    best = std::max(best, width);
    return int(best) + std::max(0, glyphs - 1) * spacing;
}

void DrawFPS(int posX, int posY)
{
    char text[32];
    std::snprintf(text, sizeof text, "%d FPS", GetFPS());
    DrawText(text, posX, posY, 20, Color{0, 228, 48, 255});
}

// ----------------------------------------------------------------------------------------------------------------
// Images and textures

Image GenImageColor(int width, int height, Color color)
{
    Image image{};
    if (width <= 0 || height <= 0)
        return image;
    auto *px = static_cast<uint8_t *>(std::malloc(size_t(width) * height * 4));
    if (!px)
        return image;
    for (size_t i = 0; i < size_t(width) * height; ++i)
    {
        px[i * 4 + 0] = color.r;
        px[i * 4 + 1] = color.g;
        px[i * 4 + 2] = color.b;
        px[i * 4 + 3] = color.a;
    }
    image.data = px;
    image.width = width;
    image.height = height;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return image;
}

void ImageCrop(Image *image, Rectangle crop)
{
    if (!image || !image->data)
        return;
    const int x = std::clamp(int(crop.x), 0, image->width), y = std::clamp(int(crop.y), 0, image->height);
    const int w = std::clamp(int(crop.width), 0, image->width - x), h = std::clamp(int(crop.height), 0, image->height - y);
    auto *px = static_cast<uint8_t *>(std::malloc(size_t(std::max(1, w * h)) * 4));
    if (!px)
        return;
    for (int j = 0; j < h; ++j)
        std::memcpy(px + size_t(j) * w * 4, static_cast<uint8_t *>(image->data) + (size_t(y + j) * image->width + x) * 4, size_t(w) * 4);
    std::free(image->data);
    image->data = px;
    image->width = w;
    image->height = h;
}

void UnloadImage(Image image) { std::free(image.data); }

// screenshots: not on the console
bool ExportImage(Image, const char *) { return false; }

Texture2D LoadTextureFromImage(Image image)
{
    Texture2D t{};
    if (!image.data || image.width <= 0 || image.height <= 0)
        return t;
    std::lock_guard<std::mutex> lock(g_texLock);
    const unsigned id = g_nextTexture++;
    Tex &tex = g_textures[id];
    tex.width = image.width;
    tex.height = image.height;
    tex.rgba.assign(static_cast<uint8_t *>(image.data), static_cast<uint8_t *>(image.data) + size_t(image.width) * image.height * 4);
    t.id = id;
    t.width = image.width;
    t.height = image.height;
    t.mipmaps = 1;
    t.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return t;
}

Image LoadImageFromTexture(Texture2D texture)
{
    Image image{};
    std::lock_guard<std::mutex> lock(g_texLock);
    Tex *tex = findTexture(texture.id);
    if (!tex || tex->rgba.empty())
        return image;
    auto *px = static_cast<uint8_t *>(std::malloc(tex->rgba.size()));
    if (!px)
        return image;
    std::memcpy(px, tex->rgba.data(), tex->rgba.size());
    image.data = px;
    image.width = tex->width;
    image.height = tex->height;
    image.mipmaps = 1;
    image.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
    return image;
}

void UpdateTexture(Texture2D texture, const void *pixels)
{
    if (!pixels)
        return;
    std::lock_guard<std::mutex> lock(g_texLock);
    Tex *tex = findTexture(texture.id);
    if (!tex)
        return;
    std::memcpy(tex->rgba.data(), pixels, tex->rgba.size());
}

void UpdateTextureRec(Texture2D texture, Rectangle rec, const void *pixels)
{
    if (!pixels)
        return;
    std::lock_guard<std::mutex> lock(g_texLock);
    Tex *tex = findTexture(texture.id);
    if (!tex)
        return;
    const int x = std::clamp(int(rec.x), 0, tex->width), y = std::clamp(int(rec.y), 0, tex->height);
    const int w = std::clamp(int(rec.width), 0, tex->width - x), h = std::clamp(int(rec.height), 0, tex->height - y);
    for (int j = 0; j < h; ++j)
        std::memcpy(tex->rgba.data() + (size_t(y + j) * tex->width + x) * 4, static_cast<const uint8_t *>(pixels) + size_t(j) * int(rec.width) * 4, size_t(w) * 4);
}

void UnloadTexture(Texture2D texture)
{
    std::lock_guard<std::mutex> lock(g_texLock);
    g_textures.erase(texture.id);
}

bool IsTextureValid(Texture2D texture)
{
    std::lock_guard<std::mutex> lock(g_texLock);
    return findTexture(texture.id) != nullptr;
}

void SetTextureFilter(Texture2D texture, int filter)
{
    std::lock_guard<std::mutex> lock(g_texLock);
    if (Tex *tex = findTexture(texture.id))
        tex->filter = filter;
}

// ----------------------------------------------------------------------------------------------------------------
// Input

bool IsKeyDown(int) { return false; }
bool IsKeyPressed(int) { return false; }

bool IsGamepadAvailable(int gamepad)
{
    std::lock_guard<std::mutex> lock(g_padLock);
    return gamepad == 0 && g_pad.connected;
}

bool IsGamepadButtonDown(int gamepad, int button)
{
    if (gamepad != 0)
        return false;
    std::lock_guard<std::mutex> lock(g_padLock);
    const uint32_t bit = buttonBit(button);
    return bit && (g_pad.now.buttons & bit) != 0;
}

bool IsGamepadButtonPressed(int gamepad, int button)
{
    if (gamepad != 0)
        return false;
    std::lock_guard<std::mutex> lock(g_padLock);
    const uint32_t bit = buttonBit(button);
    return bit && (g_pad.now.buttons & bit) != 0 && (g_pad.before.buttons & bit) == 0;
}

float GetGamepadAxisMovement(int gamepad, int axis)
{
    if (gamepad != 0)
        return 0.0f;
    std::lock_guard<std::mutex> lock(g_padLock);
    if (!g_pad.connected)
        return axis >= GAMEPAD_AXIS_LEFT_TRIGGER ? -1.0f : 0.0f;
    auto stick = [](uint8_t v) { return std::clamp((float(v) - 128.0f) / 127.0f, -1.0f, 1.0f); };
    switch (axis)
    {
    case GAMEPAD_AXIS_LEFT_X: return stick(g_pad.now.lx);
    case GAMEPAD_AXIS_LEFT_Y: return stick(g_pad.now.ly);
    case GAMEPAD_AXIS_RIGHT_X: return stick(g_pad.now.rx);
    case GAMEPAD_AXIS_RIGHT_Y: return stick(g_pad.now.ry);
    case GAMEPAD_AXIS_LEFT_TRIGGER: return g_pad.now.l2 / 127.5f - 1.0f;
    case GAMEPAD_AXIS_RIGHT_TRIGGER: return g_pad.now.r2 / 127.5f - 1.0f;
    default: return 0.0f;
    }
}

bool SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float)
{
    if (gamepad != 0)
        return false;
    std::lock_guard<std::mutex> lock(g_padLock);
    if (g_pad.handle < 0)
        return false;
    const uint8_t motors[2] = {uint8_t(std::clamp(leftMotor, 0.0f, 1.0f) * 255.0f), uint8_t(std::clamp(rightMotor, 0.0f, 1.0f) * 255.0f)};
    return scePadSetVibration(g_pad.handle, motors) >= 0;
}

// ----------------------------------------------------------------------------------------------------------------
// Audio

void InitAudioDevice(void)
{
    if (g_audioReady)
        return;
    g_audioReady = sceAudioOutInit() >= 0 || true; // already initialised also answers an error
}

void CloseAudioDevice(void) {}
bool IsAudioDeviceReady(void) { return g_audioReady; }
void SetMasterVolume(float volume) { g_masterVolume.store(std::clamp(volume, 0.0f, 1.0f)); }
void SetAudioStreamBufferSizeDefault(int) {}

AudioStream LoadAudioStream(unsigned int sampleRate, unsigned int sampleSize, unsigned int channels)
{
    AudioStream s{};
    auto *st = new Stream;
    st->rate = sampleRate;
    st->bits = sampleSize == 32 ? 32 : 16;
    st->channels = channels == 1 ? 1 : 2;
    if (sampleRate != 48000)
        std::fprintf(stderr, "[ps5] audio: a %u Hz stream is played at 48000 Hz\n", sampleRate);
    s.buffer = st;
    s.sampleRate = sampleRate;
    s.sampleSize = sampleSize;
    s.channels = channels;
    return s;
}

void UnloadAudioStream(AudioStream stream)
{
    if (auto *st = static_cast<Stream *>(stream.buffer))
        st->playing.store(false); // its thread keeps the port open (silent): streams are not unloaded while playing
}

bool IsAudioStreamValid(AudioStream stream) { return stream.buffer != nullptr; }

void SetAudioStreamCallback(AudioStream stream, AudioCallback callback)
{
    if (auto *st = static_cast<Stream *>(stream.buffer))
        st->callback = callback;
}

void PlayAudioStream(AudioStream stream)
{
    auto *st = static_cast<Stream *>(stream.buffer);
    if (!st)
        return;
    st->playing.store(true, std::memory_order_release);
    if (st->started.exchange(true))
        return;
    st->port = sceAudioOutOpen(kSystemUser, kMainPort, 0, kGrain, 48000, kS16Stereo);
    if (st->port < 0)
    {
        std::fprintf(stderr, "[ps5] audio: sceAudioOutOpen failed %#x, the game runs without sound\n", unsigned(st->port));
        return;
    }
    const int unity[2] = {32768, 32768};
    sceAudioOutSetVolume(st->port, 3, unity);
    std::fprintf(stderr, "[ps5] audio: 48000 Hz 16-bit stereo, port %d\n", st->port);
    pthread_t t;
    pthread_attr_t a;
    pthread_attr_init(&a);
    pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
    pthread_create(&t, &a, runStream, st);
    pthread_attr_destroy(&a);
}

void StopAudioStream(AudioStream stream)
{
    if (auto *st = static_cast<Stream *>(stream.buffer))
        st->playing.store(false);
}

void PauseAudioStream(AudioStream stream) { StopAudioStream(stream); }

void ResumeAudioStream(AudioStream stream)
{
    if (auto *st = static_cast<Stream *>(stream.buffer))
        st->playing.store(true);
}

bool IsAudioStreamPlaying(AudioStream stream)
{
    auto *st = static_cast<Stream *>(stream.buffer);
    return st && st->playing.load();
}

void SetAudioStreamVolume(AudioStream stream, float volume)
{
    if (auto *st = static_cast<Stream *>(stream.buffer))
        st->volume.store(std::clamp(volume, 0.0f, 1.0f));
}

// ---------------------------------------------------------------------------------------------------------------
// The Vulkan renderer's side (ps5/vk/native_renderer_vk.cpp, rc_screen.h)
extern "C" void rc_screen_gpu_owns(void)
{
    if (g_gpuOwnsScreen.exchange(true))
        return;
    sceSystemServiceHideSplashScreen();
    if (!g_frame.empty())
        std::fill(g_frame.begin(), g_frame.end(), 0u);
    std::fprintf(stderr, "[ps5] the screen is the Vulkan renderer's: the host draws its overlay only\n");
}

extern "C" int rc_screen_overlay_size(int *width, int *height)
{
    *width = kWidth;
    *height = kHeight;
    return 1;
}

// The overlay's region changed since the last call into dst (rows of x1 - x0 pixels, B8G8R8A8), 1 when there is one.
extern "C" int rc_screen_overlay_take(void *dst, size_t capacity, int *x0, int *y0, int *x1, int *y1)
{
    std::lock_guard<std::mutex> lock(g_overlayLock);
    if (g_pubX0 >= g_pubX1 || g_pubY0 >= g_pubY1 || g_overlay.empty())
        return 0;
    const size_t row = size_t(g_pubX1 - g_pubX0) * 4u;
    if (row * size_t(g_pubY1 - g_pubY0) > capacity)
        return 0;
    for (int y = g_pubY0; y < g_pubY1; ++y)
        std::memcpy(static_cast<uint8_t *>(dst) + size_t(y - g_pubY0) * row, &g_overlay[size_t(y) * kWidth + g_pubX0], row);
    *x0 = g_pubX0;
    *y0 = g_pubY0;
    *x1 = g_pubX1;
    *y1 = g_pubY1;
    g_pubX0 = g_pubX1 = g_pubY0 = g_pubY1 = 0;
    return 1;
}

// The picture the host drew last (the emulated GS's), when newer than *serial: R8G8B8A8 rows, top down.
extern "C" int rc_screen_picture_take(void *dst, size_t capacity, int *width, int *height, unsigned long long *serial)
{
    std::lock_guard<std::mutex> lock(g_pictureLock);
    if (g_pictureSerial == *serial || g_picture.empty() || g_picture.size() > capacity)
        return 0;
    std::memcpy(dst, g_picture.data(), g_picture.size());
    *width = g_pictureW;
    *height = g_pictureH;
    *serial = g_pictureSerial;
    return 1;
}

extern "C" unsigned long long rc_screen_overlay_serial(void)
{
    std::lock_guard<std::mutex> lock(g_overlayLock);
    return g_overlaySerial;
}
