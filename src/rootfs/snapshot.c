/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "droidspaces.h"

#include <ftw.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>

#include "internal.h"

/* A new, empty top layer, the size of the base image and sparse like it.
 * Its "up" directory becomes the root of the container, so it takes the
 * owner and mode the root has now: an unprivileged container recognises a
 * shifted rootfs by that owner. */
static int layer_create(const char *img_path, int n, const struct stat *root) {
  char dir[PATH_MAX], img[PATH_MAX + 32], mnt[PATH_MAX + 32];
  char sub[PATH_MAX + 64];
  struct stat base;

  ds_layers_dir(img_path, dir, sizeof(dir));
  snprintf(img, sizeof(img), "%s/%d.img", dir, n);
  snprintf(mnt, sizeof(mnt), "%s/.mnt", dir);
  if (stat(img_path, &base) < 0 || !S_ISREG(base.st_mode)) {
    ds_error("Snapshots need a rootfs image file.");
    return -1;
  }

  int fd = open(img, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0 || ftruncate(fd, base.st_size) < 0) {
    ds_error("Failed to create %s: %s", img, strerror(errno));
    if (fd >= 0)
      close(fd);
    unlink(img);
    return -1;
  }
  close(fd);

  /* 4 KiB blocks whatever the size: Android's loop devices cannot serve a
   * filesystem with smaller ones. */
  char *mkfs[] = {"mke2fs", "-t", "ext4", "-b", "4096", "-q", "-F", img, NULL};
  int ok = run_command_quiet(mkfs) == 0;
  mkdir(mnt, 0700);
  if (ok && (ok = mount_image(img, mnt, 0) == 0)) {
    snprintf(sub, sizeof(sub), "%s/work", mnt);
    ok = mkdir(sub, 0700) == 0;
    snprintf(sub, sizeof(sub), "%s/up", mnt);
    ok = ok && mkdir(sub, 0755) == 0 &&
         chown(sub, root->st_uid, root->st_gid) == 0 &&
         chmod(sub, root->st_mode & 07777) == 0;
    unmount_rootfs_img(mnt, 1);
  }
  rmdir(mnt);
  if (!ok) {
    ds_error("Failed to prepare the snapshot layer %s (is mke2fs installed?)",
             img);
    unlink(img);
    return -1;
  }
  return 0;
}

static void layer_label_path(const char *img_path, int n, char *buf,
                             size_t size) {
  char dir[PATH_MAX];
  ds_layers_dir(img_path, dir, sizeof(dir));
  snprintf(buf, size, "%s/%d.label", dir, n);
}

/* What the root of the container looks like right now, layers and all. */
static int current_root_stat(struct ds_config *cfg, struct stat *st) {
  char mnt[PATH_MAX];
  if (mount_rootfs_img(cfg->rootfs_img_path, mnt, sizeof(mnt),
                       cfg->container_name) < 0)
    return -1;
  int r = stat(mnt, st);
  sync();
  unmount_rootfs_img(mnt, 1);
  return r;
}

static int snapshot_precheck(struct ds_config *cfg) {
  if (!cfg->rootfs_img_path[0]) {
    ds_error("Snapshots need a container on a rootfs image (-i).");
    return -1;
  }
  if (is_container_running(cfg, NULL)) {
    ds_error("Stop '%s' first.", cfg->container_name);
    return -1;
  }
  return 0;
}

int ds_snapshot_create(struct ds_config *cfg, const char *label) {
  char dir[PATH_MAX], path[PATH_MAX + 32];
  struct stat root;
  if (snapshot_precheck(cfg) < 0 || current_root_stat(cfg, &root) < 0)
    return -1;

  int n = ds_layers_top(cfg->rootfs_img_path) + 1;
  ds_layers_dir(cfg->rootfs_img_path, dir, sizeof(dir));
  mkdir(dir, 0700);
  if (layer_create(cfg->rootfs_img_path, n, &root) < 0)
    return -1;

  layer_label_path(cfg->rootfs_img_path, n, path, sizeof(path));
  write_file(path, label && label[0] ? label : "snapshot");
  ds_log("Snapshot %d taken of '%s'.", n, cfg->container_name);
  return 0;
}

