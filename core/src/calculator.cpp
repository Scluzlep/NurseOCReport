#include "nurseocr/calculator.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <unordered_map>

namespace nurseocr {

namespace {

int days_in_month(int year, int month) {
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 30;
    int d = days[month - 1];
    if (month == 2) {
        bool leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
        if (leap) d = 29;
    }
    return d;
}

// Sakamoto's algorithm: returns 0 = Sunday, 1 = Monday, ..., 6 = Saturday
int day_of_week(int y, int m, int d) {
    static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

std::string trim_ascii(std::string s) {
    size_t b = 0;
    while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    size_t e = s.size();
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string strip_spaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (!std::isspace(c)) out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string canonical_shift_key(const std::string& raw) {
    std::string s = strip_spaces(raw);
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    if (s == "Z" || s == "治" || s == "白" || s == "白班" || s == "治/白" || s == "治白" || s == "治合计") {
        return "Z";
    }
    if (s == "A" || s == "A(主)" || s == "A（主）" || s == "A主" || s == "A(主)班" || s == "A合计") {
        return "A";
    }
    if (s == "P" || s == "P班" || s == "P合计") {
        return "P";
    }
    if (s == "Y" || s == "夜" || s == "夜班" || s == "大" || s == "大夜" || s == "夜合计") {
        return "Y";
    }
    if (s == "T" || s == "技术难度" || s == "难度" || s == "难" || s == "技术难度合计") {
        return "T";
    }
    return s;
}

} // namespace

std::vector<ShiftRule> default_shift_rules() {
    return {
        {"Z", "治/白", 1.00, true},
        {"A", "A(主)", 1.05, true},
        {"P", "P", 1.10, true},
        {"Y", "夜", 1.20, true},
        {"T", "技术难度", 1.00, false},
    };
}

std::vector<std::string> default_week_labels(int year, int month, int weeks) {
    std::vector<std::string> labels;
    if (weeks <= 0) return labels;
    int total_days = days_in_month(year, month);
    int cur = 1;
    for (int w = 1; w <= weeks; ++w) {
        std::ostringstream oss;
        if (cur > total_days) {
            oss << "第" << w << "周";
            labels.push_back(oss.str());
            continue;
        }
        int start_day = cur;
        int end_day = cur;
        if (w == weeks) {
            end_day = total_days;
        } else {
            // Advance until Sunday (dow == 0)
            while (end_day < total_days && day_of_week(year, month, end_day) != 0) {
                ++end_day;
            }
            // If remaining days after the final requested week would be <= 3, merge when w == weeks
            if (w == weeks - 1 && total_days - end_day <= 0) {
                end_day = total_days;
            }
        }
        oss << "第" << w << "周（" << month << "." << start_day << "–" << month << "." << end_day << "）";
        labels.push_back(oss.str());
        cur = end_day + 1;
    }
    return labels;
}

double compute_effective_coeff(const StaffEntry& entry, const CoeffFlags& flags) {
    if (!flags.enable_title_coeff && !flags.enable_duty_coeff) {
        return 1.0;
    }
    double total = 0.0;
    if (flags.enable_title_coeff) total += entry.title_coeff;
    if (flags.enable_duty_coeff) total += entry.duty_coeff;
    return total;
}

int find_shift_rule_index(const std::vector<ShiftRule>& rules,
                          const std::string& code,
                          const std::string& name) {
    // 1. Exact code or exact name
    for (size_t i = 0; i < rules.size(); ++i) {
        if (!code.empty() && rules[i].code == code) return static_cast<int>(i);
        if (!name.empty() && rules[i].name == name) return static_cast<int>(i);
    }
    // 2. Canonical key match
    std::string ck_code = canonical_shift_key(code);
    std::string ck_name = canonical_shift_key(name);
    for (size_t i = 0; i < rules.size(); ++i) {
        std::string rk1 = canonical_shift_key(rules[i].code);
        std::string rk2 = canonical_shift_key(rules[i].name);
        if ((!ck_name.empty() && (ck_name == rk1 || ck_name == rk2)) ||
            (!ck_code.empty() && (ck_code == rk1 || ck_code == rk2))) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

SheetSummary evaluate_workload(const WorkloadSheet& sheet, const AppConfig& config) {
    SheetSummary summary;
    const size_t num_rules = config.shift_rules.size();
    summary.shift_grand_totals.assign(num_rules, 0.0);

    std::unordered_map<std::string, StaffEntry> staff_map;
    for (const auto& st : config.staff) {
        std::string key = strip_spaces(st.name);
        if (!key.empty()) {
            staff_map[key] = st;
        }
    }

    const int cols_per_week = static_cast<int>(sheet.column_shift_names.size());
    std::vector<int> col_to_rule(cols_per_week, -1);
    for (int c = 0; c < cols_per_week; ++c) {
        std::string code = c < static_cast<int>(sheet.column_shift_codes.size()) ? sheet.column_shift_codes[c] : "";
        std::string name = sheet.column_shift_names[c];
        col_to_rule[c] = find_shift_rule_index(config.shift_rules, code, name);
    }

    std::vector<std::vector<double>> week_col_sums(
        std::max(0, sheet.weeks), std::vector<double>(std::max(0, cols_per_week), 0.0));

    for (const auto& row : sheet.rows) {
        PersonScore ps;
        ps.name = row.staff_name.empty() ? row.ocr_name : row.staff_name;
        ps.shift_totals.assign(num_rules, 0.0);

        if (row.name_needs_review) {
            summary.pending_review_count += 1;
        }

        std::string lookup_key = strip_spaces(ps.name);
        auto it = staff_map.find(lookup_key);
        if (it != staff_map.end()) {
            ps.matched_in_staff = true;
            ps.title_coeff = it->second.title_coeff;
            ps.duty_coeff = it->second.duty_coeff;
            ps.effective_coeff = compute_effective_coeff(it->second, config.coeff_flags);
        } else {
            ps.matched_in_staff = false;
            ps.title_coeff = 1.0;
            ps.duty_coeff = 0.0;
            ps.effective_coeff = 1.0;
            if (!lookup_key.empty()) {
                summary.alerts.push_back("未找到人员系数: " + ps.name);
            }
        }

        for (const auto& cell : row.cells) {
            if (cell.status == ReviewStatus::NeedsReview) {
                summary.pending_review_count += 1;
            }
            if (cell.kind == ValueKind::Number && cols_per_week > 0) {
                int sc = cell.shift_col;
                if (sc >= 0 && sc < cols_per_week) {
                    int rule_idx = col_to_rule[sc];
                    if (rule_idx >= 0 && rule_idx < static_cast<int>(num_rules)) {
                        ps.shift_totals[rule_idx] += cell.value;
                    }
                    if (cell.week >= 0 && cell.week < sheet.weeks) {
                        week_col_sums[cell.week][sc] += cell.value;
                    }
                }
            }
        }

        for (size_t i = 0; i < num_rules; ++i) {
            summary.shift_grand_totals[i] += ps.shift_totals[i];
            if (config.shift_rules[i].enabled) {
                ps.weighted_sum += ps.shift_totals[i] * config.shift_rules[i].weight;
            }
        }

        ps.final_score = ps.weighted_sum * ps.effective_coeff;
        summary.weighted_grand_total += ps.weighted_sum;
        summary.final_grand_total += ps.final_score;
        summary.people.push_back(std::move(ps));
    }

    // Check per-week shift column totals against expected calendar bounds
    for (int w = 0; w < sheet.weeks; ++w) {
        for (int c = 0; c < cols_per_week; ++c) {
            int rule_idx = col_to_rule[c];
            if (rule_idx < 0 || !config.shift_rules[rule_idx].enabled) continue;
            std::string ck = canonical_shift_key(sheet.column_shift_names[c]);
            double s = week_col_sums[w][c];
            if ((ck == "Z" || ck == "P" || ck == "Y") && s > 9.0) {
                std::ostringstream oss;
                oss << "第" << (w + 1) << "周 " << sheet.column_shift_names[c]
                    << " 合计为 " << format_number_trim(s) << " 偏高，建议核对";
                summary.alerts.push_back(oss.str());
            }
        }
    }

    if (summary.pending_review_count > 0) {
        std::ostringstream oss;
        oss << "待确认 " << summary.pending_review_count << " 处";
        summary.alerts.insert(summary.alerts.begin(), oss.str());
    }

    return summary;
}

std::string format_number_2dp(double value) {
    if (std::fabs(value) < 1e-9) value = 0.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << value;
    return oss.str();
}

std::string format_number_trim(double value) {
    if (std::fabs(value) < 1e-9) return "0";
    double rounded = std::round(value * 100.0) / 100.0;
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << rounded;
    std::string s = oss.str();
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

} // namespace nurseocr
