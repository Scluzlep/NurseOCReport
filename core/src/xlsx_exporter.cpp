#include "nurseocr/xlsx_exporter.h"
#include "nurseocr/calculator.h"

#include <miniz.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nurseocr {

namespace {

std::string col_letter(int zero_based_col) {
    std::string out;
    int c = zero_based_col + 1;
    while (c > 0) {
        int rem = (c - 1) % 26;
        out.insert(out.begin(), static_cast<char>('A' + rem));
        c = (c - 1) / 26;
    }
    return out;
}

std::string xml_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char ch : s) {
        switch (ch) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out.push_back(ch); break;
        }
    }
    return out;
}

std::string fmt_num_exact(double v) {
    if (std::fabs(v) < 1e-12) return "0";
    std::ostringstream oss;
    oss << std::setprecision(15) << v;
    return oss.str();
}

struct SharedStrings {
    std::vector<std::string> items;
    std::unordered_map<std::string, int> index;

    int add(const std::string& s) {
        auto it = index.find(s);
        if (it != index.end()) return it->second;
        int id = static_cast<int>(items.size());
        items.push_back(s);
        index[s] = id;
        return id;
    }

    std::string build_xml() const {
        std::ostringstream oss;
        oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n";
        oss << "<sst xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
            << "count=\"" << items.size() << "\" uniqueCount=\"" << items.size() << "\">";
        for (const auto& s : items) {
            oss << "<si><t xml:space=\"preserve\">" << xml_escape(s) << "</t></si>";
        }
        oss << "</sst>";
        return oss.str();
    }
};

// Style indices matching hospital template visual layout
enum StyleId {
    ST_DEFAULT = 0,
    ST_TITLE_DARK_BLUE = 1,   // #1F4E78 white bold 16pt
    ST_HEADER_LIGHT_BLUE = 2, // #D9EAF7 black bold centered wrap border
    ST_INPUT_INT = 3,         // #FFF2CC blue text border "0.00"
    ST_FORMULA_2DP = 4,       // #E2F0D9 black text border "0.00"
    ST_FORMULA_TRIM = 5,      // #E2F0D9 black text border "0.00"
    ST_TOTAL_INT = 6,         // #E7E6E6 bold border "0.00"
    ST_TOTAL_2DP = 7,         // #E7E6E6 bold border "0.00"
    ST_NAME_CELL = 8,         // white bold left/center border
    ST_SEQ_INT = 9,           // #FFFFFF centered border "0"
    ST_COEFF_NUM = 10,        // #FFF2CC blue text border "0.00"
    ST_WEIGHT_VAL = 11,       // #E4DFEC purple bold border "0.00"
    ST_NOTE_TITLE = 12,       // bold 13pt
    ST_NOTE_BODY = 13,        // 11pt wrap
    ST_SUM_TITLE = 14,        // #1F4E78 white bold 15pt
    ST_SUM_SUBTITLE = 15,     // #E9EDF4 dark blue 10pt
    ST_SUM_HEADER = 16,       // #D9EAF7 bold centered no-border
    ST_SUM_ZEBRA_A = 17,      // #FFFFFF center "0"
    ST_SUM_ZEBRA_B = 18,      // #F7FAFC center "0"
    ST_SUM_2DP_A = 19,        // #FFFFFF center "0.00"
    ST_SUM_2DP_B = 20,        // #F7FAFC center "0.00"
    ST_SUM_TRIM_A = 21,       // #FFFFFF center "0.00"
    ST_SUM_TRIM_B = 22,       // #F7FAFC center "0.00"
    ST_SUM_TOTAL_2DP = 23,    // #D9EAF7 bold center "0.00"
    ST_SUM_TOTAL_TRIM = 24,   // #D9EAF7 bold center "0.00"
    ST_SUM_TOTAL_LABEL = 25,  // #D9EAF7 bold center
};

