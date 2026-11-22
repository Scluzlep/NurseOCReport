#pragma once

#include "nurseocr/types.h"
#include <string>
#include <vector>

namespace nurseocr {

std::vector<ShiftRule> default_shift_rules();
std::vector<std::string> default_week_labels(int year, int month, int weeks);

double compute_effective_coeff(const StaffEntry& entry, const CoeffFlags& flags);

int find_shift_rule_index(const std::vector<ShiftRule>& rules,
                          const std::string& code,
                          const std::string& name);

SheetSummary evaluate_workload(const WorkloadSheet& sheet,
                               const AppConfig& config);

std::string format_number_2dp(double value);
std::string format_number_trim(double value);

} // namespace nurseocr
