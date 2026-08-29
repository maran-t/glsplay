// Minimal JSON field extraction.
//
// The host exchanges six message shapes with the broker, all flat objects with
// string and number fields. A full JSON library would be a dependency added
// for six messages, so this extracts the specific fields we need instead.
//
// It is deliberately not a general parser: it does not validate structure and
// does not handle nested arrays. Every value it returns is treated as
// untrusted input by the caller.

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace glsplay::json {

// Unescapes a JSON string body. SDP is full of \r\n, so this matters.
inline std::string Unescape(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] != '\\' || i + 1 >= raw.size()) {
      out += raw[i];
      continue;
    }
    switch (raw[++i]) {
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'b': out += '\b'; break;
      case 'f': out += '\f'; break;
      case '"': out += '"'; break;
      case '\\': out += '\\'; break;
      case '/': out += '/'; break;
      case 'u': {
        if (i + 4 >= raw.size()) break;
        const std::string hex(raw.substr(i + 1, 4));
        i += 4;
        const auto code = static_cast<unsigned>(std::stoul(hex, nullptr, 16));
        // Only the BMP subset that fits UTF-8 in up to three bytes. Surrogate
        // pairs do not appear in SDP or ICE candidates.
        if (code < 0x80) {
          out += static_cast<char>(code);
        } else if (code < 0x800) {
          out += static_cast<char>(0xC0 | (code >> 6));
          out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
          out += static_cast<char>(0xE0 | (code >> 12));
          out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (code & 0x3F));
        }
        break;
      }
      default: out += raw[i]; break;
    }
  }
  return out;
}

inline std::string Escape(std::string_view raw) {
  std::string out;
  out.reserve(raw.size() + 16);
  for (const char c : raw) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
          out += buf;
        } else {
          out += c;
        }
    }
  }
  return out;
}

// Finds "key": and returns the position just past the colon, or npos.
inline size_t FindKey(std::string_view json, std::string_view key) {
  std::string needle;
  needle.reserve(key.size() + 3);
  needle += '"';
  needle.append(key);
  needle += '"';

  size_t at = json.find(needle);
  while (at != std::string_view::npos) {
    size_t cursor = at + needle.size();
    while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) ++cursor;
    if (cursor < json.size() && json[cursor] == ':') return cursor + 1;
    at = json.find(needle, at + 1);
  }
  return std::string_view::npos;
}

// Extracts a string value, honouring escaped quotes inside it.
inline std::optional<std::string> GetString(std::string_view json, std::string_view key) {
  size_t cursor = FindKey(json, key);
  if (cursor == std::string_view::npos) return std::nullopt;
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) ++cursor;
  if (cursor >= json.size() || json[cursor] != '"') return std::nullopt;

  const size_t start = ++cursor;
  while (cursor < json.size()) {
    if (json[cursor] == '\\') { cursor += 2; continue; }
    if (json[cursor] == '"') break;
    ++cursor;
  }
  if (cursor >= json.size()) return std::nullopt;
  return Unescape(json.substr(start, cursor - start));
}

inline std::optional<int> GetInt(std::string_view json, std::string_view key) {
  size_t cursor = FindKey(json, key);
  if (cursor == std::string_view::npos) return std::nullopt;
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) ++cursor;
  if (cursor >= json.size()) return std::nullopt;
  if (json.compare(cursor, 4, "null") == 0) return std::nullopt;

  const size_t start = cursor;
  if (cursor < json.size() && (json[cursor] == '-' || json[cursor] == '+')) ++cursor;
  while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') ++cursor;
  if (cursor == start) return std::nullopt;
  try {
    return std::stoi(std::string(json.substr(start, cursor - start)));
  } catch (...) {
    return std::nullopt;
  }
}

inline std::optional<bool> GetBool(std::string_view json, std::string_view key) {
  size_t cursor = FindKey(json, key);
  if (cursor == std::string_view::npos) return std::nullopt;
  while (cursor < json.size() && (json[cursor] == ' ' || json[cursor] == '\t')) ++cursor;
  if (json.compare(cursor, 4, "true") == 0) return true;
  if (json.compare(cursor, 5, "false") == 0) return false;
  return std::nullopt;
}

}  // namespace glsplay::json