std::string build_styles_xml() {
    std::ostringstream oss;
    oss << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<styleSheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">
  <numFmts count="3">
    <numFmt numFmtId="176" formatCode="0.00;\-0.00;\-"/>
    <numFmt numFmtId="177" formatCode="0.00"/>
    <numFmt numFmtId="178" formatCode="0"/>
  </numFmts>
  <fonts count="8">
    <font><sz val="11"/><color rgb="FF000000"/><name val="宋体"/></font>
    <font><b/><sz val="16"/><color rgb="FFFFFFFF"/><name val="宋体"/></font>
    <font><b/><sz val="11"/><color rgb="FF000000"/><name val="宋体"/></font>
    <font><sz val="11"/><color rgb="FF0000FF"/><name val="宋体"/></font>
    <font><b/><sz val="11"/><color rgb="FF7030A0"/><name val="宋体"/></font>
    <font><b/><sz val="13"/><color rgb="FF1F4E78"/><name val="宋体"/></font>
    <font><b/><sz val="15"/><color rgb="FFFFFFFF"/><name val="宋体"/></font>
    <font><sz val="10"/><color rgb="FF1F4E78"/><name val="宋体"/></font>
  </fonts>
  <fills count="8">
    <fill><patternFill patternType="none"/></fill>
    <fill><patternFill patternType="gray125"/></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FF1F4E78"/></patternFill></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FFD9EAF7"/></patternFill></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FFFFF2CC"/></patternFill></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FFE2F0D9"/></patternFill></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FFE7E6E6"/></patternFill></fill>
    <fill><patternFill patternType="solid"><fgColor rgb="FFE4DFEC"/></patternFill></fill>
  </fills>
  <borders count="2">
    <border><left/><right/><top/><bottom/><diagonal/></border>
    <border>
      <left style="thin"><color rgb="FFB7B7B7"/></left>
      <right style="thin"><color rgb="FFB7B7B7"/></right>
      <top style="thin"><color rgb="FFB7B7B7"/></top>
      <bottom style="thin"><color rgb="FFB7B7B7"/></bottom>
      <diagonal/>
    </border>
  </borders>
  <cellStyleXfs count="1">
    <xf numFmtId="0" fontId="0" fillId="0" borderId="0"/>
  </cellStyleXfs>
  <cellXfs count="26">
    <xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0"/>
    <xf numFmtId="0" fontId="1" fillId="2" borderId="1" xfId="0" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="0" fontId="2" fillId="3" borderId="1" xfId="0" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
    <xf numFmtId="177" fontId="3" fillId="4" borderId="1" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="176" fontId="0" fillId="5" borderId="1" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="177" fontId="0" fillId="5" borderId="1" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="177" fontId="2" fillId="6" borderId="1" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="176" fontId="2" fillId="6" borderId="1" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="0" fontId="2" fillId="0" borderId="1" xfId="0" applyFont="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="178" fontId="0" fillId="0" borderId="1" xfId="0" applyNumberFormat="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="176" fontId="3" fillId="4" borderId="1" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="176" fontId="4" fillId="7" borderId="1" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyBorder="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="0" fontId="5" fillId="0" borderId="0" xfId="0" applyFont="1" applyAlignment="1"><alignment vertical="center"/></xf>
    <xf numFmtId="0" fontId="0" fillId="0" borderId="0" xfId="0" applyAlignment="1"><alignment vertical="center" wrapText="1"/></xf>
    <xf numFmtId="0" fontId="6" fillId="2" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="0" fontId="7" fillId="3" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="0" fontId="2" fillId="3" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center" wrapText="1"/></xf>
    <xf numFmtId="178" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="178" fontId="0" fillId="3" borderId="0" xfId="0" applyNumberFormat="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="176" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="176" fontId="0" fillId="3" borderId="0" xfId="0" applyNumberFormat="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="177" fontId="0" fillId="0" borderId="0" xfId="0" applyNumberFormat="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="177" fontId="0" fillId="3" borderId="0" xfId="0" applyNumberFormat="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="176" fontId="2" fillId="3" borderId="0" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="177" fontId="2" fillId="3" borderId="0" xfId="0" applyNumberFormat="1" applyFont="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
    <xf numFmtId="0" fontId="2" fillId="3" borderId="0" xfId="0" applyFont="1" applyFill="1" applyAlignment="1"><alignment horizontal="center" vertical="center"/></xf>
  </cellXfs>
</styleSheet>)";
    return oss.str();
}

void write_str_cell(std::ostringstream& oss, int r, int c, int style, SharedStrings& ss, const std::string& val) {
    int id = ss.add(val);
    oss << "<c r=\"" << col_letter(c) << r << "\" s=\"" << style << "\" t=\"s\"><v>" << id << "</v></c>";
}

void write_empty_cell(std::ostringstream& oss, int r, int c, int style) {
    oss << "<c r=\"" << col_letter(c) << r << "\" s=\"" << style << "\"/>";
}

void write_num_cell(std::ostringstream& oss, int r, int c, int style, double val) {
    oss << "<c r=\"" << col_letter(c) << r << "\" s=\"" << style << "\"><v>" << fmt_num_exact(val) << "</v></c>";
}

void write_formula_cell(std::ostringstream& oss, int r, int c, int style, const std::string& formula, double cached) {
    oss << "<c r=\"" << col_letter(c) << r << "\" s=\"" << style << "\"><f>"
        << xml_escape(formula) << "</f><v>" << fmt_num_exact(cached) << "</v></c>";
}

