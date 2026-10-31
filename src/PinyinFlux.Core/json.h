#pragma once

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pf::json {

// Minimal read-only JSON DOM, enough for vendor API responses.
struct Value {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0;
  std::string string;
  std::vector<Value> array;
  std::map<std::string, Value, std::less<>> object;

  bool IsArray() const { return type == Type::Array; }
  bool IsObject() const { return type == Type::Object; }
  bool IsString() const { return type == Type::String; }
  bool IsNumber() const { return type == Type::Number; }

  // Returns a shared null value when the key/index is missing.
  const Value& operator[](std::string_view key) const;
  const Value& operator[](size_t index) const;
  // Strings and numbers both coerce; anything else yields `fallback`.
  std::string AsString(std::string_view fallback = {}) const;
  int AsInt(int fallback = 0) const;
};

bool Parse(std::string_view text, Value& out);

// Appends `s` to `out` as a JSON string literal (with quotes).
void AppendQuoted(std::string& out, std::string_view s);

}  // namespace pf::json
