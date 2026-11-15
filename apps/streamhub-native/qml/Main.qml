import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Window

ApplicationWindow {
    id: root
    width: Math.min(860, Screen.width - 100)
    height: Math.min(540, Screen.height - 100)
    minimumWidth: 640
    minimumHeight: 460
    visible: true
    title: "串流控制台"
    color: "#070d12"

    property var cfg: ({width: 1600, height: 900, fps: 60, bitrate: 25000,
                        display_mode: "windowed", ignore_aspect: true,
                        mode: "balanced", app: "Desktop"})
    property bool cfgLoaded: false
    property bool settingsOpen: false
    property bool vmRunning: false
    property bool sunshineUp: false
    property bool clipboardUp: false
    property bool streamRunning: false
    property string streamApp: ""
    property bool fullscreen: false
    property var stats: ({ok: false})
    property var apps: []
    property var appNames: []
    property real displayHz: (typeof hostRefreshRate !== "undefined" && hostRefreshRate > 0)
                             ? hostRefreshRate : 0
    property string toastText: ""
    property bool toastVisible: false
    property bool uiActive: visibility !== Window.Minimized && visibility !== Window.Hidden

    function api(method, path, body, cb) {
        var xhr = new XMLHttpRequest()
        xhr.open(method, "http://127.0.0.1:8760" + path)
        xhr.setRequestHeader("Content-Type", "application/json")
        xhr.onreadystatechange = function () {
            if (xhr.readyState !== XMLHttpRequest.DONE)
                return
            var d = null
            try { d = JSON.parse(xhr.responseText) } catch (e) {}
            if (cb)
                cb(xhr.status >= 200 && xhr.status < 300, d, xhr.status)
        }
        xhr.send(body ? JSON.stringify(body) : null)
    }

    function refreshStatus() {
        api("GET", "/api/status", null, function (ok, d) {
            if (!ok || !d)
                return
            vmRunning = d.vm.running
            sunshineUp = d.sunshine.reachable
            clipboardUp = d.clipboard.ok
            streamRunning = d.stream.running
            streamApp = d.stream.app || ""
            if (d.settings && !cfgLoaded) {
                cfg = d.settings
                if (!cfg.mode)
                    cfg.mode = cfg.fps >= 120 ? "low_latency"
                             : (cfg.width <= 1280 ? "powersave" : "balanced")
                cfgLoaded = true
            }
        })
    }

    function refreshApps() {
        api("GET", "/api/apps", null, function (ok, d) {
            if (ok && d) {
                apps = d
                appNames = d.map(function (a) { return a.name })
            }
        })
    }

    function refreshStats() {
        if (!streamRunning) {
            stats = ({ok: false})
            return
        }
        api("GET", "/api/stream/stats", null, function (ok, d) {
            stats = (ok && d) ? d : ({ok: false})
        })
    }

    function modeLabel(m) {
        return m === "low_latency" ? "低延迟" : (m === "powersave" ? "省电" : "均衡")
    }

    function startStream(appName) {
        var body = {
            app: appName || cfg.app || "Desktop",
            width: cfg.width, height: cfg.height, fps: cfg.fps,
            bitrate: cfg.bitrate, display_mode: "windowed",
            ignore_aspect: cfg.ignore_aspect, mode: cfg.mode
        }
        api("POST", "/api/stream", body, function (ok, d, code) {
            if (ok) {
                fullscreen = false
                toast("正在串流：" + body.app)
                refreshStatus()
            } else {
                toast("启动失败 " + ((d && d.error) || code))
            }
        })
    }

    function stopStream() {
        api("POST", "/api/stream/stop", {}, function (ok) {
            fullscreen = false
            stats = ({ok: false})
            toast(ok ? "串流已停止" : "停止失败")
            refreshStatus()
        })
    }

    function toggleFullscreen() {
        if (!streamRunning)
            return
        var target = !fullscreen
        api("POST", "/api/stream/fullscreen", {fullscreen: target}, function (ok, d) {
            if (ok) {
                fullscreen = target
                toast(target ? "已全屏（Ctrl+Alt+Shift+F 可切换）" : "已恢复窗口")
            } else {
                toast("切换失败 " + ((d && d.error) || ""))
            }
        })
    }

    function applyMode(m) {
        var preset = m === "low_latency"
            ? {width: 1440, height: 900, fps: 120, bitrate: 30000}
            : (m === "powersave"
               ? {width: 1280, height: 720, fps: 30, bitrate: 15000}
               : {width: 1600, height: 900, fps: 60, bitrate: 25000})
        cfg = Object.assign({}, cfg, preset, {mode: m})
        if (streamRunning) {
            toast("正在按" + modeLabel(m) + "模式重连…")
            api("POST", "/api/stream/stop", {}, function () {
                setTimeout(function () { startStream(cfg.app) }, 900)
            })
        }
    }

    function saveSettings() {
        var old = cfg
        var body = {
            width: cfg.width, height: cfg.height, fps: cfg.fps, bitrate: cfg.bitrate,
            display_mode: "windowed", ignore_aspect: cfg.ignore_aspect, mode: cfg.mode
        }
        api("PUT", "/api/settings", body, function (ok) {
            if (!ok) {
                toast("保存失败")
                return
            }
            settingsOpen = false
            var changed = old.width !== cfg.width || old.height !== cfg.height ||
                          old.fps !== cfg.fps || old.bitrate !== cfg.bitrate ||
                          old.ignore_aspect !== cfg.ignore_aspect || old.mode !== cfg.mode
            if (streamRunning && changed) {
                toast("正在按新设置重连…")
                api("POST", "/api/stream/stop", {}, function () {
                    setTimeout(function () { startStream(cfg.app) }, 900)
                })
            } else {
                toast("设置已保存")
                refreshStatus()
            }
        })
    }

    function toast(t) {
        toastText = t
        toastVisible = true
        toastTimer.restart()
    }

    Timer {
        id: toastTimer
        interval: 2600
        onTriggered: toastVisible = false
    }

    Timer {
        id: statusTimer
        interval: 2000
        running: uiActive
        repeat: true
        onTriggered: refreshStatus()
    }

    Timer {
        id: statsTimer
        interval: 1000
        running: streamRunning && uiActive
        repeat: true
        onTriggered: refreshStats()
    }

    Component.onCompleted: {
        refreshStatus()
        refreshApps()
    }

    component StatusPill: Rectangle {
        property string label: ""
        property bool on: false
        implicitHeight: 28
        implicitWidth: pillRow.implicitWidth + 22
        radius: 14
        color: on ? "#0d2b26" : "#1a1414"
        border.color: on ? "#1f6f5f" : "#3a2a2a"
        RowLayout {
            id: pillRow
            anchors.centerIn: parent
            spacing: 6
            Rectangle {
                width: 7; height: 7; radius: 4
                color: parent.parent.on ? "#2dd4bf" : "#ef4444"
            }
            Text {
                text: parent.parent.label
                color: parent.parent.on ? "#9fe8d8" : "#d8a0a0"
                font.pixelSize: 10
            }
        }
    }

    component DarkComboBox: ComboBox {
        id: cb
        Layout.fillWidth: true
        implicitHeight: 38
        background: Rectangle { radius: 10; color: "#0a1218"; border.color: "#26323d" }
        contentItem: Text {
            leftPadding: 10
            rightPadding: 26
            text: cb.displayText
            color: "#e9f2f7"
            font.pixelSize: 12
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        indicator: Text {
            x: cb.width - width - 10
            y: cb.topPadding + (cb.availableHeight - height) / 2
            text: "▾"
            color: "#6f8290"
            font.pixelSize: 12
        }
        popup: Popup {
            y: cb.height
            width: cb.width
            implicitHeight: Math.min(contentItem.implicitHeight, 220)
            padding: 4
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: cb.popup.visible ? cb.delegateModel : null
                currentIndex: cb.highlightedIndex
                ScrollIndicator.vertical: ScrollIndicator { }
            }
            background: Rectangle { radius: 10; color: "#0a1218"; border.color: "#26323d" }
        }
        delegate: ItemDelegate {
            width: cb.width
            contentItem: Text {
                text: modelData
                color: highlighted ? "#5eead4" : "#e9f2f7"
                font.pixelSize: 12
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle { color: highlighted ? "#102a26" : "transparent" }
        }
    }

    component StatTile: Rectangle {
        property string label: ""
        property string value: ""
        Layout.fillWidth: true
        implicitHeight: 76
        radius: 14
        color: "#0a1218"
        border.color: "#1c2833"
        ColumnLayout {
            anchors.centerIn: parent
            spacing: 2
            Text { text: label; color: "#6f8290"; font.pixelSize: 10 }
            Text { text: value; color: "#e9f2f7"; font.pixelSize: 19; font.bold: true }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 22
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            Image {
                source: "../rabbit.png"
                Layout.preferredWidth: 44
                Layout.preferredHeight: 44
                fillMode: Image.PreserveAspectFit
            }
            ColumnLayout {
                spacing: 0
                Text {
                    text: "串流控制台"
                    color: "#e9f2f7"
                    font.pixelSize: 18
                    font.bold: true
                }
                Text {
                    text: "MTGPU · Windows on ARM"
                    color: "#6f8290"
                    font.pixelSize: 11
                }
            }
            Item { Layout.fillWidth: true }
            StatusPill { label: "虚拟机"; on: vmRunning }
            StatusPill { label: "Sunshine"; on: sunshineUp }
            StatusPill { label: "串流"; on: streamRunning }
            Button {
                text: "⚙"
                implicitWidth: 38
                implicitHeight: 34
                onClicked: settingsOpen = true
                background: Rectangle { radius: 10; color: "#101820"; border.color: "#26323d" }
                contentItem: Text {
                    text: parent.text; color: "#e9f2f7"; font.pixelSize: 15
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: 20
            color: "#0d151c"
            border.color: "#1c2833"

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    ColumnLayout {
                        spacing: 2
                        Text {
                            text: "Windows 11 ARM64"
                            color: "#e9f2f7"
                            font.pixelSize: 20
                            font.bold: true
                        }
                        Text {
                            text: streamRunning
                                  ? ("正在串流 · " + (streamApp || "Desktop") + " · " + modeLabel(cfg.mode))
                                  : "选择应用后开始串流"
                            color: "#8fa3b0"
                            font.pixelSize: 11
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text { text: "应用"; color: "#6f8290"; font.pixelSize: 11 }
                    DarkComboBox {
                        id: appBox
                        Layout.preferredWidth: 190
                        Layout.fillWidth: false
                        model: appNames
                        currentIndex: Math.max(0, appNames.indexOf(cfg.app))
                        onActivated: function (i) {
                            if (appNames[i])
                                cfg = Object.assign({}, cfg, {app: appNames[i]})
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12
                    StatTile {
                        label: "码率"
                        value: (stats.ok && stats.bandwidth !== undefined && stats.bandwidth !== null)
                               ? stats.bandwidth.toFixed(1) + " Mbps" : "—"
                    }
                    StatTile {
                        label: "延迟"
                        value: (stats.ok && stats.latency !== undefined && stats.latency !== null)
                               ? stats.latency.toFixed(0) + " ms" : "—"
                    }
                    StatTile {
                        label: "串流帧率"
                        value: (stats.ok && stats.rx !== undefined && stats.rx !== null)
                               ? stats.rx.toFixed(0) + " fps"
                               : cfg.fps + " fps"
                    }
                }

                Text {
                    Layout.fillWidth: true
                    text: stats.ok
                          ? ("编码 " + (stats.encode !== null ? stats.encode.toFixed(1) : "—") +
                             " ms · 渲染 " + (stats.render !== null ? stats.render.toFixed(1) : "—") +
                             " ms · 解码 " + (stats.decode !== null ? stats.decode.toFixed(2) : "—") +
                             " ms · 网络 " + (stats.network !== null ? stats.network.toFixed(0) : "—") + " ms")
                          : (streamRunning ? "等待串流统计…" : "启动串流后显示实时码率/延迟/帧率")
                    color: "#5f7280"
                    font.pixelSize: 10
                    wrapMode: Text.WordWrap
                }

                Text {
                    visible: stats.ok && (stats.encode === null || stats.encode === undefined)
                    Layout.fillWidth: true
                    text: "Sunshine 未上报编码延迟，端到端延迟暂不可用；不会把网络延迟当成总延迟。"
                    color: "#fbbf24"
                    font.pixelSize: 9
                    wrapMode: Text.WordWrap
                }

                Text {
                    Layout.fillWidth: true
                    text: displayHz > 0
                          ? ("guest 采集 / Moonlight 渲染 " + cfg.fps + "fps；host 屏幕 " + Math.round(displayHz) + "Hz，实际呈现由合成器限制。")
                          : ("guest 采集 / Moonlight 渲染 " + cfg.fps + "fps；实际呈现受 host 屏幕刷新率限制。")
                    color: "#4f6270"
                    font.pixelSize: 9
                    wrapMode: Text.WordWrap
                }

                Item { Layout.fillHeight: true }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10

                    Button {
                        Layout.fillWidth: true
                        implicitHeight: 52
                        text: streamRunning ? "停止串流" : "开始串流"
                        onClicked: streamRunning ? stopStream() : startStream()
                        background: Rectangle {
                            radius: 14
                            color: streamRunning ? "#2a1216" : "#0f3a36"
                            border.color: streamRunning ? "#7f1d1d" : "#2dd4bf"
                        }
                        contentItem: Text {
                            text: parent.text
                            color: streamRunning ? "#fca5a5" : "#a7f3d0"
                            font.pixelSize: 15
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    Button {
                        implicitHeight: 52
                        implicitWidth: 120
                        enabled: streamRunning
                        text: fullscreen ? "窗口" : "全屏"
                        onClicked: toggleFullscreen()
                        background: Rectangle {
                            radius: 14
                            color: enabled ? "#101a22" : "#0b1015"
                            border.color: enabled ? "#3a4a58" : "#1c2833"
                        }
                        contentItem: Text {
                            text: parent.text
                            color: parent.enabled ? "#5eead4" : "#3f4f5a"
                            font.pixelSize: 13
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }

                    Button {
                        visible: !vmRunning
                        implicitHeight: 52
                        implicitWidth: 130
                        text: "启动虚拟机"
                        onClicked: {
                            api("POST", "/api/vm", {action: "start"}, function (ok, d) {
                                toast(ok ? "正在启动虚拟机…" : ("启动失败 " + ((d && d.error) || "")))
                                refreshStatus()
                            })
                        }
                        background: Rectangle { radius: 14; color: "#101820"; border.color: "#26323d" }
                        contentItem: Text {
                            text: parent.text; color: "#8fa3b0"; font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Text { text: "模式"; color: "#6f8290"; font.pixelSize: 10 }
                    Repeater {
                        model: [{key: "powersave", label: "省电"},
                                {key: "balanced", label: "均衡"},
                                {key: "low_latency", label: "低延迟"}]
                        Button {
                            implicitHeight: 30
                            implicitWidth: 82
                            text: modelData.label
                            onClicked: applyMode(modelData.key)
                            background: Rectangle {
                                radius: 9
                                color: cfg.mode === modelData.key ? "#0d2b26" : "#0a1218"
                                border.color: cfg.mode === modelData.key ? "#2dd4bf" : "#1c2833"
                            }
                            contentItem: Text {
                                text: parent.text
                                color: cfg.mode === modelData.key ? "#5eead4" : "#6f8290"
                                font.pixelSize: 11
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                    Item { Layout.fillWidth: true }
                    Text {
                        text: "节能 30fps · 均衡 60fps · 低延迟 120fps"
                        color: "#4f6270"
                        font.pixelSize: 9
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: "#000000"
        opacity: settingsOpen ? 0.45 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
        MouseArea { anchors.fill: parent; onClicked: settingsOpen = false }
    }

    Rectangle {
        id: drawer
        width: 380
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
                    contentItem: Text {
                        text: parent.text; color: "#e9f2f7"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            Text { text: "分辨率"; color: "#8fa3b0"; font.pixelSize: 11 }
            DarkComboBox {
                model: ["1280x720", "1440x900", "1600x900", "1920x1080"]
                currentIndex: Math.max(0, model.indexOf(cfg.width + "x" + cfg.height))
                onActivated: function (i) {
                    var p = model[i].split("x")
                    cfg = Object.assign({}, cfg, {width: parseInt(p[0]), height: parseInt(p[1])})
                }
            }

            Text { text: "帧率"; color: "#8fa3b0"; font.pixelSize: 11 }
            DarkComboBox {
                model: ["30", "60", "120", "144", "165"]
                currentIndex: Math.max(0, model.indexOf(String(cfg.fps)))
                onActivated: function (i) { cfg = Object.assign({}, cfg, {fps: parseInt(model[i])}) }
            }
            Text {
                Layout.fillWidth: true
                text: "节能 30fps / 均衡 60fps / 低延迟 120fps；高帧率会增加 VPU/CPU 占用。"
                color: "#6f8290"; font.pixelSize: 10; wrapMode: Text.WordWrap
            }

            Text {
                text: "码率：" + Math.round(cfg.bitrate / 1000) + " Mbps"
                color: "#8fa3b0"; font.pixelSize: 11
            }
            Slider {
                Layout.fillWidth: true
                from: 5; to: 80; stepSize: 5
                value: cfg.bitrate / 1000
                onMoved: cfg = Object.assign({}, cfg, {bitrate: value * 1000})
            }
            Text {
                Layout.fillWidth: true
                text: "码率只影响画质/带宽：localhost 可放心用 30–50Mbps；调低主要省带宽，不会增加编码延迟。"
                color: "#6f8290"; font.pixelSize: 10; wrapMode: Text.WordWrap
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
                text: "⚠ 不要在 Windows 设置里手动改分辨率：切换确认框不会被串流，画面会卡住；请用此处设置，保存后自动重连。"
                color: "#fbbf24"; font.pixelSize: 10; wrapMode: Text.WordWrap
            }
            Text {
                Layout.fillWidth: true
                text: "全屏可在串流中直接切换（Moonlight 窗口 EWMH fullscreen），无需重连；也可用 Ctrl+Alt+Shift+F。"
                color: "#5eead4"; font.pixelSize: 10; wrapMode: Text.WordWrap
            }

            Item { Layout.fillHeight: true }

            Button {
                Layout.fillWidth: true
                implicitHeight: 44
                text: streamRunning ? "保存并重连" : "保存"
                onClicked: saveSettings()
                background: Rectangle { radius: 12; color: "#0f3a36"; border.color: "#2dd4bf" }
                contentItem: Text {
                    text: parent.text; color: "#a7f3d0"; font.pixelSize: 13; font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    Rectangle {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 26
        width: toastText.implicitWidth + 34
        height: 36
        radius: 18
        color: "#13202a"
        border.color: "#2dd4bf"
        opacity: toastVisible ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 160 } }
        Text {
            id: toastText
            anchors.centerIn: parent
            text: root.toastText
            color: "#a7f3d0"
            font.pixelSize: 11
        }
    }

    onClosing: function (close) {
        // Closing the window must not leave a background poller behind: stop
        // both timers and quit; the tray menu starts a fresh instance.
        close.accepted = true
        statusTimer.stop()
        statsTimer.stop()
        Qt.quit()
    }
}