std::string build_sheet1_workload(const WorkloadSheet& sheet,
                                  const AppConfig& config,
                                  const SheetSummary& summary,
                                  const ExportOptions& options,
                                  SharedStrings& ss) {
    const int n_people = static_cast<int>(sheet.rows.size());
    const int weeks = std::max(1, sheet.weeks);
    const int cols_per_week = std::max(1, static_cast<int>(sheet.column_shift_names.size()));
    const int n_rules = std::max(1, static_cast<int>(config.shift_rules.size()));

    // Columns: 0=Name | 1..weeks*cols_per_week = week inputs | sum_start.. = per-rule totals | weighted | coeff | final
    const int sum_start_col = 1 + weeks * cols_per_week;
    const int weighted_col = sum_start_col + n_rules;
    const int coeff_col = weighted_col + 1;
    const int final_col = coeff_col + 1;
    const int total_cols = final_col + 1;

    const int first_data_row = 4;
    const int total_row = first_data_row + n_people;
    const int last_data_row = std::max(first_data_row, total_row - 1);

    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        << "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" "
        << "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        << "<dimension ref=\"A1:" << col_letter(final_col) << total_row << "\"/>"
        << "<sheetViews><sheetView showGridLines=\"0\" workbookViewId=\"0\">"
        << "<pane xSplit=\"1\" ySplit=\"3\" topLeftCell=\"B4\" activePane=\"bottomRight\" state=\"frozen\"/>"
        << "</sheetView></sheetViews>"
        << "<sheetFormatPr defaultRowHeight=\"16.5\"/>"
        << "<cols>"
        << "<col min=\"1\" max=\"1\" width=\"12\" customWidth=\"1\"/>"
        << "<col min=\"2\" max=\"" << sum_start_col << "\" width=\"8.2\" customWidth=\"1\"/>"
        << "<col min=\"" << (sum_start_col + 1) << "\" max=\"" << total_cols << "\" width=\"13.95\" customWidth=\"1\"/>"
        << "</cols><sheetData>";

    std::ostringstream title;
    if (!options.department_title.empty()) title << options.department_title << " ";
    title << sheet.year << "年" << std::setw(2) << std::setfill('0') << sheet.month << "月护士工作量统计及加权算分";

    // Row 1: Title
    oss << "<row r=\"1\" ht=\"30\" customHeight=\"1\">";
    write_str_cell(oss, 1, 0, ST_TITLE_DARK_BLUE, ss, title.str());
    for (int c = 1; c < total_cols; ++c) write_empty_cell(oss, 1, c, ST_TITLE_DARK_BLUE);
    oss << "</row>";

    // Row 2: Group Headers
    oss << "<row r=\"2\" ht=\"26.1\" customHeight=\"1\">";
    write_str_cell(oss, 2, 0, ST_HEADER_LIGHT_BLUE, ss, "姓名");
    for (int w = 0; w < weeks; ++w) {
        std::string wlabel = (w < static_cast<int>(sheet.week_labels.size()))
            ? sheet.week_labels[w] : ("第" + std::to_string(w + 1) + "周");
        int base_c = 1 + w * cols_per_week;
        write_str_cell(oss, 2, base_c, ST_HEADER_LIGHT_BLUE, ss, wlabel);
        for (int k = 1; k < cols_per_week; ++k) write_empty_cell(oss, 2, base_c + k, ST_HEADER_LIGHT_BLUE);
    }
    for (int r_idx = 0; r_idx < n_rules; ++r_idx) {
        write_str_cell(oss, 2, sum_start_col + r_idx, ST_HEADER_LIGHT_BLUE, ss,
                       config.shift_rules[r_idx].name + "合计");
    }
    write_str_cell(oss, 2, weighted_col, ST_HEADER_LIGHT_BLUE, ss, "班次加权合计");
    write_str_cell(oss, 2, coeff_col, ST_HEADER_LIGHT_BLUE, ss, "总系数");
    write_str_cell(oss, 2, final_col, ST_HEADER_LIGHT_BLUE, ss, "最终总分");
    oss << "</row>";

    // Row 3: Subheaders
    oss << "<row r=\"3\" ht=\"39.95\" customHeight=\"1\">";
    write_empty_cell(oss, 3, 0, ST_HEADER_LIGHT_BLUE);
    for (int w = 0; w < weeks; ++w) {
        for (int sc = 0; sc < cols_per_week; ++sc) {
            write_str_cell(oss, 3, 1 + w * cols_per_week + sc, ST_HEADER_LIGHT_BLUE, ss,
                           sheet.column_shift_names[sc]);
        }
    }
    for (int c = sum_start_col; c < total_cols; ++c) write_empty_cell(oss, 3, c, ST_HEADER_LIGHT_BLUE);
    oss << "</row>";

    // Staff data rows
    const int staff_count = std::max(1, static_cast<int>(config.staff.size()));
    std::vector<double> col_totals(static_cast<size_t>(weeks * cols_per_week), 0.0);

    for (int ri = 0; ri < n_people; ++ri) {
        int erow = first_data_row + ri;
        const auto& wrow = sheet.rows[ri];
        const PersonScore* pscore = (ri < static_cast<int>(summary.people.size())) ? &summary.people[ri] : nullptr;

        oss << "<row r=\"" << erow << "\">";
        std::string nm = wrow.staff_name.empty() ? wrow.ocr_name : wrow.staff_name;
        write_str_cell(oss, erow, 0, ST_NAME_CELL, ss, nm);

        for (int ci = 0; ci < weeks * cols_per_week; ++ci) {
            int col = 1 + ci;
            if (ci < static_cast<int>(wrow.cells.size()) && wrow.cells[ci].kind == ValueKind::Number) {
                double v = wrow.cells[ci].value;
                col_totals[ci] += v;
                write_num_cell(oss, erow, col, ST_INPUT_INT, v);
            } else {
                write_empty_cell(oss, erow, col, ST_INPUT_INT);
            }
        }

        // Per-rule shift totals via discrete SUM over corresponding weekly columns
        for (int r_idx = 0; r_idx < n_rules; ++r_idx) {
            int col = sum_start_col + r_idx;
            std::vector<std::string> refs;
            for (int sc = 0; sc < cols_per_week; ++sc) {
                std::string code = sc < static_cast<int>(sheet.column_shift_codes.size())
                    ? sheet.column_shift_codes[sc] : "";
                if (find_shift_rule_index({config.shift_rules[r_idx]}, code, sheet.column_shift_names[sc]) == 0) {
                    for (int w = 0; w < weeks; ++w) {
                        refs.push_back(col_letter(1 + w * cols_per_week + sc) + std::to_string(erow));
                    }
                }
            }
            double cached = (pscore && r_idx < static_cast<int>(pscore->shift_totals.size()))
                ? pscore->shift_totals[r_idx] : 0.0;
            if (refs.empty()) {
                write_num_cell(oss, erow, col, ST_FORMULA_TRIM, cached);
            } else {
                std::ostringstream f;
                f << "SUM(";
                for (size_t k = 0; k < refs.size(); ++k) {
                    if (k > 0) f << ",";
                    f << refs[k];
                }
                f << ")";
                write_formula_cell(oss, erow, col, ST_FORMULA_TRIM, f.str(), cached);
            }
        }

        // Weighted sum formula referencing 班次权重!B{2+r_idx} only for enabled rules
        std::ostringstream wf;
        bool first_term = true;
        for (int r_idx = 0; r_idx < n_rules; ++r_idx) {
            if (!config.shift_rules[r_idx].enabled) continue;
            if (!first_term) wf << "+";
            wf << col_letter(sum_start_col + r_idx) << erow << "*'班次权重'!$B$" << (2 + r_idx);
            first_term = false;
        }
        if (first_term) wf << "0";
        double wsum = pscore ? pscore->weighted_sum : 0.0;
        write_formula_cell(oss, erow, weighted_col, ST_FORMULA_2DP, wf.str(), wsum);

        // Coeff lookup formula via INDEX/MATCH (defaults to 1 if person missing or both coeffs off)
        double eff_coeff = pscore ? pscore->effective_coeff : 1.0;
        std::ostringstream cf;
        cf << "IFERROR(INDEX('人员系数'!$E$2:$E$" << (1 + staff_count)
           << ",MATCH(A" << erow << ",'人员系数'!$B$2:$B$" << (1 + staff_count) << ",0)),1)";
        write_formula_cell(oss, erow, coeff_col, ST_FORMULA_2DP, cf.str(), eff_coeff);

        // Final score = weighted * coeff
        double fscore = pscore ? pscore->final_score : 0.0;
        std::string ff = col_letter(weighted_col) + std::to_string(erow) + "*" +
                         col_letter(coeff_col) + std::to_string(erow);
        write_formula_cell(oss, erow, final_col, ST_FORMULA_2DP, ff, fscore);

        oss << "</row>";
    }

    // Total Row
    oss << "<row r=\"" << total_row << "\">";
    write_str_cell(oss, total_row, 0, ST_TOTAL_INT, ss, "合计");
    if (n_people == 0) {
        for (int c = 1; c < total_cols; ++c) write_num_cell(oss, total_row, c, ST_TOTAL_2DP, 0.0);
        oss << "</row></sheetData>";
    } else {
        for (int ci = 0; ci < weeks * cols_per_week; ++ci) {
            int col = 1 + ci;
            std::string cl = col_letter(col);
            std::string f = "SUM(" + cl + std::to_string(first_data_row) + ":" + cl + std::to_string(last_data_row) + ")";
            write_formula_cell(oss, total_row, col, ST_TOTAL_INT, f, col_totals[ci]);
        }
        for (int r_idx = 0; r_idx < n_rules; ++r_idx) {
            int col = sum_start_col + r_idx;
            std::string cl = col_letter(col);
            std::string f = "SUM(" + cl + std::to_string(first_data_row) + ":" + cl + std::to_string(last_data_row) + ")";
            double gt = r_idx < static_cast<int>(summary.shift_grand_totals.size())
                ? summary.shift_grand_totals[r_idx] : 0.0;
            write_formula_cell(oss, total_row, col, ST_TOTAL_INT, f, gt);
        }
        {
            std::string cl = col_letter(weighted_col);
            std::string f = "SUM(" + cl + std::to_string(first_data_row) + ":" + cl + std::to_string(last_data_row) + ")";
            write_formula_cell(oss, total_row, weighted_col, ST_TOTAL_2DP, f, summary.weighted_grand_total);
        }
        write_formula_cell(oss, total_row, coeff_col, ST_TOTAL_2DP, "0", 0.0);
        {
            std::string cl = col_letter(final_col);
            std::string f = "SUM(" + cl + std::to_string(first_data_row) + ":" + cl + std::to_string(last_data_row) + ")";
            write_formula_cell(oss, total_row, final_col, ST_TOTAL_2DP, f, summary.final_grand_total);
        }
        oss << "</row></sheetData>";
    }

    // Merge cells
    int merge_cnt = 1 + 1 + weeks + (total_cols - sum_start_col);
    oss << "<mergeCells count=\"" << merge_cnt << "\">"
        << "<mergeCell ref=\"A1:" << col_letter(final_col) << "1\"/>"
        << "<mergeCell ref=\"A2:A3\"/>";
    for (int w = 0; w < weeks; ++w) {
        int c0 = 1 + w * cols_per_week;
        int c1 = c0 + cols_per_week - 1;
        oss << "<mergeCell ref=\"" << col_letter(c0) << "2:" << col_letter(c1) << "2\"/>";
    }
    for (int c = sum_start_col; c < total_cols; ++c) {
        oss << "<mergeCell ref=\"" << col_letter(c) << "2:" << col_letter(c) << "3\"/>";
    }
    oss << "</mergeCells>"
        << "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
        << "</worksheet>";
    return oss.str();
}

