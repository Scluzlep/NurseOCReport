#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include "editable_grid.h"
#include "photo_canvas.h"

#include "nurseocr/calculator.h"
#include "nurseocr/image_io.h"
#include "nurseocr/name_doc_reader.h"
#include "nurseocr/ocr_engine.h"
#include "nurseocr/storage.h"
#include "nurseocr/table_cv.h"
#include "nurseocr/xlsx_exporter.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace nurseocr;
using namespace nurseocr_win;

namespace {

enum ControlIds {
    ID_TAB = 100,
    // Tab 1: Weights
    ID_CHK_TITLE_COEFF = 201,
    ID_CHK_DUTY_COEFF = 202,
    ID_BTN_ADD_SHIFT = 203,
    ID_BTN_DEL_SHIFT = 204,
    ID_BTN_RESET_SHIFT = 205,
    ID_GRID_WEIGHTS = 206,

    // Tab 2: Staff
    ID_BTN_SCAN_COEFF = 301,
    ID_BTN_IMPORT_NAMES = 302,
    ID_BTN_ADD_STAFF = 303,
    ID_BTN_DEL_STAFF = 304,
    ID_BTN_CLEAR_STAFF = 305,
    ID_LBL_STAFF_HINT = 306,
    ID_GRID_STAFF = 307,

    // Tab 3: Workload
    ID_BTN_OPEN_WORKLOAD = 401,
    ID_BTN_ROT_LEFT = 402,
    ID_BTN_ROT_RIGHT = 403,
    ID_BTN_EDIT_CORNERS = 404,
    ID_BTN_NEXT_REVIEW = 405,
    ID_BTN_EXPORT_XLSX = 406,
    ID_EDIT_YEAR = 407,
    ID_LBL_YEAR = 408,
    ID_EDIT_MONTH = 409,
    ID_LBL_MONTH = 410,
    ID_LBL_STATUS = 411,
    ID_CANVAS_PHOTO = 412,
    ID_GRID_WORKLOAD = 413,
    ID_LBL_QUICK_INFO = 414,
    ID_BTN_Q_BLANK = 420,
    ID_BTN_Q_1 = 421,
    ID_BTN_Q_2 = 422,
    ID_BTN_Q_3 = 423,
    ID_BTN_Q_4 = 424,
    ID_BTN_Q_5 = 425,
    ID_BTN_Q_HALF = 426,
    ID_BTN_Q_CONFIRM = 427,
    ID_BTN_IMPORT_NAMES_WL = 428,
};

constexpr UINT WM_APP_COEFF_DONE = WM_APP + 101;
constexpr UINT WM_APP_WORKLOAD_DONE = WM_APP + 102;

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(0, len)), L'\0');
    if (len > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &out[0], len);
    return out;
}

std::string wide_to_utf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, len)), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), &out[0], len, nullptr, nullptr);
    return out;
}

std::string get_exe_dir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path p(buf);
    return wide_to_utf8(p.parent_path().wstring());
}

std::string resolve_models_dir() {
    std::filesystem::path exe_models = std::filesystem::path(utf8_to_wide(get_exe_dir())) / L"models";
    if (std::filesystem::exists(exe_models / L"PP_OCRv5_mobile_rec.ncnn.bin")) {
        return wide_to_utf8(exe_models.wstring());
    }
    std::filesystem::path cwd_models = std::filesystem::current_path() / L"models";
    if (std::filesystem::exists(cwd_models / L"PP_OCRv5_mobile_rec.ncnn.bin")) {
        return wide_to_utf8(cwd_models.wstring());
    }
    std::filesystem::path parent_models = std::filesystem::path(utf8_to_wide(get_exe_dir())).parent_path() / L"models";
    return wide_to_utf8(parent_models.wstring());
}

std::string resolve_config_path() {
    wchar_t appdata[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::filesystem::path p = std::filesystem::path(appdata) / L"NurseOCReport" / L"config.json";
        return wide_to_utf8(p.wstring());
    }
    return wide_to_utf8((std::filesystem::path(utf8_to_wide(get_exe_dir())) / L"config.json").wstring());
}

bool is_image_ext(const std::string& path) {
    std::string ext = std::filesystem::path(utf8_to_wide(path)).extension().u8string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".webp" || ext == ".bmp";
}

bool is_names_doc_ext(const std::string& path) {
    std::string ext = std::filesystem::path(utf8_to_wide(path)).extension().u8string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".txt" || ext == ".docx" || ext == ".csv" || ext == ".xlsx";
}

std::wstring open_file_dialog(HWND owner, const wchar_t* filter, const wchar_t* title) {
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameW(&ofn)) return file;
    return L"";
}

std::wstring save_xlsx_dialog(HWND owner, int year, int month) {
    wchar_t file[MAX_PATH]{};
    std::wostringstream def;
    def << L"工作量统计_" << year << L"年" << month << L"月.xlsx";
    std::wstring defs = def.str();
    std::wcsncpy(file, defs.c_str(), MAX_PATH - 1);

    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = L"Excel 表格 (*.xlsx)\0*.xlsx\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"xlsx";
    ofn.lpstrTitle = L"保存工作量表格";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn)) return file;
    return L"";
}

struct AsyncCoeffPayload {
    CoeffTableResult result;
};

struct AsyncWorkloadPayload {
    bool ok = false;
    std::string err;
    ImageRgb working_img;
    WorkloadSheet sheet;
};

struct MainApp {
    HINSTANCE inst = nullptr;
    HWND hwnd = nullptr;
    HFONT ui_font = nullptr;
    HFONT ui_bold_font = nullptr;
    HFONT title_font = nullptr;

    HWND tab_ctrl = nullptr;
    int active_tab = 2; // Start directly on Workload tab or Weights? Let's show Tab 0/2 cleanly; start on Tab 2 if configured, else Tab 2 with clear prompt

    // Tab 1 controls
    HWND chk_title_coeff = nullptr;
    HWND chk_duty_coeff = nullptr;
    HWND btn_add_shift = nullptr;
    HWND btn_del_shift = nullptr;
    HWND btn_reset_shift = nullptr;
    EditableGrid grid_weights;

    // Tab 2 controls
    HWND btn_scan_coeff = nullptr;
    HWND btn_import_names = nullptr;
    HWND btn_add_staff = nullptr;
    HWND btn_del_staff = nullptr;
    HWND btn_clear_staff = nullptr;
    HWND lbl_staff_hint = nullptr;
    EditableGrid grid_staff;

    // Tab 3 controls
    HWND btn_open_wl = nullptr;
    HWND btn_rot_left = nullptr;
    HWND btn_rot_right = nullptr;
    HWND btn_edit_corners = nullptr;
    HWND btn_next_review = nullptr;
    HWND btn_import_names_wl = nullptr;
    HWND btn_export_xlsx = nullptr;
    HWND edit_year = nullptr;
    HWND lbl_year = nullptr;
    HWND edit_month = nullptr;
    HWND lbl_month = nullptr;
    HWND lbl_status = nullptr;
    HWND lbl_quick_info = nullptr;
    HWND quick_btns[8]{};
    PhotoCanvas photo_canvas;
    EditableGrid grid_workload;

