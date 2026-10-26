# Design spec v2: the `common` library (application patterns), first batch

Repository: `/mnt/c/Repositories/Common` (WSL path; `C:\Repositories\Common` on Windows), branch
`feature/common-base-and-patterns`. Read `CLAUDE.md` (rules, layout, style) and `README.md`
(components, real-time rules, MISRA section) first: everything there applies to the new code.
v2 folds in four independent design reviews (rules, concurrency, API, portability). Where this
spec states a protocol (memory orders, stop handshakes, arithmetic), implement exactly that.

## 0. What is being built

| Target | Alias | Contents |
|---|---|---|
| `common-base` | `common::base` | Everything that existed before this branch: groups `core`, `numeric`, `containers`, `concurrency`, `time`, `system`, `diagnostics`, `control`, `protocol`, `file`. Unchanged. Umbrella `common/base.hpp`. |
| `common` | `common::common` | Patterns for building larger applications on top of `common::base` (linked PUBLIC). Groups `functional`, `object`, `event`, `state`, `task`, `app`. Umbrella `common/common.hpp` (includes `base.hpp`). |

Both share the include root `include/`. Sources in `src/<group>/`, tests in `tests/<group>/`. Base
tests build `common_base_tests` (links `common::base` only); tests of the new groups build
`common_tests`. CTest `common_base_layering` fails if a file of `common-base` includes a header of
a `common` group.

Patterns of this batch (chosen by the user): lifecycle **Unit** + **UnitManager** (`app`);
**Task** shared by a periodic/triggered **TaskScheduler** and a one-shot-job **TaskExecutor**, plus
**ActiveObject** (`task`); **Type Object** + RTTI-free object types with a registry (`object`);
**Signal** (`event`); **StateMachine** (`state`); **InplaceFunction** (`functional`). An
integration test and an example application show them working together (2.12).

## 1. Conventions for all new code

Style and rules (derived from the existing code; follow them exactly):
- C++17 only (GCC 9.4 and Clang 10 with libstdc++ 9 here, MSVC 19.32, Android NDK r29 clang + libc++).
- Namespace `common`; internal helpers in `common::detail` or `common::detail::<component>`.
- Header: `/// @file <name>.hpp`, `/// @brief <one line>`, guard `COMMON_<GROUP>_<NAME>_HPP`, system
  headers sorted, blank line, `"common/..."` headers sorted. A `.cpp` starts with its own header,
  blank line, system headers, blank line, other project headers (see `src/time/clock.cpp`). Tests:
  system headers, then project headers and `"test_framework.hpp"`; helpers in an anonymous namespace.
- 4-space indent, 120 columns, `lower_case` functions/variables, `CamelCase` types, `kConstant`,
  `member_`. Braces on every branch, brace-initialisation, `const` locals, `static_cast` only,
  fixed-width integers for stored values, `std::size_t` for sizes/indices/capacities, `std::array`
  (no C arrays), `nullptr`.
- Everything `noexcept`. `[[nodiscard]]` on queries. `Status`/`Result<T>` for errors;
  `COMMON_ASSERT` for contract violations. Never call throwing std members. `if ... else if` ends with
  `else`; `switch` over an enum class names every enumerator, no `default`, `break` in each case.
- Every loop has an explicit bound in its condition, including retry, CAS, drain and wait loops
  (a wait on a `Deadline` computes its maximum number of rounds first, with saturating arithmetic).
  The only exception is a thread's service loop that runs until it is told to stop: document it.
- No heap: no `new` except placement new into owned storage (deviation comment), no
  `std::function`/`vector`/`string`. **No `std::stable_sort`, `std::inplace_merge`,
  `std::stable_partition`: they allocate.** Never block on the RT path.
- Time: `Duration`/`TimePoint`/`MonotonicClock`/`Deadline`. In a struct that must be nothrow
  default-constructible (stored in `Result<T>`, a queue or a `SeqLock`) store times as `std::int64_t`
  nanoseconds. Arithmetic on times saturates (`common/numeric/integer.hpp`).
- Atomics: explicit memory orders and a comment on the protocol; `static_assert(...::is_always_lock_free)`
  for every atomic type used (see `watchdog.hpp`, `mpmc_queue.hpp`).
- Documentation in the library's tone (`intrusive_list.hpp`, `timer_wheel.hpp`): purpose, `@pre`,
  `@return`, which members are RT-path safe, thread-safety (which thread may call what), and an
  `@code` example on the main class. A class that embeds large storage says "give it static storage
  duration". Document every non-obvious decision.
- MISRA deviations: `// Deviation (MISRA C++:2023 Rule X.Y.Z): reason` at the site, listed in your
  report. Known: 21.6.3 placement new into owned storage and 8.2.5 `reinterpret_cast` from storage to
  the object with `std::launder` (as `StaticVector`/`ObjectPool`). No recursion (8.2.10).
- clang-format 10 here cannot read `.clang-format` (it has `InsertBraces`): format by hand.

Portability rules (each verified by a probe with the project's flags on GCC 9.4, Clang 10, NDK r29 and
MSVC 19.32):
- **No `friend` in a polymorphic class with a protected non-virtual destructor** (`Task`, `Unit`,
  `Object`, user intermediates): GCC's `-Wnon-virtual-dtor` then treats the destructor as accessible and
  fails at the class and at every derived class. Give runners access through a passkey instead (2.6).
- **A type published through `SeqLock` is trivial** — no default member initialisers, value-initialise
  it with `{}` — not only trivially copyable: GCC 9's `-Wclass-memaccess` rejects `SeqLock`'s memcpy into
  a type with a non-trivial default constructor.
- **Every defaulted default constructor is written `X() noexcept = default;`**: MSVC 19.32 deduces a
  potentially-throwing constructor for members like `std::array<ClassType, N> x_{}` (commit 814d70f).
- **Conditions on non-type template parameters use `if constexpr` or `static_assert`**: a plain `if
  (Workers > 1U)` is MSVC C4127 (fatal with /WX). `if (std::is_void<R>::value)` is fine.
- `TimePoint`'s default constructor is not `noexcept` with libstdc++ or libc++ (only with MSVC's STL).
- Never use Qt's keyword macros as identifiers (`emit`, `signals`, `slots`, `foreach`): Qt defines them
  as macros, and applications on this library may include Qt.
- Tests: self-assignment goes through a reference (`T& alias{x}; x = alias;`; `x = x;` is Clang's
  `-Wself-assign-overloaded`); pass typed null pointers to overloaded functions (`object_cast<T>(nullptr)`
  is ambiguous between the `Object*` and `const Object*` overloads).

Status codes — one rule, matching their established meaning in the library:
- `kAlreadyExists`: a duplicate registration — the same object, name or id added twice, or a task
  that is already in a runner (scheduled, queued or running).
- `kAlreadyRunning`: `start()` of something that is already running. `kNotRunning`: `stop()` of
  something that is not running; also `submit()`/`post()` refused because the runner is stopping.
- `kInvalidArgument`: a bad argument, **or a call made in the wrong state** (as `File::open` on an
  open handle): e.g. `Unit::initialize()` when not `kCreated`, `UnitManager::add()` after
  `initialize()`.
- `kResourceLimit`: a fixed capacity is exhausted (also a lock-free queue too contended to accept).
- `kNotFound`: unknown handle/name/type, or a dependency that was not added.
- `kTimeout`: a bounded wait ended. `kOutOfRange`: a value beyond a documented limit.
- Never add `Status` enumerators (that would change `common-base`).

