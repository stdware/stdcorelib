// SPDX-License-Identifier: MIT

#ifndef STDCORELIB_JSON_H
#define STDCORELIB_JSON_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdcorelib/stdc_global.h>
#include <stdcorelib/adt/array_view.h>

/// \defgroup json JSON and CBOR
///
/// stdc::json::Value reads and writes both encodings with the same document tree.

namespace stdc::cbor {

    /// \addtogroup json
    /// @{

    /// The reason and the position of the rejection of a CBOR document.
    ///
    /// The codec consists of json::Value::toCbor() and json::Value::fromCbor(), because one tree
    /// serves both encodings. This type is declared here rather than beside them, because it
    /// describes CBOR failures, and future CBOR facilities belong in the same namespace.
    struct STDC_EXPORT DecodeError {
        enum Code {
            NoError = 0,
            UnexpectedEnd,   ///< end of input in the middle of an item
            IllegalEncoding, ///< a reserved encoding, or an encoding not allowed for this item
            IllegalString,   ///< a text string that is not UTF-8
            OutOfRange,      ///< a number that CBOR can encode and json::Value cannot hold
            UnsupportedType, ///< a tag, or a map key that is not text
            NestedTooDeeply,
            TrailingContent, ///< further content after a complete item
        };

        Code code = NoError;
        size_t offset = 0; ///< offset in bytes from the start
        std::string what;  ///< description of the failure, without the position

        /// Returns whether this object reports a failed decode.
        explicit operator bool() const {
            return code != NoError;
        }

        /// Returns the description of the failure, preceded by the byte offset, or an empty
        /// string for NoError.
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

    /// The eight kinds of a Value.
    ///
    /// \note The enumeration is scoped, so that its \c Array and \c Object do not hide the two
    ///       type aliases above. An unscoped enumerator would hide them inside Value and in the
    ///       rest of this namespace.
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

    /// The reason and the position of the rejection of a JSON document.
    struct STDC_EXPORT ParseError {
        enum Code {
            NoError = 0,
            UnexpectedEnd,   ///< end of input in the middle of a value
            UnexpectedToken, ///< a token that is not allowed at its position
            IllegalNumber,   ///< a leading zero, a missing digit, or an empty exponent
            IllegalEscape,   ///< an unknown escape sequence, or an invalid surrogate pair
            IllegalString,   ///< an unescaped control character, or an invalid UTF-8 sequence
            NestedTooDeeply,
            TrailingContent,   ///< further content after a complete value
            CommentNotAllowed, ///< a comment, if \a ignoreComments was not set
        };

        Code code = NoError;
        size_t offset = 0; ///< offset in bytes from the start of the text
        size_t line = 1;   ///< line number, starting at one
        size_t column = 1; ///< column number, starting at one, in bytes rather than code points
        std::string what;  ///< description of the failure, without the position

        /// Returns whether this object reports a failed parse.
        explicit operator bool() const {
            return code != NoError;
        }

        /// Returns the description of the failure, preceded by the line and column, or an empty
        /// string for NoError.
        std::string message() const;
    };

    namespace detail {

        /// The empty objects that the readers below return if the value has another type.
        /// @{
        inline const Value &empty_value();
        inline const std::string &empty_string();
        inline const std::vector<uint8_t> &empty_binary();
        inline const Array &empty_array();
        inline const Object &empty_object();
        /// @}

    }

    /// A JSON value, modeled on the JSON classes of Qt.
    ///
    /// The tree is built by construction and read through the \c toXxx() family, which never
    /// fails. An accessor for a type that the value does not have returns the given default, and
    /// a subscript that finds nothing returns a null value. A chain of accessors therefore
    /// requires no check at each step.
    ///
    /// The \c asXxx() family complements it. It returns a pointer into the storage of the value,
    /// or \c nullptr if the value has another type, and its non-const forms are the means of
    /// changing a document after construction. These accessors require the check that the
    /// \c toXxx() family does not.
    ///
    /// \note A number keeps the form in which it was written. \c 1 parses as \c Type::Int and
    ///       \c 1.0 as \c Type::Double, which a round trip through toJson() must preserve.
    ///       Comparison ignores the distinction and compares numerically.
    ///
    ///       An integer is exact within the range of \c int64_t. A value outside that range,
    ///       including an unsigned value above \c INT64_MAX, becomes a \c Type::Double and is
    ///       exact only up to 2^53.
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

