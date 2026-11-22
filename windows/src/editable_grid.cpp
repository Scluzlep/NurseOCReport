#include "editable_grid.h"

#include <commctrl.h>
#include <shellapi.h>
#include <algorithm>
#include <cstring>

namespace nurseocr_win {

namespace {

std::string wide_to_utf8_str(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, len)), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), &out[0], len, nullptr, nullptr);
    }
    return out;
}

COLORREF tone_bg_color(GridCellTone tone, int row) {
    switch (tone) {
    case GridCellTone::Header: return RGB(217, 234, 247);
    case GridCellTone::ReviewYellow: return RGB(255, 243, 205);
    case GridCellTone::ConfirmedGreen: return RGB(232, 245, 233);
    case GridCellTone::FormulaTint: return RGB(226, 240, 217);
    case GridCellTone::TotalRow: return RGB(231, 230, 230);
    case GridCellTone::DropTargetHint: return RGB(240, 247, 255);
    default: return (row % 2 == 1) ? RGB(248, 250, 253) : RGB(255, 255, 255);
    }
}

void register_grid_class(HINSTANCE inst) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = EditableGrid::wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"NurseEditableGridClass";
    RegisterClassExW(&wc);
    registered = true;
}

} // namespace

EditableGrid::EditableGrid() = default;
EditableGrid::~EditableGrid() {
    if (edit_hwnd_) DestroyWindow(edit_hwnd_);
    if (hwnd_) DestroyWindow(hwnd_);
}

bool EditableGrid::create(HWND parent, HINSTANCE inst, int id, int x, int y, int w, int h) {
    register_grid_class(inst);
    hwnd_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"NurseEditableGridClass",
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_HSCROLL | WS_VSCROLL,
        x, y, w, h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        inst,
        this);
    if (!hwnd_) return false;
    DragAcceptFiles(hwnd_, TRUE);
    return true;
}

void EditableGrid::move(int x, int y, int w, int h) {
    if (hwnd_) {
        MoveWindow(hwnd_, x, y, w, h, TRUE);
        update_scrollbars();
    }
}

void EditableGrid::show(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
}

