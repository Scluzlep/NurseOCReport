#include "nurseocr/image_io.h"

#include <webp/decode.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_FAILURE_STRINGS
#include <stb/stb_image.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nurseocr {

namespace {

uint16_t read_u16(const uint8_t* p, bool le) {
    return le ? static_cast<uint16_t>(p[0] | (p[1] << 8))
              : static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint32_t read_u32(const uint8_t* p, bool le) {
    return le ? (static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
                 (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24))
              : ((static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
                 (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]));
}

// Parse JPEG EXIF orientation tag (0x0112). Returns 1 if absent.
int parse_jpeg_exif_orientation(const uint8_t* data, size_t size) {
    if (size < 12 || data[0] != 0xFF || data[1] != 0xD8) return 1;
    size_t pos = 2;
    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) break;
        uint8_t marker = data[pos + 1];
        if (marker == 0xD9 || marker == 0xDA) break;
        if (marker == 0x00 || (marker >= 0xD0 && marker <= 0xD7)) {
            pos += 2;
            continue;
        }
        uint16_t seg_len = read_u16(data + pos + 2, false);
        if (seg_len < 2 || pos + 2 + seg_len > size) break;
        if (marker == 0xE1 && seg_len >= 16 &&
            std::memcmp(data + pos + 4, "Exif\0\0", 6) == 0) {
            const uint8_t* tiff = data + pos + 10;
            size_t tiff_len = seg_len - 8;
            if (tiff_len < 8) return 1;
            bool le = (tiff[0] == 'I' && tiff[1] == 'I');
            bool be = (tiff[0] == 'M' && tiff[1] == 'M');
            if (!le && !be) return 1;
            if (read_u16(tiff + 2, le) != 42) return 1;
            uint32_t ifd_off = read_u32(tiff + 4, le);
            if (ifd_off + 2 > tiff_len) return 1;
            uint16_t entries = read_u16(tiff + ifd_off, le);
            for (uint16_t i = 0; i < entries; ++i) {
                size_t eoff = ifd_off + 2 + static_cast<size_t>(i) * 12;
                if (eoff + 12 > tiff_len) break;
                uint16_t tag = read_u16(tiff + eoff, le);
                if (tag == 0x0112) {
                    return static_cast<int>(read_u16(tiff + eoff + 8, le));
                }
            }
            return 1;
        }
        pos += 2 + seg_len;
    }
    return 1;
}

ImageRgb apply_exif_orientation(const ImageRgb& src, int orientation) {
    if (!src.valid()) return src;
    switch (orientation) {
    case 3:
        return rotate_image_90cw(src, 2);
    case 6:
        return rotate_image_90cw(src, 1);
    case 8:
        return rotate_image_90cw(src, 3);
    case 2: {
        ImageRgb dst = src;
        for (int y = 0; y < src.height; ++y) {
            for (int x = 0; x < src.width; ++x) {
                int sx = src.width - 1 - x;
                const uint8_t* sp = &src.data[(static_cast<size_t>(y) * src.width + sx) * 3];
                uint8_t* dp = &dst.data[(static_cast<size_t>(y) * dst.width + x) * 3];
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
            }
        }
        return dst;
    }
    case 4: {
        ImageRgb dst = src;
        for (int y = 0; y < src.height; ++y) {
            int sy = src.height - 1 - y;
            std::memcpy(&dst.data[static_cast<size_t>(y) * src.width * 3],
                        &src.data[static_cast<size_t>(sy) * src.width * 3],
                        static_cast<size_t>(src.width) * 3);
        }
        return dst;
    }
    default:
        return src;
    }
}

} // namespace

