// StackChan 木偶固件 (M5Stack CoreS3 + SCS0009 舵机)
//
// 设备自身不做任何决策, 只执行 Mac 下发的指令并上报事件。
// 通道: USB 串口(HWCDC) 与 BLE(Nordic UART Service), 两条通道共用同一帧协议:
//
//   0xA5 0x5A | type u8 | len u32 LE | payload[len] | sum8(payload)
//
//   Mac -> 设备: 1=JSON 指令, 2=JPEG 图片(320x240, 会点亮屏幕), 3=音频(见 handleAudio),
//                4=JPEG 图片(静默更新: 只换画面, 不改变亮/熄屏状态, 给后台定时刷新用)
//   设备 -> Mac: 1=JSON 事件
//
// 串口上不打印任何日志, 否则会混进事件帧(Mac 端解析虽能靠帧头重同步, 但没必要)。

#include <M5Unified.h>
#include <Avatar.h>
#include <NimBLEDevice.h>
#include <ArduinoJson.h>
#include <freertos/stream_buffer.h>

#include "scs_servo.h"
#include "adpcm.h"

// M5Stack-Avatar 内部的绘制任务句柄 (Avatar.cpp 里 m5avatar 命名空间下的全局变量)
namespace m5avatar { extern TaskHandle_t drawTaskHandle; }
using namespace m5avatar;

static const char* FW_VERSION = "puppet-0.1";

// ---------------------------------------------------------------------------
// 硬件常量 (与 xiaozhi 固件里调通的值一致)
// ---------------------------------------------------------------------------
static constexpr int SERVO_TX = 6, SERVO_RX = 7, SERVO_BAUD = 1000000;
static constexpr uint8_t SERVO_YAW_ID = 1, SERVO_PITCH_ID = 2;
static constexpr int YAW_MIN = -45, YAW_MAX = 45, PITCH_MIN = 30, PITCH_MAX = 60;

static constexpr uint8_t PY32_ADDR = 0x6F;   // IO 扩展, pin0 = 舵机 5V 电源 VM_EN
static constexpr uint32_t PY32_FREQ = 100000; // 400kHz 下通信失败, 必须 100kHz
static constexpr uint8_t SI12T_ADDR = 0x68;  // 头顶电容触摸, Output1(0x10) 每区 2bit
static constexpr uint32_t SI12T_FREQ = 100000;

static constexpr size_t MAX_FRAME = 2 * 1024 * 1024;

// 诊断: 主循环记录自己当前在哪一步; type=9 的诊断帧由接收任务直接回复,
// 主循环卡死时也能问出卡在哪里
static volatile const char* loopStage = "boot";
static volatile const char* txStage = "idle";
static volatile const char* rxStage = "boot";
static volatile uint32_t loopTick = 0;

// 看门狗: 主循环 5s 没动就把各处卡在哪记进 RTC 内存再重启, 开机随 hello 上报
RTC_NOINIT_ATTR static char hangInfo[96];
RTC_NOINIT_ATTR static uint32_t hangMagic;
static constexpr uint32_t HANG_MAGIC = 0x48414E47;

// ---------------------------------------------------------------------------
// 全局状态
// ---------------------------------------------------------------------------
static Avatar avatar;
static ScsServo servo;
static bool servoReady = false;
static bool touchReady = false;
static bool showingImage = false;
static bool autoReact = true;  // 被摸时本地先蹭一下, Mac 可关掉自己接管

// 屏幕电源: 一段时间无操作就关背光+面板休眠; 点屏幕/摸头/推图/说话唤醒
static bool screenOn = true;
static uint8_t brightness = 128;
static uint32_t sleepAfterMs = 180000;  // 0 = 永不熄屏
static uint32_t lastActivityMs = 0;

static SemaphoreHandle_t txMutex;
static StreamBufferHandle_t bleRx;
static NimBLECharacteristic* bleTx = nullptr;
static volatile bool bleConnected = false;
static volatile uint16_t bleMtu = 23;

// 头部动作: 指令写入 pending, 头部任务取走执行; 新指令会打断正在执行的序列。
struct HeadStep { int16_t yaw, pitch, ms; };
static SemaphoreHandle_t headMutex;
static HeadStep headPending[64];
static int headPendingLen = 0;
static uint32_t headGen = 0;
static int lastYaw = 0, lastPitch = 45;

