#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

class QTcpServer;
class QTcpSocket;

namespace glasshouse {

// Minimal HTTP/1.1 front end for MCP's streamable-HTTP transport, over
// QTcpServer. Qt has no HTTP server, and pulling one in for this would be
// a disproportionate dependency: the protocol surface we need is one POST
// carrying JSON-RPC, framed by Content-Length.
//
// What it deliberately does NOT implement:
//
//   * Server-sent events. GET returns 405. That is the honest answer —
//     HARNESS.md §9.1 settles on the client polling a `watchers/poll`
//     tool rather than the server pushing, so there is no stream to open
//     and pretending otherwise would invite clients to wait on one.
//   * Sessions. No `Mcp-Session-Id` is issued; the spec permits a
//     stateless server and every tool call here is self-contained.
//   * Chunked request bodies. Clients send Content-Length for JSON-RPC.
//     A chunked body is rejected with 411 rather than mis-parsed.
//
// Requests are answered asynchronously: a tool that has to wait on the
// HID or on a video frame keeps its socket open and the response is
// written when the handler calls its responder. The socket is tracked so
// a client that disconnects mid-call drops the response instead of
// writing to a dead descriptor.
class McpHttpServer : public QObject {
    Q_OBJECT
public:
    // Called with a parsed JSON-RPC request object. The handler must
    // invoke `respond` exactly once, now or later, with the full
    // JSON-RPC response object — or with an empty object for a
    // notification, which is answered 202 with no body.
    using Responder = std::function<void(const QJsonObject&)>;
    using Handler   = std::function<void(const QJsonObject&, Responder)>;

    explicit McpHttpServer(QObject* parent = nullptr);
    ~McpHttpServer() override;

    void setHandler(Handler h) { m_handler = std::move(h); }
    // Empty token disables auth. When set, requests must carry
    // `Authorization: Bearer <token>`.
    void setToken(const QString& token) { m_token = token; }

    // Returns false and populates `error` on bind failure.
    bool listen(const QString& bindAddress, quint16 port, QString* error);
    void close();
    bool isListening() const;
    quint16 port() const;

private:
    struct Conn {
        QByteArray buffer;
        qint64     expected = -1;   // Content-Length, -1 until headers seen
        int        headerEnd = -1;  // offset just past the blank line
        bool       replied = false;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket* sock);
    void tryDispatch(QTcpSocket* sock);
    void writeResponse(QTcpSocket* sock, int status,
                       const QByteArray& contentType,
                       const QByteArray& body);

    std::unique_ptr<QTcpServer>       m_server;
    QHash<QTcpSocket*, Conn>          m_conns;
    Handler                           m_handler;
    QString                           m_token;
};

}  // namespace glasshouse
