// Exercises the MCP server's HTTP transport and JSON-RPC envelope against a
// real socket — this is a hand-written HTTP front end (Qt has no HTTP
// server), so the framing and the error paths are exactly the parts that
// have to be pinned down.
//
// The session handed to the server here has no windows and no PiKVM
// clients, which is deliberate: it covers the protocol surface and the
// "nothing is connected" replies without needing hardware or a display.
// Anything that actually moves the pointer or types needs the live GUI and
// is checked there.

#include "mcpserver.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>
#include <QTest>

using namespace glasshouse;

namespace {

constexpr quint16 kPort = 18765;   // high, unlikely to collide with anything

struct Reply {
    int        status = 0;
    QByteArray body;
    bool       ok = false;
};

// Minimal HTTP client: one request, read until the server closes. The
// server answers `Connection: close`, so EOF is the end of the body.
//
// Event-loop driven rather than using waitForReadyRead(): the server under
// test lives in this same thread, and the blocking waitFor* calls only
// watch their own descriptor — they never run the event loop, so the
// server's newConnection would never fire and every request would time out.
Reply post(const QByteArray& body, const QByteArray& extraHeaders = {},
           const QByteArray& method = "POST") {
    Reply r;
    QTcpSocket sock;
    QEventLoop loop;
    QByteArray all;

    const QByteArray req = method + " /mcp HTTP/1.1\r\n"
                           "Host: 127.0.0.1\r\n"
                           "Content-Type: application/json\r\n"
                         + extraHeaders
                         + "Content-Length: " + QByteArray::number(body.size())
                         + "\r\n\r\n" + body;

    QObject::connect(&sock, &QTcpSocket::connected, &sock,
                     [&sock, &req]() { sock.write(req); });
    QObject::connect(&sock, &QTcpSocket::readyRead, &sock,
                     [&sock, &all]() { all.append(sock.readAll()); });
    QObject::connect(&sock, &QTcpSocket::disconnected, &loop,
                     &QEventLoop::quit);
    QObject::connect(&sock, &QTcpSocket::errorOccurred, &loop,
                     [&loop]() { loop.quit(); });

    QTimer guard;
    guard.setSingleShot(true);
    QObject::connect(&guard, &QTimer::timeout, &loop, &QEventLoop::quit);
    guard.start(5000);

    sock.connectToHost(QHostAddress::LocalHost, kPort);
    loop.exec();
    all.append(sock.readAll());
    if (all.isEmpty()) return r;

    const int blank = all.indexOf("\r\n\r\n");
    if (blank < 0) return r;
    const QByteArray head = all.left(blank);
    const auto parts = head.split(' ');
    if (parts.size() >= 2) r.status = parts[1].toInt();
    r.body = all.mid(blank + 4);
    r.ok   = true;
    return r;
}

QByteArray rpc(const char* method, const QJsonObject& params = {},
               int id = 1) {
    QJsonObject o{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}};
    if (!params.isEmpty()) o.insert(QStringLiteral("params"), params);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

QJsonObject bodyObject(const Reply& r) {
    return QJsonDocument::fromJson(r.body).object();
}

}  // namespace

class TestMcpServer : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    void initializeEchoesSupportedProtocol();
    void initializeFallsBackOnUnknownProtocol();
    void toolsListAdvertisesTypingAndMouse();
    void sessionToolReportsScreenGeometry();
    void notificationGetsAcceptedWithNoBody();
    void malformedJsonGetsParseError();
    void unknownMethodGetsMethodNotFound();
    void unknownToolIsReportedAsToolError();
    void inputToolsRefuseWithoutHidMaster();
    void getIsRejectedBecauseThereIsNoStream();
    void tokenIsEnforcedWhenConfigured();

private:
    McpServer* m_srv = nullptr;
};

void TestMcpServer::initTestCase() {
    m_srv = new McpServer(this);

    // Two 1920x1080 heads side by side — the §10.1 layout the coordinate
    // tests use, so geometry reported here lines up with those goldens.
    McpSessionView view;
    view.logicalDesktop = QSize(3840, 1080);
    McpScreen a;
    a.index = 0; a.monitorId = 1; a.host = QStringLiteral("10.0.0.1");
    a.monitorRect = QRect(0, 0, 1920, 1080); a.hidMaster = true;
    McpScreen b;
    b.index = 1; b.monitorId = 2; b.host = QStringLiteral("10.0.0.2");
    b.monitorRect = QRect(1920, 0, 1920, 1080);
    view.screens = {a, b};
    view.hidHost = QStringLiteral("10.0.0.1");
    view.hid     = nullptr;          // nothing connected, on purpose
    m_srv->setSession(view);

    QString err;
    QVERIFY2(m_srv->start(QStringLiteral("127.0.0.1"), kPort, QString(), &err),
             qPrintable(err));
}

