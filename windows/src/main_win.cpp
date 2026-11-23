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

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include <miniz.h>

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
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace nurseocr;
using namespace nurseocr_win;

namespace {

constexpr UINT kWmDpiChanged = 0x02E0;
constexpr int kEmbeddedModelsResId = 101;

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
    ID_BTN_WL_ADD_ROW = 429,
    ID_BTN_WL_DEL_ROW = 430,

    // Context menu IDs
    ID_CTX_WL_ADD_ROW = 501,
    ID_CTX_WL_DEL_ROW = 502,

    // Name import modal dialog controls
    ID_DLG_NAMES_LABEL = 601,
    ID_DLG_NAMES_EDIT = 602,
    ID_DLG_NAMES_COUNT = 603,
    ID_DLG_NAMES_FILE_LABEL = 604,
    ID_DLG_NAMES_PICK_FILE = 605,
    ID_DLG_NAMES_OK = 606,
    ID_DLG_NAMES_CANCEL = 607,
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

// Resolve window/screen DPI dynamically without static Win8+/Win10 imports for Win7 SP1 safety.
UINT query_window_dpi(HWND hwnd) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32 && hwnd) {
        using FnGetDpiForWindow = UINT(WINAPI*)(HWND);
        auto fn = reinterpret_cast<FnGetDpiForWindow>(
            reinterpret_cast<void*>(GetProcAddress(u32, "GetDpiForWindow")));
        if (fn) {
            UINT d = fn(hwnd);
            if (d >= 96) return d;
        }
    }
    HDC hdc = GetDC(hwnd);
    int dpi = 96;
    if (hdc) {
        dpi = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(hwnd, hdc);
    }
    return static_cast<UINT>(dpi >= 96 ? dpi : 96);
}

std::string get_exe_dir() {
    wchar_t buf[MAX_PATH]{};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::filesystem::path p(buf);
    return wide_to_utf8(p.parent_path().wstring());
}

std::string resolve_config_path() {
    wchar_t appdata[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, appdata))) {
        std::filesystem::path p = std::filesystem::path(appdata) / L"NurseOCReport" / L"config.json";
        return wide_to_utf8(p.wstring());
    }
    return wide_to_utf8((std::filesystem::path(utf8_to_wide(get_exe_dir())) / L"config.json").wstring());
}

struct PackedModelEntry {
    std::string name;
    uint64_t raw_size = 0;
    const uint8_t* comp_ptr = nullptr;
    uint64_t comp_size = 0;
};

uint32_t read_le32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t read_le64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | static_cast<uint64_t>(p[i]);
    }
    return v;
}

bool is_safe_model_filename(const std::string& name) {
    if (name.empty() || name.size() > 128) return false;
    if (name.find("..") != std::string::npos) return false;
    for (char ch : name) {
        if (ch == '/' || ch == '\\' || ch == ':' || static_cast<unsigned char>(ch) < 0x20) {
            return false;
        }
    }
    return true;
}

bool write_bytes_atomic(const std::filesystem::path& target,
                        const uint8_t* data,
                        size_t size,
                        std::string& out_err) {
    std::wstring tmp_name = target.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId());
    HANDLE h = CreateFileW(
        tmp_name.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        out_err = "Failed to create temporary cache file: " + wide_to_utf8(tmp_name);
        return false;
    }

    size_t written_total = 0;
    bool write_ok = true;
    while (written_total < size) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - written_total, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(h, data + written_total, chunk, &written, nullptr) || written == 0) {
            write_ok = false;
            break;
        }
        written_total += written;
    }
    CloseHandle(h);

    if (!write_ok) {
        DeleteFileW(tmp_name.c_str());
        out_err = "Failed to write temporary cache file: " + wide_to_utf8(tmp_name);
        return false;
    }

    if (!MoveFileExW(tmp_name.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp_name.c_str());
        out_err = "Failed to move cache file into place: " + wide_to_utf8(target.wstring());
        return false;
    }
    return true;
}

std::vector<std::filesystem::path> candidate_cache_roots() {
    std::vector<std::filesystem::path> roots;
    wchar_t buf[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, buf)) && buf[0] != L'\0') {
        roots.emplace_back(buf);
    } else {
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
        if (n > 0 && n < MAX_PATH) roots.emplace_back(buf);
    }

    wchar_t tmp_buf[MAX_PATH]{};
    DWORD tn = GetTempPathW(MAX_PATH, tmp_buf);
    if (tn > 0 && tn < MAX_PATH) {
        roots.emplace_back(tmp_buf);
    }
    return roots;
}

bool ensure_embedded_models_cached(std::string& out_cache_dir, std::string& out_err) {
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(kEmbeddedModelsResId), RT_RCDATA);
    if (!res) {
        out_err = "Missing embedded model bundle resource (RCDATA 101).";
        return false;
    }
    DWORD res_size = SizeofResource(mod, res);
    HGLOBAL res_glob = LoadResource(mod, res);
    if (!res_glob || res_size < 32) {
        out_err = "Failed to load embedded model bundle resource.";
        return false;
    }
    const uint8_t* ptr = static_cast<const uint8_t*>(LockResource(res_glob));
    if (!ptr) {
        out_err = "Failed to lock embedded model bundle resource.";
        return false;
    }

    if (std::memcmp(ptr, "NOCRMPAK", 8) != 0) {
        out_err = "Invalid embedded model pack magic.";
        return false;
    }
    uint32_t version = read_le32(ptr + 8);
    if (version != 1) {
        out_err = "Unsupported embedded model pack version.";
        return false;
    }
    std::string hash16(reinterpret_cast<const char*>(ptr + 12), 16);
    for (char ch : hash16) {
        if (!std::isxdigit(static_cast<unsigned char>(ch))) {
            out_err = "Corrupt hash digest in embedded model pack.";
            return false;
        }
    }
    uint32_t count = read_le32(ptr + 28);
    if (count != 4) {
        out_err = "Unexpected model count in embedded model pack.";
        return false;
    }

    std::vector<PackedModelEntry> entries;
    entries.reserve(count);
    size_t pos = 32;
    for (uint32_t i = 0; i < count; ++i) {
        if (pos + 20 > res_size) {
            out_err = "Truncated embedded model entry header.";
            return false;
        }
        uint32_t name_len = read_le32(ptr + pos);
        uint64_t raw_size = read_le64(ptr + pos + 4);
        uint64_t comp_size = read_le64(ptr + pos + 12);
        pos += 20;
        if (name_len == 0 || pos + name_len + comp_size > res_size || raw_size > (256ULL << 20)) {
            out_err = "Corrupt embedded model entry bounds.";
            return false;
        }
        std::string name(reinterpret_cast<const char*>(ptr + pos), name_len);
        if (!is_safe_model_filename(name)) {
            out_err = "Unsafe model entry filename.";
            return false;
        }
        pos += name_len;
        entries.push_back({std::move(name), raw_size, ptr + pos, comp_size});
        pos += static_cast<size_t>(comp_size);
    }

    auto roots = candidate_cache_roots();
    if (roots.empty()) {
        out_err = "Unable to resolve LOCALAPPDATA or TEMP directory.";
        return false;
    }

    std::wstring folder_name = L"models-" + utf8_to_wide(hash16);
    std::string last_err;
    for (const auto& root : roots) {
        std::filesystem::path cache_dir = root / L"NurseOCReport" / L"cache" / folder_name;
        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);
        if (ec) {
            last_err = "Cannot create cache directory: " + wide_to_utf8(cache_dir.wstring());
            continue;
        }

        bool all_cached = true;
        for (const auto& entry : entries) {
            std::filesystem::path fpath = cache_dir / utf8_to_wide(entry.name);
            std::error_code fec;
            if (!std::filesystem::is_regular_file(fpath, fec) ||
                std::filesystem::file_size(fpath, fec) != entry.raw_size || fec) {
                all_cached = false;
                break;
            }
        }
        if (all_cached) {
            out_cache_dir = wide_to_utf8(cache_dir.wstring());
            return true;
        }

        bool root_ok = true;
        for (const auto& entry : entries) {
            std::filesystem::path fpath = cache_dir / utf8_to_wide(entry.name);
            std::error_code fec;
            if (std::filesystem::is_regular_file(fpath, fec) &&
                std::filesystem::file_size(fpath, fec) == entry.raw_size && !fec) {
                continue;
            }
            std::vector<uint8_t> raw_buf(static_cast<size_t>(entry.raw_size));
            mz_ulong dest_len = static_cast<mz_ulong>(entry.raw_size);
            int zrc = mz_uncompress(raw_buf.data(), &dest_len,
                                    entry.comp_ptr,
                                    static_cast<mz_ulong>(entry.comp_size));
            if (zrc != MZ_OK || dest_len != entry.raw_size) {
                out_err = "Decompression failed for embedded model: " + entry.name;
                return false;
            }
            std::string werr;
            if (!write_bytes_atomic(fpath, raw_buf.data(), raw_buf.size(), werr)) {
                last_err = werr;
                root_ok = false;
                break;
            }
        }

        if (root_ok) {
            out_cache_dir = wide_to_utf8(cache_dir.wstring());
            return true;
        }
    }

    out_err = last_err.empty() ? "Failed to cache embedded OCR models." : last_err;
    return false;
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

