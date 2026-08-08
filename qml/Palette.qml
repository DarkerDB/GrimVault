// Theme.qml — Blackened-pewter-reliquary palette. Two namespaces coexist:
//
//    Legacy flat colors (Theme.gold, Theme.chalk, Theme.rarity (...))
//       used by Tooltip.qml. Do not break.
//
//    Grouped tokens for chrome (Theme.bg, Theme.metal, Theme.text,
//       Theme.state). Use these for everything new. Same source-of-truth
//       lookups expose them as both nested-object reads (Theme.bg.deep)
//       and flat aliases for rarity.

pragma Singleton

import QtQuick

QtObject {
   // ---- Fonts ----
   // Families as Qt registers them from the embedded TTFs (verified via the
   // startup `fonts:` log line — both the typographic family and these full
   // names are present; the full names keep Light/Medium distinct).

   readonly property string fontLight:  "Solmoe KimDaeGeon Light"
   readonly property string fontMedium: "Solmoe KimDaeGeon Medium"
   readonly property string fontScript: "Pelagiad"

   // ---- Grouped tokens (preferred for new code) ----

   readonly property QtObject bg: QtObject {
      readonly property color deep:     "#0A0807"
      readonly property color panel:    "#14110E"
      readonly property color recessed: "#1C1814"
   }

   readonly property QtObject metal: QtObject {
      readonly property color steel:    "#5A5147"
      readonly property color bone:     "#B8AC9F"
      readonly property color brass:    "#C8924A"
      readonly property color brassHi:  "#E9C079"
   }

   readonly property QtObject text: QtObject {
      readonly property color parchment: "#ECD99A"
      readonly property color body:      "#C9BFA7"
      readonly property color muted:     "#7A7060"
   }

   readonly property QtObject state: QtObject {
      readonly property color lantern: "#FF9A00"
      readonly property color danger:  "#B33A2A"
      readonly property color ok:      "#6E8F3C"
   }

   // ---- Legacy flat colors (used by Tooltip.qml; keep stable) ----

   readonly property color gold:       "#ffd400"
   readonly property color lightGray:  "#989898"
   readonly property color tan:        "#ffce79"
   readonly property color feather:    "#b8ac9f"
   readonly property color dust:       "#626262"
   readonly property color teal:       "#8bd1d5"
   readonly property color turquoise:  "#91dadf"
   readonly property color aqua:       "#90d9de"
   readonly property color blue:       "#00aaee"
   readonly property color oak:        "#b18063"
   readonly property color chalk:      "#ecd99a"
   readonly property color purple:     "#d067ff"
   readonly property color green:      "#80d600"
   readonly property color orange:     "#ff9a00"
   readonly property color gray:       "#888888"
   readonly property color white:      "#eeeeee"
   readonly property color red:        "#e60505"

   // Rarity colors — must match the web tooltip card (.ddb-rarity-* on
   // darkerdb.com/tooltips) so overlay and site render identically.
   readonly property color poor:       "#888888"
   readonly property color common:     "#eeeeee"
   readonly property color uncommon:   "#95fa00"
   readonly property color rare:       "#13bbff"
   readonly property color epic:       "#db8bff"
   readonly property color legendary:  "#ffa824"
   readonly property color unique:     "#e6cf95"
   readonly property color artifact:   "#fa1515"
   readonly property color unknown:    "#eeeeee"

   readonly property color hover:      "#ecd99a"
   readonly property color active:     "#b18063"

   function rarity (name) {
      switch (name) {
         case "poor":      return poor
         case "common":    return common
         case "uncommon":  return uncommon
         case "rare":      return rare
         case "epic":      return epic
         case "legendary": return legendary
         case "unique":    return unique
         case "artifact":  return artifact
      }
      return common
   }
}
