#pragma once

#include "metrics/frame_tracker.h"

#include <windows.h>
#include <evntrace.h>

struct _EVENT_RECORD;

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace yawa {

// Захват кадров через ETW: провайдеры DXGI и D3D9 отдают Present_Start/Stop.
// Инъекции в процесс игры нет, драйвер не ставится — поэтому антивирусы и античиты
// на такую сессию не реагируют. Нужны права администратора: без них StartTrace
// вернёт ERROR_ACCESS_DENIED, и вызывающая сторона уходит в режим без FPS.
class EtwFrameSession {
public:
    EtwFrameSession() = default;
    ~EtwFrameSession();

    EtwFrameSession(const EtwFrameSession&) = delete;
    EtwFrameSession& operator=(const EtwFrameSession&) = delete;

    bool start(std::string& error);
    void stop();

    bool isRunning() const { return m_running; }

    // Забирает накопленные события в трекер вызывающего потока: ETW-поток только
    // складывает сэмплы, разбор делает тот, кто владеет FrameTracker.
    std::size_t drain(FrameTracker& tracker);

    std::uint64_t eventsReceived() const { return m_eventsReceived; }
    std::uint64_t eventsRejected() const { return m_eventsRejected; }
    std::uint64_t samplesDropped() const { return m_samplesDropped; }

    static std::uint64_t qpcFrequency();

private:
    static void __stdcall eventRecordCallback(_EVENT_RECORD* record);
    void handleRecord(const _EVENT_RECORD* record);

    std::thread m_thread;
    TRACEHANDLE m_sessionHandle = 0;
    TRACEHANDLE m_traceHandle = 0;
    bool m_running = false;

    mutable std::mutex m_queueMutex;
    std::vector<PresentSample> m_queue;

    std::atomic<std::uint64_t> m_eventsReceived{ 0 };
    std::atomic<std::uint64_t> m_eventsRejected{ 0 };
    std::atomic<std::uint64_t> m_samplesDropped{ 0 };
    std::atomic<bool> m_stopRequested{ false };
};

} // namespace yawa
