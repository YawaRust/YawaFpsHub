#include "metrics/diagnosis.h"

#include "metrics/units.h"

#include <cmath>

namespace yawa {
namespace {

    bool percent(std::optional<double> value)
    {
        return value && std::isfinite(*value) && *value >= 0.0 && *value <= 100.0;
    }

    std::string percentText(double value)
    {
        return formatWithUnit(value, MetricUnit::Percent, 0);
    }

} // namespace

std::vector<Finding> diagnoseGame(const GameDiagnosisInput& input)
{
    if (input.gameProcessId == 0 || input.nowMs < input.sampledAtMs
        || input.nowMs - input.sampledAtMs > 2000) {
        return { };
    }
    if (input.capture == GameCaptureState::ConfirmedFailure && (!input.fps || *input.fps <= 0.0)) {
        return { { FindingSeverity::Warning, "capture_failed", "Кадры игры не собираются",
            "Сбор кадров для активного процесса завершился подтверждённой ошибкой.",
            "Проверьте доступ к сбору кадров и сообщения диагностики.", 95 } };
    }
    if (input.capture != GameCaptureState::Running || !input.fps || !std::isfinite(*input.fps)
        || *input.fps <= 0.0 || !input.gpuMatchesGame || !percent(input.gpuLoad)) {
        return { };
    }

    const bool threadAllowsGpu
        = !input.gameThreadLoad || (percent(input.gameThreadLoad) && *input.gameThreadLoad < 90.0);
    if (percent(input.gameCpuLoad) && *input.gpuLoad >= 95.0 && *input.gameCpuLoad <= 70.0
        && threadAllowsGpu) {
        return { { FindingSeverity::Advice, "gpu_bound", "Возможен упор в видеокарту",
            "GPU 3D процесса игры: " + percentText(*input.gpuLoad)
                + "; CPU процесса: " + percentText(*input.gameCpuLoad) + ".",
            "Проверьте настройки графики и сравните время кадра до и после изменения.", 75 } };
    }
    if (percent(input.gameThreadLoad) && *input.gameThreadLoad >= 90.0 && *input.gpuLoad <= 85.0) {
        return { { FindingSeverity::Advice, "cpu_bound", "Возможен упор в поток игры",
            "Поток игры: " + percentText(*input.gameThreadLoad)
                + "; GPU 3D процесса: " + percentText(*input.gpuLoad) + ".",
            "Проверьте настройки, влияющие на процессор, и сравните время кадра.", 75 } };
    }
    return { };
}

} // namespace yawa
