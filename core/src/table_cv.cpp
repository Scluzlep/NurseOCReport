#include "nurseocr/table_cv.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <queue>

namespace nurseocr {

namespace {

inline uint8_t clamp_u8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

GrayImage downsample_gray(const GrayImage& src, int target_max_side, float& out_scale) {
    int max_dim = std::max(src.width, src.height);
    if (max_dim <= target_max_side) {
        out_scale = 1.0f;
        return src;
    }
    out_scale = static_cast<float>(target_max_side) / static_cast<float>(max_dim);
    int dw = std::max(16, static_cast<int>(std::round(src.width * out_scale)));
    int dh = std::max(16, static_cast<int>(std::round(src.height * out_scale)));
    out_scale = static_cast<float>(dw) / static_cast<float>(src.width);

    GrayImage dst;
    dst.width = dw;
    dst.height = dh;
    dst.data.resize(static_cast<size_t>(dw) * static_cast<size_t>(dh));

    float sx_ratio = static_cast<float>(src.width) / static_cast<float>(dw);
    float sy_ratio = static_cast<float>(src.height) / static_cast<float>(dh);
    for (int y = 0; y < dh; ++y) {
        float fy = (y + 0.5f) * sy_ratio - 0.5f;
        int y0 = std::max(0, std::min(src.height - 1, static_cast<int>(std::floor(fy))));
        int y1 = std::min(src.height - 1, y0 + 1);
        float wy = std::max(0.0f, fy - y0);
        for (int x = 0; x < dw; ++x) {
            float fx = (x + 0.5f) * sx_ratio - 0.5f;
            int x0 = std::max(0, std::min(src.width - 1, static_cast<int>(std::floor(fx))));
            int x1 = std::min(src.width - 1, x0 + 1);
            float wx = std::max(0.0f, fx - x0);
            float v00 = src.data[static_cast<size_t>(y0) * src.width + x0];
            float v10 = src.data[static_cast<size_t>(y0) * src.width + x1];
            float v01 = src.data[static_cast<size_t>(y1) * src.width + x0];
            float v11 = src.data[static_cast<size_t>(y1) * src.width + x1];
            float v = (v00 * (1.0f - wx) + v10 * wx) * (1.0f - wy) +
                      (v01 * (1.0f - wx) + v11 * wx) * wy;
            dst.data[static_cast<size_t>(y) * dw + x] = clamp_u8(static_cast<int>(std::round(v)));
        }
    }
    return dst;
}

std::vector<int32_t> build_integral(const GrayImage& g) {
    int w = g.width, h = g.height;
    std::vector<int32_t> integ(static_cast<size_t>(w + 1) * static_cast<size_t>(h + 1), 0);
    for (int y = 0; y < h; ++y) {
        int32_t row_sum = 0;
        const uint8_t* src = &g.data[static_cast<size_t>(y) * w];
        int32_t* dst = &integ[static_cast<size_t>(y + 1) * (w + 1)];
        const int32_t* prev = &integ[static_cast<size_t>(y) * (w + 1)];
        for (int x = 0; x < w; ++x) {
            row_sum += src[x];
            dst[x + 1] = prev[x + 1] + row_sum;
        }
    }
    return integ;
}

inline float box_mean(const std::vector<int32_t>& integ, int w, int h, int x, int y, int rx, int ry) {
    int x0 = std::max(0, x - rx);
    int y0 = std::max(0, y - ry);
    int x1 = std::min(w - 1, x + rx);
    int y1 = std::min(h - 1, y + ry);
    int stride = w + 1;
    int32_t sum = integ[(y1 + 1) * stride + (x1 + 1)] -
                  integ[y0 * stride + (x1 + 1)] -
                  integ[(y1 + 1) * stride + x0] +
                  integ[y0 * stride + x0];
    int area = (x1 - x0 + 1) * (y1 - y0 + 1);
    return static_cast<float>(sum) / static_cast<float>(std::max(1, area));
}

bool solve_linear_system(int n, std::vector<double>& a, std::vector<double>& b, std::vector<double>& x) {
    x.assign(static_cast<size_t>(n), 0.0);
    for (int col = 0; col < n; ++col) {
        int piv = col;
        double best = std::fabs(a[col * n + col]);
        for (int r = col + 1; r < n; ++r) {
            double v = std::fabs(a[r * n + col]);
            if (v > best) {
                best = v;
                piv = r;
            }
        }
        if (best < 1e-12) return false;
        if (piv != col) {
            for (int c = col; c < n; ++c) std::swap(a[col * n + c], a[piv * n + c]);
            std::swap(b[col], b[piv]);
        }
        double inv_diag = 1.0 / a[col * n + col];
        for (int c = col; c < n; ++c) a[col * n + c] *= inv_diag;
        b[col] *= inv_diag;
        for (int r = 0; r < n; ++r) {
            if (r == col) continue;
            double f = a[r * n + col];
            if (std::fabs(f) < 1e-15) continue;
            for (int c = col; c < n; ++c) a[r * n + c] -= f * a[col * n + c];
            b[r] -= f * b[col];
        }
    }
    x = std::move(b);
    return true;
}

// Fits line ax + by + c = 0 (with a^2 + b^2 = 1) robustly via RANSAC + refit on inliers
bool fit_robust_line(const std::vector<Point2f>& pts, double& out_a, double& out_b, double& out_c, float tol = 4.0f) {
    if (pts.size() < 2) return false;
    int best_inliers = -1;
    double ba = 0, bb = 0, bc = 0;

    int n = static_cast<int>(pts.size());
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            double dx = pts[j].x - pts[i].x;
            double dy = pts[j].y - pts[i].y;
            double len = std::hypot(dx, dy);
            if (len < 8.0) continue;
            double a = -dy / len;
            double b = dx / len;
            double c = -(a * pts[i].x + b * pts[i].y);
            int cnt = 0;
            for (const auto& p : pts) {
                if (std::fabs(a * p.x + b * p.y + c) <= tol) ++cnt;
            }
            if (cnt > best_inliers) {
                best_inliers = cnt;
                ba = a; bb = b; bc = c;
            }
        }
    }
    if (best_inliers < 2) return false;

    // Refit least squares via covariance eigen-vector on inliers
    std::vector<Point2f> inliers;
    for (const auto& p : pts) {
        if (std::fabs(ba * p.x + bb * p.y + bc) <= tol) inliers.push_back(p);
    }
    double mx = 0, my = 0;
    for (const auto& p : inliers) { mx += p.x; my += p.y; }
    mx /= inliers.size();
    my /= inliers.size();
    double sxx = 0, sxy = 0, syy = 0;
    for (const auto& p : inliers) {
        double vx = p.x - mx, vy = p.y - my;
        sxx += vx * vx;
        sxy += vx * vy;
        syy += vy * vy;
    }
    double theta = 0.5 * std::atan2(2.0 * sxy, sxx - syy);
    // Tangent is (cos theta, sin theta), normal is (-sin theta, cos theta)
    out_a = -std::sin(theta);
    out_b = std::cos(theta);
    out_c = -(out_a * mx + out_b * my);
    return true;
}

bool intersect_lines(double a1, double b1, double c1,
                     double a2, double b2, double c2,
                     Point2f& out_pt) {
    double det = a1 * b2 - a2 * b1;
    if (std::fabs(det) < 1e-8) return false;
    out_pt.x = static_cast<float>((b1 * c2 - b2 * c1) / det);
    out_pt.y = static_cast<float>((c1 * a2 - c2 * a1) / det);
    return true;
}

std::vector<float> find_1d_peaks(const std::vector<float>& prof,
                                 int min_dist,
                                 float rel_thresh) {
    std::vector<float> peaks;
    int n = static_cast<int>(prof.size());
    if (n < 5) return peaks;

    float max_val = 0.0f;
    std::vector<float> nonzero;
    for (float v : prof) {
        max_val = std::max(max_val, v);
        if (v > 0.0f) nonzero.push_back(v);
    }
    if (max_val <= 1e-5f) return peaks;

    std::vector<float> sorted = prof;
    std::sort(sorted.begin(), sorted.end());
    float top_ref = sorted[static_cast<size_t>(std::min<int>(n - 1, static_cast<int>(n * 0.96)))];
    if (top_ref <= 1e-5f) top_ref = max_val;
    float thresh = top_ref * rel_thresh;

    struct Cand { int idx; float val; };
    std::vector<Cand> cands;
    for (int i = 1; i < n - 1; ++i) {
        if (prof[i] >= thresh && prof[i] >= prof[i - 1] && prof[i] >= prof[i + 1]) {
            cands.push_back({i, prof[i]});
        }
    }
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.val > b.val;
    });

    std::vector<int> chosen;
    for (const auto& c : cands) {
        bool ok = true;
        for (int p : chosen) {
            if (std::abs(p - c.idx) < min_dist) {
                ok = false;
                break;
            }
        }
        if (ok) chosen.push_back(c.idx);
    }
    std::sort(chosen.begin(), chosen.end());
    for (int idx : chosen) {
        // Parabolic subpixel refinement
        float y0 = prof[idx - 1], y1 = prof[idx], y2 = prof[idx + 1];
        float denom = (y0 - 2.0f * y1 + y2);
        float delta = (std::fabs(denom) > 1e-5f) ? 0.5f * (y0 - y2) / denom : 0.0f;
        delta = std::max(-0.5f, std::min(0.5f, delta));
        peaks.push_back(static_cast<float>(idx) + delta);
    }
    return peaks;
}

