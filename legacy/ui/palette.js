// const palette = {
//   "gray-light": "#989898",
//   gray: "#888888",
//   "gray-dark": "#626262",

//   white: "#eeeeee",
//   black: "#000000",

//   charcoal: "#080808",

//   purple: "#d067ff",
//   green: "#80d600",
//   orange: "#ff9a00",
//   yellow: "#ffd400",

//   red: '#e60505',

//   // Secondary stats
//   blue: "#00aaee",

//   // Found by
//   tan: "#ffce79",

//   // Slot type, Utility type
//   "tan-light": "#b8ac9f",

//   // Loot state
//   teal: "#8bd1d5",

//   // Description
//   oak: "#b18063",

//   chalk: "#ecd99a",

//   // -- -- //

//   // Contextual

//   poor: "#888888",
//   common: "#eeeeee",
//   uncommon: "#80d600",
//   rare: "#00aaee",
//   epic: "#d067ff",
//   legendary: "#ff9a00",
//   unique: "#ecd99a",

//   hover: "#ecd99a",
//   active: "#b18063",
// };

// export default palette;


function hexToHSL(hex) {
  // Remove the # if present
  hex = hex.replace(/^#/, '');

  // Parse the hex values
  const r = parseInt(hex.slice(0, 2), 16) / 255;
  const g = parseInt(hex.slice(2, 4), 16) / 255;
  const b = parseInt(hex.slice(4, 6), 16) / 255;

  const max = Math.max(r, g, b);
  const min = Math.min(r, g, b);
  let h, s, l = (max + min) / 2;

  if (max === min) {
      h = s = 0;
  } else {
      const d = max - min;
      s = l > 0.5 ? d / (2 - max - min) : d / (max + min);

      switch (max) {
          case r: h = (g - b) / d + (g < b ? 6 : 0); break;
          case g: h = (b - r) / d + 2; break;
          case b: h = (r - g) / d + 4; break;
      }

      h /= 6;
  }

  return { h: h * 360, s: s * 100, l: l * 100 };
}

function HSLToHex(h, s, l) {
  h /= 360;
  s /= 100;
  l /= 100;

  let r, g, b;

  if (s === 0) {
      r = g = b = l;
  } else {
      const hue2rgb = (p, q, t) => {
          if (t < 0) t += 1;
          if (t > 1) t -= 1;
          if (t < 1/6) return p + (q - p) * 6 * t;
          if (t < 1/2) return q;
          if (t < 2/3) return p + (q - p) * (2/3 - t) * 6;
          return p;
      };

      const q = l < 0.5 ? l * (1 + s) : l + s - l * s;
      const p = 2 * l - q;

      r = hue2rgb(p, q, h + 1/3);
      g = hue2rgb(p, q, h);
      b = hue2rgb(p, q, h - 1/3);
  }

  const toHex = x => {
      const hex = Math.round(x * 255).toString(16);
      return hex.length === 1 ? '0' + hex : hex;
  };

  return `#${toHex(r)}${toHex(g)}${toHex(b)}`;
}

function generateColorScale(baseColor) {
  const hsl = hexToHSL(baseColor);

  // Define lightness adjustments for each step (500 is base)
  const lightnessAdjustments = {
      50: +35,
      100: +28,
      200: +21,
      300: +14,
      400: +7,
      500: 0,    // Base color
      600: -7,
      700: -14,
      800: -21,
      900: -28
  };

  // Generate the scale
  const scale = {};
  Object.entries(lightnessAdjustments).forEach(([step, adjustment]) => {
      const newLightness = Math.max(0, Math.min(100, hsl.l + adjustment));
      scale[step] = HSLToHex(hsl.h, hsl.s, newLightness);
  });

  return scale;
}

// Usage:
const rarity = {
  POOR: "#888888",
  COMMON: "#eeeeee",
  UNCOMMON: "#80d600",
  RARE: "#00aaee",
  EPIC: "#d067ff",
  LEGENDARY: "#ff9a00",
  UNIQUE: "#ecd99a",
  ARTIFACT: "#e60505"
};

const baseColors = {
  gray: "#888888",
  charcoal: "#080808",
  purple: "#d067ff",
  green: "#80d600",
  orange: "#ff9a00",
  yellow: "#ffd400",
  red: "#e60505",
  blue: "#00aaee",
  tan: "#ffce79",
  "tan-light": "#b8ac9f",
  teal: "#8bd1d5",
  oak: "#b18063",
  "oak-dark": "#3b2a21",
  chalk: "#ecd99a",
};

// Generate the palette
const palette = Object.entries(baseColors).reduce((acc, [name, color]) => {
  acc[name] = generateColorScale(color);
  return acc;
}, {});

// Add the constants
palette.white = "#eeeeee";
palette.black = "#000000";

// Add rarity colors
Object.entries(rarity).forEach(([key, value]) => {
  palette[key.toLowerCase()] = value;
});

// Add interactive states
palette.hover = "#ecd99a";
palette.active = "#b18063";

export default palette;