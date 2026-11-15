// StreamHub native — a small Qt6/QML control panel for the mtgpu-woa stack.
// No Chromium: a QQuickWindow plus a QSystemTrayIcon; the stack itself is
// driven through the local FastAPI backend (started on demand).
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QMenu>
#include <QQmlApplicationEngine>
#include <QProcess>
#include <QQuickWindow>
#include <QSystemTrayIcon>
#include <QTimer>

static QString appDir() { return QCoreApplication::applicationDirPath(); }

static void ensureBackend()
{
    // Start the (tiny) Python control plane if it is not already listening.
    const QString cmd =
        "curl -sf --max-time 1 http://127.0.0.1:8760/api/status >/dev/null || "
        "(cd /opt/mtgpu-woa/streamhub && nohup python3 -m uvicorn backend.app:app "
        "--host 127.0.0.1 --port 8760 >> streamhub.log 2>&1 &)";
    QProcess::startDetached("bash", {"-c", cmd});
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("StreamHub");
    app.setOrganizationName("mtgpu-woa");
    app.setApplicationDisplayName("StreamHub");
    app.setQuitOnLastWindowClosed(false);

    ensureBackend();

    QQmlApplicationEngine engine;
    const QUrl url = QUrl::fromLocalFile(appDir() + "/qml/Main.qml");
    engine.load(url);
    if (engine.rootObjects().isEmpty())
        return 1;

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return 1;

    // --screenshot <path>: grab the window (works on Wayland too) and exit.
    QString shotPath;
    for (int i = 1; i < argc; ++i) {
        if (QString(argv[i]) == "--screenshot" && i + 1 < argc) {
            shotPath = QString(argv[++i]);
            break;
        }
    }
    if (!shotPath.isEmpty()) {
        QTimer::singleShot(1800, window, [window, shotPath]() {
            window->grabWindow().save(shotPath);
            QCoreApplication::quit();
        });
    }

    QSystemTrayIcon tray(QIcon(appDir() + "/icon.svg"));
    tray.setToolTip(QStringLiteral("StreamHub · MTGPU"));

    QMenu menu;
    QAction *showAct = menu.addAction(QStringLiteral("显示面板"));
    QAction *streamAct = menu.addAction(QStringLiteral("开始串流"));
    menu.addSeparator();
    QAction *quitAct = menu.addAction(QStringLiteral("退出"));
    tray.setContextMenu(&menu);

    auto showWindow = [window]() {
        window->show();
        window->raise();
        window->requestActivate();
    };
    QObject::connect(showAct, &QAction::triggered, &app, showWindow);
    QObject::connect(streamAct, &QAction::triggered, []() {
        QProcess::startDetached(
            "bash", {"-c",
                     "curl -s -X POST http://127.0.0.1:8760/api/stream "
                     "-H 'Content-Type: application/json' -d '{}' >/dev/null 2>&1"});
    });
    QObject::connect(quitAct, &QAction::triggered, &app, &QApplication::quit);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &app,
                     [showWindow](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger)
                             showWindow();
                     });
    tray.show();

    return app.exec();
}
