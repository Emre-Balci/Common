#include <cstdint>
#include <string_view>

#include "common/containers/object_pool.hpp"
#include "common/core/status.hpp"
#include "common/object/object_type.hpp"
#include "common/object/object_type_registry.hpp"
#include "test_framework.hpp"

namespace {

// Configuration-created objects: gateways that take per-instance arguments.
struct GatewayArguments final {
    const char* name{""};
    std::uint32_t bitrate{0U};
};

class Gateway final : public common::Object {
public:
    static constexpr common::ObjectTypeOf<Gateway, common::Object> kType{"Gateway", 7U};

    Gateway(const common::ObjectType& type, const void* const arguments) noexcept : type_{&type} {
        if (arguments != nullptr) {
            const GatewayArguments* const typed{static_cast<const GatewayArguments*>(arguments)};
            name_ = typed->name;
            bitrate_ = typed->bitrate;
        }
    }

    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return *type_;
    }
    [[nodiscard]] std::string_view name() const noexcept {
        return name_;
    }
    [[nodiscard]] std::uint32_t bitrate() const noexcept {
        return bitrate_;
    }

private:
    const common::ObjectType* type_;
    const char* name_{""};
    std::uint32_t bitrate_{0U};
};

// A default-constructible class.
class Logger final : public common::Object {
public:
    static constexpr common::ObjectTypeOf<Logger, common::Object> kType{"Logger"};
    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return kType;
    }
};

// Data-defined types created by one factory.
struct InstrumentProperties final {
    std::int32_t lot_size{1};
};
using InstrumentType = common::TypeObject<InstrumentProperties>;

class Instrument final : public common::Object {
public:
    static constexpr common::ObjectTypeOf<Instrument, common::Object> kType{"Instrument"};
    Instrument(const common::ObjectType& type, const void* /*arguments*/) noexcept : type_{&type} {}
    [[nodiscard]] const common::ObjectType& object_type() const noexcept override {
        return *type_;
    }

private:
    const common::ObjectType* type_;
};

}  // namespace

TEST_CASE(object_type_registry_add_and_find) {
    common::ObjectTypeRegistry<3U> registry{};
    CHECK(registry.capacity() == 3U);
    CHECK(registry.add(Gateway::kType) == common::Status::kOk);
    CHECK(registry.add(Logger::kType) == common::Status::kOk);
    CHECK(registry.size() == 2U);
    CHECK(&registry[0U] == &Gateway::kType);

    CHECK(registry.find("Gateway") == &Gateway::kType);
    CHECK(registry.find("Logger") == &Logger::kType);
    CHECK(registry.find("Log") == nullptr);
    CHECK(registry.find("LoggerX") == nullptr);
    CHECK(registry.find("") == nullptr);
    CHECK(registry.find_by_id(7U) == &Gateway::kType);
    CHECK(registry.find_by_id(0U) == nullptr);  // Logger has id 0, which never matches.
    CHECK(registry.find_by_id(8U) == nullptr);
    CHECK(registry.contains(Logger::kType));
    CHECK(!registry.contains(Instrument::kType));
}

TEST_CASE(object_type_registry_rejects_duplicates) {
    common::ObjectTypeRegistry<4U> registry{};
    REQUIRE(registry.add(Gateway::kType) == common::Status::kOk);
    CHECK(registry.add(Gateway::kType) == common::Status::kAlreadyExists);
    static constexpr common::ObjectType kSameName{"Gateway", nullptr};
    CHECK(registry.add(kSameName) == common::Status::kAlreadyExists);
    static constexpr common::ObjectType kSameId{"Other", nullptr, 7U};
    CHECK(registry.add(kSameId) == common::Status::kAlreadyExists);
    CHECK(registry.size() == 1U);

    common::ObjectTypeRegistry<1U> full{};
    REQUIRE(full.add(Logger::kType) == common::Status::kOk);
    CHECK(full.add(Gateway::kType) == common::Status::kResourceLimit);

    common::ObjectPool<Logger, 1U> pool{};
    common::ObjectFactory half{common::pool_factory(pool)};
    half.destroy = nullptr;
    CHECK(registry.add(Logger::kType, half) == common::Status::kInvalidArgument);
}

