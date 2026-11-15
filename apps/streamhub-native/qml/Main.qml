import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: root
    visible: true
    title: "StreamHub · MTGPU"
    color: "#05080c"
    // Qt6/ Wayland 下 Screen.width/height 已是逻辑像素，devicePixelRatio 由 Qt 负责缩放；
    // 不要再除 dpr，否则 HiDPI 窗口会缩得过小。
    width: Math.min(1180, Screen.width - 80)
    height: Math.min(740, Screen.height - 120)
    minimumWidth: 620
    minimumHeight: 420
    font.family: "Noto Sans CJK SC"

    property var st: ({})
    property bool vmRunning: false
    property bool sunshineUp: false
    property bool clipboardUp: false
    property bool streamRunning: false
    property string streamApp: ""
    property real vpuPct: 0
    property var vpuHistory: []
    property var apps: []
    property var cfg: ({width: 1440, height: 900, fps: 120, bitrate: 40000,
                        display_mode: "windowed", ignore_aspect: true, mode: "low_latency"})
    property bool settingsOpen: false

    onClosing: function (close) {
        close.accepted = false
        root.visible = false
    }

    function api(method, path, body, cb) {
        var xhr = new XMLHttpRequest()
        xhr.open(method, "http://127.0.0.1:8760" + path)
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.onreadystatechange = function () {
            if (xhr.readyState !== XMLHttpRequest.DONE)
                return
            var data = null
            try { data = JSON.parse(xhr.responseText) } catch (e) {}
            if (cb)
                cb(xhr.status >= 200 && xhr.status < 300, data, xhr.status)
        }
        xhr.send(body ? JSON.stringify(body) : null)
    }

    function refreshStatus() {
        api("GET", "/api/status", null, function (ok, d) {
            if (!ok || !d)
                return
            st = d
            vmRunning = d.vm.running
            sunshineUp = d.sunshine.reachable
            clipboardUp = d.clipboard.ok
            streamRunning = d.stream.running
            streamApp = d.stream.app || ""
            vpuPct = d.vpu.encoder
            if (d.settings) {
                cfg = d.settings
                if (!cfg.mode)
                    cfg.mode = cfg.fps >= 120 ? "low_latency"
                             : (cfg.width <= 1280 ? "powersave" : "balanced")
            }
            vpuHistory = vpuHistory.concat([vpuPct]).slice(-60)
            ring.requestPaint()
            spark.requestPaint()
        })
    }

    function loadApps() {
        api("GET", "/api/apps", null, function (ok, d) {
            if (ok && d)
                apps = d
        })
    }

    function startStream(appName) {
        var body = {
            app: appName || cfg.app || "Desktop",
            width: cfg.width, height: cfg.height, fps: cfg.fps, bitrate: cfg.bitrate,
            display_mode: cfg.display_mode, ignore_aspect: cfg.ignore_aspect, mode: cfg.mode
        }
        api("POST", "/api/stream", body, function (ok, d, code) {
            toast(ok ? "正在串流：" + body.app : ("启动失败 " + ((d && d.error) || code)))
            refreshStatus()
        })
    }

    function streamWithMode(mode) {
        var body = {
            app: cfg.app || "Desktop",
            width: cfg.width, height: cfg.height, fps: cfg.fps, bitrate: cfg.bitrate,
            display_mode: mode, ignore_aspect: cfg.ignore_aspect, mode: cfg.mode
        }
        var go = function () {
            api("POST", "/api/stream", body, function (ok, d, code) {
                toast(ok ? (mode === "fullscreen"
                             ? "已全屏串流（Ctrl+Alt+Shift+F 可切换）"
                             : "已窗口串流")
                      : ("启动失败 " + ((d && d.error) || code)))
                refreshStatus()
            })
        }
        if (streamRunning) {
            api("POST", "/api/stream/stop", {}, function () { setTimeout(go, 900) })
        } else {
            go()
        }
    }

    function stopStream() {
        api("POST", "/api/stream/stop", {}, function (ok) {
            toast(ok ? "已停止串流" : "停止失败")
            refreshStatus()
        })
    }

    function toggleVm() {
        api("POST", "/api/vm", {action: vmRunning ? "stop" : "start"}, function (ok, d) {
            toast(ok && d.ok ? "操作成功" : "操作失败")
            refreshStatus()
        })
    }

    function applyMode(m) {
        var preset = m === "low_latency"
            ? {width: 1440, height: 900, fps: 120, bitrate: 40000}
            : (m === "powersave"
               ? {width: 1280, height: 720, fps: 60, bitrate: 20000}
               : {width: 1600, height: 900, fps: 60, bitrate: 30000})
        cfg = Object.assign({}, cfg, preset, {mode: m})
    }

    function saveSettings() {
        var old = cfg
        var body = {
            width: cfg.width, height: cfg.height, fps: cfg.fps, bitrate: cfg.bitrate,
            display_mode: cfg.display_mode, ignore_aspect: cfg.ignore_aspect, mode: cfg.mode
        }
        api("PUT", "/api/settings", body, function (ok) {
            if (!ok) {
                toast("保存失败")
                return
            }
            settingsOpen = false
            var needsRestart = streamRunning && (old.width !== cfg.width ||
                old.height !== cfg.height || old.fps !== cfg.fps ||
                old.display_mode !== cfg.display_mode ||
                old.ignore_aspect !== cfg.ignore_aspect)
            if (needsRestart) {
                // 分辨率/模式由 Sunshine 的 display device 在串流开始时程序化切换；
                // 重启一次串流即可，绝不要求用户去 Windows 设置里改。
                toast("正在按新分辨率重连…")
                api("POST", "/api/stream/stop", {}, function () {
                    api("POST", "/api/stream", {
                        app: cfg.app || "Desktop",
                        width: cfg.width, height: cfg.height, fps: cfg.fps,
                        bitrate: cfg.bitrate, display_mode: cfg.display_mode,
                        ignore_aspect: cfg.ignore_aspect
                    }, function (ok2, d2, code2) {
                        toast(ok2 ? "已按新分辨率重连" : ("重连失败 " + ((d2 && d2.error) || code2)))
                        refreshStatus()
                    })
                })
            } else {
                toast("设置已保存")
                refreshStatus()
            }
        })
    }

    function toast(msg) {
        toastText.text = msg
        toastBox.opacity = 1
        toastTimer.restart()
    }

    Component.onCompleted: {
        console.log("screen:", Screen.width, Screen.height, "dpr:", Screen.devicePixelRatio)
        loadApps()
        refreshStatus()
    }

    Timer {
        interval: 2000
        running: true
        repeat: true
        onTriggered: refreshStatus()
    }

    Timer {
        id: toastTimer
        interval: 2400
        onTriggered: toastBox.opacity = 0
    }

    // ---- background ----
    Rectangle {
        anchors.fill: parent
        color: "#05080c"
    }
    Canvas {
        id: glow
        anchors.fill: parent
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var g1 = ctx.createRadialGradient(width * 0.16, height * 0.02, 0,
                                              width * 0.16, height * 0.02, width * 0.55)
            g1.addColorStop(0, "rgba(94,234,212,0.16)")
            g1.addColorStop(1, "rgba(94,234,212,0)")
            ctx.fillStyle = g1
            ctx.fillRect(0, 0, width, height)
            var g2 = ctx.createRadialGradient(width * 0.95, 0, 0, width * 0.95, 0, width * 0.5)
            g2.addColorStop(0, "rgba(167,139,250,0.16)")
            g2.addColorStop(1, "rgba(167,139,250,0)")
            ctx.fillStyle = g2
            ctx.fillRect(0, 0, width, height)
        }
    }

    // ---- header ----
    Item {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 64

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 12
            Rectangle {
                width: 26; height: 26; radius: 13
                gradient: Gradient {
                    GradientStop { position: 0; color: "#5eead4" }
                    GradientStop { position: 0.6; color: "#7dd3fc" }
                    GradientStop { position: 1; color: "#a78bfa" }
                }
            }
            Text {
                text: "STREAMHUB"
                color: "#e9f2f7"
                font.pixelSize: 15
                font.bold: true
                font.letterSpacing: 2
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: "MTGPU · Windows on ARM"
                color: "#8fa3b0"
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 24
            anchors.verticalCenter: parent.verticalCenter
            spacing: 9
            Pill { label: "虚拟机"; value: vmRunning ? "运行中" : "已停止"; ok: vmRunning }
            Pill { label: "Sunshine"; value: sunshineUp ? "在线" : "离线"; ok: sunshineUp }
            Pill { label: "剪贴板"; value: clipboardUp ? "同步" : "关闭"; ok: clipboardUp }
            Pill { label: "编码"; value: streamRunning ? (vpuPct > 0.5 ? "MT VPU" : "软件") : "待机"; ok: streamRunning && vpuPct > 0.5 }
            Button {
                text: "⚙"
                font.pixelSize: 15
                implicitWidth: 38; implicitHeight: 38
                onClicked: settingsOpen = !settingsOpen
                background: Rectangle {
                    radius: 12
                    color: parent.hovered ? "#1b2530" : "#101820"
                    border.color: "#26323d"
                }
                contentItem: Text {
                    text: parent.text; color: "#e9f2f7"; font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
    Rectangle {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: 1
        color: "#1a2530"
    }

    // ---- main ----
    RowLayout {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 22
        spacing: 20

        // hero
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 560
            Layout.minimumWidth: 400
            radius: 26
            color: "#0d151c"
            border.color: "#1e2a35"
            border.width: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 24
                spacing: 16

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14

                    Rectangle {
                        width: 62; height: 62; radius: 20
                        color: "#13212a"
                        border.color: "#2a3a46"
                        Grid {
                            anchors.centerIn: parent
                            columns: 2
                            spacing: 3
                            Repeater {
                                model: 4
                                Rectangle { width: 11; height: 11; radius: 2; color: "#5eead4"; opacity: 0.9 }
                            }
                        }
                    }
                    ColumnLayout {
                        spacing: 4
                        Text {
                            text: "Windows 11 <font color='#5eead4'>ARM64</font>"
                            color: "#e9f2f7"; font.pixelSize: 24; textFormat: Text.StyledText
                        }
                        Text {
                            text: "QEMU · MT VPU H.264 · Moonlight V+"
                            color: "#8fa3b0"; font.pixelSize: 12
                        }
                    }
                    Item { Layout.fillWidth: true }

                    Item {
                        width: 128; height: 128
                        Canvas {
                            id: ring
                            anchors.fill: parent
                            property real pct: vpuPct
                            onPaint: {
                                var ctx = getContext("2d")
                                ctx.reset()
                                var c = width / 2, r = width / 2 - 9
                                ctx.beginPath(); ctx.arc(c, c, r, 0, Math.PI * 2)
                                ctx.strokeStyle = "rgba(255,255,255,0.08)"; ctx.lineWidth = 9; ctx.stroke()
                                ctx.beginPath()
                                ctx.arc(c, c, r, -Math.PI / 2, -Math.PI / 2 + Math.PI * 2 * Math.min(100, pct) / 100)
                                ctx.strokeStyle = "#5eead4"; ctx.lineWidth = 9; ctx.lineCap = "round"; ctx.stroke()
                            }
                        }
                        Column {
                            anchors.centerIn: parent
                            spacing: 2
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: vpuPct.toFixed(1) + "%"
                                color: "#e9f2f7"; font.pixelSize: 24; font.bold: true
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "VPU 编码"; color: "#8fa3b0"; font.pixelSize: 10
                            }
                        }
                    }
                }

                // sparkline
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 74
                    radius: 14
                    color: "#080f15"
                    border.color: "#1b2731"
                    Canvas {
                        id: spark
                        anchors.fill: parent
                        anchors.margins: 10
                        onPaint: {
                            var ctx = getContext("2d")
                            ctx.reset()
                            var w = width, h = height
                            ctx.strokeStyle = "rgba(255,255,255,0.05)"; ctx.lineWidth = 1
                            for (var i = 1; i < 4; i++) {
                                ctx.beginPath(); ctx.moveTo(0, h * i / 4); ctx.lineTo(w, h * i / 4); ctx.stroke()
                            }
                            var d = vpuHistory
                            if (!d || d.length < 2)
                                return
                            var max = Math.max(30, Math.max.apply(null, d)) * 1.25
                            var step = w / (d.length - 1)
                            ctx.beginPath()
                            for (var k = 0; k < d.length; k++) {
                                var x = k * step
                                var y = h - (d[k] / max) * (h - 8) - 4
                                if (k === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y)
                            }
                            ctx.strokeStyle = "#5eead4"; ctx.lineWidth = 2
                            ctx.shadowColor = "#5eead4"; ctx.shadowBlur = 8
                            ctx.stroke()
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Stat { label: "分辨率"; value: cfg.width + "×" + cfg.height }
                    Stat { label: "帧率"; value: String(cfg.fps) }
                    Stat { label: "码率"; value: Math.round(cfg.bitrate / 1000) + " Mbps" }
                    Stat { label: "编码器"; value: streamRunning ? (vpuPct > 0.5 ? "MT VPU" : "软件") : "待机" }
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: 38
                    radius: 12
                    color: "#0a1218"
                    border.color: "#1c2833"
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: streamRunning
                              ? "会话：" + (streamApp || "Desktop") + " · " +
                                (cfg.mode === "low_latency" ? "低延迟" : cfg.mode === "powersave" ? "省电" : "均衡") +
                                " · Moonlight V+ · 已连接 · " + vpuPct.toFixed(1) + "% VPU"
                              : "会话：未连接 · 选择应用或点击「开始串流」"
                        color: "#8fa3b0"; font.pixelSize: 11
                    }
                }

                Row {
                    spacing: 8
                    Repeater {
                        model: ["H.264", cfg.height + "p" + cfg.fps, "NV12 → MT VPU", "Moonlight V+"]
                        delegate: Rectangle {
                            height: 25; radius: 13
                            width: chip.implicitWidth + 20
                            color: "#0b1218"
                            border.color: "#22303b"
                            Text {
                                id: chip
                                anchors.centerIn: parent
                                text: modelData
                                color: "#8fa3b0"; font.pixelSize: 10
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                RowLayout {
                    spacing: 10
                    Button {
                        text: streamRunning ? "停止串流" : "开始串流"
                        implicitHeight: 46
                        implicitWidth: 150
                        onClicked: streamRunning ? stopStream() : startStream("")
                        background: Rectangle {
                            radius: 14
                            gradient: Gradient {
                                GradientStop { position: 0; color: "#5eead4" }
                                GradientStop { position: 1; color: "#a78bfa" }
                            }
                        }
                        contentItem: Text {
                            text: parent.text
                            color: "#04110f"; font.pixelSize: 14; font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Button {
                        text: "全屏串流"
                        implicitHeight: 46
                        implicitWidth: 120
                        onClicked: streamWithMode("fullscreen")
                        background: Rectangle {
                            radius: 14; color: "#101a22"; border.color: "#3a4a58"
                        }
                        contentItem: Text {
                            text: parent.text; color: "#5eead4"; font.pixelSize: 13; font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    Button {
                        text: vmRunning ? "停止虚拟机" : "启动虚拟机"
                        implicitHeight: 46
                        onClicked: toggleVm()
                        background: Rectangle {
                            radius: 14; color: "#101a22"; border.color: "#2a3a46"
                        }
                        contentItem: Text {
                            text: parent.text; color: "#e9f2f7"; font.pixelSize: 13
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
        }

        // apps + system
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            RowLayout {
                Text { text: "应用"; color: "#e9f2f7"; font.pixelSize: 17; font.bold: true }
                Item { Layout.fillWidth: true }
                Text { text: apps.length + " 个"; color: "#8fa3b0"; font.pixelSize: 11 }
            }

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 12
                rowSpacing: 12
                Repeater {
                    model: apps
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 124
                        radius: 20
                        color: "#0d151c"
                        border.color: "#1e2a35"
                        MouseArea {
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: startStream(modelData.name)
                            onEntered: parent.border.color = "#3a4a58"
                            onExited: parent.border.color = "#1e2a35"
                        }
                        Column {
                            anchors.left: parent.left
                            anchors.leftMargin: 18
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 16
                            spacing: 6
                            Rectangle {
                                width: 40; height: 40; radius: 12
                                color: "#13212a"; border.color: "#2a3a46"
                                Text {
                                    anchors.centerIn: parent
                                    text: modelData.name.substring(0, 1).toUpperCase()
                                    color: "#5eead4"; font.pixelSize: 18; font.bold: true
                                }
                            }
                            Text {
                                text: modelData.name
                                color: "#e9f2f7"; font.pixelSize: 15; font.bold: true
                            }
                            Text {
                                text: "Moonlight 串流 · " + cfg.width + "×" + cfg.height
                                color: "#8fa3b0"; font.pixelSize: 11
                            }
                        }
                        Text {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 14
                            text: "▶"; color: "#5eead4"; font.pixelSize: 14
                            opacity: 0.85
                        }
                    }
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 128
                radius: 20
                color: "#0b1218"
                border.color: "#1c2833"
                Column {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 9
                    Row { width: parent.width; Text { text: "Moonlight"; color: "#8fa3b0"; font.pixelSize: 11 }
                          Item { width: parent.width - 180; height: 1 }
                          Text { text: "V+ · 源码构建"; color: "#e9f2f7"; font.pixelSize: 11 } }
                    Row { width: parent.width; Text { text: "Sunshine"; color: "#8fa3b0"; font.pixelSize: 11 }
                          Item { width: parent.width - 180; height: 1 }
                          Text { text: "2026.914 · 源码构建"; color: "#e9f2f7"; font.pixelSize: 11 } }
                    Row { width: parent.width; Text { text: "虚拟化"; color: "#8fa3b0"; font.pixelSize: 11 }
                          Item { width: parent.width - 180; height: 1 }
                          Text { text: "QEMU · mdev · KVM"; color: "#e9f2f7"; font.pixelSize: 11 } }
                    Row { width: parent.width; Text { text: "串流"; color: "#8fa3b0"; font.pixelSize: 11 }
                          Item { width: parent.width - 180; height: 1 }
                          Text { text: cfg.display_mode + (cfg.ignore_aspect ? " · 拉伸" : " · 保持比例"); color: "#e9f2f7"; font.pixelSize: 11 } }
                }
            }
        }
    }

    // ---- settings drawer ----
    Rectangle {
        id: drawer
        width: 330
        height: parent.height
        anchors.right: parent.right
        color: "#0a1218"
        border.color: "#26323d"
        opacity: settingsOpen ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
        x: settingsOpen ? root.width - width : root.width
        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 22
            spacing: 14

            RowLayout {
                Text { text: "设置"; color: "#e9f2f7"; font.pixelSize: 17; font.bold: true }
                Item { Layout.fillWidth: true }
                Button {
                    text: "✕"; implicitWidth: 34; implicitHeight: 34
                    onClicked: settingsOpen = false
                    background: Rectangle { radius: 10; color: "#101820"; border.color: "#26323d" }
                    contentItem: Text { text: parent.text; color: "#e9f2f7"; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                }
            }

            Text { text: "串流模式"; color: "#8fa3b0"; font.pixelSize: 11 }
            RowLayout {
                spacing: 8
                Repeater {
                    model: [{key: "powersave", label: "省电"},
                            {key: "balanced", label: "均衡"},
                            {key: "low_latency", label: "低延迟"}]
                    Button {
                        Layout.fillWidth: true
                        implicitHeight: 34
                        text: modelData.label
                        onClicked: applyMode(modelData.key)
                        background: Rectangle {
                            radius: 10
                            color: cfg.mode === modelData.key ? "#123b3a" : "#101820"
                            border.color: cfg.mode === modelData.key ? "#2dd4bf" : "#26323d"
                        }
                        contentItem: Text {
                            text: parent.text
                            color: cfg.mode === modelData.key ? "#5eead4" : "#8fa3b0"
                            font.pixelSize: 12
                            font.bold: cfg.mode === modelData.key
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }
            Text {
                Layout.fillWidth: true
                text: "低延迟：1440×900@120/165，VPU 占用与功耗上升；均衡 1600×900@60；省电 1280×720@60。"
                color: "#6f8290"; font.pixelSize: 10; wrapMode: Text.WordWrap
            }

            Text { text: "分辨率"; color: "#8fa3b0"; font.pixelSize: 11 }
            ComboBox {
                id: resBox
                Layout.fillWidth: true
                model: ["1280x720", "1440x900", "1600x900", "1920x1080"]
                currentIndex: Math.max(0, model.indexOf(cfg.width + "x" + cfg.height))
                onActivated: function (i) {
                    var p = model[i].split("x")
                    cfg = Object.assign({}, cfg, {width: parseInt(p[0]), height: parseInt(p[1])})
                }
            }

            Text { text: "帧率"; color: "#8fa3b0"; font.pixelSize: 11 }
            ComboBox {
                Layout.fillWidth: true
                model: ["60", "120", "144", "165"]
                currentIndex: Math.max(0, model.indexOf(String(cfg.fps)))
                onActivated: function (i) { cfg = Object.assign({}, cfg, {fps: parseInt(model[i])}) }
            }

            Text { text: "码率：" + Math.round(cfg.bitrate / 1000) + " Mbps"; color: "#8fa3b0"; font.pixelSize: 11 }
            Text {
                Layout.fillWidth: true
                text: "码率只影响画质/带宽：localhost 可放心用 30–50Mbps 提升画质；调低码率主要省带宽，不会增加编码延迟。"
                color: "#6f8290"; font.pixelSize: 10; wrapMode: Text.WordWrap
            }
            Slider {
                id: bitrateSlider
                Layout.fillWidth: true
                from: 5; to: 80; stepSize: 5
                value: cfg.bitrate / 1000
                onMoved: cfg = Object.assign({}, cfg, {bitrate: value * 1000})
            }

            Text { text: "显示模式"; color: "#8fa3b0"; font.pixelSize: 11 }
            ComboBox {
                Layout.fillWidth: true
                model: ["windowed", "borderless", "fullscreen"]
                currentIndex: Math.max(0, model.indexOf(cfg.display_mode))
                onActivated: function (i) { cfg = Object.assign({}, cfg, {display_mode: model[i]}) }
            }

            RowLayout {
                Text { text: "拉伸填满（忽略宽高比）"; color: "#8fa3b0"; font.pixelSize: 12 }
                Item { Layout.fillWidth: true }
                Switch {
                    checked: cfg.ignore_aspect
                    onToggled: cfg = Object.assign({}, cfg, {ignore_aspect: checked})
                }
            }

            Text {
                Layout.fillWidth: true
                text: "⚠ 不要在 Windows 设置里手动改分辨率：切换时的确认对话框不会被串流，画面会卡住；请用此处设置，保存后会自动重连。"
                color: "#fbbf24"; font.pixelSize: 10; wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: "拉伸由 Moonlight 客户端 GPU 完成：窗口放大/全屏时画面跟随放大，允许模糊。分辨率由客户端通知 Windows 程序化切换（无确认框）。"
                color: "#6f8290"; font.pixelSize: 10; wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: "低延迟建议：1440×900 + 120/165fps。Sunshine display device 会自动切刷新率；120fps 下编码延迟约 22ms（60fps 时约 50ms）。"
                color: "#5eead4"; font.pixelSize: 10; wrapMode: Text.WordWrap
            }

            Item { Layout.fillHeight: true }

            Button {
                Layout.fillWidth: true
                implicitHeight: 42
                text: streamRunning ? "保存并重连" : "保存"
                onClicked: saveSettings()
                background: Rectangle {
                    radius: 12
                    gradient: Gradient {
                        GradientStop { position: 0; color: "#5eead4" }
                        GradientStop { position: 1; color: "#a78bfa" }
                    }
                }
                contentItem: Text {
                    text: parent.text; color: "#04110f"; font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    // ---- toast ----
    Rectangle {
        id: toastBox
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 26
        width: toastText.implicitWidth + 36
        height: 42
        radius: 12
        color: "#0c141a"
        border.color: "#33414d"
        opacity: 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Text {
            id: toastText
            anchors.centerIn: parent
            color: "#e9f2f7"; font.pixelSize: 12
        }
    }

    // ---- reusable ----
    component Pill: Rectangle {
        property string label: ""
        property string value: ""
        property bool ok: false
        width: 118; height: 32
        radius: 16
        color: "#0e161d"
        border.color: "#22303b"
        Row {
            anchors.centerIn: parent
            spacing: 6
            Rectangle {
                width: 7; height: 7; radius: 3.5
                color: ok ? "#34d399" : "#fb7185"
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: label; color: "#8fa3b0"; font.pixelSize: 10
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: value; color: ok ? "#34d399" : "#fda4af"; font.pixelSize: 10; font.bold: true
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    component Stat: Rectangle {
        property string label: ""
        property string value: ""
        Layout.fillWidth: true
        implicitHeight: 58
        radius: 14
        color: "#0a1218"
        border.color: "#1b2731"
        Column {
            anchors.left: parent.left
            anchors.leftMargin: 12
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            Text { text: label; color: "#8fa3b0"; font.pixelSize: 10 }
            Text { text: value; color: "#e9f2f7"; font.pixelSize: 13; font.bold: true }
        }
    }
}
