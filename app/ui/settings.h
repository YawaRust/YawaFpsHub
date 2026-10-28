#pragma once

#include "metrics/metric_id.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace yawa {

// Набор пресетов взят из Fpsmon2.0: стиль меняет не только подложку, но и палитру,
// типографику, форму и компоновку оверлея.
enum class OverlayStyle : std::uint8_t { Neon, Rtss, Frosted, Terminal, Cyber, Mono, Esport, Retro };
enum class OverlayPosition : std::uint8_t { TopLeft, TopRight, BottomLeft, BottomRight };
enum class OverlayAnchorMode : std::uint8_t { Corner, Free };
enum class OverlayLayout : std::uint8_t { Plates, Line, Gauge };
enum class OverlaySurface : std::uint8_t { Glass, Plate, Outline, None };
// Первые восемь значений соответствуют старым OverlayStyle для миграции INI.
enum class OverlayRecipe : std::uint8_t {
    Neon,
    Rtss,
    Frosted,
    Terminal,
    Cyber,
    Mono,
    Esport,
    Retro,
    FpsLarge,
    Stream,
    Laptop,
    Hdr,
    Recording,
    Quiet,
    Focus,
    Clean
};
constexpr std::size_t kOverlayRecipeCount = 16;
enum class Accent : std::uint8_t { Mint, Ice, Violet, Amber, Coral };

enum class LabelField : std::uint8_t { Fps, Cpu, Gpu, Ram, Vram };
constexpr std::size_t kLabelCount = 5;
inline constexpr std::array<MetricId, kLabelCount> kOverlayPrimaryMetrics { MetricId::Fps, MetricId::CpuLoad,
    MetricId::GpuLoad, MetricId::RamUsed, MetricId::VramUsed };

std::array<bool, kMetricCount> defaultOverlayMetrics();
std::array<MetricId, kMetricCount> defaultOverlayOrder();
std::array<int, kMetricCount> defaultOverlayMetricColors();
std::optional<std::size_t> overlayColorGroup(MetricId id);

struct OverlayRowPosition
{
    int labelX = 12;
    int valueX = 252;
    int y = -1;
};

struct CaptureSettings
{
    int intervalMs = 1000;
    int smoothingPercent = 40;
    bool etwEnabled = true;
    bool pauseWhenIdle = true;
    bool includeDiskNet = true;
    // 60 Гц — самый частый случай: на 120-герцовом профиле бюджет кадра считался бы
    // проваленным для любой игры, которая честно держит 60 кадров.
    int targetHz = 60;
    // Авто: цель кадра берётся из герцовки монитора, определённой при запуске.
    bool targetAuto = true;
};