void EditableGrid::set_font(HFONT font, HFONT bold_font) {
    font_ = font;
    bold_font_ = bold_font ? bold_font : font;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void EditableGrid::set_columns(const std::vector<GridColumn>& cols, bool freeze_first_col) {
    commit_edit(false);
    cols_ = cols;
    freeze_first_col_ = freeze_first_col;
    scroll_x_ = 0;
    update_scrollbars();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void EditableGrid::set_rows(std::vector<std::vector<GridCell>> rows, int pinned_bottom_rows) {
    commit_edit(false);
    rows_ = std::move(rows);
    pinned_bottom_rows_ = pinned_bottom_rows;
    if (sel_row_ >= static_cast<int>(rows_.size())) sel_row_ = static_cast<int>(rows_.size()) - 1;
    if (sel_col_ >= static_cast<int>(cols_.size())) sel_col_ = static_cast<int>(cols_.size()) - 1;
    update_scrollbars();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void EditableGrid::update_cell(int row, int col, const GridCell& cell) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (col < 0 || col >= static_cast<int>(rows_[row].size())) return;
    rows_[row][col] = cell;
    if (hwnd_) {
        RECT r = cell_rect_in_client(row, col);
        InvalidateRect(hwnd_, &r, FALSE);
    }
}

void EditableGrid::set_name_drop_column(int col_idx) {
    name_drop_col_ = col_idx;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

const GridCell* EditableGrid::cell_at(int r, int c) const {
    if (r < 0 || r >= static_cast<int>(rows_.size())) return nullptr;
    if (c < 0 || c >= static_cast<int>(rows_[r].size())) return nullptr;
    return &rows_[r][c];
}

void EditableGrid::select_cell(int row, int col, bool ensure_visible) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (col < 0 || col >= static_cast<int>(cols_.size())) return;
    sel_row_ = row;
    sel_col_ = col;
    if (ensure_visible) ensure_cell_visible(row, col);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    if (on_selection_changed) on_selection_changed(sel_row_, sel_col_);
}

int EditableGrid::total_content_width() const {
    int sum = 0;
    for (const auto& c : cols_) sum += c.width;
    return sum;
}

void EditableGrid::update_scrollbars() {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;

    int visible_rows = std::max(1, (ch - header_h_) / row_h_);
    int max_scroll_y = std::max(0, static_cast<int>(rows_.size()) - visible_rows);
    scroll_y_ = std::max(0, std::min(scroll_y_, max_scroll_y));

    SCROLLINFO svi{};
    svi.cbSize = sizeof(svi);
    svi.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    svi.nMin = 0;
    svi.nMax = std::max(0, static_cast<int>(rows_.size()) - 1);
    svi.nPage = visible_rows;
    svi.nPos = scroll_y_;
    SetScrollInfo(hwnd_, SB_VERT, &svi, TRUE);

    int tot_w = total_content_width();
    int max_sx = std::max(0, tot_w - cw);
    scroll_x_ = std::max(0, std::min(scroll_x_, max_sx));

    SCROLLINFO shi{};
    shi.cbSize = sizeof(shi);
    shi.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    shi.nMin = 0;
    shi.nMax = std::max(0, tot_w - 1);
    shi.nPage = std::max(1, cw);
    shi.nPos = scroll_x_;
    SetScrollInfo(hwnd_, SB_HORZ, &shi, TRUE);
}

void EditableGrid::ensure_cell_visible(int row, int col) {
    if (!hwnd_) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    int visible_rows = std::max(1, (ch - header_h_) / row_h_);

    if (row < scroll_y_) {
        scroll_y_ = row;
    } else if (row >= scroll_y_ + visible_rows) {
        scroll_y_ = row - visible_rows + 1;
    }

    int frozen_w = (freeze_first_col_ && !cols_.empty()) ? cols_[0].width : 0;
    if (!(freeze_first_col_ && col == 0) && col >= 0 && col < static_cast<int>(cols_.size())) {
        int cx = 0;
        for (int c = (freeze_first_col_ ? 1 : 0); c < col; ++c) cx += cols_[c].width;
        int cell_w = cols_[col].width;
        int view_w = std::max(20, cw - frozen_w);
        if (cx < scroll_x_) {
            scroll_x_ = cx;
        } else if (cx + cell_w > scroll_x_ + view_w) {
            scroll_x_ = cx + cell_w - view_w;
        }
    }
    update_scrollbars();
}

RECT EditableGrid::cell_rect_in_client(int row, int col) const {
    RECT r{0, 0, 0, 0};
    if (col < 0 || col >= static_cast<int>(cols_.size())) return r;
    int y = header_h_ + (row - scroll_y_) * row_h_;
    int x = 0;
    if (freeze_first_col_ && col == 0) {
        x = 0;
    } else {
        int start_c = freeze_first_col_ ? 1 : 0;
        x = (freeze_first_col_ ? cols_[0].width : 0) - scroll_x_;
        for (int c = start_c; c < col; ++c) x += cols_[c].width;
    }
    r.left = x;
    r.top = y;
    r.right = x + cols_[col].width;
    r.bottom = y + row_h_;
    return r;
}

bool EditableGrid::hit_test(int x, int y, int& out_row, int& out_col) const {
    if (y < header_h_) return false;
    int r = scroll_y_ + (y - header_h_) / row_h_;
    if (r < 0 || r >= static_cast<int>(rows_.size())) return false;

    if (freeze_first_col_ && !cols_.empty() && x >= 0 && x < cols_[0].width) {
        out_row = r;
        out_col = 0;
        return true;
    }
    int start_c = freeze_first_col_ ? 1 : 0;
    int cur_x = (freeze_first_col_ && !cols_.empty() ? cols_[0].width : 0) - scroll_x_;
    for (int c = start_c; c < static_cast<int>(cols_.size()); ++c) {
        if (x >= cur_x && x < cur_x + cols_[c].width) {
            out_row = r;
            out_col = c;
            return true;
        }
        cur_x += cols_[c].width;
    }
    return false;
}

void EditableGrid::begin_edit(int row, int col, const std::wstring* initial_replace) {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return;
    if (col < 0 || col >= static_cast<int>(cols_.size())) return;
    if (row >= static_cast<int>(rows_.size()) - pinned_bottom_rows_) return;
    const auto& cell = rows_[row][col];
    if (!cell.editable || cell.kind == GridCellKind::ReadOnly || cell.kind == GridCellKind::Checkbox) {
        return;
    }

    commit_edit(false);
    ensure_cell_visible(row, col);
    RECT cr = cell_rect_in_client(row, col);

    edit_row_ = row;
    edit_col_ = col;
    std::wstring init_txt = initial_replace ? *initial_replace : cell.text;

    edit_hwnd_ = CreateWindowExW(
        0, L"EDIT", init_txt.c_str(),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_CENTER,
        cr.left + 1, cr.top + 1, std::max(24, static_cast<int>(cr.right - cr.left) - 2),
        std::max(20, static_cast<int>(cr.bottom - cr.top) - 2),
        hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (font_) SendMessageW(edit_hwnd_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    SetWindowSubclass(edit_hwnd_, edit_sub_proc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetFocus(edit_hwnd_);
    if (initial_replace) {
        SendMessageW(edit_hwnd_, EM_SETSEL, init_txt.size(), init_txt.size());
    } else {
        SendMessageW(edit_hwnd_, EM_SETSEL, 0, -1);
    }
}

void EditableGrid::commit_edit(bool accept) {
    if (!edit_hwnd_) return;
    HWND eh = edit_hwnd_;
    edit_hwnd_ = nullptr;
    int r = edit_row_;
    int c = edit_col_;
    edit_row_ = -1;
    edit_col_ = -1;

    if (accept && r >= 0 && r < static_cast<int>(rows_.size()) &&
        c >= 0 && c < static_cast<int>(rows_[r].size())) {
        int len = GetWindowTextLengthW(eh);
        std::wstring buf(static_cast<size_t>(len + 1), L'\0');
        GetWindowTextW(eh, &buf[0], len + 1);
        buf.resize(static_cast<size_t>(len));
        if (on_cell_edited) on_cell_edited(r, c, buf);
    }
    DestroyWindow(eh);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

LRESULT CALLBACK EditableGrid::edit_sub_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                             UINT_PTR, DWORD_PTR ref_data) {
    auto* self = reinterpret_cast<EditableGrid*>(ref_data);
    if (msg == WM_KEYDOWN) {
        if (wp == VK_RETURN) {
            self->commit_edit(true);
            if (self->sel_row_ + 1 < static_cast<int>(self->rows_.size()) - self->pinned_bottom_rows_) {
                self->select_cell(self->sel_row_ + 1, self->sel_col_);
            }
            SetFocus(self->hwnd_);
            return 0;
        } else if (wp == VK_TAB) {
            self->commit_edit(true);
            int next_c = self->sel_col_ + 1;
            int next_r = self->sel_row_;
            if (next_c >= static_cast<int>(self->cols_.size())) {
                next_c = 0;
                if (next_r + 1 < static_cast<int>(self->rows_.size()) - self->pinned_bottom_rows_) ++next_r;
            }
            self->select_cell(next_r, next_c);
            SetFocus(self->hwnd_);
            return 0;
        } else if (wp == VK_ESCAPE) {
            self->commit_edit(false);
            SetFocus(self->hwnd_);
            return 0;
        }
    } else if (msg == WM_KILLFOCUS) {
        self->commit_edit(true);
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void EditableGrid::paint(HDC hdc, const RECT& rc) {
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    HDC memdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
    HGDIOBJ old_bmp = SelectObject(memdc, bmp);

    HBRUSH bg_brush = CreateSolidBrush(RGB(255, 255, 255));
    FillRect(memdc, &rc, bg_brush);
    DeleteObject(bg_brush);

    SetBkMode(memdc, TRANSPARENT);
    HPEN grid_pen = CreatePen(PS_SOLID, 1, RGB(214, 220, 229));
    HPEN sel_pen = CreatePen(PS_SOLID, 2, RGB(30, 90, 168));
    HGDIOBJ old_pen = SelectObject(memdc, grid_pen);

    auto draw_cell_rect = [&](const RECT& cr, const std::wstring& txt, GridCellTone tone,
                              bool is_checkbox, bool checked, bool selected, bool bold, bool center) {
        RECT clipped = cr;
        if (clipped.right <= 0 || clipped.left >= cw || clipped.bottom <= 0 || clipped.top >= ch) return;

        HBRUSH br = CreateSolidBrush(tone_bg_color(tone, 0));
        FillRect(memdc, &cr, br);
        DeleteObject(br);

        SelectObject(memdc, grid_pen);
        MoveToEx(memdc, cr.left, cr.bottom - 1, nullptr);
        LineTo(memdc, cr.right - 1, cr.bottom - 1);
        LineTo(memdc, cr.right - 1, cr.top - 1);

        if (is_checkbox) {
            int box = 18;
            int bx = (cr.left + cr.right - box) / 2;
            int by = (cr.top + cr.bottom - box) / 2;
            RECT br_rc{bx, by, bx + box, by + box};
            HBRUSH cbr = CreateSolidBrush(checked ? RGB(30, 90, 168) : RGB(255, 255, 255));
            FillRect(memdc, &br_rc, cbr);
            DeleteObject(cbr);
            HPEN bpen = CreatePen(PS_SOLID, 1, checked ? RGB(30, 90, 168) : RGB(130, 140, 155));
            SelectObject(memdc, bpen);
            SelectObject(memdc, GetStockObject(NULL_BRUSH));
            Rectangle(memdc, br_rc.left, br_rc.top, br_rc.right, br_rc.bottom);
            if (checked) {
                HPEN wpen = CreatePen(PS_SOLID, 2, RGB(255, 255, 255));
                SelectObject(memdc, wpen);
                MoveToEx(memdc, bx + 4, by + 9, nullptr);
                LineTo(memdc, bx + 8, by + 13);
                LineTo(memdc, bx + 14, by + 5);
                SelectObject(memdc, grid_pen);
                DeleteObject(wpen);
            } else {
                SelectObject(memdc, grid_pen);
            }
            DeleteObject(bpen);
        } else if (!txt.empty()) {
            HFONT use_font = bold ? bold_font_ : font_;
            if (use_font) SelectObject(memdc, use_font);
            SetTextColor(memdc, (tone == GridCellTone::Header) ? RGB(20, 42, 74) : RGB(24, 30, 38));
            RECT tr = cr;
            tr.left += 5;
            tr.right -= 5;
            UINT fmt = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | (center ? DT_CENTER : DT_LEFT);
            DrawTextW(memdc, txt.c_str(), static_cast<int>(txt.size()), &tr, fmt);
        }

        if (selected) {
            SelectObject(memdc, sel_pen);
            SelectObject(memdc, GetStockObject(NULL_BRUSH));
            Rectangle(memdc, cr.left + 1, cr.top + 1, cr.right - 1, cr.bottom - 1);
        }
    };

    auto draw_col_range = [&](int c_start, int c_end, int x_start) {
        // Data rows
        int visible_rows = (ch - header_h_) / row_h_ + 2;
        int r_end = std::min(static_cast<int>(rows_.size()), scroll_y_ + visible_rows);
        for (int r = scroll_y_; r < r_end; ++r) {
            int y = header_h_ + (r - scroll_y_) * row_h_;
            int x = x_start;
            for (int c = c_start; c < c_end; ++c) {
                int w = cols_[c].width;
                if (x + w > 0 && x < cw) {
                    RECT cr{x, y, x + w, y + row_h_};
                    const auto& cell = (c < static_cast<int>(rows_[r].size())) ? rows_[r][c] : GridCell{};
                    GridCellTone t = cell.tone;
                    if (t == GridCellTone::Normal && c == name_drop_col_ && r < static_cast<int>(rows_.size()) - pinned_bottom_rows_) {
                        t = GridCellTone::DropTargetHint;
                    }
                    bool is_sel = (r == sel_row_ && c == sel_col_);
                    bool is_bold = (t == GridCellTone::TotalRow || (freeze_first_col_ && c == 0));
                    HBRUSH row_bg = CreateSolidBrush(
                        (t == GridCellTone::Normal) ? tone_bg_color(GridCellTone::Normal, r) : tone_bg_color(t, r));
                    FillRect(memdc, &cr, row_bg);
                    DeleteObject(row_bg);

                    // Draw contents on top without refilling background
                    SelectObject(memdc, grid_pen);
                    MoveToEx(memdc, cr.left, cr.bottom - 1, nullptr);
                    LineTo(memdc, cr.right - 1, cr.bottom - 1);
                    LineTo(memdc, cr.right - 1, cr.top - 1);

                    if (cell.kind == GridCellKind::Checkbox) {
                        draw_cell_rect(cr, L"", t, true, cell.checked, is_sel, false, true);
                    } else {
                        if (!cell.text.empty()) {
                            HFONT use_font = is_bold ? bold_font_ : font_;
                            if (use_font) SelectObject(memdc, use_font);
                            SetTextColor(memdc, RGB(24, 30, 38));
                            RECT tr = cr;
                            tr.left += 5;
                            tr.right -= 5;
                            UINT fmt = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                                       (cols_[c].center_align ? DT_CENTER : DT_LEFT);
                            DrawTextW(memdc, cell.text.c_str(), static_cast<int>(cell.text.size()), &tr, fmt);
                        }
                        if (is_sel) {
                            SelectObject(memdc, sel_pen);
                            SelectObject(memdc, GetStockObject(NULL_BRUSH));
                            Rectangle(memdc, cr.left + 1, cr.top + 1, cr.right - 1, cr.bottom - 1);
                        }
                    }
                }
                x += w;
            }
        }
        // Header row
        int x = x_start;
        for (int c = c_start; c < c_end; ++c) {
            int w = cols_[c].width;
            if (x + w > 0 && x < cw) {
                RECT cr{x, 0, x + w, header_h_};
                draw_cell_rect(cr, cols_[c].title, GridCellTone::Header, false, false, false, true, true);
            }
            x += w;
        }
    };

    if (!cols_.empty()) {
        int start_c = freeze_first_col_ ? 1 : 0;
        int start_x = (freeze_first_col_ ? cols_[0].width : 0) - scroll_x_;
        if (start_c < static_cast<int>(cols_.size())) {
            draw_col_range(start_c, static_cast<int>(cols_.size()), start_x);
        }
        if (freeze_first_col_) {
            draw_col_range(0, 1, 0);
        }
    }

    SelectObject(memdc, old_pen);
    DeleteObject(grid_pen);
    DeleteObject(sel_pen);

    BitBlt(hdc, 0, 0, cw, ch, memdc, 0, 0, SRCCOPY);
    SelectObject(memdc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(memdc);
}

LRESULT CALLBACK EditableGrid::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    EditableGrid* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<EditableGrid*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<EditableGrid*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handle_msg(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT EditableGrid::handle_msg(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        paint(hdc, rc);
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        update_scrollbars();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_VSCROLL: {
        commit_edit(true);
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd_, SB_VERT, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINEUP: pos -= 1; break;
        case SB_LINEDOWN: pos += 1; break;
        case SB_PAGEUP: pos -= std::max(1, static_cast<int>(si.nPage)); break;
        case SB_PAGEDOWN: pos += std::max(1, static_cast<int>(si.nPage)); break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        }
        scroll_y_ = std::max(0, pos);
        update_scrollbars();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_HSCROLL: {
        commit_edit(true);
        SCROLLINFO si{sizeof(si), SIF_ALL};
        GetScrollInfo(hwnd_, SB_HORZ, &si);
        int pos = si.nPos;
        switch (LOWORD(wp)) {
        case SB_LINELEFT: pos -= 40; break;
        case SB_LINERIGHT: pos += 40; break;
        case SB_PAGELEFT: pos -= std::max(40, static_cast<int>(si.nPage)); break;
        case SB_PAGERIGHT: pos += std::max(40, static_cast<int>(si.nPage)); break;
        case SB_THUMBTRACK: pos = si.nTrackPos; break;
        }
        scroll_x_ = std::max(0, pos);
        update_scrollbars();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (GetKeyState(VK_SHIFT) & 0x8000) {
            scroll_x_ = std::max(0, scroll_x_ - (delta / 120) * 60);
        } else {
            scroll_y_ = std::max(0, scroll_y_ - (delta / 120) * 3);
        }
        update_scrollbars();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(hwnd_);
        commit_edit(true);
        int x = static_cast<short>(LOWORD(lp));
        int y = static_cast<short>(HIWORD(lp));
        int r = -1, c = -1;
        if (hit_test(x, y, r, c)) {
            select_cell(r, c, false);
            if (r < static_cast<int>(rows_.size()) - pinned_bottom_rows_ &&
                rows_[r][c].kind == GridCellKind::Checkbox && rows_[r][c].editable) {
                bool next_v = !rows_[r][c].checked;
                rows_[r][c].checked = next_v;
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (on_checkbox_toggled) on_checkbox_toggled(r, c, next_v);
            }
        }
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        int x = static_cast<short>(LOWORD(lp));
        int y = static_cast<short>(HIWORD(lp));
        int r = -1, c = -1;
        if (hit_test(x, y, r, c)) {
            select_cell(r, c, false);
            begin_edit(r, c);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        if (wp == 'V' && (GetKeyState(VK_CONTROL) & 0x8000)) {
            if (OpenClipboard(hwnd_)) {
                HANDLE h = GetClipboardData(CF_UNICODETEXT);
                if (h) {
                    const wchar_t* txt = static_cast<const wchar_t*>(GlobalLock(h));
                    if (txt) {
                        std::wstring ws(txt);
                        GlobalUnlock(h);
                        if (on_paste_text) on_paste_text(sel_row_, sel_col_, ws);
                    }
                }
                CloseClipboard();
            }
            return 0;
        }
        if (rows_.empty() || cols_.empty()) return 0;
        if (wp == VK_UP) select_cell(std::max(0, sel_row_ - 1), std::max(0, sel_col_));
        else if (wp == VK_DOWN) select_cell(std::min(static_cast<int>(rows_.size()) - 1, sel_row_ + 1), std::max(0, sel_col_));
        else if (wp == VK_LEFT) select_cell(std::max(0, sel_row_), std::max(0, sel_col_ - 1));
        else if (wp == VK_RIGHT) select_cell(std::max(0, sel_row_), std::min(static_cast<int>(cols_.size()) - 1, sel_col_ + 1));
        else if (wp == VK_F2 || wp == VK_RETURN) {
            if (sel_row_ >= 0 && sel_col_ >= 0) begin_edit(sel_row_, sel_col_);
        }
        return 0;
    }
    case WM_CHAR: {
        if (wp >= 0x20 && sel_row_ >= 0 && sel_col_ >= 0) {
            std::wstring init(1, static_cast<wchar_t>(wp));
            begin_edit(sel_row_, sel_col_, &init);
        }
        return 0;
    }
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        POINT pt{};
        DragQueryPoint(drop, &pt);
        int hit_r = -1, hit_c = (name_drop_col_ >= 0 ? name_drop_col_ : 0);
        if (!hit_test(pt.x, pt.y, hit_r, hit_c)) {
            // Check horizontal column under cursor even if dropped in header/empty space
            int start_x = (freeze_first_col_ && !cols_.empty()) ? cols_[0].width : 0;
            if (freeze_first_col_ && !cols_.empty() && pt.x < cols_[0].width) {
                hit_c = 0;
            } else {
                int cur = start_x - scroll_x_;
                for (int c = (freeze_first_col_ ? 1 : 0); c < static_cast<int>(cols_.size()); ++c) {
                    if (pt.x >= cur && pt.x < cur + cols_[c].width) {
                        hit_c = c;
                        break;
                    }
                    cur += cols_[c].width;
                }
            }
        }
        UINT cnt = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::string> paths;
        for (UINT i = 0; i < cnt; ++i) {
            UINT len = DragQueryFileW(drop, i, nullptr, 0);
            std::wstring wpth(len + 1, L'\0');
            DragQueryFileW(drop, i, &wpth[0], len + 1);
            wpth.resize(len);
            paths.push_back(wide_to_utf8_str(wpth));
        }
        DragFinish(drop);
        if (!paths.empty() && on_files_dropped) {
            on_files_dropped(hit_c, paths);
        }
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace nurseocr_win
