// reColossus: the app's entry on a PS5. Finds the player's copy of Shadow of the Colossus (PS2, USA, SCUS-97472),
// checks it is the release this build was recompiled from, and starts the PS2Recomp runtime on its files, as
// Celosia's runner starts on the extracted disc (./celosia run).
// The first start unpacks the player's disc image (.iso) into /data/sotc/game, with a screen of its own, and the game
// starts from that folder from then on (the .iso can then be deleted), as reKing does. The console parts (affinity,
// notifications, the log through a pipe, the screens) follow reKing's ps5/main_ps5.cpp and reUndr-ps5's main_ps5.c
// (MIT, bystritskiy).
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <streambuf>
#include <thread>
#include <string>
#include <vector>

#include "raylib.h"
#include "ps5/iso9660.h"
#include "ps5/screen_ps5.h"
#include "ps5/sys_ps5.h"

// The FNV-1a 64 hashes of the four files this build's code was recompiled from (build-ps5.sh writes the file from
// the disc it was made with): kBootElf, kKernelXff, kManagerXff, kGamecoreXff.
#include "game_hashes.inc"

namespace native_renderer
{
    bool available(); // Celosia's native renderer (src/lib/native/native_renderer.cpp): made by the first call
}

#define RC_STRING2(x) #x
#define RC_STRING(x) RC_STRING2(x)
#ifdef RECOLOSSUS_VERSION_RAW
#define RECOLOSSUS_VERSION RC_STRING(RECOLOSSUS_VERSION_RAW)
#else
#define RECOLOSSUS_VERSION "dev"
#endif

// PS2Recomp's main() (ps2xRuntime/src/main.cpp, built as ps2x_main for the PS5)
int ps2x_main(int argc, char *argv[]);

namespace
{
    namespace fs = std::filesystem;

    const fs::path kData = "/data/sotc";
    constexpr const char *kBoot = "SCUS_974.72";
    const char *const kModules[] = {"KERNEL.XFF", "MANAGER.XFF", "GAMECORE.XFF"};

    void Notify(const char *text)
    {
        struct
        {
            char reserved[45];
            char message[3075];
        } note{};
        std::snprintf(note.message, sizeof note.message, "%s", text);
        sceKernelSendNotificationRequest(0, &note, sizeof note, 0);
    }

    void Log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
    // With logging: a thread that asks to sleep 1 s at a time and logs, every 5 s, how much later than asked it
    // woke at worst (the process starved of CPU time shows here first) and the flexible memory left.
    void StartMonitor();
    void Log(const char *fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        std::fprintf(stderr, "[reColossus] ");
        std::vfprintf(stderr, fmt, args);
        std::fprintf(stderr, "\n");
        va_end(args);
    }
    void StartMonitor()
    {
        std::thread([]
                    {
            using clock = std::chrono::steady_clock;
            double worstMs = 0.0;
            for (int tick = 1;; ++tick)
            {
                const auto before = clock::now();
                std::this_thread::sleep_for(std::chrono::seconds(1));
                const double late = std::chrono::duration<double, std::milli>(clock::now() - before).count() - 1000.0;
                worstMs = std::max(worstMs, late);
                if (tick % 5 == 0)
                {
                    size_t flexible = 0, direct = 0;
                    int64_t at = 0;
                    sceKernelAvailableFlexibleMemorySize(&flexible);
                    sceKernelAvailableDirectMemorySize(0, (int64_t)sceKernelGetDirectMemorySize(), 0x10000, &at, &direct);
                    Log("monitor: t=%d s, woke up to %.1f ms late, memory left: flexible %zu MB, direct (largest free) %zu MB", tick,
                        worstMs, flexible >> 20, direct >> 20);
                    worstMs = 0.0;
                }
            } })
            .detach();
    }

    // name in dir, in whatever case the disc has it
    fs::path Child(const fs::path &dir, const char *name)
    {
        std::error_code ec;
        for (const auto &e : fs::directory_iterator(dir, ec))
            if (strcasecmp(e.path().filename().c_str(), name) == 0)
                return e.path();
        return {};
    }

