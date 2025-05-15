<script setup>
import { onMounted, ref, watch } from "vue";
import Tooltip from "./components/Tooltip.vue";
import Popup from "./components/Popup.vue";
import { modes } from "./lib/modes.js";

const mode = ref(modes.automatic);
const popup = ref(false);
const settings = ref(null);
const isDebugging = ref(false);

let popupTimeout;

watch (mode, () => {
  popup.value = true;

  if (popupTimeout) {
    clearTimeout(popupTimeout);
  }

  popupTimeout = setTimeout(() => {
    popup.value = false;
  }, 750);
});

electron.on ('settings', (config) => {
  logger.debug (`Client received settings: ${JSON.stringify(config, null, 4)}`);
  settings.value = config;
  mode.value = modes [config.general.default_mode];
  document.documentElement.style.setProperty('--scale', config.general.scale);
});

electron.on ('manual:toggle', () => {
  switch (mode.value) {
    case modes.automatic:
      mode.value = modes.manual;
      break;

    case modes.manual:
      mode.value = modes.disabled;
      break;

    case modes.disabled:
      mode.value = modes.automatic;
      break;
  }

  logger.debug (`Changed mode to: ${mode.value}`);
});

electron.on("manual:debugger", () => {
  isDebugging.value = !isDebugging.value;
});

onMounted(() => {
  electron.send("ready");
});
</script>

<template>
  <teleport to="body">
    <div
      v-if="isDebugging"
      class="absolute bottom-0 left-0 right-0 top-0 border border-2 border-yellow-500 bg-red-500/5"
    ></div>
  </teleport>

  <div :class="{ debugging: isDebugging }" v-if="settings">
    <Tooltip
      :mode="mode"
      :alignment="settings.general.alignment"
      :components="settings.general.components"
    />

    <transition
      enter-active-class="transition-opacity duration-200"
      enter-from-class="opacity-0 scale-90"
      enter-to-class="opacity-100"
      leave-active-class="transition-opacity duration-200"
      leave-from-class="opacity-100"
      leave-to-class="opacity-0"
    >
      <Popup v-if="popup">
        <div>
          <span v-if="mode === modes.automatic"
            >GrimVault
            <span class="font-bold underline">Automatic Mode</span> activated.
            <br />
            <span class="text-base"
              >Hover over an item to perform a price check.</span
            ></span
          >
          <span v-if="mode === modes.manual"
            >GrimVault
            <span class="font-bold underline">Manual Mode</span> activated.
            <br />
            <span class="text-base"
              >Press F5 while hovering over an item to perform a price
              check.</span
            ></span
          >
          <span v-if="mode === modes.disabled"
            >GrimVault <span class="font-bold underline">Disabled</span>. <br />
            <span class="text-base"
              >No price checks will be ran until you re-active by pressing
              F6.</span
            ></span
          >
        </div>

        <ul class="dotted mt-3 justify-center text-sm text-gray-300">
          <li>
            <span
              :class="{ 'text-green-500 underline': mode === modes.automatic }"
              >Automatic</span
            >
          </li>
          <li>
            <span :class="{ 'text-green-500 underline': mode === modes.manual }"
              >Manual</span
            >
          </li>
          <li>
            <span
              :class="{ 'text-green-500 underline': mode === modes.disabled }"
              >Disabled</span
            >
          </li>
        </ul>
      </Popup>
    </transition>
  </div>
</template>
