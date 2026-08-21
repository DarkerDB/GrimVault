import QtQuick
import QtQuick.Layouts
import "."

GridLayout {
   id: root

   property var entries: []

   columns: 2
   columnSpacing: 1
   rowSpacing: 1

   Repeater {
      model: root.entries

      Rectangle {
         id: cell

         required property var modelData
         required property int index

         Layout.fillWidth: true
         Layout.columnSpan: index === root.entries.length - 1 && index % 2 === 0 ? 2 : 1
         implicitHeight: 29
         color: "#e6050606"

         RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 7
            anchors.rightMargin: 7
            spacing: 4

            Text {
               Layout.fillWidth: true
               text: cell.modelData.label || ""
               color: "#aaa08f"
               font.family: Theme.fontLight
               font.pixelSize: 11
               elide: Text.ElideRight
            }

            Image {
               visible: cell.modelData.money === true
               source: "qrc:/assets/images/tooltip/gold-coin.png"
               fillMode: Image.PreserveAspectFit
               Layout.preferredWidth: 15
               Layout.preferredHeight: 13
            }

            Text {
               text: cell.modelData.value || "N/A"
               color: cell.modelData.muted === true ? "#6f685d" : "#f2eadb"
               font.family: Theme.fontMedium
               font.pixelSize: 12
            }
         }
      }
   }
}