    public:
        /// \name Access with default values
        ///
        /// None of these accessors fail. If the value has another type, the default is returned.
        /// The two number types convert into each other, and no other conversion exists.
        ///
        /// \warning A form that returns a reference and accepts a default returns a reference to
        ///          that default if the type does not match. A temporary written at the call site
        ///          is therefore destroyed at the end of the full expression. The forms without a
        ///          default return a shared empty object instead and can be kept safely.
        ///
        /// The \c asXxx() family below provides access to the storage itself and distinguishes
        /// an absent value from a value that equals the default.
        /// @{

        inline bool toBool(bool defaultValue = false) const {
            return _type == Type::Bool ? _p.b : defaultValue;
        }
        inline double toDouble(double defaultValue = 0) const {
            switch (_type) {
                case Type::Int:
                    return double(_p.i);
                case Type::Double:
                    return _p.d;
                default:
                    break;
            }
            return defaultValue;
        }
        inline int64_t toInt(int64_t defaultValue = 0) const {
            switch (_type) {
                case Type::Int:
                    return _p.i;
                case Type::Double:
                    // The conversion truncates rather than rounds and is undefined if the value is
                    // out of range, as for any other cast.
                    return int64_t(_p.d);
                default:
                    break;
            }
            return defaultValue;
        }
        inline const std::string &toString() const {
            return _type == Type::String ? *_p.s : detail::empty_string();
        }
        inline const std::string &toString(const std::string &defaultValue) const {
            return _type == Type::String ? *_p.s : defaultValue;
        }
        inline std::string toString(std::string &&defaultValue) const {
            if (_type == Type::String) {
                return *_p.s;
            }
            return std::move(defaultValue);
        }
        inline const std::vector<uint8_t> &toBinary() const {
            return _type == Type::Binary ? *_p.bin : detail::empty_binary();
        }
        inline const std::vector<uint8_t> &
            toBinary(const std::vector<uint8_t> &defaultValue) const {
            return _type == Type::Binary ? *_p.bin : defaultValue;
        }
        inline std::vector<uint8_t> toBinary(std::vector<uint8_t> &&defaultValue) const {
            if (_type == Type::Binary) {
                return *_p.bin;
            }
            return std::move(defaultValue);
        }
        inline const Array &toArray() const {
            return _type == Type::Array ? *_p.arr : detail::empty_array();
        }
        inline const Array &toArray(const Array &defaultValue) const {
            return _type == Type::Array ? *_p.arr : defaultValue;
        }
        inline Array toArray(Array &&defaultValue) const {
            if (_type == Type::Array) {
                return *_p.arr;
            }
            return std::move(defaultValue);
        }
        inline const Object &toObject() const {
            return _type == Type::Object ? *_p.obj : detail::empty_object();
        }
        inline const Object &toObject(const Object &defaultValue) const {
            return _type == Type::Object ? *_p.obj : defaultValue;
        }
        inline Object toObject(Object &&defaultValue) const {
            if (_type == Type::Object) {
                return *_p.obj;
            }
            return std::move(defaultValue);
        }

        inline const Value &operator[](std::string_view key) const {
            if (_type == Type::Object) {
                auto it = _p.obj->find(key);
                if (it != _p.obj->end()) {
                    return it->second;
                }
            }
            return detail::empty_value();
        }
        inline const Value &operator[](size_t i) const {
            if (_type == Type::Array && i < _p.arr->size()) {
                return (*_p.arr)[i];
            }
            return detail::empty_value();
        }

        /// @}

