#include "confidence.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <stdexcept>

// ---------- 小工具函数（仅本文件使用） ----------

static inline double lerp(double x, double x0, double x1, double y0, double y1) {
    if (std::abs(x1 - x0) < 1e-12) return y0;
    double t = (x - x0) / (x1 - x0);
    return y0 + t * (y1 - y0);
}

// 去 UTF-8 BOM（EF BB BF）
static inline void stripUtf8Bom(std::string& s) {
    if (s.size() >= 3 &&
        static_cast<unsigned char>(s[0]) == 0xEF &&
        static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) {
        s.erase(0, 3);
    }
}

// 去首尾空白 + 去掉 \r
static inline void trim(std::string& s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    stripUtf8Bom(s);

    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
}

static inline void stripOuterQuotes(std::string& s) {
    if (s.size() >= 2) {
        char a = s.front();
        char b = s.back();
        if ((a == '"' && b == '"') || (a == '\'' && b == '\'')) {
            s = s.substr(1, s.size() - 2);
        }
    }
}

static double parseDoubleStrict(std::string s, const std::string& full_line) {
    trim(s);
    stripOuterQuotes(s);
    trim(s);

    if (s.empty()) {
        throw std::runtime_error("CSV parse error: empty numeric cell. Line: " + full_line);
    }

    try {
        size_t idx = 0;
        double v = std::stod(s, &idx);
        if (idx != s.size()) {
            throw std::runtime_error(
                "CSV parse error: numeric cell has trailing chars: [" + s + "]. Line: " + full_line
            );
        }
        return v;
    } catch (...) {
        throw std::runtime_error(
            "CSV parse error: failed to parse double from cell: [" + s + "]. Line: " + full_line
        );
    }
}

static int parseIntStrict(std::string s, const std::string& full_line) {
    trim(s);
    stripOuterQuotes(s);
    trim(s);

    if (s.empty()) {
        throw std::runtime_error("CSV parse error: empty int cell. Line: " + full_line);
    }

    try {
        size_t idx = 0;
        int v = std::stoi(s, &idx);
        if (idx != s.size()) {
            throw std::runtime_error(
                "CSV parse error: int cell has trailing chars: [" + s + "]. Line: " + full_line
            );
        }
        return v;
    } catch (...) {
        throw std::runtime_error(
            "CSV parse error: failed to parse int from cell: [" + s + "]. Line: " + full_line
        );
    }
}

static double readDoubleCell(std::stringstream& ss, const std::string& full_line) {
    std::string cell;
    if (!std::getline(ss, cell, ',')) {
        throw std::runtime_error("CSV format error (not enough columns). Line: " + full_line);
    }
    return parseDoubleStrict(cell, full_line);
}

static double interpolateConfidence(int bitrate_kbps,
                                    const std::vector<int>& tiers,
                                    const std::vector<double>& confs) {
    if (tiers.empty() || confs.size() != tiers.size()) return 0.0;

    if (bitrate_kbps <= tiers.front()) return confs.front();
    if (bitrate_kbps >= tiers.back())  return confs.back();

    auto it = std::upper_bound(tiers.begin(), tiers.end(), bitrate_kbps);
    size_t hi = static_cast<size_t>(it - tiers.begin());
    size_t lo = hi - 1;

    return lerp(
        static_cast<double>(bitrate_kbps),
        static_cast<double>(tiers[lo]),
        static_cast<double>(tiers[hi]),
        confs[lo],
        confs[hi]
    );
}

// ---------- ConfidenceRow 实现 ----------

std::vector<double> ConfidenceRow::AsVector() const {
    return {c200, c400, c610, c910, c1710, c3150};
}

// ---------- ConfidenceController 实现 ----------

ConfidenceController::ConfidenceController(int frames_per_row)
    : frames_per_row_(frames_per_row) {}

