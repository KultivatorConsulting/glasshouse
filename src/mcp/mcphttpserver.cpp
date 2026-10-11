#include "mcphttpserver.h"

#include "logging.h"

#include <QHostAddress>
#include <QJsonDocument>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>

namespace glasshouse {

namespace {

// Guard against a client that opens a socket and streams forever. JSON-RPC
// requests here are small; the only large payload travels outbound (a
// screenshot), so an inbound body this size is a bug or an attack.
constexpr qint64 kMaxBodyBytes = 1 * 1024 * 1024;

QByteArray statusText(int status) {
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        default:  return "Error";
    }
}

// Case-insensitive single-header lookup over the raw header block.
QByteArray headerValue(const QByteArray& headers, const char* name) {
    const QByteArray needle = QByteArray("\r\n") + name + ":";
    int at = headers.indexOf(needle, 0);
    if (at < 0) {
        // Could be the very first header, with no preceding CRLF.
        const QByteArray first = QByteArray(name) + ":";
        if (headers.startsWith(first)) {
            at = 0;
        } else {
            // Retry case-insensitively; clients vary on capitalisation.
            const QByteArray lower = headers.toLower();
            const QByteArray lneedle = QByteArray("\r\n")
                                     + QByteArray(name).toLower() + ":";
            at = lower.indexOf(lneedle);
            if (at < 0) {
                if (!lower.startsWith(QByteArray(name).toLower() + ":")) return {};
                at = 0;
            }
        }
    }
    const int colon = headers.indexOf(':', at);
    if (colon < 0) return {};
    int end = headers.indexOf("\r\n", colon);
    if (end < 0) end = headers.size();
    return headers.mid(colon + 1, end - colon - 1).trimmed();
}

}  // namespace

McpHttpServer::McpHttpServer(QObject* parent)
    : QObject(parent), m_server(std::make_unique<QTcpServer>()) {
    connect(m_server.get(), &QTcpServer::newConnection,
            this, &McpHttpServer::onNewConnection);
}

McpHttpServer::~McpHttpServer() = default;

bool McpHttpServer::listen(const QString& bindAddress, quint16 port,
                           QString* error) {
    QHostAddress addr;
    if (!addr.setAddress(bindAddress)) {
        if (error) {
            *error = QStringLiteral("mcp.bind is not an IP address: %1")
                         .arg(bindAddress);
        }
        return false;
    }
    if (!m_server->listen(addr, port)) {
        if (error) *error = m_server->errorString();
        return false;
    }
    return true;
}

void McpHttpServer::close() {
    m_server->close();
    for (auto it = m_conns.begin(); it != m_conns.end(); ++it) {
        it.key()->disconnectFromHost();
    }
    m_conns.clear();
}

bool    McpHttpServer::isListening() const { return m_server->isListening(); }
quint16 McpHttpServer::port() const        { return m_server->serverPort(); }

void McpHttpServer::onNewConnection() {
    while (QTcpSocket* sock = m_server->nextPendingConnection()) {
        m_conns.insert(sock, Conn{});
        connect(sock, &QTcpSocket::readyRead, this,
                [this, sock]() { onReadyRead(sock); });
        connect(sock, &QTcpSocket::disconnected, this, [this, sock]() {
            m_conns.remove(sock);
            sock->deleteLater();
        });
    }
}

