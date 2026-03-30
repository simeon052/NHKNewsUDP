"""
proxy_server.py
---------------
Arduino (W5100/W5500) は TLS 非対応のため、NHK RSS を HTTPS で取得できない場合に使う
シンプルな HTTP → HTTPS フォワードプロキシ。

【使い方】
  1. Python 3 と requests ライブラリが必要
       pip install requests
  2. このスクリプトを PC (192.168.11.x) で起動する
       python proxy_server.py
  3. NHKNewsUDP.ino の設定エリアを変更する
       RSS_HOST = "192.168.11.xxx"  ← このPCのIPアドレス
       RSS_PATH = "/nhk_rss"
       RSS_PORT = 8080

サーバーはポート 8080 で待ち受ける。
"""

from http.server import BaseHTTPRequestHandler, HTTPServer
import requests

LISTEN_HOST = "0.0.0.0"
LISTEN_PORT = 8080

NHK_RSS_URL = "https://www3.nhk.or.jp/rss/news/cat0.xml"


class ProxyHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path != "/nhk_rss":
            self.send_response(404)
            self.end_headers()
            return

        try:
            r = requests.get(NHK_RSS_URL, timeout=10,
                             headers={"Accept-Encoding": "identity"})
            body = r.content  # UTF-8 バイト列のまま転送

            self.send_response(200)
            self.send_header("Content-Type", "application/rss+xml; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            print(f"[OK] Served NHK RSS ({len(body)} bytes)")

        except Exception as e:
            self.send_response(502)
            self.end_headers()
            print(f"[ERR] {e}")

    def log_message(self, format, *args):
        pass  # アクセスログを抑制


if __name__ == "__main__":
    print(f"Proxy listening on {LISTEN_HOST}:{LISTEN_PORT}")
    print(f"  GET http://<this PC IP>:{LISTEN_PORT}/nhk_rss  →  {NHK_RSS_URL}")
    HTTPServer((LISTEN_HOST, LISTEN_PORT), ProxyHandler).serve_forever()