    // State
    OcrEngine ocr;
    std::string config_path;
    AppConfig config;
    ImageRgb working_img;
    ImageRgb selected_cell_preview;
    WorkloadSheet sheet;
    SheetSummary summary;
    bool busy = false;
    int sel_wl_row = -1;
    int sel_wl_cell_col = -2; // -1=name, >=0 index into row.cells

    void init_state() {
        config_path = resolve_config_path();
        std::string err;
        if (!load_app_config(config_path, config, err)) {
            config = create_default_config();
        }

        std::time_t now = std::time(nullptr);
        std::tm tm_now{};
#ifdef _WIN32
        localtime_s(&tm_now, &now);
#else
        localtime_r(&now, &tm_now);
#endif
        sheet.year = tm_now.tm_year + 1900;
        sheet.month = tm_now.tm_mon + 1;
        sheet.weeks = 5;
        sheet.column_shift_codes = {"A", "Z", "P", "Y", "T"};
        sheet.column_shift_names = {"A(主)", "治/白", "P", "夜", "技术难度"};

        std::string mdir = resolve_models_dir();
        if (!ocr.init_from_dir(mdir, err)) {
            // Delay showing warning until UI is up
        }
    }

    void save_config_quiet() {
        std::string err;
        save_app_config(config_path, config, err);
    }

