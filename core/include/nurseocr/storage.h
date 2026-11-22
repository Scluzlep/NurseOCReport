#pragma once

#include "nurseocr/types.h"
#include <string>

namespace nurseocr {

AppConfig create_default_config(); // Note: staff list is always empty

bool load_app_config(const std::string& json_path, AppConfig& out_config, std::string& out_err);
bool save_app_config(const std::string& json_path, const AppConfig& config, std::string& out_err);

std::string serialize_app_config_json(const AppConfig& config);
bool deserialize_app_config_json(const std::string& json_text, AppConfig& out_config, std::string& out_err);

std::string serialize_workload_sheet_json(const WorkloadSheet& sheet);
std::string serialize_sheet_summary_json(const SheetSummary& summary);
std::string serialize_coeff_result_json(const CoeffTableResult& res);

} // namespace nurseocr