std::string build_sheet2_staff(const AppConfig& config, SharedStrings& ss) {
    const int n = static_cast<int>(config.staff.size());
    const int last_row = 1 + std::max(1, n);

    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        << "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        << "<dimension ref=\"A1:E" << last_row << "\"/>"
        << "<sheetViews><sheetView showGridLines=\"0\" workbookViewId=\"0\">"
        << "<pane ySplit=\"1\" topLeftCell=\"A2\" activePane=\"bottomLeft\" state=\"frozen\"/>"
        << "</sheetView></sheetViews>"
        << "<sheetFormatPr defaultRowHeight=\"16.5\"/>"
        << "<cols>"
        << "<col min=\"1\" max=\"1\" width=\"8.02\" customWidth=\"1\"/>"
        << "<col min=\"2\" max=\"2\" width=\"11.97\" customWidth=\"1\"/>"
        << "<col min=\"3\" max=\"5\" width=\"13.95\" customWidth=\"1\"/>"
        << "</cols><sheetData>";

    oss << "<row r=\"1\" ht=\"22\" customHeight=\"1\">";
    write_str_cell(oss, 1, 0, ST_HEADER_LIGHT_BLUE, ss, "序号");
    write_str_cell(oss, 1, 1, ST_HEADER_LIGHT_BLUE, ss, "姓名");
    write_str_cell(oss, 1, 2, ST_HEADER_LIGHT_BLUE, ss, "职称系数");
    write_str_cell(oss, 1, 3, ST_HEADER_LIGHT_BLUE, ss, "职务系数");
    write_str_cell(oss, 1, 4, ST_HEADER_LIGHT_BLUE, ss, "总系数");
    oss << "</row>";

    for (int i = 0; i < n; ++i) {
        int r = 2 + i;
        const auto& st = config.staff[i];
        double eff = compute_effective_coeff(st, config.coeff_flags);
        std::string f;
        if (!config.coeff_flags.enable_title_coeff && !config.coeff_flags.enable_duty_coeff) {
            f = "1";
        } else if (config.coeff_flags.enable_title_coeff && config.coeff_flags.enable_duty_coeff) {
            f = "C" + std::to_string(r) + "+D" + std::to_string(r);
        } else if (config.coeff_flags.enable_title_coeff) {
            f = "C" + std::to_string(r);
        } else {
            f = "D" + std::to_string(r);
        }

        oss << "<row r=\"" << r << "\">";
        write_num_cell(oss, r, 0, ST_SEQ_INT, i + 1);
        write_str_cell(oss, r, 1, ST_NAME_CELL, ss, st.name);
        write_num_cell(oss, r, 2, ST_COEFF_NUM, st.title_coeff);
        write_num_cell(oss, r, 3, ST_COEFF_NUM, st.duty_coeff);
        write_formula_cell(oss, r, 4, ST_FORMULA_2DP, f, eff);
        oss << "</row>";
    }

    oss << "</sheetData>"
        << "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
        << "</worksheet>";
    return oss.str();
}

