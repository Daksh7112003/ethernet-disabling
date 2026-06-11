/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

import QtQuick                  2.12
import QtQuick.Controls         2.4
import QtQuick.Dialogs          1.3
import QtQuick.Layouts          1.12

import QtLocation               5.3
import QtPositioning            5.3
import QtQuick.Window           2.2
import QtQml.Models             2.1
import QtGraphicalEffects 1.12

import QGroundControl               1.0
import QGroundControl.Controllers   1.0
import QGroundControl.Controls      1.0
import QGroundControl.FactSystem    1.0
import QGroundControl.FlightDisplay 1.0
import QGroundControl.FlightMap     1.0
import QGroundControl.Palette       1.0
import QGroundControl.ScreenTools   1.0
import QGroundControl.Vehicle       1.0


Item {
    id: _root

    // These should only be used by MainRootWindow
    property var planController:    _planController
    property var guidedController:  _guidedController

    PlanMasterController {
        id:                     _planController
        flyView:                true
        Component.onCompleted:  start()
    }


    property bool   _mainWindowIsMap:       mapControl.pipState.state === mapControl.pipState.fullState
    property bool   _isFullWindowItemDark:  _mainWindowIsMap ? mapControl.isSatelliteMap : true
    property var    _activeVehicle:         QGroundControl.multiVehicleManager.activeVehicle
    property var    _missionController:     _planController.missionController
    property var    _geoFenceController:    _planController.geoFenceController
    property var    _rallyPointController:  _planController.rallyPointController
    property real   _margins:               ScreenTools.defaultFontPixelWidth / 2
    property var    _guidedController:      guidedActionsController
    property var    _guidedActionList:      guidedActionList
    property var    _guidedValueSlider:     guidedValueSlider
    property var    _widgetLayer:           widgetLayer
    property real   _toolsMargin:           ScreenTools.defaultFontPixelWidth * 0.75
    property rect   _centerViewport:        Qt.rect(0, 0, width, height)
    property real   _rightPanelWidth:       ScreenTools.defaultFontPixelWidth * 30
    property var    _mapControl:            mapControl

    property real   _fullItemZorder:    0
    property real   _pipItemZorder:     QGroundControl.zOrderWidgets

    function _calcCenterViewPort() {
        var newToolInset = Qt.rect(0, 0, width, height)
        toolstrip.adjustToolInset(newToolInset)
        if (QGroundControl.corePlugin.options.instrumentWidget) {
            flightDisplayViewWidgets.adjustToolInset(newToolInset)
        }
    }

    QGCToolInsets {
        id:                     _toolInsets
        leftEdgeBottomInset:    _pipOverlay.visible ? _pipOverlay.x + _pipOverlay.width : 0
        bottomEdgeLeftInset:    _pipOverlay.visible ? parent.height - _pipOverlay.y : 0
    }

    FlyViewWidgetLayer {
        id:                     widgetLayer
        anchors.top:            parent.top
        anchors.bottom:         parent.bottom
        anchors.left:           parent.left
        anchors.right:          guidedValueSlider.visible ? guidedValueSlider.left : parent.right
        z:                      _fullItemZorder + 1
        parentToolInsets:       _toolInsets
        mapControl:             _mapControl
        visible:                !QGroundControl.videoManager.fullScreen
    }


    FlyViewCustomLayer {
        id:                 customOverlay
        anchors.fill:       widgetLayer
        z:                  _fullItemZorder + 2
        parentToolInsets:   widgetLayer.totalToolInsets
        mapControl:         _mapControl
        visible:            !QGroundControl.videoManager.fullScreen
    }

    // Development tool for visualizing the insets for a paticular layer, enable if needed
    /*
    FlyViewInsetViewer {
        id:                     widgetLayerInsetViewer
        anchors.top:            parent.top
        anchors.bottom:         parent.bottom
        anchors.left:           parent.left
        anchors.right:          guidedValueSlider.visible ? guidedValueSlider.left : parent.right

        z:                      widgetLayer.z + 1

        insetsToView:           customOverlay.totalToolInsets
    }*/

    GuidedActionsController {
        id:                 guidedActionsController
        missionController:  _missionController
        actionList:         _guidedActionList
        guidedValueSlider:     _guidedValueSlider
    }

    /*GuidedActionConfirm {
        id:                         guidedActionConfirm
        anchors.margins:            _margins
        anchors.bottom:             parent.bottom
        anchors.horizontalCenter:   parent.horizontalCenter
        z:                          QGroundControl.zOrderTopMost
        guidedController:           _guidedController
        guidedValueSlider:             _guidedValueSlider
    }*/

    GuidedActionList {
        id:                         guidedActionList
        anchors.margins:            _margins
        anchors.bottom:             parent.bottom
        anchors.horizontalCenter:   parent.horizontalCenter
        z:                          QGroundControl.zOrderTopMost
        guidedController:           _guidedController
    }

    //-- Guided value slider (e.g. altitude)
    GuidedValueSlider {
        id:                 guidedValueSlider
        anchors.margins:    _toolsMargin
        anchors.right:      parent.right
        anchors.top:        parent.top
        anchors.bottom:     parent.bottom
        z:                  QGroundControl.zOrderTopMost
        radius:             ScreenTools.defaultFontPixelWidth / 2
        width:              ScreenTools.defaultFontPixelWidth * 10
        color:              qgcPal.window
        visible:            false
    }

    FlyViewMap {
        id:                     mapControl
        planMasterController:   _planController
        rightPanelWidth:        ScreenTools.defaultFontPixelHeight * 9
        pipMode:                !_mainWindowIsMap
        toolInsets:             customOverlay.totalToolInsets
        mapName:                "FlightDisplayView"
    }

    FlyViewVideo {
        id: videoControl
    }

    Rectangle {
        id: servoRelease
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: _margins * 10
        width: parent.width / 10
        height: parent.height / 12
        radius: height / 2
        color: "#4A90E2"   // soft blue button color
        border.color: "#2C3E50"
        border.width: 1

        // Subtle shadow effect
        layer.enabled: true
        layer.effect: DropShadow {
            color: "#00000066"
            radius: 8
            samples: 16
            verticalOffset: 2
        }

        Text {
            id: releaseBtn
            text: qsTr("Release")
            color: "white"
            font.bold: true
            anchors.centerIn: parent
            font.pixelSize: parent.height / 2.5
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true

            onPressed: parent.color = "#357ABD"  // darker on press
            onReleased: parent.color = "#4A90E2" // normal color
            onClicked: {
                servoButtons.visible = true
                servoRelease.visible = false
            }

            onEntered: parent.color = "#5AA0FF"  // lighter on hover
            onExited: parent.color = "#4A90E2"
        }
    }


    Rectangle {
        id: servoButtons
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: _margins * 10
        color: "grey"
        height: parent.height / 12
        width: parent.width / 3
        radius: 8

        visible: false
        Row {
            spacing: parent.width / 40
            anchors.centerIn: parent

            // Button 1
            Rectangle {
                width: servoButtons.width / 8
                height: servoButtons.height * 0.8
                color: "lightblue"
                radius: 4
                border.color: "black"

                Text {
                    anchors.centerIn: parent
                    text: "Btn 1"
                    color: "black"
                    font.bold: true
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        let pwm  = 500
_activeVehicle.servoRelease(pwm ,3)
                    }
                }
            }

            // Button 2
            Rectangle {
                width: servoButtons.width / 8
                height: servoButtons.height * 0.8
                color: "lightblue"
                radius: 4
                border.color: "black"

                Text {
                    anchors.centerIn: parent
                    text: "Btn 2"
                    color: "black"
                    font.bold: true
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        let pwm  = 833
_activeVehicle.servoRelease(pwm ,10)
                    }
                }
            }

            // Button 3
            Rectangle {
                width: servoButtons.width / 8
                height: servoButtons.height * 0.8
                color: "lightblue"
                radius: 4
                border.color: "black"

                Text {
                    anchors.centerIn: parent
                    text: "Btn 3"
                    color: "black"
                    font.bold: true
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        let pwm  = 1166
_activeVehicle.servoRelease(pwm ,3)
                    }
                }
            }


            // Button 4
            Rectangle {
                width: servoButtons.width / 8
                height: servoButtons.height * 0.8
                color: "lightblue"
                radius: 4
                border.color: "black"

                Text {
                    anchors.centerIn: parent
                    text: "Btn 4"
                    color: "black"
                    font.bold: true
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        let pwm  = 1499
_activeVehicle.servoRelease(pwm ,3)
                    }
                }
            }

            // Button 5
            Rectangle {
                width: servoButtons.width / 8
                height: servoButtons.height * 0.8
                color: "lightblue"
                radius: 4
                border.color: "black"

                Text {
                    anchors.centerIn: parent
                    text: "Btn 5"
                    color: "black"
                    font.bold: true
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        let pwm  = 1832
_activeVehicle.servoRelease(pwm ,3)
                    }
                }
            }

            // Button 6
            Rectangle {
                width: servoButtons.width / 8
                height: servoButtons.height * 0.8
                color: "lightblue"
                radius: 4
                border.color: "black"

                Text {
                    anchors.centerIn: parent
                    text: "Btn 6"
                    color: "black"
                    font.bold: true
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        let pwm  = 2165
_activeVehicle.servoRelease(pwm ,3)
                    }
                }
            }
        }
    }


    QGCPipOverlay {
        id:                     _pipOverlay
        anchors.left:           parent.left
        anchors.bottom:         parent.bottom
        anchors.margins:        _toolsMargin
        item1IsFullSettingsKey: "MainFlyWindowIsMap"
        item1:                  mapControl
        item2:                  QGroundControl.videoManager.hasVideo ? videoControl : null
        fullZOrder:             _fullItemZorder
        pipZOrder:              _pipItemZorder
        show:                   !QGroundControl.videoManager.fullScreen &&
                                    (videoControl.pipState.state === videoControl.pipState.pipState || mapControl.pipState.state === mapControl.pipState.pipState)
    }

}
