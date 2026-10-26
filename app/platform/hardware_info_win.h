#pragma once

#include "metrics/hardware_info.h"

#include <string>

namespace yawa {

class GpuRegistry;

// Один сбор сведений о машине при старте: реестр, CPUID, DXGI, WMI и EnumDisplayDevices.
// Источники — встроенные в Windows, поэтому приложение остаётся одним exe без драйверов
// и без сторонних библиотек. Если WMI недоступен, поля просто остаются пустыми:
// интерфейс покажет «нет данных», а не выдуманные цифры.
bool collectHardwareInfo(HardwareIdentity& info, const GpuRegistry& registry, std::string& error);

} // namespace yawa
