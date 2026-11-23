#include "photo_canvas.h"
#include "nurseocr/table_cv.h"

#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace nurseocr_win {

namespace {

std::string wide_to_utf8_pc(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, len)), '\0');
    if (len > 0) {
        WideCharToMultiByte(CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), &out[0], len, nullptr, nullptr);
    }
    return out;
}

void register_canvas_class(HINSTANCE inst) {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PhotoCanvas::wnd_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_APPWORKSPACE + 1);
    wc.lpszClassName = L"NursePhotoCanvasClass";
    RegisterClassExW(&wc);
    registered = true;
}

} // namespace

PhotoCanvas::PhotoCanvas() = default;
PhotoCanvas::~PhotoCanvas() {
    if (hwnd_) DestroyWindow(hwnd_);
}

bool PhotoCanvas::create(HWND parent, HINSTANCE inst, int id, int x, int y, int w, int h) {
    register_canvas_class(inst);
    hwnd_ = CreateWindowExW(
        WS_EX_CLIENTEDGE,
        L"NursePhotoCanvasClass",
        L"",
        WS_CHILD | WS_VISIBLE,
        x, y, w, h,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        inst,
        this);
    if (!hwnd_) return false;
    DragAcceptFiles(hwnd_, TRUE);
    return true;
}

void PhotoCanvas::move(int x, int y, int w, int h) {
    if (hwnd_) {
        MoveWindow(hwnd_, x, y, w, h, TRUE);
        if (img_w_ > 0 && (!has_user_view_ || zoom_ <= 0.0)) {
            fit_to_view();
        }
    }
}

void PhotoCanvas::show(bool visible) {
    if (hwnd_) ShowWindow(hwnd_, visible ? SW_SHOW : SW_HIDE);
}

