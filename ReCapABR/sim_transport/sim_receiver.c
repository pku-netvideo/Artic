/*-
* Copyright (c) 2017-2018 Razor, Inc.
*	All rights reserved.
*
* See the file LICENSE for redistribution information.
*/

#include "sim_internal.h"
#include <assert.h>

#define CACHE_SIZE 1024
#define INDEX(i)	((i) % CACHE_SIZE)
#define MAX_EVICT_DELAY_MS 6000
#define MIN_EVICT_DELAY_MS 3000

static void sim_receiver_send_fir(sim_session_t* s, sim_receiver_t* r);

/************************************************���Ż������Ķ���**********************************************************/
static sim_frame_cache_t* open_real_video_cache(sim_session_t* s)
{
	sim_frame_cache_t* cache = calloc(1, sizeof(sim_frame_cache_t));
	cache->wait_timer = s->rtt + 2 * s->rtt_var;
	cache->state = buffer_waiting;
	cache->min_seq = 0;
	cache->frame_timer = 100;
	cache->f = 1.0f;
	cache->size = CACHE_SIZE;
	cache->frames = calloc(CACHE_SIZE, sizeof(sim_frame_t));

	cache->discard_loss = skiplist_create(idu32_compare, NULL, NULL);
	return cache;
}

static inline void real_video_clean_frame(sim_session_t* session, sim_frame_cache_t* c, sim_frame_t* frame)
{
	skiplist_iter_t* iter;

	int i;
	if (frame->seg_number == 0)
		return;

	for (i = 0; i < frame->seg_number; ++i){
		if (frame->segments[i] != NULL){
			c->min_seq = frame->segments[i]->packet_id - frame->segments[i]->index + frame->segments[i]->total - 1;
			free(frame->segments[i]);
			frame->segments[i] = NULL;
		}
	}

	free(frame->segments);

	c->play_frame_ts = frame->ts;
	frame->ts = 0;
	frame->frame_type = 0;
	frame->seg_number = 0;
	frame->seg_count = 0;

	c->min_fid = frame->fid;

	while (skiplist_size(c->discard_loss) > 0){
		iter = skiplist_first(c->discard_loss);
		if (iter->key.u32 <= c->min_seq)
			skiplist_remove(c->discard_loss, iter->key);
		else
			break;
	}
}

static int evict_gop_frame(sim_session_t* s, sim_frame_cache_t* c)
{
	uint32_t i, key_frame_id;
	sim_frame_t* frame = NULL;

	key_frame_id = 0;
	/*��һ֡ȷ�������𣬴ӵڶ�֡��ʼ���*/
	for (i = c->min_fid + 2; i < c->max_fid; ++i){
		frame = &c->frames[INDEX(i)];
		if (frame->frame_type == 1){
			key_frame_id = i;
			break;
		}
	}

	if (key_frame_id == 0)
		return -1;

	sim_debug("evict_gop_frame, from frame = %u, to frame = %u!! \n", c->min_fid + 1, key_frame_id);
	for (i = c->min_fid + 1; i < key_frame_id; i++){
		c->frame_ts = c->frames[INDEX(i)].ts;
		real_video_clean_frame(s, c, &c->frames[INDEX(i)]);
	}

	c->min_fid = key_frame_id - 1;

	return 0;
}

# define max_recv_num  100000
static uint64_t max_recv_id = 0;
static uint8_t RecvCode[max_recv_num];
static int receiver_log_idx = 0, is_recv_log_file_built = 0;
static uint64_t receiver_logs[100][10];