Class hierarchies (`Object`, `Task`, `Unit` and user classes derived from them):
- A leaf class is `final` and is the only kind that is instantiated. An intermediate class (e.g.
  `Vehicle` between `Object` and `Truck`, or a test helper base derived from `Task`) declares a
  **protected non-virtual destructor and protected copy/move operations, and no friends**; otherwise
  `-Wnon-virtual-dtor` (GCC, Clang) or C4265 (MSVC, for `Task` and `Unit`, whose destructors are not
  trivial) fails the build. A class that is both instantiated and derived from is not supported.
  Verified warning-free with GCC 9, Clang 10, the NDK and MSVC 19.32 (`/w14265`).
- The interface bases themselves: protected non-virtual destructor; hooks `virtual` (pure or with a
  default); overriders use `override` only (MISRA 13.3.1); no virtual call from a constructor or
  destructor.
- Classes that are referred to by pointer, handle or thread (`Signal`, `StateMachine` and its
  `Definition`, `ObjectTypeRegistry`, `TaskScheduler`, `TaskExecutor`, `ActiveObject`, `Unit`,
  `UnitManager`, `Task`) delete copy and move, as `TimerWheel` does.

Calls from user code (callbacks into the library) — MISRA forbids recursion, so each runner states
which of its functions a callback may call, and asserts (`COMMON_ASSERT`) the forbidden ones, like
`TimerWheel::advance()` does with `advancing_`. Cross-thread checks use a thread-local "current
runner" pointer (`detail::RunnerScope`, 2.6), never a plain flag read from another thread.

Runners and supervision: `TaskScheduler`, `TaskExecutor` and `ActiveObject` each run on threads they
own (`start(config, ...)`, `stop(...)`, `running()`), can be started again after `stop()`, and expose
`Heartbeat`s. A heartbeat is **parked** (`beat(TimePoint::max())`) from construction until its thread
runs and again after an orderly stop, so that a `Watchdog` entry added during initialisation neither
expires before the runner starts nor at shutdown, while a hung thread still expires.

## 2. Components

### 2.1 `functional/inplace_function.hpp` — `InplaceFunction`

```cpp
inline constexpr std::size_t kInplaceFunctionCapacity{4U * sizeof(void*)};
/// 16 on every platform: alignof(std::max_align_t) is 16 with GCC, Clang and the NDK but 8 with MSVC,
/// and a fixed value keeps fits<>() and sizeof the same everywhere.
inline constexpr std::size_t kInplaceFunctionAlignment{16U};

template <typename Signature, std::size_t Capacity = kInplaceFunctionCapacity,
          std::size_t Alignment = kInplaceFunctionAlignment>
class InplaceFunction;   // declared only: Signature must be a function type R(Args...)

template <typename R, typename... Args, std::size_t Capacity, std::size_t Alignment>
class InplaceFunction<R(Args...), Capacity, Alignment> final {
public:
    InplaceFunction() noexcept;                                   // empty
    template <typename F, /* F is not this InplaceFunction type */>
    explicit InplaceFunction(F&& callable) noexcept;             // stores a copy of the decayed callable
    InplaceFunction(const InplaceFunction&) noexcept;  InplaceFunction(InplaceFunction&&) noexcept;  // moved-from: empty
    InplaceFunction& operator=(const InplaceFunction&) noexcept;  InplaceFunction& operator=(InplaceFunction&&) noexcept;
    ~InplaceFunction();
    R operator()(Args... args) const noexcept;                    // @pre !empty() (COMMON_ASSERT)
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] explicit operator bool() const noexcept;
    void reset() noexcept;
    template <typename F> [[nodiscard]] static constexpr bool fits() noexcept;   // all requirements below
    [[nodiscard]] static constexpr std::size_t capacity() noexcept;
};
```
- Requirements on the decayed callable `F`, each a `static_assert` with a clear message (and `fits<F>()`
  reports them all, for generic code): `sizeof(F) <= Capacity`; `alignof(F) <= Alignment` (Alignment a
  power of two); nothrow copy- and move-constructible and nothrow destructible (so `InplaceFunction`
  is nothrow copy/move-assignable and fits `SpscQueue`/`MpmcQueue`); `std::is_nothrow_invocable<const F&,
  Args...>`; and **the callable's result type, with cv and reference removed, is exactly `R`'s**
  (`void` for `void`): a lambda returning a `[[nodiscard]] Status` cannot be stored in an
  `InplaceFunction<void()>` — message: "the callable must return R; handle or discard a result
  explicitly inside it". Invoked as const (mutable lambdas are rejected; capture a pointer to state).
- The converting constructor is `explicit`; APIs that take callables are templates.
- Empty results: a null function pointer, a null pointer-to-member, or an empty `InplaceFunction` of
  the same signature and any capacity/alignment gives an empty `InplaceFunction` (as `std::function`
  does). A non-empty smaller `InplaceFunction` is stored as a callable. Pointers to member functions
  work through `std::invoke`.
- One pointer to a static constant operations table per stored type (invoke, copy-construct,
  move-construct-and-destroy-source, destroy). **Its form is fixed**: `static constexpr Operations
  kOperations{...};` as a member of a class template `detail::inplace_function::Thunks<F, R, Args...>`,
  and the InplaceFunction stores `&Thunks<Stored, R, Args...>::kOperations`. Never a static data member
  template of InplaceFunction: MSVC 19.32 reports C4268 for it (fatal with /WX). Storage
  `alignas(Alignment) std::array<std::byte, Capacity>`; placement new (21.6.3) and
  `std::launder(reinterpret_cast<F*>(...))` (8.2.5) as `StaticVector` does.
- RT-path safe: everything. A value type (not thread-safe, like any).
- Tests: empty state; lambdas with captures; return values; by-value/reference/`const&` arguments;
  function pointer; null function pointer and empty smaller InplaceFunction → empty; pointer to
  member function; copy/move and exactly-once destruction (instance-counting functor);
  self-assignment; assignments across empty/non-empty; a callable exactly filling `Capacity`;
  `fits<>()` false for: too big, over-aligned, throwing, mutable, `Status`-returning for `void`,
  `int64_t`-returning for `int32_t`; element of an `SpscQueue`; `static_assert`s of noexcept-ness.

### 2.2 `object/object_type.hpp` — `ObjectType`, `ObjectTypeOf`, `TypeObject`, `Object`, `object_cast`

