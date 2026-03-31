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

// 取得するニュース件数
const uint8_t  MAX_NEWS = 10;

// NTP 同期とスケジューラ設定
const IPAddress NTP_SERVER_IP(133, 243, 238, 243);  // ntp.nict.jp のAレコードの1つ
const uint16_t NTP_PORT = 123;
const uint16_t NTP_LOCAL_PORT = 2390;
const uint32_t JST_OFFSET_SEC = 9UL * 3600UL;
const uint32_t NTP_SYNC_INTERVAL = 6UL * 3600000UL;       // 6 時間ごとに再同期
const uint32_t SCHEDULE_CHECK_INTERVAL = 1000UL;          // 1 秒ごとに時刻判定

// ================================================================

EthernetClient client;
EthernetUDP    udp;
EthernetUDP    ntpUdp;

uint32_t       currentEpochUtc = 0;   // 最後に同期した UTC epoch
uint32_t       lastEpochSyncMs = 0;   // currentEpochUtc を記録した millis()
uint32_t       lastNtpSyncMs   = 0;
uint32_t       lastCheckMs     = 0;
bool           timeSynced      = false;
int32_t        lastTriggeredSlot = -1;

uint32_t       lastDigest = 0;
uint8_t        lastCount  = 0;
bool           hasLastDigest = false;

// Mega は SRAM 8KB あるので余裕を持ったバッファサイズにする
char titleBuf[256];
char newsBuf[MAX_NEWS][256];
char buildDateBuf[80];

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
  ntpUdp.begin(NTP_LOCAL_PORT);

  Serial.print(F("[NET] Local IP : "));
  Serial.println(Ethernet.localIP());
  Serial.print(F("[UDP] Target   : "));
  Serial.print(udpTarget);
  Serial.print(F(":"));
  Serial.println(UDP_TARGET_PORT);
  Serial.println(F("[SCH] Fetch at xx:15 and xx:45 (06:00-24:00 JST)"));
  Serial.println(F("[SYS] Ready."));

  syncTimeFromNTP();
}

void loop() {
  Ethernet.maintain();  // DHCP リース更新

  if (!timeSynced || millis() - lastNtpSyncMs >= NTP_SYNC_INTERVAL) {
    syncTimeFromNTP();
  }

  if (millis() - lastCheckMs >= SCHEDULE_CHECK_INTERVAL) {
    lastCheckMs = millis();
    runScheduledFetch();
  }
}

// ================================================================
//  HTTP 接続 → RSS 取得 → UDP 送信
// ================================================================

void fetchAndSend(bool forceSend) {
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

  // RSS XML をパースして、変更があれば UDP 送信
  parseRSS(forceSend);

  client.stop();
  Serial.println(F("[SYS] Fetch done."));
}

uint32_t nowJstEpoch() {
  if (!timeSynced) return 0;
  return currentEpochUtc + ((millis() - lastEpochSyncMs) / 1000UL) + JST_OFFSET_SEC;
}

void runScheduledFetch() {
  if (!timeSynced) return;

  uint32_t now = nowJstEpoch();
  uint32_t day = now / 86400UL;
  uint32_t secOfDay = now % 86400UL;

  uint8_t hour = secOfDay / 3600UL;
  uint8_t minute = (secOfDay % 3600UL) / 60UL;
  uint8_t second = secOfDay % 60UL;

  if (hour < 6 || hour > 23) return;

  bool scheduledRegular = (minute == 15 || minute == 45);
  bool scheduledForced  = ((hour == 7 || hour == 19) && minute == 0);
  if (!(scheduledRegular || scheduledForced)) return;
  if (second > 20) return;  // 同一スロットでの実行猶予

  int32_t slot = (int32_t)(day * 1440UL + (uint32_t)hour * 60UL + minute);
  if (slot == lastTriggeredSlot) return;

  lastTriggeredSlot = slot;
  fetchAndSend(scheduledForced);
}

