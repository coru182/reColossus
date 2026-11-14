**Bring your own game:** the app contains none of the game's files. Copy your own disc image (`.iso`) of Shadow of the
Colossus, **USA PS2 release (SCUS-97472)**, to `/data/sotc/` on the console.

1. Unpack `PPSA99071-<version>.zip` and copy the `PPSA99071` folder to `/data/homebrew/` (ShadowMountPlus registers it).
2. Copy your `.iso` to `/data/sotc/`.
3. Start the **reColossus** tile: the first start unpacks the disc image, then the game starts.

Early build: the game's models are drawn by the PS5's GPU (Celosia's native renderer, on Vulkan), which presents in
4K (the game's 4:3 picture; `RECOLOSSUS_WIDESCREEN=1` for an experimental 16:9), with the bloom at half strength. `RECOLOSSUS_EMULATED_GS=1` in `/data/sotc/env.txt` goes back to the emulated GS (slow).
See the README for controls, settings and how to report a problem.