// 音频: 播放期间 buffer 不能释放, 下一段音频来时再换
static int16_t* audioBuf = nullptr;
static size_t audioLen = 0;
static uint32_t audioRate = 16000;
static uint32_t audioStartMs = 0;

// ---------------------------------------------------------------------------
// 帧发送 (设备 -> Mac)
// ---------------------------------------------------------------------------
static void sendFrame(uint8_t type, const uint8_t* data, uint32_t len) {
  uint8_t hdr[7] = {0xA5, 0x5A, type,
                    (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24)};
  uint8_t sum = 0;
  for (uint32_t i = 0; i < len; i++) sum += data[i];

  xSemaphoreTake(txMutex, portMAX_DELAY);
  txStage = "serial_write";
  if (Serial) {
    Serial.write(hdr, sizeof(hdr));
    Serial.write(data, len);
    Serial.write(&sum, 1);
  }
  txStage = "ble_notify";
  if (bleConnected && bleTx) {
    // 事件都很小, 拼成一块按 MTU 切片 notify
    std::string buf((const char*)hdr, sizeof(hdr));
    buf.append((const char*)data, len);
    buf.push_back((char)sum);
    size_t chunk = bleMtu > 23 ? bleMtu - 3 : 20;
    for (size_t off = 0; off < buf.size(); off += chunk) {
      size_t n = std::min(chunk, buf.size() - off);
      bleTx->setValue((const uint8_t*)buf.data() + off, n);
      bleTx->notify();
    }
  }
  txStage = "idle";
  xSemaphoreGive(txMutex);
}

static void sendEvent(JsonDocument& doc) {
  std::string s;
  serializeJson(doc, s);
  sendFrame(1, (const uint8_t*)s.data(), s.size());
}

static void sendSimpleEvent(const char* evt, const char* key = nullptr, const char* val = nullptr) {
  JsonDocument doc;
  doc["evt"] = evt;
  if (key) doc[key] = val;
  sendEvent(doc);
}

// ---------------------------------------------------------------------------
// 舵机电源 (PY32) 与摸头传感器 (Si12T), 都挂在内部 I2C 上
// ---------------------------------------------------------------------------
static bool py32Read(uint8_t reg, uint8_t* out) {
  for (int i = 0; i < 3; i++) {
    if (M5.In_I2C.readRegister(PY32_ADDR, reg, out, 1, PY32_FREQ)) return true;
    delay(15);
  }
  return false;
}

static bool py32SetBit(uint8_t reg, uint8_t bit, bool v) {
  uint8_t cur = 0;
  if (!py32Read(reg, &cur)) return false;
  cur = v ? (cur | (1u << bit)) : (cur & ~(1u << bit));
  return M5.In_I2C.writeRegister8(PY32_ADDR, reg, cur, PY32_FREQ);
}

// 原版固件不碰 PY32 -> 舵机一直无电。必须先拉高 VM_EN 再初始化舵机。
static bool servoPowerOn() {
  uint8_t ver = 0;
  bool ok = false;
  for (int i = 0; i < 5 && !ok; i++) {  // PY32 上电后要几百 ms 才可靠
    delay(300);
    ok = py32Read(0x02, &ver) && ver != 0x00 && ver != 0xFF;
  }
  if (!ok) return false;
  py32SetBit(0x03, 0, true);   // 方向: 输出
  py32SetBit(0x0B, 0, false);  // 关下拉
  py32SetBit(0x09, 0, true);   // 开上拉
  py32SetBit(0x05, 0, true);   // 输出高 -> 接通 5V
  delay(200);
  uint8_t rb = 0;
  return py32Read(0x05, &rb) && (rb & 0x01);
}

