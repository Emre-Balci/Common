/// @file unit_application.cpp
/// @brief An application built from the patterns of common:
///
///   - vehicle types defined by data (TypeObject) and chosen by name from a configuration
///     (ObjectTypeRegistry);
///   - units with lifecycles and dependencies (Unit, UnitManager): the runtime owns a TaskScheduler
///     with its own thread; the sensor owns a periodic task that publishes the speed through a Signal;
///     the control unit turns the speed into operating modes with a StateMachine; the logger prints
///     on the thread of an ActiveObject, off the real-time thread; the storage unit writes a summary
///     at shutdown as a job of a TaskExecutor;
///   - every lambda that crosses a thread is an InplaceFunction: nothing allocates after start-up.
///
/// Usage: unit_application [milliseconds] [vehicle]   (defaults: 500 truck; vehicles: car, truck)

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "common/common.hpp"

namespace {

using namespace std::chrono_literals;

common::ThreadConfig thread_config(const char* const name) noexcept {
    // SCHED_OTHER, so that the example runs without privileges; a deployment would give the runtime
    // SCHED_FIFO and a CPU of its own.
    common::ThreadConfig config{};
    config.name = name;
    config.policy = common::SchedPolicy::kOther;
    config.priority = 0;
    return config;
}

// ---- Vehicle types: defined by data (the Type Object pattern) --------------------------------

struct VehicleProperties final {
    double speed_limit_kmh{0.0};
    double acceleration_kmh_per_s{0.0};
};

using VehicleType = common::TypeObject<VehicleProperties>;

// Fast accelerations, so that a short run shows the modes change.
const VehicleType kCar{"car", VehicleProperties{130.0, 300.0}};
const VehicleType kTruck{"truck", VehicleProperties{90.0, 150.0}};

// ---- Logger: an active object does the printing ---------------------------------------------

class LoggerUnit final : public common::Unit {
public:
    LoggerUnit() noexcept : Unit{"logger"} {}

    /// Any thread, never blocks: the line is printed on the logger's thread.
    void mode_changed(const char* const mode, const double speed_kmh) noexcept {
        static_cast<void>(active_.post([mode, speed_kmh]() noexcept {
            std::printf("  mode %-8s at %6.1f km/h\n", mode, speed_kmh);
        }));
    }

private:
    common::Status on_start() noexcept override {
        return active_.start(thread_config("logger"));
    }
    void on_stop() noexcept override {
        static_cast<void>(active_.stop(common::StopMode::kDrain));  // Prints what is still queued.
    }

    common::ActiveObject<64U> active_{};
};

// ---- Runtime: the scheduler and its thread ------------------------------------------------

class RuntimeUnit final : public common::Unit {
public:
    RuntimeUnit() noexcept : Unit{"runtime"} {}

    [[nodiscard]] common::TaskScheduler<4U>& scheduler() noexcept {
        return scheduler_;
    }

private:
    common::Status on_start() noexcept override {
        return scheduler_.start(thread_config("runtime"), 1ms);
    }
    void on_stop() noexcept override {
        static_cast<void>(scheduler_.stop());
    }

    common::TaskScheduler<4U> scheduler_{};
};

// ---- Sensor: a periodic task publishes the speed --------------------------------------------

class SpeedTask final : public common::Task {
public:
    SpeedTask() noexcept : Task{"speed"} {}

    /// Before initialisation: the vehicle simulated, starting at 70% of its speed limit.
    void use(const VehicleType& vehicle) noexcept {
        vehicle_ = &vehicle;
        speed_kmh_ = 0.7 * vehicle.properties().speed_limit_kmh;
    }

    common::Signal<void(double), 4U> speed_changed{};  // Notified on the runtime's thread.

private:
    // Accelerates to 120% of the limit, then brakes to half of it, and so on.
    common::Status run(const common::TaskActivation& /*activation*/) noexcept override {
        const VehicleProperties& properties{vehicle_->properties()};
        const double step_kmh{properties.acceleration_kmh_per_s * 0.01};  // A 10 ms period.
        if (speed_kmh_ > (1.2 * properties.speed_limit_kmh)) {
            braking_ = true;
        } else if (speed_kmh_ < (0.5 * properties.speed_limit_kmh)) {
            braking_ = false;
        } else {
            // Keep the direction.
        }
        speed_kmh_ += braking_ ? (-2.0 * step_kmh) : step_kmh;
        speed_changed.notify(speed_kmh_);
        return common::Status::kOk;
    }

