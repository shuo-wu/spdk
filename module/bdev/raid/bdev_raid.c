/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2018 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022, NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "bdev_raid.h"
#include "spdk/env.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/json.h"
#include "spdk/likely.h"

static bool g_shutdown_started = false;

/* List of all raid bdevs */
struct raid_all_tailq g_raid_bdev_list = TAILQ_HEAD_INITIALIZER(g_raid_bdev_list);

static TAILQ_HEAD(, raid_bdev_module) g_raid_modules = TAILQ_HEAD_INITIALIZER(g_raid_modules);

/*
 * raid_bdev_io_channel is the context of spdk_io_channel for raid bdev device. It
 * contains the relationship of raid bdev io channel with base bdev io channels.
 */
struct raid_bdev_io_channel {
	/* Array of IO channels of base bdevs */
	struct spdk_io_channel	**base_channel;

	/* Private raid module IO channel */
	struct spdk_io_channel	*module_channel;
};

static struct raid_bdev_module *
raid_bdev_module_find(enum raid_level level)
{
	struct raid_bdev_module *raid_module;

	TAILQ_FOREACH(raid_module, &g_raid_modules, link) {
		if (raid_module->level == level) {
			return raid_module;
		}
	}

	return NULL;
}

void
raid_bdev_module_list_add(struct raid_bdev_module *raid_module)
{
	if (raid_bdev_module_find(raid_module->level) != NULL) {
		SPDK_ERRLOG("module for raid level '%s' already registered.\n",
			    raid_bdev_level_to_str(raid_module->level));
		assert(false);
	} else {
		TAILQ_INSERT_TAIL(&g_raid_modules, raid_module, link);
	}
}

struct spdk_io_channel *
raid_bdev_channel_get_base_channel(struct raid_bdev_io_channel *raid_ch, uint8_t idx)
{
	return raid_ch->base_channel[idx];
}

void *
raid_bdev_channel_get_module_ctx(struct raid_bdev_io_channel *raid_ch)
{
	assert(raid_ch->module_channel != NULL);

	return spdk_io_channel_get_ctx(raid_ch->module_channel);
}

/* Function declarations */
static void	raid_bdev_examine(struct spdk_bdev *bdev);
static int	raid_bdev_init(void);
static void	raid_bdev_deconfigure(struct raid_bdev *raid_bdev,
				      raid_bdev_destruct_cb cb_fn, void *cb_arg);

/*
 * brief:
 * raid_bdev_create_cb function is a cb function for raid bdev which creates the
 * hierarchy from raid bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to raid bdev io device represented by raid_bdev
 * ctx_buf - pointer to context buffer for raid bdev io channel
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_create_cb(void *io_device, void *ctx_buf)
{
	struct raid_bdev            *raid_bdev = io_device;
	struct raid_bdev_io_channel *raid_ch = ctx_buf;
	uint8_t i;
	int ret = 0;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_create_cb, %p\n", raid_ch);

	assert(raid_bdev != NULL);
	assert(raid_bdev->state == RAID_BDEV_STATE_ONLINE);


	raid_ch->base_channel = calloc(raid_bdev->num_base_bdevs, sizeof(struct spdk_io_channel *));
	if (!raid_ch->base_channel) {
		SPDK_ERRLOG("Unable to allocate base bdevs io channel\n");
		return -ENOMEM;
	}

	spdk_spin_lock(&raid_bdev->base_bdev_lock);
	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/*
		 * Get the spdk_io_channel for all the base bdevs. This is used during
		 * split logic to send the respective child bdev ios to respective base
		 * bdev io channel.
		 */
		if (raid_bdev->base_bdev_info[i].desc == NULL) {
			continue;
		}
		raid_ch->base_channel[i] = spdk_bdev_get_io_channel(
						   raid_bdev->base_bdev_info[i].desc);
		if (!raid_ch->base_channel[i]) {
			SPDK_ERRLOG("Unable to create io channel for base bdev\n");
			ret = -ENOMEM;
			break;
		}
	}
	spdk_spin_unlock(&raid_bdev->base_bdev_lock);

	if (!ret && raid_bdev->module->get_io_channel) {
		raid_ch->module_channel = raid_bdev->module->get_io_channel(raid_bdev);
		if (!raid_ch->module_channel) {
			SPDK_ERRLOG("Unable to create io channel for raid module\n");
			ret = -ENOMEM;
		}
	}

	if (ret) {
		for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
			if (raid_ch->base_channel[i] != NULL) {
				spdk_put_io_channel(raid_ch->base_channel[i]);
			}
		}
		free(raid_ch->base_channel);
		raid_ch->base_channel = NULL;
	}
	return ret;
}

/*
 * brief:
 * raid_bdev_destroy_cb function is a cb function for raid bdev which deletes the
 * hierarchy from raid bdev to base bdev io channels. It will be called per core
 * params:
 * io_device - pointer to raid bdev io device represented by raid_bdev
 * ctx_buf - pointer to context buffer for raid bdev io channel
 * returns:
 * none
 */
static void
raid_bdev_destroy_cb(void *io_device, void *ctx_buf)
{
	struct raid_bdev *raid_bdev = io_device;
	struct raid_bdev_io_channel *raid_ch = ctx_buf;
	uint8_t i;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_destroy_cb\n");

	assert(raid_ch != NULL);
	assert(raid_ch->base_channel);

	if (raid_ch->module_channel) {
		spdk_put_io_channel(raid_ch->module_channel);
	}

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		/* Free base bdev channels */
		if (raid_ch->base_channel[i] != NULL) {
			spdk_put_io_channel(raid_ch->base_channel[i]);
		}
	}
	free(raid_ch->base_channel);
	raid_ch->base_channel = NULL;
}

/*
 * brief:
 * raid_bdev_cleanup is used to cleanup raid_bdev related data
 * structures.
 * params:
 * raid_bdev - pointer to raid_bdev
 * returns:
 * none
 */
static void
raid_bdev_cleanup(struct raid_bdev *raid_bdev)
{
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_cleanup, %p name %s, state %s\n",
		      raid_bdev, raid_bdev->bdev.name, raid_bdev_state_to_str(raid_bdev->state));
	assert(raid_bdev->state != RAID_BDEV_STATE_ONLINE);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		assert(base_info->desc == NULL);
		free(base_info->name);
	}

	TAILQ_REMOVE(&g_raid_bdev_list, raid_bdev, global_link);
}

static void
raid_bdev_free(struct raid_bdev *raid_bdev)
{
	spdk_dma_free(raid_bdev->sb);
	spdk_spin_destroy(&raid_bdev->base_bdev_lock);
	free(raid_bdev->base_bdev_info);
	free(raid_bdev->bdev.name);
	free(raid_bdev);
}

static void
raid_bdev_cleanup_and_free(struct raid_bdev *raid_bdev)
{
	raid_bdev_cleanup(raid_bdev);
	raid_bdev_free(raid_bdev);
}

/*
 * brief:
 * free resource of base bdev for raid bdev
 * params:
 * base_info - raid base bdev info
 * returns:
 * none
 */
static void
raid_bdev_free_base_bdev_resource(struct raid_base_bdev_info *base_info)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	free(base_info->name);
	base_info->name = NULL;
	if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
		spdk_uuid_set_null(&base_info->uuid);
	}

	if (base_info->desc == NULL) {
		return;
	}

	spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(base_info->desc));
	spdk_bdev_close(base_info->desc);
	base_info->desc = NULL;
	spdk_put_io_channel(base_info->app_thread_ch);
	base_info->app_thread_ch = NULL;

	if (base_info->is_configured) {
		assert(raid_bdev->num_base_bdevs_discovered);
		raid_bdev->num_base_bdevs_discovered--;
		base_info->is_configured = false;
	}
}

static void
raid_bdev_io_device_unregister_cb(void *io_device)
{
	struct raid_bdev *raid_bdev = io_device;

	if (raid_bdev->num_base_bdevs_discovered == 0) {
		/* Free raid_bdev when there are no base bdevs left */
		SPDK_DEBUGLOG(bdev_raid, "raid bdev base bdevs is 0, going to free all in destruct\n");
		raid_bdev_cleanup(raid_bdev);
		spdk_bdev_destruct_done(&raid_bdev->bdev, 0);
		raid_bdev_free(raid_bdev);
	} else {
		spdk_bdev_destruct_done(&raid_bdev->bdev, 0);
	}
}

void
raid_bdev_module_stop_done(struct raid_bdev *raid_bdev)
{
	if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
		spdk_io_device_unregister(raid_bdev, raid_bdev_io_device_unregister_cb);
	}
}

