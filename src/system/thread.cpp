#include "common/system/thread.hpp"

#include <cstddef>

#include "common/core/assert.hpp"

// The parts of thread.hpp that are the same on every platform; thread_posix.cpp and
// thread_win32.cpp implement the rest.

namespace common {

CpuSet CpuSet::single(const std::size_t cpu) noexcept {
    CpuSet set{};
    static_cast<void>(set.add(cpu));
    return set;
}

Status CpuSet::add(const std::size_t cpu) noexcept {
    Status status{Status::kInvalidArgument};
    if (cpu < kMaxCpus) {
        cpus_[cpu] = true;  // operator[] never throws, unlike bitset::set().
        status = Status::kOk;
    }
    return status;
}

void CpuSet::remove(const std::size_t cpu) noexcept {
    if (cpu < kMaxCpus) {
        cpus_[cpu] = false;
    }
}

bool CpuSet::contains(const std::size_t cpu) const noexcept {
    return (cpu < kMaxCpus) && cpus_[cpu];
}

std::size_t CpuSet::count() const noexcept {
    return cpus_.count();
}

bool CpuSet::empty() const noexcept {
    return cpus_.none();
}

Status apply_to_current_thread(const ThreadConfig& config) noexcept {
    Status status{Status::kOk};
    if (config.name != nullptr) {
        status = set_current_thread_name(config.name);
    }
    if (is_ok(status) && (!config.affinity.empty())) {
        status = set_current_thread_affinity(config.affinity);
    }
    if (is_ok(status)) {
        status = set_current_thread_scheduling(config.policy, config.priority);
    }
    return status;
}

RtThread::~RtThread() {
    COMMON_ASSERT(!running_);
}

}  // namespace common
