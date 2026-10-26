#include "metrics/frame_tracker.h"
#include "test_support.h"

namespace yawa::test {

namespace {

constexpr std::uint64_t kQpc = 10'000'000;

std::uint64_t framesToQpc(double frames, double fps)
{
    return static_cast<std::uint64_t>(frames * static_cast<double>(kQpc) / fps);
}

void testSingleStreamFps()
{
    FrameTracker tracker(kQpc);
    std::uint64_t qpc = kQpc;
    for (int index = 0; index < 200; ++index) {
        tracker.onPresent({ qpc, 42, 1, 0xAAAA });
        qpc += framesToQpc(1.0, 120.0);
    }

    CHECK(tracker.hasActiveStream());
    CHECK(tracker.activeProcessId() == 42);
    CHECK_NEAR(tracker.summary(120.0).currentFps, 120.0, 1.0);
    CHECK_NEAR(tracker.summary(120.0).low1Fps, 120.0, 1.0);
}

void testHitchLowersTailPercentiles()
{
    FrameTracker tracker(kQpc);
    std::uint64_t qpc = kQpc;

    for (int index = 0; index < 150; ++index) {
        tracker.onPresent({ qpc, 7, 1, 1 });
        qpc += framesToQpc(1.0, 120.0);
    }
    for (int index = 0; index < 50; ++index) {
        tracker.onPresent({ qpc, 7, 1, 1 });
        qpc += framesToQpc(1.0, 40.0);
    }

    const FpsSummary summary = tracker.summary(120.0);
    CHECK(summary.low1Fps < 45.0);
    CHECK(summary.low1Fps >= 40.0);
    CHECK(summary.averageFps > 60.0);
}

void testActiveStreamIsFaster()
{
    FrameTracker tracker(kQpc);

    // Оба потока живут на одних часах: игра на 144 Гц, рабочий стол на 30 Гц.
    for (int millisecond = 0; millisecond < 3000; millisecond += 1) {
        const auto qpc = kQpc + static_cast<std::uint64_t>(millisecond) * kQpc / 1000;
        if (millisecond % 7 == 0) {
            tracker.onPresent({ qpc, 200, 2, 0xBEEF });
        }
        if (millisecond % 33 == 0) {
            tracker.onPresent({ qpc, 100, 1, 0xDEAD });
        }
    }

    CHECK(tracker.activeProcessId() == 200);
    CHECK(tracker.activeSwapChain() == 0xBEEF);
    CHECK(tracker.streamCount() == 2);
    CHECK_NEAR(tracker.summary(144.0).currentFps, 144.0, 2.0);
}

// Ровно та ситуация, из-за которой в игре было 120 кадров, а программа показывала 144:
// композитор рабочего стола подаёт кадры чаще игры.
void testForegroundGameBeatsDesktopCompositor()
{
    FrameTracker tracker(kQpc);
    for (int millisecond = 0; millisecond < 3000; ++millisecond) {
        const auto qpc = kQpc + static_cast<std::uint64_t>(millisecond) * kQpc / 1000;
        if (millisecond % 7 == 0) {
            tracker.onPresent({ qpc, 500, 1, 0xD00D }); // рабочий стол: ~143 кадра
        }
        if (millisecond % 8 == 0) {
            tracker.onPresent({ qpc, 600, 2, 0x600D }); // игра: 125 кадров
        }
    }

    // Без подсказки побеждает самый быстрый поток — это и был завышенный FPS.
    CHECK(tracker.activeProcessId() == 500);

    tracker.setForegroundProcess(600);
    CHECK(tracker.activeProcessId() == 600);
    CHECK_NEAR(tracker.summary(120.0).currentFps, 125.0, 2.0);

    // Служебный процесс не становится «игрой», даже если он в активном окне.
    tracker.setForegroundProcess(500);
    const std::uint32_t system = 500;
    tracker.setSystemProcesses(&system, 1);
    CHECK(tracker.isSystemProcess(500));
    CHECK(tracker.activeProcessId() == 600);
}

void testStoppedStreamLosesActivity()
{
    FrameTracker tracker(kQpc);
    std::uint64_t qpc = kQpc;
    for (int index = 0; index < 60; ++index) {
        tracker.onPresent({ qpc, 300, 1, 1 });
        qpc += framesToQpc(1.0, 144.0);
    }
    CHECK(tracker.activeProcessId() == 300);

    // Игра закрылась, остался рабочий стол на 60 Гц.
    for (int index = 0; index < 120; ++index) {
        tracker.onPresent({ qpc, 400, 1, 2 });
        qpc += framesToQpc(1.0, 60.0);
    }
    CHECK(tracker.activeProcessId() == 400);
}

void testLongPauseDoesNotPolluteWindow()
{
    FrameTracker tracker(kQpc);
    std::uint64_t qpc = kQpc;
    for (int index = 0; index < 60; ++index) {
        tracker.onPresent({ qpc, 500, 1, 1 });
        qpc += framesToQpc(1.0, 120.0);
    }

    const std::size_t before = tracker.activeWindow()->size();
    qpc += static_cast<std::uint64_t>(kQpc) * 5;
    tracker.onPresent({ qpc, 500, 1, 1 });

    CHECK(tracker.activeWindow()->size() == before);
    CHECK_NEAR(tracker.summary(120.0).low1Fps, 120.0, 1.0);
}

void testStreamLimitKeepsWorking()
{
    FrameTracker tracker(kQpc);
    std::uint64_t qpc = kQpc;
    for (std::uint32_t process = 1; process <= 20; ++process) {
        tracker.onPresent({ qpc, process, 1, process });
        qpc += framesToQpc(1.0, 60.0);
    }

    CHECK(tracker.streamCount() <= FrameTracker::kMaxStreams);
    CHECK(tracker.droppedStreams() > 0);
    CHECK(tracker.hasActiveStream());
}

void testZeroFrequencyIsRejected()
{
    FrameTracker tracker(0);
    CHECK(!tracker.onPresent({ kQpc, 1, 1, 1 }));
    CHECK(!tracker.hasActiveStream());
}

void testResetClearsState()
{
    FrameTracker tracker(kQpc);
    tracker.onPresent({ kQpc, 1, 1, 1 });
    tracker.reset();

    CHECK(tracker.streamCount() == 0);
    CHECK(!tracker.hasActiveStream());
}

} // namespace

void runFrameTrackerTests()
{
    testSingleStreamFps();
    testHitchLowersTailPercentiles();
    testActiveStreamIsFaster();
    testForegroundGameBeatsDesktopCompositor();
    testStoppedStreamLosesActivity();
    testLongPauseDoesNotPolluteWindow();
    testStreamLimitKeepsWorking();
    testZeroFrequencyIsRejected();
    testResetClearsState();
}

} // namespace yawa::test
