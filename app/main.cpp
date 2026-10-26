#include "platform/app_paths.h"
#include "platform/log.h"
#include "platform/main_window.h"
#include "platform/pawnio_package.h"
#include "platform/win_util.h"

#include <windows.h>
#include <objbase.h>

#include <string>
#include <initializer_list>
#include <string_view>
#include <utility>

#include <cstdlib>
#include <cwctype>

namespace {

constexpr wchar_t kInstanceMutex[] = L"Global\\YawaFpsHubInstance";

bool isAlreadyRunning()
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (mutex == nullptr) {
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return true;
    }
    return false;
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t* commandLine, int)
{
    const int sensorCommand = yawa::runSensorCommandLine();
    if (sensorCommand >= 0) {
        return sensorCommand;
    }
    const std::wstring_view arguments = commandLine != nullptr ? commandLine : L"";
    const std::size_t smokeFlag = arguments.find(L"--smoke");
    const bool smoke = smokeFlag != std::wstring_view::npos;

    // --smoke=<секунды>: CI держит окно дольше, чтобы замерить потребление в установившемся режиме.
    int smokeSeconds = 8;
    if (smoke) {
        const std::size_t equals = arguments.find(L'=', smokeFlag);
        if (equals != std::wstring_view::npos) {
            int parsed = 0;
            for (std::size_t index = equals + 1; index < arguments.size() && iswdigit(arguments[index]) != 0; ++index) {
                parsed = parsed * 10 + (arguments[index] - L'0');
            }
            if (parsed > 0) {
                smokeSeconds = parsed;
            }
        }
    }

    // --style=<пресет>: CI снимает один и тот же экран оверлея во всех стилях,
    // чтобы было видно, что меняется вся композиция.
    yawa::OverlayStyle initialStyle = yawa::OverlayStyle::Frosted;
    bool hasStyle = false;
    for (const auto& [argument, style] : std::initializer_list<std::pair<std::wstring_view, yawa::OverlayStyle>>{
             { L"--style=neon", yawa::OverlayStyle::Neon }, { L"--style=rtss", yawa::OverlayStyle::Rtss },
             { L"--style=frosted", yawa::OverlayStyle::Frosted },
             { L"--style=terminal", yawa::OverlayStyle::Terminal },
             { L"--style=cyber", yawa::OverlayStyle::Cyber }, { L"--style=mono", yawa::OverlayStyle::Mono },
             { L"--style=esport", yawa::OverlayStyle::Esport },
             { L"--style=retro", yawa::OverlayStyle::Retro } }) {
        if (arguments.find(argument) != std::wstring_view::npos) {
            initialStyle = style;
            hasStyle = true;
        }
    }

    // --view=<экран> нужен CI, чтобы снять скриншоты всех экранов одним и тем же способом.
    yawa::View initialView = yawa::View::Dashboard;
    if (arguments.find(L"--view=sensors") != std::wstring_view::npos) {
        initialView = yawa::View::Sensors;
    } else if (arguments.find(L"--view=bottleneck") != std::wstring_view::npos) {
        initialView = yawa::View::Bottleneck;
    } else if (arguments.find(L"--view=system") != std::wstring_view::npos) {
        initialView = yawa::View::System;
    } else if (arguments.find(L"--view=overlay") != std::wstring_view::npos) {
        initialView = yawa::View::Overlay;
    } else if (arguments.find(L"--view=settings") != std::wstring_view::npos) {
        initialView = yawa::View::Settings;
    }
    // --size=1280x800: фиксированный размер окна для скриншотов. Прототип снимается
    // тем же размером, поэтому картинки можно сравнивать попиксельно.
    int initialWidth = 0;
    int initialHeight = 0;
    // Ищем последнее вхождение: CI передаёт базовый размер и может переопределить его
    // снимком маленького окна.
    const std::size_t sizeFlag = arguments.rfind(L"--size=");
    if (sizeFlag != std::wstring_view::npos) {
        const std::wstring_view value = arguments.substr(sizeFlag + 7);
        const std::size_t cross = value.find(L'x');
        if (cross != std::wstring_view::npos) {
            initialWidth = static_cast<int>(std::wcstol(std::wstring(value.substr(0, cross)).c_str(), nullptr, 10));
            initialHeight = static_cast<int>(std::wcstol(std::wstring(value.substr(cross + 1)).c_str(), nullptr, 10));
        }
    }

    // --scroll=640: стартовый сдвиг содержимого. Нужен CI, чтобы снять прокрученный экран
    // и проверить, что содержимое действительно едет, а не просто обрезается.
    float initialScroll = 0.0f;
    const std::size_t scrollFlag = arguments.rfind(L"--scroll=");
    if (scrollFlag != std::wstring_view::npos) {
        initialScroll = static_cast<float>(std::wcstol(
            std::wstring(arguments.substr(scrollFlag + 9)).c_str(), nullptr, 10));
    }

    // Монитор FPS не должен влиять на картинку в игре: сначала сообщаем Windows, что
    // приложение само управляет масштабом, и только потом создаём окна.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // COM нужен WIC: оверлей сохраняет свои PNG для скриншотов в CI.
    const HRESULT comResult = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    if (isAlreadyRunning()) {
        MessageBoxW(nullptr, L"YawaFpsHub уже запущен — окно свёрнуто в трей.", L"YawaFpsHub",
            MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    yawa::Log log;
    log.open(yawa::logPath());
    log.info("запуск, каталог " + yawa::executableDirectory());

    yawa::MainWindow window;
    std::string error;
    if (!window.create(instance, log, error)) {
        log.error("старт: " + error);
        MessageBoxW(nullptr, yawa::widen(error).c_str(), L"YawaFpsHub не запустился",
            MB_OK | MB_ICONERROR);
        return 1;
    }

    // Самопроверка держит окно открытым, чтобы CI успел снять скриншот интерфейса.
    window.setInitialView(initialView);
    if (initialWidth > 0 && initialHeight > 0) {
        window.setInitialSize(initialWidth, initialHeight);
    }
    if (initialScroll > 0.0f) {
        window.setInitialScroll(initialScroll);
    }
    if (hasStyle) {
        window.setInitialStyle(initialStyle);
    }
    if (arguments.find(L"--overlay") != std::wstring_view::npos) {
        window.setOverlayVisible(true);
    }
    std::wstring overlayShotDir;
    const std::size_t shotFlag = arguments.find(L"--overlay-shots=");
    if (shotFlag != std::wstring_view::npos) {
        overlayShotDir = std::wstring(arguments.substr(shotFlag + 16));
    }

    const int result = window.run(smoke ? smokeSeconds : 0, overlayShotDir);
    log.info("остановка");
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return result;
}