struct OverlaySettings
{
    OverlayPosition position = OverlayPosition::TopLeft;
    // Старые INI остаются привязаны к углу; свободное положение включается при перетаскивании плашки.
    OverlayAnchorMode anchorMode = OverlayAnchorMode::Corner;
    int originX = 0;
    int originY = 0;
    // Опциональная привязка новых свободных размещений к устройству дисплея.
    // originX/Y остаются резервной позицией для старого INI и отсутствующего монитора.
    std::string savedMonitorId;
    int monitorOriginX = 0;
    int monitorOriginY = 0;
    bool followGameMonitor = false;
    bool showSparkline = false;
    MetricId sparklineMetric = MetricId::Fps;
    OverlayLayout layout = OverlayLayout::Plates;
    OverlayStyle style = OverlayStyle::Frosted; // базовая палитра старых стилей
    OverlayRecipe recipe = OverlayRecipe::Frosted;
    OverlaySurface surface = OverlaySurface::Glass;
    bool surfaceActive = false; // старый INI не должен менять вид до явного выбора оси
    int scalePercent = 100;
    int opacityPercent = 72;
    // Ручной сдвиг от выбранного угла: даёт поставить оверлей точно по месту
    // независимо от разрешения и рамок экрана.
    int offsetX = 0;
    int offsetY = 0;
    // Оформление поверх пресета: то, что пользователь может убрать или подкрутить.
    bool showBackground = true;
    bool showBorder = true;
    bool showShadow = true;
    // -1 — радиус из пресета, иначе 0…24 в тех же единицах, что и стиль.
    int cornerRadius = -1;
    // Типографика: отдельный редактор текста, как в RivaTuner и FPS Monitor.
    // 0 — шрифт интерфейса, 1 — моноширинный, 2 — узкий, 3 — плотный.
    int fontFamily = 0;
    bool fontBold = false;
    bool fontItalic = false;
    // 0 — без обводки, 1…3 — толщина в пикселях.
    int textOutline = 0;
    // Размер именно шрифта: плашка может остаться компактной, а цифры — крупнее.
    int textScalePercent = 100;
    // -1 — значение из пресета. Иначе подложка получает свою прозрачность (0 — без фона).
    int backgroundOpacityPercent = -1;
    // -1 — отступы и межстрочный интервал из пресета, иначе ручные (px).
    int paddingPx = -1;
    int rowGapPx = -1;
    // -1 — цвет из пресета, 0…5 — цвет палитры для значений и для подписей.
    int valueColor = -1;
    int labelColor = -1;
    // Цвет каждой строки отдельно: в пресетах Fpsmon строки различаются цветом.
    std::array<int, kLabelCount> rowColors { -1, -1, -1, -1, -1 };
    // -1 — акцент из пресета, иначе цвет палитры для линий, рамки и заголовков.
    int accentColor = -1;
    // #RRGGBBAA у конкретного элемента имеет приоритет над старым индексом 0…5.
    std::optional<std::uint32_t> valueRgba;
    std::optional<std::uint32_t> labelRgba;
    std::optional<std::uint32_t> accentRgba;
    std::array<std::optional<std::uint32_t>, kLabelCount> rowRgba { };
    // -1 — толщина из пресета, 0 — без рамки, 1…3 — толщина в пикселях.
    int borderWidth = -1;
    // Сила тени и межбуквенный интервал — в процентах от пресета.
    int shadowStrength = 100;
    int letterSpacing = 100;
    // -1 — как в пресете, 0 — обычные подписи, 1 — заглавными.
    int uppercaseMode = -1;
    // -1 — как в пресете, 0 — без разделителей строк, 1 — с разделителями.
    int separatorsMode = -1;
    // -1 — как в пресете, иначе 0…2 знака после точки у кадров в секунду в оверлее.
    int fpsDecimals = -1;
    // Сквозной клик: окно оверлея полностью пропускает мышь в игру. Выключается хоткеем,
    // когда оверлей нужно перетащить.
    bool clickThrough = false;
    // visible оставлен только для переноса настроек из старого ini.
    std::array<bool, kMetricCount> metrics = defaultOverlayMetrics();
    std::array<MetricId, kMetricCount> order = defaultOverlayOrder();
    bool pixelLayout = false;
    std::array<OverlayRowPosition, kMetricCount> rowPositions { };
    std::array<int, kMetricCount> metricColors = defaultOverlayMetricColors(); // -1 — палитра стиля
    std::array<std::optional<std::uint32_t>, kMetricCount> metricRgba { };
    std::array<bool, kMetricCount> customLabelSet { };
    std::array<std::string, kMetricCount> customLabels { };
    std::array<bool, kLabelCount> visible { true, true, true, true, true };
    std::array<std::string, kLabelCount> labels { "FPS", "CPU", "GPU", "RAM", "VRAM" };
};

struct UiSettings
{
    Accent accent = Accent::Mint;
    bool compactDensity = false;
    bool animate = true;
    // По умолчанию приложение просто закрывается: неожиданный уход в трей пугает,
    // и человек ищет окно, которого больше нет.
    bool closeToTray = false;
};

struct GpuSelection
{
    bool hasSelection = false;
    std::uint32_t luidLow = 0;
    std::int32_t luidHigh = 0;
};

