// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Standard Includes
#include <endian.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <threads.h>

// DDK Includes
#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/iotxn.h>
#include <ddk/debug.h>
#include <ddk/protocol/sdmmc.h>

// Zircon Includes
#include <sync/completion.h>
#include <pretty/hexdump.h>
#include <zircon/threads.h>
#include <zircon/device/block.h>

#include "sdmmc.h"

// Various transfer states that the card can be in.
#define SDMMC_STATE_TRAN 0x4
#define SDMMC_STATE_RECV 0x5
#define SDMMC_STATE_DATA 0x6

#define SDMMC_IOTXN_RECEIVED    ZX_EVENT_SIGNALED
#define SDMMC_SHUTDOWN          ZX_USER_SIGNAL_0
#define SDMMC_SHUTDOWN_DONE     ZX_USER_SIGNAL_1

static void sdmmc_txn_cplt(iotxn_t* request, void* cookie) {
    completion_signal((completion_t*)cookie);
};

zx_status_t sdmmc_do_command(zx_device_t* dev, const uint32_t cmd,
                                    const uint32_t arg, iotxn_t* txn) {
    sdmmc_protocol_data_t* pdata = iotxn_pdata(txn, sdmmc_protocol_data_t);
    pdata->cmd = cmd;
    pdata->arg = arg;

    completion_t cplt = COMPLETION_INIT;
    txn->complete_cb = sdmmc_txn_cplt;
    txn->cookie = &cplt;

    iotxn_queue(dev, txn);

    completion_wait(&cplt, ZX_TIME_INFINITE);

    return txn->status;
}

static zx_off_t sdmmc_get_size(void* ctx) {
    sdmmc_t* sdmmc = ctx;
    return sdmmc->capacity;
}

static void sdmmc_get_info(block_info_t* info, void* ctx) {
    sdmmc_t* sdmmc = ctx;
    memset(info, 0, sizeof(*info));
    // Since we only support SDHC cards, the blocksize must be the SDHC
    // blocksize.
    info->block_size = SDHC_BLOCK_SIZE;
    info->block_count = sdmmc_get_size(ctx) / SDHC_BLOCK_SIZE;
    info->max_transfer_size = sdmmc->max_transfer_size;
}

static zx_status_t sdmmc_ioctl(void* ctx, uint32_t op, const void* cmd,
                               size_t cmdlen, void* reply, size_t max, size_t* out_actual) {
    switch (op) {
    case IOCTL_BLOCK_GET_INFO: {
        block_info_t* info = reply;
        if (max < sizeof(*info))
            return ZX_ERR_BUFFER_TOO_SMALL;
        sdmmc_get_info(info, ctx);
        *out_actual = sizeof(*info);
        return ZX_OK;
    }
    case IOCTL_BLOCK_RR_PART: {
        sdmmc_t* sdmmc = ctx;
        return device_rebind(sdmmc->zxdev);
    }
    case IOCTL_BLOCK_GET_NAME: {
        return ZX_ERR_NOT_SUPPORTED;
    }
    case IOCTL_DEVICE_SYNC: {
        iotxn_t* txn;
        zx_status_t status = iotxn_alloc(&txn, 0, 0);
        if (status != ZX_OK) {
            return status;
        }
        completion_t completion = COMPLETION_INIT;
        txn->opcode = IOTXN_OP_READ;
        txn->flags = IOTXN_SYNC_BEFORE;
        txn->offset = 0;
        txn->length = 0;
        txn->complete_cb = sdmmc_txn_cplt;
        txn->cookie = &completion;
        sdmmc_t* device = ctx;
        iotxn_queue(device->zxdev, txn);
        completion_wait(&completion, ZX_TIME_INFINITE);
        status = txn->status;
        iotxn_release(txn);
        return status;
    }
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }
    return 0;
}

static void sdmmc_unbind(void* ctx) {
    sdmmc_t* sdmmc = ctx;
    device_remove(sdmmc->zxdev);
}