    void refresh_weights_grid() {
        SendMessageW(chk_title_coeff, BM_SETCHECK, config.coeff_flags.enable_title_coeff ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(chk_duty_coeff, BM_SETCHECK, config.coeff_flags.enable_duty_coeff ? BST_CHECKED : BST_UNCHECKED, 0);

        std::vector<GridColumn> cols = {
            {L"启用", 75, GridCellKind::Checkbox, true, true},
            {L"班次名称", 220, GridCellKind::Text, true, true},
            {L"权重系数", 140, GridCellKind::Text, true, true},
        };
        grid_weights.set_columns(cols, false);

        std::vector<std::vector<GridCell>> rows;
        for (const auto& sr : config.shift_rules) {
            std::vector<GridCell> r(3);
            r[0] = {L"", GridCellKind::Checkbox, GridCellTone::Normal, sr.enabled, true};
            r[1] = {utf8_to_wide(sr.name), GridCellKind::Text, GridCellTone::Normal, false, true};
            r[2] = {utf8_to_wide(format_number_2dp(sr.weight)), GridCellKind::Text, GridCellTone::Normal, false, true};
            rows.push_back(std::move(r));
        }
        grid_weights.set_rows(std::move(rows), 0);
    }

    void refresh_staff_grid() {
        std::vector<GridColumn> cols = {
            {L"序号", 75, GridCellKind::ReadOnly, false, true},
            {L"姓名", 180, GridCellKind::Text, true, true},
            {L"职称系数", 140, GridCellKind::Text, true, true},
            {L"职务系数", 140, GridCellKind::Text, true, true},
            {L"总系数", 140, GridCellKind::ReadOnly, false, true},
        };
        grid_staff.set_columns(cols, false);
        grid_staff.set_name_drop_column(1);

        std::vector<std::vector<GridCell>> rows;
        for (size_t i = 0; i < config.staff.size(); ++i) {
            const auto& st = config.staff[i];
            double eff = compute_effective_coeff(st, config.coeff_flags);
            std::vector<GridCell> r(5);
            r[0] = {std::to_wstring(i + 1), GridCellKind::ReadOnly, GridCellTone::Normal, false, false};
            r[1] = {utf8_to_wide(st.name), GridCellKind::Text, GridCellTone::Normal, false, true};
            r[2] = {utf8_to_wide(format_number_2dp(st.title_coeff)), GridCellKind::Text, GridCellTone::Normal, false, true};
            r[3] = {utf8_to_wide(format_number_2dp(st.duty_coeff)), GridCellKind::Text, GridCellTone::Normal, false, true};
            r[4] = {utf8_to_wide(format_number_2dp(eff)), GridCellKind::ReadOnly, GridCellTone::FormulaTint, false, false};
            rows.push_back(std::move(r));
        }
        grid_staff.set_rows(std::move(rows), 0);
    }

    void recalc_and_refresh_workload() {
        summary = evaluate_workload(sheet, config);

        // Rebuild Workload grid columns dynamically
        int data_cols = sheet.weeks * static_cast<int>(sheet.column_shift_names.size());
        std::vector<GridColumn> cols;
        cols.push_back({L"姓名", 98, GridCellKind::Text, true, true});
        static const wchar_t* const kShortShift[] = {L"A", L"治", L"P", L"夜", L"难"};
        for (int w = 0; w < sheet.weeks; ++w) {
            for (size_t sc = 0; sc < sheet.column_shift_names.size(); ++sc) {
                std::wstring hdr = L"W" + std::to_wstring(w + 1) + L" ";
                hdr += (sc < 5) ? kShortShift[sc] : utf8_to_wide(sheet.column_shift_names[sc]);
                cols.push_back({hdr, 56, GridCellKind::Text, true, true});
            }
        }
        cols.push_back({L"加权合计", 90, GridCellKind::ReadOnly, false, true});
        cols.push_back({L"总系数", 78, GridCellKind::ReadOnly, false, true});
        cols.push_back({L"最终总分", 92, GridCellKind::ReadOnly, false, true});

        grid_workload.set_columns(cols, true);
        grid_workload.set_name_drop_column(0);

        std::vector<std::vector<GridCell>> rows;
        std::vector<HighlightBox> boxes;

        for (size_t ri = 0; ri < sheet.rows.size(); ++ri) {
            const auto& wrow = sheet.rows[ri];
            const PersonScore* ps = (ri < summary.people.size()) ? &summary.people[ri] : nullptr;

            std::vector<GridCell> gr(cols.size());
            std::wstring nm = utf8_to_wide(wrow.staff_name.empty() ? wrow.ocr_name : wrow.staff_name);
            GridCellTone ntone = wrow.name_needs_review ? GridCellTone::ReviewYellow : GridCellTone::Normal;
            gr[0] = {nm, GridCellKind::Text, ntone, false, true};
            boxes.push_back({static_cast<int>(ri), -1, wrow.name_quad, wrow.name_needs_review});

            for (int ci = 0; ci < data_cols; ++ci) {
                GridCell gc;
                gc.kind = GridCellKind::Text;
                gc.editable = true;
                if (ci < static_cast<int>(wrow.cells.size())) {
                    const auto& cell = wrow.cells[ci];
                    if (cell.kind == ValueKind::Number) {
                        gc.text = utf8_to_wide(format_number_trim(cell.value));
                    }
                    if (cell.status == ReviewStatus::NeedsReview) {
                        gc.tone = GridCellTone::ReviewYellow;
                    } else if (cell.status == ReviewStatus::UserConfirmed) {
                        gc.tone = GridCellTone::ConfirmedGreen;
                    }
                    boxes.push_back({static_cast<int>(ri), ci, cell.orig_quad,
                                     cell.status == ReviewStatus::NeedsReview});
                }
                gr[1 + ci] = std::move(gc);
            }

            gr[1 + data_cols] = {
                utf8_to_wide(format_number_2dp(ps ? ps->weighted_sum : 0.0)),
                GridCellKind::ReadOnly, GridCellTone::FormulaTint, false, false};
            gr[2 + data_cols] = {
                utf8_to_wide(format_number_2dp(ps ? ps->effective_coeff : 1.0)),
                GridCellKind::ReadOnly, GridCellTone::FormulaTint, false, false};
            gr[3 + data_cols] = {
                utf8_to_wide(format_number_2dp(ps ? ps->final_score : 0.0)),
                GridCellKind::ReadOnly, GridCellTone::FormulaTint, false, false};
            rows.push_back(std::move(gr));
        }

        if (!sheet.rows.empty()) {
            std::vector<GridCell> tot(cols.size());
            tot[0] = {L"合计", GridCellKind::ReadOnly, GridCellTone::TotalRow, false, false};
            for (int ci = 0; ci < data_cols; ++ci) {
                double sum_c = 0.0;
                for (const auto& wrow : sheet.rows) {
                    if (ci < static_cast<int>(wrow.cells.size()) && wrow.cells[ci].kind == ValueKind::Number) {
                        sum_c += wrow.cells[ci].value;
                    }
                }
                tot[1 + ci] = {utf8_to_wide(format_number_trim(sum_c)),
                               GridCellKind::ReadOnly, GridCellTone::TotalRow, false, false};
            }
            tot[1 + data_cols] = {utf8_to_wide(format_number_2dp(summary.weighted_grand_total)),
                                   GridCellKind::ReadOnly, GridCellTone::TotalRow, false, false};
            tot[2 + data_cols] = {L"-", GridCellKind::ReadOnly, GridCellTone::TotalRow, false, false};
            tot[3 + data_cols] = {utf8_to_wide(format_number_2dp(summary.final_grand_total)),
                                   GridCellKind::ReadOnly, GridCellTone::TotalRow, false, false};
            rows.push_back(std::move(tot));
        }

        grid_workload.set_rows(std::move(rows), sheet.rows.empty() ? 0 : 1);
        photo_canvas.set_overlays(std::move(boxes), sheet.geometry.corners);

        std::wostringstream st;
        if (busy) {
            st << L"正在识别中，请稍候...";
        } else if (sheet.rows.empty()) {
            st << L"尚未载入工作量照片";
        } else {
            st << L"共 " << sheet.rows.size() << L" 人  待确认 " << summary.pending_review_count
               << L" 处  全员总分 " << utf8_to_wide(format_number_2dp(summary.final_grand_total));
        }
        SetWindowTextW(lbl_status, st.str().c_str());
        update_quick_preview();
    }

    void update_quick_preview() {
        selected_cell_preview = ImageRgb{};
        std::wstring info = L"点击左图或右表单元格，在此核对或快捷填写";
        if (sel_wl_row >= 0 && sel_wl_row < static_cast<int>(sheet.rows.size())) {
            const auto& row = sheet.rows[sel_wl_row];
            std::wstring nm = utf8_to_wide(row.staff_name.empty() ? row.ocr_name : row.staff_name);
            if (sel_wl_cell_col == -1) {
                if (working_img.valid()) {
                    selected_cell_preview = crop_quad_image(working_img, row.name_quad, 160, 64);
                }
                info = nm + L" 姓名";
            } else if (sel_wl_cell_col >= 0 && sel_wl_cell_col < static_cast<int>(row.cells.size())) {
                const auto& cell = row.cells[sel_wl_cell_col];
                if (working_img.valid()) {
                    selected_cell_preview = crop_quad_image(working_img, cell.orig_quad, 108, 72);
                }
                std::wstring sname = (cell.shift_col < static_cast<int>(sheet.column_shift_names.size()))
                    ? utf8_to_wide(sheet.column_shift_names[cell.shift_col]) : L"";
                std::wostringstream oss;
                oss << nm << L"  第" << (cell.week + 1) << L"周 " << sname;
                if (!cell.review_reason.empty()) oss << L"  " << utf8_to_wide(cell.review_reason);
                info = oss.str();
            }
        }
        SetWindowTextW(lbl_quick_info, info.c_str());
        InvalidateRect(hwnd, nullptr, FALSE);
    }

    void jump_to_next_review() {
        if (sheet.rows.empty()) return;
        int n_rows = static_cast<int>(sheet.rows.size());
        int start_r = std::max(0, sel_wl_row);
        int total_checks = 0;
        for (const auto& r : sheet.rows) total_checks += 1 + static_cast<int>(r.cells.size());
        if (total_checks == 0) return;

        // Flatten index: (r, -1) then (r, 0..cells-1)
        auto step_next = [&](int& r, int& c) {
            if (c < 0) {
                if (!sheet.rows[r].cells.empty()) { c = 0; return; }
            } else if (c + 1 < static_cast<int>(sheet.rows[r].cells.size())) {
                ++c;
                return;
            }
            r = (r + 1) % n_rows;
            c = -1;
        };

        int cur_r = start_r;
        int cur_c = sel_wl_cell_col < -1 ? -1 : sel_wl_cell_col;
        for (int k = 0; k < total_checks; ++k) {
            step_next(cur_r, cur_c);
            bool need = false;
            if (cur_c == -1) need = sheet.rows[cur_r].name_needs_review;
            else need = (sheet.rows[cur_r].cells[cur_c].status == ReviewStatus::NeedsReview);
            if (need) {
                sel_wl_row = cur_r;
                sel_wl_cell_col = cur_c;
                grid_workload.select_cell(cur_r, cur_c == -1 ? 0 : (1 + cur_c), true);
                photo_canvas.select_highlight(cur_r, cur_c, true);
                update_quick_preview();
                return;
            }
        }
        MessageBoxW(hwnd, L"全部待确认项已处理完成，可以直接导出表格。", L"提示", MB_OK | MB_ICONINFORMATION);
    }

    void apply_quick_value(ValueKind kind, double val, bool jump_next) {
        if (sel_wl_row < 0 || sel_wl_row >= static_cast<int>(sheet.rows.size())) return;
        if (sel_wl_cell_col == -1) {
            sheet.rows[sel_wl_row].name_needs_review = false;
            recalc_and_refresh_workload();
            if (jump_next) jump_to_next_review();
            return;
        }
        if (sel_wl_cell_col < 0 || sel_wl_cell_col >= static_cast<int>(sheet.rows[sel_wl_row].cells.size())) {
            return;
        }
        auto& cell = sheet.rows[sel_wl_row].cells[sel_wl_cell_col];
        if (kind == ValueKind::Blank && val < 0.0) {
            // Confirm existing OCR value without changing number/blank
        } else {
            cell.kind = kind;
            cell.value = (kind == ValueKind::Number) ? val : 0.0;
        }
        cell.status = ReviewStatus::UserConfirmed;
        cell.review_reason.clear();
        cell.locked_by_user = true;
        recalc_and_refresh_workload();
        if (jump_next) jump_to_next_review();
    }

    void start_coeff_ocr_file(const std::string& path) {
        if (busy) return;
        if (!ocr.is_ready()) {
            MessageBoxW(hwnd, L"未找到 OCR 模型文件，请确认程序目录下的 models 文件夹完整。", L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
        busy = true;
        SetWindowTextW(lbl_staff_hint, L"正在识别系数表照片，请稍候...");
        AppConfig cfg_copy = config;
        std::thread([this, path, cfg_copy]() {
            auto* payload = new AsyncCoeffPayload();
            ImageRgb img;
            std::string err;
            if (!load_image_file(path, img, err)) {
                payload->result.ok = false;
                payload->result.error = err;
            } else {
                std::vector<std::string> dict;
                for (const auto& s : cfg_copy.staff) dict.push_back(s.name);
                ocr.process_coeff_photo(img, dict, payload->result);
            }
            PostMessageW(hwnd, WM_APP_COEFF_DONE, 0, reinterpret_cast<LPARAM>(payload));
        }).detach();
    }

    void start_workload_ocr_image(const ImageRgb& raw_img, bool auto_orient, const std::array<Point2f, 4>* manual_c) {
        if (busy) return;
        if (!ocr.is_ready()) {
            MessageBoxW(hwnd, L"未找到 OCR 模型文件，请确认程序目录下的 models 文件夹完整。", L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
        busy = true;
        wchar_t ybuf[16]{}, mbuf[16]{};
        GetWindowTextW(edit_year, ybuf, 16);
        GetWindowTextW(edit_month, mbuf, 16);
        int yr = std::max(2000, std::min(2099, _wtoi(ybuf)));
        int mo = std::max(1, std::min(12, _wtoi(mbuf)));

        SetWindowTextW(lbl_status, L"正在定位表格并识别，请稍候...");
        AppConfig cfg_copy = config;
        std::array<Point2f, 4> corners_copy{};
        bool has_manual = (manual_c != nullptr);
        if (has_manual) corners_copy = *manual_c;

        std::thread([this, raw_img, auto_orient, has_manual, corners_copy, yr, mo, cfg_copy]() {
            auto* payload = new AsyncWorkloadPayload();
            int rot = 0;
            payload->working_img = auto_orient ? ocr.auto_orient_table_photo(raw_img, rot) : raw_img;
            payload->ok = ocr.process_workload_photo(
                payload->working_img, yr, mo, cfg_copy,
                has_manual ? &corners_copy : nullptr,
                payload->sheet, payload->err);
            payload->sheet.applied_rotation_deg = rot;
            PostMessageW(hwnd, WM_APP_WORKLOAD_DONE, 0, reinterpret_cast<LPARAM>(payload));
        }).detach();
    }

    void start_workload_ocr_file(const std::string& path) {
        ImageRgb img;
        std::string err;
        if (!load_image_file(path, img, err)) {
            MessageBoxW(hwnd, utf8_to_wide(err).c_str(), L"打开照片失败", MB_OK | MB_ICONERROR);
            return;
        }
        start_workload_ocr_image(img, true, nullptr);
    }

    void import_names_into_staff(const std::string& path) {
        std::vector<std::string> names;
        std::string err;
        if (!read_names_from_file(path, names, err)) {
            MessageBoxW(hwnd, utf8_to_wide(err).c_str(), L"导入名单失败", MB_OK | MB_ICONERROR);
            return;
        }
        apply_name_list_to_staff(config.staff, names);
        save_config_quiet();
        refresh_staff_grid();
        if (!sheet.rows.empty()) {
            apply_name_list_to_workload(sheet, names);
            recalc_and_refresh_workload();
        }
    }

    void import_names_into_workload(const std::string& path) {
        std::vector<std::string> names;
        std::string err;
        if (!read_names_from_file(path, names, err)) {
            MessageBoxW(hwnd, utf8_to_wide(err).c_str(), L"导入名单失败", MB_OK | MB_ICONERROR);
            return;
        }
        if (sheet.rows.empty()) {
            import_names_into_staff(path);
            return;
        }
        apply_name_list_to_workload(sheet, names);
        recalc_and_refresh_workload();
    }

    void switch_tab(int idx) {
        active_tab = idx;
        TabCtrl_SetCurSel(tab_ctrl, idx);

        bool t0 = (idx == 0);
        bool t1 = (idx == 1);
        bool t2 = (idx == 2);

        auto sh = [](HWND h, bool v) { if (h) ShowWindow(h, v ? SW_SHOW : SW_HIDE); };
        sh(chk_title_coeff, t0);
        sh(chk_duty_coeff, t0);
        sh(btn_add_shift, t0);
        sh(btn_del_shift, t0);
        sh(btn_reset_shift, t0);
        grid_weights.show(t0);

        sh(btn_scan_coeff, t1);
        sh(btn_import_names, t1);
        sh(btn_add_staff, t1);
        sh(btn_del_staff, t1);
        sh(btn_clear_staff, t1);
        sh(lbl_staff_hint, t1);
        grid_staff.show(t1);

        sh(btn_open_wl, t2);
        sh(btn_rot_left, t2);
        sh(btn_rot_right, t2);
        sh(btn_edit_corners, t2);
        sh(btn_next_review, t2);
        sh(btn_import_names_wl, t2);
        sh(btn_export_xlsx, t2);
        sh(edit_year, t2);
        sh(lbl_year, t2);
        sh(edit_month, t2);
        sh(lbl_month, t2);
        sh(lbl_status, t2);
        sh(lbl_quick_info, t2);
        for (HWND b : quick_btns) sh(b, t2);
        photo_canvas.show(t2);
        grid_workload.show(t2);
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    void layout(int cw, int ch) {
        const int m = 10;
        MoveWindow(tab_ctrl, m, m, cw - 2 * m, 34, TRUE);
        int top = m + 42;
        int body_h = std::max(120, ch - top - m);

        // Tab 0: Weights
        MoveWindow(chk_title_coeff, m, top + 4, 140, 28, TRUE);
        MoveWindow(chk_duty_coeff, m + 150, top + 4, 140, 28, TRUE);
        MoveWindow(btn_add_shift, m + 310, top, 100, 32, TRUE);
        MoveWindow(btn_del_shift, m + 420, top, 100, 32, TRUE);
        MoveWindow(btn_reset_shift, m + 530, top, 100, 32, TRUE);
        grid_weights.move(m, top + 42, cw - 2 * m, body_h - 42);

        // Tab 1: Staff
        MoveWindow(btn_scan_coeff, m, top, 130, 32, TRUE);
        MoveWindow(btn_import_names, m + 140, top, 110, 32, TRUE);
        MoveWindow(btn_add_staff, m + 260, top, 100, 32, TRUE);
        MoveWindow(btn_del_staff, m + 370, top, 100, 32, TRUE);
        MoveWindow(btn_clear_staff, m + 480, top, 80, 32, TRUE);
        MoveWindow(lbl_staff_hint, m + 575, top + 6, std::max(100, cw - m - (m + 575)), 24, TRUE);
        grid_staff.move(m, top + 42, cw - 2 * m, body_h - 42);

        // Tab 2: Workload
        int x = m;
        auto put = [&](HWND h, int w, int hgt = 32, int dy = 0) {
            MoveWindow(h, x, top + dy, w, hgt, TRUE);
            x += w + 8;
        };
        put(btn_open_wl, 100);
        put(btn_rot_left, 76);
        put(btn_rot_right, 76);
        put(btn_edit_corners, 96);
        put(btn_next_review, 126);
        put(btn_import_names_wl, 100);
        put(edit_year, 54, 28, 2);
        put(lbl_year, 22, 24, 5);
        put(edit_month, 38, 28, 2);
        put(lbl_month, 22, 24, 5);
        put(btn_export_xlsx, 104);
        MoveWindow(lbl_status, x + 4, top + 6, std::max(80, cw - m - x - 4), 24, TRUE);

        int split_y = top + 40;
        int split_h = std::max(120, ch - split_y - m);
        int left_w = std::max(320, static_cast<int>((cw - 3 * m) * 0.45));
        int right_x = m + left_w + m;
        int right_w = std::max(300, cw - right_x - m);

        int quick_h = 112;
        int canvas_h = std::max(100, split_h - quick_h - 8);
        photo_canvas.move(m, split_y, left_w, canvas_h);

        int qy = split_y + canvas_h + 8;
        int preview_w = 128;
        MoveWindow(lbl_quick_info, m + preview_w + 10, qy + 2, std::max(100, left_w - preview_w - 10), 26, TRUE);
        int bx = m + preview_w + 10;
        int by = qy + 34;
        int avail_bw = std::max(200, left_w - preview_w - 10);
        int bw = std::max(36, (avail_bw - 7 * 6) / 8);
        for (int i = 0; i < 8; ++i) {
            MoveWindow(quick_btns[i], bx + i * (bw + 6), by, bw, 36, TRUE);
        }

        grid_workload.move(right_x, split_y, right_w, split_h);
    }

    void paint_preview(HDC hdc) {
        if (active_tab != 2) return;
        RECT rc{};
        GetClientRect(hwnd, &rc);
        int top = 52 + 40;
        int split_h = std::max(120, static_cast<int>(rc.bottom) - top - 10);
        int quick_h = 112;
        int canvas_h = std::max(100, split_h - quick_h - 8);
        int py = top + canvas_h + 8;
        RECT box{10, py, 10 + 126, py + 76};
        HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
        FillRect(hdc, &box, bg);
        DeleteObject(bg);
        FrameRect(hdc, &box, reinterpret_cast<HBRUSH>(GetStockObject(GRAY_BRUSH)));

        if (selected_cell_preview.valid()) {
            int iw = selected_cell_preview.width;
            int ih = selected_cell_preview.height;
            int stride = ((iw * 3 + 3) / 4) * 4;
            std::vector<uint8_t> bgr(static_cast<size_t>(stride) * ih, 255);
            for (int y = 0; y < ih; ++y) {
                const uint8_t* sp = &selected_cell_preview.data[static_cast<size_t>(y) * iw * 3];
                uint8_t* dp = &bgr[static_cast<size_t>(y) * stride];
                for (int x = 0; x < iw; ++x) {
                    dp[x * 3 + 0] = sp[x * 3 + 2];
                    dp[x * 3 + 1] = sp[x * 3 + 1];
                    dp[x * 3 + 2] = sp[x * 3 + 0];
                }
            }
            BITMAPINFO bmi{};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = iw;
            bmi.bmiHeader.biHeight = -ih;
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 24;
            bmi.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(hdc, HALFTONE);
            StretchDIBits(hdc, box.left + 2, box.top + 2, 122, 72,
                          0, 0, iw, ih, bgr.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
        }
    }
};

LRESULT CALLBACK main_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    MainApp* app = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = reinterpret_cast<MainApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        app->hwnd = hwnd;
    } else {
        app = reinterpret_cast<MainApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!app) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE: {
        app->ui_font = CreateFontW(17, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        app->ui_bold_font = CreateFontW(17, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        auto mk_ctrl = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int id) -> HWND {
            HWND h = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                                     0, 0, 10, 10, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app->inst, nullptr);
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(app->ui_font), TRUE);
            return h;
        };

        app->tab_ctrl = mk_ctrl(WC_TABCONTROLW, L"", TCS_TABS, ID_TAB);
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<wchar_t*>(L"1. 班次权重");
        TabCtrl_InsertItem(app->tab_ctrl, 0, &ti);
        ti.pszText = const_cast<wchar_t*>(L"2. 人员系数");
        TabCtrl_InsertItem(app->tab_ctrl, 1, &ti);
        ti.pszText = const_cast<wchar_t*>(L"3. 工作量识别");
        TabCtrl_InsertItem(app->tab_ctrl, 2, &ti);

        // Tab 0
        app->chk_title_coeff = mk_ctrl(L"BUTTON", L"启用职称系数", BS_AUTOCHECKBOX, ID_CHK_TITLE_COEFF);
        app->chk_duty_coeff = mk_ctrl(L"BUTTON", L"启用职务系数", BS_AUTOCHECKBOX, ID_CHK_DUTY_COEFF);
        app->btn_add_shift = mk_ctrl(L"BUTTON", L"新增班次", BS_PUSHBUTTON, ID_BTN_ADD_SHIFT);
        app->btn_del_shift = mk_ctrl(L"BUTTON", L"删除选中", BS_PUSHBUTTON, ID_BTN_DEL_SHIFT);
        app->btn_reset_shift = mk_ctrl(L"BUTTON", L"恢复默认", BS_PUSHBUTTON, ID_BTN_RESET_SHIFT);
        app->grid_weights.create(hwnd, app->inst, ID_GRID_WEIGHTS, 0, 0, 100, 100);
        app->grid_weights.set_font(app->ui_font, app->ui_bold_font);

        app->grid_weights.on_checkbox_toggled = [app](int r, int c, bool checked) {
            if (c == 0 && r >= 0 && r < static_cast<int>(app->config.shift_rules.size())) {
                app->config.shift_rules[r].enabled = checked;
                app->save_config_quiet();
                app->recalc_and_refresh_workload();
            }
        };
        app->grid_weights.on_cell_edited = [app](int r, int c, const std::wstring& txt) {
            if (r < 0 || r >= static_cast<int>(app->config.shift_rules.size())) return;
            if (c == 1) {
                std::string nm = wide_to_utf8(txt);
                if (!nm.empty()) app->config.shift_rules[r].name = nm;
            } else if (c == 2) {
                double v = std::wcstod(txt.c_str(), nullptr);
                if (std::isfinite(v) && v >= 0.0) app->config.shift_rules[r].weight = v;
            }
            app->save_config_quiet();
            app->refresh_weights_grid();
            app->recalc_and_refresh_workload();
        };

        // Tab 1
        app->btn_scan_coeff = mk_ctrl(L"BUTTON", L"读取系数照片", BS_PUSHBUTTON, ID_BTN_SCAN_COEFF);
        app->btn_import_names = mk_ctrl(L"BUTTON", L"导入名单", BS_PUSHBUTTON, ID_BTN_IMPORT_NAMES);
        app->btn_add_staff = mk_ctrl(L"BUTTON", L"新增人员", BS_PUSHBUTTON, ID_BTN_ADD_STAFF);
        app->btn_del_staff = mk_ctrl(L"BUTTON", L"删除选中", BS_PUSHBUTTON, ID_BTN_DEL_STAFF);
        app->btn_clear_staff = mk_ctrl(L"BUTTON", L"清空", BS_PUSHBUTTON, ID_BTN_CLEAR_STAFF);
        app->lbl_staff_hint = mk_ctrl(L"STATIC", L"可将名字文件直接拖入姓名列，或拖入系数表照片自动识别", SS_LEFT, ID_LBL_STAFF_HINT);
        app->grid_staff.create(hwnd, app->inst, ID_GRID_STAFF, 0, 0, 100, 100);
        app->grid_staff.set_font(app->ui_font, app->ui_bold_font);

        app->grid_staff.on_cell_edited = [app](int r, int c, const std::wstring& txt) {
            if (r < 0 || r >= static_cast<int>(app->config.staff.size())) return;
            if (c == 1) {
                app->config.staff[r].name = normalize_person_name(wide_to_utf8(txt));
                if (app->config.staff[r].name.empty()) app->config.staff[r].name = wide_to_utf8(txt);
            } else if (c == 2) {
                app->config.staff[r].title_coeff = std::wcstod(txt.c_str(), nullptr);
            } else if (c == 3) {
                app->config.staff[r].duty_coeff = std::wcstod(txt.c_str(), nullptr);
            }
            app->save_config_quiet();
            app->refresh_staff_grid();
            app->recalc_and_refresh_workload();
        };
        app->grid_staff.on_files_dropped = [app](int col, const std::vector<std::string>& paths) {
            (void)col;
            for (const auto& p : paths) {
                if (is_names_doc_ext(p)) app->import_names_into_staff(p);
                else if (is_image_ext(p)) app->start_coeff_ocr_file(p);
            }
        };
        app->grid_staff.on_paste_text = [app](int, int, const std::wstring& pasted) {
            auto names = parse_names_from_plain_text(wide_to_utf8(pasted));
            if (!names.empty()) {
                apply_name_list_to_staff(app->config.staff, names);
                app->save_config_quiet();
                app->refresh_staff_grid();
                app->recalc_and_refresh_workload();
            }
        };

        // Tab 2
        app->btn_open_wl = mk_ctrl(L"BUTTON", L"选择照片", BS_PUSHBUTTON, ID_BTN_OPEN_WORKLOAD);
        app->btn_rot_left = mk_ctrl(L"BUTTON", L"向左转", BS_PUSHBUTTON, ID_BTN_ROT_LEFT);
        app->btn_rot_right = mk_ctrl(L"BUTTON", L"向右转", BS_PUSHBUTTON, ID_BTN_ROT_RIGHT);
        app->btn_edit_corners = mk_ctrl(L"BUTTON", L"调整四角", BS_PUSHBUTTON, ID_BTN_EDIT_CORNERS);
        app->btn_next_review = mk_ctrl(L"BUTTON", L"下一处待确认", BS_PUSHBUTTON, ID_BTN_NEXT_REVIEW);
        app->btn_import_names_wl = mk_ctrl(L"BUTTON", L"导入名单", BS_PUSHBUTTON, ID_BTN_IMPORT_NAMES_WL);
        app->edit_year = mk_ctrl(L"EDIT", std::to_wstring(app->sheet.year).c_str(), WS_BORDER | ES_NUMBER | ES_CENTER, ID_EDIT_YEAR);
        app->lbl_year = mk_ctrl(L"STATIC", L"年", SS_CENTER, ID_LBL_YEAR);
        app->edit_month = mk_ctrl(L"EDIT", std::to_wstring(app->sheet.month).c_str(), WS_BORDER | ES_NUMBER | ES_CENTER, ID_EDIT_MONTH);
        app->lbl_month = mk_ctrl(L"STATIC", L"月", SS_CENTER, ID_LBL_MONTH);
        app->btn_export_xlsx = mk_ctrl(L"BUTTON", L"导出表格", BS_PUSHBUTTON, ID_BTN_EXPORT_XLSX);
        app->lbl_status = mk_ctrl(L"STATIC", L"尚未载入工作量照片", SS_LEFT, ID_LBL_STATUS);
        app->lbl_quick_info = mk_ctrl(L"STATIC", L"点击左图或右表单元格，在此快捷核对", SS_LEFT, ID_LBL_QUICK_INFO);

        const wchar_t* qlabels[8] = {L"空白", L"1", L"2", L"3", L"4", L"5", L"0.5", L"确认"};
        for (int i = 0; i < 8; ++i) {
            app->quick_btns[i] = mk_ctrl(L"BUTTON", qlabels[i], BS_PUSHBUTTON, ID_BTN_Q_BLANK + i);
        }

        app->photo_canvas.create(hwnd, app->inst, ID_CANVAS_PHOTO, 0, 0, 100, 100);
        app->grid_workload.create(hwnd, app->inst, ID_GRID_WORKLOAD, 0, 0, 100, 100);
        app->grid_workload.set_font(app->ui_font, app->ui_bold_font);

        app->photo_canvas.on_cell_clicked = [app](int r, int col_idx) {
            app->sel_wl_row = r;
            app->sel_wl_cell_col = col_idx;
            app->grid_workload.select_cell(r, col_idx < 0 ? 0 : (1 + col_idx), true);
            app->update_quick_preview();
        };
        app->photo_canvas.on_files_dropped = [app](const std::vector<std::string>& paths) {
            for (const auto& p : paths) {
                if (is_image_ext(p)) {
                    app->start_workload_ocr_file(p);
                    break;
                }
            }
        };
        app->grid_workload.on_selection_changed = [app](int r, int c) {
            if (r < 0 || r >= static_cast<int>(app->sheet.rows.size())) return;
            int data_cols = app->sheet.weeks * static_cast<int>(app->sheet.column_shift_names.size());
            if (c == 0) {
                app->sel_wl_row = r;
                app->sel_wl_cell_col = -1;
            } else if (c >= 1 && c <= data_cols) {
                app->sel_wl_row = r;
                app->sel_wl_cell_col = c - 1;
            } else {
                return;
            }
            app->photo_canvas.select_highlight(app->sel_wl_row, app->sel_wl_cell_col, true);
            app->update_quick_preview();
        };
        app->grid_workload.on_cell_edited = [app](int r, int c, const std::wstring& txt) {
            if (r < 0 || r >= static_cast<int>(app->sheet.rows.size())) return;
            if (c == 0) {
                std::string nm = normalize_person_name(wide_to_utf8(txt));
                app->sheet.rows[r].staff_name = nm.empty() ? wide_to_utf8(txt) : nm;
                app->sheet.rows[r].name_needs_review = false;
                app->recalc_and_refresh_workload();
                return;
            }
            int cell_idx = c - 1;
            if (cell_idx < 0 || cell_idx >= static_cast<int>(app->sheet.rows[r].cells.size())) return;
            auto& cell = app->sheet.rows[r].cells[cell_idx];
            std::string trimmed = wide_to_utf8(txt);
            size_t b = trimmed.find_first_not_of(" \t\r\n");
            if (b == std::string::npos) {
                cell.kind = ValueKind::Blank;
                cell.value = 0.0;
            } else {
                double v = std::wcstod(txt.c_str(), nullptr);
                if (std::isfinite(v) && v >= 0.0) {
                    cell.kind = ValueKind::Number;
                    cell.value = v;
                }
            }
            cell.status = ReviewStatus::UserConfirmed;
            cell.review_reason.clear();
            cell.locked_by_user = true;
            app->recalc_and_refresh_workload();
        };
        app->grid_workload.on_files_dropped = [app](int col, const std::vector<std::string>& paths) {
            for (const auto& p : paths) {
                if (is_names_doc_ext(p)) {
                    (void)col;
                    app->import_names_into_workload(p);
                } else if (is_image_ext(p)) {
                    app->start_workload_ocr_file(p);
                    break;
                }
            }
        };

        app->refresh_weights_grid();
        app->refresh_staff_grid();
        app->recalc_and_refresh_workload();
        app->switch_tab(2);

        DragAcceptFiles(hwnd, TRUE);
        if (!app->ocr.is_ready()) {
            MessageBoxW(hwnd, L"未找到 models 文件夹内的 OCR 模型文件，识别功能暂不可用。", L"提示", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    case WM_SIZE:
        app->layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        app->paint_preview(hdc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lp);
        if (hdr->idFrom == ID_TAB && hdr->code == TCN_SELCHANGE) {
            app->switch_tab(TabCtrl_GetCurSel(app->tab_ctrl));
        }
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case ID_CHK_TITLE_COEFF:
        case ID_CHK_DUTY_COEFF:
            app->config.coeff_flags.enable_title_coeff = (SendMessageW(app->chk_title_coeff, BM_GETCHECK, 0, 0) == BST_CHECKED);
            app->config.coeff_flags.enable_duty_coeff = (SendMessageW(app->chk_duty_coeff, BM_GETCHECK, 0, 0) == BST_CHECKED);
            app->save_config_quiet();
            app->refresh_staff_grid();
            app->recalc_and_refresh_workload();
            break;
        case ID_BTN_ADD_SHIFT: {
            std::string code = "S" + std::to_string(app->config.shift_rules.size() + 1);
            app->config.shift_rules.push_back({code, "新班次", 1.00, true});
            app->save_config_quiet();
            app->refresh_weights_grid();
            app->recalc_and_refresh_workload();
            break;
        }
        case ID_BTN_DEL_SHIFT: {
            int r = app->grid_weights.selected_row();
            if (r >= 0 && r < static_cast<int>(app->config.shift_rules.size())) {
                app->config.shift_rules.erase(app->config.shift_rules.begin() + r);
                app->save_config_quiet();
                app->refresh_weights_grid();
                app->recalc_and_refresh_workload();
            }
            break;
        }
        case ID_BTN_RESET_SHIFT:
            app->config.shift_rules = default_shift_rules();
            app->config.coeff_flags = {true, true};
            app->save_config_quiet();
            app->refresh_weights_grid();
            app->refresh_staff_grid();
            app->recalc_and_refresh_workload();
            break;
        case ID_BTN_SCAN_COEFF: {
            std::wstring p = open_file_dialog(hwnd, L"图片文件 (*.jpg;*.jpeg;*.png;*.webp;*.bmp)\0*.jpg;*.jpeg;*.png;*.webp;*.bmp\0全部文件 (*.*)\0*.*\0", L"选择人员系数照片");
            if (!p.empty()) app->start_coeff_ocr_file(wide_to_utf8(p));
            break;
        }
        case ID_BTN_IMPORT_NAMES: {
            std::wstring p = open_file_dialog(hwnd, L"名单文档 (*.txt;*.docx;*.csv;*.xlsx)\0*.txt;*.docx;*.csv;*.xlsx\0全部文件 (*.*)\0*.*\0", L"导入人员名字文档");
            if (!p.empty()) app->import_names_into_staff(wide_to_utf8(p));
            break;
        }
        case ID_BTN_IMPORT_NAMES_WL: {
            std::wstring p = open_file_dialog(hwnd, L"名单文档 (*.txt;*.docx;*.csv;*.xlsx)\0*.txt;*.docx;*.csv;*.xlsx\0全部文件 (*.*)\0*.*\0", L"导入人员名字校准");
            if (!p.empty()) app->import_names_into_workload(wide_to_utf8(p));
            break;
        }
        case ID_BTN_ADD_STAFF:
            app->config.staff.push_back({"新人员", 1.00, 0.00});
            app->save_config_quiet();
            app->refresh_staff_grid();
            app->recalc_and_refresh_workload();
            break;
        case ID_BTN_DEL_STAFF: {
            int r = app->grid_staff.selected_row();
            if (r >= 0 && r < static_cast<int>(app->config.staff.size())) {
                app->config.staff.erase(app->config.staff.begin() + r);
                app->save_config_quiet();
                app->refresh_staff_grid();
                app->recalc_and_refresh_workload();
            }
            break;
        }
        case ID_BTN_CLEAR_STAFF:
            if (MessageBoxW(hwnd, L"确定清空当前全部人员系数吗？", L"确认", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                app->config.staff.clear();
                app->save_config_quiet();
                app->refresh_staff_grid();
                app->recalc_and_refresh_workload();
            }
            break;
        case ID_BTN_OPEN_WORKLOAD: {
            std::wstring p = open_file_dialog(hwnd, L"图片文件 (*.jpg;*.jpeg;*.png;*.webp;*.bmp)\0*.jpg;*.jpeg;*.png;*.webp;*.bmp\0全部文件 (*.*)\0*.*\0", L"选择工作量照片");
            if (!p.empty()) app->start_workload_ocr_file(wide_to_utf8(p));
            break;
        }
        case ID_BTN_ROT_LEFT:
            if (app->working_img.valid() && !app->busy) {
                app->working_img = rotate_image_90cw(app->working_img, 3);
                app->start_workload_ocr_image(app->working_img, false, nullptr);
            }
            break;
        case ID_BTN_ROT_RIGHT:
            if (app->working_img.valid() && !app->busy) {
                app->working_img = rotate_image_90cw(app->working_img, 1);
                app->start_workload_ocr_image(app->working_img, false, nullptr);
            }
            break;
        case ID_BTN_EDIT_CORNERS:
            if (!app->working_img.valid()) break;
            if (!app->photo_canvas.corner_edit_mode()) {
                app->photo_canvas.set_corner_edit_mode(true);
                SetWindowTextW(app->btn_edit_corners, L"完成重算");
            } else {
                app->photo_canvas.set_corner_edit_mode(false);
                SetWindowTextW(app->btn_edit_corners, L"调整四角");
                auto c = app->photo_canvas.corners();
                app->start_workload_ocr_image(app->working_img, false, &c);
            }
            break;
        case ID_BTN_NEXT_REVIEW:
            app->jump_to_next_review();
            break;
        case ID_BTN_Q_BLANK: app->apply_quick_value(ValueKind::Blank, 0.0, true); break;
        case ID_BTN_Q_1: app->apply_quick_value(ValueKind::Number, 1.0, true); break;
        case ID_BTN_Q_2: app->apply_quick_value(ValueKind::Number, 2.0, true); break;
        case ID_BTN_Q_3: app->apply_quick_value(ValueKind::Number, 3.0, true); break;
        case ID_BTN_Q_4: app->apply_quick_value(ValueKind::Number, 4.0, true); break;
        case ID_BTN_Q_5: app->apply_quick_value(ValueKind::Number, 5.0, true); break;
        case ID_BTN_Q_HALF: app->apply_quick_value(ValueKind::Number, 0.5, true); break;
        case ID_BTN_Q_CONFIRM: app->apply_quick_value(ValueKind::Blank, -1.0, true); break;
        case ID_BTN_EXPORT_XLSX: {
            if (app->sheet.rows.empty()) {
                MessageBoxW(hwnd, L"请先识别工作量照片。", L"提示", MB_OK | MB_ICONINFORMATION);
                break;
            }
            wchar_t ybuf[16]{}, mbuf[16]{};
            GetWindowTextW(app->edit_year, ybuf, 16);
            GetWindowTextW(app->edit_month, mbuf, 16);
            app->sheet.year = std::max(2000, std::min(2099, _wtoi(ybuf)));
            app->sheet.month = std::max(1, std::min(12, _wtoi(mbuf)));
            app->sheet.week_labels = default_week_labels(app->sheet.year, app->sheet.month, app->sheet.weeks);
            app->summary = evaluate_workload(app->sheet, app->config);

            std::wstring out_w = save_xlsx_dialog(hwnd, app->sheet.year, app->sheet.month);
            if (!out_w.empty()) {
                ExportOptions opts;
                opts.include_summary_sheet = true;
                std::string err;
                if (export_workload_xlsx(wide_to_utf8(out_w), app->sheet, app->config, app->summary, opts, err)) {
                    ShellExecuteW(hwnd, L"open", out_w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                } else {
                    MessageBoxW(hwnd, utf8_to_wide(err).c_str(), L"导出失败", MB_OK | MB_ICONERROR);
                }
            }
            break;
        }
        }
        return 0;
    }
    case WM_APP_COEFF_DONE: {
        std::unique_ptr<AsyncCoeffPayload> payload(reinterpret_cast<AsyncCoeffPayload*>(lp));
        app->busy = false;
        if (!payload->result.ok) {
            SetWindowTextW(app->lbl_staff_hint, L"系数表识别失败，可手动新增或换一张更清晰的照片");
            MessageBoxW(hwnd, utf8_to_wide(payload->result.error).c_str(), L"识别失败", MB_OK | MB_ICONERROR);
        } else {
            app->config.staff.clear();
            for (const auto& r : payload->result.rows) {
                app->config.staff.push_back({r.matched_name, r.title_coeff, r.duty_coeff});
            }
            app->save_config_quiet();
            app->refresh_staff_grid();
            app->recalc_and_refresh_workload();
            std::wostringstream msg;
            msg << L"已识别 " << payload->result.rows.size() << L" 人系数，可直接点格修改或拖入名字文件校准";
            SetWindowTextW(app->lbl_staff_hint, msg.str().c_str());
        }
        return 0;
    }
    case WM_APP_WORKLOAD_DONE: {
        std::unique_ptr<AsyncWorkloadPayload> payload(reinterpret_cast<AsyncWorkloadPayload*>(lp));
        app->busy = false;
        if (!payload->ok) {
            SetWindowTextW(app->lbl_status, L"工作量识别失败，可旋转或重新选择照片");
            MessageBoxW(hwnd, utf8_to_wide(payload->err).c_str(), L"识别失败", MB_OK | MB_ICONERROR);
        } else {
            app->working_img = std::move(payload->working_img);
            app->sheet = std::move(payload->sheet);
            app->photo_canvas.set_image(app->working_img);
            app->sel_wl_row = -1;
            app->sel_wl_cell_col = -2;
            app->recalc_and_refresh_workload();
            if (app->summary.pending_review_count > 0) {
                app->jump_to_next_review();
            }
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        UINT cnt = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::string> paths;
        for (UINT i = 0; i < cnt; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring wpth(len + 1, L'\0');
            DragQueryFileW(drop, i, &wpth[0], len + 1);
            wpth.resize(len);
            paths.push_back(wide_to_utf8(wpth));
        }
        DragFinish(drop);
        for (const auto& p : paths) {
            if (is_names_doc_ext(p)) {
                if (app->active_tab == 2 && !app->sheet.rows.empty()) app->import_names_into_workload(p);
                else app->import_names_into_staff(p);
            } else if (is_image_ext(p)) {
                if (app->active_tab == 1) app->start_coeff_ocr_file(p);
                else app->start_workload_ocr_file(p);
                break;
            }
        }
        return 0;
    }
    case WM_DESTROY:
        if (app->ui_font) DeleteObject(app->ui_font);
        if (app->ui_bold_font) DeleteObject(app->ui_bold_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmd_line, int nCmdShow) {
    if (cmd_line && std::wcsstr(cmd_line, L"--self-test") != nullptr) {
        AppConfig cfg = create_default_config();
        OcrEngine ocr;
        std::string err;
        bool ok = ocr.init_from_dir(resolve_models_dir(), err);
        return (cfg.staff.empty() && ok) ? 0 : 1;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // Best-effort DPI awareness on Win7+
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        using FnDpi = BOOL(WINAPI*)();
        auto fn = reinterpret_cast<FnDpi>(reinterpret_cast<void*>(GetProcAddress(u32, "SetProcessDPIAware")));
        if (fn) fn();
    }

    MainApp app;
    app.inst = hInst;
    app.init_state();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = main_wnd_proc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"NurseOCReportMainWindow";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        L"NurseOCReportMainWindow",
        L"护士工作量识别统计",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1280, 820,
        nullptr, nullptr, hInst, &app);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}