void McpHttpServer::onReadyRead(QTcpSocket* sock) {
    auto it = m_conns.find(sock);
    if (it == m_conns.end()) return;
    it->buffer.append(sock->readAll());

    if (it->headerEnd < 0) {
        const int blank = it->buffer.indexOf("\r\n\r\n");
        if (blank < 0) {
            // Headers still arriving. Cap the wait so a client dribbling
            // bytes with no blank line cannot pin memory indefinitely.
            if (it->buffer.size() > kMaxBodyBytes) {
                writeResponse(sock, 413, "text/plain", "headers too large\n");
            }
            return;
        }
        it->headerEnd = blank + 4;

        const QByteArray headers = it->buffer.left(blank);

        if (!headerValue(headers, "Transfer-Encoding").isEmpty()) {
            writeResponse(sock, 411, "text/plain",
                          "chunked request bodies are not supported; "
                          "send Content-Length\n");
            return;
        }
        const QByteArray len = headerValue(headers, "Content-Length");
        it->expected = len.isEmpty() ? 0 : len.toLongLong();
        if (it->expected < 0 || it->expected > kMaxBodyBytes) {
            writeResponse(sock, 413, "text/plain", "request body too large\n");
            return;
        }

        // Only POST carries JSON-RPC. GET would be the SSE stream, which
        // this server does not open — see the header comment.
        if (!headers.startsWith("POST ")) {
            if (headers.startsWith("GET ")) {
                writeResponse(sock, 405, "text/plain",
                              "this MCP server does not open an SSE stream; "
                              "POST JSON-RPC instead\n");
            } else {
                writeResponse(sock, 405, "text/plain", "POST only\n");
            }
            return;
        }

        if (!m_token.isEmpty()) {
            const QByteArray auth = headerValue(headers, "Authorization");
            const QByteArray want = "Bearer " + m_token.toUtf8();
            if (auth != want) {
                qCWarning(lcMcp) << "rejected request with bad or missing token";
                writeResponse(sock, 401, "text/plain",
                              "missing or invalid bearer token\n");
                return;
            }
        }
    }

    tryDispatch(sock);
}

void McpHttpServer::tryDispatch(QTcpSocket* sock) {
    auto it = m_conns.find(sock);
    if (it == m_conns.end() || it->replied || it->headerEnd < 0) return;
    if (it->buffer.size() - it->headerEnd < it->expected) return;  // body incomplete

    const QByteArray body = it->buffer.mid(it->headerEnd,
                                           static_cast<int>(it->expected));

    QJsonParseError perr{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        // A parse error has no id to echo, so the JSON-RPC envelope uses
        // null — which is what the spec asks for in exactly this case.
        QJsonObject err{{"code", -32700}, {"message", perr.errorString()}};
        const QJsonObject resp{{"jsonrpc", "2.0"}, {"id", QJsonValue()},
                               {"error", err}};
        writeResponse(sock, 400, "application/json",
                      QJsonDocument(resp).toJson(QJsonDocument::Compact));
        return;
    }

    if (!m_handler) {
        writeResponse(sock, 404, "text/plain", "no handler installed\n");
        return;
    }

    // The socket may die while a tool is in flight, so capture a guarded
    // pointer and drop the response rather than writing to a dead one.
    QPointer<QTcpSocket> guard(sock);
    m_handler(doc.object(), [this, guard](const QJsonObject& resp) {
        if (!guard) return;
        if (resp.isEmpty()) {
            // Notification: acknowledged, nothing to say.
            writeResponse(guard, 202, "application/json", {});
            return;
        }
        writeResponse(guard, 200, "application/json",
                      QJsonDocument(resp).toJson(QJsonDocument::Compact));
    });
}

void McpHttpServer::writeResponse(QTcpSocket* sock, int status,
                                  const QByteArray& contentType,
                                  const QByteArray& body) {
    auto it = m_conns.find(sock);
    if (it != m_conns.end()) {
        if (it->replied) return;
        it->replied = true;
    }

    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' '
                    + statusText(status) + "\r\n";
    if (!body.isEmpty()) {
        head += "Content-Type: " + contentType + "\r\n";
    }
    head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    // One request per connection: keep-alive would mean re-arming the
    // parser for a second request on the same socket, and MCP clients
    // open a connection per call anyway.
    head += "Connection: close\r\n\r\n";

    sock->write(head);
    if (!body.isEmpty()) sock->write(body);
    sock->flush();
    sock->disconnectFromHost();
}

}  // namespace glasshouse