```cpp
inline constexpr std::size_t kMaxObjectTypeNameLength{63U};
inline constexpr std::size_t kMaxObjectTypeDepth{32U};
inline constexpr std::uint32_t kObjectRootTypeId{0xFFFF'FFFFU};   // reserved: Object::kType

class ObjectType {   // not final: ObjectTypeOf and TypeObject derive from it
public:
    /// @pre name non-null, 1..kMaxObjectTypeNameLength characters, outlives the type (a literal);
    ///      parent == nullptr or parent->depth() < kMaxObjectTypeDepth. (In a constant expression a
    ///      violation fails to compile.)
    constexpr ObjectType(const char* name, const ObjectType* parent, std::uint32_t id = 0U) noexcept;
    ~ObjectType() = default;   copy/move deleted (a type is an identity)
    [[nodiscard]] constexpr const char* name() const noexcept;
    [[nodiscard]] constexpr const ObjectType* parent() const noexcept;
    [[nodiscard]] constexpr std::uint32_t id() const noexcept;      // 0: none
    [[nodiscard]] constexpr std::size_t depth() const noexcept;     // ancestors; 0 for a root
    /// The same type: the same object; or, only when both ids are non-zero, equal ids and equal names.
    [[nodiscard]] constexpr bool same_as(const ObjectType& other) const noexcept;
    /// This type is ancestor or derives from it: walks up depth() - ancestor.depth() parents, then
    /// one same_as(). O(depth()).
    [[nodiscard]] constexpr bool is_a(const ObjectType& ancestor) const noexcept;
};

/// Checks data before it becomes a type (e.g. a loader reading reference data): kInvalidArgument for
/// an empty name, kOutOfRange for a name longer than kMaxObjectTypeNameLength or a parent already at
/// kMaxObjectTypeDepth.
[[nodiscard]] constexpr Status check_object_type(std::string_view name, const ObjectType* parent) noexcept;

/// The type of C++ class T whose base in the object hierarchy is Base (Object for a root class).
template <typename T, typename Base>
class ObjectTypeOf final : public ObjectType {
public:
    using Class = T;
    using BaseClass = Base;
    explicit constexpr ObjectTypeOf(const char* name, std::uint32_t id = 0U) noexcept;  // parent = &Base::kType
};

/// A type defined by data (Type Object pattern): a name it owns (copied), Properties shared by all its
/// instances, and a parent. E.g. instrument types read from reference data.
template <typename Properties>
class TypeObject final : /* private name-storage base first (base-from-member), then */ public ObjectType {
public:
    constexpr TypeObject(std::string_view name, const Properties& properties, std::uint32_t id = 0U) noexcept;
    constexpr TypeObject(std::string_view name, const Properties& properties, const TypeObject& parent,
                         std::uint32_t id = 0U) noexcept;
    /// A first-level type under a class type (e.g. Instrument::kType), so that an Instrument whose
    /// object_type() is this TypeObject still passes object_cast<Instrument>.
    constexpr TypeObject(std::string_view name, const Properties& properties, const ObjectType& root,
                         std::uint32_t id = 0U) noexcept;
    [[nodiscard]] constexpr const Properties& properties() const noexcept;
    [[nodiscard]] constexpr const TypeObject* parent_type() const noexcept;   // nullptr under a root
};

/// Base of objects that know their type at run time without RTTI.
class Object {
public:
    static constexpr ObjectType kType{"common::Object", nullptr, kObjectRootTypeId};
    [[nodiscard]] virtual const ObjectType& object_type() const noexcept = 0;
    [[nodiscard]] bool is_a(const ObjectType& type) const noexcept;
protected:
    Object() noexcept = default;   ~Object() = default;   protected defaulted copy/move
};

template <typename T> [[nodiscard]] T* object_cast(Object* object) noexcept;
template <typename T> [[nodiscard]] const T* object_cast(const Object* object) noexcept;
```
- The idiom, in every class of a hierarchy:
  ```cpp
  class Vehicle : public common::Object {          // intermediate: protected destructor, copy, move
  public:
      static constexpr common::ObjectTypeOf<Vehicle, common::Object> kType{"Vehicle"};
      [[nodiscard]] const common::ObjectType& object_type() const noexcept override { return kType; }
  protected:
      Vehicle() noexcept = default;  ~Vehicle() = default;  ... protected copy/move
  };
  class Truck final : public Vehicle {
  public:
      static constexpr common::ObjectTypeOf<Truck, Vehicle> kType{"Truck"};
      [[nodiscard]] const common::ObjectType& object_type() const noexcept override { return kType; }
  };
  ```
- `object_cast<T>`: `static_assert`s that T derives from `Object`; `T::kType` is an
  `ObjectTypeOf<T, B>` declared for T itself (`Class == T`: an inherited kType fails); `B` is a base
  of T. Then `is_a(T::kType) ? static_cast<T*>(object) : nullptr`.
- Identity: with id 0 a type is identified by its address; a type duplicated per module (a Windows
  DLL, or a shared object built with `-fvisibility=hidden`) needs a program-unique non-zero id, or
  casts across modules fail safely (nullptr). A name alone never establishes identity. Names are
  compared with `==` per character (MISRA 7.0.3: no hashing of character values), bounded.
- The constructors' validation (name length, depth) does **not** depend on `COMMON_ASSERT`: when it
  fails they call `common::detail::assert_fail()` directly, so it is a compile error in a constant
  expression and an abort at run time even with `COMMON_DISABLE_ASSERTS` (otherwise an over-long name
  would make the bounded comparison stop early, and object_cast could match the wrong class).
- MISRA note for users: each class's `kType` hides its base's (the idiom relies on that and
  `object_cast` checks it). If a project's checker applies Rule 6.4.1 to class scopes, it records a
  deviation for the idiom; say so in the header documentation (the library itself hides nothing).
- `TypeObject`: copies the name into its own `std::array<char, kMaxObjectTypeNameLength + 1U>` held
  by a private base declared before `ObjectType` (base-from-member), so names read into reused buffers
  (e.g. `LineReader`) are safe; constexpr use with literals still works. Its constructors assert what
  `check_object_type()` checks. Data-defined types live where they outlive their users, e.g. an
  `ObjectPool<TypeObject<P>, N>` created at initialisation.
- RT-path safe: everything. Thread-safe: types are immutable after construction.
- Tests: constexpr `static_assert`s of is_a/depth; intermediate class built warning-free; same name
  with id 0 in two namespaces → not same, `object_cast` → nullptr; same non-zero id and name → same;
  `object_cast` success/failure/nullptr/const; TypeObject hierarchy with properties under a class
  root; an object whose `object_type()` is a TypeObject; `check_object_type` errors; TypeObjects
  created in an `ObjectPool` from names in a reused buffer.

### 2.3 `object/object_type_registry.hpp` — `ObjectTypeRegistry`, `ObjectFactory`, `pool_factory`

