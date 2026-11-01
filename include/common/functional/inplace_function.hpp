/// @file inplace_function.hpp
/// @brief Type-erased callable with inline storage: std::function without the heap.

#ifndef COMMON_FUNCTIONAL_INPLACE_FUNCTION_HPP
#define COMMON_FUNCTIONAL_INPLACE_FUNCTION_HPP

#include <array>
#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <utility>

#include "common/core/assert.hpp"
#include "common/core/config.hpp"

namespace common {

/// Default inline storage of an InplaceFunction: room for four pointers, e.g. a lambda that captures
/// `this` and three more values.
inline constexpr std::size_t kInplaceFunctionCapacity{4U * sizeof(void*)};

/// Default alignment of the inline storage. It is 16 on every platform: alignof(std::max_align_t) is
/// 16 with GCC, Clang and the NDK but 8 with MSVC, and a fixed value keeps fits<>() and the size of an
/// InplaceFunction the same everywhere.
inline constexpr std::size_t kInplaceFunctionAlignment{16U};

template <typename Signature, std::size_t Capacity = kInplaceFunctionCapacity,
          std::size_t Alignment = kInplaceFunctionAlignment>
class InplaceFunction;  // Declared only: Signature must be a function type R(Args...).

namespace detail {
namespace inplace_function {

template <typename T>
struct IsInplaceFunction : std::false_type {};

template <typename Signature, std::size_t Capacity, std::size_t Alignment>
struct IsInplaceFunction<InplaceFunction<Signature, Capacity, Alignment>> : std::true_type {};

/// What an InplaceFunction does with its stored callable; one constant table per stored type.
template <typename R, typename... Args>
struct Operations final {
    R (*invoke)(const std::byte* storage, Args&&... args) noexcept;
    void (*copy)(std::byte* target, const std::byte* source) noexcept;  ///< Copy-constructs into target.
    void (*move)(std::byte* target, std::byte* source) noexcept;  ///< Move-constructs, then destroys the source.
    void (*destroy)(std::byte* storage) noexcept;
};

/// The operations for callable type F. The table is a static member of this class template, not a
/// static data member template of InplaceFunction: MSVC 19.32 reports C4268 for the latter, which
/// /WX makes fatal.
template <typename F, typename R, typename... Args>
struct Thunks final {
    // Deviation (MISRA C++:2023 Rule 8.2.5): access to the callable that placement new created in the
    // storage (see StaticVector::element).
    static F* object(std::byte* const storage) noexcept {
        return std::launder(reinterpret_cast<F*>(storage));
    }

    // Deviation (MISRA C++:2023 Rule 8.2.5): as above.
    static const F* object(const std::byte* const storage) noexcept {
        return std::launder(reinterpret_cast<const F*>(storage));
    }

    static R invoke(const std::byte* const storage, Args&&... args) noexcept {
        return std::invoke(*object(storage), std::forward<Args>(args)...);
    }

    static void copy(std::byte* const target, const std::byte* const source) noexcept {
        // Deviation (MISRA C++:2023 Rule 21.6.3): placement new into owned, suitably aligned storage.
        static_cast<void>(::new (static_cast<void*>(target)) F(*object(source)));
    }

    static void move(std::byte* const target, std::byte* const source) noexcept {
        F* const from{object(source)};
        // Deviation (MISRA C++:2023 Rule 21.6.3): placement new into owned, suitably aligned storage.
        static_cast<void>(::new (static_cast<void*>(target)) F(std::move(*from)));
        from->~F();
    }

    static void destroy(std::byte* const storage) noexcept {
        object(storage)->~F();
    }

    static constexpr Operations<R, Args...> kOperations{&invoke, &copy, &move, &destroy};
};

/// true when a const F can be called with Args without throwing and returns exactly R (after removing
/// cv and references, unless R itself is a reference): a result that only converts to R, or a
/// [[nodiscard]] Status dropped by an R of void, is rejected.
template <typename F, typename R, typename... Args>
constexpr bool returns_exactly() noexcept {
    bool exact{false};
    if constexpr (std::is_nothrow_invocable<const F&, Args...>::value) {
        using Result = std::invoke_result_t<const F&, Args...>;
        if constexpr (std::is_reference<R>::value) {
            exact = std::is_same<Result, R>::value;
        } else {
            exact = std::is_same<std::remove_cv_t<std::remove_reference_t<Result>>, R>::value;
        }
    }
    return exact;
}

}  // namespace inplace_function
}  // namespace detail

/// A callable of signature R(Args...) stored in @p Capacity bytes inside the object: what
/// std::function does, without allocating, and without exceptions. Signal slots and ActiveObject
/// commands are InplaceFunctions.
///
/// A stored callable F (decayed) must
/// - fit: sizeof(F) <= Capacity and alignof(F) <= Alignment;
/// - be nothrow copy- and move-constructible and nothrow destructible, so that an InplaceFunction is
///   nothrow copyable and movable (and fits SpscQueue and MpmcQueue);
/// - be noexcept and callable as const with Args: a mutable lambda is rejected (capture a pointer to
///   the state it changes instead);
/// - return exactly R: a lambda that returns a [[nodiscard]] Status cannot be stored in an
///   InplaceFunction<void()> and lose the error silently, and a result that only converts to R is
///   rejected at the call site rather than narrowed inside this header.
/// Each requirement is a static_assert with a message; fits<F>() reports all of them for generic code.
/// Lambdas are therefore written [...](...) noexcept { ... }.
///
/// A null function pointer, a null pointer to member, or an empty InplaceFunction of the same
/// signature (of any capacity) gives an empty InplaceFunction, as with std::function. Pointers to
/// member functions and data members are called through std::invoke.
///
/// RT-path safe: every member (no allocation; copy, move and destruction call the callable's own
/// noexcept operations). An InplaceFunction is a value: like any object, it is not thread-safe.
///
/// @code
///   using Callback = common::InplaceFunction<void(std::int32_t)>;
///   Callback on_level{[this](const std::int32_t level) noexcept { level_ = level; }};
///   if (on_level) {
///       on_level(3);
///   }
///   // Rejected at compile time: the Status would be lost.
///   // Callback dropped{[&port](std::int32_t) noexcept { return port.write(...); }};
/// @endcode
template <typename R, typename... Args, std::size_t Capacity, std::size_t Alignment>
class InplaceFunction<R(Args...), Capacity, Alignment> final {
    static_assert(Capacity > 0U, "Capacity must be positive");
    static_assert(is_power_of_two(Alignment), "Alignment must be a power of two");

