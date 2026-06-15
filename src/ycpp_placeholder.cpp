// W1 placeholder TU. Keeps `ycpp::ycpp` STATIC alive before the algorithm
// TUs land in W2+. Carries the version constant and nothing else.

#include <cstdint>

namespace ycpp {

inline constexpr uint32_t kVersionMajor = 0;
inline constexpr uint32_t kVersionMinor = 0;
inline constexpr uint32_t kVersionPatch = 1;

// Force at least one externally-visible symbol so MSVC's lib step does
// not warn about an empty translation unit.
extern "C" uint32_t ycpp_version() noexcept {
    return (kVersionMajor << 16) | (kVersionMinor << 8) | kVersionPatch;
}

} // namespace ycpp
