"""Windows CLI smoke: a valid local archive and three fail-closed inputs."""

import argparse
import subprocess
import tempfile
import zipfile
from pathlib import Path


PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010802000000907753de"
    "0000000c49444154789c63606060000000040001f6173855"
    "0000000049454e44ae426082"
)


def invoke(exe: Path, argument: str, expected_status: int, expected_text: str) -> None:
    result = subprocess.run(
        [str(exe), argument], capture_output=True, timeout=15, check=False
    )
    output = result.stdout.decode("utf-8")
    assert result.returncode == expected_status, (result.returncode, output)
    assert expected_text in output, output


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        package = Path(directory) / "report with spaces.yawa"
        with zipfile.ZipFile(package, "w", compression=zipfile.ZIP_STORED) as archive:
            archive.writestr(
                "manifest.json",
                '{"schema":1,"format":"yawa","game_frame":"omitted"}\n',
            )
            archive.writestr("system.json", '{"status":"not_collected"}\n')
            archive.writestr("session.json", '{"status":"no_game_sample"}\n')
            archive.writestr("sensors.csv", "metric,value,unit\n")
            archive.writestr("audit.json", '{"status":"not_collected"}\n')
            archive.writestr("diagnosis.json", "{}\n")
            archive.writestr("screen.png", PNG)
        invoke(args.exe, f"--report={package}", 0, ".yawa schema 1: valid")
        invoke(args.exe, f"--report={package}", 0, 'Session: {"status":"no_game_sample"}')
        broken = Path(directory) / "broken.yawa"
        broken.write_bytes(package.read_bytes().replace(b"no_game_sample", b"xo_game_sample"))
        invoke(args.exe, f"--report={broken}", 1, "Report:")
        invoke(args.exe, f"--report={Path(directory) / 'missing.yawa'}", 1, "cannot open")
        invoke(args.exe, r"--report=\\server\share\report.yawa", 1, "network paths")
        invoke(args.exe, "--report", 2, "Usage:")


if __name__ == "__main__":
    main()
