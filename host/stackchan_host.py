# /// script
# requires-python = ">=3.11"
# dependencies = ["bleak>=0.22", "pyserial>=3.5", "pillow>=10"]
# ///
"""StackChan 木偶固件的 Mac 端控制服务。

设备只执行指令/上报事件, 所有逻辑都在这里。对外提供 HTTP 接口(默认 :5310):

  POST /say      {"text": "...", "voice": "Tingting", "rate": 200}   Mac 上 TTS 后推音频
  POST /play     {"path": "/x/y.mp3"}                                  播放本地音频文件
  POST /show     {"url"|"path"|"text": ..., "ms": 5000, "fit": "contain|cover", "wake": true}
                 wake=false: 只换画面不点亮屏幕 (熄屏时也不会亮), 给后台定时刷新用
  POST /screen   {"on": true|false}                                    手动亮屏/熄屏
  POST /clear                                                          回到表情脸
  POST /head     {"yaw": -45~45, "pitch": 30~60, "ms": 400}
  POST /seq      {"steps": [[yaw, pitch, ms], ...]}
  POST /gesture  {"name": "nod|shake|dizzy|look_around|tilt"}
  POST /face     {"expr": "happy|angry|sad|doubt|sleepy|neutral"}
  POST /bubble   {"text": "..."}                                       表情脸上的气泡文字
  POST /volume   {"v": 0~255}      POST /brightness {"v": 0~255}
  POST /config   {"auto_react": false, "sleep_sec": 180}               关掉被摸时的本地反应 / 无操作多久熄屏(0=不熄)
  POST /cmd      {...}                                                 原样透传 JSON 指令
  GET  /status                     GET /events  (SSE 事件流)

设备事件(摸头/屏幕点击等)会推给 /events 订阅者, 并 POST 到环境变量
STACKCHAN_WEBHOOK 指定的地址(如果配置了)。

通道: USB 串口优先; 没插线时自动走 BLE。两条通道帧格式相同:
  0xA5 0x5A | type u8 | len u32 LE | payload | sum8(payload)
"""

import asyncio
import io
import json
import os
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import wave
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import serial
from bleak import BleakClient, BleakScanner
from PIL import Image, ImageDraw, ImageFont
from serial.tools import list_ports

PORT = int(os.environ.get("STACKCHAN_PORT", "5310"))
WEBHOOK = os.environ.get("STACKCHAN_WEBHOOK", "")
BLE_NAME = os.environ.get("STACKCHAN_BLE_NAME", "StackChan")
# macOS 上没有蓝牙授权的进程一碰 CoreBluetooth 就会被系统 SIGABRT, 没授权时设 0 关掉
BLE_ENABLED = os.environ.get("STACKCHAN_BLE", "1") != "0"
DEFAULT_VOICE = os.environ.get("STACKCHAN_VOICE", "Tingting")
FONT_PATH = "/System/Library/Fonts/Hiragino Sans GB.ttc"

NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"
NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"
ESPRESSIF_VID = 0x303A

T_JSON, T_JPEG, T_AUDIO, T_JPEG_QUIET = 1, 2, 3, 4
AUDIO_RATE = 16000

GESTURES = {
    "nod": [[0, 55, 250], [0, 35, 250], [0, 55, 250], [0, 35, 250], [0, 45, 300]],
    "shake": [[-30, 45, 250], [30, 45, 300], [-30, 45, 300], [30, 45, 300], [0, 45, 300]],
    "dizzy": [[30, 56, 300], [0, 58, 300], [-30, 56, 300], [0, 40, 300]] * 2 + [[0, 45, 400]],
    "look_around": [[-40, 50, 600], [-40, 50, 500], [40, 50, 900], [40, 50, 500], [0, 45, 600]],
    "tilt": [[0, 38, 300], [12, 52, 400], [0, 45, 400]],
}


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


