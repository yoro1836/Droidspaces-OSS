/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspaces.h"

#include <ftw.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>

/* Unprivileged containers: root inside is a plain uid outside.
 *
 * The per-boot intermediate unshares a user namespace together with the other
 * namespaces, so that every one of them is owned by it and root inside has its
 * capabilities over them and nowhere else. A helper that stayed behind in the
 * host's user namespace writes the id maps and hands over the rootfs as an
 * idmapped mount, which is what makes the files on disk, owned by real ids,
 * show up under the container's ids without a chown of the image. */

/* The fallbacks cover a libc older than the syscalls (Linux 5.2 and 5.12). */
#ifndef __NR_open_tree
#define __NR_open_tree 428
#endif
#ifndef __NR_move_mount
#define __NR_move_mount 429
#endif
#ifndef __NR_mount_setattr
#define __NR_mount_setattr 442
#endif
#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC O_CLOEXEC
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif
#ifndef MOUNT_ATTR_IDMAP
#define MOUNT_ATTR_IDMAP 0x00100000
#endif
#ifndef MOUNT_ATTR_NOSUID
#define MOUNT_ATTR_NOSUID 0x00000002
#endif

/* struct mount_attr, under our own name: old kernel headers lack it and new
 * ones define it in a header musl does not always pull in. */
struct ds_mount_attr {
  uint64_t attr_set;
  uint64_t attr_clr;
  uint64_t propagation;
  uint64_t userns_fd;
};

/* Android's shared storage is not something a foreign uid can be let into
 * by permission bits: its top directory belongs to media_rw (1023), and each
 * file to the app that made it, uid and gid both, mode 0660. So with Android
 * storage on, those ids are mapped onto themselves. Root in the container
 * then has its capabilities over such files, which the kernel grants only
 * for an inode whose uid and gid are both mapped, and still none over
 * anything owned by the host's root or system. */
#define DS_AID_MEDIA_RW 1023u
#define DS_AID_APP_START 10000u
#define DS_AID_APP_COUNT 10000u

static int write_id_map(pid_t pid, const char *file, int storage) {
  char path[64], map[256];
  snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, file);
  if (!storage) {
    snprintf(map, sizeof(map), "0 %u %u\n", DS_USERNS_BASE, DS_USERNS_RANGE);
  } else {
    unsigned app_end = DS_AID_APP_START + DS_AID_APP_COUNT;
    snprintf(map, sizeof(map),
             "0 %u %u\n%u %u 1\n%u %u %u\n%u %u %u\n%u %u %u\n", DS_USERNS_BASE,
             DS_AID_MEDIA_RW, DS_AID_MEDIA_RW, DS_AID_MEDIA_RW,
             DS_AID_MEDIA_RW + 1, DS_USERNS_BASE + DS_AID_MEDIA_RW + 1,
             DS_AID_APP_START - DS_AID_MEDIA_RW - 1, DS_AID_APP_START,
             DS_AID_APP_START, DS_AID_APP_COUNT, app_end,
             DS_USERNS_BASE + app_end, DS_USERNS_RANGE - app_end);
  }
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  ssize_t n = write(fd, map, strlen(map));
  int saved = errno;
  close(fd);
  errno = saved;
  return n == (ssize_t)strlen(map) ? 0 : -1;
}

/* The other way to make a rootfs belong to the container: move every owner on
 * disk into the mapped range, as LXC does for a kernel without idmapped
 * mounts. Android kernels that get user namespaces from the Droidspaces
 * module are such kernels: their VFS was compiled without the translation.
 * up is 1 to move ids into the range and 0 to move them back. */
static int shift_up;

