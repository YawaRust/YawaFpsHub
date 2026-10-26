(function () {
  "use strict";

  const $ = (selector, root = document) => root.querySelector(selector);
  const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];

  const TARGET_HZ = 120;
  const TARGET_FRAME_MS = 1000 / TARGET_HZ;
  const RAM_TOTAL = 64;

  const VIEWS = {
    dashboard: { eyebrow: "Мониторинг", title: "Обзор" },
    sensors: { eyebrow: "Показатели", title: "Все датчики" },
    bottleneck: { eyebrow: "Узкое место", title: "Баланс системы" },
    system: { eyebrow: "Оборудование", title: "Сведения о системе" },
    overlay: { eyebrow: "Поверх игры", title: "Оверлей" },
    settings: { eyebrow: "Настройки", title: "Поведение и вид" }
  };

  const GROUPS = {
    fps: "Кадры",
    cpu: "Процессор",
    gpu: "Видеокарта",
    mem: "Память",
    io: "Диски и сеть",
    sys: "Система"
  };

  // Прототип открывают и двойным щелчком по файлу, где localStorage может быть запрещён.
  const store = {
    read(key) {
      try {
        return localStorage.getItem(key);
      } catch (error) {
        return null;
      }
    },
    write(key, value) {
      try {
        localStorage.setItem(key, value);
      } catch (error) {
        /* настройки прототипа не критичны */
      }
    }
  };

  const formatCache = new Map();

  function fmt(value, decimals) {
    const key = decimals;
    if (!formatCache.has(key)) {
      formatCache.set(key, new Intl.NumberFormat("ru-RU", {
        minimumFractionDigits: decimals,
        maximumFractionDigits: decimals
      }));
    }
    return formatCache.get(key).format(value);
  }

  function formatClock(ms) {
    const total = Math.floor(ms / 1000);
    const hours = String(Math.floor(total / 3600)).padStart(2, "0");
    const minutes = String(Math.floor((total % 3600) / 60)).padStart(2, "0");
    const seconds = String(total % 60).padStart(2, "0");
    return `${hours}:${minutes}:${seconds}`;
  }

  const accentColor = () => getComputedStyle(document.documentElement).getPropertyValue("--accent").trim();

  function accentName() {
    return document.documentElement.dataset.accent || "mint";
  }

  function toast(message) {
    let node = $("#toast");
    if (!node) {
      node = document.createElement("div");
      node.id = "toast";
      node.className = "toast";
      document.body.appendChild(node);
    }
    node.textContent = message;
    node.classList.add("is-visible");
    clearTimeout(node.hideTimer);
    node.hideTimer = setTimeout(() => node.classList.remove("is-visible"), 2600);
  }

  const computedMetrics = {
    sessionTime: (snap) => formatClock(snap.elapsedMs),
    framesTotal: (snap) => fmt(snap.framesRendered, 0),
    framesLost: (snap) => fmt(snap.framesLost, 0),
    frameMedian: (snap) => `${fmt(snap.frameMs, 1)} мс`,
    budget: (snap) => `${fmt((snap.frameMs / TARGET_FRAME_MS) * 100, 0)}%`,
    dropped: (snap) => fmt((snap.framesLost / Math.max(1, snap.framesRendered)) * 100, 1),
    procRamFull: (snap) => `${fmt(snap.sensors.get("procRam").value, 1)} МБ`
  };

  const DEFAULT_LABELS = { fps: "FPS", cpu: "CPU", gpu: "GPU", ram: "RAM", vram: "VRAM" };

  const overlayState = {
    position: "tl",
    layout: "plates",
    style: "frosted",
    scale: 100,
    opacity: 72,
    radius: null,
    background: true,
    border: true,
    shadow: true,
    offsetX: 0,
    offsetY: 0,
    font: 0,
    bold: false,
    italic: false,
    outline: 0,
    // null — значение из пресета, как -1 в приложении.
    textScale: 100,
    bgOpacity: null,
    padding: null,
    rowGap: null,
    valueColor: null,
    labelColor: null,
    accentColor: null,
    // Цвет каждой строки оверлея: null — как в стиле.
    rowColors: { fps: null, cpu: null, gpu: null, ram: null, vram: null },
    borderWidth: null,
    shadowStrength: 100,
    tracking: 100,
    uppercase: null,
    separators: null,
    fpsDecimals: null,
    clickThrough: false,
    labels: { ...DEFAULT_LABELS }
  };

  // Палитра оверлея: индекс соответствует Color в приложении.
  const OSD_PALETTE = [null, "#ffffff", "#5ef29b", "#6aa8ff", "#b47bff", "#ffb347", "#ff6f6f"];
  const osdColor = (index) => (index === null ? null : OSD_PALETTE[index + 1] || null);
  const OSD_ROWS = [
    { key: "fps", title: "Кадры" },
    { key: "cpu", title: "Процессор" },
    { key: "gpu", title: "Видеокарта" },
    { key: "ram", title: "Память" },
    { key: "vram", title: "Видеопамять" }
  ];

  const perfState = { cpu: true, gpu: true, ram: true };

  function metricText(id, snap) {
    if (computedMetrics[id]) return computedMetrics[id](snap);
    const sensor = snap.sensors.get(id);
    if (!sensor) return null;
    return fmt(sensor.value, sensor.decimals);
  }

  function updateMetrics(snap) {
    for (const node of $$("[data-metric]")) {
      const id = node.dataset.metric;

      if (id === "fpsDelta") {
        const average = snap.sensors.get("fpsAvg").value;
        const delta = ((snap.fps - average) / Math.max(1, average)) * 100;
        node.textContent = `${delta >= 0 ? "+" : "−"}${fmt(Math.abs(delta), 1)}%`;
        node.classList.toggle("is-down", delta < 0);
        continue;
      }

      // Знаки после запятой у кадров задаются настройкой оверлея.
      const isFps = id === "fps" || id === "fpsLow1";
      const text = isFps && overlayState.fpsDecimals !== null && node.closest("#osdPreview")
        ? fmt(snap.sensors.get(id).value, overlayState.fpsDecimals)
        : metricText(id, snap);
      if (text === null) continue;

      if (node.firstElementChild) {
        const textNode = node.firstChild;
        if (textNode && textNode.nodeType === Node.TEXT_NODE) textNode.textContent = `${text} `;
      } else {
        node.textContent = text;
      }
    }
  }

  function updateBars(snap) {
    const value = (id) => snap.sensors.get(id).value;
    const widths = {
      budget: (snap.frameMs / TARGET_FRAME_MS) * 100,
      vram: (value("vramUsed") / window.Sim.gpuInfo().memoryGb) * 100,
      ramUsed: (value("ramUsed") / RAM_TOTAL) * 100,
      ramCache: (value("ramCache") / RAM_TOTAL) * 100,
      tempCpu: value("cpuTemp"),
      tempGpu: value("gpuTemp"),
      tempSsd: value("diskTemp"),
      tempChipset: value("chipsetTemp")
    };

    for (const node of $$("[data-bar]")) {
      const width = widths[node.dataset.bar];
      if (width === undefined) continue;
      node.style.width = `${Math.max(0, Math.min(100, width)).toFixed(1)}%`;
    }

    const gauges = { cpu: value("cpu"), gpu: value("gpu") };
    for (const node of $$("[data-gauge]")) {
      const load = gauges[node.dataset.gauge];
      if (load === undefined) continue;
      node.style.setProperty("--p", load.toFixed(1));
    }
  }

  const coreOffsets = Array.from({ length: 16 }, (_, index) => Math.sin(index * 2.1) * 7 + Math.cos(index / 2) * 5);
  let coreColumns = [];

  function buildCores() {
    const host = $("#coreBars");
    coreColumns = coreOffsets.map(() => {
      const column = document.createElement("div");
      column.className = "cores__col";
      const fill = document.createElement("i");
      column.appendChild(fill);
      host.appendChild(column);
      return fill;
    });
  }

  function updateCores(snap) {
    const phase = snap.elapsedMs / 2600;
    coreColumns.forEach((fill, index) => {
      const load = Math.max(3, Math.min(100, snap.cpu + coreOffsets[index] + Math.sin(phase + index) * 12));
      fill.style.height = `${load.toFixed(0)}%`;
      fill.style.setProperty("--load", load.toFixed(0));
    });
  }

  const PROCESSES = [
    { name: "Cyberpunk2077.exe", cpu: 38.2, ram: "8 412 МБ" },
    { name: "chrome.exe", cpu: 6.4, ram: "2 204 МБ" },
    { name: "steam.exe", cpu: 2.1, ram: "684 МБ" },
    { name: "Discord.exe", cpu: 1.3, ram: "512 МБ" },
    { name: "YawaFpsHub.exe", cpu: 0.3, ram: "24,6 МБ" }
  ];

  function buildProcesses() {
    const host = $("#procList");
    const list = PROCESSES.map((process) => {
      const item = document.createElement("li");
      item.className = "proc";
      item.innerHTML = `
        <span class="proc__icon">${process.name.slice(0, 2)}</span>
        <span class="proc__name"><b>${process.name}</b><span><i></i></span></span>
        <span class="proc__cpu">0%</span>
        <span class="proc__ram">${process.ram}</span>`;
      host.appendChild(item);
      return {
        cpu: item.querySelector(".proc__cpu"),
        bar: item.querySelector(".proc__name span i"),
        base: process.cpu
      };
    });
    return list;
  }

  let processRows = [];

  function updateProcesses(snap) {
    processRows.forEach((row, index) => {
      const jitter = Math.sin(snap.elapsedMs / 1800 + index * 1.7) * (row.base * 0.08);
      const cpu = Math.max(0.1, row.base + (index === 0 ? (snap.cpu - 44) * 0.55 : jitter));
      row.cpu.textContent = `${fmt(cpu, 1)}%`;
      row.bar.style.width = `${Math.min(100, (cpu / 45) * 100).toFixed(0)}%`;
    });
  }

  const sensorRows = new Map();
  const sensorFilter = { group: "all", query: "", pinnedOnly: false };

  const OSD_PRIMARY = { fps: "fps", cpu: "cpu", gpu: "gpu", ramUsed: "ram", vramUsed: "vram" };

  function saveOverlaySensors() {
    const selected = Object.fromEntries([...sensorRows].map(([id, entry]) => [id, Boolean(entry.sensor.pinned)]));
    store.write("yawa.overlaySensors", JSON.stringify(selected));
  }

  function rebuildOverlaySensors() {
    const host = $("#osdPreview");
    host.replaceChildren();
    const sensors = [...sensorRows.values()].map((entry) => entry.sensor);
    const primary = Object.keys(OSD_PRIMARY);
    sensors.sort((a, b) => {
      const ai = primary.indexOf(a.id);
      const bi = primary.indexOf(b.id);
      return (ai < 0 ? primary.length : ai) - (bi < 0 ? primary.length : bi);
    });
    for (const sensor of sensors) {
      const entry = sensorRows.get(sensor.id);
      entry.row.querySelector(".sensor-row__eye").classList.toggle("is-on", Boolean(sensor.pinned));
      if (!sensor.pinned) continue;
      const row = document.createElement("div");
      row.className = "osd__row";
      const field = OSD_PRIMARY[sensor.id];
      row.dataset.osdItem = field || sensor.id;
      const label = document.createElement("span");
      label.className = "osd__key";
      label.textContent = field ? overlayState.labels[field] : sensor.label;
      const value = document.createElement("b");
      value.className = sensor.id === "fps" ? "osd__val" : "osd__val osd__val--sm";
      value.dataset.metric = sensor.id;
      const unit = document.createElement("span");
      unit.className = "osd__unit";
      unit.textContent = sensor.unit;
      row.append(label, value, unit);
      host.appendChild(row);
    }
    for (const [id, field] of Object.entries(OSD_PRIMARY)) {
      const toggle = $(`[data-osd-toggle="${field}"]`);
      if (toggle) toggle.checked = Boolean(sensorRows.get(id)?.sensor.pinned);
    }
    updateMetrics(window.Sim.snapshot());
    applyOverlay();
  }

  function buildSensorTable() {
    const host = $("#sensorRows");
    const snapshot = window.Sim.snapshot();
    let currentGroup = "";

    for (const sensor of snapshot.sensors.values()) {
      if (sensor.group !== currentGroup) {
        currentGroup = sensor.group;
        const heading = document.createElement("div");
        heading.className = "sensor-group";
        heading.dataset.group = sensor.group;
        heading.textContent = GROUPS[sensor.group] || sensor.group;
        host.appendChild(heading);
      }

      const row = document.createElement("div");
      row.className = "sensor-row";
      row.dataset.group = sensor.group;
      row.dataset.label = sensor.label.toLowerCase();
      row.innerHTML = `
        <span class="sensor-row__name">
          <i class="sensor-row__pip" style="background:${sensor.color};color:${sensor.color}"></i>
          <span>${sensor.label}</span>
        </span>
        <span class="sensor-row__value">—</span>
        <span class="sensor-row__stat">—</span>
        <span class="sensor-row__stat">—</span>
        <span class="sensor-row__stat">—</span>
        <canvas class="spark"></canvas>
        <button class="sensor-row__eye" type="button" aria-label="Показывать в оверлее" title="Показывать в оверлее">
          <svg viewBox="0 0 24 24"><use href="#i-eye"/></svg>
        </button>`;

      const eye = row.querySelector(".sensor-row__eye");
      eye.classList.toggle("is-on", Boolean(sensor.pinned));
      eye.addEventListener("click", () => {
        sensor.pinned = !sensor.pinned;
        eye.classList.toggle("is-on", sensor.pinned);
        saveOverlaySensors();
        rebuildOverlaySensors();
        applySensorFilter();
      });

      host.appendChild(row);
      sensorRows.set(sensor.id, {
        row,
        heading: null,
        value: row.querySelector(".sensor-row__value"),
        min: row.querySelectorAll(".sensor-row__stat")[0],
        avg: row.querySelectorAll(".sensor-row__stat")[1],
        max: row.querySelectorAll(".sensor-row__stat")[2],
        spark: row.querySelector(".spark"),
        sensor
      });
    }
    const saved = store.read("yawa.overlaySensors");
    if (saved) {
      try {
        const selected = JSON.parse(saved);
        for (const [id, entry] of sensorRows) {
          if (typeof selected?.[id] === "boolean") entry.sensor.pinned = selected[id];
        }
      } catch (error) {
        /* Повреждённый выбор не мешает открыть прототип. */
      }
    }
    rebuildOverlaySensors();
  }

  function applySensorFilter() {
    const query = sensorFilter.query.trim().toLowerCase();
    let visible = 0;

    for (const entry of sensorRows.values()) {
      const matchesGroup = sensorFilter.group === "all" || entry.sensor.group === sensorFilter.group;
      const matchesQuery = !query || entry.sensor.label.toLowerCase().includes(query);
      const matchesPin = !sensorFilter.pinnedOnly || entry.sensor.pinned;
      const isVisible = matchesGroup && matchesQuery && matchesPin;
      entry.row.hidden = !isVisible;
      if (isVisible) visible += 1;
    }

    for (const heading of $$(".sensor-group")) {
      const hasVisible = [...sensorRows.values()].some(
        (entry) => entry.sensor.group === heading.dataset.group && !entry.row.hidden
      );
      heading.hidden = !hasVisible;
    }

    $("#sensorEmpty").hidden = visible > 0;
  }

  function updateSensorTable(snap) {
    for (const entry of sensorRows.values()) {
      const { sensor } = entry;
      if (entry.row.hidden) continue;
      entry.value.innerHTML = `${fmt(sensor.value, sensor.decimals)} <small>${sensor.unit}</small>`;
      entry.min.textContent = fmt(sensor.min, sensor.decimals);
      entry.avg.textContent = fmt(sensor.sum / sensor.samples, sensor.decimals);
      entry.max.textContent = fmt(sensor.max, sensor.decimals);
      if (entry.row.offsetParent) {
        window.Charts.spark(entry.spark, { values: sensor.history, color: sensor.color });
      }
    }
  }

  function drawCharts(snap) {
    const accent = accentColor();

    const frameChart = $("#frameChart");
    if (frameChart && frameChart.offsetParent) {
      window.Charts.frame(frameChart, {
        values: snap.sensors.get("frameMs").history,
        color: accent,
        targetHz: TARGET_HZ
      });
    }

    const perfChart = $("#perfChart");
    if (perfChart && perfChart.offsetParent) {
      window.Charts.multi(perfChart, {
        series: [
          { values: snap.sensors.get("cpu").history, color: "#5ef29b", visible: perfState.cpu },
          { values: snap.sensors.get("gpu").history, color: "#6aa8ff", visible: perfState.gpu },
          { values: snap.sensors.get("ramUsed").history, color: "#b47bff", visible: perfState.ram }
        ]
      });
    }

    for (const canvas of $$(".spark[data-series]")) {
      const seriesId = canvas.dataset.series;
      const sensor = snap.sensors.get(seriesId);
      if (!sensor) continue;
      window.Charts.spark(canvas, { values: sensor.history, color: sensor.color });
    }
  }

  function updateStatus(snap) {
    $("#railLive").classList.toggle("is-paused", snap.paused);
  }


  // ── «Баланс» и «Сведения о системе» ────────────────────────────────────────
  // Цвет verdict-блока зависит от того, в чем узкое место: ровно как в приложении,
  // где его подбирает severityColor() по проценту помех.
  function balanceColor(percent) {
    if (percent >= 35) return "var(--danger)";
    if (percent >= 12) return "var(--warn)";
    return "var(--accent)";
  }

  function plural(count, one, few, many) {
    const tail = count % 100;
    const last = count % 10;
    if (tail > 10 && tail < 20) return many;
    if (last === 1) return one;
    if (last > 1 && last < 5) return few;
    return many;
  }

  function setText(selector, text) {
    const node = $(selector);
    if (node) node.textContent = text;
  }

  function renderBalance(snap) {
    const balance = snap.balance;
    const machine = snap.machine;
    if (!balance || !machine) return;

    const card = $("#balanceCard");
    const tone = balanceColor(balance.percent);
    if (card) card.style.setProperty("--balance-color", tone);
    const gauge = $("#balanceGauge");
    if (gauge) gauge.style.setProperty("--accent", tone);
    if (gauge) gauge.style.setProperty("--p", balance.percent.toFixed(1));

    const gpu = machine.gpus.find((item) => item.discrete) || machine.gpus[0];
    const display = machine.displays.find((item) => item.primary) || machine.displays[0];
    const subtitle = [machine.cpu.name, gpu && gpu.name, display && window.Sim.describeDisplay(display)]
      .filter(Boolean).join(" · ");
    setText("#balanceSubtitle", subtitle || "оборудование не определено");

    const tag = $("#balanceMode");
    if (tag) {
      tag.classList.toggle("tag--live", balance.measured);
      tag.querySelector("span").textContent = balance.measured ? "ЖИВЫЕ ЗАМЕРЫ" : "ОЦЕНКА ±15 %";
    }

    const quiet = balance.kind === "idle" || balance.kind === "no-data";
    setText("#balancePercent", quiet ? "—" : `${balance.percent}%`);
    setText("#balanceHeadline", balance.headline);
    setText("#balanceDetail", balance.detail);

    const caps = (selector, text) => {
      const node = $(selector);
      if (node) node.innerHTML = text;
    };
    caps("#balanceCpuCap", `CPU <b>~${Math.round(balance.cpuCapacity)} к/с</b>`);
    caps("#balanceGpuCap", `GPU <b>~${Math.round(balance.gpuCapacity)} к/с</b>`);
    caps("#balanceExpected", balance.expected > 0
      ? `ожидание: <b>${Math.round(balance.expected)} к/с</b>` : "ожидание: —");

    const cpuIndex = window.Sim.cpuPerfIndex(machine.cpu.name, machine.cpu.cores, machine.cpu.threads);
    const gpuIndex = gpu ? window.Sim.gpuPerfIndex(gpu.name, gpu.dedicatedMb) : 0;
    const pair = [
      { label: "Процессор", value: cpuIndex > 0 ? Math.round(cpuIndex) : "—",
        note: machine.cpu.name || "нет данных", color: "var(--cpu)",
        fraction: Math.min(1, cpuIndex / 120) },
      { label: "Видеокарта", value: gpuIndex > 0 ? Math.round(gpuIndex) : "—",
        note: gpu ? gpu.name : "нет данных", color: "var(--gpu)",
        fraction: Math.min(1, gpuIndex / 150) },
      { label: "Память", value: window.Sim.describeMemory(machine.memory),
        note: machine.memory.profile ? "профиль памяти активен" : "XMP/EXPO не включён",
        color: "var(--ram)", fraction: Math.min(1, machine.memory.totalGb / 64) },
      { label: "Экран", value: display ? `${display.width}×${display.height}` : "—",
        note: display ? `${display.hz} Гц` : "нет данных", color: "var(--accent)", fraction: 1 }
    ];
    const pairHost = $("#balancePair");
    if (pairHost) {
      pairHost.innerHTML = pair.map((row) => `
        <li>
          <div class="row"><span>${row.label}</span><b>${row.value}</b></div>
          <div class="bar" style="--c:${row.color}"><i style="width:${(row.fraction * 100).toFixed(1)}%"></i></div>
          <p class="note">${row.note}</p>
        </li>`).join("");
    }

    const sensorValue = (id) => {
      const sensor = snap.sensors.get(id);
      return sensor ? sensor : null;
    };
    const corePeak = Math.max(...Array.from({ length: 8 }, (_, index) => {
      const sensor = sensorValue(`core${index}`);
      return sensor ? sensor.value : 0;
    }));
    const vramTotal = gpu ? gpu.memoryGb : 0;
    const load = [
      { label: "Процессор", id: "cpu", unit: "%", decimals: 0, color: "var(--cpu)", total: 100 },
      { label: "Пик по ядрам", value: corePeak, unit: "%", decimals: 0, color: "var(--cpu)", total: 100 },
      { label: "Видеокарта", id: "gpu", unit: "%", decimals: 0, color: "var(--gpu)", total: 100 },
      { label: "Видеопамять", id: "vramUsed", unit: "ГБ", decimals: 1, color: "var(--ram)", total: vramTotal }
    ];
    const loadHost = $("#balanceLoad");
    if (loadHost) {
      loadHost.innerHTML = load.map((row) => {
        const sensor = row.id ? sensorValue(row.id) : null;
        const known = row.value !== undefined || sensor;
        const value = row.value !== undefined ? row.value : (sensor ? sensor.value : 0);
        const share = row.total > 0 ? Math.max(0, Math.min(100, (value / row.total) * 100)) : 0;
        return `
          <li>
            <div class="row">
              <span>${row.label}</span>
              <b class="${known ? "" : "is-muted"}">${known ? `${fmt(value, row.decimals)} ${row.unit}` : "нет данных"}</b>
            </div>
            <div class="bar" style="--c:${row.color}"><i style="width:${known ? share.toFixed(1) : 0}%"></i></div>
          </li>`;
      }).join("");
    }

    const advice = balance.advice || [];
    setText("#balanceAdviceCount", advice.length
      ? `${advice.length} ${plural(advice.length, "рекомендация", "рекомендации", "рекомендаций")}`
      : "рекомендаций нет — нагрузка или данные отсутствуют");
    const adviceHost = $("#balanceAdvice");
    if (adviceHost) {
      adviceHost.innerHTML = advice.map((item) => `<li><b>${item.title}</b>${item.text}</li>`).join("");
    }
  }

  function specRows(selector, rows) {
    const host = $(selector);
    if (!host) return;
    host.innerHTML = rows.map((row) => `<li><span>${row[0]}</span><b>${row[1] || "нет данных"}</b></li>`).join("");
  }

  function renderSystem(snap) {
    const machine = snap.machine;
    if (!machine) return;

    specRows("#osCard", [
      ["Система", machine.os.product],
      ["Сборка", `${machine.os.feature} · ${machine.os.build}`],
      ["Издание", machine.os.edition],
      ["Архитектура", machine.os.arch]
    ]);

    setText("#cpuName", machine.cpu.name || "нет данных");
    const cpuIndex = window.Sim.cpuPerfIndex(machine.cpu.name, machine.cpu.cores, machine.cpu.threads);
    specRows("#cpuCard", [
      ["Ядра", `${machine.cpu.cores} ядер / ${machine.cpu.threads} потоков`],
      ["Базовая частота", `${fmt(machine.cpu.baseMhz, 0)} МГц`],
      ["Кэш L3", machine.cpu.l3Mb ? `${machine.cpu.l3Mb} МБ` : ""],
      ["Вендор", machine.cpu.vendor],
      ["Индекс", cpuIndex > 0 ? fmt(cpuIndex, 0) : ""]
    ]);

    const gpuHost = $("#gpuCard");
    if (gpuHost) {
      const primary = machine.gpus.find((item) => item.discrete) || machine.gpus[0];
      gpuHost.innerHTML = machine.gpus.map((gpu) => {
        const index = window.Sim.gpuPerfIndex(gpu.name, gpu.dedicatedMb);
        const isActive = gpu === primary;
        return `
          <li>
            <b class="${isActive ? "is-active" : ""}">${gpu.name}${isActive ? " · основная" : ""}</b>
            <span>${[gpu.vendor, gpu.dedicatedMb ? `${fmt(gpu.dedicatedMb / 1024, 1)} ГБ выделено` : "",
              gpu.driver ? `драйвер ${gpu.driver}` : "", index > 0 ? `индекс ${Math.round(index)}` : ""]
              .filter(Boolean).join(" · ")}</span>
          </li>`;
      }).join("");
    }

    const memory = machine.memory;
    setText("#memSummary", window.Sim.describeMemory(memory));
    const tck = memory.configuredMt > 0 ? 2000 / memory.configuredMt : 0;
    const filled = memory.modules.filter((module) => module.populated).length;
    specRows("#memCard", [
      ["Частота", memory.ratedMt > memory.configuredMt + 1
        ? `${fmt(memory.configuredMt, 0)} МТ/с из ${fmt(memory.ratedMt, 0)}`
        : `${fmt(memory.configuredMt, 0)} МТ/с`],
      ["Такт tCK", tck > 0 ? `${fmt(tck, 3)} нс` : ""],
      ["XMP/EXPO", memory.profile ? "включён" : "не включён"],
      ["Слоты", memory.slots ? `${filled} из ${memory.slots}` : ""]
    ]);

    const modulesHost = $("#memModules");
    if (modulesHost) {
      modulesHost.innerHTML = memory.modules.filter((module) => module.populated).map((module) => `
        <li>
          <b>${module.slot} · ${module.sizeGb} ГБ ${module.type}-${Math.round(module.configuredMt)}</b>
          <code>${[module.vendor, module.part, module.voltage ? `${fmt(module.voltage, 2)} В` : ""]
            .filter(Boolean).join(" · ")}</code>
        </li>`).join("");
    }

    const monitorHost = $("#monitorCard");
    if (monitorHost) {
      monitorHost.innerHTML = machine.displays.map((display) => `
        <li>
          <b>${display.name}${display.primary ? " · основной" : ""}</b>
          <span>${display.width}×${display.height}${display.hz ? `, ${display.hz} Гц` : ""}${
            display.scale > 1 ? `, масштаб ${Math.round(display.scale * 100)} %` : ""}</span>
        </li>`).join("");
    }

    const diskHost = $("#diskCard");
    if (diskHost) {
      const disks = machine.disks || [];
      diskHost.innerHTML = disks.map((disk) => `
        <li>
          <b>${disk.name}${disk.driveLetter ? ` · ${disk.driveLetter}` : ""}</b>
          <span>${[disk.interfaceType, disk.mediaType, disk.totalGb ? `${Math.round(disk.totalGb)} ГБ` : "",
            disk.freeGb ? `свободно ${Math.round(disk.freeGb)} ГБ` : "", disk.fileSystem].filter(Boolean).join(" · ")}</span>
        </li>`).join("") || '<li><span>нет данных</span></li>';
    }
  }

  let lastSnapshot = null;
  let systemRendered = false;

  function onData(snap) {
    lastSnapshot = snap;
    updateMetrics(snap);
    updateBars(snap);
    updateCores(snap);
    updateProcesses(snap);
    drawCharts(snap);
    updateSensorTable(snap);
    renderBalance(snap);
    if (!systemRendered) {
      // Паспорт машины статический: его достаточно отрисовать один раз за сессию.
      systemRendered = true;
      renderSystem(snap);
    }
    updateStatus(snap);

    const free = TARGET_FRAME_MS - snap.frameMs;
    $("#budgetNote").textContent = free >= 0
      ? `${fmt(free, 2)} мс свободно до ${fmt(TARGET_FRAME_MS, 2)} мс`
      : `перебор ${fmt(-free, 2)} мс к цели ${fmt(TARGET_FRAME_MS, 2)} мс`;
  }

  function switchView(name) {
    const view = VIEWS[name];
    if (!view) return;

    $$(".rail__item").forEach((item) => item.classList.toggle("is-active", item.dataset.view === name));
    $$(".view").forEach((node) => node.classList.toggle("is-active", node.dataset.view === name));
    $("#viewEyebrow").textContent = view.eyebrow;
    $("#viewTitle").textContent = view.title;

    if (lastSnapshot) {
      requestAnimationFrame(() => {
        drawCharts(lastSnapshot);
        updateSensorTable(lastSnapshot);
      });
    }
  }

  function bindNavigation() {
    $$(".rail__item").forEach((item) => {
      item.addEventListener("click", () => switchView(item.dataset.view));
    });
    $$("[data-view-link]").forEach((link) => {
      link.addEventListener("click", () => switchView(link.dataset.viewLink));
    });

    document.addEventListener("keydown", (event) => {
      if (event.target.closest("input, textarea")) return;
      const order = ["dashboard", "sensors", "bottleneck", "system", "overlay", "settings"];
      const index = Number(event.key) - 1;
      if (index >= 0 && index < order.length) switchView(order[index]);
    });
  }

  function bindPause() {
    const button = $("#pauseBtn");
    button.addEventListener("click", () => {
      const paused = !window.Sim.isPaused();
      window.Sim.setPaused(paused);
      $("#pauseLabel").textContent = paused ? "Продолжить" : "Пауза";
      button.querySelector("use").setAttribute("href", paused ? "#i-play" : "#i-pause");
      $("#liveLabel").textContent = paused ? "ПАУЗА" : "LIVE";
      document.body.classList.toggle("is-paused", paused);
      $("#statusSource").textContent = paused
        ? "Сбор приостановлен"
        : "ETW-сессия активна · кадры DX12";
      toast(paused ? "Опрос остановлен" : "Опрос возобновлён");
    });
  }

  function applyAccent(name) {
    document.documentElement.dataset.accent = name;
    store.write("yawa.accent", name);

    $$(".swatch").forEach((node) => node.classList.toggle("is-active", node.dataset.accent === name));
    $$(".accent-item").forEach((node) => node.classList.toggle("is-active", node.dataset.accent === name));

    if (lastSnapshot) drawCharts(lastSnapshot);
  }

  function bindAccents() {
    $$("[data-accent]").forEach((node) => {
      node.addEventListener("click", () => applyAccent(node.dataset.accent));
    });
  }

  const OSD_PRESETS = {
    minimal: { layout: "line", items: { cpu: false, gpu: false, ram: false, vram: false }, scale: 90 },
    standard: { layout: "plates", items: { cpu: true, gpu: true, ram: true, vram: false }, scale: 100 },
    diagnostics: { layout: "plates", items: { cpu: true, gpu: true, ram: true, vram: true }, scale: 115 }
  };

  function applyPreset(name) {
    const preset = OSD_PRESETS[name];
    if (!preset) return;

    overlayState.layout = preset.layout;
    overlayState.scale = preset.scale;
    // Как в приложении: ручные подстройки относятся к конкретному пресету.
    overlayState.bgOpacity = null;
    overlayState.padding = null;
    overlayState.rowGap = null;
    overlayState.valueColor = null;
    overlayState.labelColor = null;
    overlayState.accentColor = null;
    overlayState.rowColors = { fps: null, cpu: null, gpu: null, ram: null, vram: null };
    overlayState.borderWidth = null;
    overlayState.shadowStrength = 100;
    overlayState.tracking = 100;
    overlayState.uppercase = null;
    overlayState.separators = null;
    overlayState.fpsDecimals = null;
    $$(".swatch").forEach((node) => node.classList.toggle("is-active", node.dataset.color === "-1"));
    $$("#osdLayout button").forEach((node) => node.classList.toggle("is-active", node.dataset.layout === preset.layout));

    for (const [id, field] of Object.entries(OSD_PRIMARY)) {
      const entry = sensorRows.get(id);
      if (entry) entry.sensor.pinned = Boolean(preset.items[field]);
    }
    saveOverlaySensors();
    rebuildOverlaySensors();
    applySensorFilter();

    const scale = $("#osdScale");
    scale.value = String(preset.scale);
    $("#osdScaleValue").textContent = `${preset.scale}%`;
    scale.style.setProperty("--fill", `${((preset.scale - 70) / 90) * 100}%`);

    $$(".preset").forEach((node) => {
      const button = node.querySelector("[data-preset]");
      const tag = node.querySelector(".tag");
      const isActive = button.dataset.preset === name;
      node.classList.toggle("is-active", isActive);
      button.hidden = isActive;
      tag.hidden = !isActive;
    });

    applyOverlay();
    syncOsdControls();
    toast(`Пресет «${name === "minimal" ? "Минимум" : name === "diagnostics" ? "Диагностика" : "Стандарт"}» применён`);
  }

  function applyLabels() {
    for (const [key, label] of Object.entries(overlayState.labels)) {
      const row = $(`[data-osd-item="${key}"]`);
      if (!row) continue;
      const node = row.querySelector(".osd__key");
      if (node) node.textContent = label;
    }
    store.write("yawa.labels", JSON.stringify(overlayState.labels));
  }

  function fitOverlay() {
    const osd = $("#osdPreview");
    const scene = osd.parentElement;
    if (!scene.clientWidth || !scene.clientHeight) return;
    const scale = overlayState.scale / 100;
    const fit = Math.min(1, scene.clientWidth * 0.94 / Math.max(1, osd.scrollWidth * scale),
      scene.clientHeight * 0.9 / Math.max(1, osd.scrollHeight * scale));
    osd.style.setProperty("--osd-fit", fit.toFixed(4));
  }

  function applyOverlay() {
    const osd = $("#osdPreview");
    osd.dataset.pos = overlayState.position;
    osd.dataset.layout = overlayState.layout;
    osd.dataset.style = overlayState.style;
    osd.style.setProperty("--osd-scale", (overlayState.scale / 100).toFixed(2));
    // Прозрачность окна и прозрачность подложки — разные настройки.
    const bgFactor = overlayState.bgOpacity === null ? 1 : overlayState.bgOpacity / 100;
    osd.style.setProperty("--osd-bg", ((overlayState.opacity / 100) * bgFactor).toFixed(3));
    osd.style.setProperty("--osd-text", (overlayState.textScale / 100).toFixed(2));
    // Отступы и интервал строк: null — как в пресете.
    osd.style.padding = overlayState.padding === null
      ? ""
      : `${overlayState.padding}px ${overlayState.padding + 2}px`;
    osd.style.rowGap = overlayState.rowGap === null ? "" : `${overlayState.rowGap}px`;

    // Цвета по элементам: значения и подписи независимо от пресета.
    const colorFor = osdColor;
    const valueColor = colorFor(overlayState.valueColor);
    for (const node of osd.querySelectorAll(".osd__val")) {
      if (valueColor) node.style.setProperty("color", valueColor, "important");
      else node.style.removeProperty("color");
    }
    const labelColor = colorFor(overlayState.labelColor);
    for (const node of osd.querySelectorAll(".osd__key, .osd__unit")) {
      if (labelColor) node.style.setProperty("color", labelColor, "important");
      else node.style.removeProperty("color");
    }
    // Цвет каждой строки: строка оверлея ссылается на свой показатель.
    for (const row of OSD_ROWS) {
      const color = colorFor(overlayState.rowColors[row.key]);
      const value = osd.querySelector(`[data-osd-item="${row.key}"] .osd__val`);
      if (!value) continue;
      if (color) value.style.setProperty("color", color, "important");
      else if (!valueColor) value.style.removeProperty("color");
    }

    // Акцент — рамка и декоративные линии стиля.
    const accent = colorFor(overlayState.accentColor);
    if (accent) osd.style.setProperty("border-color", accent, "important");
    else osd.style.removeProperty("border-color");

    // Толщина рамки, сила тени и разрядка подписей.
    osd.style.setProperty("--osd-border", `${overlayState.borderWidth === null ? 1 : overlayState.borderWidth}px`);
    osd.style.setProperty("--osd-shadow", (overlayState.shadowStrength / 100).toFixed(2));
    osd.classList.toggle("osd--shadow", overlayState.shadowStrength < 100);
    osd.style.setProperty("--osd-tracking", (overlayState.tracking / 100).toFixed(2));

    // Регистр и разделители: null — как в пресете.
    osd.classList.toggle("osd--upper", overlayState.uppercase === 1);
    osd.classList.toggle("osd--separators", overlayState.separators === 1);

    // -1/null — радиус берётся из пресета; число — как в приложении.
    osd.classList.toggle("osd--radius", overlayState.radius !== null);
    if (overlayState.radius === null) osd.style.removeProperty("--osd-radius");
    else osd.style.setProperty("--osd-radius", `${overlayState.radius}px`);
    osd.style.setProperty("--osd-shift-x", `${overlayState.offsetX}px`);
    osd.style.setProperty("--osd-shift-y", `${overlayState.offsetY}px`);
    const fonts = ["inherit", "'Consolas', monospace", "'Bahnschrift SemiCondensed', 'Arial Narrow', sans-serif", "Impact, 'Arial Black', sans-serif"];
    osd.style.setProperty("--osd-font", fonts[overlayState.font] || fonts[0]);
    osd.style.setProperty("--osd-weight", overlayState.bold ? "650" : "inherit");
    osd.style.setProperty("--osd-style", overlayState.italic ? "italic" : "normal");
    osd.style.setProperty("--osd-outline", overlayState.outline > 0
      ? `0 0 ${overlayState.outline}px #000, 0 0 ${overlayState.outline * 2}px #000`
      : "none");

    // Сквозной клик: предпросмотр перестаёт ловить мышь, как настоящее окно поверх игры.
    osd.classList.toggle("osd--click-through", overlayState.clickThrough);
    osd.classList.toggle("no-background", !overlayState.background);
    osd.classList.toggle("no-border", !overlayState.border);
    osd.classList.toggle("no-shadow", !overlayState.shadow);

    fitOverlay();
    const radiusValue = $("#osdRadiusValue");
    if (radiusValue) radiusValue.textContent = `${overlayState.radius === null ? 8 : overlayState.radius} px`;
    const offsetValue = $("#osdOffsetValue");
    if (offsetValue) offsetValue.textContent = `${overlayState.offsetX} / ${overlayState.offsetY} px`;
    const radius = $("#osdRadius");
    if (radius) {
      const shown = overlayState.radius === null ? 8 : overlayState.radius;
      radius.value = String(shown);
      radius.style.setProperty("--fill", `${(shown / 24) * 100}%`);
    }
  }

  // Все органы управления оверлеем показывают значения из overlayState:
  // нужны после сброса пресета и после любой внешней правки состояния.
  function syncOsdControls() {
    const markColors = (id, value) => {
      const host = $(`#${id}`);
      if (!host) return;
      host.querySelectorAll(".swatch").forEach((node) => {
        const index = Number(node.dataset.color);
        node.classList.toggle("is-active", (index < 0 ? null : index) === value);
      });
    };
    markColors("osdValueColor", overlayState.valueColor);
    markColors("osdLabelColor", overlayState.labelColor);
    markColors("osdAccentColor", overlayState.accentColor);

    const rowHost = $("#osdRowColors");
    if (rowHost) {
      OSD_ROWS.forEach((row, index) => {
        const line = rowHost.children[index];
        if (!line) return;
        line.querySelectorAll(".swatch").forEach((node) => {
          const value = Number(node.dataset.color);
          node.classList.toggle("is-active", (value < 0 ? null : value) === overlayState.rowColors[row.key]);
        });
      });
    }

    const setRange = (id, valueId, value, max, text) => {
      const range = $(`#${id}`);
      const label = $(`#${valueId}`);
      if (range) {
        range.value = String(value);
        range.style.setProperty("--fill", `${(value / max) * 100}%`);
      }
      if (label) label.textContent = text;
    };
    const borderWidth = overlayState.borderWidth;
    setRange("osdBorder", "osdBorderValue", borderWidth === null ? 1 : borderWidth, 3,
      borderWidth === null ? "из стиля" : `${borderWidth} px`);
    setRange("osdShadow", "osdShadowValue", overlayState.shadowStrength, 100, `${overlayState.shadowStrength} %`);
    setRange("osdTracking", "osdTrackingValue", overlayState.tracking, 200, `${overlayState.tracking} %`);

    const markModes = (id, attribute, value) => {
      $$(`#${id} button`).forEach((node) => {
        const candidate = Number(node.dataset[attribute]);
        node.classList.toggle("is-active", (candidate < 0 ? null : candidate) === value);
      });
    };
    markModes("osdUppercase", "mode", overlayState.uppercase);
    markModes("osdSeparators", "mode", overlayState.separators);
    markModes("osdDecimals", "decimals", overlayState.fpsDecimals);
  }

  function bindOsdText() {
    const fonts = $$("#osdFont button");
    fonts.forEach((button) => {
      button.addEventListener("click", () => {
        overlayState.font = Number(button.dataset.font);
        fonts.forEach((node) => node.classList.toggle("is-active", node === button));
        applyOverlay();
      });
    });

    $$("[data-style-toggle]").forEach((button) => {
      button.addEventListener("click", () => {
        const key = button.dataset.styleToggle;
        overlayState[key] = !overlayState[key];
        button.classList.toggle("is-active", overlayState[key]);
        applyOverlay();
      });
    });

    const outlines = $$("#osdOutline button");
    outlines.forEach((button) => {
      button.addEventListener("click", () => {
        overlayState.outline = Number(button.dataset.outline);
        outlines.forEach((node) => node.classList.toggle("is-active", node === button));
        applyOverlay();
      });
    });
  }

  // Оверлей в предпросмотре тянется мышью — как Ctrl + мышь по настоящему оверлею.
  function bindOsdDrag() {
    const osd = $("#osdPreview");
    let drag = null;

    osd.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      drag = {
        x: event.clientX,
        y: event.clientY,
        offsetX: overlayState.offsetX,
        offsetY: overlayState.offsetY
      };
      osd.setPointerCapture(event.pointerId);
      osd.classList.add("is-dragging");
      event.preventDefault();
    });

    osd.addEventListener("pointermove", (event) => {
      if (!drag) return;
      overlayState.offsetX = Math.max(-260, Math.min(260, drag.offsetX + (event.clientX - drag.x)));
      overlayState.offsetY = Math.max(-200, Math.min(200, drag.offsetY + (event.clientY - drag.y)));
      applyOverlay();
    });

    const release = (event) => {
      if (!drag) return;
      drag = null;
      osd.classList.remove("is-dragging");
      if (osd.hasPointerCapture(event.pointerId)) osd.releasePointerCapture(event.pointerId);
    };
    osd.addEventListener("pointerup", release);
    osd.addEventListener("pointercancel", release);
  }

  function bindOsdChrome() {
    const chips = $$("#osdChrome button");
    chips.forEach((chip) => {
      chip.classList.toggle("is-active", Boolean(overlayState[chip.dataset.chrome]));
      chip.addEventListener("click", () => {
        overlayState[chip.dataset.chrome] = !overlayState[chip.dataset.chrome];
        chip.classList.toggle("is-active", overlayState[chip.dataset.chrome]);
        applyOverlay();
      });
    });

    const radius = $("#osdRadius");
    radius.addEventListener("input", () => {
      overlayState.radius = Number(radius.value);
      applyOverlay();
    });

    $$("[data-offset]").forEach((button) => {
      button.addEventListener("click", () => {
        const axis = button.dataset.offset;
        const delta = Number(button.dataset.delta);
        const next = (axis === "x" ? overlayState.offsetX : overlayState.offsetY) + delta;
        const clamped = Math.max(-200, Math.min(200, next));
        if (axis === "x") overlayState.offsetX = clamped;
        else overlayState.offsetY = clamped;
        applyOverlay();
      });
    });
  }

  // Горячие клавиши: клик по строке, затем любая клавиша. Esc — отмена, Delete — выключить.
  const VK_NAMES = {
    Space: "Space", Insert: "Insert", Delete: "Delete", Home: "Home", End: "End",
    PageUp: "PgUp", PageDown: "PgDn", PrintScreen: "PrtScr", Pause: "Pause", ScrollLock: "ScrollLock"
  };
  const hotkeys = { overlay: "F10", position: "F11", window: "F12", clickThrough: "F9" };
  let hotkeyEditing = null;

  function bindHotkeys() {
    const list = $("#hotkeyList");
    if (!list) return;

    const render = () => {
      for (const row of list.querySelectorAll(".hotkey")) {
        const key = hotkeys[row.dataset.hotkey];
        const editing = hotkeyEditing === row.dataset.hotkey;
        row.classList.toggle("is-editing", editing);
        row.querySelector(".hotkey__key").textContent = editing ? "нажмите клавишу" : key || "выключено";
      }
    };

    list.addEventListener("click", (event) => {
      const row = event.target.closest(".hotkey");
      if (!row) return;
      hotkeyEditing = row.dataset.hotkey;
      row.focus();
      render();
    });

    list.addEventListener("keydown", (event) => {
      if (!hotkeyEditing) return;
      event.preventDefault();
      event.stopPropagation();

      if (event.key === "Escape") {
        hotkeyEditing = null;
        render();
        return;
      }
      if (event.key === "Delete" || event.key === "Backspace") {
        hotkeys[hotkeyEditing] = "";
        hotkeyEditing = null;
        render();
        toast("Клавиша отключена");
        return;
      }

      let name = "";
      if (/^F([1-9]|1[0-9]|2[0-4])$/.test(event.key)) name = event.key;
      else if (event.key.length === 1 && /[a-z0-9]/i.test(event.key)) name = event.key.toUpperCase();
      else if (VK_NAMES[event.key]) name = VK_NAMES[event.key];
      if (!name) return;

      hotkeys[hotkeyEditing] = name;
      hotkeyEditing = null;
      render();
      toast(`Горячая клавиша: ${name}`);
    });
  }

  // Как в приложении: пресет приносит свои переключатели оформления и свой радиус.
  const STYLE_CHROME = {
    rtss: { background: false, border: false, shadow: false },
    mono: { background: true, border: false, shadow: false }
  };

  function bindOsdStyles() {
    $("#osdStyles").addEventListener("click", (event) => {
      const tile = event.target.closest(".style-tile");
      if (!tile) return;
      overlayState.style = tile.dataset.style;
      const chrome = STYLE_CHROME[tile.dataset.style] || { background: true, border: true, shadow: true };
      overlayState.background = chrome.background;
      overlayState.border = chrome.border;
      overlayState.shadow = chrome.shadow;
      overlayState.radius = null;
      $$("#osdChrome button").forEach((chip) => {
        chip.classList.toggle("is-active", Boolean(overlayState[chip.dataset.chrome]));
      });
      $$("#osdStyles .style-tile").forEach((node) => node.classList.toggle("is-active", node === tile));
      applyOverlay();
    });
  }

  function bindLabels() {
    for (const input of $$(".label-input")) {
      input.addEventListener("input", () => {
        const key = input.dataset.label;
        overlayState.labels[key] = input.value.trim() || DEFAULT_LABELS[key];
        applyLabels();
      });
      input.addEventListener("blur", () => {
        const key = input.dataset.label;
        input.value = overlayState.labels[key];
      });
    }

    $("#resetLabels").addEventListener("click", () => {
      overlayState.labels = { ...DEFAULT_LABELS };
      for (const input of $$(".label-input")) input.value = DEFAULT_LABELS[input.dataset.label];
      applyLabels();
      toast("Подписи сброшены");
    });
  }

  function updateGpuHeader() {
    const gpu = window.Sim.gpuInfo();
    $("#gpuName").textContent = gpu.name;
    $("#gpuMemory").textContent = `${gpu.memoryGb} ГБ`;
    $("#vramTotal").textContent = String(gpu.memoryGb);
    $("#gpuSelect").value = String(gpu.index);
    $(".gpu-picker").hidden = gpu.count < 2;

    const heading = $('.sensor-group[data-group="gpu"]');
    if (heading) heading.textContent = `Видеокарта · ${gpu.name}`;
  }

  function bindGpuPicker() {
    const sync = () => {
      const node = $("#activeGpuName");
      if (node) node.textContent = window.Sim.gpuInfo().name;
    };
    sync();
    window.Sim.subscribe(sync);

    $("#gpuSelect").addEventListener("change", (event) => {
      window.Sim.setGpu(Number(event.target.value));
      updateGpuHeader();
      sync();
      if (lastSnapshot) {
        updateBars(lastSnapshot);
        drawCharts(lastSnapshot);
        updateSensorTable(lastSnapshot);
      }
      toast(`Активный адаптер: ${window.Sim.gpuInfo().name}`);
    });
  }

  function bindOverlay() {
    $("#osdPosition").addEventListener("click", (event) => {
      const button = event.target.closest("[data-pos]");
      if (!button) return;
      overlayState.position = button.dataset.pos;
      $$("#osdPosition button").forEach((node) => node.classList.toggle("is-active", node === button));
      applyOverlay();
    });

    $("#osdLayout").addEventListener("click", (event) => {
      const button = event.target.closest("[data-layout]");
      if (!button) return;
      overlayState.layout = button.dataset.layout;
      $$("#osdLayout button").forEach((node) => node.classList.toggle("is-active", node === button));
      applyOverlay();
    });

    const scale = $("#osdScale");
    scale.addEventListener("input", () => {
      overlayState.scale = Number(scale.value);
      $("#osdScaleValue").textContent = `${overlayState.scale}%`;
      scale.style.setProperty("--fill", `${((overlayState.scale - 70) / 90) * 100}%`);
      applyOverlay();
    });

    const opacity = $("#osdOpacity");
    opacity.addEventListener("input", () => {
      overlayState.opacity = Number(opacity.value);
      $("#osdOpacityValue").textContent = `${overlayState.opacity}%`;
      opacity.style.setProperty("--fill", `${overlayState.opacity}%`);
      applyOverlay();
    });

    const textScale = $("#osdTextScale");
    textScale.addEventListener("input", () => {
      overlayState.textScale = Number(textScale.value);
      $("#osdTextScaleValue").textContent = `${overlayState.textScale}%`;
      textScale.style.setProperty("--fill", `${((overlayState.textScale - 85) / 45) * 100}%`);
      applyOverlay();
    });

    // Ползунки «как в стиле»: первое касание включает ручное значение.
    const bindAutoRange = (id, valueId, key, max, suffix) => {
      const range = $(`#${id}`);
      const label = $(`#${valueId}`);
      const render = () => {
        const value = overlayState[key];
        range.value = String(value === null ? 0 : value);
        range.style.setProperty("--fill", value === null ? "0%" : `${(value / max) * 100}%`);
        label.textContent = value === null ? "из стиля" : `${value}${suffix}`;
      };
      range.addEventListener("input", () => {
        overlayState[key] = Number(range.value);
        render();
        applyOverlay();
      });
      render();
    };
    bindAutoRange("osdBg", "osdBgValue", "bgOpacity", 100, " %");
    bindAutoRange("osdPad", "osdPadValue", "padding", 28, " px");
    bindAutoRange("osdGap", "osdGapValue", "rowGap", 14, " px");

    const bindColors = (id, key) => {
      const container = $(`#${id}`);
      const chips = [...container.querySelectorAll(".swatch")];
      chips.forEach((chip) => {
        if (chip.dataset.hex) chip.style.background = chip.dataset.hex;
        chip.addEventListener("click", () => {
          const value = Number(chip.dataset.color);
          overlayState[key] = value < 0 ? null : value;
          chips.forEach((node) => node.classList.toggle("is-active", node === chip));
          applyOverlay();
        });
      });
    };
    const bindRange = (id, valueId, key, max, suffix) => {
      const range = $(`#${id}`);
      const label = $(`#${valueId}`);
      if (!range || !label) return;
      const render = () => {
        const value = overlayState[key];
        range.value = String(value);
        range.style.setProperty("--fill", `${(value / max) * 100}%`);
        label.textContent = `${value}${suffix}`;
      };
      range.addEventListener("input", () => {
        overlayState[key] = Number(range.value);
        render();
        applyOverlay();
      });
      render();
    };
    bindColors("osdValueColor", "valueColor");
    bindColors("osdLabelColor", "labelColor");
    bindColors("osdAccentColor", "accentColor");

    // Цвета строк: пять компактных палитр, как в приложении.
    const rowHost = $("#osdRowColors");
    if (rowHost) {
      for (const row of OSD_ROWS) {
        const line = document.createElement("div");
        line.className = "swatch-row";
        line.innerHTML = `<span>${row.title}</span><div class="swatches"></div>`;
        const host = line.querySelector(".swatches");
        for (let index = -1; index < OSD_PALETTE.length - 1; index += 1) {
          const chip = document.createElement("button");
          chip.type = "button";
          chip.className = `swatch${index === -1 ? " is-active" : ""}`;
          chip.dataset.color = String(index);
          if (index < 0) {
            chip.textContent = "а";
            chip.setAttribute("aria-label", "Как в стиле");
          } else {
            chip.dataset.hex = OSD_PALETTE[index + 1];
            chip.style.background = OSD_PALETTE[index + 1];
            chip.setAttribute("aria-label", `Цвет ${index + 1}`);
          }
          chip.addEventListener("click", () => {
            overlayState.rowColors[row.key] = index < 0 ? null : index;
            host.querySelectorAll(".swatch").forEach((node) => node.classList.toggle("is-active", node === chip));
            applyOverlay();
          });
          host.appendChild(chip);
        }
        rowHost.appendChild(line);
      }
    }

    // Толщина рамки: «из стиля» — как в пресете, иначе 0…3 px.
    const borderRange = $("#osdBorder");
    const borderLabel = $("#osdBorderValue");
    const renderBorder = () => {
      const value = overlayState.borderWidth;
      borderRange.value = String(value === null ? 1 : value);
      borderRange.style.setProperty("--fill", `${((value === null ? 1 : value) / 3) * 100}%`);
      borderLabel.textContent = value === null ? "из стиля" : `${value} px`;
    };
    borderRange.addEventListener("input", () => {
      overlayState.borderWidth = Number(borderRange.value);
      renderBorder();
      applyOverlay();
    });
    renderBorder();

    bindRange("osdShadow", "osdShadowValue", "shadowStrength", 100, " %");
    bindRange("osdTracking", "osdTrackingValue", "tracking", 200, " %");

    // Тройной выбор и знаки после запятой — те же чипы, что в приложении.
    const bindMode = (id, key, attribute) => {
      const chips = [...$$(`#${id} button`)];
      chips.forEach((chip) => {
        chip.addEventListener("click", () => {
          const value = Number(chip.dataset[attribute]);
          overlayState[key] = value < 0 ? null : value;
          chips.forEach((node) => node.classList.toggle("is-active", node === chip));
          applyOverlay();
        });
      });
    };
    bindMode("osdUppercase", "uppercase", "mode");
    bindMode("osdSeparators", "separators", "mode");
    bindMode("osdDecimals", "fpsDecimals", "decimals");

    $$("[data-osd-toggle]").forEach((toggle) => {
      toggle.addEventListener("change", () => {
        const id = Object.keys(OSD_PRIMARY).find((key) => OSD_PRIMARY[key] === toggle.dataset.osdToggle);
        const entry = sensorRows.get(id);
        if (entry) entry.sensor.pinned = toggle.checked;
        saveOverlaySensors();
        rebuildOverlaySensors();
        applySensorFilter();
      });
    });

    $$("[data-preset]").forEach((button) => {
      button.addEventListener("click", () => applyPreset(button.dataset.preset));
    });

    scale.style.setProperty("--fill", `${((overlayState.scale - 70) / 90) * 100}%`);
    opacity.style.setProperty("--fill", `${overlayState.opacity}%`);
    applyOverlay();
    syncOsdControls();
  }

  function applyInterval(ms) {
    window.Sim.setIntervalMs(ms);
    const label = `${ms} мс`;
    $("#intervalChip").textContent = `Опрос ${label}`;
    $("#statusInterval").textContent = label;
    $$("#intervalSelect button").forEach((node) => {
      node.classList.toggle("is-active", Number(node.dataset.interval) === ms);
    });
    store.write("yawa.interval", String(ms));
    if (lastSnapshot) drawCharts(lastSnapshot);
  }

  function bindSettings() {
    $("#intervalSelect").addEventListener("click", (event) => {
      const button = event.target.closest("[data-interval]");
      if (button) applyInterval(Number(button.dataset.interval));
    });

    const smooth = $("#smoothRange");
    smooth.style.setProperty("--fill", `${smooth.value}%`);
    smooth.addEventListener("input", () => {
      window.Sim.setSmoothing(Number(smooth.value));
      $("#smoothValue").textContent = `${smooth.value}%`;
      smooth.style.setProperty("--fill", `${smooth.value}%`);
    });

    $("#densitySelect").addEventListener("click", (event) => {
      const button = event.target.closest("[data-density]");
      if (!button) return;
      const compact = button.dataset.density === "compact";
      document.body.classList.toggle("is-compact", compact);
      $$("#densitySelect button").forEach((node) => node.classList.toggle("is-active", node === button));
      store.write("yawa.density", button.dataset.density);
      window.dispatchEvent(new Event("resize"));
    });

    $("#etwToggle").addEventListener("change", (event) => {
      $("#statusSource").textContent = event.target.checked
        ? "ETW-сессия активна · кадры DX12"
        : "ETW выключен · только статистика DXGI";
    });

    $("#idleToggle").addEventListener("change", (event) => {
      toast(event.target.checked ? "Опрос вне игры приостановлен" : "Опрос идёт постоянно");
    });

    $("#animToggle").addEventListener("change", (event) => {
      document.body.classList.toggle("no-motion", !event.target.checked);
    });

    $$(".actions .btn").forEach((button) => {
      button.addEventListener("click", () => {
        toast("Действие появится в собранном приложении");
      });
    });
  }

  function bindSensors() {
    $("#sensorSearch").addEventListener("input", (event) => {
      sensorFilter.query = event.target.value;
      applySensorFilter();
    });

    $("#sensorFilters").addEventListener("click", (event) => {
      const button = event.target.closest("[data-group]");
      if (!button) return;
      sensorFilter.group = button.dataset.group;
      $$("#sensorFilters .filter").forEach((node) => node.classList.toggle("is-active", node === button));
      applySensorFilter();
    });

    $("#onlyPinned").addEventListener("change", (event) => {
      sensorFilter.pinnedOnly = event.target.checked;
      applySensorFilter();
    });
  }

  function bindPerfLegend() {
    $("#perfLegend").addEventListener("click", (event) => {
      const button = event.target.closest("[data-series]");
      if (!button) return;
      const series = button.dataset.series;
      perfState[series] = !perfState[series];
      button.classList.toggle("is-on", perfState[series]);
      if (lastSnapshot) drawCharts(lastSnapshot);
    });
  }

  function restorePreferences() {
    const accent = store.read("yawa.accent");
    if (accent) applyAccent(accent);

    if (store.read("yawa.density") === "compact") {
      document.body.classList.add("is-compact");
      $$("#densitySelect button").forEach((node) => {
        node.classList.toggle("is-active", node.dataset.density === "compact");
      });
    }

    const labels = store.read("yawa.labels");
    if (labels) {
      try {
        overlayState.labels = { ...DEFAULT_LABELS, ...JSON.parse(labels) };
      } catch (error) {
        overlayState.labels = { ...DEFAULT_LABELS };
      }
      for (const input of $$(".label-input")) {
        if (overlayState.labels[input.dataset.label]) input.value = overlayState.labels[input.dataset.label];
      }
    }

    const interval = Number(store.read("yawa.interval"));
    if (interval) applyInterval(interval);
  }

  buildCores();
  processRows = buildProcesses();
  buildSensorTable();
  applySensorFilter();
  bindNavigation();

  // Прямые ссылки для скриншотов: ?view=sensors открывает нужный экран,
  // ?nofx=1 выключает анимации. Это делается сразу после навигации: если
  // дальше что-то не поднимется (например, canvas в headless-браузере),
  // снимок всё равно попадёт на нужный экран, а не на «Обзор».
  const params = new URLSearchParams(window.location.search);
  const deepView = params.get("view");
  if (deepView && VIEWS[deepView]) {
    switchView(deepView);
  }
  if (params.get("nofx") === "1") {
    document.body.classList.add("no-fx");
  }

  bindPause();
  bindAccents();
  bindOverlay();
  bindOsdStyles();
  bindOsdChrome();
  bindOsdText();
  bindOsdDrag();
  bindLabels();
  bindHotkeys();
  bindGpuPicker();
  bindSettings();
  bindSensors();
  bindPerfLegend();
  restorePreferences();

  new ResizeObserver(fitOverlay).observe($("#osdPreview").parentElement);
  window.Charts.observe($("#frameChart"));
  window.Charts.observe($("#perfChart"));
  $$(".spark[data-series]").forEach((canvas) => window.Charts.observe(canvas));

  window.Sim.subscribe(onData);

  if (params.get("shot") === "1") {
    // Один статичный кадр: таймер симулятора останавливаем, CSS-переходы уже выключены.
    window.Sim.stop();
  }

})();
