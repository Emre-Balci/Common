/// @file intrusive_list.hpp
/// @brief O(1) doubly-linked list whose links live inside the elements (no allocation).

#ifndef COMMON_CONTAINERS_INTRUSIVE_LIST_HPP
#define COMMON_CONTAINERS_INTRUSIVE_LIST_HPP

#include <cstddef>
#include <functional>
#include <type_traits>

#include "common/core/assert.hpp"

namespace common {

/// Tag of the hook and the list of an element type that is in one kind of list only.
struct DefaultListTag final {};

template <typename T, typename Tag>
class IntrusiveList;

/// The links that put an element of type T into an IntrusiveList<T, Tag>. T derives publicly from
/// IntrusiveListHook<T, Tag> (CRTP), once for each Tag, i.e. once for each list it can be in at
/// the same time. See IntrusiveList for an example.
///
/// The links belong to the element's identity, not to its value:
/// - a copy (or move) of a linked element is a new element and starts unlinked;
/// - copy and move assignment leave the links of both elements unchanged.
/// So copying or assigning elements never corrupts a list. Destroying an element that is still
/// linked is a contract violation (COMMON_ASSERT): remove it from its list first.
///
/// The constructors, assignments and destructor are protected, because a hook only exists as a
/// base of its element. As a consequence, an element type cannot be brace-initialised as an
/// aggregate (the compiler rejects it): give T a constructor.
///
/// A hook belongs to the thread that owns the list the element is in.
template <typename T, typename Tag = DefaultListTag>
class IntrusiveListHook {
public:
    /// @return true while the element is in a list of this Tag.
    [[nodiscard]] bool is_linked() const noexcept {
        return owner_ != nullptr;
    }

protected:
    IntrusiveListHook() noexcept = default;

    /// The copy is a different element: it starts unlinked.
    IntrusiveListHook(const IntrusiveListHook& /*other*/) noexcept {}

    /// The new element starts unlinked; the moved-from element stays where it is.
    IntrusiveListHook(IntrusiveListHook&& /*other*/) noexcept {}

    /// Assigns the element's value, not its place: the links of both elements stay unchanged, which
    /// also makes self-assignment harmless.
    // NOLINTNEXTLINE(bugprone-unhandled-self-assignment,cert-oop54-cpp): nothing is assigned.
    IntrusiveListHook& operator=(const IntrusiveListHook& /*other*/) noexcept {
        return *this;
    }

    /// Like the copy assignment: the links of both elements stay unchanged.
    IntrusiveListHook& operator=(IntrusiveListHook&& /*other*/) noexcept {
        return *this;
    }

    /// @pre !is_linked(): an element is removed from its list before it is destroyed.
    ~IntrusiveListHook() {
        COMMON_ASSERT(owner_ == nullptr);
    }

private:
    friend class IntrusiveList<T, Tag>;

    void clear_links() noexcept {
        prev_ = nullptr;
        next_ = nullptr;
        owner_ = nullptr;
    }

    T* prev_{nullptr};
    T* next_{nullptr};
    const IntrusiveList<T, Tag>* owner_{nullptr};  // nullptr while unlinked
};

/// Doubly-linked list of elements that carry their own links, e.g. the orders queued at one price
/// level, a scheduler's ready list or an LRU list. The elements typically come from an ObjectPool.
///
/// - T derives publicly from IntrusiveListHook<T, Tag>. An element is in at most one list per Tag;
///   one hook per Tag lets it be in several lists at once.
/// - The list does not own its elements: it never creates, copies or destroys them. An element
///   must be removed before it is destroyed (its hook asserts this). clear() and the destructor
///   unlink the remaining elements, so declare a list after the storage of its elements (e.g. the
///   ObjectPool), so that the list is destroyed first.
/// - Each hook records the list that holds the element, so contains() is O(1), and every
///   operation checks with COMMON_ASSERT that the elements it is given are (or are not yet) linked
///   as its precondition requires.
/// - Every operation is O(1) except clear(), for_each() and the destructor, which take at most
///   size() steps. Nothing allocates, blocks or throws: safe on the real-time path.
/// - Not thread-safe: the list and the links of its elements belong to one thread.
///
/// @code
///   struct ByExpiry final {};  // Tag of a second list
///   class Order final : public common::IntrusiveListHook<Order>,
///                       public common::IntrusiveListHook<Order, ByExpiry> {
///   public:
///       explicit Order(const std::uint64_t id) noexcept : id_{id} {}
///       ...
///   };
///
///   common::ObjectPool<Order, 1024U> pool{};
///   common::IntrusiveList<Order> level{};                // declared after the pool
///   common::IntrusiveList<Order, ByExpiry> expiries{};
///
///   Order* const order{pool.create(42U)};
///   if (order != nullptr) {
///       level.push_back(*order);                         // in both lists at once
///       expiries.push_back(*order);
///   }
///   ...
///   Order* const filled{level.pop_front()};              // nullptr if the level is empty
///   if (filled != nullptr) {
///       expiries.remove(*filled);
///       pool.destroy(filled);
///   }
/// @endcode
template <typename T, typename Tag = DefaultListTag>
class IntrusiveList final {
    using Hook = IntrusiveListHook<T, Tag>;

public:
    IntrusiveList() noexcept = default;