// 初始化序列照搬 M5Stack 官方 StackChan 固件 (hal/drivers/Si12T), 只清 SLEEP 是不会出触摸数据的
static void touchInit() {
  uint8_t ctrl = 0;
  if (!M5.In_I2C.readRegister(SI12T_ADDR, 0x09, &ctrl, 1, SI12T_FREQ)) return;
  for (uint8_t reg = 0x0A; reg <= 0x0F; reg++) {  // 打开全部通道及参考校准
    M5.In_I2C.writeRegister8(SI12T_ADDR, reg, 0x00, SI12T_FREQ);
  }
  M5.In_I2C.writeRegister8(SI12T_ADDR, 0x09, 0x0F, SI12T_FREQ);  // 软复位
  M5.In_I2C.writeRegister8(SI12T_ADDR, 0x09, 0x07, SI12T_FREQ);
  M5.In_I2C.writeRegister8(SI12T_ADDR, 0x08, 0x22, SI12T_FREQ);  // 自动模式, 中/高中断, 响应 2+2
  for (uint8_t reg = 0x02; reg <= 0x06; reg++) {  // 灵敏度: LOW 档 level 3
    M5.In_I2C.writeRegister8(SI12T_ADDR, reg, 0x33, SI12T_FREQ);
  }
  uint8_t o1 = 0;
  if (!M5.In_I2C.readRegister(SI12T_ADDR, 0x10, &o1, 1, SI12T_FREQ) || o1 == 0xFF) return;
  touchReady = true;
}

// 返回 3 个触摸区的位图 (bit0/1/2), 读失败返回 -1
// 注意: 被摸时芯片偶尔会 NACK, 不能把读失败当成松手
static int touchZones() {
  uint8_t o1 = 0;
  if (!M5.In_I2C.readRegister(SI12T_ADDR, 0x10, &o1, 1, SI12T_FREQ) || o1 == 0xFF) return -1;
  int z = 0;
  for (int i = 0; i < 3; i++) if ((o1 >> (i * 2)) & 0x3) z |= 1 << i;
  return z;
}
// ---------------------------------------------------------------------------
// 头部动作
// ---------------------------------------------------------------------------
static int yawToPos(int deg) { return constrain(460 + deg * 16 / 5, 0, 1000); }
static int pitchToPos(int deg) { return constrain(620 + constrain(deg, 0, 88) * 16 / 5, 0, 1000); }

static void servoMove(int yaw, int pitch, int ms) {
  yaw = constrain(yaw, YAW_MIN, YAW_MAX);
  pitch = constrain(pitch, PITCH_MIN, PITCH_MAX);
  servo.writePos(SERVO_YAW_ID, yawToPos(yaw), ms);
  servo.writePos(SERVO_PITCH_ID, pitchToPos(pitch), ms);
}

// 提交一段动作序列, 打断正在执行的序列
static void headSubmit(const HeadStep* steps, int n) {
  xSemaphoreTake(headMutex, portMAX_DELAY);
  headPendingLen = std::min(n, (int)(sizeof(headPending) / sizeof(headPending[0])));
  memcpy(headPending, steps, headPendingLen * sizeof(HeadStep));
  headGen++;
  xSemaphoreGive(headMutex);
}

static void headTask(void*) {
  HeadStep steps[64];
  uint32_t seenGen = 0;
  while (true) {
    int n = 0;
    xSemaphoreTake(headMutex, portMAX_DELAY);
    if (headGen != seenGen) {
      seenGen = headGen;
      n = headPendingLen;
      memcpy(steps, headPending, n * sizeof(HeadStep));
    }
    xSemaphoreGive(headMutex);

    for (int i = 0; i < n; i++) {
      if (servoReady) servoMove(steps[i].yaw, steps[i].pitch, steps[i].ms);
      // 分段等待, 期间有新指令就立刻放弃当前序列
      uint32_t until = millis() + steps[i].ms;
      bool interrupted = false;
      while ((int32_t)(until - millis()) > 0) {
        delay(10);
        if (headGen != seenGen) { interrupted = true; break; }
      }
      if (interrupted) break;
    }
    if (n > 0 && headGen == seenGen) {
      JsonDocument doc;
      doc["evt"] = "head_done";
      sendEvent(doc);
    }
    delay(10);
  }
}

static void headTo(int yaw, int pitch, int ms) {
  lastYaw = constrain(yaw, YAW_MIN, YAW_MAX);
  lastPitch = constrain(pitch, PITCH_MIN, PITCH_MAX);
  HeadStep s{(int16_t)lastYaw, (int16_t)lastPitch, (int16_t)ms};
  headSubmit(&s, 1);
}

// 被摸时的本地即时反应: 轻蹭一下再回原朝向
static void nuzzle() {
  int y = lastYaw, p = lastPitch;
  HeadStep s[3] = {{(int16_t)(y + 8), (int16_t)(p - 6), 250},
                   {(int16_t)(y - 8), (int16_t)(p + 4), 250},
                   {(int16_t)y, (int16_t)p, 300}};
  headSubmit(s, 3);
}

