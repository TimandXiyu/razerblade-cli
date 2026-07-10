// SPDX-License-Identifier: GPL-2.0-only
// Shared constant between razerctld (daemon, owns hidraw+EC) and razerctl
// (client, TUI/CLI). Protocol is line-based text over this UNIX socket:
// client sends "<VERB> [args]\n", daemon replies "OK [data]\n" or "ERR [why]\n".
#ifndef RAZER_IPC_H
#define RAZER_IPC_H
#define RAZERCTLD_SOCK "/run/razerctld.sock"
#endif