static inline int real_video_cache_check_frame_full(sim_session_t* s, sim_frame_t* frame)
{
	if (!is_recv_log_file_built){
		is_recv_log_file_built = 1;
		FILE *file_recv_log = fopen("logs_recv.txt","w");
		fclose(file_recv_log);
	}

	int is_frame_full = frame->seg_number == frame->seg_count;
	uint64_t Fid = frame->fid;
	while (max_recv_id <= Fid)
		RecvCode[max_recv_id++] = 0;

	if (is_frame_full && ((RecvCode[Fid] & 1) == 0)){
		RecvCode[Fid] |= 1;
		uint64_t first_arrive = (uint64_t)(GET_SYS_MS());

		uint8_t *start_of_frame = frame->segments[0]->data;
		size_t frame_size;
		uint64_t sentTime;
		uint32_t bitrate;
		int FrameId;

		memcpy(&frame_size, start_of_frame, sizeof(size_t));
		start_of_frame += sizeof(size_t);
		memcpy(&FrameId, start_of_frame, sizeof(int));
		start_of_frame += sizeof(int);
		memcpy(&sentTime, start_of_frame, sizeof(uint64_t));
		start_of_frame += sizeof(uint64_t);
		memcpy(&bitrate, start_of_frame, sizeof(uint32_t));
		start_of_frame += sizeof(uint32_t);

		receiver_logs[receiver_log_idx][0] = (uint64_t)FrameId;
		receiver_logs[receiver_log_idx][1] = sentTime;
		receiver_logs[receiver_log_idx][2] = first_arrive;
		receiver_logs[receiver_log_idx][3] = first_arrive - sentTime;
		receiver_logs[receiver_log_idx][4] = (uint64_t)frame_size;
		receiver_logs[receiver_log_idx][5] = (uint64_t)Fid;
		++receiver_log_idx;
	}

	if (receiver_log_idx == 50){
		FILE *file_recv_log = fopen("logs_recv.txt", "a");
		receiver_log_idx = 0;
		for(int Idx = 0; Idx < 50; ++Idx)
			fprintf(file_recv_log,
				"Timestamp: %lu,  SentTime: %lu,  OWD: %lu,  frame size: %lu,  FrameId: %lu,  Fid: %lu\n",
				receiver_logs[Idx][2],
				receiver_logs[Idx][1],
				receiver_logs[Idx][3],
				receiver_logs[Idx][4],
				receiver_logs[Idx][0],
				receiver_logs[Idx][5]
			);
		fclose(file_recv_log);
	}

	return is_frame_full ? 0 : -1;
}

static void real_video_cache_evict_discard(sim_session_t* s, sim_frame_cache_t* c)
{
	uint32_t pos, missing_seq, i, j;
	sim_frame_t* frame;
	skiplist_item_t key;

	if (c->min_fid == c->max_fid)
		return;

	missing_seq = 0;

	pos = INDEX(c->min_fid + 1);
	frame = &c->frames[pos];
	if ((c->min_fid + 1 == frame->fid || frame->frame_type == 1) && real_video_cache_check_frame_full(s, frame) == 0)
		return;

	for (i = c->min_fid + 1; i <= c->max_fid; ++i){
		frame = &c->frames[INDEX(i)];
		if (frame->seg_number <= 0)
			continue;

		for (j = 0; j < frame->seg_number; ++j){
			if (frame->segments[j] != NULL){
				missing_seq = frame->segments[j]->packet_id + frame->segments[j]->total - frame->segments[j]->index;
				goto evict_lab;
			}
		}
	}

evict_lab:
	for (i = c->min_seq + 1; i <= missing_seq; ++i){
		key.u32 = i;
		if (skiplist_search(c->discard_loss, key) != NULL){
			if(evict_gop_frame(s, c) != 0)
				sim_receiver_send_fir(s, s->receiver);
			break;
		}
	}
}

static void real_video_cache_discard(sim_session_t* s, sim_frame_cache_t* c, uint32_t seq)
{
	skiplist_item_t key, val;

	if (seq <= c->min_seq)
		return;

	key.u32 = val.u32 = seq;
	skiplist_insert(c->discard_loss, key, val);

	real_video_cache_evict_discard(s, c);
}

static void close_real_video_cache(sim_session_t* s, sim_frame_cache_t* cache)
{
	uint32_t i;
	for (i = 0; i < cache->size; ++i)
		real_video_clean_frame(s, cache, &cache->frames[i]);

	skiplist_destroy(cache->discard_loss);

	free(cache->frames);
	free(cache);
}

