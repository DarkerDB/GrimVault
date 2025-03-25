import palette from "../web/src/palette.js";
import colors from 'tailwindcss/colors';

export default {
  content: ["./ui/**/*.{html,js,vue}"],
  theme: {
    colors: {
      transparent: "transparent",
      current: "currentColor",

      ... colors,
      ... palette,
    },
  },
  plugins: [],
};
