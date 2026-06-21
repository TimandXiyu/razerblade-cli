CC ?= gcc
CFLAGS ?= -O2 -Wall
LDLIBS ?= -ldl
all: razerctl
razerctl: razerctl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
PREFIX ?= /usr/local
UDEVRULE = /etc/udev/rules.d/99-razerctl.rules
# One-shot setup (run with sudo). Installs the binary, then makes it sudo-less:
#  - udev uaccess rule -> your user can talk to the Razer HID (fans/mode/kbd/etc.)
#  - cap_sys_admin     -> the dGPU undervolt (NvAPI clock-table write) needs it
# Caps drop on copy, so they MUST be re-applied on every install.
# (The dGPU MAX-FREQ cap still needs real root -- cap_sys_admin isn't enough for it.)
install: razerctl
	install -m755 razerctl $(PREFIX)/bin/razerctl
	setcap cap_sys_admin+epi $(PREFIX)/bin/razerctl
	printf '%s\n' 'KERNEL=="hidraw*", SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1532", ATTRS{idProduct}=="02b7", MODE="0660", TAG+="uaccess"' > $(UDEVRULE)
	udevadm control --reload-rules && udevadm trigger || true
	@echo "installed $(PREFIX)/bin/razerctl + udev rule. Re-plug or re-login if it can't reach the device yet."

uninstall:
	rm -f $(PREFIX)/bin/razerctl $(UDEVRULE)
	udevadm control --reload-rules || true

clean:
	rm -f razerctl burn *.o
.PHONY: all clean install uninstall