static void reset_real_video_cache(sim_session_t* s, sim_frame_cache_t* cache)
{
	uint32_t i;

	for (i = 0; i < cache->size; ++i)
		real_video_clean_frame(s, cache, &cache->frames[i]);

	cache->min_seq = 0;
	cache->min_fid = 0;
	cache->max_fid = 0;
	cache->play_ts = 0;
	cache->frame_ts = 0;
	cache->max_ts = 100;
	cache->frame_timer = 100;
	cache->f = 1.0f;

	cache->state = buffer_waiting;
	cache->wait_timer = SU_MAX(100, s->rtt + 2 * s->rtt_var);
	cache->loss_flag = 0;

	skiplist_clear(cache->discard_loss);
}

static void real_video_evict_frame(sim_session_t* s, sim_frame_cache_t* c, uint32_t fid)
{
	uint32_t pos, i;

	for (pos = c->max_fid + 1; pos <= fid; pos++)
		real_video_clean_frame(s, c, &c->frames[INDEX(pos)]);

	if (fid < c->min_fid + c->size)
		return;

	for (pos = c->min_fid + 1; pos < c->max_fid; ++pos){
		if (c->frames[INDEX(pos)].frame_type == 1)
			break;
	}

	for (i = c->min_fid + 1; i < pos; ++i)
		real_video_clean_frame(s, c, &c->frames[INDEX(i)]);
}

static int real_video_cache_put(sim_session_t* s, sim_frame_cache_t* c, sim_segment_t* seg)
{
	sim_frame_t* frame;
	int ret = -1;

	if (seg->index >= seg->total){
		assert(0);
		return ret;
	}
	else if (seg->fid <= c->min_fid)
		return ret;

	if (seg->fid > c->max_fid){
		if (c->max_fid > 0)
			real_video_evict_frame(s, c, seg->fid); /*���й��ڲ�λ���*/
		else if (c->min_fid == 0 && c->max_fid == 0){
			c->min_fid = seg->fid - 1;
			c->play_frame_ts = seg->timestamp;
		}

		if (c->max_fid >= 0 && c->max_fid < seg->fid && c->max_ts < seg->timestamp){
			c->frame_timer = (seg->timestamp - c->max_ts) / (seg->fid - c->max_fid);
			c->frame_timer = SU_MAX(20, SU_MIN(200, c->frame_timer));
		}
		c->max_ts = seg->timestamp;
		c->max_fid = seg->fid;
	}

	sim_debug("buffer put video frame, frame = %u, packet_id = %u\n", seg->fid, seg->packet_id);

	frame = &(c->frames[INDEX(seg->fid)]);
	frame->fid = seg->fid;
	frame->frame_type = seg->ftype;
	frame->ts = seg->timestamp;

	if (frame->seg_number == 0){
		frame->seg_count = 1;
		frame->seg_number = seg->total;
		frame->segments = calloc(frame->seg_number, sizeof(seg));
		frame->segments[seg->index] = seg;

		ret = 0;
	}
	else{
		if (frame->segments[seg->index] == NULL){
			frame->segments[seg->index] = seg;
			frame->seg_count++;
			ret = 0;
		}
	}

	return ret;
}

static void real_video_cache_check_playing(sim_session_t* s, sim_frame_cache_t* c)
{
	uint32_t space = SU_MAX(c->wait_timer, c->frame_timer);

	if (c->max_fid > c->min_fid){
		if (c->max_ts >= c->play_frame_ts + space && c->max_fid >= c->min_fid + 1){
			c->state = buffer_playing;

			c->play_ts = GET_SYS_MS();
			c->frame_ts = c->max_ts - c->frame_timer * (c->max_fid - c->min_fid - 1);
			/*sim_debug("buffer playing, frame ts = %u, min_ts = %u, c->frame_timer = %u, max ts = %u\n", c->frame_ts, min_ts, c->frame_timer, max_ts);*/
		}
	}
}

static inline void real_video_cache_check_waiting(sim_session_t* s, sim_frame_cache_t* c)
{
}

static inline void real_video_cache_sync_timestamp(sim_session_t* s, sim_frame_cache_t* c)
{
	uint64_t cur_ts = GET_SYS_MS();

	if (cur_ts >= c->play_ts + 5){
		c->frame_ts = (uint32_t)((cur_ts - c->play_ts) * c->f) + c->frame_ts;
		c->play_ts = cur_ts;
	}
}

