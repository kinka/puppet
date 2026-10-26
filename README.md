# StackChan 木偶固件

设备只执行指令、上报事件，所有逻辑都在 Mac 上。用 `pm2 start ecosystem.config.js` 独立托管两个进程，不挂在 Claude 会话里：`stackchan-host`（`host/stackchan_host.py`，端口 5310）和 `stackchan-quota`（`scripts/cpa_quota_screen.py`，把额度画成卡片推到屏幕）。

```
你的脚本/API ──HTTP──▶ stackchan-host ──USB 串口 / BLE──▶ StackChan
                 ◀── /events(SSE) / webhook ◀── 摸头、屏幕点击等事件
```

## 常用
- 命令行：`./sc`（直接运行看帮助），比如 `./sc say 你好`、`./sc show ~/a.png 5000`、`./sc g nod`、`./sc events`
- HTTP 接口：见 `host/stackchan_host.py` 文件头注释
- 事件：`hello` 若带 `last_hang`，说明上次主循环卡死被看门狗重启，内容是卡住的位置；`touch`(tap/hold/swipe_02/swipe_20, 带 ms 和 zones 位图)、`screen`(tap/hold, x, y)、`say_done`、`head_done`、`hello`、`error`
- 设 `STACKCHAN_WEBHOOK` 后，事件会 POST 到该地址
- 自动熄屏：3 分钟无操作关背光；点屏幕、摸头、推图（`/show`）、说话会点亮，唤醒那一下的触摸不上报。
  `/show` 带 `"wake": false` 只换画面不亮屏（后台定时刷新用）；`/config {"sleep_sec": N}` 改等待时间（0 = 不熄，设备重启后恢复 180）；`/screen {"on": bool}` 手动开关
- 三击头部：3 秒内连续三次短按，切换自动熄屏（`sleep_sec` 在 0 和 180 间切换）；点按和原地长按不摆头，跨触摸区的抚摸才蹭一次（连续抚摸时每 3 秒最多一次）；长按或划动会中断连击。切换成功后绿色两闪表示已关闭自动熄屏，蓝色两闪表示已开启；不弹屏、不覆盖额度卡片，也可看 `./sc status` 的 `sleep_sec` 或 `./sc events` 的 `config` 事件
- 机身灯效：`POST /led {"rgb":[255,255,0],"on_ms":500,"off_ms":500,"flashes":8}` 非阻塞闪动约 8 秒，`{"on":false}` 立即关闭；不改变屏幕或舵机动作

## 久坐提醒
- 配置：`sedentary-reminder.json`。默认工作日（周一至周五）09:00–12:00、13:00–18:00 每 60 分钟提醒，时段结束时刻不含；午休、下班后和周末不提醒。
- 每次让机身 RGB 灯黄色闪动约 13 秒，同时沿带俯仰和侧倾的椭圆轨迹缓慢转颈两轮：经过上仰、侧倾、低头等中间点，最后回中；不发声、不亮屏、不切屏。
- 由 Claude Code 的持久一次性任务读取配置、检查设备、调用 `/led` 和 `/seq` 并安排下一次提醒；固件和 host 不含久坐计时，也不需要额外常驻脚本。
- 可对 Claude Code 说“暂停久坐提醒”“恢复久坐提醒”或“把久坐提醒改为 45 分钟”；直接改配置默认在下一轮生效。
- 任务只在 Claude Code 运行且空闲时触发，不是系统级准点守护服务。原定和实际执行时间都在有效时段内、且延迟不超过 10 分钟时最多补做一次；跨午休、跨下班、周末或更久延迟会跳过动作并安排下一次有效提醒。

## 固件
- 工程：`firmware/`（PlatformIO + M5Unified + M5Stack-Avatar + NimBLE）
- 编译烧录：`cd firmware && ~/.local/bin/pio run -t upload`（先 `pm2 stop stackchan-host` 释放串口）
- 帧格式：`A5 5A | type u8 | len u32 LE | payload | sum8`，1=JSON，2=JPEG(320x240，会亮屏)，3=音频，4=JPEG(静默换图，不亮屏)(rate u32 | codec u8 0=PCM16 1=IMA ADPCM | data)
- 换回 xiaozhi：整片 flash 备份不进仓库。本地有备份时：`esptool --port /dev/cu.usbmodem2101 --baud 460800 write-flash 0 <备份.bin>`

## BLE
macOS 上进程没有蓝牙授权时，一访问 BLE 就会被系统 SIGABRT，所以 pm2 里默认 `STACKCHAN_BLE=0`。
在自己的终端里运行 `cd host && uv run --script stackchan_host.py`，按提示给终端开蓝牙权限，
之后把 `ecosystem.config.js` 里的 `STACKCHAN_BLE` 改成 `'1'`，再 `pm2 restart stackchan-host --update-env`。
