# /// script
# requires-python = ">=3.11"
# dependencies = ["pillow>=10"]
# ///
"""把各 provider(gpt/cc/gem/grok) 的账户剩余额度画成卡片显示在 StackChan 屏幕上, 每个 provider 一页。

数据与 Claude Code 状态栏同源: ~/.claude/cliproxy-quota.json, 由 cliproxy-quota.ts 采集。
剩余% = 100 - used, 与状态栏一致; 数据过期(stale)的 provider 名字后标 ~。
每个 provider 的账户按每页最多 3 个分页, 账户过多时自动续页。
在屏幕上向左划看下一页, 向右划看上一页; 双击屏幕只重新采集当前 provider。
后台每 30 分钟仍全量刷新, 各 provider 分别记录最近一次单页刷新时间。

环境变量:
  QUOTA_INTERVAL_MIN  数据刷新间隔, 默认 30
  STACKCHAN_URL       控制服务地址, 默认 http://127.0.0.1:5310
"""

import json
import os
import subprocess
import tempfile
import threading
import time
import urllib.request

from PIL import Image, ImageDraw, ImageFont

CLAUDE_DIR = os.environ.get("CLAUDE_CONFIG_DIR", os.path.expanduser("~/.claude"))
CACHE = os.path.join(CLAUDE_DIR, "cliproxy-quota.json")
COLLECTOR = os.path.join(CLAUDE_DIR, "cliproxy-quota.ts")
CACHE_TTL = 300  # 与状态栏一致: 缓存超过 5 分钟才重新采集
INTERVAL = float(os.environ.get("QUOTA_INTERVAL_MIN", "30")) * 60
STACKCHAN = os.environ.get("STACKCHAN_URL", "http://127.0.0.1:5310")
FONT = "/System/Library/Fonts/Hiragino Sans GB.ttc"
WINDOW_LABEL = {18000: "5h", 604800: "7d", 2592000: "30d"}


def log(*a):
    print(time.strftime("%m-%d %H:%M:%S"), *a, flush=True)


def cache_age(provider=None):
    try:
        with open(CACHE) as f:
            data = json.load(f)
        updated_at = (data.get("provider_updated_at") or {}).get(provider, data.get("updated_at", 0)) \
            if provider else data.get("updated_at", 0)
        return time.time() - updated_at
    except (OSError, ValueError, TypeError):
        return float("inf")


def refresh_cache(force=False, provider=None):
    if not force and cache_age(provider) < CACHE_TTL:
        return
    bun = os.path.expanduser("~/.bun/bin/bun")
    env = {k: v for k, v in os.environ.items() if "proxy" not in k.lower()}
    cmd = [bun, "--no-env-file", "--use-system-ca", COLLECTOR]
    if provider:
        cmd += ["--provider", provider]
    try:
        subprocess.run(cmd, env=env, capture_output=True, timeout=60, check=True)
    except Exception as e:
        log(f"采集失败, 用旧缓存: {e}")


# (缓存里的 key, 屏幕上的名字), 名字与状态栏一致
PROVIDERS = [("codex", "gpt"), ("claude", "cc"), ("antigravity", "gem"), ("xai", "grok")]


