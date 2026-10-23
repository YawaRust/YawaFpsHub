# YawaFpsHub

Портативный монитор FPS и состояния системы для Windows: один exe, без установки, без сторонних
библиотек и без сетевых запросов.

## Что внутри

- **FPS и frame time** — сбор кадров через ETW-провайдеры DXGI/D3D, без инъекции в процесс игры.
  1% и 0.1% low считаются по гистограмме frame time, а не по среднему.
- **Показатели системы** — процессор (общий и по ядрам), видеокарта (загрузка, VRAM, температура,
  частоты, питание), память, накопители, сеть. Источники — PDH, `NtQuerySystemInformation`, DXGI.
- **Интерфейс** — Direct2D/DirectWrite, без Electron и WebView.
- **Оверлей** — отдельное topmost-окно, не трогает кадры игры.
- **Портативность** — настройки в `YawaFpsHub.ini` рядом с exe, реестр не используется.

## Статус

Проект в разработке, идёт поэтапно (см. `docs/roadmap.md`).

- этап 1 — дизайн-система, прототип интерфейса, профиль проекта в `AGENTS.md`;
- этап 2 — ядро метрик на C++ и тесты, стили оверлея, свои подписи полей, выбор GPU;
- этап 3 — ETW-сессия кадров, платформенные сенсоры, диагностика `yawa-probe`;
- этап 4 — рабочее приложение: Direct2D-интерфейс, оверлей, трей, хоткеи, портативные настройки.

![Обзор](docs/screenshots/dashboard.png)

Остальные экраны — `docs/screenshots/sensors.png`, `docs/screenshots/overlay.png`,
`docs/screenshots/settings.png`.

Оверлей — настоящее layered-окно поверх игры, пять стилей, четыре позиции:

![Стили оверлея](docs/screenshots/overlays-preview.png)

Скриншоты снимаются в CI из живого окна (`docs/screenshots/`), а не рисуются вручную.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DYAWA_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Эти три команды собирают и проверяют ядро метрик и на Windows, и на Linux.

## Прототип интерфейса

Прототип не входит в сборку приложения и не содержит зависимостей: это статические HTML/CSS/JS.

```bash
python3 -m http.server 8000 --directory ui-prototype
```

Открыть `http://localhost:8000`. Данные в прототипе имитируются, интерфейс полностью интерактивный.

## Диагностика датчиков (Windows)

`yawa-probe` показывает, какие источники поднялись на конкретной машине, и печатает значения
в том же виде, в каком их увидит интерфейс:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
.\build\tools\probe\Release\yawa-probe.exe --seconds 10 --interval 1000
```

Ключи: `--seconds`, `--interval`, `--gpu <индекс>`, `--no-etw`, `--log <путь>`.
Подробнее об источниках и лицензиях — `docs/sensors.md`.

## Сборка приложения (Windows, MSVC)

```bash
cmake -S . -B build -A x64 -DYAWA_BUILD_APP=ON
cmake --build build --config Release
```

`YawaFpsHub.exe` появляется в каталоге сборки цели (`build/app/Release`), рядом с ним приложение
создаёт `YawaFpsHub.ini`, `YawaFpsHub.log` и `sessions\`. Автосборка, тесты, самопроверка exe и
скриншоты экранов — GitHub Actions, workflow `build`.

### Управление

- окно, трей и иконка в области уведомлений — меню с оверлеем, позицией и выходом;
- `F10` — показать или скрыть оверлей, `F11` — следующая позиция, `F12` — свернуть окно в трей;
- «Пауза» в шапке останавливает опрос счётчиков, «Оверлей» включает наложение, кружочки
  справа переключают акцент интерфейса;
- `YawaFpsHub.exe --smoke` — самопроверка: окно живёт 8 секунд, затем закрывается само
  (`--smoke=22` задаёт своё время, `--overlay` сразу показывает наложение);
- `--view=dashboard|sensors|overlay|settings` — стартовый экран (используется в CI для скриншотов);
- `--overlay-shots=<папка>` — сохраняет PNG всех пяти стилей оверлея (диагностика и демо).

## Структура

```
app/metrics/    ядро показателей: буферы, статистика, единицы, реестр GPU (без windows.h)
app/platform/   Win32: ETW-сессия кадров, PDH/DXGI/NVML-сенсоры, логгер
tools/probe/    yawa-probe, консольная диагностика датчиков
app/render/     Direct2D: темы, шрифты, графики
app/ui/         экраны и настройки
tests/          тесты портируемой части, без фреймворков
ui-prototype/   интерактивный прототип интерфейса (этап 1)
```

Правила работы с кодом — `AGENTS.md`. Формат `YawaFpsHub.ini` — `docs/settings.md`.
