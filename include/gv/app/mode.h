#pragma once

namespace gv::app {

// The overlay's operating mode, as folded from the `overlay:mode` setting
// and as held live by the Controller.
//
// Auto     — hover a tooltip and the card appears (overlay:mode automatic).
// Manual   — nothing is scanned until the scan_now hotkey (overlay:mode manual).
// Disabled — capture, OCR, API work, and presentation are stopped.
//
// Kept in its own Qt-free header so Preferences (header-only, unit-testable
// without an event loop) and Controller (a QObject) can share one enum
// rather than each carrying its own and converting between them.
enum class Mode { Auto, Manual, Disabled };

} // namespace gv::app
