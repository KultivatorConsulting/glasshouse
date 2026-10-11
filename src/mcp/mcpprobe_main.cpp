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

#include "mcpserver.h"

#include <QCoreApplication>
#include <QRect>
#include <QSize>
#include <cstdio>

using namespace glasshouse;

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

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
    srv->setSession(view);

    QString err;
    if (!srv->start(QStringLiteral("127.0.0.1"), port, QString(), &err)) {
        std::fprintf(stderr, "mcpprobe: cannot listen on 127.0.0.1:%u: %s\n",
                     port, qPrintable(err));
        return 1;
    }
    std::printf("mcpprobe listening on http://127.0.0.1:%u/mcp\n", port);
    std::fflush(stdout);

    return app.exec();
}