int ds_snapshot_list(struct ds_config *cfg) {
  char dir[PATH_MAX], path[PATH_MAX + 32], label[128], when[32];
  int top = ds_layers_top(cfg->rootfs_img_path);
  ds_layers_dir(cfg->rootfs_img_path, dir, sizeof(dir));

  /* One per line for the app: number, time, MiB written since, label. */
  for (int n = 1; n <= top; n++) {
    struct stat st = {0};
    layer_label_path(cfg->rootfs_img_path, n, path, sizeof(path));
    label[0] = '\0';
    read_file(path, label, sizeof(label));
    label[strcspn(label, "\n")] = '\0';
    stat(path, &st);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
    snprintf(path, sizeof(path), "%s/%d.img", dir, n);
    stat(path, &st);
    printf("%d\t%s\t%lld\t%s\n", n, when,
           (long long)st.st_blocks * 512 / (1024 * 1024), label);
  }
  return 0;
}

/* Back to snapshot n: everything written after it goes, later snapshots
 * included, and n gets a fresh layer to write into. */
int ds_snapshot_restore(struct ds_config *cfg, int n) {
  char dir[PATH_MAX], path[PATH_MAX + 32];
  struct stat root;
  int top = ds_layers_top(cfg->rootfs_img_path);
  if (snapshot_precheck(cfg) < 0)
    return -1;
  if (n < 1 || n > top) {
    ds_error("'%s' has no snapshot %d.", cfg->container_name, n);
    return -1;
  }

  ds_layers_dir(cfg->rootfs_img_path, dir, sizeof(dir));
  for (int i = top; i >= n; i--) {
    snprintf(path, sizeof(path), "%s/%d.img", dir, i);
    unlink(path);
    if (i > n) {
      layer_label_path(cfg->rootfs_img_path, i, path, sizeof(path));
      unlink(path);
    }
  }
  if (current_root_stat(cfg, &root) < 0 ||
      layer_create(cfg->rootfs_img_path, n, &root) < 0)
    return -1;
  ds_log("'%s' is back at snapshot %d.", cfg->container_name, n);
  return 0;
}

/* Deleting a snapshot keeps the container as it is now, so the layer above
 * the snapshot is replayed onto the one below it, through an overlay mount
 * of that lower stack: removing a path there leaves the whiteout a deeper
 * layer needs, with no overlayfs bookkeeping of our own.
 *
 * Until the replay is complete the upper layer stays in place and still
 * decides what the container sees, so one that fails or is cut short has
 * changed nothing and can simply be run again. */
static char merge_src[PATH_MAX], merge_dst[PATH_MAX];

static void copy_xattrs(const char *src, const char *dst) {
  char names[4096], value[4096];
  ssize_t len = llistxattr(src, names, sizeof(names));
  for (char *name = names; len > 0 && name < names + len;
       name += strlen(name) + 1) {
    /* the layer's own overlayfs markers are not the file's */
    if (strncmp(name, "trusted.overlay.", 16) == 0)
      continue;
    ssize_t n = lgetxattr(src, name, value, sizeof(value));
    if (n >= 0)
      lsetxattr(dst, name, value, (size_t)n, 0);
  }
}

static int copy_data(const char *src, const char *dst) {
  static char buf[1 << 20];
  int in = open(src, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  int out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  ssize_t n = -1;
  while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof(buf))) > 0)
    if (write_all(out, buf, (size_t)n) < 0) {
      n = -1;
      break;
    }
  if (in >= 0)
    close(in);
  if (out >= 0 && close(out) < 0)
    n = -1;
  return n == 0 ? 0 : -1;
}

