#pragma once

#include "render/overlay_painter.h"
#include "ui/app_model.h"

#include <windows.h>

#include <functional>
#include <utility>
#include <vector>

namespace yawa {

// Живой редактор: предпросмотр, холст строк и настройки используют ту же модель,
// что и настоящий оверлей. Собственный Renderer не зависит от главного окна.
class OverlayEditor
{
public:
    ~OverlayEditor();
    bool open(HWND owner, HINSTANCE instance, AppModel& model, const Theme& theme,
        std::function<void(const UiHit&, float, bool)> onAction, std::function<void(wchar_t)> onText,
        std::function<void(bool)> onChange);
    void close();
    void refresh();
    bool isDragging() const { return m_dragging || m_styledRowDragging; }
    bool isOpen() const { return m_window != nullptr; }

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handle(UINT message, WPARAM wParam, LPARAM lParam);
    void paint();
    void layout();
    void syncFields();
    void select(MetricId id, bool value);
    void move(int x, int y);
    float canvasScale() const;
    void fitCanvasToContent();
    void alignSelectedRows(int edge);
    void alignSelectedRtssLayers(int edge);
    std::size_t rtssLayerCount() const;
    void selectRtssLayer(int index, bool additive);
    void nudgeRtssLayers(int dx, int dy);
    void moveRtssLayer(int x, int y);
    void moveStyledRow(int x, int y);
    int logicalCoordinate(int pixels) const;
    Rect plateOnCanvas() const;
    Rect scaleGrip() const;
    Rect paddingGrip() const;
    void moveGrip(int x, int y);
    void preparePixelLayout();
    OverlayRowPosition canvasPosition(MetricId id) const;
    void editCoordinate(HWND field);
    void changed(bool finished);
    void recordHistory();
    void restoreHistory(bool redo);
    const UiHit* hitAt(float x, float y) const;
    std::vector<MetricId> catalogRows() const;

    HWND m_window = nullptr;
    HWND m_labelEdit = nullptr;
    HWND m_labelX = nullptr;
    HWND m_valueX = nullptr;
    HWND m_y = nullptr;
    HWND m_alphaEdit = nullptr;
    HWND m_rtssHeightEdit = nullptr;
    HWND m_hexEdit = nullptr;
    HWND m_rtssBgEdit = nullptr;
    HWND m_numericEdit = nullptr;
    ActionKind m_numericAction = ActionKind::None;
    HWND m_catalogSearch = nullptr;
    HBRUSH m_editBrush = nullptr;
    Renderer m_renderer;
    AppModel* m_model = nullptr;
    const Theme* m_theme = nullptr;
    std::function<void(const UiHit&, float, bool)> m_onAction;
    std::function<void(wchar_t)> m_onText;
    std::function<void(bool)> m_onChange;
    std::vector<UiHit> m_hits;
    Rect m_canvas;
    Rect m_preview;
    Rect m_fitButton;
    Rect m_eyedropperButton;
    Rect m_resetButton;
    Rect m_layers;
    Rect m_inspector;
    Rect m_controls;
    Rect m_catalog;
    MetricId m_selected = MetricId::Fps;
    UiHit m_activeHit;
    bool m_value = false;
    bool m_dragging = false;
    bool m_syncing = false;
    float m_dpiScale = 1.0f;
    bool m_hdrDisplay = false;
    float m_canvasZoom = 1.0f;
    float m_panX = 0.0f;
    float m_panY = 0.0f;
    bool m_fitPending = true;
    std::shared_ptr<const std::string> m_canvasTemplate;
    bool m_focusCanvas = true;
    float m_rowDragScale = 1.0f;
    bool m_panning = false;
    bool m_samplingColor = false;
    int m_panStartX = 0;
    int m_panStartY = 0;
    float m_panStartLeft = 0.0f;
    float m_panStartTop = 0.0f;
    OverlaySettings m_historyCommitted;
    std::vector<OverlaySettings> m_undo;
    std::vector<OverlaySettings> m_redo;
    int m_panelScroll = 0;
    int m_section = 0;
    int m_layersScroll = 0;
    std::vector<MetricId> m_hiddenLayers;
    std::vector<Rect> m_rtssLayerRects;
    float m_rtssLayerScale = 1.0f;
    std::vector<OverlayRowBounds> m_canvasRowBounds;
    float m_canvasRowScale = 1.0f;
    bool m_styledRowDragging = false;
    MetricId m_styledRowId = MetricId::Fps;
    OverlayRowOffset m_styledRowStart { };
    Rect m_styledRowBounds { };
    float m_styledRowUnit = 1.0f;
    int m_rtssSelected = -1;
    std::vector<std::size_t> m_selectedRtssLayers;
    std::vector<std::pair<std::size_t, OverlayRowOffset>> m_rtssDragOffsets;
    bool m_rtssDragging = false;
    bool m_rtssResizing = false;
    int m_rtssWidthStart = 0;
    int m_rtssHeightStart = 0;
    float m_rtssDragUnit = 1.0f;
    bool m_scaleDragging = false;
    bool m_backgroundResizing = false;
    bool m_paddingDragging = false;
    int m_backgroundStartWidth = 0;
    int m_backgroundStartHeight = 0;
    int m_gripStart = 0;
    float m_gripWidth = 1.0f;
    float m_gripHeight = 1.0f;
    float m_gripUnit = 1.0f;
    int m_rtssStartX = 0;
    int m_rtssStartY = 0;
    int m_catalogScroll = 0;
    bool m_catalogOpen = false;
    int m_dragStartX = 0;
    int m_dragStartY = 0;
    int m_offsetStartX = 0;
    int m_offsetStartY = 0;
    bool m_dragLine = false;
    bool m_dragPlate = false;
    Rect m_screenPlate { };
    float m_guideX = -1.0f;
    float m_guideY = -1.0f;
    int m_originStartX = 0;
    int m_originStartY = 0;
    std::vector<MetricId> m_selectedRows;
    std::vector<OverlayRowPosition> m_dragPositions;
};

} // namespace yawa
