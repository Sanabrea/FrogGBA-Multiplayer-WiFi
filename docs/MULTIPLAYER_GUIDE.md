# Multiplayer guide: FrogGBA + Multiplayer WiFi

This guide explains, step by step, how to link two PSPs running this build of FrogGBA so their GBA games can talk to each other: Pokémon trades and battles through the **Cable Club**, the **Union Room** (beta), and other link-cable games.

- [1. What you need](#1-what-you-need)
- [2. Installation](#2-installation)
- [3. The Link menu](#3-the-link-menu)
- [4. Connecting the two PSPs](#4-connecting-the-two-psps)
- [5. Pokémon: Cable Club trades and battles](#5-pokémon-cable-club-trades-and-battles)
- [6. Wireless Adapter: Union Room (beta)](#wireless-adapter-union-room-beta)
- [7. Link modes](#7-link-modes)
- [8. During a session: do's and don'ts](#8-during-a-session-dos-and-donts)
- [9. Messages you may see](#9-messages-you-may-see)
- [10. Troubleshooting](#10-troubleshooting)
- [11. FAQ](#11-faq)
- [12. Reporting a problem](#12-reporting-a-problem)

---

## 1. What you need

- **Two PSPs with custom firmware (CFW)** and working WiFi. Any model with WiFi works: PSP-1000, 2000, 3000 and Go. The PSP Street (E1000) has no WiFi, so it can't be used.
- **This build of FrogGBA on both PSPs, and the same version on both.** Builds with a different link protocol version ignore each other, so they never find each other.
- **A GBA game on each PSP.** For Pokémon, any two Generation 3 games can link (see [Modes](#7-link-modes)).
- **The PSPs close to each other**, in the same room. Ad-hoc WiFi is direct console-to-console: no router, access point or internet is used.

## 2. Installation

**New install**

1. Download the multiplayer build from the [Releases page](https://github.com/Sanabrea/FrogGBA-Multiplayer-WiFi/releases).
2. Unzip it and copy the `PSP` folder to the root of the Memory Stick of **each** PSP.
3. As with the original FrogGBA, put your `gba_bios.bin` in `PSP/GAME/FrogGBA/`.
4. Put your games in `PSP/GAME/FrogGBA/roms/` (or wherever you usually keep them).

**Updating an existing FrogGBA**

Replace `PSP/GAME/FrogGBA/EBOOT.PBP` on both PSPs. Saves (`save/`), save states (`state/`) and settings (`cfg/`) are kept.

> Before linking for the first time, back up your `.sav` files. Linking is safe, but this is a new feature.

## 3. The Link menu

Open the FrogGBA menu during a game (the **HOME** button, or your menu button) and choose **Link cable (ad-hoc)**:

```
Mode: Auto
Debug log: Off
Connect as HOST
Connect as GUEST
Disconnect

One PSP: HOST
The other: GUEST
WLAN switch ON

Pokemon: Auto
= Cable (Pokemon)

Status:
Connected as HOST - Cable (Pokemon)
Peer: POKEMON FI [BPRE]  resent: 3
```

| Option | What it does |
|---|---|
| **Mode** | Which link hardware to emulate: *Auto*, *Wireless (beta)*, *Cable (Pokemon)*, *Cable (Adv. Wars 1)*, *Cable (Adv. Wars 2)*. See [Link modes](#7-link-modes). Leave it on **Auto** unless you want the Union Room. |
| **Debug log** | *On (link_log.txt)* records the session in `PSP/GAME/FrogGBA/link_log.txt`. Only needed to report problems. See [DEBUG_LOG.md](DEBUG_LOG.md). |
| **Connect as HOST** | This PSP opens the link session and waits for the other one. |
| **Connect as GUEST** | This PSP looks for a HOST and joins it. |
| **Disconnect** | Closes the session on both PSPs and closes the debug log. |
| **Status** | First line: connection state and mode. Second line: the other PSP's game (title and code) and how many messages had to be re-sent over WiFi (a few is normal). |

## 4. Connecting the two PSPs

Do this with the game already loaded on both PSPs.

1. **Save in-game** on both consoles. It's a good habit before any link.
2. **Turn the WLAN switch on** on both PSPs.
3. On both PSPs, open the menu → **Link cable (ad-hoc)**. Check that **Mode** matches what you want to do. **Auto** gives the cable mode for Pokémon.
4. On **one** PSP choose **Connect as HOST**. You'll see:
   - *Starting the network...*
   - *Joining the ad-hoc network...*
   - *Waiting for the other PSP (GUEST)...*
5. On the **other** PSP choose **Connect as GUEST**. You'll see:
   - *Looking for the other PSP (HOST)...*
   - *Joining the HOST...*
6. Both PSPs show **Connected! Mode: ...** with the other console's game title. Press any button. The menu closes and you're back in the game.
7. Now use the link feature **inside the game**, exactly as if a cable (or wireless adapter) had just been plugged in.

Notes:

- It doesn't matter which PSP is HOST. The HOST only starts the session and decides the link mode.
- You can cancel the waiting screens with **O** or **□**.
- The HOST waits up to 90 seconds for a GUEST. Joining the ad-hoc network gives up after 20 seconds.
- To end the session, choose **Disconnect** on either PSP. Exiting FrogGBA also closes it, and the other PSP is told.

## 5. Pokémon: Cable Club trades and battles

This is the **Cable (Pokemon)** mode, which *Auto* picks for every Gen3 Pokémon game. It has been tested on real PSPs with FireRed (English and Spanish), with trades and battles.

### Step by step

1. Connect the PSPs as in [section 4](#4-connecting-the-two-psps), with **Mode: Auto** (or *Cable (Pokemon)*).
2. In both games, walk into a **Pokémon Center** and go **up to the 2nd floor**.
   - If you were already on the 2nd floor when you connected, go down the stairs and come back up. The game checks the link when you enter the floor.
3. Talk to the **Cable Club** attendant on both consoles.
4. Choose **Trade Center** or **Colosseum** (single or double battle) on both. The game saves before you enter. This is normal.
5. Walk to your seats. Trade or battle as usual.
6. To leave, walk out of the room on both consoles. The game saves again.

### Which games can link

| Game | Cable trades and battles |
|---|---|
| FireRed / LeafGreen | ✅ Tested: FireRed ↔ FireRed |
| Ruby / Sapphire / Emerald | ✅ Same protocol, should work (not tested on PSP yet) |
| Mixing generations (e.g. FireRed ↔ Emerald) | ✅ Supported by the emulation. The games' own rules still apply: for example, FireRed/LeafGreen need some post-game progress before they can link with Ruby/Sapphire/Emerald. |
| Pokémon ROM hacks | Usually yes, if the hack keeps the original link code. *Auto* uses the cable mode for any game whose title starts with "POKEMON". |

**Limits:** only **two** players. Four-player activities (multi battles, four-player Berry Crush, and so on) aren't possible. Other two-player cable activities, like the Record Corner or a two-player Berry Blender, use the same link and may work, but haven't been tested.

<a id="wireless-adapter-union-room-beta"></a>
## 6. Wireless Adapter: Union Room (beta)

FireRed, LeafGreen and Emerald also support the **GBA Wireless Adapter**, with the **Union Room** (chat, trades and battles with whoever is in the room) and direct wireless trades and battles. This build emulates the adapter on top of the PSP WiFi.

Status: **beta**. Tested on FireRed (Spanish ROM) on real PSPs: wireless trades, Union Room chat, and a wireless battle.

### Step by step

1. On **both** PSPs open **Link cable (ad-hoc)** and set **Mode: Wireless (beta)**. *Auto* won't pick it for Pokémon, because the cable mode is more robust.
2. **Connect HOST and GUEST before you enter the Pokémon Center** (section 4).
   - If you're already inside, go downstairs and back up once the PSPs are connected. The game looks for the adapter when you enter the 2nd floor.
3. On the 2nd floor, talk to the **Wireless Club** attendant on both consoles. This attendant only appears when the game detects the adapter.
4. Pick what you want:
   - **Direct Corner:** trade or battle with one friend.
   - **Union Room:** walk around, chat, and start trades or battles with the other player.
5. Play as usual. When you're done, leave the room on both consoles and let the game save.

**Tip:** wireless is more demanding than the cable. If it misbehaves, use the cable mode (Cable Club) for trades and battles. It does the same thing and is the most tested path.

## 7. Link modes

| Mode | Emulates | Use it for |
|---|---|---|
| **Auto** (default) | Picks the mode from the game | Almost always. Pokémon Gen3 → *Cable (Pokemon)*. Advance Wars → its cable mode. Known Wireless Adapter games → *Wireless (beta)*. |
| **Wireless (beta)** | GBA Wireless Adapter | Pokémon FireRed / LeafGreen / Emerald **Union Room** and wireless trades and battles. Other wireless games are included in *Auto* but untested. |
| **Cable (Pokemon)** | GBA link cable, Pokémon protocol | Cable Club trades and battles in Ruby, Sapphire, Emerald, FireRed and LeafGreen, and between them. |
| **Cable (Adv. Wars 1)** | GBA link cable, Advance Wars protocol | Advance Wars multiplayer (untested on hardware). |
| **Cable (Adv. Wars 2)** | GBA link cable, Advance Wars 2 protocol | Advance Wars 2 multiplayer (untested on hardware). |

**Why only some games?** A real link cable needs timing in microseconds. WiFi has delays of milliseconds, so passing every raw transfer through the air doesn't work. Instead, each mode understands the protocol of a family of games and fakes the other GBA locally, sending only the game data over WiFi. That's why it needs to know which protocol the game speaks. Other cable games (Mario Kart, Four Swords, etc.) use protocols that aren't emulated, so they **won't link**. Details are in [HOW_IT_WORKS.md](HOW_IT_WORKS.md).

**Games that Auto sends to the Wireless Adapter mode** (from gpSP's list, untested): Mario Golf: Advance Tour, Mario Tennis: Power Tour, Mega Man Battle Network 5 and 6, Digimon Racing, Dragon Ball Z: Buu's Fury, Shrek SuperSlam, The Lord of the Rings: The Third Age, Narnia, Hamtaro: Ham-Ham Games, No No No Puzzle Chailien.

### If the two PSPs choose different modes

The HOST decides the mode for the session:

1. If both choose the same mode, that's the mode.
2. A mode chosen **by hand** wins over **Auto**.
3. If both are on Auto but their games disagree (e.g. two different games), the cable mode for Pokémon wins when either side is a Pokémon game.

The *Connected!* screen always shows the final mode.

## 8. During a session: do's and don'ts

**Do**

- Keep the PSPs reasonably close.
- Let the game finish saving after a trade or battle before you exit FrogGBA or turn off the PSP. While linked, FrogGBA writes the save to the Memory Stick **in the background**, so the game keeps running. Give it a few seconds.
- Use **Disconnect** when you're done.

**Don't**

- **Don't open the menu, pause, or put the PSP to sleep** in the middle of a trade or battle. The game on the other PSP keeps waiting, and after a few seconds of silence the link is considered lost.
- **Don't use fast forward** while linked. Both games need to run at normal speed.
- Don't load a save state in the middle of a link activity. The other game won't follow.

**Performance tip:** running the PSP at 333 MHz helps keep both games at full speed.

## 9. Messages you may see

| Message | Meaning |
|---|---|
| *Load a game first* | Load a ROM before connecting. |
| *The WLAN switch is off* | Turn the WiFi switch on and try again. |
| *Starting the network...* / *Joining the ad-hoc network...* | Bringing up the PSP's ad-hoc WiFi. |
| *Ad-hoc network not available* | Couldn't join the ad-hoc network within 20 s. Check the WLAN switch and try again. |
| *Waiting for the other PSP (GUEST)...* | HOST is ready. Choose *Connect as GUEST* on the other PSP. |
| *Looking for the other PSP (HOST)...* | GUEST hasn't found a HOST yet. |
| *The other PSP is also HOST: choose GUEST there* | Both chose HOST. Cancel one and choose GUEST. |
| *Timed out waiting for the guest* | No GUEST joined within 90 s. |
| *Connected! Mode: ...* | Linked. The second line shows the other PSP's game. |
| *Connected as HOST/GUEST - mode* | Status line while linked. |
| *No answer from the other PSP...* | Nothing received for 6 s (out of range, paused, or turned off). It recovers by itself if the other PSP comes back. |
| *The other PSP disconnected* | The other PSP chose Disconnect or exited FrogGBA. |
| *Debug log: link_log.txt* / *Debug log FAILED* | Whether the debug log file could be opened. |

## 10. Troubleshooting

**The GUEST never finds the HOST**
- Both PSPs must run **exactly the same build**. Different versions ignore each other.
- WLAN switch on for both. Bring them closer.
- If both say HOST, one must be GUEST.
- In the PSP system settings (*Settings → Network Settings → Ad Hoc Mode*), set both PSPs to the **same channel** (1, 6 or 11) instead of Automatic.

**Connected, but the game says it can't communicate / nothing happens at the Cable Club**
- Make sure you entered the 2nd floor **after** connecting. If not, go down and back up.
- For the Cable Club, use **Auto** or **Cable (Pokemon)**. For the Union Room / Direct Corner, both PSPs need **Wireless (beta)**.
- Check the *Status* line: both PSPs must show the same mode.

**It's slow, or the link drops in the middle of a trade or battle**
- WiFi interference is the usual cause: home routers and other devices on the same channel. Try another ad-hoc channel (1, 6 or 11), and move away from routers.
- Don't open the menu or use fast forward during link activities.
- A high *resent* count in the status line means WiFi packets are being lost.

**One PSP froze for a few seconds after a battle or trade (older builds)**
That was the save being written to the Memory Stick. Since multiplayer v5 it's done in the background while linked.

**Still stuck?** Record a debug log on **both** PSPs and report it (see [section 12](#12-reporting-a-problem)).

## 11. FAQ

**Does it work over the internet?**
No. It uses the PSP's local ad-hoc WiFi: the two consoles talk directly to each other and must be in the same place.

**More than two PSPs?**
No, the link is for two consoles.

**Does it work in PPSSPP?**
Not tested. PPSSPP has its own ad-hoc emulation, which may or may not behave like real hardware.

**Can I trade between different languages (e.g. Spanish and English FireRed)?**
The link doesn't care about the language. The games' own compatibility rules apply.

**Is my save at risk?**
The games only save at the usual points. The background save writes a complete snapshot of the game's save memory. Still, back up your `.sav` files, and don't turn off the PSP while the game is saving.

**Does it use more battery?**
Yes, WiFi uses extra power while a session is open. Disconnect when you're done.

## 12. Reporting a problem

1. On **both** PSPs set **Debug log: On (link_log.txt)** before connecting.
2. Reproduce the problem.
3. Choose **Disconnect** (or exit FrogGBA) so the log is closed properly.
4. Copy `PSP/GAME/FrogGBA/link_log.txt` from **both** PSPs and attach them to a [GitHub issue](https://github.com/Sanabrea/FrogGBA-Multiplayer-WiFi/issues), saying what you were doing, which games and which build.

How to read the log yourself: [DEBUG_LOG.md](DEBUG_LOG.md).
