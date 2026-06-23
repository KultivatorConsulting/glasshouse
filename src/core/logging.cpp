#include "logging.h"

// Default every category to Info: debug is off unless explicitly enabled,
// e.g. QT_LOGGING_RULES='glasshouse.*.debug=true' (or per-category). Qt
// enables category debug by default, which otherwise floods syslog/journald
// with per-frame WS RX dumps and the per-second video telemetry. Env
// QT_LOGGING_RULES still overrides this default for troubleshooting.
Q_LOGGING_CATEGORY(lcPikvm,  "glasshouse.pikvm",  QtInfoMsg)
Q_LOGGING_CATEGORY(lcConfig, "glasshouse.config", QtInfoMsg)
Q_LOGGING_CATEGORY(lcHid,    "glasshouse.hid",    QtInfoMsg)
Q_LOGGING_CATEGORY(lcWs,     "glasshouse.ws",     QtInfoMsg)
Q_LOGGING_CATEGORY(lcVideo,  "glasshouse.video",  QtInfoMsg)
Q_LOGGING_CATEGORY(lcJanus,  "glasshouse.janus",  QtInfoMsg)
