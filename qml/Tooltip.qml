import QtQuick
import QtQuick.Layouts
import "."

Item {
   id: root

   property real renderScale: 1
   property var entity: ({})

   readonly property real scaleValue: Math.max (0.1, renderScale)
   readonly property var analysis: analysisSection ()
   readonly property bool analysisMode: analysis.kind === "analysis"
   readonly property var pricing: analysis.pricing || ({})
   readonly property var market: analysis.market || ({})
   readonly property var utility: analysis.utility || ({})
   readonly property int score: analysis.weighted_roll_score !== undefined
      && analysis.weighted_roll_score !== null ? analysis.weighted_roll_score
      : analysis.roll_score !== undefined && analysis.roll_score !== null
         ? analysis.roll_score : -1
   readonly property color scoreColor: score >= 70 ? "#95fa00"
      : score >= 40 ? "#f2ce72" : "#fa1515"

   function analysisSection () {
      const sections = entity.sections || []
      for (let i = 0; i < sections.length; ++i) {
         if (sections [i].kind === "analysis") return sections [i]
      }
      return ({})
   }

   function enabledWidget (name) {
      const visible = analysis.visible_sections || ({})
      return visible [name] !== false
   }

   function formatNumber (value) {
      const rounded = Math.round (Math.abs (Number (value) || 0)).toString ()
      const groups = []
      for (let end = rounded.length; end > 0; end -= 3) {
         groups.unshift (rounded.slice (Math.max (0, end - 3), end))
      }
      return groups.join (",")
   }

   function money (value, signed) {
      const amount = Number (value) || 0
      const sign = signed ? amount >= 0 ? "+" : "-" : ""
      return sign + formatNumber (amount) + " G"
   }

   function decimal (value) {
      const number = Number (value) || 0
      return number.toFixed (2).replace (/\.00$/, "").replace (/(\.\d)0$/, "$1")
   }

   function elapsed (seconds) {
      const value = Math.max (0, Number (seconds) || 0)
      if (value < 60) return "Now"
      if (value < 3600) return Math.max (1, Math.floor (value / 60)) + "m"
      if (value < 86400) return Math.floor (value / 3600) + "h"
      return Math.floor (value / 86400) + "d"
   }

   function duration (seconds) {
      const value = Math.max (0, Number (seconds) || 0)
      if (value < 60) return Math.max (1, Math.round (value)) + "s"
      if (value < 3600) return Math.max (1, Math.round (value / 60)) + "m"
      if (value < 86400) return decimal (value / 3600) + "h"
      return decimal (value / 86400) + "d"
   }

   function rate (value) {
      const percent = Math.max (0, Number (value) || 0) * 100
      if (percent >= 10) return decimal (percent) + "%"
      if (percent >= 1) return percent.toFixed (1).replace (/\.0$/, "") + "%"
      return percent.toFixed (2).replace (/0+$/, "").replace (/\.$/, "") + "%"
   }

   function ordinal (value) {
      const number = Math.max (0, Math.round (Number (value) || 0))
      const lastTwo = number % 100
      const suffix = lastTwo >= 11 && lastTwo <= 13 ? "th"
         : number % 10 === 1 ? "st"
         : number % 10 === 2 ? "nd"
         : number % 10 === 3 ? "rd" : "th"
      return number + suffix
   }

   function verdict () {
      if (analysis.tradeable === false) return {
         label: "Cannot be listed",
         hint: "Keep for crafting and quests, or sell to a merchant."
      }
      if (score >= 85) return {
         label: "Exceptional rolls",
         hint: "Keep it or list above the market median."
      }
      if (score >= 70) return {
         label: "Strong rolls",
         hint: "Worth keeping or selling at a premium."
      }
      if (score >= 40) return {
         label: "Average rolls",
         hint: "Price it close to the market median."
      }
      if (score >= 0) return {
         label: "Weak rolls",
         hint: "A good candidate for a quick sale."
      }
      return { label: "", hint: "" }
   }

   function overviewEntries () {
      const entries = []
      entries.push ({
         label: analysis.found_by ? "Found by" : "Vendor",
         value: analysis.found_by || (utility.vendor_value > 0 ? money (utility.vendor_value) : "N/A"),
         money: !analysis.found_by && utility.vendor_value > 0,
         muted: !analysis.found_by && !(utility.vendor_value > 0)
      })
      entries.push ({
         label: "Gear score",
         value: utility.gear_score > 0 ? utility.gear_score.toString () : "N/A",
         muted: !(utility.gear_score > 0)
      })
      entries.push ({
         label: "Adv. points",
         value: utility.adventure_points > 0 ? utility.adventure_points.toString () : "N/A",
         muted: !(utility.adventure_points > 0)
      })
      entries.push ({
         label: "Tradeable",
         value: analysis.tradeable === false ? "No" : "Yes",
         muted: analysis.tradeable === false
      })
      if (analysis.quantity > 1 && pricing.total_value > 0) entries.push ({
         label: "Stack of " + analysis.quantity,
         value: money (pricing.total_value), money: true
      })
      if (analysis.quantity > 1 && utility.value_per_slot > 0) entries.push ({
         label: "Per slot", value: money (utility.value_per_slot), money: true
      })
      if (utility.max_stack_size > 1) entries.push ({
         label: "Max stack", value: utility.max_stack_size.toString ()
      })
      return entries
   }

   function marketEntries () {
      const entries = []
      if (enabledWidget ("actions")) {
         entries.push ({ label: "Sell quickly", value: pricing.quick_list > 0
            ? money (pricing.quick_list) : "Unavailable", money: pricing.quick_list > 0,
            muted: !(pricing.quick_list > 0) })
         entries.push ({ label: "Lowest ask", value: pricing.lowest_ask > 0
            ? money (pricing.lowest_ask) : "Unavailable", money: pricing.lowest_ask > 0,
            muted: !(pricing.lowest_ask > 0) })
      }
      if (enabledWidget ("market_activity")) {
         const rows = [
            ["Sale median", market.median_sale_price, true],
            ["Sale avg.", market.average_sale_price, true],
            ["Sales 30d", market.sales_30d, false],
            ["Listings", market.active_listings, false]
         ]
         for (let i = 0; i < rows.length; ++i) entries.push ({
            label: rows [i][0],
            value: rows [i][1] > 0 ? rows [i][2] ? money (rows [i][1])
               : rows [i][1].toString () : "Unavailable",
            money: rows [i][2] && rows [i][1] > 0,
            muted: !(rows [i][1] > 0)
         })
         entries.push ({ label: "Sale time", value: market.median_sale_seconds > 0
            ? duration (market.median_sale_seconds) : "Unavailable",
            muted: !(market.median_sale_seconds > 0) })
         entries.push ({ label: "Supply", value: market.days_supply > 0
            ? decimal (market.days_supply) + "d" : "Unavailable",
            muted: !(market.days_supply > 0) })
         entries.push ({ label: "Samples", value: pricing.sample_size > 0
            ? pricing.sample_size.toString () : "Unavailable",
            muted: !(pricing.sample_size > 0) })
         entries.push ({ label: "Position", value: analysis.relative_percentile !== undefined
            && analysis.relative_percentile !== null
            ? "Top " + Math.max (1, 100 - analysis.relative_percentile) + "%" : "Unavailable",
            muted: analysis.relative_percentile === undefined || analysis.relative_percentile === null })
      }
      return entries
   }

   function secondaryRolls () {
      const rolls = analysis.rolls || []
      const result = []
      for (let i = 0; i < rolls.length; ++i) {
         if (!rolls [i].slot || rolls [i].slot === "secondary") result.push (rolls [i])
      }
      return result
   }

   function rollColor (value) {
      const percentile = Math.max (0, Math.min (100, Number (value) || 0))
      if (percentile >= 75) return "#95fa00"
      if (percentile >= 45) return "#f2ce72"
      return "#fa5a15"
   }

   function gradeColor (grade) {
      if (grade === "S") return "#ff9a00"
      if (grade === "A") return "#d067ff"
      if (grade === "B") return "#00aaee"
      if (grade === "C") return "#80d600"
      if (grade === "D") return "#eeeeee"
      return "#767676"
   }

   function legacySection (kind, variant) {
      const sections = entity.sections || []
      for (let i = 0; i < sections.length; ++i) {
         if (sections [i].kind === kind
             && (!variant || sections [i].variant === variant)) return sections [i]
      }
      return ({})
   }

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
         y: 0
         width: 132
         height: 12
         source: "qrc:/assets/images/tooltip/adornment-top.png"
         fillMode: Image.PreserveAspectFit
         opacity: 0.85
      }

      BorderImage {
         id: card
         objectName: "card"

         anchors.horizontalCenter: parent.horizontalCenter
         y: 11
         width: 360
         height: frame.height + 16
         source: "qrc:/assets/images/tooltip/tooltip-bg.png"
         border { left: 30; right: 30; top: 30; bottom: 30 }
         horizontalTileMode: BorderImage.Stretch
         verticalTileMode: BorderImage.Stretch

         ColumnLayout {
            id: frame
            objectName: "frame"

            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.topMargin: 6
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            height: header.implicitHeight
               + (root.analysisMode ? analysisBody.implicitHeight : legacyBody.implicitHeight)
               + credit.implicitHeight + 21
            spacing: 0

            Item {
               id: header
               Layout.fillWidth: true
               Layout.leftMargin: -15
               Layout.rightMargin: -15
               implicitHeight: 58

               Rectangle {
                  anchors.fill: parent
                  gradient: Gradient {
                     GradientStop { position: 0; color: "#c3130404" }
                     GradientStop { position: 0.65; color: "#8c0a0202" }
                     GradientStop { position: 1; color: "#e6000000" }
                  }
               }

               Image {
                  anchors.fill: parent
                  source: "qrc:/assets/images/tooltip/header-pattern.png"
                  fillMode: Image.PreserveAspectCrop
                  opacity: 0.72
               }

               Rectangle {
                  anchors.horizontalCenter: parent.horizontalCenter
                  anchors.top: parent.top
                  anchors.topMargin: 8
                  width: brand.implicitWidth + 16
                  height: 31
                  radius: 4
                  color: "#d9050000"
                  border.color: "#a67f0000"
                  border.width: 1

                  Row {
                     id: brand

                     anchors.centerIn: parent
                     spacing: 3

                     Image {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 17
                        height: 17
                        source: "qrc:/assets/images/tooltip/grimvault-mark.webp"
                        fillMode: Image.PreserveAspectFit
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
                  anchors.left: parent.left
                  anchors.right: parent.right
                  anchors.bottom: parent.bottom
                  height: 5
                  source: "qrc:/assets/images/tooltip/header-separator.png"
                  fillMode: Image.Stretch
                  opacity: 0.95
               }
            }

            ColumnLayout {
               id: analysisBody
               objectName: "analysisBody"

               Layout.fillWidth: true
               Layout.preferredHeight: visible ? implicitHeight : 0
               Layout.topMargin: 5
               Layout.leftMargin: 3
               Layout.rightMargin: 3
               visible: root.analysisMode
               spacing: 9

               Rectangle {
                  Layout.fillWidth: true
                  visible: root.enabledWidget ("market_value")
                  implicitHeight: hero.implicitHeight + 18
                  color: "#1ff6c453"
                  border.color: "#24f6c453"
                  border.width: 1

                  ColumnLayout {
                     id: hero

                     anchors.fill: parent
                     anchors.margins: 8
                     spacing: 3

                     RowLayout {
                        Layout.fillWidth: true

                        Text {
                           Layout.fillWidth: true
                           text: root.pricing.median > 0 ? "MARKET VALUE"
                              : root.utility.vendor_value > 0 ? "VENDOR VALUE" : "MARKET VALUE"
                           color: "#f6c453"
                           font.family: Theme.fontMedium
                           font.pixelSize: 11
                           font.letterSpacing: 1
                        }

                        Rectangle {
                           visible: root.pricing.confidence
                              && root.pricing.confidence !== "none"
                           implicitWidth: confidenceText.implicitWidth + 12
                           implicitHeight: confidenceText.implicitHeight + 8
                           color: root.pricing.confidence === "high" ? "#0e95fa00"
                              : root.pricing.confidence === "low" ? "#12fa1515" : "#0dffffff"
                           border.color: root.pricing.confidence === "high" ? "#4795fa00"
                              : root.pricing.confidence === "low" ? "#61fa1515" : "#2ed2c6ac"

                           Text {
                              id: confidenceText
                              anchors.centerIn: parent
                              text: (root.pricing.confidence || "medium").toUpperCase ()
                                 + " CONFIDENCE"
                              color: root.pricing.confidence === "high" ? "#95fa00"
                                 : root.pricing.confidence === "low" ? "#fa1515" : "#d0c3aa"
                              font.family: Theme.fontLight
                              font.pixelSize: 10
                              font.letterSpacing: 0.4
                           }
                        }
                     }

                     RowLayout {
                        Layout.fillWidth: true
                        spacing: 5

                        Image {
                           visible: root.pricing.median > 0 || root.utility.vendor_value > 0
                           source: "qrc:/assets/images/tooltip/gold-coin.png"
                           fillMode: Image.PreserveAspectFit
                           Layout.preferredWidth: 20
                           Layout.preferredHeight: 18
                        }

                        Text {
                           Layout.fillWidth: true
                           text: root.pricing.median > 0 ? root.money (root.pricing.median)
                              : root.utility.vendor_value > 0 ? root.money (root.utility.vendor_value)
                              : "No recent market data"
                           color: root.pricing.median > 0 || root.utility.vendor_value > 0
                              ? "#ffe2a0" : "#8d8375"
                           font.family: Theme.fontScript
                           font.pixelSize: root.pricing.median > 0
                              || root.utility.vendor_value > 0 ? 29 : 20
                        }

                        Text {
                           visible: root.pricing.low > 0 && root.pricing.high > 0
                           text: "Typical range\n" + root.money (root.pricing.low)
                              + " - " + root.money (root.pricing.high)
                           color: "#c0b5a1"
                           font.family: Theme.fontLight
                           font.pixelSize: 11
                           horizontalAlignment: Text.AlignRight
                        }
                     }

                     RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 5
                        visible: root.verdict ().label.length > 0
                        spacing: 8

                        Text {
                           text: "◆"
                           color: root.scoreColor
                           font.family: Theme.fontLight
                           font.pixelSize: 11
                        }

                        ColumnLayout {
                           Layout.fillWidth: true
                           spacing: 0

                           Text {
                              text: root.verdict ().label
                              color: root.scoreColor
                              font.family: Theme.fontMedium
                              font.pixelSize: 14
                           }

                           Text {
                              Layout.fillWidth: true
                              text: root.verdict ().hint
                              color: "#aaa08f"
                              font.family: Theme.fontLight
                              font.pixelSize: 11
                              wrapMode: Text.WordWrap
                           }
                        }
                     }
                  }
               }

               TooltipSection {
                  Layout.fillWidth: true
                  shown: root.enabledWidget ("item_overview")
                  title: "Item overview"
                  subtitle: "What this is"

                  TooltipValueGrid {
                     Layout.fillWidth: true
                     entries: root.overviewEntries ()
                  }
               }

               TooltipSection {
                  Layout.fillWidth: true
                  shown: (root.enabledWidget ("market_activity")
                     || root.enabledWidget ("actions")) && root.marketEntries ().length > 0
                  title: "Market overview"
                  subtitle: "How this item trades"

                  TooltipValueGrid {
                     Layout.fillWidth: true
                     entries: root.marketEntries ()
                  }
               }

               TooltipSection {
                  Layout.fillWidth: true
                  shown: root.enabledWidget ("similar_sales")
                     && (root.analysis.similar_sales || []).length > 0
                  title: "Recent sales"
                  subtitle: "Listings observed leaving the market"

                  Repeater {
                     model: root.analysis.similar_sales || []

                     Rectangle {
                        id: saleRow

                        required property var modelData

                        Layout.fillWidth: true
                        implicitHeight: saleColumn.implicitHeight + 14
                        color: "#e6050606"

                        ColumnLayout {
                           id: saleColumn

                           anchors.fill: parent
                           anchors.margins: 7
                           spacing: 3

                           RowLayout {
                              Layout.fillWidth: true

                              Text {
                                 Layout.fillWidth: true
                                 text: saleRow.modelData.similarity + "% similar  ·  "
                                    + root.elapsed (saleRow.modelData.age_seconds) + " ago"
                                 color: "#95fa00"
                                 font.family: Theme.fontMedium
                                 font.pixelSize: 12
                              }

                              Text {
                                 text: root.money (saleRow.modelData.price)
                                 color: "#f6c453"
                                 font.family: Theme.fontMedium
                                 font.pixelSize: 13
                              }
                           }

                           Flow {
                              Layout.fillWidth: true
                              visible: (saleRow.modelData.rolls || []).length > 0
                              spacing: 8

                              Repeater {
                                 model: saleRow.modelData.rolls || []

                                 Text {
                                    required property var modelData
                                    text: modelData.formatted_value + " " + modelData.label
                                    color: "#00aaee"
                                    font.family: Theme.fontLight
                                    font.pixelSize: 11
                                 }
                              }
                           }
                        }
                     }
                  }
               }

               TooltipSection {
                  Layout.fillWidth: true
                  shown: root.enabledWidget ("roll_quality")
                     && root.secondaryRolls ().length > 0
                  title: "Roll quality"
                  subtitle: root.analysis.weighted_roll_score !== undefined
                     ? "Item-specific market quality" : "Secondary stat magnitude"
                  value: root.score >= 0 ? root.score + " / 100" : ""
                  valueColor: root.scoreColor

                  Repeater {
                     model: root.secondaryRolls ()

                     Rectangle {
                        id: rollRow

                        required property var modelData

                        readonly property int percentile: Math.max (0,
                           Math.min (100, Number (modelData.roll_percentile) || 0))
                        readonly property bool fixed: modelData.minimum !== undefined
                           && modelData.maximum !== undefined
                           && modelData.minimum === modelData.maximum

                        Layout.fillWidth: true
                        implicitHeight: rollColumn.implicitHeight + 14
                        color: "#e6050606"

                        ColumnLayout {
                           id: rollColumn

                           anchors.fill: parent
                           anchors.leftMargin: 8
                           anchors.rightMargin: 8
                           anchors.topMargin: 6
                           anchors.bottomMargin: 7
                           spacing: 2

                           RowLayout {
                              Layout.fillWidth: true

                              Text {
                                 Layout.fillWidth: true
                                 text: rollRow.modelData.label || ""
                                 color: "#00aaee"
                                 font.family: Theme.fontLight
                                 font.pixelSize: 13
                                 elide: Text.ElideRight
                              }

                              Image {
                                 visible: !!rollRow.modelData.gem_icon_url
                                 source: rollRow.modelData.gem_icon_url || ""
                                 fillMode: Image.PreserveAspectFit
                                 Layout.preferredWidth: 15
                                 Layout.preferredHeight: 15
                              }

                              Text {
                                 text: rollRow.modelData.formatted_value || ""
                                 color: "#00aaee"
                                 font.family: Theme.fontMedium
                                 font.pixelSize: 15
                              }
                           }

                           RowLayout {
                              Layout.fillWidth: true

                              Text {
                                 Layout.fillWidth: true
                                 text: rollRow.fixed ? "Always this value"
                                    : rollRow.modelData.minimum !== undefined
                                       && rollRow.modelData.maximum !== undefined
                                       ? "+" + root.decimal (rollRow.modelData.minimum)
                                          + " - +" + root.decimal (rollRow.modelData.maximum)
                                          + ((rollRow.modelData.formatted_value || "").includes ("%")
                                             ? "%" : "") : ""
                                 color: "#aaa08f"
                                 font.family: Theme.fontLight
                                 font.pixelSize: 11
                              }

                              Text {
                                 text: rollRow.fixed ? "Fixed" : root.ordinal (rollRow.percentile)
                                    + " percentile"
                                    + (rollRow.modelData.grade ? "  ·  " + rollRow.modelData.grade : "")
                                 color: rollRow.modelData.grade
                                    ? root.gradeColor (rollRow.modelData.grade)
                                    : root.rollColor (rollRow.percentile)
                                 font.family: Theme.fontMedium
                                 font.pixelSize: 11
                              }
                           }
                        }

                        Rectangle {
                           anchors.left: parent.left
                           anchors.right: parent.right
                           anchors.leftMargin: 8
                           anchors.rightMargin: 8
                           anchors.bottom: parent.bottom
                           height: 3
                           color: "#0fffffff"

                           Rectangle {
                              width: parent.width * (rollRow.fixed ? 1 : rollRow.percentile / 100)
                              height: parent.height
                              color: rollRow.fixed ? "#38ffffff"
                                 : root.rollColor (rollRow.percentile)
                           }
                        }
                     }
                  }
               }

               TooltipSection {
                  Layout.fillWidth: true
                  shown: root.enabledWidget ("upgrade_paths")
                     && !["poor", "common", "artifact"].includes (root.analysis.item_rarity)
                     && (root.analysis.gem_plans || []).length > 0
                  title: "Upgrade paths"
                  subtitle: "Best value at maximum rolls"

                  Repeater {
                     model: root.analysis.gem_plans || []

                     Rectangle {
                        id: planCard

                        required property var modelData

                        Layout.fillWidth: true
                        implicitHeight: planColumn.implicitHeight
                        color: "#0ff6c453"
                        border.color: "#1ff6c453"
                        border.width: 1

                        ColumnLayout {
                           id: planColumn

                           anchors.left: parent.left
                           anchors.right: parent.right
                           spacing: 0

                           RowLayout {
                              Layout.fillWidth: true
                              Layout.margins: 8

                              ColumnLayout {
                                 Layout.fillWidth: true
                                 spacing: 0

                                 Text {
                                    text: planCard.modelData.sockets + " "
                                       + (planCard.modelData.sockets === 1 ? "gem" : "gems")
                                    color: "#f6ead0"
                                    font.family: Theme.fontMedium
                                    font.pixelSize: 12
                                 }

                                 Text {
                                    Layout.fillWidth: true
                                    text: "New value " + root.money (planCard.modelData.projected_value)
                                       + " · " + root.money (planCard.modelData.socket_fee) + " fee"
                                    color: "#aaa08f"
                                    font.family: Theme.fontLight
                                    font.pixelSize: 10
                                    elide: Text.ElideRight
                                 }
                              }

                              Text {
                                 text: root.money (planCard.modelData.net_uplift, true)
                                 color: planCard.modelData.net_uplift >= 0 ? "#95fa00" : "#fa1515"
                                 font.family: Theme.fontMedium
                                 font.pixelSize: 12
                              }
                           }

                           Repeater {
                              model: planCard.modelData.changes || []

                              Rectangle {
                                 id: changeRow

                                 required property var modelData

                                 Layout.fillWidth: true
                                 implicitHeight: 42
                                 color: "#26000000"

                                 RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    spacing: 5

                                    ColumnLayout {
                                       Layout.fillWidth: true
                                       spacing: 0
                                       Text { text: "REPLACE"; color: "#777064"; font.family: Theme.fontLight; font.pixelSize: 9 }
                                       Text { Layout.fillWidth: true; text: changeRow.modelData.replace_label; color: "#b5aa98"; font.family: Theme.fontLight; font.pixelSize: 11; elide: Text.ElideRight }
                                    }

                                    Text { text: "→"; color: "#b8a46e"; font.pixelSize: 12 }

                                    ColumnLayout {
                                       Layout.fillWidth: true
                                       spacing: 0
                                       Text { text: "WITH"; color: "#777064"; font.family: Theme.fontLight; font.pixelSize: 9 }
                                       Text { Layout.fillWidth: true; text: changeRow.modelData.new_label; color: "#00aaee"; font.family: Theme.fontLight; font.pixelSize: 11; elide: Text.ElideRight }
                                    }

                                    Image {
                                       source: changeRow.modelData.gem_icon_url || ""
                                       fillMode: Image.PreserveAspectFit
                                       Layout.preferredWidth: 20
                                       Layout.preferredHeight: 20
                                    }
                                 }
                              }
                           }
                        }
                     }
                  }
               }

               TooltipSection {
                  Layout.fillWidth: true
                  shown: root.enabledWidget ("trade_chat")
                     && (root.analysis.trade_chat || {}).messages
                     && (root.analysis.trade_chat || {}).messages.length > 0
                  title: "Recent trade chat"
                  subtitle: (root.analysis.trade_chat || {}).mentions_14d > 0
                     ? (root.analysis.trade_chat || {}).mentions_14d + " mentions in 14 days"
                     : "Recent player messages"

                  Repeater {
                     model: (root.analysis.trade_chat || {}).messages || []

                     Rectangle {
                        id: chatRow

                        required property var modelData
                        required property int index

                        Layout.fillWidth: true
                        implicitHeight: chatText.implicitHeight + 10
                        color: index % 2 ? "#0fffffff" : "#26000000"

                        RowLayout {
                           anchors.fill: parent
                           anchors.leftMargin: 6
                           anchors.rightMargin: 6
                           spacing: 5

                           Text {
                              text: root.elapsed (chatRow.modelData.age_seconds)
                              color: "#f6c453"
                              font.family: Theme.fontLight
                              font.pixelSize: 11
                           }

                           Text {
                              id: chatText
                              Layout.fillWidth: true
                              text: chatRow.modelData.message || ""
                              color: "#ddd4c4"
                              font.family: Theme.fontLight
                              font.pixelSize: 12
                              wrapMode: Text.Wrap
                           }
                        }
                     }
                  }
               }

               TooltipSection {
                  Layout.fillWidth: true
                  shown: root.enabledWidget ("best_drop_source")
                     && !!(root.analysis.source || {}).name
                  title: (root.analysis.source || {}).heading
                     || ((root.analysis.source || {}).kind === "drop"
                        ? "Best drop source" : "Acquisition")
                  subtitle: "Where to get another one"

                  Rectangle {
                     Layout.fillWidth: true
                     implicitHeight: 58
                     color: "#26000000"

                     RowLayout {
                        anchors.fill: parent
                        anchors.margins: 9
                        spacing: 8

                        Image {
                           visible: !!(root.analysis.source || {}).icon_url
                           source: (root.analysis.source || {}).icon_url || ""
                           fillMode: Image.PreserveAspectFit
                           Layout.preferredWidth: 40
                           Layout.preferredHeight: 40
                        }

                        ColumnLayout {
                           Layout.fillWidth: true
                           spacing: 0

                           Text {
                              Layout.fillWidth: true
                              text: (root.analysis.source || {}).name || ""
                              color: "#f2eadb"
                              font.family: Theme.fontMedium
                              font.pixelSize: 14
                              elide: Text.ElideRight
                           }

                           Text {
                              Layout.fillWidth: true
                              text: (root.analysis.source || {}).context || ""
                              color: "#aaa08f"
                              font.family: Theme.fontLight
                              font.pixelSize: 11
                              elide: Text.ElideRight
                           }
                        }

                        ColumnLayout {
                           visible: (root.analysis.source || {}).drop_rate > 0
                           spacing: 0

                           Text {
                              Layout.alignment: Qt.AlignRight
                              text: root.rate ((root.analysis.source || {}).drop_rate)
                              color: "#95fa00"
                              font.family: Theme.fontMedium
                              font.pixelSize: 17
                           }

                           Text {
                              Layout.alignment: Qt.AlignRight
                              text: "1 in " + Math.max (1,
                                 Math.round (1 / (root.analysis.source || {}).drop_rate))
                              color: "#aaa08f"
                              font.family: Theme.fontLight
                              font.pixelSize: 11
                           }
                        }
                     }
                  }
               }

               TooltipSection {
                  Layout.fillWidth: true
                  shown: root.enabledWidget ("quests")
                     && (root.analysis.quests || []).length > 0
                  title: "Quests"
                  subtitle: (root.analysis.quests || []).length > 1
                     ? (root.analysis.quests || []).length + " quests need it"
                     : "Where this is used"

                  Repeater {
                     model: root.analysis.quests || []

                     Rectangle {
                        id: questRow

                        required property var modelData

                        Layout.fillWidth: true
                        implicitHeight: 37
                        color: "#16000000"

                        RowLayout {
                           anchors.fill: parent
                           anchors.leftMargin: 9
                           anchors.rightMargin: 9
                           spacing: 8

                           Image {
                              visible: !!questRow.modelData.merchant_icon_url
                              source: questRow.modelData.merchant_icon_url || ""
                              fillMode: Image.PreserveAspectFit
                              Layout.preferredWidth: 26
                              Layout.preferredHeight: 26
                           }

                           ColumnLayout {
                              Layout.fillWidth: true
                              spacing: 0

                              Text {
                                 Layout.fillWidth: true
                                 text: questRow.modelData.quest_name
                                    || questRow.modelData.merchant_name || "Quest"
                                 color: "#f2eadb"
                                 font.family: Theme.fontMedium
                                 font.pixelSize: 12
                                 elide: Text.ElideRight
                              }

                              Text {
                                 Layout.fillWidth: true
                                 text: (questRow.modelData.merchant_name || "Unattributed")
                                    + (questRow.modelData.quest_index && questRow.modelData.quest_count
                                       ? " · Quest " + questRow.modelData.quest_index
                                          + " of " + questRow.modelData.quest_count : "")
                                 color: "#aaa08f"
                                 font.family: Theme.fontLight
                                 font.pixelSize: 11
                                 elide: Text.ElideRight
                              }
                           }

                           Text {
                              visible: questRow.modelData.quantity > 1
                              text: "×" + questRow.modelData.quantity
                              color: "#f6c453"
                              font.family: Theme.fontMedium
                              font.pixelSize: 12
                           }
                        }
                     }
                  }
               }

               TooltipSection {
                  Layout.fillWidth: true
                  shown: root.enabledWidget ("recipes")
                     && (root.analysis.recipes || []).length > 0
                  title: "Recipes"
                  subtitle: (root.analysis.recipes || []).length > 1
                     ? (root.analysis.recipes || []).length + " recipes use it"
                     : "What this crafts into"

                  Repeater {
                     model: (root.analysis.recipes || []).slice (0, 2)

                     Rectangle {
                        id: recipeRow

                        required property var modelData

                        Layout.fillWidth: true
                        implicitHeight: recipeColumn.implicitHeight + 12
                        color: "#e6050606"

                        ColumnLayout {
                           id: recipeColumn

                           anchors.left: parent.left
                           anchors.right: parent.right
                           anchors.margins: 6
                           spacing: 5

                           RowLayout {
                              Layout.fillWidth: true
                              spacing: 5

                              Image {
                                 visible: !!(recipeRow.modelData.output || {}).icon_url
                                 source: (recipeRow.modelData.output || {}).icon_url || ""
                                 fillMode: Image.PreserveAspectFit
                                 Layout.preferredWidth: 15
                                 Layout.preferredHeight: 15
                              }

                              Text {
                                 Layout.fillWidth: true
                                 text: (recipeRow.modelData.output || {}).name || "Recipe"
                                 color: "#f2eadb"
                                 font.family: Theme.fontMedium
                                 font.pixelSize: 12
                                 elide: Text.ElideRight
                              }

                              Text {
                                 text: recipeRow.modelData.merchant_name || ""
                                 color: "#aaa08f"
                                 font.family: Theme.fontLight
                                 font.pixelSize: 10
                              }
                           }

                           Flow {
                              Layout.fillWidth: true
                              spacing: 1

                              Repeater {
                                 model: recipeRow.modelData.materials || []

                                 Rectangle {
                                    id: material

                                    required property var modelData

                                    width: (recipeColumn.width - 1) / 2
                                    height: 27
                                    color: "#e6000000"

                                    RowLayout {
                                       anchors.fill: parent
                                       anchors.leftMargin: 6
                                       anchors.rightMargin: 6
                                       spacing: 5

                                       Image {
                                          visible: !!material.modelData.icon_url
                                          source: material.modelData.icon_url || ""
                                          fillMode: Image.PreserveAspectFit
                                          Layout.preferredWidth: 16
                                          Layout.preferredHeight: 16
                                       }

                                       Text {
                                          Layout.fillWidth: true
                                          text: material.modelData.name || ""
                                          color: material.modelData.is_this ? "#f6c453" : "#aaa08f"
                                          font.family: Theme.fontLight
                                          font.pixelSize: 11
                                          elide: Text.ElideRight
                                       }

                                       Text {
                                          text: "×" + Math.max (1, material.modelData.quantity || 1)
                                          color: material.modelData.is_this ? "#f6c453" : "#f2eadb"
                                          font.family: Theme.fontMedium
                                          font.pixelSize: 11
                                       }
                                    }
                                 }
                              }
                           }
                        }
                     }
                  }
               }
            }

            ColumnLayout {
               id: legacyBody

               Layout.fillWidth: true
               Layout.preferredHeight: visible ? implicitHeight : 0
               Layout.topMargin: 5
               Layout.leftMargin: 3
               Layout.rightMargin: 3
               visible: !root.analysisMode
               spacing: 6

               Text {
                  Layout.fillWidth: true
                  visible: !!root.legacySection ("text").body
                  text: root.legacySection ("text").body || ""
                  color: "#eee7d8"
                  font.family: Theme.fontLight
                  font.pixelSize: 13
                  horizontalAlignment: Text.AlignHCenter
                  wrapMode: Text.WordWrap
               }

               Repeater {
                  model: root.legacySection ("stats", "primary").entries || []

                  Text {
                     required property var modelData
                     Layout.fillWidth: true
                     text: "-  " + modelData.value + " " + modelData.label + "  -"
                     color: "#ffffff"
                     font.family: Theme.fontMedium
                     font.pixelSize: 14
                     horizontalAlignment: Text.AlignHCenter
                  }
               }

               Repeater {
                  model: root.legacySection ("stats", "secondary").entries || []

                  Text {
                     required property var modelData
                     Layout.fillWidth: true
                     text: "-  " + modelData.value + " " + modelData.label + "  -"
                     color: "#00aaee"
                     font.family: Theme.fontLight
                     font.pixelSize: 14
                     horizontalAlignment: Text.AlignHCenter
                  }
               }

               TooltipValueGrid {
                  Layout.fillWidth: true
                  visible: (root.legacySection ("rows").rows || []).length > 0
                  entries: root.legacySection ("rows").rows || []
               }
            }

            Text {
               id: credit

               Layout.alignment: Qt.AlignHCenter
               Layout.topMargin: 8
               Layout.bottomMargin: 8
               text: "POWERED BY DARKERDB.COM"
               color: "#8a8068"
               font.family: Theme.fontLight
               font.pixelSize: 9
               font.letterSpacing: 2
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