static void
_raid_bdev_destruct(void *ctxt)
{
	struct raid_bdev *raid_bdev = ctxt;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_destruct\n");

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		/*
		 * Close all base bdev descriptors for which call has come from below
		 * layers.  Also close the descriptors if we have started shutdown.
		 */
		if (g_shutdown_started || base_info->remove_scheduled == true) {
			raid_bdev_free_base_bdev_resource(base_info);
		}
	}

	if (g_shutdown_started) {
		raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
	}

	if (raid_bdev->module->stop != NULL) {
		if (raid_bdev->module->stop(raid_bdev) == false) {
			return;
		}
	}

	raid_bdev_module_stop_done(raid_bdev);
}

static int
raid_bdev_destruct(void *ctx)
{
	spdk_thread_exec_msg(spdk_thread_get_app_thread(), _raid_bdev_destruct, ctx);

	return 1;
}

void
raid_bdev_io_complete(struct raid_bdev_io *raid_io, enum spdk_bdev_io_status status)
{
	struct spdk_bdev_io *bdev_io = spdk_bdev_io_from_ctx(raid_io);

	if (spdk_unlikely(raid_io->completion_cb != NULL)) {
		raid_io->completion_cb(raid_io, status);
	} else {
		spdk_bdev_io_complete(bdev_io, status);
	}
}

/*
 * brief:
 * raid_bdev_io_complete_part - signal the completion of a part of the expected
 * base bdev IOs and complete the raid_io if this is the final expected IO.
 * The caller should first set raid_io->base_bdev_io_remaining. This function
 * will decrement this counter by the value of the 'completed' parameter and
 * complete the raid_io if the counter reaches 0. The caller is free to
 * interpret the 'base_bdev_io_remaining' and 'completed' values as needed,
 * it can represent e.g. blocks or IOs.
 * params:
 * raid_io - pointer to raid_bdev_io
 * completed - the part of the raid_io that has been completed
 * status - status of the base IO
 * returns:
 * true - if the raid_io is completed
 * false - otherwise
 */
bool
raid_bdev_io_complete_part(struct raid_bdev_io *raid_io, uint64_t completed,
			   enum spdk_bdev_io_status status)
{
	assert(raid_io->base_bdev_io_remaining >= completed);
	raid_io->base_bdev_io_remaining -= completed;

	if (status != SPDK_BDEV_IO_STATUS_SUCCESS) {
		raid_io->base_bdev_io_status = status;
	}

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_bdev_io_complete(raid_io, raid_io->base_bdev_io_status);
		return true;
	} else {
		return false;
	}
}

/*
 * brief:
 * raid_bdev_queue_io_wait function processes the IO which failed to submit.
 * It will try to queue the IOs after storing the context to bdev wait queue logic.
 * params:
 * raid_io - pointer to raid_bdev_io
 * bdev - the block device that the IO is submitted to
 * ch - io channel
 * cb_fn - callback when the spdk_bdev_io for bdev becomes available
 * returns:
 * none
 */
void
raid_bdev_queue_io_wait(struct raid_bdev_io *raid_io, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn)
{
	raid_io->waitq_entry.bdev = bdev;
	raid_io->waitq_entry.cb_fn = cb_fn;
	raid_io->waitq_entry.cb_arg = raid_io;
	spdk_bdev_queue_io_wait(bdev, ch, &raid_io->waitq_entry);
}

static void
raid_base_bdev_reset_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct raid_bdev_io *raid_io = cb_arg;

	spdk_bdev_free_io(bdev_io);

	raid_bdev_io_complete_part(raid_io, 1, success ?
				   SPDK_BDEV_IO_STATUS_SUCCESS :
				   SPDK_BDEV_IO_STATUS_FAILED);
}

static void raid_bdev_submit_reset_request(struct raid_bdev_io *raid_io);

static void
_raid_bdev_submit_reset_request(void *_raid_io)
{
	struct raid_bdev_io *raid_io = _raid_io;

	raid_bdev_submit_reset_request(raid_io);
}

/*
 * brief:
 * raid_bdev_submit_reset_request function submits reset requests
 * to member disks; it will submit as many as possible unless a reset fails with -ENOMEM, in
 * which case it will queue it for later submission
 * params:
 * raid_io
 * returns:
 * none
 */
static void
raid_bdev_submit_reset_request(struct raid_bdev_io *raid_io)
{
	struct raid_bdev		*raid_bdev;
	int				ret;
	uint8_t				i;
	struct raid_base_bdev_info	*base_info;
	struct spdk_io_channel		*base_ch;

	raid_bdev = raid_io->raid_bdev;

	if (raid_io->base_bdev_io_remaining == 0) {
		raid_io->base_bdev_io_remaining = raid_bdev->num_base_bdevs;
	}

	for (i = raid_io->base_bdev_io_submitted; i < raid_bdev->num_base_bdevs; i++) {
		base_info = &raid_bdev->base_bdev_info[i];
		base_ch = raid_io->raid_ch->base_channel[i];
		if (base_ch == NULL) {
			raid_io->base_bdev_io_submitted++;
			raid_bdev_io_complete_part(raid_io, 1, SPDK_BDEV_IO_STATUS_SUCCESS);
			continue;
		}
		ret = spdk_bdev_reset(base_info->desc, base_ch,
				      raid_base_bdev_reset_complete, raid_io);
		if (ret == 0) {
			raid_io->base_bdev_io_submitted++;
		} else if (ret == -ENOMEM) {
			raid_bdev_queue_io_wait(raid_io, spdk_bdev_desc_get_bdev(base_info->desc),
						base_ch, _raid_bdev_submit_reset_request);
			return;
		} else {
			SPDK_ERRLOG("bdev io submit error not due to ENOMEM, it should not happen\n");
			assert(false);
			raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
	}
}

/*
 * brief:
 * Callback function to spdk_bdev_io_get_buf.
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * success - True if buffer is allocated or false otherwise.
 * returns:
 * none
 */
static void
raid_bdev_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		     bool success)
{
	struct raid_bdev_io *raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;

	if (!success) {
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	raid_io->raid_bdev->module->submit_rw_request(raid_io);
}

void
raid_bdev_io_init(struct raid_bdev_io *raid_io, struct raid_bdev_io_channel *raid_ch,
		  enum spdk_bdev_io_type type, uint64_t offset_blocks,
		  uint64_t num_blocks, struct iovec *iovs, int iovcnt, void *md_buf,
		  struct spdk_memory_domain *memory_domain, void *memory_domain_ctx)
{
	struct spdk_io_channel *ch = spdk_io_channel_from_ctx(raid_ch);
	struct raid_bdev *raid_bdev = spdk_io_channel_get_io_device(ch);

	raid_io->type = type;
	raid_io->offset_blocks = offset_blocks;
	raid_io->num_blocks = num_blocks;
	raid_io->iovs = iovs;
	raid_io->iovcnt = iovcnt;
	raid_io->memory_domain = memory_domain;
	raid_io->memory_domain_ctx = memory_domain_ctx;
	raid_io->md_buf = md_buf;

	raid_io->raid_bdev = raid_bdev;
	raid_io->raid_ch = raid_ch;
	raid_io->base_bdev_io_remaining = 0;
	raid_io->base_bdev_io_submitted = 0;
	raid_io->base_bdev_io_status = SPDK_BDEV_IO_STATUS_SUCCESS;
	raid_io->completion_cb = NULL;
}

/*
 * brief:
 * raid_bdev_submit_request function is the submit_request function pointer of
 * raid bdev function table. This is used to submit the io on raid_bdev to below
 * layers.
 * params:
 * ch - pointer to raid bdev io channel
 * bdev_io - pointer to parent bdev_io on raid bdev device
 * returns:
 * none
 */
static void
raid_bdev_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct raid_bdev_io *raid_io = (struct raid_bdev_io *)bdev_io->driver_ctx;

	raid_bdev_io_init(raid_io, spdk_io_channel_get_ctx(ch), bdev_io->type,
			  bdev_io->u.bdev.offset_blocks, bdev_io->u.bdev.num_blocks,
			  bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt, bdev_io->u.bdev.md_buf,
			  bdev_io->u.bdev.memory_domain, bdev_io->u.bdev.memory_domain_ctx);

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, raid_bdev_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		raid_io->raid_bdev->module->submit_rw_request(raid_io);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		raid_bdev_submit_reset_request(raid_io);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		raid_io->raid_bdev->module->submit_null_payload_request(raid_io);
		break;

	default:
		SPDK_ERRLOG("submit request, invalid io type %u\n", bdev_io->type);
		raid_bdev_io_complete(raid_io, SPDK_BDEV_IO_STATUS_FAILED);
		break;
	}
}

