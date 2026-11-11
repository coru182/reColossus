reColossus 0.1.5 - Shadow of the Colossus (PS2, USA) on a PS5 that runs homebrew
================================================================================

The PS2 game's code, translated into C++ ahead of time (Celosia's static recompilation with PS2Recomp) and compiled
for the PS5. It is not an emulator of the PS2's processor; the PS2's other hardware (graphics synthesizer, sound,
IOP) is reimplemented by PS2Recomp's runtime. Bring your own game: the app contains none of the game's files.

This is an early build (0.1.x): the picture is drawn by the PS2 graphics chip's software reimplementation on the
CPU, at the PS2's own resolution, scaled to 1080p. Expect it to run slower than on a PS2 in heavy scenes.

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
PS2X_GS_THREADS=n        threads that draw the PS2's graphics (default: the runtime's choice)
RECOLOSSUS_DRAW_THREADS=n  threads that scale the picture to the screen (0 to 7, default 3)
PS2X_NO_SPU2_AUDIO=1     no sound
Any other setting of Celosia's runtime (its README) works the same way.

REPORTING A PROBLEM
Create an empty file /data/sotc/logging-on, start the game, and attach /data/sotc/logs/recolossus.log to an issue on
https://github.com/coru182/reColossus . Never attach game files or disc images.

An unofficial fan project, not affiliated with or endorsed by Sony Interactive Entertainment or Team Ico.
Licences: licenses/NOTICE.txt.
