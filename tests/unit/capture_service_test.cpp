#include <gv/capture/capture_service.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace gv;

namespace {

   struct Script {
      std::string       name;
      bool              initializes = true;
      std::vector<bool> captures;
      int               init_calls = 0;
      int               capture_calls = 0;
      int               shutdown_calls = 0;
      bool              initialized = false;
   };

   class ScriptStrategy final : public capture::ICaptureStrategy
   {
   public:
      explicit ScriptStrategy (std::shared_ptr<Script> script)
         : script_ (std::move (script))
      {}

      core::Result<void> initialize () override
      {
         ++script_->init_calls;
         script_->initialized = script_->initializes;
         if (script_->initialized) return {};

         return core::fail (core::Error::make (
            core::ErrorKind::Capture, "{}: unavailable", script_->name));
      }

      void shutdown () noexcept override
      {
         ++script_->shutdown_calls;
         script_->initialized = false;
      }

      core::Result<capture::Frame> capture_window (void*) override
      {
         return capture ();
      }

      core::Result<capture::Frame> capture_monitor (void*) override
      {
         return capture ();
      }

      std::string_view name () const noexcept override { return script_->name; }
      std::string_view reason () const noexcept override { return "scripted test strategy"; }

   private:
      core::Result<capture::Frame> capture ()
      {
         const auto call = static_cast<std::size_t> (script_->capture_calls++);
         const bool succeeds = call < script_->captures.size ()
            ? script_->captures [call]
            : true;

         if (!script_->initialized || !succeeds) {
            return core::fail (core::Error::make (
               core::ErrorKind::Capture, "{}: scripted failure", script_->name));
         }

         auto pixels = std::shared_ptr<std::uint8_t []> (new std::uint8_t [4] {
            0, 0, 0, 0xff
         });
         return capture::Frame {
            .data      = std::move (pixels),
            .width     = 1,
            .height    = 1,
            .stride    = 4,
            .dpi_scale = 1.0,
         };
      }

      std::shared_ptr<Script> script_;
   };

   std::shared_ptr<Script> append (
      capture::CaptureService::Strategies& strategies,
      std::string                          name,
      bool                                 initializes,
      std::initializer_list<bool>          captures = {}
   ) {
      auto script = std::make_shared<Script> (Script {
         .name        = std::move (name),
         .initializes = initializes,
         .captures    = captures,
      });
      strategies.push_back (std::make_unique<ScriptStrategy> (script));
      return script;
   }

   std::unique_ptr<capture::CaptureService> create (
      capture::CaptureService::Strategies strategies,
      int                                 threshold = 2
   ) {
      auto service = capture::CaptureService::create (
         std::move (strategies),
         capture::CaptureService::Config { .failure_threshold = threshold });
      EXPECT_TRUE (service.has_value ());
      return service.has_value () ? std::move (*service) : nullptr;
   }

} // namespace

TEST (CaptureServiceTest, StartupSelectsFirstInitializedStrategy)
{
   capture::CaptureService::Strategies strategies;
   auto wgc  = append (strategies, "wgc.borderless", false);
   auto dxgi = append (strategies, "dxgi.duplication", true);
   auto gdi  = append (strategies, "gdi.bitblt", true);

   auto service = create (std::move (strategies));
   ASSERT_NE (service, nullptr);
   EXPECT_EQ (service->current ().name (), "dxgi.duplication");
   EXPECT_EQ (wgc->init_calls, 1);
   EXPECT_EQ (dxgi->init_calls, 1);
   EXPECT_EQ (gdi->init_calls, 0);

   const auto available = service->available ();
   ASSERT_EQ (available.size (), 1);
   EXPECT_EQ (available.front (), "dxgi.duplication");
}

