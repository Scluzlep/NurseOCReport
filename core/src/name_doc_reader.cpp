#include "nurseocr/name_doc_reader.h"

#include <miniz.h>
#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_set>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace nurseocr {

namespace {

bool is_valid_utf8(const std::string& s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 0;
        uint32_t cp = 0;
        if (c <= 0x7F) {
            i += 1;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            len = 2; cp = c & 0x1F;
            if (cp < 2) return false;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3; cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            len = 4; cp = c & 0x07;
        } else {
            return false;
        }
        if (i + len > s.size()) return false;
        for (size_t k = 1; k < len; ++k) {
            unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        i += len;
    }
    return true;
}

std::string utf16_bytes_to_utf8(const uint8_t* data, size_t size, bool little_endian) {
    std::string out;
    out.reserve(size);
    for (size_t i = 0; i + 1 < size; i += 2) {
        uint16_t u = little_endian
            ? static_cast<uint16_t>(data[i] | (data[i + 1] << 8))
            : static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
        if (u == 0xFEFF) continue;
        uint32_t cp = u;
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < size) {
            uint16_t low = little_endian
                ? static_cast<uint16_t>(data[i + 2] | (data[i + 3] << 8))
                : static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + (((static_cast<uint32_t>(u - 0xD800)) << 10) | (low - 0xDC00));
                i += 2;
            }
        }
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::string decode_text_bytes(const uint8_t* bytes, size_t size) {
    if (!bytes || size == 0) return "";
    if (size >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
        return std::string(reinterpret_cast<const char*>(bytes + 3), size - 3);
    }
    if (size >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
        return utf16_bytes_to_utf8(bytes + 2, size - 2, true);
    }
    if (size >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) {
        return utf16_bytes_to_utf8(bytes + 2, size - 2, false);
    }
    std::string raw(reinterpret_cast<const char*>(bytes), size);
    if (is_valid_utf8(raw)) {
        return raw;
    }
#ifdef _WIN32
    UINT cp = 54936; // GB18030
    int wlen = MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    if (wlen <= 0) {
        cp = 936;
        wlen = MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), nullptr, 0);
    }
    if (wlen > 0) {
        std::wstring ws(static_cast<size_t>(wlen), L'\0');
        MultiByteToWideChar(cp, 0, raw.data(), static_cast<int>(raw.size()), &ws[0], wlen);
        int ulen = WideCharToMultiByte(CP_UTF8, 0, ws.data(), wlen, nullptr, 0, nullptr, nullptr);
        if (ulen > 0) {
            std::string utf8(static_cast<size_t>(ulen), '\0');
            WideCharToMultiByte(CP_UTF8, 0, ws.data(), wlen, &utf8[0], ulen, nullptr, nullptr);
            return utf8;
        }
    }
#endif
    return raw;
}

std::vector<uint32_t> utf8_to_codepoints(const std::string& s) {
    std::vector<uint32_t> cps;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c <= 0x7F) {
            cps.push_back(c);
            i += 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            uint32_t cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
            cps.push_back(cp);
            i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            uint32_t cp = ((c & 0x0F) << 12) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 2]) & 0x3F);
            cps.push_back(cp);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            uint32_t cp = ((c & 0x07) << 18) |
                          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                          ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                          (static_cast<unsigned char>(s[i + 3]) & 0x3F);
            cps.push_back(cp);
            i += 4;
        } else {
            i += 1;
        }
    }
    return cps;
}