static void sdmmc_release(void* ctx) {
    sdmmc_t* sdmmc = ctx;

    if (sdmmc->worker_thread_running) {
        zx_object_signal(sdmmc->worker_event, 0, SDMMC_SHUTDOWN);
        zx_object_wait_one(sdmmc->worker_event, SDMMC_SHUTDOWN_DONE, ZX_TIME_INFINITE, NULL);

        mtx_lock(&sdmmc->lock);
        iotxn_t* txn;
        list_for_every_entry(&sdmmc->txn_list, txn, iotxn_t, node) {
            mtx_unlock(&sdmmc->lock);
            iotxn_complete(txn, ZX_ERR_BAD_STATE, 0);
            mtx_lock(&sdmmc->lock);
        }
        mtx_unlock(&sdmmc->lock);

        thrd_join(sdmmc->worker_thread, NULL);
    }

    if (sdmmc->worker_event != ZX_HANDLE_INVALID) {
        zx_handle_close(sdmmc->worker_event);
    }

    free(sdmmc);
}

static void sdmmc_iotxn_queue(void* ctx, iotxn_t* txn) {
    zxlogf(SPEW, "sdmmc: iotxn_queue txn %p offset 0x%" PRIx64
                   " length 0x%" PRIx64 "\n", txn, txn->offset, txn->length);

    if (txn->offset % SDHC_BLOCK_SIZE) {
        zxlogf(ERROR, "sdmmc: iotxn offset not aligned to block boundary, "
                "offset =%" PRIu64 ", block size = %d\n",
                txn->offset, SDHC_BLOCK_SIZE);
        iotxn_complete(txn, ZX_ERR_INVALID_ARGS, 0);
        return;
    }

    if (txn->length % SDHC_BLOCK_SIZE) {
        zxlogf(ERROR, "sdmmc: iotxn length not aligned to block boundary, "
                "offset =%" PRIu64 ", block size = %d\n",
                txn->length, SDHC_BLOCK_SIZE);
        iotxn_complete(txn, ZX_ERR_INVALID_ARGS, 0);
        return;
    }

    sdmmc_t* sdmmc = ctx;

    mtx_lock(&sdmmc->lock);
    list_add_tail(&sdmmc->txn_list, &txn->node);
    mtx_unlock(&sdmmc->lock);

    // Wake up the worker thread
    zx_object_signal(sdmmc->worker_event, 0, SDMMC_IOTXN_RECEIVED);
}

// Block device protocol.
static zx_protocol_device_t sdmmc_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .ioctl = sdmmc_ioctl,
    .unbind = sdmmc_unbind,
    .release = sdmmc_release,
    .iotxn_queue = sdmmc_iotxn_queue,
    .get_size = sdmmc_get_size,
};