    /// Unlinks the remaining elements; they are not destroyed.
    ~IntrusiveList() {
        clear();
    }

    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList(IntrusiveList&&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;
    IntrusiveList& operator=(IntrusiveList&&) = delete;

    /// Inserts @p element before the first element.
    /// @pre !is_linked(element)
    void push_front(T& element) noexcept {
        link(element, nullptr, head_);
    }

    /// Inserts @p element after the last element.
    /// @pre !is_linked(element)
    void push_back(T& element) noexcept {
        link(element, tail_, nullptr);
    }

    /// Inserts @p element just before @p position.
    /// @pre contains(position) and !is_linked(element)
    void insert_before(T& position, T& element) noexcept {
        const Hook& position_links{linked_hook(position)};
        link(element, position_links.prev_, &position);
    }

    /// Inserts @p element just after @p position.
    /// @pre contains(position) and !is_linked(element)
    void insert_after(T& position, T& element) noexcept {
        const Hook& position_links{linked_hook(position)};
        link(element, &position, position_links.next_);
    }

    /// Unlinks @p element; it is not destroyed.
    /// @pre contains(element)
    void remove(T& element) noexcept {
        Hook& links{hook(element)};
        COMMON_ASSERT(links.owner_ == this);
        if (links.prev_ != nullptr) {
            hook(*links.prev_).next_ = links.next_;
        } else {
            head_ = links.next_;
        }
        if (links.next_ != nullptr) {
            hook(*links.next_).prev_ = links.prev_;
        } else {
            tail_ = links.prev_;
        }
        links.clear_links();
        --size_;
    }

    /// Unlinks the first element.
    /// @return the element, or nullptr if the list is empty.
    [[nodiscard]] T* pop_front() noexcept {
        T* const element{head_};
        if (element != nullptr) {
            remove(*element);
        }
        return element;
    }

    /// Unlinks the last element.
    /// @return the element, or nullptr if the list is empty.
    [[nodiscard]] T* pop_back() noexcept {
        T* const element{tail_};
        if (element != nullptr) {
            remove(*element);
        }
        return element;
    }

    /// @return the first element, or nullptr if the list is empty.
    [[nodiscard]] T* front() noexcept {
        return head_;
    }

    /// @return the first element, or nullptr if the list is empty.
    [[nodiscard]] const T* front() const noexcept {
        return head_;
    }

    /// @return the last element, or nullptr if the list is empty.
    [[nodiscard]] T* back() noexcept {
        return tail_;
    }

    /// @return the last element, or nullptr if the list is empty.
    [[nodiscard]] const T* back() const noexcept {
        return tail_;
    }

    /// @pre contains(element)
    /// @return the element after @p element, or nullptr if @p element is the last one.
    [[nodiscard]] T* next(const T& element) noexcept {
        return linked_hook(element).next_;
    }

    /// @pre contains(element)
    /// @return the element after @p element, or nullptr if @p element is the last one.
    [[nodiscard]] const T* next(const T& element) const noexcept {
        return linked_hook(element).next_;
    }

    /// @pre contains(element)
    /// @return the element before @p element, or nullptr if @p element is the first one.
    [[nodiscard]] T* prev(const T& element) noexcept {
        return linked_hook(element).prev_;
    }

    /// @pre contains(element)
    /// @return the element before @p element, or nullptr if @p element is the first one.
    [[nodiscard]] const T* prev(const T& element) const noexcept {
        return linked_hook(element).prev_;
    }

    /// @return true if @p element is in this list. O(1).
    [[nodiscard]] bool contains(const T& element) const noexcept {
        return hook(element).owner_ == this;
    }

    /// @return true if @p element is in a list of this Tag, this one or another. O(1).
    [[nodiscard]] static bool is_linked(const T& element) noexcept {
        return hook(element).is_linked();
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0U;
    }

    /// Unlinks every element; none is destroyed. O(size()).
    void clear() noexcept {
        T* current{head_};
        // size_ does not change in the loop, so it bounds the walk.
        for (std::size_t index{0U}; (index < size_) && (current != nullptr); ++index) {
            Hook& links{hook(*current)};
            current = links.next_;
            links.clear_links();
        }
        head_ = nullptr;
        tail_ = nullptr;
        size_ = 0U;
    }

    /// Calls @p visitor(element) for each element, from front to back. O(size()).
    ///
    /// The visitor must be noexcept and accept a T&. It returns void, or bool to control the walk:
    /// returning false stops it. The visitor may remove the element it was given (and then destroy
    /// it or link it into another list). Unless it returns false, it must not otherwise change this
    /// list, i.e. add, remove or move other elements; removing the next element is caught by
    /// COMMON_ASSERT. The walk makes at most as many calls as the list had elements at its start.
    ///
    /// @code
    ///   ready.for_each([&ready](Task& task) noexcept {
    ///       if (task.done()) {
    ///           ready.remove(task);
    ///       }
    ///   });
    /// @endcode
    template <typename Visitor>
    void for_each(Visitor&& visitor) noexcept {
        walk<T>(visitor);
    }

    /// Read-only walk: as above, but the visitor accepts a const T&.
    template <typename Visitor>
    void for_each(Visitor&& visitor) const noexcept {
        walk<const T>(visitor);
    }

private:
    // The element type is checked here rather than at class scope: a list may be a member of T,
    // and T is then still incomplete when the class is instantiated.
    [[nodiscard]] static Hook& hook(T& element) noexcept {
        static_assert(std::is_base_of<Hook, T>::value, "T must derive from IntrusiveListHook<T, Tag>");
        static_assert(std::is_convertible<T*, Hook*>::value,
                      "T must derive publicly and unambiguously from IntrusiveListHook<T, Tag>");
        return element;
    }

    [[nodiscard]] static const Hook& hook(const T& element) noexcept {
        static_assert(std::is_base_of<Hook, T>::value, "T must derive from IntrusiveListHook<T, Tag>");
        static_assert(std::is_convertible<const T*, const Hook*>::value,
                      "T must derive publicly and unambiguously from IntrusiveListHook<T, Tag>");
        return element;
    }

    /// @pre contains(element)
    [[nodiscard]] const Hook& linked_hook(const T& element) const noexcept {
        const Hook& links{hook(element)};
        COMMON_ASSERT(links.owner_ == this);
        return links;
    }

    /// Links @p element between the adjacent elements @p prev and @p next of this list; nullptr
    /// stands for the end of the list on that side.
    void link(T& element, T* const prev, T* const next) noexcept {
        Hook& links{hook(element)};
        COMMON_ASSERT(links.owner_ == nullptr);
        links.prev_ = prev;
        links.next_ = next;
        links.owner_ = this;
        if (prev != nullptr) {
            hook(*prev).next_ = &element;
        } else {
            head_ = &element;
        }
        if (next != nullptr) {
            hook(*next).prev_ = &element;
        } else {
            tail_ = &element;
        }
        ++size_;
    }

    /// for_each() for Element = T or const T.
    template <typename Element, typename Visitor>
    void walk(Visitor& visitor) const noexcept {
        static_assert(std::is_invocable<Visitor&, Element&>::value, "the visitor must accept the element");
        static_assert(std::is_nothrow_invocable<Visitor&, Element&>::value, "the visitor must be noexcept");
        using Return = std::invoke_result_t<Visitor&, Element&>;
        static_assert(std::is_void<Return>::value || std::is_same<Return, bool>::value,
                      "the visitor must return void, or bool to stop the walk");

        // The visitor may remove elements, so the bound is the size at the start.
        const std::size_t count{size_};
        Element* current{head_};
        bool keep_going{true};
        for (std::size_t visited{0U}; keep_going && (visited < count) && (current != nullptr); ++visited) {
            // Read before the call: the visitor may unlink the current element.
            Element* const following{hook(*current).next_};
            if constexpr (std::is_void<Return>::value) {
                std::invoke(visitor, *current);
            } else {
                keep_going = std::invoke(visitor, *current);
            }
            COMMON_ASSERT((!keep_going) || (following == nullptr) || contains(*following));
            current = following;
        }
    }

    T* head_{nullptr};
    T* tail_{nullptr};
    std::size_t size_{0U};
};

}  // namespace common

#endif  // COMMON_CONTAINERS_INTRUSIVE_LIST_HPP