std::string codepoints_to_utf8(const std::vector<uint32_t>& cps) {
    std::string out;
    for (uint32_t cp : cps) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

bool is_cjk(uint32_t cp) {
    return (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0x3400 && cp <= 0x4DBF);
}

bool is_header_or_noise_token(const std::string& tok) {
    static const char* const kNoise[] = {
        "姓名", "名字", "人员", "护士", "序号", "编号", "职称", "职务",
        "职称系数", "职务系数", "总系数", "合计", "总分", "备注", "工作量",
        "白班", "夜班", "通宵", "奖金", "奖励", "总数", "第1周", "第2周",
        "第3周", "第4周", "第5周", "第6周", "技术难度", "科室"
    };
    for (const char* w : kNoise) {
        if (tok == w) return true;
    }
    return false;
}

std::string clean_candidate_token(const std::string& raw) {
    auto cps = utf8_to_codepoints(raw);
    std::vector<uint32_t> kept;
    for (uint32_t cp : cps) {
        if (is_cjk(cp) || cp == 0x00B7) { // CJK + middle dot
            kept.push_back(cp);
        }
    }
    if (kept.size() < 2 || kept.size() > 6) return "";
    std::string out = codepoints_to_utf8(kept);
    if (is_header_or_noise_token(out)) return "";
    return out;
}

// Generic Chinese surname and name homophone/visually-similar groups (no specific real names)
const std::vector<std::vector<std::string>>& homophone_groups() {
    static const std::vector<std::vector<std::string>> kGroups = {
        {"妍", "研", "嫣", "燕", "彦", "言", "颜", "严", "阎"},
        {"玲", "琳", "凌", "灵", "铃", "苓", "伶", "龄", "菱"},
        {"静", "靖", "晶", "净", "景", "晴", "清", "青"},
        {"艳", "燕", "雁", "焰", "砚"},
        {"蓓", "倍", "贝", "佩", "培"},
        {"淑", "舒", "书", "姝", "疏", "梳"},
        {"慕", "木", "牧", "沐", "幕", "穆"},
        {"硕", "烁", "朔"},
        {"敏", "旻", "民", "悯"},
        {"芳", "方", "防", "访", "放"},
        {"丽", "莉", "利", "力", "立", "荔", "俐"},
        {"华", "花", "桦", "滑"},
        {"娟", "涓", "绢", "鹃"},
        {"婷", "廷", "亭", "庭"},
        {"梅", "美", "眉", "玫", "妹"},
        {"洁", "杰", "捷", "婕", "节"},
        {"雅", "亚", "娅", "芽"},
        {"欣", "心", "新", "鑫", "辛", "芯", "馨"},
        {"雪", "学", "薛"},
        {"娜", "纳", "哪", "妮"},
        {"霞", "夏", "侠", "峡"},
        {"玉", "宇", "雨", "羽", "语", "茹", "如", "儒", "瑜", "愉"},
        {"萍", "平", "苹", "评", "坪"},
        {"慧", "惠", "会", "卉", "汇"},
        {"琴", "芹", "秦", "勤", "沁"},
        {"英", "樱", "瑛", "迎", "盈", "莹", "颖"},
        {"红", "虹", "宏", "洪", "鸿"},
        {"春", "纯", "淳"},
        {"晓", "小", "筱", "潇", "肖", "萧"},
        {"佳", "嘉", "家", "加", "珈"},
        {"璐", "露", "路", "芦"},
        {"茜", "倩", "芊", "千", "浅"},
        {"菲", "飞", "非", "霏", "妃"},
        {"瑶", "姚", "遥", "尧"},
        {"丹", "单", "淡"},
        {"悦", "月", "越", "岳"},
        {"阳", "杨", "洋", "扬"},
        {"陈", "程", "成", "城", "承"},
        {"张", "章", "彰"},
        {"王", "汪", "旺"},
        {"李", "理", "里", "礼"},
        {"刘", "留", "柳"},
        {"赵", "兆", "照"},
        {"徐", "许", "续"},
        {"周", "邹", "洲"},
        {"黄", "皇", "煌"},
        {"吴", "武", "伍", "舞"},
        {"胡", "湖", "狐"},
        {"魏", "卫", "伟", "微", "薇"},
        {"姜", "江", "蒋"},
        {"范", "樊", "凡"},
        {"宋", "送", "颂"},
        {"传", "川", "船"},
        {"星", "兴", "行"},
        {"甜", "田", "恬"},
        {"芷", "芝", "枝", "知", "之", "志"},
        {"君", "军", "均", "俊"},
        {"冬", "东", "董"},
        {"青", "清", "庆"},
        {"华", "华"},
        {"凤", "丰", "峰", "枫", "风"},
        {"兰", "蓝", "岚"},
        {"燕", "艳", "彦"},
        {"菊", "居", "橘"},
        {"珍", "真", "贞", "榛"},
        {"秀", "袖", "绣"},
        {"云", "芸", "韵"},
        {"彩", "采", "蔡"},
        {"文", "雯", "闻"},
        {"宁", "凝", "柠"},
        {"雯", "文", "纹"},
        {"涵", "寒", "韩"},
        {"萱", "轩", "宣"},
        {"琪", "琦", "奇", "棋"},
        {"怡", "宜", "仪", "依"},
        {"洁", "杰", "子"},
    };
    return kGroups;
}

double char_sub_cost(const std::string& a, const std::string& b) {
    if (a == b) return 0.0;
    for (const auto& grp : homophone_groups()) {
        bool has_a = false, has_b = false;
        for (const auto& w : grp) {
            if (w == a) has_a = true;
            if (w == b) has_b = true;
        }
        if (has_a && has_b) return 0.22;
    }
    return 1.0;
}

std::vector<std::string> split_utf8_chars(const std::string& s) {
    auto cps = utf8_to_codepoints(s);
    std::vector<std::string> out;
    out.reserve(cps.size());
    for (uint32_t cp : cps) {
        out.push_back(codepoints_to_utf8({cp}));
    }
    return out;
}

bool extract_zip_member(const uint8_t* zip_bytes,
                        size_t zip_size,
                        const char* member_name,
                        std::string& out_data) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zip_bytes, zip_size, 0)) {
        return false;
    }
    size_t uncomp_size = 0;
    void* ptr = mz_zip_reader_extract_file_to_heap(&zip, member_name, &uncomp_size, 0);
    mz_zip_reader_end(&zip);
    if (!ptr) return false;
    out_data.assign(static_cast<const char*>(ptr), uncomp_size);
    mz_free(ptr);
    return true;
}