    uint64_t Fnv1a(const uint8_t *data, size_t size)
    {
        uint64_t h = 0xcbf29ce484222325ull;
        for (size_t i = 0; i < size; ++i)
            h = (h ^ data[i]) * 0x100000001b3ull;
        return h;
    }

    bool ReadFile(const fs::path &path, std::vector<uint8_t> &out)
    {
        std::ifstream f(path, std::ios::binary);
        if (!f)
            return false;
        out.assign(std::istreambuf_iterator<char>(f), {});
        return true;
    }

    const uint64_t *ExpectedHashes()
    {
        static const uint64_t hashes[4] = {kBootElf, kKernelXff, kManagerXff, kGamecoreXff};
        return hashes;
    }

    // 0: the release this build was made from; 1: another game (no boot ELF of SotC USA); 2: another release
    int CheckFiles(const std::vector<uint8_t> *files[4])
    {
        if (!files[0] || files[0]->empty())
            return 1;
        for (int i = 0; i < 4; ++i)
            if (!files[i] || Fnv1a(files[i]->data(), files[i]->size()) != ExpectedHashes()[i])
                return 2;
        return 0;
    }

    // the game's files: /data/sotc/game, then sotc/game on a USB drive
    bool Unpacked(const fs::path &dir)
    {
        std::error_code ec;
        if (Child(dir, kBoot).empty())
            return false;
        for (const char *m : kModules)
            if (Child(dir, m).empty())
                return false;
        return fs::is_directory(dir, ec);
    }

    fs::path GameDir() { return kData / "game"; }

    bool FindGame(fs::path &game)
    {
        std::vector<fs::path> dirs{GameDir()};
        for (int i = 0; i < 8; i++)
            dirs.push_back("/mnt/usb" + std::to_string(i) + "/sotc/game");
        for (const auto &d : dirs)
            if (Unpacked(d))
            {
                game = d;
                return true;
            }
        return false;
    }

    std::vector<fs::path> FindImages()
    {
        std::vector<fs::path> dirs{kData}, images;
        for (int i = 0; i < 8; i++)
        {
            dirs.push_back("/mnt/usb" + std::to_string(i) + "/sotc");
            dirs.push_back("/mnt/usb" + std::to_string(i));
        }
        for (const auto &dir : dirs)
        {
            std::error_code ec;
            std::vector<fs::path> here;
            for (const auto &e : fs::directory_iterator(dir, ec))
            {
                std::string n = e.path().filename().string();
                if (n.size() < 5 || n[0] == '.' || strcasecmp(n.c_str() + n.size() - 4, ".iso") != 0)
                    continue;
                if (e.is_regular_file(ec))
                    here.push_back(e.path());
            }
            std::sort(here.begin(), here.end());
            images.insert(images.end(), here.begin(), here.end());
        }
        return images;
    }

    // ------------------------------------------------------------------------------------------------------------
    // The screens before the game (screen_ps5.c). Its font has letters, digits and . , : - _ / ( ) ' % only.
    constexpr uint32_t kAccent = 0xffc9b48a;
    using Lines = std::vector<RcScreenLine>;

    void WaitForCross()
    {
        bool was = true; // a Cross still held from starting the app doesn't count
        for (;;)
        {
            PollInputEvents();
            bool down = IsGamepadButtonDown(0, GAMEPAD_BUTTON_RIGHT_FACE_DOWN);
            if (down && !was)
                return;
            was = down;
            sceKernelUsleep(50000);
        }
    }

    [[noreturn]] void Fail(const Lines &lines, const char *note)
    {
        Log("%s", note);
        Notify(note);
        if (rc_screen_show(lines.data(), unsigned(lines.size()), -1) == 0)
            WaitForCross();
        else
            sceKernelUsleep(200000);
        rc_screen_close();
        std::fflush(stderr);
        _exit(0);
    }

