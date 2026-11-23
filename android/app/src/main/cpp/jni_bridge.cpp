#include <jni.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/bitmap.h>

#include "nurseocr/calculator.h"
#include "nurseocr/image_io.h"
#include "nurseocr/name_doc_reader.h"
#include "nurseocr/ocr_engine.h"
#include "nurseocr/storage.h"
#include "nurseocr/table_cv.h"
#include "nurseocr/xlsx_exporter.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace nurseocr;

namespace {

std::mutex g_mutex;
OcrEngine g_ocr;
AppConfig g_config = create_default_config();
ImageRgb g_working_img;
WorkloadSheet g_sheet;

std::string jstring_to_utf8(JNIEnv* env, jstring js) {
    if (!js) return "";
    const char* p = env->GetStringUTFChars(js, nullptr);
    std::string out(p ? p : "");
    if (p) env->ReleaseStringUTFChars(js, p);
    return out;
}

std::vector<uint8_t> jbytes_to_vec(JNIEnv* env, jbyteArray arr) {
    if (!arr) return {};
    jsize len = env->GetArrayLength(arr);
    if (len <= 0) return {};
    std::vector<uint8_t> out(static_cast<size_t>(len));
    env->GetByteArrayRegion(arr, 0, len, reinterpret_cast<jbyte*>(out.data()));
    return out;
}

bool read_asset_bytes(AAssetManager* mgr, const char* filename, std::vector<uint8_t>& out) {
    AAsset* asset = AAssetManager_open(mgr, filename, AASSET_MODE_BUFFER);
    if (!asset) return false;
    off_t len = AAsset_getLength(asset);
    if (len <= 0) {
        AAsset_close(asset);
        return false;
    }
    out.resize(static_cast<size_t>(len));
    int read_n = AAsset_read(asset, out.data(), static_cast<size_t>(len));
    AAsset_close(asset);
    return read_n == len;
}

void ensure_sheet_initialized_unlocked() {
    if (g_sheet.weeks <= 0) g_sheet.weeks = 5;
    if (g_sheet.column_shift_codes.empty() || g_sheet.column_shift_names.empty()) {
        g_sheet.column_shift_codes = {"A", "Z", "P", "Y", "T"};
        g_sheet.column_shift_names = {"A(主)", "治/白", "P", "夜", "技术难度"};
    }
    if (g_sheet.week_labels.empty()) {
        g_sheet.week_labels = default_week_labels(g_sheet.year, g_sheet.month, g_sheet.weeks);
    }
}

WorkloadRow make_blank_workload_row(int row_idx, const std::string& name) {
    int cols_per_week = static_cast<int>(g_sheet.column_shift_names.size());
    WorkloadRow wrow;
    wrow.row = row_idx;
    wrow.ocr_name = name;
    wrow.staff_name = name;
    wrow.name_confidence = 1.0f;
    wrow.name_needs_review = false;
    wrow.cells.reserve(static_cast<size_t>(g_sheet.weeks) * static_cast<size_t>(cols_per_week));
    for (int w = 0; w < g_sheet.weeks; ++w) {
        for (int sc = 0; sc < cols_per_week; ++sc) {
            WorkloadCell cell;
            cell.row = row_idx;
            cell.week = w;
            cell.shift_col = sc;
            cell.kind = ValueKind::Blank;
            cell.value = 0.0;
            cell.ocr_kind = ValueKind::Blank;
            cell.ocr_value = 0.0;
            cell.confidence = 1.0f;
            cell.ink_cat = InkCategory::Empty;
            cell.status = ReviewStatus::UserConfirmed;
            cell.locked_by_user = true;
            wrow.cells.push_back(std::move(cell));
        }
    }
    return wrow;
}

std::string build_workload_response_json(bool ok, const std::string& err) {
    nlohmann::json resp;
    resp["ok"] = ok;
    resp["error"] = err;
    if (ok) {
        SheetSummary sum = evaluate_workload(g_sheet, g_config);
        resp["year"] = g_sheet.year;
        resp["month"] = g_sheet.month;
        resp["sheet"] = nlohmann::json::parse(serialize_workload_sheet_json(g_sheet));
        resp["summary"] = nlohmann::json::parse(serialize_sheet_summary_json(sum));
    }
    return resp.dump();
}

void write_rgb_to_rgba8888_bitmap(const ImageRgb& src, JNIEnv* env, jobject bitmap) {
    if (!src.valid() || !bitmap) return;
    AndroidBitmapInfo info{};
    if (AndroidBitmap_getInfo(env, bitmap, &info) < 0) return;
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888) return;
    void* pixels = nullptr;
    if (AndroidBitmap_lockPixels(env, bitmap, &pixels) < 0 || !pixels) return;

