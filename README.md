<h1 align="center">xboxoneformac</h1>

<p align="center">A <b>wired Xbox One controller</b> that works in Wine games on the Mac — even with Chrome open.</p>

---

Made for **[Warframe for Mac](https://github.com/carlossantos-br/warframeformac)**, but it works with any game running in a Sikarugir/Wineskin wrapper.

## The problem

- macOS has no driver for the **original wired Xbox One controller** (USB `045e:02d1`), so Wine never sees it.
- Browsers such as Chrome grab this controller for themselves. Even when it works, having Chrome open is enough for the controller to vanish from the game.
- When you play only with the controller, the Mac thinks you are away and turns off the display mid-match.

## What this project does

- A small background service. **Only while the game is running**, it takes the controller over USB (even from Chrome) and hands it to Wine as a regular Xbox controller (XInput), with **rumble**.
- While you play with the controller, **the display stays on** and moving the controller counts as activity (like moving the mouse). After 15 minutes without touching it, the Mac goes back to sleeping normally.
- When the game closes, the controller goes back to macOS and other apps.

## Install

First install [Warframe for Mac](https://github.com/carlossantos-br/warframeformac). Then, in **Terminal**:

```bash
git clone https://github.com/carlossantos-br/xboxoneformac.git
cd xboxoneformac
./install.sh
```

It asks for your password once: macOS only lets an administrator take a USB device that another app is using.
Then plug the controller in and start the game. If the game was already open, restart it.

If you reinstall Warframe for Mac later, it re-applies controller support by itself.

### Other games

```bash
./install.sh --game Warframe.x64.exe --game OtherGame.exe "/path/to/OtherGame.app"
```

`--game` is the game's `.exe` name (the list replaces the previous one, so include every game you want). The path is the Sikarugir/Wineskin wrapper app.

## Uninstall

```bash
./uninstall.sh
```

Removes the service and puts back Wine's original SDL library.

## How it works

```
USB controller ──► xboxone-bridge (service, SDL + libusb) ──► state.bin ──► SDL inside Wine ──► XInput in the game
                         ▲                                                       │
                         └──────────────── rumble (rumble.bin) ◄────────────────┘
```

- `src/xboxone-bridge.c` — the service (LaunchDaemon `org.xboxoneformac.bridge`). It detects the game by the executable name running under Wine, captures the controller, publishes its state ~250 times per second and keeps the display awake while the controller is in use.
- `src/SDL_xboxonebridge.inc` — a controller driver added to the SDL 2.32.10 that Wine uses; it reads the state and forwards rumble.
- `patches/` — SDL changes: USB support for the original Xbox One on the service side (including reconnecting the cable) and the driver registration on the Wine side.
- `prebuilt/` — ready-made binaries (SHA256 in `prebuilt/SHA256SUMS`). To rebuild everything from the official sources: `./build.sh` (needs `cmake` and `pkg-config`). The build runs the driver tests (buttons, axes, triggers, rumble, stale/invalid data, unplug and replug).

**Security:** the service runs as root only to capture the USB device. It uses no network, only touches two files in `/Library/Application Support/XboxOneForMac`, and the rumble channel accepts only fixed-size numeric records from your user. Log: `/Library/Logs/XboxOneForMac.log`.

**Tested with:** Apple Silicon Mac, macOS 26, original wired Xbox One controller (`045e:02d1`), Warframe on Sikarugir/Wine 10. Bluetooth Xbox controllers already work natively on macOS and do not need this.

---

Not affiliated with Microsoft or Digital Extremes. Our code is under the [zlib License](LICENSE). Includes [SDL](https://github.com/libsdl-org/SDL) 2.32.10 (zlib, modified as in `patches/`) and [libusb](https://github.com/libusb/libusb) 1.0.29 (LGPL-2.1, unmodified, dynamically linked) — see [`licenses/`](licenses).
