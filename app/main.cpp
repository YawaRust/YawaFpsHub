#include "platform/app_paths.h"
#include "platform/log.h"
#include "platform/main_window.h"
#include "platform/win_util.h"

#include <windows.h>

#include <string>
#include <string_view>

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

    // --view=sensors|settings нужен CI, чтобы снять скриншоты всех экранов.
    yawa::View initialView = yawa::View::Dashboard;
    if (arguments.find(L"--view=sensors") != std::wstring_view::npos) {
        initialView = yawa::View::Sensors;
    } else if (arguments.find(L"--view=settings") != std::wstring_view::npos) {
        initialView = yawa::View::Settings;
    }
    // Монитор FPS не должен влиять на картинку в игре: сначала сообщаем Windows, что
    // приложение само управляет масштабом, и только потом создаём окна.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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
    if (arguments.find(L"--overlay") != std::wstring_view::npos) {
        window.setOverlayVisible(true);
    }
    const int result = window.run(smoke ? smokeSeconds : 0);
    log.info("остановка");
    return result;
}
