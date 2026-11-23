#include "nurseocr/ocr_engine.h"
#include "nurseocr/calculator.h"
#include "nurseocr/image_io.h"
#include "nurseocr/name_doc_reader.h"

#include <net.h>
#include <ppocrv5_dict.h>
#include <mnist12_weights.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nurseocr {

namespace {

bool read_binary_file(const std::string& path, std::vector<uint8_t>& out) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(1, wlen)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
    FILE* f = _wfopen(wpath.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(sz));
    size_t n = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    out.resize(n);
    return !out.empty();
}

std::string join_path(const std::string& dir, const char* file) {
    if (dir.empty()) return file;
    char back = dir.back();
    if (back == '/' || back == '\\') return dir + file;
    return dir + "/" + file;
}

ImageRgb resize_rgb_bilinear(const ImageRgb& src, int dst_w, int dst_h) {
    ImageRgb dst;
    if (!src.valid() || dst_w <= 0 || dst_h <= 0) return dst;
    dst.width = dst_w;
    dst.height = dst_h;
    dst.data.resize(static_cast<size_t>(dst_w) * dst_h * 3);
    float sx_r = static_cast<float>(src.width) / dst_w;
    float sy_r = static_cast<float>(src.height) / dst_h;
    for (int y = 0; y < dst_h; ++y) {
        float fy = (y + 0.5f) * sy_r - 0.5f;
        int y0 = std::max(0, std::min(src.height - 1, static_cast<int>(std::floor(fy))));
        int y1 = std::min(src.height - 1, y0 + 1);
        float wy = std::max(0.0f, fy - y0);
        for (int x = 0; x < dst_w; ++x) {
            float fx = (x + 0.5f) * sx_r - 0.5f;
            int x0 = std::max(0, std::min(src.width - 1, static_cast<int>(std::floor(fx))));
            int x1 = std::min(src.width - 1, x0 + 1);
            float wx = std::max(0.0f, fx - x0);
            const uint8_t* p00 = &src.data[(static_cast<size_t>(y0) * src.width + x0) * 3];
            const uint8_t* p10 = &src.data[(static_cast<size_t>(y0) * src.width + x1) * 3];
            const uint8_t* p01 = &src.data[(static_cast<size_t>(y1) * src.width + x0) * 3];
            const uint8_t* p11 = &src.data[(static_cast<size_t>(y1) * src.width + x1) * 3];
            uint8_t* dp = &dst.data[(static_cast<size_t>(y) * dst_w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                float v = (p00[c] * (1.0f - wx) + p10[c] * wx) * (1.0f - wy) +
                          (p01[c] * (1.0f - wx) + p11[c] * wx) * wy;
                dp[c] = static_cast<uint8_t>(std::max(0, std::min(255, static_cast<int>(std::round(v)))));
            }
        }
    }
    return dst;
}

ImageRgb crop_rgb_rect(const ImageRgb& src, int x0, int y0, int x1, int y1) {
    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(src.width, x1);
    y1 = std::min(src.height, y1);
    ImageRgb dst;
    if (x1 <= x0 || y1 <= y0) return dst;
    dst.width = x1 - x0;
    dst.height = y1 - y0;
    dst.data.resize(static_cast<size_t>(dst.width) * dst.height * 3);
    for (int y = 0; y < dst.height; ++y) {
        std::memcpy(&dst.data[static_cast<size_t>(y) * dst.width * 3],
                    &src.data[(static_cast<size_t>(y0 + y) * src.width + x0) * 3],
                    static_cast<size_t>(dst.width) * 3);
    }
    return dst;
}

bool contains_any(const std::string& hay, std::initializer_list<const char*> needles) {
    for (const char* n : needles) {
        if (hay.find(n) != std::string::npos) return true;
    }
    return false;
}

bool is_cjk_char_utf8(const std::string& s) {
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            uint32_t cp = ((c & 0x0F) << 12) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 2]) & 0x3F);
            if (cp >= 0x4E00 && cp <= 0x9FFF) return true;
            i += 3;
        } else if ((c & 0xE0) == 0xC0) {
            i += 2;
        } else if ((c & 0xF8) == 0xF0) {
            i += 4;
        } else {
            i += 1;
        }
    }
    return false;
}

size_t utf8_char_count(const std::string& s) {
    size_t cnt = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0x80) == 0) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1;
        ++cnt;
    }
    return cnt;
}

std::string normalize_ascii_digits_and_dots(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size();) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c <= 0x7F) {
            if (c >= '0' && c <= '9') {
                out.push_back(static_cast<char>(c));
            } else if (c == '.' || c == ',') {
                out.push_back('.');
            } else {
                out.push_back(static_cast<char>(c));
            }
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < raw.size()) {
            uint32_t cp = ((c & 0x1F) << 6) |
                          (static_cast<unsigned char>(raw[i + 1]) & 0x3F);
            if (cp == 0x00B7) { // ·
                out.push_back('.');
            } else {
                out.append(raw, i, 2);
            }
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < raw.size()) {
            uint32_t cp = ((c & 0x0F) << 12) |
                          ((static_cast<unsigned char>(raw[i + 1]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(raw[i + 2]) & 0x3F);
            if (cp >= 0xFF10 && cp <= 0xFF19) { // Full-width ０..９
                out.push_back(static_cast<char>('0' + (cp - 0xFF10)));
            } else if (cp == 0x3002 || cp == 0x3001 || cp == 0xFF0C ||
                       cp == 0xFF0E || cp == 0x2022 || cp == 0x2027) {
                // 。 、 ， ． • ‧
                out.push_back('.');
            } else {
                out.append(raw, i, 3);
            }
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < raw.size()) {
            out.append(raw, i, 4);
            i += 4;
        } else {
            out.push_back(raw[i]);
            i += 1;
        }
    }
    return out;
}

bool parse_number_string(const std::string& raw, double& out_val) {
    std::string norm = normalize_ascii_digits_and_dots(raw);
    std::string cleaned;
    for (char ch : norm) {
        if (ch >= '0' && ch <= '9') {
            cleaned.push_back(ch);
        } else if (ch == '.') {
            // Collapse repeated consecutive dots
            if (cleaned.empty() || cleaned.back() != '.') {
                cleaned.push_back('.');
            }
        }
    }
    if (cleaned.empty()) return false;
    // Strip trailing dots
    while (!cleaned.empty() && cleaned.back() == '.') cleaned.pop_back();
    if (cleaned.empty()) return false;
    // Convert leading ".5" -> "0.5" instead of stripping leading dot!
    if (cleaned.front() == '.') {
        if (cleaned.size() == 1) return false;
        cleaned.insert(cleaned.begin(), '0');
    }
    char* endp = nullptr;
    double v = std::strtod(cleaned.c_str(), &endp);
    if (endp == cleaned.c_str() || *endp != '\0' || !std::isfinite(v)) return false;
    out_val = v;
    return true;
}

bool is_vertical_one_str(const std::string& s) {
    return s == "1" || s == "l" || s == "I" || s == "|" ||
           s == "/" || s == "!" || s == "丨" || s == "！";
}

struct DigitTopology {
    bool looks_like_open_four = false;
    bool has_lower_enclosed_loop = false;
    float upper_two_stems_ratio = 0.0f;
};

DigitTopology analyze_digit_topology(const ImageRgb& canvas_96x48) {
    DigitTopology res;
    if (!canvas_96x48.valid()) return res;
    const int cw = canvas_96x48.width;
    const int ch = canvas_96x48.height;
    std::vector<float> ink(static_cast<size_t>(cw) * ch, 0.0f);
    int minx = cw, maxx = -1, miny = ch, maxy = -1;
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            const uint8_t* p = &canvas_96x48.data[(static_cast<size_t>(y) * cw + x) * 3];
            float gray = (p[0] * 77.0f + p[1] * 150.0f + p[2] * 29.0f) / 256.0f;
            float v = std::max(0.0f, std::min(1.0f, (255.0f - gray) / 255.0f));
            ink[static_cast<size_t>(y) * cw + x] = v;
            if (v > 0.16f) {
                minx = std::min(minx, x); maxx = std::max(maxx, x);
                miny = std::min(miny, y); maxy = std::max(maxy, y);
            }
        }
    }
    if (maxx < minx || maxy < miny) return res;
    int bw = maxx - minx + 1;
    int bh = maxy - miny + 1;
    if (bh < 10 || bw < 6) return res;

    // Measure fraction of rows in upper zone [0.18*bh .. 0.46*bh] with >=2 ink runs separated by >=2px
    int uy0 = miny + static_cast<int>(std::floor(0.18f * bh));
    int uy1 = miny + static_cast<int>(std::ceil(0.46f * bh));
    uy0 = std::max(0, std::min(ch - 1, uy0));
    uy1 = std::max(uy0, std::min(ch - 1, uy1));
    int total_urows = uy1 - uy0 + 1;
    int two_stem_rows = 0;
    for (int y = uy0; y <= uy1; ++y) {
        int runs = 0;
        int gap_since_run = 999;
        bool in_run = false;
        for (int x = minx; x <= maxx; ++x) {
            float v = ink[static_cast<size_t>(y) * cw + x];
            if (v > 0.16f) {
                if (!in_run) {
                    if (runs == 0 || gap_since_run >= 2) {
                        ++runs;
                    }
                    in_run = true;
                }
                gap_since_run = 0;
            } else if (v <= 0.12f) {
                if (in_run) {
                    in_run = false;
                }
                ++gap_since_run;
            } else {
                // Soft edge pixel: keep current run/gap conservatively
                if (!in_run) ++gap_since_run;
            }
        }
        if (runs >= 2) ++two_stem_rows;
    }
    res.upper_two_stems_ratio = (total_urows > 0)
        ? (static_cast<float>(two_stem_rows) / static_cast<float>(total_urows))
        : 0.0f;

    // Flood-fill background (v <= 0.14f) from a 1px exterior frame around bbox
    int fx0 = std::max(0, minx - 1);
    int fy0 = std::max(0, miny - 1);
    int fx1 = std::min(cw - 1, maxx + 1);
    int fy1 = std::min(ch - 1, maxy + 1);
    int fw = fx1 - fx0 + 1;
    int fh = fy1 - fy0 + 1;
    std::vector<uint8_t> vis(static_cast<size_t>(fw) * fh, 0);
    std::vector<int> q;
    auto push_bg = [&](int gx, int gy) {
        int lx = gx - fx0;
        int ly = gy - fy0;
        size_t idx = static_cast<size_t>(ly) * fw + lx;
        if (!vis[idx] && ink[static_cast<size_t>(gy) * cw + gx] <= 0.14f) {
            vis[idx] = 1;
            q.push_back(static_cast<int>(idx));
        }
    };
    for (int gx = fx0; gx <= fx1; ++gx) {
        push_bg(gx, fy0);
        push_bg(gx, fy1);
    }
    for (int gy = fy0; gy <= fy1; ++gy) {
        push_bg(fx0, gy);
        push_bg(fx1, gy);
    }
    size_t qh = 0;
    while (qh < q.size()) {
        int p = q[qh++];
        int ly = p / fw, lx = p % fw;
        const int dxs[4] = {-1, 1, 0, 0};
        const int dys[4] = {0, 0, -1, 1};
        for (int k = 0; k < 4; ++k) {
            int nlx = lx + dxs[k], nly = ly + dys[k];
            if (nlx < 0 || nlx >= fw || nly < 0 || nly >= fh) continue;
            size_t ni = static_cast<size_t>(nly) * fw + nlx;
            if (!vis[ni] && ink[static_cast<size_t>(fy0 + nly) * cw + (fx0 + nlx)] <= 0.14f) {
                vis[ni] = 1;
                q.push_back(static_cast<int>(ni));
            }
        }
    }
    for (int ly = 0; ly < fh; ++ly) {
        for (int lx = 0; lx < fw; ++lx) {
            size_t ni = static_cast<size_t>(ly) * fw + lx;
            if (vis[ni] || ink[static_cast<size_t>(fy0 + ly) * cw + (fx0 + lx)] > 0.14f) continue;
            // Trace unvisited interior background hole
            std::vector<int> hq;
            vis[ni] = 1;
            hq.push_back(static_cast<int>(ni));
            double sum_gy = 0.0;
            size_t hh = 0;
            while (hh < hq.size()) {
                int hp = hq[hh++];
                int hly = hp / fw, hlx = hp % fw;
                sum_gy += static_cast<double>(fy0 + hly);
                const int dxs[4] = {-1, 1, 0, 0};
                const int dys[4] = {0, 0, -1, 1};
                for (int k = 0; k < 4; ++k) {
                    int nlx = hlx + dxs[k], nly = hly + dys[k];
                    if (nlx < 0 || nlx >= fw || nly < 0 || nly >= fh) continue;
                    size_t nni = static_cast<size_t>(nly) * fw + nlx;
                    if (!vis[nni] && ink[static_cast<size_t>(fy0 + nly) * cw + (fx0 + nlx)] <= 0.14f) {
                        vis[nni] = 1;
                        hq.push_back(static_cast<int>(nni));
                    }
                }
            }
            if (hq.size() >= 4 && (sum_gy / hq.size()) >= static_cast<double>(miny) + 0.40 * bh) {
                res.has_lower_enclosed_loop = true;
                break;
            }
        }
        if (res.has_lower_enclosed_loop) break;
    }

    res.looks_like_open_four = (res.upper_two_stems_ratio >= 0.30f && !res.has_lower_enclosed_loop);
    return res;
}

