// StreamHub native — the window launched by the rabbit tray monitor.
// No own tray icon: the rabbit tray owns the icon/status; this is its control
// window. Everything is driven through the local FastAPI backend.
#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QProcess>
#include <QScreen>
#include <QQuickWindow>
#include <QTimer>

static QString appDir() { return QCoreApplication::applicationDirPath(); }

static void ensureBackend()
{
    const QString cmd =
        "curl -sf --max-time 1 http://127.0.0.1:8760/api/status >/dev/null || "
        "(cd /opt/mtgpu-woa/streamhub && nohup python3 -m uvicorn backend.app:app "
        "--host 127.0.0.1 --port 8760 >> streamhub.log 2>&1 &)";
    QProcess::startDetached("bash", {"-c", cmd});
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName("winvm-hub");
    app.setOrganizationName("mtgpu-woa");
    app.setApplicationDisplayName("Windows on ARM");
    app.setQuitOnLastWindowClosed(true);
    app.setWindowIcon(QIcon(appDir() + "/rabbit.png"));

    ensureBackend();

    QQmlApplicationEngine engine;
    auto *screen = QGuiApplication::primaryScreen();
    engine.rootContext()->setContextProperty("hostRefreshRate",
                                            screen ? screen->refreshRate() : 0.0);
    const QUrl url = QUrl::fromLocalFile(appDir() + "/qml/Main.qml");
    engine.load(url);
    if (engine.rootObjects().isEmpty())
        return 1;

    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().constFirst());
    if (!window)
        return 1;

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

    return app.exec();
}
