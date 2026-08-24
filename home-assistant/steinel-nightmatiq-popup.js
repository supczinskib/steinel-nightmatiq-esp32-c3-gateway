(() => {
  "use strict";

  if (window.__steinelNightmatiqPopupInstalled) return;
  window.__steinelNightmatiqPopupInstalled = true;

  const ENTITIES = Object.freeze({
    output:
      "binary_sensor.steinel_nightmatiq_plus_nightmatiq_actual_light_output",
    illuminance:
      "sensor.steinel_nightmatiq_plus_nightmatiq_illuminance",
    threshold:
      "number.steinel_nightmatiq_plus_nightmatiq_twilight_threshold",
    mode: "select.steinel_nightmatiq_plus_nightmatiq_mode",
  });
  const SUMMARY_ENTITIES = Object.freeze({
    pl: "sensor.steinel_nightmatiq_stan_sensora",
    en: "sensor.steinel_nightmatiq_sensor_state",
  });
  const MODE_VALUES = new Set(["Auto", "Always On", "Always Off"]);
  const TRANSLATIONS = Object.freeze({
    pl: Object.freeze({
      locale: "pl-PL",
      areaFallback: "Ogród",
      close: "Zamknij",
      sensorState: "Stan sensora",
      illuminance: "Natężenie oświetlenia",
      operatingMode: "Tryb pracy",
      auto: "Auto",
      alwaysOn: "Zawsze włączone",
      alwaysOff: "Zawsze wyłączone",
      twilightThreshold: "Próg zmierzchowy",
      thresholdAria: "Próg zmierzchowy",
      thresholdLuxAria: "Próg zmierzchowy w luksach",
      unavailable: "Niedostępny",
      on: "Włączony",
      off: "Wyłączony",
    }),
    en: Object.freeze({
      locale: "en-GB",
      areaFallback: "Garden",
      close: "Close",
      sensorState: "Sensor state",
      illuminance: "Illuminance",
      operatingMode: "Operating mode",
      auto: "Auto",
      alwaysOn: "Always on",
      alwaysOff: "Always off",
      twilightThreshold: "Twilight threshold",
      thresholdAria: "Twilight threshold",
      thresholdLuxAria: "Twilight threshold in lux",
      unavailable: "Unavailable",
      on: "On",
      off: "Off",
    }),
  });

  const OPTIMISTIC_CONFIRMATION_TIMEOUT_MS = 5000;
  const LEGACY_TILE_CARD_TYPE = "custom:steinel-nightmatiq-tile";
  const AREA_STRATEGY_PATCH_FLAG = "__steinelNightmatiqAreaStrategyPatched";
  const AREA_VIEW_REPAIR_DELAYS = [0, 100, 500, 1500];
  let areaViewRepairGeneration = 0;

  const getHass = () => document.querySelector("home-assistant")?.hass;

  const languageFor = (hass) => {
    const configured =
      hass?.locale?.language ?? hass?.language ?? navigator.language ?? "en";
    return String(configured).toLowerCase().startsWith("pl") ? "pl" : "en";
  };

  const translationsFor = (hass) => TRANSLATIONS[languageFor(hass)];

  const summaryEntityFor = (hass) => SUMMARY_ENTITIES[languageFor(hass)];

  const openSteinelPopup = () => {
    document.querySelector("steinel-nightmatiq-popup")?.remove();
    document.body.appendChild(document.createElement("steinel-nightmatiq-popup"));
  };

  const numberState = (hass, entityId) => {
    const value = Number.parseFloat(hass?.states?.[entityId]?.state);
    return Number.isFinite(value) ? value : null;
  };

  const formatNumber = (value, locale, digits = 2) =>
    value === null
      ? "—"
      : new Intl.NumberFormat(locale, {
          minimumFractionDigits: 0,
          maximumFractionDigits: digits,
        }).format(value);

  class SteinelNightmatiqPopup extends HTMLElement {
    constructor() {
      super();
      this.attachShadow({ mode: "open" });
      this.pollTimer = null;
      this.modeConfirmationTimer = null;
      this.thresholdConfirmationTimer = null;
      this.modeRequestSequence = 0;
      this.thresholdRequestSequence = 0;
      this.optimisticMode = null;
      this.optimisticThreshold = null;
      this.thresholdEditing = false;
      this.thresholdDraft = null;
      this.text = TRANSLATIONS.en;
      this.onKeyDown = (event) => {
        if (event.key === "Escape") this.close();
      };
    }

    connectedCallback() {
      this.render();
      this.updateState();
      this.pollTimer = window.setInterval(() => this.updateState(), 500);
      window.addEventListener("keydown", this.onKeyDown);
    }

    disconnectedCallback() {
      if (this.pollTimer !== null) window.clearInterval(this.pollTimer);
      if (this.modeConfirmationTimer !== null)
        window.clearTimeout(this.modeConfirmationTimer);
      if (this.thresholdConfirmationTimer !== null)
        window.clearTimeout(this.thresholdConfirmationTimer);
      window.removeEventListener("keydown", this.onKeyDown);
    }

    close() {
      this.remove();
    }

    async setMode(mode) {
      const hass = getHass();
      if (!hass || !MODE_VALUES.has(mode)) return;

      const requestSequence = ++this.modeRequestSequence;
      this.optimisticMode = mode;
      if (this.modeConfirmationTimer !== null)
        window.clearTimeout(this.modeConfirmationTimer);
      this.modeConfirmationTimer = window.setTimeout(() => {
        if (requestSequence !== this.modeRequestSequence) return;
        this.optimisticMode = null;
        this.modeConfirmationTimer = null;
        this.updateState();
      }, OPTIMISTIC_CONFIRMATION_TIMEOUT_MS);
      this.updateState();

      try {
        await hass.callService(
          "select",
          "select_option",
          { option: mode },
          { entity_id: ENTITIES.mode },
        );
      } catch (_error) {
        if (requestSequence !== this.modeRequestSequence) return;
        this.optimisticMode = null;
        window.clearTimeout(this.modeConfirmationTimer);
        this.modeConfirmationTimer = null;
        this.updateState();
      }
    }

    async setThreshold(value) {
      const hass = getHass();
      const parsed = Number.parseInt(value, 10);
      if (!hass || !Number.isFinite(parsed)) return;

      const requestedValue = Math.min(1500, Math.max(1, parsed));
      this.thresholdEditing = false;
      this.thresholdDraft = null;
      const requestSequence = ++this.thresholdRequestSequence;
      this.optimisticThreshold = requestedValue;
      if (this.thresholdConfirmationTimer !== null)
        window.clearTimeout(this.thresholdConfirmationTimer);
      this.thresholdConfirmationTimer = window.setTimeout(() => {
        if (requestSequence !== this.thresholdRequestSequence) return;
        this.optimisticThreshold = null;
        this.thresholdConfirmationTimer = null;
        this.updateState();
      }, OPTIMISTIC_CONFIRMATION_TIMEOUT_MS);
      this.updateState();

      try {
        await hass.callService(
          "number",
          "set_value",
          { value: requestedValue },
          { entity_id: ENTITIES.threshold },
        );
      } catch (_error) {
        if (requestSequence !== this.thresholdRequestSequence) return;
        this.optimisticThreshold = null;
        window.clearTimeout(this.thresholdConfirmationTimer);
        this.thresholdConfirmationTimer = null;
        this.updateState();
      }
    }

    render() {
      const hass = getHass();
      const text = translationsFor(hass);
      const areaId = getSteinelArea(hass);
      const areaName = hass?.areas?.[areaId]?.name ?? text.areaFallback;
      this.text = text;
      this.shadowRoot.innerHTML = `
        <style>
          :host {
            --nmq-active-blue: #2196f3;
            --nmq-inactive-background: #eeeeee;
            --nmq-sensor-on-background: #fff2d3;
            position: fixed;
            inset: 0;
            z-index: 10000;
            display: grid;
            place-items: center;
            padding: 24px;
            box-sizing: border-box;
            color: var(--primary-text-color, #212121);
            font-family: var(--paper-font-body1_-_font-family, Roboto, sans-serif);
          }

          .backdrop {
            position: absolute;
            inset: 0;
            background: rgb(0 0 0 / 38%);
            backdrop-filter: blur(1px);
          }

          .dialog {
            position: relative;
            width: min(680px, calc(100vw - 48px));
            max-height: calc(100vh - 48px);
            overflow: auto;
            box-sizing: border-box;
            border-radius: 28px;
            background: var(--card-background-color, #fff);
            box-shadow: 0 18px 55px rgb(0 0 0 / 32%);
            padding: 24px 28px 30px;
          }

          .header {
            display: grid;
            grid-template-columns: 48px minmax(0, 1fr);
            align-items: center;
            margin-bottom: 18px;
          }

          .close {
            width: 44px;
            height: 44px;
            border: 0;
            border-radius: 50%;
            background: transparent;
            color: var(--primary-text-color, #212121);
            font-size: 34px;
            font-weight: 300;
            line-height: 1;
            cursor: pointer;
          }

          .close:hover { background: var(--secondary-background-color, #f2f2f2); }
          .area { color: var(--secondary-text-color, #727272); font-size: 14px; }
          h1 { margin: 2px 0 0; font-size: 24px; font-weight: 500; line-height: 1.2; }

          .summary {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 12px;
            margin: 8px 0 24px;
          }

          .summary-card {
            display: grid;
            grid-template-columns: 40px 1fr;
            align-items: center;
            min-height: 82px;
            padding: 12px 16px;
            box-sizing: border-box;
            border-radius: 18px;
            background: var(--nmq-inactive-background);
          }

          .summary-card.output-on { background: var(--nmq-sensor-on-background); }

          .icon {
            display: flex;
            align-self: stretch;
            align-items: center;
            justify-content: center;
            width: 28px;
            color: var(--nmq-active-blue);
          }

          .icon ha-icon {
            width: 28px;
            height: 28px;
            --mdc-icon-size: 28px;
          }

          .label { color: var(--secondary-text-color, #727272); font-size: 13px; }
          .value { margin-top: 3px; font-size: 22px; font-weight: 500; }

          .section {
            padding: 18px 0;
            border-top: 1px solid var(--divider-color, #e0e0e0);
          }

          .section-title { margin-bottom: 12px; font-size: 15px; font-weight: 500; }

          .modes {
            display: grid;
            grid-template-columns: repeat(3, 1fr);
            gap: 8px;
          }

          .mode {
            min-height: 46px;
            padding: 8px 10px;
            border: 1px solid var(--nmq-inactive-background);
            border-radius: 14px;
            background: var(--nmq-inactive-background);
            color: var(--primary-text-color, #212121);
            font: inherit;
            cursor: pointer;
          }

          .mode.active {
            border-color: var(--nmq-active-blue);
            background: var(--nmq-active-blue);
            color: #fff;
            font-weight: 500;
          }

          .threshold-row {
            display: grid;
            grid-template-columns: minmax(0, 1fr) 94px;
            gap: 18px;
            align-items: center;
          }

          input[type="range"] { width: 100%; accent-color: var(--nmq-active-blue); }

          .number-wrap {
            display: flex;
            align-items: center;
            gap: 6px;
            padding: 9px 10px;
            border: 1px solid var(--divider-color, #dadce0);
            border-radius: 12px;
          }

          input[type="number"] {
            min-width: 0;
            width: 100%;
            border: 0;
            outline: 0;
            background: transparent;
            color: var(--primary-text-color, #212121);
            font: inherit;
            text-align: right;
          }

          .unit { color: var(--secondary-text-color, #727272); }

          @media (max-width: 600px) {
            :host { padding: 0; align-items: end; }
            .dialog {
              width: 100vw;
              max-height: 92vh;
              border-radius: 28px 28px 0 0;
              padding: 20px 18px 26px;
            }
            .summary { grid-template-columns: 1fr; }
            .modes { grid-template-columns: 1fr; }
          }
        </style>

        <div class="backdrop" aria-hidden="true"></div>
        <section class="dialog" role="dialog" aria-modal="true" aria-labelledby="title">
          <header class="header">
            <button class="close" type="button" aria-label="${text.close}">×</button>
            <div><div class="area"></div><h1 id="title">Steinel NightmatIQ Plus</h1></div>
          </header>

          <div class="summary">
            <div id="outputCard" class="summary-card">
              <div class="icon"><ha-icon icon="mdi:toggle-switch-outline"></ha-icon></div>
              <div><div class="label">${text.sensorState}</div><div id="output" class="value">—</div></div>
            </div>
            <div class="summary-card">
              <div class="icon"><ha-icon icon="mdi:weather-sunny"></ha-icon></div>
              <div><div class="label">${text.illuminance}</div><div id="illuminance" class="value">—</div></div>
            </div>
          </div>

          <div class="section">
            <div class="section-title">${text.operatingMode}</div>
            <div class="modes">
              <button class="mode" type="button" data-mode="Auto">${text.auto}</button>
              <button class="mode" type="button" data-mode="Always On">${text.alwaysOn}</button>
              <button class="mode" type="button" data-mode="Always Off">${text.alwaysOff}</button>
            </div>
          </div>

          <div class="section">
            <div class="section-title">${text.twilightThreshold}</div>
            <div class="threshold-row">
              <input id="thresholdRange" type="range" min="1" max="1500" step="1" aria-label="${text.thresholdAria}">
              <label class="number-wrap">
                <input id="thresholdNumber" type="number" min="1" max="1500" step="1" aria-label="${text.thresholdLuxAria}">
                <span class="unit">lx</span>
              </label>
            </div>
          </div>
        </section>
      `;

      this.shadowRoot.querySelector(".area").textContent = areaName;
      this.shadowRoot.querySelector(".backdrop").addEventListener("click", () => this.close());
      this.shadowRoot.querySelector(".close").addEventListener("click", () => this.close());
      this.shadowRoot.querySelectorAll(".mode").forEach((button) =>
        button.addEventListener("click", () => this.setMode(button.dataset.mode)),
      );

      const range = this.shadowRoot.querySelector("#thresholdRange");
      const number = this.shadowRoot.querySelector("#thresholdNumber");
      const updateThresholdDraft = (value) => {
        const parsed = Number.parseInt(value, 10);
        if (!Number.isFinite(parsed)) return;
        const draft = Math.min(1500, Math.max(1, parsed));
        this.thresholdEditing = true;
        this.thresholdDraft = draft;
        range.value = String(draft);
        number.value = String(draft);
      };
      const commitThresholdDraft = () => {
        if (!this.thresholdEditing || this.thresholdDraft === null) return;
        const draft = this.thresholdDraft;
        this.thresholdEditing = false;
        this.thresholdDraft = null;
        this.setThreshold(draft);
      };

      range.addEventListener("pointerdown", () => updateThresholdDraft(range.value));
      range.addEventListener("input", () => updateThresholdDraft(range.value));
      range.addEventListener("change", commitThresholdDraft);
      range.addEventListener("pointercancel", () => {
        this.thresholdEditing = false;
        this.thresholdDraft = null;
        this.updateState();
      });
      number.addEventListener("focus", () => updateThresholdDraft(number.value));
      number.addEventListener("input", () => updateThresholdDraft(number.value));
      number.addEventListener("change", commitThresholdDraft);
      number.addEventListener("blur", commitThresholdDraft);
    }

    updateState() {
      const hass = getHass();
      if (!hass) return;

      const outputState = hass.states[ENTITIES.output]?.state;
      const isOn = outputState === "on";
      const unavailable = outputState === "unavailable" || outputState === "unknown" || outputState === undefined;
      const output = this.shadowRoot.querySelector("#output");
      const outputCard = this.shadowRoot.querySelector("#outputCard");
      output.textContent = unavailable ? this.text.unavailable : isOn ? this.text.on : this.text.off;
      outputCard.classList.toggle("output-on", isOn);

      const lux = numberState(hass, ENTITIES.illuminance);
      this.shadowRoot.querySelector("#illuminance").textContent = `${formatNumber(lux, this.text.locale)} lx`;

      const observedMode = hass.states[ENTITIES.mode]?.state;
      if (this.optimisticMode !== null && observedMode === this.optimisticMode) {
        this.optimisticMode = null;
        window.clearTimeout(this.modeConfirmationTimer);
        this.modeConfirmationTimer = null;
      }
      const mode = this.optimisticMode ?? observedMode;
      this.shadowRoot.querySelectorAll(".mode").forEach((button) =>
        button.classList.toggle("active", button.dataset.mode === mode),
      );

      const observedThreshold = numberState(hass, ENTITIES.threshold);
      if (this.optimisticThreshold !== null &&
          observedThreshold !== null &&
          Math.abs(observedThreshold - this.optimisticThreshold) < 0.01) {
        this.optimisticThreshold = null;
        window.clearTimeout(this.thresholdConfirmationTimer);
        this.thresholdConfirmationTimer = null;
      }
      const threshold = this.optimisticThreshold ?? observedThreshold;
      const range = this.shadowRoot.querySelector("#thresholdRange");
      const number = this.shadowRoot.querySelector("#thresholdNumber");
      if (threshold !== null && !this.thresholdEditing) {
        range.value = String(threshold);
        number.value = String(threshold);
      }
    }
  }

  const isSteinelOutputCard = (card) => {
    if (!card || typeof card !== "object") return false;
    if (card.entity === ENTITIES.output || card.type === LEGACY_TILE_CARD_TYPE) return true;
    return card.type === "conditional" && isSteinelOutputCard(card.card);
  };

  const isSteinelSummaryCard = (card) => {
    if (!card || typeof card !== "object") return false;
    if (Object.values(SUMMARY_ENTITIES).includes(card.entity)) return true;
    return card.type === "conditional" && isSteinelSummaryCard(card.card);
  };

  const steinelTileConfig = (condition, color, hass) => ({
    type: "conditional",
    grid_options: { columns: 6 },
    conditions: [{ entity: ENTITIES.output, ...condition }],
    card: {
      type: "tile",
      entity: summaryEntityFor(hass),
      name: translationsFor(hass).sensorState,
      color,
      vertical: false,
      tap_action: { action: "more-info" },
      icon_tap_action: { action: "more-info" },
    },
  });

  const steinelTileConfigs = (hass) => [
    steinelTileConfig({ state: "on" }, "amber", hass),
    steinelTileConfig({ state_not: "on" }, "grey", hass),
  ];

  const replaceSteinelOutputCard = (sections, hass) =>
    sections.map((section) => {
      if (!Array.isArray(section.cards)) return section;
      let changed = false;
      const cards = section.cards.flatMap((card) => {
        if (isSteinelSummaryCard(card)) return [card];
        if (!isSteinelOutputCard(card)) return [card];
        changed = true;
        return steinelTileConfigs(hass);
      });
      return changed ? { ...section, cards } : section;
    });

  const getSteinelArea = (hass) => {
    const entry = hass?.entities?.[ENTITIES.output];
    const device = entry?.device_id ? hass.devices?.[entry.device_id] : undefined;
    return entry?.area_id ?? device?.area_id;
  };

  const findElementsInOpenShadowRoots = (root, selector, found = []) => {
    if (typeof root?.querySelectorAll !== "function") return found;
    found.push(...root.querySelectorAll(selector));
    for (const element of root.querySelectorAll("*")) {
      if (element.shadowRoot)
        findElementsInOpenShadowRoots(element.shadowRoot, selector, found);
    }
    return found;
  };

  const hasSteinelTile = (viewConfig) =>
    viewConfig?.sections?.some((section) =>
      section.cards?.some(isSteinelSummaryCard),
    );

  const repairActiveSteinelAreaView = () => {
    if (!window.location.pathname.startsWith("/home/areas-")) return true;
    const views = findElementsInOpenShadowRoots(document, "hui-view");
    for (const view of views) {
      const rawViewConfig = view.lovelace?.config?.views?.[view.index];
      if (rawViewConfig?.strategy?.type !== "home-area") continue;
      const steinelArea = getSteinelArea(view.hass);
      if (!steinelArea || rawViewConfig.strategy.area !== steinelArea) continue;
      if (hasSteinelTile(view._config)) return true;
      if (typeof view._initializeConfig === "function") {
        Promise.resolve(view._initializeConfig()).catch((error) => {
          console.error("Could not refresh the Steinel tile.", error);
        });
        return true;
      }
    }
    return false;
  };

  const installSteinelAreaStrategyPatch = async () => {
    const strategy = await customElements.whenDefined("home-area-view-strategy");
    if (
      !strategy ||
      (typeof strategy[AREA_STRATEGY_PATCH_FLAG] === "function" &&
        strategy.generate === strategy[AREA_STRATEGY_PATCH_FLAG])
    ) return;
    const originalGenerate = strategy.generate;
    if (typeof originalGenerate !== "function") return;
    strategy.generate = async function (config, hass) {
      const view = await originalGenerate.call(this, config, hass);
      const steinelArea = getSteinelArea(hass);
      if (!steinelArea || config.area !== steinelArea || !Array.isArray(view.sections))
        return view;
      return { ...view, sections: replaceSteinelOutputCard(view.sections, hass) };
    };
    strategy[AREA_STRATEGY_PATCH_FLAG] = strategy.generate;
  };

  const scheduleSteinelAreaViewRepair = () => {
    const generation = ++areaViewRepairGeneration;
    for (const delay of AREA_VIEW_REPAIR_DELAYS) {
      window.setTimeout(async () => {
        if (
          generation !== areaViewRepairGeneration ||
          document.visibilityState === "hidden"
        ) return;
        await installSteinelAreaStrategyPatch();
        if (repairActiveSteinelAreaView()) areaViewRepairGeneration += 1;
      }, delay);
    }
  };

  customElements.define("steinel-nightmatiq-popup", SteinelNightmatiqPopup);

  window.addEventListener(
    "hass-more-info",
    (event) => {
      const entityId = event.detail?.entityId ?? event.detail?.entity_id;
      if (entityId !== ENTITIES.output && !Object.values(SUMMARY_ENTITIES).includes(entityId)) return;
      event.preventDefault();
      event.stopImmediatePropagation();
      openSteinelPopup();
    },
    true,
  );

  window.addEventListener("location-changed", scheduleSteinelAreaViewRepair);
  window.addEventListener("popstate", scheduleSteinelAreaViewRepair);
  window.addEventListener("pageshow", scheduleSteinelAreaViewRepair);
  document.addEventListener("visibilitychange", () => {
    if (document.visibilityState === "visible") scheduleSteinelAreaViewRepair();
  });
  scheduleSteinelAreaViewRepair();
})();
