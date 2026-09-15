/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2021 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019, 2021 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2025, Oracle and/or its affiliates.
 */

#include "spdk/stdinc.h"
#include "spdk_internal/cunit.h"
#include "common/lib/ut_multithread.c"
#include "nvmf/nvmf.c"
#include "spdk/bdev_module.h"

static bool g_add_transport_test;
static unsigned g_tgroup_live, g_tgroup_attempts, g_tgroup_fail_at;

void
nvmf_transport_poll_group_destroy(struct spdk_nvmf_transport_poll_group *group)
{
	if (g_add_transport_test) {
		g_tgroup_live--;
		free(group);
	}
}
DEFINE_STUB_V(nvmf_ctrlr_destruct, (struct spdk_nvmf_ctrlr *ctrlr));
DEFINE_STUB_V(nvmf_transport_qpair_fini, (struct spdk_nvmf_qpair *qpair,
		spdk_nvmf_transport_qpair_fini_cb cb_fn,
		void *cb_arg));
DEFINE_STUB_V(nvmf_qpair_free_aer, (struct spdk_nvmf_qpair *qpair));
DEFINE_STUB_V(nvmf_qpair_abort_pending_zcopy_reqs, (struct spdk_nvmf_qpair *qpair));
DEFINE_RETURN_MOCK(nvmf_transport_poll_group_create, struct spdk_nvmf_transport_poll_group *);
struct spdk_nvmf_transport_poll_group *
nvmf_transport_poll_group_create(struct spdk_nvmf_transport *transport,
				 struct spdk_nvmf_poll_group *group)
{
	struct spdk_nvmf_transport_poll_group *tgroup;

	HANDLE_RETURN_MOCK(nvmf_transport_poll_group_create);
	if (!g_add_transport_test || ++g_tgroup_attempts == g_tgroup_fail_at) {
		return NULL;
	}
	tgroup = calloc(1, sizeof(*tgroup));
	SPDK_CU_ASSERT_FATAL(tgroup != NULL);
	tgroup->transport = transport;
	g_tgroup_live++;
	return tgroup;
}
DEFINE_STUB(spdk_bdev_get_io_channel, struct spdk_io_channel *, (struct spdk_bdev_desc *desc),
	    NULL);
DEFINE_STUB(nvmf_ctrlr_async_event_ns_notice, int, (struct spdk_nvmf_ctrlr *ctrlr), 0);
DEFINE_STUB(nvmf_ctrlr_async_event_ana_change_notice, int,
	    (struct spdk_nvmf_ctrlr *ctrlr), 0);
DEFINE_STUB(nvmf_transport_poll_group_remove, int, (struct spdk_nvmf_transport_poll_group *group,
		struct spdk_nvmf_qpair *qpair), 0);
DEFINE_STUB_V(nvmf_transport_req_free, (struct spdk_nvmf_request *req));
DEFINE_STUB(nvmf_transport_poll_group_poll, int, (struct spdk_nvmf_transport_poll_group *group), 0);
DEFINE_STUB_V(nvmf_subsystem_remove_all_listeners, (struct spdk_nvmf_subsystem *subsystem,
		bool stop));
DEFINE_STUB_V(nvmf_subsystem_poll_group_update_ns_reservation, (const struct spdk_nvmf_ns *ns,
		struct spdk_nvmf_subsystem_pg_ns_info *pg_ns));
