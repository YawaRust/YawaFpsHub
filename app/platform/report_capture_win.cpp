#include "platform/report_capture_win.h"
#include "platform/win_util.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstring>
#include <utility>

namespace yawa {
namespace {

    using Microsoft::WRL::ComPtr;

    struct FrameRelease
    {
        IDXGIOutputDuplication* duplication;
        ~FrameRelease() { duplication->ReleaseFrame(); }
    };

    bool findOutput(HMONITOR monitor, IDXGIAdapter1** adapter, IDXGIOutput** output,
        DXGI_OUTPUT_DESC& description, std::string& error)
    {
        ComPtr<IDXGIFactory1> factory;
        const HRESULT created = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        if (FAILED(created)) {
            error = "Захват: DXGI недоступен: " + hresultText(created);
            return false;
        }
        for (UINT a = 0;; ++a) {
            ComPtr<IDXGIAdapter1> candidate;
            const HRESULT adapterResult = factory->EnumAdapters1(a, &candidate);
            if (adapterResult == DXGI_ERROR_NOT_FOUND)
                break;
            if (FAILED(adapterResult) || !candidate) {
                error = "Захват: не удалось перечислить видеоадаптеры";
                return false;
            }
            for (UINT o = 0;; ++o) {
                ComPtr<IDXGIOutput> screen;
                const HRESULT outputResult = candidate->EnumOutputs(o, &screen);
                if (outputResult == DXGI_ERROR_NOT_FOUND)
                    break;
                if (FAILED(outputResult) || !screen) {
                    error = "Захват: не удалось перечислить мониторы";
                    return false;
                }
                DXGI_OUTPUT_DESC found { };
                if (SUCCEEDED(screen->GetDesc(&found)) && found.Monitor == monitor) {
                    description = found;
                    *adapter = candidate.Detach();
                    *output = screen.Detach();
                    return true;
                }
            }
        }
        error = "Захват: монитор активной игры не найден в DXGI";
        return false;
    }

    bool isHdrOutput(IDXGIOutput* output)
    {
        ComPtr<IDXGIOutput6> output6;
        DXGI_OUTPUT_DESC1 description { };
        if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output6))) || FAILED(output6->GetDesc1(&description)))
            return false;
        return description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
            || description.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    }

    bool gameClient(HWND game, const DXGI_OUTPUT_DESC& screen, RECT& crop, std::string& error)
    {
        RECT client { };
        POINT origin { };
        if (GetClientRect(game, &client) == FALSE || ClientToScreen(game, &origin) == FALSE
            || client.right <= 0 || client.bottom <= 0) {
            error = "Захват: клиентская область игры недоступна";
            return false;
        }
        crop = RECT { origin.x - screen.DesktopCoordinates.left, origin.y - screen.DesktopCoordinates.top,
            origin.x + client.right - screen.DesktopCoordinates.left,
            origin.y + client.bottom - screen.DesktopCoordinates.top };
        const LONG screenWidth = screen.DesktopCoordinates.right - screen.DesktopCoordinates.left;
        const LONG screenHeight = screen.DesktopCoordinates.bottom - screen.DesktopCoordinates.top;
        if (crop.left < 0 || crop.top < 0 || crop.right > screenWidth || crop.bottom > screenHeight
            || crop.right <= crop.left || crop.bottom <= crop.top
            || static_cast<std::uint64_t>(client.right) * client.bottom > 16000000u) {
            error = "Захват: окно игры выходит за монитор или слишком велико";
            return false;
        }
        return true;
    }

} // namespace