static void sdmmc_do_txn(sdmmc_t* sdmmc, iotxn_t* txn) {
    zxlogf(SPEW, "sdmmc: do_txn txn %p offset 0x%" PRIx64
                   " length 0x%" PRIx64 "\n", txn, txn->offset, txn->length);

    zx_device_t* sdmmc_zxdev = sdmmc->host_zxdev;
    uint32_t cmd = 0;

    // Figure out which SD command we need to issue.
    switch(txn->opcode) {
        case IOTXN_OP_READ:
            if (txn->length > SDHC_BLOCK_SIZE) {
                cmd = SDMMC_READ_MULTIPLE_BLOCK;
            } else {
                cmd = SDMMC_READ_BLOCK;
            }
            break;
        case IOTXN_OP_WRITE:
            if (txn->length > SDHC_BLOCK_SIZE) {
                cmd = SDMMC_WRITE_MULTIPLE_BLOCK;
            } else {
                cmd = SDMMC_WRITE_BLOCK;
            }
            break;
        default:
            // Invalid opcode?
            zxlogf(SPEW, "sdmmc: iotxn_complete txn %p status %d\n", txn, ZX_ERR_INVALID_ARGS);
            iotxn_complete(txn, ZX_ERR_INVALID_ARGS, 0);
            return;
    }

    iotxn_t* clone = NULL;
    zx_status_t st = iotxn_clone(txn, &clone);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdmmc: err %d cloning iotxn\n", st);
        iotxn_complete(txn, st, 0);
        return;
    }

    clone->protocol = ZX_PROTOCOL_SDMMC;
    sdmmc_protocol_data_t* pdata = iotxn_pdata(clone, sdmmc_protocol_data_t);

    // Following commands do not use the data buffer and
    // it is safe to use the cloned iotxn

    uint8_t current_state;
    const size_t max_attempts = 10;
    size_t attempt = 0;
    for (; attempt <= max_attempts; attempt++) {
        st = sdmmc_do_command(sdmmc_zxdev, SDMMC_SEND_STATUS, sdmmc->rca << 16, clone);
        if (st != ZX_OK) {
            zxlogf(SPEW, "sdmmc: iotxn_complete txn %p status %d (SDMMC_SEND_STATUS)\n",
                    txn, st);
            iotxn_complete(txn, st, 0);
            goto out;
        }

        current_state = (pdata->response[0] >> 9) & 0xf;

        if (current_state == SDMMC_STATE_RECV) {
            st = sdmmc_do_command(sdmmc_zxdev, SDMMC_STOP_TRANSMISSION, 0, clone);
            continue;
        } else if (current_state == SDMMC_STATE_TRAN) {
            break;
        }

        zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));
    }

    if (attempt == max_attempts) {
        // Too many retries, fail.
        zxlogf(SPEW, "sdmmc: iotxn_complete txn %p status %d\n", txn, ZX_ERR_BAD_STATE);
        iotxn_complete(txn, ZX_ERR_BAD_STATE, 0);
        goto out;
    }

    // Issue the data transfer

    const uint32_t blkid = clone->offset / SDHC_BLOCK_SIZE;
    pdata->blockcount = clone->length / SDHC_BLOCK_SIZE;
    pdata->blocksize = SDHC_BLOCK_SIZE;

    st = sdmmc_do_command(sdmmc_zxdev, cmd, blkid, clone);
    if (st != ZX_OK) {
        zxlogf(SPEW, "sdmmc: iotxn_complete txn %p status %d (cmd 0x%x)\n", txn, st, cmd);
        iotxn_complete(txn, st, 0);
        goto out;
    }

    zxlogf(SPEW, "sdmmc: iotxn_complete txn %p status %d\n", txn, ZX_OK);
    iotxn_complete(txn, ZX_OK, txn->length);

out:
    if (clone) {
        iotxn_release(clone);
    }
}

