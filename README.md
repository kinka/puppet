# StackChan 木偶固件

设备只执行指令、上报事件，所有逻辑都在 Mac 上（`host/stackchan_host.py`，pm2 名 `stackchan-host`，端口 5310）。

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

## 固件
- 工程：`firmware/`（PlatformIO + M5Unified + M5Stack-Avatar + NimBLE）
- 编译烧录：`cd firmware && ~/.local/bin/pio run -t upload`（先 `pm2 stop stackchan-host` 释放串口）
- 帧格式：`A5 5A | type u8 | len u32 LE | payload | sum8`，1=JSON，2=JPEG(320x240，会亮屏)，3=音频，4=JPEG(静默换图，不亮屏)(rate u32 | codec u8 0=PCM16 1=IMA ADPCM | data)
- 换回 xiaozhi：整片 flash 备份不进仓库。本地有备份时：`esptool --port /dev/cu.usbmodem2101 --baud 460800 write-flash 0 <备份.bin>`

## BLE
macOS 上进程没有蓝牙授权时，一访问 BLE 就会被系统 SIGABRT，所以 pm2 里默认 `STACKCHAN_BLE=0`。
在自己的终端里运行 `cd host && uv run --script stackchan_host.py`，按提示给终端开蓝牙权限，
之后把 `ecosystem.config.js` 里的 `STACKCHAN_BLE` 改成 `'1'`，再 `pm2 restart stackchan-host --update-env`。
