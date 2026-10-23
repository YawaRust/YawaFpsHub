#pragma once

#include "metrics/metric_store.h"
#include "metrics/statistics.h"
#include "platform/sensors_win.h"
#include "render/d2d.h"
#include "ui/settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace yawa {

enum class View : std::uint8_t { Dashboard, Sensors, Settings };
enum class ActionKind : std::uint8_t {
    None,
    SwitchView,
    SetInterval,
    SetStyle,
    SetPosition,
    SetLayout,
    SetAccent,
    CycleGpu,
    ToggleOverlay,
    SelectLabel,
    ResetLabels,
    ToggleCompact,
    ResetSettings,
    OpenSettingsFolder,
};

struct UiHit {
    Rect rect;
    ActionKind action = ActionKind::None;
    int value = 0;
};

// Снимок для отрисовки: UI читает только его, сборщик пишет только в него.
struct AppModel {
    MetricStore store;
    FpsSummary fps;
    Settings settings;
    View view = View::Dashboard;
    SensorAvailability availability;
    std::vector<std::wstring> gpuNames;
    std::size_t activeGpu = 0;
    std::uint64_t elapsedMs = 0;
    bool etwRunning = false;
    bool paused = false;
    int editingLabel = -1;
    std::string settingsPath;

    // Буфер под историю для графиков: mutable, чтобы отрисовка оставалась константной
    // и при этом не аллоцировала память на каждом кадре.
    mutable std::array<float, kHistoryCapacity> historyScratch{};

    MetricId chartFrameTime() const { return MetricId::FrameTime; }
};

} // namespace yawa