static uint32_t real_video_ready_ms(sim_session_t* s, sim_frame_cache_t* c)
{
	sim_frame_t* frame;
	uint32_t i, min_ready_ts, max_ready_ts, ret;

	ret = c->frame_timer;
	min_ready_ts = 0;
	max_ready_ts = 0;
	for (i = c->min_fid + 1; i <= c->max_fid; ++i){
		frame = &c->frames[INDEX(i)];
		if (frame->seg_count == frame->seg_number){
			if (min_ready_ts == 0)
				min_ready_ts = frame->ts;
			max_ready_ts = frame->ts;
		}
		else
			break;
	}

	if (min_ready_ts > 0)
		ret = max_ready_ts - min_ready_ts + c->frame_timer;

	return ret;
}

/*�ж��Ƿ���FIR����*/
static int real_video_check_fir(sim_session_t* s, sim_frame_cache_t* c)
{
	uint32_t pos;
	sim_frame_t* frame;

	pos = INDEX(c->min_fid + 1);
	frame = &c->frames[pos];

	/*��һ֡�������ģ�����F.I.R�ش�*/
	if (real_video_cache_check_frame_full(s, frame) == 0)
		return -1;

	/*����������С��3�룬�����ȴ�*/
	if (c->play_frame_ts + MAX_EVICT_DELAY_MS * 3 / 5 > c->max_ts)
		return -1;

	return 0;
}

static int real_video_cache_get(sim_session_t* s, sim_frame_cache_t* c, uint8_t* data, size_t* sizep, uint8_t* payload_type, int loss)
{
	uint32_t pos, space;
	size_t size, buffer_size;
	int ret, i;
	sim_frame_t* frame;
	uint32_t play_ready_ts;
	uint8_t type;

	buffer_size = *sizep;
	*sizep = 0;
	*payload_type = 0;

	type = 0;
	size = 0;
	ret = -1;

	if (c->state == buffer_waiting)
		real_video_cache_check_playing(s, c);
	else
		real_video_cache_check_waiting(s, c);

	if (c->state != buffer_playing)
		goto err;

	space = SU_MAX(c->wait_timer, c->frame_timer);

	/*���㲥��ʱ��ͬ��*/
	
	play_ready_ts = real_video_ready_ms(s, c);

	c->f = 1.0f;
	if (loss != 0 && play_ready_ts < SU_MIN(space, 4 * c->frame_timer))
		c->f = 0.6f;
	else if (play_ready_ts > c->frame_timer * 4 && play_ready_ts > MIN_EVICT_DELAY_MS / 2)
		c->f = 3.0f;
	else if (play_ready_ts > space && play_ready_ts >= SU_MAX(80, 2 * c->frame_timer))
		c->f = 1.2f;

	real_video_cache_sync_timestamp(s, c);

	 if (c->min_fid == c->max_fid)
		goto err;

	/*�����ܲ��ŵ�֡ʱ��*/
	if (c->play_frame_ts + SU_MAX(MIN_EVICT_DELAY_MS, SU_MIN(MAX_EVICT_DELAY_MS, 4 * c->wait_timer)) < c->max_ts){
		evict_gop_frame(s, c);
	}

	//real_video_cache_evict_discard(s, c);

	pos = INDEX(c->min_fid + 1);
	frame = &c->frames[pos];
	if ((c->min_fid + 1 == frame->fid || frame->frame_type == 1) && real_video_cache_check_frame_full(s, frame) == 0){
		/*���м�Ъ�Կ��*/
		if (frame->ts > c->frame_ts + SU_MAX(500, 2 * space) && play_ready_ts > space)
			c->frame_ts = frame->ts - space;
		else if (frame->ts <= c->frame_ts){
			for (i = 0; i < frame->seg_number; ++i){
				if (size + frame->segments[i]->data_size <= buffer_size && frame->segments[i]->data_size <= SIM_VIDEO_SIZE){ /*���ݿ���*/
					memcpy(data + size, frame->segments[i]->data, frame->segments[i]->data_size);
					size += frame->segments[i]->data_size;
					type = frame->segments[i]->payload_type;
				}
				else{
					size = 0;
					break;
				}
			}

			c->frame_ts = frame->ts;

			real_video_clean_frame(s, c, frame);
			ret = 0;
		}
	}
	else{
		size = 0;
		ret = -1;
	}

err:
	*sizep = size;
	*payload_type = type;

	return ret;
}

