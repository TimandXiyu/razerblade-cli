---
name: razerctl-repo-guide
description: Orientation for an AI agent or contributor working on razerctl (Razer Blade 16 control on Linux). Covers the repo layout, the two subsystems (EC-over-hidraw and the NVIDIA dGPU undervolt), the build/install + root/capability model, and the Synapse USB-sniffing workflow for adding new EC controls. Use before changing razerctl.c, the docs, or porting a new opcode.
---

# Working on razerctl — agent guide

`razerctl` is a single-file C program (`razerctl.c`, ~900 lines, no external libs
beyond `-ldl`) that controls a Razer Blade 16 on Linux. This guide orients you
before you edit. Read it, then read the relevant section of `razerctl.c` — don't
work from memory of the protocol.

## Repo layout

- **`razerctl.c`** — the whole program. Sections, top to bottom: capability /
  self-heal helpers → HID framing (`build`/`xfer`/`snd`) → EC reads/writes →
  fan curve → NVML-lite + NvAPI dGPU undervolt → TUI → `main()` (CLI dispatch).
- **`razerctl.1`** — the man page; it is the canonical CLI reference. Keep it in
  sync whenever you add/change/remove a command.
- **`README.md`** — user-facing, deliberately concise: quick start, the
  dashboard, the dGPU undervolt, and the **Protocol reference** table (framing,
  CRC, opcode list) for forkers. The CLI table lives in the man page, not here.
- **`Makefile`** and **`CMakeLists.txt`** — two equivalent build paths. Both
  `install` targets do the same setup (see below). Keep them in lockstep.
- **`sniff/capture.ps1`** — Windows USBPcap capture driver for opcode sniffing.
- **`PLAYBOOK.md`** — personal notes, git-ignored from publication.

## Two subsystems — know which one you're touching

**1. EC control over hidraw** (fans, perf mode, keyboard, battery, EPP).
Razer routes these through 90-byte USB-HID feature reports to the keyboard MCU.
This side is **sudo-less** via a udev `uaccess` rule. Adding or fixing one of
these controls is the **Synapse-sniffing workflow** below. A single-instance
`flock` (`/tmp/razerctl.lock`) stops two processes from interleaving EC reports
and corrupting readbacks — preserve it.

**2. dGPU undervolt via NVIDIA's undocumented NvAPI** (`libnvidia-api.so`, the
interface Afterburner uses) plus NVML (`libnvidia-ml.so`) for telemetry. It reads
and rewrites the per-point voltage/frequency clock table. This is a different
world from the EC side — no hidraw, found by device not PCI address, degrades
gracefully on an iGPU-only boot. Two privilege tiers, and they are not the same:
- **Curve write** (NvAPI clock table) needs `CAP_SYS_ADMIN`. It is sudo-less
  because `make install` gives the binary the `cap_sys_admin+epi` file-cap and
  `raise_ambient_sysadmin()` promotes it into the ambient set so it survives the
  privileged helper `libnvidia-api.so` exec's.
- **Max-freq cap** (`nvmlDeviceSetGpuLockedClocks`) needs **real root** —
  `cap_sys_admin` is *not* enough, the call returns `NO_PERMISSION`. So the whole
  dGPU undervolt TUI page is **read-only unless `geteuid()==0`**: plain `razerctl`
  shows it but blocks edits; `sudo razerctl` edits. Don't reintroduce a
  partial/sudo-less edit path here — the black-and-white rule is intentional.

## Build, install, and the root/capability model

```sh
make && sudo make install            # or:
cmake -B build && cmake --build build && sudo cmake --install build
```

`sudo make install` (or the CMake equivalent) does three things, and **the file
cap drops on every copy** so it must re-run after each rebuild: installs the
binary to `/usr/local/bin`, `setcap cap_sys_admin+epi`, writes the udev rule, and
installs the man page. Note the **PATH self-heal**: if `razerctl` resolves to the
uncapped build artifact in the repo dir, `maybe_reexec_capped()` transparently
re-execs the installed capped copy (guarded once by `RZ_REEXEC`) — so a rebuilt,
uncapped binary still works. `RZ_CAPDEBUG=1` prints the cap state.

**Persistence:** `~/.config/razerctl-uv.conf` + the `razerctl-uv.service` user
unit reapply the undervolt at login (sudo-less). The max-freq cap is **not**
restored at boot (needs root). `nvtest` is a read-only curve/voltage diagnostic.

## Removed — do not resurrect without cause

- **Custom perf mode (mode 4)** and the CPU/GPU **boost** sub-levels — retired
  from the UI (no reliable GPU-TDP effect on Linux; Dynamic Boost governs that
  instead). The opcodes remain documented in the README table for forkers.