TEST (CaptureServiceTest, RuntimeFallsThroughEntireLadder)
{
   capture::CaptureService::Strategies strategies;
   auto wgc  = append (strategies, "wgc.borderless", true, { false, false });
   auto dxgi = append (strategies, "dxgi.duplication", true, { true, false, false });
   auto gdi  = append (strategies, "gdi.bitblt", true, { true });

   auto service = create (std::move (strategies));
   ASSERT_NE (service, nullptr);

   EXPECT_FALSE (service->capture_window (nullptr).has_value ());
   const auto dxgiFrame = service->capture_window (nullptr);
   ASSERT_TRUE (dxgiFrame.has_value ());
   EXPECT_EQ   (dxgiFrame->backend, capture::CaptureBackend::Dxgi);
   EXPECT_EQ    (service->current ().name (), "dxgi.duplication");
   EXPECT_EQ    (wgc->shutdown_calls, 1);

   EXPECT_FALSE (service->capture_window (nullptr).has_value ());
   const auto gdiFrame = service->capture_window (nullptr);
   ASSERT_TRUE (gdiFrame.has_value ());
   EXPECT_EQ   (gdiFrame->backend, capture::CaptureBackend::Gdi);
   EXPECT_EQ    (service->current ().name (), "gdi.bitblt");
   EXPECT_EQ    (dxgi->shutdown_calls, 1);
   EXPECT_EQ    (gdi->capture_calls, 1);
}

TEST (CaptureServiceTest, SuccessfulFrameResetsFailureCount)
{
   capture::CaptureService::Strategies strategies;
   append (strategies, "wgc.borderless", true, { false, true, false, false });
   append (strategies, "dxgi.duplication", true, { true });

   auto service = create (std::move (strategies));
   ASSERT_NE (service, nullptr);

   EXPECT_FALSE (service->capture_window (nullptr).has_value ());
   EXPECT_TRUE  (service->capture_window (nullptr).has_value ());
   EXPECT_FALSE (service->capture_window (nullptr).has_value ());
   EXPECT_EQ    (service->current ().name (), "wgc.borderless");
   EXPECT_TRUE  (service->capture_window (nullptr).has_value ());
   EXPECT_EQ    (service->current ().name (), "dxgi.duplication");
}

TEST (CaptureServiceTest, TargetChangeResetsFailureCount)
{
   capture::CaptureService::Strategies strategies;
   append (strategies, "wgc.borderless", true, { false, false, false });
   append (strategies, "dxgi.duplication", true, { true });

   auto service = create (std::move (strategies));
   ASSERT_NE (service, nullptr);

   auto* first  = reinterpret_cast<void*> (std::uintptr_t { 1 });
   auto* second = reinterpret_cast<void*> (std::uintptr_t { 2 });
   EXPECT_FALSE (service->capture_window (first).has_value ());
   EXPECT_FALSE (service->capture_window (second).has_value ());
   EXPECT_EQ    (service->current ().name (), "wgc.borderless");
   EXPECT_TRUE  (service->capture_window (second).has_value ());
   EXPECT_EQ    (service->current ().name (), "dxgi.duplication");
}

TEST (CaptureServiceTest, NewTargetRestoresPreferredStrategy)
{
   capture::CaptureService::Strategies strategies;
   auto wgc = append (
      strategies, "wgc.borderless", true, { false, false, true });
   auto dxgi = append (strategies, "dxgi.duplication", true, { true });

   auto service = create (std::move (strategies));
   ASSERT_NE (service, nullptr);

   auto* first  = reinterpret_cast<void*> (std::uintptr_t { 1 });
   auto* second = reinterpret_cast<void*> (std::uintptr_t { 2 });
   EXPECT_FALSE (service->capture_window (first).has_value ());
   EXPECT_TRUE  (service->capture_window (first).has_value ());
   EXPECT_EQ    (service->current ().name (), "dxgi.duplication");

   EXPECT_TRUE (service->capture_window (second).has_value ());
   EXPECT_EQ   (service->current ().name (), "wgc.borderless");
   EXPECT_EQ   (wgc->init_calls, 2);
   EXPECT_EQ   (dxgi->shutdown_calls, 1);
}

TEST (CaptureServiceTest, RuntimeSkipsUnavailableFallback)
{
   capture::CaptureService::Strategies strategies;
   append (strategies, "wgc.borderless", true, { false, false });
   auto dxgi = append (strategies, "dxgi.duplication", false);
   append (strategies, "gdi.bitblt", true, { true });

   auto service = create (std::move (strategies));
   ASSERT_NE (service, nullptr);

   EXPECT_FALSE (service->capture_window (nullptr).has_value ());
   EXPECT_TRUE  (service->capture_window (nullptr).has_value ());
   EXPECT_EQ    (service->current ().name (), "gdi.bitblt");
   EXPECT_EQ    (dxgi->init_calls, 1);
}
