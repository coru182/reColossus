#pragma once

// reColossus: the subset of the raylib 5.5 API the PS2Recomp runtime uses, implemented on the PS5's own system
// libraries (raylib_ps5.cpp): the picture is drawn by the CPU into VideoOut's scan-out buffers, the DualSense is read
// through libScePad and the sound goes out through libSceAudioOut. Based on Celosia's Vita shim header
// (PS2Recomp/ps2xRuntime/platform/vita/raylib/raylib.h, GPL-3.0).

#include <cstdint>

#define PS2X_PS5_SHIM 1

typedef struct Vector2
{
    float x, y;
} Vector2;

typedef struct Rectangle
{
    float x, y, width, height;
} Rectangle;

typedef struct Color
{
    unsigned char r, g, b, a;
} Color;

typedef struct Image
{
    void *data;
    int width;
    int height;
    int mipmaps;
    int format;
} Image;

typedef struct Texture
{
    unsigned int id;
    int width;
    int height;
    int mipmaps;
    int format;
} Texture;
typedef Texture Texture2D;

typedef struct AudioStream
{
    void *buffer;
    void *processor;
    unsigned int sampleRate;
    unsigned int sampleSize;
    unsigned int channels;
} AudioStream;

typedef struct Wave
{
    unsigned int frameCount;
    unsigned int sampleRate;
    unsigned int sampleSize;
    unsigned int channels;
    void *data;
} Wave;

typedef struct Sound
{
    AudioStream stream;
    unsigned int frameCount;
} Sound;

typedef void (*AudioCallback)(void *bufferData, unsigned int frames);

#define CLITERAL(type) type
#define WHITE CLITERAL(Color){255, 255, 255, 255}
#define BLACK CLITERAL(Color){0, 0, 0, 255}
#define BLANK CLITERAL(Color){0, 0, 0, 0}
#define MAGENTA CLITERAL(Color){255, 0, 255, 255}
#define GREEN CLITERAL(Color){0, 228, 48, 255}
#define RED CLITERAL(Color){230, 41, 55, 255}
#define GRAY CLITERAL(Color){130, 130, 130, 255}
#define DARKGRAY CLITERAL(Color){80, 80, 80, 255}
#define YELLOW CLITERAL(Color){253, 249, 0, 255}
#define SKYBLUE CLITERAL(Color){102, 191, 255, 255}
#define RAYWHITE CLITERAL(Color){245, 245, 245, 255}

enum
{
    FLAG_VSYNC_HINT = 0x00000040,
    FLAG_WINDOW_RESIZABLE = 0x00000004,
    FLAG_WINDOW_HIDDEN = 0x00000080,
    FLAG_WINDOW_HIGHDPI = 0x00002000,
    FLAG_MSAA_4X_HINT = 0x00000020,
};

enum
{
    PIXELFORMAT_UNCOMPRESSED_R8G8B8A8 = 7,
};

enum
{
    TEXTURE_FILTER_POINT = 0,
    TEXTURE_FILTER_BILINEAR = 1,
};

enum
{
    LOG_ALL = 0,
    LOG_TRACE,
    LOG_DEBUG,
    LOG_INFO,
    LOG_WARNING,
    LOG_ERROR,
    LOG_FATAL,
    LOG_NONE,
};

// Keyboard: none on the console; IsKeyDown() is always false.
enum KeyboardKey
{
    KEY_NULL = 0,
    KEY_ONE = 49,
    KEY_THREE = 51,
    KEY_A = 65,
    KEY_C = 67,
    KEY_D = 68,
    KEY_E = 69,
    KEY_H = 72,
    KEY_I = 73,
    KEY_J = 74,
    KEY_K = 75,
    KEY_L = 76,
    KEY_O = 79,
    KEY_Q = 81,
    KEY_S = 83,
    KEY_U = 85,
    KEY_V = 86,
    KEY_W = 87,
    KEY_X = 88,
    KEY_Y = 89,
    KEY_Z = 90,
    KEY_ENTER = 257,
    KEY_BACKSPACE = 259,
    KEY_RIGHT = 262,
    KEY_LEFT = 263,
    KEY_DOWN = 264,
    KEY_UP = 265,
    KEY_F1 = 290,
    KEY_F8 = 297,
    KEY_F9 = 298,
    KEY_F10 = 299,
    KEY_F11 = 300,
    KEY_F12 = 301,
    KEY_LEFT_CONTROL = 341,
    KEY_RIGHT_SHIFT = 344,
    KEY_RIGHT_CONTROL = 345,
};

