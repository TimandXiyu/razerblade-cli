# razerctl — a tiny Razer Blade 16 control tool for Linux

`razerctl` is a small, dependency-free **C** program that lets you control a
**Razer Blade 16** from the Linux terminal — fans, performance mode, keyboard
backlight, battery charge limit, and even an **NVIDIA dGPU undervolt** — plus a
live power dashboard. No daemon, no Python, no Synapse. It talks Razer's USB-HID
protocol straight over `hidraw`.

Think of it as a few hundred lines of C doing what Synapse does, minus the heavy
GUI.

> ⚠️ **Unofficial and reverse-engineered.** Built and tested on one machine: a
> Razer Blade 16 (2024, USB `1532:02b7`) running CachyOS (Arch-based). Opcodes
> differ across models and firmware, so treat this as a working example for *your*
> Blade, not a guarantee. **Manual fan and GPU undervolt carry real risk — I am
> not liable for thermal damage or instability. Use at your own risk.**

---

## What it can do

- **Performance mode** — Balanced / Gaming / Creator. On Linux the dGPU power
  ceiling is actually governed by NVIDIA Dynamic Boost (`nvidia-powerd`), so the
  modes pair with a Dynamic-Boost toggle: Creator parks the GPU at its ~80 W eco
  floor, Balanced/Gaming let it boost.
- **Fans** — fixed RPM (2000–4800), firmware auto, or a smart **temperature-driven
  curve** that tracks CPU + dGPU temps.
