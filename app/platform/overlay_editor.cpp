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
#include <dxgi1_6.h>
#include <windowsx.h>
#include <wrl/client.h>

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
    constexpr std::array<std::wstring_view, 4> kSections { L"Собрать", L"Оформить", L"Текст", L"Строки" };

    std::wstring rtssLayerName(std::string_view name)
    {
        const int length
            = MultiByteToWideChar(1251, 0, name.data(), static_cast<int>(name.size()), nullptr, 0);
        if (length <= 0) {
            return { };
        }
        std::wstring result(static_cast<std::size_t>(length), L'\0');
        MultiByteToWideChar(1251, 0, name.data(), static_cast<int>(name.size()), result.data(), length);
        return result;
    }

    float editorScale(HMONITOR monitor, float dpiScale)
    {
        MONITORINFO info { };
        info.cbSize = sizeof(info);
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
            return dpiScale;
        }
        const float width = static_cast<float>(info.rcWork.right - info.rcWork.left);
        const float height = static_cast<float>(info.rcWork.bottom - info.rcWork.top);
        return std::min({ dpiScale, std::max(0.5f, (width - 20.0f) / 1000.0f),
            std::max(0.5f, (height - 40.0f) / 720.0f) });
    }

    bool hdrAtWindow(HWND window)
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        if (window == nullptr || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return false;
        }
        const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
        for (UINT adapterIndex = 0;; ++adapterIndex) {
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(adapterIndex, &adapter) != S_OK) {
                break;
            }
            for (UINT outputIndex = 0;; ++outputIndex) {
                Microsoft::WRL::ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(outputIndex, &output) != S_OK) {
                    break;
                }
                DXGI_OUTPUT_DESC details { };
                if (FAILED(output->GetDesc(&details)) || details.Monitor != monitor) {
                    continue;
                }
                Microsoft::WRL::ComPtr<IDXGIOutput6> advanced;
                DXGI_OUTPUT_DESC1 color { };
                return SUCCEEDED(output.As(&advanced)) && SUCCEEDED(advanced->GetDesc1(&color))
                    && (color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
                        || color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
            }
        }
        return false;
    }

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
        case ActionKind::SetMenuOpacity:
            return { 0, 100, settings.menuOpacityPercent };
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
        case ActionKind::SetMenuOpacity:
            settings.menuOpacityPercent = value;
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

    constexpr UINT kSourceTextDone = WM_APP + 31;
    WNDPROC sourceTextOriginal = nullptr;
    LRESULT CALLBACK sourceTextProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_KEYDOWN
            && (wParam == VK_ESCAPE || (wParam == VK_RETURN && GetKeyState(VK_CONTROL) < 0))) {
            PostMessageW(GetParent(window), kSourceTextDone, wParam == VK_RETURN, 0);
            return 0;
        }
        if (message == WM_CHAR
            && (wParam == VK_ESCAPE
                || (GetKeyState(VK_CONTROL) < 0 && (wParam == L'\r' || wParam == L'\n')))) {
            return 0;
        }
        return CallWindowProcW(sourceTextOriginal, window, message, wParam, lParam);
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
        if (m_sourceTextOpen && m_model && m_sourceTextTemplate != m_model->settings.overlay.rtssTemplate) {
            finishSourceText(false);
        }
        if (m_model && m_canvasTemplate != m_model->settings.overlay.rtssTemplate) {
            m_canvasTemplate = m_model->settings.overlay.rtssTemplate;
            m_fitPending = true;
            m_rtssSelected = -1;
            m_selectedRtssLayers.clear();
        }
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
    const HMONITOR editorMonitor = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
    m_dpiScale = editorScale(editorMonitor, static_cast<float>(GetDpiForWindow(owner)) / 96.0f);
    m_historyCommitted = model.settings.overlay;
    m_layersScroll = 0;
    m_panelScroll = 0;
    m_section = 0;
    m_hiddenLayers.clear();
    m_rtssLayerRects.clear();
    m_canvasRowBounds.clear();
    m_rtssSelected = -1;
    m_selectedRtssLayers.clear();
    m_canvasZoom = 1.0f;
    m_panX = m_panY = 0.0f;
    m_fitPending = true;
    m_canvasTemplate = model.settings.overlay.rtssTemplate;
    m_focusCanvas = true;
    m_rtssDragging = m_rtssResizing = false;
    m_styledRowDragging = false;
    m_samplingColor = false;
    m_undo.clear();
    m_redo.clear();
    m_theme = &theme;
    m_onAction = std::move(onAction);
    m_onText = std::move(onText);
    m_onChange = std::move(onChange);
    MONITORINFO work { };
    work.cbSize = sizeof(work);
    const bool hasWorkArea = GetMonitorInfoW(editorMonitor, &work) != FALSE;
    const int workWidth = hasWorkArea ? work.rcWork.right - work.rcWork.left : 1320;
    const int workHeight = hasWorkArea ? work.rcWork.bottom - work.rcWork.top : 920;
    const int windowWidth = std::min(workWidth, static_cast<int>(std::lround(1320.0f * m_dpiScale)));
    const int windowHeight = std::min(workHeight, static_cast<int>(std::lround(920.0f * m_dpiScale)));
    const int windowX = hasWorkArea ? work.rcWork.left + (workWidth - windowWidth) / 2 : CW_USEDEFAULT;
    const int windowY = hasWorkArea ? work.rcWork.top + (workHeight - windowHeight) / 2 : CW_USEDEFAULT;
    m_window = CreateWindowExW(WS_EX_APPWINDOW, kEditorClass, L"Мастерская оверлея · YawaFpsHub",
        WS_OVERLAPPEDWINDOW, windowX, windowY, windowWidth, windowHeight, owner, nullptr, instance, this);
    if (!m_window) {
        m_renderer.shutdown();
        m_model = nullptr;
        return false;
    }
    m_dpiScale = static_cast<float>(GetDpiForWindow(m_window)) / 96.0f;
    m_hdrDisplay = hdrAtWindow(m_window);
    m_editBrush = CreateSolidBrush(RGB(21, 27, 37));
    const auto createEdit = [this, instance](int id, DWORD style) {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0,
            0, 10, 24, m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    m_labelEdit = createEdit(kControlIdLabel, ES_AUTOHSCROLL);
    m_labelX = createEdit(kControlIdLabelX, ES_AUTOHSCROLL);
    m_valueX = createEdit(kControlIdValueX, ES_AUTOHSCROLL);
    m_y = createEdit(kControlIdY, ES_NUMBER | ES_AUTOHSCROLL);
    m_alphaEdit = createEdit(2006, ES_NUMBER | ES_AUTOHSCROLL);
    m_rtssHeightEdit = createEdit(2009, ES_NUMBER | ES_AUTOHSCROLL);
    m_hexEdit = createEdit(2008, ES_AUTOHSCROLL);
    m_rtssBgEdit = createEdit(2010, ES_AUTOHSCROLL);
    m_numericEdit = createEdit(2007, ES_NUMBER | ES_AUTOHSCROLL);
    if (m_numericEdit) {
        numericEditOriginal = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(m_numericEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(numericEditProc)));
    }
    m_catalogSearch = createEdit(2005, ES_AUTOHSCROLL);
    m_sourceTextEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_TABSTOP | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, 0, 0, 10, 24,
        m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(2011)), instance, nullptr);
    if (m_sourceTextEdit) {
        sourceTextOriginal = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(m_sourceTextEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(sourceTextProc)));
    }
    if (!m_labelEdit || !m_labelX || !m_valueX || !m_y || !m_alphaEdit || !m_rtssHeightEdit || !m_hexEdit
        || !m_rtssBgEdit || !m_numericEdit || !m_catalogSearch || !m_sourceTextEdit) {
        close();
        return false;
    }
    for (HWND field : { m_labelEdit, m_labelX, m_valueX, m_y, m_alphaEdit, m_rtssHeightEdit, m_hexEdit,
             m_rtssBgEdit, m_numericEdit, m_catalogSearch, m_sourceTextEdit }) {
        SendMessageW(field, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    }
    SendMessageW(m_labelEdit, EM_SETLIMITTEXT, 64, 0);
    SendMessageW(m_hexEdit, EM_SETLIMITTEXT, 9, 0);
    SendMessageW(m_rtssBgEdit, EM_SETLIMITTEXT, 9, 0);
    SendMessageW(m_catalogSearch, EM_SETLIMITTEXT, 64, 0);
    SendMessageW(m_sourceTextEdit, EM_SETLIMITTEXT, 8192, 0);
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
    m_inspector = Rect { 20.0f, height - 198.0f, 540.0f, height - 20.0f };
    if (m_focusCanvas) {
        m_canvas = Rect { 184.0f, 110.0f, width - 18.0f, m_inspector.top - 12.0f };
        m_layers = Rect { 20.0f, m_canvas.top, 174.0f, m_canvas.bottom };
        m_preview = Rect { 564.0f, m_inspector.top, width - 18.0f, height - 20.0f };
        m_controls = { };
    } else {
        const float canvasWidth = std::min(
            static_cast<float>(kCanvasWidth), std::max(100.0f, height - 520.0f) * screenWidth / screenHeight);
        const float canvasHeight = canvasWidth * screenHeight / screenWidth;
        m_canvas = Rect { 184.0f, 307.0f, 184.0f + canvasWidth, 307.0f + canvasHeight };
        m_layers = Rect { 20.0f, 307.0f, 174.0f, m_inspector.top - 12.0f };
        m_preview = Rect { 20.0f, 70.0f, 540.0f, 282.0f };
        m_controls = Rect { 564.0f, 112.0f, width - 18.0f, height - 20.0f };
    }
    const float toolbarTop = m_canvas.top - 24.0f;
    const float toolbarRight = m_focusCanvas ? m_canvas.right : 540.0f;
    m_fitButton = Rect { m_focusCanvas ? toolbarRight - 330.0f : 210.0f, toolbarTop,
        m_focusCanvas ? toolbarRight - 237.0f : 303.0f, toolbarTop + 24.0f };
    m_eyedropperButton = Rect { m_focusCanvas ? toolbarRight - 231.0f : 309.0f, toolbarTop,
        m_focusCanvas ? toolbarRight - 141.0f : 399.0f, toolbarTop + 24.0f };
    m_resetButton = Rect { m_focusCanvas ? toolbarRight - 135.0f : 405.0f, toolbarTop, toolbarRight,
        toolbarTop + 24.0f };
    m_catalog = Rect { width - 386.0f, 50.0f, width - 18.0f, std::min(height - 22.0f, 462.0f) };
    m_panelScroll = std::clamp(m_panelScroll, 0,
        std::max(0,
            static_cast<int>(overlayEditorSectionHeight(m_model->settings.overlay, m_section)
                - m_controls.height() + 4.0f)));
    if (!m_labelEdit) {
        return;
    }
    const float y = m_inspector.top;
    const auto px
        = [this](float coordinate) { return static_cast<int>(std::lround(coordinate * m_dpiScale)); };
    const bool rtss = m_rtssSelected >= 0 && static_cast<std::size_t>(m_rtssSelected) < rtssLayerCount();
    SetWindowPos(m_labelEdit, nullptr, px(112), px(y + (rtss ? 40 : 44)), px(rtss ? 342.0f : 390.0f), px(26),
        SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_labelX, nullptr, px(rtss ? 66.0f : 106.0f), px(y + (rtss ? 105.0f : 92.0f)), px(58),
        px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_valueX, nullptr, px(rtss ? 181.0f : 275.0f), px(y + (rtss ? 105.0f : 92.0f)), px(58),
        px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_y, nullptr, px(rtss ? 452.0f : 408.0f), px(y + (rtss ? 72.0f : 92.0f)),
        px(rtss ? 52.0f : 58.0f), px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_alphaEdit, nullptr, px(rtss ? 299.0f : 472.0f), px(y + (rtss ? 105.0f : 139.0f)),
        px(rtss ? 58.0f : 42.0f), px(rtss ? 26.0f : 24.0f), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(
        m_rtssHeightEdit, nullptr, px(418), px(y + 105), px(58), px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_hexEdit, nullptr, px(112), px(y + 72), px(rtss ? 112.0f : 138.0f),
        px(rtss ? 26.0f : 20.0f), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_rtssBgEdit, nullptr, px(265), px(y + 72), px(125), px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(
        m_catalogSearch, nullptr, px(width - 380), px(16), px(256), px(26), SWP_NOZORDER | SWP_NOACTIVATE);
    if (m_sourceTextEdit && m_sourceTextOpen) {
        const Rect panel = sourceTextPanel();
        SetWindowPos(m_sourceTextEdit, HWND_TOP, px(panel.left + 16.0f), px(panel.top + 50.0f),
            px(panel.width() - 32.0f), px(panel.height() - 112.0f), SWP_NOACTIVATE);
    }
}

Rect OverlayEditor::sourceTextPanel() const
{
    return { m_canvas.left + 16.0f, m_canvas.top + 12.0f, m_canvas.right - 16.0f, m_canvas.bottom - 12.0f };
}

void OverlayEditor::openSourceText()
{
    if (m_sourceTextOpen || m_rtssSelected < 0
        || static_cast<std::size_t>(m_rtssSelected) >= rtssLayerCount()) {
        return;
    }
    SetFocus(m_window); // Commit any pending one-line edit first.
    m_sourceTextLayer = m_rtssSelected;
    m_sourceTextTemplate = m_model->settings.overlay.rtssTemplate;
    const auto index = static_cast<std::size_t>(m_sourceTextLayer);
    const auto& edit = m_model->settings.overlay.rtssLayerEdits[index];
    const auto& layer = m_model->settings.overlay.rtssVisual->layers[index];
    const std::wstring text = edit.text ? widen(*edit.text) : rtssLayerName(layer.text);
    SetWindowTextW(m_sourceTextEdit, text.c_str());
    m_sourceTextOpen = true;
    m_focusCanvas = true;
    layout();
    ShowWindow(m_sourceTextEdit, SW_SHOW);
    SetFocus(m_sourceTextEdit);
    refresh();
}

void OverlayEditor::finishSourceText(bool apply)
{
    if (!m_sourceTextOpen) {
        return;
    }
    bool modified = false;
    if (apply && m_model && m_sourceTextTemplate == m_model->settings.overlay.rtssTemplate
        && m_sourceTextLayer >= 0 && static_cast<std::size_t>(m_sourceTextLayer) < rtssLayerCount()) {
        std::wstring text(static_cast<std::size_t>(GetWindowTextLengthW(m_sourceTextEdit)) + 1, L'\0');
        const int length = GetWindowTextW(m_sourceTextEdit, text.data(), static_cast<int>(text.size()));
        text.resize(static_cast<std::size_t>(length));
        std::wstring normalized;
        normalized.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == L'\r' && i + 1 < text.size() && text[i + 1] == L'\n') {
                continue;
            }
            normalized.push_back(text[i] == L'\r' ? L'\n' : text[i]);
        }
        const std::string utf8 = narrow(normalized);
        if (utf8.size() > 8192) {
            MessageBoxW(m_window, L"Строка RTSS не может превышать 8192 байт UTF-8.",
                L"Слишком длинная строка", MB_OK | MB_ICONWARNING);
            SetFocus(m_sourceTextEdit);
            return;
        }
        const auto index = static_cast<std::size_t>(m_sourceTextLayer);
        auto& edit = m_model->settings.overlay.rtssLayerEdits[index];
        const auto& layer = m_model->settings.overlay.rtssVisual->layers[index];
        const std::wstring before = edit.text ? widen(*edit.text) : rtssLayerName(layer.text);
        if (normalized != before) {
            edit.text = utf8;
            modified = true;
        }
    }
    m_sourceTextOpen = false;
    m_sourceTextLayer = -1;
    m_sourceTextTemplate.reset();
    ShowWindow(m_sourceTextEdit, SW_HIDE);
    SetFocus(m_window);
    syncFields();
    if (modified) {
        changed(true);
    } else {
        refresh();
    }
}

