<template>
  <div ref="marker" class="absolute"></div>

  <transition
    enter-active-class="transition-opacity ease-out duration-200"
    enter-from-class="opacity-0 scale-90"
    enter-to-class="opacity-100 scale-100"
    leave-active-class="transition-opacity ease-in duration-200"
    leave-from-class="opacity-100 scale-100"
    leave-to-class="opacity-0 scale-90"
  >
    <div v-show="open" id="tooltip" ref="tooltip">
      <div class="tooltip-overlay"></div>
      <div class="tooltip-content">
        <div class="tooltip-title">Item Statistics</div>

        <div class="tooltip-body">
          <section v-if="primary.length">
            <div v-for="attribute in primary" class="[&:not(:last-child)]:pb-2">
              <span v-if="attribute.min !== attribute.max"
                >{{ attribute.display }} {{ attribute.min }} -
                {{ attribute.max }}</span
              >
            </div>
            <div class="tooltip-separator"></div>
          </section>

          <section v-if="item.attributes.secondary.length">
            <div
              v-for="attribute in item.attributes.secondary"
              class="[&:not(:last-child)]:pb-2"
            >
              <span class="tooltip-attribute">
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
              item.quality ||
              item.relativeQuality ||
              item.numSimilarSoldRecently
            "
          >
            <div class="tooltip-stats">
              <div v-if="item.quality" class="tooltip-stat">
                <span>Quality:</span> <span>{{ item.quality }}</span>
              </div>
              <div v-if="item.relativeQuality" class="tooltip-stat">
                <span>Relative Quality:</span>
                <span>{{ item.relativeQuality }}</span
                >%
              </div>
              <div v-if="item.numSimilarSoldRecently" class="tooltip-stat">
                <span>Similar Sold Recently:</span>
                <span>{{ item.numSimilarSoldRecently }}</span>
              </div>
              <div v-if="item.adventurePoints" class="tooltip-stat">
                <span>Adventure Points:</span>
                <span>{{ item.adventurePoints }}</span>
              </div>
            </div>
            <div class="tooltip-separator"></div>
          </section>

          <div
            class="mx-auto w-40"
            v-if="item.prices.market || item.prices.vendor"
          >
            <div class="flex" v-if="item.prices.market">
              <div class="w-[90px] text-left">Market:</div>
              <span class="gold">{{ item.prices.market }}</span>
            </div>
            <div class="flex" v-if="item.prices.vendor">
              <div class="w-[90px] text-left">Vendor:</div>
              <span class="gold">{{ item.prices.vendor }}</span>
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

<script setup>
import { createPopper } from "@popperjs/core";
import { computed, onMounted, ref } from "vue";
import { MOUSE_STILL_FOR_MS, MOUSE_WAKEUP_DISTANCE } from "../config.js";
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
});

const open = ref(false);

const marker = ref(null);
const tooltip = ref(null);

const item = ref({
  attributes: {
    primary: [],
    secondary: [],
  },

  quality: null,
  relativeQuality: null,
  numSimilarSoldRecently: null,
  adventurePoints: null,
  experience: null,

  prices: {
    market: null,
    vendor: null,
  },
});

const primary = computed(() =>
  item.value.attributes.primary.filter(
    (attribute) => attribute.min !== attribute.max,
  ),
);

onMounted(() => {
  const popper = createPopper(marker.value, tooltip.value, {
    placement: "left",
    modifiers: [
      {
        name: "preventOverflow",
        options: {
          boundary: "viewport", // Ensures it considers the entire screen
          padding: 8,
        },
      },

      {
        name: "flip",
        options: {
          fallbackPlacements: ["left", "right", "bottom", "top"],
        },
      },

      {
        name: "offset",
        options: {
          offset: [0, 5], // [skidding, distance]
        },
      },
    ],
  });

  electron.on("hover:item", async (data) => {
    if (!open.value) {
      tooltip.value.style.visibility = "hidden";
    }

    item.value.prices.market = data.market_price;
    item.value.prices.vendor = data.vendor_price;

    item.value.numSimilarSoldRecently = data.num_similar_sold_recently;
    item.value.quality = data.quality;
    item.value.relativeQuality = data.relative_quality;
    item.value.adventurePoints = data.adventure_points;
    item.value.experience = data.experience;

    item.value.attributes.primary = data.item.primary || [];
    item.value.attributes.secondary = data.item.secondary || [];

    marker.value.style.left = `${data.x}px`;
    marker.value.style.top = `${data.y}px`;

    marker.value.style.width = `${data.width}px`;
    marker.value.style.height = `${data.height}px`;

    open.value = true;

    setTimeout(async () => {
      const padding = "5px";

      if (props.alignment !== "attached") {
        marker.value.style.top = null;
        marker.value.style.bottom = null;
        marker.value.style.right = null;
        marker.value.style.left = null;
        marker.value.style.width = 0;
        marker.value.style.height = 0;
      }

      switch (props.alignment) {
        case "attached":
          break;

        case "top-right":
          marker.value.style.top = 0;
          marker.value.style.right = padding;
          break;

        case "top-left":
          marker.value.style.top = 0;
          marker.value.style.left = padding;
          break;

        case "bottom-right":
          marker.value.style.bottom = 0;
          marker.value.style.right = padding;
          break;

        case "bottom-left":
          marker.value.style.bottom = 0;
          marker.value.style.left = padding;
          break;
      }

      await popper.update();

      tooltip.value.style.visibility = "visible";

      setMouseSleepPosition();
    }, 25);
  });

  electron.on("checkForTooltips:finish", () => {
    setMouseSleepPosition();
  });

  let checkForTooltips = () => {
    if (props.mode === modes.disabled) {
      return;
    }

    logger.debug("Checking for tooltips");
    electron.send("checkForTooltips");
  };

  electron.on("manual:checkForTooltips", () => {
    if (props.mode === modes.manual) {
      checkForTooltips();
    }
  });

  onMouseStill(() => {
    switch (props.mode) {
      case modes.automatic:
        checkForTooltips();
        break;

      case modes.manual:
        break;

      case modes.disabled:
        break;
    }
  }, MOUSE_STILL_FOR_MS);

  onMouseWakeup(() => {
    open.value = false;
  }, MOUSE_WAKEUP_DISTANCE);
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
