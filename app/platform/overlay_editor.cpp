#include "platform/overlay_editor.h"

#include "metrics/overlay_metric.h"
#include "metrics/overlay_rows.h"
#include "platform/overlay_monitor.h"
#include "platform/win_util.h"
#include "render/overlay_painter.h"
#include "ui/canvas_transform.h"
#include "ui/layout.h"
#include "ui/overlay_view.h"

#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cwctype>
#include <string>
#include <utility>

namespace yawa {
namespace {
    constexpr wchar_t kEditorClass[] = L"YawaFpsHubOverlayEditor";
    constexpr int kCanvasWidth = 356;
    constexpr int kMaxY = 1490;
    constexpr int kControlIdLabel = 2001;
    constexpr int kControlIdLabelX = 2002;
    constexpr int kControlIdValueX = 2003;
    constexpr int kControlIdY = 2004;
    constexpr std::array<std::wstring_view, 4> kSections { L"Стили", L"Вид", L"Текст", L"Строки" };
    constexpr std::array<ActionKind, 4> kSectionActions { ActionKind::SetStyle, ActionKind::SetPosition,
        ActionKind::SetOverlayFont, ActionKind::ReorderMetric };

    struct NumericSlider
    {
        int minimum;
        int maximum;
        int value;
    };

    NumericSlider numericSlider(const OverlaySettings& settings, ActionKind action)
    {
        switch (action) {
        case ActionKind::SetScale:
            return { 70, 160, settings.scalePercent };
        case ActionKind::SetOpacity:
            return { 0, 100, settings.opacityPercent };
        case ActionKind::SetOverlayRadius:
            return { 0, 24, settings.cornerRadius < 0 ? 8 : settings.cornerRadius };
        case ActionKind::SetOverlayTextScale:
            return { 85, 130, settings.textScalePercent };
        case ActionKind::SetOverlayBgOpacity:
            return { 0, 100,
                settings.backgroundOpacityPercent < 0 ? 100 : settings.backgroundOpacityPercent };
        case ActionKind::SetOverlayPadding:
            return { 0, 28, settings.paddingPx < 0 ? 10 : settings.paddingPx };
        case ActionKind::SetOverlayRowGap:
            return { 0, 14, settings.rowGapPx < 0 ? 3 : settings.rowGapPx };
        case ActionKind::SetBorderWidth:
            return { 0, 3, settings.borderWidth < 0 ? 1 : settings.borderWidth };
        case ActionKind::SetShadowStrength:
            return { 0, 100, settings.shadowStrength };
        case ActionKind::SetLetterSpacing:
            return { 0, 200, settings.letterSpacing };
        default:
            return { 0, 0, 0 };
        }
    }

    void setNumericSlider(OverlaySettings& settings, ActionKind action, int value)
    {
        switch (action) {
        case ActionKind::SetScale:
            settings.scalePercent = value;
            break;
        case ActionKind::SetOpacity:
            settings.opacityPercent = value;
            break;
        case ActionKind::SetOverlayRadius:
            settings.cornerRadius = value;
            break;
        case ActionKind::SetOverlayTextScale:
            settings.textScalePercent = value;
            break;
        case ActionKind::SetOverlayBgOpacity:
            settings.backgroundOpacityPercent = value;
            break;
        case ActionKind::SetOverlayPadding:
            settings.paddingPx = value;
            break;
        case ActionKind::SetOverlayRowGap:
            settings.rowGapPx = value;
            break;
        case ActionKind::SetBorderWidth:
            settings.borderWidth = value;
            break;
        case ActionKind::SetShadowStrength:
            settings.shadowStrength = value;
            break;
        case ActionKind::SetLetterSpacing:
            settings.letterSpacing = value;
            break;
        default:
            break;
        }
    }

    std::vector<MetricId> hiddenLayerRows(
        const std::vector<MetricId>& known, const std::vector<OverlayMetricRow>& visible)
    {
        std::vector<MetricId> result;
        for (MetricId id : known) {
            if (std::none_of(visible.begin(), visible.end(),
                    [id](const OverlayMetricRow& row) { return row.id == id; })) {
                result.push_back(id);
            }
        }
        return result;
    }

    WNDPROC numericEditOriginal = nullptr;

    LRESULT CALLBACK numericEditProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_KEYDOWN && (wParam == VK_RETURN || wParam == VK_ESCAPE)) {
            if (wParam == VK_ESCAPE)
                SetWindowTextW(window, L"");
            SetFocus(GetParent(window));
            return 0;
        }
        return CallWindowProcW(numericEditOriginal, window, message, wParam, lParam);
    }

    void input(HWND window, const wchar_t* value)
    {
        if (window) {
            SetWindowTextW(window, value);
        }
    }
} // namespace

OverlayEditor::~OverlayEditor()
{
    close();
}

void OverlayEditor::close()
{
    if (m_window) {
        DestroyWindow(m_window);
    }
}

void OverlayEditor::refresh()
{
    if (m_window) {
        InvalidateRect(m_window, nullptr, FALSE);
    }
}

bool OverlayEditor::open(HWND owner, HINSTANCE instance, AppModel& model, const Theme& theme,
    std::function<void(const UiHit&, float, bool)> onAction, std::function<void(wchar_t)> onText,
    std::function<void(bool)> onChange)
{
    if (m_window) {
        ShowWindow(m_window, SW_RESTORE);
        SetForegroundWindow(m_window);
        return true;
    }
    std::string error;
    if (!m_renderer.initialize(error)) {
        return false;
    }
    WNDCLASSW cls { };
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.lpszClassName = kEditorClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        m_renderer.shutdown();
        return false;
    }
    m_model = &model;
    m_dpiScale = static_cast<float>(GetDpiForWindow(owner)) / 96.0f;
    m_historyCommitted = model.settings.overlay;
    m_layersScroll = 0;
    m_panelScroll = 0;
    m_advancedControls = false;
    m_hiddenLayers.clear();
    m_samplingColor = false;
    m_undo.clear();
    m_redo.clear();
    m_theme = &theme;
    m_onAction = std::move(onAction);
    m_onText = std::move(onText);
    m_onChange = std::move(onChange);
    m_window = CreateWindowExW(WS_EX_APPWINDOW, kEditorClass, L"Мастерская оверлея · YawaFpsHub",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        static_cast<int>(std::lround(1320.0f * m_dpiScale)),
        static_cast<int>(std::lround(920.0f * m_dpiScale)), owner, nullptr, instance, this);
    if (!m_window) {
        m_renderer.shutdown();
        m_model = nullptr;
        return false;
    }
    m_dpiScale = static_cast<float>(GetDpiForWindow(m_window)) / 96.0f;
    const auto createEdit = [this, instance](int id, DWORD style) {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0,
            0, 10, 24, m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    m_labelEdit = createEdit(kControlIdLabel, ES_AUTOHSCROLL);
    m_labelX = createEdit(kControlIdLabelX, ES_NUMBER | ES_AUTOHSCROLL);
    m_valueX = createEdit(kControlIdValueX, ES_NUMBER | ES_AUTOHSCROLL);
    m_y = createEdit(kControlIdY, ES_NUMBER | ES_AUTOHSCROLL);
    m_alphaEdit = createEdit(2006, ES_NUMBER | ES_AUTOHSCROLL);
    m_hexEdit = createEdit(2008, ES_AUTOHSCROLL);
    m_numericEdit = createEdit(2007, ES_NUMBER | ES_AUTOHSCROLL);
    if (m_numericEdit) {
        numericEditOriginal = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(m_numericEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(numericEditProc)));
    }
    m_catalogSearch = createEdit(2005, ES_AUTOHSCROLL);
    if (!m_labelEdit || !m_labelX || !m_valueX || !m_y || !m_alphaEdit || !m_hexEdit || !m_numericEdit
        || !m_catalogSearch) {
        close();
        return false;
    }
    SendMessageW(m_labelEdit, EM_SETLIMITTEXT, 64, 0);
    SendMessageW(m_hexEdit, EM_SETLIMITTEXT, 9, 0);
    SendMessageW(m_catalogSearch, EM_SETLIMITTEXT, 64, 0);
    ShowWindow(m_catalogSearch, SW_HIDE);
    ShowWindow(m_numericEdit, SW_HIDE);
    m_numericAction = ActionKind::None;
    layout();
    const auto rows = overlayMetricRows(model.settings.overlay, model.store);
    if (!rows.empty()) {
        select(rows.front().id, false);
    }
    ShowWindow(m_window, SW_SHOW);
    m_onChange(true);
    return true;
}

