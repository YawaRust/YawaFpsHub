#include "platform/app_paths.h"
#include "platform/log.h"
#include "platform/main_window.h"
#include "platform/pawnio_package.h"
#include "platform/pawnio_sensors.h"
#include "platform/report_file_win.h"
#include "platform/spd_probe_win.h"
#include "platform/win_util.h"
#include "report/html_report.h"
#include "report/package.h"

#include <objbase.h>
#include <shellapi.h>
#include <windows.h>

#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <cstdlib>
#include <cwctype>

namespace {

constexpr wchar_t kInstanceMutex[] = L"Global\\YawaFpsHubInstance";

enum class DumpCommand { None, Memory, Spd, Invalid };

struct DumpRequest
{
    DumpCommand command = DumpCommand::None;
    unsigned address = 0;
};

DumpRequest probeCommand()
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr)
        return { };
    bool mentionsProbe = false;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view arg = arguments[index];
        mentionsProbe |= arg == L"--probe" || arg == L"--dump=memory" || arg.starts_with(L"--spd");
    }
    DumpRequest request;
    if ((count == 2 && std::wstring_view(arguments[1]) == L"--dump=memory")
        || (count == 3 && std::wstring_view(arguments[1]) == L"--probe"
            && std::wstring_view(arguments[2]) == L"--dump=memory")) {
        request.command = DumpCommand::Memory;
    } else if (count == 3 && std::wstring_view(arguments[1]) == L"--probe") {
        const std::wstring_view arg = arguments[2];
        if (arg.size() == 8 && arg.starts_with(L"--spd=5") && arg[7] >= L'0' && arg[7] <= L'7') {
            request.command = DumpCommand::Spd;
            request.address = 0x50u + static_cast<unsigned>(arg[7] - L'0');
        }
    }
    LocalFree(arguments);
    if (request.command == DumpCommand::None && mentionsProbe)
        request.command = DumpCommand::Invalid;
    return request;
}

struct ReportRequest
{
    std::wstring path;
    std::wstring htmlPath;
    bool json = false;
};

// Неизвестные сочетания --report/--json не должны попадать в обычный запуск с UAC.
std::optional<ReportRequest> reportCommand()
{
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr)
        return std::nullopt;
    ReportRequest request;
    bool hasReport = false;
    bool hasJson = false;
    bool hasHtml = false;
    bool invalid = false;
    for (int index = 1; index < count; ++index) {
        const std::wstring_view arg = arguments[index];
        if (arg.starts_with(L"--report")) {
            invalid |= hasReport || !arg.starts_with(L"--report=");
            hasReport = true;
            if (arg.starts_with(L"--report="))
                request.path = std::wstring(arg.substr(9));
        } else if (arg.starts_with(L"--json")) {
            invalid |= hasJson || arg != L"--json";
            hasJson = true;
        } else if (arg.starts_with(L"--html")) {
            invalid |= hasHtml || !arg.starts_with(L"--html=");
            hasHtml = true;
            if (arg.starts_with(L"--html="))
                request.htmlPath = std::wstring(arg.substr(7));
        }
    }
    LocalFree(arguments);
    if (!hasReport && !hasJson && !hasHtml)
        return std::nullopt;
    request.json = hasJson;
    if (!hasReport || invalid || (hasJson && hasHtml) || (hasHtml && request.htmlPath.empty())
        || count != 2 + static_cast<int>(hasJson) + static_cast<int>(hasHtml))
        request.path.clear();
    return request;
}

HANDLE probeOutput()
{
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((output == nullptr || output == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS))
        output = GetStdHandle(STD_OUTPUT_HANDLE);
    return output;
}

int reportCommandOutput(const ReportRequest& request)
{
    HANDLE output = probeOutput();
    if (output == nullptr || output == INVALID_HANDLE_VALUE)
        return 2;
    std::string message;
    int status = 1;
    std::string archive;
    std::string error;
    if (request.path.empty()) {
        message = request.json
            ? "{\"error\":\"usage\"}\n"
            : "Usage: YawaFpsHub.exe --report=<local-file.yawa> [--json | --html=<local-file.html>]\n";
        status = 2;
    } else if (!yawa::readLocalReport(request.path, archive, error)) {
        message = request.json ? "{\"error\":\"invalid_report\"}\n" : "Report: " + error + "\n";
    } else {
        yawa::ReportPackageContents contents;
        if (yawa::readReportPackage(archive, contents, error)) {
            if (!request.htmlPath.empty()) {
                std::string html;
                if (yawa::buildHtmlReport(contents, html, error)
                    && yawa::writeLocalHtmlReport(request.htmlPath, html, error)) {
                    message = "HTML report created. Review the image before sharing.\n";
                    status = 0;
                } else {
                    message = "Report: " + error + "\n";
                }
            } else if (request.json) {
                message = std::string("{\"schema\":1,\"game_frame\":\"")
                    + (contents.gameFrameIncluded ? "included" : "omitted")
                    + "\",\"session\":" + contents.sessionJson + "}\n";
                status = 0;
            } else {
                message = std::string(".yawa schema 1: valid\nGame frame: ")
                    + (contents.gameFrameIncluded ? "included" : "omitted")
                    + "\nSession: " + contents.sessionJson;
                if (message.back() != '\n')
                    message += '\n';
                status = 0;
            }
        } else {
            message = request.json ? "{\"error\":\"invalid_report\"}\n" : "Report: " + error + "\n";
        }
    }
    DWORD written = 0;
    return WriteFile(output, message.data(), static_cast<DWORD>(message.size()), &written, nullptr)
            && written == message.size()
        ? status
        : 2;
}