/*
 * brief:
 * _raid_bdev_io_type_supported checks whether io_type is supported in
 * all base bdev modules of raid bdev module. If anyone among the base_bdevs
 * doesn't support, the raid device doesn't supports.
 *
 * params:
 * raid_bdev - pointer to raid bdev context
 * io_type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
inline static bool
_raid_bdev_io_type_supported(struct raid_bdev *raid_bdev, enum spdk_bdev_io_type io_type)
{
	struct raid_base_bdev_info *base_info;

	if (io_type == SPDK_BDEV_IO_TYPE_FLUSH ||
	    io_type == SPDK_BDEV_IO_TYPE_UNMAP) {
		if (raid_bdev->module->submit_null_payload_request == NULL) {
			return false;
		}
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->desc == NULL) {
			continue;
		}

		if (spdk_bdev_io_type_supported(spdk_bdev_desc_get_bdev(base_info->desc), io_type) == false) {
			return false;
		}
	}

	return true;
}

/*
 * brief:
 * raid_bdev_io_type_supported is the io_supported function for bdev function
 * table which returns whether the particular io type is supported or not by
 * raid bdev module
 * params:
 * ctx - pointer to raid bdev context
 * type - io type
 * returns:
 * true - io_type is supported
 * false - io_type is not supported
 */
static bool
raid_bdev_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
		return true;

	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return _raid_bdev_io_type_supported(ctx, io_type);

	default:
		return false;
	}

	return false;
}

/*
 * brief:
 * raid_bdev_get_io_channel is the get_io_channel function table pointer for
 * raid bdev. This is used to return the io channel for this raid bdev
 * params:
 * ctxt - pointer to raid_bdev
 * returns:
 * pointer to io channel for raid bdev
 */
static struct spdk_io_channel *
raid_bdev_get_io_channel(void *ctxt)
{
	struct raid_bdev *raid_bdev = ctxt;

	return spdk_get_io_channel(raid_bdev);
}

void
raid_bdev_write_info_json(struct raid_bdev *raid_bdev, struct spdk_json_write_ctx *w)
{
	struct raid_base_bdev_info *base_info;
	char uuid_str[SPDK_UUID_STRING_LEN];

	assert(raid_bdev != NULL);
	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &raid_bdev->bdev.uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_named_uint32(w, "strip_size_kb", raid_bdev->strip_size_kb);
	spdk_json_write_named_string(w, "state", raid_bdev_state_to_str(raid_bdev->state));
	spdk_json_write_named_string(w, "raid_level", raid_bdev_level_to_str(raid_bdev->level));
	spdk_json_write_named_bool(w, "superblock", raid_bdev->sb != NULL);
	spdk_json_write_named_uint32(w, "num_base_bdevs", raid_bdev->num_base_bdevs);
	spdk_json_write_named_uint32(w, "num_base_bdevs_discovered", raid_bdev->num_base_bdevs_discovered);
	spdk_json_write_named_uint32(w, "num_base_bdevs_operational",
				     raid_bdev->num_base_bdevs_operational);
	spdk_json_write_name(w, "base_bdevs_list");
	spdk_json_write_array_begin(w);
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		spdk_json_write_object_begin(w);
		spdk_json_write_name(w, "name");
		if (base_info->name) {
			spdk_json_write_string(w, base_info->name);
		} else {
			spdk_json_write_null(w);
		}
		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &base_info->uuid);
		spdk_json_write_named_string(w, "uuid", uuid_str);
		spdk_json_write_named_bool(w, "is_configured", base_info->is_configured);
		spdk_json_write_named_uint64(w, "data_offset", base_info->data_offset);
		spdk_json_write_named_uint64(w, "data_size", base_info->data_size);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
}

/*
 * brief:
 * raid_bdev_dump_info_json is the function table pointer for raid bdev
 * params:
 * ctx - pointer to raid_bdev
 * w - pointer to json context
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct raid_bdev *raid_bdev = ctx;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_dump_config_json\n");

	/* Dump the raid bdev configuration related information */
	spdk_json_write_named_object_begin(w, "raid");
	raid_bdev_write_info_json(raid_bdev, w);
	spdk_json_write_object_end(w);

	return 0;
}

/*
 * brief:
 * raid_bdev_write_config_json is the function table pointer for raid bdev
 * params:
 * bdev - pointer to spdk_bdev
 * w - pointer to json context
 * returns:
 * none
 */
static void
raid_bdev_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	struct raid_bdev *raid_bdev = bdev->ctxt;
	struct raid_base_bdev_info *base_info;
	char uuid_str[SPDK_UUID_STRING_LEN];

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (raid_bdev->sb != NULL) {
		/* raid bdev configuration is stored in the superblock */
		return;
	}

	spdk_json_write_object_begin(w);

	spdk_json_write_named_string(w, "method", "bdev_raid_create");

	spdk_json_write_named_object_begin(w, "params");
	spdk_json_write_named_string(w, "name", bdev->name);
	spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &raid_bdev->bdev.uuid);
	spdk_json_write_named_string(w, "uuid", uuid_str);
	spdk_json_write_named_uint32(w, "strip_size_kb", raid_bdev->strip_size_kb);
	spdk_json_write_named_string(w, "raid_level", raid_bdev_level_to_str(raid_bdev->level));
	spdk_json_write_named_bool(w, "superblock", raid_bdev->sb != NULL);

	spdk_json_write_named_array_begin(w, "base_bdevs");
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->desc) {
			spdk_json_write_string(w, spdk_bdev_desc_get_bdev(base_info->desc)->name);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);

	spdk_json_write_object_end(w);
}

static int
raid_bdev_get_memory_domains(void *ctx, struct spdk_memory_domain **domains, int array_size)
{
	struct raid_bdev *raid_bdev = ctx;
	struct raid_base_bdev_info *base_info;
	int domains_count = 0, rc = 0;

	if (raid_bdev->module->memory_domains_supported == false) {
		return 0;
	}

	spdk_spin_lock(&raid_bdev->base_bdev_lock);

	/* First loop to get the number of memory domains */
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->desc == NULL) {
			continue;
		}
		rc = spdk_bdev_get_memory_domains(spdk_bdev_desc_get_bdev(base_info->desc), NULL, 0);
		if (rc < 0) {
			goto out;
		}
		domains_count += rc;
	}

	if (!domains || array_size < domains_count) {
		goto out;
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->desc == NULL) {
			continue;
		}
		rc = spdk_bdev_get_memory_domains(spdk_bdev_desc_get_bdev(base_info->desc), domains, array_size);
		if (rc < 0) {
			goto out;
		}
		domains += rc;
		array_size -= rc;
	}
out:
	spdk_spin_unlock(&raid_bdev->base_bdev_lock);

	if (rc < 0) {
		return rc;
	}

	return domains_count;
}

/* g_raid_bdev_fn_table is the function table for raid bdev */
static const struct spdk_bdev_fn_table g_raid_bdev_fn_table = {
	.destruct		= raid_bdev_destruct,
	.submit_request		= raid_bdev_submit_request,
	.io_type_supported	= raid_bdev_io_type_supported,
	.get_io_channel		= raid_bdev_get_io_channel,
	.dump_info_json		= raid_bdev_dump_info_json,
	.write_config_json	= raid_bdev_write_config_json,
	.get_memory_domains	= raid_bdev_get_memory_domains,
};

struct raid_bdev *
raid_bdev_find_by_name(const char *name)
{
	struct raid_bdev *raid_bdev;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (strcmp(raid_bdev->bdev.name, name) == 0) {
			return raid_bdev;
		}
	}

	return NULL;
}

static struct {
	const char *name;
	enum raid_level value;
} g_raid_level_names[] = {
	{ "raid0", RAID0 },
	{ "0", RAID0 },
	{ "raid1", RAID1 },
	{ "1", RAID1 },
	{ "raid5f", RAID5F },
	{ "5f", RAID5F },
	{ "concat", CONCAT },
	{ }
};

static struct {
	const char *name;
	enum raid_bdev_state value;
} g_raid_state_names[] = {
	{ "online", RAID_BDEV_STATE_ONLINE },
	{ "configuring", RAID_BDEV_STATE_CONFIGURING },
	{ "offline", RAID_BDEV_STATE_OFFLINE },
	{ }
};

/* We have to use the typedef in the function declaration to appease astyle. */
typedef enum raid_level raid_level_t;
typedef enum raid_bdev_state raid_bdev_state_t;

raid_level_t
raid_bdev_str_to_level(const char *str)
{
	unsigned int i;

	assert(str != NULL);

	for (i = 0; g_raid_level_names[i].name != NULL; i++) {
		if (strcasecmp(g_raid_level_names[i].name, str) == 0) {
			return g_raid_level_names[i].value;
		}
	}

	return INVALID_RAID_LEVEL;
}

const char *
raid_bdev_level_to_str(enum raid_level level)
{
	unsigned int i;

	for (i = 0; g_raid_level_names[i].name != NULL; i++) {
		if (g_raid_level_names[i].value == level) {
			return g_raid_level_names[i].name;
		}
	}

	return "";
}