bool parse_docx_bytes(const uint8_t* bytes,
                      size_t size,
                      std::vector<std::string>& out_names,
                      std::string& out_err) {
    std::string xml;
    if (!extract_zip_member(bytes, size, "word/document.xml", xml)) {
        out_err = "DOCX 文件缺少正文内容";
        return false;
    }
    pugi::xml_document doc;
    if (!doc.load_buffer(xml.data(), xml.size())) {
        out_err = "DOCX 正文解析失败";
        return false;
    }
    std::ostringstream plain;
    // Walk manually so we handle paragraphs and table cells without XPath issues
    struct Walker : pugi::xml_tree_walker {
        std::ostringstream& oss;
        explicit Walker(std::ostringstream& s) : oss(s) {}
        bool for_each(pugi::xml_node& node) override {
            std::string name = node.name();
            auto colon = name.find(':');
            if (colon != std::string::npos) name = name.substr(colon + 1);
            if (name == "t") {
                oss << node.child_value();
            } else if (name == "p" || name == "tc" || name == "br") {
                oss << '\n';
            }
            return true;
        }
    } walker(plain);
    doc.traverse(walker);
    out_names = parse_names_from_plain_text(plain.str());
    return true;
}

bool parse_xlsx_names_bytes(const uint8_t* bytes,
                            size_t size,
                            std::vector<std::string>& out_names,
                            std::string& out_err) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, bytes, size, 0)) {
        out_err = "Excel 文件格式无效";
        return false;
    }
    std::vector<std::string> shared_strings;
    size_t ss_size = 0;
    if (void* ss_ptr = mz_zip_reader_extract_file_to_heap(&zip, "xl/sharedStrings.xml", &ss_size, 0)) {
        pugi::xml_document ss_doc;
        if (ss_doc.load_buffer(ss_ptr, ss_size)) {
            for (pugi::xml_node si = ss_doc.document_element().first_child(); si; si = si.next_sibling()) {
                std::string text;
                struct TextWalker : pugi::xml_tree_walker {
                    std::string& out;
                    explicit TextWalker(std::string& o) : out(o) {}
                    bool for_each(pugi::xml_node& n) override {
                        std::string nm = n.name();
                        auto c = nm.find(':');
                        if (c != std::string::npos) nm = nm.substr(c + 1);
                        if (nm == "t") out += n.child_value();
                        return true;
                    }
                } tw(text);
                si.traverse(tw);
                shared_strings.push_back(std::move(text));
            }
        }
        mz_free(ss_ptr);
    }

    std::vector<std::string> candidates;
    int file_count = static_cast<int>(mz_zip_reader_get_num_files(&zip));
    for (int i = 0; i < file_count; ++i) {
        char fname[256];
        mz_zip_reader_get_filename(&zip, static_cast<mz_uint>(i), fname, sizeof(fname));
        std::string sname(fname);
        if (sname.find("xl/worksheets/sheet") != 0 || sname.find(".xml") == std::string::npos) {
            continue;
        }
        size_t sh_size = 0;
        void* sh_ptr = mz_zip_reader_extract_to_heap(&zip, static_cast<mz_uint>(i), &sh_size, 0);
        if (!sh_ptr) continue;
        pugi::xml_document sh_doc;
        if (sh_doc.load_buffer(sh_ptr, sh_size)) {
            for (pugi::xml_node node = sh_doc.document_element().first_child(); node; node = node.next_sibling()) {
                std::string nm = node.name();
                auto colpos = nm.find(':');
                if (colpos != std::string::npos) nm = nm.substr(colpos + 1);
                if (nm != "sheetData") continue;
                for (pugi::xml_node row = node.first_child(); row; row = row.next_sibling()) {
                    for (pugi::xml_node cell = row.first_child(); cell; cell = cell.next_sibling()) {
                        std::string ref = cell.attribute("r").as_string();
                        if (ref.empty() || (ref[0] != 'A' && ref[0] != 'B') ||
                            (ref.size() > 1 && std::isalpha(static_cast<unsigned char>(ref[1])))) {
                            continue; // Only examine A and B columns for names
                        }
                        std::string t = cell.attribute("t").as_string();
                        std::string val;
                        for (pugi::xml_node ch = cell.first_child(); ch; ch = ch.next_sibling()) {
                            std::string cnm = ch.name();
                            auto c2 = cnm.find(':');
                            if (c2 != std::string::npos) cnm = cnm.substr(c2 + 1);
                            if (cnm == "v") {
                                val = ch.child_value();
                            } else if (cnm == "is") {
                                for (pugi::xml_node it = ch.first_child(); it; it = it.next_sibling()) {
                                    val += it.child_value();
                                }
                            }
                        }
                        if (t == "s" && !val.empty()) {
                            int idx = std::atoi(val.c_str());
                            if (idx >= 0 && idx < static_cast<int>(shared_strings.size())) {
                                val = shared_strings[idx];
                            } else {
                                val.clear();
                            }
                        }
                        std::string cleaned = clean_candidate_token(val);
                        if (!cleaned.empty()) {
                            candidates.push_back(std::move(cleaned));
                        }
                    }
                }
            }
        }
        mz_free(sh_ptr);
        if (!candidates.empty()) break; // First sheet containing names
    }
    mz_zip_reader_end(&zip);

    std::unordered_set<std::string> seen;
    out_names.clear();
    for (const auto& c : candidates) {
        if (seen.insert(c).second) out_names.push_back(c);
    }
    return true;
}

} // namespace

