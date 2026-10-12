// Probe driver: stands up the MCP server against a synthetic two-head
// session, with no PiKVMs and no windows.
//
// It exists because the interesting half of this server is the session it
// describes — screen indexes, the combined desktop, which host carries HID
// — and that can be exercised without hardware. Point an MCP client at it
// to develop against the tool surface before wiring anything real, which
// is exactly what HARNESS.md §8.1 calls the tier-2 consumer.
//
//   glasshouse-mcpprobe [port]
//   claude mcp add --transport http glasshouse-probe http://127.0.0.1:18766/mcp
//
// The input tools will refuse: there is no HID master, and saying so is
// the behaviour worth checking.
//
// Screen 0 is also fed a synthetic 30 fps stream with a toast that appears
// for a second every five, so the watcher tools (HARNESS.md §9.1) can be
// driven end to end — create, poll, read the history back — without a
// PiKVM. Screen 1 is deliberately left with no frames, because a watcher
// created against a stream that has not started yet has to say so.
//
// Needs a QGuiApplication rather than a QCoreApplication: the filmstrip
// draws timestamps, and QPainter::drawText reaches the font database.
// Run headless with QT_QPA_PLATFORM=offscreen.

#include "mcpserver.h"
#include "watcher.h"

#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QRect>
#include <QSize>
#include <QTimer>
#include <cstdio>

using namespace glasshouse;

namespace {

// One synthetic frame. `toast` paints a notification-shaped rectangle in
// the top right, which is where a watcher in the demo would be pointed.
QImage syntheticFrame(bool toast, int tick) {
    QImage img(1920, 1080, QImage::Format_RGB888);
    img.fill(QColor(18, 20, 24));
    QPainter p(&img);
    // A little steady motion elsewhere, so a careless whole-screen
    // rectangle demonstrates why a tight one is wanted.
    p.fillRect(QRect(40, 980, 60 + (tick % 20) * 8, 24), QColor(60, 70, 90));
    if (toast) {
        p.fillRect(QRect(1480, 60, 380, 110), QColor(240, 240, 245));
        p.fillRect(QRect(1500, 80, 40, 40), QColor(70, 130, 220));
    }
    p.end();
    return img;
}

}  // namespace

int main(int argc, char** argv) {
    QGuiApplication app(argc, argv);

    const quint16 port = argc > 1
        ? static_cast<quint16>(QString::fromLatin1(argv[1]).toUShort())
        : 18766;

    auto* srv = new McpServer(&app);

    // Two 1920x1080 heads side by side — the same layout as the coordinate
    // tests, so a pixel quoted here matches those golden wire values.
    McpSessionView view;
    view.logicalDesktop = QSize(3840, 1080);

    McpScreen left;
    left.index       = 0;
    left.monitorId   = 1;
    left.host        = QStringLiteral("10.0.0.1");
    left.monitorRect = QRect(0, 0, 1920, 1080);
    left.hidMaster   = true;

    McpScreen right;
    right.index       = 1;
    right.monitorId   = 2;
    right.host        = QStringLiteral("10.0.0.2");
    right.monitorRect = QRect(1920, 0, 1920, 1080);

    view.screens = {left, right};
    view.hidHost = left.host;
    view.hid     = nullptr;   // nothing connected: input tools must refuse

    auto* watchers = new WatcherRegistry(&app);
    view.watchers  = watchers;
    srv->setSession(view);

    // 30 fps of synthetic video into screen 0 only.
    auto* tick  = new int(0);
    auto* feeder = new QTimer(&app);
    QObject::connect(feeder, &QTimer::timeout, watchers, [watchers, tick]() {
        const int t = (*tick)++;
        const bool toast = (t / 30) % 5 == 0;
        watchers->ingestImage(0, syntheticFrame(toast, t));
    });
    feeder->start(33);

    QString err;
    if (!srv->start(QStringLiteral("127.0.0.1"), port, QString(), &err)) {
        std::fprintf(stderr, "mcpprobe: cannot listen on 127.0.0.1:%u: %s\n",
                     port, qPrintable(err));
        return 1;
    }
    std::printf("mcpprobe listening on http://127.0.0.1:%u/mcp\n", port);
    std::printf("screen 0 is fed 30fps of synthetic video (a toast appears "
                "at 1480,60 380x110 for 1s in every 5); screen 1 has no "
                "frames\n");
    std::fflush(stdout);

    return app.exec();
}