bool parse_year_month_text(const std::string& raw,
                           int& out_year,
                           bool& got_year,
                           int& out_month,
                           bool& got_month) {
    std::string s = normalize_ascii_digits_and_dots(raw);
    bool any = false;

    // Look for "20XX ... 年 ... M ... 月"
    for (size_t i = 0; i + 4 <= s.size(); ++i) {
        if (s[i] == '2' && s[i + 1] == '0' &&
            s[i + 2] >= '0' && s[i + 2] <= '9' &&
            s[i + 3] >= '0' && s[i + 3] <= '9') {
            int y = std::atoi(s.substr(i, 4).c_str());
            size_t j = i + 4;
            while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
            if (j + 3 <= s.size() && s.compare(j, 3, "年") == 0) {
                out_year = y;
                got_year = true;
                any = true;
                j += 3;
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                if (j < s.size() && s[j] >= '0' && s[j] <= '9') {
                    size_t k = j;
                    while (k < s.size() && k - j < 2 && s[k] >= '0' && s[k] <= '9') ++k;
                    int m = std::atoi(s.substr(j, k - j).c_str());
                    while (k < s.size() && (s[k] == ' ' || s[k] == '\t')) ++k;
                    if (m >= 1 && m <= 12 && k + 3 <= s.size() && s.compare(k, 3, "月") == 0) {
                        out_month = m;
                        got_month = true;
                    }
                }
            }
        }
    }

    // Fallback: "(0?[1-9]|1[0-2]) * 月" anywhere in text
    if (!got_month) {
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') continue;
            if (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') continue;
            size_t k = i;
            while (k < s.size() && s[k] >= '0' && s[k] <= '9') ++k;
            if (k - i <= 2) {
                int m = std::atoi(s.substr(i, k - i).c_str());
                size_t j = k;
                while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
                if (m >= 1 && m <= 12 && j + 3 <= s.size() && s.compare(j, 3, "月") == 0) {
                    out_month = m;
                    got_month = true;
                    any = true;
                    break;
                }
            }
        }
    }
    return any;
}

std::vector<int> extract_months_from_week_header(const std::string& raw) {
    std::string s = normalize_ascii_digits_and_dots(raw);
    std::vector<int> months;
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') continue;
        if (i > 0 && s[i - 1] >= '0' && s[i - 1] <= '9') continue;
        size_t k = i;
        while (k < s.size() && s[k] >= '0' && s[k] <= '9') ++k;
        if (k - i > 2 || k >= s.size() || s[k] != '.') continue;
        int m = std::atoi(s.substr(i, k - i).c_str());
        size_t d0 = k + 1;
        if (d0 >= s.size() || s[d0] < '0' || s[d0] > '9') continue;
        size_t d1 = d0;
        while (d1 < s.size() && s[d1] >= '0' && s[d1] <= '9') ++d1;
        // Skip two-decimal weight values like "1.05" (where day starts with 0 and len==2)
        if (d1 - d0 == 2 && s[d0] == '0') continue;
        int d = std::atoi(s.substr(d0, d1 - d0).c_str());
        if (m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            months.push_back(m);
        }
    }
    return months;
}

bool build_mnist12_patch(const ImageRgb& canvas_96x48, float out_patch[28][28]) {
    std::memset(out_patch, 0, sizeof(float) * 28 * 28);
    if (!canvas_96x48.valid()) return false;
    const int cw = canvas_96x48.width;
    const int ch = canvas_96x48.height;
    std::vector<float> ink(static_cast<size_t>(cw) * ch, 0.0f);

    int minx = cw, maxx = -1, miny = ch, maxy = -1;
    for (int y = 0; y < ch; ++y) {
        for (int x = 0; x < cw; ++x) {
            const uint8_t* p = &canvas_96x48.data[(static_cast<size_t>(y) * cw + x) * 3];
            float gray = (p[0] * 77.0f + p[1] * 150.0f + p[2] * 29.0f) / 256.0f;
            float v = std::max(0.0f, std::min(1.0f, (255.0f - gray) / 255.0f));
            ink[static_cast<size_t>(y) * cw + x] = v;
            if (v > 0.12f) {
                minx = std::min(minx, x); maxx = std::max(maxx, x);
                miny = std::min(miny, y); maxy = std::max(maxy, y);
            }
        }
    }
    if (maxx < minx || maxy < miny) {
        for (int y = 0; y < ch; ++y) {
            for (int x = 0; x < cw; ++x) {
                if (ink[static_cast<size_t>(y) * cw + x] > 0.02f) {
                    minx = std::min(minx, x); maxx = std::max(maxx, x);
                    miny = std::min(miny, y); maxy = std::max(maxy, y);
                }
            }
        }
    }
    if (maxx < minx || maxy < miny) return false;

    int bw = maxx - minx + 1;
    int bh = maxy - miny + 1;
    float scale = 20.0f / static_cast<float>(std::max(bw, bh));
    int dw = std::max(1, std::min(20, static_cast<int>(std::round(bw * scale))));
    int dh = std::max(1, std::min(20, static_cast<int>(std::round(bh * scale))));

    std::vector<float> scaled(static_cast<size_t>(dw) * dh, 0.0f);
    float max_val = 0.0f;
    for (int y = 0; y < dh; ++y) {
        float sy = miny + (y + 0.5f) * static_cast<float>(bh) / dh - 0.5f;
        int y0 = std::max(0, std::min(ch - 1, static_cast<int>(std::floor(sy))));
        int y1 = std::min(ch - 1, y0 + 1);
        float wy = std::max(0.0f, sy - y0);
        for (int x = 0; x < dw; ++x) {
            float sx = minx + (x + 0.5f) * static_cast<float>(bw) / dw - 0.5f;
            int x0 = std::max(0, std::min(cw - 1, static_cast<int>(std::floor(sx))));
            int x1 = std::min(cw - 1, x0 + 1);
            float wx = std::max(0.0f, sx - x0);
            float v00 = ink[static_cast<size_t>(y0) * cw + x0];
            float v10 = ink[static_cast<size_t>(y0) * cw + x1];
            float v01 = ink[static_cast<size_t>(y1) * cw + x0];
            float v11 = ink[static_cast<size_t>(y1) * cw + x1];
            float v = (v00 * (1.0f - wx) + v10 * wx) * (1.0f - wy) +
                      (v01 * (1.0f - wx) + v11 * wx) * wy;
            scaled[static_cast<size_t>(y) * dw + x] = v;
            max_val = std::max(max_val, v);
        }
    }
    if (max_val > 0.15f && max_val < 0.92f) {
        float gain = 0.95f / max_val;
        for (float& v : scaled) v = std::min(1.0f, v * gain);
    }

    // Center by center-of-mass in 28x28
    double sum_m = 0.0, sum_x = 0.0, sum_y = 0.0;
    for (int y = 0; y < dh; ++y) {
        for (int x = 0; x < dw; ++x) {
            float v = scaled[static_cast<size_t>(y) * dw + x];
            sum_m += v;
            sum_x += x * v;
            sum_y += y * v;
        }
    }
    float cx = (sum_m > 1e-5) ? static_cast<float>(sum_x / sum_m) : (dw - 1) * 0.5f;
    float cy = (sum_m > 1e-5) ? static_cast<float>(sum_y / sum_m) : (dh - 1) * 0.5f;
    int ox = static_cast<int>(std::round(13.5f - cx));
    int oy = static_cast<int>(std::round(13.5f - cy));

    for (int y = 0; y < dh; ++y) {
        int dy = oy + y;
        if (dy < 0 || dy >= 28) continue;
        for (int x = 0; x < dw; ++x) {
            int dx = ox + x;
            if (dx < 0 || dx >= 28) continue;
            out_patch[dy][dx] = scaled[static_cast<size_t>(y) * dw + x];
        }
    }
    return true;
}