bool quad_has_area(const std::array<Point2f, 4>& q) {
    float min_x = q[0].x, max_x = q[0].x;
    float min_y = q[0].y, max_y = q[0].y;
    for (int k = 1; k < 4; ++k) {
        min_x = std::min(min_x, q[k].x);
        max_x = std::max(max_x, q[k].x);
        min_y = std::min(min_y, q[k].y);
        max_y = std::max(max_y, q[k].y);
    }
    return (max_x - min_x) >= 2.0f && (max_y - min_y) >= 2.0f;
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

// ---------------------------------------------------------------------------
// DPI-scaled Dual-Mode Name Import Modal Dialog
// ---------------------------------------------------------------------------
struct NameImportDialogState {
    HINSTANCE inst = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr;
    HFONT bold_font = nullptr;
    HWND hwnd = nullptr;
    HWND lbl_text_hint = nullptr;
    HWND edit_names = nullptr;
    HWND lbl_count = nullptr;
    HWND lbl_file_hint = nullptr;
    HWND btn_pick_file = nullptr;
    HWND btn_ok = nullptr;
    HWND btn_cancel = nullptr;
    bool done = false;
    bool accepted = false;
    std::vector<std::string> result_names;

    int S(int px) const { return MulDiv(px, static_cast<int>(dpi), 96); }

    std::wstring read_edit_text() const {
        if (!edit_names) return L"";
        int len = GetWindowTextLengthW(edit_names);
        std::wstring buf(static_cast<size_t>(len + 1), L'\0');
        if (len > 0) GetWindowTextW(edit_names, &buf[0], len + 1);
        buf.resize(static_cast<size_t>(len));
        return buf;
    }

    void update_parsed_count() {
        auto parsed = parse_names_from_plain_text(wide_to_utf8(read_edit_text()));
        std::wstring msg = L"已识别人数：" + std::to_wstring(parsed.size()) + L" 人";
        if (lbl_count) SetWindowTextW(lbl_count, msg.c_str());
    }

    void layout(int cw, int ch) {
        int m = S(14);
        int y = m;
        int w = std::max(S(200), cw - 2 * m);

        MoveWindow(lbl_text_hint, m, y, w, S(24), TRUE);
        y += S(28);

        int bottom_reserved = S(128);
        int edit_h = std::max(S(110), ch - y - bottom_reserved);
        MoveWindow(edit_names, m, y, w, edit_h, TRUE);
        y += edit_h + S(8);

        MoveWindow(lbl_count, m, y, w, S(22), TRUE);
        y += S(28);

        MoveWindow(lbl_file_hint, m, y, w, S(22), TRUE);
        y += S(26);

        MoveWindow(btn_pick_file, m, y, std::min(w, S(310)), S(34), TRUE);

        int bw = S(104);
        int bh = S(34);
        int by = ch - m - bh;
        MoveWindow(btn_cancel, cw - m - bw, by, bw, bh, TRUE);
        MoveWindow(btn_ok, cw - m - bw * 2 - S(10), by, bw, bh, TRUE);
    }
};

LRESULT CALLBACK import_edit_sub_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                      UINT_PTR, DWORD_PTR) {
    if (msg == WM_KEYDOWN && wp == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        SendMessageW(hwnd, EM_SETSEL, 0, -1);
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

LRESULT CALLBACK name_import_dlg_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    NameImportDialogState* st = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        st = reinterpret_cast<NameImportDialogState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        st->hwnd = hwnd;
    } else {
        st = reinterpret_cast<NameImportDialogState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (!st) return DefWindowProcW(hwnd, msg, wp, lp);

    switch (msg) {
    case WM_CREATE: {
        auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int id, HFONT f) -> HWND {
            HWND h = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                                     0, 0, 10, 10, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), st->inst, nullptr);
            if (f) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
            return h;
        };
        st->lbl_text_hint = mk(L"STATIC",
                               L"方式一：直接输入或粘贴名单（支持空格、全角空格、顿号、逗号、换行分隔）",
                               SS_LEFT, ID_DLG_NAMES_LABEL, st->bold_font);
        st->edit_names = CreateWindowExW(
            WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
            0, 0, 10, 10, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(ID_DLG_NAMES_EDIT)), st->inst, nullptr);
        SendMessageW(st->edit_names, WM_SETFONT, reinterpret_cast<WPARAM>(st->font), TRUE);
        SendMessageW(st->edit_names, EM_SETLIMITTEXT, 65536, 0);
        SetWindowSubclass(st->edit_names, import_edit_sub_proc, 1, 0);

        st->lbl_count = mk(L"STATIC", L"已识别人数：0 人", SS_LEFT, ID_DLG_NAMES_COUNT, st->font);
        st->lbl_file_hint = mk(L"STATIC", L"方式二：从本地文档导入名单", SS_LEFT, ID_DLG_NAMES_FILE_LABEL, st->bold_font);
        st->btn_pick_file = mk(L"BUTTON", L"从文件选择 (.txt/.csv/.docx/.xlsx)...",
                               BS_PUSHBUTTON | WS_TABSTOP, ID_DLG_NAMES_PICK_FILE, st->font);
        st->btn_ok = mk(L"BUTTON", L"确认导入", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_DLG_NAMES_OK, st->font);
        st->btn_cancel = mk(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, ID_DLG_NAMES_CANCEL, st->font);

        RECT rc{};
        GetClientRect(hwnd, &rc);
        st->layout(rc.right - rc.left, rc.bottom - rc.top);
        SetFocus(st->edit_names);
        return 0;
    }
    case WM_SIZE:
        st->layout(LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int code = HIWORD(wp);
        if (id == ID_DLG_NAMES_EDIT && code == EN_CHANGE) {
            st->update_parsed_count();
            return 0;
        }
        if (code != BN_CLICKED && code != 1) return 0;

        if (id == ID_DLG_NAMES_PICK_FILE) {
            std::wstring path = open_file_dialog(
                hwnd,
                L"名单文档 (*.txt;*.docx;*.csv;*.xlsx)\0*.txt;*.docx;*.csv;*.xlsx\0全部文件 (*.*)\0*.*\0",
                L"选择人员名单文档");
            if (!path.empty()) {
                std::vector<std::string> names;
                std::string err;
                if (!read_names_from_file(wide_to_utf8(path), names, err)) {
                    MessageBoxW(hwnd, utf8_to_wide(err).c_str(), L"导入名单失败", MB_OK | MB_ICONERROR);
                } else if (names.empty()) {
                    MessageBoxW(hwnd, L"所选文件中未提取到有效人员姓名。", L"提示", MB_OK | MB_ICONWARNING);
                } else {
                    st->result_names = std::move(names);
                    st->accepted = true;
                    st->done = true;
                }
            }
            return 0;
        }
        if (id == ID_DLG_NAMES_OK || id == IDOK) {
            auto names = parse_names_from_plain_text(wide_to_utf8(st->read_edit_text()));
            if (names.empty()) {
                MessageBoxW(hwnd, L"请输入或粘贴至少一个姓名，或点击从文件选择。", L"提示", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            st->result_names = std::move(names);
            st->accepted = true;
            st->done = true;
            return 0;
        }
        if (id == ID_DLG_NAMES_CANCEL || id == IDCANCEL) {
            st->accepted = false;
            st->done = true;
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        st->accepted = false;
        st->done = true;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool show_name_import_dialog(HWND parent,
                             HINSTANCE inst,
                             UINT dpi,
                             HFONT font,
                             HFONT bold_font,
                             std::vector<std::string>& out_names) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = name_import_dlg_proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"NurseNameImportDlgClass";
        RegisterClassExW(&wc);
        registered = true;
    }

    NameImportDialogState st;
    st.inst = inst;
    st.dpi = std::max(96u, dpi);
    st.font = font;
    st.bold_font = bold_font;

    int dlg_w = st.S(540);
    int dlg_h = st.S(440);
    RECT pr{};
    GetWindowRect(parent, &pr);
    int x = pr.left + ((pr.right - pr.left) - dlg_w) / 2;
    int y = pr.top + ((pr.bottom - pr.top) - dlg_h) / 2;

    HWND dlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"NurseNameImportDlgClass",
        L"导入人员名单",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlg_w, dlg_h,
        parent, nullptr, inst, &st);
    if (!dlg) return false;

    EnableWindow(parent, FALSE);

    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            st.accepted = false;
            st.done = true;
            break;
        }
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    if (st.edit_names) RemoveWindowSubclass(st.edit_names, import_edit_sub_proc, 1);
    DestroyWindow(dlg);

    if (st.accepted && !st.result_names.empty()) {
        out_names = std::move(st.result_names);
        return true;
    }
    return false;
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
    UINT dpi_ = 96;
    HFONT ui_font = nullptr;
    HFONT ui_bold_font = nullptr;
    HFONT title_font = nullptr;

    HWND tab_ctrl = nullptr;
    int active_tab = 2;

    // Tab 1 controls (Weights)
    HWND chk_title_coeff = nullptr;
    HWND chk_duty_coeff = nullptr;
    HWND btn_add_shift = nullptr;
    HWND btn_del_shift = nullptr;
    HWND btn_reset_shift = nullptr;
    EditableGrid grid_weights;

    // Tab 2 controls (Staff)
    HWND btn_scan_coeff = nullptr;
    HWND btn_import_names = nullptr;
    HWND btn_add_staff = nullptr;
    HWND btn_del_staff = nullptr;
    HWND btn_clear_staff = nullptr;
    HWND lbl_staff_hint = nullptr;
    EditableGrid grid_staff;

    // Tab 3 controls (Workload)
    HWND btn_open_wl = nullptr;
    HWND btn_rot_left = nullptr;
    HWND btn_rot_right = nullptr;
    HWND btn_edit_corners = nullptr;
    HWND btn_next_review = nullptr;
    HWND btn_import_names_wl = nullptr;
    HWND btn_wl_add_row = nullptr;
    HWND btn_wl_del_row = nullptr;
    HWND btn_export_xlsx = nullptr;
    HWND edit_year_ = nullptr;
    HWND lbl_year = nullptr;
    HWND edit_month_ = nullptr;
    HWND lbl_month = nullptr;
    HWND lbl_status = nullptr;
    HWND lbl_quick_info = nullptr;
    HWND quick_btns[8]{};
    PhotoCanvas photo_canvas;
    EditableGrid grid_workload;

    RECT preview_box_{};

    // State
    OcrEngine ocr_engine_;
    std::string model_init_err_;
    std::string config_path;
    AppConfig config;
    ImageRgb working_img;
    ImageRgb selected_cell_preview;
    WorkloadSheet sheet_;
    SheetSummary summary;
    bool busy = false;
    bool syncing_ym_ = false;
    int sel_wl_row = -1;
    int sel_wl_cell_col = -2; // -1=name, >=0 index into row.cells

    int S(int px) const { return MulDiv(px, static_cast<int>(dpi_), 96); }

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
        sheet_.year = tm_now.tm_year + 1900;
        sheet_.month = tm_now.tm_mon + 1;
        sheet_.weeks = 5;
        sheet_.column_shift_codes = {"A", "Z", "P", "Y", "T"};
        sheet_.column_shift_names = {"A(主)", "治/白", "P", "夜", "技术难度"};

        std::string cache_dir;
        if (!ensure_embedded_models_cached(cache_dir, model_init_err_) ||
            !ocr_engine_.init_from_dir(cache_dir, model_init_err_)) {
            if (model_init_err_.empty()) {
                model_init_err_ = "Failed to initialize embedded OCR models.";
            }
        }
    }

    void rebuild_fonts() {
        HFONT new_ui = CreateFontW(S(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                   DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                   CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        HFONT new_bold = CreateFontW(S(17), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                     CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        HFONT new_title = CreateFontW(S(20), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

        if (ui_font) DeleteObject(ui_font);
        if (ui_bold_font) DeleteObject(ui_bold_font);
        if (title_font) DeleteObject(title_font);
        ui_font = new_ui;
        ui_bold_font = new_bold;
        title_font = new_title;

        auto setf = [&](HWND h, HFONT f) {
            if (h && f) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        };
        setf(tab_ctrl, ui_font);
        if (tab_ctrl) TabCtrl_SetPadding(tab_ctrl, S(12), S(6));

        setf(chk_title_coeff, ui_font);
        setf(chk_duty_coeff, ui_font);
        setf(btn_add_shift, ui_font);
        setf(btn_del_shift, ui_font);
        setf(btn_reset_shift, ui_font);

        setf(btn_scan_coeff, ui_font);
        setf(btn_import_names, ui_font);
        setf(btn_add_staff, ui_font);
        setf(btn_del_staff, ui_font);
        setf(btn_clear_staff, ui_font);
        setf(lbl_staff_hint, ui_font);

        setf(btn_open_wl, ui_font);
        setf(btn_rot_left, ui_font);
        setf(btn_rot_right, ui_font);
        setf(btn_edit_corners, ui_font);
        setf(btn_next_review, ui_font);
        setf(btn_import_names_wl, ui_font);
        setf(btn_wl_add_row, ui_font);
        setf(btn_wl_del_row, ui_font);
        setf(edit_year_, ui_font);
        setf(lbl_year, ui_font);
        setf(edit_month_, ui_font);
        setf(lbl_month, ui_font);
        setf(btn_export_xlsx, ui_font);
        setf(lbl_status, ui_bold_font);
        setf(lbl_quick_info, ui_font);
        for (HWND b : quick_btns) setf(b, ui_font);

        grid_weights.set_font(ui_font, ui_bold_font);
        grid_staff.set_font(ui_font, ui_bold_font);
        grid_workload.set_font(ui_font, ui_bold_font);
        photo_canvas.set_font(ui_font);
    }

    void set_dpi(UINT new_dpi) {
        dpi_ = std::max(96u, new_dpi);
        rebuild_fonts();
        grid_weights.set_dpi(dpi_);
        grid_staff.set_dpi(dpi_);
        grid_workload.set_dpi(dpi_);
        photo_canvas.set_dpi(dpi_);
    }

    void save_config_quiet() {
        std::string err;
        save_app_config(config_path, config, err);
    }

    void sync_year_month_edits() {
        syncing_ym_ = true;
        if (edit_year_ && sheet_.year >= 2000 && sheet_.year <= 2099) {
            SetWindowTextW(edit_year_, std::to_wstring(sheet_.year).c_str());
        }
        if (edit_month_ && sheet_.month >= 1 && sheet_.month <= 12) {
            SetWindowTextW(edit_month_, std::to_wstring(sheet_.month).c_str());
        }
        syncing_ym_ = false;
    }

    void apply_workload_result(ImageRgb&& img, WorkloadSheet&& recognized_sheet) {
        working_img = std::move(img);
        sheet_ = std::move(recognized_sheet);
        sync_year_month_edits();
        photo_canvas.set_image(working_img);
        sel_wl_row = -1;
        sel_wl_cell_col = -2;
        recalc_and_refresh_workload();
        if (summary.pending_review_count > 0) {
            jump_to_next_review(false);
        }
    }

    void reindex_workload_rows() {
        for (size_t i = 0; i < sheet_.rows.size(); ++i) {
            int idx = static_cast<int>(i);
            sheet_.rows[i].row = idx;
            for (auto& cell : sheet_.rows[i].cells) {
                cell.row = idx;
            }
        }
    }

    void add_workload_row() {
        if (sheet_.weeks <= 0) sheet_.weeks = 5;
        if (sheet_.column_shift_names.empty()) {
            sheet_.column_shift_codes = {"A", "Z", "P", "Y", "T"};
            sheet_.column_shift_names = {"A(主)", "治/白", "P", "夜", "技术难度"};
        }

        int new_idx = static_cast<int>(sheet_.rows.size());
        int cols_per_week = static_cast<int>(sheet_.column_shift_names.size());
        WorkloadRow wrow;
        wrow.row = new_idx;
        wrow.staff_name = "新成员";
        wrow.name_needs_review = true;
        wrow.cells.reserve(static_cast<size_t>(sheet_.weeks) * cols_per_week);
        for (int w = 0; w < sheet_.weeks; ++w) {
            for (int sc = 0; sc < cols_per_week; ++sc) {
                WorkloadCell cell;
                cell.row = new_idx;
                cell.week = w;
                cell.shift_col = sc;
                cell.kind = ValueKind::Blank;
                cell.value = 0.0;
                cell.status = ReviewStatus::AutoConfirmed;
                wrow.cells.push_back(std::move(cell));
            }
        }
        sheet_.rows.push_back(std::move(wrow));
        reindex_workload_rows();

        sel_wl_row = new_idx;
        sel_wl_cell_col = -1;
        recalc_and_refresh_workload();
        grid_workload.select_cell(new_idx, 0, true);
        grid_workload.begin_edit(new_idx, 0);
    }

    void delete_workload_row(int row_idx) {
        if (row_idx < 0 || row_idx >= static_cast<int>(sheet_.rows.size())) {
            MessageBoxW(hwnd, L"请先在右侧表格中选中要删除的人员行。", L"提示", MB_OK | MB_ICONINFORMATION);
            return;
        }
        const auto& r = sheet_.rows[static_cast<size_t>(row_idx)];
        std::wstring disp = utf8_to_wide(r.staff_name.empty() ? r.ocr_name : r.staff_name);
        std::wstring prompt = L"确定删除第 " + std::to_wstring(row_idx + 1) + L" 行";
        if (!disp.empty()) prompt += L"（" + disp + L"）";
        prompt += L"吗？";
        if (MessageBoxW(hwnd, prompt.c_str(), L"确认删除行", MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }

        sheet_.rows.erase(sheet_.rows.begin() + row_idx);
        reindex_workload_rows();

        if (sheet_.rows.empty()) {
            sel_wl_row = -1;
            sel_wl_cell_col = -2;
        } else {
            sel_wl_row = std::min(row_idx, static_cast<int>(sheet_.rows.size()) - 1);
            int max_cells = static_cast<int>(sheet_.rows[static_cast<size_t>(sel_wl_row)].cells.size());
            if (sel_wl_cell_col >= max_cells) sel_wl_cell_col = -1;
            if (sel_wl_cell_col < -1) sel_wl_cell_col = -1;
        }

        recalc_and_refresh_workload();
        if (!sheet_.rows.empty() && sel_wl_row >= 0) {
            int gcol = (sel_wl_cell_col < 0) ? 0 : (1 + sel_wl_cell_col);
            grid_workload.select_cell(sel_wl_row, gcol, true);
            photo_canvas.select_highlight(sel_wl_row, sel_wl_cell_col, true);
        } else {
            photo_canvas.select_highlight(-1, -2, false);
        }
    }

    void refresh_weights_grid() {
        SendMessageW(chk_title_coeff, BM_SETCHECK, config.coeff_flags.enable_title_coeff ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessageW(chk_duty_coeff, BM_SETCHECK, config.coeff_flags.enable_duty_coeff ? BST_CHECKED : BST_UNCHECKED, 0);

        std::vector<GridColumn> cols = {
            {L"启用", 92, GridCellKind::Checkbox, true, true},
            {L"班次名称", 280, GridCellKind::Text, true, true},
            {L"权重系数", 180, GridCellKind::Text, true, true},
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
            {L"序号", 88, GridCellKind::ReadOnly, false, true},
            {L"姓名", 240, GridCellKind::Text, true, true},
            {L"职称系数", 180, GridCellKind::Text, true, true},
            {L"职务系数", 180, GridCellKind::Text, true, true},
            {L"总系数", 180, GridCellKind::ReadOnly, false, true},
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
        summary = evaluate_workload(sheet_, config);

        int data_cols = sheet_.weeks * static_cast<int>(sheet_.column_shift_names.size());
        std::vector<GridColumn> cols;
        cols.push_back({L"姓名", 120, GridCellKind::Text, true, true});
        static const wchar_t* const kShortShift[] = {L"A", L"治", L"P", L"夜", L"难"};
        for (int w = 0; w < sheet_.weeks; ++w) {
            for (size_t sc = 0; sc < sheet_.column_shift_names.size(); ++sc) {
                std::wstring hdr = L"W" + std::to_wstring(w + 1) + L" ";
                hdr += (sc < 5) ? kShortShift[sc] : utf8_to_wide(sheet_.column_shift_names[sc]);
                cols.push_back({hdr, 66, GridCellKind::Text, true, true});
            }
        }
        cols.push_back({L"加权合计", 108, GridCellKind::ReadOnly, false, true});
        cols.push_back({L"总系数", 92, GridCellKind::ReadOnly, false, true});
        cols.push_back({L"最终总分", 108, GridCellKind::ReadOnly, false, true});

        grid_workload.set_columns(cols, true);
        grid_workload.set_name_drop_column(0);

        std::vector<std::vector<GridCell>> rows;
        std::vector<HighlightBox> boxes;

        for (size_t ri = 0; ri < sheet_.rows.size(); ++ri) {
            const auto& wrow = sheet_.rows[ri];
            const PersonScore* ps = (ri < summary.people.size()) ? &summary.people[ri] : nullptr;

            std::vector<GridCell> gr(cols.size());
            std::wstring nm = utf8_to_wide(wrow.staff_name.empty() ? wrow.ocr_name : wrow.staff_name);
            GridCellTone ntone = wrow.name_needs_review ? GridCellTone::ReviewYellow : GridCellTone::Normal;
            gr[0] = {nm, GridCellKind::Text, ntone, false, true};
            if (quad_has_area(wrow.name_quad)) {
                boxes.push_back({static_cast<int>(ri), -1, wrow.name_quad, wrow.name_needs_review});
            }

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
                    if (quad_has_area(cell.orig_quad)) {
                        boxes.push_back({static_cast<int>(ri), ci, cell.orig_quad,
                                         cell.status == ReviewStatus::NeedsReview});
                    }
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

        if (!sheet_.rows.empty()) {
            std::vector<GridCell> tot(cols.size());
            tot[0] = {L"合计", GridCellKind::ReadOnly, GridCellTone::TotalRow, false, false};
            for (int ci = 0; ci < data_cols; ++ci) {
                double sum_c = 0.0;
                for (const auto& wrow : sheet_.rows) {
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

        grid_workload.set_rows(std::move(rows), sheet_.rows.empty() ? 0 : 1);
        photo_canvas.set_overlays(std::move(boxes), sheet_.geometry.corners);

        std::wostringstream st;
        if (busy) {
            st << L"正在识别中，请稍候...";
        } else if (sheet_.rows.empty()) {
            st << L"尚未载入工作量照片";
        } else {
            st << L"共 " << sheet_.rows.size() << L" 人   待确认 " << summary.pending_review_count
               << L" 处   全员总分 " << utf8_to_wide(format_number_2dp(summary.final_grand_total));
        }
        SetWindowTextW(lbl_status, st.str().c_str());
        update_quick_preview();
    }

    void update_quick_preview() {
        selected_cell_preview = ImageRgb{};
        std::wstring info = L"点击左图或右表单元格，在此核对或快捷填写";
        if (sel_wl_row >= 0 && sel_wl_row < static_cast<int>(sheet_.rows.size())) {
            const auto& row = sheet_.rows[static_cast<size_t>(sel_wl_row)];
            std::wstring nm = utf8_to_wide(row.staff_name.empty() ? row.ocr_name : row.staff_name);
            if (sel_wl_cell_col == -1) {
                if (working_img.valid() && quad_has_area(row.name_quad)) {
                    int cw = std::max(240, S(150) * 2);
                    int ch = std::max(108, S(68) * 2);
                    selected_cell_preview = crop_quad_image(working_img, row.name_quad, cw, ch);
                }
                info = nm + L" 姓名";
            } else if (sel_wl_cell_col >= 0 && sel_wl_cell_col < static_cast<int>(row.cells.size())) {
                const auto& cell = row.cells[static_cast<size_t>(sel_wl_cell_col)];
                if (working_img.valid() && quad_has_area(cell.orig_quad)) {
                    float min_x = cell.orig_quad[0].x, max_x = cell.orig_quad[0].x;
                    float min_y = cell.orig_quad[0].y, max_y = cell.orig_quad[0].y;
                    for (int k = 1; k < 4; ++k) {
                        min_x = std::min(min_x, cell.orig_quad[k].x);
                        max_x = std::max(max_x, cell.orig_quad[k].x);
                        min_y = std::min(min_y, cell.orig_quad[k].y);
                        max_y = std::max(max_y, cell.orig_quad[k].y);
                    }
                    float qw = std::max(1.0f, max_x - min_x);
                    float qh = std::max(1.0f, max_y - min_y);
                    int crop_h = std::max(120, S(68) * 2);
                    int crop_w = std::clamp(static_cast<int>(std::round(crop_h * (qw / qh))),
                                            96, std::max(280, S(150) * 2));
                    selected_cell_preview = crop_quad_image(working_img, cell.orig_quad, crop_w, crop_h);
                }
                std::wstring sname = (cell.shift_col < static_cast<int>(sheet_.column_shift_names.size()))
                    ? utf8_to_wide(sheet_.column_shift_names[static_cast<size_t>(cell.shift_col)])
                    : L"";
                std::wostringstream oss;
                oss << nm << L"  第" << (cell.week + 1) << L"周 " << sname;
                if (!cell.review_reason.empty()) oss << L"  " << utf8_to_wide(cell.review_reason);
                info = oss.str();
            }
        }
        SetWindowTextW(lbl_quick_info, info.c_str());
        InvalidateRect(hwnd, &preview_box_, FALSE);
    }

    void jump_to_next_review(bool show_done_dialog = true) {
        if (sheet_.rows.empty()) return;
        int n_rows = static_cast<int>(sheet_.rows.size());
        int total_checks = 0;
        for (const auto& r : sheet_.rows) total_checks += 1 + static_cast<int>(r.cells.size());
        if (total_checks == 0) return;

        auto step_next = [&](int& r, int& c) {
            if (c < 0) {
                if (!sheet_.rows[static_cast<size_t>(r)].cells.empty()) {
                    c = 0;
                    return;
                }
            } else if (c + 1 < static_cast<int>(sheet_.rows[static_cast<size_t>(r)].cells.size())) {
                ++c;
                return;
            }
            r = (r + 1) % n_rows;
            c = -1;
        };

        int cur_r = (sel_wl_row < 0) ? 0 : std::min(sel_wl_row, n_rows - 1);
        int cur_c = (sel_wl_row < 0) ? -1 : (sel_wl_cell_col < -1 ? -1 : sel_wl_cell_col);
        bool check_current_first = (sel_wl_row < 0);

        for (int k = 0; k < total_checks; ++k) {
            if (!(k == 0 && check_current_first)) {
                step_next(cur_r, cur_c);
            }
            bool need = false;
            if (cur_c == -1) need = sheet_.rows[static_cast<size_t>(cur_r)].name_needs_review;
            else need = (sheet_.rows[static_cast<size_t>(cur_r)].cells[static_cast<size_t>(cur_c)].status == ReviewStatus::NeedsReview);
            if (need) {
                sel_wl_row = cur_r;
                sel_wl_cell_col = cur_c;
                grid_workload.select_cell(cur_r, cur_c == -1 ? 0 : (1 + cur_c), true);
                photo_canvas.select_highlight(cur_r, cur_c, true);
                update_quick_preview();
                return;
            }
        }
        if (show_done_dialog) {
            MessageBoxW(hwnd, L"全部待确认项已处理完成，可以直接导出表格。", L"提示", MB_OK | MB_ICONINFORMATION);
        } else {
            SetWindowTextW(lbl_quick_info, L"全部待确认项已处理完成，可直接点击导出表格");
        }
    }

    void apply_quick_value(ValueKind kind, double val, bool jump_next) {
        if (sel_wl_row < 0 || sel_wl_row >= static_cast<int>(sheet_.rows.size())) return;
        if (sel_wl_cell_col == -1) {
            sheet_.rows[static_cast<size_t>(sel_wl_row)].name_needs_review = false;
            recalc_and_refresh_workload();
            if (jump_next) jump_to_next_review(false);
            return;
        }
        if (sel_wl_cell_col < 0 ||
            sel_wl_cell_col >= static_cast<int>(sheet_.rows[static_cast<size_t>(sel_wl_row)].cells.size())) {
            return;
        }
        auto& cell = sheet_.rows[static_cast<size_t>(sel_wl_row)].cells[static_cast<size_t>(sel_wl_cell_col)];
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
        if (jump_next) jump_to_next_review(false);
    }

    void start_coeff_ocr_file(const std::string& path) {
        if (busy) return;
        if (!ocr_engine_.is_ready()) {
            MessageBoxW(hwnd, utf8_to_wide("OCR 模型未就绪: " + model_init_err_).c_str(),
                        L"提示", MB_OK | MB_ICONWARNING);
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
                ocr_engine_.process_coeff_photo(img, dict, payload->result);
            }
            PostMessageW(hwnd, WM_APP_COEFF_DONE, 0, reinterpret_cast<LPARAM>(payload));
        }).detach();
    }

    void start_workload_ocr_image(const ImageRgb& raw_img, bool auto_orient, const std::array<Point2f, 4>* manual_c) {
        if (busy) return;
        if (!ocr_engine_.is_ready()) {
            MessageBoxW(hwnd, utf8_to_wide("OCR 模型未就绪: " + model_init_err_).c_str(),
                        L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
        busy = true;
        wchar_t ybuf[16]{}, mbuf[16]{};
        GetWindowTextW(edit_year_, ybuf, 16);
        GetWindowTextW(edit_month_, mbuf, 16);
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
            payload->working_img = auto_orient ? ocr_engine_.auto_orient_table_photo(raw_img, rot) : raw_img;
            payload->ok = ocr_engine_.process_workload_photo(
                payload->working_img, yr, mo, cfg_copy,
                has_manual ? &corners_copy : nullptr,
                payload->sheet, payload->err);
            payload->sheet.applied_rotation_deg = rot;
            PostMessageW(hwnd, WM_APP_WORKLOAD_DONE, 0, reinterpret_cast<LPARAM>(payload));
        }).detach();
    }

    bool run_workload_ocr_sync(const ImageRgb& raw_img,
                               bool auto_orient,
                               const std::array<Point2f, 4>* manual_c,
                               std::string& out_err) {
        if (!ocr_engine_.is_ready()) {
            out_err = model_init_err_;
            return false;
        }
        wchar_t ybuf[16]{}, mbuf[16]{};
        GetWindowTextW(edit_year_, ybuf, 16);
        GetWindowTextW(edit_month_, mbuf, 16);
        int yr = std::max(2000, std::min(2099, _wtoi(ybuf)));
        int mo = std::max(1, std::min(12, _wtoi(mbuf)));

        int rot = 0;
        ImageRgb work = auto_orient ? ocr_engine_.auto_orient_table_photo(raw_img, rot) : raw_img;
        WorkloadSheet recognized;
        if (!ocr_engine_.process_workload_photo(work, yr, mo, config, manual_c, recognized, out_err)) {
            return false;
        }
        recognized.applied_rotation_deg = rot;
        apply_workload_result(std::move(work), std::move(recognized));
        return true;
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

    void apply_imported_names(bool prefer_workload, const std::vector<std::string>& names) {
        if (names.empty()) return;
        if (prefer_workload && !sheet_.rows.empty()) {
            apply_name_list_to_workload(sheet_, names);
            recalc_and_refresh_workload();
            return;
        }
        apply_name_list_to_staff(config.staff, names);
        save_config_quiet();
        refresh_staff_grid();
        if (!sheet_.rows.empty()) {
            apply_name_list_to_workload(sheet_, names);
            recalc_and_refresh_workload();
        }
        std::wstring msg = L"已导入并匹配 " + std::to_wstring(names.size()) + L" 人名单";
        SetWindowTextW(lbl_staff_hint, msg.c_str());
    }

    void import_names_into_staff(const std::string& path) {
        std::vector<std::string> names;
        std::string err;
        if (!read_names_from_file(path, names, err)) {
            MessageBoxW(hwnd, utf8_to_wide(err).c_str(), L"导入名单失败", MB_OK | MB_ICONERROR);
            return;
        }
        apply_imported_names(false, names);
    }

    void import_names_into_workload(const std::string& path) {
        std::vector<std::string> names;
        std::string err;
        if (!read_names_from_file(path, names, err)) {
            MessageBoxW(hwnd, utf8_to_wide(err).c_str(), L"导入名单失败", MB_OK | MB_ICONERROR);
            return;
        }
        apply_imported_names(true, names);
    }

    void open_name_import_modal(bool prefer_workload) {
        std::vector<std::string> names;
        if (show_name_import_dialog(hwnd, inst, dpi_, ui_font, ui_bold_font, names)) {
            apply_imported_names(prefer_workload, names);
        }
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
        sh(btn_wl_add_row, t2);
        sh(btn_wl_del_row, t2);
        sh(btn_export_xlsx, t2);
        sh(edit_year_, t2);
        sh(lbl_year, t2);
        sh(edit_month_, t2);
        sh(lbl_month, t2);
        sh(lbl_status, t2);
        sh(lbl_quick_info, t2);
        for (HWND b : quick_btns) sh(b, t2);
        photo_canvas.show(t2);
        grid_workload.show(t2);
        InvalidateRect(hwnd, nullptr, TRUE);
    }

    void layout(int cw, int ch) {
        if (cw <= 0 || ch <= 0) return;
        const int m = S(10);
        const int tab_h = S(36);
        MoveWindow(tab_ctrl, m, m, std::max(S(200), cw - 2 * m), tab_h, TRUE);
        int top = m + tab_h + S(8);
        int body_h = std::max(S(140), ch - top - m);

        // Tab 0: Weights
        MoveWindow(chk_title_coeff, m, top + S(4), S(144), S(28), TRUE);
        MoveWindow(chk_duty_coeff, m + S(152), top + S(4), S(144), S(28), TRUE);
        MoveWindow(btn_add_shift, m + S(310), top, S(100), S(32), TRUE);
        MoveWindow(btn_del_shift, m + S(418), top, S(100), S(32), TRUE);
        MoveWindow(btn_reset_shift, m + S(526), top, S(100), S(32), TRUE);
        grid_weights.move(m, top + S(42), std::max(S(200), cw - 2 * m), std::max(S(80), body_h - S(42)));

        // Tab 1: Staff
        MoveWindow(btn_scan_coeff, m, top, S(132), S(32), TRUE);
        MoveWindow(btn_import_names, m + S(140), top, S(104), S(32), TRUE);
        MoveWindow(btn_add_staff, m + S(252), top, S(100), S(32), TRUE);
        MoveWindow(btn_del_staff, m + S(360), top, S(100), S(32), TRUE);
        MoveWindow(btn_clear_staff, m + S(468), top, S(80), S(32), TRUE);
        int hint_x = m + S(558);
        MoveWindow(lbl_staff_hint, hint_x, top + S(6), std::max(S(120), cw - m - hint_x), S(24), TRUE);
        grid_staff.move(m, top + S(42), std::max(S(200), cw - 2 * m), std::max(S(80), body_h - S(42)));

        // Tab 2: Workload (auto-wrapping toolbar)
        int cur_x = m;
        int cur_y = top;
        int row_h = S(32);
        int gap = S(6);

        auto flow_put = [&](HWND h, int w_dp, int h_dp = 32, int dy_dp = 0, bool keep_with_prev = false) {
            int w = S(w_dp);
            int hgt = S(h_dp);
            int dy = S(dy_dp);
            if (!keep_with_prev && cur_x > m && cur_x + w > cw - m) {
                cur_x = m;
                cur_y += row_h + S(6);
            }
            MoveWindow(h, cur_x, cur_y + dy, w, hgt, TRUE);
            cur_x += w + gap;
        };

        flow_put(btn_open_wl, 96);
        flow_put(btn_rot_left, 74);
        flow_put(btn_rot_right, 74);
        flow_put(btn_edit_corners, 92);
        flow_put(btn_next_review, 122);
        flow_put(btn_import_names_wl, 96);
        flow_put(btn_wl_add_row, 78);
        flow_put(btn_wl_del_row, 104);
        flow_put(edit_year_, 56, 28, 2);
        flow_put(lbl_year, 22, 24, 5, true);
        flow_put(edit_month_, 40, 28, 2, true);
        flow_put(lbl_month, 22, 24, 5, true);
        flow_put(btn_export_xlsx, 98);

        int status_min_w = S(260);
        int status_h = S(24);
        if (cw - m - cur_x >= status_min_w) {
            MoveWindow(lbl_status, cur_x + S(4), cur_y + S(5), cw - m - cur_x - S(4), status_h, TRUE);
        } else {
            cur_y += row_h + S(6);
            MoveWindow(lbl_status, m, cur_y + S(2), std::max(S(160), cw - 2 * m), status_h, TRUE);
            row_h = status_h;
        }

        int split_y = cur_y + row_h + S(8);
        int split_h = std::max(S(140), ch - split_y - m);
        int left_w = std::clamp(static_cast<int>((cw - 3 * m) * 0.45),
                                S(420), std::max(S(420), cw - 3 * m - S(340)));
        int right_x = m + left_w + m;
        int right_w = std::max(S(260), cw - right_x - m);

        int preview_w = S(150);
        int preview_h = S(68);
        int quick_h = preview_h + S(6);
        int canvas_h = std::max(S(120), split_h - quick_h - S(8));
        photo_canvas.move(m, split_y, left_w, canvas_h);

        int qy = split_y + canvas_h + S(8);
        preview_box_ = {m, qy, m + preview_w, qy + preview_h};

        int bx = m + preview_w + S(10);
        int avail_bw = std::max(S(200), left_w - preview_w - S(10));
        MoveWindow(lbl_quick_info, bx, qy, avail_bw, S(24), TRUE);

        int by = qy + S(28);
        int qgap = S(5);
        int bw = std::max(S(32), (avail_bw - 7 * qgap) / 8);
        int bh = S(36);
        for (int i = 0; i < 8; ++i) {
            MoveWindow(quick_btns[i], bx + i * (bw + qgap), by, bw, bh, TRUE);
        }

        grid_workload.move(right_x, split_y, right_w, split_h);
    }

    void paint_preview(HDC hdc) {
        if (active_tab != 2) return;
        RECT box = preview_box_;
        if (box.right <= box.left || box.bottom <= box.top) return;

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

            int pad = std::max(2, S(2));
            int avail_w = std::max(1, static_cast<int>(box.right - box.left) - 2 * pad);
            int avail_h = std::max(1, static_cast<int>(box.bottom - box.top) - 2 * pad);
            double scale = std::min(static_cast<double>(avail_w) / iw,
                                    static_cast<double>(avail_h) / ih);
            int draw_w = std::max(1, static_cast<int>(std::round(iw * scale)));
            int draw_h = std::max(1, static_cast<int>(std::round(ih * scale)));
            int draw_x = box.left + pad + (avail_w - draw_w) / 2;
            int draw_y = box.top + pad + (avail_h - draw_h) / 2;

            SetStretchBltMode(hdc, HALFTONE);
            SetBrushOrgEx(hdc, 0, 0, nullptr);
            StretchDIBits(hdc, draw_x, draw_y, draw_w, draw_h,
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
        app->dpi_ = query_window_dpi(hwnd);
        app->rebuild_fonts();

        auto mk_ctrl = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int id) -> HWND {
            HWND h = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                                     0, 0, 10, 10, hwnd,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), app->inst, nullptr);
            SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(app->ui_font), TRUE);
            return h;
        };

        app->tab_ctrl = mk_ctrl(WC_TABCONTROLW, L"", TCS_TABS, ID_TAB);
        TabCtrl_SetPadding(app->tab_ctrl, app->S(12), app->S(6));
        TCITEMW ti{};
        ti.mask = TCIF_TEXT;
        ti.pszText = const_cast<wchar_t*>(L"1. 班次权重");
        TabCtrl_InsertItem(app->tab_ctrl, 0, &ti);
        ti.pszText = const_cast<wchar_t*>(L"2. 人员系数");
        TabCtrl_InsertItem(app->tab_ctrl, 1, &ti);
        ti.pszText = const_cast<wchar_t*>(L"3. 工作量识别");
        TabCtrl_InsertItem(app->tab_ctrl, 2, &ti);

        // Tab 0: Weights
        app->chk_title_coeff = mk_ctrl(L"BUTTON", L"启用职称系数", BS_AUTOCHECKBOX, ID_CHK_TITLE_COEFF);
        app->chk_duty_coeff = mk_ctrl(L"BUTTON", L"启用职务系数", BS_AUTOCHECKBOX, ID_CHK_DUTY_COEFF);
        app->btn_add_shift = mk_ctrl(L"BUTTON", L"新增班次", BS_PUSHBUTTON, ID_BTN_ADD_SHIFT);
        app->btn_del_shift = mk_ctrl(L"BUTTON", L"删除选中", BS_PUSHBUTTON, ID_BTN_DEL_SHIFT);
        app->btn_reset_shift = mk_ctrl(L"BUTTON", L"恢复默认", BS_PUSHBUTTON, ID_BTN_RESET_SHIFT);
        app->grid_weights.create(hwnd, app->inst, ID_GRID_WEIGHTS, 0, 0, 100, 100);

        app->grid_weights.on_checkbox_toggled = [app](int r, int c, bool checked) {
            if (c == 0 && r >= 0 && r < static_cast<int>(app->config.shift_rules.size())) {
                app->config.shift_rules[static_cast<size_t>(r)].enabled = checked;
                app->save_config_quiet();
                app->recalc_and_refresh_workload();
            }
        };
        app->grid_weights.on_cell_edited = [app](int r, int c, const std::wstring& txt) {
            if (r < 0 || r >= static_cast<int>(app->config.shift_rules.size())) return;
            if (c == 1) {
                std::string nm = wide_to_utf8(txt);
                if (!nm.empty()) app->config.shift_rules[static_cast<size_t>(r)].name = nm;
            } else if (c == 2) {
                double v = std::wcstod(txt.c_str(), nullptr);
                if (std::isfinite(v) && v >= 0.0) app->config.shift_rules[static_cast<size_t>(r)].weight = v;
            }
            app->save_config_quiet();
            app->refresh_weights_grid();
            app->recalc_and_refresh_workload();
        };

        // Tab 1: Staff
        app->btn_scan_coeff = mk_ctrl(L"BUTTON", L"读取系数照片", BS_PUSHBUTTON, ID_BTN_SCAN_COEFF);
        app->btn_import_names = mk_ctrl(L"BUTTON", L"导入名单", BS_PUSHBUTTON, ID_BTN_IMPORT_NAMES);
        app->btn_add_staff = mk_ctrl(L"BUTTON", L"新增人员", BS_PUSHBUTTON, ID_BTN_ADD_STAFF);
        app->btn_del_staff = mk_ctrl(L"BUTTON", L"删除选中", BS_PUSHBUTTON, ID_BTN_DEL_STAFF);
        app->btn_clear_staff = mk_ctrl(L"BUTTON", L"清空", BS_PUSHBUTTON, ID_BTN_CLEAR_STAFF);
        app->lbl_staff_hint = mk_ctrl(L"STATIC", L"可将名字文件直接拖入姓名列，或拖入系数表照片自动识别", SS_LEFT, ID_LBL_STAFF_HINT);
        app->grid_staff.create(hwnd, app->inst, ID_GRID_STAFF, 0, 0, 100, 100);

        app->grid_staff.on_cell_edited = [app](int r, int c, const std::wstring& txt) {
            if (r < 0 || r >= static_cast<int>(app->config.staff.size())) return;
            auto& st = app->config.staff[static_cast<size_t>(r)];
            if (c == 1) {
                st.name = normalize_person_name(wide_to_utf8(txt));
                if (st.name.empty()) st.name = wide_to_utf8(txt);
            } else if (c == 2) {
                st.title_coeff = std::wcstod(txt.c_str(), nullptr);
            } else if (c == 3) {
                st.duty_coeff = std::wcstod(txt.c_str(), nullptr);
            }
            app->save_config_quiet();
            app->refresh_staff_grid();
            app->recalc_and_refresh_workload();
        };
        app->grid_staff.on_files_dropped = [app](int, const std::vector<std::string>& paths) {
            for (const auto& p : paths) {
                if (is_names_doc_ext(p)) app->import_names_into_staff(p);
                else if (is_image_ext(p)) app->start_coeff_ocr_file(p);
            }
        };
        app->grid_staff.on_paste_text = [app](int, int, const std::wstring& pasted) {
            auto names = parse_names_from_plain_text(wide_to_utf8(pasted));
            if (!names.empty()) {
                app->apply_imported_names(false, names);
            }
        };

        // Tab 2: Workload
        app->btn_open_wl = mk_ctrl(L"BUTTON", L"选择照片", BS_PUSHBUTTON, ID_BTN_OPEN_WORKLOAD);
        app->btn_rot_left = mk_ctrl(L"BUTTON", L"向左转", BS_PUSHBUTTON, ID_BTN_ROT_LEFT);
        app->btn_rot_right = mk_ctrl(L"BUTTON", L"向右转", BS_PUSHBUTTON, ID_BTN_ROT_RIGHT);
        app->btn_edit_corners = mk_ctrl(L"BUTTON", L"调整四角", BS_PUSHBUTTON, ID_BTN_EDIT_CORNERS);
        app->btn_next_review = mk_ctrl(L"BUTTON", L"下一处待确认", BS_PUSHBUTTON, ID_BTN_NEXT_REVIEW);
        app->btn_import_names_wl = mk_ctrl(L"BUTTON", L"导入名单", BS_PUSHBUTTON, ID_BTN_IMPORT_NAMES_WL);
        app->btn_wl_add_row = mk_ctrl(L"BUTTON", L"新增行", BS_PUSHBUTTON, ID_BTN_WL_ADD_ROW);
        app->btn_wl_del_row = mk_ctrl(L"BUTTON", L"删除选中行", BS_PUSHBUTTON, ID_BTN_WL_DEL_ROW);
        app->edit_year_ = mk_ctrl(L"EDIT", std::to_wstring(app->sheet_.year).c_str(),
                                  WS_BORDER | ES_NUMBER | ES_CENTER, ID_EDIT_YEAR);
        app->lbl_year = mk_ctrl(L"STATIC", L"年", SS_CENTER, ID_LBL_YEAR);
        app->edit_month_ = mk_ctrl(L"EDIT", std::to_wstring(app->sheet_.month).c_str(),
                                   WS_BORDER | ES_NUMBER | ES_CENTER, ID_EDIT_MONTH);
        app->lbl_month = mk_ctrl(L"STATIC", L"月", SS_CENTER, ID_LBL_MONTH);
        app->btn_export_xlsx = mk_ctrl(L"BUTTON", L"导出表格", BS_PUSHBUTTON, ID_BTN_EXPORT_XLSX);
        app->lbl_status = mk_ctrl(L"STATIC", L"尚未载入工作量照片", SS_LEFT | SS_ENDELLIPSIS, ID_LBL_STATUS);
        app->lbl_quick_info = mk_ctrl(L"STATIC", L"点击左图或右表单元格，在此快捷核对",
                                      SS_LEFT | SS_ENDELLIPSIS, ID_LBL_QUICK_INFO);

        const wchar_t* qlabels[8] = {L"空白", L"1", L"2", L"3", L"4", L"5", L"0.5", L"确认"};
        for (int i = 0; i < 8; ++i) {
            app->quick_btns[i] = mk_ctrl(L"BUTTON", qlabels[i], BS_PUSHBUTTON, ID_BTN_Q_BLANK + i);
        }

        app->photo_canvas.create(hwnd, app->inst, ID_CANVAS_PHOTO, 0, 0, 100, 100);
        app->grid_workload.create(hwnd, app->inst, ID_GRID_WORKLOAD, 0, 0, 100, 100);

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
            if (r < 0 || r >= static_cast<int>(app->sheet_.rows.size())) return;
            int data_cols = app->sheet_.weeks * static_cast<int>(app->sheet_.column_shift_names.size());
            if (c == 0) {
                app->sel_wl_row = r;
                app->sel_wl_cell_col = -1;
            } else if (c >= 1 && c <= data_cols) {
                app->sel_wl_row = r;
                app->sel_wl_cell_col = c - 1;
            } else {
                app->sel_wl_row = r;
                app->sel_wl_cell_col = -1;
                app->update_quick_preview();
                return;
            }
            app->photo_canvas.select_highlight(app->sel_wl_row, app->sel_wl_cell_col, true);
            app->update_quick_preview();
        };
        app->grid_workload.on_cell_edited = [app](int r, int c, const std::wstring& txt) {
            if (r < 0 || r >= static_cast<int>(app->sheet_.rows.size())) return;
            auto& row_ref = app->sheet_.rows[static_cast<size_t>(r)];
            if (c == 0) {
                std::string nm = normalize_person_name(wide_to_utf8(txt));
                row_ref.staff_name = nm.empty() ? wide_to_utf8(txt) : nm;
                row_ref.name_needs_review = false;
                app->recalc_and_refresh_workload();
                return;
            }
            int cell_idx = c - 1;
            if (cell_idx < 0 || cell_idx >= static_cast<int>(row_ref.cells.size())) return;
            auto& cell = row_ref.cells[static_cast<size_t>(cell_idx)];
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
        app->grid_workload.on_files_dropped = [app](int, const std::vector<std::string>& paths) {
            for (const auto& p : paths) {
                if (is_names_doc_ext(p)) {
                    app->import_names_into_workload(p);
                } else if (is_image_ext(p)) {
                    app->start_workload_ocr_file(p);
                    break;
                }
            }
        };
        app->grid_workload.on_paste_text = [app](int, int, const std::wstring& pasted) {
            auto names = parse_names_from_plain_text(wide_to_utf8(pasted));
            if (!names.empty()) {
                app->apply_imported_names(true, names);
            }
        };
        app->grid_workload.on_context_menu = [app, hwnd](int r, int, POINT screen_pt) {
            HMENU menu = CreatePopupMenu();
            if (!menu) return;
            bool valid_row = (r >= 0 && r < static_cast<int>(app->sheet_.rows.size()));
            AppendMenuW(menu, MF_STRING, ID_CTX_WL_ADD_ROW, L"新增行");
            AppendMenuW(menu, MF_STRING | (valid_row ? MF_ENABLED : MF_GRAYED),
                        ID_CTX_WL_DEL_ROW, L"删除选中行");
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                     screen_pt.x, screen_pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (cmd == ID_CTX_WL_ADD_ROW) {
                app->add_workload_row();
            } else if (cmd == ID_CTX_WL_DEL_ROW && valid_row) {
                app->delete_workload_row(r);
            }
        };

        app->set_dpi(app->dpi_);
        app->refresh_weights_grid();
        app->refresh_staff_grid();
        app->recalc_and_refresh_workload();
        app->switch_tab(2);

        RECT crc{};
        GetClientRect(hwnd, &crc);
        app->layout(crc.right - crc.left, crc.bottom - crc.top);

        DragAcceptFiles(hwnd, TRUE);
        if (!app->ocr_engine_.is_ready()) {
            MessageBoxW(hwnd, utf8_to_wide("OCR 模型初始化失败: " + app->model_init_err_).c_str(),
                        L"提示", MB_OK | MB_ICONWARNING);
        }
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lp);
        mmi->ptMinTrackSize.x = app->S(920);
        mmi->ptMinTrackSize.y = app->S(620);
        return 0;
    }
    case kWmDpiChanged: {
        UINT new_dpi = HIWORD(wp);
        if (new_dpi < 96) new_dpi = LOWORD(wp);
        if (new_dpi < 96) new_dpi = 96;
        app->set_dpi(new_dpi);
        const RECT* suggested = reinterpret_cast<const RECT*>(lp);
        if (suggested) {
            SetWindowPos(hwnd, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        RECT rc{};
        GetClientRect(hwnd, &rc);
        app->layout(rc.right - rc.left, rc.bottom - rc.top);
        InvalidateRect(hwnd, nullptr, TRUE);
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
        int code = HIWORD(wp);
        if ((id == ID_EDIT_YEAR || id == ID_EDIT_MONTH) && code == EN_CHANGE) {
            if (!app->syncing_ym_) {
                wchar_t buf[16]{};
                GetWindowTextW(id == ID_EDIT_YEAR ? app->edit_year_ : app->edit_month_, buf, 16);
                int val = _wtoi(buf);
                if (id == ID_EDIT_YEAR && val >= 2000 && val <= 2099) app->sheet_.year = val;
                if (id == ID_EDIT_MONTH && val >= 1 && val <= 12) app->sheet_.month = val;
            }
            return 0;
        }
        if (code != BN_CLICKED && code != 1) return 0;

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
            std::string code_str = "S" + std::to_string(app->config.shift_rules.size() + 1);
            app->config.shift_rules.push_back({code_str, "新班次", 1.00, true});
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
        case ID_BTN_IMPORT_NAMES:
            app->open_name_import_modal(false);
            break;
        case ID_BTN_IMPORT_NAMES_WL:
            app->open_name_import_modal(true);
            break;
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
            app->jump_to_next_review(true);
            break;
        case ID_BTN_WL_ADD_ROW:
            app->add_workload_row();
            break;
        case ID_BTN_WL_DEL_ROW: {
            int r = app->grid_workload.selected_row();
            if (r < 0 || r >= static_cast<int>(app->sheet_.rows.size())) {
                r = app->sel_wl_row;
            }
            app->delete_workload_row(r);
            break;
        }
        case ID_BTN_Q_BLANK: app->apply_quick_value(ValueKind::Blank, 0.0, true); break;
        case ID_BTN_Q_1: app->apply_quick_value(ValueKind::Number, 1.0, true); break;
        case ID_BTN_Q_2: app->apply_quick_value(ValueKind::Number, 2.0, true); break;
        case ID_BTN_Q_3: app->apply_quick_value(ValueKind::Number, 3.0, true); break;
        case ID_BTN_Q_4: app->apply_quick_value(ValueKind::Number, 4.0, true); break;
        case ID_BTN_Q_5: app->apply_quick_value(ValueKind::Number, 5.0, true); break;
        case ID_BTN_Q_HALF: app->apply_quick_value(ValueKind::Number, 0.5, true); break;
        case ID_BTN_Q_CONFIRM: app->apply_quick_value(ValueKind::Blank, -1.0, true); break;
        case ID_BTN_EXPORT_XLSX: {
            if (app->sheet_.rows.empty()) {
                MessageBoxW(hwnd, L"请先识别或添加工作量数据。", L"提示", MB_OK | MB_ICONINFORMATION);
                break;
            }
            wchar_t ybuf[16]{}, mbuf[16]{};
            GetWindowTextW(app->edit_year_, ybuf, 16);
            GetWindowTextW(app->edit_month_, mbuf, 16);
            app->sheet_.year = std::max(2000, std::min(2099, _wtoi(ybuf)));
            app->sheet_.month = std::max(1, std::min(12, _wtoi(mbuf)));
            app->sheet_.week_labels = default_week_labels(app->sheet_.year, app->sheet_.month, app->sheet_.weeks);
            app->summary = evaluate_workload(app->sheet_, app->config);

            std::wstring out_w = save_xlsx_dialog(hwnd, app->sheet_.year, app->sheet_.month);
            if (!out_w.empty()) {
                ExportOptions opts;
                opts.include_summary_sheet = true;
                std::string err;
                if (export_workload_xlsx(wide_to_utf8(out_w), app->sheet_, app->config, app->summary, opts, err)) {
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
            app->apply_workload_result(std::move(payload->working_img), std::move(payload->sheet));
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
                if (app->active_tab == 2 && !app->sheet_.rows.empty()) app->import_names_into_workload(p);
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
        if (app->title_font) DeleteObject(app->title_font);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmd_line, int nCmdShow) {
    if (cmd_line && std::wcsstr(cmd_line, L"--self-test") != nullptr) {
        AppConfig cfg = create_default_config();
        OcrEngine ocr_engine_;
        std::string cache_dir;
        std::string err;
        bool ok = ensure_embedded_models_cached(cache_dir, err) &&
                  ocr_engine_.init_from_dir(cache_dir, err);
        return (cfg.staff.empty() && ok) ? 0 : 1;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // Dynamic DPI awareness (PerMonitorV2 on Win10 1703+, fallback SetProcessDPIAware on Win7+)
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (u32) {
        using FnSetDpiCtx = BOOL(WINAPI*)(HANDLE);
        auto fn_ctx = reinterpret_cast<FnSetDpiCtx>(
            reinterpret_cast<void*>(GetProcAddress(u32, "SetProcessDpiAwarenessContext")));
        bool ctx_ok = fn_ctx && fn_ctx(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4)));
        if (!ctx_ok) {
            using FnDpi = BOOL(WINAPI*)();
            auto fn = reinterpret_cast<FnDpi>(reinterpret_cast<void*>(GetProcAddress(u32, "SetProcessDPIAware")));
            if (fn) fn();
        }
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

    UINT init_dpi = query_window_dpi(nullptr);
    int win_w = MulDiv(1280, static_cast<int>(init_dpi), 96);
    int win_h = MulDiv(820, static_cast<int>(init_dpi), 96);
    RECT work_rc{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rc, 0)) {
        win_w = std::min(win_w, static_cast<int>(work_rc.right - work_rc.left));
        win_h = std::min(win_h, static_cast<int>(work_rc.bottom - work_rc.top));
    }

    HWND hwnd = CreateWindowExW(
        0,
        L"NurseOCReportMainWindow",
        L"护士工作量识别统计",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, win_w, win_h,
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
