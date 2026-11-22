#pragma once

#include "nurseocr/types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nurseocr {

enum class ImageFormat {
    Unknown = 0,
    WebP = 1,
    Jpeg = 2,
    Png = 3,
    Bmp = 4
};

ImageFormat detect_image_format(const uint8_t* bytes, size_t size);

bool decode_image_bytes(const uint8_t* bytes, size_t size, ImageRgb& out_img, std::string& out_err);
bool load_image_file(const std::string& path, ImageRgb& out_img, std::string& out_err);

ImageRgb rotate_image_90cw(const ImageRgb& src, int quarter_turns_cw);

Point2f rotate_point_90cw(Point2f pt, int src_w, int src_h, int quarter_turns_cw);

} // namespace nurseocr
