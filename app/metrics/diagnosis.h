#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace yawa {

enum class FindingSeverity { Info, Advice, Warning, Critical };
enum class GameCaptureState { Unknown, Running, ConfirmedFailure };

struct Finding
{
    FindingSeverity severity = FindingSeverity::Info;
    std::string code;
    std::string title;
    std::string detail;
    std::string action;
    unsigned confidence = 0;
};

// All loads are 0..100 and belong to gameProcessId. gpuLoad is the PID's 3D
// engine counter on the chosen LUID, not the overall GPU load.
struct GameDiagnosisInput
{
    std::uint32_t gameProcessId = 0;
    std::uint64_t nowMs = 0;
    std::uint64_t sampledAtMs = 0;
    GameCaptureState capture = GameCaptureState::Unknown;
    bool gpuMatchesGame = false;
    std::optional<double> fps;
    std::optional<double> gameCpuLoad;
    std::optional<double> gameThreadLoad;
    std::optional<double> gpuLoad;
};

std::vector<Finding> diagnoseGame(const GameDiagnosisInput& input);

} // namespace yawa
