---
name: razerctl-protocol-sniffing
description: Guideline for agents/contributors working on razerctl (Razer Blade USB-HID control on Linux) — how to capture Razer Synapse's USB feature reports on Windows with Wireshark/USBPcap, decode the opcodes, and port them into the razerctl C program. Use when adding an unknown control (a Synapse knob razerctl lacks) or fixing a write the EC accepts but ignores.
---

# Sniffing Synapse opcodes and porting them to razerctl

razerctl talks to the Blade's EC through 90-byte USB-HID feature reports
(see the README's **Protocol reference** for framing, CRC, and the known
opcode table). When a control is unknown — or a "working" write is silently
ignored — the ground truth is **what Razer Synapse itself sends on Windows**.
This is the workflow that produced the Custom-mode, boost, battery-limit, and
fan-`args[0]` discoveries; follow it rather than guessing opcodes.

## When to sniff vs. when to guess

- Guessing nearby opcodes is fine for *reads* (harmless; status `0x05` =
  not supported) — never for *writes* to anything persistent. Battery/EC
  NVRAM writes are the genuinely harmful-if-wrong case.
- Sniff when: firmware rejects your guessed write (`0x05`), a write is
  accepted (`0x02`) but has no real effect, or the knob has no known class.

## Phase 1 — capture on Windows

1. Boot Windows with Synapse installed. Install Wireshark **with the USBPcap
   component** (`winget install USBPcap.USBPcap` if it was skipped), then
   **reboot once** — the capture driver only arms after a reboot.
2. If Synapse offers a firmware update, **decline** — you want the protocol
   of the firmware you actually run on Linux.
3. Capture **one labeled segment per UI action**, across **all USB root
   hubs** (you don't know which hub the EC HID hangs off). Use the driver
   script shipped in this repo — **[`sniff/capture.ps1`](sniff/capture.ps1)**,
   run as Administrator — which loops
   `USBPcapCMD.exe -d \\.\USBPcap<N> -o <label>-hub<N>.pcap -A` per segment:
   press Enter, perform exactly ONE Synapse action (e.g. "set charge limit
   to 60"), press Enter to stop. Edit its `$actions` list to the knobs you're
   after. Always start with a **baseline segment touching nothing** (to learn
   the idle chatter) and a **sanity segment of a control you already know**
   (validates the rig: you should see the known opcode).
4. For value-carrying controls, capture a **sweep** (several distinct values:
   2500/2900/3500/4000/4800 RPM…) so the value byte identifies itself.
5. Record a notes file alongside the pcaps: what you clicked, in what order,
   any UI surprises (e.g. "there is no fan slider, only a max-fan toggle").
   The decode step lives or dies on accurate labels.

## Phase 2 — decode

- Filter for SET_REPORT control transfers to the EC HID (VID `1532`; this
  Blade 16 is PID `02b7`) with 90-byte payloads. In tshark terms, roughly:
  `tshark -r seg.pcap -Y 'usb.transfer_type==2 && usb.data_len==90' -T fields -e usb.capdata`.
- Read each frame against the known framing: byte6=class, byte7=cmd,
  byte8..=args. Diff each segment against the baseline; ignore the constant
  keyboard-LED chatter (`0x03 0x0a/0x0b` frames).
- For sweeps, line the frames up with your labels — the byte tracking the
  swept value is your value byte (RPM/100, `pct|0x80`, etc.).
- Watch the **args[0] convention**: Synapse consistently sends `args[0]=0x01`
  on fan/boost ops where old community code used `0x00`. With `0x00` the EC
  may ACK (`0x02`) and even echo the setpoint on readback **without actually
  applying it** — the classic silent failure this workflow exists to catch.
- Note adjacent frames: Synapse sometimes follows a setter with an ambiguous
  companion write (e.g. `0x07/0x0f` after the charge-limit set, which also
  fires on an unrelated toggle). Don't blindly replicate those — try the bare
  setter first.

## Phase 3 — port into razerctl.c

1. **Implement the read first** if one exists; confirm it against a state you
   know (what Synapse/BIOS last set). Only then expose the write.
2. Add a small helper following the existing pattern (`set_boost`,
   `set_charge_limit`): build args, `snd()`, treat reply status `!=0x02` as
   failure. Mirror Synapse's exact arg bytes — including `args[0]` and
   per-zone duplication (fan ops go to BOTH zones 1 and 2).
3. **Verify on-device before pushing**, with an independent signal, not just
   the ACK: tachometer settling (give it 40–50 s), RAPL package power for
   perf modes, readback where it exists, behavior (charging stops) where it
   doesn't. If no readback can confirm a persistent write, say so in the
   command's output and document the limitation honestly in the README.
4. Update the README **Protocol reference** table and command chart in the
   same commit. Unverified or ambiguous findings belong there as notes too
   (e.g. the battery percent-readback is still unknown — `0x07/0x8f` returns
   a status byte, not the percentage).

## Known open items (good first sniffs)

- **Battery charge-limit readback**: capture a Synapse session that *displays*
  an existing limit on launch — the read it issues is the missing opcode.
  Until then the setter can't be auto-verified.
- Anything answering `0x05` in the README table on other Blade models —
  the same workflow ports razerctl across the lineup.
