/* Симулятор показателей для прототипа. В приложении на этом месте — app/metrics. */
(function () {
  "use strict";

  const HISTORY = 90;
  const FRAME_TARGET_HZ = 165;
  // Столько же ОЗУ, сколько считает интерфейс: загрузка в процентах должна совпадать.
  const RAM_TOTAL = 64;

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

  const GPU_SENSOR_IDS = ["gpu", "gpuTemp", "gpuHotspot", "gpuClock", "gpuMemClock", "gpuPower", "gpuFan",
    "gpuEncode", "gpuDecode", "pcieLoad", "vramUsed"];

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
    ...Array.from({ length: 8 }, (_, index) => ({
      id: `core${index}`, label: `Ядро ${index + 1}`, group: "cpu", unit: "%", decimals: 0
    })),

    { id: "gpu", label: "Загрузка видеокарты", group: "gpu", unit: "%", decimals: 0, pinned: true },
    { id: "gpuTemp", label: "Температура GPU", group: "gpu", unit: "°C", decimals: 0, pinned: true },
    { id: "gpuHotspot", label: "Hotspot GPU", group: "gpu", unit: "°C", decimals: 0 },
    { id: "gpuClock", label: "Частота ядра", group: "gpu", unit: "МГц", decimals: 0 },
    { id: "gpuMemClock", label: "Частота памяти", group: "gpu", unit: "МГц", decimals: 0 },
    { id: "gpuPower", label: "Питание GPU", group: "gpu", unit: "Вт", decimals: 0 },
    { id: "gpuFan", label: "Вентилятор GPU", group: "gpu", unit: "об/мин", decimals: 0 },
    { id: "gpuEncode", label: "Кодирование видео", group: "gpu", unit: "%", decimals: 0 },
    { id: "gpuDecode", label: "Декодирование видео", group: "gpu", unit: "%", decimals: 0 },
    { id: "pcieLoad", label: "Нагрузка PCI-e", group: "gpu", unit: "%", decimals: 0 },
    { id: "vramUsed", label: "Видеопамять занято", group: "gpu", unit: "ГБ", decimals: 1 },

    { id: "ramUsed", label: "Память занято", group: "mem", unit: "ГБ", decimals: 1, pinned: true },
    { id: "ramLoad", label: "Загрузка ОЗУ", group: "mem", unit: "%", decimals: 0 },
    { id: "ramCache", label: "Память в кэше", group: "mem", unit: "ГБ", decimals: 1 },
    { id: "ramFree", label: "Память свободно", group: "mem", unit: "ГБ", decimals: 1 },
    { id: "ramClock", label: "Частота памяти", group: "mem", unit: "МГц", decimals: 0 },
    { id: "pageFile", label: "Своп-файл занято", group: "mem", unit: "ГБ", decimals: 1 },
    { id: "pageFileTotal", label: "Файл подкачки всего", group: "mem", unit: "ГБ", decimals: 1 },
    { id: "pageFileLoad", label: "Загрузка файла подкачки", group: "mem", unit: "%", decimals: 0 },

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
      case "core0": case "core1": case "core2": case "core3":
      case "core4": case "core5": case "core6": case "core7":
        return 38 + Number(id.slice(4)) * 2.4;
      case "gpu": return 93;
      case "gpuTemp": return 71;
      case "gpuHotspot": return 79;
      case "gpuClock": return 2835;
      case "gpuMemClock": return 11201;
      case "gpuPower": return 312;
      case "gpuFan": return 1940;
      case "gpuEncode": return 0;
      case "gpuDecode": return 21;
      case "pcieLoad": return 6;
      case "vramUsed": return 9.6;
      case "ramUsed": return 21.4;
      case "ramLoad": return 33;
      case "ramCache": return 11.6;
      case "ramFree": return 31.0;
      case "ramClock": return 6000;
      case "pageFile": return 0.9;
      case "pageFileTotal": return 8.0;
      case "pageFileLoad": return 11;
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
      ...Object.fromEntries(Array.from({ length: 8 }, (_, index) => [
        `core${index}`,
        clamp(state.cpu + Math.sin(second / (2.3 + index * 0.31) + index) * 26 + between(-6, 6), 2, 100)
      ])),
      gpu: state.gpu,
      gpuTemp: clamp(profile.tempBase + state.gpu * profile.tempK + between(-0.4, 0.4), 28, 92),
      gpuHotspot: clamp(profile.hotspotBase + state.gpu * profile.hotspotK + between(-0.6, 0.6), 30, 106),
      gpuClock: clamp(profile.clockBase + state.gpu * profile.clockK + between(-25, 25), profile.clockMin, profile.clockMax),
      gpuMemClock: profile.memClock,
      gpuPower: clamp(profile.powerBase + state.gpu * profile.powerK + between(-4, 4), 2, profile.powerMax),
      gpuFan: profile.fanK === 0 ? 0 : clamp(profile.fanBase + state.gpu * profile.fanK + between(-30, 30), 0, 2400),
      // Медиа-движки: кодер включается редко (запись/OBS), декодер — почти постоянно.
      gpuEncode: clamp(Math.max(0, Math.sin(second / 23) * 18 - 12) + (isHitch ? between(4, 12) : 0), 0, 100),
      gpuDecode: clamp(21 + Math.sin(second / 5.3) * 9 + between(-2.5, 2.5), 0, 100),
      pcieLoad: clamp(6 + state.gpu * 0.03 + Math.sin(second / 3.4) * 2.4 + between(-0.8, 0.8), 0, 60),
      vramUsed: clamp(profile.vramBase + state.gpu * profile.vramK + between(-0.1, 0.1), 0.4, profile.memoryGb - 0.3),
      ramUsed: state.ram,
      ramLoad: clamp((state.ram / RAM_TOTAL) * 100, 1, 100),
      ramCache: clamp(64 - state.ram - 31 + between(-0.2, 0.2), 4, 30),
      ramFree: clamp(64 - state.ram - 11.6, 1, 58),
      pageFile: clamp(0.9 + between(-0.05, 0.05), 0.4, 2.4),
      pageFileTotal: 8.0,
      // Файл подкачки 8 ГБ: занято около 0,9 ГБ.
      pageFileLoad: clamp((0.9 / 8) * 100 + between(-0.2, 0.2), 1, 100),
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

  // Паспорт машины. В приложении эти данные собирают CPUID, SMBIOS, DXGI и реестр
  // (app/platform/hardware_info_win.cpp); прототипу собирать нечего, поэтому профиль
  // задан списком — но поля и формат строк совпадают с тем, что отдаёт приложение.
  const MACHINE = {
    os: { product: "Windows 11 Pro", edition: "Professional", feature: "24H2", build: "26100.4500", arch: "x64" },
    cpu: {
      name: "AMD Ryzen 7 7800X3D", vendor: "AuthenticAMD",
      cores: 8, threads: 16, baseMhz: 4200, l3Mb: 96, smt: false
    },
    gpus: [
      { name: "NVIDIA GeForce RTX 4080", vendor: "NVIDIA", dedicatedMb: 16384, discrete: true, driver: "560.94" },
      { name: "AMD Radeon 780M", vendor: "AMD", dedicatedMb: 512, discrete: false, driver: "32.0.21003.1" }
    ],
    memory: {
      totalGb: 64, type: "DDR5", ratedMt: 6000, configuredMt: 6000, channels: 2, slots: 4, profile: true,
      modules: [
        { slot: "DIMM 0", sizeGb: 16, type: "DDR5", configuredMt: 6000, vendor: "G.Skill", part: "F5-6000J3038F16GX2", voltage: 1.25, populated: true },
        { slot: "DIMM 1", sizeGb: 16, type: "DDR5", configuredMt: 6000, vendor: "G.Skill", part: "F5-6000J3038F16GX2", voltage: 1.25, populated: true },
        { slot: "DIMM 2", sizeGb: 16, type: "DDR5", configuredMt: 6000, vendor: "G.Skill", part: "F5-6000J3038F16GX2", voltage: 1.25, populated: true },
        { slot: "DIMM 3", sizeGb: 16, type: "DDR5", configuredMt: 6000, vendor: "G.Skill", part: "F5-6000J3038F16GX2", voltage: 1.25, populated: true }
      ]
    },
    displays: [
      { name: "LG 27GP850-B", width: 2560, height: 1440, hz: 165, scale: 1, primary: true },
      { name: "Dell U2422H", width: 1920, height: 1080, hz: 60, scale: 1, primary: false }
    ]
  };

  // Индексы производительности — порт app/metrics/bottleneck.cpp: 100 = флагман
  // поколения. Живые замеры всегда точнее, прогноз нужен только до запуска игры.
  function modelNumber(name) {
    const matches = String(name).toUpperCase().match(/\d{3,5}/g);
    return matches ? Number(matches[0]) : 0;
  }

  function cpuPerfIndex(name, cores, threads) {
    const text = String(name || "").toUpperCase();
    if (!text) return 0;
    const model = modelNumber(text);
    let single = 0;
    if (text.includes("RYZEN") || text.includes("THREADRIPPER")) {
      const generations = { 1: 54, 2: 60, 3: 70, 5: 85, 7: 95, 9: 100 };
      single = generations[Math.floor(model / 1000)] || 48;
      single *= { 3: 0.86, 5: 0.95, 7: 1, 9: 1.03 }[(text.match(/RYZEN (\d)/) || [])[1]] || 0.95;
      if (text.includes("X3D")) single *= 1.14;
    } else if (text.includes("CORE ULTRA")) {
      single = 96 * ({ 5: 0.95, 7: 1, 9: 1.03 }[(text.match(/ULTRA (\d)/) || [])[1]] || 0.95);
    } else if (text.includes("CORE I") || text.includes("XEON")) {
      const generations = { 6: 46, 7: 50, 8: 55, 9: 60, 10: 66, 11: 74, 12: 88, 13: 94, 14: 99 };
      single = generations[Math.floor(model / 1000)] || 40;
      single *= { 3: 0.86, 5: 0.95, 7: 1, 9: 1.03 }[(text.match(/I(\d)/) || [])[1]] || 0.95;
      if (/K\b|KF$/.test(text)) single *= 1.04;
    } else if (text.includes("PENTIUM") || text.includes("CELERON")) {
      single = 42;
    } else {
      single = 60 + Math.min(threads || 0, 32) * 0.8;
    }
    const threadFactor = threads ? Math.min(1.12, Math.max(0.45, Math.sqrt(threads / 16))) : 1;
    const coresPenalty = cores && cores < 6 ? 0.9 : 1;
    const extraCores = cores > 8 ? Math.min((cores - 8) * 0.012, 0.1) : 0;
    return Math.max(0, single * threadFactor * coresPenalty * (1 + extraCores));
  }

  function gpuPerfIndex(name, dedicatedMb) {
    const text = String(name || "").toUpperCase();
    if (!text) return 0;
    const families = [
      ["GTX 10", 40], ["RTX 20", 62], ["RTX 30", 88], ["RTX 40", 122], ["RTX 50", 152],
      ["RX 9", 150], ["RX 7", 124], ["RX 6", 92], ["RX 5", 50],
      ["ARC A", 55], ["ARC B", 82], ["INTEL UHD", 12], ["IRIS", 18], ["RADEON GRAPHICS", 20]
    ];
    for (const [needle, base] of families) {
      if (!text.includes(needle)) continue;
      const model = modelNumber(text.replace(needle, ""));
      const tier = model ? Math.floor((model % 1000) / 10) * 10 : 0;
      const tiers = { 50: 0.38, 60: 0.46, 70: 0.62, 80: 0.8, 90: 1 };
      let index = base * (tiers[tier] || 0.55);
      if (text.includes("SUPER")) index *= 1.12;
      if (text.includes("TI")) index *= 1.16;
      if (text.includes("XTX")) index *= 1.2;
      else if (text.includes("XT")) index *= 1.08;
      if (text.includes("GRE")) index *= 0.94;
      if (text.includes("LAPTOP") || text.includes("MOBILE")) index *= 0.72;
      return Math.max(0, index);
    }
    if (text.includes("BASIC")) return 0;
    return dedicatedMb ? Math.min(60, Math.max(6, dedicatedMb / 96)) : 0;
  }

  function describeMemory(memory) {
    const filled = memory.modules.filter((module) => module.populated);
    if (!filled.length) return "нет данных";
    const sizes = filled.map((module) => module.sizeGb).sort((a, b) => a - b);
    const uniform = sizes.every((size) => Math.abs(size - sizes[0]) < 0.5);
    let text = uniform
      ? sizes.length + "×" + sizes[0] + " ГБ = " + memory.totalGb + " ГБ"
      : sizes.map((size) => size + " ГБ").join(" + ");
    if (memory.type) text += " " + memory.type + "-" + Math.round(memory.configuredMt);
    if (memory.channels) text += memory.channels === 1 ? ", один канал" : ", " + memory.channels + " канала";
    return text;
  }

  function describeDisplay(display) {
    let text = display.name ? display.name + " · " : "";
    text += display.width && display.height ? display.width + "×" + display.height : "нет данных";
    if (display.hz) text += " · " + display.hz + " Гц";
    return text;
  }

  function value(id, fallback) {
    const sensor = state.sensors.get(id);
    return sensor && sensor.samples ? sensor.value : fallback;
  }

  // Калькулятор узкого места: живые загрузка и кадры либо прогноз по индексам.
  function balance() {
    const cpuIndex = cpuPerfIndex(MACHINE.cpu.name, MACHINE.cpu.cores, MACHINE.cpu.threads);
    const gpu = MACHINE.gpus.find((item) => item.discrete) || MACHINE.gpus[0];
    const gpuIndex = gpuPerfIndex(gpu.name, gpu.dedicatedMb);
    const display = MACHINE.displays.find((item) => item.primary) || MACHINE.displays[0];
    const pixels = display.width * display.height || 1920 * 1080;
    const result = {
      measured: false, percent: 0, kind: "no-data", headline: "Недостаточно данных",
      detail: "", advice: [], cpuCapacity: cpuIndex * 3.2, gpuCapacity: 0, expected: 0
    };
    result.gpuCapacity = gpuIndex * 2.6 * Math.pow((1920 * 1080) / pixels, 0.85);

    const cpuLoad = value("cpu", -1);
    // Пик по ядрам: в приложении это отдельный счётчик, здесь — максимум по восьми.
    let peakCore = -1;
    for (let index = 0; index < 8; index += 1) {
        const core = value(`core${index}`, -1);
        if (core > peakCore) peakCore = core;
    }
    const gpuLoad = value("gpu", -1);
    const fps = value("fps", -1);
    const vramUsed = value("vramUsed", -1);
    const vramTotal = gpu.memoryGb;
    const ramLoad = value("ramLoad", -1);
    const pageFileLoad = value("pageFileLoad", -1);
    const gpuPower = value("gpuPower", -1);

    const pressureCpu = Math.max(cpuLoad, peakCore * 0.9);
    const haveLive = (cpuLoad >= 0 || peakCore >= 0) && gpuLoad >= 0;
    const clampPercent = (x) => Math.max(0, Math.min(100, Math.round(x)));

    if (haveLive && Math.max(pressureCpu, gpuLoad) < 22 && !(fps > 0)) {
      result.kind = "idle";
      result.headline = "Нагрузки нет";
      result.detail = "Пока ни процессор, ни видеокарта не загружены, судить об узком месте не по чему. "
        + "Запустите игру — расчёт пересчитается по живым данным.";
      return result;
    }

    if (haveLive && Math.max(pressureCpu, gpuLoad) >= 22) {
      result.measured = true;
      result.expected = fps > 0 ? fps : 0;
      if (vramTotal > 0 && vramUsed >= vramTotal - 0.2) {
        result.kind = "vram";
        result.percent = 100;
        result.headline = "Не хватает видеопамяти";
        result.detail = "Видеопамять заполнена: " + vramUsed.toFixed(1) + " из " + vramTotal
          + " ГБ. Кадры упираются не в чип, а в обмен с ОЗУ — отсюда периодические провалы.";
        result.advice.push({ title: "Снизить качество текстур", text: "Текстуры — первый потребитель видеопамяти." });
        return result;
      }
      const target = FRAME_TARGET_HZ;
      if (fps > 0 && fps >= target * 0.96 && Math.max(pressureCpu, gpuLoad) < 85) {
        result.kind = "refresh";
        result.headline = "Кадры ограничены герцовкой или вертикальной синхронизацией";
        result.detail = "FPS держится на цели " + target + " при незагруженной видеокарте: узкого места в железе нет, лимит стоит сверху.";
        result.advice.push({ title: "Снять ограничение и включить G-SYNC/FreeSync",
          text: "Переменная герцовка уберёт разрывы без жёсткого лимита." });
        result.advice.push({ title: "Поднять качество вместо борьбы за FPS",
          text: "Пока кадр упирается в лимит, запас видеокарты можно потратить на настройки." });
        return result;
      }
      const high = Math.max(pressureCpu, gpuLoad);
      const low = Math.min(pressureCpu, gpuLoad);
      result.percent = clampPercent(100 * (1 - low / Math.max(high, 1)));
      if (high - low < 12) {
        result.kind = "balanced";
        result.headline = "Связка работает ровно";
        result.detail = "Процессор загружен на " + Math.round(pressureCpu) + " %, видеокарта — на "
          + Math.round(gpuLoad) + " %: оба узла близко к пределу, простой отсутствует.";
        result.advice.push({ title: "Апгрейд одного узла кадров не добавит",
          text: "Пока оба компонента загружены ровно, замена видеокарты упрётся в процессор, и наоборот." });
        if (ramLoad > 90) {
          result.advice.push({ title: "Проверить запас ОЗУ",
            text: "Память занята на " + Math.round(ramLoad) + " %, файл подкачки — на " + Math.round(pageFileLoad) + " %." });
        }
        return result;
      }
      if (pressureCpu > gpuLoad) {
        result.kind = "cpu";
        result.headline = "Упор в процессор";
        result.detail = "Процессор держит " + Math.round(pressureCpu) + " % (самое занятое ядро — "
          + Math.round(peakCore) + " %), видеокарта — только " + Math.round(gpuLoad) + "%. "
          + result.percent + "% мощности GPU простаивает.";
      } else {
        result.kind = "gpu";
        result.headline = "Упор в видеокарту";
        result.detail = "Видеокарта загружена на " + Math.round(gpuLoad) + " %, процессор — на "
          + Math.round(pressureCpu) + "%. " + result.percent + "% мощности CPU не используется: "
          + "это нормальный режим для тяжёлой графики.";
      }
      pushAdvice(result, gpu, gpuPower, vramUsed, vramTotal);
      return result;
    }

    if (cpuIndex <= 0 || gpuIndex <= 0) {
      return result;
    }
    const high = Math.max(result.cpuCapacity, result.gpuCapacity);
    const low = Math.min(result.cpuCapacity, result.gpuCapacity);
    result.expected = low;
    result.percent = clampPercent(100 * (1 - low / high) * 1.15);
    const resolution = display.width + "×" + display.height;
    if (result.percent < 12) {
      result.kind = "balanced";
      result.headline = "Связка сбалансирована";
      result.detail = "По индексу производительности при разрешении " + resolution
        + " оба узла приходят к потолку вместе. Оценка ±15 %: живые замеры точнее.";
      result.advice.push({ title: "Держать цель кадра по герцовке монитора",
        text: "Лишние кадры при незагруженной видеокарте только добавляют задержку и нагрев." });
      return result;
    }
    if (result.cpuCapacity < result.gpuCapacity) {
      result.kind = "cpu";
      result.headline = "Упор в процессор";
      result.detail = "По индексу производительности в разрешении " + resolution + " связка даёт ~"
        + Math.round(result.cpuCapacity) + " кадров: дальше видеокарта не разгонится, "
        + result.percent + "% её мощности будет простаивать.";
    } else {
      result.kind = "gpu";
      result.headline = "Упор в видеокарту";
      result.detail = "По индексу производительности в разрешении " + resolution + " потолок — видеокарта: ~"
        + Math.round(result.gpuCapacity) + " кадров, процессор загружен не будет.";
    }
    pushAdvice(result, gpu, gpuPower, vramUsed, vramTotal);
    return result;
  }

  function pushAdvice(result, gpu, gpuPower, vramUsed, vramTotal) {
    if (result.kind === "cpu") {
      result.advice.push({ title: "Повысить разрешение или включить DSR/VSR",
        text: "Пока узкое место — процессор, кадры ограничивает не видеокарта. Перенос нагрузки на GPU почти не "
          + "меняет средний FPS, но выравнивает frame time." });
      result.advice.push({ title: "Снизить настройки, которые считает процессор",
        text: "Дальность прорисовки, плотность растительности и NPC, физика и тени. Текстуры и фильтрация "
          + "при упоре в процессор FPS не добавляют." });
      result.advice.push({ title: "Ограничить FPS на 5–10 % ниже максимума",
        text: "Процессор перестанет работать на пределе, пропадут провалы: 1 % low поднимется ближе к среднему." });
      if (MACHINE.cpu.threads && MACHINE.cpu.threads <= 8) {
        result.advice.push({ title: "Игра упирается в малое число потоков",
          text: "На " + MACHINE.cpu.threads + " потоках движку не хватает однопоточной скорости." });
      }
      if (!MACHINE.memory.profile) {
        result.advice.push({ title: "Включить XMP/EXPO в BIOS",
          text: "Память работает на " + MACHINE.memory.configuredMt + " МТ/с вместо " + MACHINE.memory.ratedMt + "." });
      }
      if (MACHINE.memory.modules.filter((m) => m.populated).length === 1) {
        result.advice.push({ title: "Поставить вторую планку — два канала",
          text: "Одноканальный режим режет пропускную способность памяти вдвое." });
      }
      return;
    }
    result.advice.push({ title: "Включить DLSS, FSR или XeSS в режиме Quality",
      text: "Рендер в более низком разрешении снимает нагрузку с видеокарты: +20…50 % кадров." });
    result.advice.push({ title: "Снизить тяжёлые для GPU эффекты",
      text: "Трассировка лучей, глобальное освещение, качество теней — их снижение даёт больше всего кадров." });
    if (vramTotal > 0 && vramUsed > vramTotal * 0.85) {
      result.advice.push({ title: "Освободить видеопамять",
        text: "Занято " + vramUsed.toFixed(1) + " из " + vramTotal + " ГБ: текстуры уже уходят в файл подкачки." });
    }
    if (gpuPower > 0) {
      result.advice.push({ title: "Проверить лимит питания и температуры",
        text: "Видеокарта держит " + Math.round(gpuPower) + " Вт. Если частота ядра ниже паспортной, а температура "
          + "выше 83 °C — карта троттлит." });
    }
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
      sensors: state.sensors,
      machine: MACHINE,
      balance: balance()
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
    // Для скриншотов: останавливаем таймер, чтобы страница «успокоилась» и
    // headless-браузер снял кадр сразу, а не ждал виртуального времени.
    stop() {
      clearInterval(timer);
    },
    describeMemory: describeMemory,
    describeDisplay: describeDisplay,
    cpuPerfIndex: cpuPerfIndex,
    gpuPerfIndex: gpuPerfIndex,
    machine() {
      return MACHINE;
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
