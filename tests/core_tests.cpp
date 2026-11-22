#include "nurseocr/calculator.h"
#include "nurseocr/image_io.h"
#include "nurseocr/name_doc_reader.h"
#include "nurseocr/ocr_engine.h"
#include "nurseocr/storage.h"
#include "nurseocr/table_cv.h"
#include "nurseocr/xlsx_exporter.h"

#include <miniz.h>
#include <pugixml.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace nurseocr;

namespace {

bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

// Builds a synthetic valid 1x1 lossless WebP image in memory using standard RIFF header
std::vector<uint8_t> make_tiny_png_bytes() {
    // Minimal 1x1 white RGB PNG
    static const uint8_t kPng[] = {
        0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,
        0xDE,0x00,0x00,0x00,0x0C,0x49,0x44,0x41,0x54,0x08,0xD7,0x63,0xF8,0xFF,0xFF,0x3F,
        0x00,0x05,0xFE,0x02,0xFE,0xDC,0xCC,0x59,0xE7,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,
        0x44,0xAE,0x42,0x60,0x82
    };
    return std::vector<uint8_t>(kPng, kPng + sizeof(kPng));
}

std::vector<uint8_t> make_synthetic_docx(const std::string& xml_body) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    mz_zip_writer_init_heap(&zip, 0, 4096);
    std::string doc =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\">"
        "<w:body>" + xml_body + "</w:body></w:document>";
    mz_zip_writer_add_mem(&zip, "word/document.xml", doc.data(), doc.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t sz = 0;
    mz_zip_writer_finalize_heap_archive(&zip, &buf, &sz);
    std::vector<uint8_t> out(static_cast<uint8_t*>(buf), static_cast<uint8_t*>(buf) + sz);
    mz_free(buf);
    mz_zip_writer_end(&zip);
    return out;
}

void test_default_config_is_blank_staff() {
    AppConfig cfg = create_default_config();
    assert(cfg.staff.empty());
    assert(cfg.shift_rules.size() == 5);
    assert(cfg.shift_rules[0].enabled == true);
    assert(cfg.shift_rules[4].enabled == false); // 技术难度 default disabled
    assert(cfg.coeff_flags.enable_title_coeff);
    assert(cfg.coeff_flags.enable_duty_coeff);

    std::string json_str = serialize_app_config_json(cfg);
    AppConfig roundtrip;
    std::string err;
    assert(deserialize_app_config_json(json_str, roundtrip, err));
    assert(roundtrip.staff.empty());
    assert(roundtrip.shift_rules.size() == 5);
    std::cout << "[PASS] test_default_config_is_blank_staff\n";
}

void test_calculator_switches_and_precision() {
    AppConfig cfg = create_default_config();
    // Synthetic fictional staff only
    cfg.staff = {
        {"张晓敏", 1.10, 0.05},
        {"王静", 1.00, 0.00},
    };

    WorkloadSheet sheet;
    sheet.year = 2026;
    sheet.month = 8;
    sheet.weeks = 2;
    sheet.column_shift_codes = {"A", "Z", "P", "Y", "T"};
    sheet.column_shift_names = {"A(主)", "治/白", "P", "夜", "技术难度"};

    WorkloadRow r1;
    r1.row = 0;
    r1.staff_name = "张晓敏";
    // Week 1: A=2, Z=1.5, P=1, Y=1, T=2(disabled)
    r1.cells = {
        {0, 0, 0, ValueKind::Number, 2.0},
        {0, 0, 1, ValueKind::Number, 1.5},
        {0, 0, 2, ValueKind::Number, 1.0},
        {0, 0, 3, ValueKind::Number, 1.0},
        {0, 0, 4, ValueKind::Number, 2.0},
        // Week 2: blank
        {0, 1, 0, ValueKind::Blank, 0.0},
        {0, 1, 1, ValueKind::Blank, 0.0},
        {0, 1, 2, ValueKind::Blank, 0.0},
        {0, 1, 3, ValueKind::Blank, 0.0},
        {0, 1, 4, ValueKind::Blank, 0.0},
    };
    WorkloadRow r2;
    r2.row = 1;
    r2.staff_name = "王静";
    r2.cells = {
        {1, 0, 0, ValueKind::Number, 1.0},
        {1, 0, 1, ValueKind::Blank, 0.0},
        {1, 0, 2, ValueKind::Blank, 0.0},
        {1, 0, 3, ValueKind::Blank, 0.0},
        {1, 0, 4, ValueKind::Blank, 0.0},
    };
    sheet.rows = {r1, r2};

    // Case 1: default switches (T disabled, title+duty enabled)
    SheetSummary sum = evaluate_workload(sheet, cfg);
    double expected_w1 = 1.5 * 1.00 + 2.0 * 1.05 + 1.0 * 1.10 + 1.0 * 1.20; // 1.5 + 2.1 + 1.1 + 1.2 = 5.90
    assert(approx(sum.people[0].weighted_sum, expected_w1));
    assert(approx(sum.people[0].effective_coeff, 1.15));
    assert(approx(sum.people[0].final_score, expected_w1 * 1.15)); // 6.785
    assert(approx(sum.people[1].final_score, 1.05));
    assert(approx(sum.final_grand_total, 6.785 + 1.05)); // full precision sum = 7.835 -> "7.84"
    assert(format_number_2dp(sum.final_grand_total) == "7.84");

    // Case 2: disable duty coeff & disable P shift, enable T shift
    cfg.coeff_flags.enable_duty_coeff = false;
    cfg.shift_rules[2].enabled = false; // P off
    cfg.shift_rules[4].enabled = true;  // 技术难度 on (weight 1.0)
    SheetSummary sum2 = evaluate_workload(sheet, cfg);
    double expected_w2 = 1.5 * 1.00 + 2.0 * 1.05 + 1.0 * 1.20 + 2.0 * 1.00; // 6.80
    assert(approx(sum2.people[0].weighted_sum, expected_w2));
    assert(approx(sum2.people[0].effective_coeff, 1.10));
    assert(approx(sum2.people[0].final_score, 6.80 * 1.10));

    // Case 3: disable both title and duty coeffs -> coeff falls back to 1.0
    cfg.coeff_flags.enable_title_coeff = false;
    cfg.coeff_flags.enable_duty_coeff = false;
    SheetSummary sum3 = evaluate_workload(sheet, cfg);
    assert(approx(sum3.people[0].effective_coeff, 1.0));
    assert(approx(sum3.people[0].final_score, 6.80));

    std::cout << "[PASS] test_calculator_switches_and_precision\n";
}

void test_name_doc_and_alignment() {
    // Plain text with UTF-8 BOM + separators + noise headers
    std::string txt = "\xEF\xBB\xBF姓名\n1 张晓敏\n王 静、陈晨；周雪";
    std::vector<std::string> names;
    std::string err;
    assert(read_names_from_bytes("staff.txt",
                                 reinterpret_cast<const uint8_t*>(txt.data()),
                                 txt.size(), names, err));
    assert(names.size() == 4);
    assert(names[0] == "张晓敏");
    assert(names[1] == "王静");
    assert(names[2] == "陈晨");
    assert(names[3] == "周雪");

    // DOCX with table paragraphs
    auto docx = make_synthetic_docx(
        "<w:p><w:r><w:t>张晓敏</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>李敏</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>陈艳</w:t></w:r></w:p>");
    std::vector<std::string> dnames;
    assert(read_names_from_bytes("names.docx", docx.data(), docx.size(), dnames, err));
    assert(dnames.size() == 3);
    assert(dnames[1] == "李敏");

    // Homophone unique alignment (1-to-1)
    std::vector<std::string> ocr = {"张小敏", "陈燕", "王 静"};
    std::vector<std::string> dict = {"张晓敏", "王静", "陈艳"};
    auto aligned = align_names_unique(ocr, dict, 0.48);
    assert(aligned[0].matched_name == "张晓敏");
    assert(aligned[1].matched_name == "陈艳");
    assert(aligned[2].matched_name == "王静");

    std::cout << "[PASS] test_name_doc_and_alignment\n";
}

void test_image_magic_and_xlsx_export() {
    auto png = make_tiny_png_bytes();
    assert(detect_image_format(png.data(), png.size()) == ImageFormat::Png);
    ImageRgb img;
    std::string err;
    assert(decode_image_bytes(png.data(), png.size(), img, err));
    assert(img.width == 1 && img.height == 1);

    // Export synthetic XLSX to temp path and inspect XML integrity
    std::filesystem::path tmp_xlsx = std::filesystem::temp_directory_path() / "nurseocr_synthetic_test.xlsx";
    AppConfig cfg = create_default_config();
    cfg.staff = {{"张晓敏", 1.12, 0.03}, {"王静", 1.00, 0.00}};

    WorkloadSheet sheet;
    sheet.year = 2026;
    sheet.month = 8;
    sheet.weeks = 4;
    sheet.week_labels = default_week_labels(2026, 8, 4);
    sheet.column_shift_codes = {"A", "Z", "P", "Y", "T"};
    sheet.column_shift_names = {"A(主)", "治/白", "P", "夜", "技术难度"};

    WorkloadRow r1{0, "张晓敏", "张晓敏", 0.99f, false};
    r1.cells.resize(20);
    r1.cells[0] = {0, 0, 0, ValueKind::Number, 3.0};
    r1.cells[1] = {0, 0, 1, ValueKind::Number, 0.5};
    WorkloadRow r2{1, "王静", "王静", 0.99f, false};
    r2.cells.resize(20);
    r2.cells[2] = {1, 0, 2, ValueKind::Number, 2.0};
    sheet.rows = {r1, r2};

    SheetSummary sum = evaluate_workload(sheet, cfg);
    ExportOptions opts;
    opts.include_summary_sheet = true;
    assert(export_workload_xlsx(tmp_xlsx.u8string(), sheet, cfg, sum, opts, err));

    // Read generated zip and check required entries and XML well-formedness
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    assert(mz_zip_reader_init_file(&zip, tmp_xlsx.string().c_str(), 0));
    assert(mz_zip_reader_locate_file(&zip, "xl/worksheets/sheet1.xml", nullptr, 0) >= 0);
    assert(mz_zip_reader_locate_file(&zip, "xl/worksheets/sheet5.xml", nullptr, 0) >= 0);
    size_t s1_sz = 0;
    void* s1_ptr = mz_zip_reader_extract_file_to_heap(&zip, "xl/worksheets/sheet1.xml", &s1_sz, 0);
    assert(s1_ptr != nullptr);
    pugi::xml_document doc;
    assert(doc.load_buffer(s1_ptr, s1_sz));
    std::string s1_txt(static_cast<const char*>(s1_ptr), s1_sz);
    assert(s1_txt.find("INDEX('人员系数'") != std::string::npos);
    mz_free(s1_ptr);
    mz_zip_reader_end(&zip);
    std::filesystem::remove(tmp_xlsx);

    std::cout << "[PASS] test_image_magic_and_xlsx_export\n";
}

void test_synthetic_table_grid_and_ocr_load() {
    // Draw a crisp synthetic 3-row x 6-col table image (name + 1 week of 5 cols)
    const int W = 900, H = 360;
    ImageRgb table;
    table.width = W;
    table.height = H;
    table.data.assign(static_cast<size_t>(W) * H * 3, 248);

    std::vector<int> xs = {60, 220, 340, 460, 580, 700, 820};
    std::vector<int> ys = {40, 100, 180, 260, 320};
    auto draw_hline = [&](int y, int x0, int x1) {
        for (int yy = y - 1; yy <= y + 1; ++yy) {
            for (int x = x0; x <= x1; ++x) {
                uint8_t* p = &table.data[(static_cast<size_t>(yy) * W + x) * 3];
                p[0] = p[1] = p[2] = 25;
            }
        }
    };
    auto draw_vline = [&](int x, int y0, int y1) {
        for (int y = y0; y <= y1; ++y) {
            for (int xx = x - 1; xx <= x + 1; ++xx) {
                uint8_t* p = &table.data[(static_cast<size_t>(y) * W + xx) * 3];
                p[0] = p[1] = p[2] = 25;
            }
        }
    };
    for (int y : ys) draw_hline(y, xs.front(), xs.back());
    for (int x : xs) draw_vline(x, ys.front(), ys.back());

    // Draw a handwritten-like vertical '1' inside cell (row=1, col=1)
    for (int y = 118; y <= 162; ++y) {
        for (int x = 278; x <= 282; ++x) {
            uint8_t* p = &table.data[(static_cast<size_t>(y) * W + x) * 3];
            p[0] = p[1] = p[2] = 20;
        }
    }

    std::array<Point2f, 4> corners{};
    assert(detect_table_corners(table, corners));
    assert(std::fabs(corners[0].x - 60.0f) < 12.0f);
    assert(std::fabs(corners[2].x - 820.0f) < 12.0f);

    RectifiedGrid grid;
    std::string err;
    assert(extract_rectified_table_grid(table, corners, 6, 4, grid, err));
    assert(grid.row_y_base.size() == ys.size());
    assert(grid.col_x_base.size() == xs.size());

    float ref = compute_table_ink_reference(grid, 1, 4, 1, 6);
    CellInkAnalysis ink1 = analyze_digit_cell(grid, 1, 1, ref);
    assert(ink1.category == InkCategory::ValidInk);
    assert(ink1.tall_thin_single_stroke);

    CellInkAnalysis empty_ink = analyze_digit_cell(grid, 1, 2, ref);
    assert(empty_ink.category == InkCategory::Empty);

    // Load real ncnn PP-OCRv5 models if present in NURSEOCR_SOURCE_DIR/models
    std::filesystem::path model_dir = std::filesystem::path(NURSEOCR_SOURCE_DIR) / "models";
    if (std::filesystem::exists(model_dir / "PP_OCRv5_mobile_rec.ncnn.bin")) {
        OcrEngine engine;
        assert(engine.init_from_dir(model_dir.u8string(), err));
        assert(engine.is_ready());
    }

    std::cout << "[PASS] test_synthetic_table_grid_and_ocr_load\n";
}

} // namespace

int main() {
    test_default_config_is_blank_staff();
    test_calculator_switches_and_precision();
    test_name_doc_and_alignment();
    test_image_magic_and_xlsx_export();
    test_synthetic_table_grid_and_ocr_load();
    std::cout << "ALL CORE TESTS PASSED\n";
    return 0;
}
