/// @file object_type_registry.hpp
/// @brief Finds object types by name or id, and creates objects through factories (e.g. from configuration).

#ifndef COMMON_OBJECT_OBJECT_TYPE_REGISTRY_HPP
#define COMMON_OBJECT_OBJECT_TYPE_REGISTRY_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

#include "common/containers/object_pool.hpp"
#include "common/core/assert.hpp"
#include "common/core/status.hpp"
#include "common/object/object_type.hpp"

namespace common {

/// Creates an object of @p type, the registered type that ObjectTypeRegistry::create() found, from
/// @p arguments (whose type is agreed between the caller of create() and the factory; nullptr if
/// none) and the factory's @p context. @return the object, or nullptr when none is available.
using ObjectCreate = Object* (*)(const ObjectType& type, const void* arguments, void* context) noexcept;

/// Destroys an object that the matching ObjectCreate made.
using ObjectDestroy = void (*)(Object& object, void* context) noexcept;

/// How a registered type creates and destroys its objects: both functions, or neither.
struct ObjectFactory final {
    ObjectCreate create{nullptr};
    ObjectDestroy destroy{nullptr};
    void* context{nullptr};
};

namespace detail {
namespace object_type_registry {

template <typename T, std::size_t Capacity>
struct PoolFactory final {
    static Object* create(const ObjectType& type, const void* const arguments, void* const context) noexcept {
        ObjectPool<T, Capacity>* const pool{static_cast<ObjectPool<T, Capacity>*>(context)};
        T* object{nullptr};
        if constexpr (std::is_nothrow_constructible<T, const ObjectType&, const void*>::value) {
            object = pool->create(type, arguments);
        } else {
            static_cast<void>(type);
            static_cast<void>(arguments);
            object = pool->create();
        }
        return object;
    }

    static void destroy(Object& object, void* const context) noexcept {
        ObjectPool<T, Capacity>* const pool{static_cast<ObjectPool<T, Capacity>*>(context)};
        T* const typed{object_cast<T>(&object)};
        COMMON_ASSERT(typed != nullptr);
        pool->destroy(typed);
    }
};

}  // namespace object_type_registry
}  // namespace detail

/// A factory that creates objects of class T in @p pool and destroys them back into it. T is
/// constructed as T(type, arguments) when it is nothrow-constructible from (const ObjectType&,
/// const void*), so that one class can serve several registered types (e.g. TypeObjects) and take
/// per-instance arguments, and as T() otherwise. create() returns nullptr when the pool is exhausted.
/// T must declare its own kType (object_cast<T> checks it). The pool must outlive the registry's use.
template <typename T, std::size_t Capacity>
[[nodiscard]] ObjectFactory pool_factory(ObjectPool<T, Capacity>& pool) noexcept {
    static_assert(std::is_base_of<Object, T>::value, "T must derive from common::Object");
    using Factory = detail::object_type_registry::PoolFactory<T, Capacity>;
    return ObjectFactory{&Factory::create, &Factory::destroy, &pool};
}

/// Up to @p MaxTypes object types, each with an optional factory, found by name or by id: e.g. the
/// unit types an application can create from a configuration file, or instrument types read from
/// reference data. @p Type is the descriptor class held: ObjectType (any type), or a TypeObject<P>,
/// so that find() returns the properties too.
///
/// - add() rejects a second type with the same name or the same non-zero id, so names are unique
///   among the registered types.
/// - create(name, arguments) calls the factory of the type named; create_as<T>() also checks the
///   object's class. destroy() returns an object through the factory of its own type.
///
/// Fill it during initialisation; afterwards it is read-only, and threads started after the
/// registration may read it concurrently. RT-path safe: find(), find_by_id(), contains(), size(),
/// operator[] (O(size() * name length)). create() and destroy() are as safe, and as thread-safe, as
/// the factory: with pool_factory, safe on the thread that owns the pool.
///
/// @code
///   common::ObjectPool<CanGateway, 2U> gateways{};
///   common::ObjectTypeRegistry<16U> types{};
///   static_cast<void>(types.add(CanGateway::kType, common::pool_factory(gateways)));
///   // From a configuration line "gateway can0 CanGateway":
///   const GatewayArguments arguments{"can0", 500'000U};
///   const common::Result<CanGateway*> gateway{types.create_as<CanGateway>("CanGateway", &arguments)};
/// @endcode
template <std::size_t MaxTypes, typename Type = ObjectType>
class ObjectTypeRegistry final {
    static_assert(MaxTypes > 0U, "MaxTypes must be positive");
    static_assert(std::is_base_of<ObjectType, Type>::value, "Type must be ObjectType or derive from it");

public:
    ObjectTypeRegistry() noexcept = default;
    ~ObjectTypeRegistry() = default;
    ObjectTypeRegistry(const ObjectTypeRegistry&) = delete;
    ObjectTypeRegistry(ObjectTypeRegistry&&) = delete;
    ObjectTypeRegistry& operator=(const ObjectTypeRegistry&) = delete;
    ObjectTypeRegistry& operator=(ObjectTypeRegistry&&) = delete;