raid_bdev_state_t
raid_bdev_str_to_state(const char *str)
{
	unsigned int i;

	assert(str != NULL);

	for (i = 0; g_raid_state_names[i].name != NULL; i++) {
		if (strcasecmp(g_raid_state_names[i].name, str) == 0) {
			return g_raid_state_names[i].value;
		}
	}

	return RAID_BDEV_STATE_MAX;
}

const char *
raid_bdev_state_to_str(enum raid_bdev_state state)
{
	unsigned int i;

	for (i = 0; g_raid_state_names[i].name != NULL; i++) {
		if (g_raid_state_names[i].value == state) {
			return g_raid_state_names[i].name;
		}
	}

	assert(false);
	return "";
}

/*
 * brief:
 * raid_bdev_fini_start is called when bdev layer is starting the
 * shutdown process
 * params:
 * none
 * returns:
 * none
 */
static void
raid_bdev_fini_start(void)
{
	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_fini_start\n");
	g_shutdown_started = true;
}

/*
 * brief:
 * raid_bdev_exit is called on raid bdev module exit time by bdev layer
 * params:
 * none
 * returns:
 * none
 */
static void
raid_bdev_exit(void)
{
	struct raid_bdev *raid_bdev, *tmp;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_exit\n");

	TAILQ_FOREACH_SAFE(raid_bdev, &g_raid_bdev_list, global_link, tmp) {
		raid_bdev_cleanup_and_free(raid_bdev);
	}
}

/*
 * brief:
 * raid_bdev_get_ctx_size is used to return the context size of bdev_io for raid
 * module
 * params:
 * none
 * returns:
 * size of spdk_bdev_io context for raid
 */
static int
raid_bdev_get_ctx_size(void)
{
	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_get_ctx_size\n");
	return sizeof(struct raid_bdev_io);
}

static struct spdk_bdev_module g_raid_if = {
	.name = "raid",
	.module_init = raid_bdev_init,
	.fini_start = raid_bdev_fini_start,
	.module_fini = raid_bdev_exit,
	.get_ctx_size = raid_bdev_get_ctx_size,
	.examine_disk = raid_bdev_examine,
	.async_init = false,
	.async_fini = false,
};
SPDK_BDEV_MODULE_REGISTER(raid, &g_raid_if)

/*
 * brief:
 * raid_bdev_init is the initialization function for raid bdev module
 * params:
 * none
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_init(void)
{
	return 0;
}

static int
_raid_bdev_create(const char *name, uint32_t strip_size, uint8_t num_base_bdevs,
		  enum raid_level level, bool superblock_enabled, const struct spdk_uuid *uuid,
		  struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	struct spdk_bdev *raid_bdev_gen;
	struct raid_bdev_module *module;
	struct raid_base_bdev_info *base_info;
	uint8_t min_operational;

	if (strnlen(name, RAID_BDEV_SB_NAME_SIZE) == RAID_BDEV_SB_NAME_SIZE) {
		SPDK_ERRLOG("Raid bdev name '%s' exceeds %d characters\n", name, RAID_BDEV_SB_NAME_SIZE - 1);
		return -EINVAL;
	}

	if (raid_bdev_find_by_name(name) != NULL) {
		SPDK_ERRLOG("Duplicate raid bdev name found: %s\n", name);
		return -EEXIST;
	}

	if (level == RAID1) {
		if (strip_size != 0) {
			SPDK_ERRLOG("Strip size is not supported by raid1\n");
			return -EINVAL;
		}
	} else if (spdk_u32_is_pow2(strip_size) == false) {
		SPDK_ERRLOG("Invalid strip size %" PRIu32 "\n", strip_size);
		return -EINVAL;
	}

	module = raid_bdev_module_find(level);
	if (module == NULL) {
		SPDK_ERRLOG("Unsupported raid level '%d'\n", level);
		return -EINVAL;
	}

	assert(module->base_bdevs_min != 0);
	if (num_base_bdevs < module->base_bdevs_min) {
		SPDK_ERRLOG("At least %u base devices required for %s\n",
			    module->base_bdevs_min,
			    raid_bdev_level_to_str(level));
		return -EINVAL;
	}

	switch (module->base_bdevs_constraint.type) {
	case CONSTRAINT_MAX_BASE_BDEVS_REMOVED:
		min_operational = num_base_bdevs - module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_MIN_BASE_BDEVS_OPERATIONAL:
		min_operational = module->base_bdevs_constraint.value;
		break;
	case CONSTRAINT_UNSET:
		if (module->base_bdevs_constraint.value != 0) {
			SPDK_ERRLOG("Unexpected constraint value '%u' provided for raid bdev '%s'.\n",
				    (uint8_t)module->base_bdevs_constraint.value, name);
			return -EINVAL;
		}
		min_operational = num_base_bdevs;
		break;
	default:
		SPDK_ERRLOG("Unrecognised constraint type '%u' in module for raid level '%s'.\n",
			    (uint8_t)module->base_bdevs_constraint.type,
			    raid_bdev_level_to_str(module->level));
		return -EINVAL;
	};

	if (min_operational == 0 || min_operational > num_base_bdevs) {
		SPDK_ERRLOG("Wrong constraint value for raid level '%s'.\n",
			    raid_bdev_level_to_str(module->level));
		return -EINVAL;
	}

	raid_bdev = calloc(1, sizeof(*raid_bdev));
	if (!raid_bdev) {
		SPDK_ERRLOG("Unable to allocate memory for raid bdev\n");
		return -ENOMEM;
	}

	spdk_spin_init(&raid_bdev->base_bdev_lock);
	raid_bdev->module = module;
	raid_bdev->num_base_bdevs = num_base_bdevs;
	raid_bdev->base_bdev_info = calloc(raid_bdev->num_base_bdevs,
					   sizeof(struct raid_base_bdev_info));
	if (!raid_bdev->base_bdev_info) {
		SPDK_ERRLOG("Unable able to allocate base bdev info\n");
		raid_bdev_free(raid_bdev);
		return -ENOMEM;
	}

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->raid_bdev = raid_bdev;
	}

	/* strip_size_kb is from the rpc param.  strip_size is in blocks and used
	 * internally and set later.
	 */
	raid_bdev->strip_size = 0;
	raid_bdev->strip_size_kb = strip_size;
	raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
	raid_bdev->level = level;
	raid_bdev->min_base_bdevs_operational = min_operational;

	if (superblock_enabled) {
		raid_bdev->sb = spdk_dma_zmalloc(RAID_BDEV_SB_MAX_LENGTH, 0x1000, NULL);
		if (!raid_bdev->sb) {
			SPDK_ERRLOG("Failed to allocate raid bdev sb buffer\n");
			raid_bdev_free(raid_bdev);
			return -ENOMEM;
		}
	}

	raid_bdev_gen = &raid_bdev->bdev;

	raid_bdev_gen->name = strdup(name);
	if (!raid_bdev_gen->name) {
		SPDK_ERRLOG("Unable to allocate name for raid\n");
		raid_bdev_free(raid_bdev);
		return -ENOMEM;
	}

	raid_bdev_gen->product_name = "Raid Volume";
	raid_bdev_gen->ctxt = raid_bdev;
	raid_bdev_gen->fn_table = &g_raid_bdev_fn_table;
	raid_bdev_gen->module = &g_raid_if;
	raid_bdev_gen->write_cache = 0;
	spdk_uuid_copy(&raid_bdev_gen->uuid, uuid);

	TAILQ_INSERT_TAIL(&g_raid_bdev_list, raid_bdev, global_link);

	*raid_bdev_out = raid_bdev;

	return 0;
}

/*
 * brief:
 * raid_bdev_create allocates raid bdev based on passed configuration
 * params:
 * name - name for raid bdev
 * strip_size - strip size in KB
 * num_base_bdevs - number of base bdevs
 * level - raid level
 * superblock_enabled - true if raid should have superblock
 * uuid - uuid to set for the bdev
 * raid_bdev_out - the created raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
raid_bdev_create(const char *name, uint32_t strip_size, uint8_t num_base_bdevs,
		 enum raid_level level, bool superblock_enabled, const struct spdk_uuid *uuid,
		 struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	int rc;

	assert(uuid != NULL);

	rc = _raid_bdev_create(name, strip_size, num_base_bdevs, level, superblock_enabled, uuid,
			       &raid_bdev);
	if (rc != 0) {
		return rc;
	}

	if (superblock_enabled && spdk_uuid_is_null(uuid)) {
		/* we need to have the uuid to store in the superblock before the bdev is registered */
		spdk_uuid_generate(&raid_bdev->bdev.uuid);
	}

	raid_bdev->num_base_bdevs_operational = num_base_bdevs;

	*raid_bdev_out = raid_bdev;

	return 0;
}

/*
 * brief:
 * Check underlying block devices against support for metadata. Do not configure
 * md support when parameters from block devices are inconsistent.
 * params:
 * raid_bdev - pointer to raid bdev
 * returns:
 * 0 - The raid bdev md parameters were successfully configured.
 * non zero - Failed to configure md.
 */