float interp_piecewise(const std::vector<float>& xs, const std::vector<float>& ys, float x) {
    int n = static_cast<int>(xs.size());
    if (n == 0) return 0.0f;
    if (n == 1 || x <= xs.front()) return ys.front();
    if (x >= xs.back()) return ys.back();
    for (int i = 0; i < n - 1; ++i) {
        if (x >= xs[i] && x <= xs[i + 1]) {
            float span = xs[i + 1] - xs[i];
            if (span < 1e-5f) return ys[i];
            float t = (x - xs[i]) / span;
            return ys[i] * (1.0f - t) + ys[i + 1] * t;
        }
    }
    return ys.back();
}

} // namespace

Point2f Homography::apply(Point2f p) const {
    double w = m[6] * p.x + m[7] * p.y + m[8];
    if (std::fabs(w) < 1e-9) return p;
    return {
        static_cast<float>((m[0] * p.x + m[1] * p.y + m[2]) / w),
        static_cast<float>((m[3] * p.x + m[4] * p.y + m[5]) / w)
    };
}

bool Homography::invert(Homography& out_inv) const {
    double a00 = m[0], a01 = m[1], a02 = m[2];
    double a10 = m[3], a11 = m[4], a12 = m[5];
    double a20 = m[6], a21 = m[7], a22 = m[8];

    double c00 = a11 * a22 - a12 * a21;
    double c01 = -(a10 * a22 - a12 * a20);
    double c02 = a10 * a21 - a11 * a20;
    double det = a00 * c00 + a01 * c01 + a02 * c02;
    if (std::fabs(det) < 1e-12) return false;
    double inv_det = 1.0 / det;

    out_inv.m[0] = c00 * inv_det;
    out_inv.m[1] = (a02 * a21 - a01 * a22) * inv_det;
    out_inv.m[2] = (a01 * a12 - a02 * a11) * inv_det;
    out_inv.m[3] = c01 * inv_det;
    out_inv.m[4] = (a00 * a22 - a02 * a20) * inv_det;
    out_inv.m[5] = (a02 * a10 - a00 * a12) * inv_det;
    out_inv.m[6] = c02 * inv_det;
    out_inv.m[7] = (a01 * a20 - a00 * a21) * inv_det;
    out_inv.m[8] = (a00 * a11 - a01 * a10) * inv_det;
    return true;
}

bool solve_homography(const std::array<Point2f, 4>& src,
                      const std::array<Point2f, 4>& dst,
                      Homography& out_h) {
    std::vector<double> a(64, 0.0);
    std::vector<double> b(8, 0.0);
    for (int i = 0; i < 4; ++i) {
        double sx = src[i].x, sy = src[i].y;
        double dx = dst[i].x, dy = dst[i].y;
        int r0 = 2 * i;
        int r1 = 2 * i + 1;
        a[r0 * 8 + 0] = sx; a[r0 * 8 + 1] = sy; a[r0 * 8 + 2] = 1.0;
        a[r0 * 8 + 6] = -dx * sx; a[r0 * 8 + 7] = -dx * sy;
        b[r0] = dx;

        a[r1 * 8 + 3] = sx; a[r1 * 8 + 4] = sy; a[r1 * 8 + 5] = 1.0;
        a[r1 * 8 + 6] = -dy * sx; a[r1 * 8 + 7] = -dy * sy;
        b[r1] = dy;
    }
    std::vector<double> sol;
    if (!solve_linear_system(8, a, b, sol)) return false;
    for (int i = 0; i < 8; ++i) out_h.m[i] = sol[i];
    out_h.m[8] = 1.0;
    return true;
}

GrayImage rgb_to_gray(const ImageRgb& rgb) {
    GrayImage g;
    if (!rgb.valid()) return g;
    g.width = rgb.width;
    g.height = rgb.height;
    g.data.resize(static_cast<size_t>(g.width) * static_cast<size_t>(g.height));
    const size_t n = g.data.size();
    for (size_t i = 0; i < n; ++i) {
        const uint8_t* p = &rgb.data[i * 3];
        g.data[i] = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
    }
    return g;
}

int estimate_rough_rotation_quarters(const ImageRgb& img) {
    if (!img.valid()) return 0;
    if (img.height > static_cast<int>(img.width * 1.12f)) {
        // Portrait image for a landscape table -> try 270 CW (90 CCW) first;
        // OCR orientation disambiguation can flip 180 if upside down.
        return 3;
    }
    return 0;
}

bool detect_table_corners(const ImageRgb& img, std::array<Point2f, 4>& out_corners) {
    if (!img.valid()) return false;
    GrayImage gray = rgb_to_gray(img);
    float scale = 1.0f;
    GrayImage ds = downsample_gray(gray, 720, scale);
    int w = ds.width, h = ds.height;
    if (w < 40 || h < 40) return false;

    auto integ = build_integral(ds);
    int rad = std::max(10, std::min(w, h) / 36);
    std::vector<uint8_t> bin(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float m = box_mean(integ, w, h, x, y, rad, rad);
            if (static_cast<float>(ds.data[static_cast<size_t>(y) * w + x]) < m - 9.5f) {
                bin[static_cast<size_t>(y) * w + x] = 1;
            }
        }
    }

    // Horizontal line pixels: local run >= 14 of 18 pixels
    int hk = std::max(14, w / 42);
    int vk = std::max(12, h / 32);
    std::vector<uint8_t> hmask(static_cast<size_t>(w) * h, 0);
    std::vector<uint8_t> vmask(static_cast<size_t>(w) * h, 0);

    for (int y = 0; y < h; ++y) {
        std::vector<int> psum(w + 1, 0);
        for (int x = 0; x < w; ++x) psum[x + 1] = psum[x] + bin[y * w + x];
        for (int x = 0; x < w; ++x) {
            int x0 = std::max(0, x - hk / 2);
            int x1 = std::min(w - 1, x + hk / 2);
            if (psum[x1 + 1] - psum[x0] >= static_cast<int>(0.72f * (x1 - x0 + 1)) && bin[y * w + x]) {
                hmask[y * w + x] = 1;
            }
        }
    }
    for (int x = 0; x < w; ++x) {
        std::vector<int> psum(h + 1, 0);
        for (int y = 0; y < h; ++y) psum[y + 1] = psum[y] + bin[y * w + x];
        for (int y = 0; y < h; ++y) {
            int y0 = std::max(0, y - vk / 2);
            int y1 = std::min(h - 1, y + vk / 2);
            if (psum[y1 + 1] - psum[y0] >= static_cast<int>(0.72f * (y1 - y0 + 1)) && bin[y * w + x]) {
                vmask[y * w + x] = 1;
            }
        }
    }

    // Dilate union by 3px radius so broken line segments merge into one grid component
    std::vector<uint8_t> grid(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!hmask[y * w + x] && !vmask[y * w + x]) continue;
            int y0 = std::max(0, y - 2), y1 = std::min(h - 1, y + 2);
            int x0 = std::max(0, x - 2), x1 = std::min(w - 1, x + 2);
            for (int yy = y0; yy <= y1; ++yy) {
                for (int xx = x0; xx <= x1; ++xx) {
                    grid[yy * w + xx] = 1;
                }
            }
        }
    }

    std::vector<int32_t> comp_id(static_cast<size_t>(w) * h, 0);
    int best_cid = 0;
    double best_score = -1.0;
    int best_x0 = 0, best_y0 = 0, best_x1 = w - 1, best_y1 = h - 1;
    int cur_cid = 0;
    std::vector<int> q;
    q.reserve(4096);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = static_cast<size_t>(y) * w + x;
            if (!grid[idx] || comp_id[idx] != 0) continue;
            ++cur_cid;
            comp_id[idx] = cur_cid;
            q.clear();
            q.push_back(static_cast<int>(idx));
            int minx = x, maxx = x, miny = y, maxy = y;
            int h_cnt = 0, v_cnt = 0;
            size_t head = 0;
            while (head < q.size()) {
                int p = q[head++];
                int py = p / w, px = p % w;
                minx = std::min(minx, px); maxx = std::max(maxx, px);
                miny = std::min(miny, py); maxy = std::max(maxy, py);
                if (hmask[p]) ++h_cnt;
                if (vmask[p]) ++v_cnt;
                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = py + dy;
                    if (ny < 0 || ny >= h) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = px + dx;
                        if (nx < 0 || nx >= w) continue;
                        size_t ni = static_cast<size_t>(ny) * w + nx;
                        if (grid[ni] && comp_id[ni] == 0) {
                            comp_id[ni] = cur_cid;
                            q.push_back(static_cast<int>(ni));
                        }
                    }
                }
            }
            int bw = maxx - minx + 1;
            int bh = maxy - miny + 1;
            if (bw >= static_cast<int>(w * 0.35f) && bh >= static_cast<int>(h * 0.30f) &&
                h_cnt >= bw * 2 && v_cnt >= bh * 2) {
                double score = static_cast<double>(bw) * bh;
                if (score > best_score) {
                    best_score = score;
                    best_cid = cur_cid;
                    best_x0 = minx; best_x1 = maxx;
                    best_y0 = miny; best_y1 = maxy;
                }
            }
        }
    }

    if (best_cid == 0) {
        // Fallback margins
        float mx = img.width * 0.05f, my = img.height * 0.08f;
        out_corners = {
            Point2f{mx, my},
            Point2f{img.width - mx, my},
            Point2f{img.width - mx, img.height - my},
            Point2f{mx, img.height - my}
        };
        return false;
    }

    // Refine outer boundary lines from the winning component's pure hmask/vmask pixels
    std::vector<Point2f> top_pts, bot_pts, left_pts, right_pts;
    int bw = best_x1 - best_x0 + 1;
    int bh = best_y1 - best_y0 + 1;

    const int slices = 24;
    for (int s = 1; s < slices; ++s) {
        int x = best_x0 + (bw * s) / slices;
        int top_y = -1, bot_y = -1;
        for (int y = best_y0; y <= best_y1; ++y) {
            size_t idx = static_cast<size_t>(y) * w + x;
            if (comp_id[idx] == best_cid && (hmask[idx] || vmask[idx])) {
                if (top_y < 0) top_y = y;
                bot_y = y;
            }
        }
        if (top_y >= 0) top_pts.push_back({static_cast<float>(x), static_cast<float>(top_y)});
        if (bot_y >= 0) bot_pts.push_back({static_cast<float>(x), static_cast<float>(bot_y)});
    }
    for (int s = 1; s < slices; ++s) {
        int y = best_y0 + (bh * s) / slices;
        int lx = -1, rx = -1;
        for (int x = best_x0; x <= best_x1; ++x) {
            size_t idx = static_cast<size_t>(y) * w + x;
            if (comp_id[idx] == best_cid && (hmask[idx] || vmask[idx])) {
                if (lx < 0) lx = x;
                rx = x;
            }
        }
        if (lx >= 0) left_pts.push_back({static_cast<float>(lx), static_cast<float>(y)});
        if (rx >= 0) right_pts.push_back({static_cast<float>(rx), static_cast<float>(y)});
    }

    double ta, tb, tc, ba, bb, bc, la, lb, lc, ra, rb, rc;
    Point2f tl{static_cast<float>(best_x0), static_cast<float>(best_y0)};
    Point2f tr{static_cast<float>(best_x1), static_cast<float>(best_y0)};
    Point2f br{static_cast<float>(best_x1), static_cast<float>(best_y1)};
    Point2f bl{static_cast<float>(best_x0), static_cast<float>(best_y1)};

    if (fit_robust_line(top_pts, ta, tb, tc, 5.5f) &&
        fit_robust_line(bot_pts, ba, bb, bc, 5.5f) &&
        fit_robust_line(left_pts, la, lb, lc, 5.5f) &&
        fit_robust_line(right_pts, ra, rb, rc, 5.5f)) {
        Point2f p_tl, p_tr, p_br, p_bl;
        if (intersect_lines(ta, tb, tc, la, lb, lc, p_tl) &&
            intersect_lines(ta, tb, tc, ra, rb, rc, p_tr) &&
            intersect_lines(ba, bb, bc, ra, rb, rc, p_br) &&
            intersect_lines(ba, bb, bc, la, lb, lc, p_bl)) {
            if (p_tl.x > -w * 0.2f && p_tr.x < w * 1.2f &&
                p_tl.y > -h * 0.2f && p_bl.y < h * 1.2f) {
                tl = p_tl; tr = p_tr; br = p_br; bl = p_bl;
            }
        }
    }

    float inv_scale = 1.0f / scale;
    auto clamp_pt = [&](Point2f p) -> Point2f {
        return {
            std::max(0.0f, std::min(static_cast<float>(img.width - 1), p.x * inv_scale)),
            std::max(0.0f, std::min(static_cast<float>(img.height - 1), p.y * inv_scale))
        };
    };
    out_corners = {clamp_pt(tl), clamp_pt(tr), clamp_pt(br), clamp_pt(bl)};
    return true;
}

