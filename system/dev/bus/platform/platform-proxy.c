// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <ddk/binding.h>
#include <ddk/debug.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/platform-device.h>
#include <ddk/protocol/usb-mode-switch.h>

#include "platform-proxy.h"

typedef struct {
    zx_device_t* zxdev;
    zx_handle_t rpc_channel;
    atomic_int next_txid;
    thrd_t proxy_thread;
} platform_dev_t;

typedef struct {
    platform_dev_t* dev;
    void* server_ctx;
    size_t max_transfer_size;
} pdev_i2c_channel_ctx_t;

static zx_status_t platform_dev_rpc(platform_dev_t* dev, pdev_req_t* req, pdev_resp_t* resp,
                                    zx_handle_t* out_handles, uint32_t out_handle_count) {
    uint32_t resp_size, handle_count;

    // even txids are used for zx_channel_call while odd ones are used for asynchronous responses.
    req->txid = atomic_fetch_add(&dev->next_txid, 2);

    zx_channel_call_args_t args = {
        .wr_bytes = req,
        .rd_bytes = resp,
        .wr_num_bytes = sizeof(*req),
        .rd_num_bytes = sizeof(*resp),
        .rd_handles = out_handles,
        .rd_num_handles = out_handle_count,
    };
    zx_status_t status = zx_channel_call(dev->rpc_channel, 0, ZX_TIME_INFINITE, &args, &resp_size,
                                         &handle_count, NULL);
    if (status != ZX_OK) {
        return status;
    } else if (resp_size != sizeof(*resp)) {
        zxlogf(ERROR, "platform_dev_rpc resp_size %u expected %zu\n", resp_size, sizeof(resp));
        status = ZX_ERR_INTERNAL;
        goto fail;
    } else if (handle_count != out_handle_count) {
        zxlogf(ERROR, "platform_dev_rpc handle count %u expected %u\n", handle_count,
                out_handle_count);
        status = ZX_ERR_INTERNAL;
        goto fail;
    } else {
        status = resp->status;
    }

fail:
    if (status != ZX_OK) {
        for (uint32_t i = 0; i < handle_count; i++) {
            zx_handle_close(out_handles[i]);
        }
    }
    return status;
}

static void pdev_i2c_complete(platform_dev_t* dev, pdev_resp_t* resp, const uint8_t* data,
                              size_t actual) {
    pdev_i2c_txn_ctx_t* ctx = &resp->i2c.txn_ctx;

    ctx->complete_cb(resp->status, data, actual, ctx->cookie);
}

static int pdev_proxy_thread(void* arg) {
    platform_dev_t* dev = arg;
    zx_handle_t channel = dev->rpc_channel;

    while (1) {
        struct {
            pdev_resp_t resp;
            uint8_t data[PDEV_I2C_MAX_TRANSFER_SIZE];
        } resp;

        uint32_t length = sizeof(resp);
        zx_signals_t observed;

        zx_status_t status = zx_object_wait_one(channel,
                                                ZX_CHANNEL_READABLE | ZX_CHANNEL_PEER_CLOSED,
                                                ZX_TIME_INFINITE, &observed);
        if (status != ZX_OK) {
            zxlogf(ERROR, "pdev_proxy_thread: zx_object_wait_one failed %d\n", status);
            return status;
        }
        status = zx_channel_read(channel, 0, &resp, NULL, length, 0, &length, NULL);
        if (status != ZX_OK) {
            zxlogf(ERROR, "pdev_proxy_thread: zx_channel_read failed %d\n", status);
            return status;
        }

        switch (resp.resp.txid) {
        case PDEV_I2C_COMPLETE:
            pdev_i2c_complete(dev, &resp.resp, resp.data, length - sizeof(resp.resp));
            break;
        default:
            zxlogf(ERROR, "pdev_proxy_thread: unknown message %u\n", resp.resp.txid);
            break;
        }
    }

    zxlogf(INFO, "pdev_proxy_thread exiting\n");
    return 0;
}

static zx_status_t pdev_ums_get_initial_mode(void* ctx, usb_mode_t* out_mode) {
    platform_dev_t* dev = ctx;
    pdev_req_t req = {
        .op = PDEV_UMS_GET_INITIAL_MODE,
    };
    pdev_resp_t resp;

    zx_status_t status = platform_dev_rpc(dev, &req, &resp, NULL, 0);
    if (status != ZX_OK) {
        return status;
    }
    *out_mode = resp.usb_mode;
    return ZX_OK;
}

static zx_status_t pdev_ums_set_mode(void* ctx, usb_mode_t mode) {
    platform_dev_t* dev = ctx;
    pdev_req_t req = {
        .op = PDEV_UMS_SET_MODE,
        .usb_mode = mode,
    };
    pdev_resp_t resp;

    return platform_dev_rpc(dev, &req, &resp, NULL, 0);
}

usb_mode_switch_protocol_ops_t usb_mode_switch_ops = {
    .get_initial_mode = pdev_ums_get_initial_mode,
    .set_mode = pdev_ums_set_mode,
};