void TestMcpServer::cleanupTestCase() {
    if (m_srv) m_srv->stop();
}

void TestMcpServer::initializeEchoesSupportedProtocol() {
    const Reply r = post(rpc("initialize", QJsonObject{
        {"protocolVersion", "2025-06-18"}}));
    QVERIFY(r.ok);
    QCOMPARE(r.status, 200);
    const QJsonObject result = bodyObject(r).value("result").toObject();
    QCOMPARE(result.value("protocolVersion").toString(),
             QStringLiteral("2025-06-18"));
    // Tools only: no resources and no subscription capability, because the
    // design has the client polling rather than the server pushing.
    const QJsonObject caps = result.value("capabilities").toObject();
    QVERIFY(caps.contains("tools"));
    QVERIFY(!caps.contains("resources"));
    QCOMPARE(result.value("serverInfo").toObject().value("name").toString(),
             QStringLiteral("glasshouse"));
}

void TestMcpServer::initializeFallsBackOnUnknownProtocol() {
    const Reply r = post(rpc("initialize", QJsonObject{
        {"protocolVersion", "1999-01-01"}}));
    QVERIFY(r.ok);
    const QString got = bodyObject(r).value("result").toObject()
                            .value("protocolVersion").toString();
    QVERIFY2(!got.isEmpty(), "server must still name a version it speaks");
    QVERIFY(got != QStringLiteral("1999-01-01"));
}

void TestMcpServer::toolsListAdvertisesTypingAndMouse() {
    const Reply r = post(rpc("tools/list"));
    QVERIFY(r.ok);
    QCOMPARE(r.status, 200);
    const QJsonArray tools =
        bodyObject(r).value("result").toObject().value("tools").toArray();
    QStringList names;
    for (const auto& t : tools) names << t.toObject().value("name").toString();

    // The two the user asked for by name, plus the session tool the whole
    // thing exists for.
    QVERIFY2(names.contains("glasshouse_type"),  qPrintable(names.join(',')));
    QVERIFY2(names.contains("glasshouse_keys"),  qPrintable(names.join(',')));
    QVERIFY2(names.contains("glasshouse_mouse_move"),  qPrintable(names.join(',')));
    QVERIFY2(names.contains("glasshouse_mouse_click"), qPrintable(names.join(',')));
    QVERIFY2(names.contains("glasshouse_session"),     qPrintable(names.join(',')));

    // Every tool needs a schema, or a client cannot call it.
    for (const auto& t : tools) {
        const QJsonObject o = t.toObject();
        QVERIFY2(o.value("inputSchema").toObject().value("type").toString()
                     == QStringLiteral("object"),
                 qPrintable(o.value("name").toString()));
        QVERIFY2(!o.value("description").toString().isEmpty(),
                 qPrintable(o.value("name").toString()));
    }
}

void TestMcpServer::sessionToolReportsScreenGeometry() {
    const Reply r = post(rpc("tools/call", QJsonObject{
        {"name", "glasshouse_session"}, {"arguments", QJsonObject{}}}));
    QVERIFY(r.ok);
    const QJsonObject result = bodyObject(r).value("result").toObject();
    QVERIFY(!result.value("isError").toBool());
    const QString text = result.value("content").toArray().at(0).toObject()
                             .value("text").toString();

    // The payload is JSON inside the text block, so parse it rather than
    // substring-matching, and assert the thing a per-PiKVM server cannot
    // express: which head sits where on the combined desktop.
    const QJsonObject payload = QJsonDocument::fromJson(text.toUtf8()).object();
    QCOMPARE(payload.value("logical_desktop").toObject().value("w").toInt(), 3840);
    const QJsonArray screens = payload.value("screens").toArray();
    QCOMPARE(screens.size(), 2);
    QCOMPARE(screens.at(0).toObject().value("desktop_rect").toObject()
                 .value("x").toInt(), 0);
    QCOMPARE(screens.at(1).toObject().value("desktop_rect").toObject()
                 .value("x").toInt(), 1920);
    QVERIFY(screens.at(0).toObject().value("hid_master").toBool());
    QVERIFY(!screens.at(1).toObject().value("hid_master").toBool());
}

