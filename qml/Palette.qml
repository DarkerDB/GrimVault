pragma Singleton

import QtQuick

QtObject {
   readonly property string fontLight:  "Solmoe KimDaeGeon Light"
   readonly property string fontMedium: "Solmoe KimDaeGeon Medium"
   readonly property string fontScript: "Pelagiad"

   readonly property QtObject bg: QtObject {
      readonly property color deep:     "#0A0807"
      readonly property color panel:    "#14110E"
      readonly property color recessed: "#1C1814"
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

}
