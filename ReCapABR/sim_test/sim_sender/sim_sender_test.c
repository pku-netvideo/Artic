/*-
* Copyright (c) 2017-2018 wenba, Inc.
*	All rights reserved.
*
* See the file LICENSE for redistribution information.
*/

/**************************************************************************
* sim_sender_test��һ��ģ����Ƶ���ݷ��͵Ĺ��̣����ڵ����շ�˫�˵���ȷ��,���ǽ���
* ��sim_transport֮�ϵģ�������������razor��sim_transport����
**************************************************************************/
#include "cf_platform.h"
#include "cf_list.h"
#include "sim_external.h"
#include "audio_log.h"

# include "confidence_c_api.h"

#include <time.h>
#include <assert.h>

enum
{
	el_change_bitrate,
	el_pause,
	el_resume,

	el_connect,
	el_disconnect,
	el_timeout,

	el_unknown = 1000
};

typedef struct
{
	int			msg_id;
	uint32_t	val;
}thread_msg_t;

static base_list_t* main_queue = NULL;
su_mutex main_mutex;


static void notify_callback(void* event, int type, uint32_t val)
{
	thread_msg_t* msg = (thread_msg_t*)calloc(1, sizeof(thread_msg_t));
	msg->msg_id = el_unknown;

	switch (type){
	case sim_connect_notify:
		msg->msg_id = el_connect;
		msg->val = val;
		break;

	case sim_network_timout:
		msg->msg_id = el_timeout;
		msg->val = val;
		break;

	case sim_disconnect_notify:
		msg->msg_id = el_disconnect;
		msg->val = val;
		break;

	case net_interrupt_notify:
		msg->msg_id = el_pause;
		msg->val = val;
		break;

	case net_recover_notify:
		msg->msg_id = el_resume;
		msg->val = val;
		break;

	default:
		free(msg);
		return;
	}

	su_mutex_lock(main_mutex);
	list_push(main_queue, msg);
	su_mutex_unlock(main_mutex);
}

static void notify_change_bitrate(void* event, uint32_t bitrate_kbps, int lost)
{
	thread_msg_t* msg = (thread_msg_t*)calloc(1, sizeof(thread_msg_t));
	msg->msg_id = el_change_bitrate;
	msg->val = bitrate_kbps;
	/*����Ϣ�ݵ����߳���*/
	su_mutex_lock(main_mutex);
	list_push(main_queue, msg);
	su_mutex_unlock(main_mutex);
}

static char g_info[1024] = { 0 };
static uint64_t sender_logs[600][10];
int sender_log_idx;

static void notify_state(void* event, const char* info)
{
	strcpy(g_info, info);
}

#define MAX_SEND_BITRATE (4000 * 8 * 1000)
#define MIN_SEND_BITRATE (20 * 8 * 1000)
#define START_SEND_BITRATE (140 * 8 * 1000)

typedef struct
{
	uint32_t	bitrate_kbps;	/*��ǰ���͵����ʣ�kbps*/
	int			record_flag;	/*�Ƿ���Կ�ʼ¼�Ʒ���*/
	uint32_t	frame_rate;		/*֡��*/
	int64_t		prev_ts;		/*��һ�η�����Ƶ��ʱ��*/
	int64_t		hb_ts;

	uint32_t	total_bytes;
	int			index;
	uint8_t*	frame;

}video_sender_t;

#define FRAME_SIZE (1024 * 1024 * 2)
#define MAX_DATA_SIZE 1024 * 1024

static int logging = 1;
static uint32_t last_bitrate;

# define UPPER_BOUND_DEFAULT 4000
# define ARTIC true
# define SET_MARGIN_BW false
# define DO_CAP_BITRATE false
// ARTIC, SET_MARGIN and CAP have at most 1 true
# define CAP_BITRATE 1000
# define MARGIN_BW 200