enum GamepadButton
{
    GAMEPAD_BUTTON_UNKNOWN = 0,
    GAMEPAD_BUTTON_LEFT_FACE_UP,
    GAMEPAD_BUTTON_LEFT_FACE_RIGHT,
    GAMEPAD_BUTTON_LEFT_FACE_DOWN,
    GAMEPAD_BUTTON_LEFT_FACE_LEFT,
    GAMEPAD_BUTTON_RIGHT_FACE_UP,
    GAMEPAD_BUTTON_RIGHT_FACE_RIGHT,
    GAMEPAD_BUTTON_RIGHT_FACE_DOWN,
    GAMEPAD_BUTTON_RIGHT_FACE_LEFT,
    GAMEPAD_BUTTON_LEFT_TRIGGER_1,
    GAMEPAD_BUTTON_LEFT_TRIGGER_2,
    GAMEPAD_BUTTON_RIGHT_TRIGGER_1,
    GAMEPAD_BUTTON_RIGHT_TRIGGER_2,
    GAMEPAD_BUTTON_MIDDLE_LEFT,
    GAMEPAD_BUTTON_MIDDLE,
    GAMEPAD_BUTTON_MIDDLE_RIGHT,
    GAMEPAD_BUTTON_LEFT_THUMB,
    GAMEPAD_BUTTON_RIGHT_THUMB,
};

enum GamepadAxis
{
    GAMEPAD_AXIS_LEFT_X = 0,
    GAMEPAD_AXIS_LEFT_Y = 1,
    GAMEPAD_AXIS_RIGHT_X = 2,
    GAMEPAD_AXIS_RIGHT_Y = 3,
    GAMEPAD_AXIS_LEFT_TRIGGER = 4,
    GAMEPAD_AXIS_RIGHT_TRIGGER = 5,
};

// Window and drawing
void InitWindow(int width, int height, const char *title);
void CloseWindow(void);
bool WindowShouldClose(void);
bool IsWindowReady(void);
void SetConfigFlags(unsigned int flags);
void SetTraceLogLevel(int logLevel);
void SetTargetFPS(int fps);
int GetFPS(void);
double GetTime(void);
float GetFrameTime(void);
int GetScreenWidth(void);
int GetScreenHeight(void);
int GetRenderWidth(void);
int GetRenderHeight(void);
void SetWindowTitle(const char *title);
void BeginDrawing(void);
void PollInputEvents(void);
void EndDrawing(void);
void ClearBackground(Color color);
void DrawTexture(Texture2D texture, int posX, int posY, Color tint);
void DrawTexturePro(Texture2D texture, Rectangle source, Rectangle dest, Vector2 origin, float rotation, Color tint);
void DrawRectangle(int posX, int posY, int width, int height, Color color);
void DrawRectangleRec(Rectangle rec, Color color);
void DrawRectangleLines(int posX, int posY, int width, int height, Color color);
void DrawText(const char *text, int posX, int posY, int fontSize, Color color);
void DrawFPS(int posX, int posY);
int MeasureText(const char *text, int fontSize);

// Images and textures (RGBA8 only)
Image GenImageColor(int width, int height, Color color);
void ImageCrop(Image *image, Rectangle crop);
void UnloadImage(Image image);
bool ExportImage(Image image, const char *fileName);
Texture2D LoadTextureFromImage(Image image);
Image LoadImageFromTexture(Texture2D texture);
void UpdateTexture(Texture2D texture, const void *pixels);
void UpdateTextureRec(Texture2D texture, Rectangle rec, const void *pixels);
void UnloadTexture(Texture2D texture);
bool IsTextureValid(Texture2D texture);
void SetTextureFilter(Texture2D texture, int filter);

// Input
bool IsKeyDown(int key);
bool IsKeyPressed(int key);
bool IsGamepadAvailable(int gamepad);
bool IsGamepadButtonDown(int gamepad, int button);
bool IsGamepadButtonPressed(int gamepad, int button);
float GetGamepadAxisMovement(int gamepad, int axis);
bool SetGamepadVibration(int gamepad, float leftMotor, float rightMotor, float duration);

// Audio (streams with a callback)
void InitAudioDevice(void);
void CloseAudioDevice(void);
bool IsAudioDeviceReady(void);
void SetMasterVolume(float volume);
void SetAudioStreamBufferSizeDefault(int size);
AudioStream LoadAudioStream(unsigned int sampleRate, unsigned int sampleSize, unsigned int channels);
void UnloadAudioStream(AudioStream stream);
bool IsAudioStreamValid(AudioStream stream);
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback);
void PlayAudioStream(AudioStream stream);
void StopAudioStream(AudioStream stream);
void PauseAudioStream(AudioStream stream);
void ResumeAudioStream(AudioStream stream);
bool IsAudioStreamPlaying(AudioStream stream);
void SetAudioStreamVolume(AudioStream stream, float volume);
