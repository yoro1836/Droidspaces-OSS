/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DS_SECURITY_SECURITY_H
#define DS_SECURITY_SECURITY_H

/* caps.c */
void ds_apply_capability_hardening(int hw_access, int privileged_mask,
                                   int sandboxing);

/* jail.c */
int ds_apply_jail_mask(int hw_access, int privileged_mask, int sandboxing);

/* peer_auth.c */
/* Return 1 only when peer_pid is confirmed to share this process's PID
 * namespace, 0 otherwise: a dead, recycled or untranslatable pid is denied.
 * This is a trust boundary and fails closed; the one positive exception is a
 * kernel without CONFIG_PID_NS, which has a single namespace. Used by the
 * abstract-socket authorizers to reject host-net container peers. */
int ds_peer_in_pidns(pid_t peer_pid);
/* Authorize a connected AF_UNIX peer: allow root, or a member of group_name,
 * but only when the peer shares our PID namespace.  Returns 1 if authorized. */
int ds_peer_authorized(int fd, const char *group_name);

/* seccomp.c */
int ds_seccomp_apply_minimal(int privileged_mask, int sandboxing_allowed);

/* KernelSU container-escape hardening: ask KSU to mark the current thread
 * (TIF_KSU_DISABLE_ESCAPE_WITH_ROOT via KSU_IOCTL_DISABLE_ESCAPE_TO_ROOT)
 * so escape_with_root_profile() refuses to escalate it.  Best-effort,
 * silent no-op on non-KSU kernels.  Must run BEFORE ds_seccomp_apply_minimal,
 * whose magic-reboot block would otherwise deny the very reboot() this uses
 * to obtain its fd.  The seccomp block is the load-bearing (tree-wide)
 * barrier; this is per-thread defense-in-depth. */
void ds_ksu_neutralize_root_escape(void);

/* KSU, then the minimal filter, then the keyring shim, in that order. */
void ds_seccomp_harden(const struct ds_config *cfg);

/* seccomp_shims.c */
int ds_seccomp_keyring_shim(int privileged_mask);
int ds_kernel_has_mount_api(void);
int ds_mount_api_shim_wanted(const struct ds_config *cfg);
int ds_seccomp_mount_api_shim(const struct ds_config *cfg);

/* selinux.c */
int set_selinux_context(const char *path, const char *context);
void ds_set_selinux_permissive(int enable);
int ds_get_selinux_status(void);
int get_selinux_context(const char *path, char *buf, size_t size);
void ds_selinux_enter_domain(void);

/* userns.c */
/* Host ids the container's 0..DS_USERNS_RANGE-1 map to. Above every id
 * Android hands out (user * 100000 + app id), and below 2^31. */
#define DS_USERNS_BASE 1000000000u
#define DS_USERNS_RANGE 65536u

struct ds_config;
/* A privileged start of a rootfs that an unprivileged one shifted on disk:
 * move the owners back first. */
void ds_userns_restore_ownership(struct ds_config *cfg);
/* Fails when --unprivileged meets an option it cannot honour. */
int ds_userns_check_config(struct ds_config *cfg);
/* In the per-boot intermediate: unshare a user namespace plus ns_flags, map
 * the ids, become root inside. Leaves the idmapped rootfs in cfg. */
int ds_userns_unshare(struct ds_config *cfg, int ns_flags);
/* In init, before and after it unshares its mount namespace. */
int ds_userns_stand_on_rootfs(struct ds_config *cfg);
int ds_userns_attach_rootfs(struct ds_config *cfg);
/* Give the container's cgroup2 directory to its root. */
void ds_userns_delegate_cgroup(const char *container_name);
/* 1 when pid lives in another user namespace than the caller. */
int ds_userns_differs(pid_t pid);

#endif /* DS_SECURITY_SECURITY_H */