bool syncTimeFromNTP() {
  uint8_t packet[48];
  memset(packet, 0, sizeof(packet));
  packet[0] = 0b11100011;
  packet[1] = 0;
  packet[2] = 6;
  packet[3] = 0xEC;
  packet[12] = 49;
  packet[13] = 0x4E;
  packet[14] = 49;
  packet[15] = 52;

  while (ntpUdp.parsePacket() > 0) {
    ntpUdp.read(packet, sizeof(packet));
  }

  ntpUdp.beginPacket(NTP_SERVER_IP, NTP_PORT);
  ntpUdp.write(packet, sizeof(packet));
  ntpUdp.endPacket();

  uint32_t start = millis();
  while (millis() - start < 2000UL) {
    int size = ntpUdp.parsePacket();
    if (size >= 48) {
      ntpUdp.read(packet, sizeof(packet));
      uint32_t secs1900 = ((uint32_t)packet[40] << 24) |
                          ((uint32_t)packet[41] << 16) |
                          ((uint32_t)packet[42] << 8)  |
                          (uint32_t)packet[43];
      const uint32_t SEVENTY_YEARS = 2208988800UL;
      currentEpochUtc = secs1900 - SEVENTY_YEARS;
      lastEpochSyncMs = millis();
      lastNtpSyncMs   = millis();
      timeSynced      = true;
      Serial.println(F("[NTP] Time synchronized."));
      return true;
    }
    delay(10);
  }

  Serial.println(F("[NTP] Timeout."));
  return false;
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

// HTTP ヘッダー終端（空行）まで読み飛ばす
// サーバー実装差異で CRLF / LF が混在しても本文開始を検出できるようにする。
void skipHttpHeader() {
  bool     sawAnyChar  = false;
  bool     lineHasChar = false;
  uint32_t t           = millis();

  while (millis() - t < 8000UL) {
    if (client.available()) {
      char c = (char)client.read();

      if (c == '\r') continue;  // CR は無視し、LF 基準で行終端判定

      if (c == '\n') {
        if (sawAnyChar && !lineHasChar) {
          // 直前行が空行 => ヘッダー終端
          return;
        }
        sawAnyChar  = true;
        lineHasChar = false;
      } else {
        lineHasChar = true;
      }
    } else if (!client.connected()) {
      return;
    } else {
      delay(1);
    }
  }

  Serial.println(F("[HTTP] Header skip timeout."));
}

// ================================================================
//  RSS XML パーサー（ストリーミング、バッファ不要）
// ================================================================

void parseRSS(bool forceSend) {
  bool     skipFirst = true;  // 最初の <title> はチャンネル名なので読み飛ばす
  uint8_t  count     = 0;
  uint32_t digest    = 2166136261UL;  // FNV-1a 32bit
  uint32_t deadline  = millis() + 60000UL;  // 最大 60 秒待つ

  buildDateBuf[0] = '\0';

  // channel の更新日時を先に取得（配信時にニュース本文より前に送る）
  if (findStr("<lastBuildDate>", deadline)) {
    int dlen = readUntilStr("</lastBuildDate>", buildDateBuf, sizeof(buildDateBuf), deadline);
    if (dlen > 0) {
      trimWhitespace(buildDateBuf);
      for (size_t i = 0; buildDateBuf[i] != '\0'; i++) {
        digest ^= (uint8_t)buildDateBuf[i];
        digest *= 16777619UL;
      }
      digest ^= 0x0A;
      digest *= 16777619UL;
    }
  }

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

    strncpy(newsBuf[count], titleBuf, sizeof(newsBuf[count]) - 1);
    newsBuf[count][sizeof(newsBuf[count]) - 1] = '\0';

    // タイトル列から簡易ダイジェストを作る
    for (size_t i = 0; newsBuf[count][i] != '\0'; i++) {
      digest ^= (uint8_t)newsBuf[count][i];
      digest *= 16777619UL;
    }
    digest ^= 0x0A;
    digest *= 16777619UL;

    count++;
  }

  if (count == 0) {
    Serial.println(F("[RSS] No items parsed."));
    Serial.println(F("[UDP] Sent 0 news items."));
    return;
  }

  if (!forceSend && hasLastDigest && count == lastCount && digest == lastDigest) {
    Serial.println(F("[RSS] No change. Skip UDP send."));
    return;
  }

  if (forceSend) {
    Serial.println(F("[RSS] Forced schedule. Send even without change."));
  }

  if (buildDateBuf[0] != '\0') {
    Serial.print(F("[TIME] "));
    Serial.println(buildDateBuf);
    sendMetaUDP("TIME", buildDateBuf);
    delay(50);
  }

  for (uint8_t i = 0; i < count; i++) {
    Serial.print(F("[NEWS] "));
    Serial.print(i + 1);
    Serial.print(F(": "));
    Serial.println(newsBuf[i]);

    sendUDP(i + 1, newsBuf[i]);
    delay(100);  // パケット間に少し間隔を開ける
  }

  lastDigest = digest;
  lastCount = count;
  hasLastDigest = true;

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
int readUntilStr(const char* end, char* buf, size_t bufSize, uint32_t deadline) {
  uint8_t elen     = (uint8_t)strlen(end);
  uint8_t matchPos = 0;
  size_t  bufPos   = 0;
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
  size_t contentLen = (size_t)(e - content);
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

// フォーマット: "<種別>|<内容>\n"  (UTF-8)
void sendMetaUDP(const char* kind, const char* value) {
  udp.beginPacket(udpTarget, UDP_TARGET_PORT);
  udp.write((const uint8_t*)kind, strlen(kind));
  udp.write((const uint8_t*)"|", 1);
  udp.write((const uint8_t*)value, strlen(value));
  udp.write((const uint8_t*)"\n", 1);
  udp.endPacket();
}