void PhotoCanvas::set_dpi(UINT dpi) {
    dpi_ = std::max(96u, dpi);
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhotoCanvas::set_font(HFONT font) {
    font_ = font;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhotoCanvas::set_image(const nurseocr::ImageRgb& img) {
    if (!img.valid()) {
        img_w_ = img_h_ = 0;
        dib_bgr_.clear();
        has_user_view_ = false;
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    img_w_ = img.width;
    img_h_ = img.height;
    dib_stride_ = ((img_w_ * 3 + 3) / 4) * 4;
    dib_bgr_.assign(static_cast<size_t>(dib_stride_) * img_h_, 255);
    for (int y = 0; y < img_h_; ++y) {
        const uint8_t* sp = &img.data[static_cast<size_t>(y) * img_w_ * 3];
        uint8_t* dp = &dib_bgr_[static_cast<size_t>(y) * dib_stride_];
        for (int x = 0; x < img_w_; ++x) {
            dp[x * 3 + 0] = sp[x * 3 + 2];
            dp[x * 3 + 1] = sp[x * 3 + 1];
            dp[x * 3 + 2] = sp[x * 3 + 0];
        }
    }
    has_user_view_ = false;
    fit_to_view();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhotoCanvas::set_overlays(std::vector<HighlightBox> boxes,
                               const std::array<nurseocr::Point2f, 4>& corners) {
    boxes_ = std::move(boxes);
    corners_ = corners;
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhotoCanvas::set_corner_edit_mode(bool enabled) {
    corner_edit_mode_ = enabled;
    if (enabled && img_w_ > 0) {
        has_user_view_ = false;
        fit_to_view();
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhotoCanvas::fit_to_view() {
    if (!hwnd_ || img_w_ <= 0 || img_h_ <= 0) return;
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    double pad = static_cast<double>(S(16));
    double vw = std::max(40.0, static_cast<double>(rc.right - rc.left));
    double vh = std::max(40.0, static_cast<double>(rc.bottom - rc.top));
    zoom_ = std::min((vw - pad) / img_w_, (vh - pad) / img_h_);
    zoom_ = std::max(0.05, std::min(4.0, zoom_));
    pan_x_ = (vw - img_w_ * zoom_) * 0.5;
    pan_y_ = (vh - img_h_ * zoom_) * 0.5;
}

POINT PhotoCanvas::img_to_screen(nurseocr::Point2f p) const {
    return {
        static_cast<LONG>(std::round(pan_x_ + p.x * zoom_)),
        static_cast<LONG>(std::round(pan_y_ + p.y * zoom_))
    };
}

nurseocr::Point2f PhotoCanvas::screen_to_img(int sx, int sy) const {
    double inv = (zoom_ > 1e-6) ? (1.0 / zoom_) : 1.0;
    return {
        static_cast<float>((sx - pan_x_) * inv),
        static_cast<float>((sy - pan_y_) * inv)
    };
}

void PhotoCanvas::focus_on_quad(const std::array<nurseocr::Point2f, 4>& quad) {
    if (!hwnd_ || img_w_ <= 0 || img_h_ <= 0) return;

    float min_x = quad[0].x, max_x = quad[0].x;
    float min_y = quad[0].y, max_y = quad[0].y;
    for (int k = 1; k < 4; ++k) {
        min_x = std::min(min_x, quad[k].x);
        max_x = std::max(max_x, quad[k].x);
        min_y = std::min(min_y, quad[k].y);
        max_y = std::max(max_y, quad[k].y);
    }
    float qw = max_x - min_x;
    float qh = max_y - min_y;
    if (qw < 1.0f || qh < 1.0f) return;

    RECT rc{};
    GetClientRect(hwnd_, &rc);
    double vw = std::max(60.0, static_cast<double>(rc.right - rc.left));
    double vh = std::max(60.0, static_cast<double>(rc.bottom - rc.top));

    double fit_zoom = std::min((vw - S(16)) / img_w_, (vh - S(16)) / img_h_);
    // Zoom so the target cell plus surrounding neighbor context fills the viewport comfortably.
    double target_w_zoom = (vw * 0.24) / std::max(12.0, static_cast<double>(qw));
    double target_h_zoom = (vh * 0.22) / std::max(12.0, static_cast<double>(qh));
    double desired = std::min(target_w_zoom, target_h_zoom);
    double min_focus = std::max(0.5, fit_zoom * 2.8);
    zoom_ = std::max(min_focus, std::min(8.0, desired));

    float cx = 0.25f * (quad[0].x + quad[1].x + quad[2].x + quad[3].x);
    float cy = 0.25f * (quad[0].y + quad[1].y + quad[2].y + quad[3].y);
    pan_x_ = vw * 0.5 - cx * zoom_;
    pan_y_ = vh * 0.5 - cy * zoom_;
    has_user_view_ = true;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhotoCanvas::select_highlight(int row, int col_index, bool center_view) {
    sel_row_ = row;
    sel_col_index_ = col_index;
    if (center_view && hwnd_ && img_w_ > 0) {
        for (const auto& b : boxes_) {
            if (b.row == row && b.col_index == col_index) {
                focus_on_quad(b.quad);
                return;
            }
        }
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void PhotoCanvas::paint(HDC hdc, const RECT& rc) {
    int cw = rc.right - rc.left;
    int ch = rc.bottom - rc.top;
    if (cw <= 0 || ch <= 0) return;

    HDC memdc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
    HGDIOBJ old_bmp = SelectObject(memdc, bmp);

    HBRUSH bg = CreateSolidBrush(RGB(238, 242, 247));
    FillRect(memdc, &rc, bg);
    DeleteObject(bg);

    if (img_w_ > 0 && img_h_ > 0 && !dib_bgr_.empty()) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = img_w_;
        bmi.bmiHeader.biHeight = -img_h_; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 24;
        bmi.bmiHeader.biCompression = BI_RGB;

        int dx = static_cast<int>(std::round(pan_x_));
        int dy = static_cast<int>(std::round(pan_y_));
        int dw = std::max(1, static_cast<int>(std::round(img_w_ * zoom_)));
        int dh = std::max(1, static_cast<int>(std::round(img_h_ * zoom_)));

        SetStretchBltMode(memdc, HALFTONE);
        SetBrushOrgEx(memdc, 0, 0, nullptr);
        StretchDIBits(memdc, dx, dy, dw, dh,
                      0, 0, img_w_, img_h_,
                      dib_bgr_.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);

        HGDIOBJ old_brush = SelectObject(memdc, GetStockObject(NULL_BRUSH));
        HPEN rev_pen = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(230, 130, 0));
        HPEN sel_pen = CreatePen(PS_SOLID, std::max(3, S(3)), RGB(0, 102, 224));
        HGDIOBJ old_pen = SelectObject(memdc, rev_pen);

        for (const auto& b : boxes_) {
            if (!b.needs_review) continue;
            POINT pts[4];
            for (int k = 0; k < 4; ++k) pts[k] = img_to_screen(b.quad[k]);
            Polygon(memdc, pts, 4);
        }

        SelectObject(memdc, sel_pen);
        for (const auto& b : boxes_) {
            if (b.row == sel_row_ && b.col_index == sel_col_index_) {
                POINT pts[4];
                for (int k = 0; k < 4; ++k) pts[k] = img_to_screen(b.quad[k]);
                Polygon(memdc, pts, 4);
                break;
            }
        }

        if (corner_edit_mode_) {
            HPEN cpen = CreatePen(PS_DASH, std::max(2, S(2)), RGB(220, 40, 40));
            SelectObject(memdc, cpen);
            POINT cpts[4];
            for (int k = 0; k < 4; ++k) cpts[k] = img_to_screen(corners_[k]);
            Polygon(memdc, cpts, 4);
            HBRUSH hbr = CreateSolidBrush(RGB(220, 40, 40));
            int hs = std::max(5, S(7));
            for (int k = 0; k < 4; ++k) {
                RECT hr{cpts[k].x - hs, cpts[k].y - hs, cpts[k].x + hs, cpts[k].y + hs};
                FillRect(memdc, &hr, hbr);
            }
            DeleteObject(hbr);
            DeleteObject(cpen);
        }

        SelectObject(memdc, old_pen);
        SelectObject(memdc, old_brush);
        DeleteObject(rev_pen);
        DeleteObject(sel_pen);
    } else {
        HGDIOBJ old_font = font_ ? SelectObject(memdc, font_) : nullptr;
        SetBkMode(memdc, TRANSPARENT);
        SetTextColor(memdc, RGB(105, 118, 135));
        RECT tr = rc;
        InflateRect(&tr, -S(16), -S(16));
        DrawTextW(memdc, L"请点击上方 选择照片 或将工作量照片直接拖入此处", -1, &tr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (old_font) SelectObject(memdc, old_font);
    }

    BitBlt(hdc, 0, 0, cw, ch, memdc, 0, 0, SRCCOPY);
    SelectObject(memdc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(memdc);
}

LRESULT CALLBACK PhotoCanvas::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    PhotoCanvas* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = reinterpret_cast<PhotoCanvas*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    } else {
        self = reinterpret_cast<PhotoCanvas*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->handle_msg(msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PhotoCanvas::handle_msg(UINT msg, WPARAM wp, LPARAM lp) {
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
    case WM_MOUSEWHEEL: {
        if (img_w_ <= 0) return 0;
        POINT pt{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
        ScreenToClient(hwnd_, &pt);
        double factor = (GET_WHEEL_DELTA_WPARAM(wp) > 0) ? 1.18 : (1.0 / 1.18);
        double next_zoom = std::max(0.05, std::min(8.0, zoom_ * factor));
        double img_x = (pt.x - pan_x_) / zoom_;
        double img_y = (pt.y - pan_y_) / zoom_;
        zoom_ = next_zoom;
        pan_x_ = pt.x - img_x * zoom_;
        pan_y_ = pt.y - img_y * zoom_;
        has_user_view_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetCapture(hwnd_);
        int x = static_cast<short>(LOWORD(lp));
        int y = static_cast<short>(HIWORD(lp));
        last_mouse_ = down_mouse_ = {x, y};
        moved_since_down_ = false;
        dragging_corner_ = -1;
        int hit_rad = std::max(10, S(14));
        if (corner_edit_mode_ && img_w_ > 0) {
            for (int k = 0; k < 4; ++k) {
                POINT sp = img_to_screen(corners_[k]);
                if (std::abs(sp.x - x) <= hit_rad && std::abs(sp.y - y) <= hit_rad) {
                    dragging_corner_ = k;
                    break;
                }
            }
        }
        if (dragging_corner_ < 0) dragging_pan_ = true;
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = static_cast<short>(LOWORD(lp));
        int y = static_cast<short>(HIWORD(lp));
        int drag_thresh = std::max(3, S(4));
        if (dragging_corner_ >= 0) {
            auto ip = screen_to_img(x, y);
            ip.x = std::max(0.0f, std::min(static_cast<float>(img_w_ - 1), ip.x));
            ip.y = std::max(0.0f, std::min(static_cast<float>(img_h_ - 1), ip.y));
            corners_[dragging_corner_] = ip;
            moved_since_down_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        } else if (dragging_pan_) {
            int dx = x - last_mouse_.x;
            int dy = y - last_mouse_.y;
            if (std::abs(x - down_mouse_.x) > drag_thresh || std::abs(y - down_mouse_.y) > drag_thresh) {
                moved_since_down_ = true;
            }
            pan_x_ += dx;
            pan_y_ += dy;
            last_mouse_ = {x, y};
            has_user_view_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        ReleaseCapture();
        int x = static_cast<short>(LOWORD(lp));
        int y = static_cast<short>(HIWORD(lp));
        bool was_click = !moved_since_down_ && dragging_corner_ < 0;
        dragging_pan_ = false;
        dragging_corner_ = -1;
        if (was_click && img_w_ > 0) {
            nurseocr::Point2f ip = screen_to_img(x, y);
            for (const auto& b : boxes_) {
                if (nurseocr::point_in_quad(ip, b.quad)) {
                    select_highlight(b.row, b.col_index, false);
                    if (on_cell_clicked) on_cell_clicked(b.row, b.col_index);
                    break;
                }
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
            paths.push_back(wide_to_utf8_pc(wpth));
        }
        DragFinish(drop);
        if (!paths.empty() && on_files_dropped) on_files_dropped(paths);
        return 0;
    }
    }
    return DefWindowProcW(hwnd_, msg, wp, lp);
}

} // namespace nurseocr_win