std::string normalize_person_name(const std::string& raw) {
    auto cps = utf8_to_codepoints(raw);
    std::vector<uint32_t> kept;
    for (uint32_t cp : cps) {
        if (cp <= 0x20 || cp == 0x3000) continue; // skip ASCII & ideographic spaces
        if (is_cjk(cp) || cp == 0x00B7) {
            kept.push_back(cp);
        }
    }
    return codepoints_to_utf8(kept);
}

std::vector<std::string> parse_names_from_plain_text(const std::string& utf8_text) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;

    // Split on ASCII whitespace, ASCII punctuation, and common Chinese separators
    std::string replaced;
    replaced.reserve(utf8_text.size());
    auto cps = utf8_to_codepoints(utf8_text);
    for (uint32_t cp : cps) {
        if (cp == '\r' || cp == '\n' || cp == '\t' || cp == ',' || cp == ';' ||
            cp == '|' || cp == '/' || cp == '\\' || cp == 0x3001 /* 、 */ ||
            cp == 0xFF0C /* ， */ || cp == 0xFF1B /* ； */ || cp == 0xFF1A /* ： */ ||
            cp == ':' || cp == 0x0020 || cp == 0x3000) {
            replaced.push_back('\n');
        } else {
            replaced += codepoints_to_utf8({cp});
        }
    }

    std::istringstream iss(replaced);
    std::string tok;
    while (std::getline(iss, tok)) {
        std::string cleaned = clean_candidate_token(tok);
        if (!cleaned.empty() && seen.insert(cleaned).second) {
            out.push_back(std::move(cleaned));
        }
    }
    return out;
}

