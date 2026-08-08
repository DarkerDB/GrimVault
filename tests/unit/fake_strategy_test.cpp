#include <gv/capture/fake_strategy.h>

#include <gtest/gtest.h>

using namespace gv;

TEST (FakeStrategyTest, ReturnsSolidFrame)
{
   capture::FakeStrategy fake;
   ASSERT_TRUE (fake.initialize ().has_value ());

   fake.push_solid (32, 16, 0x80, 0x40, 0x20);

   auto frame = fake.capture_monitor (nullptr);
   ASSERT_TRUE (frame.has_value ());
   EXPECT_EQ (frame->width,  32);
   EXPECT_EQ (frame->height, 16);
   EXPECT_EQ (frame->stride, 128);

   // BGRA — first pixel: B=0x20, G=0x40, R=0x80, A=0xff
   const auto* p = frame->data.get ();
   EXPECT_EQ (p [0], 0x20);
   EXPECT_EQ (p [1], 0x40);
   EXPECT_EQ (p [2], 0x80);
   EXPECT_EQ (p [3], 0xff);
}

TEST (FakeStrategyTest, RoundRobinsThroughLoadedFrames)
{
   capture::FakeStrategy fake;
   ASSERT_TRUE (fake.initialize ().has_value ());
   fake.push_solid (8, 8, 0xff, 0x00, 0x00);
   fake.push_solid (8, 8, 0x00, 0xff, 0x00);

   auto a = fake.capture_monitor (nullptr);
   auto b = fake.capture_monitor (nullptr);
   auto c = fake.capture_monitor (nullptr);

   ASSERT_TRUE (a.has_value ());
   ASSERT_TRUE (b.has_value ());
   ASSERT_TRUE (c.has_value ());

   // First frame: pure red. BGRA → B=0, G=0, R=0xff.
   EXPECT_EQ (a->data [2], 0xff);
   // Second frame: pure green. BGRA → B=0, G=0xff, R=0.
   EXPECT_EQ (b->data [1], 0xff);
   // Loops back to red.
   EXPECT_EQ (c->data [2], 0xff);
}

TEST (FakeStrategyTest, FailsWhenNoFramesLoaded)
{
   capture::FakeStrategy fake;
   ASSERT_TRUE (fake.initialize ().has_value ());
   auto r = fake.capture_monitor (nullptr);
   EXPECT_FALSE (r.has_value ());
   EXPECT_EQ (r.error ().kind, core::ErrorKind::Capture);
}

TEST (FakeStrategyTest, NameAndReasonNonEmpty)
{
   capture::FakeStrategy fake;
   EXPECT_EQ      (fake.name (), "fake");
   EXPECT_FALSE   (fake.reason ().empty ());
}