        /// \name Access to the storage
        ///
        /// These accessors return the payload of the value, or \c nullptr if the value has
        /// another type. Nothing is converted and nothing is substituted, which distinguishes
        /// them from the \c toXxx() family above. asDouble() on an \c Int returns null, whereas
        /// toDouble() returns the number.
        ///
        /// These accessors also distinguish an absent value from a value that equals the default,
        /// which \c toInt(-1) cannot.
        ///
        /// The non-const forms return a writable pointer and are the only means of changing a
        /// document after construction. A reference form deliberately does not exist. A type
        /// that does not match has no object to refer to, and returning the shared empty object
        /// would let the write of one caller reach every other reader of that object.
        ///
        /// \code
        ///   if (auto *o = doc.asObject()) {
        ///       o->emplace("count", json::Value(1));
        ///   }
        /// \endcode
        /// @{

        inline const bool *asBool() const {
            return _type == Type::Bool ? &_p.b : nullptr;
        }
        inline bool *asBool() {
            return _type == Type::Bool ? &_p.b : nullptr;
        }
        inline const int64_t *asInt() const {
            return _type == Type::Int ? &_p.i : nullptr;
        }
        inline int64_t *asInt() {
            return _type == Type::Int ? &_p.i : nullptr;
        }
        inline const double *asDouble() const {
            return _type == Type::Double ? &_p.d : nullptr;
        }
        inline double *asDouble() {
            return _type == Type::Double ? &_p.d : nullptr;
        }
        inline const std::string *asString() const {
            return _type == Type::String ? _p.s : nullptr;
        }
        inline std::string *asString() {
            return _type == Type::String ? _p.s : nullptr;
        }
        inline const std::vector<uint8_t> *asBinary() const {
            return _type == Type::Binary ? _p.bin : nullptr;
        }
        inline std::vector<uint8_t> *asBinary() {
            return _type == Type::Binary ? _p.bin : nullptr;
        }
        inline const Array *asArray() const {
            return _type == Type::Array ? _p.arr : nullptr;
        }
        inline Array *asArray() {
            return _type == Type::Array ? _p.arr : nullptr;
        }
        inline const Object *asObject() const {
            return _type == Type::Object ? _p.obj : nullptr;
        }
        inline Object *asObject() {
            return _type == Type::Object ? _p.obj : nullptr;
        }

        /// @}

    public:
        bool operator==(const Value &RHS) const;
        inline bool operator!=(const Value &RHS) const {
            return !(*this == RHS);
        }

    public:
        /// Returns the serialized JSON text of this value.
        ///
        /// \param indent the number of spaces per indentation level, or a negative number for no
        ///        indentation
        std::string toJson(int indent = -1) const;

        /// Returns the value that the JSON text \a json denotes.
        ///
        /// \param json the text to parse
        /// \param ignoreComments whether comments are treated as whitespace (true) or reported
        ///        as a parse error (false)
        /// \param error receives the reason for a rejection, and is cleared on success. A rejected
        ///        document and the text \c null both produce a null value, and \a error
        ///        distinguishes them.
        static Value fromJson(std::string_view json, bool ignoreComments,
                              ParseError *error = nullptr);

        std::vector<uint8_t> toCbor() const;

        /// Returns the value that the CBOR data \a cbor encodes.
        ///
        /// \param cbor the bytes to decode
        /// \param error receives the reason for a rejection, and is cleared on success. A rejected
        ///        document and an encoded null both produce a null value, and \a error
        ///        distinguishes them.
        static Value fromCbor(array_view<uint8_t> cbor, cbor::DecodeError *error = nullptr);

    private:
        // The alternatives are all trivially copyable. The payload is therefore copied as one
        // object rather than one member at a time. _type alone determines the active member.
        //
        // Every alternative larger than a scalar is stored behind a pointer that the value owns
        // and copies together with the value. A std::string alone is larger than all other
        // members together.
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

        // Frees the memory that the active alternative owns, if any, and makes the value null.
        void reset() noexcept;
        void copyFrom(const Value &RHS);
    };

    namespace detail {

        inline const Value &empty_value() {
            static const Value instance;
            return instance;
        }

        inline const std::string &empty_string() {
            static const std::string instance;
            return instance;
        }

        inline const std::vector<uint8_t> &empty_binary() {
            static const std::vector<uint8_t> instance;
            return instance;
        }

        inline const Array &empty_array() {
            static const Array instance;
            return instance;
        }

        inline const Object &empty_object() {
            static const Object instance;
            return instance;
        }

    }

    /// @}
}

#endif // STDCORELIB_JSON_H