bool captureForegroundGameClient(std::uint32_t gamePid, GameClientFrame& frame, std::string& error)
{
    error.clear();
    const HWND game = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (game == nullptr || gamePid == 0 || IsIconic(game) != FALSE) {
        error = "Захват: активная игра не выбрана";
        return false;
    }
    GetWindowThreadProcessId(game, &foregroundPid);
    if (foregroundPid != gamePid) {
        error = "Захват: активное окно не принадлежит выбранной игре";
        return false;
    }
    const HMONITOR monitor = MonitorFromWindow(game, MONITOR_DEFAULTTONULL);
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    DXGI_OUTPUT_DESC screen { };
    if (monitor == nullptr) {
        error = "Захват: монитор активной игры не определён";
        return false;
    }
    if (!findOutput(monitor, adapter.GetAddressOf(), output.GetAddressOf(), screen, error))
        return false;
    if (isHdrOutput(output.Get())) {
        error = "Захват: HDR-кадр требует тонмаппинга; сохраните карточку без кадра";
        return false;
    }
    RECT crop { };
    if (!gameClient(game, screen, crop, error))
        return false;

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const HRESULT created = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(created)) {
        error = "Захват: устройство игры/монитора недоступно: " + hresultText(created);
        return false;
    }
    ComPtr<IDXGIOutput1> output1;
    ComPtr<IDXGIOutputDuplication> duplication;
    const HRESULT duplicated = output.As(&output1);
    if (FAILED(duplicated) || FAILED(output1->DuplicateOutput(device.Get(), &duplication))) {
        error = "Захват: Desktop Duplication не разрешён для этого монитора";
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info { };
    ComPtr<IDXGIResource> resource;
    const HRESULT acquired = duplication->AcquireNextFrame(200, &info, &resource);
    if (FAILED(acquired)) {
        error = acquired == DXGI_ERROR_WAIT_TIMEOUT ? "Захват: монитор пока не выдал новый кадр"
                                                    : "Захват: кадр не получен: " + hresultText(acquired);
        return false;
    }
    FrameRelease release { duplication.Get() };
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(resource.As(&texture))) {
        error = "Захват: текстура кадра недоступна";
        return false;
    }
    D3D11_TEXTURE2D_DESC description { };
    texture->GetDesc(&description);
    if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM
        || static_cast<std::uint64_t>(description.Width) * description.Height > 16000000u
        || crop.right > static_cast<LONG>(description.Width)
        || crop.bottom > static_cast<LONG>(description.Height)) {
        error = "Захват: формат кадра не поддерживается";
        return false;
    }
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&description, nullptr, &staging))) {
        error = "Захват: не удалось создать буфер кадра";
        return false;
    }
    context->CopyResource(staging.Get(), texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped { };
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "Захват: не удалось прочитать кадр";
        return false;
    }
    if (mapped.RowPitch < description.Width * 4u) {
        context->Unmap(staging.Get(), 0);
        error = "Захват: неверный шаг строк кадра";
        return false;
    }
    GameClientFrame captured;
    captured.width = static_cast<unsigned>(crop.right - crop.left);
    captured.height = static_cast<unsigned>(crop.bottom - crop.top);
    captured.bgra.resize(static_cast<std::size_t>(captured.width) * captured.height * 4u);
    bool hasPixels = false;
    for (unsigned y = 0; y < captured.height; ++y) {
        const auto* source = static_cast<const std::uint8_t*>(mapped.pData)
            + static_cast<std::size_t>(crop.top + y) * mapped.RowPitch + crop.left * 4;
        auto* target = captured.bgra.data() + static_cast<std::size_t>(y) * captured.width * 4u;
        std::memcpy(target, source, static_cast<std::size_t>(captured.width) * 4u);
        for (unsigned x = 0; x < captured.width; ++x) {
            hasPixels |= (target[x * 4] | target[x * 4 + 1] | target[x * 4 + 2]) != 0;
            target[x * 4 + 3] = 255;
        }
    }
    context->Unmap(staging.Get(), 0);
    if (GetForegroundWindow() != game || IsIconic(game) != FALSE || !hasPixels) {
        error = hasPixels ? "Захват: активная игра сменилась во время снимка"
                          : "Захват: окно вернуло пустой или защищённый кадр";
        return false;
    }
    frame = std::move(captured);
    return true;
}

} // namespace yawa