static void try_send_video(video_sender_t* sender,  confidence_handle_t confidence_handler, uint64_t now_ts)
{
	uint8_t* pos = sender->frame, ftype;
	int64_t space;
	size_t frame_size = 0;
	if (sender->record_flag == 0)
		return;

	if (now_ts >= sender->prev_ts + 1000 / sender->frame_rate){
		if ((ARTIC && SET_MARGIN_BW) || (ARTIC && DO_CAP_BITRATE) || (SET_MARGIN_BW && DO_CAP_BITRATE))
			print("!!! WARNING: among ARTIC, setting BW cap and setting BW margin, at most 1 mode is selected !!!")

		uint32_t suggested_bitrate = sender->bitrate_kbps; // The suggested br of gcc or bbr
		uint32_t bitrate_upper_bound = DO_CAP_BITRATE ? CAP_BITRATE : UPPER_BOUND_DEFAULT
		if(ARTIC)
			bitrate_upper_bound = (uint32_t)confidence_adjust(confidence_handler, sender->index, last_bitrate, suggest_bitrate, 0.8);
		uint32_t final_bitrate = suggested_bitrate > bitrate_upper_bound ? bitrate_upper_bound : suggested_bitrate;
		if (SET_MARGIN_BW) {
			if (final_bitrate < 400) final_bitrate += MARGIN_BW;
			final_bitrate -= MARGIN_BW;
		}

		space = (now_ts - sender->prev_ts);
		frame_size = final_bitrate / 8 * space;
		last_bitrate = final_bitrate;
		sender->prev_ts = now_ts;
		/*
		if (frame_size > 200){
			frame_size -= 200;
			frame_size = frame_size + rand() % 400;
		}*/

		frame_size = frame_size > MAX_DATA_SIZE ? MAX_DATA_SIZE : frame_size;

		sender_logs[sender_log_idx][0] = (uint64_t)(sender->index);
		sender_logs[sender_log_idx][1] = now_ts;
		sender_logs[sender_log_idx][2] = (uint64_t)space;
		sender_logs[sender_log_idx][3] = (uint64_t)(sender->bitrate_kbps);
		sender_logs[sender_log_idx][4] = (uint64_t)frame_size;
		sender_logs[sender_log_idx][5] = (uint64_t)bound;
		if (logging) printf("%lu\n", (uint64_t)(sender->index));
		++sender_log_idx;

		memcpy(pos, &frame_size, sizeof(frame_size));
		pos += sizeof(frame_size);
		memcpy(pos, &sender->index, sizeof(sender->index));
		pos += sizeof(sender->index);
		memcpy(pos, &now_ts, sizeof(now_ts));
		pos += sizeof(now_ts);
		ftype = 0;
		if (sender->index % (sender->frame_rate * 4) == 0) /*�ؼ�֡*/
			ftype = 1;

		sim_send_video(0, ftype, sender->frame, frame_size);

		++sender->index;
		/*ֻ����һ֡��һ��*/
		/*if (++sender->index > 20000)
			sender->record_flag = 0;*/
	}
}

