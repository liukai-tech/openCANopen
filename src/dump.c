/* Copyright (c) 2014-2017, Marel
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
 * OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

#include <stdio.h>
#include <unistd.h>

#include "socketcan.h"
#include "canopen.h"
#include "canopen/nmt.h"
#include "canopen/sdo.h"
#include "canopen/network.h"
#include "canopen/heartbeat.h"
#include "canopen/emcy.h"
#include "canopen/dump.h"
#include "net-util.h"
#include "sock.h"
#include "canopen/sdo-dict.h"
#include "vector.h"
#include "canopen/error.h"
#include "time-utils.h"
#include "trace-buffer.h"

#ifndef CAN_MAX_DLC
#define CAN_MAX_DLC 8
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))

#define printx(cf, fmt, ...) \
	printf(fmt "%s\n", ## __VA_ARGS__, (cf)->can_id & CAN_RTR_FLAG ? " [RTR]" : "")

struct node_state {
	uint32_t current_mux;
	struct vector sdo_data;
	int device_type;
};

static enum co_dump_options options_ = 0;
static struct node_state node_state_[127] = { 0 };
static uint64_t current_time_ = 0;

char* strlcpy(char* dst, const char* src, size_t size);
const char* hexdump(const void* data, size_t size);

static inline void print_ts(void)
{
	uint64_t t = current_time_;

	if (options_ & CO_DUMP_TIMESTAMP)
		printf("%llu.%06llu ", t / 1000000ULL, t % 1000000ULL);
}

static inline struct node_state* get_node_state(int nodeid)
{
	return 0 < nodeid && nodeid <= 127 ? &node_state_[nodeid - 1] : NULL;
}

static void node_state_init(void)
{
	for (int i = 0; i < 127; ++i)
		vector_init(&node_state_[i].sdo_data, 8);
}

static const char* nmt_cs_str(enum nmt_cs cs)
{
	switch (cs) {
	case NMT_CS_START:			return "start";
	case NMT_CS_STOP:			return "stop";
	case NMT_CS_ENTER_PREOPERATIONAL:	return "enter-preoperational";
	case NMT_CS_RESET_NODE:			return "reset-node";
	case NMT_CS_RESET_COMMUNICATION:	return "reset-communication";
	default:				return "unknown";
	}

	abort();
	return NULL;
}

static int dump_nmt(struct can_frame* cf)
{
	if (!(options_ & CO_DUMP_FILTER_NMT))
		return 0;

	int nodeid = nmt_get_nodeid(cf);
	enum nmt_cs cs = nmt_get_cs(cf);

	print_ts();

	if (nodeid == 0)
		printx(cf, "NMT ALL %s", nmt_cs_str(cs));
	else
		printx(cf, "NMT %d %s", nodeid, nmt_cs_str(cs));

	return 0;
}

static int dump_sync(struct can_frame* cf)
{
	if (!(options_ & CO_DUMP_FILTER_SYNC))
		return 0;

	(void)cf;

	print_ts();
	printx(cf, "SYNC");

	return 0;
}

static int dump_timestamp(struct can_frame* cf)
{
	if (!(options_ & CO_DUMP_FILTER_TIMESTAMP))
		return 0;

	(void)cf;

	print_ts();
	printx(cf, "TIMESTAMP TODO");

	return 0;
}

static int dump_emcy(struct canopen_msg* msg, struct can_frame* cf)
{
	if (!(options_ & CO_DUMP_FILTER_EMCY))
		return 0;

	print_ts();

	if (cf->can_dlc == 0) {
		printx(cf, "EMCY %d EMPTY", msg->id);
		return 0;
	}

	struct node_state* state = get_node_state(msg->id);

	unsigned int code = emcy_get_code(cf);
	unsigned int register_ = emcy_get_register(cf);
	uint64_t manufacturer_error = emcy_get_manufacturer_error(cf);
	printx(cf, "EMCY %d code=%#x,register=%#x,manufacturer-error=%#llx,dlc=%d,text=\"%s\"",
	       msg->id, code, register_, manufacturer_error, cf->can_dlc,
	       error_code_to_string(code, state->device_type & 0xffff));

	return 0;
}

static int is_pdo_in_filter(int n)
{
	return options_ & (1 << (n + CO_DUMP_PDO_FILTER_SHIFT - 1));
}

static int dump_pdo(int type, int n, struct canopen_msg* msg,
		    struct can_frame* cf)
{
	if (!is_pdo_in_filter(n))
		return 0;

	print_ts();

	printx(cf, "%cPDO%d %d length=%d,data=%s", type, n, msg->id,
	       cf->can_dlc, hexdump(cf->data, cf->can_dlc));

	return 0;
}

static size_t get_expediated_size(const struct can_frame* cf)
{
	size_t max_size = cf->can_dlc - SDO_EXPEDIATED_DATA_IDX;

	return sdo_is_size_indicated(cf)
	     ? MIN(sdo_get_expediated_size(cf), max_size) : max_size;
}

static int dump_sdo_dl_init_req(struct canopen_msg* msg, struct can_frame* cf)
{
	struct node_state* state = get_node_state(msg->id);

	int is_expediated = sdo_is_expediated(cf);
	int is_size_indicated = sdo_is_size_indicated(cf);

	int index = sdo_get_index(cf);
	int subindex = sdo_get_subindex(cf);

	if (state && !is_expediated)
		state->current_mux = SDO_MUX(index, subindex);

	print_ts();

	printf("RSDO %d init-download-%s index=%x,subindex=%d", msg->id,
	       is_expediated ? "expediated" : "segment", index, subindex);

	if (!is_expediated && is_size_indicated && cf->can_dlc == CAN_MAX_DLC) {
		size_t size = sdo_get_indicated_size(cf);
		printx(cf, ",size=%d", size);
		if (state) {
			vector_reserve(&state->sdo_data, size);
			vector_clear(&state->sdo_data);
		}
	} else if (is_expediated) {
		size_t size = get_expediated_size(cf);
		printx(cf, ",size=%d,data=%s", size,
		       hexdump(&cf->data[SDO_EXPEDIATED_DATA_IDX], size));
	}

	return 0;
}

static struct vector string_buffer_;

static char* make_string(const char* str, size_t size)
{
	vector_assign(&string_buffer_, "\"", 1);
	vector_append(&string_buffer_, str, size);
	vector_append(&string_buffer_, "\"", 2);
	return string_buffer_.data;
}

static const char* get_segment_data(const struct node_state* state,
				    const void* data, size_t size)
{
	if (state && sdo_dict_type(state->current_mux) == CANOPEN_VISIBLE_STRING)
		return make_string(data, size);

	return hexdump(data, size);
}

static int dump_sdo_dl_seg_req(struct canopen_msg* msg, struct can_frame* cf)
{
	struct node_state* state = get_node_state(msg->id);

	int is_end = sdo_is_end_segment(cf);

	const void* data = &cf->data[SDO_SEGMENT_IDX];
	size_t size = cf->can_dlc - SDO_SEGMENT_IDX;

	if (state)
		vector_append(&state->sdo_data, data, size);

	print_ts();

	printf("RSDO %d download-segment%s size=%d,data=%s", msg->id,
	       is_end ? "-end" : "", size, get_segment_data(state, data, size));

	if (state && is_end) {
		const void* final_data = state->sdo_data.data;
		size_t final_size = state->sdo_data.index;

		printf(",final-size=%d,final-data=%s", final_size,
		       get_segment_data(state, final_data, final_size));

		state->current_mux = 0;
	}

	printx(cf, "");
	return 0;
}

static int dump_sdo_ul_init_req(struct canopen_msg* msg, struct can_frame* cf)
{
	int index = sdo_get_index(cf);
	int subindex = sdo_get_subindex(cf);

	print_ts();
	printx(cf, "RSDO %d init-upload-segment index=%x,subindex=%d", msg->id,
	       index, subindex);

	return 0;

}

static int dump_sdo_ul_seg_req(struct canopen_msg* msg, struct can_frame* cf)
{
	print_ts();
	printx(cf, "RSDO %d upload-segment", msg->id);

	return 0;
}

static int dump_sdo_abort(int type, struct canopen_msg* msg,
			  struct can_frame* cf)
{
	int index = sdo_get_index(cf);
	int subindex = sdo_get_subindex(cf);

	const char* reason = sdo_strerror(sdo_get_abort_code(cf));

	print_ts();
	printx(cf, "%cSDO %d abort index=%x,subindex=%d,reason=\"%s\"", type,
	       msg->id, index, subindex, reason);
	return 0;
}

static int dump_rsdo(struct canopen_msg* msg, struct can_frame* cf)
{
	if (!(options_ & CO_DUMP_FILTER_SDO))
		return 0;

	enum sdo_ccs cs = sdo_get_cs(cf);

	switch (cs)
	{
	case SDO_CCS_DL_INIT_REQ: return dump_sdo_dl_init_req(msg, cf);
	case SDO_CCS_DL_SEG_REQ: return dump_sdo_dl_seg_req(msg, cf);
	case SDO_CCS_UL_INIT_REQ: return dump_sdo_ul_init_req(msg, cf);
	case SDO_CCS_UL_SEG_REQ: return dump_sdo_ul_seg_req(msg, cf);
	case SDO_CCS_ABORT: return dump_sdo_abort('R', msg, cf);
	default:
		print_ts();
		printx(cf, "RSDO %d unknown-command-specifier", msg->id);
	}

	return 0;
}

static int dump_sdo_ul_init_res(struct canopen_msg* msg, struct can_frame* cf)
{
	struct node_state* state = get_node_state(msg->id);

	int is_expediated = sdo_is_expediated(cf);
	int is_size_indicated = sdo_is_size_indicated(cf);

	int index = sdo_get_index(cf);
	int subindex = sdo_get_subindex(cf);

	if (state && !is_expediated)
		state->current_mux = SDO_MUX(index, subindex);

	print_ts();

	printf("TSDO %d init-upload-%s index=%x,subindex=%d", msg->id,
	       is_expediated ? "expediated" : "segment", index, subindex);

	if (!is_expediated && is_size_indicated && cf->can_dlc == CAN_MAX_DLC) {
		size_t size = sdo_get_indicated_size(cf);
		printx(cf, ",size=%d", size);
		if (state) {
			vector_reserve(&state->sdo_data, size);
			vector_clear(&state->sdo_data);
		}
	} else if (is_expediated) {
		size_t size = get_expediated_size(cf);
		const void* payload = &cf->data[SDO_EXPEDIATED_DATA_IDX];

		if (index == 0x1000 && subindex == 0)
			byteorder2(&state->device_type, payload,
				   sizeof(state->device_type),
				   MIN(sizeof(state->device_type), size));

		printx(cf, ",size=%d,data=%s", size, hexdump(payload, size));
	}

	return 0;
}

static int dump_sdo_ul_seg_res(struct canopen_msg* msg, struct can_frame* cf)
{
	struct node_state* state = get_node_state(msg->id);

	int is_end = sdo_is_end_segment(cf);

	const void* data = &cf->data[SDO_SEGMENT_IDX];
	size_t size = cf->can_dlc - SDO_SEGMENT_IDX;

	if (state)
		vector_append(&state->sdo_data, data, size);

	print_ts();

	printf("TSDO %d upload-segment%s size=%d,data=%s", msg->id,
	       is_end ? "-end" : "", size, get_segment_data(state, data, size));

	if (state && is_end) {
		const void* final_data = state->sdo_data.data;
		size_t final_size = state->sdo_data.index;

		printf(",final-size=%d,final-data=%s", final_size,
		       get_segment_data(state, final_data, final_size));
	}

	printx(cf, "");
	return 0;
}

static int dump_sdo_dl_init_res(struct canopen_msg* msg, struct can_frame* cf)
{
	print_ts();
	printx(cf, "TSDO %d init-download-segment", msg->id);

	return 0;

}

static int dump_sdo_dl_seg_res(struct canopen_msg* msg, struct can_frame* cf)
{
	int is_end = sdo_is_end_segment(cf);

	print_ts();
	printx(cf, "TSDO %d download-segment%s", msg->id, is_end ? "-end" : "");

	return 0;
}

static int dump_tsdo(struct canopen_msg* msg, struct can_frame* cf)
{
	if (!(options_ & CO_DUMP_FILTER_SDO))
		return 0;

	enum sdo_scs cs = sdo_get_cs(cf);

	switch (cs) {
	case SDO_SCS_DL_INIT_RES: return dump_sdo_dl_init_res(msg, cf);
	case SDO_SCS_DL_SEG_RES: return dump_sdo_dl_seg_res(msg, cf);
	case SDO_SCS_UL_INIT_RES: return dump_sdo_ul_init_res(msg, cf);
	case SDO_SCS_UL_SEG_RES: return dump_sdo_ul_seg_res(msg, cf);
	case SDO_SCS_ABORT: return dump_sdo_abort('T', msg, cf);
	default:
		print_ts();
		printx(cf, "TSDO %d unknown-command-specifier", msg->id);
	}

	return 0;
}

static const char* state_str(enum nmt_state state)
{
	switch (state) {
	case NMT_STATE_STOPPED: return "stopped";
	case NMT_STATE_OPERATIONAL: return "operational";
	case NMT_STATE_PREOPERATIONAL: return "pre-operational";
	default: return "UNKNOWN";
	}

	abort();
	return NULL;
}

static int dump_heartbeat(struct canopen_msg* msg, struct can_frame* cf)
{
	if (!(options_ & CO_DUMP_FILTER_HEARTBEAT))
		return 0;

	enum nmt_state state = heartbeat_get_state(cf);

	print_ts();

	if (heartbeat_is_bootup(cf)) {
		printx(cf, "HEARTBEAT %d bootup", msg->id);
	} else if (state == 1) {
		printx(cf, "HEARTBEAT %d poll", msg->id);
	} else {
		printx(cf, "HEARTBEAT %d state=%s", msg->id, state_str(state));
	}

	return 0;
}

static int multiplex(struct can_frame* cf)
{
	struct canopen_msg msg;

	if (canopen_get_object_type(&msg, cf) != 0)
		return -1;

	switch (msg.object) {
	case CANOPEN_NMT: return dump_nmt(cf);
	case CANOPEN_SYNC: return dump_sync(cf);
	case CANOPEN_TIMESTAMP: return dump_timestamp(cf);
	case CANOPEN_EMCY: return dump_emcy(&msg, cf);
	case CANOPEN_TPDO1: return dump_pdo('T', 1, &msg, cf);
	case CANOPEN_TPDO2: return dump_pdo('T', 2, &msg, cf);
	case CANOPEN_TPDO3: return dump_pdo('T', 3, &msg, cf);
	case CANOPEN_TPDO4: return dump_pdo('T', 4, &msg, cf);
	case CANOPEN_RPDO1: return dump_pdo('R', 1, &msg, cf);
	case CANOPEN_RPDO2: return dump_pdo('R', 2, &msg, cf);
	case CANOPEN_RPDO3: return dump_pdo('R', 3, &msg, cf);
	case CANOPEN_RPDO4: return dump_pdo('R', 4, &msg, cf);
	case CANOPEN_TSDO: return dump_tsdo(&msg, cf);
	case CANOPEN_RSDO: return dump_rsdo(&msg, cf);
	case CANOPEN_HEARTBEAT: return dump_heartbeat(&msg, cf);
	default:
		break;
	}

	abort();
	return -1;
}

static void run_dumper(struct sock* sock)
{
	struct can_frame cf;

	while (1) {
		memset(&cf, 0, sizeof(cf));

		if (sock_recv(sock, &cf, MSG_WAITALL) <= 0)
			break;

		current_time_ = gettime_us(CLOCK_REALTIME);
		multiplex(&cf);
	}
}

static void resolve_filters(enum co_dump_options options)
{
	options_ |= options & ~CO_DUMP_FILTER_MASK;

	options_ |= options & CO_DUMP_FILTER_MASK
		  ? options & CO_DUMP_FILTER_MASK
		  : CO_DUMP_FILTER_MASK;
}

static int dump_file(const char* path, enum co_dump_options options)
{
	FILE* stream = fopen(path, "r");
	if (!stream)
		return -1;

	struct tb_frame frame;
	while (fread(&frame, sizeof(frame), 1, stream)) {
		current_time_ = frame.timestamp;
		multiplex(&frame.cf);
	}

	fclose(stream);
	return 0;
}

__attribute__((visibility("default")))
int co_dump(const char* addr, enum co_dump_options options)
{
	vector_init(&string_buffer_, 256);
	node_state_init();

	resolve_filters(options);

	if (options & CO_DUMP_FILE) {
		if (dump_file(addr, options) < 0) {
			perror("Could not read file");
			return 1;
		}

		return 0;
	}

	struct sock sock;
	enum sock_type type = options & CO_DUMP_TCP ? SOCK_TYPE_TCP
						    : SOCK_TYPE_CAN;
	if (sock_open(&sock, type, addr, NULL) < 0) {
		perror("Could not open CAN bus");
		return 1;
	}

	if (type == SOCK_TYPE_CAN)
		net_fix_sndbuf(sock.fd);

	run_dumper(&sock);

	sock_close(&sock);
	return 0;
}