```cpp
/// Creates an object of type (the registered type that create() found) from arguments whose type is
/// agreed between the caller of create() and the factory; nullptr when none is available.
using ObjectCreate = Object* (*)(const ObjectType& type, const void* arguments, void* context) noexcept;
using ObjectDestroy = void (*)(Object& object, void* context) noexcept;
struct ObjectFactory final {
    ObjectCreate create{nullptr};
    ObjectDestroy destroy{nullptr};
    void* context{nullptr};
};
/// Creates T in pool: T(type, arguments) when T is nothrow-constructible from (const ObjectType&,
/// const void*), T() otherwise; destroys through object_cast<T> back into the pool.
template <typename T, std::size_t Capacity>
[[nodiscard]] ObjectFactory pool_factory(ObjectPool<T, Capacity>& pool) noexcept;

template <std::size_t MaxTypes, typename Type = ObjectType>
class ObjectTypeRegistry final {
public:
    /// kOk; kInvalidArgument (a factory with only one of create/destroy); kAlreadyExists (the same type
    /// object, a same_as type, the same name, or the same non-zero id); kResourceLimit (full).
    Status add(const Type& type, const ObjectFactory& factory = ObjectFactory{}) noexcept;
    [[nodiscard]] const Type* find(std::string_view name) const noexcept;
    [[nodiscard]] const Type* find_by_id(std::uint32_t id) const noexcept;   // 0 never matches
    [[nodiscard]] bool contains(const ObjectType& type) const noexcept;
    /// kNotFound, kNotSupported (no factory), kResourceLimit (the factory returned nullptr).
    [[nodiscard]] Result<Object*> create(std::string_view name, const void* arguments = nullptr) const noexcept;
    /// create(), then object_cast<T>; on a mismatch destroys the object again: kInvalidArgument.
    template <typename T>
    [[nodiscard]] Result<T*> create_as(std::string_view name, const void* arguments = nullptr) const noexcept;
    /// Through the factory of the object's own type (exact match). kNotFound, kNotSupported.
    Status destroy(Object& object) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] const Type& operator[](std::size_t index) const noexcept;   // registration order
    [[nodiscard]] static constexpr std::size_t capacity() noexcept;
};
```
- Filled during initialisation, then read-only (reads from threads started afterwards are safe).
  RT-path safe: `find`, `find_by_id`, `contains`, `size`, `operator[]`; `create`/`destroy` are as safe
  and as thread-safe as the factory (`pool_factory`: yes, on the pool's thread).
- Tests: duplicates (name, id, object), capacity, find, create/destroy via `pool_factory` (pool
  exhaustion → kResourceLimit; destroying an unregistered type → kNotFound); one factory creating
  instances of three TypeObjects, each with the right `object_type()`; `create_as` with arguments
  (two instances with different names); mismatch → kInvalidArgument and no leak.

### 2.4 `event/signal.hpp` — `Signal`, `Connection`, `ScopedConnection`

```cpp
struct Connection final { std::uint32_t index{UINT32_MAX}; std::uint32_t generation{0U}; };  // == and !=

template <typename Signature, std::size_t MaxSlots, std::size_t SlotCapacity = kInplaceFunctionCapacity>
class Signal;   // declared only
template <typename... Args, std::size_t MaxSlots, std::size_t SlotCapacity>
class Signal<void(Args...), MaxSlots, SlotCapacity> final {
public:
    using Slot = InplaceFunction<void(Args...), SlotCapacity>;
    Signal() noexcept;  ~Signal();  copy/move deleted
    /// kInvalidArgument (an empty callable), kResourceLimit (no free slot; inside an emission a slot
    /// disconnected during it is only freed when the emission ends).
    template <typename F> [[nodiscard]] Result<Connection> connect(F&& slot) noexcept;
    Status disconnect(Connection connection) noexcept;     // kNotFound if unknown or stale
    void disconnect_all() noexcept;
    [[nodiscard]] bool connected(Connection connection) const noexcept;
    /// Calls the connected slots in connection order with args (as lvalues). @return slots called.
    /// Named notify, not emit: Qt defines emit as a macro. @pre not called from a slot of this signal
    /// (COMMON_ASSERT; no recursion)
    std::size_t notify(Args... args) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;  [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] static constexpr std::size_t capacity() noexcept;
};

class ScopedConnection final {   // disconnects on destruction; move-only; type-erased (any Signal)
public:
    ScopedConnection() noexcept = default;
    template <typename SignalType> ScopedConnection(SignalType& signal, Connection connection) noexcept;
    ~ScopedConnection();   ScopedConnection(ScopedConnection&&) noexcept;   ScopedConnection& operator=(ScopedConnection&&) noexcept;
    copy deleted
    void reset() noexcept;                            // disconnects now
    [[nodiscard]] Connection release() noexcept;      // stops managing; stays connected
    [[nodiscard]] Connection connection() const noexcept;
};
```
- During a notification (a `notify()` call): a slot may connect, disconnect any connection (itself
  included) and call `connected()`/`size()`. A disconnect takes effect at once for `connected()`
  (false), `disconnect()` (kNotFound) and `size()`; a slot disconnected during the notification is not
  called later in it; its callable is destroyed only when the notification ends (never while it
  runs). A slot connected during a notification is first called by the next one. `~Signal()`: @pre
  not notifying (asserted).
- Connection order is kept in an order list of slot indices (O(MaxSlots) work on connect/disconnect,
  no sorting).
- Slot callables must return `void` exactly (an `InplaceFunction` rule). Rvalue-reference `Args` are
  rejected (`static_assert`).
- Not thread-safe: a signal belongs to one thread; cross-thread notification goes through a queue or
  an `ActiveObject`. The signal must outlive its `ScopedConnection`s. RT-path safe: everything.
- Tests: connect/notify/disconnect; connection order after index reuse; capacity; stale connections;
  disconnect of self/others/later slots during notify; connect during notify (not called until the
  next); kResourceLimit inside a notification; ScopedConnection move/reset/release; empty smaller
  InplaceFunction → kInvalidArgument; no callable destroyed twice or while running.

### 2.5 `state/state_machine.hpp` — `StateMachine`

```cpp
enum class TransitionKind : std::uint8_t {
    kExternal,   ///< exit(source), action, enter(target) — also when source == target
    kInternal,   ///< action only; target must equal source
};
struct NoEventData final {};

template <typename State, typename Event, typename Context, typename Data = NoEventData>
class StateMachine final {     // State and Event are enumerations
public:
    using Guard = bool (*)(const Context& context, Event event, const Data& data) noexcept;
    using Action = void (*)(Context& context, Event event, const Data& data) noexcept;
    using StateAction = void (*)(Context& context) noexcept;
    using Observer = void (*)(Context& context, State from, Event event, State to) noexcept;
    struct Transition final {
        State source; Event event; State target;
        Guard guard{nullptr};    Action action{nullptr};    TransitionKind kind{TransitionKind::kExternal};
    };
    struct StateActions final { State state; StateAction on_entry{nullptr}; StateAction on_exit{nullptr}; };

    /// The immutable part, shared by any number of machines (e.g. one per order). Init phase: validates
    /// the tables once (COMMON_ASSERT: internal transitions have target == source; each state at most
    /// once in state_actions). The tables are not copied: keep them alive (static constexpr arrays).
    class Definition final {
    public:
        Definition(Span<const Transition> transitions, Span<const StateActions> state_actions,
                   Observer observer = nullptr) noexcept;
        copy/move deleted
    };

    /// O(1), RT-path safe.
    StateMachine(const Definition& definition, State initial, Context& context) noexcept;
    void start() noexcept;          // @pre !started(): state() = initial, started() = true, then its on_entry
    /// First transition (table order) from state() on event whose guard allows it: external: on_exit of
    /// the source, action, state() = target, on_entry of the target; internal: action only. Then the
    /// observer (from, event, to). @return true if one fired; false: ignored (no observer call).
    bool dispatch(Event event, const Data& data = Data{}) noexcept;
    [[nodiscard]] bool can_dispatch(Event event, const Data& data = Data{}) const noexcept;  // guards only
    void reset() noexcept;          // back to not started, no exit action (e.g. a unit started again)
    [[nodiscard]] State state() const noexcept;
    [[nodiscard]] bool started() const noexcept;
};
```
- Re-entry: `start()`, `dispatch()` and `reset()` set an in-transition flag for their whole duration
  (guards, on_exit, action, on_entry, observer) and `COMMON_ASSERT` it is clear on entry. `state()`,
  `started()` and `can_dispatch()` stay callable inside: `state()` is the source during the guards,
  on_exit and the action, the target from on_entry on; for an internal transition the source
  throughout. A follow-up event goes through a queue (e.g. an `ActiveObject` command, whose `post()`
  can fail: handle it).
- Define the tables at namespace scope **after** the Context class (as `static constexpr` members of
  the Context class they fail with Clang 10: member access into an incomplete type). Captureless
  `noexcept` lambdas convert to the function pointers.
- RT-path safe: everything except `Definition`'s constructor (init phase). Not thread-safe (one
  owning thread per machine; the Definition is immutable and shareable).
- Tests: a mode machine (init/run/degraded/safe) with entry/exit counts; an order machine whose Fill
  data selects PartiallyFilled or Filled through a guard; first-match order; internal vs external
  self-transition; ignored events; can_dispatch; observer once per fired transition; state() as seen
  from each hook; reset() then start() again; 1000 machines sharing one Definition (static storage).

