<script setup>
import { computed, nextTick, onBeforeUnmount, onMounted, ref } from "vue";
import { createPopper } from "@popperjs/core";
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
});

const popperNode = ref(null);

const isTooltipActive = ref(false);

const tooltipNode = ref(null);
const tooltipVisibility = ref("hidden");

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
});

const item = ref({
  attributes: {
    primary: [],
    secondary: [],
  },

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

const scan = () => {
  if (props.mode === modes.disabled) {
    return;
  }

  logger.debug("Checking for tooltips");
  electron.send("scan");
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

electron.on("clear", () => {
  isTooltipActive.value = false;
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

  electron.on("hover:item", async (data) => {
    isTooltipActive.value = false;
    // if (!isTooltipActive.value) {
    //   tooltipVisibility.value = 'hidden';
    // }

    item.value.prices.market = data.pricing.market;
    item.value.prices.vendor = data.pricing.vendor;

    // item.value.numSimilarSoldRecently = data.num_similar_sold_recently;
    // item.value.relativeQuality = data.relative_quality;
    item.value.quality = data.quality;
    item.value.adventurePoints = data.adventure_points;
    // item.value.experience = data.experience;

    item.value.quests = data.quests;
    item.value.attributes.primary = data.item.primary || [];
    item.value.attributes.secondary = data.item.secondary || [];

    // Update the marker position.

    if (props.alignment === "attached") {
      markerTop.value = data.y - (gameBounds.value ? gameBounds.value.y : 0);
      markerLeft.value = data.x - (gameBounds.value ? gameBounds.value.x : 0);
      markerWidth.value = data.width;
      markerHeight.value = data.height;
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

    // setTimeout (async () => {
    // await popper.update ();
    // tooltipVisibility.value = 'visible';
    // }, 25);

    setMouseSleepPosition();

    isTooltipActive.value = true;
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
  if (popper) {
    popper.destroy();
  }
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
      transform: `translate(${markerLeft}px, ${markerTop + (markerHeight ? markerHeight / 2 : 0)}px)`,
    }"
  >
    <Popper
      placement="left"
      offsetDistance="5"
      :show="isTooltipActive"
      ref="popperNode"
    >
      <div
        id="marker"
        class="h-[1px]"
        :style="{
          width: `${markerWidth}px`,
        }"
      ></div>

      <template #content>
        <transition
          enter-active-class="transition-opacity ease-out duration-200"
          enter-from-class="opacity-0 scale-90"
          enter-to-class="opacity-100 scale-100"
          leave-active-class="transition-opacity ease-in duration-200"
          leave-from-class="opacity-100 scale-100"
          leave-to-class="opacity-0 scale-90"
        >
          <div id="tooltip" ref="tooltipNode">
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
                </div>

                <div class="tooltip-separator"></div>

                <div class="text-xs" style="color: var(--dnd-oak)">
                  Powered by DarkerDB.com
                </div>
              </div>
            </div>
          </div>
        </transition>
      </template>
    </Popper>
  </div>
</template>