static zx_status_t pdev_gpio_config(void* ctx, uint32_t index, gpio_config_flags_t flags) {
    platform_dev_t* dev = ctx;
    pdev_req_t req = {
        .op = PDEV_GPIO_CONFIG,
        .index = index,
        .gpio_flags = flags,
    };
    pdev_resp_t resp;

    return platform_dev_rpc(dev, &req, &resp, NULL, 0);
}

static zx_status_t pdev_gpio_read(void* ctx, uint32_t index, uint8_t* out_value) {
    platform_dev_t* dev = ctx;
    pdev_req_t req = {
        .op = PDEV_GPIO_READ,
        .index = index,
    };
    pdev_resp_t resp;

    zx_status_t status = platform_dev_rpc(dev, &req, &resp, NULL, 0);
    if (status != ZX_OK) {
        return status;
    }
    *out_value = resp.gpio_value;
    return ZX_OK;
}

static zx_status_t pdev_gpio_write(void* ctx, uint32_t index, uint8_t value) {
    platform_dev_t* dev = ctx;
    pdev_req_t req = {
        .op = PDEV_GPIO_WRITE,
        .index = index,
        .gpio_value = value,
    };
    pdev_resp_t resp;

    return platform_dev_rpc(dev, &req, &resp, NULL, 0);
}

static gpio_protocol_ops_t gpio_ops = {
    .config = pdev_gpio_config,
    .read = pdev_gpio_read,
    .write = pdev_gpio_write,
};

static zx_status_t pdev_i2c_transact(void* ctx, const void* write_buf, size_t write_length,
                                     size_t read_length, i2c_complete_cb complete_cb,
                                     void* cookie) {
    pdev_i2c_channel_ctx_t* channel_ctx = ctx;
    if (!read_length && !write_length) {
        return ZX_ERR_INVALID_ARGS;
    }
    if (write_length > channel_ctx->max_transfer_size ||
        read_length > channel_ctx->max_transfer_size) {
        return ZX_ERR_OUT_OF_RANGE;
    }

    pdev_req_t req = {
        .op = PDEV_I2C_TRANSACT,
        .i2c = {
            .txn_ctx = {
                .write_length = write_length,
                .read_length = read_length,
                .complete_cb = complete_cb,
                .cookie = cookie,
            },
            .server_ctx = channel_ctx->server_ctx,
        },
    };
    pdev_resp_t resp;

    return platform_dev_rpc(channel_ctx->dev, &req, &resp, NULL, 0);
}

static zx_status_t pdev_i2c_set_bitrate(void* ctx, uint32_t bitrate) {
    pdev_i2c_channel_ctx_t* channel_ctx = ctx;
    pdev_req_t req = {
        .op = PDEV_I2C_SET_BITRATE,
        .i2c = {
            .server_ctx = channel_ctx->server_ctx,
            .bitrate = bitrate,
        },
    };
    pdev_resp_t resp;

    return platform_dev_rpc(channel_ctx->dev, &req, &resp, NULL, 0);
}

static zx_status_t pdev_i2c_get_max_transfer_size(void* ctx, size_t* out_size) {
    pdev_i2c_channel_ctx_t* channel_ctx = ctx;
    *out_size = channel_ctx->max_transfer_size;
    return ZX_OK;
}

static void pdev_i2c_channel_release(void* ctx) {
    pdev_i2c_channel_ctx_t* channel_ctx = ctx;
    pdev_req_t req = {
        .op = PDEV_I2C_CHANNEL_RELEASE,
        .i2c = {
            .server_ctx = channel_ctx->server_ctx,
        },
    };
    pdev_resp_t resp;

    platform_dev_rpc(channel_ctx->dev, &req, &resp, NULL, 0);
    free(channel_ctx);
}

static i2c_channel_ops_t pdev_i2c_channel_ops = {
    .transact = pdev_i2c_transact,
    .set_bitrate = pdev_i2c_set_bitrate,
    .channel_release = pdev_i2c_channel_release,
};

static zx_status_t pdev_i2c_get_channel(void* ctx, uint32_t channel_id, i2c_channel_t* channel) {
    platform_dev_t* dev = ctx;
    pdev_i2c_channel_ctx_t* channel_ctx = calloc(1, sizeof(pdev_i2c_channel_ctx_t));
    if (!channel_ctx) {
        return ZX_ERR_NO_MEMORY;
    }

    pdev_req_t req = {
        .op = PDEV_I2C_GET_CHANNEL,
        .index = channel_id,
    };
    pdev_resp_t resp;

    zx_status_t status = platform_dev_rpc(dev, &req, &resp, NULL, 0);
    if (status == ZX_OK) {
        channel_ctx->dev = dev;
        channel_ctx->server_ctx = resp.i2c.server_ctx;
        channel_ctx->max_transfer_size = resp.i2c.max_transfer_size;
        channel->ops = &pdev_i2c_channel_ops;
        channel->ctx = channel_ctx;
    } else {
        free(channel_ctx);
    }

    return status;
}