static int shift_one(const char *path, const struct stat *st, int type,
                     struct FTW *ftw) {
  (void)type;
  (void)ftw;
  uid_t uid = st->st_uid;
  gid_t gid = st->st_gid;
  int uid_in = uid >= DS_USERNS_BASE && uid - DS_USERNS_BASE < DS_USERNS_RANGE;
  int gid_in = gid >= DS_USERNS_BASE && gid - DS_USERNS_BASE < DS_USERNS_RANGE;

  if (shift_up) {
    if (uid < DS_USERNS_RANGE)
      uid += DS_USERNS_BASE;
    if (gid < DS_USERNS_RANGE)
      gid += DS_USERNS_BASE;
  } else {
    if (uid_in)
      uid -= DS_USERNS_BASE;
    if (gid_in)
      gid -= DS_USERNS_BASE;
  }
  if (uid == st->st_uid && gid == st->st_gid)
    return 0;

  if (lchown(path, uid, gid) < 0) {
    ds_warn("[USERNS] chown %s: %s", path, strerror(errno));
    return 0;
  }
  /* chown drops the setuid and setgid bits of a regular file. */
  if (S_ISREG(st->st_mode) && (st->st_mode & (S_ISUID | S_ISGID)))
    chmod(path, st->st_mode & 07777);
  return 0;
}

static int shift_tree(const char *rootfs, int up) {
  shift_up = up;
  return nftw(rootfs, shift_one, 64, FTW_PHYS | FTW_MOUNT);
}

static int rootfs_is_shifted(const char *rootfs) {
  struct stat st;
  return lstat(rootfs, &st) == 0 && st.st_uid >= DS_USERNS_BASE &&
         st.st_uid - DS_USERNS_BASE < DS_USERNS_RANGE;
}

void ds_userns_restore_ownership(struct ds_config *cfg) {
  if (cfg->unprivileged || !rootfs_is_shifted(cfg->rootfs_path))
    return;
  ds_log("Rootfs ownership was shifted for unprivileged mode, moving it "
         "back...");
  shift_tree(cfg->rootfs_path, 0);
}

/* A detached copy of the rootfs mount. With idmap set its ids are translated
 * through the container's user namespace, which only a process with
 * CAP_SYS_ADMIN over the filesystem's own user namespace may ask for, hence
 * the helper. */
static int rootfs_copy(const char *rootfs, pid_t owner, int idmap) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/ns/user", (int)owner);
  int userns_fd = idmap ? open(path, O_RDONLY | O_CLOEXEC) : -1;
  if (idmap && userns_fd < 0)
    return -1;

  int tree = (int)syscall(__NR_open_tree, AT_FDCWD, rootfs,
                          OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
  if (tree < 0) {
    int saved = errno;
    if (userns_fd >= 0)
      close(userns_fd);
    errno = saved;
    return -1;
  }

  /* Android mounts /data nosuid, and the copy would inherit that: sudo and su
   * in a directory rootfs need the bit gone. Setuid in here only ever reaches
   * the container's own root. */
  /* Private as well: a copy of a shared mount is its peer, and Android's
   * mounts are shared. pivot_root refuses a shared new root, and what the
   * container mounts under its root would show up on the host. */
  struct ds_mount_attr attr = {.attr_set = idmap ? MOUNT_ATTR_IDMAP : 0,
                               .attr_clr = MOUNT_ATTR_NOSUID,
                               .propagation = MS_PRIVATE,
                               .userns_fd = idmap ? (uint64_t)userns_fd : 0};
  if (syscall(__NR_mount_setattr, tree, "", AT_EMPTY_PATH, &attr,
              sizeof(attr)) < 0 &&
      idmap) {
    int saved = errno;
    close(tree);
    close(userns_fd);
    errno = saved;
    return -1;
  }
  if (userns_fd >= 0)
    close(userns_fd);
  return tree;
}

/* The rootfs as the container will see it: idmapped where the kernel can,
 * shifted on disk where it cannot. */
static int prepare_rootfs(const char *rootfs, pid_t owner) {
  struct stat before, after;
  if (lstat(rootfs, &before) < 0)
    return -1;

  if (!rootfs_is_shifted(rootfs) && !getenv("DS_USERNS_NO_IDMAP")) {
    int tree = rootfs_copy(rootfs, owner, 1);
    /* A kernel can accept the request and still not translate anything, so
     * look at the result instead of trusting the return value. */
    if (tree >= 0 && fstat(tree, &after) == 0 &&
        after.st_uid == before.st_uid + DS_USERNS_BASE)
      return tree;
    if (tree >= 0)
      close(tree);
    ds_log("[USERNS] No idmapped mounts here, shifting the rootfs ownership "
           "instead (once, this can take a while)...");
  }

  if (!rootfs_is_shifted(rootfs) && shift_tree(rootfs, 1) < 0)
    return -1;
  return rootfs_copy(rootfs, owner, 0);
}

