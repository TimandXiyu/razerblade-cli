# razerctl — a tiny Razer Blade 16 control tool for Linux

`razerctl` lets you control a **Razer Blade 16** from the Linux terminal —
fans, performance mode, keyboard backlight, battery charge limit, and even an
**NVIDIA dGPU undervolt** — plus a live power dashboard. No Python, no Synapse.

It's two small, dependency-free **C** programs:

- **`razerctld`** — a background daemon (systemd system service, root) that
  owns the Razer USB-HID device and the temp-driven fan curve. It persists
  your perf mode, fan mode, keyboard backlight, and battery charge limit to
  disk and re-applies them at boot, and keeps them consistent in the
  background whether or not anything is watching.
- **`razerctl`** — the CLI/TUI **client**, unprivileged, talking to the daemon
  over a local socket. This is what you actually run day to day.

Think of it as a few hundred lines of C doing what Synapse does, minus the
heavy GUI.

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

Then build and install. Two equivalent ways — pick whichever you like:

```sh
git clone https://github.com/TimandXiyu/blade-cli.git
cd blade-cli

# Option A — plain Makefile
make
sudo make install

# Option B — CMake (standard configure/build/install)
cmake -B build
cmake --build build
sudo cmake --install build
```

Both install `razerctl` + `razerctld` in `/usr/local/bin`, and enable+start
`razerctld` as a systemd service (`systemctl status razerctld` to check).

Try it:

```sh
razerctl get           # prints your perf mode + fan setpoint -- no sudo needed
razerctl               # no arguments → launches the dashboard
```

If `razerctl get` prints your mode and fan setpoint, you're good. 🎉

### Running without `sudo`

`razerctld` (the daemon) is the only thing that touches the Razer HID device,
and it runs as root via systemd — so `razerctl` itself (fans, perf mode,
keyboard, battery limit, the dashboard) never needs `sudo`. It talks to the
daemon over `/run/razerctld.sock`, which is world-writable on the assumption
this is a single-user laptop.

The **one exception** is the dGPU **undervolt** page/subcommand (`razerctl uv
...`) — that's unrelated to the daemon, drives NVIDIA's NvAPI directly, and
still needs `sudo razerctl` to actually write a curve (reading is fine
unprivileged). See [Undervolting the dGPU](#undervolting-the-dgpu).

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

The **dGPU undervolt ▸** row opens the undervolt sub-page (press Enter).

---

## Undervolting the dGPU

On the **dGPU undervolt** page you can shift the GPU's voltage/frequency curve to
run cooler and quieter at the same clocks — like Afterburner's curve undervolt:

- **Undervolt (mV)** — how far to shift the curve left (lower voltage for a given
  clock). Start small, e.g. 30–50 mV.
- **Min freq** — clocks below this stay at stock voltage (keeps idle stable).
- **Max freq** — a hard clock ceiling, like a power slider. `off` = no cap.
- **Apply** / **Reset** — apply your settings, or return the GPU to stock.

**You must run it as root to change anything here.** Launch with
`sudo razerctl`. Plain `razerctl` opens this page **read-only** — you can watch
the live readout but the settings are locked, and the page says so up top. Simple
rule: want to touch the GPU, use `sudo razerctl`.

From the command line:

```sh
sudo razerctl uv 50 1700 2400   # -50 mV above 1700 MHz, capped at 2400 MHz
sudo razerctl uv reset          # back to stock, clear the saved profile
razerctl nvtest                 # read-only: dump the live curve + voltage (no sudo)
```

**Defaults:** the page opens at **Min freq 1695 MHz** and **Max freq 2400 MHz** so
you don't have to hold the arrow keys every time — just tweak from there.

**Persistence:** your undervolt is saved and re-applied automatically at login.
The **max-freq cap is not** auto-restored on reboot (it needs root) — re-apply it
with `sudo razerctl` each session if you want it.

> ⚠️ If your panel is driven by the dGPU (BIOS dGPU-only / MUX mode), an unstable
> curve can crash the display (recoverable). Tune gently: apply a modest offset,
> confirm it holds under load, then push further. `Reset` returns to stock.

---

## Command-line reference

The dashboard is self-explanatory, and every command-line option is documented in
the man page:

```sh
man razerctl
```

(or `razerctl` with no arguments for the dashboard, or any wrong argument to print
a quick usage line).

---

## How it works

Razer routes fan / perf / RGB control through 90-byte USB-HID feature reports
(the openrazer format) to the keyboard MCU — **not** the ACPI EC. `razerctld`
builds those reports and sends them via `HIDIOCSFEATURE` / `HIDIOCGFEATURE`; the
temp-driven fan curve also lives there, since it's an ongoing control loop that
has to keep running whether or not `razerctl` is open. `razerctl` never opens
hidraw at all — it sends line-based commands over a UNIX socket and gets a
response back (see `razer_ipc.h` for the socket path; the protocol is a plain
`VERB args\n` → `OK/ERR ...\n` request/response, one round-trip per command). The
dGPU undervolt is separate from all of this: it drives NVIDIA's undocumented
NvAPI through `libnvidia-api.so` (the same interface Afterburner uses on
Windows) to read and rewrite the per-point clock table, with NVML for
clock/power/temperature readouts and the optional locked-clock cap — that part
stays in `razerctl` directly, unrelated to the daemon.

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