    [[noreturn]] void NotFound()
    {
        Fail({{260, 7, kAccent, "Game not found"},
              {380, 4, 0, "reColossus runs your own copy of the PS2 game, USA release."},
              {450, 4, 0, "Copy your disc image (.iso) to one of these places:"},
              {520, 4, kAccent, "/data/sotc/"},
              {570, 4, kAccent, "/mnt/usb0/sotc/  or  /mnt/usb0/   (usb0 to usb7)"},
              {650, 4, 0, "Or copy the files of the disc to this folder:"},
              {700, 4, kAccent, "/data/sotc/game/"},
              {860, 4, 0, "Then start reColossus again. Press Cross to close."}},
             "reColossus: game not found. Copy your disc image (.iso) to /data/sotc/. See README.txt.");
    }

    [[noreturn]] void Damaged()
    {
        Fail({{260, 7, kAccent, "The disc image is damaged"},
              {380, 4, 0, "It isn't a PS2 DVD image, or it is incomplete."},
              {440, 4, 0, "Make it again from your disc, or copy it to the console again."},
              {860, 4, 0, "Press Cross to close."}},
             "reColossus: the disc image is damaged. See README.txt.");
    }

    std::string g_refusedPath;

    [[noreturn]] void OtherGame()
    {
        static std::string remove;
        remove = "Remove " + g_refusedPath + " and copy yours instead.";
        Fail({{260, 7, kAccent, "This is another game"},
              {380, 4, 0, "reColossus runs Shadow of the Colossus for the PS2, USA release."},
              {440, 4, 0, remove.c_str()},
              {860, 4, 0, "Press Cross to close."}},
             "reColossus: that disc is another game. It needs Shadow of the Colossus (PS2, USA).");
    }

    [[noreturn]] void OtherRelease()
    {
        static std::string remove;
        remove = "Remove " + g_refusedPath + " and copy yours instead.";
        Fail({{260, 7, kAccent, "This is another release of the game"},
              {380, 4, 0, "This build was made from the USA release (SCUS-97472)"},
              {440, 4, 0, "and only runs that one."},
              {500, 4, 0, remove.c_str()},
              {860, 4, 0, "Press Cross to close."}},
             "reColossus: this is another release of the game (not the USA one this build was made from).");
    }

    [[noreturn]] void NoRoom(uint64_t total)
    {
        static char line[96];
        std::snprintf(line, sizeof line, "The game needs about %.1f GB free on the console.", double(total) / 1e9);
        Fail({{260, 7, kAccent, "Not enough room"},
              {380, 4, 0, line},
              {440, 4, 0, "Make room, or put the unpacked disc in sotc/game on a USB drive."},
              {860, 4, 0, "Press Cross to close."}},
             "reColossus: not enough room on the console to unpack the disc image. See README.txt.");
    }

    char g_sizeLine[96];
    bool g_shown = true;

    void Progress(unsigned percent, uint64_t total)
    {
        if (percent == 0)
            std::snprintf(g_sizeLine, sizeof g_sizeLine, "This happens once. The game needs about %.1f GB on the console.",
                          double(total) / 1e9);
        const RcScreenLine lines[] = {{260, 7, kAccent, "Unpacking your disc image"},
                                      {380, 4, 0, g_sizeLine},
                                      {440, 4, 0, "When it is done, the game starts and you can delete the .iso."}};
        if (g_shown)
            g_shown = rc_screen_show(lines, 3, int(percent)) == 0;
        if (!g_shown && percent % 20 == 0)
        {
            char note[96];
            std::snprintf(note, sizeof note, "reColossus: unpacking your disc image (once), %u%%", percent);
            Notify(note);
        }
    }

