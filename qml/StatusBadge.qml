// Small always-on-top corner badge pinned inside the game window. One
// glance answers "is GrimVault alive": dot color = signed-in state, label =
// scan mode, and the dot pulses whenever a tooltip makes it through the
// OCR pipeline.
import QtQuick
import "."

Item {
   id: root

   width:  row.width + 22
   height: 26

   property bool isAuto:     true
   property bool isSignedIn: false
   property string locale:   "en"

   function pulse () { pulseAnim.restart () }

   Rectangle {
      anchors.fill: parent
      radius:       6
      color:        Theme.bg.panel
      opacity:      0.82
      border.color: Theme.bg.recessed
      border.width: 1
   }

   Row {
      id: row
      anchors.centerIn: parent
      spacing: 7

      Rectangle {
         id: dot
         width: 8; height: 8; radius: 4
         anchors.verticalCenter: parent.verticalCenter
         color: root.isSignedIn ? Theme.state.ok : Theme.state.danger

         SequentialAnimation {
            id: pulseAnim
            NumberAnimation { target: dot; property: "scale"; to: 1.8; duration: 110 }
            NumberAnimation { target: dot; property: "scale"; to: 1.0; duration: 340; easing.type: Easing.OutQuad }
         }
      }

      Text {
         anchors.verticalCenter: parent.verticalCenter
         text: root.isSignedIn
            ? (root.isAuto ? "AUTO" : "MANUAL") + " (" + root.locale + ")"
            : "SIGNED OUT"
         color: Theme.text.body
         font.family: Theme.fontLight
         font.pixelSize: 11
         font.letterSpacing: 1
      }
   }
}