void TestMcpServer::notificationGetsAcceptedWithNoBody() {
    // `notifications/initialized` has no id. Answering it with a JSON-RPC
    // response is a protocol violation, so the transport must send 202 and
    // an empty body.
    const QByteArray body = QJsonDocument(QJsonObject{
        {"jsonrpc", "2.0"}, {"method", "notifications/initialized"}})
            .toJson(QJsonDocument::Compact);
    const Reply r = post(body);
    QVERIFY(r.ok);
    QCOMPARE(r.status, 202);
    QVERIFY(r.body.isEmpty());
}

void TestMcpServer::malformedJsonGetsParseError() {
    const Reply r = post("{ this is not json ");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 400);
    QCOMPARE(bodyObject(r).value("error").toObject().value("code").toInt(),
             -32700);
}

void TestMcpServer::unknownMethodGetsMethodNotFound() {
    const Reply r = post(rpc("nonsense/method"));
    QVERIFY(r.ok);
    QCOMPARE(bodyObject(r).value("error").toObject().value("code").toInt(),
             -32601);
}

void TestMcpServer::unknownToolIsReportedAsToolError() {
    // A bad tool name is the model's mistake, not a transport fault, so it
    // comes back as tool content with isError — something the model can
    // read and correct — rather than a JSON-RPC error.
    const Reply r = post(rpc("tools/call", QJsonObject{
        {"name", "glasshouse_nope"}, {"arguments", QJsonObject{}}}));
    QVERIFY(r.ok);
    QCOMPARE(r.status, 200);
    const QJsonObject result = bodyObject(r).value("result").toObject();
    QVERIFY(result.value("isError").toBool());
    QVERIFY(result.value("content").toArray().at(0).toObject()
                .value("text").toString().contains("unknown tool"));
}

void TestMcpServer::inputToolsRefuseWithoutHidMaster() {
    // No HID master in the test session. Every input tool must say so
    // plainly instead of pretending the keystroke went somewhere.
    const char* tools[] = {"glasshouse_type", "glasshouse_keys",
                           "glasshouse_mouse_move", "glasshouse_mouse_click",
                           "glasshouse_mouse_scroll"};
    for (const char* name : tools) {
        const Reply r = post(rpc("tools/call", QJsonObject{
            {"name", name},
            {"arguments", QJsonObject{{"text", "hello"},
                                      {"keys", QJsonArray{"Enter"}},
                                      {"x", 10}, {"y", 10}, {"screen", 0},
                                      {"dy", 3}}}}));
        QVERIFY2(r.ok, name);
        const QJsonObject result = bodyObject(r).value("result").toObject();
        QVERIFY2(result.value("isError").toBool(), name);
        const QString text = result.value("content").toArray().at(0)
                                 .toObject().value("text").toString();
        QVERIFY2(text.contains(QStringLiteral("HID")), qPrintable(name + (": " + text)));
    }
}

void TestMcpServer::getIsRejectedBecauseThereIsNoStream() {
    // An MCP client may try GET to open an SSE stream. 405 is the honest
    // answer; leaving it hanging would make a client wait for push that
    // never comes.
    const Reply r = post({}, {}, "GET");
    QVERIFY(r.ok);
    QCOMPARE(r.status, 405);
}

void TestMcpServer::tokenIsEnforcedWhenConfigured() {
    m_srv->stop();
    QString err;
    QVERIFY2(m_srv->start(QStringLiteral("127.0.0.1"), kPort,
                          QStringLiteral("s3cret"), &err), qPrintable(err));

    const Reply noAuth = post(rpc("tools/list"));
    QVERIFY(noAuth.ok);
    QCOMPARE(noAuth.status, 401);

    const Reply wrong = post(rpc("tools/list"),
                             "Authorization: Bearer wrong\r\n");
    QVERIFY(wrong.ok);
    QCOMPARE(wrong.status, 401);

    const Reply right = post(rpc("tools/list"),
                             "Authorization: Bearer s3cret\r\n");
    QVERIFY(right.ok);
    QCOMPARE(right.status, 200);
}

QTEST_GUILESS_MAIN(TestMcpServer)
#include "test_mcpserver.moc"