bool run_mnist12_inference(const ImageRgb& canvas_96x48,
                           int& out_digit,
                           float& out_prob,
                           float& out_second_prob,
                           float out_probs[10]) {
    float in[28][28];
    if (!build_mnist12_patch(canvas_96x48, in)) {
        out_digit = -1;
        out_prob = 0.0f;
        out_second_prob = 0.0f;
        std::memset(out_probs, 0, sizeof(float) * 10);
        return false;
    }

    // Conv1: 5x5, pad=2, 1->8 + Bias + ReLU -> 8x28x28
    float c1[8][28][28];
    for (int oc = 0; oc < 8; ++oc) {
        float bias = mnist12::kParameter6[oc];
        for (int y = 0; y < 28; ++y) {
            for (int x = 0; x < 28; ++x) {
                float s = bias;
                for (int ky = 0; ky < 5; ++ky) {
                    int iy = y + ky - 2;
                    if (iy < 0 || iy >= 28) continue;
                    for (int kx = 0; kx < 5; ++kx) {
                        int ix = x + kx - 2;
                        if (ix < 0 || ix >= 28) continue;
                        s += in[iy][ix] * mnist12::kParameter5[(oc * 5 + ky) * 5 + kx];
                    }
                }
                c1[oc][y][x] = (s > 0.0f) ? s : 0.0f;
            }
        }
    }

    // MaxPool1: 2x2, stride=2 -> 8x14x14
    float m1[8][14][14];
    for (int c = 0; c < 8; ++c) {
        for (int y = 0; y < 14; ++y) {
            for (int x = 0; x < 14; ++x) {
                float v00 = c1[c][y * 2][x * 2];
                float v01 = c1[c][y * 2][x * 2 + 1];
                float v10 = c1[c][y * 2 + 1][x * 2];
                float v11 = c1[c][y * 2 + 1][x * 2 + 1];
                m1[c][y][x] = std::max(std::max(v00, v01), std::max(v10, v11));
            }
        }
    }

    // Conv2: 5x5, pad=2, 8->16 + Bias + ReLU -> 16x14x14
    float c2[16][14][14];
    for (int oc = 0; oc < 16; ++oc) {
        float bias = mnist12::kParameter88[oc];
        for (int y = 0; y < 14; ++y) {
            for (int x = 0; x < 14; ++x) {
                float s = bias;
                for (int ic = 0; ic < 8; ++ic) {
                    for (int ky = 0; ky < 5; ++ky) {
                        int iy = y + ky - 2;
                        if (iy < 0 || iy >= 14) continue;
                        for (int kx = 0; kx < 5; ++kx) {
                            int ix = x + kx - 2;
                            if (ix < 0 || ix >= 14) continue;
                            s += m1[ic][iy][ix] *
                                 mnist12::kParameter87[((oc * 8 + ic) * 5 + ky) * 5 + kx];
                        }
                    }
                }
                c2[oc][y][x] = (s > 0.0f) ? s : 0.0f;
            }
        }
    }

    // MaxPool2: 3x3, stride=3, pad=0 -> 16x4x4
    float m2[16][4][4];
    for (int c = 0; c < 16; ++c) {
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                float mx = -1e9f;
                for (int dy = 0; dy < 3; ++dy) {
                    for (int dx = 0; dx < 3; ++dx) {
                        mx = std::max(mx, c2[c][y * 3 + dy][x * 3 + dx]);
                    }
                }
                m2[c][y][x] = mx;
            }
        }
    }

    // Reshape(256) -> MatMul(256, 10) + Bias -> Softmax[10]
    float logits[10];
    for (int k = 0; k < 10; ++k) {
        logits[k] = mnist12::kParameter194[k];
    }
    for (int idx = 0; idx < 256; ++idx) {
        int c = idx / 16;
        int rem = idx % 16;
        int y = rem / 4;
        int x = rem % 4;
        float feat = m2[c][y][x];
        if (feat == 0.0f) continue;
        const float* wrow = &mnist12::kParameter193[idx * 10];
        for (int k = 0; k < 10; ++k) {
            logits[k] += feat * wrow[k];
        }
    }

    float max_lg = logits[0];
    for (int k = 1; k < 10; ++k) max_lg = std::max(max_lg, logits[k]);
    float sum_exp = 0.0f;
    for (int k = 0; k < 10; ++k) {
        out_probs[k] = std::exp(logits[k] - max_lg);
        sum_exp += out_probs[k];
    }
    int best_d = 0;
    float best_p = -1.0f;
    float second_p = -1.0f;
    for (int k = 0; k < 10; ++k) {
        out_probs[k] /= std::max(1e-9f, sum_exp);
        if (out_probs[k] > best_p) {
            second_p = best_p;
            best_p = out_probs[k];
            best_d = k;
        } else if (out_probs[k] > second_p) {
            second_p = out_probs[k];
        }
    }
    out_digit = best_d;
    out_prob = best_p;
    out_second_prob = std::max(0.0f, second_p);
    return true;
}

bool parse_coeff_number(const std::string& raw, double& out_val) {
    // Fix common OCR omissions like "110" or "005" when dot got swallowed by underline,
    // or "l.05" / "I.1"
    std::string mapped;
    for (size_t i = 0; i < raw.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(raw[i]);
        if (c >= '0' && c <= '9') {
            mapped.push_back(static_cast<char>(c));
        } else if (c == '.' || c == ',') {
            mapped.push_back('.');
        } else if (c == 'l' || c == 'I' || c == '|') {
            mapped.push_back('1');
        } else if (c == 'O' || c == 'o') {
            mapped.push_back('0');
        }
    }
    if (mapped.empty()) return false;
    if (mapped.find('.') == std::string::npos) {
        // Coeffs are typically 0.xx or 1.xx; if 3 digits starting with 0 or 1, insert decimal
        if (mapped.size() == 3 && (mapped[0] == '0' || mapped[0] == '1')) {
            mapped.insert(mapped.begin() + 1, '.');
        } else if (mapped.size() == 2 && (mapped[0] == '0' || mapped[0] == '1')) {
            mapped.insert(mapped.begin() + 1, '.');
        }
    }
    return parse_number_string(mapped, out_val);
}

} // namespace

struct OcrEngine::Impl {
    ncnn::Net det_net;
    ncnn::Net rec_net;
    std::vector<uint8_t> det_param_buf;
    std::vector<uint8_t> det_bin_buf;
    std::vector<uint8_t> rec_param_buf;
    std::vector<uint8_t> rec_bin_buf;

    // Dict indices (+1 shifted for CTC output column, where 0 is CTC blank)
    std::vector<std::pair<int, char>> numeric_tokens; // (col_index_in_out, char)
    std::vector<std::pair<int, char>> shape_digit_tokens; // non-digit tokens easily confused with 1/3/0

    void build_dict_maps() {
        numeric_tokens.clear();
        shape_digit_tokens.clear();
        for (int i = 0; i < character_dict_size; ++i) {
            std::string tok = character_dict[i];
            int col = i + 1;
            if (tok.size() == 1 && tok[0] >= '0' && tok[0] <= '9') {
                numeric_tokens.push_back({col, tok[0]});
            } else if (tok == ".") {
                numeric_tokens.push_back({col, '.'});
            } else if (tok == "l" || tok == "I" || tok == "|" || tok == "/" ||
                       tok == "!" || tok == "丨" || tok == "！") {
                shape_digit_tokens.push_back({col, '1'});
            } else if (tok == "·" || tok == "。" || tok == "、" || tok == "," || tok == "，") {
                shape_digit_tokens.push_back({col, '.'});
            } else if (tok == "Z" || tok == "z") {
                shape_digit_tokens.push_back({col, '2'});
            } else if (tok == "了" || tok == "ろ") {
                shape_digit_tokens.push_back({col, '3'});
            } else if (tok == "O" || tok == "o" || tok == "D") {
                shape_digit_tokens.push_back({col, '0'});
            }
        }
    }

    bool load_nets(std::string& out_err) {
        det_net.clear();
        rec_net.clear();

        auto configure = [](ncnn::Net& net) {
            net.opt.use_vulkan_compute = false;
            net.opt.use_fp16_packed = false;
            net.opt.use_fp16_storage = false;
            net.opt.use_fp16_arithmetic = false;
            net.opt.num_threads = 1;
        };
        configure(det_net);
        configure(rec_net);

        // ncnn load_param_mem expects null-terminated string buffer
        std::vector<uint8_t> det_p = det_param_buf;
        if (det_p.empty() || det_p.back() != '\0') det_p.push_back('\0');
        std::vector<uint8_t> rec_p = rec_param_buf;
        if (rec_p.empty() || rec_p.back() != '\0') rec_p.push_back('\0');

        if (det_net.load_param_mem(reinterpret_cast<const char*>(det_p.data())) != 0 ||
            det_net.load_model(det_bin_buf.data()) == 0) {
            out_err = "文字检测模型加载失败";
            return false;
        }
        if (rec_net.load_param_mem(reinterpret_cast<const char*>(rec_p.data())) != 0 ||
            rec_net.load_model(rec_bin_buf.data()) == 0) {
            out_err = "文字识别模型加载失败";
            return false;
        }

        build_dict_maps();
        return true;
    }

    void run_rec_raw(const ImageRgb& rgb_48h, ncnn::Mat& out_logits) const {
        if (!rgb_48h.valid()) return;
        ncnn::Mat in = ncnn::Mat::from_pixels(
            rgb_48h.data.data(), ncnn::Mat::PIXEL_RGB2BGR, rgb_48h.width, rgb_48h.height);
        const float mean_vals[3] = {127.5f, 127.5f, 127.5f};
        const float norm_vals[3] = {1.0f / 127.5f, 1.0f / 127.5f, 1.0f / 127.5f};
        in.substract_mean_normalize(mean_vals, norm_vals);

        ncnn::Extractor ex = rec_net.create_extractor();
        ex.input("in0", in);
        ex.extract("out0", out_logits);
    }