- **dGPU undervolt** *(RTX 40-series)* — a per-point voltage/frequency curve editor
  (the same idea as MSI Afterburner's curve), with an optional max-frequency cap.
  See [Undervolting the dGPU](#undervolting-the-dgpu).
- **Keyboard backlight** — solid colour presets (white / red / purple / green) + off.
- **Battery charge limit** — cap charging at 60/70/80 % to preserve battery health.
- **CPU EPP** — energy-vs-performance bias (intel_pstate).
- **Live dashboard** — fan RPM, battery draw, dGPU power state, CPU temp & power,
  CPU busy %, all in a clean arrow-key TUI.

---

## Quick start

You need `git` and a C compiler. That's it.

```sh
# Arch / CachyOS / Manjaro
sudo pacman -S --needed git base-devel
# Ubuntu / Debian / Pop!_OS
sudo apt update && sudo apt install -y git build-essential
# Fedora
sudo dnf install -y git gcc make
```

Then build and install:

```sh
git clone https://github.com/TimandXiyu/blade-cli.git
cd blade-cli
make
sudo make install      # installs to /usr/local/bin and sets up sudo-less access
```

Try it:

```sh
razerctl get           # prints your perf mode + fan setpoint
razerctl               # no arguments → launches the dashboard
```

If `razerctl get` prints your mode and fan setpoint, you're good. 🎉

> **No `make install`?** You can run it straight from the folder with `sudo
> ./razerctl get`. But `make install` is recommended — it puts `razerctl` on your
> `PATH` and grants the one capability the dGPU undervolt needs (see below).

### Running without `sudo`

`make install` already sets things up so fans, perf mode, keyboard, and the dGPU
**undervolt** all work without `sudo`. Under the hood it installs a udev rule so
your user can talk to the Razer HID device, and gives the binary the
`cap_sys_admin` capability the GPU clock write needs. If you ever want the udev
rule by itself:

```sh
sudo tee /etc/udev/rules.d/99-razerctl.rules >/dev/null <<'EOF'
KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1532", ATTRS{idProduct}=="02b7", MODE="0660", TAG+="uaccess"
EOF
sudo udevadm control --reload-rules && sudo udevadm trigger
```

The **one exception** is the dGPU **max-frequency cap**, which needs real root —
see [Undervolting the dGPU](#undervolting-the-dgpu).

---

## The dashboard

Run `razerctl` with no arguments. Navigation is all arrow keys — no hotkey
cheat-sheet to memorise:

- **↑ / ↓** — move between settings
- **← / →** — change the selected setting
- **Enter** — open a sub-page or toggle the selected item
- **Esc** — go back (from the dGPU page to the main page)
- **q** — quit

The main page lets you set perf mode, fan mode (Auto / Manual / Curve), manual
fan RPM, keyboard colour, CPU EPP, Dynamic Boost, and the battery charge limit.
It also has a live readout up top (fan RPM, battery watts, dGPU state, CPU temp
& power, CPU busy %), refreshed every few seconds so navigation stays snappy.

Two rows open extra actions: **dGPU undervolt ▸** (Enter to open the undervolt
page) and **Reclaim dGPU** (restart KWin to let an idle dGPU drop back to its
near-0 W deep-sleep state after unplugging an external display).

---

## Undervolting the dGPU

On the **dGPU undervolt** page you can shift the GPU's voltage/frequency curve to
run cooler and quieter at the same clocks — like Afterburner's curve undervolt:

- **Undervolt (mV)** — how far to shift the curve left (lower voltage for a given
  clock). Start small, e.g. 30–50 mV.
- **Min freq** — clocks below this stay at stock voltage (keeps idle stable).
- **Max freq** — a hard clock ceiling, like a power slider. `off` = no cap.
- **Apply** / **Reset** — apply your settings, or return the GPU to stock.

**Sudo matters here.** The page shows a banner telling you which mode you're in:

- 🟢 **root** (`sudo razerctl`) — undervolt **and** max-freq cap both work.
- 🟡 **sudo-less** (plain `razerctl`) — undervolt works, but the **Max-freq cap
  needs `sudo`**. The row is tagged accordingly and Apply will tell you if a cap
  was skipped.

So: **if you want to set a max-frequency cap, launch the tool with
`sudo razerctl`.** If you only undervolt, plain `razerctl` is fine.

From the command line:

```sh
sudo razerctl uv 50 1000 2700   # -50 mV above 1000 MHz, capped at 2700 MHz
razerctl uv 50 1000             # undervolt only (no cap) — works sudo-less
sudo razerctl uv reset          # back to stock, clear the saved profile
razerctl nvtest                 # read-only: dump the live curve + voltage
```

**Persistence:** your undervolt is saved and re-applied automatically at login
(sudo-less). The **max-freq cap is not** auto-restored on reboot, because it needs
root — re-apply it with `sudo razerctl` each session if you want it.

> ⚠️ If your panel is driven by the dGPU (BIOS dGPU-only / MUX mode), an unstable
> curve can crash the display (recoverable). Tune gently: apply a modest offset,
> confirm it holds under load, then push further. `Reset` returns to stock.

---

## Command-line reference

| Command | What it does |
|---|---|
| `razerctl` | launch the TUI dashboard |
| `razerctl get` | print perf mode + fan setpoint + EPP |
| `razerctl mode <balanced\|gaming\|creator>` | set performance mode |
| `razerctl fan auto` | hand the fans back to firmware |
| `razerctl fan <2000-4800>` | set a fixed fan RPM |
| `razerctl fancurve` | temperature-driven auto fan (Ctrl-C restores auto) |
| `razerctl rpm` | live fan RPM, 2 s interval |
| `razerctl battery <50-100\|off\|status>` | battery charge limit (see note) |
| `razerctl kbd <white\|red\|purple\|green\|off>` | keyboard backlight |
| `razerctl epp [0-255]` | show or set CPU energy-vs-performance bias (0 = max perf) |
| `razerctl powerd <on\|off\|status>` | toggle NVIDIA Dynamic Boost (`nvidia-powerd`) |
| `razerctl power <max\|save\|status>` | `max` = boost on (≤175 W) · `save` = boost off |
| `razerctl reclaim` | restart KWin to release an idle dGPU → back to deep sleep |
| `razerctl uv <mV> <min> [max]` | apply a dGPU undervolt (max-freq cap needs sudo) |
| `razerctl uv reset` | reset the dGPU to stock + clear the saved profile |
| `razerctl nvtest` | read-only dGPU curve / voltage diagnostic |

**Notes:**
- The fan **tachometer ramps slowly** (~40–50 s to settle after a change) — that's
  the sensor, not a bug.
- The EC firmware has the final say on fan speed, so you may see it deviate from
  what you set. That's a built-in safety net; leave it be.
- Battery limit **can't be read back as a percentage** (the firmware only returns a
  status byte), so confirm it behaviourally — charging should stop near the limit.
- `epp` is reset by **TLP** on every AC↔battery switch.

---

## How it works

Razer routes fan / perf / RGB control through 90-byte USB-HID feature reports
(the openrazer format) to the keyboard MCU — **not** the ACPI EC. `razerctl`
builds those reports and sends them via `HIDIOCSFEATURE` / `HIDIOCGFEATURE`. The
dGPU undervolt is separate: it drives NVIDIA's undocumented NvAPI through
`libnvidia-api.so` (the same interface Afterburner uses on Windows) to read and
rewrite the per-point clock table, with NVML for clock/power/temperature
readouts and the optional locked-clock cap.

---

## Acknowledgements

Protocol knowledge derived from **Ashcon Mohseninia (rnd-ash)** and the
[`razer-laptop-control`](https://github.com/rnd-ash/razer-laptop-control) project —
huge thanks for the original reverse-engineering of Razer's laptop fan/power
protocol. Thanks also to [openrazer](https://github.com/openrazer/openrazer) for
the base report format, [`tdakhran/razer-ctl`](https://github.com/tdakhran/razer-ctl)
for Blade 16 reference, and the **nvcurve** project for the NvAPI clock-table
offsets used by the undervolt.

## License

GPL-2.0 — see [LICENSE](LICENSE). Chosen to match `razer-laptop-control`, from
whose GPL-2.0 source the protocol details were learned.

---

## Protocol reference (for forking / extending)

Your Blade probably speaks slightly different opcodes — especially across Intel
vs AMD generations. Treat this repo as a **guide to reverse-engineering your own**
rather than a drop-in. The reliable approach: sniff what Synapse sends on Windows
and port it. The full workflow (Wireshark + USBPcap capture, decode, on-device
verification) is in [SKILL.md](SKILL.md), with the capture script at
[`sniff/capture.ps1`](sniff/capture.ps1). If you get your variant working, PRs
are welcome.

All ops are a 90-byte report sent via `HIDIOCSFEATURE` (then `HIDIOCGFEATURE` to
read the reply). Framing: `byte0` = report id `0x00`, `byte1` = status,
`byte2` = transaction id **`0x1f`**, `byte5` = data_size, `byte6` = command class,
`byte7` = command id, `byte8..` = args, `byte89` = CRC (`XOR` of bytes `2..87`).
Reply **status byte**: `0x02` = OK, `0x05` = not-supported, `0x01` = busy, `0x03` = fail.

| Operation | class | cmd | data_size | args `[a0,a1,...]` | Notes |
|---|---|---|---|---|---|
| Get firmware version | `0x00` | `0x81` | `0x02` | — | sanity/probe |
| Get serial | `0x00` | `0x82` | `0x16` | — | |
| **Get perf mode** | `0x0d` | `0x82` | `0x04` | `[0x00,0x01]` | mode in reply `args[2]`; 0=Bal 1=Gaming 2=Creator |
| **Set perf mode** | `0x0d` | `0x02` | `0x04` | `[0x00,0x01,mode,fanflag]` | mode in **args[2]**; `fanflag`=1 if manual fan active |
| Get CPU/GPU boost | `0x0d` | `0x87` | `0x03` | `[0x01,zone]` | zone 1=cpu 2=gpu; level in reply `args[2]` |
| Set CPU/GPU boost | `0x0d` | `0x07` | `0x03` | `[0x01,zone,level]` | the old "Custom mode" levers; retired from the UI but the opcodes still work |
| **Fan: enable manual** | `0x0d` | `0x02` | `0x04` | `[0x00,zone,mode,0x01]` | send for **both** zone `0x01` and `0x02` |
| **Fan: set RPM** | `0x0d` | `0x01` | `0x03` | `[0x01,zone,rpm/100]` | per zone, after enabling manual; `args[0]` must be `0x01` — with `0x00` the EC echoes the setpoint but won't settle there |
| **Fan: auto** | `0x0d` | `0x02` | `0x04` | `[0x00,zone,mode,0x00]` | flag=0 on both zones |
| Fan: setpoint readback | `0x0d` | `0x81` | `0x04` | `[0x00,0x01]` | commanded value, in reply `args[2]`*100 |
| **Fan: tachometer** | `0x0d` | `0x88` | `0x04` | `[0x00,zone]` | measured RPM (`args[2]`*100); ramps ~40-50s |
| **Kbd brightness** | `0x03` | `0x03` | `0x03` | `[0x01,0x05,level]` | level 0-255 (0=off) |
| Kbd brightness read | `0x03` | `0x83` | `0x03` | `[0x01,0x05,0x00]` | in reply `args[2]` |
| **Kbd row colours** | `0x03` | `0x0b` | `0x34` | `[0xff,row,0x00,0x0f, ...45 bytes RGB]` | 6 rows (0-5) × 15 keys × R,G,B at `args[7]` |
| **Kbd commit frame** | `0x03` | `0x0a` | `0x02` | `[0x05,0x00]` | apply uploaded matrix (custom effect) |
| **Battery: set charge limit** | `0x07` | `0x12` | `0x01` | `[pct\|0x80]` | bit7=enable (60→`0xbc`, 80→`0xd0`); write with bit7 clear = disable, keeps the value (`0x41`) |
| Battery: charge-limit read | `0x07` | `0x8f` | `0x01` | `[0x00]` | returns a STATUS byte, **not** the % — the true percent-readback opcode is still unknown |

CRC reference (C):
```c
unsigned char crc=0; for (int i=2;i<88;i++) crc ^= report[1+i];  // report[1] = struct byte 0
```

Control hidraw nodes for this device: `/dev/hidraw3..6` (any that answers
Get-firmware works). The dGPU undervolt path is independent of hidraw — it goes
through `libnvidia-api.so` / NVML and finds the GPU by device, so it survives PCI
address changes and degrades gracefully on an iGPU-only boot.