static uint32_t real_video_cache_get_min_seq(sim_session_t* s, sim_frame_cache_t* c)
{
	return c->min_seq;
}

static uint32_t real_video_cache_delay(sim_session_t* s, sim_frame_cache_t* c)
{
	if (c->max_fid <= c->min_fid)
		return 0;

	return c->max_ts - c->play_frame_ts;
}

/*********************************************��Ƶ���ն˴���*************************************************/
typedef struct
{
	int64_t				loss_ts;
	int64_t				ts;					/*��������ʱ�̣�һ��Ҫ��һ�����ڲŽ������������������һ����RTT�ı�����ϵ*/
	int					count;				/*��������ʱ��*/
}sim_loss_t;

static void loss_free(skiplist_item_t key, skiplist_item_t val, void* args)
{
	if (val.ptr != NULL)
		free(val.ptr);
}

/*���ն˽���feedback�� ��feedback���͵����Ͷ�*/
static void send_sim_feedback(void* handler, const uint8_t* payload, int payload_size)
{
	sim_header_t header;
	sim_feedback_t feedback;
	sim_receiver_t* r = (sim_receiver_t*)handler;
	sim_session_t* s = r->s;

	if (payload_size > SIM_FEEDBACK_SIZE){
		/*sim_error("feedback size > SIM_FEEDBACK_SIZE\n");*/
		return;
	}

	INIT_SIM_HEADER(header, SIM_FEEDBACK, s->uid);

	feedback.base_packet_id = r->base_seq;

	feedback.feedback_size = payload_size;
	memcpy(feedback.feedback, payload, payload_size);

	sim_encode_msg(&s->sstrm, &header, &feedback);
	sim_session_network_send(s, &s->sstrm);

	/*sim_debug("sim send SIM_FEEDBACK, feedback size = %u\n", payload_size);*/
}

sim_receiver_t* sim_receiver_create(sim_session_t* s, int transport_type)
{
	sim_receiver_t* r = calloc(1, sizeof(sim_receiver_t));

	r->loss = skiplist_create(idu32_compare, loss_free, NULL);
	r->cache = open_real_video_cache(s);
	r->cache_ts = GET_SYS_MS();
	r->s = s;
	r->fir_state = fir_normal;

	/*����һ�����ն˵�ӵ�����ƶ���*/
	r->cc_type = transport_type;
	r->cc = razor_receiver_create(r->cc_type, MIN_BITRATE, MAX_BITRATE, SIM_SEGMENT_HEADER_SIZE, r, send_sim_feedback);

	r->recover = sim_fec_create(s);

	return r;
}

void sim_receiver_destroy(sim_session_t* s, sim_receiver_t* r)
{
	if (r == NULL)
		return;

	assert(r->cache && r->loss);
	skiplist_destroy(r->loss);
	close_real_video_cache(s, r->cache);

	/*���ٽ��ն˵�ӵ�����ƶ���*/
	if (r->cc != NULL){
		razor_receiver_destroy(r->cc);
		r->cc = NULL;
	}

	if (r->recover != NULL){
		sim_fec_destroy(s, r->recover);
		r->recover = NULL;
	}

	free(r);
}

void sim_receiver_reset(sim_session_t* s, sim_receiver_t* r, int transport_type)
{
	reset_real_video_cache(s, r->cache);
	skiplist_clear(r->loss);

	r->base_uid = 0;
	r->base_seq = 0;
	r->actived = 0;
	r->max_seq = 0;
	r->max_ts = 0;
	r->ack_ts = GET_SYS_MS();
	r->active_ts = r->ack_ts;
	r->loss_count = 0;
	r->fir_state = fir_normal;
	r->fir_seq = 0;

	if (r->recover != NULL)
		sim_fec_reset(s, r->recover);

	/*���´���һ��CC����*/
	if (r->cc != NULL){
		razor_receiver_destroy(r->cc);
		r->cc = NULL;
	}

	r->cc_type = transport_type;
	r->cc = razor_receiver_create(r->cc_type, MIN_BITRATE, MAX_BITRATE, SIM_SEGMENT_HEADER_SIZE, r, send_sim_feedback);

	r->acked_count = 0;
}