    bool rec_general_line(const ImageRgb& rgb, std::string& out_text, float& out_conf) const {
        out_text.clear();
        out_conf = 0.0f;
        if (!rgb.valid() || rgb.width < 2 || rgb.height < 2) return false;

        int target_h = 48;
        int target_w = std::max(16, static_cast<int>(std::round(rgb.width * (48.0f / rgb.height))));
        // Add subtle side white padding when crop is narrow so CTC doesn't drop edge glyphs
        ImageRgb scaled = resize_rgb_bilinear(rgb, target_w, target_h);
        int pad_x = (target_w < 64) ? 12 : 4;
        ImageRgb padded;
        padded.width = target_w + 2 * pad_x;
        padded.height = target_h;
        padded.data.assign(static_cast<size_t>(padded.width) * padded.height * 3, 255);
        for (int y = 0; y < target_h; ++y) {
            std::memcpy(&padded.data[(static_cast<size_t>(y) * padded.width + pad_x) * 3],
                        &scaled.data[static_cast<size_t>(y) * scaled.width * 3],
                        static_cast<size_t>(scaled.width) * 3);
        }

        ncnn::Mat out;
        run_rec_raw(padded, out);
        if (out.empty() || out.h <= 0 || out.w <= 1) return false;

        int last_tok = 0;
        float score_sum = 0.0f;
        int char_cnt = 0;
        for (int t = 0; t < out.h; ++t) {
            const float* row = out.row(t);
            int best_idx = 0;
            float best_val = row[0];
            for (int j = 1; j < out.w; ++j) {
                if (row[j] > best_val) {
                    best_val = row[j];
                    best_idx = j;
                }
            }
            if (best_idx == last_tok) continue;
            last_tok = best_idx;
            if (best_idx <= 0) continue;
            int dict_id = best_idx - 1;
            if (dict_id >= 0 && dict_id < character_dict_size) {
                out_text += character_dict[dict_id];
                // Convert logit to approximate softmax confidence over top candidates
                score_sum += std::max(0.0f, std::min(1.0f, best_val));
                ++char_cnt;
            }
        }
        if (char_cnt > 0) {
            out_conf = score_sum / char_cnt;
        }
        return !out_text.empty();
    }

    std::vector<RectF> run_det_boxes(const ImageRgb& rgb, int max_side) const {
        std::vector<RectF> boxes;
        if (!rgb.valid()) return boxes;

        float scale = 1.0f;
        int w = rgb.width, h = rgb.height;
        if (std::max(w, h) > max_side) {
            scale = static_cast<float>(max_side) / std::max(w, h);
            w = std::max(32, static_cast<int>(std::round(w * scale)));
            h = std::max(32, static_cast<int>(std::round(h * scale)));
        }
        ImageRgb resized = (w == rgb.width && h == rgb.height) ? rgb : resize_rgb_bilinear(rgb, w, h);

        int pad_w = ((w + 31) / 32) * 32 - w;
        int pad_h = ((h + 31) / 32) * 32 - h;
        int pw = w + pad_w;
        int ph = h + pad_h;
        ImageRgb canvas;
        canvas.width = pw;
        canvas.height = ph;
        canvas.data.assign(static_cast<size_t>(pw) * ph * 3, 114);
        int ox = pad_w / 2, oy = pad_h / 2;
        for (int y = 0; y < h; ++y) {
            std::memcpy(&canvas.data[(static_cast<size_t>(oy + y) * pw + ox) * 3],
                        &resized.data[static_cast<size_t>(y) * w * 3],
                        static_cast<size_t>(w) * 3);
        }

        ncnn::Mat in = ncnn::Mat::from_pixels(canvas.data.data(), ncnn::Mat::PIXEL_RGB2BGR, pw, ph);
        const float mean_vals[3] = {0.485f * 255.f, 0.456f * 255.f, 0.406f * 255.f};
        const float norm_vals[3] = {1.0f / 0.229f / 255.f, 1.0f / 0.224f / 255.f, 1.0f / 0.225f / 255.f};
        in.substract_mean_normalize(mean_vals, norm_vals);

        ncnn::Extractor ex = det_net.create_extractor();
        ex.input("in0", in);
        ncnn::Mat out;
        ex.extract("out0", out);
        if (out.empty() || out.w <= 0 || out.h <= 0) return boxes;

        int bw = out.w, bh = out.h;
        const float* prob = out.channel(0);
        std::vector<uint8_t> bin(static_cast<size_t>(bw) * bh, 0);
        for (size_t i = 0; i < bin.size(); ++i) {
            if (prob[i] > 0.30f) bin[i] = 1;
        }
        std::vector<uint8_t> vis(bin.size(), 0);
        std::vector<int> q;
        for (int y = 0; y < bh; ++y) {
            for (int x = 0; x < bw; ++x) {
                size_t idx = static_cast<size_t>(y) * bw + x;
                if (!bin[idx] || vis[idx]) continue;
                vis[idx] = 1;
                q.clear();
                q.push_back(static_cast<int>(idx));
                int minx = x, maxx = x, miny = y, maxy = y;
                float psum = 0.0f;
                size_t head = 0;
                while (head < q.size()) {
                    int p = q[head++];
                    int py = p / bw, px = p % bw;
                    minx = std::min(minx, px); maxx = std::max(maxx, px);
                    miny = std::min(miny, py); maxy = std::max(maxy, py);
                    psum += prob[p];
                    for (int dy = -1; dy <= 1; ++dy) {
                        int ny = py + dy;
                        if (ny < 0 || ny >= bh) continue;
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = px + dx;
                            if (nx < 0 || nx >= bw) continue;
                            size_t ni = static_cast<size_t>(ny) * bw + nx;
                            if (bin[ni] && !vis[ni]) {
                                vis[ni] = 1;
                                q.push_back(static_cast<int>(ni));
                            }
                        }
                    }
                }
                if (psum / q.size() < 0.50f) continue;
                int rx0 = static_cast<int>(std::floor((minx - 3 - ox) / scale));
                int ry0 = static_cast<int>(std::floor((miny - 3 - oy) / scale));
                int rx1 = static_cast<int>(std::ceil((maxx + 4 - ox) / scale));
                int ry1 = static_cast<int>(std::ceil((maxy + 4 - oy) / scale));
                if (rx1 - rx0 >= 8 && ry1 - ry0 >= 8) {
                    boxes.push_back({static_cast<float>(rx0), static_cast<float>(ry0),
                                     static_cast<float>(rx1 - rx0), static_cast<float>(ry1 - ry0)});
                }
            }
        }
        return boxes;
    }
};

OcrEngine::OcrEngine() : impl_(new Impl()) {}
OcrEngine::~OcrEngine() { delete impl_; }

bool OcrEngine::init_from_dir(const std::string& model_dir, std::string& out_err) {
    ready_ = false;
    if (!read_binary_file(join_path(model_dir, "PP_OCRv5_mobile_det.ncnn.param"), impl_->det_param_buf) ||
        !read_binary_file(join_path(model_dir, "PP_OCRv5_mobile_det.ncnn.bin"), impl_->det_bin_buf) ||
        !read_binary_file(join_path(model_dir, "PP_OCRv5_mobile_rec.ncnn.param"), impl_->rec_param_buf) ||
        !read_binary_file(join_path(model_dir, "PP_OCRv5_mobile_rec.ncnn.bin"), impl_->rec_bin_buf)) {
        out_err = "缺少 OCR 模型文件，请确认 models 目录完整";
        return false;
    }
    ready_ = impl_->load_nets(out_err);
    return ready_;
}

bool OcrEngine::init_from_memory(const std::vector<uint8_t>& det_param,
                                 const std::vector<uint8_t>& det_bin,
                                 const std::vector<uint8_t>& rec_param,
                                 const std::vector<uint8_t>& rec_bin,
                                 std::string& out_err) {
    ready_ = false;
    impl_->det_param_buf = det_param;
    impl_->det_bin_buf = det_bin;
    impl_->rec_param_buf = rec_param;
    impl_->rec_bin_buf = rec_bin;
    ready_ = impl_->load_nets(out_err);
    return ready_;
}

bool OcrEngine::recognize_text_line(const ImageRgb& rgb, std::string& out_text, float& out_conf) const {
    if (!ready_) return false;
    return impl_->rec_general_line(rgb, out_text, out_conf);
}

std::vector<TextBoxOcr> OcrEngine::detect_and_recognize(const ImageRgb& rgb, int max_side) const {
    std::vector<TextBoxOcr> out;
    if (!ready_) return out;
    auto boxes = impl_->run_det_boxes(rgb, max_side);
    for (const auto& b : boxes) {
        if (b.w < b.h * 0.9f) continue; // focus on horizontal text boxes
        ImageRgb sub = crop_rgb_rect(rgb, static_cast<int>(b.x), static_cast<int>(b.y),
                                     static_cast<int>(b.x + b.w), static_cast<int>(b.y + b.h));
        std::string txt;
        float conf = 0.0f;
        if (impl_->rec_general_line(sub, txt, conf) && !txt.empty()) {
            out.push_back({b, txt, conf});
        }
    }
    return out;
}

ImageRgb OcrEngine::auto_orient_table_photo(const ImageRgb& src, int& out_rot_deg) const {
    out_rot_deg = 0;
    if (!src.valid()) return src;
    int rough_q = estimate_rough_rotation_quarters(src);
    if (rough_q == 0 || !ready_) {
        out_rot_deg = rough_q * 90;
        return rotate_image_90cw(src, rough_q);
    }

    // Portrait photo: compare 90 CCW (3) vs 90 CW (1) using top-region horizontal Chinese/number recognition
    auto score_candidate = [&](const ImageRgb& cand) -> int {
        int s = 0;
        auto boxes = impl_->run_det_boxes(cand, 640);
        int checked = 0;
        for (const auto& b : boxes) {
            if (b.w <= b.h * 1.15f) continue;
            ImageRgb sub = crop_rgb_rect(cand, static_cast<int>(b.x), static_cast<int>(b.y),
                                         static_cast<int>(b.x + b.w), static_cast<int>(b.y + b.h));
            std::string txt;
            float conf = 0.0f;
            if (impl_->rec_general_line(sub, txt, conf) && conf > 0.55f) {
                if (contains_any(txt, {"工作量", "系数", "姓名", "序号", "职称", "职务", "第", "护士", "统计"})) {
                    s += 100;
                } else if (is_cjk_char_utf8(txt)) {
                    s += 10;
                }
            }
            if (++checked >= 10) break;
        }
        return s;
    };

    ImageRgb cand_ccw = rotate_image_90cw(src, 3);
    ImageRgb cand_cw = rotate_image_90cw(src, 1);
    int s_ccw = score_candidate(cand_ccw);
    int s_cw = score_candidate(cand_cw);
    if (s_cw > s_ccw) {
        out_rot_deg = 90;
        return cand_cw;
    }
    out_rot_deg = 270;
    return cand_ccw;
}