LRESULT CALLBACK OverlayEditor::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    auto* editor = reinterpret_cast<OverlayEditor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return editor ? editor->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

void OverlayEditor::layout()
{
    RECT client { };
    GetClientRect(m_window, &client);
    const float height = static_cast<float>(client.bottom) / m_dpiScale;
    const float width = static_cast<float>(client.right) / m_dpiScale;
    const float screenWidth = static_cast<float>(std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)));
    const float screenHeight = static_cast<float>(std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN)));
    const float canvasWidth = std::min(
        static_cast<float>(kCanvasWidth), std::max(100.0f, height - 520.0f) * screenWidth / screenHeight);
    const float canvasHeight = canvasWidth * screenHeight / screenWidth;
    m_canvas = Rect { 184.0f, 307.0f, 184.0f + canvasWidth, 307.0f + canvasHeight };
    m_inspector = Rect { 20.0f, height - 198.0f, 540.0f, height - 20.0f };
    m_layers = Rect { 20.0f, 307.0f, 174.0f, m_inspector.top - 12.0f };
    m_controls = Rect { 564.0f, 112.0f, width - 18.0f, height - 20.0f };
    m_catalog = Rect { width - 386.0f, 50.0f, width - 18.0f, std::min(height - 22.0f, 462.0f) };
    m_panelScroll = m_advancedControls
        ? std::clamp(m_panelScroll, 0,
              std::max(0,
                  static_cast<int>(
                      overlayEditorControlsHeight(m_model->settings.overlay) - m_controls.height())))
        : 0;
    if (!m_labelEdit) {
        return;
    }
    const float y = m_inspector.top;
    const auto px
        = [this](float coordinate) { return static_cast<int>(std::lround(coordinate * m_dpiScale)); };
    SetWindowPos(m_labelEdit, nullptr, px(112), px(y + 44), px(390), px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_labelX, nullptr, px(106), px(y + 92), px(58), px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_valueX, nullptr, px(275), px(y + 92), px(58), px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_y, nullptr, px(408), px(y + 92), px(58), px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_alphaEdit, nullptr, px(472), px(y + 139), px(42), px(24), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_hexEdit, nullptr, px(112), px(y + 72), px(138), px(20), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(
        m_catalogSearch, nullptr, px(width - 380), px(16), px(256), px(26), SWP_NOZORDER | SWP_NOACTIVATE);
}

void OverlayEditor::syncFields()
{
    if (!m_model || !m_labelEdit) {
        return;
    }
    const OverlaySettings& settings = m_model->settings.overlay;
    const auto row = canvasPosition(m_selected);
    std::string label(metricInfo(m_selected).label);
    for (const auto& entry : overlayMetricRows(settings, m_model->store)) {
        if (entry.id == m_selected) {
            label = entry.label;
            break;
        }
    }
    m_syncing = true;
    const std::wstring shown = widen(label);
    input(m_labelEdit, shown.c_str());
    input(m_labelX, std::to_wstring(row.labelX).c_str());
    input(m_valueX, std::to_wstring(row.valueX).c_str());
    input(m_y, std::to_wstring(row.y).c_str());
    const auto rgba = settings.metricRgba[static_cast<std::size_t>(m_selected)];
    input(m_alphaEdit, std::to_wstring(rgba ? ((*rgba & 0xFFu) * 100u + 127u) / 255u : 100u).c_str());
    const std::wstring hex = rgba ? widen(overlayRgbaHex(*rgba)) : L"";
    input(m_hexEdit, hex.c_str());
    m_syncing = false;
    const bool rowsMovable = settings.layout == OverlayLayout::Plates;
    EnableWindow(m_labelX, rowsMovable);
    EnableWindow(m_valueX, rowsMovable);
    EnableWindow(m_y, rowsMovable);
}

void OverlayEditor::select(MetricId id, bool value)
{
    m_selected = id;
    m_value = value;
    m_model->editingLabel = -1;
    syncFields();
    refresh();
}

void OverlayEditor::changed(bool finished)
{
    if (finished) {
        auto& placement = m_model->settings.overlay;
        if (placement.anchorMode == OverlayAnchorMode::Free
            && (placement.originX != m_historyCommitted.originX
                || placement.originY != m_historyCommitted.originY
                || m_historyCommitted.anchorMode != OverlayAnchorMode::Free)) {
            rememberOverlayMonitor(placement, overlayVirtualScreen());
        }
        recordHistory();
    }
    if (m_onChange) {
        m_onChange(finished);
    }
    refresh();
}

void OverlayEditor::recordHistory()
{
    Settings previous;
    previous.overlay = m_historyCommitted;
    Settings current;
    current.overlay = m_model->settings.overlay;
    if (serializeSettings(previous) == serializeSettings(current)) {
        return;
    }
    m_undo.push_back(m_historyCommitted);
    if (m_undo.size() > 30) {
        m_undo.erase(m_undo.begin());
    }
    m_redo.clear();
    m_historyCommitted = current.overlay;
    refresh();
}

void OverlayEditor::restoreHistory(bool redo)
{
    if (m_dragging || m_panning) {
        return;
    }
    auto& source = redo ? m_redo : m_undo;
    auto& destination = redo ? m_undo : m_redo;
    if (source.empty()) {
        return;
    }
    destination.push_back(m_model->settings.overlay);
    m_model->settings.overlay = source.back();
    source.pop_back();
    m_historyCommitted = m_model->settings.overlay;
    m_selectedRows.clear();
    m_model->editingLabel = -1;
    const auto rows = overlayMetricRows(m_model->settings.overlay, m_model->store);
    if (!rows.empty()
        && std::none_of(rows.begin(), rows.end(), [this](const auto& row) { return row.id == m_selected; })) {
        m_selected = rows.front().id;
    }
    syncFields();
    if (m_onChange) {
        m_onChange(true);
    }
    refresh();
}

OverlayRowPosition OverlayEditor::canvasPosition(MetricId id) const
{
    const auto& settings = m_model->settings.overlay;
    const auto position = settings.rowPositions[static_cast<std::size_t>(id)];
    if (settings.pixelLayout && position.y >= 0) {
        return position;
    }
    int index = 0;
    for (const auto& row : overlayMetricRows(settings, m_model->store)) {
        if (row.id == id) {
            break;
        }
        ++index;
    }
    return OverlayRowPosition { 12, 252, std::min(12 + index * 28, kMaxY) };
}

void OverlayEditor::preparePixelLayout()
{
    auto& settings = m_model->settings.overlay;
    if (settings.pixelLayout) {
        return;
    }
    const auto rows = overlayMetricRows(settings, m_model->store);
    for (const auto& row : rows) {
        settings.rowPositions[static_cast<std::size_t>(row.id)] = canvasPosition(row.id);
    }
    settings.pixelLayout = true;
}

float OverlayEditor::canvasScale() const
{
    return m_canvas.width() * m_canvasZoom
        / static_cast<float>(std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)));
}

int OverlayEditor::logicalCoordinate(int pixels) const
{
    return static_cast<int>(std::lround(static_cast<float>(pixels) / m_dpiScale));
}

Rect OverlayEditor::plateOnCanvas() const
{
    const CanvasAxis horizontal { m_canvas.left, m_panX, canvasScale() };
    const CanvasAxis vertical { m_canvas.top, m_panY, canvasScale() };
    return Rect { horizontal.toCanvas(m_screenPlate.left), vertical.toCanvas(m_screenPlate.top),
        horizontal.toCanvas(m_screenPlate.right), vertical.toCanvas(m_screenPlate.bottom) };
}