std::string build_sheet3_weights(const AppConfig& config, SharedStrings& ss) {
    const int n = static_cast<int>(config.shift_rules.size());
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        << "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        << "<dimension ref=\"A1:C" << (1 + std::max(1, n)) << "\"/>"
        << "<sheetViews><sheetView showGridLines=\"0\" workbookViewId=\"0\"/></sheetViews>"
        << "<sheetFormatPr defaultRowHeight=\"16.5\"/>"
        << "<cols>"
        << "<col min=\"1\" max=\"1\" width=\"16\" customWidth=\"1\"/>"
        << "<col min=\"2\" max=\"2\" width=\"14\" customWidth=\"1\"/>"
        << "<col min=\"3\" max=\"3\" width=\"14\" customWidth=\"1\"/>"
        << "</cols><sheetData>";

    oss << "<row r=\"1\" ht=\"22\" customHeight=\"1\">";
    write_str_cell(oss, 1, 0, ST_HEADER_LIGHT_BLUE, ss, "班次名称");
    write_str_cell(oss, 1, 1, ST_HEADER_LIGHT_BLUE, ss, "权重");
    write_str_cell(oss, 1, 2, ST_HEADER_LIGHT_BLUE, ss, "状态");
    oss << "</row>";

    for (int i = 0; i < n; ++i) {
        int r = 2 + i;
        oss << "<row r=\"" << r << "\">";
        write_str_cell(oss, r, 0, ST_NAME_CELL, ss, config.shift_rules[i].name);
        write_num_cell(oss, r, 1, ST_WEIGHT_VAL, config.shift_rules[i].weight);
        write_str_cell(oss, r, 2, ST_NAME_CELL, ss, config.shift_rules[i].enabled ? "启用" : "停用");
        oss << "</row>";
    }

    oss << "</sheetData>"
        << "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
        << "</worksheet>";
    return oss.str();
}