    using Operations = detail::inplace_function::Operations<R, Args...>;

public:
    /// An empty function.
    InplaceFunction() noexcept : operations_{nullptr} {}

    /// Stores a copy of @p callable (moved when given an rvalue). See the class documentation for the
    /// requirements, each checked by a static_assert.
    template <typename F, typename Stored = std::decay_t<F>,
              typename = std::enable_if_t<!std::is_same<Stored, InplaceFunction>::value>>
    explicit InplaceFunction(F&& callable) noexcept : operations_{nullptr} {
        static_assert(sizeof(Stored) <= Capacity, "the callable does not fit: raise Capacity or capture less");
        static_assert(alignof(Stored) <= Alignment, "the callable is over-aligned: raise Alignment");
        static_assert(std::is_nothrow_copy_constructible<Stored>::value &&
                          std::is_nothrow_move_constructible<Stored>::value &&
                          std::is_nothrow_destructible<Stored>::value,
                      "the callable must be nothrow copy- and move-constructible and nothrow destructible");
        static_assert(std::is_nothrow_invocable<const Stored&, Args...>::value,
                      "the callable must be noexcept and callable as const with Args (no mutable lambda)");
        static_assert(detail::inplace_function::returns_exactly<Stored, R, Args...>(),
                      "the callable must return R; handle or discard a result explicitly inside it");
        if (!is_null(callable)) {
            // Deviation (MISRA C++:2023 Rule 21.6.3): placement new into owned, suitably aligned storage.
            static_cast<void>(::new (static_cast<void*>(storage_.data())) Stored(std::forward<F>(callable)));
            operations_ = &detail::inplace_function::Thunks<Stored, R, Args...>::kOperations;
        }
    }

    InplaceFunction(const InplaceFunction& other) noexcept : operations_{nullptr} {
        copy_from(other);
    }

    /// @p other is left empty.
    InplaceFunction(InplaceFunction&& other) noexcept : operations_{nullptr} {
        move_from(other);
    }

    InplaceFunction& operator=(const InplaceFunction& other) noexcept {
        if (this != &other) {
            reset();
            copy_from(other);
        }
        return *this;
    }

    /// @p other is left empty.
    InplaceFunction& operator=(InplaceFunction&& other) noexcept {
        if (this != &other) {
            reset();
            move_from(other);
        }
        return *this;
    }

    ~InplaceFunction() {
        reset();
    }

    /// Calls the stored callable.
    /// @pre !empty()
    R operator()(Args... args) const noexcept {
        COMMON_ASSERT(operations_ != nullptr);
        return operations_->invoke(storage_.data(), std::forward<Args>(args)...);
    }

    [[nodiscard]] bool empty() const noexcept {
        return operations_ == nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return operations_ != nullptr;
    }

    /// Destroys the stored callable; the function becomes empty.
    void reset() noexcept {
        if (operations_ != nullptr) {
            operations_->destroy(storage_.data());
            operations_ = nullptr;
        }
    }

    /// @return true if a callable of type F (decayed) meets every requirement of the class.
    template <typename F>
    [[nodiscard]] static constexpr bool fits() noexcept {
        using Stored = std::decay_t<F>;
        return (sizeof(Stored) <= Capacity) && (alignof(Stored) <= Alignment) &&
               std::is_nothrow_copy_constructible<Stored>::value && std::is_nothrow_move_constructible<Stored>::value &&
               std::is_nothrow_destructible<Stored>::value &&
               detail::inplace_function::returns_exactly<Stored, R, Args...>();
    }

    [[nodiscard]] static constexpr std::size_t capacity() noexcept {
        return Capacity;
    }

private:
    template <typename Stored>
    [[nodiscard]] static bool is_null(const Stored& callable) noexcept {
        bool null{false};
        if constexpr (std::is_pointer<Stored>::value || std::is_member_pointer<Stored>::value) {
            null = (callable == nullptr);
        } else if constexpr (detail::inplace_function::IsInplaceFunction<Stored>::value) {
            null = callable.empty();
        } else {
            static_cast<void>(callable);
        }
        return null;
    }

    void copy_from(const InplaceFunction& other) noexcept {
        if (other.operations_ != nullptr) {
            other.operations_->copy(storage_.data(), other.storage_.data());
            operations_ = other.operations_;
        }
    }

    void move_from(InplaceFunction& other) noexcept {
        if (other.operations_ != nullptr) {
            other.operations_->move(storage_.data(), other.storage_.data());
            operations_ = other.operations_;
            other.operations_ = nullptr;
        }
    }

    alignas(Alignment) std::array<std::byte, Capacity> storage_{};
    const Operations* operations_;
};

}  // namespace common

#endif  // COMMON_FUNCTIONAL_INPLACE_FUNCTION_HPP
