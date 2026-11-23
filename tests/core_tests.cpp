#include "nurseocr/calculator.h"
#include "nurseocr/image_io.h"
#include "nurseocr/name_doc_reader.h"
#include "nurseocr/ocr_engine.h"
#include "nurseocr/storage.h"
#include "nurseocr/table_cv.h"
#include "nurseocr/xlsx_exporter.h"

#include <miniz.h>
#include <pugixml.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace nurseocr;

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::cerr << "CHECK FAILED at " << __FILE__ << ":" << __LINE__ << ": "       \
                      << #cond << "\n";                                                  \
            std::exit(1);                                                                \
        }                                                                                \
    } while (false)

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
    CHECK(cfg.staff.empty());
    CHECK(cfg.shift_rules.size() == 5);
    CHECK(cfg.shift_rules[0].enabled == true);
    CHECK(cfg.shift_rules[4].enabled == false); // 技术难度 default disabled
    CHECK(cfg.coeff_flags.enable_title_coeff);
    CHECK(cfg.coeff_flags.enable_duty_coeff);

    std::string json_str = serialize_app_config_json(cfg);
    AppConfig roundtrip;
    std::string err;
    CHECK(deserialize_app_config_json(json_str, roundtrip, err));
    CHECK(roundtrip.staff.empty());
    CHECK(roundtrip.shift_rules.size() == 5);
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

    auto make_num_cell = [](int r, int w, int sc, double v) {
        WorkloadCell c;
        c.row = r;
        c.week = w;
        c.shift_col = sc;
        c.kind = ValueKind::Number;
        c.value = v;
        return c;
    };
    auto make_blank_cell = [](int r, int w, int sc) {
        WorkloadCell c;
        c.row = r;
        c.week = w;
        c.shift_col = sc;
        c.kind = ValueKind::Blank;
        return c;
    };

    WorkloadRow r1;
    r1.row = 0;
    r1.staff_name = "张晓敏";
    // Week 1: A=2, Z=1.5, P=1, Y=1, T=2(disabled)
    r1.cells = {
        make_num_cell(0, 0, 0, 2.0),
        make_num_cell(0, 0, 1, 1.5),
        make_num_cell(0, 0, 2, 1.0),
        make_num_cell(0, 0, 3, 1.0),
        make_num_cell(0, 0, 4, 2.0),
        make_blank_cell(0, 1, 0),
        make_blank_cell(0, 1, 1),
        make_blank_cell(0, 1, 2),
        make_blank_cell(0, 1, 3),
        make_blank_cell(0, 1, 4),
    };
    WorkloadRow r2;
    r2.row = 1;
    r2.staff_name = "王静";
    r2.cells = {
        make_num_cell(1, 0, 0, 1.0),
        make_blank_cell(1, 0, 1),
        make_blank_cell(1, 0, 2),
        make_blank_cell(1, 0, 3),
        make_blank_cell(1, 0, 4),
    };
    sheet.rows = {r1, r2};

    // Case 1: default switches (T disabled, title+duty enabled)
    SheetSummary sum = evaluate_workload(sheet, cfg);
    double expected_w1 = 1.5 * 1.00 + 2.0 * 1.05 + 1.0 * 1.10 + 1.0 * 1.20; // 5.90
    CHECK(approx(sum.people[0].weighted_sum, expected_w1));
    CHECK(approx(sum.people[0].effective_coeff, 1.15));
    CHECK(approx(sum.people[0].final_score, expected_w1 * 1.15)); // 6.785
    CHECK(approx(sum.people[1].final_score, 1.05));
    CHECK(approx(sum.final_grand_total, 6.785 + 1.05)); // full precision sum = 7.835 -> "7.84"
    CHECK(format_number_2dp(sum.final_grand_total) == "7.84");

    // Case 2: disable duty coeff & disable P shift, enable T shift
    cfg.coeff_flags.enable_duty_coeff = false;
    cfg.shift_rules[2].enabled = false; // P off
    cfg.shift_rules[4].enabled = true;  // 技术难度 on (weight 1.0)
    SheetSummary sum2 = evaluate_workload(sheet, cfg);
    double expected_w2 = 1.5 * 1.00 + 2.0 * 1.05 + 1.0 * 1.20 + 2.0 * 1.00; // 6.80
    CHECK(approx(sum2.people[0].weighted_sum, expected_w2));
    CHECK(approx(sum2.people[0].effective_coeff, 1.10));
    CHECK(approx(sum2.people[0].final_score, expected_w2 * 1.10));

    // Case 3: disable both title and duty coeffs -> coeff falls back to 1.0
    cfg.coeff_flags.enable_title_coeff = false;
    cfg.coeff_flags.enable_duty_coeff = false;
    SheetSummary sum3 = evaluate_workload(sheet, cfg);
    CHECK(approx(sum3.people[0].effective_coeff, 1.0));
    CHECK(approx(sum3.people[0].final_score, 6.80));

    std::cout << "[PASS] test_calculator_switches_and_precision\n";
}

