#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include "config_portal.h"
#include "secrets.h"

static const char PORTAL_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="vi">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Aegis Sight</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  body{font-family:-apple-system,sans-serif;background:#f0f2f5;display:flex;min-height:100vh;align-items:center;justify-content:center}
  .card{background:#fff;border-radius:16px;padding:32px 24px;margin:20px;width:100%;max-width:400px;box-shadow:0 4px 24px rgba(0,0,0,.1)}
  h1{font-size:24px;color:#111;text-align:center;margin-bottom:4px}
  .sub{font-size:14px;color:#666;text-align:center;margin-bottom:24px}
  label{display:block;font-size:14px;font-weight:600;color:#333;margin-top:16px;margin-bottom:4px}
  input{width:100%;padding:10px 12px;border:1px solid #ccc;border-radius:8px;font-size:16px;outline:none;transition:border .2s}
  input:focus{border-color:#2563eb}
  .btn{width:100%;padding:12px;border:none;border-radius:8px;font-size:16px;font-weight:600;cursor:pointer;margin-top:20px;transition:opacity .2s}
  .btn-primary{background:#2563eb;color:#fff}
  .btn-primary:hover{opacity:.9}
  .btn-danger{background:#dc2626;color:#fff}
  .btn-danger:hover{opacity:.9}
  .or{text-align:center;margin:12px 0;color:#999;font-size:12px}
  .note{font-size:12px;color:#888;text-align:center;margin-top:16px}
  .icon{font-size:36px;text-align:center;margin-bottom:8px}
  .status{font-size:13px;color:#666;text-align:center;margin-top:12px;padding:8px;background:#f8f9fa;border-radius:8px}
</style>
</head>
<body>
<div class="card">
  <div class="icon">&#x1F9D0;</div>
  <h1>Aegis Sight</h1>
  <div class="sub">Nhập thông tin kết nối</div>
  <form id="cfg" action="/save" method="POST" onsubmit="return confirm('Lưu và khởi động lại?')">
    <label>Wi-Fi SSID</label>
    <input name="ssid" id="ssid" required placeholder="Tên Wi-Fi">
    <label>Mật khẩu Wi-Fi</label>
    <input name="pass" id="pass" type="password" placeholder="Để trống nếu mạng mở">
    <label>Gemini API Key</label>
    <input name="apikey" id="apikey" required placeholder="AIza..." pattern="AIza[a-zA-Z0-9_-]+">
    <button class="btn btn-primary" type="submit">Lưu &amp; Khởi động lại</button>
  </form>
  <div class="or">hoặc</div>
  <form action="/clear" method="POST" onsubmit="return confirm('Xoá toàn bộ cấu hình?')">
    <button class="btn btn-danger" type="submit">Xoá cấu hình</button>
  </form>
  <div class="note">Sau khi lưu, thiết bị sẽ tự khởi động lại và kết nối.</div>
</div>
<script>
  var u=new URL(window.location);
  var p=u.searchParams;
  if(p.get('ssid')) document.getElementById('ssid').value=p.get('ssid');
  if(p.get('apikey')) document.getElementById('apikey').value=p.get('apikey');
</script>
</body>
</html>
)rawliteral";

static WebServer server(80);
static DNSServer dns;
static bool saved = false;

static void handle_root(void) {
    String html = FPSTR(PORTAL_HTML);
    html.replace("</head>",
        "<script>"
        "var u=new URL(window.location);"
        "var p=u.searchParams;"
        "if(p.get('ssid')) document.getElementById('ssid').value=p.get('ssid');"
        "if(p.get('apikey')) document.getElementById('apikey').value=p.get('apikey');"
        "</script></head>");
    server.send(200, "text/html; charset=utf-8", html);
}

static void handle_save(void) {
    String ssid   = server.arg("ssid");
    String pass   = server.arg("pass");
    String apikey = server.arg("apikey");

    ssid.trim();
    apikey.trim();

    if (ssid.length() == 0 || apikey.length() == 0) {
        server.send(400, "text/plain; charset=utf-8", "Thiếu SSID hoặc API Key");
        return;
    }

    secrets_set(SK_WIFI_SSID, ssid);
    secrets_set(SK_WIFI_PASS, pass);
    secrets_set(SK_GEMINI_KEY, apikey);

    String page = "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='3'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<style>body{font-family:sans-serif;text-align:center;padding:40px}</style>"
                  "</head><body><h2>Đã lưu!</h2>"
                  "<p>Đang khởi động lại...</p></body></html>";
    server.send(200, "text/html; charset=utf-8", page);

    saved = true;
    delay(500);
    ESP.restart();
}

static void handle_clear(void) {
    secrets_clear();
    String page = "<html><head><meta charset='utf-8'><meta http-equiv='refresh' content='2'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "</head><body style='font-family:sans-serif;text-align:center;padding:40px'>"
                  "<h2>Đã xoá cấu hình</h2><p>Làm mới trang để nhập lại...</p></body></html>";
    server.send(200, "text/html; charset=utf-8", page);
}

static void handle_not_found(void) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString(), true);
    server.send(302, "text/plain", "");
}

void config_portal_start(void) {
    Serial.println("\n========================================");
    Serial.println("  CONFIG PORTAL — first-time setup");
    Serial.println("========================================");

    WiFi.mode(WIFI_AP);
    WiFi.softAPConfig(IPAddress(192, 168, 4, 1),
                      IPAddress(192, 168, 4, 1),
                      IPAddress(255, 255, 255, 0));
    WiFi.softAP(PORTAL_AP_SSID);

    IPAddress apIP = WiFi.softAPIP();
    Serial.printf("  AP SSID: %s\n", PORTAL_AP_SSID);
    Serial.printf("  AP IP:   %s\n", apIP.toString().c_str());

    dns.start(53, "*", apIP);

    server.on("/", handle_root);
    server.on("/save", HTTP_POST, handle_save);
    server.on("/clear", HTTP_POST, handle_clear);
    server.onNotFound(handle_not_found);

    server.begin();

    uint32_t start = millis();
    while (!saved && (millis() - start) < PORTAL_AP_TIMEOUT_MS) {
        dns.processNextRequest();
        server.handleClient();
        yield();
    }

    if (!saved) {
        Serial.println("[PORTAL] Timeout — rebooting...");
        ESP.restart();
    }
}
