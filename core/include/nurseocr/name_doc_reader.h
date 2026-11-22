#pragma once

#include "nurseocr/types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nurseocr {

struct NameMatchOutcome {
    std::string matched_name;
    double score = 0.0; // 0.0 .. 1.0
    bool exact = false;
};

// Extracts unique person names from .txt, .csv, .docx, or .xlsx bytes/file.
// Handles UTF-8 (with/without BOM) and GBK/GB18030 plain text cleanly.
bool read_names_from_file(const std::string& path,
                          std::vector<std::string>& out_names,
                          std::string& out_err);

bool read_names_from_bytes(const std::string& filename_hint,
                           const uint8_t* bytes,
                           size_t size,
                           std::vector<std::string>& out_names,
                           std::string& out_err);

std::vector<std::string> parse_names_from_plain_text(const std::string& utf8_or_gbk_text);

std::string normalize_person_name(const std::string& raw);

// Computes similarity in [0, 1] between an OCR string and a dictionary name,
// combining UTF-8 character edit distance and common homophone/shape groups.
double person_name_similarity(const std::string& ocr_raw, const std::string& dict_name);

// Globally unique 1-to-1 optimal assignment (Hungarian algorithm) from OCR names
// to imported dictionary names. Leaves low-similarity rows unchanged/flagged.
std::vector<NameMatchOutcome> align_names_unique(
    const std::vector<std::string>& ocr_names,
    const std::vector<std::string>& dict_names,
    double min_accept_score = 0.48);

// Applies name list calibration onto workload rows or staff coeff rows in place.
int apply_name_list_to_workload(WorkloadSheet& sheet,
                                const std::vector<std::string>& dict_names);

int apply_name_list_to_staff(std::vector<StaffEntry>& staff,
                             const std::vector<std::string>& dict_names);

} // namespace nurseocr