/* What the helper reports back. step names the call that failed. */
struct userns_reply {
  int err;
  char step[24];
};

static void helper_main(struct ds_config *cfg, pid_t owner, int sock) {
  struct userns_reply reply = {0};
  int fds[2] = {-1, -1};
  char go;

  /* The parent says when its user namespace exists. End of file means it
   * died first, and there is nothing to map. */
  if (read(sock, &go, 1) != 1)
    _exit(1);

  int storage = cfg->android_storage && is_android();
  if (write_id_map(owner, "uid_map", storage) < 0) {
    reply.err = errno;
    safe_strncpy(reply.step, "uid_map", sizeof(reply.step));
  } else if (write_id_map(owner, "gid_map", storage) < 0) {
    reply.err = errno;
    safe_strncpy(reply.step, "gid_map", sizeof(reply.step));
  } else if ((fds[0] = prepare_rootfs(cfg->rootfs_path, owner)) < 0) {
    reply.err = errno;
    safe_strncpy(reply.step, "rootfs", sizeof(reply.step));
  } else if ((fds[1] = open(cfg->rootfs_path,
                            O_PATH | O_DIRECTORY | O_CLOEXEC)) < 0) {
    reply.err = errno;
    safe_strncpy(reply.step, "open rootfs", sizeof(reply.step));
  }

  if (reply.err || ds_send_fds(sock, &reply, sizeof(reply), fds, 2) < 0) {
    if (!reply.err)
      reply.err = EIO;
    if (write(sock, &reply, sizeof(reply)) < 0) {
      /* the parent reads end of file and fails the boot */
    }
    _exit(1);
  }
  _exit(0);
}

int ds_userns_unshare(struct ds_config *cfg, int ns_flags) {
  int sv[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0) {
    ds_error("[USERNS] socketpair failed: %s", strerror(errno));
    return -1;
  }

  pid_t self = getpid();
  pid_t helper = fork();
  if (helper < 0) {
    ds_error("[USERNS] fork failed: %s", strerror(errno));
    close(sv[0]);
    close(sv[1]);
    return -1;
  }
  if (helper == 0) {
    close(sv[0]);
    helper_main(cfg, self, sv[1]);
  }
  close(sv[1]);

  /* One call: the kernel creates the user namespace first and makes it the
   * owner of the others. Unshared one after the other they would belong to
   * the host, and root in here could not so much as set its hostname. */
  if (unshare(CLONE_NEWUSER | ns_flags) < 0) {
    ds_error("[USERNS] unshare failed: %s", strerror(errno));
    if (errno == EINVAL || errno == ENOSPC || errno == EPERM)
      ds_error("[USERNS] The kernel needs CONFIG_USER_NS, and "
               "/proc/sys/user/max_user_namespaces above zero.");
    close(sv[0]);
    waitpid(helper, NULL, 0);
    return -1;
  }

  struct userns_reply reply = {.err = EIO};
  int fds[2] = {-1, -1}, count = 0;
  int ok = write(sv[0], "1", 1) == 1 &&
           ds_recv_fds(sv[0], &reply, sizeof(reply), fds, 2, &count) ==
               (ssize_t)sizeof(reply);
  close(sv[0]);
  while (waitpid(helper, NULL, 0) < 0 && errno == EINTR)
    ;

  if (!ok || reply.err || count != 2) {
    reply.step[sizeof(reply.step) - 1] = '\0';
    ds_error("[USERNS] %s failed: %s", reply.step[0] ? reply.step : "helper",
             strerror(reply.err));
    for (int i = 0; i < count; i++)
      close(fds[i]);
    return -1;
  }

  /* We still carry the host's root uid, which has no mapping in here. */
  if (setgroups(0, NULL) < 0 || setresgid(0, 0, 0) < 0 ||
      setresuid(0, 0, 0) < 0) {
    ds_error("[USERNS] Failed to become root in the new namespace: %s",
             strerror(errno));
    close(fds[0]);
    close(fds[1]);
    return -1;
  }

  /* The uid change cleared the dumpable flag, and /proc/self of a process
   * without it belongs to the host's root: init could not open its own
   * /proc/self/fd, which the console bind goes through. */
  prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);

  cfg->userns_rootfs_fd = fds[0];
  cfg->userns_rootfs_dir_fd = fds[1];
  return 0;
}

