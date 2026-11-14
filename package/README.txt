reColossus 0.3.1 - Shadow of the Colossus (PS2, USA) on a PS5 that runs homebrew
================================================================================

The PS2 game's code, translated into C++ ahead of time (Celosia's static recompilation with PS2Recomp) and compiled
for the PS5. It is not an emulator of the PS2's processor; the PS2's other hardware (graphics synthesizer, sound,
IOP) is reimplemented by PS2Recomp's runtime. Bring your own game: the app contains none of the game's files.

The game's models are drawn by the PS5's GPU (Celosia's native renderer, here on Vulkan): the PS2's vector unit and
graphics chip are not emulated for them. Since 0.3.0 the renderer puts its picture on the screen itself, in 4K
(3840 x 2160 in the game's 16:9 mode, 2880 x 2160 with bars at the sides in its 4:3 mode).
Something drawn wrong? RECOLOSSUS_EMULATED_GS=1 in env.txt goes back to the emulated graphics chip of 0.1.x (slow,
but as the PS2 drew it).

THE PICTURE
- 16:9 without bars: in the game, Options > Screen Aspect Ratio > 16:9. The game widens its own view and reColossus
  fills the screen with it (it follows the setting after a moment).
- 4:3 (the game's default): bars at the sides, nothing distorted. The settings menu (touchpad twice), Screen:
  Stretch, fills the screen with it anyway (everything wider).
- RECOLOSSUS_ASPECT=4:3 or 16:9 in env.txt shows the picture at that aspect whatever the game's setting.
Too bright? 0.3.0 halves the bloom (the glow around the sky and light). The settings menu's Bloom strength sets it
(0 none, 1 the PS2's full strength), and RECOLOSSUS_GAMMA=1.2 in env.txt darkens the mid tones.

INSTALL
1. Copy the PPSA99071 folder to /data/homebrew/ on the console (ShadowMountPlus registers it: the reColossus
   tile appears on the home screen after about 30 seconds).
2. Copy your own disc image (.iso) of Shadow of the Colossus, USA release (SCUS-97472), to /data/sotc/
   (create the folder), or to a sotc folder (or the top) of a USB drive.
3. Start the tile. The first start unpacks the disc image into /data/sotc/game/ (the screen says how much room it needs), then the game
   starts. From then on it starts straight away, and you can delete the .iso.
   Already have the disc's files (SCUS_974.72, KERNEL.XFF, ...)? Copy them to /data/sotc/game/ instead.

CONTROLS
The DualSense works as a PS2 controller: Cross Circle Square Triangle, L1 R1 L2 R2, L3 R3, the sticks, the d-pad.
OPTIONS is Start, the touchpad click is Select. Press the touchpad twice quickly for the settings menu.

YOUR FILES
/data/homebrew/PPSA99071/   the app
/data/sotc/game/            the game's files, unpacked from your disc image at the first start
/data/sotc/memcards/        saves (PCSX2-format memory cards: Mcd001.ps2)
/data/sotc/env.txt          optional: the runtime's settings, one NAME=value per line (see below)
/data/sotc/logs/            logs, only while the file /data/sotc/logging-on exists

SETTINGS (env.txt)
RECOLOSSUS_EMULATED_GS=1 the PS2's graphics chip emulated instead of the native renderer (slow)
RECOLOSSUS_ASPECT=x      4:3 or 16:9: the picture shown at that aspect (default: the game's own setting)
RECOLOSSUS_OUTPUT=n      the screen mode: 2160 (4K, default), 1440 or 1080
RECOLOSSUS_RENDER_SCALE=n  the picture drawn at n times the screen's pixels (0.25 to 1, default 1); lower is lighter
                         on the GPU
RECOLOSSUS_GAMMA=n       the picture's mid tones: 1 as drawn (default), 1.2 darker, 0.9 lighter
SOTC_BLOOM_SCALE=n       the bloom: 0 none, 0.5 (default), 1 the PS2's full strength
RECOLOSSUS_PRESENT=host  the picture shown as in 0.2 (read back from the GPU, 1080p); then SOTC_RENDER_SCALE=n draws
                         it at 512 x 448 times n
PS2X_GS_THREADS=n        threads that draw the PS2's graphics when it is emulated (default 8)
RECOLOSSUS_DRAW_THREADS=n  threads that scale the picture to the screen (0 to 7, default 3)
PS2X_NO_SPU2_AUDIO=1     no sound
Any other setting of Celosia's runtime (its README) works the same way.

REPORTING A PROBLEM
Create an empty file /data/sotc/logging-on, start the game, and attach /data/sotc/logs/recolossus.log to an issue on
https://github.com/coru182/reColossus . Never attach game files or disc images.

An unofficial fan project, not affiliated with or endorsed by Sony Interactive Entertainment or Team Ico.
Licences: licenses/NOTICE.txt.
