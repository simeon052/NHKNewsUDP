/**
 * NHKNewsUDP_UnoR4WiFi.ino
 *
 * Arduino Uno R4 WiFi 用
 * NHK NEWS WEB の RSS を HTTPS で直接取得し、各ニュースタイトルを
 * UDP (UTF-8) で指定ホストに送信する。
 *
 * 依存ライブラリ:
 *   - WiFiS3 (Arduino 標準添付)
 *   - ArduinoHttpClient (Sketch → ライブラリをインクルード → ArduinoHttpClient)
 *
 * ✅ 利点: Ethernet Shield と異なり、HTTPS が直接利用できるため
 *   プロキシサーバーが不要！
 *
 * UDP パケットフォーマット (UTF-8):
 *   "<番号>|<タイトル>\n"
 *   例) "1|政府が新方針を発表\n"
 */
#include <WiFiS3.h>
#include <ArduinoHttpClient.h>
#include <WiFiUdp.h>

// ================================================================
//  ★ 設定エリア ― 環境に合わせて変更してください ★
// ================================================================

// WiFi 設定
const char* SSID = "YOUR_SSID";
const char* PASSWORD = "YOUR_PASSWORD";

// UDP 送信先
IPAddress udpTarget(192, 168, 11, 7);
const uint16_t UDP_TARGET_PORT = 50050;
const uint16_t UDP_LOCAL_PORT = 8888;

// RSS フィード (HTTPS で直接取得)
const char RSS_HOST[] = "www3.nhk.or.jp";
const char RSS_PATH[] = "/rss/news/cat0.xml";
const uint16_t RSS_PORT = 443;

// 取得するニュース件数と取得間隔
const uint8_t MAX_NEWS = 10;
const uint32_t INTERVAL = 3600000UL;  // 1 時間 (ms)

// ================================================================
WiFiSSLClient wifiClient;
HttpClient httpClient(wifiClient, RSS_HOST, RSS_PORT);
WiFiUDP udp;
uint32_t lastFetch;

// Uno R4 は SRAM が充分にあるのでバッファを大きめに
char titleBuf[512];

// ================================================================
//  setup / loop
// ================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);

  Serial.println(F("\n[SYS] NHK News UDP Sender (Uno R4 WiFi)"));

  // WiFi 接続
  connectWiFi();

  udp.begin(UDP_LOCAL_PORT);

  Serial.print(F("[UDP] Target   : "));
  Serial.print(udpTarget);
  Serial.print(F(":"));
  Serial.println(UDP_TARGET_PORT);
  Serial.println(F("[SYS] Ready."));

  // 起動直後にすぐ取得
  lastFetch = (uint32_t)(millis() - INTERVAL);
}

void loop() {
  // WiFi 再接続チェック
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[NET] WiFi disconnected. Reconnecting..."));
    connectWiFi();
  }

  if (millis() - lastFetch >= INTERVAL) {
    lastFetch = millis();
    fetchAndSend();
  }

  delay(1000);
}

// ================================================================
//  WiFi 接続
// ================================================================

void connectWiFi() {
  Serial.println(F("[NET] Connecting to WiFi..."));
  WiFi.begin(SSID, PASSWORD);

  uint32_t timeout = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - timeout < 20000UL) {
    delay(500);
    Serial.print(F("."));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print(F("[NET] Connected! IP: "));
    Serial.println(WiFi.localIP());
  } else {
    Serial.println(F("[NET] WiFi connection failed!"));
  }
}

// ================================================================
//  HTTPS 接続 → RSS 取得 → UDP 送信
// ================================================================

void fetchAndSend() {
  Serial.println(F("--------------------"));
  Serial.print(F("[HTTPS] Connecting to "));
  Serial.print(RSS_HOST);
  Serial.print(F(":"));
  Serial.println(RSS_PORT);

  if (!wifiClient.connect(RSS_HOST, RSS_PORT)) {
    Serial.println(F("[HTTPS] Connection failed."));
    return;
  }

  // HTTP GET リクエスト送信
  httpClient.get(RSS_PATH);

  // ステータスコード確認
  int statusCode = httpClient.responseStatusCode();
  Serial.print(F("[HTTPS] Status: "));
  Serial.println(statusCode);

  if (statusCode != 200) {
    Serial.println(F("[HTTPS] Unexpected status code."));
    httpClient.stop();
    return;
  }

  // HTTP ヘッダーをスキップ
  httpClient.skipResponseHeaders();

  // RSS XML をパースして UDP 送信
  parseRSS();

  httpClient.stop();
  Serial.println(F("[SYS] Fetch done."));
}

