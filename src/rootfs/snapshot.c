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
  /* A resize grows the layer being written, so the newest one has the size
   * the user last asked for. */
  struct stat prev;
  snprintf(mnt, sizeof(mnt), "%s/%d.img", dir, n - 1);
  if (n > 1 && stat(mnt, &prev) == 0 && prev.st_size > base.st_size)
    base.st_size = prev.st_size;
  snprintf(mnt, sizeof(mnt), "%s/.mnt", dir);

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

/* A clone shares the frozen images of its origin by hard link. Before one is
 * written to, by a merge, it gets a copy of its own. Holes stay holes. */
static int own_copy(const char *img) {
  struct stat st;
  char tmp[PATH_MAX + 8];
  static char buf[1 << 20];
  if (stat(img, &st) < 0 || st.st_nlink < 2)
    return 0;

  ds_log("%s is shared with a clone, copying it first...", img);
  snprintf(tmp, sizeof(tmp), "%s.own", img);
  int in = open(img, O_RDONLY | O_CLOEXEC);
  int out = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  int ok = in >= 0 && out >= 0 && ftruncate(out, st.st_size) == 0;
  off_t data = 0, hole;
  while (ok && (data = lseek(in, data, SEEK_DATA)) >= 0) {
    hole = lseek(in, data, SEEK_HOLE);
    if (hole < 0)
      hole = st.st_size;
    while (ok && data < hole) {
      size_t want = (size_t)(hole - data) < sizeof(buf) ? (size_t)(hole - data)
                                                        : sizeof(buf);
      ssize_t got = pread(in, buf, want, data);
      ok = got > 0 && pwrite(out, buf, (size_t)got, data) == got;
      data += got;
    }
  }
  /* ENXIO is lseek's "no more data", the normal way out */
  ok = ok && errno == ENXIO;
  if (in >= 0)
    close(in);
  if (out >= 0 && (fsync(out) < 0 || close(out) < 0))
    ok = 0;
  if (!ok || rename(tmp, img) < 0) {
    ds_error("Failed to copy %s: %s", img, strerror(errno));
    unlink(tmp);
    return -1;
  }
  return 0;
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

  /* The image the merge writes into: the layer below, or the base. */
  if (n == 1)
    safe_strncpy(to, img, sizeof(to));
  else
    snprintf(to, sizeof(to), "%s/%d.img", dir, n - 1);

  int failed = own_copy(to) < 0 || mount_image(from, src, 1) < 0;
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

/* A second container that starts out as this one is now. The two share every
 * image up to this moment by hard link and each writes to a layer of its own,
 * so a clone is as quick and as cheap as a snapshot. */
int ds_container_clone(struct ds_config *cfg, const char *new_name) {
  char src_dir[PATH_MAX], dst_cfg[PATH_MAX], dst_dir[PATH_MAX];
  char dst_img[PATH_MAX * 2], from[PATH_MAX * 2], to[PATH_MAX * 2];
  char label[160], line[PATH_MAX * 2];
  const char *img = cfg->rootfs_img_path;
  struct stat root;

  if (snapshot_precheck(cfg) < 0)
    return -1;
  if (!new_name || !validate_container_name(new_name) ||
      ds_config_host_path(new_name, dst_cfg, sizeof(dst_cfg)) < 0) {
    ds_error("Give the clone a valid name: clone NEW_NAME.");
    return -1;
  }
  safe_strncpy(dst_dir, dst_cfg, sizeof(dst_dir));
  *strrchr(dst_dir, '/') = '\0';
  if (access(dst_dir, F_OK) == 0) {
    ds_error("A container named '%s' already exists.", new_name);
    return -1;
  }

  /* Freeze what the origin has now. Both go on from this snapshot. */
  snprintf(label, sizeof(label), "before clone %s", new_name);
  if (ds_snapshot_create(cfg, label) < 0 || current_root_stat(cfg, &root) < 0)
    return -1;
  int frozen = ds_layers_top(img) - 1;

  const char *base = strrchr(img, '/');
  base = base ? base + 1 : img;
  snprintf(dst_img, sizeof(dst_img), "%s/%s", dst_dir, base);
  ds_layers_dir(img, src_dir, sizeof(src_dir));
  snprintf(to, sizeof(to), "%s.layers", dst_img);
  int ok = mkdir_p(to, 0700) == 0 && link(img, dst_img) == 0;
  for (int i = 1; ok && i <= frozen; i++) {
    snprintf(from, sizeof(from), "%s/%d.img", src_dir, i);
    snprintf(to, sizeof(to), "%s.layers/%d.img", dst_img, i);
    ok = link(from, to) == 0;
    snprintf(from, sizeof(from), "%s/%d.label", src_dir, i);
    snprintf(to, sizeof(to), "%s.layers/%d.label", dst_img, i);
    if (read_file(from, label, sizeof(label)) > 0)
      write_file(to, label);
  }
  if (!ok) {
    ds_error("Failed to link the images into %s: %s (clones need the same "
             "filesystem as their origin)",
             dst_dir, strerror(errno));
    return -1;
  }
  if (layer_create(dst_img, frozen + 1, &root) < 0)
    return -1;
  snprintf(label, sizeof(label), "cloned from %s", cfg->container_name);
  layer_label_path(dst_img, frozen + 1, to, sizeof(to));
  write_file(to, label);

  /* The origin's config, line for line, so the keys only the app knows come
   * along. The uuid stays behind: it names one container. */
  if (!cfg->config_file[0])
    ds_config_host_path(cfg->container_name, cfg->config_file,
                        sizeof(cfg->config_file));
  FILE *in = fopen(cfg->config_file, "re"), *out = fopen(dst_cfg, "we");
  while (in && out && fgets(line, sizeof(line), in)) {
    if (strncmp(line, "name=", 5) == 0)
      fprintf(out, "name=%s\n", new_name);
    else if (strncmp(line, "hostname=", 9) == 0)
      fprintf(out, "hostname=%s\n", new_name);
    else if (strncmp(line, "rootfs_path=", 12) == 0)
      fprintf(out, "rootfs_path=%s\n", dst_img);
    else if (strncmp(line, "env_file=", 9) == 0) {
      line[strcspn(line, "\n")] = '\0';
      snprintf(to, sizeof(to), "%s/.env", dst_dir);
      if (read_file(line + 9, from, sizeof(from)) > 0)
        write_file(to, from);
      fprintf(out, "env_file=%s\n", to);
    } else if (strncmp(line, "uuid=", 5) != 0 &&
               strncmp(line, "pidfile=", 8) != 0)
      fputs(line, out);
  }
  ok = in && out;
  if (in)
    fclose(in);
  if (out && fclose(out) != 0)
    ok = 0;
  if (!ok) {
    ds_error("Failed to write %s: %s", dst_cfg, strerror(errno));
    return -1;
  }
  ds_log("'%s' cloned to '%s'.", cfg->container_name, new_name);
  return 0;
}
