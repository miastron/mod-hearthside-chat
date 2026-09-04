#ifndef MOD_HS_HTTP_SERVER_H
#define MOD_HS_HTTP_SERVER_H

// The authenticated HTTP control API, lifted structurally from
// mod-playerbots-characters' own server (bind-then-thread lifecycle,
// route-per-lambda registration) but with this module's simpler
// static-bearer-token auth (hs_http_auth.h) in place of PBC's per-account
// OTP/session system, and no WebSocket or frontend-serving surface. Read
// routes need only a valid token; mutating routes additionally require
// g_HsHttpControlEnable.
//
// Disabled by default (HearthsideChat.HttpServerPort = 0). A bind failure
// (port in use, bad address) or a missing HttpServerPrivateKey logs and
// leaves the server off. The rest of the module continues normally either
// way.

void Hs_HttpServerStart();
void Hs_HttpServerStop();
// Review C18: Hs_HttpServerIsRunning() was declared and defined here and
// had no caller anywhere in the module -- removed rather than left as an
// export nothing exercises. The state it exposed is still tracked
// internally (s_running in the .cpp), which is what Hs_HttpServerStop
// needs; an operator reads the server's state from the startup log line,
// which reports the bind result directly.

#endif // MOD_HS_HTTP_SERVER_H