// ---------------------------------------------------------------------------
// 显示
// ---------------------------------------------------------------------------
static void setScreen(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  if (on) {
    M5.Display.wakeup();
    M5.Display.setBrightness(brightness);
  } else {
    M5.Display.setBrightness(0);
    M5.Display.sleep();  // 面板休眠后显存内容保留, 熄屏期间仍可往里画
  }
  JsonDocument doc;
  doc["evt"] = "screen_power";
  doc["on"] = on;
  sendEvent(doc);
}

// 有操作: 计时清零, 熄屏中则点亮
static void activity() {
  lastActivityMs = millis();
  setScreen(true);
}

static void checkScreenSleep() {
  if (screenOn && sleepAfterMs && millis() - lastActivityMs > sleepAfterMs) setScreen(false);
}

// 表情脸和图片都只在主循环里画, 不存在两个任务同时写屏的问题
static void showImage(const uint8_t* jpg, size_t len) {
  showingImage = true;
  // 不先涂黑: Mac 端推的都是满屏图, 直接覆盖, 换图时不会闪
  M5.Display.startWrite();
  M5.Display.drawJpg(jpg, len, 0, 0, 320, 240, 0, 0, 0.0f, 0.0f, datum_t::middle_center);
  M5.Display.endWrite();
}

static void showAvatar() { showingImage = false; }

static void drawAvatar() {
  static uint32_t last = 0;
  if (showingImage || !screenOn || millis() - last < 33) return;
  last = millis();
  avatar.draw();
}

// Avatar 自带的绘制任务与主循环抢屏幕(stop/start 有竞态, 显示图片时会崩)。
// 初始化时临时把自己提到更高优先级, 让新建的绘制任务一次都跑不上就删掉,
// 只保留负责眨眼/呼吸/眼神的状态任务, 绘制改由主循环 drawAvatar() 负责。
static void avatarInit() {
  UBaseType_t prio = uxTaskPriorityGet(nullptr);
  vTaskPrioritySet(nullptr, configMAX_PRIORITIES - 1);
  avatar.init(8);
  vTaskDelete(drawTaskHandle);
  // setExpression() 内部会 vTaskSuspend(drawTaskHandle); 句柄为空时挂起的是调用者自己,
  // 所以换成一个什么都不做的占位任务
  xTaskCreatePinnedToCore([](void*) { while (true) delay(60000); }, "avatar_stub", 1024, nullptr, 1,
                          &drawTaskHandle, APP_CPU_NUM);
  vTaskPrioritySet(nullptr, prio);
}

static Expression parseExpr(const char* s) {
  if (!s) return Expression::Neutral;
  if (!strcmp(s, "happy")) return Expression::Happy;
  if (!strcmp(s, "angry")) return Expression::Angry;
  if (!strcmp(s, "sad")) return Expression::Sad;
  if (!strcmp(s, "doubt")) return Expression::Doubt;
  if (!strcmp(s, "sleepy")) return Expression::Sleepy;
  return Expression::Neutral;
}

// ---------------------------------------------------------------------------
// 音频: payload = rate u32 LE | codec u8 (0=PCM16 LE, 1=IMA ADPCM 4bit) | data
// 整段收完再播 (PSRAM 足够), BLE 慢一点也不会断音
// ---------------------------------------------------------------------------
static void handleAudio(const uint8_t* p, size_t len) {
  if (len < 5) return;
  uint32_t rate = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
  uint8_t codec = p[4];
  p += 5; len -= 5;

  M5.Speaker.stop();
  while (M5.Speaker.isPlaying()) delay(1);
  free(audioBuf);
  audioBuf = nullptr;

  size_t samples = codec == 1 ? len * 2 : len / 2;
  audioBuf = (int16_t*)ps_malloc(samples * sizeof(int16_t));
  if (!audioBuf) { sendSimpleEvent("error", "msg", "audio oom"); return; }
  if (codec == 1) adpcmDecode(p, len, audioBuf);
  else memcpy(audioBuf, p, samples * 2);

  audioLen = samples;
  audioRate = rate;
  audioStartMs = millis();
  M5.Speaker.playRaw(audioBuf, audioLen, audioRate, false, 1, 0, true);
}