// ================================================================
//  RSS XML パーサー（ストリーミング）
// ================================================================

void parseRSS() {
  bool skipFirst = true;  // 最初の <title> はチャンネル名なので読み飛ばす
  uint8_t count = 0;
  uint32_t deadline = millis() + 60000UL;  // 最大 60 秒待つ

  while (count < MAX_NEWS && millis() < deadline) {
    if (!findStr("<title>", deadline)) break;

    int len = readUntilStr("</title>", titleBuf, sizeof(titleBuf), deadline);
    if (len < 0) break;

    if (skipFirst) {
      skipFirst = false;
      continue;
    }

    stripCDATA(titleBuf);
    trimWhitespace(titleBuf);

    if (titleBuf[0] == '\0') continue;  // 空タイトルはスキップ

    Serial.print(F("[NEWS] "));
    Serial.print(count + 1);
    Serial.print(F(": "));
    Serial.println(titleBuf);

    sendUDP(count + 1, titleBuf);
    count++;
    delay(100);
  }

  Serial.print(F("[UDP] Sent "));
  Serial.print(count);
  Serial.println(F(" news items."));
}

// ================================================================
//  文字列検索ユーティリティ
// ================================================================

// str が見つかるまでストリームを読み進める
bool findStr(const char* str, uint32_t deadline) {
  uint8_t slen = (uint8_t)strlen(str);
  uint8_t matchPos = 0;

  while (millis() < deadline) {
    if (wifiClient.available()) {
      char c = (char)wifiClient.read();
      if (c == str[matchPos]) {
        if (++matchPos == slen) return true;
      } else {
        matchPos = (c == str[0]) ? 1 : 0;
      }
    } else if (!wifiClient.connected()) {
      return false;
    } else {
      delay(1);
    }
  }
  return false;
}

// end 文字列が来るまでの内容を buf に格納する
// 戻り値: 格納した文字数（タイムアウト/切断時は -1）
int readUntilStr(const char* end, char* buf, uint8_t bufSize, uint32_t deadline) {
  uint8_t elen = (uint8_t)strlen(end);
  uint8_t matchPos = 0;
  uint8_t bufPos = 0;
  char partial[16];  // </title> は 8 文字なので余裕あり

  while (millis() < deadline) {
    if (wifiClient.available()) {
      char c = (char)wifiClient.read();

      if (c == end[matchPos]) {
        partial[matchPos] = c;
        if (++matchPos == elen) {
          buf[bufPos] = '\0';
          return (int)bufPos;
        }
      } else {
        // 部分マッチ分をバッファに書き出す
        for (uint8_t i = 0; i < matchPos; i++) {
          if (bufPos < bufSize - 1) buf[bufPos++] = partial[i];
        }
        matchPos = 0;
        if (c == end[0]) {
          partial[0] = c;
          matchPos = 1;
        } else {
          if (bufPos < bufSize - 1) buf[bufPos++] = c;
        }
      }
    } else if (!wifiClient.connected()) {
      buf[bufPos] = '\0';
      return -1;
    } else {
      delay(1);
    }
  }

  buf[bufPos] = '\0';
  return -1;
}

// ================================================================
//  文字列加工ユーティリティ
// ================================================================

// <![CDATA[...]]> を剥がす
void stripCDATA(char* buf) {
  const char* TAG_START = "<![CDATA[";
  const char* TAG_END = "]]>";

  char* s = strstr(buf, TAG_START);
  if (!s) return;
  char* e = strstr(s, TAG_END);
  if (!e) return;

  char* content = s + 9;  // strlen("<![CDATA[") == 9
  uint8_t contentLen = (uint8_t)(e - content);
  memmove(buf, content, contentLen);
  buf[contentLen] = '\0';
}

// 先頭・末尾の空白・改行をトリムする
void trimWhitespace(char* buf) {
  // 末尾トリム
  int len = strlen(buf);
  while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t' ||
                     buf[len - 1] == '\r' || buf[len - 1] == '\n')) {
    buf[--len] = '\0';
  }
  // 先頭トリム
  char* p = buf;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (p != buf) memmove(buf, p, strlen(p) + 1);
}

// ================================================================
//  UDP 送信
// ================================================================

// フォーマット: "<番号>|<タイトル>\n"  (UTF-8)
void sendUDP(uint8_t index, const char* title) {
  char header[5];
  snprintf(header, sizeof(header), "%u|", index);

  udp.beginPacket(udpTarget, UDP_TARGET_PORT);
  udp.write((const uint8_t*)header, strlen(header));
  udp.write((const uint8_t*)title, strlen(title));
  udp.write((const uint8_t*)"\n", 1);
  udp.endPacket();
}