static void main_loop_event()
{
	confidence_handle_t hdl = confidence_create(150);
	confidence_load_csv(hdl, "confidence.csv", 1);

	FILE *file_send_log = fopen("logs_send.txt", "w");
	sender_log_idx = 0;

	last_bitrate = 4000;

	video_sender_t sender = {0};

	thread_msg_t* msg;
	int run = 1;
	int disconnecting = 0;
	sender.frame = (uint8_t*)malloc(FRAME_SIZE);
	int64_t prev_ts, now_ts, start_ts, end_ts;

	prev_ts = now_ts = start_ts = GET_SYS_MS();
	end_ts = start_ts + ((int64_t)1000) * (13 * 60 + 30 + 2);

	while (run){
		su_mutex_lock(main_mutex);
		if (main_queue->size > 0){
			msg = (thread_msg_t*)list_front(main_queue);
			list_pop(main_queue);
			
			su_mutex_unlock(main_mutex);

			switch (msg->msg_id){
			case el_connect:
				if (msg->val == 0){
					printf("connect success!\n");
					sender.record_flag = 1;
					sender.total_bytes = 0;
					sender.frame_rate = 30;
					sender.hb_ts = sender.prev_ts = GET_SYS_MS();
					sender.bitrate_kbps = START_SEND_BITRATE / 1000;
				}
				else{
					printf("connect failed, result = %u!\n", msg->val);
					run = 0;
					sender.record_flag = 0;
				}
				break;

			case el_timeout:
				printf("network timeout!\n");
				run = 0;
				sender.record_flag = 0;
				break;

			case el_disconnect:
				printf("connect failed!\n");
				run = 0;
				sender.record_flag = 0;
				break;

			case el_pause:
				printf("pause sender!\n");
				sender.record_flag = 0;
				break;

			case el_resume:
				printf("resume sender!\n");
				sender.record_flag = 1;
				break;

			case el_change_bitrate:
				if (msg->val <= MAX_SEND_BITRATE / 1000){
					sender.bitrate_kbps = msg->val;
				}
				else{
					sender.bitrate_kbps = MAX_SEND_BITRATE / 1000;
				}

				//printf("set bytes rate = %ukb/s\n", sender.bitrate_kbps / 8);
				break;
			}
			free(msg);
		}
		else{
			su_mutex_unlock(main_mutex);
		}
	
		now_ts = GET_SYS_MS();
		if (now_ts > end_ts) break;

		if (now_ts >= 1000 + prev_ts) logging = 1; else logging = 0;
		try_send_video(&sender, hdl, now_ts);

		if (now_ts >= 1000 + prev_ts){
			// printf(",    Frame id: %d\n", sender.index);
			prev_ts = now_ts;
		}
		if (sender_log_idx >= 50){
			sender_log_idx = 0;
			for (int Id = 0; Id < 50; ++Id)
				fprintf(file_send_log,
					"FrameId: %lu,  Timestamp: %lu,  space: %lu,  bitrate: %lu,  bitrate bound: %lu,  frame size: %lu\n",
					sender_logs[Id][0],
					sender_logs[Id][1],
					sender_logs[Id][2],
					sender_logs[Id][3],
					sender_logs[Id][5],
					sender_logs[Id][4]
				);
		}

		su_sleep(0, 2000);
		if (sender.index >= 20000 && disconnecting == 0){
			sim_disconnect();
			disconnecting = 1;
		}
	}
	for (int Id = 0; Id < sender_log_idx; ++Id)
		fprintf(file_send_log,
			"FrameId: %lu,  Timestamp: %lu,  space: %lu,  bitrate: %lu,  bitrate bound: %lu,  frame size: %lu\n",
			sender_logs[Id][0],
			sender_logs[Id][1],
			sender_logs[Id][2],
			sender_logs[Id][3],
			sender_logs[Id][5],
			sender_logs[Id][4]
		);

	fclose(file_send_log);
	free(sender.frame);
}

# define USE_GCC_NOT_BBR false
int main(int argc, const char* argv[])
{

	srand((uint32_t)time(NULL));

	if (open_win_log("sender.log") != 0){
		assert(0);
		return -1;
	}

	main_mutex = su_create_mutex();
	main_queue = create_list();

	sim_init(16000, NULL, log_win_write, notify_callback, notify_change_bitrate, notify_state);
	sim_set_bitrates(MIN_SEND_BITRATE, START_SEND_BITRATE, MAX_SEND_BITRATE * 5/4);

	if (sim_connect(1000, "10.0.0.1", 16001, USE_GCC_NOT_BBR ? gcc_transpost : bbr_transport, 0, 0) != 0){ // mahi-mahi assigns this IP to the virtual network interface.
	// if (sim_connect(1000, "127.0.0.1", 16001, gcc_transport, 0, 0) != 0){ // without mahi-mahi
		printf("sim connect failed!\n");
		goto err;
	}

	main_loop_event();

err:
	sim_destroy();
	su_destroy_mutex(main_mutex);
	destroy_list(main_queue);
	close_win_log();

	return 0;
}