    // The images, until one of the game is unpacked into the game folder (through game.part, renamed when complete).
    bool UnpackImage(const std::vector<fs::path> &images)
    {
        const fs::path target = GameDir(), part = target.string() + ".part";
        int worst = 0; // 1 damaged, 2 another game, 3 another release
        uint64_t noRoom = 0;
        for (const auto &image : images)
        {
            std::error_code ec;
            fs::remove_all(part, ec);
            int error = 0;
            RcIso *iso = rc_iso_open(image.c_str(), &error);
            if (!iso)
            {
                Log("unpack: %s: %s", image.c_str(), error == RC_ISO_DAMAGED ? "damaged" : "not a disc image");
                worst = std::max(worst, error == RC_ISO_DAMAGED ? 1 : 0);
                continue;
            }
            // the four files the recompiled code comes from, checked before anything is copied
            std::vector<uint8_t> data[4];
            const std::vector<uint8_t> *files[4] = {};
            const char *names[4] = {kBoot, kModules[0], kModules[1], kModules[2]};
            for (int i = 0; i < 4; ++i)
            {
                uint8_t *bytes = nullptr;
                size_t size = 0;
                if (rc_iso_read_file(iso, names[i], &bytes, &size) == RC_ISO_OK)
                {
                    data[i].assign(bytes, bytes + size);
                    files[i] = &data[i];
                }
                std::free(bytes);
            }
            const int check = CheckFiles(files);
            if (check)
            {
                Log("unpack: %s is %s", image.c_str(), check == 1 ? "another game" : "another release");
                if (1 + check > worst)
                {
                    worst = 1 + check;
                    g_refusedPath = image.string();
                }
                rc_iso_close(iso);
                continue;
            }
            Log("unpack: %s into %s", image.c_str(), target.c_str());
            const uint64_t total = rc_iso_total(iso);
            int rc = rc_iso_extract(iso, part.c_str(), Progress);
            rc_iso_close(iso);
            if (rc == RC_ISO_OK)
            {
                if (fs::exists(target, ec)) // an incomplete game folder there already: kept beside, not deleted
                {
                    std::string aside = target.string() + ".incomplete";
                    for (int n = 2; fs::exists(aside, ec); n++)
                        aside = target.string() + ".incomplete-" + std::to_string(n);
                    fs::rename(target, aside, ec);
                }
                fs::rename(part, target, ec);
                if (!ec && Unpacked(target))
                {
                    Log("unpack: done");
                    return true;
                }
                rc = RC_ISO_WRITE;
            }
            Log("unpack: %s: %s", image.c_str(), rc == RC_ISO_WRITE ? "the folder could not be written (no room?)" : "damaged");
            fs::remove_all(part, ec);
            if (rc == RC_ISO_WRITE)
                noRoom = total;
            else
                worst = std::max(worst, 1);
        }
        if (noRoom)
            NoRoom(noRoom);
        if (worst == 2)
            OtherGame();
        if (worst == 3)
            OtherRelease();
        if (worst == 1 || !images.empty())
            Damaged();
        return false;
    }