// Горячие клавиши хранятся как виртуальные коды Windows: F10 = 0x79, F11 = 0x7A, F12 = 0x7B.
// Ноль означает «выключено» — клавиша не регистрируется.
struct HotkeySettings
{
    unsigned int overlay = 0x79;
    unsigned int position = 0x7A;
    unsigned int window = 0x7B;
    // F9 переключает сквозной клик по оверлею.
    unsigned int clickThrough = 0x78;
};

struct Settings
{
    CaptureSettings capture;
    OverlaySettings overlay;
    UiSettings ui;
    GpuSelection gpu;
    HotkeySettings hotkeys;
};

bool parseOverlayRgba(std::string_view text, std::uint32_t& rgba);
std::string overlayRgbaHex(std::uint32_t rgba);
Settings defaultSettings();
void moveOverlayMetric(OverlaySettings& settings, MetricId moved, MetricId before);
void enableOverlayPixelLayout(OverlaySettings& settings);
// Перевод смещения между экраном и сценой редактора (обе стороны используют одни размеры).
int scaledOverlayOffset(int offset, double sourceExtent, double targetExtent);
void setOverlayMetricLabel(OverlaySettings& settings, MetricId id, std::string label);

// Формат — docs/settings.md. Неизвестные ключи пропускаются, отсутствующие остаются
// значениями по умолчанию: файл мог остаться от прошлой версии.
bool parseSettings(std::string_view text, Settings& target, std::string& error);
std::string serializeSettings(const Settings& settings);

std::string_view labelKey(LabelField field);
const std::string& labelValue(const OverlaySettings& overlay, LabelField field);
void setLabel(OverlaySettings& overlay, LabelField field, std::string value);

// Ставит переключатели оформления в значения пресета: у RTSS нет ни фона, ни рамки,
// у фростед-стекла есть и то, и другое.
void applyStyleDefaults(OverlaySettings& overlay, OverlayStyle style);

struct OverlayRecipeSpec
{
    std::string_view key;
    OverlayStyle palette;
    OverlayLayout shape;
    OverlaySurface surface;
    int fontFamily;
    int scalePercent;
    bool onlyFps;
};
const std::array<OverlayRecipeSpec, kOverlayRecipeCount>& overlayRecipes();
void applyOverlayRecipe(OverlaySettings& overlay, OverlayRecipe recipe);
void setOverlaySurface(OverlaySettings& overlay, OverlaySurface surface);
bool parseOverlayRecipe(std::string_view key, OverlayRecipe& recipe);
std::string_view overlayRecipeKey(OverlayRecipe recipe);
std::string_view overlaySurfaceKey(OverlaySurface surface);
bool parseOverlaySurface(std::string_view key, OverlaySurface& surface);
// Только белый список полей оформления; не включает имя игры, монитор или показания датчиков.
std::string serializeOverlayRecipeJson(const OverlaySettings& overlay);
// Импорт только осей и общих цветов; при ошибке overlay остаётся неизменным.
bool importOverlayRecipeJson(std::string_view json, OverlaySettings& overlay, std::string& error);
// Поддерживает только однозначно сопоставимые текстовые датчики RTSS OverlayEditor v2.
bool importRtssOverlay(std::string_view text, OverlaySettings& overlay, std::string& error);
void copyOverlayPlacement(OverlaySettings& target, const OverlaySettings& source);
OverlaySettings overlayForGame(const OverlaySettings& base, const OverlaySettings& profile);
std::string gameProfileKey(std::wstring_view processName);

std::string_view overlayStyleKey(OverlayStyle style);
std::string_view overlayPositionKey(OverlayPosition position);
std::string_view overlayLayoutKey(OverlayLayout layout);
std::string_view accentKey(Accent accent);

bool parseOverlayStyle(std::string_view text, OverlayStyle& style);
bool parseOverlayPosition(std::string_view text, OverlayPosition& position);
bool parseOverlayLayout(std::string_view text, OverlayLayout& layout);
bool parseAccent(std::string_view text, Accent& accent);

} // namespace yawa
