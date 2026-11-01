/// @file object_type.hpp
/// @brief Run-time types without RTTI: type descriptors (Type Object pattern), checked downcasts.

#ifndef COMMON_OBJECT_OBJECT_TYPE_HPP
#define COMMON_OBJECT_OBJECT_TYPE_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "common/core/assert.hpp"
#include "common/core/status.hpp"

namespace common {

/// Longest name an ObjectType may have, in characters.
inline constexpr std::size_t kMaxObjectTypeNameLength{63U};

/// Most ancestors an ObjectType may have.
inline constexpr std::size_t kMaxObjectTypeDepth{32U};

/// Id of Object::kType, the root of every object hierarchy. Do not give it to another type.
inline constexpr std::uint32_t kObjectRootTypeId{0xFFFF'FFFFU};

class ObjectType;

namespace detail {
namespace object_type {

/// Length of @p name, or kMaxObjectTypeNameLength + 1 if it is longer; 0 for nullptr.
[[nodiscard]] constexpr std::size_t bounded_length(const char* const name) noexcept {
    std::size_t length{0U};
    if (name != nullptr) {
        while ((length <= kMaxObjectTypeNameLength) && (name[length] != '\0')) {
            ++length;
        }
    }
    return length;
}

/// Compares two valid names (each at most kMaxObjectTypeNameLength characters) character by
/// character with ==. No character is hashed or used as a number (MISRA C++:2023 Rule 7.0.3).
[[nodiscard]] constexpr bool names_equal(const char* const lhs, const char* const rhs) noexcept {
    bool equal{true};
    bool ended{false};
    for (std::size_t i{0U}; (i <= kMaxObjectTypeNameLength) && equal && (!ended); ++i) {
        equal = (lhs[i] == rhs[i]);
        ended = (lhs[i] == '\0');
    }
    return equal;
}

/// Compares a valid name with @p text, which need not be NUL-terminated and may contain NUL
/// characters: the comparison never reads past the end of @p name.
[[nodiscard]] constexpr bool name_equals(const char* const name, const std::string_view text) noexcept {
    bool equal{text.size() <= kMaxObjectTypeNameLength};
    for (std::size_t i{0U}; equal && (i < text.size()); ++i) {
        equal = (name[i] != '\0') && (name[i] == text[i]);
    }
    return equal && (name[text.size()] == '\0');
}

/// true if @p text has no NUL character (a name is stored NUL-terminated).
[[nodiscard]] constexpr bool has_no_nul(const std::string_view text) noexcept {
    bool clean{true};
    for (std::size_t i{0U}; clean && (i < text.size()); ++i) {
        clean = (text[i] != '\0');
    }
    return clean;
}

/// The name storage of a TypeObject, a private base constructed before its ObjectType base
/// (base-from-member), so that the ObjectType can point to it.
class NameStorage {
protected:
    /// Copies @p name. A name that check_object_type() rejects is a contract violation, checked
    /// whether or not assertions are enabled.
    explicit constexpr NameStorage(const std::string_view name) noexcept : characters_{} {
        if (name.empty() || (name.size() > kMaxObjectTypeNameLength) || (!has_no_nul(name))) {
            ::common::detail::assert_fail("valid TypeObject name", __FILE__, __LINE__);
        }
        for (std::size_t i{0U}; (i < name.size()) && (i < kMaxObjectTypeNameLength); ++i) {
            characters_[i] = name[i];
        }
    }

    [[nodiscard]] constexpr const char* stored_name() const noexcept {
        return characters_.data();
    }

private:
    std::array<char, kMaxObjectTypeNameLength + 1U> characters_;
};

}  // namespace object_type
}  // namespace detail

/// Run-time identity of a kind of object: a name, an optional numeric id, and the type it derives
/// from (its parent). Types form trees: is_a() asks whether a type is, or derives from, another.
///
/// Identity: two descriptors are the same type when they are the same object, or, when both have a
/// non-zero id, when their ids and names are equal. A name alone never identifies a type, so two
/// unrelated classes that happen to share a name are never confused. A type defined in a header can
/// exist once per module (a Windows DLL, or a shared object built with -fvisibility=hidden): give
/// such a type a non-zero id unique in the program, or casts across modules fail (safely: nullptr).
///
/// ObjectType is used directly for types defined in code without a class (e.g. message kinds), and
/// through ObjectTypeOf (the type of a C++ class) and TypeObject (a type defined by data, with
/// properties). It cannot be copied: a type is an identity, referred to by pointer.
///
/// Every member is constexpr and noexcept, never allocates and takes at most O(depth) steps: safe on
/// the real-time path. Types are immutable: they can be read from any thread.
class ObjectType {
public:
    /// @pre @p name has 1 to kMaxObjectTypeNameLength characters and outlives the type (normally a
    ///      string literal); @p parent is nullptr or has fewer than kMaxObjectTypeDepth ancestors.
    ///      The check does not depend on COMMON_ASSERT: in a constant expression a violation does not
    ///      compile, at run time it aborts (otherwise names could be compared incompletely).
    constexpr ObjectType(const char* const name, const ObjectType* const parent, const std::uint32_t id = 0U) noexcept
        : name_{name}, parent_{parent}, id_{id}, depth_{(parent != nullptr) ? (parent->depth_ + 1U) : 0U} {
        const std::size_t length{detail::object_type::bounded_length(name)};
        const bool valid{(length > 0U) && (length <= kMaxObjectTypeNameLength) &&
                         ((parent == nullptr) || (parent->depth_ < kMaxObjectTypeDepth))};
        if (!valid) {
            ::common::detail::assert_fail("valid ObjectType name and depth", __FILE__, __LINE__);
        }
    }

    ~ObjectType() = default;
    ObjectType(const ObjectType&) = delete;
    ObjectType(ObjectType&&) = delete;
    ObjectType& operator=(const ObjectType&) = delete;
    ObjectType& operator=(ObjectType&&) = delete;

    [[nodiscard]] constexpr const char* name() const noexcept {
        return name_;
    }

    /// The type this one derives from; nullptr for a root.
    [[nodiscard]] constexpr const ObjectType* parent() const noexcept {
        return parent_;
    }

    /// The numeric id given to the constructor; 0 means none.
    [[nodiscard]] constexpr std::uint32_t id() const noexcept {
        return id_;
    }

    /// Number of ancestors; 0 for a root.
    [[nodiscard]] constexpr std::size_t depth() const noexcept {
        return depth_;
    }

    /// @return true if @p other is the same type: the same object, or (both ids non-zero) the same id
    ///         and name.
    [[nodiscard]] constexpr bool same_as(const ObjectType& other) const noexcept {
        return (this == &other) ||
               ((id_ != 0U) && (id_ == other.id_) && detail::object_type::names_equal(name_, other.name_));
    }

    /// @return true if this type is @p ancestor or derives from it, directly or indirectly. Walks up
    ///         depth() - ancestor.depth() parents, then compares once: O(depth()).
    [[nodiscard]] constexpr bool is_a(const ObjectType& ancestor) const noexcept {
        bool result{false};
        if (ancestor.depth_ <= depth_) {
            const ObjectType* type{this};
            const std::size_t steps{depth_ - ancestor.depth_};
            for (std::size_t step{0U}; (step < steps) && (type != nullptr); ++step) {
                type = type->parent_;
            }
            result = (type != nullptr) && type->same_as(ancestor);
        }
        return result;
    }

private:
    const char* name_;
    const ObjectType* parent_;
    std::uint32_t id_;
    std::size_t depth_;
};

/// Checks data before it becomes a type, e.g. in a loader that reads types from reference data.
/// Constructing an ObjectType or TypeObject from rejected data is a contract violation (it aborts).
/// @return kOk; kInvalidArgument for an empty name or one containing a NUL character; kOutOfRange for
///         a name longer than kMaxObjectTypeNameLength, or a @p parent that already has
///         kMaxObjectTypeDepth ancestors.
[[nodiscard]] constexpr Status check_object_type(const std::string_view name, const ObjectType* const parent) noexcept {
    Status status{Status::kOk};
    if (name.empty() || (!detail::object_type::has_no_nul(name))) {
        status = Status::kInvalidArgument;
    } else if (name.size() > kMaxObjectTypeNameLength) {
        status = Status::kOutOfRange;
    } else if ((parent != nullptr) && (parent->depth() >= kMaxObjectTypeDepth)) {
        status = Status::kOutOfRange;
    } else {
        // Valid.
    }
    return status;
}

/// The type of C++ class T, whose base in the object hierarchy is Base (Object for a root class).
/// Each class of a hierarchy declares its own:
/// @code
///   static constexpr common::ObjectTypeOf<Truck, Vehicle> kType{"Truck"};
/// @endcode
/// The parent is Base::kType, so the type tree follows the class tree, and object_cast<T> checks at
/// compile time that T::kType names T itself and that Base is a base class of T.
template <typename T, typename Base>
class ObjectTypeOf final : public ObjectType {
public:
    using Class = T;
    using BaseClass = Base;

    /// @pre as for ObjectType
    explicit constexpr ObjectTypeOf(const char* const name, const std::uint32_t id = 0U) noexcept
        : ObjectType{name, &Base::kType, id} {}
};

/// A type defined by data (the Type Object pattern): a name it owns, Properties shared by all of its
/// instances, and a parent type whose properties it refines. E.g. instrument types (with tick size,
/// lot size and currency) read from reference data at start-up, while all instruments are objects of
/// one class that point to their type.
///
/// The name is copied (at most kMaxObjectTypeNameLength characters), so names read into a reused
/// buffer, e.g. by a LineReader, are safe; check them first with check_object_type(). A TypeObject
/// lives where it outlives its instances and the registries that refer to it, e.g. in an
/// ObjectPool<TypeObject<P>, N> filled during initialisation, or as a static constexpr object.
template <typename Properties>
class TypeObject final : private detail::object_type::NameStorage, public ObjectType {
    static_assert(std::is_nothrow_copy_constructible<Properties>::value, "Properties must be nothrow copy-constructible");

public:
    /// A root type. @pre check_object_type(name, nullptr) == kOk
    constexpr TypeObject(const std::string_view name, const Properties& properties, const std::uint32_t id = 0U) noexcept
        : NameStorage{name}, ObjectType{stored_name(), nullptr, id}, properties_{properties}, parent_type_{nullptr} {}

    /// A type that refines @p parent. @pre check_object_type(name, &parent) == kOk
    constexpr TypeObject(const std::string_view name, const Properties& properties, const TypeObject& parent,
                         const std::uint32_t id = 0U) noexcept
        : NameStorage{name},
          ObjectType{stored_name(), &parent, id},
          properties_{properties},
          parent_type_{&parent} {}

    /// A first-level type under a class type, e.g. Instrument::kType, so that an Instrument whose
    /// object_type() is this TypeObject still passes object_cast<Instrument>.
    /// @pre check_object_type(name, &root) == kOk
    constexpr TypeObject(const std::string_view name, const Properties& properties, const ObjectType& root,
                         const std::uint32_t id = 0U) noexcept
        : NameStorage{name}, ObjectType{stored_name(), &root, id}, properties_{properties}, parent_type_{nullptr} {}

    ~TypeObject() = default;
    TypeObject(const TypeObject&) = delete;
    TypeObject(TypeObject&&) = delete;
    TypeObject& operator=(const TypeObject&) = delete;
    TypeObject& operator=(TypeObject&&) = delete;

    [[nodiscard]] constexpr const Properties& properties() const noexcept {
        return properties_;
    }

    /// The parent TypeObject; nullptr for a root or a type directly under a class type.
    [[nodiscard]] constexpr const TypeObject* parent_type() const noexcept {
        return parent_type_;
    }

private:
    Properties properties_;
    const TypeObject* parent_type_;
};

/// Base of objects that know their type at run time, without RTTI (the library builds with
/// -fno-rtti, so dynamic_cast is not available). Each class of a hierarchy declares its type and
/// returns it:
/// @code
///   class Vehicle : public common::Object {          // intermediate: protected destructor, copy, move
///   public:
///       static constexpr common::ObjectTypeOf<Vehicle, common::Object> kType{"Vehicle"};
///       [[nodiscard]] const common::ObjectType& object_type() const noexcept override { return kType; }
///   protected:
///       Vehicle() noexcept = default;
///       ~Vehicle() = default;
///       Vehicle(const Vehicle&) noexcept = default;
///       Vehicle(Vehicle&&) noexcept = default;
///       Vehicle& operator=(const Vehicle&) noexcept = default;
///       Vehicle& operator=(Vehicle&&) noexcept = default;
///   };
///   class Truck final : public Vehicle {
///   public:
///       static constexpr common::ObjectTypeOf<Truck, Vehicle> kType{"Truck"};
///       [[nodiscard]] const common::ObjectType& object_type() const noexcept override { return kType; }
///   };
///
///   common::Object& object{...};
///   if (object.is_a(Vehicle::kType)) { ... }
///   Truck* const truck{common::object_cast<Truck>(&object)};  // nullptr if it is not a Truck
/// @endcode
///
/// - A leaf class is final and is the only kind that is instantiated. An intermediate class (Vehicle)
///   declares a protected non-virtual destructor, protected copy and move operations, and no friends;
///   otherwise -Wnon-virtual-dtor fails the build.
/// - An object of a type defined by data returns its TypeObject from object_type(); its first-level
///   types are created under the class's kType, so object_cast to the class still works.
/// - MISRA C++:2023: each class's kType hides its base's kType, which the idiom relies on (object_cast
///   checks that T::kType belongs to T). If a project's checker applies Rule 6.4.1 to class scopes,
///   the project records a deviation for the idiom. This header itself hides nothing.
///
/// is_a() is RT-path safe (O(depth)). Thread-safety is that of the derived class.
class Object {
public:
    /// The root of every object hierarchy.
    static constexpr ObjectType kType{"common::Object", nullptr, kObjectRootTypeId};

    /// The type of this object: the kType of its class, or its TypeObject.
    [[nodiscard]] virtual const ObjectType& object_type() const noexcept = 0;

    /// @return true if this object is of type @p type or of a type derived from it.
    [[nodiscard]] bool is_a(const ObjectType& type) const noexcept {
        return object_type().is_a(type);
    }

protected:
    Object() noexcept = default;
    ~Object() = default;
    Object(const Object&) noexcept = default;
    Object(Object&&) noexcept = default;
    Object& operator=(const Object&) noexcept = default;
    Object& operator=(Object&&) noexcept = default;
};

namespace detail {
namespace object_type {

template <typename Descriptor, typename = void>
struct DeclaredClass final {
    using type = void;
};

template <typename Descriptor>
struct DeclaredClass<Descriptor, std::void_t<typename Descriptor::Class>> final {
    using type = typename Descriptor::Class;
};

template <typename Descriptor, typename = void>
struct DeclaredBase final {
    using type = void;
};

template <typename Descriptor>
struct DeclaredBase<Descriptor, std::void_t<typename Descriptor::BaseClass>> final {
    using type = typename Descriptor::BaseClass;
};

/// Compile-time checks that make object_cast<T> safe.
template <typename T>
constexpr void check_cast_target() noexcept {
    static_assert(std::is_base_of<Object, T>::value, "T must derive from common::Object");
    using Descriptor = std::remove_cv_t<decltype(T::kType)>;
    static_assert(std::is_same<typename DeclaredClass<Descriptor>::type, T>::value,
                  "T must declare its own type: static constexpr common::ObjectTypeOf<T, Base> kType{...}");
    static_assert(std::is_base_of<typename DeclaredBase<Descriptor>::type, T>::value,
                  "the Base of T::kType must be a base class of T");
}

}  // namespace object_type
}  // namespace detail

/// Checked downcast without RTTI.
/// @return @p object as a T, or nullptr when it is nullptr or not a T (its type is not T::kType or a
///         type derived from it). T must declare its own kType (checked at compile time).
template <typename T>
[[nodiscard]] T* object_cast(Object* const object) noexcept {
    detail::object_type::check_cast_target<T>();
    return ((object != nullptr) && object->is_a(T::kType)) ? static_cast<T*>(object) : nullptr;
}

/// Checked downcast without RTTI, for const objects; see above.
template <typename T>
[[nodiscard]] const T* object_cast(const Object* const object) noexcept {
    detail::object_type::check_cast_target<T>();
    return ((object != nullptr) && object->is_a(T::kType)) ? static_cast<const T*>(object) : nullptr;
}

}  // namespace common

#endif  // COMMON_OBJECT_OBJECT_TYPE_HPP
