#include "nurseocr/calculator.h"
#include "nurseocr/image_io.h"
#include "nurseocr/name_doc_reader.h"
#include "nurseocr/ocr_engine.h"
#include "nurseocr/storage.h"
#include "nurseocr/xlsx_exporter.h"

#include <iostream>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

using namespace nurseocr;

int main(int argc, char** argv) {
    std::vector<std::string> utf8_args;
#ifdef _WIN32
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    if (wargv) {
        for (int i = 0; i < wargc; ++i) {
            int len = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr, 0, nullptr, nullptr);
            std::string s(static_cast<size_t>(std::max(1, len)), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, &s[0], len, nullptr, nullptr);
            if (!s.empty() && s.back() == '\0') s.pop_back();
            utf8_args.push_back(std::move(s));
        }
        LocalFree(wargv);
    }
#else
    for (int i = 0; i < argc; ++i) utf8_args.emplace_back(argv[i]);
#endif
    const int n_args = static_cast<int>(utf8_args.size());
    std::string model_dir = "models";
    std::string coeff_img_path;
    std::string workload_img_path;
    std::string names_doc_path;
    std::string out_xlsx_path;
    int year = 2026;
    int month = 8;

    for (int i = 1; i < n_args; ++i) {
        const std::string& a = utf8_args[i];
        auto next_arg = [&]() -> std::string {
            return (i + 1 < n_args) ? utf8_args[++i] : "";
        };
        if (a == "--models") model_dir = next_arg();
        else if (a == "--coeff") coeff_img_path = next_arg();
        else if (a == "--workload") workload_img_path = next_arg();
        else if (a == "--names") names_doc_path = next_arg();
        else if (a == "--out") out_xlsx_path = next_arg();
        else if (a == "--year") year = std::atoi(next_arg().c_str());
        else if (a == "--month") month = std::atoi(next_arg().c_str());
    }

    OcrEngine engine;
    std::string err;
    if (!engine.init_from_dir(model_dir, err)) {
        std::cerr << "Init failed: " << err << "\n";
        return 1;
    }

    AppConfig cfg = create_default_config();
    std::vector<std::string> dict_names;
    if (!names_doc_path.empty()) {
        if (!read_names_from_file(names_doc_path, dict_names, err)) {
            std::cerr << "Names file error: " << err << "\n";
            return 1;
        }
        std::cout << "Loaded name list count: " << dict_names.size() << "\n";
    }

    if (!coeff_img_path.empty()) {
        ImageRgb cimg;
        if (!load_image_file(coeff_img_path, cimg, err)) {
            std::cerr << "Coeff image load failed: " << err << "\n";
            return 1;
        }
        CoeffTableResult cres;
        if (!engine.process_coeff_photo(cimg, dict_names, cres)) {
            std::cerr << "Coeff OCR failed: " << cres.error << "\n";
            return 1;
        }
        std::cout << "Coeff rows: " << cres.rows.size()
                  << " (auto-rot=" << cres.applied_rotation_deg << " deg)\n";
        for (const auto& r : cres.rows) {
            std::cout << "  [" << r.row + 1 << "] " << r.ocr_name << " -> " << r.matched_name
                      << "  title=" << r.title_coeff << " duty=" << r.duty_coeff
                      << " total=" << r.total_coeff
                      << (r.needs_review ? (" (" + r.review_reason + ")") : "") << "\n";
            cfg.staff.push_back({r.matched_name, r.title_coeff, r.duty_coeff});
        }
    }

    if (!workload_img_path.empty()) {
        ImageRgb wimg;
        if (!load_image_file(workload_img_path, wimg, err)) {
            std::cerr << "Workload image load failed: " << err << "\n";
            return 1;
        }
        int rot = 0;
        ImageRgb oriented = engine.auto_orient_table_photo(wimg, rot);
        WorkloadSheet sheet;
        if (!engine.process_workload_photo(oriented, year, month, cfg, nullptr, sheet, err)) {
            std::cerr << "Workload OCR failed: " << err << "\n";
            return 1;
        }
        sheet.applied_rotation_deg = rot;
        SheetSummary sum = evaluate_workload(sheet, cfg);
        std::cout << "Workload grid: " << sheet.rows.size() << " rows x " << sheet.weeks
                  << " weeks, pending=" << sum.pending_review_count
                  << ", grandTotal=" << format_number_2dp(sum.final_grand_total) << "\n";
        for (size_t i = 0; i < sheet.rows.size(); ++i) {
            const auto& row = sheet.rows[i];
            std::cout << "  row " << i + 1 << ": " << row.ocr_name << " -> " << row.staff_name;
            if (i < sum.people.size()) {
                std::cout << " | weighted=" << format_number_2dp(sum.people[i].weighted_sum)
                          << " coeff=" << format_number_2dp(sum.people[i].effective_coeff)
                          << " final=" << format_number_2dp(sum.people[i].final_score);
            }
            std::cout << "\n    cells: ";
            for (const auto& c : row.cells) {
                if (c.kind == ValueKind::Number) std::cout << format_number_trim(c.value);
                else std::cout << ".";
                if (c.status == ReviewStatus::NeedsReview) std::cout << "?";
                std::cout << " ";
            }
            std::cout << "\n";
        }
        for (const auto& al : sum.alerts) {
            std::cout << "  ALERT: " << al << "\n";
        }

        if (!out_xlsx_path.empty()) {
            ExportOptions opts;
            opts.include_summary_sheet = true;
            if (!export_workload_xlsx(out_xlsx_path, sheet, cfg, sum, opts, err)) {
                std::cerr << "XLSX export failed: " << err << "\n";
                return 1;
            }
            std::cout << "Exported XLSX: " << out_xlsx_path << "\n";
        }
    }

    return 0;
}
