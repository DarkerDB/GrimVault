#include <gv/capture/capture_service.h>
#include <gv/ui/screen.h>

#include <QEventLoop>
#include <QPainter>
#include <QTimer>
#include <QWidget>

#include <Windows.h>
#include <gtest/gtest.h>

namespace {

// Solid native windows model the game (red) and translucent overlay (blue).
// Input QColor(255, 0, 0) paints a red client area, including its center.
class CaptureCard final : public QWidget
{
  public:
   explicit CaptureCard (QColor color)
       : QWidget (nullptr, Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool),
         color_ (color)
   {
      setAttribute (Qt::WA_TranslucentBackground);
      setAttribute (Qt::WA_ShowWithoutActivating);
      setFocusPolicy (Qt::NoFocus);
      resize (80, 80);
   }

  protected:
   // Input: repaint request. Output: every client pixel has color_.
   // Example: blue (0, 0, 255) fills the complete 80x80 test card.
   void paintEvent (QPaintEvent*) override
   {
      QPainter painter { this };
      painter.fillRect (rect (), color_);
   }

  private:
   QColor color_;
};

// Read the actual DXGI frame until the target center has the expected RGB.
// Input: excluded blue overlay over red. Output: true for Qt::red, not blue.
testing::AssertionResult captures_color (gv::capture::CaptureService& capture, QWidget& target,
                                         QColor expected)
{
   const auto deadline = std::chrono::steady_clock::now () + std::chrono::seconds { 3 };
   const HWND handle = reinterpret_cast<HWND> (target.winId ());
   RECT bounds {};
   ::GetWindowRect (handle, &bounds);
   const int x = (bounds.left + bounds.right) / 2;
   const int y = (bounds.top + bounds.bottom) / 2;

   QColor last_color;
   int observed_frames = 0;

   while (std::chrono::steady_clock::now () < deadline) {
      QEventLoop loop;
      QTimer::singleShot (30, &loop, &QEventLoop::quit);
      loop.exec ();

      const auto frame = capture.capture_window (handle);

      if (!frame.has_value () || frame->empty ()) {
         continue;
      }

      const int local_x = x - frame->origin_x;
      const int local_y = y - frame->origin_y;
      const bool inside =
         local_x >= 0 && local_y >= 0 && local_x < frame->width && local_y < frame->height;

      if (!inside) {
         continue;
      }

      const auto* pixel = frame->data.get () + local_y * frame->stride + local_x * 4;
      const QColor actual { pixel[2], pixel[1], pixel[0] };
      last_color = actual;
      ++observed_frames;

      if (actual == expected) {
         return testing::AssertionSuccess ();
      }
   }

   DWORD point_pid = 0;
   const HWND at_point = ::WindowFromPoint (POINT { x, y });
   ::GetWindowThreadProcessId (at_point, &point_pid);

   return testing::AssertionFailure ()
          << "Expected " << expected.name ().toStdString () << " at desktop center " << x << ","
          << y << "; observed " << last_color.name ().toStdString () << " across "
          << observed_frames << " frames" << "; covering process=" << point_pid
          << "; test process=" << ::GetCurrentProcessId ()
          << ". Run capture tests on an unlocked interactive desktop.";
}

TEST (Renderer, DesktopCaptureSeesGameBehindExcludedOverlay)
{
   auto created = gv::capture::CaptureService::create ();
   ASSERT_TRUE (created.has_value ());
   auto capture = std::move (*created);
   ASSERT_TRUE (capture->set_mode (gv::capture::CaptureMode::ForceDxgi).has_value ());

   CaptureCard game { Qt::red };
   CaptureCard overlay { Qt::blue };
   game.move (300, 300);
   overlay.move (300, 300);
   game.show ();
   overlay.show ();
   ASSERT_TRUE (captures_color (*capture, game, Qt::blue));
   gv::ui::screen::make_passthrough (overlay.windowHandle ());
   EXPECT_TRUE (overlay.isVisible ());
   EXPECT_TRUE (captures_color (*capture, game, Qt::red));

   overlay.hide ();
   overlay.show ();
   gv::ui::screen::make_passthrough (overlay.windowHandle ());
   EXPECT_TRUE (captures_color (*capture, game, Qt::red));
}

}
