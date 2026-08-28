// confidence_c_api.cpp
#include "confidence.hpp"
#include "confidence_c_api.h"
#include <exception>

confidence_handle_t confidence_create(int frames_per_row) {
    try {
        return new ConfidenceController(frames_per_row);
    } catch (...) {
        return nullptr;
    }
}

void confidence_destroy(confidence_handle_t h) {
    delete static_cast<ConfidenceController*>(h);
}

int confidence_load_csv(confidence_handle_t h, const char* path, int has_header) {
    if (!h || !path) return -1;
    try {
        static_cast<ConfidenceController*>(h)->LoadFromCsv(path, has_header != 0);
        return 0;
    } catch (...) {
        return -2;
    }
}

double confidence_get(confidence_handle_t h, int frame_id, int bitrate_kbps) {
    if (!h) return -1.0;
    try {
        return static_cast<ConfidenceController*>(h)->GetConfidence(frame_id, bitrate_kbps);
    } catch (...) {
        return -2.0;
    }
}

int confidence_adjust(confidence_handle_t h,
                      int frame_id,
                      int current_kbps,
                      int cc_kbps,
                      double threshold) {
    if (!h) return -1;
    try {
        return static_cast<ConfidenceController*>(h)->AdjustBitrate(
            frame_id, current_kbps, cc_kbps, threshold, nullptr
        );
    } catch (...) {
        return -2;
    }
}