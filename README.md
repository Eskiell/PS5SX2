<p align="center">
  <img src="ps5/docs/banner.png" alt="PS5SX2" width="100%">
</p>

<h3 align="center">PCSX2, the PS2 emulator, running natively on a jailbroken PS5.</h3>

<p align="center">
  <img alt="Platform: jailbroken PS5" src="https://img.shields.io/badge/platform-jailbroken%20PS5-8c6bff">
  <img alt="Renderer: Vulkan" src="https://img.shields.io/badge/renderer-Vulkan-6f55d9">
  <img alt="Based on PCSX2" src="https://img.shields.io/badge/based%20on-PCSX2-5a45c8">
  <img alt="Licence: GPL-3.0-or-later" src="https://img.shields.io/badge/licence-GPL--3.0--or--later-3d3a8c">
</p>

---

## What is PS5SX2?

PS5SX2 is a port of [PCSX2](https://github.com/PCSX2/pcsx2) to the PS5. It runs as a native app on a jailbroken console:

- PCSX2's recompilers run straight on the PS5's CPU.
- Its Vulkan renderer draws through a native Vulkan driver for the PS5's GPU.
- You pick a game from a cover-flow shelf and play it upscaled on a 4K TV.
- You change its settings from your phone while you play.

## A passion project

PS5SX2 is a passion project. It started as a "can this even work?" experiment, done in spare time out of love for the PS2's library and the fun of seeing those games on new hardware. There's no company or schedule behind it, and nothing is for sale.

Expect rough edges. Bug reports with logs are very welcome, and so is patience.

## Highlights

- **Native, not streamed.** The emulator, its recompilers and its renderer all run on the console itself.
- **Up to 6x native resolution.** PCSX2's hardware renderer runs on Vulkan with 4K output, and FSR is one of the display filters.
- **A shelf for your games.** A 3D cover flow of your library. Covers download automatically the first time you start it.
- **Settings from your phone.** The shelf shows a QR code: scan it and a settings page opens on your phone.
  - Change the resolution, aspect ratio, filters, patches and more, for all games or one game.
  - Most changes apply while you play.
- **Widescreen and 60 FPS patches.** Put PCSX2 patch files (`.pnach`) in `/data/PCSX2/patches/` and switch them on per game from the settings page.
- **Online play.** PCSX2's emulated network adapter goes out through the PS5's own connection. SOCOM II has played online matches on [PS Rewired](https://psrewired.com)'s revival servers.
- **Recommended settings** for the games played during development, one tap away on the settings page.
- **Logs that survive.** The last sessions' boot, emulator and settings logs stay on the console, so a problem can be tracked down afterwards.

## What you need

- **A jailbroken PS5.** Development happens on a PS5 Pro on firmware 11.40. Other models and firmware haven't been tested.
- **The PS5SX2 Helper payload**, from the releases, loaded together with kstuff. It's based on OnionHEN. It jailbreaks PS5SX2 when it starts, which the emulator's recompilers need, and gives the app access to `/data`.
- **[ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus)**, loaded at every boot like kstuff. It mounts PS5SX2 from `/data/homebrew/PPSA99203/` and puts it on the home screen.
- **Your own PS2 BIOS**, dumped from your own console.
- **Your own games**, as `.iso` files.

No BIOS, games or keys come with PS5SX2.

## Getting started

1. Copy the app from the release to `/data/homebrew/PPSA99203/`. ShadowMountPlus finds it there and adds it to the home screen.
2. Put your BIOS in `/data/PCSX2/bios/` and your games in `/data/PCSX2/games/`.
3. Add `PPSA99203` to `/data/whitelist.txt`, on a line of its own.
4. Load kstuff, ShadowMountPlus and the PS5SX2 Helper, then start PS5SX2 from the home screen.

The first start downloads the covers for your games, then the shelf opens.

## Controls

**On the shelf**

| Button | Does |
|---|---|
| D-pad or left stick | Browse the games |
| L1 / R1 | Jump a page |
| Cross or OPTIONS | Play |

**In a game**

| Combo | Does |
|---|---|
| Hold L1 + R1, click the touchpad | Back to the shelf |
| Hold L3 + R3, then D-pad Up / Down | Save / load state (slot 1) |
| Touch the left / right third of the touchpad + Cross | Save / load state (slot 1) |
| Hold L3 + R3 and let go | Next display filter |

## The settings page

Scan the QR code in the corner of the shelf with a phone on the same network. You can change settings for all games or for one game:

- **Display:** resolution (1x to 6x), aspect ratio, widescreen patches and display filter (FSR, FSR soft, Classic, CRT).
- **Graphics:** texture filtering, anisotropic filtering, blending accuracy and mipmapping.
- **Performance:** EE cycle rate and skip, and MTVU.
- **Patches:** the game's patch groups, such as 60 FPS.

Most changes show up in the running game straight away. The page marks the few that need a restart. **Recommended** puts back the settings tuned for that game.

## Online play

PS5SX2 emulates the PS2's network adapter on top of the PS5's own connection. SOCOM II (SCUS-97275) has played online matches on PS Rewired's servers.

<details>
<summary>How SOCOM II was set up</summary>

1. **The DNAS bypass:** put PS Rewired's DNAS bypass cheat (`0F6FC6CF.pnach`, linked from their [SOCOM II guide](https://psrewired.com/guides/socom2)) in `/data/PCSX2/cheats/`.
2. **The game's settings file:** add these lines to `/data/PCSX2/settings/SOCOM II - U.S. Navy SEALs (USA).ini`:

   ```ini
   DEV9/Eth/EthEnable=true
   DEV9/Eth/EthApi=Sockets
   DEV9/Eth/EthDevice=Auto
   DEV9/Eth/InterceptDHCP=true
   DEV9/Eth/ModeDNS1=Manual
   DEV9/Eth/DNS1=67.222.156.250
   EmuCore/EnableCheats=true
   Cheats/Enable=General Cheats\DNAS Bypass
   EmuCore/EnableWideScreenPatches=false
   ```
3. **In the game:** create a network configuration (automatic settings work), connect, pick a universe, download the update and make your account.

PS Rewired doesn't allow the widescreen patch online. Stretching the picture to 16:9 on the settings page is fine. See [PS Rewired's guides](https://psrewired.com) for other games.

</details>

## Games played during development

These are the settings PS5SX2 recommends for each. This isn't a full compatibility list. Widescreen patches are on by default for every game that has one.

| Game | Recommended |
|---|---|
| **Ratchet & Clank** (PAL)<br><sub>SCES-50916</sub> | 6x, holds 50 fps. PS5SX2 has built-in widescreen for this disc. |
| **Ratchet & Clank 3**<br><sub>SCUS-97353</sub> | 6x |
| **God of War**<br><sub>SCUS-97399</sub> | 6x |
| **Gran Turismo 4**<br><sub>SCUS-97328</sub> | 6x. Also set 16:9 in the game's own options. |
| **Kingdom Hearts Final Mix** (English patch)<br><sub>SLPS-25198</sub> | 6x, Cross/Circle swap patch |
| **The Lord of the Rings: The Fellowship of the Ring**<br><sub>SLUS-20520</sub> | 6x, 60 FPS patch |
| **The Lord of the Rings: The Two Towers**<br><sub>SLUS-20578</sub> | 6x, 60 FPS patch |
| **The Lord of the Rings: The Return of the King**<br><sub>SLUS-20770</sub> | 6x, 60 FPS patch |
| **Castlevania: Lament of Innocence**<br><sub>SLUS-20733</sub> | 6x, MTVU off and EE clock at 100%. The game freezes with MTVU on. |
| **Need for Speed: Most Wanted** (Black Edition)<br><sub>SLUS-21351</sub> | 6x |
| **Oni**<br><sub>SLUS-20064</sub> | 4x, since 6x runs too slowly |
| **SOCOM II: U.S. Navy SEALs**<br><sub>SCUS-97275</sub> | 6x, online (see above) |

## Known limitations

- **Tested on one console,** a PS5 Pro. The 6x presets may be too heavy for a regular PS5.
- **No RetroAchievements** yet.
- **Restart needed for a few settings:** the renderer and MTVU only change when the game restarts.
- **Covers need the PS5SX2 Helper,** which gives the app `/data` before it starts. Without it, nothing downloads.

## Building from source

The PS5 layer lives in [`ps5/`](ps5/). [`ps5/README.md`](ps5/README.md) covers the layout, dependencies and build steps. Everything else in this repository is PCSX2, with the port's changes as commits on top.

## Credits

- **The PCSX2 Dev Team,** for PCSX2, which does all the emulating.
- **Mihawk-99,** for PS5_Vulkan.
- **BlackBearReloaded,** for ps5-native-app-boilerplate.
- **John Törnblom,** for the ps5-payload-dev SDK.
- **[xlenore/ps2-covers](https://github.com/xlenore/ps2-covers),** for the covers the shelf downloads.
- **[PS Rewired](https://psrewired.com),** for keeping PS2 online games alive.
- **OnionHEN,** which the PS5SX2 Helper is based on.
- **drakmor,** for [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus), and **VoidWhisper,** for ShadowMount, which it's based on.
- **Project Nayuki's QR Code generator and Font Awesome Free:** see [`ps5/README.md`](ps5/README.md) for their licences.

## Licence

PS5SX2 is GPL-3.0-or-later, like PCSX2. The licence text is [`COPYING.GPLv3`](COPYING.GPLv3). Third-party parts keep their own licences.

## Disclaimer

PS5SX2 is an independent project. It isn't affiliated with or endorsed by the PCSX2 team or Sony Interactive Entertainment.

"PlayStation", "PS2" and "PS5" are trademarks of Sony Interactive Entertainment. Game names belong to their owners.

Please use your own BIOS and your own games.

---

<p align="center">
  Made by Spyros: Discord <b>sword.pdf</b> · X <a href="https://x.com/sword_pdf">@sword_pdf</a>
</p>
