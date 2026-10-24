/// @file common.hpp
/// @brief Umbrella header: includes every header of the library.

#ifndef COMMON_COMMON_HPP
#define COMMON_COMMON_HPP

#include "common/concurrency/mpmc_queue.hpp"
#include "common/concurrency/seqlock.hpp"
#include "common/concurrency/spin.hpp"
#include "common/concurrency/spsc_queue.hpp"
#include "common/concurrency/triple_buffer.hpp"
#include "common/containers/object_pool.hpp"
#include "common/containers/ring_buffer.hpp"
#include "common/containers/span.hpp"
#include "common/containers/static_vector.hpp"
#include "common/control/debouncer.hpp"
#include "common/control/hysteresis.hpp"
#include "common/control/low_pass_filter.hpp"
#include "common/control/pid_controller.hpp"
#include "common/control/slew_rate_limiter.hpp"
#include "common/core/assert.hpp"
#include "common/core/config.hpp"
#include "common/core/status.hpp"
#include "common/diagnostics/latency_histogram.hpp"
#include "common/diagnostics/watchdog.hpp"
#include "common/file/file.hpp"
#include "common/numeric/float.hpp"
#include "common/numeric/integer.hpp"
#include "common/protocol/rate_limiter.hpp"
#include "common/system/memory.hpp"
#include "common/system/thread.hpp"
#include "common/time/clock.hpp"

#endif  // COMMON_COMMON_HPP
