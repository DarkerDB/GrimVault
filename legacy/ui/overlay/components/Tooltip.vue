<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref, watch } from "vue";
import {
  MOUSE_STILL_FOR_MS,
  MOUSE_WAKEUP_DISTANCE,
  EDGE_PADDING,
} from "../config.js";

import {
  onMouseStill,
  onMouseWakeup,
  setMouseSleepPosition,
} from "../lib/mouse.js";

import { modes } from "../lib/modes.js";
import { interpolateColor } from "../lib/util.js";

const props = defineProps({
  mode: {
    type: Number,
  },

  alignment: {
    type: String,
  },

  components: {
    type: Array,
  },

  debug: {
    type: Boolean,
    default: false,
  },
});

const isTooltipActive = ref(false);

const tooltipNode = ref(null);
const tooltipWidth = ref(0);
const tooltipHeight = ref(0);
const tooltipVisibility = ref("hidden");

// Track scan ID to ignore stale results
const currentScanId = ref(0);

// Track loading state for spinner
const isLoading = ref(false);
const errorMessage = ref(null);

// Window state for gating tooltip scans
const windowState = ref({
  canScan: false,
  visible: false,
  focused: false,
});

// Track mouse position during scan for motion compensation
const scanStartMousePos = ref({ x: 0, y: 0 });
const currentMousePos = ref({ x: 0, y: 0 });

const markerNode = ref(null);

const markerTop = ref(0);
const markerLeft = ref(0);
const markerWidth = ref(0);
const markerHeight = ref(0);

// The game bounds are necessary to determine the overlay offset since the screen
// capture provides absolute coordinates relative to the monitor the game is
// running on, not relative to the game window.
const gameBounds = ref(null);

electron.on("game:bounds", (bounds) => {
  gameBounds.value = bounds;
  logger.debug(`[COORDS] Game bounds received: x=${bounds.x}, y=${bounds.y}, width=${bounds.width}, height=${bounds.height}`);
  if (bounds.scale) {
    logger.debug(`[COORDS] DPI scale: ${bounds.scale}`);
  }
});

// Listen for window state updates to gate tooltip scanning
electron.on("game:state", (state) => {
  windowState.value = state;
  logger.debug(`Window state updated: canScan=${state.canScan}, visible=${state.visible}, focused=${state.focused}`);
});

const item = ref({
  attributes: {
    primary: [],
    secondary: [],
  },

  demand: null,
  quality: null,
  // relativeQuality: null,
  // numSimilarSoldRecently: null,
  adventurePoints: null,
  // experience: null,

  quests: [],

  prices: {
    market: null,
    live: null,
    vendor: null,
  },
});

const primary = computed(() =>
  item.value.attributes.primary.filter(
    (attribute) => attribute.min !== attribute.max,
  ),
);

// Computed property to determine if tooltip content should be shown
const shouldShowContent = computed(() => {
  // Only show content if we have marker position and actual content (not loading)
  return markerWidth.value > 0 && (errorMessage.value !== null || isTooltipActive.value);
});

// Watch for tooltip visibility changes to measure dimensions
watch([shouldShowContent], () => {
  nextTick(() => {
    if (tooltipNode.value) {
      const rect = tooltipNode.value.getBoundingClientRect();
      tooltipWidth.value = rect.width;
      tooltipHeight.value = rect.height;
      logger.debug(`Tooltip dimensions: ${tooltipWidth.value}x${tooltipHeight.value}`);
    }
  });
});

// Computed property for tooltip positioning
const PADDING = 10;
const tooltipPosition = computed(() => {
  if (!markerWidth.value || !tooltipWidth.value || !tooltipHeight.value) {
    return { left: 0, top: 0 };
  }

  const availableLeft = markerLeft.value;
  const neededSpace = tooltipWidth.value + PADDING;
  const shouldPlaceLeft = availableLeft >= neededSpace;

  // Position horizontally: left of marker (preferred) or right if not enough space
  const left = shouldPlaceLeft
    ? -(tooltipWidth.value + PADDING)
    : markerWidth.value + PADDING;

  // Position vertically: center align with marker
  const top = (markerHeight.value / 2) - (tooltipHeight.value / 2);

  // logger.debug(`Tooltip positioning: shouldPlaceLeft=${shouldPlaceLeft}, left=${left}, top=${top}`);

  return { left, top };
});