static int
raid_bdev_configure_md(struct raid_bdev *raid_bdev)
{
	struct spdk_bdev *base_bdev;
	uint8_t i;

	for (i = 0; i < raid_bdev->num_base_bdevs; i++) {
		if (raid_bdev->base_bdev_info[i].desc == NULL) {
			continue;
		}
		base_bdev = spdk_bdev_desc_get_bdev(raid_bdev->base_bdev_info[i].desc);

		/* Currently, RAID bdevs do not support DIF or DIX, so a RAID bdev cannot
		 * be created on top of any bdev which supports it */
		if (spdk_bdev_get_dif_type(base_bdev) != SPDK_DIF_DISABLE) {
			SPDK_ERRLOG("at least one base bdev has DIF or DIX enabled "
				    "- unsupported RAID configuration\n");
			return -EPERM;
		}

		if (i == 0) {
			raid_bdev->bdev.md_len = spdk_bdev_get_md_size(base_bdev);
			raid_bdev->bdev.md_interleave = spdk_bdev_is_md_interleaved(base_bdev);
			continue;
		}

		if (raid_bdev->bdev.md_len != spdk_bdev_get_md_size(base_bdev) ||
		    raid_bdev->bdev.md_interleave != spdk_bdev_is_md_interleaved(base_bdev)) {
			SPDK_ERRLOG("base bdevs are configured with different metadata formats\n");
			return -EPERM;
		}
	}

	return 0;
}

static void
raid_bdev_configure_cont(struct raid_bdev *raid_bdev)
{
	struct spdk_bdev *raid_bdev_gen = &raid_bdev->bdev;
	int rc;

	raid_bdev->state = RAID_BDEV_STATE_ONLINE;
	SPDK_DEBUGLOG(bdev_raid, "io device register %p\n", raid_bdev);
	SPDK_DEBUGLOG(bdev_raid, "blockcnt %" PRIu64 ", blocklen %u\n",
		      raid_bdev_gen->blockcnt, raid_bdev_gen->blocklen);
	spdk_io_device_register(raid_bdev, raid_bdev_create_cb, raid_bdev_destroy_cb,
				sizeof(struct raid_bdev_io_channel),
				raid_bdev_gen->name);
	rc = spdk_bdev_register(raid_bdev_gen);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to register raid bdev and stay at configuring state\n");
		if (raid_bdev->module->stop != NULL) {
			raid_bdev->module->stop(raid_bdev);
		}
		spdk_io_device_unregister(raid_bdev, NULL);
		raid_bdev->state = RAID_BDEV_STATE_CONFIGURING;
		return;
	}
	SPDK_DEBUGLOG(bdev_raid, "raid bdev generic %p\n", raid_bdev_gen);
	SPDK_DEBUGLOG(bdev_raid, "raid bdev is created with name %s, raid_bdev %p\n",
		      raid_bdev_gen->name, raid_bdev);
}

static void
raid_bdev_configure_write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	if (status == 0) {
		raid_bdev_configure_cont(raid_bdev);
	} else {
		SPDK_ERRLOG("Failed to write raid bdev '%s' superblock: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
		if (raid_bdev->module->stop != NULL) {
			raid_bdev->module->stop(raid_bdev);
		}
	}
}

/*
 * brief:
 * If raid bdev config is complete, then only register the raid bdev to
 * bdev layer and remove this raid bdev from configuring list and
 * insert the raid bdev to configured list
 * params:
 * raid_bdev - pointer to raid bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
static int
raid_bdev_configure(struct raid_bdev *raid_bdev)
{
	uint32_t blocklen = 0;
	struct raid_base_bdev_info *base_info;
	struct spdk_bdev *base_bdev;
	int rc = 0;

	assert(raid_bdev->state == RAID_BDEV_STATE_CONFIGURING);
	assert(raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs_operational);

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		if (base_info->desc == NULL) {
			continue;
		}
		base_bdev = spdk_bdev_desc_get_bdev(base_info->desc);

		/* Check blocklen for all base bdevs that it should be same */
		if (blocklen == 0) {
			blocklen = base_bdev->blocklen;
		} else if (blocklen != base_bdev->blocklen) {
			/*
			 * Assumption is that all the base bdevs for any raid bdev should
			 * have same blocklen
			 */
			SPDK_ERRLOG("Blocklen of various bdevs not matching\n");
			return -EINVAL;
		}
	}
	assert(blocklen > 0);

	/* The strip_size_kb is read in from user in KB. Convert to blocks here for
	 * internal use.
	 */
	raid_bdev->strip_size = (raid_bdev->strip_size_kb * 1024) / blocklen;
	if (raid_bdev->strip_size == 0 && raid_bdev->level != RAID1) {
		SPDK_ERRLOG("Strip size cannot be smaller than the device block size\n");
		return -EINVAL;
	}
	raid_bdev->strip_size_shift = spdk_u32log2(raid_bdev->strip_size);
	raid_bdev->blocklen_shift = spdk_u32log2(blocklen);
	raid_bdev->bdev.blocklen = blocklen;

	rc = raid_bdev_configure_md(raid_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("raid metadata configuration failed\n");
		return rc;
	}

	rc = raid_bdev->module->start(raid_bdev);
	if (rc != 0) {
		SPDK_ERRLOG("raid module startup callback failed\n");
		return rc;
	}

	if (raid_bdev->sb != NULL) {
		if (spdk_uuid_is_null(&raid_bdev->sb->uuid)) {
			/* NULL UUID is not valid in the sb so it means that we are creating a new
			 * raid bdev and should initialize the superblock.
			 */
			raid_bdev_init_superblock(raid_bdev);
		} else {
			assert(spdk_uuid_compare(&raid_bdev->sb->uuid, &raid_bdev->bdev.uuid) == 0);
			if (raid_bdev->sb->block_size != blocklen) {
				SPDK_ERRLOG("blocklen does not match value in superblock\n");
				rc = -EINVAL;
			}
			if (raid_bdev->sb->raid_size != raid_bdev->bdev.blockcnt) {
				SPDK_ERRLOG("blockcnt does not match value in superblock\n");
				rc = -EINVAL;
			}
			if (rc != 0) {
				if (raid_bdev->module->stop != NULL) {
					raid_bdev->module->stop(raid_bdev);
				}
				return rc;
			}
		}

		raid_bdev_write_superblock(raid_bdev, raid_bdev_configure_write_sb_cb, NULL);
	} else {
		raid_bdev_configure_cont(raid_bdev);
	}

	return 0;
}

/*
 * brief:
 * If raid bdev is online and registered, change the bdev state to
 * configuring and unregister this raid device. Queue this raid device
 * in configuring list
 * params:
 * raid_bdev - pointer to raid bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * none
 */
static void
raid_bdev_deconfigure(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn,
		      void *cb_arg)
{
	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
		return;
	}

	raid_bdev->state = RAID_BDEV_STATE_OFFLINE;
	assert(raid_bdev->num_base_bdevs_discovered);
	SPDK_DEBUGLOG(bdev_raid, "raid bdev state changing from online to offline\n");

	spdk_bdev_unregister(&raid_bdev->bdev, cb_fn, cb_arg);
}

/*
 * brief:
 * raid_bdev_find_base_info_by_bdev function finds the base bdev info by bdev.
 * params:
 * base_bdev - pointer to base bdev
 * returns:
 * base bdev info if found, otherwise NULL.
 */
static struct raid_base_bdev_info *
raid_bdev_find_base_info_by_bdev(struct spdk_bdev *base_bdev)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
			if (base_info->desc != NULL &&
			    spdk_bdev_desc_get_bdev(base_info->desc) == base_bdev) {
				return base_info;
			}
		}
	}

	return NULL;
}

static void
raid_bdev_remove_base_bdev_done(struct raid_base_bdev_info *base_info, int status)
{
	assert(base_info->remove_scheduled);

	base_info->remove_scheduled = false;
	if (base_info->remove_cb != NULL) {
		base_info->remove_cb(base_info->remove_cb_ctx, status);
	}
}

