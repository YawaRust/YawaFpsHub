/* Отрисовка графиков прототипа. В приложении те же формулы переносятся в Direct2D. */
(function () {
  "use strict";

  const registry = new Map();

  function prepare(canvas) {
    const rect = canvas.getBoundingClientRect();
    const width = Math.max(1, Math.round(rect.width));
    const height = Math.max(1, Math.round(rect.height));
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    if (canvas.width !== width * dpr || canvas.height !== height * dpr) {
      canvas.width = width * dpr;
      canvas.height = height * dpr;
    }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, width, height);
    return { ctx, width, height };
  }

  function drawGrid(ctx, width, height, rows, labels) {
    ctx.save();
    ctx.font = '9.5px "Segoe UI", system-ui, sans-serif';
    ctx.textBaseline = "middle";
    for (let i = 0; i <= rows; i += 1) {
      const y = (height / rows) * i;
      ctx.strokeStyle = i === rows ? "rgba(255,255,255,0.09)" : "rgba(255,255,255,0.045)";
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(0, Math.round(y) + 0.5);
      ctx.lineTo(width, Math.round(y) + 0.5);
      ctx.stroke();
      if (labels) {
        ctx.fillStyle = "rgba(255,255,255,0.3)";
        ctx.textAlign = "left";
        ctx.fillText(labels[i], 0, Math.max(7, y - 7));
      }
    }
    ctx.restore();
  }

  function tracePath(ctx, points) {
    ctx.beginPath();
    ctx.moveTo(points[0].x, points[0].y);
    for (let i = 1; i < points.length; i += 1) {
      const prev = points[i - 1];
      const point = points[i];
      const midX = (prev.x + point.x) / 2;
      ctx.bezierCurveTo(midX, prev.y, midX, point.y, point.x, point.y);
    }
  }

  function toPoints(values, width, height, min, max, inset) {
    const span = Math.max(0.0001, max - min);
    const innerHeight = Math.max(1, height - inset * 2);
    return values.map((value, index) => ({
      x: (index / Math.max(1, values.length - 1)) * width,
      y: inset + innerHeight - ((value - min) / span) * innerHeight
    }));
  }

  function alphaColor(hex, alpha) {
    const value = hex.replace("#", "");
    const r = parseInt(value.slice(0, 2), 16);
    const g = parseInt(value.slice(2, 4), 16);
    const b = parseInt(value.slice(4, 6), 16);
    return `rgba(${r},${g},${b},${alpha})`;
  }

  function drawSeries(ctx, values, width, height, color, options) {
    const min = options.min;
    const max = options.max;
    const points = toPoints(values, width, height, min, max, options.inset);

    if (options.fill) {
      const gradient = ctx.createLinearGradient(0, 0, 0, height);
      gradient.addColorStop(0, alphaColor(color, options.fill));
      gradient.addColorStop(1, alphaColor(color, 0));
      tracePath(ctx, points);
      ctx.lineTo(width, height);
      ctx.lineTo(0, height);
      ctx.closePath();
      ctx.fillStyle = gradient;
      ctx.fill();
    }

    tracePath(ctx, points);
    ctx.strokeStyle = color;
    ctx.lineWidth = options.lineWidth || 1.6;
    ctx.lineJoin = "round";
    ctx.lineCap = "round";
    ctx.stroke();

    if (options.glow) {
      ctx.save();
      ctx.globalCompositeOperation = "lighter";
      ctx.filter = "blur(6px)";
      ctx.strokeStyle = alphaColor(color, 0.35);
      ctx.lineWidth = 3;
      ctx.stroke();
      ctx.restore();
    }
  }

  function frame(canvas, options) {
    const { ctx, width, height } = prepare(canvas);
    const values = options.values;
    if (values.length < 2) return;

    const peak = Math.max(...values);
    const max = Math.max(peak * 1.3, 10);
    const min = 0;
    const inset = 6;

    drawGrid(ctx, width, height, 3, ["", "", "", ""]);
    drawSeries(ctx, values, width, height, options.color, { min, max, inset, fill: 0.22, lineWidth: 1.7, glow: true });

    const targetMs = 1000 / (options.targetHz || 120);
    const targetY = inset + (height - inset * 2) - ((targetMs - min) / (max - min)) * (height - inset * 2);
    if (targetY > inset && targetY < height - inset) {
      ctx.save();
      ctx.setLineDash([4, 5]);
      ctx.strokeStyle = "rgba(255,255,255,0.2)";
      ctx.lineWidth = 1;
      ctx.beginPath();
      ctx.moveTo(0, targetY);
      ctx.lineTo(width, targetY);
      ctx.stroke();
      ctx.restore();
    }

    const last = values[values.length - 1];
    const size = toPoints([last], width, height, min, max, inset)[0];
    ctx.beginPath();
    ctx.arc(size.x - 1.5, size.y, 2.6, 0, Math.PI * 2);
    ctx.fillStyle = options.color;
    ctx.shadowColor = options.color;
    ctx.shadowBlur = 9;
    ctx.fill();
    ctx.shadowBlur = 0;

    registry.set(canvas, { type: "frame", options });
  }

  function multi(canvas, options) {
    const { ctx, width, height } = prepare(canvas);
    drawGrid(ctx, width, height, 4, ["100", "75", "50", "25", "0"]);

    for (const series of options.series) {
      if (!series.visible || series.values.length < 2) continue;
      drawSeries(ctx, series.values, width, height, series.color, {
        min: 0,
        max: 100,
        inset: 4,
        fill: 0.1,
        lineWidth: 1.6,
        glow: true
      });
    }

    registry.set(canvas, { type: "multi", options });
  }

  function spark(canvas, options) {
    const { ctx, width, height } = prepare(canvas);
    const values = options.values.slice(-45);
    if (values.length < 2) return;

    const peak = Math.max(...values);
    const floor = Math.min(...values);
    const max = peak + (peak - floor) * 0.25 + 0.001;
    const min = Math.max(0, floor - (peak - floor) * 0.25);
    const points = toPoints(values, width, height, min, max, 2);

    const gradient = ctx.createLinearGradient(0, 0, 0, height);
    gradient.addColorStop(0, alphaColor(options.color, 0.34));
    gradient.addColorStop(1, alphaColor(options.color, 0));
    tracePath(ctx, points);
    ctx.lineTo(width, height);
    ctx.lineTo(0, height);
    ctx.closePath();
    ctx.fillStyle = gradient;
    ctx.fill();

    tracePath(ctx, points);
    ctx.strokeStyle = alphaColor(options.color, 0.9);
    ctx.lineWidth = 1.3;
    ctx.stroke();

    registry.set(canvas, { type: "spark", options });
  }

  const observer = new ResizeObserver((entries) => {
    for (const entry of entries) {
      const canvas = entry.target;
      const entryConfig = registry.get(canvas);
      if (!entryConfig) continue;
      if (entryConfig.type === "frame") frame(canvas, entryConfig.options);
      if (entryConfig.type === "multi") multi(canvas, entryConfig.options);
      if (entryConfig.type === "spark") spark(canvas, entryConfig.options);
    }
  });

  window.Charts = {
    frame,
    multi,
    spark,
    observe(canvas) {
      observer.observe(canvas);
    }
  };
})();
