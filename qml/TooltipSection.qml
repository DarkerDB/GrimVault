import QtQuick
import QtQuick.Layouts
import "."

ColumnLayout {
   id: root

   property bool shown: true
   property string title: ""
   property string subtitle: ""
   property string value: ""
   property color valueColor: "#95fa00"
   default property alias content: contentColumn.data

   visible: shown
   spacing: 7

   TooltipSectionHeader {
      Layout.fillWidth: true
      Layout.leftMargin: 4
      Layout.rightMargin: 4
      title: root.title
      subtitle: root.subtitle
      value: root.value
      valueColor: root.valueColor
   }

   ColumnLayout {
      id: contentColumn
      Layout.fillWidth: true
      spacing: 1
   }

   Rectangle {
      Layout.fillWidth: true
      Layout.topMargin: 3
      implicitHeight: 1
      color: "#1adcc8a0"
   }
}