void test_name_doc_and_alignment() {
    // Plain text with UTF-8 BOM + separators + noise headers
    std::string txt = "\xEF\xBB\xBF姓名\n1 张晓敏\n王 静、陈晨；周雪";
    std::vector<std::string> names;
    std::string err;
    CHECK(read_names_from_bytes("staff.txt",
                                reinterpret_cast<const uint8_t*>(txt.data()),
                                txt.size(), names, err));
    CHECK(names.size() == 4);
    CHECK(names[0] == "张晓敏");
    CHECK(names[1] == "王静");
    CHECK(names[2] == "陈晨");
    CHECK(names[3] == "周雪");

    // Direct text input with ASCII space, full-width space, tab, newline, 、 ， , ; ；
    std::string direct =
        "张晓敏 王静\xE3\x80\x80陈晨\t李敏\r\n"
        "周雪\xE3\x80\x81赵敏\xEF\xBC\x8C孙丽,吴敏;郑敏\xEF\xBC\x9B钱敏";
    auto split_names = parse_names_from_plain_text(direct);
    CHECK(split_names.size() == 10);
    CHECK(split_names[0] == "张晓敏");
    CHECK(split_names[1] == "王静");
    CHECK(split_names[2] == "陈晨");
    CHECK(split_names[3] == "李敏");
    CHECK(split_names[4] == "周雪");
    CHECK(split_names[5] == "赵敏");
    CHECK(split_names[6] == "孙丽");
    CHECK(split_names[7] == "吴敏");
    CHECK(split_names[8] == "郑敏");
    CHECK(split_names[9] == "钱敏");

    // DOCX with table paragraphs
    auto docx = make_synthetic_docx(
        "<w:p><w:r><w:t>张晓敏</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>李敏</w:t></w:r></w:p>"
        "<w:p><w:r><w:t>陈艳</w:t></w:r></w:p>");
    std::vector<std::string> dnames;
    CHECK(read_names_from_bytes("names.docx", docx.data(), docx.size(), dnames, err));
    CHECK(dnames.size() == 3);
    CHECK(dnames[1] == "李敏");

    // Homophone unique alignment (1-to-1)
    std::vector<std::string> ocr = {"张小敏", "陈燕", "王 静"};
    std::vector<std::string> dict = {"张晓敏", "王静", "陈艳"};
    auto aligned = align_names_unique(ocr, dict, 0.48);
    CHECK(aligned[0].matched_name == "张晓敏");
    CHECK(aligned[1].matched_name == "陈艳");
    CHECK(aligned[2].matched_name == "王静");

    std::cout << "[PASS] test_name_doc_and_alignment\n";
}

void test_image_magic_and_xlsx_export() {
    auto png = make_tiny_png_bytes();
    CHECK(detect_image_format(png.data(), png.size()) == ImageFormat::Png);
    ImageRgb img;
    std::string err;
    CHECK(decode_image_bytes(png.data(), png.size(), img, err));
    CHECK(img.width == 1 && img.height == 1);

    // Export synthetic XLSX to temp path and inspect XML integrity & 0.00 formatting
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

    WorkloadRow r1;
    r1.row = 0;
    r1.ocr_name = "张晓敏";
    r1.staff_name = "张晓敏";
    r1.name_confidence = 0.99f;
    r1.cells.resize(20);
    r1.cells[0].row = 0; r1.cells[0].week = 0; r1.cells[0].shift_col = 0;
    r1.cells[0].kind = ValueKind::Number; r1.cells[0].value = 3.0;
    r1.cells[1].row = 0; r1.cells[1].week = 0; r1.cells[1].shift_col = 1;
    r1.cells[1].kind = ValueKind::Number; r1.cells[1].value = 0.5;
    WorkloadRow r2;
    r2.row = 1;
    r2.ocr_name = "王静";
    r2.staff_name = "王静";
    r2.name_confidence = 0.99f;
    r2.cells.resize(20);
    r2.cells[2].row = 1; r2.cells[2].week = 0; r2.cells[2].shift_col = 2;
    r2.cells[2].kind = ValueKind::Number; r2.cells[2].value = 2.0;
    sheet.rows = {r1, r2};

    SheetSummary sum = evaluate_workload(sheet, cfg);
    ExportOptions opts;
    opts.include_summary_sheet = true;
    CHECK(export_workload_xlsx(tmp_xlsx.u8string(), sheet, cfg, sum, opts, err));

    // Read generated zip and check required entries, XML well-formedness, and 0.00 styles
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    CHECK(mz_zip_reader_init_file(&zip, tmp_xlsx.string().c_str(), 0));
    CHECK(mz_zip_reader_locate_file(&zip, "xl/worksheets/sheet1.xml", nullptr, 0) >= 0);
    CHECK(mz_zip_reader_locate_file(&zip, "xl/worksheets/sheet5.xml", nullptr, 0) >= 0);

    auto read_zip_str = [&](const char* member) -> std::string {
        size_t sz = 0;
        void* ptr = mz_zip_reader_extract_file_to_heap(&zip, member, &sz, 0);
        CHECK(ptr != nullptr);
        pugi::xml_document doc;
        CHECK(doc.load_buffer(ptr, sz));
        std::string txt(static_cast<const char*>(ptr), sz);
        mz_free(ptr);
        return txt;
    };

    std::string s1_txt = read_zip_str("xl/worksheets/sheet1.xml");
    std::string styles_txt = read_zip_str("xl/styles.xml");
    CHECK(s1_txt.find("INDEX(&apos;人员系数&apos;") != std::string::npos);
    // Keep blank shift cells empty (D4 is blank) and numeric shift cells populated (B4=3, C4=0.5)
    CHECK(s1_txt.find("<c r=\"D4\" s=\"3\"/>") != std::string::npos);
    CHECK(s1_txt.find("<c r=\"B4\" s=\"3\"><v>3</v></c>") != std::string::npos);
    CHECK(s1_txt.find("<c r=\"C4\" s=\"3\"><v>0.5</v></c>") != std::string::npos);
    // Replace formatCode="0.##" with 0.00 while preserving formatCode="0" for seq numbers
    CHECK(styles_txt.find("formatCode=\"0.##\"") == std::string::npos);
    CHECK(styles_txt.find("formatCode=\"0.00\"") != std::string::npos);
    CHECK(styles_txt.find("formatCode=\"0\"") != std::string::npos);

    mz_zip_reader_end(&zip);
    std::filesystem::remove(tmp_xlsx);

    std::cout << "[PASS] test_image_magic_and_xlsx_export\n";
}

