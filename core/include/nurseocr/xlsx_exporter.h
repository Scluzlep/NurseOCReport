#pragma once

#include "nurseocr/types.h"
#include <string>

namespace nurseocr {

struct ExportOptions {
    bool include_summary_sheet = true;
    std::string department_title; // optional prefix, empty by default
};

bool export_workload_xlsx(const std::string& out_path,
                          const WorkloadSheet& sheet,
                          const AppConfig& config,
                          const SheetSummary& summary,
                          const ExportOptions& options,
                          std::string& out_err);

} // namespace nurseocr
