/*
 * Copyright 2024 Sergio Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include "agx_device.h"

int agx_virtio_simple_ioctl(struct agx_device *dev, unsigned cmd, void *_req);

/* DRM_IOCTL_ASAHI_GET_PARAMS of any group under virtio: the area `pointer`
 * names (input for groups VM_STATUS and VM_STATUS_MIRROR) is appended to the
 * GET_PARAMS ccmd after struct drm_asahi_get_params, and the host answers the
 * area as the response payload. Returns 0, or -1 with errno set (as drmIoctl).
 */
int agx_virtio_get_params_area(struct agx_device *dev,
                               struct drm_asahi_get_params *params);

bool agx_virtio_open_device(struct agx_device *dev);