void OcrEngine::recognize_digit_from_analysis(const CellInkAnalysis& ink, WorkloadCell& cell) const {
    cell.ink_cat = ink.category;

    if (ink.category == InkCategory::Empty) {
        cell.kind = ValueKind::Blank;
        cell.value = 0.0;
        cell.ocr_kind = ValueKind::Blank;
        cell.ocr_value = 0.0;
        cell.confidence = 0.99f;
        cell.status = ReviewStatus::AutoConfirmed;
        cell.review_reason.clear();
        return;
    }

    if (ink.category == InkCategory::SuspectErased) {
        cell.kind = ValueKind::Blank;
        cell.value = 0.0;
        cell.ocr_kind = ValueKind::Blank;
        cell.ocr_value = 0.0;
        cell.confidence = 0.35f;
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "疑似擦除或偏淡";
        return;
    }

    ncnn::Mat out;
    impl_->run_rec_raw(ink.prepared_digit_canvas, out);
    if (out.empty() || out.h <= 0 || out.w <= 1) {
        cell.kind = ValueKind::Blank;
        cell.value = 0.0;
        cell.confidence = 0.1f;
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "识别失败";
        return;
    }

    // 1. Decode pure numeric tokens {blank, '0'..'9', '.'} alongside shape fallbacks and global non-digit scores
    std::string unconstrained;
    std::string num_str;
    std::string pure_num_str;
    int last_u = 0;
    int last_n = 0;
    int last_pure = 0;
    float min_margin = 999.0f;
    float top_prob_sum = 0.0f;
    int num_chars = 0;
    float pure_prob_sum = 0.0f;
    int pure_chars = 0;
    float u_max_prob = 0.0f;
    float max_other_non_shape_prob = 0.0f;
    bool ctc_saw_vertical_token = false;

    for (int t = 0; t < out.h; ++t) {
        const float* row = out.row(t);

        // Unconstrained argmax
        int u_best = 0;
        for (int j = 1; j < out.w; ++j) {
            if (row[j] > row[u_best]) u_best = j;
        }
        if (u_best > 0) {
            u_max_prob = std::max(u_max_prob, row[u_best]);
        }
        if (u_best != last_u) {
            last_u = u_best;
            if (u_best > 0 && u_best - 1 < character_dict_size) {
                std::string tok = character_dict[u_best - 1];
                unconstrained += tok;
                if (is_vertical_one_str(tok)) ctc_saw_vertical_token = true;
            }
        }

        // Constrained pure numeric subset: blank(0) + numeric_tokens
        float blank_logit = row[0];
        int p_best_col = 0;
        char p_best_ch = '\0';
        float p_best_logit = blank_logit;
        float p_second_logit = -1e9f;
        bool u_is_numeric = false;

        for (const auto& pair : impl_->numeric_tokens) {
            int col = pair.first;
            if (col >= out.w) continue;
            if (col == u_best) u_is_numeric = true;
            float lg = row[col];
            if (lg > p_best_logit) {
                p_second_logit = p_best_logit;
                p_best_logit = lg;
                p_best_col = col;
                p_best_ch = pair.second;
            } else if (lg > p_second_logit) {
                p_second_logit = lg;
            }
        }
        if (blank_logit < p_best_logit && blank_logit > p_second_logit) {
            p_second_logit = blank_logit;
        }

        if (p_best_col != last_pure) {
            last_pure = p_best_col;
            if (p_best_col > 0 && p_best_ch != '\0') {
                pure_num_str.push_back(p_best_ch);
                pure_prob_sum += std::max(0.0f, std::min(1.0f, p_best_logit));
                ++pure_chars;
            }
        }

        // Check if unconstrained picked a shape-similar symbol or truly unrelated non-digit symbol
        int n_best_col = p_best_col;
        char n_best_ch = p_best_ch;
        float n_best_logit = p_best_logit;
        float n_second_logit = p_second_logit;
        bool u_is_shape = false;
        if (u_best > 0) {
            for (const auto& sp : impl_->shape_digit_tokens) {
                if (sp.first == u_best) {
                    u_is_shape = true;
                    if (n_best_col == 0 && row[u_best] > blank_logit) {
                        n_best_col = u_best;
                        n_best_ch = sp.second;
                        n_best_logit = row[u_best];
                        n_second_logit = blank_logit;
                        if (sp.second == '1') ctc_saw_vertical_token = true;
                    }
                    break;
                }
            }
        }
        if (u_best > 0 && !u_is_numeric && !u_is_shape) {
            max_other_non_shape_prob = std::max(max_other_non_shape_prob, row[u_best]);
        }

        if (n_best_col != last_n) {
            last_n = n_best_col;
            if (n_best_col > 0 && n_best_ch != '\0') {
                num_str.push_back(n_best_ch);
                float margin = n_best_logit - n_second_logit;
                min_margin = std::min(min_margin, margin);
                top_prob_sum += std::max(0.0f, std::min(1.0f, n_best_logit));
                ++num_chars;
            }
        }
    }

    // Check if unconstrained preserved a decimal dot (e.g. "0·5" or ".5" or "1。5") better than num_str
    double unconst_val = 0.0;
    bool unconst_parsed = parse_number_string(unconstrained, unconst_val);
    double ctc_val = 0.0;
    bool ctc_parsed = parse_number_string(num_str, ctc_val);
    if (unconst_parsed && std::fabs(unconst_val - std::round(unconst_val)) > 1e-4 &&
        (!ctc_parsed || std::fabs(ctc_val - std::round(ctc_val)) < 1e-4)) {
        ctc_val = unconst_val;
        ctc_parsed = true;
        std::ostringstream oss;
        oss << ctc_val;
        num_str = oss.str();
    }

    // Prevent single thin vertical '1' from splitting into "11" in CTC
    if (ctc_parsed && num_str == "11" && ink.tall_thin_single_stroke &&
        ink.bbox_h > 0 && ink.bbox_w < ink.bbox_h * 0.65f) {
        num_str = "1";
        ctc_val = 1.0;
    }

    float ctc_conf = (num_chars > 0) ? (top_prob_sum / num_chars) : 0.0f;
    float pure_ctc_conf = (pure_chars > 0) ? (pure_prob_sum / pure_chars) : 0.0f;
    if (min_margin > 100.0f) min_margin = 0.0f;

    // 2. Run MNIST-12 on 28x28 normalized foreground patch
    int mnist_digit = -1;
    float mnist_prob = 0.0f;
    float mnist_second = 0.0f;
    float mnist_probs[10]{};
    bool has_mnist = run_mnist12_inference(
        ink.prepared_digit_canvas, mnist_digit, mnist_prob, mnist_second, mnist_probs);

    // 3. Fuse CTC + MNIST-12 + stroke geometry + topology
    DigitTopology topo = analyze_digit_topology(ink.prepared_digit_canvas);
    bool is_multi_or_decimal = ctc_parsed &&
        (std::fabs(ctc_val - std::round(ctc_val)) > 1e-4 ||
         (ctc_val >= 10.0 && (ink.component_count >= 2 || pure_ctc_conf >= 0.65f ||
                              (ink.bbox_h > 0 && ink.bbox_w >= ink.bbox_h * 0.65f))));

    double final_val = 0.0;
    bool has_val = false;
    float final_conf = 0.0f;
    bool ambiguous = false;
    std::string ambig_reason;
    bool is_standalone_zero = false;

    if (is_multi_or_decimal) {
        final_val = ctc_val;
        has_val = true;
        final_conf = ctc_conf;
        if (!(ctc_conf >= 0.68f && min_margin >= 0.18f && max_other_non_shape_prob < 0.40f)) {
            ambiguous = true;
            ambig_reason = "置信偏低";
        }
    } else {
        int ctc_digit = -1;
        if (ctc_parsed && ctc_val >= 0.0 && ctc_val <= 9.0 &&
            std::fabs(ctc_val - std::round(ctc_val)) < 1e-6) {
            ctc_digit = static_cast<int>(std::round(ctc_val));
        }

        bool ctc_is_one = (ctc_digit == 1) || ctc_saw_vertical_token || is_vertical_one_str(unconstrained);
        if (ctc_digit < 0 && unconstrained == "了") {
            ctc_digit = 3;
            ctc_conf = std::max(ctc_conf, 0.50f);
        }
        bool thin_supported_one = ink.tall_thin_single_stroke &&
                                  (ink.rel_peak >= 0.45f || ink.strong_px >= 25) &&
                                  ((ctc_digit == 1 && pure_ctc_conf >= 0.20f) ||
                                   (ctc_is_one && u_max_prob >= 0.28f));
        bool strong_one_token = thin_supported_one ||
                                (ctc_digit == 1 && pure_ctc_conf >= 0.35f) ||
                                (ctc_is_one && u_max_prob >= 0.42f);

        if (ctc_is_one && (ink.tall_thin_single_stroke || (has_mnist && mnist_digit == 1))) {
            // Vertical-stroke '1' supported by CTC token + geometry or MNIST-12
            final_val = 1.0;
            has_val = true;
            float p1 = has_mnist ? mnist_probs[1] : 0.0f;
            bool strong_conflict = (has_mnist && mnist_digit != 1 && mnist_prob >= 0.72f && !ink.tall_thin_single_stroke);
            bool weak_mnist_for_wide = (has_mnist && !ink.tall_thin_single_stroke && (mnist_digit != 1 || mnist_prob < 0.75f));
            if (strong_conflict || !strong_one_token || weak_mnist_for_wide) {
                ambiguous = true;
                ambig_reason = strong_conflict ? "数字待核" : "置信偏低";
                final_conf = std::min(0.62f, std::max({pure_ctc_conf, u_max_prob * 0.70f, p1}));
            } else {
                final_conf = std::max({pure_ctc_conf, p1, ink.tall_thin_single_stroke ? 0.78f : 0.65f});
            }
        } else if (ctc_digit < 0 && ink.tall_thin_single_stroke) {
            // CTC had no '1' or vertical token: flag NeedsReview even for vertical strokes
            if (!has_mnist || mnist_digit == 1 || mnist_probs[1] >= 0.25f) {
                final_val = 1.0;
                has_val = true;
                final_conf = has_mnist ? std::min(0.62f, std::max(0.35f, mnist_probs[1])) : 0.45f;
            } else {
                final_val = static_cast<double>(mnist_digit);
                has_val = true;
                final_conf = std::min(0.62f, mnist_prob);
            }
            ambiguous = true;
            ambig_reason = "置信偏低";
        } else if (topo.looks_like_open_four &&
                   ((ctc_digit == 6) || (has_mnist && mnist_digit == 6) ||
                    unconstrained == "U" || unconstrained == "u")) {
            // Open-top two-stem '4' cannot safely become unflagged '6' or collapse into 'U'
            final_val = 4.0;
            has_val = true;
            if (ctc_digit == 4 && has_mnist && mnist_digit == 4 && pure_ctc_conf >= 0.60f) {
                final_conf = std::max(0.70f, pure_ctc_conf);
            } else {
                final_conf = std::min(0.60f, std::max(0.35f, pure_ctc_conf));
                ambiguous = true;
                ambig_reason = "数字待核";
            }
        } else if (ctc_digit >= 0 && has_mnist && ctc_digit == mnist_digit) {
            // CTC and MNIST-12 agree: auto-confirm only when both branches give solid, unambiguous support
            final_val = static_cast<double>(ctc_digit);
            has_val = true;
            is_standalone_zero = (ctc_digit == 0);
            float ctc_support = pure_ctc_conf;
            if (ctc_digit == 2 && (unconstrained == "Z" || unconstrained == "z") && u_max_prob >= 0.65f) {
                ctc_support = std::max(ctc_support, 0.75f * u_max_prob);
            } else if (ctc_digit == 3 && unconstrained == "了" && u_max_prob >= 0.65f) {
                ctc_support = std::max(ctc_support, 0.65f * u_max_prob);
            } else if (ctc_digit == 0 && (unconstrained == "O" || unconstrained == "o") && u_max_prob >= 0.65f) {
                ctc_support = std::max(ctc_support, 0.75f * u_max_prob);
            }
            float mnist_margin = mnist_prob - mnist_second;
            bool both_strong = (mnist_prob >= 0.78f && mnist_margin >= 0.42f &&
                                ((ctc_support >= 0.26f) ||
                                 (ctc_support >= 0.08f && mnist_prob >= 0.96f && mnist_margin >= 0.88f && max_other_non_shape_prob < 0.80f)));
            bool ctc_dominant_agreed = (pure_ctc_conf >= 0.85f && mnist_prob >= 0.55f && mnist_margin >= 0.15f && max_other_non_shape_prob < 0.35f);
            bool no_strong_conflict = !(pure_ctc_conf < 0.20f && max_other_non_shape_prob >= 0.50f && ctc_support < 0.35f && mnist_prob < 0.97f);
            if ((!both_strong && !ctc_dominant_agreed) || !no_strong_conflict) {
                ambiguous = true;
                ambig_reason = "置信偏低";
                final_conf = std::min(std::max(0.15f, std::max(pure_ctc_conf, ctc_support)), mnist_prob);
            } else {
                final_conf = std::max(ctc_support, mnist_prob * 0.95f);
            }
        } else if (ctc_digit >= 0 && has_mnist) {
            // CTC and MNIST-12 disagree: ALWAYS require manual review
            if (mnist_digit == 1 && (ctc_is_one || ink.tall_thin_single_stroke) && mnist_prob >= 0.75f && strong_one_token) {
                final_val = 1.0;
                has_val = true;
                final_conf = mnist_prob;
            } else {
                int chosen = (pure_ctc_conf >= 0.25f || mnist_prob < 0.80f) ? ctc_digit : mnist_digit;
                final_val = static_cast<double>(chosen);
                has_val = true;
                final_conf = std::min(0.62f, std::max(pure_ctc_conf, mnist_prob * 0.65f));
                is_standalone_zero = (chosen == 0);
                ambiguous = true;
                ambig_reason = "数字待核";
            }
        } else if (ctc_digit >= 0) {
            final_val = static_cast<double>(ctc_digit);
            has_val = true;
            final_conf = pure_ctc_conf;
            is_standalone_zero = (ctc_digit == 0);
            if (min_margin < 0.15f || pure_ctc_conf < 0.55f || max_other_non_shape_prob >= 0.40f) {
                ambiguous = true;
                ambig_reason = "置信偏低";
            }
        } else if (has_mnist && mnist_prob >= 0.45f) {
            final_val = static_cast<double>(mnist_digit);
            has_val = true;
            final_conf = std::min(0.60f, mnist_prob);
            is_standalone_zero = (mnist_digit == 0);
            ambiguous = true;
            ambig_reason = "置信偏低";
        }
    }

    if (!has_val) {
        cell.kind = ValueKind::Blank;
        cell.value = 0.0;
        cell.ocr_kind = ValueKind::Blank;
        cell.ocr_value = 0.0;
        cell.confidence = 0.2f;
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "字迹模糊";
        return;
    }

    // Map tiny-ink standalone "0" (strong_px < 26) to Blank
    if (is_standalone_zero && ink.strong_px < 26 && ink.category != InkCategory::SuspectOverwrite) {
        cell.kind = ValueKind::Blank;
        cell.value = 0.0;
        cell.ocr_kind = ValueKind::Blank;
        cell.ocr_value = 0.0;
        cell.confidence = 0.92f;
        cell.status = ReviewStatus::AutoConfirmed;
        cell.review_reason.clear();
        return;
    }

    cell.kind = ValueKind::Number;
    cell.value = final_val;
    cell.ocr_kind = ValueKind::Number;
    cell.ocr_value = final_val;
    cell.confidence = std::max(0.0f, std::min(0.99f, final_conf));

    // Review triggers: SuspectOverwrite, faint ink, unusual > 7.0, weird decimals, or genuine ambiguity
    cell.status = ReviewStatus::AutoConfirmed;
    cell.review_reason.clear();

    if (ink.category == InkCategory::SuspectOverwrite) {
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "疑似涂改";
    } else if (ink.rel_peak < 0.35f || ink.strong_px < 12) {
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "字迹偏淡";
    } else if (final_val > 7.0) {
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "数值偏大";
    } else {
        double frac = std::fabs(final_val - std::round(final_val));
        if (frac > 1e-4 && std::fabs(frac - 0.5) > 1e-4) {
            cell.status = ReviewStatus::NeedsReview;
            cell.review_reason = "小数待核";
        } else if (ambiguous) {
            cell.status = ReviewStatus::NeedsReview;
            cell.review_reason = ambig_reason;
        }
    }
}

