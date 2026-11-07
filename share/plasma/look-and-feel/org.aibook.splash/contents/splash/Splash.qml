/*
    aibook splash screen: black background, the MTGPU rabbit in the centre
    (about a quarter of the screen wide), and the coloured frame lighting up
    one corner at a time, looping.

    The rabbit never changes: the animation only fades the four coloured frame
    quadrant sprites over a grey copy of the icon.  The ear tip that reaches
    into the top-right corner belongs to the grey base, so it is not touched.
*/
import QtQuick

Rectangle {
    id: root

    color: "black"

    // 0 .. 2*pi, one full turn per loop
    property real phase: 0

    // About a quarter of the screen wide, but never taller than half of it.
    readonly property real _size: Math.min(width / 4, height / 2)

    NumberAnimation on phase {
        from: 0
        to: Math.PI * 2
        duration: 2800
        loops: Animation.Infinite
        running: true
    }

    Item {
        id: icon

        anchors.centerIn: parent
        width: root._size
        height: root._size

        Image {
            id: base

            anchors.fill: parent
            source: "images/icon-gray.png"
            sourceSize.width: icon.width
            sourceSize.height: icon.height
            fillMode: Image.PreserveAspectFit
            smooth: true
            asynchronous: true
        }

        // Each sprite is the full-size icon with only one coloured frame
        // quadrant kept; stacking them over the grey base needs no offset or
        // scaling maths at all.
        Image {
            anchors.fill: base
            source: "images/q-tl.png"
            sourceSize.width: icon.width
            sourceSize.height: icon.height
            fillMode: Image.PreserveAspectFit
            smooth: true
            asynchronous: true
            opacity: Math.max(0, Math.sin(root.phase))
        }

        Image {
            anchors.fill: base
            source: "images/q-tr.png"
            sourceSize.width: icon.width
            sourceSize.height: icon.height
            fillMode: Image.PreserveAspectFit
            smooth: true
            asynchronous: true
            opacity: Math.max(0, Math.sin(root.phase - Math.PI / 2))
        }

        Image {
            anchors.fill: base
            source: "images/q-br.png"
            sourceSize.width: icon.width
            sourceSize.height: icon.height
            fillMode: Image.PreserveAspectFit
            smooth: true
            asynchronous: true
            opacity: Math.max(0, Math.sin(root.phase - Math.PI))
        }

        Image {
            anchors.fill: base
            source: "images/q-bl.png"
            sourceSize.width: icon.width
            sourceSize.height: icon.height
            fillMode: Image.PreserveAspectFit
            smooth: true
            asynchronous: true
            opacity: Math.max(0, Math.sin(root.phase - Math.PI * 1.5))
        }
    }
}
