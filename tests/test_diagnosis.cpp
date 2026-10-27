#include "metrics/diagnosis.h"
#include "test_support.h"

#include <limits>

namespace yawa::test {

void runDiagnosisTests()
{
    GameDiagnosisInput game;
    game.gameProcessId = 421;
    game.nowMs = 10000;
    game.sampledAtMs = 9500;
    game.capture = GameCaptureState::Running;
    game.fps = 120.0;
    game.gameCpuLoad = 42.0;
    game.gpuLoad = 99.0;
    CHECK(diagnoseGame(game).empty()); // GPU load is not tied to the game's adapter.

    game.gpuMatchesGame = true;
    auto findings = diagnoseGame(game);
    CHECK(findings.size() == 1);
    if (!findings.empty()) {
        CHECK(findings[0].code == "gpu_bound");
        CHECK(findings[0].detail.find("99") != std::string::npos);
        CHECK(findings[0].detail.find("42") != std::string::npos);
        CHECK(findings[0].confidence < 100);
    }
    game.fps = 30.0; // Bottleneck is about the active process, not display refresh.
    CHECK(diagnoseGame(game).size() == 1);
    game.sampledAtMs = 7900;
    CHECK(diagnoseGame(game).empty());
    game.sampledAtMs = 9500;
    game.gameProcessId = 0;
    CHECK(diagnoseGame(game).empty());
    game.gameProcessId = 421;
    game.gameCpuLoad.reset();
    CHECK(diagnoseGame(game).empty());
    game.gameCpuLoad = -1.0;
    CHECK(diagnoseGame(game).empty());
    game.gameCpuLoad = 42.0;
    game.gpuLoad = std::numeric_limits<double>::quiet_NaN();
    CHECK(diagnoseGame(game).empty());

    game.gpuLoad = 70.0;
    game.gameThreadLoad = 95.0;
    findings = diagnoseGame(game);
    CHECK(findings.size() == 1);
    if (!findings.empty())
        CHECK(findings[0].code == "cpu_bound");
    game.gameThreadLoad.reset();
    CHECK(diagnoseGame(game).empty());
    game.capture = GameCaptureState::Unknown;
    CHECK(diagnoseGame(game).empty());
    game.capture = GameCaptureState::ConfirmedFailure;
    CHECK(diagnoseGame(game).empty()); // Error and valid frames conflict: no false claim.
    game.fps.reset();
    findings = diagnoseGame(game);
    CHECK(findings.size() == 1);
    if (!findings.empty())
        CHECK(findings[0].code == "capture_failed");
    game.capture = GameCaptureState::Running;
    CHECK(diagnoseGame(game).empty());
}

} // namespace yawa::test
