// Tooltip.qml — Diablo-esque overlay. Mirrors ui/overlay/components/Tooltip.vue:
//    - 9-slice parchment border (Background_TooltipBorder.png)
//    - Tiled texture (Background_TooltipTexture.png)
//    - SaintKDG title + body fonts
//    - Rarity-colored title
//
// Bound from C++ (OverlayWindow::present) via setProperty:
//    title:     string
//    rarity:    string ("poor", "common", ..., "artifact")
//    primary:   list<string>
//    secondary: list<string>

import QtQuick
import QtQuick.Layouts
import "."

Item {
   id: root

   property string title:     ""
   property string rarity:    "common"
   property var    primary:   []
   property var    secondary: []

   width:  Math.max (320, content.implicitWidth + 48)
   height: content.implicitHeight + 48

   BorderImage {
      anchors.fill:    parent
      anchors.margins: -10
      source:           "qrc:/assets/images/Background_TooltipBorder.png"
      border.left:      21
      border.right:     21
      border.top:       21
      border.bottom:    21
      horizontalTileMode: BorderImage.Stretch
      verticalTileMode:   BorderImage.Stretch
   }

   Image {
      anchors.fill:    parent
      anchors.margins: 10
      source:           "qrc:/assets/images/Background_TooltipTexture.png"
      fillMode:         Image.Tile
      opacity:          0.92
   }

   ColumnLayout {
      id: content
      anchors.fill:    parent
      anchors.margins: 24
      spacing:         6

      Text {
         id: titleText
         Layout.alignment: Qt.AlignHCenter
         text:             root.title
         color:            Theme.rarity (root.rarity)
         font.family:      "SaintKDG_Medium"
         font.pixelSize:   22
         horizontalAlignment: Text.AlignHCenter
         Layout.fillWidth: true
         wrapMode:         Text.WordWrap
      }

      Image {
         Layout.alignment:  Qt.AlignHCenter
         Layout.fillWidth:  true
         Layout.maximumHeight: 8
         source:             "qrc:/assets/images/Tooltip_SeparatorThick.png"
         fillMode:           Image.PreserveAspectFit
         visible:            root.primary.length > 0 || root.secondary.length > 0
      }

      Repeater {
         model: root.primary
         delegate: Text {
            Layout.alignment:  Qt.AlignHCenter
            text:               modelData
            color:              Theme.blue
            font.family:        "SaintKDG_Light"
            font.pixelSize:     16
         }
      }

      Image {
         Layout.alignment:  Qt.AlignHCenter
         Layout.fillWidth:  true
         Layout.maximumHeight: 6
         source:             "qrc:/assets/images/Tooltip_SeparatorThin.png"
         fillMode:           Image.PreserveAspectFit
         visible:            root.primary.length > 0 && root.secondary.length > 0
      }

      Repeater {
         model: root.secondary
         delegate: Text {
            Layout.alignment:  Qt.AlignHCenter
            text:               modelData
            color:              Theme.dust
            font.family:        "SaintKDG_Light"
            font.pixelSize:     14
         }
      }

      Item { Layout.fillHeight: true; Layout.preferredHeight: 4 }

      Text {
         Layout.alignment:  Qt.AlignRight
         text:               "Powered by DarkerDB.com"
         color:              Theme.lightGray
         font.family:        "SaintKDG_Light"
         font.pixelSize:     8
      }
   }
}