def load_pages():
    """返回 [(provider 名, 是否过期, [(账户名, [(窗口, 剩余%, 重置时间戳)])]), ...], 以及数据更新时间。
    同一账户出现在多个 CPA 实例时按邮箱去重; 数据不一致时保留剩余更少的值。"""
    with open(CACHE) as f:
        data = json.load(f)
    pages = []
    provider_times = data.get("provider_updated_at") or {}
    for key, label in PROVIDERS:
        accounts, stale = {}, False
        for inst_name, inst in (data.get("instances") or {}).items():
            quota = (inst or {}).get(key) or {}
            stale = stale or bool(quota.get("stale"))
            for acc_index, acc in enumerate(quota.get("accounts") or []):
                if key == "xai":  # grok 只有周额度, 字段直接平铺在账户上
                    raw = [{"seconds": 604800, "used": acc.get("used"), "reset_at": acc.get("reset_at")}]
                else:
                    raw = acc.get("windows") or []
                wins = []
                for w in sorted(raw, key=lambda w: w.get("seconds") or 0):
                    if w.get("used") is None:
                        continue
                    wl = "wk" if key == "xai" else WINDOW_LABEL.get(w.get("seconds"), f"{(w.get('seconds') or 0) // 3600}h")
                    wins.append((wl, max(0, round(100 - w["used"])), w.get("reset_at")))
                if not wins:
                    continue
                email = acc.get("email")
                identity = email.casefold() if email else (inst_name, acc_index)
                name, merged = accounts.setdefault(identity, (email or "?", {}))
                for wl, left, reset_at in wins:
                    previous = merged.get(wl)
                    if previous is None or left < previous[0]:
                        merged[wl] = (left, reset_at)
        rows = [((name.split("@")[0])[:12], [(wl, *values) for wl, values in wins.items()])
                for name, wins in accounts.values()]
        if rows:
            updated_at = provider_times.get(key, data.get("updated_at", 0))
            for start in range(0, len(rows), 3):
                pages.append((label, stale, rows[start:start + 3], updated_at))
    return pages, data.get("updated_at", 0)


def fmt_eta(reset_at):
    secs = int((reset_at or 0) - time.time())
    if secs <= 0:
        return ""
    d, h, m = secs // 86400, secs % 86400 // 3600, secs % 3600 // 60
    return f"{d}d{h}h" if d else f"{h}h{m}m" if h else f"{m}m"


def color(left):
    return (240, 110, 110) if left <= 10 else (240, 210, 100) if left <= 30 else (120, 210, 140)


