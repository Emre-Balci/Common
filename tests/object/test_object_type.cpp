#include <array>
#include <cstdint>
#include <string_view>

#include "common/containers/object_pool.hpp"
#include "common/core/status.hpp"
#include "common/object/object_type.hpp"
#include "test_framework.hpp"

namespace {

// A class hierarchy: Vehicle (intermediate) -> Truck, Car (leaves).
class Vehicle : public common::Object {
public:
    static constexpr common::ObjectTypeOf<Vehicle, common::Object> kType{"Vehicle"};
    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return kType;
    }

protected:
    Vehicle() noexcept = default;
    ~Vehicle() = default;
    Vehicle(const Vehicle&) noexcept = default;
    Vehicle(Vehicle&&) noexcept = default;
    Vehicle& operator=(const Vehicle&) noexcept = default;
    Vehicle& operator=(Vehicle&&) noexcept = default;
};

class Truck final : public Vehicle {
public:
    static constexpr common::ObjectTypeOf<Truck, Vehicle> kType{"Truck"};
    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return kType;
    }
};

class Car final : public Vehicle {
public:
    static constexpr common::ObjectTypeOf<Car, Vehicle> kType{"Car"};
    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return kType;
    }
};

// Two unrelated classes with the same type name and no id: never the same type.
namespace plant {
class Sensor final : public common::Object {
public:
    static constexpr common::ObjectTypeOf<Sensor, common::Object> kType{"Sensor"};
    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return kType;
    }
};
}  // namespace plant

namespace vehicle {
class Sensor final : public common::Object {
public:
    static constexpr common::ObjectTypeOf<Sensor, common::Object> kType{"Sensor"};
    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return kType;
    }
};
}  // namespace vehicle

// Types defined by data: instrument types with properties, under the class type Instrument.
struct InstrumentProperties final {
    std::int64_t tick_size{1};
    std::int32_t lot_size{1};
};
using InstrumentType = common::TypeObject<InstrumentProperties>;

class Instrument final : public common::Object {
public:
    static constexpr common::ObjectTypeOf<Instrument, common::Object> kType{"Instrument"};

    explicit Instrument(const InstrumentType& type) noexcept : type_{&type} {}

    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return *type_;
    }
    [[nodiscard]] const InstrumentType& instrument_type() const noexcept {
        return *type_;
    }

private:
    const InstrumentType* type_;
};

constexpr InstrumentType kEquity{"Equity", InstrumentProperties{1, 100}, Instrument::kType};
constexpr InstrumentType kDerivative{"Derivative", InstrumentProperties{5, 1}, Instrument::kType};
constexpr InstrumentType kOption{"Option", InstrumentProperties{5, 100}, kDerivative};

// Compile-time checks: the hierarchy is constant, and so is is_a().
static_assert(Truck::kType.is_a(Vehicle::kType), "");
static_assert(Truck::kType.is_a(common::Object::kType), "");
static_assert(Truck::kType.is_a(Truck::kType), "");
static_assert(!Vehicle::kType.is_a(Truck::kType), "");
static_assert(!Truck::kType.is_a(Car::kType), "");
static_assert(Truck::kType.depth() == 2U, "");
static_assert(Truck::kType.parent() == &Vehicle::kType, "");
static_assert(common::Object::kType.depth() == 0U, "");
static_assert(common::Object::kType.id() == common::kObjectRootTypeId, "");
static_assert(!plant::Sensor::kType.same_as(vehicle::Sensor::kType), "a name alone is not an identity");
static_assert(!plant::Sensor::kType.is_a(vehicle::Sensor::kType), "");
static_assert(kOption.is_a(kDerivative), "");
static_assert(kOption.is_a(Instrument::kType), "");
static_assert(!kOption.is_a(kEquity), "");
static_assert(kOption.parent_type() == &kDerivative, "");
static_assert(kDerivative.parent_type() == nullptr, "under a class type");
static_assert(kOption.properties().lot_size == 100, "");

}  // namespace

TEST_CASE(object_type_identity_and_ids) {
    // Two descriptors with the same non-zero id and name are one type (a copy in another module).
    static constexpr common::ObjectType kOrder{"Order", nullptr, 42U};
    static constexpr common::ObjectType kOrderCopy{"Order", nullptr, 42U};
    CHECK(kOrder.same_as(kOrderCopy));
    CHECK(kOrderCopy.is_a(kOrder));

    // Without ids, only the object itself.
    static constexpr common::ObjectType kQuote{"Quote", nullptr};
    static constexpr common::ObjectType kQuoteCopy{"Quote", nullptr};
    CHECK(kQuote.same_as(kQuote));
    CHECK(!kQuote.same_as(kQuoteCopy));

    // Same id, different names: different types.
    static constexpr common::ObjectType kFill{"Fill", nullptr, 42U};
    CHECK(!kOrder.same_as(kFill));
    CHECK(kOrder.id() == 42U);
    CHECK(std::string_view{kOrder.name()} == "Order");
}

TEST_CASE(object_type_is_a_and_depth) {
    static constexpr common::ObjectType kMessage{"Message", nullptr};
    static constexpr common::ObjectType kOrderMessage{"OrderMessage", &kMessage};
    static constexpr common::ObjectType kNewOrder{"NewOrder", &kOrderMessage};
    static constexpr common::ObjectType kHeartbeat{"Heartbeat", &kMessage};
    CHECK(kNewOrder.depth() == 2U);
    CHECK(kNewOrder.is_a(kMessage));
    CHECK(kNewOrder.is_a(kOrderMessage));
    CHECK(!kNewOrder.is_a(kHeartbeat));
    CHECK(!kMessage.is_a(kNewOrder));
    CHECK(!kHeartbeat.is_a(kOrderMessage));
}

