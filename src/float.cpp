#include "common/float.hpp"

#include <cfenv>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif

namespace common {
namespace {

#if defined(__x86_64__) || defined(__i386__)
constexpr std::uint32_t kMxcsrFlushToZero{0x8000U};
constexpr std::uint32_t kMxcsrDenormalsAreZero{0x0040U};
constexpr std::uint32_t kMxcsrFlushMask{kMxcsrFlushToZero | kMxcsrDenormalsAreZero};
#elif defined(__aarch64__)
constexpr std::uint64_t kFpcrFlushToZero{static_cast<std::uint64_t>(1U) << 24U};

// Deviation (MISRA C++:2023 Rule 10.4.1): FPCR is only reachable through inline assembly.
std::uint64_t read_fpcr() noexcept {
    std::uint64_t value{0U};
    asm volatile("mrs %0, fpcr" : "=r"(value));
    return value;
}

void write_fpcr(const std::uint64_t value) noexcept {
    asm volatile("msr fpcr, %0" : : "r"(value));
}
#endif

constexpr int kTrapMask{FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW};

}  // namespace

Status enable_flush_denormals() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_setcsr(_mm_getcsr() | kMxcsrFlushMask);
    return Status::kOk;
#elif defined(__aarch64__)
    write_fpcr(read_fpcr() | kFpcrFlushToZero);
    return Status::kOk;
#else
    return Status::kNotSupported;
#endif
}

Status disable_flush_denormals() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_setcsr(_mm_getcsr() & ~kMxcsrFlushMask);
    return Status::kOk;
#elif defined(__aarch64__)
    write_fpcr(read_fpcr() & ~kFpcrFlushToZero);
    return Status::kOk;
#else
    return Status::kNotSupported;
#endif
}

bool denormals_flushed() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    return (_mm_getcsr() & kMxcsrFlushMask) == kMxcsrFlushMask;
#elif defined(__aarch64__)
    return (read_fpcr() & kFpcrFlushToZero) != 0U;
#else
    return false;
#endif
}

void clear_fp_exceptions() noexcept {
    static_cast<void>(std::feclearexcept(FE_ALL_EXCEPT));
}

FpExceptions test_fp_exceptions() noexcept {
    const int raised{std::fetestexcept(FE_ALL_EXCEPT)};
    FpExceptions result{};
    result.invalid = (raised & FE_INVALID) != 0;
    result.divide_by_zero = (raised & FE_DIVBYZERO) != 0;
    result.overflow = (raised & FE_OVERFLOW) != 0;
    result.underflow = (raised & FE_UNDERFLOW) != 0;
    result.inexact = (raised & FE_INEXACT) != 0;
    return result;
}

Status enable_fp_traps() noexcept {
#if defined(__GLIBC__)
    return (::feenableexcept(kTrapMask) == -1) ? Status::kNotSupported : Status::kOk;
#else
    return Status::kNotSupported;
#endif
}

Status disable_fp_traps() noexcept {
#if defined(__GLIBC__)
    return (::fedisableexcept(kTrapMask) == -1) ? Status::kNotSupported : Status::kOk;
#else
    return Status::kNotSupported;
#endif
}

bool rounding_is_nearest() noexcept {
    return std::fegetround() == FE_TONEAREST;
}

}  // namespace common