void OverlayEditor::move(int x, int y)
{
    if (!m_dragging) {
        return;
    }
    auto& settings = m_model->settings.overlay;
    m_guideX = m_guideY = -1.0f;
    const bool grid = GetKeyState(VK_SHIFT) < 0;
    if (m_dragPlate) {
        const int screenWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int screenHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        const int plateWidth = static_cast<int>(std::lround(m_screenPlate.width()));
        const int plateHeight = static_cast<int>(std::lround(m_screenPlate.height()));
        int nextX = m_originStartX + static_cast<int>(std::lround((x - m_dragStartX) / canvasScale()));
        int nextY = m_originStartY + static_cast<int>(std::lround((y - m_dragStartY) / canvasScale()));
        if (grid) {
            nextX = snapGrid(nextX);
            nextY = snapGrid(nextY);
        } else {
            const std::array<int, 5> xGuides { 0, screenWidth / 20, screenWidth / 2, screenWidth * 19 / 20,
                screenWidth };
            const std::array<int, 5> yGuides { 0, screenHeight / 20, screenHeight / 2, screenHeight * 19 / 20,
                screenHeight };
            const int threshold = std::max(1, static_cast<int>(std::lround(6.0f / canvasScale())));
            const AxisSnap snappedX = snapToGuides(nextX, plateWidth, xGuides, threshold);
            const AxisSnap snappedY = snapToGuides(nextY, plateHeight, yGuides, threshold);
            nextX = snappedX.value;
            nextY = snappedY.value;
            if (snappedX.guide >= 0) {
                m_guideX = CanvasAxis { m_canvas.left, m_panX, canvasScale() }.toCanvas(
                    static_cast<float>(snappedX.guide));
            }
            if (snappedY.guide >= 0) {
                m_guideY = CanvasAxis { m_canvas.top, m_panY, canvasScale() }.toCanvas(
                    static_cast<float>(snappedY.guide));
            }
        }
        settings.originX = std::clamp(nextX, 0, std::max(0, screenWidth - plateWidth));
        settings.originY = std::clamp(nextY, 0, std::max(0, screenHeight - plateHeight));
    } else if (m_dragLine) {
        const int nextX = m_offsetStartX + static_cast<int>(std::lround((x - m_dragStartX) / canvasScale()));
        const int nextY = m_offsetStartY + static_cast<int>(std::lround((y - m_dragStartY) / canvasScale()));
        settings.offsetX = std::clamp(grid ? snapGrid(nextX) : nextX, -8192, 8192);
        settings.offsetY = std::clamp(grid ? snapGrid(nextY) : nextY, -8192, 8192);
    } else {
        int deltaX = static_cast<int>(std::lround((x - m_dragStartX) / m_rowDragScale));
        int deltaY = static_cast<int>(std::lround((y - m_dragStartY) / m_rowDragScale));
        if (!m_dragPositions.empty()) {
            const auto& primary = m_dragPositions.front();
            if (grid) {
                deltaX = snapGrid(primary.labelX + deltaX) - primary.labelX;
                deltaY = snapGrid(primary.y + deltaY) - primary.y;
            } else {
                std::vector<int> baselines;
                for (const auto& row : overlayMetricRows(settings, m_model->store)) {
                    if (std::find(m_selectedRows.begin(), m_selectedRows.end(), row.id)
                        == m_selectedRows.end()) {
                        baselines.push_back(canvasPosition(row.id).y);
                    }
                }
                const int threshold = 4; // интервалы строк 28 px: зона привязки не перекрывает всю шкалу
                const std::array<int, 3> columns { 0, 235, 470 };
                const AxisSnap snappedX = snapToGuides(primary.labelX + deltaX, 0, columns, threshold);
                const AxisSnap snappedY = snapToGuides(primary.y + deltaY, 0, baselines, threshold);
                deltaX = snappedX.value - primary.labelX;
                deltaY = snappedY.value - primary.y;
                if (snappedX.guide >= 0) {
                    m_guideX = plateOnCanvas().left + snappedX.guide * m_rowDragScale;
                }
                if (snappedY.guide >= 0) {
                    m_guideY = plateOnCanvas().top + snappedY.guide * m_rowDragScale;
                }
            }
        }
        for (const auto& position : m_dragPositions) {
            deltaX = std::clamp(deltaX, -std::min(position.labelX, position.valueX),
                470 - std::max(position.labelX, position.valueX));
            deltaY = std::clamp(deltaY, -position.y, kMaxY - position.y);
        }
        for (std::size_t index = 0; index < m_selectedRows.size(); ++index) {
            auto& row = settings.rowPositions[static_cast<std::size_t>(m_selectedRows[index])];
            row.labelX = m_dragPositions[index].labelX + deltaX;
            row.valueX = m_dragPositions[index].valueX + deltaX;
            row.y = m_dragPositions[index].y + deltaY;
        }
        syncFields();
    }
    changed(false);
}

void OverlayEditor::editCoordinate(HWND field)
{
    if (m_syncing || !field || m_model->settings.overlay.layout != OverlayLayout::Plates) {
        return;
    }
    wchar_t buffer[24] { };
    GetWindowTextW(field, buffer, 24);
    wchar_t* end = nullptr;
    const long number = std::wcstol(buffer, &end, 10);
    if (end != buffer && *end == L'\0') {
        preparePixelLayout();
        auto& row = m_model->settings.overlay.rowPositions[static_cast<std::size_t>(m_selected)];
        int& selected = field == m_labelX ? row.labelX : field == m_valueX ? row.valueX : row.y;
        selected = std::clamp(static_cast<int>(number), 0, field == m_y ? kMaxY : 470);
        m_model->settings.overlay.pixelLayout = true;
        changed(true);
    }
    syncFields();
}

std::vector<MetricId> OverlayEditor::catalogRows() const
{
    wchar_t query[65] { };
    if (m_catalogSearch) {
        GetWindowTextW(m_catalogSearch, query, 65);
    }
    std::wstring filter(query);
    const auto fold = [](std::wstring& value) {
        for (wchar_t& symbol : value) {
            symbol = symbol >= L'А' && symbol <= L'Я' ? static_cast<wchar_t>(symbol + 32)
                : symbol == L'Ё'                      ? L'ё'
                                                      : static_cast<wchar_t>(std::towlower(symbol));
        }
    };
    fold(filter);
    std::vector<MetricId> result;
    for (const MetricInfo& info : kMetricTable) {
        if (displayMetric(info.id) != info.id) {
            continue;
        }
        std::wstring label = widen(info.label);
        std::wstring key = widen(info.key);
        fold(label);
        fold(key);
        if (filter.empty() || label.find(filter) != std::wstring::npos
            || key.find(filter) != std::wstring::npos) {
            result.push_back(info.id);
        }
    }
    return result;
}

const UiHit* OverlayEditor::hitAt(float x, float y) const
{
    if (!m_controls.contains(x, y)) {
        return nullptr;
    }
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) {
        if (it->rect.contains(x, y)) {
            return &*it;
        }
    }
    return nullptr;
}

