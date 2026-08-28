// confidence_c_api.h
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef void* confidence_handle_t;

confidence_handle_t confidence_create(int frames_per_row);
void confidence_destroy(confidence_handle_t h);

int confidence_load_csv(confidence_handle_t h, const char* path, int has_header);

double confidence_get(confidence_handle_t h, int frame_id, int bitrate_kbps);

int confidence_adjust(confidence_handle_t h,
                      int frame_id,
                      int current_kbps,
                      int cc_kbps,
                      double threshold);

#ifdef __cplusplus
}
#endif