bool OcrEngine::process_workload_photo(const ImageRgb& working_img,
                                       int year,
                                       int month,
                                       const AppConfig& config,
                                       const std::array<Point2f, 4>* manual_corners,
                                       WorkloadSheet& out_sheet,
                                       std::string& out_err) const {
    if (!ready_) {
        out_err = "OCR 模型尚未加载";
        return false;
    }
    if (!working_img.valid()) {
        out_err = "图片无效";
        return false;
    }

    std::array<Point2f, 4> corners{};
    if (manual_corners) {
        corners = *manual_corners;
    } else {
        detect_table_corners(working_img, corners);
    }

    RectifiedGrid grid;
    if (!extract_rectified_table_grid(working_img, corners, 20, 6, grid, out_err)) {
        return false;
    }

    int total_row_bands = static_cast<int>(grid.row_y_base.size()) - 1;
    int total_col_bands = static_cast<int>(grid.col_x_base.size()) - 1;
    if (total_row_bands < 3 || total_col_bands < 6) {
        out_err = "表格行列数不足，请检查图片或调整四角";
        return false;
    }

    // 1. Identify header bands vs data bands by height, col-0 tokens, and cols 1..min(6, cols-1)
    struct RowBand {
        int r_idx;
        std::string ocr_name;
        float conf;
    };
    std::vector<RowBand> band_names(total_row_bands);
    for (int r = 0; r < total_row_bands; ++r) {
        band_names[r].r_idx = r;
        ImageRgb crop = grid.crop_cell_rect(r, 0, 2);
        impl_->rec_general_line(crop, band_names[r].ocr_name, band_names[r].conf);
    }

    std::vector<float> row_heights(total_row_bands, 0.0f);
    for (int r = 0; r < total_row_bands; ++r) {
        row_heights[r] = grid.row_y_base[r + 1] - grid.row_y_base[r];
    }
    float median_lower_row_h = 0.0f;
    {
        int start_l = std::min(total_row_bands - 1, (total_row_bands > 5) ? 2 : 1);
        std::vector<float> lower_hs(row_heights.begin() + start_l, row_heights.end());
        if (!lower_hs.empty()) {
            std::sort(lower_hs.begin(), lower_hs.end());
            median_lower_row_h = lower_hs[lower_hs.size() / 2];
        }
    }

    auto is_single_char_header_token = [](const std::string& norm) {
        return norm == "姓" || norm == "名" || norm == "周" ||
               norm == "份" || norm == "序" || norm == "号";
    };

    auto crop_rect_span = [&](int r0, int c0, int r1, int c1, int pad_px) -> ImageRgb {
        Point2f tl = grid.rect_intersection(r0, c0);
        Point2f tr = grid.rect_intersection(r0, c1);
        Point2f br = grid.rect_intersection(r1, c1);
        Point2f bl = grid.rect_intersection(r1, c0);
        int x0 = std::max(0, static_cast<int>(std::floor(std::min(tl.x, bl.x))) + pad_px);
        int x1 = std::min(grid.rectified_rgb.width, static_cast<int>(std::ceil(std::max(tr.x, br.x))) - pad_px);
        int y0 = std::max(0, static_cast<int>(std::floor(std::min(tl.y, tr.y))) + pad_px);
        int y1 = std::min(grid.rectified_rgb.height, static_cast<int>(std::ceil(std::max(bl.y, br.y))) - pad_px);
        return crop_rgb_rect(grid.rectified_rgb, x0, y0, x1, y1);
    };

    int last_header_row = -1;
    int scan_hdr_end = std::min(total_row_bands, 4);
    for (int r = 0; r < scan_hdr_end; ++r) {
        bool is_hdr = false;
        if (median_lower_row_h > 4.0f && row_heights[r] > 1.60f * median_lower_row_h) {
            is_hdr = true;
        }
        std::string norm = normalize_person_name(band_names[r].ocr_name);
        if (!is_hdr && !norm.empty()) {
            if (is_single_char_header_token(norm) ||
                contains_any(norm, {"姓名", "周份", "名份", "惟名份", "姓周份", "姓名份",
                                    "班次", "技术", "难度", "合计", "总分", "序号", "编号"})) {
                is_hdr = true;
            }
        }
        if (!is_hdr) {
            int max_c = std::min(6, total_col_bands - 1);
            for (int c = 1; c <= max_c; ++c) {
                ImageRgb hc = grid.crop_cell_rect(r, c, 1);
                std::string ht;
                float hconf = 0.0f;
                if (impl_->rec_general_line(hc, ht, hconf) &&
                    contains_any(ht, {"第一周", "第二周", "第1周", "第2周",
                                      "主", "治", "白", "夜", "技术", "难度", "1.05", "1.1", "1.2"})) {
                    is_hdr = true;
                    break;
                }
                if (hc.height > hc.width * 1.4f) {
                    ImageRgb hrot = rotate_image_90cw(hc, 3);
                    if (impl_->rec_general_line(hrot, ht, hconf) &&
                        contains_any(ht, {"主", "治", "白", "夜", "技术", "难度", "1.05", "1.1", "1.2"})) {
                        is_hdr = true;
                        break;
                    }
                }
            }
            if (!is_hdr && total_col_bands >= 6) {
                ImageRgb merged = crop_rect_span(r, 1, r + 1, std::min(total_col_bands, 6), 1);
                std::string mt;
                float mconf = 0.0f;
                if (impl_->rec_general_line(merged, mt, mconf) &&
                    contains_any(mt, {"第一周", "第二周", "第1周", "第2周"})) {
                    is_hdr = true;
                }
            }
        }
        if (is_hdr) {
            last_header_row = r;
        }
    }

    int first_data_row = -1;
    for (int r = last_header_row + 1; r < total_row_bands; ++r) {
        std::string norm = normalize_person_name(band_names[r].ocr_name);
        if (norm.empty()) continue; // skip leading blank rows before first real staff row
        if (r < 4 && (is_single_char_header_token(norm) || utf8_char_count(norm) < 2 ||
                      contains_any(norm, {"姓名", "周份", "名份", "惟名份", "班次", "技术", "难度", "合计", "总分"}))) {
            continue;
        }
        first_data_row = r;
        break;
    }
    if (first_data_row < 0) {
        first_data_row = std::max(last_header_row + 1, (total_row_bands > 10) ? 2 : 1);
        first_data_row = std::min(first_data_row, std::max(0, total_row_bands - 1));
    }

    int end_data_row = total_row_bands;
    for (int r = total_row_bands - 1; r > first_data_row; --r) {
        std::string norm = normalize_person_name(band_names[r].ocr_name);
        if (contains_any(norm, {"合计", "总计", "汇总"}) ||
            (r == total_row_bands - 1 && norm.empty())) {
            end_data_row = r;
        } else {
            break;
        }
    }

    // 2. Determine weeks & active data columns (paper layout: Name | W * 5 cols | optional 7 summary cols)
    const int cols_per_week = 5;
    int data_cols_avail = total_col_bands - 1;
    int weeks = data_cols_avail / cols_per_week;

    // Check top header row for "合计" start column
    int summary_col_start = total_col_bands;
    if (first_data_row > 0) {
        for (int c = 16; c < total_col_bands; ++c) {
            ImageRgb hc = grid.crop_cell_rect(0, c, 1);
            std::string ht;
            float hconf = 0.0f;
            if (impl_->rec_general_line(hc, ht, hconf) && contains_any(ht, {"合计", "总分"})) {
                summary_col_start = c;
                break;
            }
        }
    }
    if (summary_col_start < total_col_bands) {
        weeks = std::max(1, (summary_col_start - 1) / cols_per_week);
    } else if (total_col_bands == 33 || total_col_bands == 28 || total_col_bands == 38) {
        // Standard paper sheet has 7 empty summary columns on the right
        weeks = (total_col_bands - 1 - 7) / cols_per_week;
    } else if (weeks > 5) {
        // Check if trailing columns are completely empty across all data rows
        int non_empty_last_col = 1;
        for (int c = 1; c < total_col_bands; ++c) {
            bool has_ink = false;
            for (int r = first_data_row; r < std::min(end_data_row, first_data_row + 5); ++r) {
                CellInkAnalysis st = analyze_digit_cell(grid, r, c, 100.0f);
                if (st.max_diff >= 35.0f) {
                    has_ink = true;
                    break;
                }
            }
            if (has_ink) non_empty_last_col = c;
        }
        int needed_weeks = std::max(4, std::min(6, (non_empty_last_col + cols_per_week - 1) / cols_per_week));
        if (needed_weeks * cols_per_week + 1 <= total_col_bands) {
            weeks = needed_weeks;
        }
    }
    weeks = std::max(1, std::min(6, weeks));
    int active_data_cols = std::min(total_col_bands - 1, weeks * cols_per_week);

    // 2b. Auto-recognize Year & Month from Photo
    int detected_year = year;
    int detected_month = month;
    bool got_year = false;
    bool got_month = false;

    // Primary #1: Banner region directly above top border corners[0]..corners[1] (~12-14% table height upward)
    {
        float table_h = 0.5f * (std::hypot(corners[3].x - corners[0].x, corners[3].y - corners[0].y) +
                                std::hypot(corners[2].x - corners[1].x, corners[2].y - corners[1].y));
        float top_w = std::hypot(corners[1].x - corners[0].x, corners[1].y - corners[0].y);
        float max_top_y = std::max(corners[0].y, corners[1].y);
        if (top_w > 60.0f && table_h > 40.0f && max_top_y >= 12.0f) {
            float up_h = std::max(24.0f, table_h * 0.14f);
            float dx = (corners[1].x - corners[0].x) / top_w;
            float dy = (corners[1].y - corners[0].y) / top_w;
            // Upward normal perpendicular to top edge
            float nx = dy;
            float ny = -dx;
            if (ny > 0.0f) { nx = -nx; ny = -ny; }
            auto clamp_pt = [&](Point2f p) -> Point2f {
                return {
                    std::max(0.0f, std::min(static_cast<float>(working_img.width - 1), p.x)),
                    std::max(0.0f, std::min(static_cast<float>(working_img.height - 1), p.y))
                };
            };
            Point2f b_tl = clamp_pt({corners[0].x + nx * up_h, corners[0].y + ny * up_h});
            Point2f b_tr = clamp_pt({corners[1].x + nx * up_h, corners[1].y + ny * up_h});
            Point2f b_br = clamp_pt(corners[1]);
            Point2f b_bl = clamp_pt(corners[0]);
            float span_y = std::max(b_bl.y - b_tl.y, b_br.y - b_tr.y);
            if (span_y >= 12.0f) {
                int bw = std::max(240, std::min(1600, static_cast<int>(std::round(top_w))));
                int bh = std::max(32, std::min(240, static_cast<int>(std::round(span_y * 1.15f))));
                ImageRgb banner = crop_quad_image(working_img, {b_tl, b_tr, b_br, b_bl}, bw, bh);
                if (banner.valid()) {
                    auto boxes = detect_and_recognize(banner, 1280);
                    for (const auto& tb : boxes) {
                        parse_year_month_text(tb.text, detected_year, got_year, detected_month, got_month);
                        if (got_year && got_month) break;
                    }
                    if (!got_year || !got_month) {
                        std::string btxt;
                        float bconf = 0.0f;
                        if (impl_->rec_general_line(banner, btxt, bconf)) {
                            parse_year_month_text(btxt, detected_year, got_year, detected_month, got_month);
                        }
                    }
                }
            }
        }
    }

    // Fallback #2: OCR merged 5-col week headers in row 0 ("第一周 8.1 - 8.2", "第二周 8.3 - 8.9")
    if (!got_month && first_data_row > 0) {
        std::vector<int> m_votes(13, 0);
        int w_scan = std::min(weeks, 4);
        for (int w = 0; w < w_scan; ++w) {
            int c0 = 1 + w * cols_per_week;
            int c1 = std::min(total_col_bands, c0 + cols_per_week);
            if (c1 <= c0) continue;
            ImageRgb wh = crop_rect_span(0, c0, 1, c1, 1);
            if (!wh.valid()) continue;
            auto wboxes = detect_and_recognize(wh, 640);
            for (const auto& wb : wboxes) {
                for (int m : extract_months_from_week_header(wb.text)) {
                    ++m_votes[m];
                }
            }
            std::string wtxt;
            float wconf = 0.0f;
            if (impl_->rec_general_line(wh, wtxt, wconf)) {
                for (int m : extract_months_from_week_header(wtxt)) {
                    ++m_votes[m];
                }
            }
        }
        int best_m = 0, best_cnt = 0;
        for (int m = 1; m <= 12; ++m) {
            if (m_votes[m] > best_cnt) {
                best_cnt = m_votes[m];
                best_m = m;
            }
        }
        if (best_m >= 1 && best_cnt > 0) {
            detected_month = best_m;
            got_month = true;
        }
    }

    out_sheet = WorkloadSheet{};
    out_sheet.year = detected_year;
    out_sheet.month = detected_month;
    out_sheet.weeks = weeks;
    out_sheet.week_labels = default_week_labels(detected_year, detected_month, weeks);
    out_sheet.column_shift_codes = {"A", "Z", "P", "Y", "T"};
    out_sheet.column_shift_names = {"A(主)", "治/白", "P", "夜", "技术难度"};

    out_sheet.geometry.img_width = working_img.width;
    out_sheet.geometry.img_height = working_img.height;
    out_sheet.geometry.corners = corners;
    out_sheet.geometry.grid_rows = end_data_row - first_data_row;
    out_sheet.geometry.grid_cols = 1 + active_data_cols;

    // 3. Compute table-wide ink reference across data cells
    float table_ref = compute_table_ink_reference(
        grid, first_data_row, end_data_row, 1, 1 + active_data_cols);

    // 4. Analyze ink & run multi-threaded digit OCR across all active cells
    const int n_rows = end_data_row - first_data_row;
    out_sheet.rows.resize(static_cast<size_t>(n_rows));

    struct Task {
        int r_idx;
        int c_idx;
        int grid_r;
        int grid_c;
        CellInkAnalysis ink;
    };
    std::vector<Task> tasks;
    tasks.reserve(static_cast<size_t>(n_rows) * active_data_cols);

    for (int ri = 0; ri < n_rows; ++ri) {
        int gr = first_data_row + ri;
        WorkloadRow& wrow = out_sheet.rows[ri];
        wrow.row = ri;
        std::string norm_nm = normalize_person_name(band_names[gr].ocr_name);
        wrow.ocr_name = norm_nm.empty() ? band_names[gr].ocr_name : norm_nm;
        wrow.staff_name = wrow.ocr_name;
        wrow.name_confidence = band_names[gr].conf;
        wrow.name_needs_review = (wrow.staff_name.empty() || band_names[gr].conf < 0.65f);
        wrow.name_quad = grid.cell_orig_quad(gr, 0);
        wrow.cells.resize(static_cast<size_t>(active_data_cols));

        for (int ci = 0; ci < active_data_cols; ++ci) {
            int gc = 1 + ci;
            WorkloadCell& cell = wrow.cells[ci];
            cell.row = ri;
            cell.week = ci / cols_per_week;
            cell.shift_col = ci % cols_per_week;
            cell.orig_quad = grid.cell_orig_quad(gr, gc);

            // Check whether this shift column's rule is enabled; skip default OCR on disabled columns
            // UNLESS clear ink is present (so miswritten marks in disabled columns still alert the user!)
            CellInkAnalysis ink = analyze_digit_cell(grid, gr, gc, table_ref);
            tasks.push_back({ri, ci, gr, gc, std::move(ink)});
        }
    }

    unsigned int n_threads = std::min(4u, std::max(1u, std::thread::hardware_concurrency()));
    std::vector<std::thread> workers;
    const size_t total_tasks = tasks.size();
    for (unsigned int tid = 0; tid < n_threads; ++tid) {
        workers.emplace_back([&, tid]() {
            for (size_t idx = tid; idx < total_tasks; idx += n_threads) {
                const auto& t = tasks[idx];
                WorkloadCell& cell = out_sheet.rows[t.r_idx].cells[t.c_idx];
                int rule_idx = find_shift_rule_index(
                    config.shift_rules,
                    out_sheet.column_shift_codes[cell.shift_col],
                    out_sheet.column_shift_names[cell.shift_col]);
                bool shift_enabled = (rule_idx < 0) ? true : config.shift_rules[rule_idx].enabled;

                if (!shift_enabled) {
                    // Disabled column (e.g. 技术难度): keep blank by default, but alert if strong ink spotted
                    cell.kind = ValueKind::Blank;
                    cell.value = 0.0;
                    cell.ink_cat = t.ink.category;
                    if (t.ink.category == InkCategory::ValidInk ||
                        t.ink.category == InkCategory::SuspectOverwrite) {
                        cell.status = ReviewStatus::NeedsReview;
                        cell.review_reason = "停用栏有字迹";
                    } else {
                        cell.status = ReviewStatus::AutoConfirmed;
                        cell.review_reason.clear();
                    }
                    continue;
                }
                recognize_digit_from_analysis(t.ink, cell);
            }
        });
    }
    for (auto& th : workers) th.join();

    // 5. Align recognized names against config.staff (if user has imported/configured staff names)
    if (!config.staff.empty()) {
        std::vector<std::string> dict;
        dict.reserve(config.staff.size());
        for (const auto& st : config.staff) {
            if (!st.name.empty()) dict.push_back(st.name);
        }
        apply_name_list_to_workload(out_sheet, dict);
    }

    return true;
}

