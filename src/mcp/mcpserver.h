#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QRect>
#include <QSize>
#include <QString>
#include <functional>
#include <memory>

namespace glasshouse {

class McpHttpServer;
class PiKvmClient;
class VideoWindow;

// One head of the session, as the MCP client sees it.
//
// This is the whole reason Glasshouse supplies its own MCP server rather
// than reusing the PiKVM one (HARNESS.md §3): a per-PiKVM server speaks
// per-PiKVM pixels and cannot express "the monitor on the left", because
// no single PiKVM knows the others exist. The relationship between heads
// only exists where the streams meet, which is here.
struct McpScreen {
    int          index       = 0;        // 0-based; what tools take as `screen`
    int          monitorId   = 0;        // config target.monitors[].id
    QString      host;                   // the PiKVM serving this head
    QRect        monitorRect;            // this head within the logical desktop
    VideoWindow* window      = nullptr;  // frame source for screenshots
    PiKvmClient* client      = nullptr;  // per-head client
    bool         hidMaster   = false;
};

// Everything the tools are allowed to touch. main.cpp builds this from its
// instance list, so the MCP module never reaches into the viewer's internals.
struct McpSessionView {
    QSize            logicalDesktop;
    QList<McpScreen> screens;
    // The one client carrying keyboard and mouse for the target. Null if
    // the configured hid_master is not among the running windows, in which
    // case the input tools report that rather than silently picking one.
    PiKvmClient*     hid = nullptr;
    QString          hidHost;
};

// JSON-RPC + MCP protocol over McpHttpServer, with the Glasshouse tool set.
//
// Lives on the GUI thread and touches the session directly, so every tool
// runs in the same event loop as the windows it reads from. Tools that have
// to wait — a paste sequencing its chunks, a settle delay before a
// screenshot — answer asynchronously; nothing blocks the loop.
class McpServer : public QObject {
    Q_OBJECT
public:
    explicit McpServer(QObject* parent = nullptr);
    ~McpServer() override;

    // The view is copied on each call, so main.cpp can hand over a fresh
    // one whenever the session changes (a window closing, HID master
    // moving) without the server holding stale pointers.
    void setSession(const McpSessionView& view);

    bool start(const QString& bind, quint16 port, const QString& token,
               QString* error);
    void stop();
    bool isRunning() const;

signals:
    // Emitted for anything a tool did that changed the target's state, so
    // the viewer can surface it. The user should be able to tell that
    // something else is driving their keyboard.
    void toolActivity(const QString& summary);

private:
    using Done = std::function<void(const QJsonObject& result,
                                    const QString& error)>;

    void handleRequest(const QJsonObject& req,
                       std::function<void(const QJsonObject&)> respond);
    void callTool(const QString& name, const QJsonObject& args, Done done);
    QJsonArray toolSchemas() const;

    // Tools.
    void toolSession(const QJsonObject& args, Done done);
    void toolScreenshot(const QJsonObject& args, Done done);
    void toolType(const QJsonObject& args, Done done);
    void toolKeys(const QJsonObject& args, Done done);
    void toolMouseMove(const QJsonObject& args, Done done);
    void toolMouseClick(const QJsonObject& args, Done done);
    void toolMouseScroll(const QJsonObject& args, Done done);

    // Resolve `screen` (or the only screen, when there is one) or fail.
    const McpScreen* screenFor(const QJsonObject& args, QString* error) const;
    // Turn a tool's screen-relative or desktop-absolute point into a
    // desktop pixel. Reports which form it used in `how` for the reply.
    bool pointFor(const QJsonObject& args, QPointF* outDesktopPx,
                  QString* how, QString* error) const;
    // Guard every input tool: there has to be a HID master, and kvmd has
    // to report the target's HID online, or the event goes nowhere.
    bool hidReady(QString* error) const;

    std::unique_ptr<McpHttpServer> m_http;
    McpSessionView                 m_session;
};

}  // namespace glasshouse