    const VehicleType* vehicle_{&kCar};
    double speed_kmh_{0.0};
    bool braking_{false};
};

class SensorUnit final : public common::Unit {
public:
    explicit SensorUnit(RuntimeUnit& runtime) noexcept : Unit{"sensor"}, runtime_{&runtime} {}

    [[nodiscard]] SpeedTask& task() noexcept {
        return task_;
    }

private:
    // "Owning parts" (unit.hpp): the task runs only between on_start() and on_stop().
    common::Status on_initialize() noexcept override {
        common::TaskSchedule schedule{common::TaskSchedule::periodic(10ms)};
        schedule.budget = 1ms;
        schedule.suspended = true;
        return runtime_->scheduler().add(task_, schedule);
    }
    common::Status on_start() noexcept override {
        task_.resume();
        return common::Status::kOk;
    }
    void on_stop() noexcept override {
        task_.suspend();
        static_cast<void>(common::wait_until_state(task_, common::TaskState::kSuspended, common::Deadline::after(1s)));
    }
    void on_shutdown() noexcept override {
        static_cast<void>(runtime_->scheduler().remove(task_));
    }

    RuntimeUnit* runtime_;
    SpeedTask task_{};
};

// ---- Control: operating modes from the speed --------------------------------------------------

enum class Mode : std::uint8_t { kNormal, kLimited };
enum class ModeEvent : std::uint8_t { kOverLimit, kBelowLimit };

struct ControlContext final {
    LoggerUnit* logger{nullptr};
    double speed_kmh{0.0};
    std::uint32_t limitations{0U};
};

using ModeMachine = common::StateMachine<Mode, ModeEvent, ControlContext>;

void enter_limited(ControlContext& context) noexcept {
    ++context.limitations;
    context.logger->mode_changed("limited", context.speed_kmh);
}

void enter_normal(ControlContext& context) noexcept {
    context.logger->mode_changed("normal", context.speed_kmh);
}

constexpr std::array<ModeMachine::Transition, 2U> kModeTransitions{{
    {Mode::kNormal, ModeEvent::kOverLimit, Mode::kLimited},
    {Mode::kLimited, ModeEvent::kBelowLimit, Mode::kNormal},
}};
constexpr std::array<ModeMachine::StateActions, 2U> kModeActions{{
    {Mode::kLimited, &enter_limited},
    {Mode::kNormal, &enter_normal},
}};

class ControlUnit final : public common::Unit {
public:
    ControlUnit(SensorUnit& sensor, LoggerUnit& logger) noexcept : Unit{"control"}, sensor_{&sensor} {
        context_.logger = &logger;
    }

    /// Before initialisation: the vehicle controlled (the sensor starts it at 70% of its limit).
    void use(const VehicleType& vehicle) noexcept {
        limit_kmh_ = vehicle.properties().speed_limit_kmh;
        context_.speed_kmh = 0.7 * limit_kmh_;
    }

    /// After shutdown: how often the speed was limited.
    [[nodiscard]] std::uint32_t limitations() const noexcept {
        return context_.limitations;
    }

private:
    common::Status on_initialize() noexcept override {
        // Started here, before the sensor's task runs: from then on only the runtime's thread (through
        // the slot) touches the machine, until shutdown.
        machine_.start();
        const common::Result<common::Connection> connection{sensor_->task().speed_changed.connect(
            [this](const double speed_kmh) noexcept { on_speed(speed_kmh); })};
        if (connection.has_value()) {
            connection_ = common::ScopedConnection{sensor_->task().speed_changed, connection.value()};
        } else {
            machine_.reset();
        }
        return connection.status();
    }
    void on_shutdown() noexcept override {
        connection_.reset();
        machine_.reset();
    }

    void on_speed(const double speed_kmh) noexcept {
        context_.speed_kmh = speed_kmh;
        static_cast<void>(machine_.dispatch((speed_kmh > limit_kmh_) ? ModeEvent::kOverLimit : ModeEvent::kBelowLimit));
    }

