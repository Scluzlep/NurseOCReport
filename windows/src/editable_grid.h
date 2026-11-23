#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <functional>
#include <string>
#include <vector>

namespace nurseocr_win {

enum class GridCellKind {
    Text = 0,
    Checkbox = 1,
    ReadOnly = 2
};

enum class GridCellTone {
    Normal = 0,
    Header = 1,
    ReviewYellow = 2,
    ConfirmedGreen = 3,
    FormulaTint = 4,
    TotalRow = 5,
    DropTargetHint = 6
};

struct GridCell {
    std::wstring text;
    GridCellKind kind = GridCellKind::Text;
    GridCellTone tone = GridCellTone::Normal;
    bool checked = false;
    bool editable = true;
};

struct GridColumn {
    std::wstring title;
    int width = 100; // Logical 96-DPI pixel width
    GridCellKind default_kind = GridCellKind::Text;
    bool editable = true;
    bool center_align = true;
};

class EditableGrid {
public:
    EditableGrid();
    ~EditableGrid();

    bool create(HWND parent, HINSTANCE inst, int id, int x, int y, int w, int h);
    HWND hwnd() const { return hwnd_; }
    void move(int x, int y, int w, int h);
    void show(bool visible);

    void set_dpi(UINT dpi);
    void set_font(HFONT font, HFONT bold_font);
    void set_columns(const std::vector<GridColumn>& cols, bool freeze_first_col);
    void set_rows(std::vector<std::vector<GridCell>> rows, int pinned_bottom_rows = 0);
    void update_cell(int row, int col, const GridCell& cell);

    void select_cell(int row, int col, bool ensure_visible = true);
    void begin_edit(int row, int col, const std::wstring* initial_replace = nullptr);
    void commit_edit(bool accept);

    int selected_row() const { return sel_row_; }
    int selected_col() const { return sel_col_; }
    int row_count() const { return static_cast<int>(rows_.size()); }
    int col_count() const { return static_cast<int>(cols_.size()); }
    const GridCell* cell_at(int r, int c) const;

    void set_name_drop_column(int col_idx); // highlights column hint if >= 0

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

    // Callbacks
    std::function<void(int row, int col)> on_selection_changed;
    std::function<void(int row, int col, const std::wstring& new_text)> on_cell_edited;
    std::function<void(int row, int col, bool new_checked)> on_checkbox_toggled;
    std::function<void(int col, const std::vector<std::string>& utf8_paths)> on_files_dropped;
    std::function<void(int row, int col, const std::wstring& pasted_text)> on_paste_text;
    std::function<void(int row, int col, POINT screen_pt)> on_context_menu;

private:
    static LRESULT CALLBACK edit_sub_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                          UINT_PTR sub_id, DWORD_PTR ref_data);

    int S(int px) const { return MulDiv(px, static_cast<int>(dpi_), 96); }
    void recompute_metrics();
    void reposition_edit();
    LRESULT handle_msg(UINT msg, WPARAM wp, LPARAM lp);
    void paint(HDC hdc, const RECT& rc);
    void update_scrollbars();
    void ensure_cell_visible(int row, int col);
    bool hit_test(int x, int y, int& out_row, int& out_col) const;
    RECT cell_rect_in_client(int row, int col) const;
    int total_content_width() const;

    HWND hwnd_ = nullptr;
    HWND edit_hwnd_ = nullptr;
    HFONT font_ = nullptr;
    HFONT bold_font_ = nullptr;
    HBRUSH edit_bg_brush_ = nullptr;
    UINT dpi_ = 96;

    std::vector<GridColumn> cols_;
    std::vector<int> col_widths_;
    std::vector<std::vector<GridCell>> rows_;
    bool freeze_first_col_ = false;
    int pinned_bottom_rows_ = 0;
    int name_drop_col_ = -1;

    int header_h_ = 36;
    int row_h_ = 32;
    int scroll_x_ = 0;
    int scroll_y_ = 0; // in rows

    int sel_row_ = -1;
    int sel_col_ = -1;
    int edit_row_ = -1;
    int edit_col_ = -1;
};

} // namespace nurseocr_win
