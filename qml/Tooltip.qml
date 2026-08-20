import QtQuick
import QtQuick.Layouts
import "."

Item {
   id: root

   property real   renderScale: 1
   property bool   analysis:    false
   property bool   showMarket:  true
   property string body:        ""
   property var    primary:     []
   property var    secondary:   []
   property var    details:     []
   property int    market:      0
   property int    marketLow:   0
   property int    marketHigh:  0
   property int    vendor:      0
   property int    rollScore:   -1
   property string confidence:  ""

   readonly property bool hasBody:      body.length > 0
   readonly property bool hasSecondary: secondary.length > 0
   readonly property bool hasDetails:   details.length > 0
   readonly property real scaleValue:   Math.max (0.1, renderScale)
   readonly property color scoreColor:  rollScore >= 70 ? Theme.green
      : rollScore >= 40 ? Theme.tan : Theme.red
   readonly property string verdict: rollScore >= 85 ? "Exceptional rolls"
      : rollScore >= 70 ? "Strong rolls"
      : rollScore >= 40 ? "Average rolls"
      : rollScore >= 0 ? "Weak rolls" : ""

   width: surface.width * scaleValue
   height: surface.height * scaleValue

   Item {
      id: surface
      width: 380
      height: card.height + 26
      scale: root.scaleValue
      transformOrigin: Item.TopLeft

      Image {
         anchors.horizontalCenter: card.horizontalCenter
         y: card.y - 11
         width: 132
         height: 12
         source: "qrc:/assets/images/tooltip/adornment-top.png"
         fillMode: Image.PreserveAspectFit
         opacity: 0.85
      }

      BorderImage {
         id: card
         anchors.horizontalCenter: parent.horizontalCenter
         y: 11
         width: 360
         height: frame.implicitHeight + 24
         source: "qrc:/assets/images/tooltip/tooltip-bg.png"
         border { left: 30; right: 30; top: 30; bottom: 30 }
         horizontalTileMode: BorderImage.Stretch
         verticalTileMode: BorderImage.Stretch

         ColumnLayout {
            id: frame
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.margins: 8
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            spacing: 0

            Item {
               Layout.fillWidth: true
               Layout.leftMargin: -14
               Layout.rightMargin: -14
               implicitHeight: 67

               Rectangle {
                  anchors.fill: parent
                  gradient: Gradient {
                     GradientStop { position: 0; color: "#60130404" }
                     GradientStop { position: 0.65; color: "#590a0202" }
                     GradientStop { position: 1; color: "#bf000000" }
                  }
               }

               Image {
                  anchors.fill: parent
                  source: "qrc:/assets/images/tooltip/header-pattern.png"
                  fillMode: Image.PreserveAspectCrop
                  opacity: 0.9
               }

               Rectangle {
                  anchors.horizontalCenter: parent.horizontalCenter
                  anchors.top: parent.top
                  anchors.topMargin: 13
                  width: brand.implicitWidth + 20
                  height: 30
                  radius: 4
                  color: "#d9050000"
                  border.color: "#a67f0000"
                  border.width: 1

                  Row {
                     id: brand
                     anchors.centerIn: parent
                     spacing: 1

                     Item {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16

                        Rectangle {
                           x: 1
                           y: 1
                           width: 11
                           height: 11
                           radius: 6
                           color: "#420500"
                           border.color: "#b89a5e"
                           border.width: 2

                           Rectangle {
                              anchors.centerIn: parent
                              width: 6
                              height: 6
                              radius: 3
                              color: "#ef1f1f"
                           }
                        }

                        Rectangle {
                           x: 10
                           y: 11
                           width: 7
                           height: 2
                           rotation: 45
                           transformOrigin: Item.Left
                           color: "#b89a5e"
                        }
                     }

                     Text {
                        text: "Grim"
                        color: "#ef1f1f"
                        font.family: Theme.fontMedium
                        font.pixelSize: 19
                        font.letterSpacing: 0.6
                     }

                     Text {
                        text: "Vault"
                        color: "#a41818"
                        font.family: Theme.fontMedium
                        font.pixelSize: 19
                        font.letterSpacing: 0.6
                     }
                  }
               }

               Image {
                  anchors.bottom: parent.bottom
                  anchors.left: parent.left
                  anchors.right: parent.right
                  height: 8
                  source: "qrc:/assets/images/tooltip/header-separator.png"
                  fillMode: Image.Stretch
                  opacity: 0.95
               }
            }

            ColumnLayout {
               Layout.fillWidth: true
               Layout.topMargin: 5
               Layout.leftMargin: 3
               Layout.rightMargin: 3
               spacing: 0

               Rectangle {
                  Layout.fillWidth: true
                  visible: root.analysis && root.showMarket
                  implicitHeight: heroContent.implicitHeight + 18
                  color: "#1ff6c453"
                  border.color: "#24f6c453"
                  border.width: 1

                  ColumnLayout {
                     id: heroContent
                     anchors.fill: parent
                     anchors.margins: 8
                     spacing: 3

                     RowLayout {
                        Layout.fillWidth: true

                        Text {
                           Layout.fillWidth: true
                           text: root.market > 0 ? "MARKET VALUE" : root.vendor > 0 ? "VENDOR VALUE" : "MARKET VALUE"
                           color: "#f6c453"
                           font.family: Theme.fontMedium
                           font.pixelSize: 11
                           font.letterSpacing: 1
                        }

                        Rectangle {
                           visible: root.confidence.length > 0 && root.confidence !== "none"
                           implicitWidth: confidenceText.implicitWidth + 12
                           implicitHeight: confidenceText.implicitHeight + 8
                           color: root.confidence === "high" ? "#0e95fa00"
                              : root.confidence === "low" ? "#12fa1515" : "#0dffffff"
                           border.color: root.confidence === "high" ? "#4795fa00"
                              : root.confidence === "low" ? "#61fa1515" : "#2ed2c6ac"

                           Text {
                              id: confidenceText
                              anchors.centerIn: parent
                              text: root.confidence.toUpperCase () + " CONFIDENCE"
                              color: root.confidence === "high" ? Theme.green
                                 : root.confidence === "low" ? Theme.red : "#d0c3aa"
                              font.family: Theme.fontLight
                              font.pixelSize: 10
                              font.letterSpacing: 0.4
                           }
                        }
                     }

                     RowLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        Image {
                           visible: root.market > 0 || root.vendor > 0
                           source: "qrc:/assets/images/tooltip/gold-coin.png"
                           fillMode: Image.PreserveAspectFit
                           Layout.preferredWidth: 20
                           Layout.preferredHeight: 18
                        }

                        Text {
                           Layout.fillWidth: true
                           text: root.market > 0 ? root.market : root.vendor > 0 ? root.vendor : "No recent market data"
                           color: root.market > 0 || root.vendor > 0 ? "#ffe2a0" : "#8d8375"
                           font.family: Theme.fontScript
                           font.pixelSize: root.market > 0 || root.vendor > 0 ? 29 : 20
                        }

                        Text {
                           visible: root.marketLow > 0 && root.marketHigh > 0
                           text: "Typical range\n" + root.marketLow + " – " + root.marketHigh + " G"
                           color: "#c0b5a1"
                           font.family: Theme.fontLight
                           font.pixelSize: 11
                           horizontalAlignment: Text.AlignRight
                        }
                     }

                     RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 5
                        visible: root.verdict.length > 0
                        spacing: 8

                        Rectangle {
                           Layout.fillWidth: true
                           Layout.preferredHeight: 1
                           color: "#12ffffff"
                        }

                        Text {
                           text: "◆ " + root.verdict
                           color: root.scoreColor
                           font.family: Theme.fontMedium
                           font.pixelSize: 13
                        }
                     }
                  }
               }

               Text {
                  Layout.fillWidth: true
                  Layout.topMargin: 8
                  visible: root.hasBody
                  text: root.body
                  color: "#eee7d8"
                  font.family: Theme.fontLight
                  font.pixelSize: 13
                  wrapMode: Text.WordWrap
               }

               Repeater {
                  model: root.primary
                  delegate: Text {
                     required property string modelData

                     Layout.fillWidth: true
                     Layout.topMargin: 6
                     text: modelData
                     color: "#ffffff"
                     font.family: Theme.fontMedium
                     font.pixelSize: 14
                     horizontalAlignment: Text.AlignHCenter
                     wrapMode: Text.WordWrap
                  }
               }

               ColumnLayout {
                  Layout.fillWidth: true
                  Layout.topMargin: 10
                  visible: root.hasSecondary
                  spacing: 1

                  RowLayout {
                     Layout.fillWidth: true
                     Layout.leftMargin: 4
                     Layout.rightMargin: 4

                     Text {
                        Layout.fillWidth: true
                        text: root.analysis ? "ROLL QUALITY" : "ATTRIBUTES"
                        color: "#f6c453"
                        font.family: Theme.fontMedium
                        font.pixelSize: 12
                        font.letterSpacing: 1
                     }

                     Text {
                        visible: root.rollScore >= 0
                        text: root.rollScore + " / 100"
                        color: root.scoreColor
                        font.family: Theme.fontScript
                        font.pixelSize: 18
                     }
                  }

                  Repeater {
                     model: root.secondary
                     delegate: Rectangle {
                        id: rollRow
                        required property string modelData

                        Layout.fillWidth: true
                        implicitHeight: rollText.implicitHeight + 12
                        color: "#e6050606"

                        Text {
                           id: rollText
                           anchors.fill: parent
                           anchors.margins: 6
                           text: rollRow.modelData
                           color: "#00aaee"
                           font.family: Theme.fontLight
                           font.pixelSize: 13
                           wrapMode: Text.WordWrap
                        }
                     }
                  }
               }

               ColumnLayout {
                  Layout.fillWidth: true
                  Layout.topMargin: 10
                  visible: root.hasDetails
                  spacing: 1

                  Text {
                     Layout.fillWidth: true
                     Layout.leftMargin: 4
                     Layout.bottomMargin: 6
                     text: root.analysis ? "ITEM OVERVIEW" : "DETAILS"
                     color: "#f6c453"
                     font.family: Theme.fontMedium
                     font.pixelSize: 12
                     font.letterSpacing: 1
                  }

                  Repeater {
                     model: root.details
                     delegate: Rectangle {
                        id: detailRow
                        required property string modelData
                        required property int index

                        Layout.fillWidth: true
                        implicitHeight: 27
                        color: index % 2 ? "#0fffffff" : "#19000000"

                        RowLayout {
                           anchors.fill: parent
                           spacing: 6

                           Text {
                              Layout.fillWidth: true
                              Layout.leftMargin: 7
                              text: detailRow.modelData.split (": ") [0]
                              color: "#aaa08f"
                              font.family: Theme.fontLight
                              font.pixelSize: 11
                              elide: Text.ElideRight
                           }

                           Text {
                              Layout.rightMargin: 7
                              text: detailRow.modelData.split (": ").slice (1).join (": ")
                              color: "#f2eadb"
                              font.family: Theme.fontMedium
                              font.pixelSize: 12
                           }
                        }
                     }
                  }
               }
            }

            ColumnLayout {
               Layout.fillWidth: true
               Layout.topMargin: 10
               spacing: 8

               Rectangle {
                  Layout.fillWidth: true
                  implicitHeight: 1
                  color: "#1adcc8a0"
               }

               Text {
                  Layout.alignment: Qt.AlignHCenter
                  text: "POWERED BY DARKERDB.COM"
                  color: "#8a8068"
                  font.family: Theme.fontLight
                  font.pixelSize: 9
                  font.letterSpacing: 2
               }
            }
         }
      }

      Image {
         anchors.horizontalCenter: card.horizontalCenter
         y: card.y + card.height - 1
         width: 162
         height: 14
         source: "qrc:/assets/images/tooltip/adornment-bottom.png"
         fillMode: Image.PreserveAspectFit
         opacity: 0.85
         mirrorVertically: true
      }
   }
}
