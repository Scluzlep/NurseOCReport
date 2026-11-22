#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace nurseocr {

struct Point2f {
    float x = 0.0f;
    float y = 0.0f;
};

struct RectF {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
};

struct ImageRgb {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data; // RGBRGB... length = width * height * 3

    bool valid() const {
        return width > 0 && height > 0 &&
               data.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 3;
    }
};

struct ShiftRule {
    std::string code;   // "A", "Z", "P", "Y", "T", or custom
    std::string name;   // e.g. "治/白", "A(主)", "P", "夜", "技术难度"
    double weight = 1.0;
    bool enabled = true;
};

struct CoeffFlags {
    bool enable_title_coeff = true;
    bool enable_duty_coeff = true;
};

struct StaffEntry {
    std::string name;
    double title_coeff = 1.0;
    double duty_coeff = 0.0;
};

enum class ValueKind {
    Blank = 0,
    Number = 1
};

enum class ReviewStatus {
    AutoConfirmed = 0,
    NeedsReview = 1,
    UserConfirmed = 2
};

enum class InkCategory {
    Empty = 0,
    SuspectErased = 1,
    ValidInk = 2,
    SuspectOverwrite = 3
};

struct WorkloadCell {
    int row = 0;
    int week = 0;
    int shift_col = 0;

    ValueKind kind = ValueKind::Blank;
    double value = 0.0;

    ValueKind ocr_kind = ValueKind::Blank;
    double ocr_value = 0.0;

    float confidence = 0.0f;
    InkCategory ink_cat = InkCategory::Empty;
    ReviewStatus status = ReviewStatus::AutoConfirmed;
    std::string review_reason;

    std::array<Point2f, 4> orig_quad{}; // TL, TR, BR, BL in working image coords
    bool locked_by_user = false;
};

struct WorkloadRow {
    int row = 0;
    std::string ocr_name;
    std::string staff_name;
    float name_confidence = 0.0f;
    bool name_needs_review = false;
    std::array<Point2f, 4> name_quad{};
    std::vector<WorkloadCell> cells; // size = weeks * cols_per_week
};

struct TableGeometry {
    int img_width = 0;
    int img_height = 0;
    int grid_rows = 0; // data rows
    int grid_cols = 0; // 1 (name) + weeks * cols_per_week
    std::array<Point2f, 4> corners{}; // TL, TR, BR, BL of table body + header
    // Optional full mesh of intersections: (grid_rows + 1) * (grid_cols + 1)
    std::vector<Point2f> grid_points;
};

struct WorkloadSheet {
    int year = 2026;
    int month = 8;
    int weeks = 5;
    std::vector<std::string> week_labels;
    std::vector<std::string> column_shift_codes; // e.g. {"A", "Z", "P", "Y", "T"}
    std::vector<std::string> column_shift_names; // e.g. {"A(主)", "治/白", "P", "夜", "技术难度"}
    std::vector<WorkloadRow> rows;
    TableGeometry geometry;
    int applied_rotation_deg = 0;
};

struct StaffCoeffRowOcr {
    int row = 0;
    std::string ocr_name;
    std::string matched_name;
    double title_coeff = 1.0;
    double duty_coeff = 0.0;
    double total_coeff = 1.0;
    bool needs_review = false;
    std::string review_reason;
    std::array<Point2f, 4> row_quad{};
};

struct CoeffTableResult {
    bool ok = false;
    std::string error;
    int applied_rotation_deg = 0;
    std::vector<StaffCoeffRowOcr> rows;
};

struct PersonScore {
    std::string name;
    double title_coeff = 1.0;
    double duty_coeff = 0.0;
    double effective_coeff = 1.0;
    bool matched_in_staff = false;
    std::vector<double> shift_totals; // parallel to ShiftRule list
    double weighted_sum = 0.0;
    double final_score = 0.0;
};

struct SheetSummary {
    std::vector<PersonScore> people;
    std::vector<double> shift_grand_totals;
    double weighted_grand_total = 0.0;
    double final_grand_total = 0.0;
    int pending_review_count = 0;
    std::vector<std::string> alerts;
};

struct AppConfig {
    std::vector<ShiftRule> shift_rules;
    CoeffFlags coeff_flags;
    std::vector<StaffEntry> staff; // Always empty by default
};

} // namespace nurseocr