static void
raid_bdev_remove_base_bdev_write_sb_cb(int status, struct raid_bdev *raid_bdev, void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;

	if (status != 0) {
		SPDK_ERRLOG("Failed to write raid bdev '%s' superblock: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
	}

	raid_bdev_remove_base_bdev_done(base_info, status);
}

static void
raid_bdev_remove_base_bdev_on_unquiesced(void *ctx, int status)
{
	struct raid_base_bdev_info *base_info = ctx;
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to unquiesce raid bdev %s: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
		goto out;
	}

	spdk_spin_lock(&raid_bdev->base_bdev_lock);
	raid_bdev_free_base_bdev_resource(base_info);
	spdk_spin_unlock(&raid_bdev->base_bdev_lock);

	if (raid_bdev->sb) {
		struct raid_bdev_superblock *sb = raid_bdev->sb;
		struct raid_bdev_sb_base_bdev *sb_base_bdev = NULL;
		uint8_t slot = raid_bdev_base_bdev_slot(base_info);
		uint8_t i;

		for (i = 0; i < sb->base_bdevs_size; i++) {
			sb_base_bdev = &sb->base_bdevs[i];

			if (sb_base_bdev->state == RAID_SB_BASE_BDEV_CONFIGURED &&
			    sb_base_bdev->slot == slot) {
				break;
			}
		}

		assert(i < sb->base_bdevs_size);

		/* TODO: distinguish between failure and intentional removal */
		sb_base_bdev->state = RAID_SB_BASE_BDEV_FAILED;

		raid_bdev_write_superblock(raid_bdev, raid_bdev_remove_base_bdev_write_sb_cb, base_info);
		return;
	}
out:
	raid_bdev_remove_base_bdev_done(base_info, status);
}

static void
raid_bdev_channel_remove_base_bdev(struct spdk_io_channel_iter *i)
{
	struct raid_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *ch = spdk_io_channel_iter_get_channel(i);
	struct raid_bdev_io_channel *raid_ch = spdk_io_channel_get_ctx(ch);
	uint8_t idx = raid_bdev_base_bdev_slot(base_info);

	SPDK_DEBUGLOG(bdev_raid, "slot: %u raid_ch: %p\n", idx, raid_ch);

	if (raid_ch->base_channel[idx] != NULL) {
		spdk_put_io_channel(raid_ch->base_channel[idx]);
		raid_ch->base_channel[idx] = NULL;
	}

	spdk_for_each_channel_continue(i, 0);
}

static void
raid_bdev_channels_remove_base_bdev_done(struct spdk_io_channel_iter *i, int status)
{
	struct raid_base_bdev_info *base_info = spdk_io_channel_iter_get_ctx(i);
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	spdk_bdev_unquiesce(&raid_bdev->bdev, &g_raid_if, raid_bdev_remove_base_bdev_on_unquiesced,
			    base_info);
}

static void
raid_bdev_remove_base_bdev_on_quiesced(void *ctx, int status)
{
	struct raid_base_bdev_info *base_info = ctx;
	struct raid_bdev *raid_bdev = base_info->raid_bdev;

	if (status != 0) {
		SPDK_ERRLOG("Failed to quiesce raid bdev %s: %s\n",
			    raid_bdev->bdev.name, spdk_strerror(-status));
		raid_bdev_remove_base_bdev_done(base_info, status);
		return;
	}

	spdk_for_each_channel(raid_bdev, raid_bdev_channel_remove_base_bdev, base_info,
			      raid_bdev_channels_remove_base_bdev_done);
}

/*
 * brief:
 * raid_bdev_remove_base_bdev function is called by below layers when base_bdev
 * is removed. This function checks if this base bdev is part of any raid bdev
 * or not. If yes, it takes necessary action on that particular raid bdev.
 * params:
 * base_bdev - pointer to base bdev which got removed
 * cb_fn - callback function
 * cb_arg - argument to callback function
 * returns:
 * 0 - success
 * non zero - failure
 */
int
raid_bdev_remove_base_bdev(struct spdk_bdev *base_bdev, raid_bdev_remove_base_bdev_cb cb_fn,
			   void *cb_ctx)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "%s\n", base_bdev->name);

	/* Find the raid_bdev which has claimed this base_bdev */
	base_info = raid_bdev_find_base_info_by_bdev(base_bdev);
	if (!base_info) {
		SPDK_ERRLOG("bdev to remove '%s' not found\n", base_bdev->name);
		return -ENODEV;
	}
	raid_bdev = base_info->raid_bdev;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	if (base_info->remove_scheduled) {
		return 0;
	}

	assert(base_info->desc);
	base_info->remove_scheduled = true;
	base_info->remove_cb = cb_fn;
	base_info->remove_cb_ctx = cb_ctx;

	if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
		/*
		 * As raid bdev is not registered yet or already unregistered,
		 * so cleanup should be done here itself.
		 *
		 * Removing a base bdev at this stage does not change the number of operational
		 * base bdevs, only the number of discovered base bdevs.
		 */
		raid_bdev_free_base_bdev_resource(base_info);
		if (raid_bdev->num_base_bdevs_discovered == 0) {
			/* There is no base bdev for this raid, so free the raid device. */
			raid_bdev_cleanup_and_free(raid_bdev);
		}
	} else if (raid_bdev->num_base_bdevs_operational-- == raid_bdev->min_base_bdevs_operational) {
		/*
		 * After this base bdev is removed there will not be enough base bdevs
		 * to keep the raid bdev operational.
		 */
		raid_bdev_deconfigure(raid_bdev, cb_fn, cb_ctx);
	} else {
		int ret;

		ret = spdk_bdev_quiesce(&raid_bdev->bdev, &g_raid_if,
					raid_bdev_remove_base_bdev_on_quiesced, base_info);
		if (ret != 0) {
			base_info->remove_scheduled = false;
		}
	}

	return 0;
}

/*
 * brief:
 * raid_bdev_resize_base_bdev function is called by below layers when base_bdev
 * is resized. This function checks if the smallest size of the base_bdevs is changed.
 * If yes, call module handler to resize the raid_bdev if implemented.
 * params:
 * base_bdev - pointer to base bdev which got resized.
 * returns:
 * none
 */
static void
raid_bdev_resize_base_bdev(struct spdk_bdev *base_bdev)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "raid_bdev_resize_base_bdev\n");

	base_info = raid_bdev_find_base_info_by_bdev(base_bdev);

	/* Find the raid_bdev which has claimed this base_bdev */
	if (!base_info) {
		SPDK_ERRLOG("raid_bdev whose base_bdev '%s' not found\n", base_bdev->name);
		return;
	}
	raid_bdev = base_info->raid_bdev;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	SPDK_NOTICELOG("base_bdev '%s' was resized: old size %" PRIu64 ", new size %" PRIu64 "\n",
		       base_bdev->name, base_info->blockcnt, base_bdev->blockcnt);

	if (raid_bdev->module->resize) {
		raid_bdev->module->resize(raid_bdev);
	}
}

/*
 * brief:
 * raid_bdev_event_base_bdev function is called by below layers when base_bdev
 * triggers asynchronous event.
 * params:
 * type - event details.
 * bdev - bdev that triggered event.
 * event_ctx - context for event.
 * returns:
 * none
 */
static void
raid_bdev_event_base_bdev(enum spdk_bdev_event_type type, struct spdk_bdev *bdev,
			  void *event_ctx)
{
	int rc;

	switch (type) {
	case SPDK_BDEV_EVENT_REMOVE:
		rc = raid_bdev_remove_base_bdev(bdev, NULL, NULL);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to remove base bdev %s: %s\n",
				    spdk_bdev_get_name(bdev), spdk_strerror(-rc));
		}
		break;
	case SPDK_BDEV_EVENT_RESIZE:
		raid_bdev_resize_base_bdev(bdev);
		break;
	default:
		SPDK_NOTICELOG("Unsupported bdev event: type %d\n", type);
		break;
	}
}

/*
 * brief:
 * Deletes the specified raid bdev
 * params:
 * raid_bdev - pointer to raid bdev
 * cb_fn - callback function
 * cb_arg - argument to callback function
 */
void
raid_bdev_delete(struct raid_bdev *raid_bdev, raid_bdev_destruct_cb cb_fn, void *cb_arg)
{
	struct raid_base_bdev_info *base_info;

	SPDK_DEBUGLOG(bdev_raid, "delete raid bdev: %s\n", raid_bdev->bdev.name);

	if (raid_bdev->destroy_started) {
		SPDK_DEBUGLOG(bdev_raid, "destroying raid bdev %s is already started\n",
			      raid_bdev->bdev.name);
		if (cb_fn) {
			cb_fn(cb_arg, -EALREADY);
		}
		return;
	}

	raid_bdev->destroy_started = true;

	RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
		base_info->remove_scheduled = true;

		if (raid_bdev->state != RAID_BDEV_STATE_ONLINE) {
			/*
			 * As raid bdev is not registered yet or already unregistered,
			 * so cleanup should be done here itself.
			 */
			raid_bdev_free_base_bdev_resource(base_info);
		}
	}

	if (raid_bdev->num_base_bdevs_discovered == 0) {
		/* There is no base bdev for this raid, so free the raid device. */
		raid_bdev_cleanup_and_free(raid_bdev);
		if (cb_fn) {
			cb_fn(cb_arg, 0);
		}
	} else {
		raid_bdev_deconfigure(raid_bdev, cb_fn, cb_arg);
	}
}

