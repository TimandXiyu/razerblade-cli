CC ?= gcc
CFLAGS ?= -O2 -Wall
LDLIBS ?= -ldl
all: razerctl
razerctl: razerctl.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)
PREFIX ?= /usr/local
# install + grant cap_sys_admin so the dGPU undervolt (NvAPI clock-table write)
# works WITHOUT sudo. The EC/hidraw side is already sudo-less via udev uaccess.
# Caps are dropped on copy, so they MUST be re-applied on every install.
install: razerctl
	install -m755 razerctl $(PREFIX)/bin/razerctl
	setcap cap_sys_admin+epi $(PREFIX)/bin/razerctl

clean:
	rm -f razerctl burn *.o
.PHONY: all clean install
