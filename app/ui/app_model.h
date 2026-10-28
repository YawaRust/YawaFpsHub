#pragma once

#include "metrics/bottleneck.h"
#include "metrics/diagnosis.h"
#include "metrics/metric_store.h"
#include "metrics/statistics.h"
#include "platform/sensors_win.h"
#include "render/d2d.h"
#include "ui/settings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yawa {

enum class View : std::uint8_t { Dashboard, Sensors, Overlay, Settings, Bottleneck, System };

// Что именно нажали в интерфейсе. Окно разбирает эти события и меняет состояние,
// сами экраны остаются «только рисующими».
enum class ActionKind : std::uint8_t {
    None,
    SwitchView,
    SetInterval,
    SetTargetHz,
    SetStyle,
    ToggleFollowGameMonitor,
    SetPosition,
    SetLayout,
    SetOverlaySurface,
    SetAccent,
    SetDensity,
    CycleGpu,
    ToggleOverlay,
    TogglePause,
    ToggleEtw,
    ToggleIdlePause,
    ToggleDiskNet,
    SelectLabel,
    ResetLabels,
    ToggleCompact,
    ToggleTray,
    OpenSettingsFolder,
    ResetSettings,
    SetScale,
    SetOpacity,
    ToggleMetric,
    ToggleSensor,
    EnableCpuSensors,
    ReorderMetric,
    OpenOverlayEditor,
    ExportOverlayRecipe,
    ImportOverlayRecipe,
    CreateDiagnosticReport,
    OpenDiagnosticReport,
    SetSensorFilter,
    ClearLabel,
    ResetOverlayPlacement,
    DragOverlay,
    ApplyLabelPreset,
    OffsetX,
    OffsetY,
    WindowMinimize,
    WindowMaximize,
    WindowClose,
    FocusSensorSearch,
    ToggleOverlayOnly,
    ToggleOverlayBackground,
    ToggleOverlayBorder,
    ToggleOverlayShadow,
    SetOverlayRadius,
    SetOverlayFont,
    ToggleOverlayBold,
    ToggleOverlayItalic,
    ChangeOverlayOutline,
    SelectGpu,
    ToggleGpuMenu,
    CaptureHotkey,
    // Расширенные настройки оформления оверлея.
    SetOverlayTextScale,
    SetOverlayBgOpacity,
    SetOverlayPadding,
    SetOverlayRowGap,
    SetValueColor,
    SetLabelColor,
    SetAccentColor,
    // Цвет конкретной строки: value = поле * 8 + (цвет + 1), 0 — «как в стиле».
    SetRowColor,
    SetBorderWidth,
    SetShadowStrength,
    SetLetterSpacing,
    SetUppercase,
    SetSeparators,
    SetFpsDecimals,
    ToggleOverlayClickThrough,
    // Полосы прокрутки: бегунок тянется мышью, дорожка листает на страницу.
    ScrollContentTrack,
    ScrollContentThumb,
    ScrollSensorsTrack,
    ScrollSensorsThumb,
};

struct UiHit
{
    Rect rect { };
    ActionKind action = ActionKind::None;
    int value = 0;
};

// Всё, что нужно экранам для отрисовки. Снимок собирается в потоке окна,
// поэтому экраны не трогают ни датчики, ни файлы.
struct AppModel
{
    MetricStore store;
    Settings settings;
    FpsSummary fps;
    // Всего кадров отрисовано активной игрой — строка «Отрисовано кадров».
    std::uint64_t framesPresented = 0;
    SensorAvailability availability;
    std::optional<AmdDdr4Timings> memoryTimings;
    std::vector<std::wstring> gpuNames;
    std::size_t activeGpu = 0;
    std::uint64_t elapsedMs = 0;
    double selfCpu = 0.0;
    double selfRamGb = 0.0;
    std::uint32_t selfThreads = 0;
    std::uint32_t cpuCores = 0;
    std::uint32_t cpuThreads = 0;
    std::wstring cpuName;
    bool etwRunning = false;
    bool paused = false;
    bool overlayVisible = false;
    // Текст последней ошибки оверлея: показывается в статус-баре вместо подсказок.
    std::wstring overlayError;
    View view = View::Dashboard;
    int editingLabel = -1;
    // -1 — все группы, иначе номер группы датчиков из списка экрана «Датчики».
    int sensorFilter = -1;
    // Пути и имя программы хранятся в UTF-8, как и остальные строки настроек.
    std::string settingsPath;
    std::string logPath;
    std::string sessionsPath;
    std::wstring foregroundApp;
    // Строка поиска на экране датчиков и фильтр «только в оверлее».
    std::string sensorQuery;
    bool sensorSearchActive = false;
    bool overlayOnly = false;
    // Редактор горячих клавиш: -1 — не редактируем, иначе номер действия (0…2).
    int hotkeyEditing = -1;
    // Выпадающий список выбора GPU на обзоре.
    bool gpuMenuOpen = false;
    int sensorScroll = 0;
    // Предел прокрутки таблицы датчиков: нужен, чтобы бегунок работал мышью.
    float sensorScrollMax = 0.0f;
    float sensorViewportHeight = 0.0f;
    Rect sensorScrollTrack { };
    // Прокрутка содержимого: экран рисуется в «демо-высоте», окно показывает его часть.
    float contentScroll = 0.0f;
    // Режим монитора, определённый при запуске: показываем его и берём из него цель кадра.
    int displayWidth = 0;
    int displayHeight = 0;
    int displayRefreshHz = 0;
    float contentHeight = 0.0f;
    // Состояние окна для своего заголовка: перетаскивание и кнопки рисуются вручную.
    bool maximized = false;
    // Паспорт машины собирается один раз при старте, узкое место пересчитывается
    // из последних показаний: экраны только рисуют то, что лежит в модели.
    HardwareIdentity hardware;
    std::vector<DiskSmartReading> diskHealth;
    BottleneckResult balance;
    std::vector<Finding> findings;
    GameDiagnosisInput diagnosisInput;
    std::wstring gameName;
    std::uint32_t gameProcessId = 0;
    double gameCpuLoad = -1.0;
    double gamePeakThreadLoad = -1.0;
    double gameGpuLoad = -1.0;
    float cursorX = -100.0f;
    float cursorY = -100.0f;
    bool cursorInside = false;
};

constexpr std::size_t kFavoriteHistoryPoints = 240;

// Ползунки тянутся мышью: обработчик нажатия должен знать их полный список.
inline bool isSliderAction(ActionKind action)
{
    switch (action) {
    case ActionKind::SetScale:
    case ActionKind::SetOpacity:
    case ActionKind::SetOverlayRadius:
    case ActionKind::SetOverlayTextScale:
    case ActionKind::SetOverlayBgOpacity:
    case ActionKind::SetOverlayPadding:
    case ActionKind::SetOverlayRowGap:
    case ActionKind::SetBorderWidth:
    case ActionKind::SetShadowStrength:
    case ActionKind::SetLetterSpacing:
        return true;
    default:
        return false;
    }
}

// Наведение считается по координатам курсора, которые окно обновляет на WM_MOUSEMOVE.
// Так экран остаётся «только рисующим» и не заводит собственного состояния.
inline bool isHovered(const AppModel& model, const Rect& rect)
{
    return model.cursorInside && rect.contains(model.cursorX, model.cursorY);
}

} // namespace yawa