### 2.6 `task/task.hpp` + `src/task/task.cpp` — `Task` (shared interface) and runner support

```cpp
enum class TaskState : std::uint8_t {
    kIdle,       ///< In no runner (initial; also after TaskScheduler::remove()).
    kScheduled,  ///< In a TaskScheduler, waiting for its next activation or a trigger.
    kSuspended,  ///< In a TaskScheduler, not activated until resumed.
    kQueued,     ///< Submitted to a TaskExecutor, waiting for a worker.
    kRunning,    ///< run() is executing.
    kDone,       ///< Job finished: run() returned kOk.
    kFailed,     ///< Job finished: run() returned an error.
    kCancelled,  ///< Job cancelled before it ran, or it returned an error after a cancel request.
};
[[nodiscard]] constexpr const char* to_string(TaskState state) noexcept;
[[nodiscard]] constexpr bool is_finished(TaskState state) noexcept;    // kDone, kFailed, kCancelled

/// How a runner's stop() treats work that is queued (TaskExecutor, ActiveObject).
enum class StopMode : std::uint8_t {
    kDrain,    ///< Refuse new work; run everything already queued; then the threads exit.
    kCancel,   ///< Refuse new work; queued jobs become kCancelled / queued commands are discarded;
               ///< running jobs get cancel(); the threads exit after their current item.
};

/// Clock of a runner: &MonotonicClock::now, or a test's fake clock. The now passed to align()/
/// run_once() comes from the same clock.
using TaskClock = TimePoint (*)() noexcept;

struct TaskActivation final {
    TimePoint now;            ///< When this run started (runner's clock, read just before run()).
    TimePoint release;        ///< When it became due: activation time (scheduler) or submission time (executor).
    Deadline deadline;        ///< When it should be finished; Deadline::never() if none.
    std::uint64_t sequence;   ///< Earlier runs of this task.
    std::uint64_t missed;     ///< Periodic activations skipped just before this run (0 for jobs and triggered runs).
};

/// A trivial aggregate (no default member initialisers: it is published through SeqLock, see §1):
/// value-initialise it, TaskStatistics s{}, for every counter 0 and last_status == Status::kOk.
/// static_assert(std::is_trivial<TaskStatistics>::value). Times in ns of the runner's clock.
struct TaskStatistics final {
    std::uint64_t runs;
    std::uint64_t failures;            ///< runs that returned an error
    std::uint64_t overruns;            ///< runs longer than their budget
    std::uint64_t deadline_misses;     ///< runs that finished after their deadline
    std::uint64_t missed_activations;  ///< periodic activations skipped
    std::int64_t last_execution_ns;
    std::int64_t min_execution_ns;     ///< 0 before the first run
    std::int64_t max_execution_ns;
    std::int64_t total_execution_ns;   ///< saturating sum (mean = total / runs)
    std::int64_t max_lateness_ns;      ///< longest delay from release to start (never negative)
    Status last_status;
};

class Task {
public:
    [[nodiscard]] const char* name() const noexcept;
    [[nodiscard]] TaskState state() const noexcept;                        // any thread (acquire)
    [[nodiscard]] bool try_statistics(TaskStatistics& out) const noexcept;  // any thread (SeqLock snapshot)
    /// Requests, any thread, wait-free; they never change state() themselves:
    void cancel() noexcept;      // a queued job is not run (kCancelled when dequeued); run() may stop early
    [[nodiscard]] bool cancel_requested() const noexcept;
    void trigger() noexcept;     // TaskScheduler: one extra run at its next pass (coalesced). Executor: ignored.
    void suspend() noexcept;     // TaskScheduler: no activations from its next pass on. Executor: ignored.
    void resume() noexcept;      // TaskScheduler: back on the grid at its next pass (no catch-up).
protected:
    explicit Task(const char* name) noexcept;   // name: static storage; nullptr becomes ""
    ~Task();    // @pre in no runner: state() is kIdle or finished (COMMON_ASSERT)
    copy/move deleted
    virtual Status run(const TaskActivation& activation) noexcept = 0;   // bounded work, runner's thread
};

/// Non-RT (they sleep). kOk when reached, kTimeout, kInvalidArgument (poll <= 0), or a sleep error.
/// At most remaining/poll + 1 rounds (saturating); each sleeps until min(now + poll, deadline).
Status wait_until_finished(const Task& task, const Deadline& deadline, Duration poll = std::chrono::milliseconds{1}) noexcept;
Status wait_until_state(const Task& task, TaskState state, const Deadline& deadline, Duration poll = std::chrono::milliseconds{1}) noexcept;
```
Internals (for the runners; `detail`, not for applications):
- **State and cancel request share one atomic** (the cancel request is a bit next to the state), so a
  claim clears a stale cancel request in the same compare-exchange, a `cancel()` after the claim is
  never lost (`fetch_or`), and `cancel()` never writes the state. Trigger and suspension requests are
  separate atomics in the task: `trigger()` = `exchange(true, release)` (an RMW, so every coalesced
  trigger is in the release sequence the scheduler reads); `suspend()`/`resume()` store a desired
  suspension (release) that the scheduler applies.
