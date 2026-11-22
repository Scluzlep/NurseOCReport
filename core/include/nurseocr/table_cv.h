#pragma once

#include "nurseocr/types.h"
#include <vector>

namespace nurseocr {

struct GrayImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> data; // 1 byte per pixel

    bool valid() const {
        return width > 0 && height > 0 &&
               data.size() == static_cast<size_t>(width) * static_cast<size_t>(height);
    }
};

struct Homography {
    double m[9]{1, 0, 0, 0, 1, 0, 0, 0, 1};

    Point2f apply(Point2f p) const;
    bool invert(Homography& out_inv) const;
};

struct RectifiedGrid {
    ImageRgb rectified_rgb;
    GrayImage rectified_gray;
    Homography h_orig_to_rect;
    Homography h_rect_to_orig;

    // Curve-aware intersections in rectified pixel coords:
    // size = (y_lines.size()) * (x_lines.size())
    std::vector<float> row_y_base;
    std::vector<float> col_x_base;
    // Strip-sampled curves for curved paper compensation
    int h_strips = 0;
    std::vector<float> strip_x_centers;
    std::vector<float> row_y_strips; // row_y_strips[r * h_strips + s]
    int v_strips = 0;
    std::vector<float> strip_y_centers;
    std::vector<float> col_x_strips; // col_x_strips[c * v_strips + s]

    Point2f rect_intersection(int r, int c) const;
    std::array<Point2f, 4> cell_orig_quad(int r, int c) const;
    ImageRgb crop_cell_rect(int r, int c, int pad_px = 0) const;
};

GrayImage rgb_to_gray(const ImageRgb& rgb);

// Estimates whether portrait image needs 90/270 deg rotation based on aspect ratio and line densities.
// Returns 0 or 1 (90 CW) or 3 (270 CW). Further text-orientation disambiguation can flip 180.
int estimate_rough_rotation_quarters(const ImageRgb& img);

bool solve_homography(const std::array<Point2f, 4>& src,
                      const std::array<Point2f, 4>& dst,
                      Homography& out_h);

// Automatically detects the outer 4 corners of the primary grid table in img.
bool detect_table_corners(const ImageRgb& img, std::array<Point2f, 4>& out_corners);

// Builds perspective-rectified image and fits curved grid lines across strips.
bool extract_rectified_table_grid(const ImageRgb& img,
                                  const std::array<Point2f, 4>& corners,
                                  int expected_min_cols,
                                  int expected_min_rows,
                                  RectifiedGrid& out_grid,
                                  std::string& out_err);

// Crops an upright cell image using bilinear sampling from arbitrary quadrilateral in src.
ImageRgb crop_quad_image(const ImageRgb& src,
                         const std::array<Point2f, 4>& quad,
                         int dst_w,
                         int dst_h);

// Erases horizontal printed underlines inside a cell RGB crop (used for coeff table).
ImageRgb erase_bottom_underline(const ImageRgb& cell_rgb);

struct CellInkAnalysis {
    InkCategory category = InkCategory::Empty;
    float max_diff = 0.0f;
    float rel_peak = 0.0f;
    int strong_px = 0;
    float ink_area_ratio = 0.0f;
    bool tall_thin_single_stroke = false;
    ImageRgb prepared_digit_canvas; // 96x48 dark ink on white background, centered
};

// Computes table-wide 90th-percentile ink reference contrast across candidate cells.
float compute_table_ink_reference(const RectifiedGrid& grid,
                                  int start_row,
                                  int end_row,
                                  int start_col,
                                  int end_col);

// Analyzes one rectified cell: local background subtraction, border & bleed removal,
// relative ink categorization, and centered padding into a 96x48 RGB image for rec.
CellInkAnalysis analyze_digit_cell(const RectifiedGrid& grid,
                                   int row,
                                   int col,
                                   float table_ink_ref);

// Checks if point p (in original image coords) lies inside convex quadrilateral quad.
bool point_in_quad(Point2f p, const std::array<Point2f, 4>& quad);

} // namespace nurseocr
