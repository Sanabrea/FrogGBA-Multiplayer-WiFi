# 🐸 FrogGBA + Multiplayer WiFi for Pokémon

### Trade and battle between two PSPs, wirelessly. No link cable, router or internet needed.

![Platform](https://img.shields.io/badge/platform-PSP%20(CFW)-0070d1)
![Link cable](https://img.shields.io/badge/link%20cable-working-2ea44f)
![Wireless Adapter](https://img.shields.io/badge/Wireless%20Adapter-beta-orange)
![License](https://img.shields.io/badge/license-GPLv2-blue)

This is a fork of [FrogGBA](https://github.com/tzubertowski/FrogGBA), the fast Game Boy Advance emulator for PSP by Prosty. It adds **multiplayer between two real PSPs over ad-hoc WiFi**. Two consoles in the same room behave as if they were connected with a GBA link cable, or as if each one had a GBA Wireless Adapter plugged in.

The main target is **Pokémon Generation 3**. You can trade and battle with a friend just like in 2004, but on two PSPs.

| What you can do | Games | Status on real PSPs |
|---|---|---|
| Cable Club **trades** and **battles** (Pokémon Center 2F) | Ruby, Sapphire, Emerald, FireRed, LeafGreen (and between them) | ✅ Tested: FireRed ↔ FireRed, English and Spanish ROMs |
| **Union Room** chat, trades and battles (Wireless Adapter) | FireRed, LeafGreen, Emerald | 🧪 Beta. Tested on FireRed: trade, chat, battle |
| Multiplayer over the link cable | Advance Wars 1 and 2 | ⚙️ Included, not tested on hardware yet |

> **New to this?** Read the step-by-step guide: **[docs/MULTIPLAYER_GUIDE.md](docs/MULTIPLAYER_GUIDE.md)**

## Download

**→ [Download the multiplayer build from the Releases page](https://github.com/Sanabrea/FrogGBA-Multiplayer-WiFi/releases) ←**

- **New install:** unzip and copy the `PSP` folder to the root of the Memory Stick of **each** PSP. As with the original FrogGBA, put your `gba_bios.bin` in `PSP/GAME/FrogGBA/`. A PSP with custom firmware (CFW) is required.
- **Already using FrogGBA:** just replace `PSP/GAME/FrogGBA/EBOOT.PBP`. Your saves, states and settings are kept.

## Quick start (Pokémon)

1. Install this build of FrogGBA on **both** PSPs. Both need the **same version**.
2. Turn the **WLAN switch on** on both PSPs and load your Pokémon game. Save in-game.
3. On both PSPs, open the FrogGBA menu and choose **Link cable (ad-hoc)**. Leave **Mode: Auto**.
4. On one PSP choose **Connect as HOST**, on the other **Connect as GUEST**. When you see **Connected!** the menu closes by itself.
5. In both games, go up to the **2nd floor of the Pokémon Center** and talk to the **Cable Club** attendant. Trade or battle!

For the Union Room (wireless), choose **Mode: Wireless (beta)** on both PSPs and connect **before** going up to the 2nd floor. See the [guide](docs/MULTIPLAYER_GUIDE.md#wireless-adapter-union-room-beta).

## Documentation

| Document | What's inside |
|---|---|
| [Multiplayer guide](docs/MULTIPLAYER_GUIDE.md) | Requirements, installation, step-by-step connection, every mode, menu reference, troubleshooting, FAQ |
| [How it works](docs/HOW_IT_WORKS.md) | Architecture, the ad-hoc transport, the reliable channel, cable and Wireless Adapter emulation, flow control, saving |
| [Debug log](docs/DEBUG_LOG.md) | How to record `link_log.txt` and how to read it |
| [Building and testing](docs/BUILDING_AND_TESTING.md) | Compiling for PSP, and the PC simulators used to test the link without hardware |
| [Multiplayer changelog](docs/CHANGELOG_MULTIPLAYER.md) | What changed in each test build and why |

## Highlights

- **Wireless link between 2 PSPs** using the PSP's own ad-hoc WiFi. Nothing else is required.
- **Pokémon cable emulation that tolerates WiFi lag.** The protocol is emulated at a higher level instead of sending each raw transfer (a real cable needs microsecond timing that WiFi can't give).
- **GBA Wireless Adapter emulation** (beta) for the Union Room, with flow control tuned to what the games can actually digest.
- **Reliable, ordered transport** on top of ad-hoc datagrams: selective acknowledgements, adaptive retransmission timeout and packing several messages per packet.
- **Background saving while linked.** Writing the save to the Memory Stick no longer freezes the console in the middle of a link session.
- **Built-in diagnostics.** An optional `link_log.txt` records every connection step, protocol event and a status line per second.

## Credits

- **FrogGBA** by Prosty ([tzubertowski](https://github.com/tzubertowski)), based on TempGBA by Nebuleon, Normmatt and BassAceGold, TempGBA4PSP-mod, and gameplaySP kai by Exophase and takka.
- **Link cable and Wireless Adapter protocol emulation** ported from [gpSP for libretro](https://github.com/libretro/gpsp) by David Guillen Fandos (`serial.c`, `serial_proto.c`, `rfu.c`).
- **Multiplayer WiFi port for PSP** (ad-hoc transport, reliable channel, flow control, background saving, diagnostics, test tools): this fork.
- Protocol research helped by the [pret](https://github.com/pret) decompilations of Pokémon FireRed and Emerald.

Licensed under the GNU GPL v2 or later, like the code it is based on (see [LICENSE](LICENSE)).

---

# Original FrogGBA README

> The text below is the original FrogGBA README, kept for reference. Its download link points to the original emulator, **without** multiplayer. For the multiplayer build, use [this fork's Releases](https://github.com/Sanabrea/FrogGBA-Multiplayer-WiFi/releases).

## FrogGBA - TempGBA mod by Prosty

## Download & Installation
![FrogGBA Icon](source/res/ICON0.png)

**→ [Download the latest release here](https://github.com/tzubertowski/FrogGBA/releases) ←**

You need to have a CFW installed on your PSP. 
- Unzip the folder
- Copy the unzipped PSP folder into root of your PSP SD card
- Launch the emulator on the console

### TL;DR - Key Improvements in v0.2.2
• 🚀 **gpSP Kai Optimizations** - 10-25% performance gains in demanding titles
• 💯 **Full Speed Castlevania** - Water sections now run at 30+ FPS (was 15-20)
• 🎮 **Recent Games Menu** - Quick access to your last 10 played titles
• 🖼️ **Lightning-Fast Overlays** - Full-screen borders with zero performance impact
• ⚡ **No More Stutters** - Smart cache management eliminates micro-stutters
• 🎨 **Color Correction** - GPSP/Retro modes with optimized lookup tables
• 📐 **Aspect Ratio Options** - Core/Zoom/Stretch modes
• 💾 **Rock-Solid Stability** - Fixed exit crashes and save/load issues

## About
This is a modified version of TempGBA - a Game Boy Advance emulator for PlayStation Portable.

This mod is based on:
- [TempGBA](https://github.com/Nebuleon/TempGBA) by Nebuleon, Normmatt, and BassAceGold
- TempGBA4PSP-mod (TempGBA4PSP-26731020), http://www1.axfc.net/uploader/so/3063963

## What's New in FrogGBA (Since v1.0)

### 🖼️ Overlay System (Enhanced in v0.2.2)
- **Lightning-Fast Loading**: Completely reworked overlay system loads instantly with zero performance impact
- **Full-Screen Borders**: Overlays can now stretch across the entire PSP display
- **Custom Overlays/Borders**: Render PNG overlays above the game window for bezels, borders, and custom frames
- **X/Y Offset Controls**: Position the game window anywhere on screen
- **Online Overlay Generator**: Convert PNG images to OVL format at [froggba.onrender.com](https://froggba.onrender.com)
- **Multiple Overlay Slots**: Store up to 10 different overlays and switch between them
- **RLE Compression**: Advanced compression handles pixel-dense designs without slowdown

### 📐 Display Options
- **Aspect Ratio Modes**:
  - **Core Provided (3:2)**: Original GBA aspect ratio with proper scaling
  - **Zoom (Fill Screen)**: Fills screen vertically, crops sides slightly
  - **Stretch (Full PSP)**: Stretches to fill entire 480×272 screen
- **Color Correction**: 
  - GPSP mode with optimized lookup tables
  - Retro mode for authentic GBA colors
  - Off for raw output
- **FPS Display**: SELECT + Square to toggle performance counter

### ⚡ Performance Optimizations
- **gpSP Kai Engine**: Implemented proven optimizations from the legendary gpSP Kai
  - Static translation caches for reduced memory fragmentation
  - Cache invalidation reduction for smoother frame pacing  
  - Optimized tile rendering pipeline matching gpSP Kai approach
- **Multi-Layer Rendering**: Fixed 4-layer Mode 0 bottlenecks
  - **Castlevania Series**: Water sections now run at full 30+ FPS (was 15-20 FPS)
  - **Golden Sun Series**: Battle effects and world map rendering improved
  - **Fire Emblem**: Large battle animations no longer cause slowdown
- **Memory Management**: Optimized for PSP architecture
  - 2MB ROM cache, 256KB RAM cache (reduced from previous oversized caches)
  - Pre-warmed cache lines reduce initial load stutters
- **Audio Processing**: Improved buffering reduces crackling and provides smoother sound
- **Volatile Memory Support**: Uses additional PSP RAM for maximum performance
  - Works on all PSP models (1000/2000/3000/Go)

### 💾 Save State Improvements
- **Auto Menu Exit**: Menu automatically closes after loading/saving states
- **Better Memory Management**: Temporarily frees overlay memory during save/load operations
- **File Validation**: Prevents crashes from corrupted or empty save states
- **Persistent Settings**: All new options are saved in config

### 🎮 Quality of Life  
- **Recent Games Menu**: Your last 10 played titles appear at the top when loading a game
- **Fast Forward**: SELECT + R toggles 2x/3x speed modes
- **Turbo Buttons**: Triangle and Square as dedicated turbo buttons
- **ROM Validation**: Prevents crashes when loading invalid files
- **Clean Exit**: Fixed BSOD crashes when exiting the emulator
- **Smooth Transitions**: Eliminated micro-stutters during level loading

### 🔧 Technical Improvements
- **PSP Cycle Batching**: Reduces dynamic recompiler overhead
- **Cache Invalidation Reduction**: Conservative cache management
- **Timer Prescaling Optimization**: Better audio/video sync
- **Smart Memory Allocation**: Dynamic allocation reduces static memory usage by ~550KB

### 📈 Performance Impact (v0.2.2)
- **Castlevania Series**: Water sections 15-20 FPS → 30+ FPS (100% speed achieved!)
- **Golden Sun Series**: Battle effects now run without slowdown
- **Fire Emblem**: Large animations no longer cause frame drops
- **Kirby Amazing Mirror**: Eliminated level transition stutters
- **Overall**: 10-25% improvement in demanding titles, consistent 60 FPS in most games

## 🎨 How to Use Custom Overlays

### Creating Overlays
1. **Visit the Online Generator**: Go to [froggba.onrender.com](https://froggba.onrender.com)
2. **Upload Your PNG**: 
   - Image must be exactly 480×272 pixels (PSP screen size)
   - Transparent areas will show the game
   - Opaque areas will be the border/bezel
3. **Download OVL File**: Click convert and download the generated .ovl file
4. **Install on PSP**:
   - Copy the .ovl file to `/PSP/GAME/FrogGBA/overlays/` on your Memory Stick
   - Files will appear in the Overlay menu in-game

### Using Overlays in FrogGBA
1. Press **HOME** during gameplay to open the menu
2. Navigate to **Overlay Settings**
3. Select your overlay from the list
4. Adjust X/Y offset if needed to position the game window
5. Enable/disable overlays with the toggle option

### Tips for Best Results
- Design overlays with a transparent center area for the game (240×160 minimum)
- Use PNG transparency for see-through areas
- Keep border designs simple to avoid hitting the 30,000 pixel limit
- Test different aspect ratios with your overlays (Core/Zoom/Stretch)

## Original TempGBA Features

- Added gpsp kai's cheats function
- Added Chinese language support  
- Added restore function
- New menu icon
- Imported code from TempGBA-mod-dstwo-26750220
- Modern PSP SDK compatibility
- Docker-based build system