void ConfidenceController::LoadFromCsv(const std::string& csv_path, bool has_header) {
    std::ifstream fin(csv_path);
    if (!fin.is_open()) {
        throw std::runtime_error("Failed to open csv: " + csv_path);
    }

    std::string line;
    if (has_header) {
        if (!std::getline(fin, line)) {
            throw std::runtime_error("CSV is empty: " + csv_path);
        }
    }

    rows_.clear();
    cache_built_ = false;
    row_confs_cache_.clear();

    while (std::getline(fin, line)) {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string cell;

        ConfidenceRow row;

        if (!std::getline(ss, cell, ',')) continue;
        row.original_index = parseIntStrict(cell, line);

        row.c200  = readDoubleCell(ss, line);
        row.c400  = readDoubleCell(ss, line);
        row.c610  = readDoubleCell(ss, line);
        row.c910  = readDoubleCell(ss, line);
        row.c1710 = readDoubleCell(ss, line);
        row.c3150 = readDoubleCell(ss, line);

        rows_.push_back(row);
    }

    if (rows_.empty()) {
        throw std::runtime_error("No data rows loaded from: " + csv_path);
    }
}

double ConfidenceController::GetConfidence(int frame_id, int bitrate_kbps) const {
    const auto& confs = GetRowConfsByFrame(frame_id);
    return interpolateConfidence(bitrate_kbps, tiers_kbps, confs);
}