- **`reclaim`** (KWin restart to drop the dGPU) — we park the dGPU by rebooting
  into an iGPU-only BIOS state, so it was meaningless in dGPU-only MUX mode.

## TUI conventions

Arrow-key driven: Up/Down select a row, Left/Right change it, Enter opens a
sub-page or toggles, Esc backs out of the dGPU page, `q` quits. Only `q` is a
bare hotkey. Telemetry is cached and refreshed on a timer (≈5 s) and the fan
curve ticks on its own timer — **never** re-poll sensors or spawn `nvidia-smi`
per keystroke (it floods the EC and stalls the UI). Render in place (no
full-screen clear per frame). When you add a row, update the row count constant
(`MROWS`/`DROWS`) and the wrap-around.

---

# Synapse opcode sniffing — adding a new EC control

When an EC control is unknown, or a "working" write is silently ignored, the
ground truth is **what Razer Synapse sends on Windows**. This workflow produced
the battery-limit and fan-`args[0]` fixes; follow it rather than guessing.

## When to sniff vs. guess

- Guessing nearby opcodes is fine for *reads* (harmless; status `0x05` = not
  supported) — never for *writes* to anything persistent. Battery/EC NVRAM
  writes are the genuinely harmful-if-wrong case.
- Sniff when: firmware rejects your guessed write (`0x05`), a write is accepted
  (`0x02`) but has no real effect, or the knob has no known class.

## Phase 1 — capture on Windows

1. Boot Windows with Synapse installed. Install Wireshark **with the USBPcap
   component** (`winget install USBPcap.USBPcap` if skipped), then **reboot
   once** — the capture driver only arms after a reboot.
2. If Synapse offers a firmware update, **decline** — you want the protocol of
   the firmware you actually run on Linux.
3. Capture **one labeled segment per UI action**, across **all USB root hubs**
   (you don't know which hub the EC HID hangs off). Use **`sniff/capture.ps1`**
   (run as Administrator): it loops `USBPcapCMD.exe -d \\.\USBPcap<N> -o
   <label>-hub<N>.pcap -A` per segment — press Enter, perform exactly ONE Synapse
   action, press Enter to stop. Edit its `$actions` list. Always start with a
   **baseline segment touching nothing** and a **sanity segment of a known
   control** (validates the rig).
4. For value-carrying controls, capture a **sweep** (2500/2900/3500/4000/4800
   RPM…) so the value byte identifies itself.
5. Keep a notes file alongside the pcaps: what you clicked, in what order, any UI
   surprises. The decode step lives or dies on accurate labels.

## Phase 2 — decode

- Filter SET_REPORT control transfers to the EC HID (VID `1532`, PID `02b7`) with
  90-byte payloads:
  `tshark -r seg.pcap -Y 'usb.transfer_type==2 && usb.data_len==90' -T fields -e usb.capdata`.
- Read each frame against the framing (byte6=class, byte7=cmd, byte8..=args).
  Diff against the baseline; ignore constant keyboard-LED chatter (`0x03 0x0a/0x0b`).
- For sweeps, line frames up with labels — the byte tracking the swept value is
  your value byte (RPM/100, `pct|0x80`, …).
- Watch the **`args[0]` convention**: Synapse sends `args[0]=0x01` on fan/boost
  ops where old community code used `0x00`. With `0x00` the EC may ACK (`0x02`)
  and echo the setpoint on readback **without applying it** — the classic silent
  failure this workflow exists to catch.
- Synapse sometimes follows a setter with a companion write (e.g. `0x07/0x0f`
  after the charge-limit set). Try the bare setter first before replicating it.

## Phase 3 — port into razerctl.c

1. **Implement the read first** if one exists; confirm it against a known state.
   Only then expose the write.
2. Add a helper following the existing pattern (`set_charge_limit`, etc.): build
   args, `snd()`, treat reply status `!=0x02` as failure. Mirror Synapse's exact
   arg bytes — including `args[0]` and per-zone duplication (fan ops go to BOTH
   zones 1 and 2).
3. **Verify on-device before pushing**, with an independent signal, not just the
   ACK: tachometer settling (40–50 s), RAPL package power for perf modes,
   readback where it exists, behavior (charging stops) where it doesn't. If no
   readback can confirm a persistent write, say so in the command output and
   document the limitation honestly.
4. Update **three** things in the same commit: the README **Protocol reference**
   table, the **man page** (`razerctl.1`), and the `usage:` line in `main()`.

## Known open items (good first sniffs)

- **Battery charge-limit readback**: capture a Synapse session that *displays* an
  existing limit on launch — the read it issues is the missing opcode. Until
  then the setter can't be auto-verified.
- Anything answering `0x05` in the README table on other Blade models — the same
  workflow ports razerctl across the lineup.
