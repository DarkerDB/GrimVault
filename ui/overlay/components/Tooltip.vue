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

  components: {
    type: Array
  }
});

const open = ref(false);

const markerNode = ref(null);

const marker = ref({
  top: null,
  right: null,
  bottom: null,
  left: null,

  width: 0,
  height: 0,
});

// The game bounds are necessary to determine the overlay offset since the screen
// capture provides absolute coordinates relative to the monitor the game is 
// running on, not relative to the game window.
const gameBounds = ref (null);

electron.on ('game:bounds', (bounds) => {
  gameBounds.value = bounds;
});

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
    live: null,
    vendor: null,
  },
});

const primary = computed(() =>
  item.value.attributes.primary.filter(
    (attribute) => attribute.min !== attribute.max,
  ),
);

onMounted (() => {
  logger.info ('Tooltip mounted');

  const popper = createPopper(markerNode.value, tooltip.value, {
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
    console.log (data);
    console.log (gameBounds.value);
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

    // Update the marker position.

    open.value = true;

    setTimeout (async () => {
      const padding = 5;

      if (props.alignment === 'attached') {
        marker.value.top    = data.y  - (gameBounds.value ? gameBounds.value.y : 0);
        marker.value.right  = null;
        marker.value.bottom = null;
        marker.value.left   = data.x - (gameBounds.value ? gameBounds.value.x : 0);
        marker.value.width  = data.width;
        marker.value.height = data.height;
      } else {
        marker.value.top    = null;
        marker.value.bottom = null;
        marker.value.right  = null;
        marker.value.left   = null;
        marker.value.width  = 0;
        marker.value.height = 0;
      }

      switch (props.alignment) {
        case 'attached':
          break;

        case "top-right":
          marker.value.top = 0;
          marker.value.right = padding;
          break;

        case "top-left":
          marker.value.top = 0;
          marker.value.left = padding;
          break;

        case "bottom-right":
          marker.value.bottom = 0;
          marker.value.right = padding;
          break;

        case "bottom-left":
          marker.value.bottom = 0;
          marker.value.left = padding;
          break;
      }

      await popper.update ();

      tooltip.value.style.visibility = "visible";

      setMouseSleepPosition ();
    }, 25);
  });

  electron.on ('clear', () => {
    open.value = false;
  });

  electron.on ('scan:finish', () => {
    setMouseSleepPosition ();
  });

  let scan = () => {
    if (props.mode === modes.disabled) {
      return;
    }

    logger.debug ('Checking for tooltips');
    electron.send ('scan');
  };

  electron.on("manual:scan", () => {
    if (props.mode === modes.manual) {
      scan();
    }
  });

  onMouseStill(() => {
    switch (props.mode) {
      case modes.automatic:
        scan();
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

  // If we are attached make small mouse movements adjust the marker position.
  if (props.alignment === 'attached') {
    let previousMousePosition = null;

    window.addEventListener ('mousemove', (event) => {
      let currentMousePosition = {
        x: event.clientX,
        y: event.clientY
      };

      if (previousMousePosition) {
        marker.value.left += currentMousePosition.x - previousMousePosition.x;
        marker.value.top  += currentMousePosition.y - previousMousePosition.y;

        popper.update ();
      }

      previousMousePosition = currentMousePosition;
    });
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
    id="marker"  
    ref="markerNode" 
    class="absolute"
    :style="{ 
      top:    marker.top    !== null ? `${marker.top}px`    : '',
      right:  marker.right  !== null ? `${marker.right}px`  : '',
      bottom: marker.bottom !== null ? `${marker.bottom}px` : '',
      left:   marker.left   !== null ? `${marker.left}px`   : '',

      width:  marker.width  !== null ? `${marker.width}px`  : '',
      height: marker.height !== null ? `${marker.height}px` : '',
    }"
  ></div>

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
        <div class="tooltip-title" v-if="props.components.includes ('header')">
          Item Statistics
        </div>

        <div class="tooltip-body" :class="{ 'mt-3' : !props.components.includes ('header') }">
          <section v-if="props.components.includes ('primary') && primary.length">
            <div v-for="attribute in primary" class="[&:not(:last-child)]:pb-2">
              <span v-if="attribute.min !== attribute.max"
                >{{ attribute.display }} {{ attribute.min }} -
                {{ attribute.max }}</span
              >
            </div>
            <div class="tooltip-separator"></div>
          </section>

          <section v-if="props.components.includes ('secondary') && item.attributes.secondary.length">
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
              props.components.includes ('details') && 
              (
                item.quality ||
                item.relativeQuality ||
                item.numSimilarSoldRecently
              )
            "
          >
            <div class="tooltip-stats">
              <!-- <div v-if="item.quality" class="tooltip-stat">
                <span>Quality:</span> <span>{{ item.quality }}</span>
              </div> -->
              <!-- <div v-if="item.relativeQuality" class="tooltip-stat">
                <span>Relative Quality:</span>
                <span>{{ item.relativeQuality }}</span
                >%
              </div> -->
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
            v-if="props.components.includes ('pricing') && (item.prices.market !== null || item.prices.vendor !== null)"
          >
            <div class="flex items-center" v-if="item.prices.market !== null">
              <span>Market:</span>
              <span class="gold ml-2">{{ item.prices.market }}</span>
            </div>
            <div class="flex items-center" v-if="item.prices.vendor !== null">
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