bool read_names_from_bytes(const std::string& filename_hint,
                           const uint8_t* bytes,
                           size_t size,
                           std::vector<std::string>& out_names,
                           std::string& out_err) {
    if (!bytes || size == 0) {
        out_err = "名单文件为空";
        return false;
    }
    std::string lower = filename_hint;
    for (char& c : lower) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    // Detect ZIP-based formats (.docx / .xlsx) by PK magic or extension
    bool is_zip = (size >= 4 && bytes[0] == 'P' && bytes[1] == 'K' && bytes[2] == 3 && bytes[3] == 4);
    if (is_zip || lower.size() >= 5 && lower.substr(lower.size() - 5) == ".docx") {
        if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".xlsx") {
            return parse_xlsx_names_bytes(bytes, size, out_names, out_err);
        }
        if (parse_docx_bytes(bytes, size, out_names, out_err)) {
            return true;
        }
        if (lower.size() >= 5 && lower.substr(lower.size() - 5) == ".docx") {
            return false;
        }
        return parse_xlsx_names_bytes(bytes, size, out_names, out_err);
    }

    std::string text = decode_text_bytes(bytes, size);
    out_names = parse_names_from_plain_text(text);
    if (out_names.empty()) {
        out_err = "文件中未找到有效人员名字";
        return false;
    }
    return true;
}

bool read_names_from_file(const std::string& path,
                          std::vector<std::string>& out_names,
                          std::string& out_err) {
#ifdef _WIN32
    int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring wpath(static_cast<size_t>(std::max(1, wlen)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &wpath[0], wlen);
    FILE* f = _wfopen(wpath.c_str(), L"rb");
#else
    FILE* f = std::fopen(path.c_str(), "rb");
#endif
    if (!f) {
        out_err = "无法打开名单文件";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) {
        std::fclose(f);
        out_err = "名单文件为空";
        return false;
    }
    std::vector<uint8_t> buf(static_cast<size_t>(sz));
    size_t n = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    buf.resize(n);
    return read_names_from_bytes(path, buf.data(), buf.size(), out_names, out_err);
}

double person_name_similarity(const std::string& ocr_raw, const std::string& dict_name) {
    std::string a_norm = normalize_person_name(ocr_raw);
    std::string b_norm = normalize_person_name(dict_name);
    if (a_norm.empty() || b_norm.empty()) return 0.0;
    if (a_norm == b_norm) return 1.0;

    auto va = split_utf8_chars(a_norm);
    auto vb = split_utf8_chars(b_norm);
    size_t m = va.size(), n = vb.size();
    std::vector<std::vector<double>> dp(m + 1, std::vector<double>(n + 1, 0.0));
    for (size_t i = 0; i <= m; ++i) dp[i][0] = static_cast<double>(i);
    for (size_t j = 0; j <= n; ++j) dp[0][j] = static_cast<double>(j);

    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            double sub = dp[i - 1][j - 1] + char_sub_cost(va[i - 1], vb[j - 1]);
            double del = dp[i - 1][j] + 1.0;
            double ins = dp[i][j - 1] + 1.0;
            dp[i][j] = std::min({sub, del, ins});
        }
    }
    double max_len = static_cast<double>(std::max(m, n));
    double sim = 1.0 - dp[m][n] / max_len;
    return std::max(0.0, std::min(1.0, sim));
}

