# razer-cli (`razerctl`)

A tiny, dependency-free **C** tool to control a **Razer Blade 16** on Linux —
fan, performance mode, and keyboard RGB — plus a live terminal power dashboard.
It talks Razer's USB-HID vendor protocol directly over `hidraw` (no daemon, no libusb).

> ⚠️ **Unofficial.** Reverse-engineered protocol. Tested on a Razer Blade 16
> (USB `1532:02b7`, firmware 1.3). Opcodes can differ across models/firmware —
> use at your own risk, especially manual fan control.

## Features
- **Perf mode** — Balanced / Gaming / Creator (verified against CPU package power)
- **Fan** — manual RPM (2000–5300) or auto
- **Keyboard** — solid-colour presets (white/red/purple/green) + off, brightness-locked
- **TUI dashboard** — live fan RPM, battery draw, dGPU power state, CPU temp / package & core power, C-state residency; pausable polling

## Installation

No experience needed — copy/paste each block into a terminal.

### 1. Install the build tools
You only need `git` and a C compiler (`gcc`). Pick your distro:

```sh
# Arch / CachyOS / Manjaro
sudo pacman -S --needed git base-devel

# Ubuntu / Debian / Pop!_OS
sudo apt update && sudo apt install -y git build-essential

# Fedora
sudo dnf install -y git gcc make
```

### 2. Download and build
```sh
git clone https://github.com/TimandXiyu/blade-cli.git
cd blade-cli
make
```
This creates an executable called `razerctl` in the folder. If `make` isn't
installed, run `gcc -O2 -o razerctl razerctl.c` instead.

### 3. Try it (with sudo)
`hidraw` (the device it talks to) is owned by root, so test with `sudo`:
```sh
sudo ./razerctl get
```
If you see your perf mode + fan setpoint printed, it works. 🎉

### 4. (Optional) Run without `sudo` every time
Install a udev rule so your normal user can use it directly:
```sh
sudo tee /etc/udev/rules.d/99-razerctl.rules >/dev/null <<'EOF'
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1532", ATTRS{idProduct}=="02b7", MODE="0660", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger
```
Now `./razerctl get` works without `sudo`.

### 5. (Optional) Run it from anywhere
Copy the binary into your `PATH` so you can just type `razerctl`:
```sh
sudo install -m755 razerctl /usr/local/bin/
razerctl            # launches the dashboard
```

### Troubleshooting
- **`no responding 1532:02b7 hidraw`** — your Razer model/USB id differs. Run
  `lsusb | grep 1532` to find yours; this tool targets `1532:02b7` (Blade 16).
- **`Permission denied`** — use `sudo`, or do step 4.
- **`gcc: command not found`** — redo step 1.

## Usage
```sh
razerctl get                              # show perf mode + fan setpoint
razerctl rpm                              # live fan RPM, 2s interval
razerctl mode balanced|gaming|creator     # set performance mode
razerctl fan auto                         # return fan to firmware control
razerctl fan 4000                         # manual RPM (clamped 2000-5300)
razerctl kbd white|red|purple|green|off   # keyboard backlight
razerctl                                  # no args -> TUI dashboard
```
TUI keys: `m` mode · `f` fan auto/manual · `+`/`-` RPM ±500 · `k` kbd colour · `p` pause monitor · `r` refresh · `q` quit.

> Note: the fan tachometer reading ramps slowly (~40–50 s to settle after a change).

## How it works
Razer routes fan/perf/RGB control through 90-byte USB-HID feature reports
(the openrazer report format) to the keyboard MCU — **not** the ACPI EC. This
tool builds those reports and sends them via `HIDIOCSFEATURE`/`HIDIOCGFEATURE`.