# ---------------------------------------------------------------------------
# 帧编解码
# ---------------------------------------------------------------------------
def encode_frame(ftype: int, payload: bytes) -> bytes:
    return b"\xa5\x5a" + struct.pack("<BI", ftype, len(payload)) + payload + bytes([sum(payload) & 0xFF])


class FrameParser:
    """按字节流解析帧, 遇到乱码会靠帧头重新同步。"""

    def __init__(self, on_frame):
        self.buf = bytearray()
        self.on_frame = on_frame

    def feed(self, data: bytes):
        self.buf += data
        while True:
            i = self.buf.find(b"\xa5\x5a")
            if i < 0:
                del self.buf[:-1]
                return
            del self.buf[:i]
            if len(self.buf) < 7:
                return
            ftype, n = struct.unpack_from("<BI", self.buf, 2)
            if n > 1 << 20:
                del self.buf[:2]
                continue
            if len(self.buf) < 7 + n + 1:
                return
            payload = bytes(self.buf[7 : 7 + n])
            ok = (sum(payload) & 0xFF) == self.buf[7 + n]
            del self.buf[: 7 + n + 1]
            if ok:
                self.on_frame(ftype, payload)


# ---------------------------------------------------------------------------
# IMA ADPCM 编码 (与固件 adpcm.h 对应), BLE 下把音频压到 1/4
# ---------------------------------------------------------------------------
_STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
         73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449,
         494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272,
         2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
         10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767]
_INDEX = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]


