#include <gv/capture/capture_service.h>
#include <gv/core/logger.h>

#include "wgc_strategy.h"
#include "dxgi_dup_strategy.h"
#include "gdi_strategy.h"

#include <vector>

namespace gv::capture {

namespace {

   // Probe ladder, in order of preference:
   //    wgc.borderless   Win10 20H1+ / Win11 — no yellow capture border
   //    wgc.bordered     Win10 1803..19045 — visible yellow border
   //    dxgi.duplication Win8+ — monitor-only, vision crops
   //    gdi.bitblt       Vista+ — last resort
   //
   // For each candidate, initialize() is tried. The first that succeeds
   // wins. WGC is split into two logical entries even though the same class
   // backs both — the borderless property is what distinguishes them.
   std::unique_ptr<ICaptureStrategy> probe ()
   {
      // 1. Try WGC. If initialized successfully and borderless() is true,
      //    return immediately. If borderless is false on the FIRST WGC
      //    instance, we still prefer it over DXGI/GDI (the yellow border is
      //    cosmetic, capture still works).
      auto wgc = std::make_unique<WgcStrategy> ();
      auto wr  = wgc->initialize ();

      if (wr.has_value ()) {
         core::Logger::info ("capture: probe selected {} — {}", wgc->name (), wgc->reason ());
         return wgc;
      }

      core::Logger::warn ("capture: WGC unavailable: {}; trying DXGI", wr.error ().message);

      auto dxgi = std::make_unique<DxgiDuplicationStrategy> ();
      auto dr   = dxgi->initialize ();

      if (dr.has_value ()) {
         core::Logger::info ("capture: probe selected {} — {}", dxgi->name (), dxgi->reason ());
         return dxgi;
      }

      core::Logger::warn ("capture: DXGI unavailable: {}; falling back to GDI", dr.error ().message);

      auto gdi = std::make_unique<GdiBitBltStrategy> ();
      auto gr  = gdi->initialize ();

      if (gr.has_value ()) {
         core::Logger::info ("capture: probe selected {} — {}", gdi->name (), gdi->reason ());
         return gdi;
      }

      core::Logger::error ("capture: ALL strategies failed; GDI error: {}", gr.error ().message);
      return nullptr;
   }

   std::unique_ptr<ICaptureStrategy> build (std::string_view name)
   {
      if (name == "wgc" || name == "wgc.borderless" || name == "wgc.bordered") {
         return std::make_unique<WgcStrategy> ();
      }
      if (name == "dxgi" || name == "dxgi.duplication") {
         return std::make_unique<DxgiDuplicationStrategy> ();
      }
      if (name == "gdi" || name == "gdi.bitblt") {
         return std::make_unique<GdiBitBltStrategy> ();
      }
      return nullptr;
   }

} // namespace

struct CaptureService::Impl
{
   std::unique_ptr<ICaptureStrategy> strategy;
};

CaptureService::CaptureService  (std::unique_ptr<Impl> impl) : impl_ (std::move (impl)) {}
CaptureService::~CaptureService ()
{
   if (impl_ && impl_->strategy) {
      impl_->strategy->shutdown ();
   }
}

core::Result<std::unique_ptr<CaptureService>> CaptureService::create ()
{
   auto strategy = probe ();

   if (!strategy) {
      return core::fail (core::Error::make (core::ErrorKind::Capture,
         "capture: no working strategy on this system"));
   }

   auto impl = std::make_unique<Impl> ();
   impl->strategy = std::move (strategy);

   return std::unique_ptr<CaptureService> { new CaptureService (std::move (impl)) };
}

ICaptureStrategy& CaptureService::current () noexcept
{
   return *impl_->strategy;
}

core::Result<void> CaptureService::switch_to (std::string_view strategy_name)
{
   auto next = build (strategy_name);

   if (!next) {
      return core::fail (core::Error::make (core::ErrorKind::InvalidArgument,
         "capture: unknown strategy '{}'", strategy_name));
   }

   auto r = next->initialize ();
   if (!r.has_value ()) return core::fail (r.error ());

   if (impl_->strategy) impl_->strategy->shutdown ();
   impl_->strategy = std::move (next);

   core::Logger::info ("capture: switched to {} — {}", impl_->strategy->name (), impl_->strategy->reason ());
   return {};
}

std::vector<std::string_view> CaptureService::available () const
{
   return {
      "wgc.borderless",
      "wgc.bordered",
      "dxgi.duplication",
      "gdi.bitblt",
   };
}

} // namespace gv::capture
