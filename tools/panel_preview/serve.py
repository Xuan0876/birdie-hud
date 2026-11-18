"""Preview the real toolbar panel in a browser, with a fake Birdie (no Birdie.exe, no sim).

  python tools/panel_preview/serve.py [--port 8770]
  http://127.0.0.1:8770/birdie.html?en&page=2&still

Serves panel/birdie-panel/html_ui/InGamePanels/Birdie as it is on disk and slips fake.js + preview.js in front of
birdie.js, so every edit to the panel shows on reload. Query: en (English), page=0..3, still (no animations,
for screenshots), peek (the tray card), style=<name>, fact=<scenario in fake.js>, welcome (the first-run card),
ig=none|current|old|foreign|nofolder and ignote=installed|removed|failed (the in-sim panel row).
"""
import argparse
import functools
import http.server
from pathlib import Path

HERE = Path(__file__).resolve().parent
PAGE = HERE.parent.parent / "panel" / "birdie-panel" / "html_ui" / "InGamePanels" / "Birdie"
INJECT = '<script src="/_preview/fake.js"></script><script src="/_preview/preview.js"></script>'


class Handler(http.server.SimpleHTTPRequestHandler):
    def translate_path(self, path):
        p = path.split("?")[0].split("#")[0]
        if p.startswith("/_preview/"):
            return str(HERE / p[len("/_preview/"):])
        return super().translate_path(path)

    def do_GET(self):
        if self.path.split("?")[0] in ("/", "/birdie.html"):
            html = (PAGE / "birdie.html").read_text(encoding="utf-8")
            html = html.replace('<script type="text/javascript" src="birdie.js">', INJECT + '<script type="text/javascript" src="birdie.js">', 1)
            body = html.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, *a):
        pass


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8770)
    a = ap.parse_args()
    print(f"http://127.0.0.1:{a.port}/birdie.html")
    http.server.ThreadingHTTPServer(("127.0.0.1", a.port), functools.partial(Handler, directory=str(PAGE))).serve_forever()