DEFINE_STUB(spdk_nvmf_subsystem_destroy, int, (struct spdk_nvmf_subsystem *subsystem,
		nvmf_subsystem_destroy_cb cpl_cb, void *cpl_cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_listener, struct spdk_nvmf_subsystem_listener *,
	    (struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_listener, struct spdk_nvmf_subsystem_listener *,
	    (struct spdk_nvmf_subsystem *subsystem,
	     struct spdk_nvmf_subsystem_listener *prev_listener), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next, struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_nqn, const char *,
	    (const struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_allow_any_host, bool,
	    (const struct spdk_nvmf_subsystem *subsystem), true);
DEFINE_STUB(spdk_nvmf_subsystem_get_sn, const char *,
	    (const struct spdk_nvmf_subsystem *subsystem),
	    NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_mn, const char *,
	    (const struct spdk_nvmf_subsystem *subsystem),
	    NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_max_namespaces, uint32_t,
	    (const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_min_cntlid, uint16_t,
	    (const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_max_cntlid, uint16_t,
	    (const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_host, struct spdk_nvmf_host *,
	    (struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_host_get_nqn, const char *, (const struct spdk_nvmf_host *host), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_host, struct spdk_nvmf_host *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_host *prev_host), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_ns, struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_ana_reporting, bool, (struct spdk_nvmf_subsystem *subsystem),
	    false);
DEFINE_STUB_V(spdk_nvmf_ns_get_opts, (const struct spdk_nvmf_ns *ns,
				      struct spdk_nvmf_ns_opts *opts, size_t opts_size));
DEFINE_STUB(spdk_nvmf_ns_get_id, uint32_t, (const struct spdk_nvmf_ns *ns), 0);
DEFINE_STUB(spdk_nvmf_ns_get_bdev, struct spdk_bdev *, (struct spdk_nvmf_ns *ns), NULL);
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_ns, struct spdk_nvmf_ns *,
	    (struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns), NULL);
DEFINE_STUB(spdk_nvmf_transport_listen, int, (struct spdk_nvmf_transport *transport,
		const struct spdk_nvme_transport_id *trid,
		struct spdk_nvmf_listen_opts *opts), 0);
DEFINE_STUB(spdk_nvmf_transport_stop_listen, int,
	    (struct spdk_nvmf_transport *transport,
	     const struct spdk_nvme_transport_id *trid), 0)
DEFINE_STUB(nvmf_transport_get_optimal_poll_group, struct spdk_nvmf_transport_poll_group *,
	    (struct spdk_nvmf_transport *transport, struct spdk_nvmf_qpair *qpair), NULL);
DEFINE_STUB(nvmf_transport_poll_group_add, int,
	    (struct spdk_nvmf_transport_poll_group *group,
	     struct spdk_nvmf_qpair *qpair), 0);
DEFINE_STUB(nvmf_transport_qpair_get_peer_trid, int,
	    (struct spdk_nvmf_qpair *qpair,
	     struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(nvmf_transport_qpair_get_local_trid, int,
	    (struct spdk_nvmf_qpair *qpair,
	     struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(nvmf_transport_qpair_get_listen_trid, int,
	    (struct spdk_nvmf_qpair *qpair,
	     struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB_V(spdk_nvmf_request_exec, (struct spdk_nvmf_request *req));
DEFINE_STUB_V(spdk_nvmf_request_zcopy_start, (struct spdk_nvmf_request *req));
DEFINE_STUB(spdk_nvmf_get_transport_name, const char *,
	    (struct spdk_nvmf_transport *transport), NULL);
DEFINE_STUB(spdk_nvmf_transport_destroy, int, (struct spdk_nvmf_transport *transport,
		spdk_nvmf_transport_destroy_done_cb cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_type, enum spdk_nvmf_subtype,
	    (struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_first, struct spdk_nvmf_subsystem *,
	    (struct spdk_nvmf_tgt *tgt), NULL);
DEFINE_STUB_V(nvmf_transport_dump_opts, (struct spdk_nvmf_transport *transport,
		struct spdk_json_write_ctx *w, bool named));
DEFINE_STUB_V(nvmf_transport_listen_dump_trid, (const struct spdk_nvme_transport_id *trid,
		struct spdk_json_write_ctx *w));
DEFINE_STUB(spdk_nvme_transport_id_compare, int, (const struct spdk_nvme_transport_id *trid1,
		const struct spdk_nvme_transport_id *trid2), 0);
DEFINE_STUB_V(spdk_nvmf_send_discovery_log_notice, (struct spdk_nvmf_tgt *tgt,
		const char *hostnqn));
DEFINE_STUB(nvmf_nqn_is_valid, bool, (const char *nqn), true);
DEFINE_STUB(nvmf_nqn_is_discovery, bool, (const char *nqn), true);
DEFINE_STUB(spdk_key_get_name, const char *, (struct spdk_key *k), NULL);
DEFINE_STUB(nvmf_qpair_auth_init, int, (struct spdk_nvmf_qpair *q), 0);
DEFINE_STUB_V(nvmf_qpair_auth_destroy, (struct spdk_nvmf_qpair *q));
DEFINE_STUB_V(nvmf_tgt_stop_mdns_prr, (struct spdk_nvmf_tgt *tgt));

struct spdk_io_channel {
	struct spdk_thread		*thread;
	struct io_device		*dev;
	uint32_t			ref;
	uint32_t			destroy_ref;
	TAILQ_ENTRY(spdk_io_channel)	tailq;
	spdk_io_channel_destroy_cb	destroy_cb;

	uint8_t				_padding[48];
};

uint64_t
spdk_bdev_get_num_blocks(const struct spdk_bdev *bdev)
{
	return bdev->blockcnt;
}

const struct spdk_uuid *
spdk_bdev_get_uuid(const struct spdk_bdev *bdev)
{
	return &bdev->uuid;
}

static int
ut_add_transport_pg_create(void *io_device, void *ctx)
{
	struct spdk_nvmf_poll_group *group = ctx;

	TAILQ_INIT(&group->tgroups);
	return 0;
}

static void
ut_add_transport_pg_destroy(void *io_device, void *ctx)
{
	struct spdk_nvmf_poll_group *group = ctx;
	struct spdk_nvmf_transport_poll_group *tgroup;

	while ((tgroup = TAILQ_FIRST(&group->tgroups)) != NULL) {
		TAILQ_REMOVE(&group->tgroups, tgroup, link);
		nvmf_transport_poll_group_destroy(tgroup);
	}
}

struct ut_add_transport_ctx {
	struct spdk_nvmf_transport *transport;
	struct spdk_nvmf_poll_group *groups[2];
	struct spdk_thread *thread;
	unsigned calls;
	int status;
};

static void
ut_add_transport_done(void *arg, int status)
{
	struct ut_add_transport_ctx *ctx = arg;
	unsigned i;

	CU_ASSERT(spdk_get_thread() == ctx->thread);
	CU_ASSERT(++ctx->calls == 1);
	ctx->status = status;
	if (status != 0) {
		for (i = 0; i < SPDK_COUNTOF(ctx->groups); i++) {
			CU_ASSERT(nvmf_get_transport_poll_group(ctx->groups[i], ctx->transport) == NULL);
		}
		/* Error completion returns ownership only after every channel unwinds. */
		free(ctx->transport);
		ctx->transport = NULL;
	}
}

static void
test_nvmf_tgt_add_transport_overlap(void)
{
	const struct spdk_nvmf_transport_ops tcp = { .name = "TCP" };
	const struct spdk_nvmf_transport_ops vfio = { .name = "VFIOUSER" };
	struct spdk_io_channel *channels[2];
	struct spdk_nvmf_tgt tgt;
	struct ut_add_transport_ctx first, second, retry;
	unsigned scenario, i;

	/* Duplicate type, partial setup. */
	for (scenario = 0; scenario < 2; scenario++) {
		allocate_threads(2);
		set_thread(0);
		memset(&tgt, 0, sizeof(tgt));
		memset(&first, 0, sizeof(first));
		memset(&second, 0, sizeof(second));
		memset(&retry, 0, sizeof(retry));
		TAILQ_INIT(&tgt.transports);
		RB_INIT(&tgt.subsystems);
		spdk_io_device_register(&tgt, ut_add_transport_pg_create, ut_add_transport_pg_destroy,
					sizeof(struct spdk_nvmf_poll_group), "add_transport");
		for (i = 0; i < SPDK_COUNTOF(channels); i++) {
			set_thread(i);
			channels[i] = spdk_get_io_channel(&tgt);
			SPDK_CU_ASSERT_FATAL(channels[i] != NULL);
			first.groups[i] = second.groups[i] = retry.groups[i] =
					spdk_io_channel_get_ctx(channels[i]);
		}
		set_thread(0);
		first.thread = second.thread = retry.thread = spdk_get_thread();
		MOCK_CLEAR(nvmf_transport_poll_group_create);
		g_add_transport_test = true;
		g_tgroup_attempts = 0;
		g_tgroup_fail_at = scenario == 1 ? 2 : 0;
		first.transport = calloc(1, sizeof(*first.transport));
		SPDK_CU_ASSERT_FATAL(first.transport != NULL);
		first.transport->ops = &tcp;
		spdk_nvmf_tgt_add_transport(&tgt, first.transport, ut_add_transport_done, &first);
		CU_ASSERT(first.calls == 0);
		if (scenario == 0) {
			second.transport = calloc(1, sizeof(*second.transport));
			SPDK_CU_ASSERT_FATAL(second.transport != NULL);
			second.transport->ops = &tcp;
			spdk_nvmf_tgt_add_transport(&tgt, second.transport, ut_add_transport_done, &second);
			CU_ASSERT(second.calls == 0);
		}
		poll_threads();
		CU_ASSERT(first.calls == 1);
		if (scenario == 0) {
			CU_ASSERT(first.status == 0);
			CU_ASSERT(second.calls == 1);
			CU_ASSERT(second.status == -EEXIST);
			CU_ASSERT(TAILQ_FIRST(&tgt.transports) == first.transport);
			CU_ASSERT(TAILQ_NEXT(first.transport, link) == NULL);
			CU_ASSERT(g_tgroup_live == 2);
		} else {
			CU_ASSERT(first.status == -1);
			CU_ASSERT(TAILQ_EMPTY(&tgt.transports));
			CU_ASSERT(g_tgroup_live == 0);
		}
		/* A compatible retry must succeed after the failed operation's callback. */
		set_thread(0);
		g_tgroup_fail_at = 0;
		retry.transport = calloc(1, sizeof(*retry.transport));
		SPDK_CU_ASSERT_FATAL(retry.transport != NULL);
		retry.transport->ops = &vfio;
		spdk_nvmf_tgt_add_transport(&tgt, retry.transport, ut_add_transport_done, &retry);
		poll_threads();
		CU_ASSERT(retry.calls == 1 && retry.status == 0);
		for (i = 0; i < SPDK_COUNTOF(channels); i++) {
			set_thread(i);
			spdk_put_io_channel(channels[i]);
		}
		poll_threads();
		CU_ASSERT(g_tgroup_live == 0);
		set_thread(0);
		spdk_io_device_unregister(&tgt, NULL);
		poll_threads();
		free(first.transport);
		free(retry.transport);
		g_add_transport_test = false;
		free_threads();
	}
}

static void
test_nvmf_tgt_options(void)
{
	struct spdk_nvmf_target_opts dflt_opts = {
		.size = SPDK_SIZEOF(&dflt_opts, dhchap_dhgroups),
		.name = "nvmf_test_tgt",
	};
	struct spdk_nvmf_target_opts dup_enable_opts = {
		.size = SPDK_SIZEOF(&dflt_opts, dup_host_policy),
		.name = "nvmf_test_tgt",
		.dup_host_policy = SPDK_NVMF_SUBSYSTEM_DUP_HOST_POLICY_RESTRICT_PER_LISTENER,
	};
	struct spdk_nvmf_tgt *tgt;

	allocate_threads(1);
	set_thread(0);

	/* Ensure dup_host_policy is set to ALLOW when not specified */
	tgt = spdk_nvmf_tgt_create(&dflt_opts);
	SPDK_CU_ASSERT_FATAL(tgt);
	SPDK_CU_ASSERT_FATAL(tgt->dup_host_policy == SPDK_NVMF_SUBSYSTEM_DUP_HOST_POLICY_ALLOW);
	spdk_nvmf_tgt_destroy(tgt, NULL, NULL);
	poll_threads();

	tgt = spdk_nvmf_tgt_create(&dup_enable_opts);
	SPDK_CU_ASSERT_FATAL(tgt);
	SPDK_CU_ASSERT_FATAL(
		tgt->dup_host_policy == SPDK_NVMF_SUBSYSTEM_DUP_HOST_POLICY_RESTRICT_PER_LISTENER);
	spdk_nvmf_tgt_destroy(tgt, NULL, NULL);
	poll_threads();
}

static void
test_nvmf_tgt_create_poll_group(void)
{
	int rc;
	struct spdk_thread		*thread = NULL;
	struct spdk_nvmf_tgt		tgt = {};
	struct spdk_nvmf_poll_group	group = {};
	struct spdk_nvmf_transport	transport = {};
	struct spdk_nvmf_subsystem	subsystem = {};
	struct spdk_nvmf_ns		ns = {};
	struct spdk_bdev		bdev = {};
	struct spdk_io_channel		ch = {};
	struct spdk_nvmf_transport_poll_group transport_pg = {};

	thread = spdk_thread_create(NULL, NULL);
	SPDK_CU_ASSERT_FATAL(thread != NULL);
	spdk_set_thread(thread);

	/* Create group with single subsystem */
	ch.thread = thread;
	MOCK_SET(spdk_bdev_get_io_channel, &ch);

	tgt.max_subsystems = 1;
	RB_INIT(&tgt.subsystems);

	/* Make sure subsystem has enough in subnqn so it can be
	 * inserted into RB-tree.
	 */
	snprintf(subsystem.subnqn, sizeof(subsystem.subnqn), "abc");
	RB_INSERT(subsystem_tree, &tgt.subsystems, &subsystem);
	subsystem.id = 0;
	subsystem.max_nsid = 1;
	subsystem.ns = calloc(1, sizeof(struct spdk_nvmf_ns *));
	SPDK_CU_ASSERT_FATAL(subsystem.ns != NULL);
	MOCK_SET(spdk_nvmf_subsystem_get_first, &subsystem);

	subsystem.ns[0] = &ns;
	TAILQ_INIT(&ns.registrants);
	ns.bdev = &bdev;
	spdk_uuid_generate(&bdev.uuid);
	bdev.blockcnt = 512;

	TAILQ_INIT(&tgt.transports);
	TAILQ_INIT(&tgt.poll_groups);
	tgt.num_poll_groups = 0;
	pthread_mutex_init(&tgt.mutex, NULL);
	transport.tgt = &tgt;
	TAILQ_INSERT_TAIL(&tgt.transports, &transport, link);

	MOCK_SET(nvmf_transport_poll_group_create, &transport_pg);
	rc = nvmf_tgt_create_poll_group((void *)&tgt, (void *)&group);
	MOCK_SET(nvmf_transport_poll_group_create, NULL);
	CU_ASSERT(rc == 0);
	CU_ASSERT(group.num_sgroups == 1);
	CU_ASSERT(group.sgroups != NULL);
	CU_ASSERT(group.sgroups[0].state == SPDK_NVMF_SUBSYSTEM_ACTIVE);
	CU_ASSERT(group.sgroups[0].ns_info[0].channel == &ch);
	CU_ASSERT(!memcmp(&group.sgroups[0].ns_info[0].uuid, &bdev.uuid, 16));
	CU_ASSERT(group.sgroups[0].ns_info[0].num_blocks == 512);
	CU_ASSERT(TAILQ_FIRST(&tgt.poll_groups) == &group);
	CU_ASSERT(tgt.num_poll_groups == 1);
	CU_ASSERT(group.thread == thread);

	nvmf_tgt_destroy_poll_group((void *)&tgt, (void *)&group);
	CU_ASSERT(TAILQ_EMPTY(&tgt.poll_groups));
	CU_ASSERT(tgt.num_poll_groups == 0);
	free(subsystem.ns);
	MOCK_CLEAR(spdk_nvmf_subsystem_get_first);

	spdk_thread_exit(thread);
	while (!spdk_thread_is_exited(thread)) {
		spdk_thread_poll(thread, 0, 0);
	}
	spdk_thread_destroy(thread);
	MOCK_CLEAR(spdk_bdev_get_io_channel);
}

static void
test_nvmf_target_opts_copy_bounds_size(void)
{
	struct {
		struct spdk_nvmf_target_opts opts;
		uint32_t canary;
	} dst = {};
	struct {
		struct spdk_nvmf_target_opts opts;
		uint8_t extra[32];
	} src = {};

	dst.canary = 0xdeadbeef;
	src.opts.size = sizeof(src);
	snprintf(src.opts.name, sizeof(src.opts.name), "%s", "nvmf_target");
	src.opts.max_subsystems = 17;
	src.opts.discovery_filter = SPDK_NVMF_TGT_DISCOVERY_FILTER_ANY;
	memset(src.extra, 0x5a, sizeof(src.extra));

	nvmf_target_opts_copy(&dst.opts, &src.opts, src.opts.size);

	CU_ASSERT(dst.canary == 0xdeadbeef);
	CU_ASSERT(dst.opts.size == sizeof(src));
	CU_ASSERT_STRING_EQUAL(dst.opts.name, "nvmf_target");
	CU_ASSERT(dst.opts.max_subsystems == 17);
	CU_ASSERT(dst.opts.discovery_filter == SPDK_NVMF_TGT_DISCOVERY_FILTER_ANY);
}

int
main(int argc, char **argv)
{
	CU_pSuite	suite = NULL;
	unsigned int	num_failures;

	CU_initialize_registry();

	suite = CU_add_suite("nvmf", NULL, NULL);

	CU_ADD_TEST(suite, test_nvmf_tgt_create_poll_group);
	CU_ADD_TEST(suite, test_nvmf_target_opts_copy_bounds_size);
	CU_ADD_TEST(suite, test_nvmf_tgt_add_transport_overlap);
	CU_ADD_TEST(suite, test_nvmf_tgt_options);

	num_failures = spdk_ut_run_tests(argc, argv, NULL);
	CU_cleanup_registry();
	return num_failures;
}
