// json.hpp : 极小的 JSON 解析器（DOM 式），仅用于解析 GitHub API 响应
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace json {

struct Val {
    enum T { Null, Bool, Num, Str, Arr, Obj };
    T t = Null;
    bool b = false;
    double num = 0.0;
    std::string s;
    std::vector<Val> arr;
    std::vector<std::pair<std::string, Val>> kv;

    const Val* get(const char* key) const {
        if (t != Obj) return nullptr;
        for (auto& e : kv)
            if (e.first == key) return &e.second;
        return nullptr;
    }
    std::string str_or(const char* def = "") const { return t == Str ? s : std::string(def); }
    bool boolean_or(bool def = false) const { return t == Bool ? b : def; }
};

class Parser {
public:
    explicit Parser(const std::string& src) : s_(src) {}

    bool parse(Val& out, std::string& err) {
        pos_ = 0;
        depth_ = 0;
        ws();
        if (!value(out, err)) return false;
        ws();
        if (pos_ != s_.size()) return fail(err, "JSON 尾部有多余数据");
        return true;
    }

private:
    const std::string& s_;
    size_t pos_ = 0;
    int depth_ = 0;

    void ws() {
        while (pos_ < s_.size()) {
            char c = s_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++pos_;
            else break;
        }
    }

    bool fail(std::string& e, const char* m) {
        e = std::string(m) + "（偏移 " + std::to_string(pos_) + "）";
        return false;
    }

    bool value(Val& v, std::string& err) {
        if (++depth_ > 200) return fail(err, "JSON 嵌套过深");
        bool ok;
        if (pos_ >= s_.size()) {
            ok = fail(err, "JSON 提前结束");
        } else {
            char c = s_[pos_];
            if (c == '{') { v.t = Val::Obj; ok = object(v, err); }
            else if (c == '[') { v.t = Val::Arr; ok = array(v, err); }
            else if (c == '"') { v.t = Val::Str; ok = string_(v.s, err); }
            else if (c == 't') { v.t = Val::Bool; v.b = true; ok = lit("true", err); }
            else if (c == 'f') { v.t = Val::Bool; v.b = false; ok = lit("false", err); }
            else if (c == 'n') { v.t = Val::Null; ok = lit("null", err); }
            else ok = number(v, err);
        }
        --depth_;
        return ok;
    }

    bool lit(const char* w, std::string& err) {
        for (const char* p = w; *p; ++p, ++pos_)
            if (pos_ >= s_.size() || s_[pos_] != *p) return fail(err, "非法字面量");
        return true;
    }

    bool number(Val& v, std::string& err) {
        char* end = nullptr;
        double d = std::strtod(s_.c_str() + pos_, &end);
        if (end == s_.c_str() + pos_) return fail(err, "非法数字");
        v.t = Val::Num;
        v.num = d;
        pos_ = (size_t)(end - s_.c_str());
        return true;
    }

    static void utf8_append(std::string& out, unsigned cp) {
        if (cp >= 0xD800 && cp <= 0xDFFF) cp = 0xFFFD; // 孤立代理对 → 替换字符
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& u, std::string& err) {
        if (pos_ + 4 > s_.size()) return fail(err, "\\u 转义不完整");
        u = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            u <<= 4;
            if (c >= '0' && c <= '9') u |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') u |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') u |= (unsigned)(c - 'A' + 10);
            else return fail(err, "非法 \\u 转义");
        }
        return true;
    }

    bool u_escape(std::string& out, std::string& err) {
        unsigned hi;
        if (!hex4(hi, err)) return false;
        if (hi >= 0xD800 && hi <= 0xDBFF && pos_ + 1 < s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
            size_t save = pos_;
            pos_ += 2;
            unsigned lo;
            if (hex4(lo, err) && lo >= 0xDC00 && lo <= 0xDFFF) {
                utf8_append(out, 0x10000 + ((hi - 0xD800) << 10) + (lo - 0xDC00));
                return true;
            }
            pos_ = save; // 不是合法代理对，按孤立代理处理
        }
        utf8_append(out, hi);
        return true;
    }

    bool string_(std::string& out, std::string& err) {
        ++pos_; // 跳过开引号
        out.clear();
        while (pos_ < s_.size()) {
            unsigned char c = (unsigned char)s_[pos_];
            if (c == '"') {
                ++pos_;
                return true;
            }
            if (c == '\\') {
                ++pos_;
                if (pos_ >= s_.size()) break;
                char e = s_[pos_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u':
                        if (!u_escape(out, err)) return false;
                        break;
                    default: return fail(err, "非法转义字符");
                }
            } else {
                out += (char)c;
                ++pos_;
            }
        }
        return fail(err, "字符串未闭合");
    }

    bool array(Val& v, std::string& err) {
        ++pos_;
        ws();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return true;
        }
        for (;;) {
            Val item;
            ws();
            if (!value(item, err)) return false;
            v.arr.push_back(std::move(item));
            ws();
            if (pos_ >= s_.size()) return fail(err, "数组未闭合");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == ']') { ++pos_; return true; }
            return fail(err, "数组缺少 , 或 ]");
        }
    }

    bool object(Val& v, std::string& err) {
        ++pos_;
        ws();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return true;
        }
        for (;;) {
            ws();
            std::string key;
            if (pos_ >= s_.size() || s_[pos_] != '"') return fail(err, "对象键必须是字符串");
            if (!string_(key, err)) return false;
            ws();
            if (pos_ >= s_.size() || s_[pos_] != ':') return fail(err, "对象缺少 :");
            ++pos_;
            Val item;
            ws();
            if (!value(item, err)) return false;
            v.kv.emplace_back(std::move(key), std::move(item));
            ws();
            if (pos_ >= s_.size()) return fail(err, "对象未闭合");
            if (s_[pos_] == ',') { ++pos_; continue; }
            if (s_[pos_] == '}') { ++pos_; return true; }
            return fail(err, "对象缺少 , 或 }");
        }
    }
};

} // namespace json