struct Glyph24 {
    const char* utf8;
    uint32_t rows[24];
};

const Glyph24 kTestGlyphs[] = {
    {"0", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x01E000, 0x021000, 0x021000, 0x040800, 0x040800, 0x040800, 0x040800, 0x040800, 0x040800, 0x040800, 0x040800, 0x021000, 0x021000, 0x01E000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"1", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x004000, 0x01C000, 0x004000, 0x004000, 0x004000, 0x004000, 0x004000, 0x004000, 0x004000, 0x004000, 0x004000, 0x004000, 0x004000, 0x01F000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"2", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x01E000, 0x021000, 0x040800, 0x040800, 0x040800, 0x000800, 0x001000, 0x002000, 0x004000, 0x008000, 0x010000, 0x020800, 0x040800, 0x07F800, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"3", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x03C000, 0x042000, 0x041000, 0x041000, 0x001000, 0x002000, 0x00E000, 0x001000, 0x000800, 0x000800, 0x040800, 0x040800, 0x041000, 0x03E000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"4", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x002000, 0x006000, 0x006000, 0x00A000, 0x00A000, 0x012000, 0x022000, 0x022000, 0x042000, 0x07F800, 0x002000, 0x002000, 0x002000, 0x00F800, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"5", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x03F800, 0x020000, 0x020000, 0x020000, 0x020000, 0x02E000, 0x031000, 0x000800, 0x000800, 0x000800, 0x040800, 0x040800, 0x041000, 0x03E000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"6", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x00E000, 0x011000, 0x021000, 0x040000, 0x040000, 0x05E000, 0x061000, 0x040800, 0x040800, 0x040800, 0x040800, 0x020800, 0x021000, 0x01E000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"7", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x07F800, 0x040800, 0x041000, 0x002000, 0x002000, 0x002000, 0x004000, 0x004000, 0x008000, 0x008000, 0x008000, 0x008000, 0x008000, 0x008000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"8", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x01E000, 0x021000, 0x040800, 0x040800, 0x040800, 0x021000, 0x01E000, 0x021000, 0x040800, 0x040800, 0x040800, 0x040800, 0x021000, 0x01E000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"9", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x01E000, 0x021000, 0x041000, 0x040800, 0x040800, 0x040800, 0x041800, 0x022800, 0x01C800, 0x000800, 0x001000, 0x021000, 0x022000, 0x01C000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {".", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x060000, 0x060000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"-", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x07F800, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"年", {0x000000, 0x000000, 0x000000, 0x000000, 0x018000, 0x010000, 0x03F7F0, 0x020800, 0x040800, 0x080840, 0x12FFE0, 0x030800, 0x030800, 0x030810, 0x0FFFF8, 0x000800, 0x000800, 0x000800, 0x000800, 0x000800, 0x000800, 0x000000, 0x000000, 0x000000}},
    {"月", {0x000000, 0x000000, 0x000000, 0x01FF80, 0x018080, 0x018080, 0x018080, 0x018080, 0x01FF80, 0x018080, 0x018080, 0x018080, 0x01FF80, 0x010080, 0x010080, 0x010080, 0x010080, 0x020080, 0x040080, 0x0C0380, 0x100100, 0x000000, 0x000000, 0x000000}},
    {"第", {0x000000, 0x000000, 0x000000, 0x010200, 0x030200, 0x037E70, 0x048580, 0x08C880, 0x13F7C0, 0x000840, 0x000840, 0x03FFC0, 0x020840, 0x020800, 0x07DFD0, 0x002830, 0x004820, 0x008820, 0x0308E0, 0x0C0840, 0x100800, 0x000000, 0x000000, 0x000000}},
    {"一", {0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000010, 0x0FFFF0, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"周", {0x000000, 0x000000, 0x000000, 0x03FFE0, 0x020020, 0x020820, 0x020820, 0x027F20, 0x020820, 0x020820, 0x03FFA0, 0x020020, 0x027F20, 0x024320, 0x044320, 0x044320, 0x047F20, 0x044320, 0x080020, 0x1000E0, 0x100040, 0x000000, 0x000000, 0x000000}},
    {"惟", {0x000000, 0x000000, 0x000000, 0x021000, 0x021900, 0x021180, 0x021080, 0x033EF8, 0x0AA100, 0x0AE100, 0x1A7FF0, 0x02A100, 0x022100, 0x022100, 0x023FF0, 0x022100, 0x022100, 0x022100, 0x023FF8, 0x022000, 0x022000, 0x000000, 0x000000, 0x000000}},
    {"名", {0x000000, 0x000000, 0x000000, 0x002000, 0x003000, 0x006000, 0x007F40, 0x00C0C0, 0x008180, 0x014300, 0x022600, 0x040C00, 0x001000, 0x007FE0, 0x01C020, 0x064020, 0x084020, 0x004020, 0x004020, 0x007FE0, 0x004000, 0x000000, 0x000000, 0x000000}},
    {"份", {0x000000, 0x000000, 0x000000, 0x000100, 0x018900, 0x010900, 0x011880, 0x021080, 0x022040, 0x072060, 0x064030, 0x0A97D0, 0x120840, 0x020840, 0x020840, 0x0208C0, 0x021080, 0x021080, 0x022080, 0x024380, 0x038100, 0x000000, 0x000000, 0x000000}},
    {"张", {0x000000, 0x000000, 0x000000, 0x000800, 0x0FC800, 0x008820, 0x008860, 0x008880, 0x0F8900, 0x088A00, 0x080C00, 0x087DF0, 0x0F8A00, 0x088900, 0x008900, 0x008900, 0x010880, 0x010940, 0x010A30, 0x070C10, 0x020800, 0x000000, 0x000000, 0x000000}},
    {"晓", {0x000000, 0x000000, 0x000000, 0x000800, 0x000800, 0x0F8820, 0x088FC0, 0x08F440, 0x0886C0, 0x088380, 0x0F8310, 0x088CD0, 0x08B030, 0x08F6E0, 0x088900, 0x088900, 0x0F9900, 0x089110, 0x083108, 0x0041F8, 0x018000, 0x000000, 0x000000, 0x000000}},
    {"敏", {0x000000, 0x000000, 0x000000, 0x020100, 0x020100, 0x07FD00, 0x040200, 0x0803D8, 0x16FA20, 0x051620, 0x049620, 0x049120, 0x0FFF20, 0x041140, 0x049140, 0x0890C0, 0x0FFCC0, 0x001140, 0x002220, 0x00E418, 0x004800, 0x000000, 0x000000, 0x000000}},
    {"王", {0x000000, 0x000000, 0x000000, 0x000000, 0x000020, 0x07EFF0, 0x001000, 0x001000, 0x001000, 0x001000, 0x001000, 0x001000, 0x03FFC0, 0x001000, 0x001000, 0x001000, 0x001000, 0x001000, 0x001010, 0x0FFFF8, 0x000000, 0x000000, 0x000000, 0x000000}},
    {"静", {0x000000, 0x000000, 0x000000, 0x010200, 0x010600, 0x0FF7C0, 0x010840, 0x07E880, 0x011FE0, 0x0FF120, 0x000120, 0x0FFFF8, 0x084120, 0x0FC120, 0x0847E0, 0x0FC120, 0x084100, 0x084100, 0x084100, 0x09CF00, 0x088200, 0x000000, 0x000000, 0x000000}},
    {"李", {0x000000, 0x000000, 0x000000, 0x001000, 0x001000, 0x0FDBF0, 0x003400, 0x005400, 0x00D200, 0x011180, 0x0210E0, 0x0CFD30, 0x100300, 0x000400, 0x000C00, 0x0FFFF0, 0x000800, 0x000800, 0x000800, 0x007800, 0x001000, 0x000000, 0x000000, 0x000000}},
    {"丽", {0x000000, 0x000000, 0x000000, 0x000010, 0x0FFFF8, 0x000000, 0x07E7F0, 0x042420, 0x042420, 0x042420, 0x052520, 0x052520, 0x05A4A0, 0x04A4A0, 0x04A4A0, 0x042420, 0x042420, 0x042420, 0x042420, 0x04E4E0, 0x084420, 0x000000, 0x000000, 0x000000}},
};

void draw_glyph24(ImageRgb& img, int x, int y, const std::string& ch, int scale = 2) {
    const Glyph24* found = nullptr;
    for (const auto& g : kTestGlyphs) {
        if (ch == g.utf8) {
            found = &g;
            break;
        }
    }
    if (!found) return;
    for (int gy = 0; gy < 24; ++gy) {
        uint32_t row = found->rows[gy];
        for (int gx = 0; gx < 24; ++gx) {
            if ((row >> (23 - gx)) & 1U) {
                for (int sy = 0; sy < scale; ++sy) {
                    int py = y + gy * scale + sy;
                    if (py < 0 || py >= img.height) continue;
                    for (int sx = 0; sx < scale; ++sx) {
                        int px = x + gx * scale + sx;
                        if (px < 0 || px >= img.width) continue;
                        uint8_t* p = &img.data[(static_cast<size_t>(py) * img.width + px) * 3];
                        p[0] = p[1] = p[2] = 20;
                    }
                }
            }
        }
    }
}

void draw_text24(ImageRgb& img, int x, int y, const std::vector<std::string>& chars, int scale = 2) {
    int cx = x;
    for (const auto& ch : chars) {
        draw_glyph24(img, cx, y, ch, scale);
        bool is_ascii = (ch.size() == 1);
        cx += (is_ascii ? 12 : 22) * scale;
    }
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
    CHECK(detect_table_corners(table, corners));
    CHECK(std::fabs(corners[0].x - 60.0f) < 12.0f);
    CHECK(std::fabs(corners[2].x - 820.0f) < 12.0f);

    RectifiedGrid grid;
    std::string err;
    CHECK(extract_rectified_table_grid(table, corners, 6, 4, grid, err));
    CHECK(grid.row_y_base.size() == ys.size());
    CHECK(grid.col_x_base.size() == xs.size());

    float ref = compute_table_ink_reference(grid, 1, 4, 1, 6);
    CellInkAnalysis ink1 = analyze_digit_cell(grid, 1, 1, ref);
    CHECK(ink1.category == InkCategory::ValidInk);
    CHECK(ink1.tall_thin_single_stroke);

    CellInkAnalysis empty_ink = analyze_digit_cell(grid, 1, 2, ref);
    CHECK(empty_ink.category == InkCategory::Empty);

    // Load real ncnn PP-OCRv5 models if present in NURSEOCR_SOURCE_DIR/models
    std::filesystem::path model_dir = std::filesystem::path(NURSEOCR_SOURCE_DIR) / "models";
    if (std::filesystem::exists(model_dir / "PP_OCRv5_mobile_rec.ncnn.bin")) {
        OcrEngine engine;
        CHECK(engine.init_from_dir(model_dir.u8string(), err));
        CHECK(engine.is_ready());
    }

    std::cout << "[PASS] test_synthetic_table_grid_and_ocr_load\n";
}

void test_ocr_digits_decimals_headers_and_banner() {
    std::filesystem::path model_dir = std::filesystem::path(NURSEOCR_SOURCE_DIR) / "models";
    if (!std::filesystem::exists(model_dir / "PP_OCRv5_mobile_rec.ncnn.bin")) {
        std::cout << "[SKIP] test_ocr_digits_decimals_headers_and_banner (no models dir)\n";
        return;
    }

    OcrEngine engine;
    std::string err;
    CHECK(engine.init_from_dir(model_dir.u8string(), err));

    // Build synthetic 5-row x 11-col (2 weeks) workload sheet with banner "2026年09月",
    // "惟名份" header row, tall vertical subheader row, and 3 staff rows containing 0..9, .5, 1.5, 12.
    const int W = 1500, H = 680;
    ImageRgb img;
    img.width = W;
    img.height = H;
    img.data.assign(static_cast<size_t>(W) * H * 3, 250);

    // Banner directly above top border (y=28..72)
    draw_text24(img, 540, 26, {"2", "0", "2", "6", "年", "0", "9", "月"}, 2);

    std::vector<int> xs = {60, 260, 370, 480, 590, 700, 810, 920, 1030, 1140, 1250, 1360};
    // Row 0 (header: 50px), Row 1 (tall subheader: 130px > 1.6*60), Rows 2..4 (staff rows: 60px each)
    std::vector<int> ys = {96, 146, 276, 336, 396, 456};
    auto draw_hline = [&](int y, int x0, int x1) {
        for (int yy = y - 1; yy <= y + 1; ++yy) {
            for (int x = x0; x <= x1; ++x) {
                uint8_t* p = &img.data[(static_cast<size_t>(yy) * W + x) * 3];
                p[0] = p[1] = p[2] = 25;
            }
        }
    };
    auto draw_vline = [&](int x, int y0, int y1) {
        for (int y = y0; y <= y1; ++y) {
            for (int xx = x - 1; xx <= x + 1; ++xx) {
                uint8_t* p = &img.data[(static_cast<size_t>(y) * W + xx) * 3];
                p[0] = p[1] = p[2] = 25;
            }
        }
    };
    for (int y : ys) draw_hline(y, xs.front(), xs.back());
    for (int x : xs) draw_vline(x, ys.front(), ys.back());

    // Header row 0: "惟名份" in col 0 and "第一周 11.2-11.8" in week 0 span
    draw_text24(img, 85, 106, {"惟", "名", "份"}, 1);
    draw_text24(img, 360, 106, {"第", "一", "周", "1", "1", ".", "2", "-", "1", "1", ".", "8"}, 1);

    // 3 real staff rows (rows 2, 3, 4)
    draw_text24(img, 85, 290, {"张", "晓", "敏"}, 2);
    draw_text24(img, 85, 350, {"王", "静"}, 2);
    draw_text24(img, 85, 410, {"李", "丽"}, 2);

    // Populate cells in row 2 (all enabled cols 0..3 and 5..8, skip col 4/9 which are disabled T):
    // W0: c0="0" c1="1" c2="8" c3="9" | W1: c5=".5" c6="1.5" c7="12" c8="4"
    // Row 3: W0: c0="2" c1="3" c2="5" c3="6" | W1: c5="7" c6="0.5"
    auto put_cell_text = [&](int row_idx, int col_idx, const std::vector<std::string>& chars) {
        int cx = (xs[col_idx] + xs[col_idx + 1]) / 2 - static_cast<int>(chars.size()) * 11;
        int cy = (ys[row_idx] + ys[row_idx + 1]) / 2 - 20;
        draw_text24(img, cx, cy, chars, 2);
    };
    put_cell_text(2, 1, {"0"});
    put_cell_text(2, 2, {"1"});
    put_cell_text(2, 3, {"8"});
    put_cell_text(2, 4, {"9"});
    put_cell_text(2, 6, {".", "5"});
    put_cell_text(2, 7, {"1", ".", "5"});
    put_cell_text(2, 8, {"1", "2"});
    put_cell_text(2, 9, {"4"});

    put_cell_text(3, 1, {"2"});
    put_cell_text(3, 2, {"3"});
    put_cell_text(3, 3, {"5"});
    put_cell_text(3, 4, {"6"});
    put_cell_text(3, 6, {"7"});
    put_cell_text(3, 7, {"0", ".", "5"});

    AppConfig cfg = create_default_config();
    WorkloadSheet sheet;
    CHECK(engine.process_workload_photo(img, 2024, 1, cfg, nullptr, sheet, err));

    // Verify banner auto-detected 2026-09 (overriding 2024-01) and week_labels updated
    CHECK(sheet.year == 2026);
    CHECK(sheet.month == 9);
    CHECK(sheet.week_labels == default_week_labels(2026, 9, sheet.weeks));

    // Verify "惟名份" and tall vertical subheader row did NOT leak into sheet.rows
    CHECK(sheet.rows.size() == 3);

    // Verify full 0-9, leading .5 -> 0.5, 1.5, 12, and value > 7.0 review flag (without clamping!)
    const auto& r0 = sheet.rows[0].cells;
    CHECK(r0[0].kind == ValueKind::Number && approx(r0[0].value, 0.0, 1e-4));
    CHECK(r0[1].kind == ValueKind::Number && approx(r0[1].value, 1.0, 1e-4));
    CHECK(r0[1].status == ReviewStatus::AutoConfirmed);
    CHECK(r0[2].kind == ValueKind::Number && approx(r0[2].value, 8.0, 1e-4));
    CHECK(r0[2].status == ReviewStatus::NeedsReview); // > 7.0 flagged while keeping 8.0
    CHECK(r0[3].kind == ValueKind::Number && approx(r0[3].value, 9.0, 1e-4));
    CHECK(r0[3].status == ReviewStatus::NeedsReview); // > 7.0 flagged while keeping 9.0
    CHECK(r0[5].kind == ValueKind::Number && approx(r0[5].value, 0.5, 1e-4)); // leading .5 -> 0.5
    CHECK(r0[5].status == ReviewStatus::AutoConfirmed);
    CHECK(r0[6].kind == ValueKind::Number && approx(r0[6].value, 1.5, 1e-4));
    CHECK(r0[6].status == ReviewStatus::AutoConfirmed);
    CHECK(r0[7].kind == ValueKind::Number && approx(r0[7].value, 12.0, 1e-4));
    CHECK(r0[7].status == ReviewStatus::NeedsReview); // > 7.0 flagged while keeping 12.0
    CHECK(r0[8].kind == ValueKind::Number && approx(r0[8].value, 4.0, 1e-4));

    const auto& r1 = sheet.rows[1].cells;
    CHECK(r1[0].kind == ValueKind::Number && approx(r1[0].value, 2.0, 1e-4));
    CHECK(r1[1].kind == ValueKind::Number && approx(r1[1].value, 3.0, 1e-4));
    CHECK(r1[2].kind == ValueKind::Number && approx(r1[2].value, 5.0, 1e-4));
    CHECK(r1[3].kind == ValueKind::Number && approx(r1[3].value, 6.0, 1e-4));
    CHECK(r1[5].kind == ValueKind::Number && approx(r1[5].value, 7.0, 1e-4));
    CHECK(r1[6].kind == ValueKind::Number && approx(r1[6].value, 0.5, 1e-4));

    // Verify Fallback #2: crop out top banner -> extracts month=11 from week header "11.2-11.8"
    ImageRgb cropped;
    int crop_y0 = 86;
    cropped.width = W;
    cropped.height = H - crop_y0;
    cropped.data.assign(
        img.data.begin() + static_cast<size_t>(crop_y0) * W * 3,
        img.data.end());
    WorkloadSheet sheet_fb;
    CHECK(engine.process_workload_photo(cropped, 2024, 1, cfg, nullptr, sheet_fb, err));
    CHECK(sheet_fb.year == 2024);
    CHECK(sheet_fb.month == 11);
    CHECK(sheet_fb.rows.size() == 3);

    // Regression test: curved open-top handwritten '4' whose descending right stem crosses
    // the bottom horizontal border must not silently misread as unflagged '6'.
    ImageRgb cross_img;
    cross_img.width = W;
    cross_img.height = H;
    cross_img.data.assign(static_cast<size_t>(W) * H * 3, 250);
    for (int y : ys) {
        for (int yy = y - 1; yy <= y + 1; ++yy) {
            for (int x = xs.front(); x <= xs.back(); ++x) {
                uint8_t* p = &cross_img.data[(static_cast<size_t>(yy) * W + x) * 3];
                p[0] = p[1] = p[2] = 25;
            }
        }
    }
    for (int x : xs) {
        for (int y = ys.front(); y <= ys.back(); ++y) {
            for (int xx = x - 1; xx <= x + 1; ++xx) {
                uint8_t* p = &cross_img.data[(static_cast<size_t>(y) * W + xx) * 3];
                p[0] = p[1] = p[2] = 25;
            }
        }
    }
    draw_text24(cross_img, 85, 290, {"张", "晓", "敏"}, 2);
    draw_text24(cross_img, 85, 350, {"王", "静"}, 2);
    {
        auto draw_thick_seg = [&](int px0, int py0, int px1, int py1) {
            int steps = std::max(std::abs(px1 - px0), std::abs(py1 - py0)) * 2 + 1;
            for (int s = 0; s <= steps; ++s) {
                float t = static_cast<float>(s) / steps;
                int cx = static_cast<int>(std::round(px0 + (px1 - px0) * t));
                int cy = static_cast<int>(std::round(py0 + (py1 - py0) * t));
                for (int dy = -2; dy <= 2; ++dy) {
                    for (int dx = -2; dx <= 2; ++dx) {
                        int px = cx + dx, py = cy + dy;
                        if (px >= 0 && px < W && py >= 0 && py < H) {
                            uint8_t* p = &cross_img.data[(static_cast<size_t>(py) * W + px) * 3];
                            p[0] = p[1] = p[2] = 28;
                        }
                    }
                }
            }
        };
        // Cell (row 2, col 1): xs=[260..370], ys=[276..336]
        // Curved open-top left U-stroke + cross-border vertical tail descending to y=341
        int mx = (xs[1] + xs[2]) / 2;
        draw_thick_seg(mx - 10, 290, mx - 13, 308);
        draw_thick_seg(mx - 13, 308, mx - 8, 323);
        draw_thick_seg(mx - 8, 323, mx + 12, 322);
        draw_thick_seg(mx + 6, 292, mx + 4, 341);
    }
    WorkloadSheet sheet_cross;
    CHECK(engine.process_workload_photo(cross_img, 2026, 8, cfg, nullptr, sheet_cross, err));
    CHECK(sheet_cross.rows.size() >= 1);
    const auto& c4 = sheet_cross.rows[0].cells[0];
    CHECK(c4.kind == ValueKind::Number);
    CHECK(approx(c4.value, 4.0, 1e-4) || c4.status == ReviewStatus::NeedsReview);
    CHECK(!(approx(c4.value, 6.0, 1e-4) && c4.status != ReviewStatus::NeedsReview));

    // Faint interior stroke inside cell (row 2, col 2) must trigger NeedsReview rather than silent AutoConfirmed Blank,
    // while neighboring empty cell (row 2, col 3) must remain AutoConfirmed Blank.
    {
        int faint_cx = (xs[2] + xs[3]) / 2;
        int faint_cy = (ys[2] + ys[3]) / 2;
        for (int dy = -9; dy <= 9; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                int px = faint_cx + dx;
                int py = faint_cy + dy;
                uint8_t* p = &cross_img.data[(static_cast<size_t>(py) * W + px) * 3];
                p[0] = p[1] = p[2] = 232; // diff = 18 on 250 paper
            }
        }
        WorkloadSheet sheet_faint;
        CHECK(engine.process_workload_photo(cross_img, 2026, 8, cfg, nullptr, sheet_faint, err));
        CHECK(sheet_faint.rows.size() >= 1);
        CHECK(sheet_faint.rows[0].cells[1].status == ReviewStatus::NeedsReview);
        CHECK(sheet_faint.rows[0].cells[2].kind == ValueKind::Blank);
        CHECK(sheet_faint.rows[0].cells[2].status == ReviewStatus::AutoConfirmed);
    }

    // High-resolution reduced-contrast table photo (3600x1632) must match canonical extraction without grid-line false positives
    {
        const int HW = 3600, HH = 1632;
        ImageRgb hi_img;
        hi_img.width = HW;
        hi_img.height = HH;
        hi_img.data.resize(static_cast<size_t>(HW) * HH * 3);
        for (int y = 0; y < HH; ++y) {
            float sy = (y + 0.5f) * static_cast<float>(H) / HH - 0.5f;
            int y0 = std::max(0, std::min(H - 1, static_cast<int>(std::floor(sy))));
            int y1 = std::min(H - 1, y0 + 1);
            float wy = std::max(0.0f, sy - y0);
            for (int x = 0; x < HW; ++x) {
                float sx = (x + 0.5f) * static_cast<float>(W) / HW - 0.5f;
                int x0 = std::max(0, std::min(W - 1, static_cast<int>(std::floor(sx))));
                int x1 = std::min(W - 1, x0 + 1);
                float wx = std::max(0.0f, sx - x0);
                for (int c = 0; c < 3; ++c) {
                    float p00 = img.data[(static_cast<size_t>(y0) * W + x0) * 3 + c];
                    float p10 = img.data[(static_cast<size_t>(y0) * W + x1) * 3 + c];
                    float p01 = img.data[(static_cast<size_t>(y1) * W + x0) * 3 + c];
                    float p11 = img.data[(static_cast<size_t>(y1) * W + x1) * 3 + c];
                    float v = (p00 * (1.0f - wx) + p10 * wx) * (1.0f - wy) +
                              (p01 * (1.0f - wx) + p11 * wx) * wy;
                    // Reduce contrast to simulate low-contrast mobile capture
                    int lc = static_cast<int>(std::round(42.0f + (v / 255.0f) * 185.0f));
                    hi_img.data[(static_cast<size_t>(y) * HW + x) * 3 + c] =
                        static_cast<uint8_t>(std::max(0, std::min(255, lc)));
                }
            }
        }
        WorkloadSheet sheet_hi;
        CHECK(engine.process_workload_photo(hi_img, 2024, 1, cfg, nullptr, sheet_hi, err));
        CHECK(sheet_hi.rows.size() == 3);
        const auto& hr0 = sheet_hi.rows[0].cells;
        CHECK(hr0[1].kind == ValueKind::Number && approx(hr0[1].value, 1.0, 1e-4));
        CHECK(hr0[8].kind == ValueKind::Number && approx(hr0[8].value, 4.0, 1e-4));
        const auto& hr1 = sheet_hi.rows[1].cells;
        CHECK(hr1[0].kind == ValueKind::Number && approx(hr1[0].value, 2.0, 1e-4));
        CHECK(hr1[1].kind == ValueKind::Number && approx(hr1[1].value, 3.0, 1e-4));
        CHECK(hr1[7].kind == ValueKind::Blank && hr1[7].status == ReviewStatus::AutoConfirmed);
        CHECK(hr1[8].kind == ValueKind::Blank && hr1[8].status == ReviewStatus::AutoConfirmed);
    }

    std::cout << "[PASS] test_ocr_digits_decimals_headers_and_banner\n";
}

} // namespace

int main() {
    test_default_config_is_blank_staff();
    test_calculator_switches_and_precision();
    test_name_doc_and_alignment();
    test_image_magic_and_xlsx_export();
    test_synthetic_table_grid_and_ocr_load();
    test_ocr_digits_decimals_headers_and_banner();
    std::cout << "ALL CORE TESTS PASSED\n";
    return 0;
}