    // ------------------------------------------------------------------------------------------------------------
    // The log: stderr and stdout through a pipe a thread of its own drains to a file (reKing's OpenLog): the console
    // ends a crashing app at once, and the file still holds nearly everything up to that moment.
    struct Sink
    {
        int from, to;
    };
    void *Drain(void *p)
    {
        auto *s = static_cast<Sink *>(p);
        char buf[16384];
        for (;;)
        {
            ssize_t n = read(s->from, buf, sizeof buf);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            if (s->to < 0)
                continue; // no log: the lines are dropped
            for (ssize_t done = 0; done < n;)
            {
                ssize_t w = write(s->to, buf + done, size_t(n - done));
                if (w < 0 && errno == EINTR)
                    continue;
                if (w <= 0)
                    break;
                done += w;
            }
        }
        return nullptr;
    }
    FILE *OpenLog(const char *path)
    {
        int fd = -1;
        if (path && (fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0)
            return nullptr;
        // Logging on or off, the lines go through the pipe: a line is one write into memory, and the drain thread
        // takes them to the file in blocks of up to 16 KB. (0.2.1 and 0.2.2 wrote into the file unbuffered: the
        // PS5's libc writes a printf in pieces, each a write to /data, and the native renderer's frame log took
        // longer to write than a frame: every frame was slow, logged a line, and the game never left the loading
        // ring, slowing until frames took seconds.)
        int ends[2];
        if (pipe(ends) == 0)
        {
            static Sink sink;
            sink = {ends[0], fd};
            pthread_t t;
            if (FILE *stream = fdopen(ends[1], "w"); stream && pthread_create(&t, nullptr, Drain, &sink) == 0)
            {
                pthread_detach(t);
                std::setvbuf(stream, nullptr, _IOLBF, 16384); // a line, however long, in one write
                return stream;
            }
            close(ends[0]);
            close(ends[1]);
        }
        return fd >= 0 ? fdopen(fd, "w") : nullptr;
    }

    // std::cout and std::cerr into the log too: libc++'s streams keep the FILE they started with, so they are given
    // a buffer of their own that writes into the log's FILE
    class FileBuf : public std::streambuf
    {
    public:
        explicit FileBuf(FILE *f) : m_file(f) {}

    protected:
        int overflow(int c) override
        {
            if (c != EOF)
                std::fputc(c, m_file);
            return c == EOF ? 0 : c;
        }
        std::streamsize xsputn(const char *s, std::streamsize n) override { return std::streamsize(std::fwrite(s, 1, size_t(n), m_file)); }
        int sync() override { return std::fflush(m_file); }

    private:
        FILE *m_file;
    };

    // /data/sotc/env.txt: NAME=value lines, the runtime's settings (as the Vita build's ux0:data/celosia/env.txt)
    void LoadEnv()
    {
        std::ifstream f(kData / "env.txt");
        std::string line;
        while (std::getline(f, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                line.pop_back();
            if (line.empty() || line[0] == '#')
                continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0)
                continue;
            setenv(line.substr(0, eq).c_str(), line.substr(eq + 1).c_str(), 1);
            Log("env: %s", line.c_str());
        }
    }
}

int main(int, char **)
{
    scePthreadSetaffinity((unsigned long)pthread_self(), 0x1fff); // the cores a title may use
    for (const char *sub : {"", "/logs", "/memcards", "/shadercache"})
    {
        std::string d = kData.string() + sub;
        mkdir(d.c_str(), 0777);
        chmod(d.c_str(), 0777);
    }
    // Logging is for finding a problem: on only while the file /data/sotc/logging-on exists; off, every line goes
    // into the pipe and is dropped there, so nothing that writes to stdout or stderr ever waits on the disk.
    std::error_code ec;
    const bool logging = fs::exists(kData / "logging-on", ec);
    if (FILE *log = OpenLog(logging ? (kData / "logs/recolossus.log").c_str() : nullptr))
    {
        stderr = log;
        stdout = log;
        static FileBuf streams(log);
        std::cout.rdbuf(&streams);
        std::cerr.rdbuf(&streams);
        std::clog.rdbuf(&streams);
    }
    Log("reColossus %s", RECOLOSSUS_VERSION);
    // where the program was loaded: a crash report's addresses minus these give the offsets in the build's symbols
    Log("addresses: ps2x_main=%p Log=%p", reinterpret_cast<void *>(&ps2x_main), reinterpret_cast<void *>(&Log));

    // the runtime's settings: the memory cards beside the app's data, then the player's own (env.txt)
    setenv("PS2X_MEMCARD_DIR", (kData / "memcards").c_str(), 0);
    // Mesa's shader cache (the Vulkan renderer's GPU code kept between runs), in a folder the app can write: as
    // reKing does (its 0.1.0 crashed at device creation over the cache's mmap)
    setenv("MESA_SHADER_CACHE_DIR", (kData / "shadercache/mesa").c_str(), 0);
    setenv("XDG_CACHE_HOME", (kData / "shadercache").c_str(), 0);
    LoadEnv();
    // The GS rasterizer's worker threads: the runtime takes hardware_concurrency() - 2, and on the console that
    // count isn't the cores a title has (0.1.3 ran at 3 fps). A PS5 title has 13 hardware threads (7 cores); the
    // game's own threads (EE, VU1, IOP, audio) take about 4 of them.
    Log("cpu: hardware_concurrency() = %u", std::thread::hardware_concurrency());
    setenv("PS2X_GS_THREADS", "8", 0);
    Log("cpu: GS raster threads %s", getenv("PS2X_GS_THREADS"));
    if (logging)
    {
        // with logging on: every 5 s, how busy each emulation thread is and where a frame's time goes
        setenv("PS2X_STATS", "1", 0);
        // the VU1 microcode the game runs that this build has no recompiled code for: logged and saved, to be
        // compiled into a later build
        mkdir((kData / "vu1").c_str(), 0777);
        setenv("PS2X_VU1_DUMP_MISSING", (kData / "vu1").c_str(), 0);
        // the native renderer (Vulkan): where each frame's time goes, every 60 frames and for each slow one, and
        // what its textures and meshes hold, every 5 s
        setenv("SOTC_FRAME_LOG", "1", 0);
        setenv("SOTC_GL_MEM_LOG", "1", 0);
        // where the game's own code spends its time (the runtime's sampling profiler: pc and ra, 1000 a second,
        // the file rewritten every 5 s), from 3 s in
        setenv("PS2X_PROFILE", (kData / "logs/guest_profile.txt").c_str(), 0);
        setenv("PS2X_PROFILE_START", "3", 0);
        StartMonitor();
    }
    // The native renderer (Celosia's game overrides draw through it when SOTC_NATIVE is set: Celosia's main sets it
    // on Linux only, which the PS5 is not; 0.2.0 drew with the emulated GS for that). RECOLOSSUS_EMULATED_GS=1
    // (env.txt): the PS2's graphics chip emulated as in 0.1.x instead.
    if (!getenv("RECOLOSSUS_EMULATED_GS"))
        setenv("SOTC_NATIVE", "1", 0);
    Log("renderer: SOTC_NATIVE=%s%s", getenv("SOTC_NATIVE") ? getenv("SOTC_NATIVE") : "(unset)",
        getenv("SOTC_NATIVE") ? " (native, on Vulkan)" : " (the emulated GS)");

    fs::path game;
    if (!FindGame(game))
    {
        if (!UnpackImage(FindImages()))
            NotFound();
        game = GameDir();
    }
    Log("game: %s", game.c_str());
    g_refusedPath = game.string() + "/";
    {
        std::vector<uint8_t> data[4];
        const std::vector<uint8_t> *files[4] = {};
        const char *names[4] = {kBoot, kModules[0], kModules[1], kModules[2]};
        for (int i = 0; i < 4; ++i)
            if (ReadFile(Child(game, names[i]), data[i]))
                files[i] = &data[i];
        const int check = CheckFiles(files);
        if (check == 1)
            OtherGame();
        if (check == 2)
            OtherRelease();
    }
    Log("game: the release this build was made from");
    rc_screen_close(); // the runtime's window opens VideoOut itself
    sceSystemServiceHideSplashScreen();
    // The Vulkan renderer up before the runtime's window: it takes the display (VK_KHR_display, 4K) when it can, and
    // the host then draws only its overlay; else the host shows the picture at 1920x1080 as before.
    if (getenv("SOTC_NATIVE"))
        Log("renderer: Vulkan %s", native_renderer::available() ? "up" : "failed (the emulated GS draws)");

    // relative paths of the runtime (its logs, the VU1 microcode it saves) land in /data/sotc
    chdir(kData.c_str());
    std::string boot = Child(game, kBoot).string();
    char arg0[] = "reColossus";
    char *argv[] = {arg0, boot.data(), nullptr};
    Log("starting the runtime: %s", boot.c_str());
    const int rc = ps2x_main(2, argv);
    Log("the runtime returned %d", rc);
    std::fflush(stderr);
    return rc;
}
