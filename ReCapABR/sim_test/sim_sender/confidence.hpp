// confidence.hpp
#pragma once
#include <ostream>
#include <string>
#include <vector>

struct ConfidenceRow {
    int original_index = 0;
    double c200 = 0.0, c400 = 0.0, c610 = 0.0, c910 = 0.0, c1710 = 0.0, c3150 = 0.0;
    std::vector<double> AsVector() const;
};

class ConfidenceController {
public:
    const std::vector<int> tiers_kbps{200, 400, 610, 910, 1710, 3150};
    const char* save_log_to_path = "_behavior.log";

    explicit Controller(int frames_per_row = 150);

    void LoadFromCsv(const std::string& csv_path, bool has_header = true);
    double Get(int frame_id, int bitrate_kbps) const;

    int AdjustBitrate(int frame_id,
                      int current_bitrate_kbps,
                      int cc_bitrate_kbps,
                      double threshold,
                      std::ostream* log = nullptr) const;

private:
    int computeRowIndex(int frame_id) const;
    const std::vector<double>& GetRowConfsByFrame(int frame_id) const;
    void ensureCacheBuilt() const;

    int nextTierAbove(int bitrate_kbps) const;

private:
    int frames_per_row_ = 150;
    std::vector<Row> rows_;
    mutable bool cache_built_ = false;
    mutable std::vector<std::vector<double>> row_confs_cache_;
};