int sim_receiver_active(sim_session_t* s, sim_receiver_t* r, uint32_t uid)
{
	if (r->actived == 1)
		return -1;

	r->actived = 1;
	r->cache->frame_timer = 50;		/*��дһ��Ĭ�ϵĲ���֡���*/

	r->base_uid = uid;
	r->active_ts = GET_SYS_MS();
	r->fir_state = fir_normal;

	if (r->recover != NULL)
		sim_fec_active(s, r->recover);

	return 0;
}

#define FIR_DELAY_TIME 2000
static void sim_receiver_send_fir(sim_session_t* s, sim_receiver_t* r)
{
	sim_fir_t fir;
	sim_header_t header;

	int64_t cur_ts = GET_SYS_MS();
	if (r->fir_ts + FIR_DELAY_TIME < cur_ts){
		INIT_SIM_HEADER(header, SIM_FIR, s->uid);
		fir.fir_seq = (r->fir_state == fir_flightting) ? r->fir_seq : (++r->fir_seq);

		sim_encode_msg(&s->sstrm, &header, &fir);
		sim_session_network_send(s, &s->sstrm);

		r->fir_ts = cur_ts;
	}
}

static void sim_receiver_update_loss(sim_session_t* s, sim_receiver_t* r, uint32_t seq, uint32_t ts)
{
	uint32_t i, space;
	skiplist_item_t key, val;
	skiplist_iter_t* iter;
	int64_t now_ts;

	key.u32 = seq;
	skiplist_remove(r->loss, key);

	now_ts = GET_SYS_MS();
	if (r->max_ts + 3000 < ts){
		skiplist_clear(r->loss);
		sim_receiver_send_fir(s, r);
	}
	else if (s->rtt >= CACHE_MAX_DELAY){
		skiplist_clear(r->loss);
		sim_receiver_send_fir(s, r);
	}
	else{
		if (s->rtt/2 < s->rtt_var)
			space = (s->rtt_var + s->rtt)/ 2;
		else
			space = s->rtt;

		for (i = r->max_seq + 1; i < seq; ++i){
			key.u32 = i;
			iter = skiplist_search(r->loss, key);
			if (iter == NULL){
				sim_loss_t* l = calloc(1, sizeof(sim_loss_t));
				l->ts = now_ts - space;						/*������һ�������ش���ʱ��*/
				l->loss_ts = now_ts;
				l->count = 0;
				val.ptr = l;

				skiplist_insert(r->loss, key, val);
			}
			/*sim_debug("add loss, seq = %u\n", key.u32);*/
		}
	}
}

static inline void sim_receiver_send_ack(sim_session_t* s, sim_receiver_t* r, sim_segment_ack_t* ack)
{
	uint32_t count, i;
	sim_header_t header;
	INIT_SIM_HEADER(header, SIM_SEG_ACK, s->uid);

	ack->ack_num = 0;

	if (r->base_seq < r->max_seq){
		count = r->acked_count >= ACK_NUM ? ACK_NUM : r->acked_count;
		for (i = 0; i < count; ++i){
			if (r->base_seq < r->ackeds[i])
				ack->acked[ack->ack_num++] = (uint16_t)(r->ackeds[i] - r->base_seq);
		}
	}

	r->acked_count = 0;

	sim_encode_msg(&s->sstrm, &header, ack);
	sim_session_network_send(s, &s->sstrm);
	/*sim_debug("send SEG_ACK, base = %u, ack_id = %u\n", ack->base_packet_id, ack->acked_packet_id);*/
}

static void video_real_update_base(sim_session_t* s, sim_receiver_t* r)
{
	skiplist_iter_t* iter;
	skiplist_item_t key;
	uint32_t min_seq;

	min_seq = real_video_cache_get_min_seq(s, r->cache);
	for (key.u32 = r->base_seq + 1; key.u32 <= min_seq; ++key.u32)
		skiplist_remove(r->loss, key);

	if (skiplist_size(r->loss) == 0)
		r->base_seq = r->max_seq;
	else{
		iter = skiplist_first(r->loss);
		r->base_seq = (iter->key.u32 > 0) ? (iter->key.u32 - 1) : r->base_seq;
	}
}

