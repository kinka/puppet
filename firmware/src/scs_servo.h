// Feetech SCS0009 串行总线舵机的最小驱动 (Arduino HardwareSerial 版)。
// 协议: 0xFF 0xFF ID LEN INST PARAMS... CHK, CHK = ~(ID+LEN+INST+ΣPARAMS)
// 坑: 16 位寄存器在线上是大端(低地址放高字节); 出厂 Status Return Level=1,
// 写指令不应答, 所以写完不等回包。
#pragma once
#include <Arduino.h>

class ScsServo {
 public:
  void begin(HardwareSerial& port, uint32_t baud, int rx, int tx) {
    port_ = &port;
    port_->begin(baud, SERIAL_8N1, rx, tx);
  }

  void writePos(uint8_t id, uint16_t pos, uint16_t timeMs, uint16_t speed = 0) {
    if (pos > 1023) pos = 1023;
    uint8_t p[6] = {(uint8_t)(pos >> 8), (uint8_t)pos, (uint8_t)(timeMs >> 8),
                    (uint8_t)timeMs, (uint8_t)(speed >> 8), (uint8_t)speed};
    writeReg(id, 42, p, sizeof(p));
  }

  void enableTorque(uint8_t id, bool on) {
    uint8_t v = on ? 1 : 0;
    writeReg(id, 40, &v, 1);
  }

 private:
  HardwareSerial* port_ = nullptr;

  void writeReg(uint8_t id, uint8_t addr, const uint8_t* data, uint8_t n) {
    if (!port_) return;
    uint8_t buf[16];
    buf[0] = 0xFF; buf[1] = 0xFF; buf[2] = id; buf[3] = n + 3; buf[4] = 0x03; buf[5] = addr;
    memcpy(buf + 6, data, n);
    uint32_t sum = 0;
    for (int i = 2; i < 6 + n; i++) sum += buf[i];
    buf[6 + n] = ~sum;
    port_->write(buf, 7 + n);
    port_->flush();
    while (port_->available()) port_->read();  // 半双工, 丢掉自己发出去的回显
  }
};
