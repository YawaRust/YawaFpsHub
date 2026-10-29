document.documentElement.dataset.rtssInitialSparkline = String(Boolean(document.getElementById("osdSparklinePoints")));
window.addEventListener("error", (event) => {
  document.documentElement.dataset.rtssScriptError = `${event.message} ${event.filename}:${event.lineno}`;
  document.documentElement.dataset.rtssSparkline = String(Boolean(document.querySelector("#osdSparklinePoints")));
  document.documentElement.dataset.rtssPreviewChildren = String(document.querySelector("#osdPreview")?.children.length);
  document.documentElement.dataset.rtssMarkupSparkline = String(document.body.innerHTML.includes("osdSparklinePoints"));
});
document.addEventListener("DOMContentLoaded", () => {
  const result = document.documentElement.dataset;
  const bytes = (encoded) => Uint8Array.from(atob(encoded), (symbol) => symbol.charCodeAt(0));
  const waitFor = async (find) => {
    for (let attempt = 0; attempt < 30; ++attempt) {
      const match = find();
      if (match) return match;
      await new Promise((resolve) => setTimeout(resolve, 50));
    }
    throw new Error("слой RTSS не появился в демо");
  };
  const importLayout = (layout, png) => {
    const input = document.querySelector("#importOsdRecipeFile");
    const files = new DataTransfer();
    files.items.add(new File([bytes(layout)], "rtss_crop.ovl"));
    if (png) files.items.add(new File([bytes(png)], "rtss_crop.png", { type: "image/png" }));
    input.files = files.files;
    input.dispatchEvent(new Event("change", { bubbles: true }));
  };

  (async () => {
    window.confirm = () => true;
    result.rtssStage = "wide";
    importLayout(window.rtssSmoke.wide);
    const wide = await waitFor(() => [...document.querySelectorAll(".rtss-visual-layer")]
      .find((layer) => layer.textContent.includes("WWWWWWWWWW")));
    if (parseFloat(wide.style.width) < 500) throw new Error("узкий слой для Arial W");

    result.rtssStage = "image";
    importLayout(window.rtssSmoke.image, window.rtssSmoke.png);
    const picture = await waitFor(() => document.querySelector(".rtss-visual-layer img"));
    await picture.decode();
    if (!picture.isConnected || picture.naturalWidth !== 8 || picture.style.visibility !== "visible") {
      throw new Error("PNG RTSS удалён или не показан после загрузки");
    }

    document.querySelector('#osdPreview [data-metric="cpuTemp"]').textContent = "64 °C";
    const tagged = "[Master]\nImplementation=2\n[General]\nSources=1\nLayers=1\n"
      + "[Source0]\nName=CPU temperature\nProvider=HAL\nID=CPU temperature\n"
      + "[Layer0]\nText=CPU %CPU temperature% C\n";
    result.rtssStage = "tagged";
    importLayout(btoa(tagged));
    const value = await waitFor(() => [...document.querySelectorAll(".rtss-visual-layer")]
      .find((layer) => layer.textContent.startsWith("CPU ")));
    if (value.textContent !== "CPU 64 C") throw new Error("единицы RTSS-датчика продублированы");
    result.rtssSmoke = "pass";
  })().catch((error) => {
    result.rtssSmoke = "fail";
    result.rtssError = error.message;
    result.rtssToast = document.querySelector("#toast")?.textContent || "";
  });
});