int ConfidenceController::AdjustBitrate(int frame_id,
                                       int current_bitrate_kbps,
                                       int cc_bitrate_kbps,
                                       double threshold,
                                       std::ostream* log) const {
    if (rows_.empty()) {
        int out = std::min(current_bitrate_kbps, cc_bitrate_kbps);
        if (log) {
            (*log) << "ADJUST"
                   << " frame=" << frame_id
                   << " row=" << -1
                   << " cur=" << current_bitrate_kbps
                   << " cc=" << cc_bitrate_kbps
                   << " thr=" << threshold
                   << " conf_cur=" << -1
                   << " action=NO_ROWS"
                   << " target=" << out
                   << " out=" << out
                   << " reason=table_empty"
                   << "\n";
        }
        return out;
    }

    int row_idx = computeRowIndex(frame_id);

    // 1) current > CC：直接降到 CC
    if (current_bitrate_kbps > cc_bitrate_kbps) {
        if (log) {
            (*log) << "ADJUST"
                   << " frame=" << frame_id
                   << " row=" << row_idx
                   << " cur=" << current_bitrate_kbps
                   << " cc=" << cc_bitrate_kbps
                   << " thr=" << threshold
                   << " conf_cur=" << GetConfidence(frame_id, current_bitrate_kbps)
                   << " action=CAP_TO_CC"
                   << " target=" << cc_bitrate_kbps
                   << " out=" << cc_bitrate_kbps
                   << " reason=cur_gt_cc"
                   << "\n";
        }
        return cc_bitrate_kbps;
    }

    // 2) current <= CC：看置信度
    const auto& confs = GetRowConfsByFrame(frame_id);
    double cur_conf = interpolateConfidence(current_bitrate_kbps, tiers_kbps, confs);

    // 2.1) 置信度够：保持当前码率
    if (cur_conf >= threshold) {
        if (log) {
            (*log) << "ADJUST"
                   << " frame=" << frame_id
                   << " row=" << row_idx
                   << " cur=" << current_bitrate_kbps
                   << " cc=" << cc_bitrate_kbps
                   << " thr=" << threshold
                   << " conf_cur=" << cur_conf
                   << " action=KEEP"
                   << " target=" << current_bitrate_kbps
                   << " out=" << current_bitrate_kbps
                   << " reason=conf_ok_and_cur_le_cc"
                   << "\n";
        }
        return current_bitrate_kbps;
    }

    // 2.2) 置信度不够：找第一个 >= threshold 的档位 A
    int A = -1;
    for (int i = 0; i < static_cast<int>(tiers_kbps.size()); ++i) {
        if (confs[i] >= threshold) {
            A = i;
            break;
        }
    }

    // 2.2.a) 找不到：上调一个档位（相对 current 的 next tier）
    if (A == -1) {
        int max_tier = tiers_kbps.back();
        int target = current_bitrate_kbps;

        if (current_bitrate_kbps < max_tier) {
            target = nextTierAbove(current_bitrate_kbps);  // 一定 >= current
        } else {
            target = current_bitrate_kbps;
        }

        int out = std::min(cc_bitrate_kbps, target);

        if (log) {
            (*log) << "ADJUST"
                   << " frame=" << frame_id
                   << " row=" << row_idx
                   << " cur=" << current_bitrate_kbps
                   << " cc=" << cc_bitrate_kbps
                   << " thr=" << threshold
                   << " conf_cur=" << cur_conf
                   << " action=STEP_UP_ONE_TIER"
                   << " A=" << A
                   << " max_tier=" << max_tier
                   << " target=" << target
                   << " out=" << out
                   << " reason=no_tier_meets_threshold"
                   << "\n";
        }
        return out;
    }

    // 2.2.b) 找到了 A：在 A-1 与 A 之间反插值求阈值码率
    double target_bitrate = 0.0;
    int lo_tier = -1, hi_tier = -1;
    double lo_c = 0.0, hi_c = 0.0;
    double t = 0.0;
    std::string reason;

    if (A == 0) {
        target_bitrate = static_cast<double>(tiers_kbps[0]);
        reason = "A_is_0_use_min_tier";
    } else {
        lo_tier = tiers_kbps[A - 1];
        hi_tier = tiers_kbps[A];
        lo_c = confs[A - 1];
        hi_c = confs[A];

        if (std::abs(hi_c - lo_c) < 1e-12) {
            target_bitrate = static_cast<double>(hi_tier);
            reason = "flat_conf_segment_use_hi_tier";
        } else {
            t = (threshold - lo_c) / (hi_c - lo_c);
            t = std::max(0.0, std::min(1.0, t));
            target_bitrate = lo_tier + t * (hi_tier - lo_tier);
            reason = "inverse_lerp_between_A-1_and_A";
        }
    }

    // 为了倾向“刚好 >= threshold”，取 ceil
    int target_int = static_cast<int>(std::ceil(target_bitrate + 1e-9));
    int out = std::min(cc_bitrate_kbps, target_int);

    if (log) {
        (*log) << "ADJUST"
               << " frame=" << frame_id
               << " row=" << row_idx
               << " cur=" << current_bitrate_kbps
               << " cc=" << cc_bitrate_kbps
               << " thr=" << threshold
               << " conf_cur=" << cur_conf
               << " action=RAISE_TO_THRESHOLD"
               << " A=" << A
               << " lo_tier=" << lo_tier
               << " hi_tier=" << hi_tier
               << " lo_c=" << lo_c
               << " hi_c=" << hi_c
               << " t=" << t
               << " target=" << target_int
               << " out=" << out
               << " reason=" << reason
               << "\n";
    }

    return out;
}

int ConfidenceController::computeRowIndex(int frame_id) const {
    int row_idx = frame_id / frames_per_row_;
    if (row_idx < 0) row_idx = 0;
    if (row_idx >= static_cast<int>(rows_.size())) {
        row_idx = static_cast<int>(rows_.size()) - 1;
    }
    return row_idx;
}

const std::vector<double>& ConfidenceController::GetRowConfsByFrame(int frame_id) const {
    int row_idx = computeRowIndex(frame_id);
    ensureCacheBuilt();
    return row_confs_cache_[row_idx];
}

void ConfidenceController::ensureCacheBuilt() const {
    if (cache_built_) return;
    row_confs_cache_.clear();
    row_confs_cache_.reserve(rows_.size());
    for (const auto& r : rows_) {
        row_confs_cache_.push_back(r.AsVector());
    }
    cache_built_ = true;
}

int ConfidenceController::nextTierAbove(int bitrate_kbps) const {
    auto it = std::upper_bound(tiers_kbps.begin(), tiers_kbps.end(), bitrate_kbps);
    if (it == tiers_kbps.end()) return tiers_kbps.back();
    return *it;
}