std::string build_sheet4_notes(const WorkloadSheet& sheet, const AppConfig& config, SharedStrings& ss) {
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        << "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        << "<dimension ref=\"A1:A6\"/>"
        << "<sheetFormatPr defaultRowHeight=\"18\"/>"
        << "<cols><col min=\"1\" max=\"1\" width=\"100\" customWidth=\"1\"/></cols><sheetData>";

    std::vector<std::string> notes = {
        "工作量统计说明",
        "1. 统计周期：" + std::to_string(sheet.year) + "年" + std::to_string(sheet.month) + "月，共 " +
            std::to_string(sheet.weeks) + " 周，统计人数 " + std::to_string(sheet.rows.size()) + " 人。",
        "2. 算分公式：班次加权合计乘以总系数，全精度求和并保留两位小数展示。",
        std::string("3. 个人系数启用情况：职称系数") + (config.coeff_flags.enable_title_coeff ? "已启用" : "未启用") +
            "，职务系数" + (config.coeff_flags.enable_duty_coeff ? "已启用" : "未启用") + "。",
        "4. 黄色底纹单元格为识别录入值，绿色底纹单元格为自动关联公式。"
    };
    for (size_t i = 0; i < notes.size(); ++i) {
        int r = static_cast<int>(i + 1);
        oss << "<row r=\"" << r << "\" ht=\"22\" customHeight=\"1\">";
        write_str_cell(oss, r, 0, (i == 0) ? ST_NOTE_TITLE : ST_NOTE_BODY, ss, notes[i]);
        oss << "</row>";
    }
    oss << "</sheetData></worksheet>";
    return oss.str();
}

