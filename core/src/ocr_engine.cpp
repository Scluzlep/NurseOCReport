#include "nurseocr/ocr_engine.h"
#include "nurseocr/calculator.h"
#include "nurseocr/image_io.h"
#include "nurseocr/name_doc_reader.h"

#include <net.h>
#include <ppocrv5_dict.h>

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

bool parse_number_string(const std::string& raw, double& out_val) {
    std::string cleaned;
    for (char ch : raw) {
        if (ch >= '0' && ch <= '9') cleaned.push_back(ch);
        else if (ch == '.' || ch == ',') cleaned.push_back('.');
    }
    if (cleaned.empty()) return false;
    // Remove leading/trailing dots
    while (!cleaned.empty() && cleaned.front() == '.') cleaned.erase(cleaned.begin());
    while (!cleaned.empty() && cleaned.back() == '.') cleaned.pop_back();
    if (cleaned.empty()) return false;
    char* endp = nullptr;
    double v = std::strtod(cleaned.c_str(), &endp);
    if (endp == cleaned.c_str() || *endp != '\0' || !std::isfinite(v)) return false;
    out_val = v;
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
            } else if (tok == "l" || tok == "I" || tok == "|" || tok == "/" || tok == "丨") {
                shape_digit_tokens.push_back({col, '1'});
            } else if (tok == "了" || tok == "z" || tok == "Z" || tok == "ろ") {
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
        cell.review_reason = "疑似擦除";
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

    // 1. Decode constrained numeric tokens {blank, '0'..'9', '.'} + shape fallbacks
    // Compute logits across numeric subset at each timestep
    std::string unconstrained;
    std::string num_str;
    int last_u = 0;
    int last_n = 0;
    float min_margin = 999.0f;
    float top_prob_sum = 0.0f;
    int num_chars = 0;

    for (int t = 0; t < out.h; ++t) {
        const float* row = out.row(t);

        // Unconstrained argmax
        int u_best = 0;
        for (int j = 1; j < out.w; ++j) {
            if (row[j] > row[u_best]) u_best = j;
        }
        if (u_best != last_u) {
            last_u = u_best;
            if (u_best > 0 && u_best - 1 < character_dict_size) {
                unconstrained += character_dict[u_best - 1];
            }
        }

        // Constrained numeric subset: blank(0) + numeric_tokens
        float blank_logit = row[0];
        int n_best_col = 0;
        char n_best_ch = '\0';
        float n_best_logit = blank_logit;
        float n_second_logit = -1e9f;

        for (const auto& pair : impl_->numeric_tokens) {
            int col = pair.first;
            if (col >= out.w) continue;
            float lg = row[col];
            if (lg > n_best_logit) {
                n_second_logit = n_best_logit;
                n_best_logit = lg;
                n_best_col = col;
                n_best_ch = pair.second;
            } else if (lg > n_second_logit) {
                n_second_logit = lg;
            }
        }
        if (blank_logit < n_best_logit && blank_logit > n_second_logit) {
            n_second_logit = blank_logit;
        }

        // Also check if unconstrained picked a shape-similar symbol strongly over blank
        if (n_best_col == 0 && u_best > 0) {
            for (const auto& sp : impl_->shape_digit_tokens) {
                if (sp.first == u_best && row[u_best] > blank_logit) {
                    n_best_col = u_best;
                    n_best_ch = sp.second;
                    n_best_logit = row[u_best];
                    break;
                }
            }
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

    // Geometry-assisted recovery if valid thin vertical stroke collapsed to empty in CTC
    bool inferred_thin_one = false;
    if (num_str.empty()) {
        if (unconstrained == "了" || unconstrained == "z" || unconstrained == "Z") {
            num_str = "3";
        } else if (unconstrained == "l" || unconstrained == "I" || unconstrained == "|" ||
                   unconstrained == "/" || unconstrained == "丨" || ink.tall_thin_single_stroke) {
            num_str = "1";
            inferred_thin_one = true;
        }
    }

    double parsed = 0.0;
    if (!parse_number_string(num_str, parsed)) {
        cell.kind = ValueKind::Blank;
        cell.value = 0.0;
        cell.confidence = 0.2f;
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "字迹模糊";
        return;
    }

    cell.kind = ValueKind::Number;
    cell.value = parsed;
    cell.ocr_kind = ValueKind::Number;
    cell.ocr_value = parsed;
    cell.confidence = (num_chars > 0) ? (top_prob_sum / num_chars) : 0.45f;
    if (inferred_thin_one) cell.confidence = std::min(cell.confidence, 0.55f);

    // Review triggers
    cell.status = ReviewStatus::AutoConfirmed;
    cell.review_reason.clear();

    if (ink.category == InkCategory::SuspectOverwrite) {
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "疑似涂改";
    } else if (ink.rel_peak < 0.45f || ink.strong_px < 16) {
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "字迹偏淡";
    } else if (parsed > 7.0) {
        cell.status = ReviewStatus::NeedsReview;
        cell.review_reason = "数值偏大";
    } else {
        double frac = std::fabs(parsed - std::round(parsed));
        if (frac > 1e-4 && std::fabs(frac - 0.5) > 1e-4) {
            cell.status = ReviewStatus::NeedsReview;
            cell.review_reason = "小数待核";
        } else if (inferred_thin_one || min_margin < 0.06f || cell.confidence < 0.42f) {
            cell.status = ReviewStatus::NeedsReview;
            cell.review_reason = "置信偏低";
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

    // 1. Identify header bands vs data bands by scanning col 0
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

    int first_data_row = -1;
    for (int r = 0; r < total_row_bands; ++r) {
        std::string norm = normalize_person_name(band_names[r].ocr_name);
        if (norm.empty()) continue;
        if (contains_any(norm, {"姓名", "周份", "班次", "主", "白", "夜", "技术", "合计", "总分"})) {
            continue;
        }
        first_data_row = r;
        break;
    }
    if (first_data_row < 0) {
        first_data_row = (total_row_bands > 10) ? 2 : 1;
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

    out_sheet = WorkloadSheet{};
    out_sheet.year = year;
    out_sheet.month = month;
    out_sheet.weeks = weeks;
    out_sheet.week_labels = default_week_labels(year, month, weeks);
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