/*����ack��nackȷ�ϣ������㻺�����ĵȴ�ʱ��*/
#define ACK_REAL_TIME	20
#define ACK_HB_TIME		200
static void video_real_ack(sim_session_t* s, sim_receiver_t* r, int hb, uint32_t seq)
{
	uint64_t cur_ts;
	sim_segment_ack_t ack;
	skiplist_iter_t* iter;
	skiplist_item_t key;
	sim_loss_t* l;
	uint32_t delay, space_factor;
	int max_count = 0;

	uint32_t numbers[NACK_NUM];
	int i, evict_count = 0;

	cur_ts = GET_SYS_MS();
	/*�������������*/
	if ((hb == 0 && r->ack_ts + ACK_REAL_TIME < cur_ts) || (r->ack_ts + ACK_HB_TIME < cur_ts)){

		ack.acked_packet_id = seq;

		video_real_update_base(s, r);

		ack.base_packet_id = r->base_seq;
		ack.nack_num = 0;
		SKIPLIST_FOREACH(r->loss, iter){
			l = (sim_loss_t*)iter->val.ptr;

			if (iter->key.u32 <= r->base_seq)
				continue;

			space_factor = SU_MAX(10, s->rtt + s->rtt_var) + l->count * SU_MIN(100, SU_MAX(10, s->rtt_var)); /*���ڼ򵥵�ӵ����������ֹGET��ˮ*/
			if (l->count >= 15 || l->loss_ts + MIN_EVICT_DELAY_MS / 2 < cur_ts){
				if (evict_count < NACK_NUM)
					numbers[evict_count++] = iter->key.u32;
			}
			else if (l->ts + space_factor <= cur_ts && ack.nack_num < NACK_NUM){
				ack.nack[ack.nack_num++] = iter->key.u32 - r->base_seq;
				l->ts = cur_ts;

				r->loss_count++;
				l->count++;
			}

			if (l->count > max_count)
				max_count = l->count;
		}

		if (s->rtt >= CACHE_MAX_DELAY && ack.nack_num > 0){
			skiplist_clear(r->loss);
			sim_receiver_send_fir(s, r);

			ack.nack_num = 0;
		}
		
		sim_receiver_send_ack(s, r, &ack);

		r->ack_ts = cur_ts;

		/*���ݶ����ش�ȷ����Ƶ��������Ҫ�Ļ���ʱ��*/
		if (max_count >= 1)
			delay = (max_count + 7) * (s->rtt + s->rtt_var) / 8;
		else
			delay = SU_MAX(80, (s->rtt + s->rtt_var) / 4);

		r->cache->wait_timer = (r->cache->wait_timer * 7 + delay) / 8;
		r->cache->wait_timer = SU_MAX(r->cache->frame_timer, r->cache->wait_timer);

		/*for (i = 0; i < evict_count; i++){
			key.u32 = numbers[i];
			skiplist_remove(r->loss, key);
			real_video_cache_discard(s, r->cache, key.u32);
		}*/
	}
}

static int sim_receiver_internal_put(sim_session_t* s, sim_receiver_t* r, sim_segment_t* seg)
{
	uint32_t seq;

	if (r->max_seq == 0 && seg->ftype == 0)
		return -1;

	seq = seg->packet_id;
	if (r->actived == 0 || seg->packet_id <= r->base_seq)
		return -1;

	if (r->max_seq == 0 && seg->packet_id > seg->index){
		r->max_seq = seg->packet_id - seg->index - 1;
		r->base_seq = seg->packet_id - seg->index - 1;
		r->max_ts = seg->timestamp;
	}

	sim_receiver_update_loss(s, r, seq, seg->timestamp);
	if (real_video_cache_put(s, r->cache, seg) != 0)
		return -1;

	if (seg->ftype == 1)
		r->fir_state = fir_normal;

	r->max_seq = SU_MAX(r->max_seq, seq);
	r->max_ts = SU_MAX(r->max_ts, seg->timestamp);

	r->ackeds[r->acked_count++ % ACK_NUM] = seg->packet_id;

	return 0;
}