ImageRgb crop_quad_image(const ImageRgb& src,
                         const std::array<Point2f, 4>& quad,
                         int dst_w,
                         int dst_h) {
    ImageRgb dst;
    if (!src.valid() || dst_w <= 0 || dst_h <= 0) return dst;
    dst.width = dst_w;
    dst.height = dst_h;
    dst.data.resize(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 3, 255);

    std::array<Point2f, 4> rect_pts = {
        Point2f{0.0f, 0.0f},
        Point2f{static_cast<float>(dst_w - 1), 0.0f},
        Point2f{static_cast<float>(dst_w - 1), static_cast<float>(dst_h - 1)},
        Point2f{0.0f, static_cast<float>(dst_h - 1)}
    };
    Homography h_inv;
    if (!solve_homography(rect_pts, quad, h_inv)) return dst;

    for (int y = 0; y < dst_h; ++y) {
        for (int x = 0; x < dst_w; ++x) {
            double denom = h_inv.m[6] * x + h_inv.m[7] * y + h_inv.m[8];
            if (std::fabs(denom) < 1e-8) continue;
            float sx = static_cast<float>((h_inv.m[0] * x + h_inv.m[1] * y + h_inv.m[2]) / denom);
            float sy = static_cast<float>((h_inv.m[3] * x + h_inv.m[4] * y + h_inv.m[5]) / denom);
            if (sx < 0.0f || sy < 0.0f || sx > src.width - 1.0f || sy > src.height - 1.0f) continue;
            int x0 = static_cast<int>(std::floor(sx));
            int y0 = static_cast<int>(std::floor(sy));
            int x1 = std::min(src.width - 1, x0 + 1);
            int y1 = std::min(src.height - 1, y0 + 1);
            float wx = sx - x0, wy = sy - y0;
            const uint8_t* p00 = &src.data[(static_cast<size_t>(y0) * src.width + x0) * 3];
            const uint8_t* p10 = &src.data[(static_cast<size_t>(y0) * src.width + x1) * 3];
            const uint8_t* p01 = &src.data[(static_cast<size_t>(y1) * src.width + x0) * 3];
            const uint8_t* p11 = &src.data[(static_cast<size_t>(y1) * src.width + x1) * 3];
            uint8_t* dp = &dst.data[(static_cast<size_t>(y) * dst_w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                float val = (p00[c] * (1.0f - wx) + p10[c] * wx) * (1.0f - wy) +
                            (p01[c] * (1.0f - wx) + p11[c] * wx) * wy;
                dp[c] = clamp_u8(static_cast<int>(std::round(val)));
            }
        }
    }
    return dst;
}

Point2f RectifiedGrid::rect_intersection(int r, int c) const {
    if (r < 0 || r >= static_cast<int>(row_y_base.size()) ||
        c < 0 || c >= static_cast<int>(col_x_base.size())) {
        return {0.0f, 0.0f};
    }
    float x = col_x_base[c];
    float y = row_y_base[r];
    for (int iter = 0; iter < 2; ++iter) {
        if (h_strips > 0 && !strip_x_centers.empty()) {
            std::vector<float> ys(h_strips);
            for (int s = 0; s < h_strips; ++s) ys[s] = row_y_strips[static_cast<size_t>(r) * h_strips + s];
            y = interp_piecewise(strip_x_centers, ys, x);
        }
        if (v_strips > 0 && !strip_y_centers.empty()) {
            std::vector<float> xs(v_strips);
            for (int s = 0; s < v_strips; ++s) xs[s] = col_x_strips[static_cast<size_t>(c) * v_strips + s];
            x = interp_piecewise(strip_y_centers, xs, y);
        }
    }
    return {x, y};
}

std::array<Point2f, 4> RectifiedGrid::cell_orig_quad(int r, int c) const {
    Point2f p0 = h_rect_to_orig.apply(rect_intersection(r, c));
    Point2f p1 = h_rect_to_orig.apply(rect_intersection(r, c + 1));
    Point2f p2 = h_rect_to_orig.apply(rect_intersection(r + 1, c + 1));
    Point2f p3 = h_rect_to_orig.apply(rect_intersection(r + 1, c));
    return {p0, p1, p2, p3};
}

ImageRgb RectifiedGrid::crop_cell_rect(int r, int c, int pad_px) const {
    Point2f tl = rect_intersection(r, c);
    Point2f tr = rect_intersection(r, c + 1);
    Point2f br = rect_intersection(r + 1, c + 1);
    Point2f bl = rect_intersection(r + 1, c);

    int x0 = std::max(0, static_cast<int>(std::floor(std::min(tl.x, bl.x))) + pad_px);
    int x1 = std::min(rectified_rgb.width, static_cast<int>(std::ceil(std::max(tr.x, br.x))) - pad_px);
    int y0 = std::max(0, static_cast<int>(std::floor(std::min(tl.y, tr.y))) + pad_px);
    int y1 = std::min(rectified_rgb.height, static_cast<int>(std::ceil(std::max(bl.y, br.y))) - pad_px);

    ImageRgb crop;
    if (x1 <= x0 + 2 || y1 <= y0 + 2) return crop;
    crop.width = x1 - x0;
    crop.height = y1 - y0;
    crop.data.resize(static_cast<size_t>(crop.width) * static_cast<size_t>(crop.height) * 3);
    for (int y = 0; y < crop.height; ++y) {
        const uint8_t* sp = &rectified_rgb.data[(static_cast<size_t>(y0 + y) * rectified_rgb.width + x0) * 3];
        uint8_t* dp = &crop.data[static_cast<size_t>(y) * crop.width * 3];
        std::memcpy(dp, sp, static_cast<size_t>(crop.width) * 3);
    }
    return crop;
}

bool extract_rectified_table_grid(const ImageRgb& img,
                                  const std::array<Point2f, 4>& corners,
                                  int expected_min_cols,
                                  int expected_min_rows,
                                  RectifiedGrid& out_grid,
                                  std::string& out_err) {
    if (!img.valid()) {
        out_err = "图片无效";
        return false;
    }

    float w_top = std::hypot(corners[1].x - corners[0].x, corners[1].y - corners[0].y);
    float w_bot = std::hypot(corners[2].x - corners[3].x, corners[2].y - corners[3].y);
    float h_left = std::hypot(corners[3].x - corners[0].x, corners[3].y - corners[0].y);
    float h_right = std::hypot(corners[2].x - corners[1].x, corners[2].y - corners[1].y);

    float w_est = std::max(w_top, w_bot);
    float h_est = std::max(h_left, h_right);
    if (w_est < 60.0f || h_est < 60.0f) {
        out_err = "未检测到有效表格区域，请重新拍照或手动调整四角";
        return false;
    }

    int rect_w = std::max(1400, std::min(2200, static_cast<int>(std::round(w_est * 1.25f))));
    int rect_h = std::max(600, static_cast<int>(std::round(rect_w * (h_est / w_est))));
    const float pad = 18.0f;

    std::array<Point2f, 4> dst_pts = {
        Point2f{pad, pad},
        Point2f{rect_w - 1.0f - pad, pad},
        Point2f{rect_w - 1.0f - pad, rect_h - 1.0f - pad},
        Point2f{pad, rect_h - 1.0f - pad}
    };
    if (!solve_homography(corners, dst_pts, out_grid.h_orig_to_rect) ||
        !out_grid.h_orig_to_rect.invert(out_grid.h_rect_to_orig)) {
        out_err = "四角透视矩阵无效，请重新调整四角";
        return false;
    }

    // Warp into padded destination using H_rect_to_orig directly
    out_grid.rectified_rgb.width = rect_w;
    out_grid.rectified_rgb.height = rect_h;
    out_grid.rectified_rgb.data.assign(static_cast<size_t>(rect_w) * rect_h * 3, 255);
    const Homography& hinv = out_grid.h_rect_to_orig;
    for (int y = 0; y < rect_h; ++y) {
        for (int x = 0; x < rect_w; ++x) {
            double denom = hinv.m[6] * x + hinv.m[7] * y + hinv.m[8];
            if (std::fabs(denom) < 1e-8) continue;
            float sx = static_cast<float>((hinv.m[0] * x + hinv.m[1] * y + hinv.m[2]) / denom);
            float sy = static_cast<float>((hinv.m[3] * x + hinv.m[4] * y + hinv.m[5]) / denom);
            if (sx < 0.0f || sy < 0.0f || sx > img.width - 1.0f || sy > img.height - 1.0f) continue;
            int x0 = static_cast<int>(std::floor(sx));
            int y0 = static_cast<int>(std::floor(sy));
            int x1 = std::min(img.width - 1, x0 + 1);
            int y1 = std::min(img.height - 1, y0 + 1);
            float wx = sx - x0, wy = sy - y0;
            const uint8_t* p00 = &img.data[(static_cast<size_t>(y0) * img.width + x0) * 3];
            const uint8_t* p10 = &img.data[(static_cast<size_t>(y0) * img.width + x1) * 3];
            const uint8_t* p01 = &img.data[(static_cast<size_t>(y1) * img.width + x0) * 3];
            const uint8_t* p11 = &img.data[(static_cast<size_t>(y1) * img.width + x1) * 3];
            uint8_t* dp = &out_grid.rectified_rgb.data[(static_cast<size_t>(y) * rect_w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                float v = (p00[c] * (1.0f - wx) + p10[c] * wx) * (1.0f - wy) +
                          (p01[c] * (1.0f - wx) + p11[c] * wx) * wy;
                dp[c] = clamp_u8(static_cast<int>(std::round(v)));
            }
        }
    }

    out_grid.rectified_gray = rgb_to_gray(out_grid.rectified_rgb);
    const GrayImage& g = out_grid.rectified_gray;
    auto integ = build_integral(g);
    int rad = std::max(18, std::min(rect_w, rect_h) / 48);

    std::vector<uint8_t> dark_bin(static_cast<size_t>(rect_w) * rect_h, 0);
    std::vector<uint8_t> dark_amt(static_cast<size_t>(rect_w) * rect_h, 0);
    for (int y = 0; y < rect_h; ++y) {
        for (int x = 0; x < rect_w; ++x) {
            float bg = box_mean(integ, rect_w, rect_h, x, y, rad, rad);
            float d = bg - static_cast<float>(g.data[static_cast<size_t>(y) * rect_w + x]);
            if (d > 14.0f) {
                dark_bin[static_cast<size_t>(y) * rect_w + x] = 1;
                dark_amt[static_cast<size_t>(y) * rect_w + x] = clamp_u8(static_cast<int>(d));
            }
        }
    }

    // Horizontal line mask: continuous run over hk pixels (wide enough to reject single-cell underlines)
    int hk = std::max(45, rect_w / 32);
    // Vertical line mask: continuous run over vk pixels (~1.5 row heights) inside a single strip
    int vk = std::max(46, rect_h / 16);
    std::vector<uint8_t> hline(static_cast<size_t>(rect_w) * rect_h, 0);
    std::vector<uint8_t> vline(static_cast<size_t>(rect_w) * rect_h, 0);

    for (int y = 0; y < rect_h; ++y) {
        std::vector<int> psum(rect_w + 1, 0);
        const uint8_t* row = &dark_bin[static_cast<size_t>(y) * rect_w];
        for (int x = 0; x < rect_w; ++x) psum[x + 1] = psum[x] + row[x];
        for (int x = 0; x < rect_w; ++x) {
            int x0 = std::max(0, x - hk / 2);
            int x1 = std::min(rect_w - 1, x + hk / 2);
            if (row[x] && psum[x1 + 1] - psum[x0] >= static_cast<int>(0.76f * (x1 - x0 + 1))) {
                hline[static_cast<size_t>(y) * rect_w + x] = dark_amt[static_cast<size_t>(y) * rect_w + x];
            }
        }
    }
    for (int x = 0; x < rect_w; ++x) {
        std::vector<int> psum(rect_h + 1, 0);
        for (int y = 0; y < rect_h; ++y) psum[y + 1] = psum[y] + dark_bin[static_cast<size_t>(y) * rect_w + x];
        for (int y = 0; y < rect_h; ++y) {
            int y0 = std::max(0, y - vk / 2);
            int y1 = std::min(rect_h - 1, y + vk / 2);
            if (dark_bin[static_cast<size_t>(y) * rect_w + x] &&
                psum[y1 + 1] - psum[y0] >= static_cast<int>(0.78f * (y1 - y0 + 1))) {
                vline[static_cast<size_t>(y) * rect_w + x] = dark_amt[static_cast<size_t>(y) * rect_w + x];
            }
        }
    }

    // 1. Detect horizontal lines across S_h = 8 strips to fit curved top/inner rows
    const int S_h = 8;
    out_grid.h_strips = S_h;
    out_grid.strip_x_centers.resize(S_h);
    std::vector<std::vector<float>> h_strip_profs(S_h, std::vector<float>(rect_h, 0.0f));
    int x_inner0 = static_cast<int>(pad);
    int x_inner1 = rect_w - static_cast<int>(pad);
    int inner_w = std::max(100, x_inner1 - x_inner0);

    for (int s = 0; s < S_h; ++s) {
        int sx0 = x_inner0 + (inner_w * s) / S_h;
        int sx1 = x_inner0 + (inner_w * (s + 1)) / S_h;
        out_grid.strip_x_centers[s] = 0.5f * (sx0 + sx1);
        int rx0 = std::max(0, sx0 - 24);
        int rx1 = std::min(rect_w - 1, sx1 + 24);
        for (int y = 0; y < rect_h; ++y) {
            float acc = 0.0f;
            const uint8_t* hr = &hline[static_cast<size_t>(y) * rect_w];
            for (int x = rx0; x <= rx1; ++x) acc += hr[x];
            h_strip_profs[s][y] = acc / std::max(1, rx1 - rx0 + 1);
        }
    }

    // Detect initial horizontal line peaks from central strips so curved outer rows never split into 2 peaks
    std::vector<float> h_global_prof(rect_h, 0.0f);
    for (int y = 1; y < rect_h - 1; ++y) {
        h_global_prof[y] = 0.55f * h_strip_profs[S_h / 2 - 1][y] + 0.45f * h_strip_profs[S_h / 2][y];
    }
    std::vector<float> h_smooth(rect_h, 0.0f);
    for (int y = 1; y < rect_h - 1; ++y) {
        h_smooth[y] = 0.25f * h_global_prof[y - 1] + 0.5f * h_global_prof[y] + 0.25f * h_global_prof[y + 1];
    }

    int min_row_dist = std::max(22, rect_h / 45);
    out_grid.row_y_base = find_1d_peaks(h_smooth, min_row_dist, 0.20f);
    if (static_cast<int>(out_grid.row_y_base.size()) < expected_min_rows) {
        out_grid.row_y_base = find_1d_peaks(h_smooth, std::max(16, rect_h / 55), 0.12f);
    }
    if (static_cast<int>(out_grid.row_y_base.size()) < 4) {
        out_err = "未能检测到清晰横向表格线，请重新对准或调整四角";
        return false;
    }

    // Track each horizontal line across the S_h strips (nearest local peak within +-14px)
    const int n_rlines = static_cast<int>(out_grid.row_y_base.size());
    out_grid.row_y_strips.resize(static_cast<size_t>(n_rlines) * S_h);
    for (int r = 0; r < n_rlines; ++r) {
        float y_ref = out_grid.row_y_base[r];
        std::vector<float> tracked(S_h, y_ref);
        std::vector<bool> valid(S_h, false);
        auto track_h = [&](int s, float cur_y) {
            int y0 = std::max(1, static_cast<int>(std::round(cur_y)) - 14);
            int y1 = std::min(rect_h - 2, static_cast<int>(std::round(cur_y)) + 14);
            int best_y = -1;
            float best_v = 0.8f;
            for (int y = y0; y <= y1; ++y) {
                if (h_strip_profs[s][y] > best_v) {
                    best_v = h_strip_profs[s][y];
                    best_y = y;
                }
            }
            if (best_y > 0) {
                float p0 = h_strip_profs[s][best_y - 1];
                float p1 = h_strip_profs[s][best_y];
                float p2 = h_strip_profs[s][best_y + 1];
                float den = p0 - 2.0f * p1 + p2;
                float off = std::fabs(den) > 1e-4f ? std::max(-0.5f, std::min(0.5f, 0.5f * (p0 - p2) / den)) : 0.0f;
                tracked[s] = best_y + off;
                valid[s] = true;
            }
        };
        int mid_h = S_h / 2;
        for (int s = mid_h - 1; s >= 0; --s) {
            float cur = (s == mid_h - 1 || !valid[s + 1]) ? y_ref : tracked[s + 1];
            track_h(s, cur);
        }
        for (int s = mid_h; s < S_h; ++s) {
            float cur = (s == mid_h || !valid[s - 1]) ? y_ref : tracked[s - 1];
            track_h(s, cur);
        }
        for (int s = 0; s < S_h; ++s) {
            if (!valid[s]) {
                float nearest = y_ref;
                int best_d = 99;
                for (int k = 0; k < S_h; ++k) {
                    if (valid[k] && std::abs(k - s) < best_d) {
                        best_d = std::abs(k - s);
                        nearest = tracked[k];
                    }
                }
                tracked[s] = nearest;
            }
            out_grid.row_y_strips[static_cast<size_t>(r) * S_h + s] = tracked[s];
        }
    }

    // 2. Detect vertical lines across S_v = 8 vertical strips inside main table body
    int y_body0 = static_cast<int>(out_grid.row_y_base.front() +
                                   0.08f * (out_grid.row_y_base.back() - out_grid.row_y_base.front()));
    int y_body1 = static_cast<int>(out_grid.row_y_base.back());
    y_body0 = std::max(0, std::min(rect_h - 10, y_body0));
    y_body1 = std::max(y_body0 + 10, std::min(rect_h - 1, y_body1));

    const int S_v = 8;
    out_grid.v_strips = S_v;
    out_grid.strip_y_centers.resize(S_v);
    std::vector<std::vector<float>> v_strip_profs(S_v, std::vector<float>(rect_w, 0.0f));
    int body_h = y_body1 - y_body0;
    for (int s = 0; s < S_v; ++s) {
        int sy0 = y_body0 + (body_h * s) / S_v;
        int sy1 = y_body0 + (body_h * (s + 1)) / S_v;
        out_grid.strip_y_centers[s] = 0.5f * (sy0 + sy1);
        for (int y = sy0; y <= sy1; ++y) {
            const uint8_t* vr = &vline[static_cast<size_t>(y) * rect_w];
            for (int x = 0; x < rect_w; ++x) v_strip_profs[s][x] += vr[x];
        }
        float inv_len = 1.0f / std::max(1, sy1 - sy0 + 1);
        for (int x = 0; x < rect_w; ++x) v_strip_profs[s][x] *= inv_len;
    }

    // Detect vertical lines primarily from central vertical strips so slant/bend across height doesn't smear peaks
    std::vector<float> v_global_prof(rect_w, 0.0f);
    for (int x = 1; x < rect_w - 1; ++x) {
        v_global_prof[x] = 0.25f * (v_strip_profs[2][x] + v_strip_profs[3][x] +
                                    v_strip_profs[4][x] + v_strip_profs[5][x]);
    }
    std::vector<float> v_smooth(rect_w, 0.0f);
    for (int x = 1; x < rect_w - 1; ++x) {
        v_smooth[x] = 0.25f * v_global_prof[x - 1] + 0.5f * v_global_prof[x] + 0.25f * v_global_prof[x + 1];
    }

    int min_col_dist = std::max(24, rect_w / 54);
    out_grid.col_x_base = find_1d_peaks(v_smooth, min_col_dist, 0.28f);
    if (static_cast<int>(out_grid.col_x_base.size()) < expected_min_cols) {
        out_grid.col_x_base = find_1d_peaks(v_smooth, min_col_dist, 0.18f);
    }

    // Filter out isolated tall strokes (e.g. a handwritten '1' in a single cell) that only span a few strips
    {
        std::vector<float> verified_cols;
        const int min_active_strips = std::max(2, (S_v + 1) / 2);
        for (float px : out_grid.col_x_base) {
            int active_strips = 0;
            int xc = static_cast<int>(std::round(px));
            int x0 = std::max(0, xc - 24);
            int x1 = std::min(rect_w - 1, xc + 24);
            for (int s = 0; s < S_v; ++s) {
                float mx = 0.0f;
                for (int x = x0; x <= x1; ++x) mx = std::max(mx, v_strip_profs[s][x]);
                if (mx > 3.0f) ++active_strips;
            }
            if (active_strips >= min_active_strips) {
                verified_cols.push_back(px);
            }
        }
        if (static_cast<int>(verified_cols.size()) >= 4) {
            out_grid.col_x_base = std::move(verified_cols);
        }
    }

    // Repair any missing vertical lines inside regular narrow data columns (handles 1 or multiple missed lines)
    if (out_grid.col_x_base.size() >= 6) {
        std::vector<float> widths;
        for (size_t i = 2; i < out_grid.col_x_base.size(); ++i) {
            float g_w = out_grid.col_x_base[i] - out_grid.col_x_base[i - 1];
            if (g_w < rect_w * 0.15f) widths.push_back(g_w);
        }
        if (!widths.empty()) {
            std::sort(widths.begin(), widths.end());
            float med_w = widths[widths.size() / 2];
            if (med_w > 12.0f) {
                std::vector<float> fixed;
                fixed.push_back(out_grid.col_x_base[0]);
                for (size_t i = 1; i < out_grid.col_x_base.size(); ++i) {
                    float gap = out_grid.col_x_base[i] - out_grid.col_x_base[i - 1];
                    // Skip col 0..1 (name column is naturally 2x-3x wide)
                    if (i >= 2 && gap > med_w * 1.72f) {
                        int k = static_cast<int>(std::round(gap / med_w));
                        if (k >= 2) {
                            for (int m = 1; m < k; ++m) {
                                fixed.push_back(out_grid.col_x_base[i - 1] + (gap * m) / k);
                            }
                        }
                    }
                    fixed.push_back(out_grid.col_x_base[i]);
                }
                out_grid.col_x_base = std::move(fixed);
            }
        }
    }

    if (static_cast<int>(out_grid.col_x_base.size()) < 4) {
        out_err = "未能检测到清晰竖向表格线，请重新对准或调整四角";
        return false;
    }

    const int n_clines = static_cast<int>(out_grid.col_x_base.size());
    out_grid.col_x_strips.resize(static_cast<size_t>(n_clines) * S_v);
    std::vector<std::vector<float>> all_tracked(n_clines, std::vector<float>(S_v, 0.0f));
    std::vector<std::vector<bool>> all_valid(n_clines, std::vector<bool>(S_v, false));

    for (int c = 0; c < n_clines; ++c) {
        float x_ref = out_grid.col_x_base[c];
        auto& tracked = all_tracked[c];
        auto& valid = all_valid[c];
        std::fill(tracked.begin(), tracked.end(), x_ref);
        auto track_strip = [&](int s, float cur_x) {
            int x0 = std::max(1, static_cast<int>(std::round(cur_x)) - 10);
            int x1 = std::min(rect_w - 2, static_cast<int>(std::round(cur_x)) + 10);
            int best_x = -1;
            float best_v = 1.0f;
            for (int x = x0; x <= x1; ++x) {
                float local_v = 0.25f * v_strip_profs[s][x - 1] +
                                0.50f * v_strip_profs[s][x] +
                                0.25f * v_strip_profs[s][x + 1];
                if (local_v > best_v) {
                    best_v = local_v;
                    best_x = x;
                }
            }
            if (best_x > 0) {
                float p0 = v_strip_profs[s][best_x - 1];
                float p1 = v_strip_profs[s][best_x];
                float p2 = v_strip_profs[s][best_x + 1];
                float den = p0 - 2.0f * p1 + p2;
                float off = std::fabs(den) > 1e-4f ? std::max(-0.5f, std::min(0.5f, 0.5f * (p0 - p2) / den)) : 0.0f;
                tracked[s] = best_x + off;
                valid[s] = true;
            }
        };
        int mid_v = S_v / 2;
        for (int s = mid_v - 1; s >= 0; --s) {
            float pred = x_ref;
            if (s == mid_v - 2) {
                pred = tracked[s + 1];
            } else if (s < mid_v - 2) {
                pred = 2.0f * tracked[s + 1] - tracked[s + 2];
            }
            track_strip(s, pred);
            if (!valid[s]) tracked[s] = pred;
        }
        for (int s = mid_v; s < S_v; ++s) {
            float pred = x_ref;
            if (s == mid_v) {
                pred = valid[mid_v - 1] ? tracked[mid_v - 1] : x_ref;
            } else if (s == mid_v + 1) {
                pred = 2.0f * tracked[s - 1] - tracked[s - 2];
            } else {
                pred = 2.0f * tracked[s - 1] - tracked[s - 2];
            }
            track_strip(s, pred);
            if (!valid[s]) tracked[s] = pred;
        }
    }

    // Refine any invalid strip points using median displacement of neighboring valid columns
    const int mid_v = S_v / 2;
    for (int c = 0; c < n_clines; ++c) {
        for (int s = 0; s < S_v; ++s) {
            if (!all_valid[c][s]) {
                std::vector<float> shifts;
                for (int dc = -4; dc <= 4; ++dc) {
                    int nc = c + dc;
                    if (nc < 0 || nc >= n_clines || nc == c) continue;
                    if (all_valid[nc][s] && all_valid[nc][mid_v - 1]) {
                        shifts.push_back(all_tracked[nc][s] - all_tracked[nc][mid_v - 1]);
                    }
                }
                if (!shifts.empty()) {
                    std::sort(shifts.begin(), shifts.end());
                    float med_shift = shifts[shifts.size() / 2];
                    float anchor = all_valid[c][mid_v - 1] ? all_tracked[c][mid_v - 1] : out_grid.col_x_base[c];
                    all_tracked[c][s] = anchor + med_shift;
                }
            }
            out_grid.col_x_strips[static_cast<size_t>(c) * S_v + s] = all_tracked[c][s];
        }
    }

    return true;
}

ImageRgb erase_bottom_underline(const ImageRgb& cell_rgb) {
    if (!cell_rgb.valid() || cell_rgb.height < 8 || cell_rgb.width < 8) return cell_rgb;
    ImageRgb out = cell_rgb;
    GrayImage g = rgb_to_gray(cell_rgb);
    int w = g.width, h = g.height;

    // Estimate bright paper background (85th percentile of upper rows)
    std::vector<uint8_t> vals = g.data;
    std::sort(vals.begin(), vals.end());
    uint8_t bg = vals[static_cast<size_t>(vals.size() * 0.85)];
    if (bg < 120) bg = 220;

    // Detect bottom printed horizontal underline spanning >= 60% width in lower 26% of crop
    int y_start = std::max(0, static_cast<int>(h * 0.74f));
    for (int y = y_start; y < h; ++y) {
        int dark_cnt = 0;
        for (int x = 0; x < w; ++x) {
            if (g.data[static_cast<size_t>(y) * w + x] < static_cast<int>(bg) - 28) {
                ++dark_cnt;
            }
        }
        if (dark_cnt >= static_cast<int>(w * 0.60f)) {
            // Erase line pixels in this row only if above pixel is relatively bright (preserve vertical text stems)
            for (int x = 0; x < w; ++x) {
                bool above_dark = (y >= 2 && g.data[static_cast<size_t>(y - 2) * w + x] < static_cast<int>(bg) - 35);
                if (!above_dark) {
                    uint8_t* dp = &out.data[(static_cast<size_t>(y) * w + x) * 3];
                    dp[0] = bg; dp[1] = bg; dp[2] = bg;
                }
            }
        }
    }
    return out;
}

namespace {

struct RawCellStats {
    float max_diff = 0.0f;
    float bg = 220.0f;
    std::vector<float> diff; // w * h
    int w = 0;
    int h = 0;
    int component_count = 0;
};

RawCellStats extract_cell_diff(const RectifiedGrid& grid, int row, int col) {
    RawCellStats st;
    Point2f tl = grid.rect_intersection(row, col);
    Point2f tr = grid.rect_intersection(row, col + 1);
    Point2f br = grid.rect_intersection(row + 1, col + 1);
    Point2f bl = grid.rect_intersection(row + 1, col);

    const GrayImage& g = grid.rectified_gray;
    float left_x = 0.5f * (tl.x + bl.x);
    float right_x = 0.5f * (tr.x + br.x);
    float top_y = 0.5f * (tl.y + tr.y);
    float bot_y = 0.5f * (bl.y + br.y);
    float cell_w_est = right_x - left_x;
    float cell_h_est = bot_y - top_y;
    if (cell_w_est < 8.0f || cell_h_est < 8.0f) return st;

    // Estimate local background around the candidate cell (85th percentile) before border snapping
    {
        int bx0 = std::max(0, static_cast<int>(std::round(left_x)) - 8);
        int bx1 = std::min(g.width - 1, static_cast<int>(std::round(right_x)) + 8);
        int by0 = std::max(0, static_cast<int>(std::round(top_y)) - 8);
        int by1 = std::min(g.height - 1, static_cast<int>(std::round(bot_y)) + 8);
        std::vector<uint8_t> bg_samples;
        bg_samples.reserve(static_cast<size_t>(std::max(1, bx1 - bx0 + 1)) * std::max(1, by1 - by0 + 1));
        for (int y = by0; y <= by1; ++y) {
            const uint8_t* rowp = &g.data[static_cast<size_t>(y) * g.width];
            for (int x = bx0; x <= bx1; ++x) bg_samples.push_back(rowp[x]);
        }
        if (!bg_samples.empty()) {
            size_t nth = static_cast<size_t>(bg_samples.size() * 0.85);
            std::nth_element(bg_samples.begin(), bg_samples.begin() + nth, bg_samples.end());
            st.bg = static_cast<float>(bg_samples[nth]);
        }
    }

    // Locally snap left_x/right_x (+-7px) and top_y/bot_y (+-5px) to actual dark grid lines
    auto snap_v_line = [&](float x_est) -> float {
        int base_x = static_cast<int>(std::round(x_est));
        int sy0 = std::max(0, static_cast<int>(std::round(top_y)) - 4);
        int sy1 = std::min(g.height - 1, static_cast<int>(std::round(bot_y)) + 4);
        int span = sy1 - sy0 + 1;
        if (span < 6) return x_est;
        int best_x = base_x;
        float best_score = -1e9f;
        bool found = false;
        for (int dx = -7; dx <= 7; ++dx) {
            int x = base_x + dx;
            if (x < 0 || x >= g.width) continue;
            int dark = 0;
            float sum_d = 0.0f;
            for (int y = sy0; y <= sy1; ++y) {
                float d = std::max(0.0f, st.bg - static_cast<float>(g.data[static_cast<size_t>(y) * g.width + x]));
                sum_d += d;
                if (d >= 15.0f) ++dark;
            }
            float dark_frac = static_cast<float>(dark) / span;
            float mean_d = sum_d / span;
            if (dark_frac >= 0.52f && mean_d >= 14.0f) {
                float sc = mean_d + 24.0f * dark_frac - 0.35f * std::abs(dx);
                if (sc > best_score) {
                    best_score = sc;
                    best_x = x;
                    found = true;
                }
            }
        }
        return found ? static_cast<float>(best_x) : x_est;
    };

    auto snap_h_line = [&](float y_est, float lx, float rx) -> float {
        int base_y = static_cast<int>(std::round(y_est));
        int sx0 = std::max(0, static_cast<int>(std::round(lx)) - 4);
        int sx1 = std::min(g.width - 1, static_cast<int>(std::round(rx)) + 4);
        int span = sx1 - sx0 + 1;
        if (span < 6) return y_est;
        int best_y = base_y;
        float best_score = -1e9f;
        bool found = false;
        for (int dy = -5; dy <= 5; ++dy) {
            int y = base_y + dy;
            if (y < 0 || y >= g.height) continue;
            const uint8_t* rp = &g.data[static_cast<size_t>(y) * g.width];
            int dark = 0;
            float sum_d = 0.0f;
            for (int x = sx0; x <= sx1; ++x) {
                float d = std::max(0.0f, st.bg - static_cast<float>(rp[x]));
                sum_d += d;
                if (d >= 15.0f) ++dark;
            }
            float dark_frac = static_cast<float>(dark) / span;
            float mean_d = sum_d / span;
            if (dark_frac >= 0.52f && mean_d >= 14.0f) {
                float sc = mean_d + 24.0f * dark_frac - 0.35f * std::abs(dy);
                if (sc > best_score) {
                    best_score = sc;
                    best_y = y;
                    found = true;
                }
            }
        }
        return found ? static_cast<float>(best_y) : y_est;
    };

    float slx = snap_v_line(left_x);
    float srx = snap_v_line(right_x);
    if (srx - slx >= std::max(10.0f, cell_w_est * 0.55f)) {
        left_x = slx;
        right_x = srx;
    }
    float sty = snap_h_line(top_y, left_x, right_x);
    float sby = snap_h_line(bot_y, left_x, right_x);
    if (sby - sty >= std::max(10.0f, cell_h_est * 0.55f)) {
        top_y = sty;
        bot_y = sby;
    }

    cell_w_est = right_x - left_x;
    cell_h_est = bot_y - top_y;
    int trim_x = (cell_w_est >= 28.0f) ? 4 : 2;
    int top_trim_y = (cell_h_est >= 24.0f) ? 3 : 2;
    int bot_trim_y = 1;
    int x0 = std::max(0, static_cast<int>(std::round(left_x)) + trim_x);
    int x1 = std::min(g.width, static_cast<int>(std::round(right_x)) - trim_x);
    int y0 = std::max(0, static_cast<int>(std::round(top_y)) + top_trim_y);
    int y1 = std::min(g.height, static_cast<int>(std::round(bot_y)) - bot_trim_y);
    if (x1 <= x0 + 4 || y1 <= y0 + 4) return st;

    st.w = x1 - x0;
    st.h = y1 - y0;
    st.diff.assign(static_cast<size_t>(st.w) * st.h, 0.0f);

    // Recompute local background = 85th percentile of tight window around snapped cell
    {
        int bx0 = std::max(0, x0 - 6), bx1 = std::min(g.width - 1, x1 + 6);
        int by0 = std::max(0, y0 - 6), by1 = std::min(g.height - 1, y1 + 6);
        std::vector<uint8_t> bg_samples;
        bg_samples.reserve(static_cast<size_t>(bx1 - bx0 + 1) * (by1 - by0 + 1));
        for (int y = by0; y <= by1; ++y) {
            const uint8_t* rowp = &g.data[static_cast<size_t>(y) * g.width];
            for (int x = bx0; x <= bx1; ++x) bg_samples.push_back(rowp[x]);
        }
        std::nth_element(bg_samples.begin(),
                         bg_samples.begin() + static_cast<size_t>(bg_samples.size() * 0.85),
                         bg_samples.end());
        st.bg = static_cast<float>(bg_samples[static_cast<size_t>(bg_samples.size() * 0.85)]);
    }

    for (int y = 0; y < st.h; ++y) {
        const uint8_t* rp = &g.data[static_cast<size_t>(y0 + y) * g.width + x0];
        for (int x = 0; x < st.w; ++x) {
            float d = std::max(0.0f, st.bg - static_cast<float>(rp[x]));
            st.diff[static_cast<size_t>(y) * st.w + x] = d;
        }
    }

    // Record columns belonging to a narrow interior descending vertical stroke just above bottom border
    std::vector<uint8_t> bottom_stem_col(static_cast<size_t>(st.w), 0);
    if (st.h >= 8) {
        int cy_lo = std::max(1, st.h - 6);
        int cy_hi = std::max(cy_lo, st.h - 4);
        for (int y = cy_lo; y <= cy_hi; ++y) {
            int dark = 0;
            for (int x = 0; x < st.w; ++x) {
                if (st.diff[static_cast<size_t>(y) * st.w + x] > 18.0f) ++dark;
            }
            if (dark >= static_cast<int>(st.w * 0.55f)) continue;
            for (int x = 3; x < st.w - 3; ++x) {
                if (st.diff[static_cast<size_t>(y) * st.w + x] > 20.0f) {
                    bottom_stem_col[static_cast<size_t>(x)] = 1;
                }
            }
        }
        // Dilate stem mask by +-1px horizontally for slight slant across bottom border
        std::vector<uint8_t> dilated = bottom_stem_col;
        for (int x = 3; x < st.w - 3; ++x) {
            if (bottom_stem_col[static_cast<size_t>(x)]) {
                dilated[static_cast<size_t>(x - 1)] = 1;
                dilated[static_cast<size_t>(x + 1)] = 1;
            }
        }
        bottom_stem_col.swap(dilated);
    }

    // Clear residual border grid lines along outer edges while preserving crossing vertical stems at bottom
    auto clear_edge_row = [&](int y, bool is_bottom) {
        int dark = 0;
        for (int x = 0; x < st.w; ++x) if (st.diff[static_cast<size_t>(y) * st.w + x] > 18.0f) ++dark;
        if (dark >= static_cast<int>(st.w * 0.65f)) {
            for (int x = 0; x < st.w; ++x) {
                if (is_bottom && bottom_stem_col[static_cast<size_t>(x)]) {
                    continue;
                }
                st.diff[static_cast<size_t>(y) * st.w + x] = 0.0f;
            }
        }
    };
    auto clear_edge_col = [&](int x) {
        int dark = 0;
        for (int y = 0; y < st.h; ++y) if (st.diff[static_cast<size_t>(y) * st.w + x] > 18.0f) ++dark;
        if (dark >= static_cast<int>(st.h * 0.65f)) {
            for (int y = 0; y < st.h; ++y) st.diff[static_cast<size_t>(y) * st.w + x] = 0.0f;
        }
    };
    int top_edge_rows = std::min(3, st.h / 4);
    for (int k = 0; k < top_edge_rows; ++k) {
        clear_edge_row(k, false);
        clear_edge_row(st.h - 1 - k, true);
    }
    for (int k = 0; k < std::min(3, st.w / 4); ++k) {
        clear_edge_col(k);
        clear_edge_col(st.w - 1 - k);
    }

    // Recover cross-border descending tail (e.g. lower stem of '4'/'7'/'9') below y1 along narrow stem
    if (st.h >= 6 && y1 < g.height) {
        int tail_y = st.h - 1;
        int stem_x0 = st.w, stem_x1 = -1;
        int best_span_w = 0;
        for (int x = 4; x < st.w - 4;) {
            bool active = bottom_stem_col[static_cast<size_t>(x)] &&
                          (st.diff[static_cast<size_t>(tail_y) * st.w + x] > 20.0f ||
                           st.diff[static_cast<size_t>(tail_y - 1) * st.w + x] > 20.0f);
            if (!active) {
                ++x;
                continue;
            }
            int rx = x;
            while (rx < st.w - 4 &&
                   bottom_stem_col[static_cast<size_t>(rx)] &&
                   (st.diff[static_cast<size_t>(tail_y) * st.w + rx] > 18.0f ||
                    st.diff[static_cast<size_t>(tail_y - 1) * st.w + rx] > 18.0f)) {
                ++rx;
            }
            int span_w = rx - x;
            if (span_w >= 1 && span_w <= 10 && span_w > best_span_w) {
                best_span_w = span_w;
                stem_x0 = x;
                stem_x1 = rx - 1;
            }
            x = rx;
        }
        if (stem_x1 >= stem_x0) {
            int max_extra = std::min(6, g.height - y1);
            int sx0 = std::max(3, stem_x0 - 2);
            int sx1 = std::min(st.w - 4, stem_x1 + 2);
            for (int dy = 0; dy < max_extra; ++dy) {
                int ey = y1 + dy;
                const uint8_t* rp = &g.data[static_cast<size_t>(ey) * g.width + x0];
                std::vector<float> extra_row(static_cast<size_t>(st.w), 0.0f);
                bool any_stem_px = false;
                for (int x = sx0; x <= sx1; ++x) {
                    float d = std::max(0.0f, st.bg - static_cast<float>(rp[x]));
                    if (d > 18.0f) {
                        extra_row[static_cast<size_t>(x)] = d;
                        any_stem_px = true;
                    }
                }
                if (!any_stem_px) break;
                st.diff.insert(st.diff.end(), extra_row.begin(), extra_row.end());
                ++st.h;
            }
        }
    }

    // Remove border grid line components and neighbor bleed strokes dangling from top border
    std::vector<uint8_t> fg(static_cast<size_t>(st.w) * st.h, 0);
    for (size_t i = 0; i < fg.size(); ++i) {
        if (st.diff[i] > 20.0f) fg[i] = 1;
    }
    std::vector<uint8_t> vis(fg.size(), 0);
    std::vector<int> comp;
    for (int y = 0; y < st.h; ++y) {
        for (int x = 0; x < st.w; ++x) {
            size_t idx = static_cast<size_t>(y) * st.w + x;
            if (!fg[idx] || vis[idx]) continue;
            comp.clear();
            vis[idx] = 1;
            comp.push_back(static_cast<int>(idx));
            int minx = x, maxx = x, miny = y, maxy = y;
            double sum_y = 0.0;
            size_t head = 0;
            while (head < comp.size()) {
                int p = comp[head++];
                int py = p / st.w, px = p % st.w;
                minx = std::min(minx, px); maxx = std::max(maxx, px);
                miny = std::min(miny, py); maxy = std::max(maxy, py);
                sum_y += py;
                for (int dy = -1; dy <= 1; ++dy) {
                    int ny = py + dy;
                    if (ny < 0 || ny >= st.h) continue;
                    for (int dx = -1; dx <= 1; ++dx) {
                        int nx = px + dx;
                        if (nx < 0 || nx >= st.w) continue;
                        size_t ni = static_cast<size_t>(ny) * st.w + nx;
                        if (fg[ni] && !vis[ni]) {
                            vis[ni] = 1;
                            comp.push_back(static_cast<int>(ni));
                        }
                    }
                }
            }
            double cy = sum_y / comp.size();
            int cw = maxx - minx + 1;
            int ch = maxy - miny + 1;
            // 1. Vertical grid line residue clinging to outer left/right border margins or spanning full height
            bool side_vbar = ((maxx <= 5 || minx >= st.w - 6) && cw <= 7) ||
                             ((minx <= 5 || maxx >= st.w - 6) && cw <= 9 && ch >= st.h * 0.35f) ||
                             (miny <= 2 && maxy >= st.h - 3 && cw <= 6);
            // 2. Horizontal grid line residue clinging to top/bottom edge
            bool edge_hbar = ((maxy <= 3 || miny >= st.h - 4) && ch <= 5 && cw >= st.w * 0.40f);
            // 3. Top-bleed tail from row above: touches top edge, stops well before center
            bool top_bleed = (miny <= 1 && maxy < st.h * 0.42f && cy < st.h * 0.24f && ch < st.h * 0.40f);
            if (side_vbar || edge_hbar || top_bleed) {
                for (int p : comp) st.diff[p] = 0.0f;
            } else if (comp.size() >= 3) {
                ++st.component_count;
            }
        }
    }

    // Core inner region max_diff (avoid corners)
    int cx0 = std::min(st.w - 1, 2), cx1 = std::max(cx0 + 1, st.w - 2);
    int cy0 = std::min(st.h - 1, 2), cy1 = std::max(cy0 + 1, st.h - 2);
    for (int y = cy0; y < cy1; ++y) {
        for (int x = cx0; x < cx1; ++x) {
            st.max_diff = std::max(st.max_diff, st.diff[static_cast<size_t>(y) * st.w + x]);
        }
    }
    return st;
}

} // namespace

float compute_table_ink_reference(const RectifiedGrid& grid,
                                  int start_row,
                                  int end_row,
                                  int start_col,
                                  int end_col) {
    std::vector<float> peaks;
    for (int r = start_row; r < end_row; ++r) {
        for (int c = start_col; c < end_col; ++c) {
            RawCellStats st = extract_cell_diff(grid, r, c);
            if (st.max_diff >= 30.0f) {
                peaks.push_back(st.max_diff);
            }
        }
    }
    if (peaks.empty()) return 100.0f;
    std::sort(peaks.begin(), peaks.end());
    float p90 = peaks[static_cast<size_t>(peaks.size() * 0.90)];
    return std::max(60.0f, p90);
}

CellInkAnalysis analyze_digit_cell(const RectifiedGrid& grid,
                                   int row,
                                   int col,
                                   float table_ink_ref) {
    CellInkAnalysis res;
    RawCellStats st = extract_cell_diff(grid, row, col);
    if (st.w <= 0 || st.h <= 0) {
        res.category = InkCategory::Empty;
        return res;
    }

    res.max_diff = st.max_diff;
    res.cell_w = st.w;
    res.cell_h = st.h;
    res.component_count = st.component_count;
    float ref = std::max(60.0f, table_ink_ref);
    res.rel_peak = st.max_diff / ref;

    float strong_thresh = std::max(26.0f, ref * 0.36f);
    float core_thresh = std::max(16.0f, ref * 0.20f);

    int core_px = 0;
    int minx = st.w, maxx = -1, miny = st.h, maxy = -1;
    for (int y = 0; y < st.h; ++y) {
        for (int x = 0; x < st.w; ++x) {
            float d = st.diff[static_cast<size_t>(y) * st.w + x];
            if (d >= strong_thresh) ++res.strong_px;
            if (d >= core_thresh) {
                ++core_px;
                minx = std::min(minx, x); maxx = std::max(maxx, x);
                miny = std::min(miny, y); maxy = std::max(maxy, y);
            }
        }
    }
    res.core_px = core_px;
    res.ink_area_ratio = static_cast<float>(res.strong_px) / static_cast<float>(st.w * st.h);

    if (st.max_diff < 22.0f ||
        (res.strong_px < 6 && core_px < 20 && res.rel_peak < 0.45f) ||
        (res.rel_peak < 0.19f && res.strong_px < 2)) {
        res.category = InkCategory::Empty;
        return res;
    }

    // Erased ghost detection: visible smudge/remnant but far fewer strong ink pixels than any real digit
    if (res.strong_px < 8 && res.rel_peak < 0.65f) {
        res.category = InkCategory::SuspectErased;
        return res;
    }

    if (res.ink_area_ratio > 0.21f) {
        res.category = InkCategory::SuspectOverwrite;
    } else {
        res.category = InkCategory::ValidInk;
    }

    // Geometry check for thin vertical '1'
    if (maxx >= minx && maxy >= miny) {
        int bw = maxx - minx + 1;
        int bh = maxy - miny + 1;
        res.bbox_w = bw;
        res.bbox_h = bh;
        if (bh >= st.h * 0.35f && bh >= bw * 2.0f && bw <= st.w * 0.42f) {
            res.tall_thin_single_stroke = true;
        }
    }

    // Render clean dark-on-white centered 96x48 canvas for PP-OCRv5_mobile_rec
    const int cw = 96, ch = 48;
    res.prepared_digit_canvas.width = cw;
    res.prepared_digit_canvas.height = ch;
    res.prepared_digit_canvas.data.assign(static_cast<size_t>(cw) * ch * 3, 255);

    if (maxx >= minx && maxy >= miny) {
        int bw = maxx - minx + 1;
        int bh = maxy - miny + 1;
        float target_h = 30.0f;
        float scale = std::min(target_h / std::max(1, bh), 60.0f / std::max(1, bw));
        int dw = std::max(2, static_cast<int>(std::round(bw * scale)));
        int dh = std::max(2, static_cast<int>(std::round(bh * scale)));
        int ox = (cw - dw) / 2;
        int oy = (ch - dh) / 2;
        float ink_scale = 235.0f / std::max(38.0f, st.max_diff);

        for (int y = 0; y < dh; ++y) {
            float sy = miny + (y + 0.5f) / scale - 0.5f;
            int y0 = std::max(0, std::min(st.h - 1, static_cast<int>(std::floor(sy))));
            int y1 = std::min(st.h - 1, y0 + 1);
            float wy = std::max(0.0f, sy - y0);
            for (int x = 0; x < dw; ++x) {
                float sx = minx + (x + 0.5f) / scale - 0.5f;
                int x0 = std::max(0, std::min(st.w - 1, static_cast<int>(std::floor(sx))));
                int x1 = std::min(st.w - 1, x0 + 1);
                float wx = std::max(0.0f, sx - x0);
                float d00 = st.diff[y0 * st.w + x0];
                float d10 = st.diff[y0 * st.w + x1];
                float d01 = st.diff[y1 * st.w + x0];
                float d11 = st.diff[y1 * st.w + x1];
                float d = (d00 * (1.0f - wx) + d10 * wx) * (1.0f - wy) +
                          (d01 * (1.0f - wx) + d11 * wx) * wy;
                int gray_val = 255 - static_cast<int>(std::round(d * ink_scale));
                uint8_t gv = clamp_u8(gray_val);
                uint8_t* dp = &res.prepared_digit_canvas.data[(static_cast<size_t>(oy + y) * cw + (ox + x)) * 3];
                dp[0] = gv; dp[1] = gv; dp[2] = gv;
            }
        }
    }

    return res;
}

bool point_in_quad(Point2f p, const std::array<Point2f, 4>& quad) {
    bool pos = false, neg = false;
    for (int i = 0; i < 4; ++i) {
        Point2f a = quad[i];
        Point2f b = quad[(i + 1) % 4];
        float cross = (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
        if (cross > 0.0f) pos = true;
        if (cross < 0.0f) neg = true;
        if (pos && neg) return false;
    }
    return true;
}

} // namespace nurseocr
