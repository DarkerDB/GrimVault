#include <gv/ui/tray_menu.h>

#include <gv/core/env_resolver.h>
#include <gv/core/logger.h>
#include <gv/core/version.h>

#include <QColor>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScreen>
#include <QVBoxLayout>

#include <cctype>
#include <string>

namespace gv::ui {

namespace {

   // Game fonts — registered at app startup in main.cpp and shared with
   // the in-game tooltip (qml/Tooltip.qml). Qt matches on the family name
   // embedded in the TTF ("Solmoe KimDaeGeon ..."), not the filename
   // (SaintKDG_*.ttf) — the filename silently falls back to the system font.
   constexpr const char* k_font_body   = "Solmoe KimDaeGeon Light";
   constexpr const char* k_font_strong = "Solmoe KimDaeGeon Medium";

   // Palette mirrors qml/Palette.qml. Kept inline so the tray works
   // before any QQuickWindow exists.
   constexpr const char* k_bg          = "#14110E";  // panel
   constexpr const char* k_border      = "#1C1814";  // recessed
   constexpr const char* k_separator   = "#2E2822";  // muted steel
   constexpr const char* k_text_body   = "#C9BFA7";  // body
   constexpr const char* k_text_dim    = "#7A7060";  // muted
   constexpr const char* k_text_strong = "#ECD99A";  // parchment
   constexpr const char* k_hover_text  = "#E9C079";  // brassHi

   // DarkerDB brand accent — the blood red of the site's realm mark
   // (spark realms.ts: color #EF1F1F on a near-black ground). Hover rows
   // get a thick red left edge plus a red wash fading rightward, echoing
   // the site's selected-tab motif.
   constexpr const char* k_accent      = "#EF1F1F";
   constexpr const char* k_accent_bg   =
      "qlineargradient(x1: 0, y1: 0, x2: 1, y2: 0,"
      " stop: 0 rgba(127, 0, 0, 64), stop: 1 rgba(10, 3, 3, 0))";
   constexpr const char* k_dot_ok      = "#6E8F3C";  // ok green
   constexpr const char* k_dot_off     = "#B33A2A";  // danger red

   constexpr int k_radius        = 10;
   constexpr int k_shadow_margin = 16;
   constexpr int k_min_width     = 220;
   constexpr int k_sep_height    = 11;

   // Tiny solid disc — wrapped so we can place it in a layout without
   // subclassing QLabel.
   class Dot : public QWidget
   {
   public:
      explicit Dot (QWidget* parent = nullptr) : QWidget (parent)
      {
         setFixedSize (10, 10);
         setAttribute (Qt::WA_TransparentForMouseEvents);
      }

      void set_color (const QColor& c) { color_ = c; update (); }

   protected:
      void paintEvent (QPaintEvent*) override
      {
         QPainter p (this);
         p.setRenderHint (QPainter::Antialiasing);
         p.setPen   (Qt::NoPen);
         p.setBrush (color_);
         p.drawEllipse (rect ().adjusted (1, 1, -1, -1));
      }

   private:
      QColor color_ { k_dot_off };
   };

   // Thin painted line centered in a fixed-height widget, so the QVBoxLayout
   // reliably reserves space above + below the line. QSS margin on a free
   // QWidget doesn't translate to layout space, which is why we paint.
   class Separator : public QWidget
   {
   public:
      explicit Separator (QWidget* parent = nullptr) : QWidget (parent)
      {
         setFixedHeight (k_sep_height);
         setAttribute (Qt::WA_TransparentForMouseEvents);
      }

   protected:
      void paintEvent (QPaintEvent*) override
      {
         QPainter p (this);
         p.setPen (QColor (k_separator));
         const int y = height () / 2;
         p.drawLine (12, y, width () - 12, y);
      }
   };