def render(provider, page, pages, refreshing=False) -> str:
    label, stale, accounts, updated_at = provider
    img = Image.new("RGB", (320, 240), (18, 18, 24))
    d = ImageDraw.Draw(img)
    f_title, f_name, f_pct, f_small = (ImageFont.truetype(FONT, s) for s in (24, 16, 20, 13))
    d.text((12, 6), label + ("~" if stale else ""), font=f_title, fill="white")
    d.text((90, 14), "剩余额度", font=f_small, fill=(130, 130, 145))
    d.text((308, 12), f"{page + 1}/{pages}", font=f_small, fill=(140, 140, 150), anchor="ra")

    # 每个账户一块: 名字一行, 下面各窗口并排 (最多两列); 只有一个窗口(如 grok 只有周额度)时占满整行
    top, bottom = 40, 222
    block_h = min(62, (bottom - top) // max(len(accounts), 1))
    y = top
    for name, wins in accounts:
        d.text((12, y), name, font=f_name, fill=(200, 200, 215))
        col_w = 296 if len(wins) == 1 else 144
        for k, (wl, left, reset_at) in enumerate(wins[:2]):
            x = 12 + k * (col_w + 8)
            c = color(left)
            ry = y + 20
            d.text((x, ry + 4), wl, font=f_small, fill=(150, 150, 160))
            d.text((x + 30, ry), f"{left}%", font=f_pct, fill=c)
            eta = fmt_eta(reset_at)
            if eta:
                d.text((x + col_w, ry + 5), eta, font=f_small, fill=(110, 110, 120), anchor="ra")
            by = ry + 25
            d.rounded_rectangle((x, by, x + col_w, by + 5), 2, fill=(50, 50, 60))
            if left > 0:
                d.rounded_rectangle((x, by, x + col_w * left // 100, by + 5), 2, fill=c)
        y += block_h
    stamp = time.strftime("%H:%M", time.localtime(updated_at)) if updated_at else "--:--"
    footer = ("刷新中…", (240, 210, 100)) if refreshing else (f"数据 {stamp}", (90, 90, 100))
    d.text((308, 236), footer[0], font=f_small, fill=footer[1], anchor="rb")
    path = os.path.join(tempfile.gettempdir(), "stackchan-quota.jpg")
    img.save(path, "JPEG", quality=90)
    return path


def render_empty() -> str:
    img = Image.new("RGB", (320, 240), (18, 18, 24))
    d = ImageDraw.Draw(img)
    d.text((160, 120), "额度: 暂无数据", font=ImageFont.truetype(FONT, 20), fill=(160, 160, 170), anchor="mm")
    path = os.path.join(tempfile.gettempdir(), "stackchan-quota.jpg")
    img.save(path, "JPEG", quality=90)
    return path


_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def show(path, wake):
    # 后台定时刷新用 wake=False 静默换图, 不把熄了的屏幕点亮
    body = json.dumps({"path": path, "wake": wake}).encode()
    req = urllib.request.Request(f"{STACKCHAN}/show", body, {"Content-Type": "application/json"})
    return json.loads(_opener.open(req, timeout=30).read())


# 在独立线程里订阅控制服务的事件流: 左右划动翻页, 双击刷新; 断了就重连
DOUBLE_TAP_SEC = 0.5
flip = 0                     # 待处理的翻页步数, 左划 +1 右划 -1
flip_lock = threading.Lock()
refresh_req = threading.Event()
redraw_req = threading.Event()  # 设备重连后固件已回到表情脸, 强制重推当前页
wake = threading.Event()     # 有操作时立刻处理, 不等下次数据刷新


def listen_screen():
    global flip
    last_tap = 0.0
    while True:
        try:
            with _opener.open(f"{STACKCHAN}/events", timeout=60) as stream:
                # 也可能错过设备未连 host 时发出的 hello, 所以事件流恢复本身就触发重画
                redraw_req.set()
                wake.set()
                for line in stream:
                    if not line.startswith(b"data: "):
                        continue
                    evt = json.loads(line[6:])
                    if evt.get("evt") == "hello" or (evt.get("evt") == "link" and evt.get("via")):
                        redraw_req.set()
                        wake.set()
                        continue
                    if evt.get("evt") != "screen":
                        continue
                    if evt.get("type") == "tap":
                        now = time.time()
                        if now - last_tap < DOUBLE_TAP_SEC:
                            last_tap = 0.0
                            refresh_req.set()
                            wake.set()
                        else:
                            last_tap = now
                        continue
                    step = {"swipe_left": 1, "swipe_right": -1}.get(evt.get("type"))
                    if step:
                        with flip_lock:
                            flip += step
                        wake.set()
        except Exception as e:
            log(f"事件流断开, 5 秒后重连: {e}")
            time.sleep(5)


def main():
    global flip
    threading.Thread(target=listen_screen, daemon=True).start()
    pages, next_refresh = [], 0
    page, shown = 0, None
    while True:
        wake.clear()
        try:
            redraw = redraw_req.is_set()
            if redraw:
                redraw_req.clear()
                shown = None
            forced = refresh_req.is_set()
            current_provider = next(key for key, label in PROVIDERS if label == pages[page][0]) if pages else None
            if forced and pages:  # 先给个"刷新中"的反馈, 采集要几秒
                show(render(pages[page], page, len(pages), refreshing=True), wake=True)
            if forced or time.time() >= next_refresh:
                refresh_req.clear()
                refresh_cache(force=forced, provider=current_provider if forced else None)
                pages, _ = load_pages()
                next_refresh = time.time() + INTERVAL
                shown = None  # 数据变了, 强制重画
                log((f"双击刷新 {current_provider}: " if forced else "数据已刷新: ") + (", ".join(
                    f"{p[0]}({len(p[2])}个账户)" for p in pages) or "无数据"))
            with flip_lock:
                step, flip = flip, 0
            user_action = forced or step != 0 or redraw
            if not pages:
                if shown != "empty":
                    show(render_empty(), wake=user_action)
                    shown = "empty"
            else:
                page = (page + step) % len(pages)
                key = (page, pages[page][3])
                if shown != key:
                    show(render(pages[page], page, len(pages)), wake=user_action)
                    shown = key
        except Exception as e:
            log(f"失败: {e}")
            shown = None
        wake.wait(max(1, min(60, next_refresh - time.time())))


if __name__ == "__main__":
    main()
