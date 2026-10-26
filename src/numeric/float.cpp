#include "common/numeric/float.hpp"

#include <cfenv>
#include <cstdint>

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include <xmmintrin.h>
#endif

#if defined(_WIN32)
#include <float.h>  // _controlfp_s
#endif

namespace common {
namespace {

#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
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

#if defined(_WIN32)
// Exception mask bits of the floating-point control word; a set bit disables the trap.
constexpr unsigned int kTrapMask{static_cast<unsigned int>(_EM_INVALID) | static_cast<unsigned int>(_EM_ZERODIVIDE) |
                                 static_cast<unsigned int>(_EM_OVERFLOW)};
#elif defined(__GLIBC__)
constexpr int kTrapMask{FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW};
#endif

}  // namespace

Status enable_flush_denormals() noexcept {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
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
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
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
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
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
#elif defined(_WIN32)
    unsigned int control{0U};
    return (::_controlfp_s(&control, 0U, kTrapMask) == 0) ? Status::kOk : Status::kNotSupported;
#else
    return Status::kNotSupported;
#endif
}

Status disable_fp_traps() noexcept {
#if defined(__GLIBC__)
    return (::fedisableexcept(kTrapMask) == -1) ? Status::kNotSupported : Status::kOk;
#elif defined(_WIN32)
    unsigned int control{0U};
    return (::_controlfp_s(&control, kTrapMask, kTrapMask) == 0) ? Status::kOk : Status::kNotSupported;
#else
    return Status::kNotSupported;
#endif
}

bool rounding_is_nearest() noexcept {
    return std::fegetround() == FE_TONEAREST;
}

}  // namespace common