std::vector<NameMatchOutcome> align_names_unique(
    const std::vector<std::string>& ocr_names,
    const std::vector<std::string>& dict_names,
    double min_accept_score) {

    const int n_rows = static_cast<int>(ocr_names.size());
    const int n_dict = static_cast<int>(dict_names.size());
    std::vector<NameMatchOutcome> result(static_cast<size_t>(n_rows));
    for (int i = 0; i < n_rows; ++i) {
        result[i].matched_name = normalize_person_name(ocr_names[i]);
        if (result[i].matched_name.empty()) result[i].matched_name = ocr_names[i];
    }
    if (n_rows == 0 || n_dict == 0) return result;

    // Pad with n_rows dummy unmatched columns of cost (1.0 - min_accept_score) so rows can stay unmatched
    const int dim = std::max(n_rows, n_dict + n_rows);
    const double unmatch_cost = 1.0 - min_accept_score;

    std::vector<std::vector<double>> cost(
        static_cast<size_t>(dim + 1), std::vector<double>(static_cast<size_t>(dim + 1), 1.5));

    for (int r = 1; r <= n_rows; ++r) {
        for (int d = 1; d <= n_dict; ++d) {
            double sim = person_name_similarity(ocr_names[r - 1], dict_names[d - 1]);
            cost[r][d] = 1.0 - sim;
        }
        // Dummy column for row r itself (leaving row unmatched)
        cost[r][n_dict + r] = unmatch_cost;
    }
    for (int r = n_rows + 1; r <= dim; ++r) {
        for (int c = 1; c <= dim; ++c) {
            cost[r][c] = 0.0;
        }
    }

    // Standard Jonker-Volgenant / Kuhn-Munkres potential algorithm O(dim^3)
    std::vector<double> u(static_cast<size_t>(dim + 1), 0.0);
    std::vector<double> v(static_cast<size_t>(dim + 1), 0.0);
    std::vector<int> p(static_cast<size_t>(dim + 1), 0);
    std::vector<int> way(static_cast<size_t>(dim + 1), 0);

    for (int i = 1; i <= dim; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(static_cast<size_t>(dim + 1), std::numeric_limits<double>::infinity());
        std::vector<bool> used(static_cast<size_t>(dim + 1), false);
        do {
            used[j0] = true;
            int i0 = p[j0];
            double delta = std::numeric_limits<double>::infinity();
            int j1 = 0;
            for (int j = 1; j <= dim; ++j) {
                if (!used[j]) {
                    double cur = cost[i0][j] - u[i0] - v[j];
                    if (cur < minv[j]) {
                        minv[j] = cur;
                        way[j] = j0;
                    }
                    if (minv[j] < delta) {
                        delta = minv[j];
                        j1 = j;
                    }
                }
            }
            for (int j = 0; j <= dim; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);

        do {
            int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }

    std::vector<int> row_to_col(static_cast<size_t>(n_rows + 1), -1);
    for (int j = 1; j <= dim; ++j) {
        if (p[j] >= 1 && p[j] <= n_rows) {
            row_to_col[p[j]] = j;
        }
    }

    for (int r = 1; r <= n_rows; ++r) {
        int c = row_to_col[r];
        if (c >= 1 && c <= n_dict) {
            double sim = 1.0 - cost[r][c];
            if (sim >= min_accept_score) {
                result[r - 1].matched_name = dict_names[c - 1];
                result[r - 1].score = sim;
                result[r - 1].exact = (sim > 0.999);
            }
        }
    }
    return result;
}

int apply_name_list_to_workload(WorkloadSheet& sheet, const std::vector<std::string>& dict_names) {
    if (sheet.rows.empty() || dict_names.empty()) return 0;
    std::vector<std::string> ocr;
    ocr.reserve(sheet.rows.size());
    for (const auto& r : sheet.rows) {
        ocr.push_back(!r.ocr_name.empty() ? r.ocr_name : r.staff_name);
    }
    auto aligned = align_names_unique(ocr, dict_names, 0.48);
    int updated = 0;
    for (size_t i = 0; i < sheet.rows.size(); ++i) {
        if (aligned[i].score >= 0.48) {
            if (sheet.rows[i].staff_name != aligned[i].matched_name) {
                ++updated;
            }
            sheet.rows[i].staff_name = aligned[i].matched_name;
            sheet.rows[i].name_confidence = static_cast<float>(aligned[i].score);
            sheet.rows[i].name_needs_review = (!aligned[i].exact && aligned[i].score < 0.72);
        } else {
            sheet.rows[i].name_needs_review = true;
        }
    }
    return updated;
}

int apply_name_list_to_staff(std::vector<StaffEntry>& staff, const std::vector<std::string>& dict_names) {
    if (dict_names.empty()) return 0;
    if (staff.empty()) {
        for (const auto& nm : dict_names) {
            staff.push_back(StaffEntry{nm, 1.0, 0.0});
        }
        return static_cast<int>(staff.size());
    }
    std::vector<std::string> current;
    current.reserve(staff.size());
    for (const auto& s : staff) current.push_back(s.name);
    auto aligned = align_names_unique(current, dict_names, 0.48);
    int updated = 0;
    for (size_t i = 0; i < staff.size(); ++i) {
        if (aligned[i].score >= 0.48 && staff[i].name != aligned[i].matched_name) {
            staff[i].name = aligned[i].matched_name;
            ++updated;
        }
    }
    return updated;
}

} // namespace nurseocr