const scan = () => {
  if (props.mode === modes.disabled) {
    return;
  }

  // Gate scanning based on window state
  if (!windowState.value.canScan) {
    logger.debug("Scan blocked: game window not focused or not visible");
    return;
  }

  // Increment scan ID to track this scan
  const scanId = ++currentScanId.value;

  // Capture current mouse position at scan start for motion compensation
  scanStartMousePos.value = {
    x: currentMousePos.value.x,
    y: currentMousePos.value.y,
  };

  logger.debug(`Checking for tooltips (scan #${scanId})`);
  electron.send("scan", { scanId });
};

onMouseStill(() => {
  switch (props.mode) {
    case modes.automatic:
      scan();
      break;

    case modes.manual:
    case modes.disabled:
      break;
  }
}, MOUSE_STILL_FOR_MS);

onMouseWakeup(() => {
  isTooltipActive.value = false;
}, MOUSE_WAKEUP_DISTANCE);

electron.on("scan:start", () => {
  isLoading.value = true;
  errorMessage.value = null;
});

electron.on("clear", (data) => {
  // Ignore stale clear events
  if (data?.scanId && data.scanId !== currentScanId.value) {
    logger.debug(`Ignoring stale clear event (scanId ${data.scanId} vs current ${currentScanId.value})`);
    return;
  }

  isTooltipActive.value = false;
  isLoading.value = false;
  errorMessage.value = null;
});

electron.on("scan:finish", () => {
  setMouseSleepPosition();
});

electron.on("manual:scan", () => {
  if (props.mode === modes.manual) {
    scan();
  }
});

onMounted(() => {
  logger.info("Tooltip mounted");

  // Track current mouse position for motion compensation during scans
  window.addEventListener("mousemove", (event) => {
    currentMousePos.value = {
      x: event.clientX,
      y: event.clientY,
    };
  });

  electron.on("hover:item", async (data) => {
    // Ignore stale scan results
    if (data.scanId !== currentScanId.value) {
      logger.debug(`Ignoring stale scan result (scanId ${data.scanId} vs current ${currentScanId.value})`);
      return;
    }

    // Clear previous state
    isLoading.value = false;
    errorMessage.value = null;
    isTooltipActive.value = false;

    // Wait for DOM to update
    await nextTick();

    logger.debug(`[COORDS] Tooltip from native: x=${data.x}, y=${data.y}, width=${data.width}, height=${data.height}`);

    item.value.prices.market = data.pricing.market;
    item.value.prices.density = data.pricing.density;
    item.value.prices.vendor = data.pricing.vendor;

    // item.value.numSimilarSoldRecently = data.num_similar_sold_recently;
    // item.value.relativeQuality = data.relative_quality;
    item.value.demand = data.demand;
    item.value.quality = data.quality;
    item.value.adventurePoints = data.adventure_points;
    // item.value.experience = data.experience;

    item.value.quests = data.quests;
    item.value.attributes.primary = data.item.primary || [];
    item.value.attributes.secondary = data.item.secondary || [];

    // Calculate mouse movement during scan for motion compensation
    const mouseDeltaX = currentMousePos.value.x - scanStartMousePos.value.x;
    const mouseDeltaY = currentMousePos.value.y - scanStartMousePos.value.y;

    // Update the marker position with motion compensation
    // Coordinates from WGC window capture are already window-relative

    if (props.alignment === "attached") {
      markerTop.value = data.y + mouseDeltaY;
      markerLeft.value = data.x + mouseDeltaX;
      markerWidth.value = data.width;
      markerHeight.value = data.height;

      logger.debug(`[COORDS] Marker position set: left=${markerLeft.value}, top=${markerTop.value}, width=${markerWidth.value}, height=${markerHeight.value}`);
      logger.debug(`[COORDS] Marker final transform: translate(${markerLeft.value}px, ${markerTop.value}px)`);
      logger.debug(`[COORDS] Mouse delta: x=${mouseDeltaX}, y=${mouseDeltaY}`);
    }

    switch (props.alignment) {
      case "attached":
        break;

      case "top-right":
        markerTop.value = EDGE_PADDING;
        markerLeft.value = gameBounds.value.width - EDGE_PADDING;
        break;

      case "top-left":
        markerTop.value = EDGE_PADDING;
        markerLeft.value = EDGE_PADDING;
        break;

      case "bottom-right":
        markerTop.value = gameBounds.value.height - EDGE_PADDING;
        markerLeft.value = gameBounds.value.width - EDGE_PADDING;
        break;

      case "bottom-left":
        markerTop.value = gameBounds.value.height - EDGE_PADDING;
        markerLeft.value = EDGE_PADDING;
        break;
    }

    setMouseSleepPosition();

    isTooltipActive.value = true;
  });

  electron.on("hover:error", async (data) => {
    // Ignore stale scan results
    if (data.scanId !== currentScanId.value) {
      logger.debug(`Ignoring stale error (scanId ${data.scanId} vs current ${currentScanId.value})`);
      return;
    }

    isLoading.value = false;
    isTooltipActive.value = false;

    // Set error message and position
    errorMessage.value = data.message || "Unknown error occurred";

    const mouseDeltaX = currentMousePos.value.x - scanStartMousePos.value.x;
    const mouseDeltaY = currentMousePos.value.y - scanStartMousePos.value.y;

    if (props.alignment === "attached") {
      markerTop.value = data.y + mouseDeltaY;
      markerLeft.value = data.x + mouseDeltaX;
      markerWidth.value = data.width || 100;
      markerHeight.value = data.height || 50;
    }

    setMouseSleepPosition();
  });

  // If we are attached make small mouse movements adjust the marker position.
  if (props.alignment === "attached") {
    let previousMousePosition = null;

    window.addEventListener("mousemove", (event) => {
      let currentMousePosition = {
        x: event.clientX,
        y: event.clientY,
      };

      if (previousMousePosition) {
        markerLeft.value += currentMousePosition.x - previousMousePosition.x;
        markerTop.value += currentMousePosition.y - previousMousePosition.y;

        // marker.value.left += currentMousePosition.x - previousMousePosition.x;
        // marker.value.top  += currentMousePosition.y - previousMousePosition.y;

        // tooltipNode.value.style.left = parseFloat (tooltipNode.value.style.left) + (currentMousePosition.x - previousMousePosition.x);
        // tooltipNode.value.style.top = parseFloat (tooltipNode.value.style.top) + (currentMousePosition.y - previousMousePosition.y);
      }

      previousMousePosition = currentMousePosition;
    });
  }
});