def adpcm_encode(pcm: bytes) -> bytes:
    samples = struct.unpack(f"<{len(pcm) // 2}h", pcm[: len(pcm) // 2 * 2])
    pred, index = 0, 0
    out = bytearray((len(samples) + 1) // 2)
    for i, s in enumerate(samples):
        step = _STEP[index]
        diff = s - pred
        nib = 0
        if diff < 0:
            nib = 8
            diff = -diff
        if diff >= step:
            nib |= 4
            diff -= step
        if diff >= step >> 1:
            nib |= 2
            diff -= step >> 1
        if diff >= step >> 2:
            nib |= 1
        # 用与解码端完全相同的方式重建 predictor, 防止误差累积
        d = step >> 3
        if nib & 4:
            d += step
        if nib & 2:
            d += step >> 1
        if nib & 1:
            d += step >> 2
        pred = max(-32768, min(32767, pred - d if nib & 8 else pred + d))
        index = max(0, min(88, index + _INDEX[nib]))
        if i & 1:
            out[i >> 1] |= nib << 4
        else:
            out[i >> 1] = nib
    return bytes(out)


# ---------------------------------------------------------------------------
# 通道
# ---------------------------------------------------------------------------
class SerialLink:
    name = "usb"

    def __init__(self, hub):
        self.hub = hub
        self.ser = None
        self.lock = threading.Lock()
        threading.Thread(target=self._run, daemon=True).start()

    @property
    def connected(self):
        return self.ser is not None

    def _find_port(self):
        for p in list_ports.comports():
            if p.vid == ESPRESSIF_VID:
                return p.device
        return None

    def _run(self):
        while True:
            dev = self._find_port()
            if not dev:
                time.sleep(2)
                continue
            try:
                ser = serial.Serial(dev, 115200, timeout=0.1)
                parser = FrameParser(self.hub.on_frame)
                self.ser = ser
                log(f"USB 已连接 {dev}")
                self.hub.on_link_change()
                while True:
                    data = ser.read(4096)
                    if data:
                        parser.feed(data)
            except (serial.SerialException, OSError) as e:
                log(f"USB 断开: {e}")
            finally:
                self.ser = None
                self.hub.on_link_change()
            time.sleep(1)

    def send(self, frame: bytes):
        with self.lock:
            if not self.ser:
                raise ConnectionError("USB 未连接")
            self.ser.write(frame)
            self.ser.flush()


class BleLink:
    """USB 没插时才连 BLE; USB 一插上就主动断开 BLE, 避免事件重复。"""

    name = "ble"

    def __init__(self, hub):
        self.hub = hub
        self.client = None
        self.loop = asyncio.new_event_loop()
        threading.Thread(target=self.loop.run_forever, daemon=True).start()
        asyncio.run_coroutine_threadsafe(self._run(), self.loop)

    @property
    def connected(self):
        return self.client is not None and self.client.is_connected

    async def _run(self):
        while True:
            try:
                if self.hub.usb.connected:
                    await asyncio.sleep(2)
                    continue
                dev = await BleakScanner.find_device_by_name(BLE_NAME, timeout=8)
                if not dev or self.hub.usb.connected:
                    continue
                parser = FrameParser(self.hub.on_frame)
                gone = asyncio.Event()
                client = BleakClient(dev, disconnected_callback=lambda _: self.loop.call_soon_threadsafe(gone.set))
                await client.connect()
                await client.start_notify(NUS_TX, lambda _, data: parser.feed(bytes(data)))
                self.client = client
                log(f"BLE 已连接 {dev.address} mtu={client.mtu_size}")
                self.hub.on_link_change()
                while not gone.is_set():
                    if self.hub.usb.connected:
                        log("USB 已插入, 断开 BLE")
                        await client.disconnect()
                        break
                    try:
                        await asyncio.wait_for(gone.wait(), 1)
                    except asyncio.TimeoutError:
                        pass
            except Exception as e:
                log(f"BLE 错误: {e!r}")
                await asyncio.sleep(3)
            finally:
                if self.client:
                    self.client = None
                    log("BLE 断开")
                    self.hub.on_link_change()

    async def _write(self, frame: bytes):
        client = self.client
        if not client or not client.is_connected:
            raise ConnectionError("BLE 未连接")
        chunk = max(20, client.mtu_size - 3)
        for off in range(0, len(frame), chunk):
            await client.write_gatt_char(NUS_RX, frame[off : off + chunk], response=False)

    def send(self, frame: bytes):
        timeout = 10 + len(frame) / 5000
        asyncio.run_coroutine_threadsafe(self._write(frame), self.loop).result(timeout)


class Hub:
    def __init__(self):
        self.events = deque(maxlen=100)
        self.subscribers: list[queue.Queue] = []
        self.sub_lock = threading.Lock()
        self.send_lock = threading.Lock()
        self.clear_timer = None
        self.device_info = {}
        self.usb = SerialLink(self)
        self.ble = BleLink(self) if BLE_ENABLED else None

    @property
    def link(self):
        if self.usb.connected:
            return self.usb
        if self.ble and self.ble.connected:
            return self.ble
        return None

    def on_link_change(self):
        link = self.link
        if link:
            threading.Thread(target=self._safe_send_json, args=({"cmd": "ping"},), daemon=True).start()
        self.publish({"evt": "link", "via": link.name if link else None})

    def _safe_send_json(self, cmd):
        try:
            time.sleep(0.3)
            self.send_json(cmd)
        except Exception as e:
            log(f"ping 失败: {e}")

    # ---- 下行 ----
    def _send(self, ftype, payload):
        link = self.link
        if not link:
            raise ConnectionError("设备未连接 (USB 和 BLE 都不通)")
        with self.send_lock:
            link.send(encode_frame(ftype, payload))
        return link.name

    def send_json(self, cmd: dict):
        return self._send(T_JSON, json.dumps(cmd, ensure_ascii=False).encode())

    def send_image(self, jpeg: bytes, ms: int = 0, wake: bool = True):
        if self.clear_timer:
            self.clear_timer.cancel()
            self.clear_timer = None
        via = self._send(T_JPEG if wake else T_JPEG_QUIET, jpeg)
        if ms > 0:
            self.clear_timer = threading.Timer(ms / 1000, lambda: self._safe_send_json({"cmd": "clear"}))
            self.clear_timer.daemon = True
            self.clear_timer.start()
        return via

    def send_audio(self, pcm: bytes, rate: int = AUDIO_RATE):
        # USB 直接发 PCM; BLE 带宽小, 压成 ADPCM
        if self.link is self.usb:
            return self._send(T_AUDIO, struct.pack("<IB", rate, 0) + pcm)
        return self._send(T_AUDIO, struct.pack("<IB", rate, 1) + adpcm_encode(pcm))

    # ---- 上行 ----
    def on_frame(self, ftype, payload):
        if ftype != T_JSON:
            return
        try:
            evt = json.loads(payload)
        except ValueError:
            return
        if evt.get("evt") in ("pong", "hello"):
            self.device_info = evt
        self.publish(evt)

    def publish(self, evt: dict):
        evt = {**evt, "ts": time.time()}
        self.events.append(evt)
        log("事件", json.dumps(evt, ensure_ascii=False))
        with self.sub_lock:
            for q in self.subscribers:
                q.put(evt)
        if WEBHOOK and evt.get("evt") not in ("link", "pong", "head_done"):
            threading.Thread(target=self._post_webhook, args=(evt,), daemon=True).start()

    def _post_webhook(self, evt):
        try:
            req = urllib.request.Request(WEBHOOK, json.dumps(evt, ensure_ascii=False).encode(),
                                         {"Content-Type": "application/json"})
            urllib.request.urlopen(req, timeout=5).read()
        except Exception as e:
            log(f"webhook 失败: {e}")


# ---------------------------------------------------------------------------
# 素材处理: TTS / 音频转码 / 图片
# ---------------------------------------------------------------------------
def _read_wav_pcm(path) -> bytes:
    with wave.open(path) as w:
        return w.readframes(w.getnframes())


def tts(text: str, voice: str, rate: int | None) -> bytes:
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "a.wav")
        cmd = ["say", "-v", voice, "-o", out, f"--data-format=LEI16@{AUDIO_RATE}"]
        if rate:
            cmd += ["-r", str(rate)]
        subprocess.run(cmd + [text], check=True, timeout=30)
        return _read_wav_pcm(out)


def load_audio_file(path: str) -> bytes:
    with tempfile.TemporaryDirectory() as d:
        out = os.path.join(d, "a.wav")
        subprocess.run(["afconvert", "-f", "WAVE", "-d", f"LEI16@{AUDIO_RATE}", "-c", "1", path, out],
                       check=True, timeout=60)
        return _read_wav_pcm(out)


def _fit(img: Image.Image, mode: str) -> Image.Image:
    img = img.convert("RGB")
    W, H = 320, 240
    if mode == "cover":
        scale = max(W / img.width, H / img.height)
    else:
        scale = min(W / img.width, H / img.height)
    img = img.resize((max(1, round(img.width * scale)), max(1, round(img.height * scale))), Image.LANCZOS)
    canvas = Image.new("RGB", (W, H), "black")
    canvas.paste(img, ((W - img.width) // 2, (H - img.height) // 2))
    return canvas


def render_text(text: str, size: int | None, color: str, bg: str) -> Image.Image:
    img = Image.new("RGB", (320, 240), bg)
    draw = ImageDraw.Draw(img)
    # 自动挑能放下的最大字号, 按字符换行(中文没有空格)
    for fs in ([size] if size else range(64, 15, -4)):
        font = ImageFont.truetype(FONT_PATH, fs)
        lines, cur = [], ""
        for ch in text:
            if ch == "\n" or draw.textlength(cur + ch, font=font) > 300:
                lines.append(cur)
                cur = "" if ch == "\n" else ch
            else:
                cur += ch
        lines.append(cur)
        line_h = fs * 1.25
        if len(lines) * line_h <= 224:
            break
    y = (240 - len(lines) * line_h) / 2
    for ln in lines:
        w = draw.textlength(ln, font=font)
        draw.text(((320 - w) / 2, y), ln, font=font, fill=color)
        y += line_h
    return img


def make_jpeg(body: dict) -> bytes:
    if "text" in body:
        img = render_text(str(body["text"]), body.get("size"), body.get("color", "white"), body.get("bg", "black"))
    elif "url" in body:
        req = urllib.request.Request(body["url"], headers={"User-Agent": "stackchan-host"})
        img = _fit(Image.open(io.BytesIO(urllib.request.urlopen(req, timeout=15).read())), body.get("fit", "contain"))
    elif "path" in body:
        img = _fit(Image.open(os.path.expanduser(body["path"])), body.get("fit", "contain"))
    else:
        raise ValueError("需要 text / url / path 之一")
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=int(body.get("quality", 80)))
    return buf.getvalue()


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------
hub: Hub


def handle_post(path: str, body: dict):
    if path == "/say":
        text = str(body.get("text", "")).strip()
        if not text:
            raise ValueError("text 不能为空")
        pcm = tts(text, body.get("voice", DEFAULT_VOICE), body.get("rate"))
        if body.get("bubble"):
            hub.send_json({"cmd": "text", "text": text})
        return {"via": hub.send_audio(pcm), "seconds": round(len(pcm) / 2 / AUDIO_RATE, 2)}
    if path == "/play":
        pcm = load_audio_file(os.path.expanduser(body["path"]))
        return {"via": hub.send_audio(pcm), "seconds": round(len(pcm) / 2 / AUDIO_RATE, 2)}
    if path == "/show":
        jpeg = make_jpeg(body)
        via = hub.send_image(jpeg, int(body.get("ms", 0)), bool(body.get("wake", True)))
        return {"via": via, "bytes": len(jpeg)}
    if path == "/gesture":
        name = body.get("name", "")
        if name not in GESTURES:
            raise ValueError(f"未知动作, 可选: {', '.join(GESTURES)}")
        return {"via": hub.send_json({"cmd": "seq", "steps": GESTURES[name]})}
    if path == "/bubble":
        return {"via": hub.send_json({"cmd": "text", "text": body.get("text", "")})}
    simple = {"/head": "head", "/seq": "seq", "/face": "face", "/clear": "clear",
              "/volume": "volume", "/brightness": "brightness", "/config": "config", "/stop": "stop",
              "/screen": "screen"}
    if path in simple:
        return {"via": hub.send_json({**body, "cmd": simple[path]})}
    if path == "/cmd":
        return {"via": hub.send_json(body)}
    return None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _json(self, code, obj):
        data = json.dumps(obj, ensure_ascii=False).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        if self.path == "/status":
            link = hub.link
            return self._json(200, {"connected": link is not None, "via": link.name if link else None,
                                    "device": hub.device_info, "recent_events": list(hub.events)[-10:]})
        if self.path == "/events":
            return self._sse()
        self._json(404, {"error": "not found"})

    def _sse(self):
        q = queue.Queue()
        with hub.sub_lock:
            hub.subscribers.append(q)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        try:
            while True:
                try:
                    evt = q.get(timeout=15)
                    self.wfile.write(f"data: {json.dumps(evt, ensure_ascii=False)}\n\n".encode())
                except queue.Empty:
                    self.wfile.write(b": keepalive\n\n")
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass
        finally:
            with hub.sub_lock:
                hub.subscribers.remove(q)

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(n) or b"{}") if n else {}
            res = handle_post(self.path, body)
            if res is None:
                return self._json(404, {"error": "not found"})
            self._json(200, {"ok": True, **res})
        except ConnectionError as e:
            self._json(503, {"ok": False, "error": str(e)})
        except Exception as e:
            self._json(400, {"ok": False, "error": f"{type(e).__name__}: {e}"})


def main():
    global hub
    hub = Hub()
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    srv.daemon_threads = True
    log(f"StackChan host 监听 :{PORT}" + (f", webhook -> {WEBHOOK}" if WEBHOOK else ""))
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
