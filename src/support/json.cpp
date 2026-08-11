// SPDX-License-Identifier: MIT

#include "json.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "utf.h"
#include "vlarray.h"

namespace {

    using namespace stdc;

    struct EmptyValues {
        static inline const JsonValue &nullValue() {
            static const JsonValue null;
            return null;
        }
        static inline const JsonArray &emptyArray() {
            static const JsonArray emptyArray;
            return emptyArray;
        }
        static inline const JsonObject &emptyObject() {
            static const JsonObject emptyObject;
            return emptyObject;
        }
    };

    // ------------------------------------------------------------------------------------------
    // Text output
    // ------------------------------------------------------------------------------------------

    void appendUtf8(std::string &out, char32_t cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | (cp >> 6));
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | (cp >> 12));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | (cp >> 18));
            out += char(0x80 | ((cp >> 12) & 0x3F));
            out += char(0x80 | ((cp >> 6) & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }

    void quoteTo(std::string &out, std::string_view s) {
        std::string sanitized;
        if (!stdc::utf::is_valid_utf8(s)) {
            // There is nowhere to report this from, and refusing to serialize would make toJson()
            // the one accessor that can fail. Substituting U+FFFD is what stdc::utf does by
            // default, and it leaves the rest of the document readable.
            sanitized = stdc::utf::utf32_to_utf8(stdc::utf::utf8_to_utf32(s));
            s = sanitized;
        }

        out += '"';
        for (size_t i = 0; i < s.size();) {
            // Most text needs no escaping, so it goes out one range at a time. A byte above
            // 0x7F is ordinary here, which is what keeps the text UTF-8 rather than escapes.
            const size_t start = i;
            while (i < s.size() && s[i] != '"' && s[i] != '\\' && uint8_t(s[i]) >= 0x20) {
                ++i;
            }
            out.append(s.data() + start, i - start);
            if (i == s.size()) {
                break;
            }
            const char c = s[i++];
            switch (c) {
                case '"':
                    out += "\\\"";
                    continue;
                case '\\':
                    out += "\\\\";
                    continue;
                case '\b':
                    out += "\\b";
                    continue;
                case '\f':
                    out += "\\f";
                    continue;
                case '\n':
                    out += "\\n";
                    continue;
                case '\r':
                    out += "\\r";
                    continue;
                case '\t':
                    out += "\\t";
                    continue;
                default:
                    break;
            }
            // A control character with no name of its own, and nothing else: the run stops only
            // at a quote, a backslash or a byte below 0x20, and the switch took the rest.
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\u%04x", unsigned(uint8_t(c)));
            out += buf;
        }
        out += '"';
    }

    void formatDouble(std::string &out, double d) {
        if (!std::isfinite(d)) {
            // JSON cannot write these at all. Null is what the value reads back as.
            out += "null";
            return;
        }

        char buf[40];
        size_t n;
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
        auto res = std::to_chars(buf, buf + sizeof(buf), d);
        n = size_t(res.ptr - buf);
#else
        // The shortest form that reads back as the same value. Seventeen digits always suffice for
        // a double, but most values need fewer and look better for it.
        int written = 0;
        for (int precision = 15; precision <= 17; ++precision) {
            written = std::snprintf(buf, sizeof(buf), "%.*g", precision, d);
            if (std::strtod(buf, nullptr) == d) {
                break;
            }
        }
        n = size_t(written);
#endif
        std::string_view sv(buf, n);
        out += sv;

        // A double that happens to be integral has to keep looking like one, or a round trip
        // through text turns it into an integer.
        if (sv.find_first_of(".eE") == std::string_view::npos) {
            out += ".0";
        }
    }

    template <class T>
    void appendInteger(std::string &out, T value) {
        char buf[32];
        const auto result = std::to_chars(buf, buf + sizeof(buf), value);
        out.append(buf, result.ptr);
    }

    void dumpTo(std::string &out, const JsonValue &v, int indent, int depth) {
        const bool pretty = indent > 0;

        auto newline = [&](int d) {
            if (pretty) {
                out += '\n';
                out.append(size_t(indent) * size_t(d), ' ');
            }
        };

        switch (v.type()) {
            case JsonValue::Null:
                out += "null";
                return;
            case JsonValue::Bool:
                out += v.toBool() ? "true" : "false";
                return;
            case JsonValue::Int:
                appendInteger(out, v.toInt());
                return;
            case JsonValue::Double:
                formatDouble(out, v.toDouble());
                return;
            case JsonValue::String:
                quoteTo(out, v.toStringView());
                return;
            case JsonValue::Binary: {
                // Binary has no JSON form. This is the shape it takes so a document holding one is
                // still writable, and it does not read back as binary.
                out += "{\"bytes\":[";
                const auto &bytes = v.toBinary();
                for (size_t i = 0; i < bytes.size(); ++i) {
                    if (i) {
                        out += ',';
                    }
                    appendInteger(out, unsigned(bytes[i]));
                }
                out += "],\"subtype\":null}";
                return;
            }
            case JsonValue::Array: {
                const auto &arr = v.toArray();
                if (arr.empty()) {
                    out += "[]";
                    return;
                }
                out += '[';
                for (size_t i = 0; i < arr.size(); ++i) {
                    if (i) {
                        out += ',';
                    }
                    newline(depth + 1);
                    dumpTo(out, arr[i], indent, depth + 1);
                }
                newline(depth);
                out += ']';
                return;
            }
            case JsonValue::Object: {
                const auto &obj = v.toObject();
                if (obj.empty()) {
                    out += "{}";
                    return;
                }
                out += '{';
                bool first = true;
                for (const auto &item : obj) {
                    if (!first) {
                        out += ',';
                    }
                    first = false;
                    newline(depth + 1);
                    quoteTo(out, item.first);
                    out += pretty ? ": " : ":";
                    dumpTo(out, item.second, indent, depth + 1);
                }
                newline(depth);
                out += '}';
                return;
            }
        }
    }

    // ------------------------------------------------------------------------------------------
    // Text input
    // ------------------------------------------------------------------------------------------

    /// A recursive descent parser over the whole input.
    ///
    /// \note The nesting limit is not a formality. Without it a document of nothing but opening
    ///       brackets overflows the stack, and a package manifest does not necessarily come from
    ///       someone trustworthy.
    class Parser {
    public:
        /// Deep enough for real documents -- the deepest in the JSON test corpus nests 468 -- and
        /// far enough below what the stack can take. A debug build on Windows, where the frames
        /// are widest and the stack is a megabyte, faults at around 950 levels, so this keeps
        /// most of a factor of two in the worst case and much more in a release build.
        static constexpr int maxDepth = 512;

        Parser(std::string_view text, bool comments) : _s(text), _comments(comments) {
        }

        bool parse(JsonValue *out) {
            // A byte order mark carries no information in UTF-8, but editors on Windows write one
            // anyway. RFC 8259 lets a parser skip it, so skip it.
            if (_s.size() >= 3 && _s.compare(0, 3, "\xEF\xBB\xBF") == 0) {
                _pos = 3;
            }
            skipSpace();
            if (!parseValue(out, 0)) {
                return false;
            }
            skipSpace();
            if (_pos != _s.size()) {
                return fail("trailing content after the value");
            }
            return true;
        }

        const std::string &error() const {
            return _error;
        }

    private:
        bool fail(const std::string &what) {
            if (!_error.empty()) {
                return false;
            }
            // Counted here rather than tracked as we go, since it only matters once.
            size_t line = 1;
            size_t column = 1;
            for (size_t i = 0; i < _pos && i < _s.size(); ++i) {
                if (_s[i] == '\n') {
                    ++line;
                    column = 1;
                } else {
                    ++column;
                }
            }
            _error = "parse error at line " + std::to_string(line) + ", column " +
                     std::to_string(column) + ": " + what;
            return false;
        }

        bool atEnd() const {
            return _pos >= _s.size();
        }

        char peek() const {
            return _s[_pos];
        }

        void skipSpace() {
            for (;;) {
                while (!atEnd() &&
                       (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) {
                    ++_pos;
                }
                if (!_comments || _pos + 1 >= _s.size() || peek() != '/') {
                    return;
                }
                if (_s[_pos + 1] == '/') {
                    _pos += 2;
                    while (!atEnd() && peek() != '\n') {
                        ++_pos;
                    }
                } else if (_s[_pos + 1] == '*') {
                    _pos += 2;
                    while (_pos + 1 < _s.size() && !(peek() == '*' && _s[_pos + 1] == '/')) {
                        ++_pos;
                    }
                    // An unterminated comment is caught by whatever expected a value next.
                    _pos = _pos + 1 < _s.size() ? _pos + 2 : _s.size();
                } else {
                    return;
                }
            }
        }

        bool literal(std::string_view word) {
            if (_s.compare(_pos, word.size(), word) != 0) {
                return false;
            }
            _pos += word.size();
            return true;
        }

        bool parseValue(JsonValue *out, int depth) {
            if (depth > maxDepth) {
                return fail("nested too deeply");
            }
            if (atEnd()) {
                return fail("expected a value");
            }
            switch (peek()) {
                case 'n':
                    if (!literal("null")) {
                        return fail("expected a value");
                    }
                    *out = JsonValue();
                    return true;
                case 't':
                    if (!literal("true")) {
                        return fail("expected a value");
                    }
                    *out = JsonValue(true);
                    return true;
                case 'f':
                    if (!literal("false")) {
                        return fail("expected a value");
                    }
                    *out = JsonValue(false);
                    return true;
                case '"': {
                    std::string s;
                    if (!parseString(&s)) {
                        return false;
                    }
                    *out = JsonValue(std::move(s));
                    return true;
                }
                case '[':
                    return parseArray(out, depth);
                case '{':
                    return parseObject(out, depth);
                default:
                    return parseNumber(out);
            }
        }

        bool parseArray(JsonValue *out, int depth) {
            ++_pos; // '['
            JsonArray arr;
            skipSpace();
            if (!atEnd() && peek() == ']') {
                ++_pos;
                *out = JsonValue(std::move(arr));
                return true;
            }
            for (;;) {
                skipSpace();
                // The slot has to exist before the value can be parsed into it, so the count is
                // guessed here rather than deduced from a comma later. Two is the guess; a
                // one-element array pays one unused slot, not another allocation.
                if (arr.empty()) {
                    arr.reserve(2);
                }
                arr.emplace_back();
                if (!parseValue(&arr.back(), depth + 1)) {
                    return false;
                }
                skipSpace();
                if (atEnd()) {
                    return fail("expected ',' or ']'");
                }
                bool hasNext = false;
                if (peek() == ',') {
                    ++_pos;
                    hasNext = true;
                } else if (peek() == ']') {
                    ++_pos;
                } else {
                    return fail("expected ',' or ']'");
                }

                if (!hasNext) {
                    *out = JsonValue(std::move(arr));
                    return true;
                }
            }
        }

        bool parseObject(JsonValue *out, int depth) {
            ++_pos; // '{'
            JsonObject obj;
            skipSpace();
            if (!atEnd() && peek() == '}') {
                ++_pos;
                *out = JsonValue(std::move(obj));
                return true;
            }
            for (;;) {
                skipSpace();
                if (atEnd() || peek() != '"') {
                    return fail("expected a key");
                }
                std::string key;
                if (!parseString(&key)) {
                    return false;
                }
                skipSpace();
                if (atEnd() || peek() != ':') {
                    return fail("expected ':'");
                }
                ++_pos;
                skipSpace();
                // A repeated key keeps the last one, which is what every JSON reader does: the
                // node is taken first, and the value is parsed straight into it, over whatever a
                // previous occurrence of the key left there.
                auto it = obj.try_emplace(std::move(key)).first;
                if (!parseValue(&it->second, depth + 1)) {
                    return false;
                }
                skipSpace();
                if (atEnd()) {
                    return fail("expected ',' or '}'");
                }
                if (peek() == ',') {
                    ++_pos;
                    continue;
                }
                if (peek() == '}') {
                    ++_pos;
                    *out = JsonValue(std::move(obj));
                    return true;
                }
                return fail("expected ',' or '}'");
            }
        }

        bool hex4(char32_t *out) {
            if (_pos + 4 > _s.size()) {
                return false;
            }
            char32_t v = 0;
            for (int i = 0; i < 4; ++i) {
                char c = _s[_pos + size_t(i)];
                v <<= 4;
                if (c >= '0' && c <= '9') {
                    v |= char32_t(c - '0');
                } else if (c >= 'a' && c <= 'f') {
                    v |= char32_t(c - 'a' + 10);
                } else if (c >= 'A' && c <= 'F') {
                    v |= char32_t(c - 'A' + 10);
                } else {
                    return false;
                }
            }
            _pos += 4;
            *out = v;
            return true;
        }

        bool parseString(std::string *out) {
            ++_pos; // '"'
            std::string res;
            bool hasNonAscii = false;
            for (;;) {
                // Ordinary bytes dominate real documents. Append each uninterrupted range once,
                // leaving the switch below only the escapes and errors it actually has to decode.
                const size_t start = _pos;
                while (!atEnd() && peek() != '"' && peek() != '\\' && uint8_t(peek()) >= 0x20) {
                    hasNonAscii |= uint8_t(peek()) >= 0x80;
                    ++_pos;
                }
                res.append(_s.data() + start, _pos - start);
                if (atEnd()) {
                    return fail("unterminated string");
                }
                char c = peek();
                if (c == '"') {
                    ++_pos;
                    break;
                }
                if (uint8_t(c) < 0x20) {
                    return fail("control character in string");
                }

                ++_pos;
                if (atEnd()) {
                    return fail("unterminated escape");
                }
                char e = peek();
                ++_pos;
                switch (e) {
                    case '"':
                        res += '"';
                        break;
                    case '\\':
                        res += '\\';
                        break;
                    case '/':
                        res += '/';
                        break;
                    case 'b':
                        res += '\b';
                        break;
                    case 'f':
                        res += '\f';
                        break;
                    case 'n':
                        res += '\n';
                        break;
                    case 'r':
                        res += '\r';
                        break;
                    case 't':
                        res += '\t';
                        break;
                    case 'u': {
                        char32_t cp;
                        if (!hex4(&cp)) {
                            return fail("expected four hexadecimal digits");
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            // A high surrogate means nothing without the low one after it.
                            if (_pos + 1 >= _s.size() || _s[_pos] != '\\' || _s[_pos + 1] != 'u') {
                                return fail("expected a low surrogate");
                            }
                            _pos += 2;
                            char32_t low;
                            if (!hex4(&low)) {
                                return fail("expected four hexadecimal digits");
                            }
                            if (low < 0xDC00 || low > 0xDFFF) {
                                return fail("expected a low surrogate");
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            return fail("unpaired low surrogate");
                        }
                        appendUtf8(res, cp);
                        break;
                    }
                    default:
                        return fail("unknown escape");
                }
            }

            // ASCII was checked for controls above and is UTF-8 already. Only strings containing
            // raw high bytes need the full validator; \u escapes were validated while decoded.
            if (hasNonAscii && !stdc::utf::is_valid_utf8(res)) {
                return fail("string is not valid UTF-8");
            }
            *out = std::move(res);
            return true;
        }

        bool parseNumber(JsonValue *out) {
            size_t start = _pos;
            if (!atEnd() && peek() == '-') {
                ++_pos;
            }
            size_t digitsStart = _pos;
            while (!atEnd() && peek() >= '0' && peek() <= '9') {
                ++_pos;
            }
            if (_pos == digitsStart) {
                return fail("expected a value");
            }
            // A leading zero is not one number, it is two written together.
            if (_s[digitsStart] == '0' && _pos - digitsStart > 1) {
                _pos = digitsStart;
                return fail("number has a leading zero");
            }

            bool isDouble = false;
            if (!atEnd() && peek() == '.') {
                isDouble = true;
                ++_pos;
                size_t fracStart = _pos;
                while (!atEnd() && peek() >= '0' && peek() <= '9') {
                    ++_pos;
                }
                if (_pos == fracStart) {
                    return fail("expected a digit after the decimal point");
                }
            }
            if (!atEnd() && (peek() == 'e' || peek() == 'E')) {
                isDouble = true;
                ++_pos;
                if (!atEnd() && (peek() == '+' || peek() == '-')) {
                    ++_pos;
                }
                size_t expStart = _pos;
                while (!atEnd() && peek() >= '0' && peek() <= '9') {
                    ++_pos;
                }
                if (_pos == expStart) {
                    return fail("expected a digit in the exponent");
                }
            }

            const char *first = _s.data() + start;
            const char *last = _s.data() + _pos;

            if (!isDouble) {
                // The written form decides the type. An integer too large for its type becomes a
                // double rather than a parse error, which is what every other reader does.
                if (_s[start] == '-') {
                    int64_t v;
                    if (std::from_chars(first, last, v).ec == std::errc()) {
                        *out = JsonValue(v);
                        return true;
                    }
                } else {
                    uint64_t v;
                    if (std::from_chars(first, last, v).ec == std::errc()) {
                        *out = JsonValue(v);
                        return true;
                    }
                }
            }

            // Floating-point from_chars is not everywhere yet. Where it is available, it avoids
            // copying every number merely to give strtod a terminator.
#if defined(__cpp_lib_to_chars) && __cpp_lib_to_chars >= 201611L
            double parsed;
            const auto result = std::from_chars(first, last, parsed, std::chars_format::general);
            if (result.ec == std::errc() && result.ptr == last) {
                *out = JsonValue(parsed);
                return true;
            }
#endif

            vlarray<char, 64> text(first, last);
            text.push_back('\0');
            char *end = nullptr;
            double d = std::strtod(text.data(), &end);
            if (end != text.data() + text.size() - 1) {
                return fail("malformed number");
            }
            *out = JsonValue(d);
            return true;
        }

        std::string_view _s;
        size_t _pos = 0;
        bool _comments;
        std::string _error;
    };

    // ------------------------------------------------------------------------------------------
    // CBOR
    // ------------------------------------------------------------------------------------------

    namespace cbor {

        void putHead(std::vector<uint8_t> &out, uint8_t major, uint64_t arg) {
            auto m = uint8_t(major << 5);
            if (arg < 24) {
                out.push_back(uint8_t(m | arg));
            } else if (arg <= 0xFF) {
                out.push_back(uint8_t(m | 24));
                out.push_back(uint8_t(arg));
            } else if (arg <= 0xFFFF) {
                out.push_back(uint8_t(m | 25));
                out.push_back(uint8_t(arg >> 8));
                out.push_back(uint8_t(arg));
            } else if (arg <= 0xFFFFFFFF) {
                out.push_back(uint8_t(m | 26));
                for (int shift = 24; shift >= 0; shift -= 8) {
                    out.push_back(uint8_t(arg >> shift));
                }
            } else {
                out.push_back(uint8_t(m | 27));
                for (int shift = 56; shift >= 0; shift -= 8) {
                    out.push_back(uint8_t(arg >> shift));
                }
            }
        }

        void putBytes(std::vector<uint8_t> &out, uint8_t major, std::string_view s) {
            putHead(out, major, s.size());
            out.insert(out.end(), s.begin(), s.end());
        }

        void encode(std::vector<uint8_t> &out, const JsonValue &v) {
            switch (v.type()) {
                case JsonValue::Null:
                    out.push_back(0xF6);
                    return;
                case JsonValue::Bool:
                    out.push_back(v.toBool() ? 0xF5 : 0xF4);
                    return;
                case JsonValue::Int: {
                    auto i = v.toInt();
                    if (i >= 0) {
                        putHead(out, 0, uint64_t(i));
                    } else {
                        // Major type 1 stores minus one minus the value, which is how the most
                        // negative integer encodes without needing a wider type than it has.
                        putHead(out, 1, uint64_t(-(i + 1)));
                    }
                    return;
                }
                case JsonValue::Double: {
                    out.push_back(0xFB);
                    double d = v.toDouble();
                    uint64_t bits;
                    std::memcpy(&bits, &d, sizeof(bits));
                    for (int shift = 56; shift >= 0; shift -= 8) {
                        out.push_back(uint8_t(bits >> shift));
                    }
                    return;
                }
                case JsonValue::String:
                    putBytes(out, 3, v.toStringView());
                    return;
                case JsonValue::Binary: {
                    const auto &b = v.toBinary();
                    putHead(out, 2, b.size());
                    out.insert(out.end(), b.begin(), b.end());
                    return;
                }
                case JsonValue::Array: {
                    const auto &arr = v.toArray();
                    putHead(out, 4, arr.size());
                    for (const auto &item : arr) {
                        encode(out, item);
                    }
                    return;
                }
                case JsonValue::Object: {
                    const auto &obj = v.toObject();
                    putHead(out, 5, obj.size());
                    for (const auto &item : obj) {
                        putBytes(out, 3, item.first);
                        encode(out, item.second);
                    }
                    return;
                }
            }
        }

        /// \note Tags are rejected rather than handled. Nothing writes them here, and accepting a
        ///       shape we never produce is surface with no reader. Indefinite lengths are a
        ///       different matter: we never write one, but other encoders do, and a decoder that
        ///       cannot read them cannot read their output.
        class Decoder {
        public:
            static constexpr int maxDepth = Parser::maxDepth;

            /// The initial byte that ends an indefinite-length string, array or map.
            static constexpr uint8_t breakByte = 0xFF;

            explicit Decoder(stdc::array_view<uint8_t> data) : _d(data) {
            }

            bool decode(JsonValue *out) {
                if (!decodeValue(out, 0)) {
                    return false;
                }
                if (_pos != _d.size()) {
                    return fail("trailing bytes after the value");
                }
                return true;
            }

            const std::string &error() const {
                return _error;
            }

        private:
            bool fail(const std::string &what) {
                if (_error.empty()) {
                    _error = "cbor error at byte " + std::to_string(_pos) + ": " + what;
                }
                return false;
            }

            bool take(uint8_t *out) {
                if (_pos >= _d.size()) {
                    return fail("input ended early");
                }
                *out = _d[_pos++];
                return true;
            }

            bool takeBig(int bytes, uint64_t *out) {
                if (_pos + size_t(bytes) > _d.size()) {
                    return fail("input ended early");
                }
                uint64_t v = 0;
                for (int i = 0; i < bytes; ++i) {
                    v = (v << 8) | _d[_pos++];
                }
                *out = v;
                return true;
            }

            /// Reads the argument that follows an initial byte.
            ///
            /// \param indefinite Where to report minor 31, which stands for a length that is not
            ///        given up front. Only the string, array and map types may carry one, so
            ///        passing null is how the rest reject it.
            bool argument(uint8_t initial, uint64_t *out, bool *indefinite = nullptr) {
                if (indefinite) {
                    *indefinite = false;
                }
                uint8_t minor = initial & 0x1F;
                if (minor < 24) {
                    *out = minor;
                    return true;
                }
                switch (minor) {
                    case 24:
                        return takeBig(1, out);
                    case 25:
                        return takeBig(2, out);
                    case 26:
                        return takeBig(4, out);
                    case 27:
                        return takeBig(8, out);
                    case 31:
                        if (!indefinite) {
                            return fail("this type cannot have an indefinite length");
                        }
                        *indefinite = true;
                        return true;
                    default:
                        return fail("reserved length encoding");
                }
            }

            bool rawBytes(uint64_t count, std::string *out) {
                if (count > _d.size() - _pos) {
                    return fail("input ended early");
                }
                out->assign(reinterpret_cast<const char *>(_d.data() + _pos), size_t(count));
                _pos += size_t(count);
                return true;
            }

            /// Whether the next byte ends an indefinite-length item, consuming it if so.
            bool atBreak(bool *broke) {
                if (_pos >= _d.size()) {
                    return fail("input ended before the break");
                }
                *broke = _d[_pos] == breakByte;
                if (*broke) {
                    ++_pos;
                }
                return true;
            }

            /// Reads the pieces of an indefinite-length string up to the break and joins them.
            ///
            /// Each piece is a definite-length string of the same major type, and a text piece has
            /// to be well formed on its own -- a split through the middle of a code point is not
            /// something the concatenation would show.
            bool chunkedBytes(uint8_t major, std::string *out) {
                for (;;) {
                    bool broke = false;
                    if (!atBreak(&broke)) {
                        return false;
                    }
                    if (broke) {
                        return true;
                    }

                    uint8_t initial;
                    if (!take(&initial)) {
                        return false;
                    }
                    if (uint8_t(initial >> 5) != major) {
                        return fail(
                            "an indefinite-length string is made of strings of its own kind");
                    }
                    if ((initial & 0x1F) == 31) {
                        return fail("a piece of an indefinite-length string has to have a length");
                    }
                    uint64_t count = 0;
                    if (!argument(initial, &count)) {
                        return false;
                    }
                    std::string chunk;
                    if (!rawBytes(count, &chunk)) {
                        return false;
                    }
                    if (major == 3 && !stdc::utf::is_valid_utf8(chunk)) {
                        return fail("text string is not valid UTF-8");
                    }
                    *out += chunk;
                }
            }

            bool decodeValue(JsonValue *out, int depth) {
                if (depth > maxDepth) {
                    return fail("nested too deeply");
                }
                uint8_t initial;
                if (!take(&initial)) {
                    return false;
                }
                auto major = uint8_t(initial >> 5);
                uint64_t arg = 0;

                switch (major) {
                    case 0:
                        if (!argument(initial, &arg)) {
                            return false;
                        }
                        *out = JsonValue(arg);
                        return true;
                    case 1: {
                        if (!argument(initial, &arg)) {
                            return false;
                        }
                        if (arg > uint64_t(INT64_MAX)) {
                            return fail("negative integer is out of range");
                        }
                        *out = JsonValue(-int64_t(arg) - 1);
                        return true;
                    }
                    case 2: {
                        bool indefinite = false;
                        if (!argument(initial, &arg, &indefinite)) {
                            return false;
                        }
                        std::string raw;
                        if (indefinite ? !chunkedBytes(2, &raw) : !rawBytes(arg, &raw)) {
                            return false;
                        }
                        *out = JsonValue(stdc::array_view<uint8_t>(
                            reinterpret_cast<const uint8_t *>(raw.data()), raw.size()));
                        return true;
                    }
                    case 3: {
                        bool indefinite = false;
                        if (!argument(initial, &arg, &indefinite)) {
                            return false;
                        }
                        std::string raw;
                        if (indefinite) {
                            if (!chunkedBytes(3, &raw)) {
                                return false;
                            }
                        } else {
                            if (!rawBytes(arg, &raw)) {
                                return false;
                            }
                            if (!stdc::utf::is_valid_utf8(raw)) {
                                return fail("text string is not valid UTF-8");
                            }
                        }
                        *out = JsonValue(std::move(raw));
                        return true;
                    }
                    case 4: {
                        bool indefinite = false;
                        if (!argument(initial, &arg, &indefinite)) {
                            return false;
                        }
                        JsonArray arr;
                        if (!indefinite) {
                            // Every value occupies at least one byte, so this also rejects an
                            // impossible count before reserve can trust untrusted input.
                            if (arg > _d.size() - _pos) {
                                return fail("input ended early");
                            }
                            arr.reserve(size_t(arg));
                        }
                        for (uint64_t i = 0; indefinite || i < arg; ++i) {
                            if (indefinite) {
                                bool broke = false;
                                if (!atBreak(&broke)) {
                                    return false;
                                }
                                if (broke) {
                                    break;
                                }
                            }
                            JsonValue item;
                            if (!decodeValue(&item, depth + 1)) {
                                return false;
                            }
                            arr.push_back(std::move(item));
                        }
                        *out = JsonValue(std::move(arr));
                        return true;
                    }
                    case 5: {
                        bool indefinite = false;
                        if (!argument(initial, &arg, &indefinite)) {
                            return false;
                        }
                        JsonObject obj;
                        for (uint64_t i = 0; indefinite || i < arg; ++i) {
                            if (indefinite) {
                                bool broke = false;
                                if (!atBreak(&broke)) {
                                    return false;
                                }
                                if (broke) {
                                    break;
                                }
                            }
                            JsonValue key;
                            if (!decodeValue(&key, depth + 1)) {
                                return false;
                            }
                            if (!key.isString()) {
                                return fail("a map key has to be a text string");
                            }
                            JsonValue value;
                            if (!decodeValue(&value, depth + 1)) {
                                return false;
                            }
                            obj[key.toString()] = std::move(value);
                        }
                        *out = JsonValue(std::move(obj));
                        return true;
                    }
                    case 6:
                        return fail("tags are not supported");
                    default:
                        break;
                }

                // Major type 7, the simple values and the floats.
                switch (initial) {
                    case 0xF4:
                        *out = JsonValue(false);
                        return true;
                    case 0xF5:
                        *out = JsonValue(true);
                        return true;
                    case 0xF6:
                    case 0xF7:
                        *out = JsonValue();
                        return true;
                    case 0xF9: {
                        uint64_t bits;
                        if (!takeBig(2, &bits)) {
                            return false;
                        }
                        *out = JsonValue(fromHalf(uint16_t(bits)));
                        return true;
                    }
                    case 0xFA: {
                        uint64_t bits;
                        if (!takeBig(4, &bits)) {
                            return false;
                        }
                        auto narrow = uint32_t(bits);
                        float f;
                        std::memcpy(&f, &narrow, sizeof(f));
                        *out = JsonValue(double(f));
                        return true;
                    }
                    case 0xFB: {
                        uint64_t bits;
                        if (!takeBig(8, &bits)) {
                            return false;
                        }
                        double d;
                        std::memcpy(&d, &bits, sizeof(d));
                        *out = JsonValue(d);
                        return true;
                    }
                    case breakByte:
                        return fail("a break outside an indefinite-length item");
                    default:
                        return fail("unsupported initial byte");
                }
            }

            static double fromHalf(uint16_t h) {
                int exponent = (h >> 10) & 0x1F;
                int mantissa = h & 0x3FF;
                double value;
                if (exponent == 0) {
                    value = std::ldexp(double(mantissa), -24);
                } else if (exponent != 31) {
                    value = std::ldexp(double(mantissa + 1024), exponent - 25);
                } else {
                    value = mantissa == 0 ? HUGE_VAL : NAN;
                }
                return (h & 0x8000) ? -value : value;
            }

            stdc::array_view<uint8_t> _d;
            size_t _pos = 0;
            std::string _error;
        };

    }

}

namespace stdc {

    JsonValue::JsonValue(Type type) : _type(type) {
        _p.u = 0;
        switch (type) {
            case String:
                _p.s = new std::string();
                break;
            case Binary:
                _p.bin = new std::vector<uint8_t>();
                break;
            case Array:
                _p.arr = new JsonArray();
                break;
            case Object:
                _p.obj = new JsonObject();
                break;
            default:
                break;
        }
    }

    JsonValue::JsonValue(bool b) : _type(Bool) {
        _p.b = b;
    }

    JsonValue::JsonValue(double d) : _type(Double) {
        _p.d = d;
    }

    JsonValue::JsonValue(int64_t i) : _type(Int) {
        _p.i = i;
    }

    JsonValue::JsonValue(uint64_t u) {
        // There is no unsigned type to put this in. Anything an int64_t can hold stays exact, and
        // the range above it settles for a double.
        if (u <= uint64_t(INT64_MAX)) {
            _type = Int;
            _p.i = int64_t(u);
        } else {
            _type = Double;
            _p.d = double(u);
        }
    }

    JsonValue::JsonValue(std::string s) : _type(String) {
        _p.s = new std::string(std::move(s));
    }

    JsonValue::JsonValue(stdc::array_view<uint8_t> bytes) : _type(Binary) {
        _p.bin = new std::vector<uint8_t>(bytes.begin(), bytes.end());
    }

    JsonValue::JsonValue(const JsonArray &a) : _type(Array) {
        _p.arr = new JsonArray(a);
    }

    JsonValue::JsonValue(JsonArray &&a) noexcept : _type(Array) {
        _p.arr = new JsonArray(std::move(a));
    }

    JsonValue::JsonValue(const JsonObject &o) : _type(Object) {
        _p.obj = new JsonObject(o);
    }

    JsonValue::JsonValue(JsonObject &&o) noexcept : _type(Object) {
        _p.obj = new JsonObject(std::move(o));
    }

    JsonValue::~JsonValue() {
        reset();
    }

    JsonValue::JsonValue(const JsonValue &RHS) {
        copyFrom(RHS);
    }

    JsonValue::JsonValue(JsonValue &&RHS) noexcept : _type(RHS._type), _p(RHS._p) {
        RHS._type = Null;
        RHS._p.u = 0;
    }

    JsonValue &JsonValue::operator=(const JsonValue &RHS) {
        if (this != &RHS) {
            reset();
            copyFrom(RHS);
        }
        return *this;
    }

    void JsonValue::swap(JsonValue &RHS) noexcept {
        std::swap(_type, RHS._type);
        std::swap(_p, RHS._p);
    }

    void JsonValue::reset() noexcept {
        switch (_type) {
            case String:
                delete _p.s;
                break;
            case Binary:
                delete _p.bin;
                break;
            case Array:
                delete _p.arr;
                break;
            case Object:
                delete _p.obj;
                break;
            default:
                break;
        }
        _type = Null;
        _p.u = 0;
    }

    void JsonValue::copyFrom(const JsonValue &RHS) {
        _type = RHS._type;
        switch (_type) {
            case String:
                _p.s = new std::string(*RHS._p.s);
                break;
            case Binary:
                _p.bin = new std::vector<uint8_t>(*RHS._p.bin);
                break;
            case Array:
                _p.arr = new JsonArray(*RHS._p.arr);
                break;
            case Object:
                _p.obj = new JsonObject(*RHS._p.obj);
                break;
            default:
                _p = RHS._p;
                break;
        }
    }

    bool JsonValue::toBool(bool defaultValue) const {
        return _type == Bool ? _p.b : defaultValue;
    }

    double JsonValue::toDouble(double defaultValue) const {
        switch (_type) {
            case Int:
                return double(_p.i);
            case Double:
                return _p.d;
            default:
                return defaultValue;
        }
    }

    int64_t JsonValue::toInt(int64_t defaultValue) const {
        switch (_type) {
            case Int:
                return _p.i;
            case Double:
                // Truncated, not rounded, and undefined once the value is out of range, which is
                // what a cast does everywhere else too.
                return int64_t(_p.d);
            default:
                return defaultValue;
        }
    }

    std::string_view JsonValue::toStringView(std::string_view defaultValue) const {
        return _type == String ? std::string_view(*_p.s) : defaultValue;
    }

    const std::string &JsonValue::toString(const std::string &defaultValue) const {
        return _type == String ? *_p.s : defaultValue;
    }

    std::string JsonValue::toString(std::string &&defaultValue) const {
        if (_type == String) {
            return *_p.s;
        }
        return std::move(defaultValue);
    }

    stdc::array_view<uint8_t>
        JsonValue::toBinaryView(stdc::array_view<uint8_t> defaultValue) const {
        if (_type != Binary) {
            return defaultValue;
        }
        return stdc::array_view<uint8_t>(_p.bin->data(), _p.bin->size());
    }

    const std::vector<uint8_t> &
        JsonValue::toBinary(const std::vector<uint8_t> &defaultValue) const {
        return _type == Binary ? *_p.bin : defaultValue;
    }

    std::vector<uint8_t> JsonValue::toBinary(std::vector<uint8_t> &&defaultValue) const {
        if (_type == Binary) {
            return *_p.bin;
        }
        return std::move(defaultValue);
    }

    const JsonArray &JsonValue::toArray() const {
        return _type == Array ? *_p.arr : EmptyValues::emptyArray();
    }

    const JsonArray &JsonValue::toArray(const JsonArray &defaultValue) const {
        return _type == Array ? *_p.arr : defaultValue;
    }

    JsonArray JsonValue::toArray(JsonArray &&defaultValue) const {
        if (_type == Array) {
            return *_p.arr;
        }
        return std::move(defaultValue);
    }

    const JsonObject &JsonValue::toObject() const {
        return _type == Object ? *_p.obj : EmptyValues::emptyObject();
    }

    const JsonObject &JsonValue::toObject(const JsonObject &defaultValue) const {
        return _type == Object ? *_p.obj : defaultValue;
    }

    JsonObject JsonValue::toObject(JsonObject &&defaultValue) const {
        if (_type == Object) {
            return *_p.obj;
        }
        return std::move(defaultValue);
    }

    const JsonValue &JsonValue::operator[](std::string_view key) const {
        if (_type == Object) {
            auto it = _p.obj->find(key);
            if (it != _p.obj->end()) {
                return it->second;
            }
        }
        return EmptyValues::nullValue();
    }

    const JsonValue &JsonValue::operator[](size_t i) const {
        if (_type == Array && i < _p.arr->size()) {
            return (*_p.arr)[i];
        }
        return EmptyValues::nullValue();
    }

    bool JsonValue::operator==(const JsonValue &RHS) const {
        // A number written as 1 and a number written as 1.0 are the same number. Nothing else
        // compares across types.
        if (isNumber() && RHS.isNumber()) {
            if (_type == Int && RHS._type == Int) {
                return _p.i == RHS._p.i;
            }
            // A large integer loses precision here, which is the price of 1 and 1.0 being equal.
            return toDouble() == RHS.toDouble();
        }

        if (_type != RHS._type) {
            return false;
        }
        switch (_type) {
            case Null:
                return true;
            case Bool:
                return _p.b == RHS._p.b;
            case String:
                return *_p.s == *RHS._p.s;
            case Binary:
                return *_p.bin == *RHS._p.bin;
            case Array:
                return *_p.arr == *RHS._p.arr;
            case Object:
                return *_p.obj == *RHS._p.obj;
            default:
                return false;
        }
    }

    std::string JsonValue::toJson(int indent) const {
        std::string res;
        dumpTo(res, *this, indent, 0);
        return res;
    }

    JsonValue JsonValue::fromJson(std::string_view json, bool ignoreComments, std::string *error) {
        if (error) {
            error->clear();
        }
        Parser parser(json, ignoreComments);
        JsonValue res;
        if (!parser.parse(&res)) {
            if (error) {
                *error = parser.error();
            }
            return JsonValue();
        }
        return res;
    }

    std::vector<uint8_t> JsonValue::toCbor() const {
        std::vector<uint8_t> res;
        cbor::encode(res, *this);
        return res;
    }

    JsonValue JsonValue::fromCbor(stdc::array_view<uint8_t> cbor, std::string *error) {
        if (error) {
            error->clear();
        }
        cbor::Decoder decoder(cbor);
        JsonValue res;
        if (!decoder.decode(&res)) {
            if (error) {
                *error = decoder.error();
            }
            return JsonValue();
        }
        return res;
    }

}