    SensorUnit* sensor_;
    double limit_kmh_{130.0};
    ControlContext context_{};
    const ModeMachine::Definition definition_{kModeTransitions, kModeActions};
    ModeMachine machine_{definition_, Mode::kNormal, context_};
    common::ScopedConnection connection_{};
};

// ---- Storage: a background job at shutdown ---------------------------------------------------

class SummaryJob final : public common::Task {
public:
    explicit SummaryJob(const SpeedTask& speed) noexcept : Task{"summary"}, speed_{&speed} {}

private:
    // Runs on the executor's worker: the I/O stays off the real-time thread.
    common::Status run(const common::TaskActivation& /*activation*/) noexcept override {
        common::TaskStatistics statistics{};
        common::Status status{common::Status::kTimeout};
        if (speed_->try_statistics(statistics)) {
            const double mean_us{(statistics.runs > 0U) ? (static_cast<double>(statistics.total_execution_ns) /
                                                           (1000.0 * static_cast<double>(statistics.runs)))
                                                        : 0.0};
            std::printf("speed task: %llu runs, mean %.1f us, max %.1f us, %llu overruns, %llu missed activations\n",
                        static_cast<unsigned long long>(statistics.runs), mean_us,
                        static_cast<double>(statistics.max_execution_ns) / 1000.0,
                        static_cast<unsigned long long>(statistics.overruns),
                        static_cast<unsigned long long>(statistics.missed_activations));
            status = common::Status::kOk;
        }
        return status;
    }

    const SpeedTask* speed_;
};

class StorageUnit final : public common::Unit {
public:
    explicit StorageUnit(SensorUnit& sensor) noexcept : Unit{"storage"}, summary_{sensor.task()} {}

private:
    common::Status on_start() noexcept override {
        return background_.start(thread_config("storage"));
    }
    void on_stop() noexcept override {
        // Submitted just before the stop; kDrain runs it before the worker ends.
        static_cast<void>(background_.submit(summary_));
        static_cast<void>(background_.stop(common::StopMode::kDrain));
    }

    SummaryJob summary_;
    common::TaskExecutor<1U, 4U> background_{};
};

// Static storage: the units, then the manager that brings them up and down.
LoggerUnit g_logger{};
RuntimeUnit g_runtime{};
SensorUnit g_sensor{g_runtime};
ControlUnit g_control{g_sensor, g_logger};
StorageUnit g_storage{g_sensor};
common::UnitManager<8U> g_units{};
common::ObjectTypeRegistry<4U, VehicleType> g_vehicle_types{};

}  // namespace

int main(const int argc, const char* const argv[]) {
    const long milliseconds{(argc > 1) ? std::strtol(argv[1], nullptr, 10) : 500L};
    const std::string_view vehicle_name{(argc > 2) ? std::string_view{argv[2]} : std::string_view{"truck"}};

    // ---- Configuration -------------------------------------------------------------------------
    static_cast<void>(g_vehicle_types.add(kCar));
    static_cast<void>(g_vehicle_types.add(kTruck));
    const VehicleType* const vehicle{g_vehicle_types.find(vehicle_name)};
    if (vehicle == nullptr) {
        std::printf("unknown vehicle type\n");
        return EXIT_FAILURE;
    }
    g_sensor.task().use(*vehicle);
    g_control.use(*vehicle);

    // ---- Wiring: each unit after the units it needs -----------------------------------------
    static_cast<void>(g_sensor.depends_on(g_runtime));
    static_cast<void>(g_control.depends_on(g_sensor));
    static_cast<void>(g_control.depends_on(g_logger));
    static_cast<void>(g_storage.depends_on(g_sensor));
    for (common::Unit* const unit : std::array<common::Unit*, 5U>{{&g_storage, &g_control, &g_sensor, &g_runtime, &g_logger}}) {
        static_cast<void>(g_units.add(*unit));
    }

    // ---- Up, run, down ------------------------------------------------------------------------
    std::printf("%s (limit %.0f km/h), %ld ms\n", vehicle->name(), vehicle->properties().speed_limit_kmh, milliseconds);
    common::Status status{g_units.initialize()};
    if (common::is_ok(status)) {
        status = g_units.start();
    }
    if (!common::is_ok(status)) {
        const common::Unit* const failed{g_units.failed_unit()};
        std::printf("start failed: %s (unit %s)\n", common::to_string(status), (failed != nullptr) ? failed->name() : "-");
        g_units.shutdown();
        return EXIT_FAILURE;
    }
    // One printf per line: the logger's thread prints at the same time.
    common::StaticString<128U> order{};
    for (std::size_t i{0U}; i < g_units.size(); ++i) {
        static_cast<void>(order.append(' '));
        static_cast<void>(order.append(std::string_view{g_units[i].name()}));
    }
    std::printf("units started in this order:%s\n", order.c_str());

    static_cast<void>(common::sleep_for(std::chrono::milliseconds{(milliseconds > 0L) ? milliseconds : 0L}));

    g_units.stop();  // Reverse order: storage writes the summary, the sensor's task stops, ...
    g_units.shutdown();
    std::printf("speed limited %u times; every unit shut down\n", g_control.limitations());
    return EXIT_SUCCESS;
}