static zx_status_t pdev_i2c_get_channel_by_address(void* ctx, uint32_t bus_id, uint16_t address,
                                                   i2c_channel_t* channel) {
    return ZX_ERR_NOT_SUPPORTED;
}

static i2c_protocol_ops_t i2c_ops = {
    .get_channel = pdev_i2c_get_channel,
    .get_channel_by_address = pdev_i2c_get_channel_by_address,
};

static zx_status_t platform_dev_get_protocol(void* ctx, uint32_t proto_id, void* out) {
    switch (proto_id) {
    case ZX_PROTOCOL_USB_MODE_SWITCH: {
        usb_mode_switch_protocol_t* proto = out;
        proto->ctx = ctx;
        proto->ops = &usb_mode_switch_ops;
        return ZX_OK;
    }
    case ZX_PROTOCOL_GPIO: {
        gpio_protocol_t* proto = out;
        proto->ctx = ctx;
        proto->ops = &gpio_ops;
        return ZX_OK;
    }
    case ZX_PROTOCOL_I2C: {
        i2c_protocol_t* proto = out;
        proto->ctx = ctx;
        proto->ops = &i2c_ops;
        return ZX_OK;
    }
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }
}

static zx_status_t platform_dev_map_mmio(void* ctx, uint32_t index, uint32_t cache_policy,
                                         void** vaddr, size_t* size, zx_handle_t* out_handle) {
    platform_dev_t* dev = ctx;
    pdev_req_t req = {
        .op = PDEV_GET_MMIO,
        .index = index,
    };
    pdev_resp_t resp;
    zx_handle_t vmo_handle;

    zx_status_t status = platform_dev_rpc(dev, &req, &resp, &vmo_handle, 1);
    if (status != ZX_OK) {
        return status;
    }

    size_t vmo_size;
    status = zx_vmo_get_size(vmo_handle, &vmo_size);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_map_mmio: zx_vmo_get_size failed %d\n", status);
        goto fail;
    }

    status = zx_vmo_set_cache_policy(vmo_handle, cache_policy);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_map_mmio: zx_vmo_set_cache_policy failed %d\n", status);
        goto fail;
    }

    status = zx_vmar_map(zx_vmar_root_self(), 0, vmo_handle, 0, vmo_size,
                         ZX_VM_FLAG_PERM_READ | ZX_VM_FLAG_PERM_WRITE | ZX_VM_FLAG_MAP_RANGE,
                         (uintptr_t*)vaddr);
    if (status != ZX_OK) {
        zxlogf(ERROR, "platform_dev_map_mmio: zx_vmar_map failed %d\n", status);
        goto fail;
    }

    *size = vmo_size;
    *out_handle = vmo_handle;
    return ZX_OK;

fail:
    zx_handle_close(vmo_handle);
    return status;
}

static zx_status_t platform_dev_map_interrupt(void* ctx, uint32_t index, zx_handle_t* out_handle) {
    platform_dev_t* dev = ctx;
    pdev_req_t req = {
        .op = PDEV_GET_INTERRUPT,
        .index = index,
    };
    pdev_resp_t resp;

    return platform_dev_rpc(dev, &req, &resp, out_handle, 1);
}

static platform_device_protocol_ops_t platform_dev_proto_ops = {
    .get_protocol = platform_dev_get_protocol,
    .map_mmio = platform_dev_map_mmio,
    .map_interrupt = platform_dev_map_interrupt,
};

static void platform_dev_release(void* ctx) {
    platform_dev_t* dev = ctx;

    zx_handle_close(dev->rpc_channel);
    free(dev);
}

static zx_protocol_device_t platform_dev_proto = {
    .version = DEVICE_OPS_VERSION,
    .release = platform_dev_release,
};

zx_status_t platform_proxy_create(void* ctx, zx_device_t* parent, const char* name,
                                  const char* args, zx_handle_t rpc_channel) {
    platform_dev_t* dev = calloc(1, sizeof(platform_dev_t));
    if (!dev) {
        return ZX_ERR_NO_MEMORY;
    }
    dev->rpc_channel = rpc_channel;

    device_add_args_t add_args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = name,
        .ctx = dev,
        .ops = &platform_dev_proto,
        .proto_id = ZX_PROTOCOL_PLATFORM_DEV,
        .proto_ops = &platform_dev_proto_ops,
    };

    zx_status_t status = device_add(parent, &add_args, &dev->zxdev);
    if (status != ZX_OK) {
        zx_handle_close(rpc_channel);
        free(dev);
    }

    thrd_create_with_name(&dev->proxy_thread, pdev_proxy_thread, dev, "pdev_proxy_thread");
    // proxy thread will exit when it gets ZX_ERR_PEER_CLOSED
    thrd_detach(dev->proxy_thread);

    return status;
}

static zx_driver_ops_t platform_bus_proxy_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .create = platform_proxy_create,
};

ZIRCON_DRIVER_BEGIN(platform_bus_proxy, platform_bus_proxy_driver_ops, "zircon", "0.1", 1)
    // devmgr loads us directly, so we need no binding information here
    BI_ABORT_IF_AUTOBIND,
ZIRCON_DRIVER_END(platform_bus_proxy)
