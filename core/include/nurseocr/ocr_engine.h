#pragma once

#include "nurseocr/types.h"
#include "nurseocr/table_cv.h"
#include <string>
#include <vector>

namespace nurseocr {

struct TextBoxOcr {
    RectF box;
    std::string text;
    float confidence = 0.0f;
};

class OcrEngine {
public:
    OcrEngine();
    ~OcrEngine();

    OcrEngine(const OcrEngine&) = delete;
    OcrEngine& operator=(const OcrEngine&) = delete;

    // Loads PP-OCRv5 mobile det & rec models from filesystem directory.
    bool init_from_dir(const std::string& model_dir, std::string& out_err);

    // Loads models from explicit memory buffers (useful for Android AssetManager).
    bool init_from_memory(const std::vector<uint8_t>& det_param,
                          const std::vector<uint8_t>& det_bin,
                          const std::vector<uint8_t>& rec_param,
                          const std::vector<uint8_t>& rec_bin,
                          std::string& out_err);

    bool is_ready() const { return ready_; }

    // Recognizes general text line in an RGB crop (automatically resizes height to 48).
    bool recognize_text_line(const ImageRgb& rgb,
                             std::string& out_text,
                             float& out_conf) const;

    // Recognizes text boxes using det + rec (used e.g. for orientation check).
    std::vector<TextBoxOcr> detect_and_recognize(const ImageRgb& rgb,
                                                 int max_side = 960) const;

    // Auto-orients an input photo to upright landscape table orientation (0, 90, 180, 270 CW).
    ImageRgb auto_orient_table_photo(const ImageRgb& src, int& out_rot_deg) const;

    // Full pipeline for monthly workload photo.
    // If manual_corners != nullptr, uses those corners instead of auto-detecting.
    bool process_workload_photo(const ImageRgb& working_img,
                                int year,
                                int month,
                                const AppConfig& config,
                                const std::array<Point2f, 4>* manual_corners,
                                WorkloadSheet& out_sheet,
                                std::string& out_err) const;

    // Full pipeline for staff coefficient table photo.
    bool process_coeff_photo(const ImageRgb& src_img,
                             const std::vector<std::string>& optional_name_dict,
                             CoeffTableResult& out_res) const;

    // Re-runs OCR only on a subset of cells after manual grid/corner adjustment,
    // preserving cells already locked by user.
    bool reocr_unlocked_cells(const ImageRgb& working_img,
                              const std::array<Point2f, 4>& corners,
                              WorkloadSheet& sheet,
                              std::string& out_err) const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    bool ready_ = false;

    void recognize_digit_from_analysis(const CellInkAnalysis& ink,
                                       WorkloadCell& cell) const;
};

} // namespace nurseocr
