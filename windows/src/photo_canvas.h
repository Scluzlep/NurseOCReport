#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "nurseocr/types.h"
#include <array>
#include <functional>
#include <string>
#include <vector>

namespace nurseocr_win {

struct HighlightBox {
    int row = 0;
    int col_index = -1; // -1 for name col, >= 0 for cell index in row.cells
    std::array<nurseocr::Point2f, 4> quad{};
    bool needs_review = false;
};

class PhotoCanvas {
public:
    PhotoCanvas();
    ~PhotoCanvas();

    bool create(HWND parent, HINSTANCE inst, int id, int x, int y, int w, int h);
    HWND hwnd() const { return hwnd_; }
    void move(int x, int y, int w, int h);
    void show(bool visible);

    void set_image(const nurseocr::ImageRgb& img);
    void set_overlays(std::vector<HighlightBox> boxes,
                      const std::array<nurseocr::Point2f, 4>& corners);
    void select_highlight(int row, int col_index, bool center_view = true);

    void set_corner_edit_mode(bool enabled);
    bool corner_edit_mode() const { return corner_edit_mode_; }
    std::array<nurseocr::Point2f, 4> corners() const { return corners_; }

    std::function<void(int row, int col_index)> on_cell_clicked;
    std::function<void(const std::vector<std::string>& utf8_paths)> on_files_dropped;

    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT handle_msg(UINT msg, WPARAM wp, LPARAM lp);
    void paint(HDC hdc, const RECT& rc);
    void fit_to_view();
    POINT img_to_screen(nurseocr::Point2f p) const;
    nurseocr::Point2f screen_to_img(int sx, int sy) const;

    HWND hwnd_ = nullptr;
    int img_w_ = 0;
    int img_h_ = 0;
    std::vector<uint8_t> dib_bgr_; // bottom-up or top-down BGR24 padded to 4-byte stride
    int dib_stride_ = 0;

    double zoom_ = 1.0;
    double pan_x_ = 0.0;
    double pan_y_ = 0.0;

    std::vector<HighlightBox> boxes_;
    std::array<nurseocr::Point2f, 4> corners_{};
    int sel_row_ = -1;
    int sel_col_index_ = -2;
    bool corner_edit_mode_ = false;

    bool dragging_pan_ = false;
    int dragging_corner_ = -1;
    POINT last_mouse_{};
    POINT down_mouse_{};
    bool moved_since_down_ = false;
};

} // namespace nurseocr_win
