#pragma once

#include <windows.h>

#include <string_view>

namespace yawa {

// Повышение запускается после показа окна: отсутствие UAC не блокирует интерфейс.
void requestSensorElevation(HWND window, UINT completionMessage);
// Только дочерний --sensor-ready=<pid> устанавливает пакет и ждёт выхода родителя.
// true означает, что дочернему процессу не следует открывать главное окно.
bool prepareSensorsForLaunch(std::wstring_view arguments);

// -1 означает, что аргументы не относятся к служебным командам.
int runSensorCommandLine();

} // namespace yawa
