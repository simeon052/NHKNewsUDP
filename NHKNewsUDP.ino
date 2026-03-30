/**
 * NHKNewsUDP.ino
 *
 * Arduino Mega + Ethernet Shield (W5100/W5500) 用
 * NHK NEWS WEB の RSS を HTTP で取得し、各ニュースタイトルを
 * UDP (UTF-8) で指定ホストに送信する。
 *
 * 依存ライブラリ:
 *   - Ethernet  (Arduino 標準添付)
 *   - EthernetUdp (同上)
 *   - SPI       (同上)
 *
 * ⚠ 注意: Ethernet Shield (W5100/W5500) は TLS 非対応。
 *   NHK RSS が HTTP → HTTPS リダイレクトを返す場合は
 *   同梱の proxy_server.py をローカル PC で起動し、
 *   RSS_HOST / RSS_PATH をプロキシに向けてください。
 *
 * UDP パケットフォーマット (UTF-8):
 *   "<番号>|<タイトル>\n"
 *   例) "1|政府が新方針を発表\n"
 */

#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>

// ================================================================
//  ★ 設定エリア ― 環境に合わせて変更してください ★
// ================================================================

// Arduino の MAC アドレス（他機器と被らなければ何でも可）
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };

// 固定 IP を使う場合は下行のコメントを外す（DHCP を使う場合はコメントのまま）
// IPAddress localIP(192, 168, 11, 200);

// UDP 送信先
IPAddress     udpTarget(192, 168, 11, 7);
const uint16_t UDP_TARGET_PORT = 50050;
const uint16_t UDP_LOCAL_PORT  = 8888;

// RSS フィード (HTTP のみ対応)
// HTTP でアクセスできない場合は proxy_server.py 経由に変更する
const char RSS_HOST[] = "www3.nhk.or.jp";
const char RSS_PATH[] = "/rss/news/cat0.xml";
const uint16_t RSS_PORT = 80;

// 取得するニュース件数と取得間隔
const uint8_t  MAX_NEWS = 10;
const uint32_t INTERVAL = 3600000UL;  // 1 時間 (ms)

// ================================================================

EthernetClient client;
EthernetUDP    udp;
uint32_t       lastFetch;

// Mega は SRAM 8KB あるので余裕を持ったバッファサイズにする
char titleBuf[256];

// ================================================================
//  setup / loop
// ================================================================

void setup() {
  Serial.begin(115200);
  while (!Serial);

  // Arduino Mega のハードウェア SS ピン (53) を OUTPUT にしないと
  // SPI が誤動作することがある
  pinMode(53, OUTPUT);

#ifdef localIP
  Ethernet.begin(mac, localIP);
  Serial.println(F("[NET] Static IP mode"));
#else
  Serial.println(F("[NET] Waiting for DHCP..."));
  if (Ethernet.begin(mac) == 0) {
    Serial.println(F("[NET] DHCP failed! Halted."));
    while (true) delay(1000);
  }
#endif

  delay(1000);
  udp.begin(UDP_LOCAL_PORT);

  Serial.print(F("[NET] Local IP : "));
  Serial.println(Ethernet.localIP());
  Serial.print(F("[UDP] Target   : "));
  Serial.print(udpTarget);
  Serial.print(F(":"));
  Serial.println(UDP_TARGET_PORT);
  Serial.println(F("[SYS] Ready."));

  // 起動直後にすぐ取得
  lastFetch = (uint32_t)(millis() - INTERVAL);
}

void loop() {
  Ethernet.maintain();  // DHCP リース更新

  if (millis() - lastFetch >= INTERVAL) {
    lastFetch = millis();
    fetchAndSend();
  }
}

// ================================================================
//  HTTP 接続 → RSS 取得 → UDP 送信
// ================================================================