static void
raid_bdev_configure_base_bdev_cont(struct raid_base_bdev_info *base_info)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;
	int rc;

	base_info->is_configured = true;

	raid_bdev->num_base_bdevs_discovered++;
	assert(raid_bdev->num_base_bdevs_discovered <= raid_bdev->num_base_bdevs);
	assert(raid_bdev->num_base_bdevs_operational <= raid_bdev->num_base_bdevs);
	assert(raid_bdev->num_base_bdevs_operational >= raid_bdev->min_base_bdevs_operational);

	/*
	 * Configure the raid bdev when the number of discovered base bdevs reaches the number
	 * of base bdevs we know to be operational members of the array. Usually this is equal
	 * to the total number of base bdevs (num_base_bdevs) but can be less - when the array is
	 * degraded.
	 */
	if (raid_bdev->num_base_bdevs_discovered == raid_bdev->num_base_bdevs_operational) {
		rc = raid_bdev_configure(raid_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to configure raid bdev: %s\n", spdk_strerror(-rc));
		}
	}
}

static void
raid_bdev_configure_base_bdev_check_sb_cb(const struct raid_bdev_superblock *sb, int status,
		void *ctx)
{
	struct raid_base_bdev_info *base_info = ctx;

	switch (status) {
	case 0:
		/* valid superblock found */
		SPDK_ERRLOG("Existing raid superblock found on bdev %s\n", base_info->name);
		raid_bdev_free_base_bdev_resource(base_info);
		break;
	case -EINVAL:
		/* no valid superblock */
		raid_bdev_configure_base_bdev_cont(base_info);
		break;
	default:
		SPDK_ERRLOG("Failed to examine bdev %s: %s\n",
			    base_info->name, spdk_strerror(-status));
		break;
	}
}

static int
raid_bdev_configure_base_bdev(struct raid_base_bdev_info *base_info, bool existing)
{
	struct raid_bdev *raid_bdev = base_info->raid_bdev;
	struct spdk_bdev_desc *desc;
	struct spdk_bdev *bdev;
	const struct spdk_uuid *bdev_uuid;
	int rc;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());
	assert(base_info->desc == NULL);

	/*
	 * Base bdev can be added by name or uuid. Here we assure both properties are set and valid
	 * before claiming the bdev.
	 */

	if (!spdk_uuid_is_null(&base_info->uuid)) {
		char uuid_str[SPDK_UUID_STRING_LEN];
		const char *bdev_name;

		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &base_info->uuid);

		/* UUID of a bdev is registered as its alias */
		bdev = spdk_bdev_get_by_name(uuid_str);
		if (bdev == NULL) {
			return -ENODEV;
		}

		bdev_name = spdk_bdev_get_name(bdev);

		if (base_info->name == NULL) {
			assert(existing == true);
			base_info->name = strdup(bdev_name);
			if (base_info->name == NULL) {
				return -ENOMEM;
			}
		} else if (strcmp(base_info->name, bdev_name) != 0) {
			SPDK_ERRLOG("Name mismatch for base bdev '%s' - expected '%s'\n",
				    bdev_name, base_info->name);
			return -EINVAL;
		}
	}

	assert(base_info->name != NULL);

	rc = spdk_bdev_open_ext(base_info->name, true, raid_bdev_event_base_bdev, NULL, &desc);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("Unable to create desc on bdev '%s'\n", base_info->name);
		}
		return rc;
	}

	bdev = spdk_bdev_desc_get_bdev(desc);
	bdev_uuid = spdk_bdev_get_uuid(bdev);

	if (spdk_uuid_is_null(&base_info->uuid)) {
		spdk_uuid_copy(&base_info->uuid, bdev_uuid);
	} else if (spdk_uuid_compare(&base_info->uuid, bdev_uuid) != 0) {
		SPDK_ERRLOG("UUID mismatch for base bdev '%s'\n", base_info->name);
		spdk_bdev_close(desc);
		return -EINVAL;
	}

	rc = spdk_bdev_module_claim_bdev(bdev, NULL, &g_raid_if);
	if (rc != 0) {
		SPDK_ERRLOG("Unable to claim this bdev as it is already claimed\n");
		spdk_bdev_close(desc);
		return rc;
	}

	SPDK_DEBUGLOG(bdev_raid, "bdev %s is claimed\n", bdev->name);

	assert(raid_bdev->state != RAID_BDEV_STATE_ONLINE);

	base_info->app_thread_ch = spdk_bdev_get_io_channel(desc);
	if (base_info->app_thread_ch == NULL) {
		SPDK_ERRLOG("Failed to get io channel\n");
		spdk_bdev_module_release_bdev(bdev);
		spdk_bdev_close(desc);
		return -ENOMEM;
	}

	base_info->desc = desc;
	base_info->blockcnt = bdev->blockcnt;

	if (raid_bdev->sb != NULL) {
		uint64_t data_offset;

		if (base_info->data_offset == 0) {
			assert((RAID_BDEV_MIN_DATA_OFFSET_SIZE % bdev->blocklen) == 0);
			data_offset = RAID_BDEV_MIN_DATA_OFFSET_SIZE / bdev->blocklen;
		} else {
			data_offset = base_info->data_offset;
		}

		if (bdev->optimal_io_boundary != 0) {
			data_offset = spdk_divide_round_up(data_offset,
							   bdev->optimal_io_boundary) * bdev->optimal_io_boundary;
			if (base_info->data_offset != 0 && base_info->data_offset != data_offset) {
				SPDK_WARNLOG("Data offset %lu on bdev '%s' is different than optimal value %lu\n",
					     base_info->data_offset, base_info->name, data_offset);
				data_offset = base_info->data_offset;
			}
		}

		base_info->data_offset = data_offset;
	}

	if (base_info->data_offset >= bdev->blockcnt) {
		SPDK_ERRLOG("Data offset %lu exceeds base bdev capacity %lu on bdev '%s'\n",
			    base_info->data_offset, bdev->blockcnt, base_info->name);
		rc = -EINVAL;
		goto out;
	}

	if (base_info->data_size == 0) {
		base_info->data_size = bdev->blockcnt - base_info->data_offset;
	} else if (base_info->data_offset + base_info->data_size > bdev->blockcnt) {
		SPDK_ERRLOG("Data offset and size exceeds base bdev capacity %lu on bdev '%s'\n",
			    bdev->blockcnt, base_info->name);
		rc = -EINVAL;
		goto out;
	}

	if (existing) {
		raid_bdev_configure_base_bdev_cont(base_info);
	} else {
		/* check for existing superblock when using a new bdev */
		rc = raid_bdev_load_base_bdev_superblock(desc, base_info->app_thread_ch,
				raid_bdev_configure_base_bdev_check_sb_cb, base_info);
		if (rc) {
			SPDK_ERRLOG("Failed to read bdev %s superblock: %s\n",
				    bdev->name, spdk_strerror(-rc));
		}
	}
out:
	if (rc != 0) {
		raid_bdev_free_base_bdev_resource(base_info);
	}
	return rc;
}

/*
 * brief:
 * raid_bdev_add_base_device function is the actual function which either adds
 * the nvme base device to existing raid bdev or create a new raid bdev. It also claims
 * the base device and keep the open descriptor.
 * params:
 * raid_bdev - pointer to raid bdev
 * name - name of the base bdev
 * slot - position to add base bdev
 * returns:
 * 0 - success
 * non zero - failure
 */
int
raid_bdev_add_base_device(struct raid_bdev *raid_bdev, const char *name, uint8_t slot)
{
	struct raid_base_bdev_info *base_info;
	int rc;

	if (slot >= raid_bdev->num_base_bdevs) {
		return -EINVAL;
	}

	base_info = &raid_bdev->base_bdev_info[slot];

	if (base_info->name != NULL) {
		SPDK_ERRLOG("Slot %u on raid bdev '%s' already assigned to bdev '%s'\n",
			    slot, raid_bdev->bdev.name, base_info->name);
		return -EBUSY;
	}

	if (!spdk_uuid_is_null(&base_info->uuid)) {
		char uuid_str[SPDK_UUID_STRING_LEN];

		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str), &base_info->uuid);
		SPDK_ERRLOG("Slot %u on raid bdev '%s' already assigned to bdev with uuid %s\n",
			    slot, raid_bdev->bdev.name, uuid_str);
		return -EBUSY;
	}

	base_info->name = strdup(name);
	if (base_info->name == NULL) {
		return -ENOMEM;
	}

	rc = raid_bdev_configure_base_bdev(base_info, false);
	if (rc != 0) {
		if (rc != -ENODEV) {
			SPDK_ERRLOG("Failed to allocate resource for bdev '%s'\n", name);
		}
		return rc;
	}

	return 0;
}