int memoryDumpCommandOutput()
{
    HANDLE output = probeOutput();
    if (output == nullptr || output == INVALID_HANDLE_VALUE)
        return 2;
    yawa::Log unopenedLog;
    yawa::PawnioSensors sensors;
    const auto dump = sensors.initialize(unopenedLog) ? sensors.readMemoryDump() : std::nullopt;
    const std::string text = dump ? yawa::formatAmdDdr4MemoryDump(*dump)
                                  : "UMC dump unavailable (requires AMD 17h/71h or 19h/50h, accessible "
                                    "PawnIO and readable DDR4 channels).\n";
    DWORD written = 0;
    return WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
            && written == text.size()
        ? (dump ? 0 : 1)
        : 2;
}

int spdProbeCommandOutput(unsigned address)
{
    HANDLE output = probeOutput();
    if (output == nullptr || output == INVALID_HANDLE_VALUE)
        return 2;
    yawa::Ddr4SpdProbeResult result { };
    std::string error;
    const bool success = yawa::readCezanneDdr4Spd(address, result, error);
    std::string text;
    if (success) {
        text = "DDR4 SPD passport: base CRC valid, minimum tCK "
            + std::to_string(result.passport.minimumCyclePs) + " ps";
        if (result.passport.moduleCapacityMiB)
            text += ", capacity " + std::to_string(*result.passport.moduleCapacityMiB) + " MiB";
        if (result.partNumber)
            text += ", module part number (unverified) " + *result.partNumber;
        text += ".\nXMP 2.0 passport: ";
        if (result.profiles.empty()) {
            text += "no signature";
        } else {
            for (const auto& profile : result.profiles) {
                if (profile.number != result.profiles.front().number)
                    text += "; ";
                text += "profile " + std::to_string(profile.number) + " minimum tCK "
                    + std::to_string(profile.minimumCyclePs) + " ps";
            }
        }
        text += ". Active profile: unknown (SPD does not report controller state).\n";
    } else {
        text = "SPD probe unavailable: " + error + "\n";
    }
    DWORD written = 0;
    return WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
            && written == text.size()
        ? (success ? 0 : 1)
        : 2;
}

bool anotherInstanceExists()
{
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kInstanceMutex);
    if (mutex == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    CloseHandle(mutex);
    return true;
}

bool isAlreadyRunning()
{
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (mutex == nullptr) {
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        return true;
    }
    return false;
}

void showExistingWindow()
{
    const HWND window = FindWindowW(L"YawaFpsHubMain", nullptr);
    if (window != nullptr) {
        ShowWindow(window, SW_RESTORE);
        SetForegroundWindow(window);
    }
}

} // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t* commandLine, int)
{
    const auto report = reportCommand();
    if (report)
        return reportCommandOutput(*report);
    const DumpRequest probe = probeCommand();
    if (probe.command != DumpCommand::None) {
        if (probe.command == DumpCommand::Memory)
            return memoryDumpCommandOutput();
        if (probe.command == DumpCommand::Spd)
            return spdProbeCommandOutput(probe.address);
        return 2;
    }
    const int sensorCommand = yawa::runSensorCommandLine();
    if (sensorCommand >= 0) {
        return sensorCommand;
    }
    yawa::Log log;
    log.open(yawa::logPath());
    log.info("вход в приложение");
    const std::wstring_view arguments = commandLine != nullptr ? commandLine : L"";
    const std::size_t smokeFlag = arguments.find(L"--smoke");
    const bool smoke = smokeFlag != std::wstring_view::npos;
    if (!smoke && arguments.empty() && anotherInstanceExists()) {
        log.info("уже запущен другой экземпляр");
        showExistingWindow();
        return 0;
    }
    if (yawa::prepareSensorsForLaunch(arguments)) {
        log.info("повышенный дочерний процесс завершён без запуска окна");
        return 0;
    }

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
        showExistingWindow();
        return 0;
    }

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
    const std::size_t rtssFlag = arguments.find(L"--smoke-rtss=");
    if (smoke && rtssFlag != std::wstring_view::npos) {
        const std::wstring filename(arguments.substr(rtssFlag + 13));
        std::string package;
        if (!yawa::readTextFile(yawa::narrow(filename), package) || package.size() > 8 * 1024 * 1024
            || !window.smokeImportRtss(package, error, filename,
                arguments.find(L"--smoke-rtss-image") != std::wstring_view::npos,
                arguments.find(L"--smoke-rtss-crop") != std::wstring_view::npos,
                arguments.find(L"--smoke-rtss-autowidth") != std::wstring_view::npos)) {
            log.error("самопроверка RTSS: " + (error.empty() ? "не удалось прочитать файл" : error));
            return 2;
        }
    }
    if (arguments.find(L"--overlay") != std::wstring_view::npos) {
        window.setOverlayVisible(true);
    }
    std::wstring overlayShotDir;
    const std::size_t shotFlag = arguments.find(L"--overlay-shots=");
    if (shotFlag != std::wstring_view::npos) {
        overlayShotDir = std::wstring(arguments.substr(shotFlag + 16));
    }

    if (arguments.find(L"--editor") != std::wstring_view::npos) {
        window.openEditorForSmoke();
    }
    if (!smoke && arguments.empty()) {
        window.scheduleSensorElevation();
    }
    const int result = window.run(smoke ? smokeSeconds : 0, overlayShotDir);
    log.info("остановка");
    if (SUCCEEDED(comResult)) {
        CoUninitialize();
    }
    return result;
}