static int sdmmc_worker_thread(void* arg) {
    zx_status_t st = ZX_OK;
    sdmmc_t* sdmmc = (sdmmc_t*)arg;

    // TODO: get max transfer size from host
    sdmmc->max_transfer_size = 32 * 1024 * 1024; // 32M

    iotxn_t* setup_txn = NULL;
    // Allocate a single iotxn that we use to bootstrap the card with.
    static_assert(SDHC_BLOCK_SIZE <= PAGE_SIZE, "");
    if ((st = iotxn_alloc(&setup_txn, 0, SDHC_BLOCK_SIZE)) != ZX_OK) {
        zxlogf(ERROR, "sdmmc: failed to allocate iotxn for setup, rc = %d\n", st);
        device_remove(sdmmc->zxdev);
        return st;
    }

    // Reset the card.
    sdmmc_hw_reset(&sdmmc->host);

    // No matter what state the card is in, issuing the GO_IDLE_STATE command will
    // put the card into the idle state.
    if ((st = sdmmc_do_command(sdmmc->host_zxdev, SDMMC_GO_IDLE_STATE, 0, setup_txn)) != ZX_OK) {
        zxlogf(ERROR, "sdmmc: SDMMC_GO_IDLE_STATE failed, retcode = %d\n", st);
        iotxn_release(setup_txn);
        device_remove(sdmmc->zxdev);
        return st;
    }

    // Probe for SD, then MMC
    if ((st = sdmmc_probe_sd(sdmmc, setup_txn)) != ZX_OK) {
        if ((st = sdmmc_probe_mmc(sdmmc, setup_txn)) != ZX_OK) {
            zxlogf(ERROR, "sdmmc: failed to probe\n");
            iotxn_release(setup_txn);
            device_remove(sdmmc->zxdev);
            return st;
        }
    }

    iotxn_release(setup_txn);

    device_make_visible(sdmmc->zxdev);

    for (;;) {
        // don't loop until txn_list is empty to check for SDMMC_SHUTDOWN
        // between each txn.
        mtx_lock(&sdmmc->lock);
        iotxn_t* txn = list_remove_head_type(&sdmmc->txn_list, iotxn_t, node);
        mtx_unlock(&sdmmc->lock);
        if (txn) {
            sdmmc_do_txn(sdmmc, txn);
        } else {
            zx_object_signal(sdmmc->worker_event, SDMMC_IOTXN_RECEIVED, 0);
        }

        uint32_t pending;
        zx_status_t st = zx_object_wait_one(sdmmc->worker_event,
                SDMMC_IOTXN_RECEIVED | SDMMC_SHUTDOWN, ZX_TIME_INFINITE, &pending);
        if (st != ZX_OK) {
            zxlogf(ERROR, "sdmmc: worker thread wait failed, retcode = %d\n", st);
            break;
        }
        if (pending & SDMMC_SHUTDOWN) {
            zx_object_signal(sdmmc->worker_event, pending, SDMMC_SHUTDOWN_DONE);
            break;
        }
    }

    zxlogf(TRACE, "sdmmc: worker thread terminated\n");

    return 0;
}

static zx_status_t sdmmc_bind(void* ctx, zx_device_t* dev) {
    // Allocate the device.
    sdmmc_t* sdmmc = calloc(1, sizeof(*sdmmc));
    if (!sdmmc) {
        zxlogf(ERROR, "sdmmc: no memory to allocate sdmmc device!\n");
        return ZX_ERR_NO_MEMORY;
    }

    if (device_get_protocol(dev, ZX_PROTOCOL_SDMMC, &sdmmc->host)) {
        zxlogf(ERROR, "sdmmc: failed to get sdmmc protocol\n");
        free(sdmmc);
        return ZX_ERR_NOT_SUPPORTED;
    }

    sdmmc->host_zxdev = dev;
    mtx_init(&sdmmc->lock, mtx_plain);
    list_initialize(&sdmmc->txn_list);

    zx_status_t st = zx_event_create(0, &sdmmc->worker_event);
    if (st != ZX_OK) {
        zxlogf(ERROR, "sdmmc: failed to create event, retcode = %d\n", st);
        free(sdmmc);
        return st;
    }

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = (sdmmc->type == SDMMC_TYPE_SD) ? "sd" : "mmc",
        .ctx = sdmmc,
        .ops = &sdmmc_device_proto,
        .proto_id = ZX_PROTOCOL_BLOCK_CORE,
        .flags = DEVICE_ADD_INVISIBLE,
    };

    st = device_add(dev, &args, &sdmmc->zxdev);
    if (st != ZX_OK) {
        free(sdmmc);
        return st;
    }

    // bootstrap in a thread
    int rc = thrd_create_with_name(&sdmmc->worker_thread, sdmmc_worker_thread, sdmmc,
                                   "sdmmc-worker");
    if (rc != thrd_success) {
        device_remove(sdmmc->zxdev);
        return thrd_status_to_zx_status(rc);
    }
    sdmmc->worker_thread_running = true;

    return ZX_OK;
}

static zx_driver_ops_t sdmmc_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = sdmmc_bind,
};

// The formatter does not play nice with these macros.
// clang-format off
ZIRCON_DRIVER_BEGIN(sdmmc, sdmmc_driver_ops, "zircon", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, ZX_PROTOCOL_SDMMC),
ZIRCON_DRIVER_END(sdmmc)
// clang-format on