LRESULT OverlayEditor::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CLOSE:
        close();
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT cursor { };
            if (GetCursorPos(&cursor) && ScreenToClient(m_window, &cursor)) {
                cursor.x = logicalCoordinate(cursor.x);
                cursor.y = logicalCoordinate(cursor.y);
                if (m_canvas.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))
                    && (GetKeyState(VK_SPACE) < 0 || m_panning
                        || plateOnCanvas().contains(
                            static_cast<float>(cursor.x), static_cast<float>(cursor.y)))) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
                    return TRUE;
                }
            }
        }
        break;
    case WM_NCDESTROY: {
        const HWND window = m_window;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        m_window = nullptr;
        m_labelEdit = m_labelX = m_valueX = m_y = m_alphaEdit = m_hexEdit = m_numericEdit = m_catalogSearch
            = nullptr;
        m_renderer.shutdown();
        m_model = nullptr;
        m_hits.clear();
        m_onAction = { };
        m_onText = { };
        m_onChange = { };
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_SIZE:
        m_renderer.resize(LOWORD(lParam), HIWORD(lParam), m_dpiScale);
        if (m_model) {
            layout();
        }
        refresh();
        return 0;
    case WM_DPICHANGED: {
        m_dpiScale = static_cast<float>(HIWORD(wParam)) / 96.0f;
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(m_window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
            suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        layout();
        refresh();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize
            = POINT { static_cast<LONG>(1000.0f * m_dpiScale), static_cast<LONG>(720.0f * m_dpiScale) };
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps { };
        BeginPaint(m_window, &ps);
        paint();
        EndPaint(m_window, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(m_window, &cursor);
        cursor.x = logicalCoordinate(cursor.x);
        cursor.y = logicalCoordinate(cursor.y);
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 65;
        if (const UiHit* hit = hitAt(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
            hit && isSliderAction(hit->action)) {
            const auto slider = numericSlider(m_model->settings.overlay, hit->action);
            const int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
            setNumericSlider(m_model->settings.overlay, hit->action,
                std::clamp(slider.value + (delta > 0 ? step : -step), slider.minimum, slider.maximum));
            changed(true);
            return 0;
        }
        if (m_layers.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))) {
            const auto rows = overlayMetricRows(m_model->settings.overlay, m_model->store);
            const int visible = std::max(1, static_cast<int>((m_layers.height() - 38.0f) / 28.0f));
            m_layersScroll = std::clamp(m_layersScroll - delta / 28, 0,
                std::max(0,
                    static_cast<int>(rows.size() + hiddenLayerRows(m_hiddenLayers, rows).size()) - visible));
        } else if (m_catalogOpen
            && m_catalog.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))) {
            m_catalogScroll = std::clamp(
                m_catalogScroll - delta / 28, 0, std::max(0, static_cast<int>(catalogRows().size()) - 12));
        } else if (m_canvas.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))) {
            const float previous = canvasScale();
            m_canvasZoom = std::clamp(m_canvasZoom * (delta > 0 ? 1.15f : 1.0f / 1.15f), 0.75f, 6.0f);
            m_panX = CanvasAxis { m_canvas.left, m_panX, previous }.panAtZoom(
                static_cast<float>(cursor.x), canvasScale());
            m_panY = CanvasAxis { m_canvas.top, m_panY, previous }.panAtZoom(
                static_cast<float>(cursor.y), canvasScale());
        } else if (m_advancedControls
            && m_controls.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))) {
            m_panelScroll = std::clamp(m_panelScroll - delta, 0,
                std::max(0,
                    static_cast<int>(
                        overlayEditorControlsHeight(m_model->settings.overlay) - m_controls.height())));
        }
        refresh();
        return 0;
    }
    case WM_COMMAND: {
        const HWND field = reinterpret_cast<HWND>(lParam);
        if (field == m_catalogSearch && HIWORD(wParam) == EN_CHANGE) {
            m_catalogScroll = 0;
            refresh();
        } else if (field == m_labelEdit && HIWORD(wParam) == EN_CHANGE && !m_syncing) {
            wchar_t label[65] { };
            GetWindowTextW(field, label, 65);
            setOverlayMetricLabel(m_model->settings.overlay, m_selected, narrow(label));
            changed(false);
        } else if (field == m_labelEdit && HIWORD(wParam) == EN_KILLFOCUS) {
            changed(true);
        } else if ((field == m_labelX || field == m_valueX || field == m_y)
            && HIWORD(wParam) == EN_KILLFOCUS) {
            editCoordinate(field);
        } else if (field == m_numericEdit && HIWORD(wParam) == EN_KILLFOCUS
            && m_numericAction != ActionKind::None) {
            wchar_t buffer[16] { };
            GetWindowTextW(field, buffer, 16);
            wchar_t* end = nullptr;
            const long number = std::wcstol(buffer, &end, 10);
            const ActionKind action = m_numericAction;
            m_numericAction = ActionKind::None;
            ShowWindow(m_numericEdit, SW_HIDE);
            if (end != buffer && *end == L'\0') {
                const auto slider = numericSlider(m_model->settings.overlay, action);
                setNumericSlider(m_model->settings.overlay, action,
                    std::clamp(static_cast<int>(number), slider.minimum, slider.maximum));
                changed(true);
            }
            refresh();
        } else if (field == m_hexEdit && HIWORD(wParam) == EN_KILLFOCUS && !m_syncing) {
            wchar_t buffer[16] { };
            GetWindowTextW(m_hexEdit, buffer, 16);
            auto& rgba = m_model->settings.overlay.metricRgba[static_cast<std::size_t>(m_selected)];
            std::uint32_t parsed = 0;
            if (buffer[0] == L'\0') {
                if (rgba) {
                    rgba.reset();
                    changed(true);
                }
            } else if (parseOverlayRgba(narrow(buffer), parsed)) {
                if (rgba != parsed) {
                    rgba = parsed;
                    changed(true);
                }
            } else {
                MessageBoxW(m_window, L"Введите цвет в формате #RRGGBBAA или очистите поле.",
                    L"Неверный цвет RGBA", MB_OK | MB_ICONWARNING);
            }
            syncFields();
        } else if (field == m_alphaEdit && HIWORD(wParam) == EN_KILLFOCUS && !m_syncing) {
            wchar_t buffer[8] { };
            GetWindowTextW(m_alphaEdit, buffer, 8);
            wchar_t* end = nullptr;
            const long percent = std::wcstol(buffer, &end, 10);
            if (end != buffer && *end == L'\0' && percent >= 0 && percent <= 100) {
                auto& rgba = m_model->settings.overlay.metricRgba[static_cast<std::size_t>(m_selected)];
                if (!rgba && percent == 100) {
                    return 0;
                }
                const std::uint32_t base = rgba.value_or(0xFFFFFFFFu);
                rgba = (base & 0xFFFFFF00u) | static_cast<std::uint32_t>((percent * 255 + 50) / 100);
                changed(true);
            }
            syncFields();
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = logicalCoordinate(GET_X_LPARAM(lParam));
        const int y = logicalCoordinate(GET_Y_LPARAM(lParam));
        if (y >= 283 && y < 307 && x >= 309 && x < 399) {
            m_samplingColor = !m_samplingColor;
            refresh();
            return 0;
        }
        if (m_samplingColor) {
            m_samplingColor = false;
            const bool preview = Rect { 20.0f, 70.0f, 540.0f, 282.0f }.contains(
                static_cast<float>(x), static_cast<float>(y));
            if (preview || m_canvas.contains(static_cast<float>(x), static_cast<float>(y))) {
                POINT screen { static_cast<LONG>(std::lround(x * m_dpiScale)),
                    static_cast<LONG>(std::lround(y * m_dpiScale)) };
                ClientToScreen(m_window, &screen);
                if (WindowFromPoint(screen) == m_window) {
                    const HDC desktop = GetDC(nullptr);
                    const COLORREF sampled = desktop ? GetPixel(desktop, screen.x, screen.y) : CLR_INVALID;
                    if (desktop)
                        ReleaseDC(nullptr, desktop);
                    if (sampled != CLR_INVALID) {
                        auto& rgba
                            = m_model->settings.overlay.metricRgba[static_cast<std::size_t>(m_selected)];
                        const std::uint32_t alpha = rgba ? *rgba & 0xFFu : 0xFFu;
                        rgba = (static_cast<std::uint32_t>(GetRValue(sampled)) << 24)
                            | (static_cast<std::uint32_t>(GetGValue(sampled)) << 16)
                            | (static_cast<std::uint32_t>(GetBValue(sampled)) << 8) | alpha;
                        syncFields();
                        changed(true);
                    }
                }
            }
            refresh();
            return 0;
        }
        if (y >= 12 && y < 44 && x >= 440 && x < 530) {
            restoreHistory(x >= 485);
            return 0;
        }
        if (x >= static_cast<int>(m_controls.right - 350.0f)
            && x < static_cast<int>(m_controls.right - 230.0f) && y >= 12 && y < 46) {
            m_onAction(UiHit { { }, ActionKind::ImportOverlayRecipe, 0 }, 0.0f, true);
            recordHistory();
            syncFields();
            refresh();
            return 0;
        }
        if (x >= static_cast<int>(m_controls.right - 224.0f)
            && x < static_cast<int>(m_controls.right - 112.0f) && y >= 12 && y < 46) {
            m_onAction(UiHit { { }, ActionKind::ExportOverlayRecipe, 0 }, 0.0f, true);
            return 0;
        }
        if (x >= static_cast<int>(m_controls.right - 108.0f) && y >= 12 && y < 46) {
            m_catalogOpen = !m_catalogOpen;
            ShowWindow(m_catalogSearch, m_catalogOpen ? SW_SHOW : SW_HIDE);
            if (m_catalogOpen) {
                SetFocus(m_catalogSearch);
            } else {
                SetFocus(m_window);
            }
            refresh();
            return 0;
        }
        if (m_catalogOpen && m_catalog.contains(static_cast<float>(x), static_cast<float>(y))) {
            const int item = (y - static_cast<int>(m_catalog.top) - 34) / 28;
            const auto options = catalogRows();
            const int index = item + m_catalogScroll;
            if (y >= m_catalog.top + 34 && item >= 0 && item < 12 && index >= 0
                && index < static_cast<int>(options.size())) {
                auto& settings = m_model->settings.overlay;
                const MetricId id = options[static_cast<std::size_t>(index)];
                const std::size_t metric = static_cast<std::size_t>(id);
                settings.metrics[metric] = !settings.metrics[metric];
                for (MetricId detail : kPairedMetricDetails) {
                    if (displayMetric(detail) == id) {
                        settings.metrics[static_cast<std::size_t>(detail)] = false;
                    }
                }
                if (settings.metrics[metric]) {
                    if (settings.pixelLayout) {
                        enableOverlayPixelLayout(settings);
                    }
                    select(id, false);
                }
                changed(true);
            }
            return 0;
        }
        if (m_catalogOpen) {
            m_catalogOpen = false;
            ShowWindow(m_catalogSearch, SW_HIDE);
        }
        if (x >= static_cast<int>(m_controls.left) && x < static_cast<int>(m_controls.right) && y >= 67
            && y < 102) {
            if (x >= static_cast<int>(m_controls.right - 124.0f)) {
                m_advancedControls = !m_advancedControls;
                m_panelScroll = 0;
                refresh();
                return 0;
            }
            if (!m_advancedControls) {
                return 0;
            }
            const float width = (m_controls.width() - 130.0f) / static_cast<float>(kSections.size());
            const int tab = static_cast<int>((x - m_controls.left) / width);
            if (tab >= 0 && tab < static_cast<int>(kSections.size())) {
                const auto anchor = std::find_if(m_hits.begin(), m_hits.end(), [tab](const UiHit& hit) {
                    return hit.action == kSectionActions[static_cast<std::size_t>(tab)];
                });
                if (anchor != m_hits.end()) {
                    const int offset = static_cast<int>(anchor->rect.top - m_controls.top) + m_panelScroll;
                    m_panelScroll = std::clamp(offset - (tab == 0 ? 0 : 58), 0,
                        std::max(0,
                            static_cast<int>(overlayEditorControlsHeight(m_model->settings.overlay)
                                - m_controls.height())));
                }
                // Навигация по разделам не меняет оверлей и не сбрасывает undo/redo.
                refresh();
            }
            return 0;
        }
        if (m_layers.contains(static_cast<float>(x), static_cast<float>(y))) {
            const int index = (y - static_cast<int>(m_layers.top) - 34) / 28 + m_layersScroll;
            const auto rows = overlayMetricRows(m_model->settings.overlay, m_model->store);
            const auto hidden = hiddenLayerRows(m_hiddenLayers, rows);
            if (y >= m_layers.top + 34 && index >= 0
                && index < static_cast<int>(rows.size() + hidden.size())) {
                const bool visible = index < static_cast<int>(rows.size());
                const MetricId id = visible ? rows[static_cast<std::size_t>(index)].id
                                            : hidden[static_cast<std::size_t>(index) - rows.size()];
                if (x < m_layers.left + 37.0f) {
                    auto& settings = m_model->settings.overlay;
                    settings.metrics[static_cast<std::size_t>(id)] = !visible;
                    if (visible) {
                        for (MetricId detail : kPairedMetricDetails) {
                            if (displayMetric(detail) == id) {
                                settings.metrics[static_cast<std::size_t>(detail)] = false;
                            }
                        }
                        if (std::find(m_hiddenLayers.begin(), m_hiddenLayers.end(), id)
                            == m_hiddenLayers.end()) {
                            m_hiddenLayers.push_back(id);
                        }
                    }
                    changed(true);
                    const auto remaining = overlayMetricRows(settings, m_model->store);
                    if (!remaining.empty()) {
                        select(visible ? remaining.front().id : id, false);
                    }
                } else {
                    select(id, false);
                }
            }
            return 0;
        }
        if (x >= 405 && x < 540 && y >= 283 && y < 307) {
            m_model->settings.overlay.pixelLayout = false;
            changed(true);
            return 0;
        }
        if (m_inspector.contains(static_cast<float>(x), static_cast<float>(y))) {
            const int localX = x - static_cast<int>(m_inspector.left);
            const int localY = y - static_cast<int>(m_inspector.top);
            if (localY < 35 && localX > 278) {
                if (localX > 405) {
                    auto& settings = m_model->settings.overlay;
                    settings.metrics[static_cast<std::size_t>(m_selected)] = false;
                    for (MetricId detail : kPairedMetricDetails) {
                        if (displayMetric(detail) == m_selected) {
                            settings.metrics[static_cast<std::size_t>(detail)] = false;
                        }
                    }
                    changed(true);
                    const auto rows = overlayMetricRows(settings, m_model->store);
                    if (!rows.empty()) {
                        select(rows.front().id, false);
                    }
                } else {
                    m_model->settings.overlay.customLabelSet[static_cast<std::size_t>(m_selected)] = false;
                    syncFields();
                    changed(true);
                }
                return 0;
            }
            if (localY >= 137 && localY <= 166) {
                const int color = (localX - 20) / 56 - 1;
                if (localX >= 20 && color >= -1 && color < kOverlayColorCount && localX < 411) {
                    const auto index = static_cast<std::size_t>(m_selected);
                    m_model->settings.overlay.metricColors[index] = color;
                    m_model->settings.overlay.metricRgba[index].reset();
                    changed(true);
                    syncFields();
                } else if (localX >= 411 && localX < 470) {
                    auto& rgba = m_model->settings.overlay.metricRgba[static_cast<std::size_t>(m_selected)];
                    static COLORREF custom[16] { };
                    const std::uint32_t current = rgba.value_or(0xFFFFFFFFu);
                    CHOOSECOLORW picker { };
                    picker.lStructSize = sizeof(picker);
                    picker.hwndOwner = m_window;
                    picker.lpCustColors = custom;
                    picker.rgbResult
                        = RGB((current >> 24) & 0xFFu, (current >> 16) & 0xFFu, (current >> 8) & 0xFFu);
                    picker.Flags = CC_RGBINIT | CC_FULLOPEN;
                    if (ChooseColorW(&picker)) {
                        rgba = (static_cast<std::uint32_t>(GetRValue(picker.rgbResult)) << 24)
                            | (static_cast<std::uint32_t>(GetGValue(picker.rgbResult)) << 16)
                            | (static_cast<std::uint32_t>(GetBValue(picker.rgbResult)) << 8)
                            | (current & 0xFFu);
                        changed(true);
                        syncFields();
                    }
                }
                return 0;
            }
        }
        if (m_canvas.contains(static_cast<float>(x), static_cast<float>(y))) {
            if (GetKeyState(VK_SPACE) < 0) {
                m_panning = true;
                m_panStartX = x;
                m_panStartY = y;
                m_panStartLeft = m_panX;
                m_panStartTop = m_panY;
                SetFocus(m_window);
                SetCapture(m_window);
                return 0;
            }
            const Rect plate = plateOnCanvas();
            if (m_model->settings.overlay.layout != OverlayLayout::Plates
                && plate.contains(static_cast<float>(x), static_cast<float>(y))) {
                const auto& settings = m_model->settings.overlay;
                m_dragPlate = settings.anchorMode == OverlayAnchorMode::Free;
                m_dragLine = !m_dragPlate;
                m_dragging = true;
                m_dragStartX = x;
                m_dragStartY = y;
                m_offsetStartX = settings.offsetX;
                m_offsetStartY = settings.offsetY;
                m_originStartX = settings.originX;
                m_originStartY = settings.originY;
                SetFocus(m_window);
                SetCapture(m_window);
                return 0;
            }
            if (m_model->settings.overlay.layout != OverlayLayout::Plates
                || !plate.contains(static_cast<float>(x), static_cast<float>(y))) {
                return 0;
            }
            const auto beginPlateDrag = [&] {
                auto& settings = m_model->settings.overlay;
                if (settings.anchorMode == OverlayAnchorMode::Corner) {
                    settings.originX = std::max(0, static_cast<int>(std::lround(m_screenPlate.left)));
                    settings.originY = std::max(0, static_cast<int>(std::lround(m_screenPlate.top)));
                    settings.anchorMode = OverlayAnchorMode::Free;
                    settings.offsetX = 0;
                    settings.offsetY = 0;
                }
                m_originStartX = settings.originX;
                m_originStartY = settings.originY;
                m_dragStartX = x;
                m_dragStartY = y;
                m_dragLine = false;
                m_dragPlate = true;
                m_dragging = true;
                SetFocus(m_window);
                SetCapture(m_window);
                changed(false);
            };
            if (GetKeyState(VK_MENU) < 0) {
                beginPlateDrag();
                return 0;
            }
            const auto rows = overlayMetricRows(m_model->settings.overlay, m_model->store);
            const float rowScale = canvasScale() * m_model->settings.overlay.scalePercent / 100.0f;
            for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                const auto row = canvasPosition(it->id);
                const float rowY = plate.top + row.y * rowScale;
                if (y < rowY || y >= rowY + std::max(12.0f, 26.0f * rowScale)) {
                    continue;
                }
                const bool pixelLayout = m_model->settings.overlay.pixelLayout;
                const bool value = x >= (pixelLayout ? plate.left + row.valueX * rowScale
                                                     : plate.left + plate.width() * 0.55f);
                if (!value
                    && (x < plate.left + row.labelX * rowScale
                        || (pixelLayout && x >= plate.left + (row.labelX + 230) * rowScale))) {
                    continue;
                }
                const bool isSelected
                    = std::find(m_selectedRows.begin(), m_selectedRows.end(), it->id) != m_selectedRows.end();
                if (GetKeyState(VK_CONTROL) < 0) {
                    if (isSelected) {
                        m_selectedRows.erase(
                            std::remove(m_selectedRows.begin(), m_selectedRows.end(), it->id),
                            m_selectedRows.end());
                    } else {
                        m_selectedRows.push_back(it->id);
                    }
                    select(it->id, value);
                    return 0;
                }
                if (!isSelected) {
                    m_selectedRows = { it->id };
                }
                select(it->id, value);
                preparePixelLayout();
                m_dragPositions.clear();
                m_dragPositions.push_back(
                    m_model->settings.overlay.rowPositions[static_cast<std::size_t>(it->id)]);
                std::vector<MetricId> ordered { it->id };
                for (MetricId selected : m_selectedRows) {
                    if (selected != it->id) {
                        ordered.push_back(selected);
                        m_dragPositions.push_back(
                            m_model->settings.overlay.rowPositions[static_cast<std::size_t>(selected)]);
                    }
                }
                m_selectedRows = std::move(ordered);
                m_dragPlate = false;
                m_dragLine = false;
                m_dragStartX = x;
                m_dragStartY = y;
                m_rowDragScale = rowScale;
                m_dragging = true;
                SetFocus(m_window);
                SetCapture(m_window);
                return 0;
            }
            beginPlateDrag();
            return 0;
        }
        if (const UiHit* hit = hitAt(static_cast<float>(x), static_cast<float>(y))) {
            if (hit->value == -1000 && isSliderAction(hit->action)) {
                const auto slider = numericSlider(m_model->settings.overlay, hit->action);
                m_numericAction = hit->action;
                input(m_numericEdit, std::to_wstring(slider.value).c_str());
                const auto px
                    = [this](float value) { return static_cast<int>(std::lround(value * m_dpiScale)); };
                SetWindowPos(m_numericEdit, HWND_TOP, px(hit->rect.left), px(hit->rect.top),
                    px(hit->rect.width()), px(24.0f), SWP_SHOWWINDOW | SWP_NOACTIVATE);
                SetFocus(m_numericEdit);
                SendMessageW(m_numericEdit, EM_SETSEL, 0, -1);
                return 0;
            }
            m_activeHit = *hit;
            SetFocus(m_window);
            if (hit->action == ActionKind::ReorderMetric || isSliderAction(hit->action)) {
                SetCapture(m_window);
                if (isSliderAction(hit->action)) {
                    m_onAction(*hit, static_cast<float>(x), false);
                }
            } else {
                m_onAction(*hit, static_cast<float>(x), true);
                recordHistory();
            }
            refresh();
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = logicalCoordinate(GET_X_LPARAM(lParam));
        const int y = logicalCoordinate(GET_Y_LPARAM(lParam));
        if (m_panning) {
            m_panX = m_panStartLeft + x - m_panStartX;
            m_panY = m_panStartTop + y - m_panStartY;
            refresh();
        } else if (m_dragging) {
            move(x, y);
        } else if (GetCapture() == m_window && isSliderAction(m_activeHit.action)) {
            m_onAction(m_activeHit, static_cast<float>(x), false);
            refresh();
        } else if (GetCapture() == m_window && m_activeHit.action == ActionKind::ReorderMetric) {
            if (const UiHit* hit = hitAt(static_cast<float>(x), static_cast<float>(y))) {
                if (hit->action == ActionKind::ReorderMetric && hit->value != m_activeHit.value) {
                    moveOverlayMetric(m_model->settings.overlay, static_cast<MetricId>(m_activeHit.value),
                        static_cast<MetricId>(hit->value));
                    changed(false);
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        const int x = logicalCoordinate(GET_X_LPARAM(lParam));
        const int y = logicalCoordinate(GET_Y_LPARAM(lParam));
        if (GetCapture() == m_window) {
            if (m_panning) {
                m_panning = false;
                ReleaseCapture();
                return 0;
            }
            if (m_dragging) {
                move(x, y);
                m_dragging = false;
                m_dragPlate = false;
                m_dragLine = false;
                m_guideX = m_guideY = -1.0f;
                changed(true);
            } else if (isSliderAction(m_activeHit.action)) {
                m_onAction(m_activeHit, static_cast<float>(x), true);
                recordHistory();
            } else if (m_activeHit.action == ActionKind::ReorderMetric) {
                changed(true);
            }
            m_activeHit.action = ActionKind::None;
            ReleaseCapture();
            return 0;
        }
        break;
    }
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (m_samplingColor) {
                m_samplingColor = false;
                refresh();
                return 0;
            }
            if (m_model->editingLabel >= 0) {
                m_onText(27);
            } else {
                close();
            }
            return 0;
        }
        if (m_model->editingLabel >= 0) {
            break;
        }
        if (GetKeyState(VK_CONTROL) < 0 && (wParam == 'Z' || wParam == 'Y')) {
            restoreHistory(wParam == 'Y' || GetKeyState(VK_SHIFT) < 0);
            return 0;
        }
        if (wParam == VK_TAB) {
            m_value = !m_value;
            refresh();
            return 0;
        }
        if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) {
            if (m_model->settings.overlay.layout != OverlayLayout::Plates) {
                auto& settings = m_model->settings.overlay;
                const int delta = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
                if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                    settings.offsetX
                        = std::clamp(settings.offsetX + (wParam == VK_LEFT ? -delta : delta), -8192, 8192);
                } else {
                    settings.offsetY
                        = std::clamp(settings.offsetY + (wParam == VK_UP ? -delta : delta), -8192, 8192);
                }
                changed(true);
                return 0;
            }
            preparePixelLayout();
            auto& settings = m_model->settings.overlay;
            const int delta = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
            if (m_selectedRows.empty()) {
                m_selectedRows.push_back(m_selected);
            }
            int deltaX = wParam == VK_LEFT ? -delta : wParam == VK_RIGHT ? delta : 0;
            int deltaY = wParam == VK_UP ? -delta : wParam == VK_DOWN ? delta : 0;
            for (MetricId id : m_selectedRows) {
                const auto& row = settings.rowPositions[static_cast<std::size_t>(id)];
                deltaX = std::clamp(
                    deltaX, -std::min(row.labelX, row.valueX), 470 - std::max(row.labelX, row.valueX));
                deltaY = std::clamp(deltaY, -row.y, kMaxY - row.y);
            }
            for (MetricId id : m_selectedRows) {
                auto& row = settings.rowPositions[static_cast<std::size_t>(id)];
                row.labelX += deltaX;
                row.valueX += deltaX;
                row.y += deltaY;
            }
            settings.pixelLayout = true;
            syncFields();
            changed(true);
            return 0;
        }
        break;
    case WM_CHAR:
        if (m_model->editingLabel >= 0) {
            m_onText(static_cast<wchar_t>(wParam));
            refresh();
            return 0;
        }
        break;
    }
    return DefWindowProcW(m_window, message, wParam, lParam);
}

