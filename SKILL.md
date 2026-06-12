---
name: razerctl
description: Control a Razer Blade 16 (USB 1532:02b7) on Linux — fan speed, performance modes, CPU/GPU boost levels, keyboard RGB, battery charge limit, NVIDIA Dynamic Boost, and a live TUI power dashboard. Use when adjusting fans/thermals/power on a Razer Blade laptop, or as a protocol reference for extending Razer USB-HID control.
---

# razerctl — Razer Blade 16 control on Linux

A single-file, dependency-free C tool that speaks Razer's USB-HID vendor
protocol directly over `hidraw` — no daemon, no libusb, no kernel module.
This file orients a newcomer (human or agent) in minutes; the
[README](README.md) has the full install walkthrough, command chart, and
protocol table.

## Is this tool applicable?

- Hardware: reverse-engineered on a **Razer Blade 16, USB `1532:02b7`,
  firmware 1.3**. Check yours with `lsusb | grep 1532`. Other Blades often
  share the protocol but opcodes are **not guaranteed** — probe gently
  (`razerctl get` is read-only) before writing anything.
- OS: Linux with `hidraw` (any mainstream distro). Some features assume
  intel_pstate (EPP), NVIDIA proprietary driver (`powerd`, dGPU temp), or
  KWin/Wayland (`reclaim`) — everything else works without them.

## Quick start

```sh
git clone https://github.com/TimandXiyu/blade-cli.git
cd blade-cli && make
sudo ./razerctl get        # read-only probe: prints perf mode + fan setpoint
./razerctl                 # no args = live TUI dashboard
```

To run without sudo, install the udev `uaccess` rule from README step 4.

## Command map

| Task | Command |
|---|---|
| Live dashboard (fan RPM, battery W, dGPU state, temps, C-states) | `razerctl` |
| Performance mode | `razerctl mode balanced\|gaming\|creator\|custom` |
| CPU/GPU power sub-levels (Custom mode only) | `razerctl boost cpu high` |
| Manual fan / back to auto | `razerctl fan 4000` / `razerctl fan auto` |
| Temperature-driven fan curve (foreground loop) | `razerctl fancurve` |
| Battery charge limit | `razerctl battery 80` / `off` / `status` |
| Keyboard backlight | `razerctl kbd white\|red\|purple\|green\|off` |
| CPU energy/perf bias (intel_pstate EPP) | `razerctl epp 128` |
| NVIDIA Dynamic Boost daemon | `razerctl powerd on\|off\|status` |
| Release dGPU after undocking (KWin restart) | `razerctl reclaim` |

## Things that will bite you (read before driving it)

1. **Manual fan is a sequence, not one write.** A bare RPM write is silently
   ignored. You must send manual-enable (`0x0d/0x02`, arg3=1) to **both** fan
   zones, then the RPM (`0x0d/0x01`) per zone — and the RPM write's `args[0]`
   must be `0x01` (with `0x00` the EC echoes the setpoint back but doesn't
   settle the fan there). `set_fan()` in `razerctl.c` does all of this.
2. **The tachometer is slow.** Real measured RPM takes ~40–50 s to settle
   after a change. Don't conclude "it didn't work" at 10 s — use the setpoint
   readback (`0x0d/0x81`, instant) to confirm the command landed.
3. **RPM range is 2000–4800** (Synapse's rated range; the tool clamps).
4. **`boost` only works in Custom mode** — firmware rejects it otherwise
   (status `0x05`). CPU has 4 levels (0–3), GPU has 3 (0–2).
5. **The battery charge-limit write can't be auto-verified.** The setter
   (`0x07/0x12`, arg = `pct|0x80`) is well-attested, but no readback opcode
   for the stored percentage is known — `0x07/0x8f` returns a status byte.
   Verify behaviorally: watch charging stop near the limit.
6. **Never run `razerctl reclaim` under sudo.** It restarts KWin bound to the
   iGPU, which needs the *user's* Wayland session env; under root it silently
   no-ops and the dGPU is never released. Run the whole tool sudo-less.
7. **Don't poll the dGPU with `nvidia-smi` to "check" it's asleep** — that
   wakes it. Read sysfs (`/sys/bus/pci/devices/.../power_state`) instead;
   the TUI and fancurve already do this (a D3cold dGPU is never woken).
8. **EPP is transient under TLP**: TLP rewrites it on every AC↔battery
   switch, so `razerctl epp` is a live nudge until the next power change.
9. **`powerd on` is non-persistent** (runtime start/stop via polkit, not
   enable/disable) — a reboot returns to off, which is what lets the dGPU
   reach D3cold.
10. **After suspend/resume the hidraw `uaccess` ACL can drop** (the HID
    re-enumerates). If razerctl suddenly reports
    `no responding 1532:02b7 hidraw`, don't reach for sudo — re-trigger udev:
    `sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=hidraw --action=add`

## Extending / porting to another Blade

- The full opcode table (framing, classes, args, CRC) is in the README's
  **Protocol reference** section. All ops are 90-byte feature reports via
  `HIDIOCSFEATURE`/`HIDIOCGFEATURE`; reply status `0x02`=OK, `0x05`=not
  supported, `0x01`=busy, `0x03`=fail.
- Start by probing Get-firmware (`0x00/0x81`) on each `hidraw` node to find
  the one that answers; that's the control node.
- When firmware rejects a write, suspect (in order): wrong `args[0]`
  constant, missing per-zone duplication, a mode precondition (e.g. Custom
  for boost), or a genuinely different opcode on your model.
- The fan-curve tunables (`CPU_T_LO/HI`, `GPU_T_LO/HI`, `EMA_UP/DOWN`,
  `FAN_STEP`, `FAN_LOOP_S`) are `#define`s at the top of the curve block in
  `razerctl.c`.

## Provenance

Protocol knowledge derives from
[`rnd-ash/razer-laptop-control`](https://github.com/rnd-ash/razer-laptop-control)
and [openrazer](https://github.com/openrazer/openrazer), refined and corrected
against a USB capture of Razer Synapse on Windows (fan `args[0]` fix, Custom
mode, boost levels, charge limit). GPL-2.0.