onBeforeUnmount(() => {
  // Cleanup if needed
});

function getGradeColor(grade) {
  const colors = {
    S: "var(--dnd-unique)",
    A: "var(--dnd-legendary)",
    B: "var(--dnd-epic)",
    C: "var(--dnd-rare)",
    D: "var(--dnd-uncommon)",
    F: "var(--dnd-common)",
  };

  return colors[grade] || "inherit";
}
</script>

<template>
  <div
    ref="markerNode"
    class="absolute"
    :style="{
      // Transform: markerLeft/markerTop are exact window-relative coords from WGC
      // Position marker directly at the tooltip coordinates (no offset needed)
      transform: `translate(${markerLeft}px, ${markerTop}px)`,
    }"
  >
    <!-- Single Marker - green border on detected tooltip region (debug only) -->
    <div
      v-if="markerWidth > 0"
      id="marker"
      :class="{ 'border-2 border-green-500': props.debug }"
      :style="{
        width: `${markerWidth}px`,
        height: `${markerHeight}px`,
      }"
    ></div>

    <!-- Tooltip content - manually positioned relative to marker -->
    <transition
      enter-active-class="transition-opacity duration-200 ease-out"
      enter-from-class="opacity-0"
      enter-to-class="opacity-100"
      leave-active-class="transition-opacity duration-150 ease-in"
      leave-from-class="opacity-100"
      leave-to-class="opacity-0"
    >
      <div
        v-if="shouldShowContent"
        ref="tooltipNode"
        class="absolute"
        :class="{ 'border-2 border-yellow-500': props.debug }"
        :style="{
          left: `${tooltipPosition.left}px`,
          top: `${tooltipPosition.top}px`,
        }"
      >
      <!-- Error Tooltip -->
      <div v-if="errorMessage !== null" id="tooltip">
        <div class="tooltip-overlay"></div>
        <div class="tooltip-content">
          <div class="error-title-wrapper">
            <span class="error-icon">⚠</span>
            <span>Error</span>
          </div>
          <div class="tooltip-body">
            <div class="error-message">{{ errorMessage }}</div>
          </div>
        </div>
      </div>

      <!-- Regular Tooltip -->
      <div v-else-if="isTooltipActive" id="tooltip">
        <div class="tooltip-overlay"></div>
        <div class="tooltip-content">
          <div
            class="tooltip-title"
            v-if="props.components.includes('header')"
          >
            Item Statistics
          </div>

          <div
            class="tooltip-body"
            :class="{ 'mt-3': !props.components.includes('header') }"
          >
            <section
              v-if="props.components.includes('primary') && primary.length"
            >
              <div
                v-for="attribute in primary"
                class="[&:not(:last-child)]:pb-2"
              >
                <span v-if="attribute.min !== attribute.max"
                  >{{ attribute.display }} {{ attribute.min }} -
                  {{ attribute.max }}</span
                >
              </div>
              <div class="tooltip-separator"></div>
            </section>

            <section
              v-if="
                props.components.includes('secondary') &&
                item.attributes.secondary.length
              "
            >
              <div
                v-for="attribute in item.attributes.secondary"
                class="[&:not(:last-child)]:pb-2"
              >
                <span class="tooltip-attribute text-nowrap">
                  <span
                    >{{
                      (attribute.value > 0
                        ? "+"
                        : attribute.value < 0
                          ? "-"
                          : "") +
                      attribute.value +
                      (attribute.is_percentage ? "%" : "")
                    }}
                  </span>
                  <span>{{ attribute.display }}</span>
                </span>

                <div class="text-base">
                  ({{ attribute.min }} - {{ attribute.max }}) (<span
                    :style="`color: ${getGradeColor(attribute.grade)}`"
                    >{{ attribute.grade }}</span
                  >)
                </div>
              </div>
              <div class="tooltip-separator"></div>
            </section>

            <section
              v-if="
                props.components.includes('details') &&
                (item.quality ||
                  item.relativeQuality ||
                  item.demand ||
                  item.numSimilarSoldRecently)
              "
            >
              <div class="tooltip-stats">
                <!-- <div
                  v-if="item.numSimilarSoldRecently"
                  class="tooltip-stat"
                >
                  <span>Similar Sold Recently:</span>
                  <span>{{ item.numSimilarSoldRecently }}</span>
                </div> -->
                <div v-if="item.demand" class="tooltip-stat">
                  <span>Demand:</span>
                  <span
                    :style="{ color: interpolateColor(item.demand, 10) }"
                    >{{ item.demand }} / 10</span
                  >
                </div>
                <div v-if="item.adventurePoints" class="tooltip-stat">
                  <span>Adventure Points:</span>
                  <span>{{ item.adventurePoints }}</span>
                </div>
              </div>
              <div class="tooltip-separator"></div>
            </section>

            <section
              v-if="
                props.components.includes('quests') && item.quests.length
              "
            >
              <div class="text-lg">
                <span style="color: var(--dnd-feather)">Quest Item</span>

                <div v-for="quest in item.quests" class="text-nowrap">
                  <span style="color: var(--dnd-aqua)"
                    >{{ quest.merchant }}
                    <span style="color: var(--dnd-dust)"
                      >{{ quest.title }}:</span
                    ></span
                  >
                  <span class="ml-2">{{ quest.count }}x</span>
                </div>
              </div>
              <div class="tooltip-separator"></div>
            </section>

            <div
              class="mx-auto w-40"
              v-if="
                props.components.includes('pricing') &&
                (item.prices.market !== null || item.prices.vendor !== null)
              "
            >
              <div
                class="flex items-center"
                v-if="item.prices.market !== null"
              >
                <span>Market:</span>
                <span class="gold ml-2">{{ item.prices.market }}</span>
              </div>
              <div
                class="flex items-center"
                v-if="item.prices.vendor !== null"
              >
                <span>Vendor:</span>
                <span class="gold ml-2">{{ item.prices.vendor }}</span>
              </div>
              <div
                class="flex items-center"
                v-if="item.prices.density !== null"
              >
                <span>Density:</span>
                <span class="gold ml-2">{{ item.prices.density }}</span>
              </div>
            </div>

            <div class="tooltip-separator"></div>

            <div class="text-xs" style="color: var(--dnd-oak)">
              Powered by DarkerDB.com
            </div>
          </div>
        </div>
      </div>
      </div>
    </transition>
  </div>
</template>

<style scoped>
.error-title-wrapper {
  @apply py-3 text-[1.65rem] flex items-center justify-center gap-2;
  color: #ef4444;
}

.error-title-wrapper:after {
  @apply content-[''] block w-full h-1 mt-2 mb-1 absolute left-0;
  background-image: url('@assets/images/Tooltip_SeparatorThick.png');
  background-size: contain;
  background-position: center;
}

.error-icon {
  font-size: 1.5rem;
}

.error-message {
  @apply text-[1.15rem] text-center;
  color: #fecaca;
  line-height: 1.5;
}
</style>
