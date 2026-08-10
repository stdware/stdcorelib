// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_JSON_H
#define STDCORELIB_JSON_H

#include <cstdint>
#include <string>
#include <string_view>
#include <map>
#include <vector>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/adt/array_view.h>

/// \defgroup json JSON and CBOR
///
/// stdc::JsonValue reads and writes both, over the same tree.

namespace stdc {

    /// \addtogroup json
    /// @{

    class JsonValue;

    using JsonArray = std::vector<JsonValue>;

    using JsonObject = std::map<std::string, JsonValue>;

    /// JsonValue - An immutable JSON value, shaped after Qt's.
    ///
    /// Reading never fails and never throws. An accessor asked for a type the value does not have
    /// hands back the default it was given, and a subscript that finds nothing hands back a null
    /// value, so a chain of them needs no check at each step.
    ///
    /// \note A number keeps the form it was written in. \c 1 parses as \c Int and \c 1.0 as
    ///       \c Double, which is what a round trip through \c toJson has to preserve. Comparison
    ///       ignores the distinction and compares numerically.
    ///
    ///       An integer is exact, up to the range of \c int64_t. Anything outside it, including an
    ///       unsigned value above \c INT64_MAX, becomes a \c Double and is exact only up to 2^53.
    ///
    /// \warning Accessors returning a reference or view borrow this value. Calling one on a
    ///          temporary leaves the result dangling at the end of the statement.
    class STDC_EXPORT JsonValue {
    public:
        enum Type {
            Null = 0,
            Bool,
            Double,
            Int,
            String,
            Binary,
            Array,
            Object,
        };

        JsonValue(Type = Null);
        JsonValue(bool b);
        JsonValue(double d);
        inline JsonValue(int i) : JsonValue(int64_t(i)) {
        }
        inline JsonValue(uint32_t i) : JsonValue(uint64_t(i)) {
        }
        JsonValue(int64_t i);
        JsonValue(uint64_t u);
        JsonValue(std::string s);
        inline JsonValue(const char *s, int size = -1)
            : JsonValue(size < 0 ? std::string(s) : std::string(s, size)) {
        }
        JsonValue(array_view<uint8_t> bytes);
        inline JsonValue(const uint8_t *data, int size)
            : JsonValue(array_view<uint8_t>(data, size)) {
        }
        JsonValue(const JsonArray &a);
        JsonValue(JsonArray &&a) noexcept;
        JsonValue(const JsonObject &o);
        JsonValue(JsonObject &&o) noexcept;
        ~JsonValue();

        JsonValue(const JsonValue &RHS);
        JsonValue(JsonValue &&RHS) noexcept;
        JsonValue &operator=(const JsonValue &RHS);
        inline JsonValue &operator=(JsonValue &&RHS) noexcept {
            swap(RHS);
            return *this;
        }

        void swap(JsonValue &RHS) noexcept;

    public:
        inline Type type() const {
            return _type;
        }
        inline bool isNull() const {
            return type() == Null;
        }
        inline bool isBool() const {
            return type() == Bool;
        }
        inline bool isDouble() const {
            return type() == Double;
        }
        inline bool isInt() const {
            return type() == Int;
        }
        inline bool isNumber() const {
            return isDouble() || isInt();
        }
        inline bool isString() const {
            return type() == String;
        }
        inline bool isArray() const {
            return type() == Array;
        }
        inline bool isObject() const {
            return type() == Object;
        }

        bool toBool(bool defaultValue = false) const;
        double toDouble(double defaultValue = 0) const;
        int64_t toInt(int64_t defaultValue = 0) const;
        std::string_view toStringView(std::string_view defaultValue = {}) const;
        const std::string &toString(const std::string &defaultValue = {}) const;
        std::string toString(std::string &&defaultValue) const;
        array_view<uint8_t> toBinaryView(array_view<uint8_t> defaultValue = {}) const;
        const std::vector<uint8_t> &toBinary(const std::vector<uint8_t> &defaultValue = {}) const;
        std::vector<uint8_t> toBinary(std::vector<uint8_t> &&defaultValue) const;
        const JsonArray &toArray() const;
        const JsonArray &toArray(const JsonArray &defaultValue) const;
        JsonArray toArray(JsonArray &&defaultValue) const;
        const JsonObject &toObject() const;
        const JsonObject &toObject(const JsonObject &defaultValue) const;
        JsonObject toObject(JsonObject &&defaultValue) const;

        const JsonValue &operator[](std::string_view key) const;
        const JsonValue &operator[](size_t i) const;

        bool operator==(const JsonValue &RHS) const;
        inline bool operator!=(const JsonValue &RHS) const {
            return !(*this == RHS);
        }

    public:
        /// Returns the serialized JSON text of this value.
        ///
        /// \param indent The number of spaces to indent the JSON text. If negative, no indentation
        ///        is performed.
        std::string toJson(int indent = -1) const;

        /// Returns the serialized JsonValue instance of the given JSON text.
        ///
        /// \param json The text to parse.
        /// \param ignoreComments Whether comments should be ignored and treated like whitespace
        ///        (true) or yield a parse error (false)
        /// \param error Set to why the text was rejected, or cleared on success. A rejected
        ///        document and the text \c null both come back as a null value, so this tells
        ///        them apart.
        static JsonValue fromJson(std::string_view json, bool ignoreComments,
                                  std::string *error = nullptr);

        std::vector<uint8_t> toCbor() const;

        /// Returns the JsonValue the given CBOR encodes.
        ///
        /// \param cbor The bytes to decode.
        /// \param error Set to why the bytes were rejected, or cleared on success. A rejected
        ///        document and an encoded null both come back as a null value, so this tells
        ///        them apart.
        static JsonValue fromCbor(array_view<uint8_t> cbor, std::string *error = nullptr);

    private:
        // The alternatives, all trivially copyable, so the payload moves as one object rather
        // than one member at a time. Which member is live is _type and nothing else.
        //
        // Anything larger than a scalar sits behind a pointer this owns, and is copied when the
        // value is. A std::string alone is wider than everything here put together.
        union Payload {
            bool b;
            int64_t i;
            uint64_t u;
            double d;
            std::string *s;
            std::vector<uint8_t> *bin;
            JsonArray *arr;
            JsonObject *obj;
        };

        Type _type;
        Payload _p;

        // Frees what the live alternative owns, if it owns anything, and becomes null.
        void reset() noexcept;
        void copyFrom(const JsonValue &RHS);
    };

    /// @}
}

#endif // STDCORELIB_JSON_H
