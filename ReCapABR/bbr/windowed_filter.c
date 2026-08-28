/*-
* Copyright (c) 2017-2018 Razor, Inc.
*	All rights reserved.
*
* See the file LICENSE for redistribution information.
*/

#include "windowed_filter.h"
#include "razor_log.h"

int max_val_func(int64_t v1, int64_t v2)
{
	if (v1 >= v2)
		return 1;
	else
		return 0;
}

int min_val_func(int64_t v1, int64_t v2)
{
	if (v1 <= v2)
		return 1;
	else
		return 0;
}

void wnd_filter_init(windowed_filter_t* filter, int64_t wnd_size, compare_func_f comp)
{
	filter->wnd_size = wnd_size;
	filter->comp_func = comp;

	wnd_filter_reset(filter, 0, 0);
}

void wnd_filter_reset(windowed_filter_t* filter, int64_t new_sample, int64_t new_ts)
{
	int i;
	for (i = 0; i < 3; i++){
		filter->estimates[i].sample = new_sample;
		filter->estimates[i].ts = new_ts;
	}
}

void wnd_filter_print(windowed_filter_t* filter)
{
	razor_debug("sample[0] = %llu, sample[1] = %llu, sample[2] = %llu\n", filter->estimates[0].sample, filter->estimates[1].sample, filter->estimates[2].sample);
}

void wnd_filter_set_window_size(windowed_filter_t* filter, int64_t wnd_size)
{
	filter->wnd_size = wnd_size;
}

void wnd_filter_update(windowed_filter_t* filter, int64_t new_sample, int64_t new_ts)
{
	// printf("UPDATE!! %lld\n", new_sample);
	wnd_sample_t sample = { new_sample, new_ts };

	/*������ǵ�һ�����¡�filter�м�¼��ֵ������ʱ�䴰����������ֵ�����µ�ֵ���и��Ǹ���*/
	if (filter->estimates[0].sample == 0 || filter->comp_func(new_sample, filter->estimates[0].sample) > 0
		|| new_ts - filter->estimates[2].ts > filter->wnd_size){
		wnd_filter_reset(filter, new_sample, new_ts);
		return;
	}

	/*�¸��µ�ֵ�ǵڶ����ֵ����1��2λ��ֵ���и���*/
	if (filter->comp_func(new_sample, filter->estimates[1].sample) > 0){
		filter->estimates[1] = sample;
		filter->estimates[2] = sample;
	}
	else if (filter->comp_func(new_sample, filter->estimates[2].sample) > 0){/*���µ�ֵΪ������ֻ��ĩβ��2���и���*/
		filter->estimates[2] = sample;
	}

	/*���й�����̭,���ж�0λ�Ƿ���̭�����ж�1�Ƿ���̭,��Ϊ2λ���ڵ�һ��if�������ж�*/
	if (new_ts - filter->estimates[0].ts > filter->wnd_size){
		filter->estimates[0] = filter->estimates[1];
		filter->estimates[1] = filter->estimates[2];
		filter->estimates[2] = sample;

		/*�ٴζ��ƶ����filter���й�����̭�ж�*/
		if (new_ts - filter->estimates[0].ts > filter->wnd_size){
			filter->estimates[0] = filter->estimates[1];
			filter->estimates[1] = filter->estimates[2];
		}

		return;
	}

	/*��ʱ����Ⱥ������ֵͬ�ĸ���*/
	if (filter->estimates[0].sample == filter->estimates[1].sample
		&& new_ts - filter->estimates[1].ts > (filter->wnd_size >> 2)){/*0��λ������ֵ��1��λ�ϵ�ֵ��ȣ���1��λ��ʱ�������뵱ǰʱ��㳬�����ڵ�1/4,��ζ��1 2��λ����Ҫ���µ�����ֵ����*/
		filter->estimates[1] = filter->estimates[2] = sample;
		return;
	}

	/*ͬ��ԭ������2��λ�ϵ�ֵ���и���*/
	if (filter->estimates[1].sample == filter->estimates[2].sample
		&& new_ts - filter->estimates[2].ts > (filter->wnd_size >> 1))
		filter->estimates[2] = sample;
}

int64_t wnd_filter_best(windowed_filter_t* filter)
{
	return filter->estimates[0].sample;
}

int64_t wnd_filter_second_best(windowed_filter_t* filter)
{
	return filter->estimates[1].sample;
}

int64_t wnd_filter_third_best(windowed_filter_t* filter)
{
	return filter->estimates[2].sample;
}