TEST_CASE(object_type_registry_creates_with_arguments) {
    static common::ObjectPool<Gateway, 2U> gateways{};
    common::ObjectTypeRegistry<4U> registry{};
    REQUIRE(registry.add(Gateway::kType, common::pool_factory(gateways)) == common::Status::kOk);

    const GatewayArguments can0{"can0", 500'000U};
    const GatewayArguments can1{"can1", 250'000U};
    const common::Result<Gateway*> first{registry.create_as<Gateway>("Gateway", &can0)};
    const common::Result<Gateway*> second{registry.create_as<Gateway>("Gateway", &can1)};
    REQUIRE(first.has_value() && second.has_value());
    CHECK(first.value()->name() == "can0");
    CHECK(second.value()->bitrate() == 250'000U);
    CHECK(gateways.in_use() == 2U);

    // The pool is exhausted.
    CHECK(registry.create("Gateway", &can0).status() == common::Status::kResourceLimit);
    CHECK(registry.create("Unknown").status() == common::Status::kNotFound);

    CHECK(registry.destroy(*first.value()) == common::Status::kOk);
    CHECK(registry.destroy(*second.value()) == common::Status::kOk);
    CHECK(gateways.in_use() == 0U);
}

TEST_CASE(object_type_registry_factories_and_errors) {
    static common::ObjectPool<Logger, 2U> loggers{};  // Room for the object create_as destroys again.
    common::ObjectTypeRegistry<4U> registry{};
    REQUIRE(registry.add(Logger::kType, common::pool_factory(loggers)) == common::Status::kOk);
    static constexpr common::ObjectType kNoFactory{"NoFactory", &common::Object::kType};
    REQUIRE(registry.add(kNoFactory) == common::Status::kOk);
    CHECK(registry.create("NoFactory").status() == common::Status::kNotSupported);

    const common::Result<common::Object*> logger{registry.create("Logger")};  // Default-constructed.
    REQUIRE(logger.has_value());
    CHECK(logger.value()->is_a(Logger::kType));

    // create_as with the wrong class destroys the object again.
    CHECK(registry.create_as<Gateway>("Logger").status() == common::Status::kInvalidArgument);
    CHECK(loggers.in_use() == 1U);  // Only the first logger.
    CHECK(registry.destroy(*logger.value()) == common::Status::kOk);
    CHECK(loggers.in_use() == 0U);

    // An object of an unregistered type.
    static common::ObjectPool<Gateway, 1U> gateways{};
    Gateway* const unregistered{gateways.create(Gateway::kType, nullptr)};
    REQUIRE(unregistered != nullptr);
    CHECK(registry.destroy(*unregistered) == common::Status::kNotFound);
    gateways.destroy(unregistered);
}

TEST_CASE(object_type_registry_of_type_objects) {
    static constexpr InstrumentType kEquity{"Equity", InstrumentProperties{100}, Instrument::kType};
    static constexpr InstrumentType kFuture{"Future", InstrumentProperties{1}, Instrument::kType};
    static constexpr InstrumentType kIndexFuture{"IndexFuture", InstrumentProperties{10}, kFuture};
    static common::ObjectPool<Instrument, 4U> instruments{};

    // One factory serves every instrument type; find() returns the TypeObject with its properties.
    common::ObjectTypeRegistry<8U, InstrumentType> registry{};
    const common::ObjectFactory factory{common::pool_factory(instruments)};
    REQUIRE(registry.add(kEquity, factory) == common::Status::kOk);
    REQUIRE(registry.add(kFuture, factory) == common::Status::kOk);
    REQUIRE(registry.add(kIndexFuture, factory) == common::Status::kOk);
    const InstrumentType* const found{registry.find("IndexFuture")};
    REQUIRE(found != nullptr);
    CHECK(found->properties().lot_size == 10);
    CHECK(found->parent_type() == &kFuture);

    const common::Result<Instrument*> equity{registry.create_as<Instrument>("Equity")};
    const common::Result<Instrument*> index_future{registry.create_as<Instrument>("IndexFuture")};
    REQUIRE(equity.has_value() && index_future.has_value());
    CHECK(&equity.value()->object_type() == &kEquity);
    CHECK(index_future.value()->is_a(kFuture));
    CHECK(!equity.value()->is_a(kFuture));
    CHECK(registry.destroy(*equity.value()) == common::Status::kOk);
    CHECK(registry.destroy(*index_future.value()) == common::Status::kOk);
    CHECK(instruments.in_use() == 0U);
}
