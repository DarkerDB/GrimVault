#pragma once

#include <gv/core/error.h>

#if __has_include (<expected>) && defined (__cpp_lib_expected)
   #include <expected>
   #define GV_HAS_STD_EXPECTED 1
#else
   #define GV_HAS_STD_EXPECTED 0
#endif

namespace gv::core {

#if GV_HAS_STD_EXPECTED

template <typename T, typename E = Error>
using Result = std::expected<T, E>;

template <typename E = Error>
inline auto fail (E&& err)
{
   return std::unexpected<std::decay_t<E>> (std::forward<E> (err));
}

#else
   #error "C++23 std::expected is required. Use /std:c++latest or upgrade toolchain."
#endif

} // namespace gv::core