    /// Registers @p type, which must outlive the registry, with an optional @p factory.
    /// @return kOk; kInvalidArgument if the factory has only one of create and destroy;
    ///         kAlreadyExists if the type, a type with the same name, or a type with the same non-zero
    ///         id is registered; kResourceLimit when MaxTypes types are registered.
    Status add(const Type& type, const ObjectFactory& factory = ObjectFactory{}) noexcept {
        Status status{Status::kOk};
        if ((factory.create == nullptr) != (factory.destroy == nullptr)) {
            status = Status::kInvalidArgument;
        } else if (is_registered(type)) {
            status = Status::kAlreadyExists;
        } else if (size_ >= MaxTypes) {
            status = Status::kResourceLimit;
        } else {
            entries_[size_] = Entry{&type, factory};
            ++size_;
        }
        return status;
    }

    /// @return the type named @p name, or nullptr.
    [[nodiscard]] const Type* find(const std::string_view name) const noexcept {
        const Entry* const entry{find_entry(name)};
        return (entry != nullptr) ? entry->type : nullptr;
    }

    /// @return the type with the non-zero id @p id, or nullptr (also for id 0).
    [[nodiscard]] const Type* find_by_id(const std::uint32_t id) const noexcept {
        const Type* found{nullptr};
        if (id != 0U) {
            for (std::size_t index{0U}; (index < size_) && (found == nullptr); ++index) {
                if (entries_[index].type->id() == id) {
                    found = entries_[index].type;
                }
            }
        }
        return found;
    }

    /// @return true if @p type (or the same type, see ObjectType::same_as) is registered.
    [[nodiscard]] bool contains(const ObjectType& type) const noexcept {
        return find_same(type) != nullptr;
    }

    /// Creates an object of the type named @p name through its factory.
    /// @return the object; kNotFound if no such type is registered; kNotSupported if it has no factory;
    ///         kResourceLimit if the factory returned nullptr.
    [[nodiscard]] Result<Object*> create(const std::string_view name, const void* const arguments = nullptr) const noexcept {
        Result<Object*> result{Result<Object*>::failure(Status::kNotFound)};
        const Entry* const entry{find_entry(name)};
        if (entry == nullptr) {
            // kNotFound.
        } else if (entry->factory.create == nullptr) {
            result = Result<Object*>::failure(Status::kNotSupported);
        } else {
            Object* const object{entry->factory.create(*entry->type, arguments, entry->factory.context)};
            result = (object != nullptr) ? Result<Object*>::success(object)
                                         : Result<Object*>::failure(Status::kResourceLimit);
        }
        return result;
    }

    /// create(), then a check that the object is a T (object_cast). On a mismatch the object is
    /// destroyed again.
    /// @return the object; the errors of create(); kInvalidArgument if the object is not a T.
    template <typename T>
    [[nodiscard]] Result<T*> create_as(const std::string_view name, const void* const arguments = nullptr) const noexcept {
        Result<T*> result{Result<T*>::failure(Status::kNotFound)};
        const Result<Object*> created{create(name, arguments)};
        if (!created.has_value()) {
            result = Result<T*>::failure(created.status());
        } else {
            T* const typed{object_cast<T>(created.value())};
            if (typed != nullptr) {
                result = Result<T*>::success(typed);
            } else {
                static_cast<void>(destroy(*created.value()));
                result = Result<T*>::failure(Status::kInvalidArgument);
            }
        }
        return result;
    }

    /// Destroys @p object through the factory of its own type (object.object_type(), exactly).
    /// @return kOk; kNotFound if its type is not registered; kNotSupported if the type has no factory.
    Status destroy(Object& object) const noexcept {
        Status status{Status::kNotFound};
        const Entry* const entry{find_same(object.object_type())};
        if (entry == nullptr) {
            // kNotFound.
        } else if (entry->factory.destroy == nullptr) {
            status = Status::kNotSupported;
        } else {
            entry->factory.destroy(object, entry->factory.context);
            status = Status::kOk;
        }
        return status;
    }

    /// Number of registered types.
    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    /// The types in registration order.
    /// @pre index < size()
    [[nodiscard]] const Type& operator[](const std::size_t index) const noexcept {
        COMMON_ASSERT(index < size_);
        return *entries_[index].type;
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return MaxTypes;
    }

private:
    struct Entry final {
        const Type* type{nullptr};
        ObjectFactory factory{};
    };

    [[nodiscard]] bool is_registered(const Type& type) const noexcept {
        bool registered{false};
        for (std::size_t index{0U}; (index < size_) && (!registered); ++index) {
            const Type& existing{*entries_[index].type};
            registered = existing.same_as(type) ||
                         detail::object_type::names_equal(existing.name(), type.name()) ||
                         ((type.id() != 0U) && (existing.id() == type.id()));
        }
        return registered;
    }

    [[nodiscard]] const Entry* find_entry(const std::string_view name) const noexcept {
        const Entry* found{nullptr};
        for (std::size_t index{0U}; (index < size_) && (found == nullptr); ++index) {
            if (detail::object_type::name_equals(entries_[index].type->name(), name)) {
                found = &entries_[index];
            }
        }
        return found;
    }

    [[nodiscard]] const Entry* find_same(const ObjectType& type) const noexcept {
        const Entry* found{nullptr};
        for (std::size_t index{0U}; (index < size_) && (found == nullptr); ++index) {
            if (entries_[index].type->same_as(type)) {
                found = &entries_[index];
            }
        }
        return found;
    }

    std::array<Entry, MaxTypes> entries_{};
    std::size_t size_{0U};
};

}  // namespace common

#endif  // COMMON_OBJECT_OBJECT_TYPE_REGISTRY_HPP