- **No `friend` in `Task`** (§1: GCC's -Wnon-virtual-dtor). Runners reach the internals through a
  passkey declared in `task.hpp` before `Task`: `namespace detail { class TaskAccess; class TaskKey final
  { friend class TaskAccess; explicit TaskKey() noexcept = default; }; }` — the `explicit` keeps it from
  being an aggregate, so only `TaskAccess` can make a key (`TaskKey{}` elsewhere does not compile;
  verified on GCC, Clang and MSVC). `Task` has public members that take a `detail::TaskKey` (e.g. one
  that runs `run()`, and one that returns a reference to a non-polymorphic `detail::TaskControl` struct
  holding the state atomic, the request flags, the submission fields and the statistics), documented as
  "for the runners only". `detail::TaskAccess` wraps them.
- `detail::TaskAccess` gives the runners: `claim(task, desired)` — compare-exchange from
  kIdle/kDone/kFailed/kCancelled to `desired` (success `acq_rel`, failure `acquire`, bounded attempts),
  clearing the cancel bit; the acquire half orders the previous runner's writes before the new one, so
  the statistics `SeqLock` keeps a single writer at a time; `set_state` (release, keeps the cancel
  bit; bounded CAS); `take_trigger` (`exchange(false, acquire)`); `clear_trigger`;
  `desired_suspension`; submission release/deadline store/load (plain fields written only by the
  claimer after a won claim and published by the queue's release/acquire); and
  `execute(task, activation, budget, clock)` which calls `run()`, measures execution with `clock`,
  updates the runner's copy of the statistics (runs, failures, overrun vs budget (zero: none), deadline
  miss, lateness = now − release clamped at 0, min/max/total saturating, last_status,
  missed_activations += activation.missed), publishes them (`SeqLock::store`) **before** the caller's
  state store, and returns `run()`'s status.
- `detail::RunnerScope` (in `task.cpp`, used by all three runners): a constant-initialised
  `thread_local const void*` naming the runner whose thread or pass is executing user code on this
  thread; `RunnerScope scope{this};` sets and restores it; `detail::current_runner()` reads it. Runners
  assert `current_runner() != this` in `start`, `stop`, `run_pending`, `run_once`, `add`, `remove`, ...
- `static_assert` that `std::atomic<std::uint8_t>`, `std::atomic<bool>` are always lock-free.
- Tests (task core alone, through `TaskAccess::execute` with a fake clock): statistics arithmetic
  (min/max/total/overrun/deadline miss/lateness clamp/saturation/missed), failures and last_status,
  claim/cancel-bit semantics (a cancel after a claim survives; a claim clears an old one), the
  request flags, names, `to_string`, `is_finished`, `wait_until_*` (timeout, immediate success,
  kInvalidArgument for poll <= 0), `RunnerScope` nesting, `try_statistics` from another thread (TSan).

### 2.7 `task/task_scheduler.hpp` + `src/task/task_scheduler.cpp` — `TaskScheduler`

```cpp
struct TaskSchedule final {
    Duration period{};         ///< > 0: periodic. Zero requires triggered_only.
    Duration offset{};         ///< first activation at origin + offset; >= 0
    Duration budget{};         ///< execution time above which a run is an overrun; zero: no budget
    Duration deadline{};       ///< relative to the release; zero: the period (none for a triggered-only task)
    std::uint8_t priority{0U}; ///< order among tasks due in the same pass (higher first; ties: add() order). Never preempts.
    bool triggered_only{false};///< runs only when triggered (period must be zero)
    bool suspended{false};     ///< added as kSuspended (e.g. resumed later in a unit's on_start())
    [[nodiscard]] static constexpr TaskSchedule periodic(Duration period, Duration offset = Duration{}) noexcept;
    [[nodiscard]] static constexpr TaskSchedule triggered() noexcept;
};

template <std::size_t MaxTasks>
class TaskScheduler final {
public:
    explicit TaskScheduler(TaskClock clock = &MonotonicClock::now) noexcept;
    ~TaskScheduler();   // @pre !running(); remaining tasks become kIdle (declare it after its tasks)
    copy/move deleted
    /// @pre !running(), not inside a pass. Claims the task (kIdle or finished → kScheduled/kSuspended),
    /// then clears its trigger and cancel requests. kInvalidArgument (negative period/offset/budget/
    /// deadline; period zero without triggered_only, or triggered_only with a period), kAlreadyExists
    /// (the task is in a runner), kResourceLimit (MaxTasks).
    Status add(Task& task, const TaskSchedule& schedule) noexcept;
    Status remove(Task& task) noexcept;          // @pre !running(), not inside a pass. kNotFound. → kIdle
    /// Starts the scheduler's own thread: aligns the grid at clock() and then, until stop(), sleeps
    /// until min(next_activation(), now + trigger_latency), runs a pass and beats heartbeat().
    /// kAlreadyRunning, kInvalidArgument (trigger_latency <= 0), or an RtThread::start() error.
    Status start(const ThreadConfig& config, Duration trigger_latency = std::chrono::milliseconds{1}) noexcept;
    /// Non-RT: ends after the current pass and joins; applies pending suspension requests. kNotRunning.
    Status stop() noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const Heartbeat& heartbeat() const noexcept;    // parked while not running
    /// For a hand-written loop or tests (only while !running()): sets the grid origin.
    void align(TimePoint origin) noexcept;
    /// One pass at now (only while !running(), or by the own thread). @return number of runs (<= MaxTasks).
    std::size_t run_once(TimePoint now) noexcept;
    [[nodiscard]] TimePoint next_activation() const noexcept;   // earliest periodic activation; TimePoint::max() if none
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] static constexpr std::size_t capacity() noexcept;
};
```
- Ordering: entries are kept ordered by (priority descending, add order) at `add()` (O(MaxTasks)
  insertion, init phase); `remove()` keeps the order; a pass walks them once and never sorts.
- A pass at `now`: (1) apply suspension requests (kScheduled ⇄ kSuspended; a resumed task's next =
  first grid point ≥ now, triggers taken while suspended are dropped); (2) for each entry in order,
  re-check its suspension request immediately before deciding (so a task suspended by an earlier
  task in the same pass does not run), then it runs if periodically due (`now >= next`) or if
  `take_trigger()` succeeds; `RunnerScope` and kRunning around `execute`, then kScheduled — or
  kSuspended if a suspension was requested during its run. Triggers and resumes during a pass take
  effect at a later pass. A trigger during the task's own run gives one more run at a later pass.
- Activation arithmetic (all int64 ns, saturating; a saturated next is never due): each periodic
  task keeps `next`, its first grid point not yet released. `align(origin)`: next = origin + offset
  (repeating it realigns, counts nothing missed, keeps statistics and pending triggers). A pass with
  now ≥ next: k = (now − next) / period (one division); release = next + k·period; missed = k
  (`TaskActivation::missed`, and added to `missed_activations`); next = release + period; deadline =
  release + (deadline > 0 ? deadline : period). A triggered run of a task that is not periodically
  due: release = now, missed = 0, deadline = now + deadline (never if zero); next unchanged. `add()`
  after an alignment and resume: next = the first point origin + offset + j·period ≥ t, t = the now
  of the last align()/pass. Activations before add() or while suspended are not missed. Unlike
  `PeriodicTimer`, the latest overdue point is run late, not skipped.
- Calls from `run()`: `Task::trigger/suspend/resume/cancel` of any task are allowed; `add`, `remove`,
  `align`, `run_once`, `start`, `stop` are asserted (`RunnerScope`).
- `priority` never preempts: rate groups that must preempt each other (1 ms over 100 ms) use one
  TaskScheduler per thread with rate-monotonic `ThreadConfig` priorities. Say so in the docs.
- RT-path safe: `run_once`, `next_activation`, `size`, `heartbeat`, the task requests. `add`, `remove`,
  `start`, `stop`: init/non-RT, one control thread. Give it static storage duration.
- `src/task/task_scheduler.cpp`: the non-template parts in `common::detail::task_scheduler`
  (activation arithmetic, running one entry, the thread loop body if practical).
- Tests (fake clock for everything deterministic): offsets/periods; priority order and add-order
  ties; the stall case (period 10 ms, stall from 10 to run_once(35): release 30, missed 2, lateness ≥ 5
  ms, deadline 40, next 40); overrun and deadline miss (a task advancing the fake clock in run());
  triggered-only tasks, coalescing, trigger from another std::thread (TSan); trigger during own run;
  suspend/resume (requests applied at pass start; suspension of a later task in the same pass;
  self-suspend from run(); no missed activations while suspended); add after align; remove; asserts
  are not tested (they abort); next_activation; capacity; invalid schedules; `start()`/`stop()` with a
  real thread, `wait_until_state()` after a suspend from another thread, start again after stop.

### 2.8 `task/task_executor.hpp` + `src/task/task_executor.cpp` — `TaskExecutor`

```cpp
template <std::size_t Workers, std::size_t QueueCapacity>
class TaskExecutor final {
public:
    explicit TaskExecutor(TaskClock clock = &MonotonicClock::now) noexcept;
    ~TaskExecutor();   // @pre !running(); cancels jobs still queued (declare it after its tasks)
    copy/move deleted
    /// Starts Workers RtThreads (all with config). kAlreadyRunning, kInvalidArgument (idle_sleep <= 0),
    /// or an RtThread::start() error (the workers already started are stopped again). Allowed after stop().
    Status start(const ThreadConfig& config, Duration idle_sleep = std::chrono::milliseconds{1}) noexcept;
    /// Any thread, lock-free, bounded. kOk; kAlreadyExists (the task is in a runner); kResourceLimit
    /// (queue full or too contended, or the claim exhausted its attempts); kNotRunning (stopping).
    /// Allowed before start(): run_pending() can run the jobs without workers.
    Status submit(Task& task, const Deadline& deadline = Deadline::never()) noexcept;
    /// Runs up to max_jobs queued jobs on the calling thread (also while workers run). @return jobs dequeued.
    std::size_t run_pending(std::size_t max_jobs) noexcept;
    /// Non-RT, control thread. kOk; kNotRunning (no workers were running; queued jobs are still handled
    /// per mode); kTimeout (submits still in flight after the bounded wait: stays stopping; call again).
    Status stop(StopMode mode = StopMode::kDrain) noexcept;
    void request_stop() noexcept;   // any thread, a job included: refuses new submits; stop() still joins
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const Heartbeat& heartbeat(std::size_t worker) const noexcept;   // @pre worker < Workers
};
```
- `submit()`: (1) `in_flight_.fetch_add(1, seq_cst)`; if `stopping_.load(seq_cst)`: decrement, return
  kNotRunning without touching the task. (2) Claim (`TaskAccess::claim` → kQueued); on failure
  decrement, return kAlreadyExists — a losing submit writes nothing to the task. (3) Only after the
  claim: store release = clock() and the deadline. (4) `try_push`; on failure store the claimed
  previous state back (release; the cancel bit stays cleared) and return kResourceLimit. (5)
  `in_flight_.fetch_sub(1, release)`. Between (2) and (4) other threads may see kQueued.
- Consumers (a worker, `run_pending()`, `stop()`'s cleanup) are the only writers of a queued job's
  state: pop; if the cancel bit is set → kCancelled without running; else kRunning, `RunnerScope`,
  `execute`, then kDone (kOk), kCancelled (error and cancel requested), kFailed (other error) — the
  final store is the last access to the task. A job is in the queue exactly while it is kQueued.
- `stop(mode)`: `stopping_.store(true, seq_cst)`; wait until `in_flight_.load(seq_cst) == 0`, sleeping
  `idle_sleep` per round, at most `kMaxStopWaitRounds` rounds (else kTimeout); then tell the workers
  to exit (a separate exit flag): kDrain — they take jobs until the queue is empty (with no producer
  in flight a failing `try_pop` means empty), then exit; kCancel — `cancel()` each running job; they
  exit after their current job; join; then every job still queued (at most QueueCapacity) becomes
  kCancelled. `start()` clears the flags. Without running workers (never started, or stopped already),
  `stop()` in either mode cancels the jobs still queued (kCancelled) and returns kNotRunning: it never
  runs jobs on the control thread. Calls from a job: `submit`, `request_stop` and task requests
  allowed; `start`, `stop`, `run_pending` asserted (`RunnerScope`; joining itself would deadlock).
- Worker service loop (until told to exit): pop and run a job, or `sleep_for(idle_sleep)`; beat its
  heartbeat each round (parked before start and after stop).
- RT-path safe: `submit`, `request_stop`, `running`, `heartbeat`. `run_pending`: as the jobs it runs.
  `start`/`stop`: one control thread. Give it static storage duration.
- `src/task/task_executor.cpp`: the non-template job logic in `common::detail::task_executor`.
- Tests: jobs complete on workers (`wait_until_finished`); failure; cancel before run (stays kQueued
  until dequeued, then kCancelled); cancel while running (run() sees it, ends kCancelled); resubmit
  after finish; double submit → kAlreadyExists; queue full → kResourceLimit and the task unchanged
  (idle again); run_pending without workers; stop(kDrain) runs a job submitted just before it;
  stop(kCancel) cancels queued and signals running; stop() without workers cancels queued jobs and
  returns kNotRunning; many producers × submit racing stop() (TSan; every task ends finished or was
  refused with kNotRunning); start again after stop; heartbeats parked.

### 2.9 `task/active_object.hpp` — `ActiveObject` (header-only)

```cpp
template <std::size_t QueueCapacity, std::size_t CommandCapacity = kInplaceFunctionCapacity>
class ActiveObject final {
public:
    using Command = InplaceFunction<void(), CommandCapacity>;
    /// Called on the object's thread at least every idle_sleep (and after each batch of commands).
    using Poll = void (*)(TimePoint now, void* context) noexcept;
    ActiveObject() noexcept;
    ~ActiveObject();   // @pre !running(); queued commands are destroyed unrun (counted in discarded())
    copy/move deleted
    /// kAlreadyRunning, kInvalidArgument (idle_sleep < 0), or an RtThread::start() error. idle_sleep == 0
    /// busy-polls with cpu_relax() (for a thread on an isolated core; it uses the whole core). Allowed
    /// after stop().
    Status start(const ThreadConfig& config, Duration idle_sleep = std::chrono::milliseconds{1},
                 Poll poll = nullptr, void* poll_context = nullptr) noexcept;
    /// Any thread (its own included), lock-free: queues a noexcept callable returning void, run on the
    /// object's thread one at a time in queue order. kOk; kResourceLimit (full or too contended);
    /// kNotRunning (stopping). Allowed before start(): run_pending() can then run the commands.
    template <typename F> Status post(F&& command) noexcept;
    /// Non-RT, control thread. kDrain: every command accepted before runs, then the thread exits;
    /// kCancel: queued commands are discarded. kOk, kNotRunning, kTimeout (as TaskExecutor::stop).
    Status stop(StopMode mode = StopMode::kDrain) noexcept;
    void request_stop() noexcept;   // any thread, a command included: refuses posts; the thread drains and
                                    // leaves its loop; stop() still joins (running() stays true until then)
    /// Runs up to max_commands queued commands on the calling thread. @pre !running()
    std::size_t run_pending(std::size_t max_commands) noexcept;
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const Heartbeat& heartbeat() const noexcept;      // beaten every loop; parked when not running
    [[nodiscard]] std::uint64_t executed() const noexcept;          // any thread
    [[nodiscard]] std::uint64_t discarded() const noexcept;         // any thread
};
```
- The same in-flight/stopping protocol as `TaskExecutor` (2.8): `post()` = fetch_add(seq_cst), check
  stopping (seq_cst), push, fetch_sub; `stop()` waits for in-flight posts (bounded) before the drain,
  so a `post()` that returned kOk always runs (kDrain) or is counted in `discarded()` (kCancel).
- One loop round: run at most QueueCapacity commands; call `poll(now)` if set; beat the heartbeat;
  sleep `idle_sleep` only if no command ran. Exit when told to (drain: queue empty and none in flight).
  Without a running thread, `stop()` in either mode discards the queued commands (counted) and returns
  kNotRunning: it never runs commands on the control thread.
- Calls from a command or the poll hook: `post`, `request_stop` allowed; `start`, `stop`,
  `run_pending` asserted (`RunnerScope`). Composition is the intended use: a class holds an
  `ActiveObject` and its public functions post lambdas that run on the object's thread, so its state
  needs no locks; the poll hook hosts the periodic work of that thread (e.g. a `TimerWheel::advance`).
- RT-path safe: `post`, `request_stop`, `running`, `heartbeat`, `executed`, `discarded`. Give it static
  storage duration. Queue: `MpmcQueue<Command, QueueCapacity>`.
- Tests: order and thread of execution; many posting threads (TSan); post from a command; queue full;
  stop(kDrain) runs everything accepted, post after stop → kNotRunning; stop(kCancel) discards and
  counts; posts racing stop() (TSan; every kOk post ran or was counted); request_stop from a command;
  run_pending without a thread; poll hook runs at least every idle_sleep and can advance a TimerWheel;
  idle_sleep == 0; restart after stop; heartbeat parked/advancing; capacity-limit commands.

### 2.10 `app/unit.hpp` + `src/app/unit.cpp` — `Unit`

```cpp
enum class UnitState : std::uint8_t {
    kCreated,      ///< Holds no resources: constructed, or shut down.
    kInitialized,  ///< Resources acquired, not running: initialised, or stopped.
    kRunning,      ///< Started.
};
[[nodiscard]] constexpr const char* to_string(UnitState state) noexcept;
inline constexpr std::size_t kMaxUnitDependencies{8U};
inline constexpr std::size_t kMaxUnitNameLength{31U};

class Unit {
public:
    /// For loaders that name units from configuration: 1..kMaxUnitNameLength characters.
    [[nodiscard]] static constexpr bool is_valid_name(std::string_view name) noexcept;
    [[nodiscard]] const char* name() const noexcept;                // the unit's own copy
    [[nodiscard]] UnitState state() const noexcept;                 // any thread (atomic)
    /// Init phase, @pre state() == kCreated (asserted). kInvalidArgument (itself), kAlreadyExists (twice),
    /// kResourceLimit (more than kMaxUnitDependencies).
    Status depends_on(Unit& dependency) noexcept;
    [[nodiscard]] std::size_t dependency_count() const noexcept;
    [[nodiscard]] const Unit& dependency(std::size_t index) const noexcept;   // @pre index < dependency_count()
    /// Lifecycle drivers (normally called by a UnitManager), one control thread:
    Status initialize() noexcept;  // kCreated → on_initialize() → kInitialized; other states: kInvalidArgument
    Status start() noexcept;       // kInitialized → on_start() → kRunning; kRunning: kAlreadyRunning; kCreated: kInvalidArgument
    void stop() noexcept;          // kRunning → on_stop() → kInitialized; otherwise nothing
    void shutdown() noexcept;      // stops first if running; kInitialized → on_shutdown() → kCreated
protected:
    explicit Unit(std::string_view name) noexcept;   // @pre is_valid_name(name) (asserted); copied (StaticString)
    ~Unit();     // @pre state() == kCreated (asserted)
    copy/move deleted
    virtual Status on_initialize() noexcept;   // acquire resources; on failure undo and return the error (stays kCreated)
    virtual Status on_start() noexcept;        // begin operating; on failure undo and return the error (stays kInitialized)
    virtual void on_stop() noexcept;           // must not fail
    virtual void on_shutdown() noexcept;       // must not fail
};
```
- A driver called from a hook of the same unit is asserted (an in-hook flag). A hook that fails must
  leave the unit as it found it.
- "Owning parts" — a documentation section with an `@code` example in `unit.hpp` (the recipe that
  composes units with the other patterns): `on_initialize()`: add the unit's tasks to their scheduler
  with `TaskSchedule::suspended = true`, connect slots (keep `ScopedConnection` members), add the
  heartbeats of its runners (parked) to the supervisor's `Watchdog`. `on_start()`: start its runners,
  then `resume()` its tasks. `on_stop()`: `suspend()` its tasks and `wait_until_state(task,
  kSuspended, deadline)`, then stop its runners. `on_shutdown()`: `remove()` its tasks and `reset()`
  its `ScopedConnection`s (every unit's threads have stopped by then). A unit owning a runner that
  other units use (scheduler, executor, supervisor) is a dependency of those units. Composition over
  multiple inheritance (deriving from both `Unit` and `Task` makes `name()` ambiguous).
- Tests: lifecycle transitions and wrong-state Status codes; hook failure leaves the state; dependency
  bookkeeping; `is_valid_name`; names copied from a reused buffer.

### 2.11 `app/unit_manager.hpp` + `src/app/unit_manager.cpp` — `UnitManager`

```cpp
template <std::size_t MaxUnits>
class UnitManager final {
public:
    UnitManager() noexcept;
    ~UnitManager();    // shutdown(): declare it after its units
    copy/move deleted
    /// Init phase. kAlreadyExists (the same unit, or one of the same name), kResourceLimit,
    /// kInvalidArgument (called after initialize()).
    Status add(Unit& unit) noexcept;
    /// The checks of initialize() without calling any hook: every dependency added (kNotFound), no cycle
    /// (kInvalidArgument), every unit kCreated (kInvalidArgument). failed_unit() names the offender.
    [[nodiscard]] Status validate() noexcept;
    /// validate(), then the deterministic dependency order (Kahn; among ready units the earliest added),
    /// then initialise in that order; on a failure, the units this call initialised are shut down in
    /// reverse order and the error is returned; failed_unit() names the unit.
    Status initialize() noexcept;
    /// Every unit must be kInitialized (kAlreadyRunning if one runs, kInvalidArgument if one is
    /// kCreated), checked before any hook; starts in order; on a failure stops exactly the units this
    /// call started, in reverse order. A single stopped unit is restarted with Unit::start().
    Status start() noexcept;
    void stop() noexcept;          // running units, reverse order
    void shutdown() noexcept;      // stop(), then shuts down the initialised units in reverse order
    void clear() noexcept;         // forgets every unit; @pre every unit kCreated
    [[nodiscard]] Unit* find(std::string_view name) noexcept;
    [[nodiscard]] const Unit* find(std::string_view name) const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] Unit& operator[](std::size_t index) noexcept;        // lifecycle order once ordered, else add() order
    [[nodiscard]] const Unit& operator[](std::size_t index) const noexcept;
    [[nodiscard]] const Unit* failed_unit() const noexcept;           // of the last validate/initialize/start
    [[nodiscard]] static constexpr std::size_t capacity() noexcept;
};
```
- `src/app/unit_manager.cpp`: the non-template algorithms over `Span`s in
  `common::detail::unit_manager` (order O(n²) bounded by MaxUnits, initialise/start with rollback,
  stop/shutdown in reverse). Manager functions called from a unit hook are asserted.
- Init/shutdown phase; one control thread.
- Tests: order from dependencies (chains, diamonds, independent units keep add order); cycle and
  missing dependency (validate and initialize, no hook called, failed_unit set); rollback on initialise
  and on start failure, with hook logs; start with a running unit → kAlreadyRunning; stop/shutdown
  reverse order; re-initialise after shutdown; clear(); find; duplicate names; capacity; destructor
  shuts down.

### 2.12 Integration test and example (after every component)

- `tests/app/test_application.cpp`: the patterns together, with real threads but no timing-based
  verdicts. ECU-like: a runtime unit owns a `TaskScheduler` (started in its on_start), a control
  unit's tasks follow the "Owning parts" recipe; a test proves a unit's task never runs outside
  [on_start, on_stop); a mode-manager `StateMachine` driven through an `ActiveObject`; supervision
  with a `Watchdog` over parked/live heartbeats. Trading-like: order state machines sharing one
  `Definition` with fill data, driven by an `ActiveObject` with a `TimerWheel` in its poll hook;
  instrument `TypeObject`s from text in a reused buffer; units created from a configuration table
  through `ObjectTypeRegistry::create_as` with arguments (two gateway instances with different names);
  a final persistence job submitted before `TaskExecutor::stop(kDrain)` runs.
- `examples/unit_application.cpp` (links `common::common`): a small application built the same way,
  printing what happens and the task statistics; `./unit_application [milliseconds]`.

## 3. Shared files (owned by the integrator)

`CMakeLists.txt`, `tests/CMakeLists.txt`, `examples/CMakeLists.txt`, `include/common/base.hpp`,
`include/common/common.hpp`, `README.md`, `CLAUDE.md`. Implementers do not edit them in the shared tree;
if you need a change (e.g. an extra source file), make it in your private copy and report it.
Library sources of `common` (already in CMake): `src/task/task.cpp`, `src/task/task_scheduler.cpp`,
`src/task/task_executor.cpp`, `src/app/unit.cpp`, `src/app/unit_manager.cpp`.