// 按播放进度取当前音量驱动嘴型
static void updateMouth() {
  if (showingImage) return;
  if (!audioBuf || !M5.Speaker.isPlaying()) {
    if (audioBuf && audioStartMs) {
      audioStartMs = 0;
      avatar.setMouthOpenRatio(0);
      sendSimpleEvent("say_done");
    }
    return;
  }
  size_t idx = (size_t)((uint64_t)(millis() - audioStartMs) * audioRate / 1000);
  if (idx >= audioLen) return;
  size_t end = std::min(audioLen, idx + 512);
  uint32_t acc = 0;
  for (size_t i = idx; i < end; i++) acc += abs(audioBuf[i]);
  float level = end > idx ? (float)acc / (end - idx) / 6000.0f : 0;
  avatar.setMouthOpenRatio(std::min(1.0f, level));
}

// ---------------------------------------------------------------------------
// 指令处理
// ---------------------------------------------------------------------------
static void handleJson(const uint8_t* p, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, p, len)) { sendSimpleEvent("error", "msg", "bad json"); return; }
  const char* cmd = doc["cmd"] | "";

  if (!strcmp(cmd, "ping")) {
    JsonDocument r;
    r["evt"] = "pong";
    r["fw"] = FW_VERSION;
    r["servo"] = servoReady;
    r["touch"] = touchReady;
    r["id"] = doc["id"];
    sendEvent(r);
  } else if (!strcmp(cmd, "head")) {
    headTo(doc["yaw"] | lastYaw, doc["pitch"] | lastPitch, doc["ms"] | 400);
  } else if (!strcmp(cmd, "seq")) {
    HeadStep steps[64];
    int n = 0;
    for (JsonArrayConst s : doc["steps"].as<JsonArrayConst>()) {
      if (n >= 64) break;
      steps[n++] = {(int16_t)(s[0] | 0), (int16_t)(s[1] | 45), (int16_t)(s[2] | 300)};
    }
    if (n > 0) {
      lastYaw = steps[n - 1].yaw;
      lastPitch = steps[n - 1].pitch;
      headSubmit(steps, n);
    }
  } else if (!strcmp(cmd, "face")) {
    activity();
    showAvatar();
    avatar.setExpression(parseExpr(doc["expr"] | "neutral"));
  } else if (!strcmp(cmd, "text")) {
    activity();
    showAvatar();
    avatar.setSpeechText(doc["text"] | "");
  } else if (!strcmp(cmd, "clear")) {
    showAvatar();
    avatar.setSpeechText("");
  } else if (!strcmp(cmd, "stop")) {
    M5.Speaker.stop();
  } else if (!strcmp(cmd, "volume")) {
    M5.Speaker.setVolume(constrain(doc["v"] | 180, 0, 255));
  } else if (!strcmp(cmd, "brightness")) {
    brightness = constrain(doc["v"] | 128, 1, 255);
    if (screenOn) M5.Display.setBrightness(brightness);
  } else if (!strcmp(cmd, "screen")) {
    if (doc["on"] | true) activity();
    else setScreen(false);
  } else if (!strcmp(cmd, "i2c_read")) {
    // 调试用: 读内部 I2C 上任意芯片的一段寄存器, 不用重刷固件就能摸索新芯片
    uint8_t addr = doc["addr"] | 0, reg = doc["reg"] | 0, n = constrain(doc["n"] | 1, 1, 64);
    uint32_t freq = doc["freq"] | 100000;
    uint8_t data[64];
    JsonDocument r;
    r["evt"] = "i2c";
    r["addr"] = addr;
    r["reg"] = reg;
    if (M5.In_I2C.readRegister(addr, reg, data, n, freq)) {
      JsonArray a = r["data"].to<JsonArray>();
      for (int i = 0; i < n; i++) a.add(data[i]);
    } else {
      r["error"] = "nack";
    }
    sendEvent(r);
  } else if (!strcmp(cmd, "i2c_write")) {
    bool ok = M5.In_I2C.writeRegister8(doc["addr"] | 0, doc["reg"] | 0, doc["val"] | 0, doc["freq"] | 100000);
    JsonDocument r;
    r["evt"] = "i2c";
    r["write_ok"] = ok;
    sendEvent(r);
  } else if (!strcmp(cmd, "config")) {
    if (doc["auto_react"].is<bool>()) autoReact = doc["auto_react"];
    if (doc["sleep_sec"].is<int>()) sleepAfterMs = (uint32_t)max(0, doc["sleep_sec"].as<int>()) * 1000;
  } else {
    sendSimpleEvent("error", "msg", "unknown cmd");
  }
}

