CC ?= gcc
CFLAGS ?= -O2 -Wall
LDLIBS ?= -ldl
all: razerctl razerctld
razerctl: razerctl.c razer_ipc.h
	$(CC) $(CFLAGS) -o $@ razerctl.c $(LDLIBS)
razerctld: razerctld.c razer_ipc.h
	$(CC) $(CFLAGS) -o $@ razerctld.c
PREFIX ?= /usr/local
UNITDIR = /etc/systemd/system
# razerctld (daemon, root, owns hidraw+EC) runs as a system service; razerctl (client,
# TUI/CLI) is a plain unprivileged binary that talks to it over /run/razerctld.sock --
# no udev rule / setcap needed anymore, the client never touches hidraw directly.
install: razerctl razerctld
	install -m755 razerctl $(PREFIX)/bin/razerctl
	install -m755 razerctld $(PREFIX)/bin/razerctld
	install -Dm644 razerctl.1 $(PREFIX)/share/man/man1/razerctl.1
	install -Dm644 razerctld.service $(UNITDIR)/razerctld.service
	install -Dm755 zz-razerctld-resume /usr/lib/systemd/system-sleep/zz-razerctld-resume
	systemctl daemon-reload
	systemctl enable --now razerctld
	@echo "installed $(PREFIX)/bin/razerctl + razerctld + man page + resume hook. razerctld is enabled+running (state in /etc/razerctld/state.conf)."

uninstall:
	systemctl disable --now razerctld || true
	rm -f $(PREFIX)/bin/razerctl $(PREFIX)/bin/razerctld $(PREFIX)/share/man/man1/razerctl.1 $(UNITDIR)/razerctld.service
	rm -f /usr/lib/systemd/system-sleep/zz-razerctld-resume
	systemctl daemon-reload || true

clean:
	rm -f razerctl razerctld burn *.o
.PHONY: all clean install uninstall