ImageFormat detect_image_format(const uint8_t* bytes, size_t size) {
    if (!bytes) return ImageFormat::Unknown;
    if (size >= 12 &&
        std::memcmp(bytes, "RIFF", 4) == 0 &&
        std::memcmp(bytes + 8, "WEBP", 4) == 0) {
        return ImageFormat::WebP;
    }
    if (size >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF) {
        return ImageFormat::Jpeg;
    }
    if (size >= 8 && std::memcmp(bytes, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return ImageFormat::Png;
    }
    if (size >= 2 && bytes[0] == 'B' && bytes[1] == 'M') {
        return ImageFormat::Bmp;
    }
    return ImageFormat::Unknown;
}

bool decode_image_bytes(const uint8_t* bytes, size_t size, ImageRgb& out_img, std::string& out_err) {
    if (!bytes || size < 12) {
        out_err = "图片数据无效";
        return false;
    }

    ImageFormat fmt = detect_image_format(bytes, size);
    if (fmt == ImageFormat::WebP) {
        int w = 0, h = 0;
        uint8_t* rgb = WebPDecodeRGB(bytes, size, &w, &h);
        if (!rgb || w <= 0 || h <= 0) {
            if (rgb) WebPFree(rgb);
            out_err = "WebP 图片解码失败";
            return false;
        }
        out_img.width = w;
        out_img.height = h;
        out_img.data.assign(rgb, rgb + static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
        WebPFree(rgb);
        return true;
    }

    int w = 0, h = 0, comp = 0;
    stbi_uc* pixels = stbi_load_from_memory(bytes, static_cast<int>(size), &w, &h, &comp, 3);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        out_err = "图片解码失败，请确认是常见照片格式";
        return false;
    }
    ImageRgb decoded;
    decoded.width = w;
    decoded.height = h;
    decoded.data.assign(pixels, pixels + static_cast<size_t>(w) * static_cast<size_t>(h) * 3);
    stbi_image_free(pixels);

    if (fmt == ImageFormat::Jpeg) {
        int orient = parse_jpeg_exif_orientation(bytes, size);
        if (orient > 1 && orient <= 8) {
            decoded = apply_exif_orientation(decoded, orient);
        }
    }
    out_img = std::move(decoded);
    return true;
}

bool load_image_file(const std::string& path, ImageRgb& out_img, std::string& out_err) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(1, wlen)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
    FILE* f = _wfopen(wpath.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        out_err = "无法打开图片文件";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        out_err = "图片文件为空";
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    buf.resize(n);
    return decode_image_bytes(buf.data(), buf.size(), out_img, out_err);
}

ImageRgb rotate_image_90cw(const ImageRgb& src, int quarter_turns_cw) {
    if (!src.valid()) return src;
    int turns = ((quarter_turns_cw % 4) + 4) % 4;
    if (turns == 0) return src;

    ImageRgb dst;
    if (turns == 1) {
        dst.width = src.height;
        dst.height = src.width;
        dst.data.resize(src.data.size());
        for (int y = 0; y < src.height; ++y) {
            for (int x = 0; x < src.width; ++x) {
                int nx = src.height - 1 - y;
                int ny = x;
                const uint8_t* sp = &src.data[(static_cast<size_t>(y) * src.width + x) * 3];
                uint8_t* dp = &dst.data[(static_cast<size_t>(ny) * dst.width + nx) * 3];
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
            }
        }
    } else if (turns == 2) {
        dst.width = src.width;
        dst.height = src.height;
        dst.data.resize(src.data.size());
        for (int y = 0; y < src.height; ++y) {
            for (int x = 0; x < src.width; ++x) {
                int nx = src.width - 1 - x;
                int ny = src.height - 1 - y;
                const uint8_t* sp = &src.data[(static_cast<size_t>(y) * src.width + x) * 3];
                uint8_t* dp = &dst.data[(static_cast<size_t>(ny) * dst.width + nx) * 3];
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
            }
        }
    } else {
        dst.width = src.height;
        dst.height = src.width;
        dst.data.resize(src.data.size());
        for (int y = 0; y < src.height; ++y) {
            for (int x = 0; x < src.width; ++x) {
                int nx = y;
                int ny = src.width - 1 - x;
                const uint8_t* sp = &src.data[(static_cast<size_t>(y) * src.width + x) * 3];
                uint8_t* dp = &dst.data[(static_cast<size_t>(ny) * dst.width + nx) * 3];
                dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
            }
        }
    }
    return dst;
}

Point2f rotate_point_90cw(Point2f pt, int src_w, int src_h, int quarter_turns_cw) {
    int turns = ((quarter_turns_cw % 4) + 4) % 4;
    switch (turns) {
    case 1: return {static_cast<float>(src_h) - pt.y, pt.x};
    case 2: return {static_cast<float>(src_w) - pt.x, static_cast<float>(src_h) - pt.y};
    case 3: return {pt.y, static_cast<float>(src_w) - pt.x};
    default: return pt;
    }
}

} // namespace nurseocr