   // "dev" → "Dev", "qa" → "QA", "prod" → "Prod".
   QString pretty_env_name (std::string_view env)
   {
      if (env == "qa") return QStringLiteral ("QA");
      if (env.empty ()) return {};
      std::string out { env };
      out [0] = static_cast<char> (std::toupper (static_cast<unsigned char> (out [0])));
      return QString::fromStdString (out);
   }

} // namespace

TrayMenu::TrayMenu (QWidget* parent)
   : QWidget (parent,
        Qt::Popup
      | Qt::FramelessWindowHint
      | Qt::NoDropShadowWindowHint)
{
   // Qt::Popup gives us the standard menu dismissal contract that every
   // other Win32 app honors: any mouse-down outside the popup closes it,
   // and Escape closes it too. The trade-off is that opening the menu
   // from inside Windows' notification-area overflow flyout will
   // collapse that flyout — accepted as the price of correct dismissal.
   //
   // Qt::WindowStaysOnTopHint is intentionally absent — Popup widgets
   // are already always-on-top, and combining the flags trips a Qt
   // warning on some Windows DPI configs.
   setAttribute (Qt::WA_TranslucentBackground);
   setAttribute (Qt::WA_AlwaysShowToolTips, true);

   auto* outer = new QVBoxLayout (this);
   outer->setContentsMargins (k_shadow_margin, k_shadow_margin,
                              k_shadow_margin, k_shadow_margin);
   outer->setSpacing (0);

   auto* content = new QWidget (this);
   content->setObjectName ("trayMenuContent");
   outer->addWidget (content);

   auto* body = new QVBoxLayout (content);
   body->setContentsMargins (0, 10, 0, 10);
   body->setSpacing (0);

   // --- Header --------------------------------------------------------
   {
      auto* header = new QWidget (content);
      auto* hl = new QHBoxLayout (header);
      hl->setContentsMargins (16, 4, 16, 10);
      hl->setSpacing (10);

      status_dot_ = new Dot (header);
      hl->addWidget (status_dot_, 0, Qt::AlignVCenter);

      auto* title = new QLabel (header);
      title->setTextFormat (Qt::RichText);
      // Set the base font on the QLabel — QLabel's rich-text renderer
      // honors inline font-family declarations unreliably, but the
      // widget's QFont propagates as the default for all runs in the
      // document. The <span>s then layer size + color on top.
      title->setFont (QFont (QString::fromLatin1 (k_font_strong)));
      title->setText (QStringLiteral (
         R"(<span style="font-weight: 600; color: %1; font-size: 12px;">GrimVault</span>)"
         R"(<span style="color: %2; font-size: 10px;"> (%3) v%4</span>)")
         .arg (k_text_strong)
         .arg (k_text_dim)
         .arg (pretty_env_name (gv::core::active_env ().name))
         .arg (QString::fromLatin1 (gv::core::version::string)));
      hl->addWidget (title, 1, Qt::AlignVCenter);

      body->addWidget (header);
   }

   body->addWidget (make_separator ());

   // --- Main actions ---------------------------------------------------
   {
      auto* btn = add_item (body, QStringLiteral ("Settings"));
      connect (btn, &QPushButton::clicked, this, [this] {
         hide ();
         emit settings_requested ();
      });
   }
   {
      auto* btn = add_item (body, QStringLiteral ("Logs"));
      connect (btn, &QPushButton::clicked, this, [this] {
         hide ();
         emit logs_requested ();
      });
   }
   {
      auto* btn = add_item (body, QStringLiteral ("Check for updates"));
      connect (btn, &QPushButton::clicked, this, [this] {
         hide ();
         emit check_updates_requested ();
      });
   }

   body->addWidget (make_separator ());

   // --- Account / exit (last section) ----------------------------------
   auth_btn_ = add_item (body, QStringLiteral ("Sign In"));
   connect (auth_btn_, &QPushButton::clicked, this, [this] {
      hide ();
      if (signed_in_) emit sign_out_requested ();
      else            emit sign_in_requested  ();
   });
   {
      auto* btn = add_item (body, QStringLiteral ("Exit"));
      connect (btn, &QPushButton::clicked, this, [this] {
         hide ();
         emit quit_requested ();
      });
   }

   content->setMinimumWidth (k_min_width);

   refresh_dot  ();
   refresh_auth ();
}

QPushButton* TrayMenu::add_item (QVBoxLayout* body, const QString& label)
{
   auto* btn = new QPushButton (label);
   btn->setFlat (true);
   btn->setCursor (Qt::PointingHandCursor);
   // QFont set on the widget so the game font binds reliably; QSS only
   // layers padding/color/hover on top.
   btn->setFont (QFont (QString::fromLatin1 (k_font_body)));
   // The transparent left border is always present so the text doesn't
   // shift when the hover accent paints over it.
   btn->setStyleSheet (QStringLiteral (R"qss(
      QPushButton {
         border: none;
         border-left: 3px solid transparent;
         text-align: left;
         padding: 5px 16px 5px 13px;
         color: %1;
         background: transparent;
         font-size: 12px;
      }
      QPushButton:hover {
         border-left: 3px solid %2;
         background: %3;
         color:      %4;
      }
      QPushButton:disabled {
         color: %5;
      }
   )qss")
      .arg (k_text_body)
      .arg (k_accent)
      .arg (k_accent_bg)
      .arg (k_hover_text)
      .arg (k_text_dim));
   body->addWidget (btn);
   return btn;
}

QWidget* TrayMenu::make_separator () const
{
   return new Separator ();
}

void TrayMenu::set_signed_in (bool yes)
{
   gv::core::log::ui.info ("tray: set_signed_in({})", yes ? "true" : "false");
   signed_in_ = yes;
   refresh_dot  ();
   refresh_auth ();
}

void TrayMenu::refresh_dot ()
{
   if (status_dot_) {
      static_cast<Dot*> (status_dot_)->set_color (
         QColor (signed_in_ ? k_dot_ok : k_dot_off));
   }
}

void TrayMenu::refresh_auth ()
{
   if (!auth_btn_) return;
   auth_btn_->setText (signed_in_
      ? QStringLiteral ("Log Out")
      : QStringLiteral ("Sign In"));
}

void TrayMenu::popup_at (const QPoint& global_pos)
{
   adjustSize ();

   auto* screen = QGuiApplication::screenAt (global_pos);
   if (!screen) screen = QGuiApplication::primaryScreen ();
   const auto avail = screen->availableGeometry ();

   QPoint origin = global_pos;
   origin.rx () -= width ();
   origin.ry () -= height ();

   if (origin.x () < avail.left ())  origin.setX (avail.left ());
   if (origin.y () < avail.top ())   origin.setY (avail.top ());
   if (origin.x () + width  () > avail.right  ()) origin.setX (avail.right  () - width  ());
   if (origin.y () + height () > avail.bottom ()) origin.setY (avail.bottom () - height ());

   move (origin);
   show ();
   // Qt::Popup auto-activates and grabs mouse capture, so we don't need
   // installEventFilter or raise () — outside clicks and Escape both
   // close the popup natively.
}

void TrayMenu::hideEvent (QHideEvent* ev)
{
   QWidget::hideEvent (ev);
}

void TrayMenu::paintEvent (QPaintEvent* /*ev*/)
{
   QPainter p (this);
   p.setRenderHint (QPainter::Antialiasing);

   const QRectF inner = rect ().adjusted (
      k_shadow_margin, k_shadow_margin,
      -k_shadow_margin, -k_shadow_margin);

   // Soft drop shadow — expanding rounded rects with falling alpha.
   for (int i = 1; i <= 8; ++i) {
      const QRectF r = inner.adjusted (-i, -i + 1, i, i + 2);
      QPainterPath path;
      path.addRoundedRect (r, k_radius + i, k_radius + i);
      QColor c (0, 0, 0);
      c.setAlpha (10 - i);
      p.fillPath (path, c);
   }

   // Panel.
   QPainterPath panel;
   panel.addRoundedRect (inner, k_radius, k_radius);
   p.fillPath (panel, QColor (k_bg));

   // 1px border.
   QPen border_pen { QColor { k_border } };
   border_pen.setWidth (1);
   p.setPen   (border_pen);
   p.setBrush (Qt::NoBrush);
   p.drawPath (panel);
}

} // namespace gv::ui
