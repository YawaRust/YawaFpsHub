/* Симулятор показателей для прототипа. В приложении на этом месте — app/metrics. */
(function () {
  "use strict";

  const HISTORY = 90;
  const FRAME_TARGET_HZ = 165;

  const GPU_PROFILES = [
    {
      name: "GeForce RTX 4080", memoryGb: 16,
      load: 93, tempBase: 52, tempK: 0.22, hotspotBase: 62, hotspotK: 0.2,
      clockBase: 2620, clockK: 2.4, clockMin: 2100, clockMax: 2900, memClock: 11201,
      powerBase: 120, powerK: 2.1, powerMax: 360, fanBase: 1200, fanK: 8.4,
      vramBase: 8.1, vramK: 0.017
    },
    {
      name: "AMD Radeon 780M", memoryGb: 4,
      load: 34, tempBase: 40, tempK: 0.2, hotspotBase: 46, hotspotK: 0.22,
      clockBase: 2100, clockK: 4.2, clockMin: 1200, clockMax: 2500, memClock: 2800,
      powerBase: 5, powerK: 0.62, powerMax: 62, fanBase: 0, fanK: 0,
      vramBase: 1.1, vramK: 0.014
    }
  ];

  const GPU_SENSOR_IDS = ["gpu", "gpuTemp", "gpuHotspot", "gpuClock", "gpuMemClock", "gpuPower", "gpuFan", "vramUsed"];

  const SENSOR_DEFS = [
    { id: "fps", label: "FPS", group: "fps", unit: "fps", decimals: 0, pinned: true },
    { id: "frameMs", label: "Frame time", group: "fps", unit: "мс", decimals: 2 },
    { id: "fpsLow1", label: "1% low", group: "fps", unit: "fps", decimals: 0, pinned: true },
    { id: "fpsLow01", label: "0.1% low", group: "fps", unit: "fps", decimals: 0 },
    { id: "fpsAvg", label: "Средний FPS", group: "fps", unit: "fps", decimals: 1 },
    { id: "framesDropped", label: "Пропущено за секунду", group: "fps", unit: "шт", decimals: 0 },

    { id: "cpu", label: "Загрузка процессора", group: "cpu", unit: "%", decimals: 0, pinned: true },
    { id: "cpuTemp", label: "Температура CPU", group: "cpu", unit: "°C", decimals: 0, pinned: true },
    { id: "cpuClock", label: "Частота CPU", group: "cpu", unit: "МГц", decimals: 0 },
    { id: "cpuPower", label: "Питание CPU", group: "cpu", unit: "Вт", decimals: 0 },
    { id: "cpuGameThread", label: "Поток игры", group: "cpu", unit: "%", decimals: 0 },

    { id: "gpu", label: "Загрузка видеокарты", group: "gpu", unit: "%", decimals: 0, pinned: true },
    { id: "gpuTemp", label: "Температура GPU", group: "gpu", unit: "°C", decimals: 0, pinned: true },
    { id: "gpuHotspot", label: "Hotspot GPU", group: "gpu", unit: "°C", decimals: 0 },
    { id: "gpuClock", label: "Частота ядра", group: "gpu", unit: "МГц", decimals: 0 },
    { id: "gpuMemClock", label: "Частота памяти", group: "gpu", unit: "МГц", decimals: 0 },
    { id: "gpuPower", label: "Питание GPU", group: "gpu", unit: "Вт", decimals: 0 },
    { id: "gpuFan", label: "Вентилятор GPU", group: "gpu", unit: "об/мин", decimals: 0 },
    { id: "vramUsed", label: "Видеопамять занято", group: "gpu", unit: "ГБ", decimals: 1 },

    { id: "ramUsed", label: "Память занято", group: "mem", unit: "ГБ", decimals: 1, pinned: true },
    { id: "ramCache", label: "Память в кэше", group: "mem", unit: "ГБ", decimals: 1 },
    { id: "ramFree", label: "Память свободно", group: "mem", unit: "ГБ", decimals: 1 },
    { id: "ramClock", label: "Частота памяти", group: "mem", unit: "МГц", decimals: 0 },
    { id: "pageFile", label: "Своп-файл", group: "mem", unit: "ГБ", decimals: 1 },

    { id: "diskRead", label: "Диск: чтение", group: "io", unit: "МБ/с", decimals: 0 },
    { id: "diskWrite", label: "Диск: запись", group: "io", unit: "МБ/с", decimals: 0 },
    { id: "diskTemp", label: "Температура NVMe", group: "io", unit: "°C", decimals: 0 },
    { id: "netDown", label: "Сеть: входящий", group: "io", unit: "Мбит/с", decimals: 1 },
    { id: "netUp", label: "Сеть: исходящий", group: "io", unit: "Мбит/с", decimals: 1 },

    { id: "chipsetTemp", label: "Температура чипсета", group: "sys", unit: "°C", decimals: 0 },
    { id: "procCpu", label: "Собственное потребление CPU", group: "sys", unit: "%", decimals: 1 },
    { id: "procRam", label: "Собственное потребление ОЗУ", group: "sys", unit: "МБ", decimals: 1 },
    { id: "procThreads", label: "Потоков в процессе", group: "sys", unit: "шт", decimals: 0 }
  ];

  const GROUP_COLORS = {
    fps: "#5ef29b",
    cpu: "#5ef29b",
    gpu: "#6aa8ff",
    mem: "#b47bff",
    io: "#4dd6d6",
    sys: "#ffb347"
  };

  function mulberry32(seed) {
    return function () {
      seed = (seed + 0x6d2b79f5) | 0;
      let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
      t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
      return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
  }

  const random = mulberry32(Date.now() & 0xffff);
  const between = (low, high) => low + random() * (high - low);

  function baseValue(id) {
    switch (id) {
      case "fps": return 142;
      case "frameMs": return 7.04;
      case "fpsLow1": return 118;
      case "fpsLow01": return 96;
      case "fpsAvg": return 138.4;
      case "framesDropped": return 1204;
      case "cpu": return 44;
      case "cpuTemp": return 64;
      case "cpuClock": return 4925;
      case "cpuPower": return 78;
      case "cpuGameThread": return 62;
      case "gpu": return 93;
      case "gpuTemp": return 71;
      case "gpuHotspot": return 79;
      case "gpuClock": return 2835;
      case "gpuMemClock": return 11201;
      case "gpuPower": return 312;
      case "gpuFan": return 1940;
      case "vramUsed": return 9.6;
      case "ramUsed": return 21.4;
      case "ramCache": return 11.6;
      case "ramFree": return 31.0;
      case "ramClock": return 6000;
      case "pageFile": return 0.9;
      case "diskRead": return 184;
      case "diskWrite": return 42;
      case "diskTemp": return 48;
      case "netDown": return 2.4;
      case "netUp": return 0.6;
      case "chipsetTemp": return 52;
      case "procCpu": return 0.3;
      case "procRam": return 24.6;
      case "procThreads": return 3;
      default: return 0;
    }
  }

  const sessionStart = Date.now();

  const state = {
    intervalMs: 1000,
    paused: false,
    gpuIndex: 0,
    gpuBackup: [null, null],
    smoothing: 40,
    tick: 0,
    hitchLeft: 0,
    nextHitchAt: 9,
    fps: 142,
    frameMs: 7.04,
    cpu: 44,
    gpu: 93,
    ram: 21.4,
    framesRendered: 712428,
    framesLost: 1204,
    fpsSum: 0,
    fpsSamples: 0,
    sensors: new Map(),
    listeners: []
  };

  for (const def of SENSOR_DEFS) {
    const value = baseValue(def.id);
    state.sensors.set(def.id, {
      ...def,
      color: GROUP_COLORS[def.group],
      value,
      min: value,
      max: value,
      sum: value,
      samples: 1,
      history: new Array(HISTORY).fill(value)
    });
  }

  function smoothed(key, next, factor) {
    return state[key] + (next - state[key]) * factor;
  }

  function clamp(value, low, high) {
    return Math.min(high, Math.max(low, value));
  }

  function tick() {
    state.tick += 1;
    const second = state.tick;

    const isHitch = state.hitchLeft > 0 || (second >= state.nextHitchAt && random() < 0.55);
    if (isHitch && state.hitchLeft === 0) {
      state.hitchLeft = Math.round(between(1, 3));
      state.nextHitchAt = second + Math.round(between(14, 26));
    }
    state.hitchLeft = Math.max(0, state.hitchLeft - (isHitch ? 1 : 0));

    const wave = Math.sin(second / 7.4) * 3.2 + Math.sin(second / 2.1) * 1.4;
    const cpuLoad = clamp(44 + wave + between(-4, 4) + (isHitch ? between(10, 18) : 0), 12, 99);
    const profile = GPU_PROFILES[state.gpuIndex];
    const gpuLoad = clamp(profile.load + between(-2.5, 2) + (isHitch ? between(-6, 2) : 0), 8, 100);

    const cpuPush = (cpuLoad / 100) * between(0.05, 0.09);
    const targetFps = isHitch
      ? between(94, 121)
      : 146 - cpuPush * 210 + between(-3.5, 3.5);
    state.fps = clamp(smoothed("fps", targetFps, state.hitchLeft > 0 ? 0.85 : 0.38), 45, 175);
    state.frameMs = 1000 / state.fps;

    state.cpu = clamp(smoothed("cpu", cpuLoad, 0.45), 5, 100);
    state.gpu = clamp(smoothed("gpu", gpuLoad, 0.45), 20, 100);
    state.ram = clamp(smoothed("ram", 21.4 + Math.sin(second / 31) * 1.3 + between(-0.25, 0.25), 0.15), 8, 58);

    const frameHistory = state.sensors.get("fps").history;
    const recent = frameHistory.slice(-30).filter((value) => value > 0);
    const worst = recent.length ? Math.min(...recent) : state.fps;

    const rendered = Math.round((state.fps * state.intervalMs) / 1000);
    const lost = Math.round((state.frameMs > 1000 / FRAME_TARGET_HZ ? between(0.07, 0.5) : 0.01) * rendered);
    state.framesRendered += rendered;
    state.framesLost += lost;
    state.fpsSum += state.fps;
    state.fpsSamples += 1;

    const values = {
      fps: state.fps,
      frameMs: state.frameMs,
      fpsLow1: clamp(worst * 0.98, 40, state.fps),
      fpsLow01: clamp(worst * 0.78, 30, state.fps),
      fpsAvg: state.fpsSum / state.fpsSamples,
      framesDropped: lost,
      cpu: state.cpu,
      cpuTemp: clamp(50 + state.cpu * 0.32 + between(-0.6, 0.6), 35, 92),
      cpuClock: clamp(4980 - (100 - state.cpu) * 2.4 + between(-35, 35), 3600, 5050),
      cpuPower: clamp(46 + state.cpu * 0.76 + between(-2, 2), 22, 142),
      cpuGameThread: clamp(state.cpu * 1.35 + between(-4, 4), 20, 100),
      gpu: state.gpu,
      gpuTemp: clamp(profile.tempBase + state.gpu * profile.tempK + between(-0.4, 0.4), 28, 92),
      gpuHotspot: clamp(profile.hotspotBase + state.gpu * profile.hotspotK + between(-0.6, 0.6), 30, 106),
      gpuClock: clamp(profile.clockBase + state.gpu * profile.clockK + between(-25, 25), profile.clockMin, profile.clockMax),
      gpuMemClock: profile.memClock,
      gpuPower: clamp(profile.powerBase + state.gpu * profile.powerK + between(-4, 4), 2, profile.powerMax),
      gpuFan: profile.fanK === 0 ? 0 : clamp(profile.fanBase + state.gpu * profile.fanK + between(-30, 30), 0, 2400),
      vramUsed: clamp(profile.vramBase + state.gpu * profile.vramK + between(-0.1, 0.1), 0.4, profile.memoryGb - 0.3),
      ramUsed: state.ram,
      ramCache: clamp(64 - state.ram - 31 + between(-0.2, 0.2), 4, 30),
      ramFree: clamp(64 - state.ram - 11.6, 1, 58),
      pageFile: clamp(0.9 + between(-0.05, 0.05), 0.4, 2.4),
      diskRead: clamp(isHitch ? between(320, 680) : 184 + Math.sin(second / 4) * 60 + between(-25, 25), 4, 720),
      diskWrite: clamp(42 + Math.sin(second / 5.6) * 18 + between(-8, 8), 0, 180),
      diskTemp: clamp(48 + between(-1.2, 1.2), 38, 62),
      netDown: clamp(isHitch ? between(20, 90) : 2.4 + Math.sin(second / 9) * 1.5 + between(-0.6, 0.6), 0.1, 120),
      netUp: clamp(0.6 + between(-0.2, 0.5), 0.05, 12),
      chipsetTemp: clamp(52 + between(-0.8, 0.8), 42, 68),
      procCpu: clamp(0.3 + (isHitch ? 0.9 : 0) + between(-0.08, 0.12), 0.05, 2.2),
      procRam: clamp(24.6 + Math.sin(second / 20) * 0.8 + between(-0.4, 0.4), 18, 34),
      procThreads: 3
    };

    for (const [id, sensor] of state.sensors) {
      if (!(id in values)) continue;
      const next = values[id];
      const factor = state.smoothing / 100;
      sensor.value = sensor.history[sensor.history.length - 1] * factor + next * (1 - factor);
      sensor.history.push(sensor.value);
      if (sensor.history.length > HISTORY) sensor.history.shift();
      sensor.min = Math.min(sensor.min, sensor.value);
      sensor.max = Math.max(sensor.max, sensor.value);
      sensor.sum += sensor.value;
      sensor.samples += 1;
    }

    for (const listener of state.listeners) listener(snapshot());
  }

  function snapshot() {
    return {
      fps: state.fps,
      frameMs: state.frameMs,
      cpu: state.cpu,
      gpu: state.gpu,
      ram: state.ram,
      paused: state.paused,
      intervalMs: state.intervalMs,
      elapsedMs: Date.now() - sessionStart,
      framesRendered: state.framesRendered,
      framesLost: state.framesLost,
      sensors: state.sensors
    };
  }

  let timer = null;

  function schedule() {
    clearInterval(timer);
    if (state.paused) return;
    timer = setInterval(tick, state.intervalMs);
  }

  // Первый проход заполняет историю, чтобы графики не открывались пустыми.
  for (let i = 0; i < 60; i += 1) tick();
  state.listeners.length = 0;
  schedule();

  function typicalValue(id, profile) {
    switch (id) {
      case "gpu": return profile.load;
      case "gpuTemp": return profile.tempBase + profile.load * profile.tempK;
      case "gpuHotspot": return profile.hotspotBase + profile.load * profile.hotspotK;
      case "gpuClock": return profile.clockBase + profile.load * profile.clockK;
      case "gpuMemClock": return profile.memClock;
      case "gpuPower": return profile.powerBase + profile.load * profile.powerK;
      case "gpuFan": return profile.fanK === 0 ? 0 : profile.fanBase + profile.load * profile.fanK;
      case "vramUsed": return profile.vramBase + profile.load * profile.vramK;
      default: return 0;
    }
  }

  function captureGpu(index) {
    const saved = new Map();
    for (const id of GPU_SENSOR_IDS) {
      const sensor = state.sensors.get(id);
      saved.set(id, {
        value: sensor.value,
        min: sensor.min,
        max: sensor.max,
        sum: sensor.sum,
        samples: sensor.samples,
        history: sensor.history.slice()
      });
    }
    state.gpuBackup[index] = saved;
  }

  function restoreGpu(index) {
    const profile = GPU_PROFILES[index];
    const saved = state.gpuBackup[index];

    for (const id of GPU_SENSOR_IDS) {
      const sensor = state.sensors.get(id);
      const entry = saved && saved.get(id);
      if (entry) {
        Object.assign(sensor, entry, { history: entry.history.slice() });
        continue;
      }
      const typical = typicalValue(id, profile);
      sensor.value = typical;
      sensor.min = typical;
      sensor.max = typical;
      sensor.sum = typical;
      sensor.samples = 1;
      sensor.history = new Array(HISTORY).fill(typical);
    }

    state.gpu = profile.load;
  }

  window.Sim = {
    historyLength: HISTORY,
    groups: GROUP_COLORS,
    snapshot,
    subscribe(listener) {
      state.listeners.push(listener);
      listener(snapshot());
    },
    setPaused(paused) {
      state.paused = paused;
      schedule();
      for (const listener of state.listeners) listener(snapshot());
    },
    setIntervalMs(intervalMs) {
      state.intervalMs = intervalMs;
      schedule();
      for (const listener of state.listeners) listener(snapshot());
    },
    setSmoothing(percent) {
      state.smoothing = percent;
    },
    isPaused() {
      return state.paused;
    },
    gpuInfo() {
      const profile = GPU_PROFILES[state.gpuIndex];
      return {
        index: state.gpuIndex,
        count: GPU_PROFILES.length,
        name: profile.name,
        memoryGb: profile.memoryGb
      };
    },
    setGpu(index) {
      if (index === state.gpuIndex || !GPU_PROFILES[index]) return;
      captureGpu(state.gpuIndex);
      state.gpuIndex = index;
      restoreGpu(index);
      for (const listener of state.listeners) listener(snapshot());
    }
  };
})();
