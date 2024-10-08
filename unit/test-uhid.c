// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2014  Intel Corporation. All rights reserved.
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>

#include <glib.h>

#include "src/shared/uhid.h"
#include "src/shared/util.h"

#include "src/shared/tester.h"

struct test_pdu {
	bool valid;
	const uint8_t *data;
	size_t size;
};

struct test_device {
	const char *name;
	uint32_t vendor;
	uint32_t product;
	uint32_t version;
	uint32_t country;
	uint8_t type;
	struct iovec map;
};

struct test_data {
	char *test_name;
	struct test_device *test_device;
	struct test_pdu *pdu_list;
};

struct context {
	struct bt_uhid *uhid;
	guint source;
	guint process;
	int fd;
	unsigned int pdu_offset;
	const struct test_data *data;
};

#define event(args...)						\
	{							\
		.valid = true,					\
		.data = (void *) args,				\
		.size = sizeof(*args),				\
	}

#define define_test_device(name, function, device, args...)		\
	do {								\
		const struct test_pdu pdus[] = {			\
			args, { }					\
		};							\
		static struct test_data data;				\
		data.test_name = g_strdup(name);			\
		data.test_device = device;				\
		data.pdu_list = util_memdup(pdus, sizeof(pdus));	\
		tester_add(name, &data, NULL, function, NULL);		\
	} while (0)

#define define_test(name, function, args...)			\
	define_test_device(name, function, NULL, args)

static void test_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	tester_debug("%s%s\n", prefix, str);
}

static void test_free(gconstpointer user_data)
{
	const struct test_data *data = user_data;

	g_free(data->test_name);
	g_free(data->pdu_list);
}

static void destroy_context(struct context *context)
{
	if (context->source > 0)
		g_source_remove(context->source);

	bt_uhid_unregister_all(context->uhid);
	bt_uhid_unref(context->uhid);

	test_free(context->data);
	g_free(context);
}

static gboolean context_quit(gpointer user_data)
{
	struct context *context = user_data;

	if (context == NULL)
		return FALSE;

	if (context->process > 0)
		g_source_remove(context->process);

	destroy_context(context);
	tester_test_passed();

	return FALSE;
}

static gboolean send_pdu(gpointer user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	ssize_t len;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	len = write(context->fd, pdu->data, pdu->size);

	if (tester_use_debug())
		util_hexdump('<', pdu->data, len, test_debug, "uHID: ");

	g_assert_cmpint(len, ==, pdu->size);

	context->process = 0;
	return FALSE;
}

static void context_process(struct context *context)
{
	if (!context->data->pdu_list[context->pdu_offset].valid) {
		context_quit(context);
		return;
	}

	context->process = g_idle_add(send_pdu, context);
}

static gboolean test_handler(GIOChannel *channel, GIOCondition cond,
							gpointer user_data)
{
	struct context *context = user_data;
	const struct test_pdu *pdu;
	unsigned char buf[sizeof(struct uhid_event)];
	ssize_t len;
	int fd;

	pdu = &context->data->pdu_list[context->pdu_offset++];

	if (cond & (G_IO_NVAL | G_IO_ERR | G_IO_HUP)) {
		context->source = 0;
		g_print("%s: cond %x\n", __func__, cond);
		return FALSE;
	}

	fd = g_io_channel_unix_get_fd(channel);

	len = read(fd, buf, sizeof(buf));

	g_assert(len > 0);

	if (tester_use_debug())
		util_hexdump('>', buf, len, test_debug, "uHID: ");

	g_assert_cmpint(len, ==, pdu->size);

	g_assert(memcmp(buf, pdu->data, pdu->size) == 0);

	context_process(context);

	return TRUE;
}

static struct context *create_context(gconstpointer data)
{
	struct context *context = g_new0(struct context, 1);
	const struct test_data *test_data = data;
	GIOChannel *channel;
	int err, sv[2];
	uid_t uid = getuid();

	context->data = data;

	/* Device testings requires extra permissions in order to be able to
	 * create devices.
	 */
	if (test_data->test_device && !uid) {
		context->uhid = bt_uhid_new_default();
		if (!context->uhid) {
			tester_test_abort();
			context_quit(context);
			return NULL;
		}
		return context;
	}

	err = socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv);
	g_assert(err == 0);

	context->uhid = bt_uhid_new(sv[0]);
	g_assert(context->uhid != NULL);

	channel = g_io_channel_unix_new(sv[1]);

	g_io_channel_set_close_on_unref(channel, TRUE);
	g_io_channel_set_encoding(channel, NULL, NULL);
	g_io_channel_set_buffered(channel, FALSE);

	context->source = g_io_add_watch(channel,
				G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL,
				test_handler, context);
	g_assert(context->source > 0);

	g_io_channel_unref(channel);

	context->fd = sv[1];

	return context;
}