## Acknowledgements
Protocol knowledge derived from **Ashcon Mohseninia (rnd-ash)** and the
[`razer-laptop-control`](https://github.com/rnd-ash/razer-laptop-control)
project — huge thanks for the original reverse-engineering of Razer's laptop
fan/power protocol. Also thanks to the [openrazer](https://github.com/openrazer/openrazer)
project for the base report format, and to [`tdakhran/razer-ctl`](https://github.com/tdakhran/razer-ctl)
for Blade 16 reference.

## License
GPL-2.0 — see [LICENSE](LICENSE). Chosen to match `razer-laptop-control`,
from whose GPL-2.0 source the protocol details were learned.

## Protocol reference (for forking / extending)
All ops are a 90-byte report sent via `HIDIOCSFEATURE` (then `HIDIOCGFEATURE` to read the reply).
Framing: `byte0`=report id `0x00`, `byte1`=status, `byte2`=transaction id **`0x1f`**, `byte5`=data_size,
`byte6`=command class, `byte7`=command id, `byte8..`=args, `byte89`=CRC (`XOR` of bytes `2..87`).
Reply **status byte**: `0x02`=OK, `0x05`=not-supported, `0x01`=busy, `0x03`=fail.

| Operation | class | cmd | data_size | args `[a0,a1,...]` | Notes |
|---|---|---|---|---|---|
| Get firmware version | `0x00` | `0x81` | `0x02` | — | sanity/probe |
| Get serial | `0x00` | `0x82` | `0x16` | — | |
| **Get perf mode** | `0x0d` | `0x82` | `0x04` | `[0x00,0x01]` | mode in reply `args[2]`; 0=Bal 1=Gaming 2=Creator |
| **Set perf mode** | `0x0d` | `0x02` | `0x04` | `[0x00,0x01,mode,fanflag]` | mode in **args[2]**; `fanflag`=1 if manual fan active |
| Get CPU/GPU boost | `0x0d` | `0x87` | `0x03` | `[0x00,zone]` | zone 1=cpu 2=gpu (Custom only) |
| Set CPU/GPU boost | `0x0d` | `0x07` | `0x03` | `[0x00,zone,level]` | requires Custom mode (rejected on fw1.3) |
| **Fan: enable manual** | `0x0d` | `0x02` | `0x04` | `[0x00,zone,mode,0x01]` | send for **both** zone `0x01` and `0x02` |
| **Fan: set RPM** | `0x0d` | `0x01` | `0x03` | `[0x00,zone,rpm/100]` | per zone; after enabling manual |
| **Fan: auto** | `0x0d` | `0x02` | `0x04` | `[0x00,zone,mode,0x00]` | flag=0 on both zones |
| Fan: setpoint readback | `0x0d` | `0x81` | `0x04` | `[0x00,0x01]` | commanded value, in reply `args[2]`*100 |
| **Fan: tachometer** | `0x0d` | `0x88` | `0x04` | `[0x00,zone]` | measured RPM (`args[2]`*100); ramps ~40-50s |
| **Kbd brightness** | `0x03` | `0x03` | `0x03` | `[0x01,0x05,level]` | level 0-255 (0=off) |
| Kbd brightness read | `0x03` | `0x83` | `0x03` | `[0x01,0x05,0x00]` | in reply `args[2]` |
| **Kbd row colours** | `0x03` | `0x0b` | `0x34` | `[0xff,row,0x00,0x0f, ...45 bytes RGB]` | 6 rows (0-5) × 15 keys × R,G,B at `args[7]` |
| **Kbd commit frame** | `0x03` | `0x0a` | `0x02` | `[0x05,0x00]` | apply uploaded matrix (custom effect) |

CRC reference (C):
```c
unsigned char crc=0; for (int i=2;i<88;i++) crc ^= report[1+i];  // report[1] = struct byte 0
```

Control hidraw nodes for this device: `/dev/hidraw3..6` (any that answers Get-firmware works).

## razerctl command chart
| Command | Action |
|---|---|
| `razerctl get` | print perf mode + fan setpoint |
| `razerctl rpm` | live fan RPM (2s loop) |
| `razerctl mode <balanced\|gaming\|creator>` | set perf mode |
| `razerctl fan auto` | hand fan back to firmware |
| `razerctl fan <2000-5300>` | manual fan RPM |
| `razerctl kbd <white\|red\|purple\|green\|off>` | keyboard backlight |
| `sudo razerctl powerd <on\|off\|status>` | toggle NVIDIA `nvidia-powerd` (Dynamic Boost); `off` lets the dGPU reach D3cold (~0W) on hybrid setups |
| `razerctl reclaim` | restart KWin to release the dGPU after undocking (KWin auto-grabs it for an external but never frees it) → returns to D3cold. Brief screen flicker. |
| `razerctl` | launch TUI dashboard |