void fetchAndSend() {
  Serial.println(F("--------------------"));
  Serial.print(F("[HTTP] Connecting to "));
  Serial.print(RSS_HOST);
  Serial.print(F(":"));
  Serial.println(RSS_PORT);

  if (!client.connect(RSS_HOST, RSS_PORT)) {
    Serial.println(F("[HTTP] Connection failed."));
    return;
  }

  // HTTP/1.0 を使うことでチャンク転送を回避する
  client.print(F("GET "));
  client.print(RSS_PATH);
  client.println(F(" HTTP/1.0"));
  client.print(F("Host: "));
  client.println(RSS_HOST);
  client.println(F("Accept-Encoding: identity"));
  client.println(F("Connection: close"));
  client.println();

  // レスポンス待ち（最大 15 秒）
  uint32_t t = millis();
  while (!client.available() && millis() - t < 15000UL) delay(10);

  if (!client.available()) {
    Serial.println(F("[HTTP] No response."));
    client.stop();
    return;
  }

  // ステータス行を表示（デバッグ用）
  printStatusLine();

  // HTTP ヘッダー末尾 (\r\n\r\n) を読み飛ばす
  skipHttpHeader();

  // RSS XML をパースして UDP 送信
  parseRSS();

  client.stop();
  Serial.println(F("[SYS] Fetch done."));
}

// ================================================================
//  HTTP ユーティリティ
// ================================================================

// 1 行目（ステータス行）を Serial に出力
void printStatusLine() {
  char c;
  Serial.print(F("[HTTP] "));
  uint32_t t = millis();
  while (millis() - t < 3000UL) {
    if (client.available()) {
      c = (char)client.read();
      if (c == '\n') { Serial.println(); return; }
      if (c != '\r') Serial.print(c);
    }
  }
}

// \r\n\r\n が来るまで読み飛ばす
void skipHttpHeader() {
  uint8_t  state = 0;
  uint32_t t     = millis();
  while (millis() - t < 8000UL) {
    if (client.available()) {
      char c = (char)client.read();
      if      (state == 0 && c == '\r') state = 1;
      else if (state == 1 && c == '\n') state = 2;
      else if (state == 2 && c == '\r') state = 3;
      else if (state == 3 && c == '\n') return;
      else state = (c == '\r') ? 1 : 0;
    } else if (!client.connected()) return;
    else delay(1);
  }
}

// ================================================================
//  RSS XML パーサー（ストリーミング、バッファ不要）
// ================================================================

void parseRSS() {
  bool    skipFirst = true;  // 最初の <title> はチャンネル名なので読み飛ばす
  uint8_t count     = 0;
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
    delay(100);  // パケット間に少し間隔を開ける
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
  uint8_t slen     = (uint8_t)strlen(str);
  uint8_t matchPos = 0;

  while (millis() < deadline) {
    if (client.available()) {
      char c = (char)client.read();
      if (c == str[matchPos]) {
        if (++matchPos == slen) return true;
      } else {
        matchPos = (c == str[0]) ? 1 : 0;
      }
    } else if (!client.connected() && !client.available()) {
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
  uint8_t elen     = (uint8_t)strlen(end);
  uint8_t matchPos = 0;
  uint8_t bufPos   = 0;
  char    partial[16];  // </title> は 8 文字なので余裕あり

  while (millis() < deadline) {
    if (client.available()) {
      char c = (char)client.read();

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
          matchPos   = 1;
        } else {
          if (bufPos < bufSize - 1) buf[bufPos++] = c;
        }
      }
    } else if (!client.connected() && !client.available()) {
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
  const char* TAG_END   = "]]>";

  char* s = strstr(buf, TAG_START);
  if (!s) return;
  char* e = strstr(s, TAG_END);
  if (!e) return;

  char*   content    = s + 9;  // strlen("<![CDATA[") == 9
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
  udp.write((const uint8_t*)title,  strlen(title));
  udp.write((const uint8_t*)"\n",   1);
  udp.endPacket();
}