void OverlayEditor::syncFields()
{
    if (!m_model || !m_labelEdit) {
        return;
    }
    const bool isRtss = m_rtssSelected >= 0 && static_cast<std::size_t>(m_rtssSelected) < rtssLayerCount();
    for (HWND field : { m_labelX, m_valueX, m_alphaEdit }) {
        ShowWindow(field, SW_SHOW);
    }
    ShowWindow(m_rtssHeightEdit, isRtss ? SW_SHOW : SW_HIDE);
    ShowWindow(m_rtssBgEdit, isRtss ? SW_SHOW : SW_HIDE);
    for (HWND field : { m_labelEdit, m_y, m_hexEdit }) {
        ShowWindow(field, SW_SHOW);
    }
    SendMessageW(m_labelEdit, EM_SETLIMITTEXT, isRtss ? 8192 : 64, 0);
    layout();
    if (isRtss) {
        const auto index = static_cast<std::size_t>(m_rtssSelected);
        const auto& edit = m_model->settings.overlay.rtssLayerEdits[index];
        const auto& layer = m_model->settings.overlay.rtssVisual->layers[index];
        m_syncing = true;
        const std::wstring source = edit.text ? widen(*edit.text) : rtssLayerName(layer.text);
        input(m_labelEdit, source.c_str());
        const std::uint32_t color = edit.textColor.value_or(layer.textColor);
        const std::wstring hex = widen(overlayRgbaHex((color << 8) | (color >> 24)));
        input(m_hexEdit, hex.c_str());
        const auto background = edit.backgroundColor.value_or(layer.backgroundColor);
        const std::wstring bgHex = edit.backgroundColor || layer.hasBackground
            ? widen(overlayRgbaHex((background << 8) | (background >> 24)))
            : L"";
        input(m_rtssBgEdit, bgHex.c_str());
        input(m_y, std::to_wstring(edit.size == 0 ? layer.size : edit.size).c_str());
        input(m_labelX, std::to_wstring(edit.x).c_str());
        input(m_valueX, std::to_wstring(edit.y).c_str());
        input(m_alphaEdit, std::to_wstring(edit.width).c_str());
        input(m_rtssHeightEdit, std::to_wstring(edit.height).c_str());
        m_syncing = false;
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
    SetFocus(m_window);
    m_rtssSelected = -1;
    m_selectedRtssLayers.clear();
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
    const bool sameRtss = previous.overlay.rtssTemplate == current.overlay.rtssTemplate
        && previous.overlay.rtssExchange == current.overlay.rtssExchange;
    previous.overlay.rtssTemplate.reset();
    previous.overlay.rtssExchange.reset();
    current.overlay.rtssTemplate.reset();
    current.overlay.rtssExchange.reset();
    if (sameRtss && serializeSettings(previous) == serializeSettings(current)) {
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
    if (m_dragging || m_panning || m_rtssDragging || m_rtssResizing || m_styledRowDragging || m_scaleDragging
        || m_paddingDragging) {
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
    if (m_canvasTemplate != m_model->settings.overlay.rtssTemplate) {
        m_selectedRtssLayers.clear();
        m_rtssSelected = -1;
    }
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

void OverlayEditor::fitCanvasToContent()
{
    Rect content = m_screenPlate;
    if (rtssLayerCount() > 0) {
        bool found = false;
        for (const Rect& layer : m_rtssLayerRects) {
            if (layer.right <= layer.left || layer.bottom <= layer.top) {
                continue;
            }
            if (!found) {
                content = layer;
                found = true;
            } else {
                content.left = std::min(content.left, layer.left);
                content.top = std::min(content.top, layer.top);
                content.right = std::max(content.right, layer.right);
                content.bottom = std::max(content.bottom, layer.bottom);
            }
        }
    }
    if (content.width() <= 0.0f || content.height() <= 0.0f) {
        return;
    }
    const ScreenBox bounds { static_cast<int>(std::floor(content.left)),
        static_cast<int>(std::floor(content.top)), static_cast<int>(std::ceil(content.right)),
        static_cast<int>(std::ceil(content.bottom)) };
    const CanvasFit fit = fitCanvasContent(bounds, m_canvas.width(), m_canvas.height(),
        static_cast<float>(GetSystemMetrics(SM_CXVIRTUALSCREEN)));
    m_canvasZoom = fit.zoom;
    m_panX = fit.panX;
    m_panY = fit.panY;
}

void OverlayEditor::alignSelectedRows(int edge)
{
    if (rtssLayerCount() > 0) {
        alignSelectedRtssLayers(edge);
        return;
    }
    if (m_canvasRowBounds.empty() || edge < 0 || edge > 5) {
        return;
    }
    auto& settings = m_model->settings.overlay;
    const auto selected = [this](MetricId id) {
        return m_selectedRows.empty()
            ? id == m_selected
            : std::find(m_selectedRows.begin(), m_selectedRows.end(), id) != m_selectedRows.end();
    };
    const auto reference = std::find_if(m_canvasRowBounds.begin(), m_canvasRowBounds.end(),
        [&selected](const OverlayRowBounds& row) { return selected(row.id); });
    if (reference == m_canvasRowBounds.end()) {
        return;
    }
    const bool vertical = edge >= 3;
    const bool single = std::count_if(m_canvasRowBounds.begin(), m_canvasRowBounds.end(),
                            [&selected](const OverlayRowBounds& row) { return selected(row.id); })
        == 1;
    const Rect& anchor = single ? m_screenPlate : reference->rect;
    const float unit = std::max(0.001f, m_canvasRowScale);
    bool changedAny = false;
    for (const OverlayRowBounds& item : m_canvasRowBounds) {
        if (!selected(item.id) || (!single && item.id == reference->id)) {
            continue;
        }
        const float delta = vertical
            ? alignCanvasEdge(anchor.top, anchor.bottom, item.rect.top, item.rect.bottom, edge - 3)
            : alignCanvasEdge(anchor.left, anchor.right, item.rect.left, item.rect.right, edge);
        const int shift = static_cast<int>(std::lround(delta / unit));
        if (shift == 0) {
            continue;
        }
        if (settings.pixelLayout && settings.layout == OverlayLayout::Plates) {
            auto& position = settings.rowPositions[static_cast<std::size_t>(item.id)];
            if (vertical) {
                position.y = std::clamp(position.y + shift, 0, kMaxY);
            } else {
                const int dx = std::clamp(shift, -std::min(position.labelX, position.valueX),
                    470 - std::max(position.labelX, position.valueX));
                position.labelX += dx;
                position.valueX += dx;
            }
        } else {
            auto& offset = settings.styleRowOffsets[static_cast<std::size_t>(item.id)];
            int& coordinate = vertical ? offset.y : offset.x;
            coordinate = std::clamp(coordinate + shift, -2048, 2048);
        }
        changedAny = true;
    }
    if (changedAny) {
        syncFields();
        changed(true);
    }
}

void OverlayEditor::alignSelectedRtssLayers(int edge)
{
    if (edge < 0 || edge > 5) {
        return;
    }
    const auto visible = [this](std::size_t index) {
        return index < m_rtssLayerRects.size() && m_rtssLayerRects[index].right > m_rtssLayerRects[index].left
            && m_rtssLayerRects[index].bottom > m_rtssLayerRects[index].top;
    };
    if (std::count_if(m_selectedRtssLayers.begin(), m_selectedRtssLayers.end(), visible) < 2) {
        const float width = static_cast<float>(GetSystemMetrics(SM_CXVIRTUALSCREEN));
        const float height = static_cast<float>(GetSystemMetrics(SM_CYVIRTUALSCREEN));
        if (m_screenPlate.width() <= 0.0f || m_screenPlate.height() <= 0.0f) {
            return;
        }
        const Rect screen { 18.0f, 18.0f, width - 18.0f, height - 18.0f };
        const bool vertical = edge >= 3;
        const float delta = vertical
            ? alignCanvasEdge(screen.top, screen.bottom, m_screenPlate.top, m_screenPlate.bottom, edge - 3)
            : alignCanvasEdge(screen.left, screen.right, m_screenPlate.left, m_screenPlate.right, edge);
        if (std::abs(delta) < 0.5f) {
            return;
        }
        auto& settings = m_model->settings.overlay;
        if (settings.anchorMode != OverlayAnchorMode::Free) {
            settings.originX = static_cast<int>(std::lround(m_screenPlate.left));
            settings.originY = static_cast<int>(std::lround(m_screenPlate.top));
            settings.anchorMode = OverlayAnchorMode::Free;
            settings.offsetX = settings.offsetY = 0;
        }
        int& origin = vertical ? settings.originY : settings.originX;
        origin = std::clamp(origin + static_cast<int>(std::lround(delta)), 0, 32768);
        changed(true);
        return;
    }
    const auto first = std::find_if(m_selectedRtssLayers.begin(), m_selectedRtssLayers.end(), visible);
    const std::size_t anchor = *first;
    const Rect& reference = m_rtssLayerRects[anchor];
    const bool vertical = edge >= 3;
    const int side = vertical ? edge - 3 : edge;
    auto& edits = m_model->settings.overlay.rtssLayerEdits;
    bool moved = false;
    for (std::size_t index : m_selectedRtssLayers) {
        if (index == anchor || !visible(index) || index >= edits.size()) {
            continue;
        }
        const Rect& box = m_rtssLayerRects[index];
        const float delta = vertical
            ? alignCanvasEdge(reference.top, reference.bottom, box.top, box.bottom, side)
            : alignCanvasEdge(reference.left, reference.right, box.left, box.right, side);
        int& coordinate = vertical ? edits[index].y : edits[index].x;
        const int aligned = std::clamp(
            coordinate + static_cast<int>(std::lround(delta / std::max(0.001f, m_rtssLayerScale))), -2048,
            2048);
        moved |= aligned != coordinate;
        coordinate = aligned;
    }
    if (moved) {
        syncFields();
        changed(true);
    }
}

void OverlayEditor::selectRtssLayer(int index, bool additive)
{
    SetFocus(m_window);
    const auto selected = std::find(
        m_selectedRtssLayers.begin(), m_selectedRtssLayers.end(), static_cast<std::size_t>(index));
    if (!additive) {
        m_selectedRtssLayers = { static_cast<std::size_t>(index) };
    } else if (selected == m_selectedRtssLayers.end()) {
        m_selectedRtssLayers.push_back(static_cast<std::size_t>(index));
    } else if (m_selectedRtssLayers.size() > 1) {
        m_selectedRtssLayers.erase(selected);
        index = static_cast<int>(m_selectedRtssLayers.back());
    }
    m_rtssSelected = index;
    m_model->editingLabel = -1;
    syncFields();
    refresh();
}

void OverlayEditor::nudgeRtssLayers(int dx, int dy)
{
    auto& edits = m_model->settings.overlay.rtssLayerEdits;
    for (std::size_t index : m_selectedRtssLayers) {
        if (index >= edits.size()) {
            return;
        }
        dx = std::clamp(dx, -2048 - edits[index].x, 2048 - edits[index].x);
        dy = std::clamp(dy, -2048 - edits[index].y, 2048 - edits[index].y);
    }
    if (dx == 0 && dy == 0) {
        return;
    }
    for (std::size_t index : m_selectedRtssLayers) {
        edits[index].x += dx;
        edits[index].y += dy;
    }
    syncFields();
    changed(true);
}

std::size_t OverlayEditor::rtssLayerCount() const
{
    const auto& settings = m_model->settings.overlay;
    return settings.recipe == OverlayRecipe::Rtss && settings.rtssTemplate && settings.rtssVisual
        ? settings.rtssVisual->layers.size()
        : 0;
}

void OverlayEditor::moveRtssLayer(int x, int y)
{
    if (m_rtssSelected < 0 || static_cast<std::size_t>(m_rtssSelected) >= rtssLayerCount()) {
        return;
    }
    auto& edit = m_model->settings.overlay.rtssLayerEdits[static_cast<std::size_t>(m_rtssSelected)];
    if (m_rtssResizing) {
        const int width = std::clamp(
            m_rtssWidthStart + static_cast<int>(std::lround((x - m_rtssStartX) / m_rtssDragUnit)), 16, 2048);
        const int height = std::clamp(
            m_rtssHeightStart + static_cast<int>(std::lround((y - m_rtssStartY) / m_rtssDragUnit)), 16, 2048);
        if (width != edit.width || height != edit.height) {
            edit.width = width;
            edit.height = height;
            changed(false);
        }
        return;
    }
    int dx = static_cast<int>(std::lround((x - m_rtssStartX) / m_rtssDragUnit));
    int dy = static_cast<int>(std::lround((y - m_rtssStartY) / m_rtssDragUnit));
    for (const auto& entry : m_rtssDragOffsets) {
        dx = std::clamp(dx, -2048 - entry.second.x, 2048 - entry.second.x);
        dy = std::clamp(dy, -2048 - entry.second.y, 2048 - entry.second.y);
    }
    bool moved = false;
    for (const auto& [index, start] : m_rtssDragOffsets) {
        auto& layer = m_model->settings.overlay.rtssLayerEdits[index];
        moved |= layer.x != start.x + dx || layer.y != start.y + dy;
        layer.x = start.x + dx;
        layer.y = start.y + dy;
    }
    if (moved) {
        changed(false);
    }
}

void OverlayEditor::moveStyledRow(int x, int y)
{
    const int dx = static_cast<int>(std::lround((x - m_dragStartX) / m_styledRowUnit));
    const int dy = static_cast<int>(std::lround((y - m_dragStartY) / m_styledRowUnit));
    OverlayRowOffset next = moveOverlayRowOffset(m_styledRowStart, dx, dy);
    const int minX = static_cast<int>(std::ceil(-m_styledRowBounds.left / m_canvasRowScale));
    const int maxX = static_cast<int>(
        std::floor((GetSystemMetrics(SM_CXVIRTUALSCREEN) - m_styledRowBounds.right) / m_canvasRowScale));
    const int minY = static_cast<int>(std::ceil(-m_styledRowBounds.top / m_canvasRowScale));
    const int maxY = static_cast<int>(
        std::floor((GetSystemMetrics(SM_CYVIRTUALSCREEN) - m_styledRowBounds.bottom) / m_canvasRowScale));
    next.x = clampDraggedRow(next.x, m_styledRowStart.x, minX, maxX);
    next.y = clampDraggedRow(next.y, m_styledRowStart.y, minY, maxY);
    auto& current = m_model->settings.overlay.styleRowOffsets[static_cast<std::size_t>(m_styledRowId)];
    if (current.x != next.x || current.y != next.y) {
        current = next;
        changed(false);
    }
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

Rect OverlayEditor::scaleGrip() const
{
    const Rect plate = plateOnCanvas();
    return { plate.right - 9.0f, plate.bottom - 9.0f, plate.right + 8.0f, plate.bottom + 8.0f };
}

Rect OverlayEditor::paddingGrip() const
{
    const auto& settings = m_model->settings.overlay;
    if (settings.layout != OverlayLayout::Plates || settings.pixelLayout || rtssLayerCount() > 0) {
        return { };
    }
    const Rect plate = plateOnCanvas();
    const float unit = canvasScale() * settings.scalePercent / 100.0f;
    const float offset = (settings.paddingPx < 0 ? 10.0f : static_cast<float>(settings.paddingPx)) * unit;
    return { plate.left + offset - 6.0f, plate.top + offset - 6.0f, plate.left + offset + 7.0f,
        plate.top + offset + 7.0f };
}

void OverlayEditor::moveGrip(int x, int y)
{
    auto& settings = m_model->settings.overlay;
    if (m_scaleDragging && m_backgroundResizing) {
        const int width = std::clamp(
            m_backgroundStartWidth + static_cast<int>(std::lround((x - m_dragStartX) / m_gripUnit)), 80,
            2000);
        const int height = std::clamp(
            m_backgroundStartHeight + static_cast<int>(std::lround((y - m_dragStartY) / m_gripUnit)), 40,
            1600);
        if (settings.backgroundWidthPx != width || settings.backgroundHeightPx != height) {
            settings.backgroundWidthPx = width;
            settings.backgroundHeightPx = height;
            changed(false);
        }
    } else if (m_scaleDragging) {
        const float dx = static_cast<float>(x - m_dragStartX) / std::max(1.0f, m_gripWidth);
        const float dy = static_cast<float>(y - m_dragStartY) / std::max(1.0f, m_gripHeight);
        const int next
            = std::clamp(static_cast<int>(std::lround(m_gripStart * (1.0f + (dx + dy) / 2.0f))), 70, 160);
        if (next != settings.scalePercent) {
            settings.scalePercent = next;
            changed(false);
        }
    } else if (m_paddingDragging) {
        const float unit = std::max(0.001f, canvasScale() * settings.scalePercent / 100.0f);
        const int delta
            = static_cast<int>(std::lround(((x - m_dragStartX) + (y - m_dragStartY)) / (2.0f * unit)));
        const int next = std::clamp(m_gripStart + delta, 0, 28);
        if (next != settings.paddingPx) {
            settings.paddingPx = next;
            changed(false);
        }
    }
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
    case WM_CTLCOLOREDIT:
        if (m_editBrush) {
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(233, 238, 244));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(21, 27, 37));
            return reinterpret_cast<LRESULT>(m_editBrush);
        }
        break;
    case WM_CLOSE:
        if (m_sourceTextOpen) {
            finishSourceText(false);
        } else {
            close();
        }
        return 0;
    case kSourceTextDone:
        finishSourceText(wParam != 0);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT cursor { };
            if (GetCursorPos(&cursor) && ScreenToClient(m_window, &cursor)) {
                cursor.x = logicalCoordinate(cursor.x);
                cursor.y = logicalCoordinate(cursor.y);
                if (m_sourceTextOpen) {
                    const Rect panel = sourceTextPanel();
                    const bool button = cursor.y >= panel.bottom - 48.0f && cursor.y < panel.bottom - 14.0f
                        && ((cursor.x >= panel.right - 258.0f && cursor.x < panel.right - 142.0f)
                            || (cursor.x >= panel.right - 132.0f && cursor.x < panel.right - 16.0f));
                    SetCursor(LoadCursorW(nullptr, button ? IDC_HAND : IDC_ARROW));
                    return TRUE;
                }
                const int inspectorX = cursor.x - static_cast<int>(m_inspector.left);
                const int inspectorY = cursor.y - static_cast<int>(m_inspector.top);
                if (m_rtssSelected >= 0 && static_cast<std::size_t>(m_rtssSelected) < rtssLayerCount()
                    && ((inspectorY >= 40 && inspectorY < 70
                            && ((inspectorX >= 12 && inspectorX < 90)
                                || (inspectorX >= 440 && inspectorX < 484)))
                        || (inspectorY >= 132 && inspectorY <= 166
                            && ((inspectorX >= 410 && inspectorX < 452)
                                || (inspectorX >= 462 && inspectorX < 504))))) {
                    SetCursor(LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
                if (m_canvas.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))
                    && m_screenPlate.width() > 0.0f && m_screenPlate.height() > 0.0f
                    && (scaleGrip().contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))
                        || paddingGrip().contains(
                            static_cast<float>(cursor.x), static_cast<float>(cursor.y)))) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENWSE));
                    return TRUE;
                }
                if (m_canvas.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))
                    && !m_model->settings.overlay.pixelLayout && GetKeyState(VK_MENU) >= 0) {
                    const CanvasAxis horizontal { m_canvas.left, m_panX, canvasScale() };
                    const CanvasAxis vertical { m_canvas.top, m_panY, canvasScale() };
                    for (const auto& row : m_canvasRowBounds) {
                        if (Rect { horizontal.toCanvas(row.rect.left), vertical.toCanvas(row.rect.top),
                                horizontal.toCanvas(row.rect.right), vertical.toCanvas(row.rect.bottom) }
                                .contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))) {
                            SetCursor(LoadCursorW(nullptr, IDC_HAND));
                            return TRUE;
                        }
                    }
                }
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
        m_labelEdit = m_labelX = m_valueX = m_y = m_alphaEdit = m_rtssHeightEdit = m_hexEdit = m_rtssBgEdit
            = m_numericEdit = m_catalogSearch = m_sourceTextEdit = nullptr;
        m_sourceTextOpen = false;
        m_sourceTextLayer = -1;
        m_sourceTextTemplate.reset();
        m_renderer.shutdown();
        if (m_editBrush) {
            DeleteObject(m_editBrush);
            m_editBrush = nullptr;
        }
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
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        const HMONITOR monitor = MonitorFromRect(suggested, MONITOR_DEFAULTTONEAREST);
        m_dpiScale = editorScale(monitor, static_cast<float>(HIWORD(wParam)) / 96.0f);
        MONITORINFO work { };
        work.cbSize = sizeof(work);
        int x = suggested->left;
        int y = suggested->top;
        int width = suggested->right - suggested->left;
        int height = suggested->bottom - suggested->top;
        if (GetMonitorInfoW(monitor, &work)) {
            const int left = static_cast<int>(work.rcWork.left);
            const int top = static_cast<int>(work.rcWork.top);
            const int right = static_cast<int>(work.rcWork.right);
            const int bottom = static_cast<int>(work.rcWork.bottom);
            width = std::min(width, right - left);
            height = std::min(height, bottom - top);
            x = std::clamp(x, left, right - width);
            y = std::clamp(y, top, bottom - height);
        }
        SetWindowPos(m_window, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
        m_hdrDisplay = hdrAtWindow(m_window);
        layout();
        refresh();
        return 0;
    }
    case WM_EXITSIZEMOVE:
    case WM_DISPLAYCHANGE:
        if (m_window != nullptr) {
            m_hdrDisplay = hdrAtWindow(m_window);
            refresh();
        }
        break;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = POINT { static_cast<LONG>(1000.0f * m_dpiScale + 20.0f),
            static_cast<LONG>(720.0f * m_dpiScale + 40.0f) };
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
        if (m_sourceTextOpen) {
            return 0;
        }
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
                    static_cast<int>(rows.size() + hiddenLayerRows(m_hiddenLayers, rows).size()
                        + rtssLayerCount()) - visible));
        } else if (m_catalogOpen
            && m_catalog.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))) {
            m_catalogScroll = std::clamp(
                m_catalogScroll - delta / 28, 0, std::max(0, static_cast<int>(catalogRows().size()) - 12));
        } else if (m_canvas.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))) {
            const float previous = canvasScale();
            m_canvasZoom = std::clamp(m_canvasZoom * (delta > 0 ? 1.15f : 1.0f / 1.15f), 0.15f, 20.0f);
            m_panX = CanvasAxis { m_canvas.left, m_panX, previous }.panAtZoom(
                static_cast<float>(cursor.x), canvasScale());
            m_panY = CanvasAxis { m_canvas.top, m_panY, previous }.panAtZoom(
                static_cast<float>(cursor.y), canvasScale());
        } else if (m_controls.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))) {
            m_panelScroll = std::clamp(m_panelScroll - delta, 0,
                std::max(0,
                    static_cast<int>(overlayEditorSectionHeight(m_model->settings.overlay, m_section)
                        - m_controls.height() + 4.0f)));
        }
        refresh();
        return 0;
    }
    case WM_COMMAND: {
        if (m_sourceTextOpen) {
            return 0;
        }
        const HWND field = reinterpret_cast<HWND>(lParam);
        if (m_rtssSelected >= 0 && static_cast<std::size_t>(m_rtssSelected) < rtssLayerCount()
            && (field == m_labelEdit || field == m_labelX || field == m_valueX || field == m_y
                || field == m_alphaEdit || field == m_rtssHeightEdit || field == m_hexEdit
                || field == m_rtssBgEdit)) {
            auto& edit = m_model->settings.overlay.rtssLayerEdits[static_cast<std::size_t>(m_rtssSelected)];
            if (field == m_labelEdit && HIWORD(wParam) == EN_CHANGE && !m_syncing) {
                wchar_t buffer[8193] { };
                GetWindowTextW(field, buffer, 8193);
                std::string text = narrow(buffer);
                if (text.size() > 8192) {
                    const auto& layer = m_model->settings.overlay.rtssVisual->layers[
                        static_cast<std::size_t>(m_rtssSelected)];
                    const std::wstring previous = edit.text ? widen(*edit.text) : rtssLayerName(layer.text);
                    m_syncing = true;
                    input(m_labelEdit, previous.c_str());
                    m_syncing = false;
                    return 0;
                }
                edit.text = std::move(text);
                changed(false);
            } else if (field == m_labelEdit && HIWORD(wParam) == EN_KILLFOCUS && !m_syncing) {
                changed(true);
            } else if (field == m_hexEdit && HIWORD(wParam) == EN_KILLFOCUS && !m_syncing) {
                wchar_t buffer[16] { };
                GetWindowTextW(field, buffer, 16);
                std::uint32_t rgba = 0;
                if (buffer[0] == L'\0') {
                    if (edit.textColor) {
                        edit.textColor.reset();
                        changed(true);
                    }
                } else if (parseOverlayRgba(narrow(buffer), rgba)) {
                    edit.textColor = (rgba >> 8) | (rgba << 24);
                    changed(true);
                } else {
                    MessageBoxW(m_window, L"Цвет текста: #RRGGBBAA или пустое поле (из RTSS).",
                        L"Неверный цвет RGBA", MB_OK | MB_ICONWARNING);
                }
                syncFields();
            } else if (field == m_rtssBgEdit && HIWORD(wParam) == EN_KILLFOCUS && !m_syncing) {
                wchar_t buffer[16] { };
                GetWindowTextW(field, buffer, 16);
                std::uint32_t rgba = 0;
                if (buffer[0] == L'\0') {
                    if (edit.backgroundColor) {
                        edit.backgroundColor.reset();
                        changed(true);
                    }
                } else if (parseOverlayRgba(narrow(buffer), rgba)) {
                    edit.backgroundColor = (rgba >> 8) | (rgba << 24);
                    changed(true);
                } else {
                    MessageBoxW(m_window, L"Фон слоя: #RRGGBBAA или пустое поле (из RTSS).",
                        L"Неверный цвет RGBA", MB_OK | MB_ICONWARNING);
                }
                syncFields();
            } else if (field == m_y && HIWORD(wParam) == EN_KILLFOCUS && !m_syncing) {
                wchar_t buffer[16] { };
                GetWindowTextW(field, buffer, 16);
                wchar_t* end = nullptr;
                const long size = std::wcstol(buffer, &end, 10);
                if (end != buffer && *end == L'\0' && size >= 10 && size <= 400) {
                    edit.size = static_cast<int>(size);
                    changed(true);
                } else {
                    MessageBoxW(m_window, L"Размер шрифта RTSS: от 10 до 400 %.", L"Неверный размер",
                        MB_OK | MB_ICONWARNING);
                }
                syncFields();
            } else if ((field == m_labelX || field == m_valueX || field == m_alphaEdit
                           || field == m_rtssHeightEdit)
                && HIWORD(wParam) == EN_KILLFOCUS && !m_syncing) {
                wchar_t buffer[16] { };
                GetWindowTextW(field, buffer, 16);
                wchar_t* end = nullptr;
                const long number = std::wcstol(buffer, &end, 10);
                const bool extent = field == m_alphaEdit || field == m_rtssHeightEdit;
                const bool valid = end != buffer && *end == L'\0'
                    && (extent ? number == 0 || (number >= 16 && number <= 2048)
                               : number >= -2048 && number <= 2048);
                if (valid) {
                    int& coordinate = field == m_labelX ? edit.x
                        : field == m_valueX             ? edit.y
                        : field == m_alphaEdit          ? edit.width
                                                        : edit.height;
                    if (coordinate != number) {
                        coordinate = static_cast<int>(number);
                        changed(true);
                    }
                } else {
                    MessageBoxW(m_window,
                        extent ? L"Размер слоя: 0 (из RTSS) или от 16 до 2048 px."
                               : L"Смещение слоя: от -2048 до 2048 px.",
                        L"Неверная геометрия", MB_OK | MB_ICONWARNING);
                }
                syncFields();
            }
            return 0;
        }
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
        if (m_sourceTextOpen) {
            const Rect panel = sourceTextPanel();
            if (x >= panel.right - 258.0f && x < panel.right - 142.0f && y >= panel.bottom - 48.0f
                && y < panel.bottom - 14.0f) {
                finishSourceText(true);
            } else if (x >= panel.right - 132.0f && x < panel.right - 16.0f && y >= panel.bottom - 48.0f
                && y < panel.bottom - 14.0f) {
                finishSourceText(false);
            }
            return 0;
        }
        if (x >= 20 && x < 159 && y >= 37 && y < 62) {
            m_focusCanvas = !m_focusCanvas;
            m_catalogOpen = false;
            ShowWindow(m_catalogSearch, SW_HIDE);
            m_panelScroll = 0;
            m_fitPending = true;
            layout();
            refresh();
            return 0;
        }
        if (m_focusCanvas && y >= 63 && y < 85 && x >= 184 && x < 184 + 6 * 88) {
            alignSelectedRows((x - 184) / 88);
            return 0;
        }
        if (m_fitButton.contains(static_cast<float>(x), static_cast<float>(y))) {
            m_samplingColor = false;
            m_fitPending = true;
            refresh();
            return 0;
        }
        if (m_eyedropperButton.contains(static_cast<float>(x), static_cast<float>(y))) {
            m_samplingColor = !m_samplingColor;
            refresh();
            return 0;
        }
        if (m_samplingColor) {
            m_samplingColor = false;
            const bool preview = m_preview.contains(static_cast<float>(x), static_cast<float>(y));
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
                        if (m_rtssSelected >= 0
                            && static_cast<std::size_t>(m_rtssSelected) < rtssLayerCount()) {
                            const auto index = static_cast<std::size_t>(m_rtssSelected);
                            auto& edit = m_model->settings.overlay.rtssLayerEdits[index];
                            const auto& layer = m_model->settings.overlay.rtssVisual->layers[index];
                            const std::uint32_t current = edit.textColor.value_or(layer.textColor);
                            const std::uint32_t alpha = (current >> 24) == 0 ? 0xFFu : (current >> 24);
                            edit.textColor = (alpha << 24)
                                | (static_cast<std::uint32_t>(GetRValue(sampled)) << 16)
                                | (static_cast<std::uint32_t>(GetGValue(sampled)) << 8)
                                | static_cast<std::uint32_t>(GetBValue(sampled));
                        } else {
                            auto& rgba
                                = m_model->settings.overlay.metricRgba[static_cast<std::size_t>(m_selected)];
                            const std::uint32_t alpha = rgba ? *rgba & 0xFFu : 0xFFu;
                            rgba = (static_cast<std::uint32_t>(GetRValue(sampled)) << 24)
                                | (static_cast<std::uint32_t>(GetGValue(sampled)) << 16)
                                | (static_cast<std::uint32_t>(GetBValue(sampled)) << 8) | alpha;
                        }
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
        if (x >= static_cast<int>(m_catalog.right - 350.0f)
            && x < static_cast<int>(m_catalog.right - 230.0f) && y >= 12 && y < 46) {
            m_onAction(UiHit { { }, ActionKind::ImportOverlayRecipe, 0 }, 0.0f, true);
            recordHistory();
            syncFields();
            refresh();
            return 0;
        }
        if (x >= static_cast<int>(m_catalog.right - 224.0f) && x < static_cast<int>(m_catalog.right - 112.0f)
            && y >= 12 && y < 46) {
            m_onAction(UiHit { { }, ActionKind::ExportOverlayRecipe, 0 }, 0.0f, true);
            return 0;
        }
        if (x >= static_cast<int>(m_catalog.right - 108.0f) && y >= 12 && y < 46) {
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
            const float tabWidth = m_controls.width() / static_cast<float>(kSections.size());
            const int tab = static_cast<int>((x - m_controls.left) / tabWidth);
            if (tab >= 0 && tab < static_cast<int>(kSections.size())) {
                m_section = tab;
                m_panelScroll = 0;
                refresh();
            }
            return 0;
        }
        if (m_layers.contains(static_cast<float>(x), static_cast<float>(y))) {
            const int index = (y - static_cast<int>(m_layers.top) - 34) / 28 + m_layersScroll;
            const auto rows = overlayMetricRows(m_model->settings.overlay, m_model->store);
            const auto hidden = hiddenLayerRows(m_hiddenLayers, rows);
            const int firstRtss = static_cast<int>(rows.size() + hidden.size());
            if (y >= m_layers.top + 34 && index >= firstRtss
                && index < firstRtss + static_cast<int>(rtssLayerCount())) {
                selectRtssLayer(index - firstRtss, x >= m_layers.left + 37.0f && GetKeyState(VK_CONTROL) < 0);
                if (x < m_layers.left + 37.0f) {
                    auto& edit
                        = m_model->settings.overlay.rtssLayerEdits[static_cast<std::size_t>(m_rtssSelected)];
                    edit.visible = !edit.visible;
                    changed(true);
                } else {
                    refresh();
                }
                SetFocus(m_window);
                return 0;
            }
            if (y >= m_layers.top + 34 && index >= 0 && index < firstRtss) {
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
        if (m_resetButton.contains(static_cast<float>(x), static_cast<float>(y))) {
            auto& settings = m_model->settings.overlay;
            Settings previous;
            previous.overlay = settings;
            restoreOverlayTemplate(settings);
            Settings restored;
            restored.overlay = settings;
            if (serializeSettings(previous) == serializeSettings(restored)) {
                MessageBoxW(m_window, L"Раскладка уже соответствует шаблону.", L"Вернуть шаблон",
                    MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            m_selectedRows.clear();
            m_fitPending = true;
            syncFields();
            changed(true);
            return 0;
        }
        if (m_inspector.contains(static_cast<float>(x), static_cast<float>(y))) {
            const int localX = x - static_cast<int>(m_inspector.left);
            const int localY = y - static_cast<int>(m_inspector.top);
            if (m_rtssSelected >= 0 && static_cast<std::size_t>(m_rtssSelected) < rtssLayerCount()) {
                auto& edit
                    = m_model->settings.overlay.rtssLayerEdits[static_cast<std::size_t>(m_rtssSelected)];
                if (localY < 35 && localX > 278) {
                    if (localX > 405) {
                        edit.visible = !edit.visible;
                    } else {
                        edit.x = edit.y = 0;
                        edit.text.reset();
                        edit.textColor.reset();
                        edit.backgroundColor.reset();
                        edit.size = 0;
                        edit.width = edit.height = 0;
                        syncFields();
                    }
                    changed(true);
                } else if (localY >= 40 && localY < 70 && localX >= 440 && localX < 484) {
                    openSourceText();
                } else if (localY >= 40 && localY < 70 && localX >= 12 && localX < 90) {
                    SetFocus(m_window);
                    if (edit.text) {
                        edit.text.reset();
                        syncFields();
                        changed(true);
                    }
                } else if (localY >= 132 && localY <= 166 && localX >= 12 && localX < 405) {
                    const int direction = (localX - 12) / 98;
                    const int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
                    nudgeRtssLayers(direction == 0 ? -step : direction == 1 ? step : 0,
                        direction == 2 ? -step : direction == 3 ? step : 0);
                } else if (localY >= 132 && localY <= 166
                    && ((localX >= 410 && localX < 452) || (localX >= 462 && localX < 504))) {
                    SetFocus(m_window);
                    const bool background = localX >= 462;
                    const auto& layer = m_model->settings.overlay.rtssVisual->layers[
                        static_cast<std::size_t>(m_rtssSelected)];
                    auto& selected
                        = m_model->settings.overlay.rtssLayerEdits[static_cast<std::size_t>(m_rtssSelected)];
                    const std::uint32_t current = background
                        ? selected.backgroundColor.value_or(layer.backgroundColor)
                        : selected.textColor.value_or(layer.textColor);
                    static COLORREF custom[16] { };
                    CHOOSECOLORW picker { };
                    picker.lStructSize = sizeof(picker);
                    picker.hwndOwner = m_window;
                    picker.lpCustColors = custom;
                    picker.rgbResult = RGB((current >> 16) & 0xFFu, (current >> 8) & 0xFFu, current & 0xFFu);
                    picker.Flags = CC_RGBINIT | CC_FULLOPEN;
                    if (ChooseColorW(&picker)) {
                        const std::uint32_t alpha = (current >> 24) == 0 ? 0xFFu : (current >> 24);
                        const std::uint32_t color = (alpha << 24)
                            | (static_cast<std::uint32_t>(GetRValue(picker.rgbResult)) << 16)
                            | (static_cast<std::uint32_t>(GetGValue(picker.rgbResult)) << 8)
                            | static_cast<std::uint32_t>(GetBValue(picker.rgbResult));
                        if (background)
                            selected.backgroundColor = color;
                        else
                            selected.textColor = color;
                        changed(true);
                        syncFields();
                    }
                }
                return 0;
            }
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
            if (localY >= 118 && localY < 137 && localX >= 210 && localX < 399) {
                auto& size
                    = m_model->settings.overlay.metricTextScalePercent[static_cast<std::size_t>(m_selected)];
                const int next = localX < 258 ? std::max(70, size - 10)
                    : localX < 306            ? std::min(200, size + 10)
                                              : 100;
                if (size != next) {
                    size = next;
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
            if (rtssLayerCount() == 0 && m_screenPlate.width() > 0.0f && m_screenPlate.height() > 0.0f
                && (scaleGrip().contains(static_cast<float>(x), static_cast<float>(y))
                    || paddingGrip().contains(static_cast<float>(x), static_cast<float>(y)))) {
                m_scaleDragging = scaleGrip().contains(static_cast<float>(x), static_cast<float>(y));
                m_backgroundResizing = m_scaleDragging && rtssLayerCount() == 0
                    && m_model->settings.overlay.layout == OverlayLayout::Plates;
                m_paddingDragging = !m_scaleDragging;
                m_gripUnit = std::max(0.001f, canvasScale() * m_canvasRowScale);
                if (m_backgroundResizing) {
                    auto& settings = m_model->settings.overlay;
                    m_backgroundStartWidth = settings.backgroundWidthPx >= 80
                        ? settings.backgroundWidthPx
                        : static_cast<int>(std::lround(m_screenPlate.width() / m_canvasRowScale));
                    m_backgroundStartHeight = settings.backgroundHeightPx >= 40
                        ? settings.backgroundHeightPx
                        : static_cast<int>(std::lround(m_screenPlate.height() / m_canvasRowScale));
                    if (settings.anchorMode == OverlayAnchorMode::Corner) {
                        settings.originX = static_cast<int>(std::lround(m_screenPlate.left));
                        settings.originY = static_cast<int>(std::lround(m_screenPlate.top));
                        settings.anchorMode = OverlayAnchorMode::Free;
                        settings.offsetX = settings.offsetY = 0;
                    }
                }
                m_dragStartX = x;
                m_dragStartY = y;
                m_gripStart = m_scaleDragging                 ? m_model->settings.overlay.scalePercent
                    : m_model->settings.overlay.paddingPx < 0 ? 10
                                                              : m_model->settings.overlay.paddingPx;
                m_gripWidth = plateOnCanvas().width();
                m_gripHeight = plateOnCanvas().height();
                SetFocus(m_window);
                SetCapture(m_window);
                return 0;
            }
            const CanvasAxis horizontal { m_canvas.left, m_panX, canvasScale() };
            const CanvasAxis vertical { m_canvas.top, m_panY, canvasScale() };
            for (std::size_t index = m_rtssLayerRects.size(); index > 0; --index) {
                const Rect& box = m_rtssLayerRects[index - 1];
                const Rect onCanvas { horizontal.toCanvas(box.left), vertical.toCanvas(box.top),
                    horizontal.toCanvas(box.right), vertical.toCanvas(box.bottom) };
                if (box.right > box.left && onCanvas.contains(static_cast<float>(x), static_cast<float>(y))) {
                    const int selectedIndex = static_cast<int>(index - 1);
                    if (GetKeyState(VK_CONTROL) < 0) {
                        selectRtssLayer(selectedIndex, true);
                        SetFocus(m_window);
                        return 0;
                    }
                    if (std::find(m_selectedRtssLayers.begin(), m_selectedRtssLayers.end(), index - 1)
                        == m_selectedRtssLayers.end()) {
                        selectRtssLayer(selectedIndex, false);
                    } else {
                        m_rtssSelected = selectedIndex;
                        syncFields();
                    }
                    const auto& edit = m_model->settings.overlay.rtssLayerEdits[index - 1];
                    m_rtssResizing = onCanvas.width() > 22.0f && onCanvas.height() > 22.0f
                        && x >= onCanvas.right - 12.0f && y >= onCanvas.bottom - 12.0f;
                    m_rtssDragging = !m_rtssResizing;
                    m_rtssDragUnit = std::max(0.001f, canvasScale() * m_rtssLayerScale);
                    m_rtssWidthStart = edit.width > 0
                        ? edit.width
                        : static_cast<int>(std::lround(box.width() / m_rtssLayerScale));
                    m_rtssHeightStart = edit.height > 0
                        ? edit.height
                        : static_cast<int>(std::lround(box.height() / m_rtssLayerScale));
                    m_rtssStartX = x;
                    m_rtssStartY = y;
                    m_rtssDragOffsets.clear();
                    for (std::size_t layerIndex : m_selectedRtssLayers) {
                        const auto& layerEdit = m_model->settings.overlay.rtssLayerEdits[layerIndex];
                        m_rtssDragOffsets.emplace_back(
                            layerIndex, OverlayRowOffset { layerEdit.x, layerEdit.y });
                    }
                    SetFocus(m_window);
                    SetCapture(m_window);
                    refresh();
                    return 0;
                }
            }
            if (GetKeyState(VK_MENU) >= 0
                && (!m_model->settings.overlay.pixelLayout
                    || m_model->settings.overlay.layout != OverlayLayout::Plates)) {
                for (auto it = m_canvasRowBounds.rbegin(); it != m_canvasRowBounds.rend(); ++it) {
                    const Rect onCanvas { horizontal.toCanvas(it->rect.left), vertical.toCanvas(it->rect.top),
                        horizontal.toCanvas(it->rect.right), vertical.toCanvas(it->rect.bottom) };
                    if (!onCanvas.contains(static_cast<float>(x), static_cast<float>(y))) {
                        continue;
                    }
                    if (GetKeyState(VK_CONTROL) < 0) {
                        const auto selected = std::find(m_selectedRows.begin(), m_selectedRows.end(), it->id);
                        if (selected == m_selectedRows.end())
                            m_selectedRows.push_back(it->id);
                        else
                            m_selectedRows.erase(selected);
                        select(it->id, false);
                        return 0;
                    }
                    select(it->id, false);
                    m_styledRowDragging = true;
                    m_styledRowId = it->id;
                    m_styledRowStart
                        = m_model->settings.overlay.styleRowOffsets[static_cast<std::size_t>(it->id)];
                    m_styledRowBounds = it->rect;
                    m_styledRowUnit = std::max(0.001f, canvasScale() * m_canvasRowScale);
                    m_dragStartX = x;
                    m_dragStartY = y;
                    SetFocus(m_window);
                    SetCapture(m_window);
                    return 0;
                }
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
        if (m_sourceTextOpen) {
            return 0;
        }
        const int x = logicalCoordinate(GET_X_LPARAM(lParam));
        const int y = logicalCoordinate(GET_Y_LPARAM(lParam));
        if (m_rtssDragging || m_rtssResizing) {
            moveRtssLayer(x, y);
        } else if (m_styledRowDragging) {
            moveStyledRow(x, y);
        } else if (m_scaleDragging || m_paddingDragging) {
            moveGrip(x, y);
        } else if (m_panning) {
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
            if (m_rtssDragging || m_rtssResizing) {
                moveRtssLayer(x, y);
                m_rtssDragging = m_rtssResizing = false;
                changed(true);
                ReleaseCapture();
                return 0;
            }
            if (m_styledRowDragging) {
                moveStyledRow(x, y);
                m_styledRowDragging = false;
                changed(true);
                ReleaseCapture();
                return 0;
            }
            if (m_scaleDragging || m_paddingDragging) {
                moveGrip(x, y);
                m_scaleDragging = m_paddingDragging = m_backgroundResizing = false;
                changed(true);
                ReleaseCapture();
                return 0;
            }
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
    case WM_CAPTURECHANGED:
        if (m_rtssDragging || m_rtssResizing || m_styledRowDragging || m_scaleDragging || m_paddingDragging) {
            m_rtssDragging = m_rtssResizing = m_styledRowDragging = m_scaleDragging = m_paddingDragging
                = m_backgroundResizing = false;
            changed(true);
        }
        return 0;
    case WM_KEYDOWN:
        if (m_sourceTextOpen) {
            if (wParam == VK_ESCAPE || (wParam == VK_RETURN && GetKeyState(VK_CONTROL) < 0)) {
                finishSourceText(wParam == VK_RETURN);
            }
            return 0;
        }
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
            if (m_rtssSelected >= 0 && static_cast<std::size_t>(m_rtssSelected) < rtssLayerCount()) {
                const int step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
                nudgeRtssLayers(wParam == VK_LEFT ? -step : wParam == VK_RIGHT ? step : 0,
                    wParam == VK_UP ? -step : wParam == VK_DOWN ? step : 0);
                return 0;
            }
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
        if (m_sourceTextOpen) {
            return 0;
        }
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
    const Rect modeButton { 20.0f, 37.0f, 159.0f, 62.0f };
    m_renderer.fillRoundRect(modeButton, 6.0f, theme.accentSoft);
    m_renderer.text(m_focusCanvas ? L"Настройки →" : L"Большой холст →", modeButton, TextRole::Tiny,
        theme.accent, TextAlign::Center);
    m_renderer.text(m_hdrDisplay ? L"HDR-экран: цвет SDR корректирует Windows"
            : m_focusCanvas      ? L"Слои · большой холст · выбранный элемент снизу"
                                 : L"Слои и холст · настройки справа",
        Rect { 166.0f, 37.0f, 540.0f, 61.0f }, TextRole::Small, theme.textDim);

    if (m_focusCanvas) {
        const std::array<std::wstring_view, 6> labels { L"Слева", L"Центр X", L"Справа", L"Сверху",
            L"Центр Y", L"Снизу" };
        for (int index = 0; index < 6; ++index) {
            const Rect button { 184.0f + index * 88.0f, 63.0f, 268.0f + index * 88.0f, 85.0f };
            m_renderer.fillRoundRect(button, 4.0f, theme.panelSoft);
            m_renderer.text(labels[static_cast<std::size_t>(index)], button, TextRole::Tiny, theme.accent,
                TextAlign::Center);
        }
        m_renderer.text(rtssLayerCount() > 0 ? L"1 слой — по экрану · Ctrl+клик: 2+ слоёв"
                                             : L"1 строка — по плашке · Ctrl+клик: 2+ строк",
            Rect { 720.0f, 63.0f, width - 20.0f, 85.0f }, TextRole::Tiny, theme.textDim);
    }
    drawOverlayEditorPreview(m_renderer, *m_model, theme, m_preview, GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN));
    const auto& rtss = m_model->settings.overlay;
    const std::wstring canvasHint = rtssLayerCount() > 0
        ? L"RTSS: тяга — переместить слой · угол слоя — размер · часть тегов упрощена"
        : rtss.rtssOmittedLayers > 0
        ? L"RTSS: " + std::to_wstring(rtss.rtssOmittedLayers) + L" слоёв без связанной метрики"
        : rtss.layout != OverlayLayout::Plates
        ? L"Строка: двигайте весь блок; порядок — в разделе «Строки»"
        : L"Угол — размер фона · Колесо — зум · Ctrl — выбор · Alt — плашка";
    const float hintLeft = m_focusCanvas ? m_canvas.left : 20.0f;
    m_renderer.text(clipText(m_renderer, canvasHint, TextRole::Tiny, m_fitButton.left - hintLeft - 8.0f),
        Rect { hintLeft, m_fitButton.top + 1.0f, m_fitButton.left - 4.0f, m_fitButton.bottom - 3.0f },
        TextRole::Tiny, rtss.rtssOmittedLayers > 0 ? theme.accent : theme.textDim);
    m_renderer.fillRoundRect(m_fitButton, 6.0f, theme.panelSoft);
    m_renderer.text(L"Вписать", m_fitButton, TextRole::Tiny, theme.accent, TextAlign::Center);
    m_renderer.fillRoundRect(m_eyedropperButton, 6.0f, m_samplingColor ? theme.accentSoft : theme.panelSoft);
    m_renderer.text(L"Пипетка", m_eyedropperButton, TextRole::Tiny, theme.accent, TextAlign::Center);
    m_renderer.fillRoundRect(m_resetButton, 6.0f, theme.panelSoft);
    m_renderer.text(L"Вернуть шаблон", m_resetButton, TextRole::Tiny, theme.accent, TextAlign::Center);
    const auto rows = overlayMetricRows(m_model->settings.overlay, m_model->store);
    const auto hidden = hiddenLayerRows(m_hiddenLayers, rows);
    const std::size_t rtssCount = rtssLayerCount();
    m_renderer.fillRoundRect(m_layers, 10.0f, theme.panel);
    m_renderer.text(L"Слои · глазик",
        Rect { m_layers.left + 10.0f, m_layers.top + 8.0f, m_layers.right - 8.0f, m_layers.top + 29.0f },
        TextRole::Small, theme.text);
    m_layersScroll = std::clamp(m_layersScroll, 0,
        std::max(0,
            static_cast<int>(rows.size() + hidden.size() + rtssCount)
                - std::max(1, static_cast<int>((m_layers.height() - 38.0f) / 28.0f))));
    m_renderer.pushClip(m_layers);
    for (std::size_t index = static_cast<std::size_t>(m_layersScroll);
        index < rows.size() + hidden.size() + rtssCount; ++index) {
        const float top = m_layers.top + 34.0f + static_cast<float>(index - m_layersScroll) * 28.0f;
        if (top + 26.0f > m_layers.bottom) {
            break;
        }
        const Rect row { m_layers.left + 6.0f, top, m_layers.right - 6.0f, top + 25.0f };
        if (index >= rows.size() + hidden.size()) {
            const std::size_t layerIndex = index - rows.size() - hidden.size();
            const auto& layer = m_model->settings.overlay.rtssVisual->layers[layerIndex];
            const bool shown = m_model->settings.overlay.rtssLayerEdits[layerIndex].visible;
            const std::wstring label = layer.name.empty() ? L"RTSS · слой " + std::to_wstring(layerIndex)
                                                          : L"RTSS · " + rtssLayerName(layer.name);
            m_renderer.fillRoundRect(row, 5.0f,
                std::find(m_selectedRtssLayers.begin(), m_selectedRtssLayers.end(), layerIndex)
                        != m_selectedRtssLayers.end()
                    ? theme.accentSoft
                    : theme.panelSoft);
            m_renderer.text(shown ? L"◉" : L"○", Rect { row.left + 3.0f, top, row.left + 29.0f, top + 25.0f },
                TextRole::Small, shown ? theme.accent : theme.textDim, TextAlign::Center);
            m_renderer.text(clipText(m_renderer, label, TextRole::Tiny, row.width() - 38.0f),
                Rect { row.left + 31.0f, top, row.right - 3.0f, top + 25.0f }, TextRole::Tiny,
                static_cast<int>(layerIndex) == m_rtssSelected ? theme.text : theme.textDim);
            continue;
        }
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
    if (m_focusCanvas) {
        for (int step = 1; step * 20.0f < m_canvas.width(); ++step) {
            const float x = m_canvas.left + step * 20.0f;
            m_renderer.drawLine(x, m_canvas.top, x, m_canvas.bottom,
                theme.line.withAlpha(step % 5 == 0 ? 0.5f : 0.16f), 1.0f);
        }
        for (int step = 1; step * 20.0f < m_canvas.height(); ++step) {
            const float y = m_canvas.top + step * 20.0f;
            m_renderer.drawLine(m_canvas.left, y, m_canvas.right, y,
                theme.line.withAlpha(step % 5 == 0 ? 0.5f : 0.16f), 1.0f);
        }
    } else {
        for (int step = 1; step < 4; ++step) {
            const float x = m_canvas.left + m_canvas.width() * step / 4.0f;
            const float y = m_canvas.top + m_canvas.height() * step / 4.0f;
            m_renderer.drawLine(x, m_canvas.top, x, m_canvas.bottom, theme.line.withAlpha(0.35f), 1.0f);
            m_renderer.drawLine(m_canvas.left, y, m_canvas.right, y, theme.line.withAlpha(0.35f), 1.0f);
        }
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
        static_cast<float>(m_model->settings.overlay.scalePercent) / 100.0f, formats, &m_rtssLayerRects,
        &m_rtssLayerScale, &m_canvasRowBounds, &m_canvasRowScale);
    target->SetTransform(transform);
    if (!m_selectedRtssLayers.empty()) {
        const CanvasAxis horizontal { m_canvas.left, m_panX, screenScale };
        const CanvasAxis vertical { m_canvas.top, m_panY, screenScale };
        for (std::size_t index : m_selectedRtssLayers) {
            if (index >= m_rtssLayerRects.size()) {
                continue;
            }
            const Rect& box = m_rtssLayerRects[index];
            if (box.right <= box.left || box.bottom <= box.top) {
                continue;
            }
            const Rect selected { horizontal.toCanvas(box.left), vertical.toCanvas(box.top),
                horizontal.toCanvas(box.right), vertical.toCanvas(box.bottom) };
            m_renderer.strokeRoundRect(selected, 3.0f, theme.accent, 1.5f);
            if (static_cast<int>(index) == m_rtssSelected) {
                m_renderer.fillRoundRect({ selected.right - 9.0f, selected.bottom - 9.0f,
                                             selected.right + 3.0f, selected.bottom + 3.0f },
                    2.0f, theme.accent);
            }
        }
    }
    if (m_rtssSelected < 0
        && (!m_model->settings.overlay.pixelLayout
            || m_model->settings.overlay.layout != OverlayLayout::Plates)) {
        const CanvasAxis horizontal { m_canvas.left, m_panX, screenScale };
        const CanvasAxis vertical { m_canvas.top, m_panY, screenScale };
        for (const auto& row : m_canvasRowBounds) {
            if (row.id == m_selected) {
                m_renderer.strokeRoundRect(
                    { horizontal.toCanvas(row.rect.left), vertical.toCanvas(row.rect.top),
                        horizontal.toCanvas(row.rect.right), vertical.toCanvas(row.rect.bottom) },
                    3.0f, theme.accent, 1.5f);
                break;
            }
        }
    }
    if (m_rtssSelected < 0 && m_model->settings.overlay.pixelLayout
        && m_model->settings.overlay.layout == OverlayLayout::Plates) {
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
    if (rtssLayerCount() == 0 && m_screenPlate.width() > 0.0f && m_screenPlate.height() > 0.0f) {
        m_renderer.fillRoundRect(scaleGrip(), 4.0f, theme.accent);
        m_renderer.strokeRoundRect(scaleGrip(), 4.0f, theme.background, 1.0f);
        const Rect padding = paddingGrip();
        if (padding.right > padding.left) {
            m_renderer.fillRoundRect(padding, 3.0f, theme.accentSoft);
            m_renderer.strokeRoundRect(padding, 3.0f, theme.accent, 1.5f);
        }
    }
    m_renderer.popClip();
    m_renderer.strokeRoundRect(m_canvas, 10.0f, theme.line);

    m_renderer.fillRoundRect(m_inspector, 10.0f, theme.panel);
    if (m_rtssSelected >= 0 && static_cast<std::size_t>(m_rtssSelected) < rtssLayerCount()) {
        const std::size_t index = static_cast<std::size_t>(m_rtssSelected);
        const auto& layer = m_model->settings.overlay.rtssVisual->layers[index];
        const auto& edit = m_model->settings.overlay.rtssLayerEdits[index];
        const std::wstring name
            = layer.name.empty() ? L"Слой " + std::to_wstring(index) : rtssLayerName(layer.name);
        m_renderer.text(clipText(m_renderer, L"RTSS · " + name, TextRole::Heading, 280.0f),
            Rect { 32.0f, m_inspector.top + 7.0f, 316.0f, m_inspector.top + 34.0f }, TextRole::Heading,
            theme.text);
        m_renderer.text(L"Сбросить правки",
            Rect { 318.0f, m_inspector.top + 9.0f, 425.0f, m_inspector.top + 32.0f }, TextRole::Tiny,
            theme.accent);
        m_renderer.text(edit.visible ? L"Скрыть" : L"Показать",
            Rect { 434.0f, m_inspector.top + 9.0f, 508.0f, m_inspector.top + 32.0f }, TextRole::Tiny,
            theme.warn);
        m_renderer.text(L"Текст ↩", Rect { 32.0f, m_inspector.top + 43.0f, 110.0f, m_inspector.top + 68.0f },
            TextRole::Small, theme.accent);
        const Rect expand { 460.0f, m_inspector.top + 40.0f, 504.0f, m_inspector.top + 66.0f };
        m_renderer.fillRoundRect(expand, 4.0f, theme.accentSoft);
        m_renderer.text(L"↗", expand, TextRole::Small, theme.accent, TextAlign::Center);
        m_renderer.text(L"Цвет", Rect { 32.0f, m_inspector.top + 76.0f, 110.0f, m_inspector.top + 98.0f },
            TextRole::Small, theme.text);
        m_renderer.text(L"Фон", Rect { 229.0f, m_inspector.top + 76.0f, 260.0f, m_inspector.top + 98.0f },
            TextRole::Tiny, theme.text);
        m_renderer.text(L"Шр.%", Rect { 394.0f, m_inspector.top + 76.0f, 450.0f, m_inspector.top + 98.0f },
            TextRole::Tiny, theme.text);
        const std::array<std::wstring_view, 4> geometry { L"X", L"Y", L"Ш", L"В" };
        for (int field = 0; field < 4; ++field) {
            const float left = 32.0f + field * 116.0f;
            m_renderer.text(geometry[static_cast<std::size_t>(field)],
                Rect { left, m_inspector.top + 105.0f, left + 25.0f, m_inspector.top + 131.0f },
                TextRole::Tiny, theme.textDim);
        }
        const std::array<std::wstring_view, 4> directions { L"←", L"→", L"↑", L"↓" };
        for (int direction = 0; direction < 4; ++direction) {
            const float left = 32.0f + static_cast<float>(direction) * 98.0f;
            const Rect button { left, m_inspector.top + 132.0f, left + 92.0f, m_inspector.top + 166.0f };
            m_renderer.fillRoundRect(button, 6.0f, theme.panelSoft);
            m_renderer.text(directions[static_cast<std::size_t>(direction)], button, TextRole::Small,
                theme.accent, TextAlign::Center);
        }
        const std::array<std::uint32_t, 2> swatches { edit.textColor.value_or(layer.textColor),
            edit.backgroundColor.value_or(layer.backgroundColor) };
        for (int field = 0; field < 2; ++field) {
            const float left = m_inspector.left + (field == 0 ? 410.0f : 462.0f);
            const Rect button { left, m_inspector.top + 132.0f, left + 42.0f, m_inspector.top + 166.0f };
            const std::uint32_t argb = swatches[static_cast<std::size_t>(field)];
            m_renderer.fillRoundRect(button, 5.0f, theme.panelSoft);
            m_renderer.fillRoundRect(
                Rect { left + 4.0f, button.top + 3.0f, left + 38.0f, button.top + 19.0f }, 3.0f,
                fromRgba((argb << 8) | (argb >> 24)));
            m_renderer.text(field == 0 ? L"Т" : L"Ф",
                Rect { left, button.top + 19.0f, left + 42.0f, button.bottom }, TextRole::Tiny, theme.text,
                TextAlign::Center);
        }
    } else {
        const std::wstring title
            = rows.empty() ? L"Добавьте показатель через «+ Датчик»" : widen(metricInfo(m_selected).label);
        m_renderer.text(clipText(m_renderer, title, TextRole::Heading, 280.0f),
            Rect { 32.0f, m_inspector.top + 7.0f, 320.0f, m_inspector.top + 34.0f }, TextRole::Heading,
            theme.text);
        m_renderer.text(L"Сбросить имя",
            Rect { 320.0f, m_inspector.top + 9.0f, 425.0f, m_inspector.top + 32.0f }, TextRole::Tiny,
            theme.accent);
        m_renderer.text(L"Скрыть", Rect { 434.0f, m_inspector.top + 9.0f, 508.0f, m_inspector.top + 32.0f },
            TextRole::Tiny, theme.warn);
        m_renderer.text(L"Подпись", Rect { 32.0f, m_inspector.top + 46.0f, 103.0f, m_inspector.top + 67.0f },
            TextRole::Small, theme.textDim);
        m_renderer.text(L"RGBA", Rect { 32.0f, m_inspector.top + 72.0f, 103.0f, m_inspector.top + 92.0f },
            TextRole::Tiny, theme.textDim);
        m_renderer.text(L"X подписи",
            Rect { 32.0f, m_inspector.top + 96.0f, 106.0f, m_inspector.top + 119.0f }, TextRole::Tiny,
            theme.textDim);
        m_renderer.text(L"X значения",
            Rect { 182.0f, m_inspector.top + 96.0f, 276.0f, m_inspector.top + 119.0f }, TextRole::Tiny,
            theme.textDim);
        m_renderer.text(L"Y", Rect { 380.0f, m_inspector.top + 96.0f, 403.0f, m_inspector.top + 119.0f },
            TextRole::Tiny, theme.textDim);
        const int size
            = m_model->settings.overlay.metricTextScalePercent[static_cast<std::size_t>(m_selected)];
        m_renderer.text(L"Текст: " + std::to_wstring(size) + L" %",
            Rect { 32.0f, m_inspector.top + 119.0f, 205.0f, m_inspector.top + 137.0f }, TextRole::Tiny,
            theme.textDim);
        for (const auto& [left, label] : std::array<std::pair<float, std::wstring_view>, 3> {
                 { { 230.0f, L"−" }, { 278.0f, L"+" }, { 326.0f, L"100 %" } } }) {
            const Rect button { left, m_inspector.top + 118.0f, left + (left > 320.0f ? 72.0f : 42.0f),
                m_inspector.top + 136.0f };
            m_renderer.fillRoundRect(button, 4.0f, theme.panelSoft);
            m_renderer.text(label, button, TextRole::Tiny, theme.accent, TextAlign::Center);
        }
        const auto palette = overlayColorPalette();
        const int selectedColor
            = m_model->settings.overlay.metricColors[static_cast<std::size_t>(m_selected)];
        for (int n = -1; n < kOverlayColorCount; ++n) {
            const float x = 40.0f + static_cast<float>(n + 1) * 56.0f;
            const Rect chip { x, m_inspector.top + 137.0f, x + 30.0f, m_inspector.top + 167.0f };
            m_renderer.fillRoundRect(
                chip, 8.0f, n < 0 ? theme.panelSoft : palette[static_cast<std::size_t>(n)]);
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
    }

    if (width >= 1200.0f && !m_focusCanvas) {
        m_renderer.text(L"Оформление · шрифт · подписи · датчики",
            Rect { m_controls.left, 12.0f, width - 400.0f, 44.0f }, TextRole::Heading, theme.text);
    }
    m_renderer.fillRoundRect(Rect { width - 368.0f, 12.0f, width - 248.0f, 45.0f }, 8.0f, theme.panelSoft);
    m_renderer.text(L"Импорт файла", Rect { width - 366.0f, 12.0f, width - 250.0f, 45.0f }, TextRole::Tiny,
        theme.accent, TextAlign::Center);
    m_renderer.fillRoundRect(Rect { width - 242.0f, 12.0f, width - 130.0f, 45.0f }, 8.0f, theme.panelSoft);
    m_renderer.text(L"Экспорт файла", Rect { width - 240.0f, 12.0f, width - 132.0f, 45.0f }, TextRole::Tiny,
        theme.accent, TextAlign::Center);
    m_renderer.fillRoundRect(Rect { width - 110.0f, 12.0f, width - 18.0f, 45.0f }, 8.0f, theme.accentSoft);
    m_renderer.text(m_catalogOpen ? L"Закрыть" : L"+ Датчик",
        Rect { width - 108.0f, 12.0f, width - 18.0f, 45.0f }, TextRole::Small, theme.accent,
        TextAlign::Center);
    m_hits.clear();
    if (!m_focusCanvas) {
        m_renderer.pushClip(m_controls);
        m_renderer.pushTranslate(0.0f, -static_cast<float>(m_panelScroll));
        const Rect controlsArea { m_controls.left, m_controls.top + 4.0f, m_controls.right - 6.0f,
            m_controls.top + overlayEditorSectionHeight(m_model->settings.overlay, m_section) };
        drawOverlayEditorSection(m_renderer, *m_model, theme, controlsArea, m_section, m_hits);
        m_renderer.popTranslate();
        m_renderer.popClip();
        for (UiHit& hit : m_hits) {
            hit.rect.top -= static_cast<float>(m_panelScroll);
            hit.rect.bottom -= static_cast<float>(m_panelScroll);
        }
        for (std::size_t index = 0; index < kSections.size(); ++index) {
            const float tabWidth = m_controls.width() / static_cast<float>(kSections.size());
            const Rect tab { m_controls.left + tabWidth * static_cast<float>(index), 68.0f,
                m_controls.left + tabWidth * static_cast<float>(index + 1) - 6.0f, 101.0f };
            m_renderer.fillRoundRect(
                tab, 8.0f, static_cast<int>(index) == m_section ? theme.accentSoft : theme.panelSoft);
            m_renderer.text(kSections[index], tab, TextRole::Small,
                static_cast<int>(index) == m_section ? theme.accent : theme.text, TextAlign::Center);
        }
        if (overlayEditorSectionHeight(m_model->settings.overlay, m_section) > m_controls.height()) {
            const float fraction = static_cast<float>(m_panelScroll)
                / overlayEditorSectionHeight(m_model->settings.overlay, m_section);
            m_renderer.fillRoundRect(
                Rect { m_controls.right - 4.0f, m_controls.top + fraction * m_controls.height(),
                    m_controls.right, m_controls.top + fraction * m_controls.height() + 55.0f },
                2.0f, theme.accent);
        }
    }
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
    if (m_sourceTextOpen) {
        const Rect panel = sourceTextPanel();
        m_renderer.fillRoundRect(panel, 10.0f, theme.panel);
        m_renderer.strokeRoundRect(panel, 10.0f, theme.accent, 1.0f);
        m_renderer.text(L"Строка RTSS · многострочный редактор",
            { panel.left + 16.0f, panel.top + 10.0f, panel.right - 16.0f, panel.top + 34.0f },
            TextRole::Heading, theme.text);
        m_renderer.text(L"8192 байт UTF-8 · Enter — новая строка · Ctrl+Enter — применить · Esc — отмена",
            { panel.left + 16.0f, panel.top + 32.0f, panel.right - 16.0f, panel.top + 49.0f }, TextRole::Tiny,
            theme.textDim);
        const Rect apply { panel.right - 258.0f, panel.bottom - 48.0f, panel.right - 142.0f,
            panel.bottom - 14.0f };
        const Rect cancel { panel.right - 132.0f, panel.bottom - 48.0f, panel.right - 16.0f,
            panel.bottom - 14.0f };
        m_renderer.fillRoundRect(apply, 5.0f, theme.accentSoft);
        m_renderer.fillRoundRect(cancel, 5.0f, theme.panelSoft);
        m_renderer.text(L"Применить", apply, TextRole::Small, theme.accent, TextAlign::Center);
        m_renderer.text(L"Отмена", cancel, TextRole::Small, theme.text, TextAlign::Center);
    }
    m_renderer.text(L"Ctrl+клик: несколько строк · Shift+тяга: сетка 8 px · Shift+стрелки: 10 px",
        Rect { 20.0f, height - 19.0f, width - 20.0f, height }, TextRole::Tiny, theme.textFaint);
    m_renderer.end();
    if (m_fitPending) {
        m_fitPending = false;
        fitCanvasToContent();
        refresh();
    }
}

} // namespace yawa