std::string build_sheet5_summary(const WorkloadSheet& sheet,
                                 const AppConfig& config,
                                 const SheetSummary& summary,
                                 const ExportOptions& options,
                                 SharedStrings& ss) {
    const int n = static_cast<int>(summary.people.size());
    const int n_rules = static_cast<int>(config.shift_rules.size());
    const int weeks = std::max(1, sheet.weeks);
    const int cols_per_week = std::max(1, static_cast<int>(sheet.column_shift_names.size()));
    const int wl_sum_start = 1 + weeks * cols_per_week;
    const int wl_weighted = wl_sum_start + n_rules;
    const int wl_final = wl_weighted + 2;
    const int wl_last_row = 3 + std::max(1, n);

    // Columns: 0=序号 | 1=姓名 | 2=职称 | 3=职务 | 4=总系数 | 5..4+n_rules=shift totals | 5+n_rules=加权合计 | 6+n_rules=最终总分
    const int shift_col0 = 5;
    const int weighted_col = shift_col0 + n_rules;
    const int final_col = weighted_col + 1;
    const int total_cols = final_col + 1;

    const int first_row = 4;
    const int total_row = first_row + n;
    const int last_row = std::max(first_row, total_row - 1);

    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        << "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\">"
        << "<dimension ref=\"A1:" << col_letter(final_col) << total_row << "\"/>"
        << "<sheetViews><sheetView workbookViewId=\"0\">"
        << "<pane ySplit=\"3\" topLeftCell=\"A4\" activePane=\"bottomLeft\" state=\"frozen\"/>"
        << "</sheetView></sheetViews>"
        << "<sheetFormatPr defaultRowHeight=\"22\"/>"
        << "<cols>"
        << "<col min=\"1\" max=\"1\" width=\"8\" customWidth=\"1\"/>"
        << "<col min=\"2\" max=\"2\" width=\"13\" customWidth=\"1\"/>"
        << "<col min=\"3\" max=\"5\" width=\"12\" customWidth=\"1\"/>"
        << "<col min=\"6\" max=\"" << (5 + n_rules) << "\" width=\"13\" customWidth=\"1\"/>"
        << "<col min=\"" << (6 + n_rules) << "\" max=\"" << total_cols << "\" width=\"17\" customWidth=\"1\"/>"
        << "</cols><sheetData>";

    std::ostringstream title;
    if (!options.department_title.empty()) title << options.department_title << " ";
    title << sheet.year << "年" << sheet.month << "月工作量统计汇总";

    oss << "<row r=\"1\" ht=\"36\" customHeight=\"1\">";
    write_str_cell(oss, 1, 0, ST_SUM_TITLE, ss, title.str());
    for (int c = 1; c < total_cols; ++c) write_empty_cell(oss, 1, c, ST_SUM_TITLE);
    oss << "</row>";

    std::ostringstream sub;
    sub << "已启用班次权重：";
    bool first_w = true;
    for (const auto& sr : config.shift_rules) {
        if (!sr.enabled) continue;
        if (!first_w) sub << "  ";
        sub << sr.name << " " << format_number_2dp(sr.weight);
        first_w = false;
    }
    oss << "<row r=\"2\" ht=\"27\" customHeight=\"1\">";
    write_str_cell(oss, 2, 0, ST_SUM_SUBTITLE, ss, sub.str());
    for (int c = 1; c < total_cols; ++c) write_empty_cell(oss, 2, c, ST_SUM_SUBTITLE);
    oss << "</row>";

    oss << "<row r=\"3\" ht=\"34\" customHeight=\"1\">";
    write_str_cell(oss, 3, 0, ST_SUM_HEADER, ss, "序号");
    write_str_cell(oss, 3, 1, ST_SUM_HEADER, ss, "姓名");
    write_str_cell(oss, 3, 2, ST_SUM_HEADER, ss, "职称系数");
    write_str_cell(oss, 3, 3, ST_SUM_HEADER, ss, "职务系数");
    write_str_cell(oss, 3, 4, ST_SUM_HEADER, ss, "总系数");
    for (int r_idx = 0; r_idx < n_rules; ++r_idx) {
        write_str_cell(oss, 3, shift_col0 + r_idx, ST_SUM_HEADER, ss, config.shift_rules[r_idx].name);
    }
    write_str_cell(oss, 3, weighted_col, ST_SUM_HEADER, ss, "班次加权合计");
    write_str_cell(oss, 3, final_col, ST_SUM_HEADER, ss, "最终总分");
    oss << "</row>";

    const int staff_count = std::max(1, static_cast<int>(config.staff.size()));
    for (int i = 0; i < n; ++i) {
        int r = first_row + i;
        bool alt = (i % 2 == 1);
        int st_int = alt ? ST_SUM_ZEBRA_B : ST_SUM_ZEBRA_A;
        int st_2dp = alt ? ST_SUM_2DP_B : ST_SUM_2DP_A;
        int st_trim = alt ? ST_SUM_TRIM_B : ST_SUM_TRIM_A;
        const auto& p = summary.people[i];

        oss << "<row r=\"" << r << "\" ht=\"25\" customHeight=\"1\">";
        write_num_cell(oss, r, 0, st_int, i + 1);
        write_str_cell(oss, r, 1, st_int, ss, p.name);

        auto coeff_lookup = [&](const std::string& col_l, double fallback) -> std::string {
            std::ostringstream cf;
            cf << "IFERROR(INDEX('人员系数'!$" << col_l << "$2:$" << col_l << "$" << (1 + staff_count)
               << ",MATCH(B" << r << ",'人员系数'!$B$2:$B$" << (1 + staff_count) << ",0)),"
               << fmt_num_exact(fallback) << ")";
            return cf.str();
        };
        write_formula_cell(oss, r, 2, st_2dp, coeff_lookup("C", p.title_coeff), p.title_coeff);
        write_formula_cell(oss, r, 3, st_2dp, coeff_lookup("D", p.duty_coeff), p.duty_coeff);
        write_formula_cell(oss, r, 4, st_2dp, coeff_lookup("E", p.effective_coeff), p.effective_coeff);

        auto wl_lookup = [&](int wl_col_idx) -> std::string {
            std::string cl = col_letter(wl_col_idx);
            std::ostringstream wl;
            wl << "IFERROR(INDEX('工作量计算'!$" << cl << "$4:$" << cl << "$" << wl_last_row
               << ",MATCH(B" << r << ",'工作量计算'!$A$4:$A$" << wl_last_row << ",0)),0)";
            return wl.str();
        };
        for (int r_idx = 0; r_idx < n_rules; ++r_idx) {
            double v = (r_idx < static_cast<int>(p.shift_totals.size())) ? p.shift_totals[r_idx] : 0.0;
            write_formula_cell(oss, r, shift_col0 + r_idx, st_trim, wl_lookup(wl_sum_start + r_idx), v);
        }
        write_formula_cell(oss, r, weighted_col, st_2dp, wl_lookup(wl_weighted), p.weighted_sum);
        write_formula_cell(oss, r, final_col, st_2dp, wl_lookup(wl_final), p.final_score);
        oss << "</row>";
    }

    oss << "<row r=\"" << total_row << "\" ht=\"28\" customHeight=\"1\">";
    write_str_cell(oss, total_row, 0, ST_SUM_TOTAL_LABEL, ss, "合计");
    for (int c = 1; c < 5; ++c) write_empty_cell(oss, total_row, c, ST_SUM_TOTAL_LABEL);
    if (n == 0) {
        for (int r_idx = 0; r_idx < n_rules; ++r_idx) {
            write_num_cell(oss, total_row, shift_col0 + r_idx, ST_SUM_TOTAL_TRIM, 0.0);
        }
        write_num_cell(oss, total_row, weighted_col, ST_SUM_TOTAL_2DP, 0.0);
        write_num_cell(oss, total_row, final_col, ST_SUM_TOTAL_2DP, 0.0);
    } else {
        for (int r_idx = 0; r_idx < n_rules; ++r_idx) {
            std::string cl = col_letter(shift_col0 + r_idx);
            double gt = r_idx < static_cast<int>(summary.shift_grand_totals.size())
                ? summary.shift_grand_totals[r_idx] : 0.0;
            write_formula_cell(oss, total_row, shift_col0 + r_idx, ST_SUM_TOTAL_TRIM,
                               "SUM(" + cl + "4:" + cl + std::to_string(last_row) + ")", gt);
        }
        {
            std::string cl = col_letter(weighted_col);
            write_formula_cell(oss, total_row, weighted_col, ST_SUM_TOTAL_2DP,
                               "SUM(" + cl + "4:" + cl + std::to_string(last_row) + ")",
                               summary.weighted_grand_total);
        }
        {
            std::string cl = col_letter(final_col);
            write_formula_cell(oss, total_row, final_col, ST_SUM_TOTAL_2DP,
                               "SUM(" + cl + "4:" + cl + std::to_string(last_row) + ")",
                               summary.final_grand_total);
        }
    }
    oss << "</row></sheetData>";

    std::string fc_l = col_letter(final_col);
    oss << "<mergeCells count=\"2\">"
        << "<mergeCell ref=\"A1:" << fc_l << "1\"/>"
        << "<mergeCell ref=\"A2:" << fc_l << "2\"/>"
        << "</mergeCells>";
    if (n > 0) {
        oss << "<conditionalFormatting sqref=\"" << fc_l << "4:" << fc_l << last_row << "\">"
            << "<cfRule type=\"dataBar\" priority=\"1\">"
            << "<dataBar><cfvo type=\"min\"/><cfvo type=\"max\"/><color rgb=\"FF5596CA\"/></dataBar>"
            << "</cfRule></conditionalFormatting>";
    }
    oss << "<pageMargins left=\"0.7\" right=\"0.7\" top=\"0.75\" bottom=\"0.75\" header=\"0.3\" footer=\"0.3\"/>"
        << "</worksheet>";
    return oss.str();
}

} // namespace

