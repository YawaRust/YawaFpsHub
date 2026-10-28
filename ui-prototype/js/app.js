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
    anchorMode: "corner",
    originX: 0,
    originY: 0,
    followGameMonitor: false,
    sparkline: false,
    sparklineMetric: "fps",
    layout: "plates",
    style: "frosted",
    recipe: "frosted",
    surface: null,
    scale: 100,
    opacity: 72,
    menuOpacity: 0,
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
    metricRgba: {},
    metricOrder: [],
    customRgba: { value: null, label: null, accent: null,
      fps: null, cpu: null, gpu: null, ram: null, vram: null },
    borderWidth: null,
    shadowStrength: 100,
    tracking: 100,
    uppercase: null,
    separators: null,
    fpsDecimals: null,
    clickThrough: false,
    labels: { ...DEFAULT_LABELS },
    rowPositions: {}
  };

  const overlayHistory = { undo: [], redo: [], committed: null, gesture: false, restoring: false };

  function recordOverlayHistory() {
    if (overlayHistory.gesture || overlayHistory.restoring) return;
    const next = JSON.stringify(overlayState);
    if (overlayHistory.committed === null) {
      overlayHistory.committed = next;
    } else if (next !== overlayHistory.committed) {
      overlayHistory.undo.push(overlayHistory.committed);
      if (overlayHistory.undo.length > 30) overlayHistory.undo.shift();
      overlayHistory.redo = [];
      overlayHistory.committed = next;
    }
    $(`[data-history="undo"]`).disabled = !overlayHistory.undo.length;
    $(`[data-history="redo"]`).disabled = !overlayHistory.redo.length;
  }

  function restoreOverlayHistory(direction) {
    const source = overlayHistory[direction];
    if (!source.length) return;
    const other = direction === "undo" ? "redo" : "undo";
    overlayHistory[other].push(JSON.stringify(overlayState));
    const snapshot = source.pop();
    const previousGraph = overlayState.sparklineMetric;
    Object.assign(overlayState, JSON.parse(snapshot));
    if (previousGraph !== overlayState.sparklineMetric) {
      sparklineHistory.length = 0;
      $("#osdSparklinePoints").setAttribute("points", "");
    }
    overlayHistory.committed = snapshot;
    overlayHistory.restoring = true;
    applyOverlay();
    overlayHistory.restoring = false;
    syncOsdControls();
    $$("#osdPosition button").forEach((button) => button.classList.toggle("is-active",
      button.dataset.pos === (overlayState.anchorMode === "free" ? "free" : overlayState.position)));
    $$("#osdLayout button").forEach((button) => button.classList.toggle("is-active",
      button.dataset.layout === overlayState.layout));
    $$("#osdStyles .style-tile").forEach((tile) => tile.classList.toggle("is-active",
      tile.dataset.recipe === overlayState.recipe));
    $$("#osdSurface button").forEach((button) => button.classList.toggle("is-active",
      button.dataset.surface === overlayState.surface));
    $$("#osdFont button").forEach((button) => button.classList.toggle("is-active",
      Number(button.dataset.font) === overlayState.font));
    $$("#osdChrome button").forEach((button) => button.classList.toggle("is-active",
      Boolean(overlayState[button.dataset.chrome])));
    $$("[data-style-toggle]").forEach((button) => button.classList.toggle("is-active",
      Boolean(overlayState[button.dataset.styleToggle])));
    $$("#osdOutline button").forEach((button) => button.classList.toggle("is-active",
      Number(button.dataset.outline) === overlayState.outline));
    $$(".label-input").forEach((input) => { input.value = overlayState.labels[input.dataset.label] || ""; });
    applyLabels();
    $("#osdFollowGame").checked = overlayState.followGameMonitor;
    $("#osdScale").value = overlayState.scale;
    $("#osdOpacity").value = overlayState.opacity;
    $("#osdScaleValue").textContent = `${overlayState.scale}%`;
    $("#osdOpacityValue").textContent = `${overlayState.opacity}%`;
    $(`[data-history="undo"]`).disabled = !overlayHistory.undo.length;
    $(`[data-history="redo"]`).disabled = !overlayHistory.redo.length;
  }

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
    for (const [id, fraction] of [["cpu", snap.cpu], ["gpu", snap.gpu]]) {
      const row = document.querySelector(`#osdPreview [data-osd-item="${id}"]`);
      if (row) row.style.setProperty("--gauge-fill", `${Math.max(0, Math.min(100, fraction))}%`);
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
  let selectedLayer = "fps";
  let colorTarget = "metric";
  const currentRgba = () => colorTarget === "metric"
    ? overlayState.metricRgba[selectedLayer] : overlayState.customRgba[colorTarget];
  const setRgba = (value) => {
    if (colorTarget === "metric") {
      if (value) overlayState.metricRgba[selectedLayer] = value;
      else delete overlayState.metricRgba[selectedLayer];
    } else overlayState.customRgba[colorTarget] = value;
  };
  const displayedLayers = new Set(["fps", "fpsLow1", "cpu", "cpuTemp", "gpu", "gpuTemp", "ramUsed", "vramUsed"]);

  function selectOverlayLayer(id) {
    selectedLayer = id;
    const hex = currentRgba() || "#FFFFFFFF";
    $("#osdLayerName").textContent = colorTarget === "metric"
      ? sensorRows.get(id)?.sensor.label || id : $("#osdColorTarget").selectedOptions[0].textContent;
    $("#osdLayerHex").value = hex;
    $("#osdLayerPicker").value = hex.slice(0, 7);
    $("#osdLayerAlpha").value = String(Math.round(parseInt(hex.slice(7, 9), 16) / 255 * 100));
    $("#osdLayerAlphaValue").textContent = `${$("#osdLayerAlpha").value}%`;
    $$("#osdLayers .workshop-layer__name").forEach((button) => {
      button.classList.toggle("is-active", button.dataset.layer === id);
    });
    $$("#osdPreview .osd__row").forEach((row) => {
      row.classList.toggle("is-selected", row.querySelector(`[data-metric="${id}"]`) !== null);
    });
  }

  function renderOverlayLayers() {
    const host = $("#osdLayers");
    host.replaceChildren();
    for (const [id, entry] of sensorRows) {
      if (entry.sensor.pinned) displayedLayers.add(id);
      if (!displayedLayers.has(id)) continue;
      const row = document.createElement("div");
      row.className = "workshop-layer";
      const name = document.createElement("button");
      name.type = "button";
      name.className = "workshop-layer__name";
      name.dataset.layer = id;
      name.textContent = entry.sensor.label;
      name.addEventListener("click", () => selectOverlayLayer(id));
      const eye = document.createElement("button");
      eye.type = "button";
      eye.className = `workshop-layer__eye${entry.sensor.pinned ? " is-on" : ""}`;
      eye.textContent = entry.sensor.pinned ? "◉" : "○";
      eye.setAttribute("aria-label", `${entry.sensor.pinned ? "Скрыть" : "Показать"} ${entry.sensor.label}`);
      eye.addEventListener("click", () => {
        entry.sensor.pinned = !entry.sensor.pinned;
        saveOverlaySensors();
        rebuildOverlaySensors();
        applySensorFilter();
      });
      row.append(name, eye);
      host.appendChild(row);
    }
    selectOverlayLayer(selectedLayer);
  }

  function bindLayerColor() {
    const hexInput = $("#osdLayerHex");
    $("#osdColorTarget").addEventListener("change", (event) => {
      colorTarget = event.target.value;
      selectOverlayLayer(selectedLayer);
    });
    const picker = $("#osdLayerPicker");
    const alpha = $("#osdLayerAlpha");
    const apply = (rgb, percent) => {
      const opacity = Math.round(Number(percent) * 255 / 100).toString(16).padStart(2, "0").toUpperCase();
      setRgba(`${rgb.toUpperCase()}${opacity}`);
      selectOverlayLayer(selectedLayer);
      applyOverlay();
    };
    hexInput.addEventListener("change", () => {
      const value = hexInput.value.trim();
      if (!/^#[0-9a-fA-F]{8}$/.test(value)) {
        selectOverlayLayer(selectedLayer);
        return;
      }
      setRgba(value.toUpperCase());
      selectOverlayLayer(selectedLayer);
      applyOverlay();
    });
    picker.addEventListener("input", () => apply(picker.value, alpha.value));
    alpha.addEventListener("input", () => apply(picker.value, alpha.value));
    $("#osdLayerAuto").addEventListener("click", () => {
      setRgba(null);
      selectOverlayLayer(selectedLayer);
      applyOverlay();
    });
    const sampleButton = $("#osdLayerSample");
    let sampling = false;
    sampleButton.addEventListener("click", () => {
      sampling = !sampling;
      sampleButton.classList.toggle("is-active", sampling);
      if (sampling) toast("Нажмите на текст или плашку предпросмотра. Градиенты недоступны.");
    });
    $("#gameScene").addEventListener("pointerdown", (event) => {
      if (!sampling) return;
      sampling = false;
      sampleButton.classList.remove("is-active");
      event.preventDefault();
      event.stopImmediatePropagation();
      const node = event.target.closest("#osdPreview .osd__val, #osdPreview .osd__key, #osdPreview .osd__unit, #osdPreview");
      if (!node) {
        toast("Здесь цвет задан градиентом; выберите текст или плашку.");
        return;
      }
      const style = getComputedStyle(node);
      const source = node.id === "osdPreview" ? style.backgroundColor : style.color;
      const channels = source.match(/[\d.]+/g)?.map(Number);
      if (!channels || channels.length < 3 || (channels.length > 3 && channels[3] === 0)) {
        toast("У этого элемента нет сплошного цвета.");
        return;
      }
      const rgb = channels.slice(0, 3).map((channel) => Math.round(channel).toString(16).padStart(2, "0")).join("");
      const opacity = Math.round((channels[3] ?? 1) * 255).toString(16).padStart(2, "0");
      setRgba(`#${rgb}${opacity}`.toUpperCase());
      selectOverlayLayer(selectedLayer);
      applyOverlay();
    }, true);
  }

  function saveOverlaySensors() {
    const selected = Object.fromEntries([...sensorRows].map(([id, entry]) => [id, Boolean(entry.sensor.pinned)]));
    store.write("yawa.overlaySensors", JSON.stringify(selected));
  }

  function rebuildOverlaySensors() {
    const host = $("#osdPreview");
    host.replaceChildren();
    const sensors = [...sensorRows.values()].map((entry) => entry.sensor);
    const primary = [...overlayState.metricOrder, ...Object.keys(OSD_PRIMARY)];
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
    renderOverlayLayers();
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
        <span class="sensor-row__value"><span class="sensor-row__reading">—</span><button class="sensor-row__access" type="button" hidden>Включить</button></span>
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

      row.querySelector(".sensor-row__access").addEventListener("click", () => {
        toast("В приложении доступ к датчикам запрашивается через стандартное подтверждение Windows");
      });
      host.appendChild(row);
      sensorRows.set(sensor.id, {
        row,
        heading: null,
        value: row.querySelector(".sensor-row__reading"),
        access: row.querySelector(".sensor-row__access"),
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
    // Только демонстрационный параметр: обычные показания не считаются «неподдерживаемыми».
    const demo = new URLSearchParams(location.search).get("sensor_state");
    for (const entry of sensorRows.values()) {
      const { sensor } = entry;
      if (entry.row.hidden) continue;
      const reason = sensor.id === "cpuTemp" && ["access", "unsupported", "unknown"].includes(demo)
        ? demo : null;
      entry.access.hidden = reason !== "access";
      entry.row.dataset.reason = reason || "";
      if (reason) {
        entry.value.textContent = reason === "access" ? "нужен доступ"
          : reason === "unsupported" ? "не поддерживается" : "нет данных";
        entry.min.textContent = entry.avg.textContent = entry.max.textContent = "—";
        continue;
      }
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
        note: "XMP/EXPO из SMBIOS не определяется",
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

  const diagnosisCard = { image: null, masks: [], draft: null };
  const diagnosisFrame = { x: 48, y: 138, width: 1080, height: 740 };

  function drawDiagnosisCard() {
    const canvas = $("#diagnosisCanvas");
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    ctx.fillStyle = "#101820";
    ctx.fillRect(0, 0, 1920, 1080);
    ctx.fillStyle = "#e8f2f3";
    ctx.font = "bold 39px sans-serif";
    ctx.fillText("YawaFpsHub · диагностическая карточка", 48, 71);
    ctx.font = "bold 24px sans-serif";
    ctx.fillStyle = "#f8bb70";
    ctx.fillText("ПРОТОТИП · НЕ РЕАЛЬНЫЙ ЗАМЕР", 49, 113);
    const box = diagnosisFrame;
    ctx.fillStyle = "#17252b";
    ctx.fillRect(box.x, box.y, box.width, box.height);
    const image = diagnosisCard.image;
    if (image) {
      const zoom = Number($("#diagnosisZoom").value) / 100;
      const ratio = Math.max(box.width / image.width, box.height / image.height) * zoom;
      const cropWidth = box.width / ratio;
      const cropHeight = box.height / ratio;
      const left = (image.width - cropWidth) * Number($("#diagnosisPanX").value) / 100;
      const top = (image.height - cropHeight) * Number($("#diagnosisPanY").value) / 100;
      ctx.drawImage(image, left, top, cropWidth, cropHeight, box.x, box.y, box.width, box.height);
    } else {
      ctx.fillStyle = "#96a9b1";
      ctx.font = "27px sans-serif";
      ctx.fillText("Кадр не добавлен · данных нет", box.x + 90, box.y + 340);
    }
    ctx.fillStyle = "#111b21";
    for (const mask of [...diagnosisCard.masks, diagnosisCard.draft].filter(Boolean)) {
      ctx.fillRect(mask.x, mask.y, mask.width, mask.height);
    }
    ctx.fillStyle = "#18282e";
    ctx.fillRect(1160, 138, 712, 740);
    ctx.fillStyle = "#5ef29b";
    ctx.font = "bold 24px sans-serif";
    ctx.fillText($("#diagnosisSource").textContent, 1190, 198);

    function wrap(text, x, y, width, step, limit) {
      let line = "";
      let count = 0;
      for (const word of text.split(/\s+/)) {
        const next = line ? `${line} ${word}` : word;
        if (ctx.measureText(next).width > width && line) {
          ctx.fillText(line, x, y);
          y += step;
          if (++count === limit) return y;
          line = word;
        } else {
          line = next;
        }
      }
      if (line) ctx.fillText(line, x, y);
      return y + step;
    }
    ctx.fillStyle = "#f2f8f8";
    ctx.font = "bold 37px sans-serif";
    wrap($("#diagnosisTitle").textContent, 1190, 271, 640, 49, 2);
    ctx.fillStyle = "#b8cbd0";
    ctx.font = "27px sans-serif";
    wrap($("#diagnosisEvidence").textContent, 1190, 411, 640, 38, 5);
    let y = 680;
    for (const item of $$("#diagnosisActions li").slice(0, 3)) {
      y = wrap(`• ${item.textContent}`, 1190, y, 640, 36, 2);
    }
    ctx.fillStyle = "#96a9b1";
    ctx.font = "23px sans-serif";
    ctx.fillText("Нет проверенных сведений о системе, температуре или сессии", 49, 947);
    ctx.fillText("Картинка добавлена вручную; приложение не проверяло, что это кадр игры", 49, 994);
  }

  function bindDiagnosisWorkshop() {
    const canvas = $("#diagnosisCanvas");
    const picker = $("#diagnosisFile");
    for (const selector of ["#diagnosisZoom", "#diagnosisPanX", "#diagnosisPanY"]) {
      $(selector).addEventListener("input", drawDiagnosisCard);
    }
    picker.addEventListener("change", () => {
      const file = picker.files?.[0];
      picker.value = "";
      if (!file) return;
      if (!/^image\/(png|jpeg|webp)$/.test(file.type) || file.size > 16 * 1024 * 1024) {
        toast("Нужен PNG, JPEG или WebP до 16 МиБ");
        return;
      }
      const url = URL.createObjectURL(file);
      const image = new Image();
      image.onload = () => {
        URL.revokeObjectURL(url);
        if (image.width * image.height > 16000000) {
          toast("Слишком большое изображение");
          return;
        }
        diagnosisCard.image = image;
        diagnosisCard.masks = [];
        drawDiagnosisCard();
      };
      image.onerror = () => { URL.revokeObjectURL(url); toast("Не удалось открыть изображение"); };
      image.src = url;
    });
    $("#diagnosisClear").addEventListener("click", () => {
      diagnosisCard.image = null;
      diagnosisCard.masks = [];
      diagnosisCard.draft = null;
      drawDiagnosisCard();
    });
    const position = (event) => {
      const bounds = canvas.getBoundingClientRect();
      const x = (event.clientX - bounds.left) * canvas.width / bounds.width;
      const y = (event.clientY - bounds.top) * canvas.height / bounds.height;
      const box = diagnosisFrame;
      return {
        x: Math.max(box.x, Math.min(box.x + box.width, x)),
        y: Math.max(box.y, Math.min(box.y + box.height, y))
      };
    };
    let start = null;
    canvas.addEventListener("pointerdown", (event) => {
      if (!diagnosisCard.image) return;
      const point = position(event);
      const box = diagnosisFrame;
      const bounds = canvas.getBoundingClientRect();
      const x = (event.clientX - bounds.left) * canvas.width / bounds.width;
      const y = (event.clientY - bounds.top) * canvas.height / bounds.height;
      if (x < box.x || x > box.x + box.width || y < box.y || y > box.y + box.height) return;
      start = point;
      canvas.setPointerCapture(event.pointerId);
    });
    canvas.addEventListener("pointermove", (event) => {
      if (!start) return;
      const end = position(event);
      diagnosisCard.draft = { x: Math.min(start.x, end.x), y: Math.min(start.y, end.y),
        width: Math.abs(end.x - start.x), height: Math.abs(end.y - start.y) };
      drawDiagnosisCard();
    });
    const finish = (event) => {
      if (!start) return;
      if (event.type === "pointerup" && diagnosisCard.draft
        && diagnosisCard.draft.width > 4 && diagnosisCard.draft.height > 4) {
        diagnosisCard.masks.push(diagnosisCard.draft);
      }
      start = null;
      diagnosisCard.draft = null;
      drawDiagnosisCard();
    };
    canvas.addEventListener("pointerup", finish);
    canvas.addEventListener("pointercancel", finish);
    $("#diagnosisSave").addEventListener("click", () => {
      drawDiagnosisCard();
      canvas.toBlob((blob) => {
        if (!blob) { toast("PNG не создан"); return; }
        const url = URL.createObjectURL(blob);
        const link = document.createElement("a");
        link.href = url;
        link.download = "yawa-demo-card.png";
        link.click();
        setTimeout(() => URL.revokeObjectURL(url), 1000);
      }, "image/png");
    });
    drawDiagnosisCard();
  }

  function bindDiagnosisPreview() {
    const source = $("#diagnosisSource");
    const title = $("#diagnosisTitle");
    const evidence = $("#diagnosisEvidence");
    const actions = $("#diagnosisActions");
    $("#diagnosisDemo").addEventListener("click", () => {
      source.textContent = "УСЛОВНЫЙ ПРИМЕР · НЕ ЗАМЕР";
      title.textContent = "Пример: упор в видеокарту";
      evidence.textContent = "Если GPU игры 99 %, CPU игры 42 % и кадр выходит за бюджет, можно предположить упор в GPU. Здесь числа вымышлены.";
      actions.replaceChildren();
      for (const text of ["Проверить настройки графики и апскейл.", "Сравнить время кадра до и после изменения."]) {
        const item = document.createElement("li");
        item.textContent = text;
        actions.appendChild(item);
      }
      drawDiagnosisCard();
    });
    $("#diagnosisUnknown").addEventListener("click", () => {
      source.textContent = "НЕТ ДАННЫХ";
      title.textContent = "Недостаточно данных для вывода";
      evidence.textContent = "Нужны кадры и показатели процесса активной игры; общая загрузка ПК не подходит.";
      actions.replaceChildren();
      const item = document.createElement("li");
      item.textContent = "Запустите игру и проверьте сбор кадров.";
      actions.appendChild(item);
      drawDiagnosisCard();
    });
    bindDiagnosisWorkshop();
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
      ["XMP/EXPO", "не определяется из SMBIOS"],
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
          <span title="NVMe SMART: износ — оценка контроллера; объём записи приблизителен. Нет данных не означает нулевой износ.">NVMe SMART: нет данных для этого диска</span>
        </li>`).join("") || '<li><span>нет данных</span></li>';
    }
  }

  let lastSnapshot = null;
  let systemRendered = false;

  const sparklineHistory = [];
  function onData(snap) {
    lastSnapshot = snap;
    const reading = snap.sensors.get(overlayState.sparklineMetric)?.value;
    if (overlayState.sparkline && Number.isFinite(reading) && reading >= 0) {
      sparklineHistory.push(reading);
      if (sparklineHistory.length > 32) sparklineHistory.shift();
      const low = Math.min(...sparklineHistory);
      const span = Math.max(1, Math.max(...sparklineHistory) - low);
      $("#osdSparklinePoints").setAttribute("points", sparklineHistory.map((value, index) =>
        `${index * 160 / Math.max(1, sparklineHistory.length - 1)},${22 - 16 * (value - low) / span}`).join(" "));
    } else if (overlayState.sparkline) {
      sparklineHistory.length = 0;
      $("#osdSparklinePoints").setAttribute("points", "");
    }
    $("#osdPreview").classList.toggle("is-sparkline", overlayState.sparkline && sparklineHistory.length >= 2);
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
    if ($("[data-view=overlay].is-active") && name !== "overlay") {
      overlayHistory.undo = [];
      overlayHistory.redo = [];
      overlayHistory.committed = JSON.stringify(overlayState);
      recordOverlayHistory();
    }

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

  function updateFrameStatus() {
    const paused = window.Sim.isPaused();
    const enabled = $("#etwToggle").checked;
    $("#sourceChipText").textContent = paused ? "Опрос на паузе" : enabled ? "Кадры доступны" : "Кадры выключены";
    $("#statusSource").textContent = paused ? "Опрос приостановлен"
      : enabled ? "Кадры доступны · сбор идёт" : "Сбор кадров выключен";
    const source = $(".sources .source__state");
    source.textContent = enabled ? "работает" : "выключено";
    source.classList.toggle("source__state--ok", enabled);
    source.classList.toggle("source__state--off", !enabled);
    $("#aboutFramesState").textContent = enabled ? "работает" : "выключено";
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
      updateFrameStatus();
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
    overlayState.metricRgba = {};
    for (const key of Object.keys(overlayState.customRgba)) overlayState.customRgba[key] = null;
    selectOverlayLayer(selectedLayer);
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
    recordOverlayHistory();
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
    osd.dataset.anchor = overlayState.anchorMode;
    osd.style.setProperty("--osd-origin-x", `${overlayState.originX}px`);
    osd.style.setProperty("--osd-origin-y", `${overlayState.originY}px`);
    osd.dataset.layout = overlayState.layout;
    osd.classList.toggle("is-sparkline", overlayState.sparkline && sparklineHistory.length >= 2);
    osd.style.setProperty("--menu-opacity", (overlayState.menuOpacity / 100).toFixed(2));
    $("#osdMenuOpacity").value = String(overlayState.menuOpacity);
    $("#osdMenuOpacityValue").textContent = `${overlayState.menuOpacity}%`;
    $("#osdMenuScene").textContent = osd.classList.contains("is-menu-scene")
      ? "Вернуть оверлей · Ctrl+F11"
      : `Меню: ${overlayState.menuOpacity === 0 ? "скрыть" : "приглушить"} оверлей · Ctrl+F11`;
    const graphNames = { fps: "FPS", cpu: "CPU", gpu: "GPU", frameMs: "кадр" };
    const graphName = graphNames[overlayState.sparklineMetric] || "FPS";
    $("#osdSparkline").textContent = overlayState.sparkline ? `График: ${graphName}` : "График: выкл";
    $("#osdSparklinePoints").closest("svg").setAttribute("aria-label", `История: ${graphName}`);
    osd.dataset.style = overlayState.style;
    osd.dataset.surface = overlayState.surface || "";
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
    const valueColor = overlayState.customRgba.value || colorFor(overlayState.valueColor);
    for (const node of osd.querySelectorAll(".osd__val")) {
      if (valueColor) node.style.setProperty("color", valueColor, "important");
      else node.style.removeProperty("color");
    }
    const labelColor = overlayState.customRgba.label || colorFor(overlayState.labelColor);
    for (const node of osd.querySelectorAll(".osd__key, .osd__unit")) {
      if (labelColor) node.style.setProperty("color", labelColor, "important");
      else node.style.removeProperty("color");
    }
    // Цвет каждой строки: строка оверлея ссылается на свой показатель.
    for (const row of OSD_ROWS) {
      const color = overlayState.customRgba[row.key] || colorFor(overlayState.rowColors[row.key]);
      const value = osd.querySelector(`[data-osd-item="${row.key}"] .osd__val`);
      if (!value) continue;
      if (color) value.style.setProperty("color", color, "important");
      else if (!valueColor) value.style.removeProperty("color");
    }

    for (const node of osd.querySelectorAll("[data-metric]")) {
      const id = node.dataset.metric;
      const group = id.startsWith("vram") ? "vram"
        : sensorRows.get(id)?.sensor.group === "mem" ? "ram" : sensorRows.get(id)?.sensor.group;
      const rgba = overlayState.customRgba[group];
      if (rgba) node.style.setProperty("color", rgba, "important");
    }
    for (const [id, rgba] of Object.entries(overlayState.metricRgba)) {
      if (!/^#[0-9A-Fa-f]{8}$/.test(rgba)) continue;
      const value = osd.querySelector(`[data-metric="${id}"]`);
      if (value) value.style.setProperty("color", rgba, "important");
    }

    // Акцент — рамка и декоративные линии стиля.
    const accent = overlayState.customRgba.accent || colorFor(overlayState.accentColor);
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
    osd.querySelectorAll(".osd__row").forEach((row) => {
      const position = overlayState.rowPositions[row.dataset.osdItem] || { x: 0, y: 0 };
      row.style.translate = `${position.x}px ${position.y}px`;
    });

    fitOverlay();
    const radiusValue = $("#osdRadiusValue");
    if (radiusValue) radiusValue.textContent = `${overlayState.radius === null ? 8 : overlayState.radius} px`;
    const offsetValue = $("#osdOffsetValue");
    $("#osdOffsetLabel").textContent = overlayState.anchorMode === "free" ? "Координаты" : "Смещение";
    if (offsetValue) {
      offsetValue.textContent = overlayState.anchorMode === "free"
        ? `${Math.round(overlayState.originX)} / ${Math.round(overlayState.originY)} px`
        : `${overlayState.offsetX} / ${overlayState.offsetY} px`;
    }
    const radius = $("#osdRadius");
    if (radius) {
      const shown = overlayState.radius === null ? 8 : overlayState.radius;
      radius.value = String(shown);
      radius.style.setProperty("--fill", `${(shown / 24) * 100}%`);
    }
    recordOverlayHistory();
  }

  // Все органы управления оверлеем показывают значения из overlayState:
  // нужны после сброса пресета и после любой внешней правки состояния.
  function syncOsdControls() {
    $("#osdFollowGame").checked = overlayState.followGameMonitor;
    $("#osdSparkline").textContent = overlayState.sparkline ? `График: ${
      { fps: "FPS", cpu: "CPU", gpu: "GPU", frameMs: "кадр" }[overlayState.sparklineMetric] || "FPS"}`
      : "График: выкл";
    $$("#osdSurface button").forEach((button) => button.classList.toggle("is-active",
      button.dataset.surface === overlayState.surface));
    $$("#osdStyles .style-tile").forEach((tile) => tile.classList.toggle("is-active",
      tile.dataset.recipe === overlayState.recipe));
    $$("#osdLayout button").forEach((button) => button.classList.toggle("is-active",
      button.dataset.layout === overlayState.layout));
    $$("#osdFont button").forEach((button) => button.classList.toggle("is-active",
      Number(button.dataset.font) === overlayState.font));
    $("#osdScale").value = overlayState.scale;
    $("#osdScaleValue").textContent = `${overlayState.scale}%`;
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

  const canvasView = { zoom: 1, panX: 0, panY: 0, spaceDown: false };

  function updateCanvasView() {
    const viewport = $("#gameViewport");
    viewport.style.setProperty("--canvas-zoom", canvasView.zoom);
    viewport.style.setProperty("--canvas-pan-x", `${canvasView.panX}px`);
    viewport.style.setProperty("--canvas-pan-y", `${canvasView.panY}px`);
  }

  function freeOverlayAnchor() {
    if (overlayState.anchorMode === "free") return;
    const osd = $("#osdPreview");
    const screen = osd.offsetParent.getBoundingClientRect();
    const plate = osd.getBoundingClientRect();
    overlayState.originX = (plate.left - screen.left) / canvasView.zoom;
    overlayState.originY = (plate.top - screen.top) / canvasView.zoom;
    overlayState.offsetX = 0;
    overlayState.offsetY = 0;
    overlayState.anchorMode = "free";
    $$("#osdPosition button").forEach((button) => {
      button.classList.toggle("is-active", button.dataset.pos === "free");
    });
    applyOverlay();
  }

  function bindCanvasView() {
    const scene = $("#gameScene");
    let pan = null;
    scene.addEventListener("wheel", (event) => {
      event.preventDefault();
      const bounds = scene.getBoundingClientRect();
      const x = event.clientX - bounds.left;
      const y = event.clientY - bounds.top;
      const next = Math.max(0.75, Math.min(4, canvasView.zoom * (event.deltaY < 0 ? 1.15 : 1 / 1.15)));
      canvasView.panX = x - ((x - canvasView.panX) / canvasView.zoom) * next;
      canvasView.panY = y - ((y - canvasView.panY) / canvasView.zoom) * next;
      canvasView.zoom = next;
      updateCanvasView();
    }, { passive: false });
    document.addEventListener("keydown", (event) => {
      if (event.code === "Space" && !event.target.closest("input, textarea, button")) {
        canvasView.spaceDown = true;
        event.preventDefault();
      }
    });
    document.addEventListener("keyup", (event) => {
      if (event.code === "Space") canvasView.spaceDown = false;
    });
    window.addEventListener("blur", () => { canvasView.spaceDown = false; });
    scene.addEventListener("pointerdown", (event) => {
      if (!canvasView.spaceDown || event.button !== 0) return;
      pan = { x: event.clientX, y: event.clientY, left: canvasView.panX, top: canvasView.panY };
      scene.setPointerCapture(event.pointerId);
      scene.classList.add("is-panning");
      event.preventDefault();
      event.stopPropagation();
    }, true);
    scene.addEventListener("pointermove", (event) => {
      if (!pan) return;
      canvasView.panX = pan.left + event.clientX - pan.x;
      canvasView.panY = pan.top + event.clientY - pan.y;
      updateCanvasView();
    });
    const release = (event) => {
      if (!pan) return;
      pan = null;
      scene.classList.remove("is-panning");
      if (scene.hasPointerCapture(event.pointerId)) scene.releasePointerCapture(event.pointerId);
    };
    scene.addEventListener("pointerup", release);
    scene.addEventListener("pointercancel", release);
  }

  // Ctrl выбирает строки; Shift во время движения привязывает позицию к сетке.
  function bindOsdDrag() {
    const osd = $("#osdPreview");
    const guides = $("#osdGuides");
    const selected = new Set();
    let drag = null;
    const grid = (value) => Math.round(value / 8) * 8;
    const snap = (value, extent, targets) => {
      let best = { value, guide: null, distance: 7 };
      targets.forEach((target) => [0, extent / 2, extent].forEach((edge) => {
        const distance = Math.abs(value + edge - target);
        if (distance < best.distance) best = { value: value + target - value - edge, guide: target, distance };
      }));
      return best;
    };
    const showGuide = (axis, value) => {
      const node = $(`#osdGuide${axis.toUpperCase()}`);
      node.classList.toggle("is-active", value !== null);
      if (value !== null) guides.style.setProperty(`--guide-${axis}`, `${value}px`);
    };
    const markRows = () => osd.querySelectorAll(".osd__row").forEach((node) => {
      node.classList.toggle("is-selected", selected.has(node.dataset.osdItem));
    });

    osd.addEventListener("pointerdown", (event) => {
      if (event.button !== 0) return;
      const row = overlayState.layout === "plates" && !event.altKey
        ? event.target.closest(".osd__row") : null;
      if (row) selectOverlayLayer(row.querySelector("[data-metric]")?.dataset.metric || "fps");
      if (row && event.ctrlKey) {
        if (selected.has(row.dataset.osdItem)) selected.delete(row.dataset.osdItem);
        else selected.add(row.dataset.osdItem);
        markRows();
        event.preventDefault();
        return;
      }
      if (row) {
        if (!selected.has(row.dataset.osdItem)) {
          selected.clear();
          selected.add(row.dataset.osdItem);
        }
        markRows();
      } else freeOverlayAnchor();
      drag = {
        x: event.clientX, y: event.clientY, row,
        rows: row ? [...selected].map((key) => ({ key,
          x: overlayState.rowPositions[key]?.x || 0,
          y: overlayState.rowPositions[key]?.y || 0 })) : [],
        originX: overlayState.originX, originY: overlayState.originY
      };
      overlayHistory.gesture = true;
      guides.classList.add("is-active");
      osd.setPointerCapture(event.pointerId);
      osd.classList.add("is-dragging");
      event.preventDefault();
    });

    osd.addEventListener("pointermove", (event) => {
      if (!drag) return;
      const screen = osd.offsetParent.getBoundingClientRect();
      const plate = osd.getBoundingClientRect();
      if (drag.row) {
        const scale = plate.width / Math.max(1, osd.offsetWidth);
        const primary = drag.rows.find((item) => item.key === drag.row.dataset.osdItem);
        let nextX = primary.x + (event.clientX - drag.x) / scale;
        let nextY = primary.y + (event.clientY - drag.y) / scale;
        let guideY = null;
        if (event.shiftKey) {
          nextX = grid(nextX);
          nextY = grid(nextY);
        } else {
          const others = [...osd.querySelectorAll(".osd__row:not(.is-hidden)")]
            .filter((item) => !selected.has(item.dataset.osdItem));
          const baseline = others.map((item) => item.offsetTop
            + (overlayState.rowPositions[item.dataset.osdItem]?.y || 0) - drag.row.offsetTop);
          const snapped = snap(nextY, 0, baseline);
          nextY = snapped.value;
          guideY = snapped.guide;
        }
        const dx = nextX - primary.x;
        const dy = nextY - primary.y;
        drag.rows.forEach((item) => {
          const position = { x: item.x + dx, y: item.y + dy };
          overlayState.rowPositions[item.key] = position;
          const node = osd.querySelector(`[data-osd-item="${item.key}"]`);
          if (node) node.style.translate = `${position.x}px ${position.y}px`;
        });
        showGuide("x", null);
        showGuide("y", guideY === null ? null : (plate.top - screen.top) / canvasView.zoom
          + (drag.row.offsetTop + guideY) * (scale / canvasView.zoom));
      } else {
        const width = screen.width / canvasView.zoom;
        const height = screen.height / canvasView.zoom;
        const plateWidth = plate.width / canvasView.zoom;
        const plateHeight = plate.height / canvasView.zoom;
        let x = drag.originX + (event.clientX - drag.x) / canvasView.zoom;
        let y = drag.originY + (event.clientY - drag.y) / canvasView.zoom;
        const xSnap = event.shiftKey ? { value: grid(x), guide: null }
          : snap(x, plateWidth, [0, width * .05, width / 2, width * .95, width]);
        const ySnap = event.shiftKey ? { value: grid(y), guide: null }
          : snap(y, plateHeight, [0, height * .05, height / 2, height * .95, height]);
        x = Math.max(0, Math.min(width - plateWidth, xSnap.value));
        y = Math.max(0, Math.min(height - plateHeight, ySnap.value));
        overlayState.originX = x;
        overlayState.originY = y;
        showGuide("x", xSnap.guide);
        showGuide("y", ySnap.guide);
        applyOverlay();
      }
    });

    const release = (event) => {
      if (!drag) return;
      drag = null;
      overlayHistory.gesture = false;
      recordOverlayHistory();
      guides.classList.remove("is-active");
      showGuide("x", null);
      showGuide("y", null);
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
        if (chip.dataset.chrome !== "clickThrough") {
          overlayState.surface = null;
          $$("#osdSurface button").forEach((node) => node.classList.remove("is-active"));
        }
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
        if (overlayState.anchorMode === "free") {
          const screen = $("#osdPreview").offsetParent.getBoundingClientRect();
          const plate = $("#osdPreview").getBoundingClientRect();
          const key = axis === "x" ? "originX" : "originY";
          const limit = (axis === "x" ? screen.width - plate.width : screen.height - plate.height) / canvasView.zoom;
          overlayState[key] = Math.max(0, Math.min(limit, overlayState[key] + delta));
        } else {
          const next = (axis === "x" ? overlayState.offsetX : overlayState.offsetY) + delta;
          const clamped = Math.max(-200, Math.min(200, next));
          if (axis === "x") overlayState.offsetX = clamped;
          else overlayState.offsetY = clamped;
        }
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

  // Первые восемь ключей — миграция старых стилей без изменения их базового вида.
  const OSD_RECIPES = [
    ["neon", "Неон", "neon", "plates", null, 0, 100],
    ["rtss", "RTSS", "rtss", "plates", null, 0, 100],
    ["frosted", "Стекло", "frosted", "plates", null, 0, 100],
    ["terminal", "Терминал", "terminal", "plates", null, 0, 100],
    ["cyber", "Кибер", "cyber", "plates", null, 0, 100],
    ["mono", "Минимал", "mono", "plates", null, 0, 100],
    ["esport", "Esport", "esport", "plates", null, 0, 100],
    ["retro", "Retro", "retro", "plates", null, 0, 100],
    ["fps_large", "Только FPS", "neon", "line", "none", 0, 130],
    ["stream", "Для стрима", "frosted", "line", "plate", 0, 110],
    ["laptop", "Ноутбук 13″", "mono", "line", "none", 2, 85],
    ["hdr", "Для HDR", "esport", "line", "outline", 0, 120],
    ["recording", "Для записи", "terminal", "plates", "plate", 1, 105],
    ["quiet", "Не отвлекать", "frosted", "plates", "glass", 0, 85],
    ["focus", "Фокус", "cyber", "gauge", "outline", 2, 110],
    ["clean", "Чистая шкала", "mono", "gauge", "plate", 1, 100]
  ];
  const STYLE_CHROME = {
    rtss: { background: false, border: false, shadow: false },
    mono: { background: true, border: false, shadow: false }
  };

  function parseOverlayRecipeJson(text) {
    const data = JSON.parse(text);
    const isObject = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
    const fields = ["schema", "recipe", "palette", "shape", "surface", "typeface", "scale", "colors"];
    const palettes = OSD_RECIPES.slice(0, 8).map(([key]) => key);
    const fonts = ["sans", "mono", "condensed", "dense"];
    if (!isObject(data) || data.schema !== 1 || Object.keys(data).some((key) => !fields.includes(key))
        || !OSD_RECIPES.some(([key]) => key === data.recipe) || !palettes.includes(data.palette)
        || !["plates", "line", "gauge"].includes(data.shape)
        || ![null, "glass", "plate", "outline", "none", "custom"].includes(data.surface)
        || !fonts.includes(data.typeface) || !Number.isInteger(data.scale)
        || data.scale < 70 || data.scale > 160) {
      throw new Error("Неверная схема или параметры рецепта");
    }
    const colorKeys = ["value", "label", "accent", "fps", "cpu", "gpu", "ram", "vram"];
    if (data.colors !== undefined && (!isObject(data.colors)
        || Object.entries(data.colors).some(([key, value]) => !colorKeys.includes(key)
          || typeof value !== "string" || !/^#[0-9a-fA-F]{8}$/.test(value)))) {
      throw new Error("Неверный формат цвета рецепта");
    }
    return data;
  }

  function bindOsdStyles() {
    const gallery = $("#osdStyles");
    gallery.querySelectorAll(".style-tile").forEach((tile) => { tile.dataset.recipe = tile.dataset.style; });
    OSD_RECIPES.slice(8).forEach(([key, name, style, shape, surface]) => {
      const tile = document.createElement("button");
      tile.type = "button";
      tile.className = "style-tile";
      tile.dataset.recipe = key;
      tile.dataset.style = style;
      const preview = document.createElement("span");
      preview.className = "style-tile__preview";
      preview.dataset.style = style;
      const value = document.createElement("b"); value.textContent = "142";
      const label = document.createElement("i"); label.textContent = "FPS";
      const extra = document.createElement("em"); extra.textContent = shape === "gauge" ? "▰▰▱" : "46%";
      preview.append(value, label, extra);
      const caption = document.createElement("span");
      caption.className = "style-tile__name";
      caption.textContent = name;
      const detail = document.createElement("em");
      detail.textContent = `${shape === "gauge" ? "шкала" : shape === "line" ? "строка" : "плитка"} · ${surface}`;
      caption.append(detail);
      tile.append(preview, caption);
      gallery.append(tile);
    });
    const chromeForSurface = (surface, style) => {
      if (!surface) return STYLE_CHROME[style] || { background: true, border: true, shadow: true };
      return { background: surface === "glass" || surface === "plate",
        border: surface === "glass" || surface === "outline", shadow: surface === "glass" };
    };
    gallery.addEventListener("click", (event) => {
      const tile = event.target.closest(".style-tile");
      if (!tile) return;
      const recipe = OSD_RECIPES.find(([key]) => key === tile.dataset.recipe);
      if (!recipe) return;
      const [key, , style, layout, surface, font, scale] = recipe;
      const standard = new Set(["fps", "fpsLow1", "cpu", "cpuTemp", "gpu", "gpuTemp", "ramUsed", "vramUsed"]);
      for (const [id, entry] of sensorRows) {
        entry.sensor.pinned = key === "fps_large" ? id === "fps" : standard.has(id);
      }
      saveOverlaySensors(); rebuildOverlaySensors(); applySensorFilter();
      Object.assign(overlayState, { recipe: key, style, layout, surface, font, scale, radius: null,
        bgOpacity: null, padding: null, rowGap: null, valueColor: null, labelColor: null,
        accentColor: null, borderWidth: null, shadowStrength: 100, tracking: 100,
        uppercase: null, separators: null, fpsDecimals: null, textScale: 100 });
      overlayState.rowColors = { fps: null, cpu: null, gpu: null, ram: null, vram: null };
      overlayState.metricRgba = {};
      for (const field of Object.keys(overlayState.customRgba)) overlayState.customRgba[field] = null;
      selectOverlayLayer(selectedLayer);
      const chrome = chromeForSurface(surface, style);
      Object.assign(overlayState, chrome);
      syncOsdControls();
      $$("#osdChrome button").forEach((chip) => {
        chip.classList.toggle("is-active", Boolean(overlayState[chip.dataset.chrome]));
      });
      applyOverlay();
    });
    const fileInput = $("#importOsdRecipeFile");
    $("#importOsdRecipe").addEventListener("click", () => fileInput.click());
    fileInput.addEventListener("change", async () => {
      const file = fileInput.files?.[0];
      if (!file) return;
      try {
        if (file.name.toLowerCase().endsWith(".ovl")) {
          if (file.size > 65536) throw new Error("Файл RTSS слишком большой");
          const text = await file.text();
          if (!/^Implementation=2\s*$/mi.test(text) || !/^Layers=([1-9]\d*)\s*$/mi.test(text)) {
            throw new Error("Ожидался текстовый шаблон RTSS OverlayEditor v2");
          }
          const layers = Number(text.match(/^Layers=(\d+)\s*$/mi)[1]);
          if (layers > 256) throw new Error("Слишком много слоёв RTSS");
          const known = new Map([["<fr>", "fps"], ["%framerate%", "fps"],
            ["%frametime%", "frameMs"], ["%cpu usage%", "cpu"],
            ["%cpu temperature%", "cpuTemp"], ["%cpu clock%", "cpuClock"],
            ["%cpu power%", "cpuPower"], ["%gpu1 usage%", "gpu"],
            ["%gpu1 temperature%", "gpuTemp"], ["%gpu1 clock%", "gpuClock"],
            ["%gpu1 power%", "gpuPower"], ["%gpu1 memory usage%", "vramUsed"],
            ["%total ram%", "ramUsed"]]);
          const chosen = new Set();
          const colors = {};
          const sections = [...text.matchAll(/^\[Layer(\d+)\]\s*$([\s\S]*?)(?=^\[|$(?![\s\S]))/gmi)];
          for (const [, index, body] of sections) {
            if (Number(index) >= layers) continue;
            const line = body.match(/^Text=(.*)$/mi)?.[1]?.trim().toLowerCase();
            if (!line) continue;
            const encoded = body.match(/^TextColor=([A-Fa-f0-9]{6}(?:[A-Fa-f0-9]{2})?)\s*$/mi)?.[1];
            for (const [tag, metric] of known) {
              if (!line.includes(tag)) continue;
              if (!chosen.has(metric) && encoded) {
                colors[metric] = encoded.length === 8
                  ? `#${encoded.slice(2)}${encoded.slice(0, 2)}`.toUpperCase()
                  : `#${encoded}FF`.toUpperCase();
              }
              chosen.add(metric);
            }
          }
          if (!chosen.size) throw new Error("Нет поддерживаемых текстовых датчиков RTSS");
          gallery.querySelector('[data-recipe="rtss"]').click();
          for (const [id, entry] of sensorRows) entry.sensor.pinned = chosen.has(id);
          overlayState.metricOrder = [...chosen];
          overlayState.metricRgba = colors;
          saveOverlaySensors(); rebuildOverlaySensors(); applySensorFilter(); applyOverlay();
          toast("Показатели, порядок и текстовые цвета перенесены; позиции и внешние источники нет");
          return;
        }
        if (file.size > 16384) throw new Error("Файл рецепта слишком большой (предел 16 КБ)");
        const data = parseOverlayRecipeJson(await file.text());
        const fonts = ["sans", "mono", "condensed", "dense"];
        Object.assign(overlayState, { recipe: data.recipe, style: data.palette, layout: data.shape,
          surface: data.surface, font: fonts.indexOf(data.typeface), scale: data.scale,
          radius: null, bgOpacity: null, padding: null, rowGap: null,
          valueColor: null, labelColor: null, accentColor: null,
          borderWidth: null, shadowStrength: 100, tracking: 100,
          uppercase: null, separators: null, fpsDecimals: null, textScale: 100 });
        overlayState.rowColors = { fps: null, cpu: null, gpu: null, ram: null, vram: null };
        for (const key of Object.keys(overlayState.customRgba)) {
          overlayState.customRgba[key] = data.colors?.[key]?.toUpperCase() || null;
        }
        // В schema 1 нет фоновых ручных параметров для surface: custom.
        if (data.surface !== "custom") Object.assign(overlayState, chromeForSurface(data.surface, data.palette));
        selectOverlayLayer(selectedLayer);
        syncOsdControls();
        $$("#osdChrome button").forEach((chip) => chip.classList.toggle("is-active",
          Boolean(overlayState[chip.dataset.chrome])));
        applyOverlay();
        toast(data.surface === "custom" ? "Рецепт загружен; ручная поверхность осталась прежней"
          : "Рецепт загружен. Состав строк и личные настройки не изменены");
      } catch (error) {
        toast(`Не удалось импортировать: ${error instanceof Error ? error.message : "ошибка чтения"}`);
      } finally {
        fileInput.value = "";
      }
    });
    $("#exportOsdRtss").addEventListener("click", () => {
      const sources = new Map([["fps", ["<FR>", "", "FPS"]],
        ["frameMs", ["%Frametime%", "Frametime", "ms"]],
        ["cpu", ["%CPU usage%", "CPU usage", "%"]],
        ["cpuTemp", ["%CPU temperature%", "CPU temperature", "C"]],
        ["cpuClock", ["%CPU clock%", "CPU clock", "MHz"]],
        ["cpuPower", ["%CPU power%", "CPU power", "W"]],
        ["gpu", ["%GPU1 usage%", "GPU1 usage", "%"]],
        ["gpuTemp", ["%GPU1 temperature%", "GPU1 temperature", "C"]],
        ["gpuClock", ["%GPU1 clock%", "GPU1 clock", "MHz"]],
        ["gpuPower", ["%GPU1 power%", "GPU1 power", "W"]],
        ["vramUsed", ["%GPU1 memory usage%", "GPU1 memory usage", "MB"]],
        ["ramUsed", ["%Total RAM%", "Total RAM", "MB"]]]);
      const ordered = [...new Set([...overlayState.metricOrder, ...sensorRows.keys()])];
      const rows = ordered.filter((id) => sensorRows.get(id)?.sensor.pinned && sources.has(id));
      if (!rows.length) { toast("Нет показателей для экспорта в RTSS"); return; }
      const sourceRows = rows.filter((id) => sources.get(id)[1]);
      let text = `[Master]\r\nImplementation=2\r\nFontFace=Consolas\r\nFontHeight=-9\r\nFontWeight=400\r\nZoomRatio=3\r\n[Settings]\r\nRefreshPeriod=1000\r\n[General]\r\nSources=${sourceRows.length}\r\nTables=0\r\nLayers=${rows.length}\r\n`;
      sourceRows.forEach((id, index) => {
        const [, name, units] = sources.get(id);
        text += `[Source${index}]\r\nName=${name}\r\nUnits=${units}\r\nFormat=\r\nFormula=\r\nProvider=HAL\r\nID=${name}\r\n`;
      });
      rows.forEach((id, index) => {
        const [tag, name] = sources.get(id);
        const rgba = overlayState.metricRgba[id];
        const color = /^#[0-9A-Fa-f]{8}$/.test(rgba) ? `${rgba.slice(7)}${rgba.slice(1, 7)}` : "FFFFFFFF";
        text += `[Layer${index}]\r\nName=${name || "Framerate"}\r\nText=${tag}\r\nPositionX=4\r\nPositionY=${5 + index * 14}\r\nExtentX=0\r\nExtentY=0\r\nExtentOrigin=0\r\nFixedAlignment=1\r\nTextColor=${color}\r\n`;
      });
      const url = URL.createObjectURL(new Blob([text], { type: "text/plain" }));
      const link = document.createElement("a");
      link.href = url;
      link.download = "yawa-overlay.ovl";
      link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
      toast("Экспортированы только датчики RTSS HAL и FPS; проверьте шаблон в RTSS");
    });
    $("#exportOsdRecipe").addEventListener("click", () => {
      const { recipe, style, layout, surface, font, scale } = overlayState;
      const data = { schema: 1, recipe, palette: style, shape: layout,
        surface, typeface: ["sans", "mono", "condensed", "dense"][font], scale };
      const colors = Object.fromEntries(Object.entries(overlayState.customRgba)
        .filter(([, rgba]) => typeof rgba === "string" && /^#[0-9A-Fa-f]{8}$/.test(rgba)));
      if (Object.keys(colors).length) data.colors = colors;
      const json = JSON.stringify(data, null, 2);
      const url = URL.createObjectURL(new Blob([json + "\n"], { type: "application/json" }));
      const link = document.createElement("a");
      link.href = url;
      link.download = "yawa-overlay-recipe.json";
      link.click();
      setTimeout(() => URL.revokeObjectURL(url), 1000);
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
    $$("[data-view=overlay] input[type=range]").forEach((range) => {
      range.addEventListener("pointerdown", () => { overlayHistory.gesture = true; });
      const finish = () => {
        overlayHistory.gesture = false;
        recordOverlayHistory();
      };
      range.addEventListener("pointerup", finish);
      range.addEventListener("pointercancel", finish);
    });
    $("#osdHistory").addEventListener("click", (event) => {
      const button = event.target.closest("[data-history]");
      if (button) restoreOverlayHistory(button.dataset.history);
    });
    document.addEventListener("keydown", (event) => {
      if (!$("[data-view=overlay]").classList.contains("is-active") || !event.ctrlKey
        || event.key.toLowerCase() !== "z" || event.target.closest("input, textarea")) return;
      event.preventDefault();
      restoreOverlayHistory(event.shiftKey ? "redo" : "undo");
    });
    $("#osdPosition").addEventListener("click", (event) => {
      const button = event.target.closest("[data-pos]");
      if (!button) return;
      if (button.dataset.pos === "free") {
        freeOverlayAnchor();
      } else {
        overlayState.position = button.dataset.pos;
        overlayState.anchorMode = "corner";
        overlayState.offsetX = 0;
        overlayState.offsetY = 0;
        $$("#osdPosition button").forEach((node) => node.classList.toggle("is-active", node === button));
        applyOverlay();
      }
    });

    $("#osdFollowGame").addEventListener("change", (event) => {
      overlayState.followGameMonitor = event.target.checked;
      recordOverlayHistory();
    });
    $("#osdSparkline").addEventListener("click", () => {
      const choices = ["fps", "cpu", "gpu", "frameMs"];
      if (!overlayState.sparkline) {
        overlayState.sparkline = true;
        overlayState.sparklineMetric = choices[0];
      } else {
        const next = choices.indexOf(overlayState.sparklineMetric) + 1;
        if (next === choices.length) overlayState.sparkline = false;
        else overlayState.sparklineMetric = choices[next];
      }
      sparklineHistory.length = 0;
      $("#osdSparklinePoints").setAttribute("points", "");
      applyOverlay();
    });
    $("#osdMenuScene").addEventListener("click", () => {
      $("#osdPreview").classList.toggle("is-menu-scene");
      applyOverlay();
    });
    document.addEventListener("keydown", (event) => {
      if (event.ctrlKey && event.key === "F11" && $("[data-view=overlay]").classList.contains("is-active")) {
        event.preventDefault();
        $("#osdMenuScene").click();
      }
    });
    $("#osdMenuOpacity").addEventListener("input", (event) => {
      overlayState.menuOpacity = Number(event.target.value);
      applyOverlay();
    });

    $("#osdLayout").addEventListener("click", (event) => {
      const button = event.target.closest("[data-layout]");
      if (!button) return;
      overlayState.layout = button.dataset.layout;
      $$("#osdLayout button").forEach((node) => node.classList.toggle("is-active", node === button));
      applyOverlay();
    });
    $("#osdSurface").addEventListener("click", (event) => {
      const button = event.target.closest("[data-surface]");
      if (!button) return;
      overlayState.surface = button.dataset.surface;
      overlayState.background = ["glass", "plate"].includes(overlayState.surface);
      overlayState.border = ["glass", "outline"].includes(overlayState.surface);
      overlayState.shadow = overlayState.surface === "glass";
      $$("#osdChrome button").forEach((chip) => chip.classList.toggle("is-active",
        Boolean(overlayState[chip.dataset.chrome])));
      $$("#osdSurface button").forEach((node) => node.classList.toggle("is-active", node === button));
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

    // Число редактируется на месте; колесо над ползунком — 1, с Shift — 10.
    const editable = ["osdScale", "osdTextScale", "osdOpacity", "osdBg", "osdRadius",
      "osdPad", "osdGap", "osdBorder", "osdShadow", "osdTracking"];
    for (const id of editable) {
      const range = $(`#${id}`);
      const label = $(`#${id}Value`);
      if (!range || !label) continue;
      range.step = "1";
      range.addEventListener("wheel", (event) => {
        event.preventDefault();
        range.value = String(Math.max(Number(range.min), Math.min(Number(range.max),
          Number(range.value) + (event.deltaY < 0 ? 1 : -1) * (event.shiftKey ? 10 : 1))));
        range.dispatchEvent(new Event("input", { bubbles: true }));
      }, { passive: false });
      label.classList.add("editable-number");
      label.tabIndex = 0;
      label.setAttribute("role", "button");
      label.title = "Нажмите для ввода числа";
      const edit = () => {
        if (label.hidden) return;
        const input = document.createElement("input");
        input.type = "number";
        input.min = range.min;
        input.max = range.max;
        input.step = "1";
        input.value = range.value;
        input.className = "editable-number__input";
        label.hidden = true;
        label.after(input);
        let cancelled = false;
        input.addEventListener("keydown", (event) => {
          if (event.key === "Escape") { cancelled = true; input.blur(); }
          if (event.key === "Enter") input.blur();
        });
        input.addEventListener("blur", () => {
          if (!cancelled && input.value !== "") {
            range.value = String(Math.max(Number(range.min), Math.min(Number(range.max), Number(input.value))));
            range.dispatchEvent(new Event("input", { bubbles: true }));
          }
          input.remove();
          label.hidden = false;
        }, { once: true });
        input.focus();
        input.select();
      };
      label.addEventListener("click", edit);
      label.addEventListener("keydown", (event) => { if (event.key === "Enter") edit(); });
    }
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

    $("#etwToggle").addEventListener("change", updateFrameStatus);
    $("#sourceHelp").addEventListener("click", () => {
      toast("Журнал: %LOCALAPPDATA%\\YawaFpsHub\\YawaFpsHub.log");
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

  const workshopFields = $$(".workshop-inspector > .field");
  let textFields = false;
  workshopFields.forEach((field) => {
    if (field.querySelector("#osdFont")) textFields = true;
    if (field.querySelector("#osdOffsetValue")) textFields = false;
    field.dataset.editorPanel = textFields ? "text" : "appearance";
  });
  const showWorkshopSection = (section) => {
    $("#overlayGrid").dataset.section = section;
    $$(".workshop-nav button").forEach((button) => {
      button.classList.toggle("is-active", button.dataset.editorSection === section);
    });
    workshopFields.forEach((field) => {
      field.classList.toggle("is-visible", section === "assemble" ? field.classList.contains("quick-field")
        : section !== "rows" && field.dataset.editorPanel === section);
    });
  };
  $(".workshop-nav").addEventListener("click", (event) => {
    const button = event.target.closest("[data-editor-section]");
    if (button) showWorkshopSection(button.dataset.editorSection);
  });
  showWorkshopSection("assemble");

  bindPause();
  bindAccents();
  bindOverlay();
  bindCanvasView();
  bindOsdStyles();
  bindLayerColor();
  bindOsdChrome();
  bindOsdText();
  bindOsdDrag();
  bindLabels();
  bindHotkeys();
  bindGpuPicker();
  bindSettings();
  bindSensors();
  bindPerfLegend();
  bindDiagnosisPreview();
  restorePreferences();
  overlayHistory.undo = [];
  overlayHistory.redo = [];
  overlayHistory.committed = JSON.stringify(overlayState);
  recordOverlayHistory();
  updateFrameStatus();

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
