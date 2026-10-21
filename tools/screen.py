#!/usr/bin/env python3
"""Trajectories — веб-отображение экрана устройства по USB.

Читает кадры с USB-CDC порта и отдаёт их в браузер. Из зависимостей
только pyserial: страница обновляется через Server-Sent Events, которые
умеет стандартная библиотека, поэтому ни Flask, ни websockets не нужны.

    pip install pyserial
    python3 screen.py --port /dev/cu.usbmodem1101
    открыть http://localhost:8080

Порт можно не указывать — скрипт попробует найти устройство сам.
"""

# Аннотации откладываются: без этого "str | None" ломает Python 3.9,
# который стоит в macOS по умолчанию.
from __future__ import annotations

import argparse
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial
import serial.tools.list_ports

W, H = 128, 32
FRAME_HEX = W * H // 8 * 2          # 512 байт кадра -> 1024 hex-символа

# Последний принятый кадр. Счётчик нужен, чтобы SSE отдавал клиенту
# только новое, а не гонял одну и ту же картинку.
state = {"hex": None, "seq": 0, "connected": False, "port": ""}
lock = threading.Lock()


def find_port() -> str | None:
    """Ищет ESP32-C3: у него USB JTAG/serial от Espressif, VID 0x303a."""
    for p in serial.tools.list_ports.comports():
        if p.vid == 0x303A:
            return p.device
    return None


def reader(port: str, baud: int) -> None:
    """Держит порт открытым, шлёт keepalive и складывает кадры в state."""
    while True:
        try:
            with serial.Serial(port, baud, timeout=1) as s:
                with lock:
                    state["connected"] = True
                    state["port"] = port
                print(f"порт открыт: {port}")
                last_ping = 0.0
                while True:
                    now = time.monotonic()
                    # Прошивка транслирует, пока слышит нас, и сама
                    # замолкает через 2 с тишины.
                    if now - last_ping > 0.5:
                        s.write(b"S")
                        last_ping = now
                    line = s.readline()
                    if len(line) >= FRAME_HEX + 1 and line[0:1] == b"F":
                        with lock:
                            state["hex"] = line[1:FRAME_HEX + 1].decode("ascii", "ignore")
                            state["seq"] += 1
        except (serial.SerialException, OSError) as e:
            with lock:
                state["connected"] = False
            print(f"нет связи ({e}); повтор через 2 с")
            time.sleep(2)


PAGE = """<!doctype html>
<meta charset="utf-8">
<title>Trajectories</title>
<style>
  :root { color-scheme: dark; }
  body { margin:0; min-height:100vh; display:flex; flex-direction:column;
         align-items:center; justify-content:center; gap:18px;
         background:#0b0d10; color:#8b949e;
         font:13px/1.5 ui-monospace, SFMono-Regular, Menlo, monospace; }
  #wrap { padding:20px; background:#000; border:1px solid #21262d; border-radius:10px; }
  /* Экран увеличен целыми пикселями — сглаживание убило бы растр. */
  canvas { image-rendering:pixelated; display:block; width:1024px; height:256px; }
  .row { display:flex; gap:10px; align-items:center; }
  button { font:inherit; color:#c9d1d9; background:#161b22; cursor:pointer;
           border:1px solid #30363d; border-radius:6px; padding:6px 14px; }
  button:hover { border-color:#8b949e; }
  #st::before { content:"● "; color:#f85149; }
  #st.on::before { color:#3fb950; }
</style>
<div id="wrap"><canvas id="c" width="128" height="32"></canvas></div>
<div class="row">
  <span id="st">нет связи</span>
  <button id="save">Сохранить PNG</button>
  <span id="fps"></span>
</div>
<script>
const cv = document.getElementById('c'), cx = cv.getContext('2d');
const img = cx.createImageData(128, 32);
let frames = 0, shown = 0;

function draw(hex) {
  const b = new Uint8Array(512);
  for (let i = 0; i < 512; i++) b[i] = parseInt(hex.substr(i * 2, 2), 16);
  const d = img.data;
  for (let y = 0; y < 32; y++) {
    for (let x = 0; x < 128; x++) {
      // SSD1306 адресуется страницами: байт — это 8 вертикальных
      // пикселей, а не 8 горизонтальных подряд.
      const on = (b[x + (y >> 3) * 128] >> (y & 7)) & 1;
      const o = (y * 128 + x) * 4;
      d[o] = on ? 0xe6 : 0x08; d[o+1] = on ? 0xed : 0x0c;
      d[o+2] = on ? 0xf3 : 0x10; d[o+3] = 255;
    }
  }
  cx.putImageData(img, 0, 0);
  frames++;
}

const es = new EventSource('/stream');
es.onmessage = e => {
  document.getElementById('st').textContent = 'подключено';
  document.getElementById('st').className = 'on';
  draw(e.data);
};
es.onerror = () => {
  document.getElementById('st').textContent = 'нет связи';
  document.getElementById('st').className = '';
};
setInterval(() => {
  document.getElementById('fps').textContent = (frames - shown) + ' fps';
  shown = frames;
}, 1000);

document.getElementById('save').onclick = () => {
  // Экран увеличиваем целыми пикселями, чтобы снимок был чётким.
  const s = 8, o = document.createElement('canvas');
  o.width = 128 * s; o.height = 32 * s;
  const ox = o.getContext('2d');
  ox.imageSmoothingEnabled = false;
  ox.drawImage(cv, 0, 0, o.width, o.height);
  o.toBlob(bl => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(bl);
    a.download = 'trajectories-' + Date.now() + '.png';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
};
</script>
"""


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):        # без построчного лога каждого кадра
        pass

    def do_GET(self):
        if self.path == "/":
            body = PAGE.encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return

        if self.path == "/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.end_headers()
            last = -1
            try:
                while True:
                    with lock:
                        h, seq = state["hex"], state["seq"]
                    if h and seq != last:
                        last = seq
                        self.wfile.write(f"data: {h}\n\n".encode())
                        self.wfile.flush()
                    time.sleep(0.02)
            except (BrokenPipeError, ConnectionResetError):
                return       # вкладку закрыли — это норма, не ошибка
            return

        self.send_error(404)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", help="последовательный порт устройства")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--http", type=int, default=8080)
    a = ap.parse_args()

    port = a.port or find_port()
    if not port:
        print("устройство не найдено — укажите порт через --port")
        print("доступные:", ", ".join(p.device for p in serial.tools.list_ports.comports()) or "нет")
        return

    threading.Thread(target=reader, args=(port, a.baud), daemon=True).start()
    print(f"открой http://localhost:{a.http}")
    ThreadingHTTPServer(("127.0.0.1", a.http), Handler).serve_forever()


if __name__ == "__main__":
    main()