static const struct uhid_event ev_create = {
	.type = UHID_CREATE,
};

static const struct uhid_event ev_destroy = {
	.type = UHID_DESTROY,
};

static const struct uhid_event ev_feature_answer = {
	.type = UHID_FEATURE_ANSWER,
};

static const struct uhid_event ev_input = {
	.type = UHID_INPUT,
};

static const struct uhid_event ev_output = {
	.type = UHID_OUTPUT,
};

static const struct uhid_event ev_feature = {
	.type = UHID_FEATURE,
};

static void test_client(gconstpointer data)
{
	struct context *context = create_context(data);
	struct test_device *device;
	int err;

	if (!context)
		return;

	device = context->data->test_device;
	if (device)
		err = bt_uhid_create(context->uhid, device->name,
					BDADDR_ANY, BDADDR_ANY,
					device->vendor, device->product,
					device->version, device->country,
					device->type, device->map.iov_base,
					device->map.iov_len);
	else
		err = bt_uhid_create(context->uhid, "", NULL, NULL, 0, 0, 0, 0,
					BT_UHID_NONE, NULL, 0);

	if (err < 0) {
		tester_debug("create failed: %s\n", strerror(-err));
		tester_test_failed();
	}

	if (g_str_equal(context->data->test_name, "/uhid/command/destroy")) {
		err = bt_uhid_destroy(context->uhid, true);
		if (err < 0)
			tester_test_failed();
	}

	if (g_str_equal(context->data->test_name,
				"/uhid/command/feature_answer")) {
		err = bt_uhid_send(context->uhid, &ev_feature_answer);
		if (err < 0)
			tester_test_failed();
	}

	if (g_str_equal(context->data->test_name, "/uhid/command/input")) {
		err = bt_uhid_input(context->uhid, 0, NULL, 0);
		if (err < 0)
			tester_test_failed();
	}

	context_quit(context);
}

static void handle_output(struct uhid_event *ev, void *user_data)
{
	g_assert_cmpint(ev->type, ==, UHID_OUTPUT);

	context_quit(user_data);
}

static void handle_feature(struct uhid_event *ev, void *user_data)
{
	g_assert_cmpint(ev->type, ==, UHID_FEATURE);

	context_quit(user_data);
}

static void test_server(gconstpointer data)
{
	struct context *context = create_context(data);

	bt_uhid_register(context->uhid, UHID_OUTPUT, handle_output, context);
	bt_uhid_register(context->uhid, UHID_FEATURE, handle_feature, context);

	g_idle_add(send_pdu, context);
}


static struct test_device mx_anywhere_3 = {
	.name = "MX Anywhere 3",
	.vendor = 0x46D,
	.product = 0xB025,
	.version = 0x14,
	.country = 0x00,
	.type = BT_UHID_MOUSE,
	.map = UTIL_IOV_INIT(0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02,
				0x09, 0x01, 0xA1, 0x00, 0x95, 0x10, 0x75, 0x01,
				0x15, 0x00, 0x25, 0x01, 0x05, 0x09, 0x19, 0x01,
				0x29, 0x10, 0x81, 0x02, 0x05, 0x01, 0x16, 0x01,
				0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02,
				0x09, 0x30, 0x09, 0x31, 0x81, 0x06, 0x15, 0x81,
				0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x09, 0x38,
				0x81, 0x06, 0x95, 0x01, 0x05, 0x0C, 0x0A, 0x38,
				0x02, 0x81, 0x06, 0xC0, 0xC0, 0x06, 0x43, 0xFF,
				0x0A, 0x02, 0x02, 0xA1, 0x01, 0x85, 0x11, 0x75,
				0x08, 0x95, 0x13, 0x15, 0x00, 0x26, 0xFF, 0x00,
				0x09, 0x02, 0x81, 0x00, 0x09, 0x02, 0x91, 0x00,
				0xC0),
};

int main(int argc, char *argv[])
{
	tester_init(&argc, &argv);

	define_test("/uhid/command/create", test_client, event(&ev_create));
	define_test("/uhid/command/destroy", test_client, event(&ev_destroy));
	define_test("/uhid/command/feature_answer", test_client,
						event(&ev_feature_answer));
	define_test("/uhid/command/input", test_client, event(&ev_input));

	define_test("/uhid/event/output", test_server, event(&ev_output));
	define_test("/uhid/event/feature", test_server, event(&ev_feature));

	define_test_device("/uhid/device/mx_anywhere_3", test_client,
					&mx_anywhere_3, event(&ev_create));

	return tester_run();
}
