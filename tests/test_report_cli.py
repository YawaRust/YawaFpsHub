"""Windows CLI smoke: a valid local archive and three fail-closed inputs."""

import argparse
import base64
import json
import re
import subprocess
import tempfile
import zipfile
from pathlib import Path


PNG = bytes.fromhex(
    "89504e470d0a1a0a0000000d4948445200000001000000010802000000907753de"
    "0000000c49444154789c63606060000000040001f6173855"
    "0000000049454e44ae426082"
)


def invoke(exe: Path, arguments: str | list[str], expected_status: int, expected_text: str) -> str:
    if isinstance(arguments, str):
        arguments = [arguments]
    result = subprocess.run(
        [str(exe), *arguments], capture_output=True, timeout=15, check=False
    )
    output = result.stdout.decode("utf-8")
    assert result.returncode == expected_status, (result.returncode, output)
    assert expected_text in output, output
    return output


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
            archive.writestr("diagnosis.json", '{"findings":[{"title":"<script>bad</script>"}]}\n')
            archive.writestr("screen.png", PNG)
        invoke(args.exe, f"--report={package}", 0, ".yawa schema 1: valid")
        invoke(args.exe, f"--report={package}", 0, 'Session: {"status":"no_game_sample"}')
        parsed = json.loads(invoke(args.exe, [f"--report={package}", "--json"], 0, '"schema":1'))
        assert parsed == {
            "schema": 1,
            "game_frame": "omitted",
            "session": {"status": "no_game_sample"},
        }
        reverse = json.loads(invoke(args.exe, ["--json", f"--report={package}"], 0, '"schema":1'))
        assert reverse == parsed
        html_path = Path(directory) / "offline report.html"
        invoke(args.exe, [f"--report={package}", f"--html={html_path}"], 0,
               "HTML report created")
        html = html_path.read_text(encoding="utf-8")
        embedded = re.search(r'src="data:image/png;base64,([A-Za-z0-9+/=]+)"', html)
        assert embedded and base64.b64decode(embedded.group(1)) == PNG
        assert "default-src 'none'" in html and "<script" not in html
        assert "&lt;script&gt;bad&lt;/script&gt;" in html
        assert "Заявленные выводы пакета" in html
        assert "Паспорт системы, аудит и история датчиков в этой версии пакета не собраны" in html
        assert "http://" not in html and "https://" not in html
        invoke(args.exe, [f"--report={package}", f"--html={html_path}"], 1,
               "уже существует")
        assert html_path.read_text(encoding="utf-8") == html
        broken = Path(directory) / "broken.yawa"
        broken.write_bytes(package.read_bytes().replace(b"no_game_sample", b"xo_game_sample"))
        invoke(args.exe, f"--report={broken}", 1, "Report:")
        assert json.loads(invoke(args.exe, [f"--report={broken}", "--json"],
                                 1, '"error":"invalid_report"')) == {"error": "invalid_report"}
        invalid_html = Path(directory) / "invalid.html"
        invoke(args.exe, [f"--report={broken}", f"--html={invalid_html}"], 1, "Report:")
        assert not invalid_html.exists()
        assert json.loads(invoke(args.exe,
                                 [f"--report={package}", "--json", f"--html={invalid_html}"],
                                 2, '"error":"usage"')) == {"error": "usage"}
        assert not invalid_html.exists()
        invoke(args.exe, f"--report={Path(directory) / 'missing.yawa'}", 1, "cannot open")
        invoke(args.exe, r"--report=\\server\share\report.yawa", 1, "network paths")
        invoke(args.exe, "--report", 2, "Usage:")
        assert json.loads(invoke(args.exe, "--json", 2, '"error":"usage"')) == {"error": "usage"}
        assert json.loads(invoke(args.exe, [f"--report={package}", "--json", "--probe"],
                                 2, '"error":"usage"')) == {"error": "usage"}


if __name__ == "__main__":
    try:
        main()
    except Exception:
        # Actions logs can be unavailable outside the runner; expose the actual
        # failing assertion in a check annotation rather than only "exit code 1".
        import traceback

        detail = traceback.format_exc().replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")
        print(f"::error title=Report CLI smoke::{detail}", flush=True)
        raise
