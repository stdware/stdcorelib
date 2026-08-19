// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_JSON_H
#define STDCORELIB_JSON_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/adt/array_view.h>

/// \defgroup json JSON and CBOR
///
/// stdc::json::Value reads and writes both, over the same tree.

namespace stdc::cbor {

    /// \addtogroup json
    /// @{

    /// Why a CBOR document was turned down, and where.
    ///
    /// The codec itself is json::Value::toCbor() and json::Value::fromCbor(), since one tree
    /// serves both encodings. This sits here rather than beside them because it says what went
    /// wrong with CBOR, and whatever else CBOR grows will want the same home.
    struct STDC_EXPORT DecodeError {
        enum Code {
            NoError = 0,
            UnexpectedEnd,   ///< the bytes stopped in the middle of an item
            IllegalEncoding, ///< a reserved encoding, or one this item is not allowed
            IllegalString,   ///< a text string that is not UTF-8
            OutOfRange,      ///< a number CBOR can write and json::Value cannot hold
            UnsupportedType, ///< tags, and map keys that are not text
            NestedTooDeeply,
            TrailingContent, ///< a whole item, and then more after it
        };

        Code code = NoError;
        size_t offset = 0; ///< bytes from the start
        std::string what;  ///< what was wrong, without the position in front of it

        explicit operator bool() const {
            return code != NoError;
        }

        std::string message() const;
    };

    /// @}
}

namespace stdc::json {

    /// \addtogroup json
    /// @{

    class Value;

    using Array = std::vector<Value>;

    using Object = std::map<std::string, Value, std::less<>>;

    /// Which of the eight kinds a Value is.
    ///
    /// \note Scoped, so that the \c Array and \c Object here leave the two names above alone. An
    ///       unscoped enumerator would hide them inside Value and everywhere else in here.
    enum class Type {
        Null = 0,
        Bool,
        Double,
        Int,
        String,
        Binary,
        Array,
        Object,
    };

    /// Why a JSON document was turned down, and where.
    struct STDC_EXPORT ParseError {
        enum Code {
            NoError = 0,
            UnexpectedEnd,   ///< the text stopped in the middle of something
            UnexpectedToken, ///< something was there, and it was not what belongs at that point
            IllegalNumber,   ///< a leading zero, a missing digit, an exponent with nothing in it
            IllegalEscape,   ///< an escape nothing answers to, or a broken surrogate pair
            IllegalString,   ///< a raw control character, or bytes that are not UTF-8
            NestedTooDeeply,
            TrailingContent,   ///< a whole value, and then more after it
            CommentNotAllowed, ///< a comment, where \a ignoreComments was not asked for
        };

        Code code = NoError;
        size_t offset = 0; ///< bytes from the start of the text
        size_t line = 1;   ///< counting from one
        size_t column = 1; ///< counting from one, in bytes rather than code points
        std::string what;  ///< what was wrong, without the position in front of it

        /// Whether this says a parse failed.
        explicit operator bool() const {
            return code != NoError;
        }

        /// The failure in words, with the line and column in front of it. Empty for NoError.
        std::string message() const;
    };

    /// An immutable JSON value, shaped after Qt's.
    ///
    /// Reading never fails and never throws. An accessor asked for a type the value does not have
    /// hands back the default it was given, and a subscript that finds nothing hands back a null
    /// value, so a chain of them needs no check at each step.
    ///
    /// \note A number keeps the form it was written in. \c 1 parses as \c Type::Int and \c 1.0 as
    ///       \c Type::Double, which is what a round trip through toJson() has to preserve.
    ///       Comparison ignores the distinction and compares numerically.
    ///
    ///       An integer is exact, up to the range of \c int64_t. Anything outside it, including an
    ///       unsigned value above \c INT64_MAX, becomes a \c Type::Double and is exact only up to
    ///       2^53.
    class STDC_EXPORT Value {
    public:
        Value(Type = Type::Null);
        Value(bool b);
        Value(double d);
        inline Value(int i) : Value(int64_t(i)) {
        }
        inline Value(uint32_t i) : Value(uint64_t(i)) {
        }
        Value(int64_t i);
        Value(uint64_t u);
        Value(std::string s);
        inline Value(const char *s, int size = -1)
            : Value(size < 0 ? std::string(s) : std::string(s, size)) {
        }
        Value(array_view<uint8_t> bytes);
        Value(std::vector<uint8_t> bytes);
        inline Value(const uint8_t *data, int size) : Value(array_view<uint8_t>(data, size)) {
        }
        Value(const Array &a);
        Value(Array &&a);
        Value(const Object &o);
        Value(Object &&o);
        ~Value();

        Value(const Value &RHS);
        Value(Value &&RHS) noexcept;
        Value &operator=(const Value &RHS);
        inline Value &operator=(Value &&RHS) noexcept {
            swap(RHS);
            return *this;
        }

        void swap(Value &RHS) noexcept;

    public:
        inline Type type() const {
            return _type;
        }
        inline bool isNull() const {
            return type() == Type::Null;
        }
        inline bool isBool() const {
            return type() == Type::Bool;
        }
        inline bool isDouble() const {
            return type() == Type::Double;
        }
        inline bool isInt() const {
            return type() == Type::Int;
        }
        inline bool isNumber() const {
            return isDouble() || isInt();
        }
        inline bool isString() const {
            return type() == Type::String;
        }
        inline bool isArray() const {
            return type() == Type::Array;
        }
        inline bool isObject() const {
            return type() == Type::Object;
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
        const Array &toArray() const;
        const Array &toArray(const Array &defaultValue) const;
        Array toArray(Array &&defaultValue) const;
        const Object &toObject() const;
        const Object &toObject(const Object &defaultValue) const;
        Object toObject(Object &&defaultValue) const;

        const Value &operator[](std::string_view key) const;
        const Value &operator[](size_t i) const;

        bool operator==(const Value &RHS) const;
        inline bool operator!=(const Value &RHS) const {
            return !(*this == RHS);
        }

    public:
        /// Returns the serialized JSON text of this value.
        ///
        /// \param indent The number of spaces to indent the JSON text. If negative, no indentation
        ///        is performed.
        std::string toJson(int indent = -1) const;

        /// Returns the value the given JSON text spells.
        ///
        /// \param json The text to parse.
        /// \param ignoreComments Whether comments should be ignored and treated like whitespace
        ///        (true) or yield a parse error (false)
        /// \param error Set to why the text was rejected, or cleared on success. A rejected
        ///        document and the text \c null both come back as a null value, so this tells
        ///        them apart.
        static Value fromJson(std::string_view json, bool ignoreComments,
                              ParseError *error = nullptr);

        std::vector<uint8_t> toCbor() const;

        /// Returns the value the given CBOR encodes.
        ///
        /// \param cbor The bytes to decode.
        /// \param error Set to why the bytes were rejected, or cleared on success. A rejected
        ///        document and an encoded null both come back as a null value, so this tells
        ///        them apart.
        static Value fromCbor(array_view<uint8_t> cbor, cbor::DecodeError *error = nullptr);

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
            Array *arr;
            Object *obj;
        };

        Type _type;
        Payload _p;

        // Frees what the live alternative owns, if it owns anything, and becomes null.
        void reset() noexcept;
        void copyFrom(const Value &RHS);
    };

    /// @}
}

#endif // STDCORELIB_JSON_H