static void handleFrame(uint8_t type, const uint8_t* p, size_t len) {
  switch (type) {
    case 1: handleJson(p, len); break;
    case 2: activity(); showImage(p, len); break;
    case 3: activity(); handleAudio(p, len); break;
    case 4: showImage(p, len); break;
  }
}

// ---------------------------------------------------------------------------
// 帧解析: 每个通道一个解析任务, 收齐的帧放进队列交给主循环处理。
// 接收与处理分开, 画图/解码再慢也不会让串口缓冲溢出丢字节。
// ---------------------------------------------------------------------------
struct Frame { uint8_t type; uint8_t* data; uint32_t len; };
static QueueHandle_t frameQueue;

static void sendDiag() {
  JsonDocument r;
  r["evt"] = "diag";
  r["stage"] = (const char*)loopStage;
  r["loop_age_ms"] = millis() - loopTick;
  r["queued"] = uxQueueMessagesWaiting(frameQueue);
  r["speaker_playing"] = M5.Speaker.isPlaying();
  r["heap"] = ESP.getFreeHeap();
  r["psram"] = ESP.getFreePsram();
  sendEvent(r);
}

class FrameParser {
 public:
  void feed(const uint8_t* data, size_t n) {
    lastByteMs_ = millis();
    for (size_t i = 0; i < n; i++) {
      uint8_t b = data[i];
      switch (state_) {
        case 0: state_ = (b == 0xA5) ? 1 : 0; break;
        case 1: state_ = (b == 0x5A) ? 2 : (b == 0xA5 ? 1 : 0); break;
        case 2: type_ = b; hdrPos_ = 0; len_ = 0; state_ = 3; break;
        case 3:
          len_ |= (uint32_t)b << (8 * hdrPos_);
          if (++hdrPos_ == 4) {
            if (len_ > MAX_FRAME) { state_ = 0; break; }
            buf_ = (uint8_t*)ps_malloc(std::max<uint32_t>(len_, 1));
            if (!buf_) { state_ = 0; break; }
            pos_ = 0;
            sum_ = 0;
            state_ = len_ ? 4 : 5;
          }
          break;
        case 4: {
          // 大块数据整段拷贝, 不逐字节走状态机
          size_t take = std::min<size_t>(len_ - pos_, n - i);
          memcpy(buf_ + pos_, data + i, take);
          for (size_t k = 0; k < take; k++) sum_ += data[i + k];
          pos_ += take;
          i += take - 1;
          if (pos_ == len_) state_ = 5;
          break;
        }
        case 5:
          if (b == sum_ && type_ == 9) {
            free(buf_);
            sendDiag();
          } else if (b == sum_) {
            Frame f{type_, buf_, len_};
            rxStage = "queue_full";
            xQueueSend(frameQueue, &f, portMAX_DELAY);
            rxStage = "read";
          } else {
            free(buf_);
            sendSimpleEvent("error", "msg", "checksum");
          }
          buf_ = nullptr;
          state_ = 0;
          break;
      }
    }
  }

  // 半截帧超过 1s 没有后续数据就丢弃, 防止丢字节后把后续指令全吞掉
  void checkTimeout() {
    if (state_ != 0 && millis() - lastByteMs_ > 1000) {
      free(buf_);
      buf_ = nullptr;
      state_ = 0;
      sendSimpleEvent("error", "msg", "frame timeout");
    }
  }

 private:
  int state_ = 0, hdrPos_ = 0;
  uint8_t type_ = 0, sum_ = 0;
  uint32_t len_ = 0, pos_ = 0, lastByteMs_ = 0;
  uint8_t* buf_ = nullptr;
};

static FrameParser serialParser, bleParser;

static void watchdogTask(void*) {
  while (true) {
    delay(500);
    if (millis() - loopTick > 5000) {
      snprintf(hangInfo, sizeof(hangInfo), "loop=%s tx=%s rx=%s q=%u",
               (const char*)loopStage, (const char*)txStage, (const char*)rxStage,
               (unsigned)uxQueueMessagesWaiting(frameQueue));
      hangMagic = HANG_MAGIC;
      esp_restart();
    }
  }
}

