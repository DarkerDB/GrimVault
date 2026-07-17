#include <gv/capture/frame.h>

#ifdef _WIN32
   #include <Windows.h>
#endif

namespace gv::capture {

CursorPos cursor_now () noexcept
{
#ifdef _WIN32
   POINT p {};
   if (::GetCursorPos (&p)) {
      return { p.x, p.y, true };
   }
#endif
   return {};
}

} // namespace gv::capture