static int
raid_bdev_create_from_sb(const struct raid_bdev_superblock *sb, struct raid_bdev **raid_bdev_out)
{
	struct raid_bdev *raid_bdev;
	uint8_t i;
	int rc;

	rc = _raid_bdev_create(sb->name, (sb->strip_size * sb->block_size) / 1024, sb->num_base_bdevs,
			       sb->level, true, &sb->uuid, &raid_bdev);
	if (rc != 0) {
		return rc;
	}

	assert(sb->length <= RAID_BDEV_SB_MAX_LENGTH);
	memcpy(raid_bdev->sb, sb, sb->length);

	for (i = 0; i < sb->base_bdevs_size; i++) {
		const struct raid_bdev_sb_base_bdev *sb_base_bdev = &sb->base_bdevs[i];
		struct raid_base_bdev_info *base_info = &raid_bdev->base_bdev_info[sb_base_bdev->slot];

		if (sb_base_bdev->state == RAID_SB_BASE_BDEV_CONFIGURED) {
			spdk_uuid_copy(&base_info->uuid, &sb_base_bdev->uuid);
			raid_bdev->num_base_bdevs_operational++;
		}

		base_info->data_offset = sb_base_bdev->data_offset;
		base_info->data_size = sb_base_bdev->data_size;
	}

	*raid_bdev_out = raid_bdev;
	return 0;
}

static void
raid_bdev_examine_no_sb(struct spdk_bdev *bdev)
{
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *base_info;

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		RAID_FOR_EACH_BASE_BDEV(raid_bdev, base_info) {
			if (base_info->desc == NULL && base_info->name != NULL &&
			    strcmp(bdev->name, base_info->name) == 0) {
				raid_bdev_configure_base_bdev(base_info, true);
				break;
			}
		}
	}
}

static void
raid_bdev_examine_sb(const struct raid_bdev_superblock *sb, struct spdk_bdev *bdev)
{
	const struct raid_bdev_sb_base_bdev *sb_base_bdev;
	struct raid_bdev *raid_bdev;
	struct raid_base_bdev_info *iter, *base_info;
	uint8_t i;
	int rc;

	if (sb->block_size != bdev->blocklen) {
		SPDK_WARNLOG("Bdev %s block size (%u) does not match the value in superblock (%u)\n",
			     bdev->name, sb->block_size, bdev->blocklen);
		return;
	}

	if (spdk_uuid_is_null(&sb->uuid)) {
		SPDK_WARNLOG("NULL raid bdev UUID in superblock on bdev %s\n", bdev->name);
		return;
	}

	TAILQ_FOREACH(raid_bdev, &g_raid_bdev_list, global_link) {
		if (spdk_uuid_compare(&raid_bdev->bdev.uuid, &sb->uuid) == 0) {
			break;
		}
	}

	if (raid_bdev) {
		if (sb->seq_number > raid_bdev->sb->seq_number) {
			SPDK_DEBUGLOG(bdev_raid,
				      "raid superblock seq_number on bdev %s (%lu) greater than existing raid bdev %s (%lu)\n",
				      bdev->name, sb->seq_number, raid_bdev->bdev.name, raid_bdev->sb->seq_number);

			if (raid_bdev->state != RAID_BDEV_STATE_CONFIGURING) {
				SPDK_WARNLOG("Newer version of raid bdev %s superblock found on bdev %s but raid bdev is not in configuring state.\n",
					     raid_bdev->bdev.name, bdev->name);
				return;
			}

			/* remove and then recreate the raid bdev using the newer superblock */
			raid_bdev_delete(raid_bdev, NULL, NULL);
			raid_bdev = NULL;
		} else if (sb->seq_number < raid_bdev->sb->seq_number) {
			SPDK_DEBUGLOG(bdev_raid,
				      "raid superblock seq_number on bdev %s (%lu) smaller than existing raid bdev %s (%lu)\n",
				      bdev->name, sb->seq_number, raid_bdev->bdev.name, raid_bdev->sb->seq_number);
			/* use the current raid bdev superblock */
			sb = raid_bdev->sb;
		}
	}

	for (i = 0; i < sb->base_bdevs_size; i++) {
		sb_base_bdev = &sb->base_bdevs[i];

		assert(spdk_uuid_is_null(&sb_base_bdev->uuid) == false);

		if (spdk_uuid_compare(&sb_base_bdev->uuid, spdk_bdev_get_uuid(bdev)) == 0) {
			break;
		}
	}

	if (i == sb->base_bdevs_size) {
		SPDK_DEBUGLOG(bdev_raid, "raid superblock does not contain this bdev's uuid\n");
		return;
	}

	if (!raid_bdev) {
		rc = raid_bdev_create_from_sb(sb, &raid_bdev);
		if (rc != 0) {
			SPDK_ERRLOG("Failed to create raid bdev %s: %s\n",
				    sb->name, spdk_strerror(-rc));
		}
	}

	if (sb_base_bdev->state != RAID_SB_BASE_BDEV_CONFIGURED) {
		SPDK_NOTICELOG("Bdev %s is not an active member of raid bdev %s. Ignoring.\n",
			       bdev->name, raid_bdev->bdev.name);
		return;
	}

	base_info = NULL;
	RAID_FOR_EACH_BASE_BDEV(raid_bdev, iter) {
		if (spdk_uuid_compare(&iter->uuid, spdk_bdev_get_uuid(bdev)) == 0) {
			base_info = iter;
			break;
		}
	}

	if (base_info == NULL) {
		SPDK_ERRLOG("Bdev %s is not a member of raid bdev %s\n",
			    bdev->name, raid_bdev->bdev.name);
		return;
	}

	rc = raid_bdev_configure_base_bdev(base_info, true);
	if (rc != 0) {
		SPDK_ERRLOG("Failed to configure bdev %s as base bdev of raid %s: %s\n",
			    bdev->name, raid_bdev->bdev.name, spdk_strerror(-rc));
	}
}

struct raid_bdev_examine_ctx {
	struct spdk_bdev_desc *desc;
	struct spdk_io_channel *ch;
};

static void
raid_bdev_examine_ctx_free(struct raid_bdev_examine_ctx *ctx)
{
	if (!ctx) {
		return;
	}

	if (ctx->ch) {
		spdk_put_io_channel(ctx->ch);
	}

	if (ctx->desc) {
		spdk_bdev_close(ctx->desc);
	}

	free(ctx);
}

static void
raid_bdev_examine_load_sb_cb(const struct raid_bdev_superblock *sb, int status, void *_ctx)
{
	struct raid_bdev_examine_ctx *ctx = _ctx;
	struct spdk_bdev *bdev = spdk_bdev_desc_get_bdev(ctx->desc);

	switch (status) {
	case 0:
		/* valid superblock found */
		SPDK_DEBUGLOG(bdev_raid, "raid superblock found on bdev %s\n", bdev->name);
		raid_bdev_examine_sb(sb, bdev);
		break;
	case -EINVAL:
		/* no valid superblock, check if it can be claimed anyway */
		raid_bdev_examine_no_sb(bdev);
		break;
	default:
		SPDK_ERRLOG("Failed to examine bdev %s: %s\n",
			    bdev->name, spdk_strerror(-status));
		break;
	}

	raid_bdev_examine_ctx_free(ctx);
	spdk_bdev_module_examine_done(&g_raid_if);
}

static void
raid_bdev_examine_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
}

/*
 * brief:
 * raid_bdev_examine function is the examine function call by the below layers
 * like bdev_nvme layer. This function will check if this base bdev can be
 * claimed by this raid bdev or not.
 * params:
 * bdev - pointer to base bdev
 * returns:
 * none
 */
static void
raid_bdev_examine(struct spdk_bdev *bdev)
{
	struct raid_bdev_examine_ctx *ctx;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		SPDK_ERRLOG("Failed to examine bdev %s: %s\n",
			    bdev->name, spdk_strerror(ENOMEM));
		goto err;
	}

	rc = spdk_bdev_open_ext(spdk_bdev_get_name(bdev), false, raid_bdev_examine_event_cb, NULL,
				&ctx->desc);
	if (rc) {
		SPDK_ERRLOG("Failed to open bdev %s: %s\n",
			    bdev->name, spdk_strerror(-rc));
		goto err;
	}

	ctx->ch = spdk_bdev_get_io_channel(ctx->desc);
	if (!ctx->ch) {
		SPDK_ERRLOG("Failed to get io channel for bdev %s\n", bdev->name);
		goto err;
	}

	rc = raid_bdev_load_base_bdev_superblock(ctx->desc, ctx->ch, raid_bdev_examine_load_sb_cb, ctx);
	if (rc) {
		SPDK_ERRLOG("Failed to read bdev %s superblock: %s\n",
			    bdev->name, spdk_strerror(-rc));
		goto err;
	}

	return;
err:
	raid_bdev_examine_ctx_free(ctx);
	spdk_bdev_module_examine_done(&g_raid_if);
}

/* Log component for bdev raid bdev module */
SPDK_LOG_REGISTER_COMPONENT(bdev_raid)
