/*
 * Droidspaces v7 - High-performance Container Runtime
 *
 * Copyright (C) 2026 ravindu644 <droidcasts@protonmail.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DS_ROOTFS_INTERNAL_H
#define DS_ROOTFS_INTERNAL_H

/* image.c */
int mount_image(const char *img_path, const char *mount_point, int read_only);
int mount_layers(const char *img_path, const char *mount_point, int top);

#endif
