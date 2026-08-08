// Tooltip.qml — QML port of the web tooltip card (.ddb-tip-card on
// darkerdb.com/tooltips). Same assets, fonts, colors, and section order so
// the in-game overlay renders identically to the site embed:
//
//    adornment-top
//    ┌ tooltip-bg 9-slice ───────────────┐
//    │ header-pattern band + rarity name │
//    │ primary stats        (white 14px) │
//    │ ·· dots-separator ··              │
//    │ secondary            (blue italic)│
//    │ ── separator ──                   │
//    │ details rows  (gray label/value)  │
//    │ ── separator ──                   │
//    │ Market / Vendor  coin + gold      │
//    │ POWERED BY DARKERDB.COM           │
//    └───────────────────────────────────┘
//    adornment-bottom
//
// Bound from C++ (OverlayWindow::present) via setProperty:
//    title:     string
//    rarity:    string ("poor", "common", ..., "artifact")
//    primary:   list<string>
//    secondary: list<string>
//    details:   list<string>   ("Label: Value")
//    market:    int (gold, 0 = hide row)
//    vendor:    int (gold, 0 = hide row)

import QtQuick
import QtQuick.Layouts
import "."

Item {
   id: root

   property string title:     ""
   property string rarity:    "common"
   property var    primary:   []
   property var    secondary: []
   property var    details:   []
   property int    market:    0
   property int    vendor:    0

   readonly property bool hasStats:   primary.length > 0
   readonly property bool hasEnchant: secondary.length > 0
   readonly property bool hasDetails: details.length > 0
   readonly property bool hasPricing: market > 0 || vendor > 0

   width:  340
   height: card.height + 26

   // Adornment geometry mirrors .ddb-tip-card::before/::after exactly:
   // top 132x12 at -11px, bottom 162x14 at -13px flipped vertically.
   Image {
      anchors.horizontalCenter: card.horizontalCenter
      y:        card.y - 11
      width:    132
      height:   12
      source:   "qrc:/assets/images/tooltip/adornment-top.png"
      fillMode: Image.PreserveAspectFit
      opacity:  0.85
   }

   BorderImage {
      id: card
      anchors.horizontalCenter: parent.horizontalCenter
      y:      11
      width:  320
      height: frame.implicitHeight + 24

      source: "qrc:/assets/images/tooltip/tooltip-bg.png"
      border { left: 30; right: 30; top: 30; bottom: 30 }
      horizontalTileMode: BorderImage.Stretch
      verticalTileMode:   BorderImage.Stretch

      ColumnLayout {
         id: frame
         anchors.top:     parent.top
         anchors.left:    parent.left
         anchors.right:   parent.right
         anchors.margins: 8
         anchors.leftMargin:  14
         anchors.rightMargin: 14
         spacing: 6

         // ---- Header: rarity-tinted pattern band + name + separator ----
         Item {
            Layout.fillWidth:    true
            Layout.leftMargin:   -14
            Layout.rightMargin:  -14
            implicitHeight: nameText.implicitHeight + 23 + sep.height + 8

            Rectangle {
               anchors.fill: parent
               gradient: Gradient {
                  GradientStop { position: 0.0; color: Qt.alpha (Theme.rarity (root.rarity), 0.30) }
                  GradientStop { position: 1.0; color: "transparent" }
               }
            }

            Image {
               anchors.fill: parent
               source:       "qrc:/assets/images/tooltip/header-pattern.png"
               fillMode:     Image.PreserveAspectCrop
               opacity:      0.9
            }

            // The game tooltip already names the item — this card is the
            // statistics companion (matches the old overlay's header), with
            // the server-resolved name as a rarity-colored subtitle.
            Column {
               id: nameText
               anchors.top:              parent.top
               anchors.topMargin:        13
               anchors.horizontalCenter: parent.horizontalCenter
               width:   parent.width - 30
               spacing: 2

               Text {
                  width:            parent.width
                  text:             "Item Statistics"
                  color:            Theme.legendary
                  font.family:      Theme.fontMedium
                  font.pixelSize:   19
                  font.letterSpacing: 0.6
                  horizontalAlignment: Text.AlignHCenter
               }

               Text {
                  width:            parent.width
                  visible:          root.title.length > 0
                  text:             root.title
                  color:            Theme.rarity (root.rarity)
                  font.family:      Theme.fontMedium
                  font.pixelSize:   14
                  horizontalAlignment: Text.AlignHCenter
                  wrapMode:         Text.WordWrap
               }
            }

            Image {
               id: sep
               anchors.bottom: parent.bottom
               anchors.left:   parent.left
               anchors.right:  parent.right
               height:   8
               source:   "qrc:/assets/images/tooltip/header-separator.png"
               fillMode: Image.Stretch
               opacity:  0.85
            }
         }

         // ---- Primary stats ----
         Repeater {
            model: root.primary
            delegate: Text {
               Layout.fillWidth:    true
               text:                modelData
               color:               "#ffffff"
               font.family:         Theme.fontMedium
               font.pixelSize:      14
               horizontalAlignment: Text.AlignHCenter
               wrapMode:            Text.WordWrap
            }
         }

         Image {
            Layout.fillWidth: true
            Layout.preferredHeight: 8
            visible:  root.hasStats && (root.hasEnchant || root.hasDetails || root.hasPricing)
            source:   "qrc:/assets/images/tooltip/dots-separator.png"
            fillMode: Image.PreserveAspectFit
            opacity:  0.85
         }

         // ---- Secondary enchantments ----
         Repeater {
            model: root.secondary
            delegate: Text {
               Layout.fillWidth:    true
               text:                modelData
               color:               "#00aaee"
               font.family:         Theme.fontScript
               font.pixelSize:      14
               font.italic:         true
               horizontalAlignment: Text.AlignHCenter
               wrapMode:            Text.WordWrap
            }
         }

         Image {
            Layout.fillWidth: true
            Layout.preferredHeight: 8
            visible:  root.hasEnchant && (root.hasDetails || root.hasPricing)
            source:   "qrc:/assets/images/tooltip/separator.png"
            fillMode: Image.PreserveAspectFit
            opacity:  0.85
         }

         // ---- Detail rows ("Label: Value") ----
         // Two columns on a shared axis (labels right-aligned, values left)
         // so consecutive rows line up instead of centering ragged.
         Repeater {
            model: root.details
            delegate: RowLayout {
               Layout.fillWidth: true
               spacing: 8

               Text {
                  Layout.preferredWidth: frame.width * 0.46
                  text:           modelData.split (": ") [0] + ":"
                  color:          "#767676"
                  font.family:    Theme.fontLight
                  font.pixelSize: 13
                  horizontalAlignment: Text.AlignRight
                  elide:          Text.ElideLeft
               }

               Text {
                  Layout.fillWidth: true
                  text:           modelData.split (": ").slice (1).join (": ")
                  color:          "#acacac"
                  font.family:    Theme.fontLight
                  font.pixelSize: 13
                  wrapMode:       Text.WordWrap
               }
            }
         }

         Image {
            Layout.fillWidth: true
            Layout.preferredHeight: 8
            visible:  root.hasPricing && (root.hasStats || root.hasEnchant || root.hasDetails)
            source:   "qrc:/assets/images/tooltip/separator.png"
            fillMode: Image.PreserveAspectFit
            opacity:  0.85
         }

         // ---- Pricing ----
         Repeater {
            model: [
               { label: "Market", value: root.market },
               { label: "Vendor", value: root.vendor },
            ].filter (r => r.value > 0)

            delegate: RowLayout {
               Layout.alignment: Qt.AlignHCenter
               spacing: 8

               Text {
                  text:           modelData.label + ":"
                  color:          Theme.feather
                  font.family:    Theme.fontMedium
                  font.pixelSize: 15
               }

               Image {
                  source:   "qrc:/assets/images/tooltip/gold-coin.png"
                  fillMode: Image.PreserveAspectFit
                  Layout.preferredWidth:  24
                  Layout.preferredHeight: 20
               }

               Text {
                  text:           modelData.value
                  color:          Theme.gold
                  font.family:    Theme.fontMedium
                  font.pixelSize: 15
               }
            }
         }

         // ---- Credit footer ----
         ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 8

            Rectangle {
               Layout.fillWidth: true
               implicitHeight: 1
               color: "#1adcc8a0"
            }

            Text {
               Layout.alignment:   Qt.AlignHCenter
               text:               "POWERED BY DARKERDB.COM"
               color:              "#8a8068"
               font.family:        Theme.fontLight
               font.pixelSize:     9
               font.letterSpacing: 2
            }
         }
      }
   }

   Image {
      anchors.horizontalCenter: card.horizontalCenter
      y:        card.y + card.height - 1
      width:    162
      height:   14
      source:   "qrc:/assets/images/tooltip/adornment-bottom.png"
      fillMode: Image.PreserveAspectFit
      opacity:  0.85
      mirrorVertically: true
   }
}
