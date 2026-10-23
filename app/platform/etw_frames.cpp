#include "platform/etw_frames.h"
#include "platform/win_util.h"

#include <windows.h>
#include <evntrace.h>
#include <evntcons.h>

#include <cstddef>

namespace yawa {
namespace {

// GUID и идентификаторы событий взяты из манифестов Microsoft и сверены с PresentMon
// (GameTechDev/PresentMon, PresentData/ETW), поэтому привязка к версии Windows не рвётся.
constexpr GUID kDxgiProvider{ 0xCA11C036, 0x0102, 0x4A2D, { 0xA6, 0xAD, 0xF0, 0x3C, 0xFE, 0xD5, 0xD3, 0xC9 } };
constexpr GUID kD3d9Provider{ 0x783ACA0A, 0x790E, 0x4D7F, { 0x84, 0x51, 0xAA, 0x85, 0x05, 0x11, 0xC6, 0xB9 } };

// Present_Stop описывает завершение того же кадра, поэтому кадром считается только Start.
constexpr unsigned short kDxgiPresentStart = 0x2A;
constexpr unsigned short kD3d9PresentStart = 0x0001;

// Ключ провайдеров: события Present помечены 0x8000000000000002 (Analytic | Events).
constexpr unsigned long long kPresentKeyword = 0x8000000000000002ull;
constexpr unsigned char kInformationLevel = 4;

constexpr wchar_t kSessionName[] = L"YawaFpsHub-Frames";
constexpr std::size_t kMaxQueuedSamples = 8192;

struct EventTraceProperties {
    EVENT_TRACE_PROPERTIES properties{};
    wchar_t sessionName[64]{};
};

} // namespace

struct DxgiPresentStartPayload {
    std::uint64_t swapChain;
    std::uint32_t flags;
    std::int32_t syncInterval;
};

struct D3d9PresentStartPayload {
    std::uint64_t swapChain;
    std::uint32_t flags;
};

EtwFrameSession::~EtwFrameSession()
{
    stop();
}

std::uint64_t EtwFrameSession::qpcFrequency()
{
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    return static_cast<std::uint64_t>(frequency.QuadPart);
}

bool EtwFrameSession::start(std::string& error)
{
    if (m_running) {
        return true;
    }

    EventTraceProperties trace{};
    trace.properties.Wnode.BufferSize = sizeof(EventTraceProperties);
    trace.properties.Wnode.Flags = WNODE_FLAG_TRACED_GUID;
    trace.properties.Wnode.ClientContext = 1; // QPC: те же тики, что у QueryPerformanceCounter
    trace.properties.Wnode.Guid = kDxgiProvider;
    trace.properties.LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
    trace.properties.BufferSize = 64;
    trace.properties.MinimumBuffers = 256;
    trace.properties.MaximumBuffers = 1024;
    trace.properties.LoggerNameOffset = offsetof(EventTraceProperties, sessionName);

    // Сессия могла остаться от прошлого запуска, например после падения процесса.
    ControlTraceW(0, kSessionName, &trace.properties, EVENT_TRACE_CONTROL_STOP);

    trace.properties.Wnode.BufferSize = sizeof(EventTraceProperties);
    TRACEHANDLE sessionHandle = 0;
    ULONG status = StartTraceW(&sessionHandle, kSessionName, &trace.properties);
    if (status != ERROR_SUCCESS) {
        if (status == ERROR_ACCESS_DENIED) {
            error = "нужны права администратора для ETW-сессии";
        } else if (status == ERROR_ALREADY_EXISTS) {
            error = "ETW-сессия с таким именем уже запущена";
        } else {
            error = "StartTrace: " + win32ErrorText(status);
        }
        return false;
    }

    m_sessionHandle = sessionHandle;

    struct ProviderToEnable {
        const GUID* guid;
        const char* name;
    };
    const ProviderToEnable providers[] = {
        { &kDxgiProvider, "Microsoft-Windows-DXGI" },
        { &kD3d9Provider, "Microsoft-Windows-D3D9" },
    };

    for (const ProviderToEnable& provider : providers) {
        const ULONG enableStatus = EnableTraceEx2(sessionHandle, provider.guid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
            kInformationLevel, kPresentKeyword, 0, 0, nullptr);
        if (enableStatus != ERROR_SUCCESS) {
            error = std::string("EnableTraceEx2 ") + provider.name + ": " + win32ErrorText(enableStatus);
            stop();
            return false;
        }
    }

    EVENT_TRACE_LOGFILEW logFile{};
    logFile.LoggerName = const_cast<wchar_t*>(kSessionName);
    logFile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
    logFile.EventRecordCallback = &EtwFrameSession::eventRecordCallback;
    logFile.Context = this;

    const TRACEHANDLE traceHandle = OpenTraceW(&logFile);
    if (traceHandle == INVALID_PROCESSTRACE_HANDLE) {
        error = "OpenTrace: " + win32ErrorText(GetLastError());
        stop();
        return false;
    }

    m_traceHandle = traceHandle;
    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread([this]() {
        ProcessTrace(&m_traceHandle, 1, nullptr, nullptr);
    });

    return true;
}

void EtwFrameSession::stop()
{
    if (!m_running && m_sessionHandle == 0) {
        return;
    }

    m_stopRequested = true;

    if (m_sessionHandle != 0) {
        EventTraceProperties trace{};
        trace.properties.Wnode.BufferSize = sizeof(EventTraceProperties);
        trace.properties.LoggerNameOffset = offsetof(EventTraceProperties, sessionName);
        ControlTraceW(m_sessionHandle, kSessionName, &trace.properties, EVENT_TRACE_CONTROL_STOP);
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    if (m_traceHandle != 0) {
        CloseTrace(m_traceHandle);
        m_traceHandle = 0;
    }

    m_sessionHandle = 0;
    m_running = false;

    std::lock_guard<std::mutex> guard(m_queueMutex);
    m_queue.clear();
}

void __stdcall EtwFrameSession::eventRecordCallback(_EVENT_RECORD* record)
{
    if (record == nullptr || record->UserContext == nullptr) {
        return;
    }
    static_cast<EtwFrameSession*>(record->UserContext)->handleRecord(record);
}

void EtwFrameSession::handleRecord(const _EVENT_RECORD* event)
{
    if (m_stopRequested.load(std::memory_order_relaxed)) {
        return;
    }

    const EVENT_HEADER& header = event->EventHeader;
    const unsigned short id = header.EventDescriptor.Id;
    const std::uint32_t length = event->UserDataLength;
    const auto* payload = static_cast<const unsigned char*>(event->UserData);

    bool isPresent = false;
    std::uint64_t swapChain = 0;

    if (header.ProviderId == kDxgiProvider) {
        if (id == kDxgiPresentStart && payload != nullptr && length >= sizeof(DxgiPresentStartPayload)) {
            swapChain = reinterpret_cast<const DxgiPresentStartPayload*>(payload)->swapChain;
            isPresent = true;
        }
    } else if (header.ProviderId == kD3d9Provider) {
        if (id == kD3d9PresentStart && payload != nullptr && length >= sizeof(D3d9PresentStartPayload)) {
            swapChain = reinterpret_cast<const D3d9PresentStartPayload*>(payload)->swapChain;
            isPresent = true;
        }
    } else {
        return;
    }

    if (!isPresent) {
        m_eventsRejected.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    m_eventsReceived.fetch_add(1, std::memory_order_relaxed);

    std::lock_guard<std::mutex> guard(m_queueMutex);
    if (m_queue.size() >= kMaxQueuedSamples) {
        m_samplesDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    PresentSample sample;
    sample.qpc = static_cast<std::uint64_t>(header.TimeStamp.QuadPart);
    sample.processId = header.ProcessId;
    sample.threadId = header.ThreadId;
    sample.swapChain = swapChain;
    m_queue.push_back(sample);
}

std::size_t EtwFrameSession::drain(FrameTracker& tracker)
{
    std::vector<PresentSample> pending;
    {
        std::lock_guard<std::mutex> guard(m_queueMutex);
        if (m_queue.empty()) {
            return 0;
        }
        pending.swap(m_queue);
    }

    for (const PresentSample& sample : pending) {
        tracker.onPresent(sample);
    }
    return pending.size();
}

} // namespace yawa