static int merge_one(const char *path, const struct stat *st, int type,
                     struct FTW *ftw) {
  (void)type;
  (void)ftw;
  char dst[PATH_MAX * 2], opaque = 0, target[PATH_MAX];
  struct stat old;
  snprintf(dst, sizeof(dst), "%s%s", merge_dst, path + strlen(merge_src));
  int exists = lstat(dst, &old) == 0;
  int ok = 1;

  /* a 0:0 character device is overlayfs for "deleted here" */
  if (S_ISCHR(st->st_mode) && st->st_rdev == 0) {
    if (exists && remove_recursive(dst) < 0)
      goto fail;
    return 0;
  }

  if (S_ISDIR(st->st_mode)) {
    /* An opaque directory replaced whatever was below it. The root of the
     * layer is never one, and removing it would be removing the rootfs. */
    if (path[strlen(merge_src)])
      lgetxattr(path, "trusted.overlay.opaque", &opaque, 1);
    if (exists && (!S_ISDIR(old.st_mode) || opaque == 'y')) {
      if (remove_recursive(dst) < 0)
        goto fail;
      exists = 0;
    }
    ok = exists || mkdir(dst, 0700) == 0;
  } else {
    if (exists && remove_recursive(dst) < 0)
      goto fail;
    if (S_ISREG(st->st_mode)) {
      ok = copy_data(path, dst) == 0;
    } else if (S_ISLNK(st->st_mode)) {
      ssize_t n = readlink(path, target, sizeof(target) - 1);
      ok = n >= 0 && (target[n] = '\0', symlink(target, dst) == 0);
    } else {
      ok = mknod(dst, st->st_mode, st->st_rdev) == 0;
    }
  }
  if (!ok || lchown(dst, st->st_uid, st->st_gid) < 0)
    goto fail;
  if (!S_ISLNK(st->st_mode) && chmod(dst, st->st_mode & 07777) < 0)
    goto fail;
  copy_xattrs(path, dst);
  struct timespec times[2] = {st->st_atim, st->st_mtim};
  utimensat(AT_FDCWD, dst, times, AT_SYMLINK_NOFOLLOW);
  return 0;

fail:
  ds_error("Merging %s: %s", dst, strerror(errno));
  return 1;
}

int ds_snapshot_delete(struct ds_config *cfg, int n) {
  char dir[PATH_MAX], from[PATH_MAX + 32], to[PATH_MAX + 32];
  char src[PATH_MAX + 32];
  const char *img = cfg->rootfs_img_path;
  int top = ds_layers_top(img);
  if (snapshot_precheck(cfg) < 0)
    return -1;
  if (n < 1 || n > top) {
    ds_error("'%s' has no snapshot %d.", cfg->container_name, n);
    return -1;
  }

  ds_layers_dir(img, dir, sizeof(dir));
  snprintf(src, sizeof(src), "%s/.src", dir);
  snprintf(merge_dst, sizeof(merge_dst), "%s/.dst", dir);
  snprintf(merge_src, sizeof(merge_src), "%s/up", src);
  snprintf(from, sizeof(from), "%s/%d.img", dir, n);
  mkdir(src, 0700);
  mkdir(merge_dst, 0700);

  int failed = mount_image(from, src, 1) < 0;
  if (!failed) {
    failed = (n == 1 ? mount_image(img, merge_dst, 0)
                     : mount_layers(img, merge_dst, n - 1)) < 0;
    if (!failed) {
      ds_log("Merging snapshot %d into what is below it...", n);
      failed = nftw(merge_src, merge_one, 64, FTW_PHYS) != 0;
      sync();
      unmount_rootfs_img(merge_dst, 1);
    }
    unmount_rootfs_img(src, 1);
  }
  rmdir(src);
  rmdir(merge_dst);
  if (failed) {
    ds_error("Snapshot %d was not deleted, the container is unchanged.", n);
    return -1;
  }

  /* Close the gap: layer and label i become i - 1. */
  for (int i = n; i <= top; i++) {
    const char *ext[] = {"img", "label"};
    for (int e = 0; e < 2; e++) {
      snprintf(to, sizeof(to), "%s/%d.%s", dir, i, ext[e]);
      snprintf(from, sizeof(from), "%s/%d.%s", dir, i + 1, ext[e]);
      if (i == top || rename(from, to) < 0)
        unlink(to);
    }
  }
  rmdir(dir); /* goes only when the last layer has */
  ds_log("Snapshot %d deleted.", n);
  return 0;
}