bool OcrEngine::reocr_unlocked_cells(const ImageRgb& working_img,
                                     const std::array<Point2f, 4>& corners,
                                     WorkloadSheet& sheet,
                                     std::string& out_err) const {
    AppConfig tmp_cfg;
    tmp_cfg.shift_rules = default_shift_rules();
    WorkloadSheet fresh;
    if (!process_workload_photo(working_img, sheet.year, sheet.month, tmp_cfg, &corners, fresh, out_err)) {
        return false;
    }
    size_t n_rows = std::min(sheet.rows.size(), fresh.rows.size());
    for (size_t r = 0; r < n_rows; ++r) {
        sheet.rows[r].name_quad = fresh.rows[r].name_quad;
        size_t n_cells = std::min(sheet.rows[r].cells.size(), fresh.rows[r].cells.size());
        for (size_t c = 0; c < n_cells; ++c) {
            if (sheet.rows[r].cells[c].locked_by_user) {
                sheet.rows[r].cells[c].orig_quad = fresh.rows[r].cells[c].orig_quad;
            } else {
                sheet.rows[r].cells[c] = fresh.rows[r].cells[c];
            }
        }
    }
    sheet.geometry = fresh.geometry;
    return true;
}

bool OcrEngine::process_coeff_photo(const ImageRgb& src_img,
                                    const std::vector<std::string>& optional_name_dict,
                                    CoeffTableResult& out_res) const {
    out_res = CoeffTableResult{};
    if (!ready_) {
        out_res.error = "OCR 模型尚未加载";
        return false;
    }
    if (!src_img.valid()) {
        out_res.error = "图片无效";
        return false;
    }

    ImageRgb oriented = auto_orient_table_photo(src_img, out_res.applied_rotation_deg);
    std::array<Point2f, 4> corners{};
    detect_table_corners(oriented, corners);

    RectifiedGrid grid;
    if (!extract_rectified_table_grid(oriented, corners, 5, 4, grid, out_res.error)) {
        return false;
    }

    int total_rows = static_cast<int>(grid.row_y_base.size()) - 1;
    int total_cols = static_cast<int>(grid.col_x_base.size()) - 1;
    if (total_rows < 2 || total_cols < 5) {
        out_res.error = "系数表行列数不足，请重新对准拍照";
        return false;
    }

    // Locate header row and semantic columns (序号, 姓名, 职称系数, 职务系数, 总系数)
    int header_row = 0;
    int col_name = 1, col_title = 2, col_duty = 3, col_total = 4;
    for (int r = 0; r < std::min(3, total_rows); ++r) {
        int found_name = -1, found_title = -1, found_duty = -1, found_total = -1;
        for (int c = 0; c < std::min(7, total_cols); ++c) {
            ImageRgb hc = erase_bottom_underline(grid.crop_cell_rect(r, c, 2));
            std::string txt;
            float cf = 0.0f;
            if (impl_->rec_general_line(hc, txt, cf)) {
                if (contains_any(txt, {"姓名"})) found_name = c;
                else if (contains_any(txt, {"职称"})) found_title = c;
                else if (contains_any(txt, {"职务"})) found_duty = c;
                else if (contains_any(txt, {"总系数", "总"})) found_total = c;
            }
        }
        if (found_name >= 0 || found_title >= 0) {
            header_row = r;
            if (found_name >= 0) col_name = found_name;
            if (found_title >= 0) col_title = found_title;
            if (found_duty >= 0) col_duty = found_duty;
            if (found_total >= 0) col_total = found_total;
            break;
        }
    }

    for (int r = header_row + 1; r < total_rows; ++r) {
        ImageRgb name_crop = erase_bottom_underline(grid.crop_cell_rect(r, col_name, 4));
        std::string raw_name;
        float name_conf = 0.0f;
        impl_->rec_general_line(name_crop, raw_name, name_conf);
        std::string norm_name = normalize_person_name(raw_name);
        if (norm_name.empty() && r == total_rows - 1) continue;
        if (contains_any(norm_name, {"合计", "总计", "备注"})) break;
        if (norm_name.empty()) continue;

        auto read_coeff_col = [&](int col, double& val) -> bool {
            if (col < 0 || col >= total_cols) return false;
            ImageRgb cc = erase_bottom_underline(grid.crop_cell_rect(r, col, 4));
            std::string t;
            float cf = 0.0f;
            if (!impl_->rec_general_line(cc, t, cf)) return false;
            return parse_coeff_number(t, val);
        };

        double title_v = 1.0, duty_v = 0.0, total_v = 1.0;
        bool ok_t = read_coeff_col(col_title, title_v);
        bool ok_d = read_coeff_col(col_duty, duty_v);
        bool ok_tot = read_coeff_col(col_total, total_v);

        StaffCoeffRowOcr row_ocr;
        row_ocr.row = static_cast<int>(out_res.rows.size());
        row_ocr.ocr_name = norm_name;
        row_ocr.matched_name = norm_name;
        row_ocr.row_quad = grid.cell_orig_quad(r, col_name);

        if (ok_t && ok_d && ok_tot) {
            if (std::fabs((title_v + duty_v) - total_v) <= 0.015) {
                row_ocr.title_coeff = title_v;
                row_ocr.duty_coeff = duty_v;
                row_ocr.total_coeff = total_v;
                row_ocr.needs_review = false;
            } else if (duty_v > 0.9 && total_v < 3.0 && title_v < 3.0 && total_v >= title_v - 0.005) {
                row_ocr.title_coeff = title_v;
                row_ocr.duty_coeff = std::max(0.0, std::round((total_v - title_v) * 1000.0) / 1000.0);
                row_ocr.total_coeff = total_v;
                row_ocr.needs_review = true;
                row_ocr.review_reason = "职务系数已推算";
            } else if (title_v == 1.0 && total_v >= 1.04 && total_v <= 1.40 && duty_v < 0.50 && total_v > duty_v) {
                row_ocr.title_coeff = std::round((total_v - duty_v) * 1000.0) / 1000.0;
                row_ocr.duty_coeff = duty_v;
                row_ocr.total_coeff = total_v;
                row_ocr.needs_review = true;
                row_ocr.review_reason = "职称系数已推算";
            } else {
                row_ocr.title_coeff = title_v;
                row_ocr.duty_coeff = duty_v;
                row_ocr.total_coeff = title_v + duty_v;
                row_ocr.needs_review = true;
                row_ocr.review_reason = "系数之和待核";
            }
        } else if (ok_t && ok_tot && total_v >= title_v - 0.005) {
            row_ocr.title_coeff = title_v;
            row_ocr.duty_coeff = std::max(0.0, std::round((total_v - title_v) * 1000.0) / 1000.0);
            row_ocr.total_coeff = total_v;
            row_ocr.needs_review = true;
            row_ocr.review_reason = "职务系数已推算";
        } else {
            row_ocr.title_coeff = ok_t ? title_v : 1.0;
            row_ocr.duty_coeff = ok_d ? duty_v : 0.0;
            row_ocr.total_coeff = row_ocr.title_coeff + row_ocr.duty_coeff;
            row_ocr.needs_review = true;
            row_ocr.review_reason = "系数待核";
        }
        out_res.rows.push_back(std::move(row_ocr));
    }

    if (out_res.rows.empty()) {
        out_res.error = "未能识别到人员系数行";
        return false;
    }

    if (!optional_name_dict.empty()) {
        std::vector<std::string> raw_names;
        for (const auto& row : out_res.rows) raw_names.push_back(row.ocr_name);
        auto aligned = align_names_unique(raw_names, optional_name_dict, 0.48);
        for (size_t i = 0; i < out_res.rows.size(); ++i) {
            if (aligned[i].score >= 0.48) {
                out_res.rows[i].matched_name = aligned[i].matched_name;
            }
        }
    }

    out_res.ok = true;
    return true;
}

} // namespace nurseocr