static void sim_receiver_recover(sim_session_t* s, sim_receiver_t* r)
{
	skiplist_iter_t* iter;
	skiplist_t* recover_map;
	sim_segment_t* seg, *in_seg;

	/*���Ѿ��ָ��İ����д���*/
	recover_map = r->recover->recover_packets;
	while (skiplist_size(recover_map) > 0){
		iter = skiplist_first(recover_map);
		seg = iter->val.ptr;

		in_seg = malloc(sizeof(sim_segment_t));
		*in_seg = *seg;

		skiplist_remove(recover_map, iter->key);

		if (sim_receiver_internal_put(s, r, in_seg) == 0){
			sim_debug("fec recover video segment, packet id = %u\n", in_seg->packet_id);
			sim_fec_put_segment(s, r->recover, in_seg); /*���ָ��İ����뵽FEC�����ָ���������*/
		}
		else
			free(in_seg);
	}
}

int sim_receiver_put(sim_session_t* s, sim_receiver_t* r, sim_segment_t* seg)
{
	int rc;

	/*ӵ������*/
	if (r->cc != NULL)
		r->cc->on_received(r->cc, seg->transport_seq, seg->timestamp + seg->send_ts, seg->data_size + SIM_SEGMENT_HEADER_SIZE, seg->remb);

	rc = sim_receiver_internal_put(s, r, seg);
	if (rc != 0)
		return rc;
		
	/*sim_debug("put video segment, packet id = %u\n", seg->packet_id);*/
	/*����FEC �ָ�*/
	if (r->recover != NULL && seg->fec_id > 0){
		sim_fec_put_segment(s, r->recover, seg);
		sim_receiver_recover(s, r);
	}

	video_real_ack(s, r, 0, seg->packet_id);

	return rc;
}

int sim_receiver_put_fec(sim_session_t* s, sim_receiver_t* r, sim_fec_t* fec)
{
	if (r->cc != NULL)
		r->cc->on_received(r->cc, fec->transport_seq, fec->send_ts, fec->fec_data_size + SIM_SEGMENT_HEADER_SIZE, 1);

	if (r->recover != NULL){
		sim_fec_put_fec_packet(s, r->recover, fec);
		sim_receiver_recover(s, r);
	}

	return 0;
}

int sim_receiver_padding(sim_session_t* s, sim_receiver_t* r, uint16_t transport_seq, uint32_t send_ts, size_t data_size)
{
	/*ӵ������*/
	if (r->cc != NULL)
		r->cc->on_received(r->cc, transport_seq, send_ts, data_size + SIM_SEGMENT_HEADER_SIZE, 1);

	return 0;
}

/*��ȡ��Ƶ֡����*/
int sim_receiver_get(sim_session_t* s, sim_receiver_t* r, uint8_t* data, size_t* sizep, uint8_t* payload_type)
{
	if (r == NULL || r->actived == 0)
		return -1;

	return real_video_cache_get(s, r->cache, data, sizep, payload_type, skiplist_size(r->loss));
}

void sim_receiver_timer(sim_session_t* s, sim_receiver_t* r, int64_t now_ts)
{
	video_real_ack(s, r, 1, 0);

	/*ÿ1�볢����һ�λ������ȴ�ʱ��*/
	if (r->cache_ts + SU_MAX(s->rtt + s->rtt_var, 500) < now_ts){
		if (r->loss_count == 0)
			r->cache->wait_timer = SU_MAX(r->cache->wait_timer * 7 / 8, (s->rtt + s->rtt_var) / 2);
		else if (r->cache->wait_timer > 2 * (s->rtt + s->rtt_var))
			r->cache->wait_timer = SU_MAX(r->cache->wait_timer * 15 / 16, (s->rtt + s->rtt_var));

		r->cache_ts = now_ts;
		r->loss_count = 0;
	}

	/*ӵ����������*/
	if (r->cc != NULL)
		r->cc->heartbeat(r->cc);

	if (r->recover != NULL)
		sim_fec_evict(s, r->recover, now_ts);
}

void sim_receiver_update_rtt(sim_session_t* s, sim_receiver_t* r)
{
	if (r->cc != NULL)
		r->cc->update_rtt(r->cc, s->rtt + s->rtt_var);
}

uint32_t sim_receiver_cache_delay(sim_session_t* s, sim_receiver_t* r)
{
	return real_video_cache_delay(s, r->cache);
}