    int dw = static_cast<int>(info.width);
    int dh = static_cast<int>(info.height);
    float sx_r = static_cast<float>(src.width) / std::max(1, dw);
    float sy_r = static_cast<float>(src.height) / std::max(1, dh);

    for (int y = 0; y < dh; ++y) {
        int sy = std::min(src.height - 1, std::max(0, static_cast<int>((y + 0.5f) * sy_r)));
        uint8_t* dst_row = static_cast<uint8_t*>(pixels) + static_cast<size_t>(y) * info.stride;
        const uint8_t* src_row = &src.data[static_cast<size_t>(sy) * src.width * 3];
        for (int x = 0; x < dw; ++x) {
            int sx = std::min(src.width - 1, std::max(0, static_cast<int>((x + 0.5f) * sx_r)));
            const uint8_t* sp = &src_row[sx * 3];
            uint8_t* dp = &dst_row[x * 4];
            dp[0] = sp[0];
            dp[1] = sp[1];
            dp[2] = sp[2];
            dp[3] = 255;
        }
    }
    AndroidBitmap_unlockPixels(env, bitmap);
}

} // namespace

extern "C" {

JNIEXPORT jboolean JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeInit(
    JNIEnv* env, jobject, jobject asset_manager) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_ocr.is_ready()) return JNI_TRUE;
    AAssetManager* mgr = AAssetManager_fromJava(env, asset_manager);
    if (!mgr) return JNI_FALSE;

    std::vector<uint8_t> dp, db, rp, rb;
    if (!read_asset_bytes(mgr, "PP_OCRv5_mobile_det.ncnn.param", dp) ||
        !read_asset_bytes(mgr, "PP_OCRv5_mobile_det.ncnn.bin", db) ||
        !read_asset_bytes(mgr, "PP_OCRv5_mobile_rec.ncnn.param", rp) ||
        !read_asset_bytes(mgr, "PP_OCRv5_mobile_rec.ncnn.bin", rb)) {
        return JNI_FALSE;
    }
    std::string err;
    return g_ocr.init_from_memory(dp, db, rp, rb, err) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeGetDefaultConfigJson(
    JNIEnv* env, jobject) {
    return env->NewStringUTF(serialize_app_config_json(create_default_config()).c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeSetConfigJson(
    JNIEnv* env, jobject, jstring config_json) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string s = jstring_to_utf8(env, config_json);
    std::string err;
    AppConfig parsed;
    if (!deserialize_app_config_json(s, parsed, err)) {
        return env->NewStringUTF(build_workload_response_json(false, err).c_str());
    }
    g_config = std::move(parsed);
    if (g_sheet.rows.empty()) {
        return env->NewStringUTF("{\"ok\":true}");
    }
    return env->NewStringUTF(build_workload_response_json(true, "").c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeParseNamesText(
    JNIEnv* env, jobject, jstring raw_text) {
    std::string raw = jstring_to_utf8(env, raw_text);
    std::vector<std::string> names = parse_names_from_plain_text(raw);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& n : names) {
        arr.push_back(n);
    }
    return env->NewStringUTF(arr.dump().c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeParseNamesBytes(
    JNIEnv* env, jobject, jstring filename_hint, jbyteArray file_bytes) {
    std::string hint = jstring_to_utf8(env, filename_hint);
    auto bytes = jbytes_to_vec(env, file_bytes);
    std::vector<std::string> names;
    std::string err;
    nlohmann::json resp;
    if (read_names_from_bytes(hint, bytes.data(), bytes.size(), names, err)) {
        resp["ok"] = true;
        resp["names"] = names;
    } else {
        resp["ok"] = false;
        resp["error"] = err;
    }
    return env->NewStringUTF(resp.dump().c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeAlignStaffWithNames(
    JNIEnv* env, jobject, jstring names_json_array) {
    std::lock_guard<std::mutex> lock(g_mutex);
    nlohmann::json resp;
    try {
        auto arr = nlohmann::json::parse(jstring_to_utf8(env, names_json_array));
        std::vector<std::string> dict;
        for (const auto& item : arr) {
            if (item.is_string()) dict.push_back(item.get<std::string>());
        }
        apply_name_list_to_staff(g_config.staff, dict);
        resp["ok"] = true;
        resp["config"] = nlohmann::json::parse(serialize_app_config_json(g_config));
        if (!g_sheet.rows.empty()) {
            apply_name_list_to_workload(g_sheet, dict);
            SheetSummary sum = evaluate_workload(g_sheet, g_config);
            resp["sheet"] = nlohmann::json::parse(serialize_workload_sheet_json(g_sheet));
            resp["summary"] = nlohmann::json::parse(serialize_sheet_summary_json(sum));
        }
    } catch (const std::exception& ex) {
        resp["ok"] = false;
        resp["error"] = ex.what();
    }
    return env->NewStringUTF(resp.dump().c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeApplyNamesToWorkload(
    JNIEnv* env, jobject, jstring names_json_array) {
    std::lock_guard<std::mutex> lock(g_mutex);
    try {
        auto arr = nlohmann::json::parse(jstring_to_utf8(env, names_json_array));
        std::vector<std::string> dict;
        for (const auto& item : arr) {
            if (item.is_string()) dict.push_back(item.get<std::string>());
        }
        if (g_sheet.rows.empty()) {
            ensure_sheet_initialized_unlocked();
            for (const auto& nm : dict) {
                std::string norm = normalize_person_name(nm);
                std::string final_nm = norm.empty() ? nm : norm;
                if (!final_nm.empty()) {
                    g_sheet.rows.push_back(
                        make_blank_workload_row(static_cast<int>(g_sheet.rows.size()), final_nm));
                }
            }
            g_sheet.geometry.grid_rows = static_cast<int>(g_sheet.rows.size());
        } else {
            apply_name_list_to_workload(g_sheet, dict);
        }
        return env->NewStringUTF(build_workload_response_json(true, "").c_str());
    } catch (const std::exception& ex) {
        return env->NewStringUTF(build_workload_response_json(false, ex.what()).c_str());
    }
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeProcessCoeffBytes(
    JNIEnv* env, jobject, jbyteArray img_bytes) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto bytes = jbytes_to_vec(env, img_bytes);
    ImageRgb img;
    std::string err;
    CoeffTableResult res;
    if (!decode_image_bytes(bytes.data(), bytes.size(), img, err)) {
        res.ok = false;
        res.error = err;
        return env->NewStringUTF(serialize_coeff_result_json(res).c_str());
    }
    std::vector<std::string> dict;
    for (const auto& s : g_config.staff) dict.push_back(s.name);
    g_ocr.process_coeff_photo(img, dict, res);
    if (res.ok) {
        g_config.staff.clear();
        for (const auto& r : res.rows) {
            g_config.staff.push_back({r.matched_name, r.title_coeff, r.duty_coeff});
        }
    }
    return env->NewStringUTF(serialize_coeff_result_json(res).c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeProcessWorkloadBytes(
    JNIEnv* env, jobject, jbyteArray img_bytes, jint year, jint month) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto bytes = jbytes_to_vec(env, img_bytes);
    ImageRgb raw;
    std::string err;
    if (!decode_image_bytes(bytes.data(), bytes.size(), raw, err)) {
        return env->NewStringUTF(build_workload_response_json(false, err).c_str());
    }
    int rot = 0;
    g_working_img = g_ocr.auto_orient_table_photo(raw, rot);
    WorkloadSheet next_sheet;
    if (!g_ocr.process_workload_photo(g_working_img, year, month, g_config, nullptr, next_sheet, err)) {
        return env->NewStringUTF(build_workload_response_json(false, err).c_str());
    }
    next_sheet.applied_rotation_deg = rot;
    g_sheet = std::move(next_sheet);
    return env->NewStringUTF(build_workload_response_json(true, "").c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeRotateWorkload(
    JNIEnv* env, jobject, jint quarter_turns_cw, jint year, jint month) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_working_img.valid()) {
        return env->NewStringUTF(build_workload_response_json(false, "尚未载入照片").c_str());
    }
    g_working_img = rotate_image_90cw(g_working_img, quarter_turns_cw);
    std::string err;
    WorkloadSheet next_sheet;
    if (!g_ocr.process_workload_photo(g_working_img, year, month, g_config, nullptr, next_sheet, err)) {
        return env->NewStringUTF(build_workload_response_json(false, err).c_str());
    }
    g_sheet = std::move(next_sheet);
    return env->NewStringUTF(build_workload_response_json(true, "").c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeUpdateWorkloadCorners(
    JNIEnv* env, jobject, jfloatArray flat8, jint year, jint month) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_working_img.valid()) {
        return env->NewStringUTF(build_workload_response_json(false, "尚未载入照片").c_str());
    }
    jfloat buf[8]{};
    if (!flat8 || env->GetArrayLength(flat8) < 8) {
        return env->NewStringUTF(build_workload_response_json(false, "角点参数无效").c_str());
    }
    env->GetFloatArrayRegion(flat8, 0, 8, buf);
    std::array<Point2f, 4> corners = {
        Point2f{buf[0], buf[1]}, Point2f{buf[2], buf[3]},
        Point2f{buf[4], buf[5]}, Point2f{buf[6], buf[7]}
    };
    g_sheet.year = year;
    g_sheet.month = month;
    std::string err;
    if (!g_ocr.reocr_unlocked_cells(g_working_img, corners, g_sheet, err)) {
        return env->NewStringUTF(build_workload_response_json(false, err).c_str());
    }
    return env->NewStringUTF(build_workload_response_json(true, "").c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeEditCell(
    JNIEnv* env, jobject, jint row, jint col_index, jboolean is_blank, jdouble value, jstring name_text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (row < 0 || row >= static_cast<int>(g_sheet.rows.size())) {
        return env->NewStringUTF(build_workload_response_json(false, "行号越界").c_str());
    }
    auto& wrow = g_sheet.rows[row];
    if (col_index < 0) {
        if (name_text) {
            std::string raw = jstring_to_utf8(env, name_text);
            std::string norm = normalize_person_name(raw);
            wrow.staff_name = norm.empty() ? raw : norm;
        }
        wrow.name_needs_review = false;
    } else if (col_index < static_cast<int>(wrow.cells.size())) {
        auto& cell = wrow.cells[col_index];
        if (value >= 0.0 || is_blank) {
            if (is_blank) {
                cell.kind = ValueKind::Blank;
                cell.value = 0.0;
            } else {
                cell.kind = ValueKind::Number;
                cell.value = value;
            }
        }
        cell.status = ReviewStatus::UserConfirmed;
        cell.review_reason.clear();
        cell.locked_by_user = true;
    }
    return env->NewStringUTF(build_workload_response_json(true, "").c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeDeleteWorkloadRow(
    JNIEnv* env, jobject, jint row_index) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (row_index < 0 || row_index >= static_cast<int>(g_sheet.rows.size())) {
        return env->NewStringUTF(build_workload_response_json(false, "行号越界").c_str());
    }
    g_sheet.rows.erase(g_sheet.rows.begin() + row_index);
    for (int r = 0; r < static_cast<int>(g_sheet.rows.size()); ++r) {
        g_sheet.rows[r].row = r;
        for (auto& cell : g_sheet.rows[r].cells) {
            cell.row = r;
        }
    }
    g_sheet.geometry.grid_rows = static_cast<int>(g_sheet.rows.size());
    return env->NewStringUTF(build_workload_response_json(true, "").c_str());
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeAddWorkloadRow(
    JNIEnv* env, jobject, jstring staff_name) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::string raw = jstring_to_utf8(env, staff_name);
    size_t b = 0;
    while (b < raw.size() && std::isspace(static_cast<unsigned char>(raw[b]))) ++b;
    size_t e = raw.size();
    while (e > b && std::isspace(static_cast<unsigned char>(raw[e - 1]))) --e;
    std::string trimmed = raw.substr(b, e - b);
    std::string norm = normalize_person_name(trimmed);
    std::string final_name = norm.empty() ? trimmed : norm;
    if (final_name.empty()) {
        return env->NewStringUTF(build_workload_response_json(false, "姓名不能为空").c_str());
    }

    ensure_sheet_initialized_unlocked();
    int new_row_idx = static_cast<int>(g_sheet.rows.size());
    g_sheet.rows.push_back(make_blank_workload_row(new_row_idx, final_name));
    g_sheet.geometry.grid_rows = static_cast<int>(g_sheet.rows.size());
    return env->NewStringUTF(build_workload_response_json(true, "").c_str());
}

JNIEXPORT jintArray JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeGetWorkingImageSize(JNIEnv* env, jobject) {
    std::lock_guard<std::mutex> lock(g_mutex);
    jint sz[2] = {g_working_img.width, g_working_img.height};
    jintArray out = env->NewIntArray(2);
    env->SetIntArrayRegion(out, 0, 2, sz);
    return out;
}

JNIEXPORT void JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeRenderWorkingImage(
    JNIEnv* env, jobject, jobject out_bitmap) {
    std::lock_guard<std::mutex> lock(g_mutex);
    write_rgb_to_rgba8888_bitmap(g_working_img, env, out_bitmap);
}

JNIEXPORT void JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeRenderCellPreview(
    JNIEnv* env, jobject, jint row, jint col_index, jobject out_bitmap) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_working_img.valid() || !out_bitmap || row < 0 || row >= static_cast<int>(g_sheet.rows.size())) return;
    AndroidBitmapInfo info{};
    if (AndroidBitmap_getInfo(env, out_bitmap, &info) < 0) return;
    const auto& wrow = g_sheet.rows[row];
    std::array<Point2f, 4> quad{};
    if (col_index < 0) {
        quad = wrow.name_quad;
    } else if (col_index < static_cast<int>(wrow.cells.size())) {
        quad = wrow.cells[col_index].orig_quad;
    } else {
        return;
    }
    if (std::hypot(quad[1].x - quad[0].x, quad[1].y - quad[0].y) <= 1.0f ||
        std::hypot(quad[3].x - quad[0].x, quad[3].y - quad[0].y) <= 1.0f) {
        return;
    }
    float cx = 0.25f * (quad[0].x + quad[1].x + quad[2].x + quad[3].x);
    float cy = 0.25f * (quad[0].y + quad[1].y + quad[2].y + quad[3].y);
    std::array<Point2f, 4> padded_quad = quad;
    for (auto& pt : padded_quad) {
        pt.x = cx + (pt.x - cx) * 1.12f;
        pt.y = cy + (pt.y - cy) * 1.12f;
    }
    int cw = std::max(64, static_cast<int>(info.width));
    int ch = std::max(48, static_cast<int>(info.height));
    ImageRgb crop = crop_quad_image(g_working_img, padded_quad, cw, ch);
    write_rgb_to_rgba8888_bitmap(crop, env, out_bitmap);
}

JNIEXPORT jstring JNICALL
Java_com_scluzlep_nurseocreport_NativeBridge_nativeExportXlsx(
    JNIEnv* env, jobject, jstring out_path, jint year, jint month) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_sheet.rows.empty()) {
        return env->NewStringUTF("请先识别工作量照片");
    }
    g_sheet.year = year;
    g_sheet.month = month;
    g_sheet.week_labels = default_week_labels(year, month, g_sheet.weeks);
    SheetSummary sum = evaluate_workload(g_sheet, g_config);
    ExportOptions opts;
    opts.include_summary_sheet = true;
    std::string err;
    if (!export_workload_xlsx(jstring_to_utf8(env, out_path), g_sheet, g_config, sum, opts, err)) {
        return env->NewStringUTF(err.c_str());
    }
    return env->NewStringUTF("");
}

} // extern "C"
