#include "nurseocr/storage.h"
#include "nurseocr/calculator.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nurseocr {

namespace {

bool read_utf8_file(const std::string& path, std::string& out_data, std::string& out_err) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(1, wlen)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
    FILE* f = _wfopen(wpath.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        out_err = "无法打开文件: " + path;
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    out_data.resize(static_cast<size_t>(sz));
    if (sz > 0) {
        size_t n = std::fread(&out_data[0], 1, static_cast<size_t>(sz), f);
        out_data.resize(n);
    }
    std::fclose(f);
    return true;
}

bool write_utf8_file(const std::string& path, const std::string& data, std::string& out_err) {
    try {
#ifdef _WIN32
        int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
        std::wstring wpath(static_cast<size_t>(std::max(1, wlen)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
        std::filesystem::path p(wpath);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        FILE* f = _wfopen(wpath.c_str(), L"wb");
#else
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }
        FILE* f = std::fopen(path.c_str(), "wb");
#endif
        if (!f) {
            out_err = "无法写入文件: " + path;
            return false;
        }
        if (!data.empty()) {
            std::fwrite(data.data(), 1, data.size(), f);
        }
        std::fclose(f);
        return true;
    } catch (const std::exception& ex) {
        out_err = ex.what();
        return false;
    }
}

} // namespace

AppConfig create_default_config() {
    AppConfig cfg;
    cfg.shift_rules = default_shift_rules();
    cfg.coeff_flags = CoeffFlags{true, true};
    cfg.staff.clear(); // Zero built-in personnel data
    return cfg;
}

std::string serialize_app_config_json(const AppConfig& config) {
    nlohmann::json j;
    j["coeffFlags"] = {
        {"enableTitleCoeff", config.coeff_flags.enable_title_coeff},
        {"enableDutyCoeff", config.coeff_flags.enable_duty_coeff},
    };
    j["shiftRules"] = nlohmann::json::array();
    for (const auto& r : config.shift_rules) {
        j["shiftRules"].push_back({
            {"code", r.code},
            {"name", r.name},
            {"weight", r.weight},
            {"enabled", r.enabled},
        });
    }
    j["staff"] = nlohmann::json::array();
    for (const auto& s : config.staff) {
        j["staff"].push_back({
            {"name", s.name},
            {"titleCoeff", s.title_coeff},
            {"dutyCoeff", s.duty_coeff},
        });
    }
    return j.dump(2);
}

bool deserialize_app_config_json(const std::string& json_text, AppConfig& out_config, std::string& out_err) {
    try {
        nlohmann::json j = nlohmann::json::parse(json_text);
        AppConfig cfg = create_default_config();
        if (j.contains("coeffFlags") && j["coeffFlags"].is_object()) {
            const auto& cf = j["coeffFlags"];
            cfg.coeff_flags.enable_title_coeff = cf.value("enableTitleCoeff", true);
            cfg.coeff_flags.enable_duty_coeff = cf.value("enableDutyCoeff", true);
        }
        if (j.contains("shiftRules") && j["shiftRules"].is_array() && !j["shiftRules"].empty()) {
            cfg.shift_rules.clear();
            for (const auto& r : j["shiftRules"]) {
                ShiftRule sr;
                sr.code = r.value("code", "");
                sr.name = r.value("name", "");
                sr.weight = r.value("weight", 1.0);
                sr.enabled = r.value("enabled", true);
                if (!sr.name.empty()) {
                    cfg.shift_rules.push_back(std::move(sr));
                }
            }
        }
        if (j.contains("staff") && j["staff"].is_array()) {
            cfg.staff.clear();
            for (const auto& s : j["staff"]) {
                StaffEntry se;
                se.name = s.value("name", "");
                se.title_coeff = s.value("titleCoeff", 1.0);
                se.duty_coeff = s.value("dutyCoeff", 0.0);
                if (!se.name.empty()) {
                    cfg.staff.push_back(std::move(se));
                }
            }
        }
        out_config = std::move(cfg);
        return true;
    } catch (const std::exception& ex) {
        out_err = ex.what();
        return false;
    }
}

bool load_app_config(const std::string& json_path, AppConfig& out_config, std::string& out_err) {
    std::string raw;
    if (!read_utf8_file(json_path, raw, out_err)) {
        return false;
    }
    if (raw.empty()) {
        out_config = create_default_config();
        return true;
    }
    return deserialize_app_config_json(raw, out_config, out_err);
}

bool save_app_config(const std::string& json_path, const AppConfig& config, std::string& out_err) {
    return write_utf8_file(json_path, serialize_app_config_json(config), out_err);
}

std::string serialize_workload_sheet_json(const WorkloadSheet& sheet) {
    nlohmann::json j;
    j["year"] = sheet.year;
    j["month"] = sheet.month;
    j["weeks"] = sheet.weeks;
    j["weekLabels"] = sheet.week_labels;
    j["columnShiftCodes"] = sheet.column_shift_codes;
    j["columnShiftNames"] = sheet.column_shift_names;
    j["appliedRotationDeg"] = sheet.applied_rotation_deg;
    j["geometry"] = {
        {"imgWidth", sheet.geometry.img_width},
        {"imgHeight", sheet.geometry.img_height},
        {"gridRows", sheet.geometry.grid_rows},
        {"gridCols", sheet.geometry.grid_cols},
        {"corners", nlohmann::json::array({
            {{"x", sheet.geometry.corners[0].x}, {"y", sheet.geometry.corners[0].y}},
            {{"x", sheet.geometry.corners[1].x}, {"y", sheet.geometry.corners[1].y}},
            {{"x", sheet.geometry.corners[2].x}, {"y", sheet.geometry.corners[2].y}},
            {{"x", sheet.geometry.corners[3].x}, {"y", sheet.geometry.corners[3].y}},
        })}
    };
    j["rows"] = nlohmann::json::array();
    for (const auto& r : sheet.rows) {
        nlohmann::json jr;
        jr["row"] = r.row;
        jr["ocrName"] = r.ocr_name;
        jr["staffName"] = r.staff_name;
        jr["nameConfidence"] = r.name_confidence;
        jr["nameNeedsReview"] = r.name_needs_review;
        jr["nameQuad"] = nlohmann::json::array({
            {{"x", r.name_quad[0].x}, {"y", r.name_quad[0].y}},
            {{"x", r.name_quad[1].x}, {"y", r.name_quad[1].y}},
            {{"x", r.name_quad[2].x}, {"y", r.name_quad[2].y}},
            {{"x", r.name_quad[3].x}, {"y", r.name_quad[3].y}},
        });
        jr["cells"] = nlohmann::json::array();
        for (const auto& c : r.cells) {
            nlohmann::json jc;
            jc["row"] = c.row;
            jc["week"] = c.week;
            jc["shiftCol"] = c.shift_col;
            jc["isBlank"] = (c.kind == ValueKind::Blank);
            jc["value"] = c.value;
            jc["confidence"] = c.confidence;
            jc["status"] = static_cast<int>(c.status);
            jc["reviewReason"] = c.review_reason;
            jc["quad"] = nlohmann::json::array({
                {{"x", c.orig_quad[0].x}, {"y", c.orig_quad[0].y}},
                {{"x", c.orig_quad[1].x}, {"y", c.orig_quad[1].y}},
                {{"x", c.orig_quad[2].x}, {"y", c.orig_quad[2].y}},
                {{"x", c.orig_quad[3].x}, {"y", c.orig_quad[3].y}},
            });
            jr["cells"].push_back(std::move(jc));
        }
        j["rows"].push_back(std::move(jr));
    }
    return j.dump();
}

std::string serialize_sheet_summary_json(const SheetSummary& summary) {
    nlohmann::json j;
    j["pendingReviewCount"] = summary.pending_review_count;
    j["alerts"] = summary.alerts;
    j["shiftGrandTotals"] = summary.shift_grand_totals;
    j["weightedGrandTotal"] = summary.weighted_grand_total;
    j["finalGrandTotal"] = summary.final_grand_total;
    j["people"] = nlohmann::json::array();
    for (const auto& p : summary.people) {
        j["people"].push_back({
            {"name", p.name},
            {"titleCoeff", p.title_coeff},
            {"dutyCoeff", p.duty_coeff},
            {"effectiveCoeff", p.effective_coeff},
            {"matchedInStaff", p.matched_in_staff},
            {"shiftTotals", p.shift_totals},
            {"weightedSum", p.weighted_sum},
            {"finalScore", p.final_score},
        });
    }
    return j.dump();
}

std::string serialize_coeff_result_json(const CoeffTableResult& res) {
    nlohmann::json j;
    j["ok"] = res.ok;
    j["error"] = res.error;
    j["appliedRotationDeg"] = res.applied_rotation_deg;
    j["rows"] = nlohmann::json::array();
    for (const auto& r : res.rows) {
        j["rows"].push_back({
            {"row", r.row},
            {"ocrName", r.ocr_name},
            {"matchedName", r.matched_name},
            {"titleCoeff", r.title_coeff},
            {"dutyCoeff", r.duty_coeff},
            {"totalCoeff", r.total_coeff},
            {"needsReview", r.needs_review},
            {"reviewReason", r.review_reason},
        });
    }
    return j.dump();
}

} // namespace nurseocr