static void serialRxTask(void*) {
  static uint8_t buf[4096];
  while (true) {
    size_t n = Serial.available();
    if (n) {
      serialParser.feed(buf, Serial.read(buf, std::min(n, sizeof(buf))));
    } else {
      serialParser.checkTimeout();
      delay(1);
    }
  }
}

// ---------------------------------------------------------------------------
// BLE (Nordic UART Service): RX 写入 -> 流缓冲 -> 解析任务
// ---------------------------------------------------------------------------
#define NUS_SERVICE "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_RX      "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define NUS_TX      "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, ble_gap_conn_desc* desc) override {
    bleConnected = true;
    // 连接间隔尽量短, 传图传音频快一些 (单位 1.25ms)
    s->updateConnParams(desc->conn_handle, 6, 12, 0, 400);
  }
  void onDisconnect(NimBLEServer*) override {
    bleConnected = false;
    bleMtu = 23;
    NimBLEDevice::startAdvertising();
  }
  void onMTUChange(uint16_t mtu, ble_gap_conn_desc*) override { bleMtu = mtu; }
};

static void bleRxTask(void*) {
  static uint8_t buf[4096];
  while (true) {
    size_t n = xStreamBufferReceive(bleRx, buf, sizeof(buf), pdMS_TO_TICKS(200));
    if (n) bleParser.feed(buf, n);
    else bleParser.checkTimeout();
  }
}

class RxCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c) override {
    std::string v = c->getValue();
    // 解析任务来不及消费时阻塞 BLE 任务, 形成反压而不是丢数据
    xStreamBufferSend(bleRx, v.data(), v.size(), pdMS_TO_TICKS(2000));
  }
};

static void bleInit() {
  bleRx = xStreamBufferCreate(32 * 1024, 1);
  xTaskCreatePinnedToCore(bleRxTask, "ble_rx", 4096, nullptr, 4, nullptr, PRO_CPU_NUM);
  NimBLEDevice::init("StackChan");
  NimBLEDevice::setMTU(517);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEServer* server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCB());
  NimBLEService* svc = server->createService(NUS_SERVICE);
  NimBLECharacteristic* rx = svc->createCharacteristic(NUS_RX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  rx->setCallbacks(new RxCB());
  bleTx = svc->createCharacteristic(NUS_TX, NIMBLE_PROPERTY::NOTIFY);
  svc->start();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE);
  adv->setScanResponse(true);
  adv->start();
}

// ---------------------------------------------------------------------------
// 摸头 / 屏幕触摸 事件
//   touch: tap(短按松开) / hold(按住 1s, 每次按下只报一次) / swipe_02, swipe_20(从 0 区划到 2 区 / 反向)
// ---------------------------------------------------------------------------
static void pollTouch() {
  // 芯片输出是一阵一阵的, 按住不动也会短暂掉回 0; 连续 200ms 都是 0 才算松手
  static constexpr uint32_t RELEASE_MS = 200;
  static uint32_t lastPoll = 0, downAt = 0, lastSeenAt = 0;
  static uint8_t firstZone = 0, lastZone = 0, seenZones = 0;
  static bool down = false, holdSent = false;
  if (!touchReady || millis() - lastPoll < 50) return;
  lastPoll = millis();

  int z = touchZones();
  if (z < 0) return;
  uint32_t now = millis();

  if (z) {
    lastSeenAt = now;
    lastZone = z;
    if (!down) {  // 按下
      down = true;
      downAt = now;
      holdSent = false;
      firstZone = z;
      seenZones = z;
      activity();
      if (autoReact) {
        nuzzle();
        if (!showingImage) avatar.setExpression(Expression::Happy);
      }
    } else {
      seenZones |= z;
      if (!holdSent && now - downAt > 1000 && seenZones != 0x7) {
        holdSent = true;
        sendSimpleEvent("touch", "type", "hold");
      }
    }
  } else if (down && now - lastSeenAt >= RELEASE_MS) {  // 松开
    down = false;
    const char* type = nullptr;
    // 从一端摸到另一端算划动: zone0 起 zone2 止 / 反之
    if ((firstZone & 0x1) && !(firstZone & 0x4) && (lastZone & 0x4)) type = "swipe_02";
    else if ((firstZone & 0x4) && !(firstZone & 0x1) && (lastZone & 0x1)) type = "swipe_20";
    else if (!holdSent) type = "tap";
    if (type) {
      JsonDocument doc;
      doc["evt"] = "touch";
      doc["type"] = type;
      doc["ms"] = lastSeenAt - downAt;
      doc["zones"] = seenZones;
      sendEvent(doc);
    }
    if (autoReact && !showingImage) avatar.setExpression(Expression::Neutral);
  }
}