void OverlayEditor::paint()
{
    if (!m_renderer.begin(m_window)) {
        return;
    }
    m_renderer.target()->SetDpi(96.0f * m_dpiScale, 96.0f * m_dpiScale);
    const Theme& theme = *m_theme;
    m_renderer.clear(theme.background);
    const float width = m_renderer.width() / m_dpiScale;
    const float height = m_renderer.height() / m_dpiScale;
    m_renderer.fillRadialGlow(250.0f, -80.0f, 490.0f, theme.accent.withAlpha(0.12f));
    m_renderer.text(
        L"Мастерская оверлея", Rect { 20.0f, 12.0f, 420.0f, 44.0f }, TextRole::Heading, theme.text);
    m_renderer.text(L"↶", Rect { 440.0f, 12.0f, 480.0f, 44.0f }, TextRole::Heading,
        m_undo.empty() ? theme.textFaint : theme.accent, TextAlign::Center);
    m_renderer.text(L"↷", Rect { 485.0f, 12.0f, 525.0f, 44.0f }, TextRole::Heading,
        m_redo.empty() ? theme.textFaint : theme.accent, TextAlign::Center);
    m_renderer.text(L"Слева — слои и холст. Справа — настройки выбранного вида.",
        Rect { 20.0f, 37.0f, 540.0f, 61.0f }, TextRole::Small, theme.textDim);

    drawOverlayEditorPreview(m_renderer, *m_model, theme, Rect { 20.0f, 70.0f, 540.0f, 282.0f },
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    m_renderer.text(m_model->settings.overlay.layout != OverlayLayout::Plates
            ? L"Строка: двигайте весь блок; порядок — в разделе «Строки»"
            : L"Колесо — зум · Ctrl — выбор · Alt — плашка · Shift — сетка",
        Rect { 20.0f, 284.0f, 303.0f, 304.0f }, TextRole::Tiny, theme.textDim);
    const Rect eyedropper { 309.0f, 283.0f, 399.0f, 307.0f };
    m_renderer.fillRoundRect(eyedropper, 6.0f, m_samplingColor ? theme.accentSoft : theme.panelSoft);
    m_renderer.text(L"Пипетка", eyedropper, TextRole::Tiny, theme.accent, TextAlign::Center);
    m_renderer.text(L"Вернуть шаблон", Rect { 405.0f, 283.0f, 540.0f, 307.0f }, TextRole::Tiny, theme.accent);
    const auto rows = overlayMetricRows(m_model->settings.overlay, m_model->store);
    const auto hidden = hiddenLayerRows(m_hiddenLayers, rows);
    m_renderer.fillRoundRect(m_layers, 10.0f, theme.panel);
    m_renderer.text(L"Слои · глазик",
        Rect { m_layers.left + 10.0f, m_layers.top + 8.0f, m_layers.right - 8.0f, m_layers.top + 29.0f },
        TextRole::Small, theme.text);
    m_layersScroll = std::clamp(m_layersScroll, 0,
        std::max(0,
            static_cast<int>(rows.size() + hidden.size())
                - std::max(1, static_cast<int>((m_layers.height() - 38.0f) / 28.0f))));
    m_renderer.pushClip(m_layers);
    for (std::size_t index = static_cast<std::size_t>(m_layersScroll); index < rows.size() + hidden.size();
        ++index) {
        const float top = m_layers.top + 34.0f + static_cast<float>(index - m_layersScroll) * 28.0f;
        if (top + 26.0f > m_layers.bottom) {
            break;
        }
        const Rect row { m_layers.left + 6.0f, top, m_layers.right - 6.0f, top + 25.0f };
        const bool visible = index < rows.size();
        const MetricId id = visible ? rows[index].id : hidden[index - rows.size()];
        const std::string_view label = visible ? std::string_view(rows[index].label) : metricInfo(id).label;
        m_renderer.fillRoundRect(row, 5.0f, id == m_selected ? theme.accentSoft : theme.panelSoft);
        m_renderer.text(visible ? L"◉" : L"○", Rect { row.left + 3.0f, top, row.left + 29.0f, top + 25.0f },
            TextRole::Small, visible ? theme.accent : theme.textDim, TextAlign::Center);
        m_renderer.text(clipText(m_renderer, widen(label), TextRole::Tiny, row.width() - 38.0f),
            Rect { row.left + 31.0f, top, row.right - 3.0f, top + 25.0f }, TextRole::Tiny,
            id == m_selected ? theme.text : theme.textDim);
    }
    m_renderer.popClip();
    m_renderer.fillRoundRect(m_canvas, 10.0f, theme.panel);
    m_renderer.pushClip(m_canvas);
    for (int step = 1; step < 4; ++step) {
        const float x = m_canvas.left + m_canvas.width() * step / 4.0f;
        const float y = m_canvas.top + m_canvas.height() * step / 4.0f;
        m_renderer.drawLine(x, m_canvas.top, x, m_canvas.bottom, theme.line.withAlpha(0.35f), 1.0f);
        m_renderer.drawLine(m_canvas.left, y, m_canvas.right, y, theme.line.withAlpha(0.35f), 1.0f);
    }
    ID2D1RenderTarget* target = m_renderer.target();
    D2D1::Matrix3x2F transform;
    target->GetTransform(&transform);
    const float screenScale = canvasScale();
    target->SetTransform(D2D1::Matrix3x2F::Scale(screenScale, screenScale)
        * D2D1::Matrix3x2F::Translation(m_canvas.left + m_panX, m_canvas.top + m_panY) * transform);
    const OverlayFormats formats { m_renderer.dwrite(), m_renderer.textFormat(TextRole::Heading),
        m_renderer.textFormat(TextRole::Value), m_renderer.textFormat(TextRole::Small),
        m_renderer.textFormat(TextRole::Mono) };
    const auto text = makeOverlayText(m_model->settings.overlay, m_model->store);
    const Rect screen { 0.0f, 0.0f, static_cast<float>(GetSystemMetrics(SM_CXVIRTUALSCREEN)),
        static_cast<float>(GetSystemMetrics(SM_CYVIRTUALSCREEN)) };
    m_screenPlate = paintOverlay(target, m_model->settings.overlay, theme, text, screen,
        static_cast<float>(m_model->settings.overlay.scalePercent) / 100.0f, formats);
    target->SetTransform(transform);
    if (m_model->settings.overlay.layout == OverlayLayout::Plates) {
        const Rect plate = plateOnCanvas();
        const float rowScale = screenScale * m_model->settings.overlay.scalePercent / 100.0f;
        for (const auto& entry : rows) {
            if (m_selected != entry.id
                && std::find(m_selectedRows.begin(), m_selectedRows.end(), entry.id)
                    == m_selectedRows.end()) {
                continue;
            }
            const auto row = canvasPosition(entry.id);
            const float x = plate.left + (m_value ? row.valueX : row.labelX) * rowScale;
            const float y = plate.top + row.y * rowScale;
            m_renderer.strokeRoundRect(
                Rect { x - 2.0f, y - 2.0f, x + 90.0f * rowScale, y + std::max(12.0f, 26.0f * rowScale) },
                4.0f, theme.accent, 1.0f);
        }
    }
    if (m_dragging) {
        if (m_guideX >= 0.0f) {
            m_renderer.drawLine(m_guideX, m_canvas.top, m_guideX, m_canvas.bottom, theme.accent, 1.5f);
        }
        if (m_guideY >= 0.0f) {
            m_renderer.drawLine(m_canvas.left, m_guideY, m_canvas.right, m_guideY, theme.accent, 1.5f);
        }
        if (m_dragPlate) {
            const float left = m_canvas.left + m_panX + m_canvas.width() * 0.05f * m_canvasZoom;
            const float top = m_canvas.top + m_panY + m_canvas.height() * 0.05f * m_canvasZoom;
            const float right = m_canvas.left + m_panX + m_canvas.width() * 0.95f * m_canvasZoom;
            const float bottom = m_canvas.top + m_panY + m_canvas.height() * 0.95f * m_canvasZoom;
            m_renderer.strokeRoundRect(
                Rect { left, top, right, bottom }, 4.0f, theme.accent.withAlpha(0.3f), 1.0f);
        }
    }
    m_renderer.popClip();
    m_renderer.strokeRoundRect(m_canvas, 10.0f, theme.line);

    m_renderer.fillRoundRect(m_inspector, 10.0f, theme.panel);
    const std::wstring title
        = rows.empty() ? L"Добавьте показатель через «+ Датчик»" : widen(metricInfo(m_selected).label);
    m_renderer.text(clipText(m_renderer, title, TextRole::Heading, 280.0f),
        Rect { 32.0f, m_inspector.top + 7.0f, 320.0f, m_inspector.top + 34.0f }, TextRole::Heading,
        theme.text);
    m_renderer.text(L"Сбросить имя", Rect { 320.0f, m_inspector.top + 9.0f, 425.0f, m_inspector.top + 32.0f },
        TextRole::Tiny, theme.accent);
    m_renderer.text(L"Скрыть", Rect { 434.0f, m_inspector.top + 9.0f, 508.0f, m_inspector.top + 32.0f },
        TextRole::Tiny, theme.warn);
    m_renderer.text(L"Подпись", Rect { 32.0f, m_inspector.top + 46.0f, 103.0f, m_inspector.top + 67.0f },
        TextRole::Small, theme.textDim);
    m_renderer.text(L"RGBA", Rect { 32.0f, m_inspector.top + 72.0f, 103.0f, m_inspector.top + 92.0f },
        TextRole::Tiny, theme.textDim);
    m_renderer.text(L"X подписи", Rect { 32.0f, m_inspector.top + 96.0f, 106.0f, m_inspector.top + 119.0f },
        TextRole::Tiny, theme.textDim);
    m_renderer.text(L"X значения", Rect { 182.0f, m_inspector.top + 96.0f, 276.0f, m_inspector.top + 119.0f },
        TextRole::Tiny, theme.textDim);
    m_renderer.text(L"Y", Rect { 380.0f, m_inspector.top + 96.0f, 403.0f, m_inspector.top + 119.0f },
        TextRole::Tiny, theme.textDim);
    const auto palette = overlayColorPalette();
    const int selectedColor = m_model->settings.overlay.metricColors[static_cast<std::size_t>(m_selected)];
    for (int n = -1; n < kOverlayColorCount; ++n) {
        const float x = 40.0f + static_cast<float>(n + 1) * 56.0f;
        const Rect chip { x, m_inspector.top + 137.0f, x + 30.0f, m_inspector.top + 167.0f };
        m_renderer.fillRoundRect(chip, 8.0f, n < 0 ? theme.panelSoft : palette[static_cast<std::size_t>(n)]);
        if (n == selectedColor) {
            m_renderer.strokeRoundRect(chip, 8.0f, theme.accent, 2.0f);
        }
        if (n < 0) {
            m_renderer.text(L"А", chip, TextRole::Small, theme.text, TextAlign::Center);
        }
    }
    const auto custom = m_model->settings.overlay.metricRgba[static_cast<std::size_t>(m_selected)];
    m_renderer.text(L"α %", Rect { 471.0f, m_inspector.top + 122.0f, 519.0f, m_inspector.top + 138.0f },
        TextRole::Tiny, theme.textDim, TextAlign::Center);
    const Rect picker { 430.0f, m_inspector.top + 138.0f, 470.0f, m_inspector.top + 165.0f };
    m_renderer.fillRoundRect(
        picker, 6.0f, custom ? fromRgba((*custom >> 8) | ((*custom & 0xFFu) << 24)) : theme.panelSoft);
    m_renderer.strokeRoundRect(picker, 6.0f, theme.line, 1.0f);
    m_renderer.text(
        L"RGB", picker, TextRole::Tiny, custom ? theme.background : theme.accent, TextAlign::Center);

    if (width >= 1200.0f) {
        m_renderer.text(L"Оформление · шрифт · подписи · датчики",
            Rect { m_controls.left, 12.0f, width - 400.0f, 44.0f }, TextRole::Heading, theme.text);
    }
    m_renderer.fillRoundRect(Rect { width - 368.0f, 12.0f, width - 248.0f, 45.0f }, 8.0f, theme.panelSoft);
    m_renderer.text(L"Импорт JSON", Rect { width - 366.0f, 12.0f, width - 250.0f, 45.0f }, TextRole::Tiny,
        theme.accent, TextAlign::Center);
    m_renderer.fillRoundRect(Rect { width - 242.0f, 12.0f, width - 130.0f, 45.0f }, 8.0f, theme.panelSoft);
    m_renderer.text(L"Экспорт JSON", Rect { width - 240.0f, 12.0f, width - 132.0f, 45.0f }, TextRole::Tiny,
        theme.accent, TextAlign::Center);
    m_renderer.fillRoundRect(Rect { width - 110.0f, 12.0f, width - 18.0f, 45.0f }, 8.0f, theme.accentSoft);
    m_renderer.text(m_catalogOpen ? L"Закрыть" : L"+ Датчик",
        Rect { width - 108.0f, 12.0f, width - 18.0f, 45.0f }, TextRole::Small, theme.accent,
        TextAlign::Center);
    m_renderer.pushClip(m_controls);
    m_renderer.pushTranslate(0.0f, -static_cast<float>(m_panelScroll));
    m_hits.clear();
    const Rect controlsArea { m_controls.left, m_controls.top + 4.0f, m_controls.right - 6.0f,
        m_controls.top + overlayEditorControlsHeight(m_model->settings.overlay) };
    if (m_advancedControls) {
        drawOverlayEditorControls(m_renderer, *m_model, theme, controlsArea, m_hits);
    } else {
        drawOverlayEditorQuickControls(m_renderer, *m_model, theme, controlsArea, m_hits);
    }
    m_renderer.popTranslate();
    m_renderer.popClip();
    for (UiHit& hit : m_hits) {
        hit.rect.top -= static_cast<float>(m_panelScroll);
        hit.rect.bottom -= static_cast<float>(m_panelScroll);
    }
    if (m_advancedControls) {
        std::size_t activeSection = 0;
        for (std::size_t index = 1; index < kSectionActions.size(); ++index) {
            const auto anchor = std::find_if(m_hits.begin(), m_hits.end(),
                [index](const UiHit& hit) { return hit.action == kSectionActions[index]; });
            if (anchor != m_hits.end() && anchor->rect.top <= m_controls.top + 120.0f) {
                activeSection = index;
            }
        }
        for (std::size_t index = 0; index < kSections.size(); ++index) {
            const float tabWidth = (m_controls.width() - 130.0f) / static_cast<float>(kSections.size());
            const Rect tab { m_controls.left + tabWidth * static_cast<float>(index), 68.0f,
                m_controls.left + tabWidth * static_cast<float>(index + 1) - 6.0f, 101.0f };
            m_renderer.fillRoundRect(tab, 8.0f, index == activeSection ? theme.accentSoft : theme.panelSoft);
            m_renderer.text(kSections[index], tab, TextRole::Small,
                index == activeSection ? theme.accent : theme.text, TextAlign::Center);
        }
        const float fraction
            = m_panelScroll / std::max(overlayEditorControlsHeight(m_model->settings.overlay), 1.0f);
        m_renderer.fillRoundRect(
            Rect { m_controls.right - 4.0f, m_controls.top + fraction * m_controls.height(), m_controls.right,
                m_controls.top + fraction * m_controls.height() + 55.0f },
            2.0f, theme.accent);
    } else {
        m_renderer.text(L"Быстро: рецепт → раскладка → перетаскивание",
            Rect { m_controls.left + 8.0f, 68.0f, m_controls.right - 132.0f, 101.0f }, TextRole::Tiny,
            theme.textDim);
    }
    const Rect mode { m_controls.right - 124.0f, 68.0f, m_controls.right - 2.0f, 101.0f };
    m_renderer.fillRoundRect(mode, 8.0f, theme.accentSoft);
    m_renderer.text(m_advancedControls ? L"← Просто" : L"Подробнее →", mode, TextRole::Tiny, theme.accent,
        TextAlign::Center);
    if (m_catalogOpen) {
        m_renderer.fillRoundRect(m_catalog, 10.0f, theme.panel);
        m_renderer.strokeRoundRect(m_catalog, 10.0f, theme.accent, 1.0f);
        m_renderer.text(L"Поиск датчиков · нажмите строку, чтобы включить",
            Rect {
                m_catalog.left + 12.0f, m_catalog.top + 6.0f, m_catalog.right - 8.0f, m_catalog.top + 30.0f },
            TextRole::Tiny, theme.textDim);
        const auto options = catalogRows();
        const int count = std::min(12, static_cast<int>(options.size()) - m_catalogScroll);
        for (int index = 0; index < count; ++index) {
            const MetricId id = options[static_cast<std::size_t>(index + m_catalogScroll)];
            const float y = m_catalog.top + 34.0f + static_cast<float>(index) * 28.0f;
            const bool selected = m_model->settings.overlay.metrics[static_cast<std::size_t>(id)];
            if (selected) {
                m_renderer.fillRoundRect(Rect { m_catalog.left + 6.0f, y, m_catalog.right - 6.0f, y + 26.0f },
                    5.0f, theme.accentSoft);
            }
            m_renderer.text(widen(metricInfo(id).label),
                Rect { m_catalog.left + 12.0f, y, m_catalog.right - 62.0f, y + 26.0f }, TextRole::Small,
                selected ? theme.text : theme.textDim);
            m_renderer.text(selected ? L"вкл" : L"+",
                Rect { m_catalog.right - 57.0f, y, m_catalog.right - 12.0f, y + 26.0f }, TextRole::Tiny,
                selected ? theme.accent : theme.textFaint, TextAlign::Right);
        }
    }
    m_renderer.text(L"Ctrl+клик: несколько строк · Shift+тяга: сетка 8 px · Shift+стрелки: 10 px",
        Rect { 20.0f, height - 19.0f, width - 20.0f, height }, TextRole::Tiny, theme.textFaint);
    m_renderer.end();
}

} // namespace yawa