TEST_CASE(object_type_object_cast) {
    Truck truck{};
    Car car{};
    common::Object& as_object{truck};
    CHECK(as_object.is_a(Vehicle::kType));
    CHECK(as_object.is_a(common::Object::kType));
    CHECK(!as_object.is_a(Car::kType));

    CHECK(common::object_cast<Truck>(&as_object) == &truck);
    CHECK(common::object_cast<Vehicle>(&as_object) == &truck);
    CHECK(common::object_cast<Car>(&as_object) == nullptr);
    CHECK(common::object_cast<Car>(static_cast<common::Object*>(&car)) == &car);

    const common::Object& as_const{car};
    const Car* const const_car{common::object_cast<Car>(&as_const)};
    CHECK(const_car == &car);
    CHECK(common::object_cast<Truck>(&as_const) == nullptr);

    common::Object* const none{nullptr};
    CHECK(common::object_cast<Truck>(none) == nullptr);
}

TEST_CASE(object_type_same_names_do_not_cast) {
    vehicle::Sensor vehicle_sensor{};
    plant::Sensor plant_sensor{};
    CHECK(common::object_cast<plant::Sensor>(static_cast<common::Object*>(&vehicle_sensor)) == nullptr);
    CHECK(common::object_cast<vehicle::Sensor>(static_cast<common::Object*>(&plant_sensor)) == nullptr);
    CHECK(common::object_cast<vehicle::Sensor>(static_cast<common::Object*>(&vehicle_sensor)) == &vehicle_sensor);
}

TEST_CASE(object_type_type_objects_with_properties) {
    const Instrument option{kOption};
    const Instrument equity{kEquity};
    CHECK(option.is_a(kDerivative));
    CHECK(option.is_a(Instrument::kType));
    CHECK(!equity.is_a(kDerivative));
    // The class cast works for every instrument type, because the types sit under Instrument::kType.
    CHECK(common::object_cast<Instrument>(static_cast<const common::Object*>(&option)) == &option);
    CHECK(option.instrument_type().properties().tick_size == 5);
    CHECK(std::string_view{option.object_type().name()} == "Option");
}

TEST_CASE(object_type_check_object_type) {
    CHECK(common::check_object_type("Equity", nullptr) == common::Status::kOk);
    CHECK(common::check_object_type("", nullptr) == common::Status::kInvalidArgument);
    const std::array<char, 3U> with_nul{{'a', '\0', 'b'}};
    CHECK(common::check_object_type(std::string_view{with_nul.data(), with_nul.size()}, nullptr) ==
          common::Status::kInvalidArgument);
    const std::string_view longest{"0123456789012345678901234567890123456789012345678901234567890123"};
    CHECK(common::check_object_type(longest.substr(0U, common::kMaxObjectTypeNameLength), nullptr) ==
          common::Status::kOk);
    CHECK(common::check_object_type(longest, nullptr) == common::Status::kOutOfRange);

    // A chain of kMaxObjectTypeDepth + 1 types: the last one cannot have a child.
    // (A string_view, not a literal: libc++'s string_view(const char*) is not noexcept, which
    // ObjectPool::create requires of the construction.)
    static common::ObjectPool<InstrumentType, common::kMaxObjectTypeDepth + 1U> chain{};
    const std::string_view level_name{"Level"};
    const InstrumentType* parent{chain.create(level_name, InstrumentProperties{}, Instrument::kType)};
    REQUIRE(parent != nullptr);
    // Instrument::kType is at depth 1, so the first level is at depth 2.
    for (std::size_t step{0U}; (step < common::kMaxObjectTypeDepth) && (parent->depth() < common::kMaxObjectTypeDepth);
         ++step) {
        REQUIRE(common::check_object_type(level_name, parent) == common::Status::kOk);
        parent = chain.create(level_name, InstrumentProperties{}, *parent);
        REQUIRE(parent != nullptr);
    }
    CHECK(parent->depth() == common::kMaxObjectTypeDepth);
    CHECK(common::check_object_type(level_name, parent) == common::Status::kOutOfRange);
}

TEST_CASE(object_type_type_objects_from_a_reused_buffer) {
    // A loader reads names into one buffer it reuses; the types keep their own copies.
    static common::ObjectPool<InstrumentType, 4U> types{};
    std::array<char, 16U> buffer{};
    const std::array<std::string_view, 3U> lines{{"Bond", "Future", "Swap"}};
    std::array<const InstrumentType*, 3U> created{};
    for (std::size_t i{0U}; i < lines.size(); ++i) {
        buffer.fill('\0');
        for (std::size_t c{0U}; c < lines[i].size(); ++c) {
            buffer[c] = lines[i][c];
        }
        const std::string_view name{buffer.data(), lines[i].size()};
        REQUIRE(common::check_object_type(name, &Instrument::kType) == common::Status::kOk);
        created[i] = types.create(name, InstrumentProperties{static_cast<std::int64_t>(i), 1}, Instrument::kType);
        REQUIRE(created[i] != nullptr);
    }
    buffer.fill('x');
    CHECK(std::string_view{created[0U]->name()} == "Bond");
    CHECK(std::string_view{created[1U]->name()} == "Future");
    CHECK(std::string_view{created[2U]->name()} == "Swap");
    CHECK(created[2U]->properties().tick_size == 2);
    CHECK(created[1U]->is_a(Instrument::kType));
}
