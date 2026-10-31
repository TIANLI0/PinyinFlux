#include "json.h"

#include <cstdlib>
#include <cstring>

namespace pf::json {

namespace {

const Value& Null() {
  static const Value kNull;
  return kNull;
}

class Parser {
 public:
  explicit Parser(std::string_view s) : s_(s) {}

  bool ParseDocument(Value& out) {
    if (!ParseValue(out, 0)) return false;
    SkipWs();
    return pos_ == s_.size();
  }

 private:
  static constexpr int kMaxDepth = 64;

  void SkipWs() {
    while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\r' || s_[pos_] == '\n'))
      ++pos_;
  }

  bool Consume(std::string_view lit) {
    if (s_.substr(pos_, lit.size()) != lit) return false;
    pos_ += lit.size();
    return true;
  }

  bool ParseValue(Value& v, int depth) {
    if (depth > kMaxDepth) return false;
    SkipWs();
    if (pos_ >= s_.size()) return false;
    char c = s_[pos_];
    if (c == '{') return ParseObject(v, depth);
    if (c == '[') return ParseArray(v, depth);
    if (c == '"') {
      v.type = Value::Type::String;
      return ParseString(v.string);
    }
    if (Consume("true")) { v.type = Value::Type::Bool; v.boolean = true; return true; }
    if (Consume("false")) { v.type = Value::Type::Bool; v.boolean = false; return true; }
    if (Consume("null")) { v.type = Value::Type::Null; return true; }
    return ParseNumber(v);
  }

  bool ParseNumber(Value& v) {
    size_t start = pos_;
    while (pos_ < s_.size() && (std::strchr("+-0123456789.eE", s_[pos_]) != nullptr)) ++pos_;
    if (start == pos_) return false;
    std::string tmp(s_.substr(start, pos_ - start));
    char* end = nullptr;
    v.number = std::strtod(tmp.c_str(), &end);
    v.type = Value::Type::Number;
    return end && *end == '\0';
  }

  static void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool ParseHex4(unsigned& out) {
    if (pos_ + 4 > s_.size()) return false;
    out = 0;
    for (int i = 0; i < 4; ++i) {
      char c = s_[pos_++];
      out <<= 4;
      if (c >= '0' && c <= '9') out |= c - '0';
      else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
      else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
      else return false;
    }
    return true;
  }

  bool ParseString(std::string& out) {
    ++pos_;  // opening quote
    while (pos_ < s_.size()) {
      char c = s_[pos_++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= s_.size()) return false;
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
        case 'u': {
          unsigned cp;
          if (!ParseHex4(cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF && Consume("\\u")) {
            unsigned lo;
            if (!ParseHex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          AppendUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  bool ParseArray(Value& v, int depth) {
    v.type = Value::Type::Array;
    ++pos_;
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == ']') { ++pos_; return true; }
    while (true) {
      v.array.emplace_back();
      if (!ParseValue(v.array.back(), depth + 1)) return false;
      SkipWs();
      if (pos_ >= s_.size()) return false;
      if (s_[pos_] == ',') { ++pos_; continue; }
      if (s_[pos_] == ']') { ++pos_; return true; }
      return false;
    }
  }

  bool ParseObject(Value& v, int depth) {
    v.type = Value::Type::Object;
    ++pos_;
    SkipWs();
    if (pos_ < s_.size() && s_[pos_] == '}') { ++pos_; return true; }
    while (true) {
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != '"') return false;
      std::string key;
      if (!ParseString(key)) return false;
      SkipWs();
      if (pos_ >= s_.size() || s_[pos_] != ':') return false;
      ++pos_;
      Value& child = v.object[key];
      if (!ParseValue(child, depth + 1)) return false;
      SkipWs();
      if (pos_ >= s_.size()) return false;
      if (s_[pos_] == ',') { ++pos_; continue; }
      if (s_[pos_] == '}') { ++pos_; return true; }
      return false;
    }
  }

  std::string_view s_;
  size_t pos_ = 0;
};

}  // namespace

const Value& Value::operator[](std::string_view key) const {
  if (type != Type::Object) return Null();
  auto it = object.find(key);
  return it == object.end() ? Null() : it->second;
}

const Value& Value::operator[](size_t index) const {
  if (type != Type::Array || index >= array.size()) return Null();
  return array[index];
}

std::string Value::AsString(std::string_view fallback) const {
  if (type == Type::String) return string;
  if (type == Type::Number) return std::to_string(static_cast<long long>(number));
  return std::string(fallback);
}

int Value::AsInt(int fallback) const {
  if (type == Type::Number) return static_cast<int>(number);
  if (type == Type::String) return std::atoi(string.c_str());
  return fallback;
}

bool Parse(std::string_view text, Value& out) {
  out = Value();
  return Parser(text).ParseDocument(out);
}

void AppendQuoted(std::string& out, std::string_view s) {
  static constexpr char kHex[] = "0123456789abcdef";
  out += '"';
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 15];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  out += '"';
}

}  // namespace pf::json
