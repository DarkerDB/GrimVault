import QtQuick
import QtQuick.Layouts
import "."

RowLayout {
   id: root

   property string title: ""
   property string subtitle: ""
   property string value: ""
   property color valueColor: "#95fa00"

   spacing: 8

   ColumnLayout {
      Layout.fillWidth: true
      spacing: 0

      Text {
         Layout.fillWidth: true
         text: root.title.toUpperCase ()
         color: "#f6c453"
         font.family: Theme.fontMedium
         font.pixelSize: 12
         font.letterSpacing: 0.9
         elide: Text.ElideRight
      }

      Text {
         Layout.fillWidth: true
         visible: root.subtitle.length > 0
         text: root.subtitle
         color: "#aaa08f"
         font.family: Theme.fontLight
         font.pixelSize: 11
         elide: Text.ElideRight
      }
   }

   Text {
      visible: root.value.length > 0
      text: root.value
      color: root.valueColor
      font.family: Theme.fontScript
      font.pixelSize: 25
   }
}