bool export_workload_xlsx(const std::string& out_path,
                          const WorkloadSheet& sheet,
                          const AppConfig& config,
                          const SheetSummary& summary,
                          const ExportOptions& options,
                          std::string& out_err) {
    SharedStrings ss;
    std::string s1 = build_sheet1_workload(sheet, config, summary, options, ss);
    std::string s2 = build_sheet2_staff(config, ss);
    std::string s3 = build_sheet3_weights(config, ss);
    std::string s4 = build_sheet4_notes(sheet, config, ss);
    std::string s5 = options.include_summary_sheet
        ? build_sheet5_summary(sheet, config, summary, options, ss) : "";

    std::string summary_tab = std::to_string(sheet.month) + "月汇总";

    std::ostringstream content_types;
    content_types << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
  <Override PartName="/xl/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.styles+xml"/>
  <Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>
  <Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/worksheets/sheet2.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/worksheets/sheet3.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>
  <Override PartName="/xl/worksheets/sheet4.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>)";
    if (options.include_summary_sheet) {
        content_types << "\n  <Override PartName=\"/xl/worksheets/sheet5.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>";
    }
    content_types << "\n</Types>";

    std::string root_rels = R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>)";

    std::ostringstream wb;
    wb << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <sheets>
    <sheet name="工作量计算" sheetId="1" r:id="rId1"/>
    <sheet name="人员系数" sheetId="2" r:id="rId2"/>
    <sheet name="班次权重" sheetId="3" r:id="rId3"/>
    <sheet name="说明" sheetId="4" r:id="rId4"/>)";
    if (options.include_summary_sheet) {
        wb << "\n    <sheet name=\"" << xml_escape(summary_tab) << "\" sheetId=\"5\" r:id=\"rId5\"/>";
    }
    wb << "\n  </sheets>\n  <calcPr calcId=\"191029\" fullCalcOnLoad=\"1\"/>\n</workbook>";

    std::ostringstream wb_rels;
    wb_rels << R"(<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet2.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet3.xml"/>
  <Relationship Id="rId4" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet4.xml"/>)";
    int next_rid = 5;
    if (options.include_summary_sheet) {
        wb_rels << "\n  <Relationship Id=\"rId" << next_rid++ << "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet5.xml\"/>";
    }
    wb_rels << "\n  <Relationship Id=\"rId" << next_rid++ << "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles\" Target=\"styles.xml\"/>"
            << "\n  <Relationship Id=\"rId" << next_rid++ << "\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings\" Target=\"sharedStrings.xml\"/>"
            << "\n</Relationships>";

    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 65536)) {
        out_err = "初始化 Excel 压缩器失败";
        return false;
    }

    auto add_entry = [&](const char* name, const std::string& content) -> bool {
        return mz_zip_writer_add_mem(&zip, name, content.data(), content.size(), MZ_DEFAULT_COMPRESSION) != 0;
    };

    bool ok = add_entry("[Content_Types].xml", content_types.str()) &&
              add_entry("_rels/.rels", root_rels) &&
              add_entry("xl/workbook.xml", wb.str()) &&
              add_entry("xl/_rels/workbook.xml.rels", wb_rels.str()) &&
              add_entry("xl/styles.xml", build_styles_xml()) &&
              add_entry("xl/sharedStrings.xml", ss.build_xml()) &&
              add_entry("xl/worksheets/sheet1.xml", s1) &&
              add_entry("xl/worksheets/sheet2.xml", s2) &&
              add_entry("xl/worksheets/sheet3.xml", s3) &&
              add_entry("xl/worksheets/sheet4.xml", s4) &&
              (!options.include_summary_sheet || add_entry("xl/worksheets/sheet5.xml", s5));

    void* zip_buf = nullptr;
    size_t zip_size = 0;
    if (!ok || !mz_zip_writer_finalize_heap_archive(&zip, &zip_buf, &zip_size)) {
        mz_zip_writer_end(&zip);
        out_err = "生成 Excel 数据失败";
        return false;
    }

#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, out_path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(1, wlen)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, out_path.c_str(), -1, &wpath[0], wlen);
    FILE* f = _wfopen(wpath.c_str(), L"wb");
#else
    FILE* f = std::fopen(out_path.c_str(), "wb");
#endif
    if (!f) {
        mz_free(zip_buf);
        mz_zip_writer_end(&zip);
        out_err = "无法创建输出 Excel 文件";
        return false;
    }
    size_t written = std::fwrite(zip_buf, 1, zip_size, f);
    std::fclose(f);
    mz_free(zip_buf);
    mz_zip_writer_end(&zip);

    if (written != zip_size) {
        out_err = "写入 Excel 文件不完整";
        return false;
    }
    return true;
}

} // namespace nurseocr