static void pollScreen() {
  static bool swallow = false;  // 这次触摸是用来唤醒屏幕的, 松手前都不上报
  M5.update();
  auto t = M5.Touch.getDetail();
  if (t.wasPressed()) {
    if (!screenOn) swallow = true;
    activity();
  }
  if (swallow) {
    if (t.state == m5::touch_state_t::none) swallow = false;
    return;
  }
  if (t.isPressed()) lastActivityMs = millis();  // 按住/拖动期间不熄屏
  if (t.wasFlicked() || t.wasDragged()) {  // 划动: 松手时按主方向判断, 太短的不算
    int dx = t.distanceX(), dy = t.distanceY();
    const char* type = nullptr;
    if (abs(dx) >= 40 && abs(dx) > abs(dy)) type = dx < 0 ? "swipe_left" : "swipe_right";
    else if (abs(dy) >= 40) type = dy < 0 ? "swipe_up" : "swipe_down";
    if (type) {
      JsonDocument doc;
      doc["evt"] = "screen";
      doc["type"] = type;
      doc["dx"] = dx;
      doc["dy"] = dy;
      sendEvent(doc);
    }
  } else if (t.wasClicked() || t.wasHold()) {
    JsonDocument doc;
    doc["evt"] = "screen";
    doc["type"] = t.wasHold() ? "hold" : "tap";
    doc["x"] = t.x;
    doc["y"] = t.y;
    sendEvent(doc);
  }
}

// ---------------------------------------------------------------------------
void setup() {
  auto cfg = M5.config();
  cfg.serial_baudrate = 0;  // 串口自己管, M5 不要往上打印
  M5.begin(cfg);
  Serial.setRxBufferSize(32 * 1024);  // 默认 256B, 大帧会溢出丢字节
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // 没有主机读时不要阻塞

  txMutex = xSemaphoreCreateMutex();
  headMutex = xSemaphoreCreateMutex();

  M5.Speaker.setVolume(180);
  M5.Display.setBrightness(brightness);
  avatar.setSpeechFont(&fonts::efontCN_16);
  avatarInit();

  if (servoPowerOn()) {
    servo.begin(Serial1, SERVO_BAUD, SERVO_RX, SERVO_TX);
    servo.enableTorque(SERVO_YAW_ID, true);
    servo.enableTorque(SERVO_PITCH_ID, true);
    servoReady = true;
    servoMove(0, 45, 600);
  }
  touchInit();
  frameQueue = xQueueCreate(8, sizeof(Frame));
  xTaskCreatePinnedToCore(serialRxTask, "serial_rx", 4096, nullptr, 5, nullptr, PRO_CPU_NUM);
  xTaskCreatePinnedToCore(headTask, "head", 4096, nullptr, 3, nullptr, APP_CPU_NUM);
  bleInit();
  loopTick = millis();
  lastActivityMs = millis();
  xTaskCreatePinnedToCore(watchdogTask, "watchdog", 3072, nullptr, configMAX_PRIORITIES - 2, nullptr, PRO_CPU_NUM);
  JsonDocument hello;
  hello["evt"] = "hello";
  hello["fw"] = FW_VERSION;
  if (hangMagic == HANG_MAGIC) {
    hello["last_hang"] = (const char*)hangInfo;
    hangMagic = 0;
  }
  sendEvent(hello);
}

void loop() {
  loopTick = millis();
  Frame f;
  loopStage = "wait_frame";
  if (xQueueReceive(frameQueue, &f, pdMS_TO_TICKS(5))) {
    loopStage = f.type == 1 ? "json" : f.type == 2 ? "image" : "audio";
    handleFrame(f.type, f.data, f.len);
    free(f.data);
  }
  loopStage = "touch";
  pollTouch();
  loopStage = "screen";
  pollScreen();
  loopStage = "mouth";
  updateMouth();
  loopStage = "avatar";
  drawAvatar();
  loopStage = "screen_sleep";
  checkScreenSleep();
}