int ds_userns_attach_rootfs(struct ds_config *cfg) {
  /* Our working directory is the rootfs directory, set by
   * ds_userns_stand_on_rootfs() before the mount namespace was unshared, so
   * "." names it in this namespace without a path walk. Root in here could
   * not walk it: the directories above it belong to the host's root. */
  if (syscall(__NR_move_mount, cfg->userns_rootfs_fd, "", AT_FDCWD, ".",
              MOVE_MOUNT_F_EMPTY_PATH) < 0) {
    ds_error("[USERNS] Failed to attach the idmapped rootfs: %s",
             strerror(errno));
    return -1;
  }
  /* The directory we stand on is now under the new mount, step up onto it. */
  if (fchdir(cfg->userns_rootfs_fd) < 0) {
    ds_error("[USERNS] Failed to enter the idmapped rootfs: %s",
             strerror(errno));
    return -1;
  }
  close(cfg->userns_rootfs_fd);
  close(cfg->userns_rootfs_dir_fd);
  cfg->userns_rootfs_fd = cfg->userns_rootfs_dir_fd = -1;

  /* Again from in here, for a kernel whose mount_setattr() did not take the
   * propagation change in the helper. */
  if (mount(NULL, ".", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
    ds_warn("[USERNS] Failed to make the rootfs private: %s", strerror(errno));
  return 0;
}

int ds_userns_stand_on_rootfs(struct ds_config *cfg) {
  if (fchdir(cfg->userns_rootfs_dir_fd) < 0) {
    ds_error("[USERNS] Cannot enter the rootfs directory as the container's "
             "root: %s",
             strerror(errno));
    ds_error("[USERNS] It needs search permission for others (chmod o+x).");
    return -1;
  }
  return 0;
}

void ds_userns_delegate_cgroup(const char *container_name) {
  char safe_name[256], dir[PATH_MAX], path[PATH_MAX + 32];
  sanitize_container_name(container_name, safe_name, sizeof(safe_name));
  snprintf(dir, sizeof(dir), "/sys/fs/cgroup/droidspaces/%s", safe_name);

  /* The files a delegatee must own to manage its subtree, see "Delegation"
   * in the kernel's cgroup-v2 documentation. The limit files stay ours. */
  const char *files[] = {"", "/cgroup.procs", "/cgroup.subtree_control",
                         "/cgroup.threads"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    snprintf(path, sizeof(path), "%s%s", dir, files[i]);
    if (chown(path, DS_USERNS_BASE, DS_USERNS_BASE) < 0 && errno != ENOENT)
      ds_warn("[USERNS] chown %s: %s", path, strerror(errno));
  }
}

int ds_userns_differs(pid_t pid) {
  char path[64];
  struct stat ours, its;
  snprintf(path, sizeof(path), "/proc/%d/ns/user", (int)pid);
  if (stat("/proc/self/ns/user", &ours) < 0 || stat(path, &its) < 0)
    return 0;
  return ours.st_ino != its.st_ino;
}

int ds_userns_check_config(struct ds_config *cfg) {
  if (!cfg->unprivileged)
    return 0;

  const struct {
    int set;
    const char *what;
  } clash[] = {
      {cfg->hw_access, "--hw-access"},
      {cfg->volatile_mode, "--volatile"},
      {cfg->force_cgroupv1, "--force-cgroupv1"},
  };
  int bad = 0;
  for (size_t i = 0; i < sizeof(clash) / sizeof(clash[0]); i++)
    if (clash[i].set) {
      ds_error("--unprivileged cannot be combined with %s.", clash[i].what);
      bad = 1;
    }
  if (bad)
    return -1;

  if (!ds_cgroup_host_is_v2()) {
    ds_error("--unprivileged needs cgroup v2 on the host: a v1 hierarchy "
             "cannot be handed to a user namespace.");
    return -1;
  }
  return 0;
}
