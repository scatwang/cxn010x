#include <Arduino.h>
#include <Wire.h>
#include <map>

#include "HexDump.h"
#include "cxn010x.h"

#define I2C_SONY_CXNProjector   0x77

#define INRANGE(val,l,h) (std::min((int8_t)h, std::max((int8_t)l, (int8_t)val)))

// 部分参考
// https://learn.adafruit.com/adafruit-seesaw-atsamd09-breakout/reading-and-writing-data

bool CXN_Send_Command(uint8_t * cmd, uint8_t sz) {
  Wire.beginTransmission(I2C_SONY_CXNProjector);
  Wire.write(cmd, sz);
  Serial.println("send command:");
  HexDump(Serial, cmd, sz);
  return 0 == Wire.endTransmission();
}

bool CXN_Send_Command(uint8_t cmdCode) {
  uint8_t cmd[] = {cmdCode, 0x00};
  return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
}

CXNProjector::CXNProjector() : stat(STATE_POWER_OFF) {
  m_HueU = m_HueV = m_SaturationU = m_SaturationV = m_Sharpness = m_Brightness = m_Contrast = 0;
}

CXNProjector::~CXNProjector() {}

// 当 CMD_REQ 引脚 = 1 读取通知
void CXNProjector::OnNotify() {
  int num = this->ReadNotify(data, 32);
  if (num == 0) {
    return;
  }
  waitForNotify = false;

  switch (data[0]) {
    case 0x00:
      this->OnBootNotify(&data[0], num);
      break;
    case 0x01:  //开启输入
      if (data[1] == 0x01 && data[2] == 0x00) {
        this->stat = STATE_ACTIVE;
      }
      break;
    case 0x02: // 关闭输入
      if (data[1] == 0x01 && data[2] == 0x00) { //停止输入成功.
        if (stat == STATE_BOOT_READY_OFF)
          this->Shutdown(false);
        else if (stat == STATE_BOOT_READY_REBOOT) {
          this->Shutdown(true);
        } else {
          stat = STATE_READY;
        }
      }
      break;
    case 0x0B:
      if (data[1] == 0x01 && data[2] == 0x00) {
        // 正常关机 或者重启.
        this->PowerOff();
      }
      break;
    case 0x10:  //系统检测到异常. 紧急停机 并发送此通知.
      switch (data[2]) {
        case 0x80:  // 检测到激光安全模块异常, 执行紧急停机.
          break;
        case 0x81:  // 固件内部发生错误, 执行紧急停机.
          break;
        case 0x82:  // 激光异常, 紧急停机.
          break;
        case 0x83: // UnderFlow occurred and recovery processing performed. 如果恢复没有问题,将继续.
          break;
      }
      break;
    case 0x11:  // 温度紧急通知, 温度异常恢复通知
      if (data[1] == 0x01 && data[2] == 0x80) {
        stat = STATE_MUTE;
      } else if (data[1] == 0x01 && data[2] == 0x00) {
        stat = STATE_ACTIVE;
      }
      break;
    case 0x12: //命令处理异常发送此通知.
      //命令发送速度过快 等...
      break;
    case 0x27: // GetOpticalAlignment response
      if (0x0E == data[1] && 0x00 == data[2]) {
        memcpy((void*)(&opticalAlignment), data + 3, sizeof(opticalAlignment));
      }
      break;
    case 0x40: // 获取所有图像质量信息.
      if (0x0A == data[1] && 0x00 == data[2]) { //数据长度 10 字节
        m_Contrast    = (int8_t)data[3];  //OP2
        m_Brightness  = (int8_t)data[4];  //OP3
        m_HueU        = (int8_t)data[5];
        m_HueV        = (int8_t)data[6];
        m_SaturationU = (int8_t)data[7];
        m_SaturationV = (int8_t)data[8];
        m_Sharpness   = (int8_t)data[10];
      }
      break;
    case 0xA0: // temperature
      (*getTempCallback)((int8_t)data[3]);
      temperature = (uint8_t)data[3];
      break;
    case 0xA1: // cumulativeOperatingTime
      memcpy(&cumulativeOperatingTime, data + 3, 4);
      Serial.println(cumulativeOperatingTime);
      break; 
    case 0xCA: //获取故障信息通知结果.
      break;
    case 0xCB: //清除故障信息通知结果.
      break;
  }
}

// 开机状态,TODO: 处理引导通知,如果有异常发生,清除异常.
void CXNProjector::OnBootNotify(uint8_t * data, int num) {
  if (0x00 == data[0]) {
    switch (data[2]) {
      case 0x00:
        stat = STATE_READY;
        this->StartInput();
        return;
      case 0x80:  // 发生内部故障,不能工作.
        break;
      case 0x81: case 0x82: case 0x83: case 0x84: // 内部故障.
        break;
      default:
        break;
    }
    this->GetTroubleInfo();
  }
}

void CXNProjector::PowerOn() {
  if (STATE_POWER_OFF == stat) {
    stat = STATE_POWER_ON;
    digitalWrite(CXNProjector_POWER_PIN, HIGH);
  }
}
void CXNProjector::PowerOff() {
  if (stat == STATE_BOOT_READY_OFF) {
    digitalWrite(CXNProjector_POWER_PIN, LOW);  //断开光机电源
    stat = STATE_POWER_OFF;
  }
}

bool CXNProjector::StartInput() {
  //if(STATE_ACTIVE != stat){
  return CXN_Send_Command(0x01);
  //}
  //return false;
}

bool CXNProjector::StopInput() {
  return CXN_Send_Command(0x02);
}

bool CXNProjector::Shutdown(bool isReboot) {
  if (stat == STATE_ACTIVE) {
    stat = isReboot ? STATE_BOOT_READY_REBOOT : STATE_BOOT_READY_OFF;
    return this->StopInput();
  } else {
    uint8_t cmd[] = {0x0B, 0x01, 0x00};
    cmd[2] = isReboot ? 0x01 : 0x00;
    return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
  }
}

bool CXNProjector::GetTroubleInfo() {
  uint8_t cmd[] = {0xCA, 0x05, 0x01, 0x24, 0x10, 0x06, 0x00};
  return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
}


bool CXNProjector::ClearTroubleInfo() {
  uint8_t cmd [] = {0xCB, 0x05, 0x01, 0x24, 0x10, 0x10, 0x06, 0x00, 0x00};
  return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
}


bool CXNProjector::GetOpticalAlignment() {
  return CXN_Send_Command(0x27);
}

bool CXNProjector::SetOpticalAlignment(int8_t * data, size_t size) {
  uint8_t cmd[32] = {0x28, 0x0D};
  memcpy(cmd + 2, data, size);
  return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
}

bool CXNProjector::GetBiphase() {
  return CXN_Send_Command(0x29);
}


bool CXNProjector::GetTemperature(void (*callback)(uint8_t)) {
  if (callback != NULL) {
    Serial.println("store callback");
    //callbacks[0xA0] = callback;
    getTempCallback = callback;
    //Serial.println((int)callback);
  }
  return CXN_Send_Command(0xA0);
}

uint8_t CXNProjector::GetTemperatureValue() {
  return temperature;
}

bool CXNProjector::GetCumulativeOperatingTime() {
  return CXN_Send_Command(0xA1);
}

unsigned int CXNProjector::GetCumulativeOperatingTimeValue() {
  return cumulativeOperatingTime;
}

//获取所有图像质量信息
bool CXNProjector::GetAllPictureQualityInfo() {
  return CXN_Send_Command(0x40);
}

bool CXNProjector::SetBrightness(int8_t val) {
  uint8_t cmd[] = {CXNProjector_CMD_SET_LIGHT, 0x01, 0x00};
  val = std::min((int8_t)31, std::max((int8_t) - 31, val));

  cmd[2] = (uint8_t)(0xFF & val);
  return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
}

//设置锐度
bool CXNProjector::SetSharpness(int8_t val) {
  uint8_t cmd[] = {0x4F, 0x01, 0x00};
  val = INRANGE(0, 6, val);
  cmd[3] = (uint8_t)(0xFF & val);
  return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
}

// 设置对比度
bool CXNProjector::SetContrast(int8_t val) {
  uint8_t cmd[] = {CXNProjector_CMD_SET_CONTRAST, 0x01, 0x00};
  val = INRANGE(-15, 15, val);
  cmd[2] = (uint8_t)(0xFF & val);
  return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
}

// 设置饱和度
bool CXNProjector::SetSaturat(int8_t val) {
  uint8_t cmd[] = {CXNProjector_CMD_SET_SATURATION, 0x02, 0x02, 0x00};
  val = INRANGE(-15, 15, val);
  cmd[3] = (uint8_t)(0xFF & val);

  return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
}

bool CXNProjector::SetVideoPosition() {
  uint8_t cmd[] = {0x26, 0x09, (uint8_t)m_Pan, (uint8_t)m_Tilt, (uint8_t)m_Flip, 0x64, 0x00, 0x00, 0x00, 0x00, 0x00};
  return 0 == CXN_Send_Command(cmd, sizeof(cmd) / sizeof(cmd[0]));
}

//图像翻转
bool CXNProjector::SetFlip(int8_t flip) {
  if (flip == 0 || flip == 1 || flip == 2 || flip == 3) {
    m_Flip = flip;
    return this->SetVideoPosition();
  }
  return false;
}

//左右梯形校正
bool CXNProjector::SetPan(int8_t pan) {
  if (pan >= -30 && pan <= 30) {
    m_Pan = pan;
    return this->SetVideoPosition();
  }
  return false;
}

// 上下梯形校正
bool CXNProjector::SetTilt(int8_t tilt) {
  if (tilt >= -20 && tilt <= 30) {
    m_Tilt = tilt;
    return this->SetVideoPosition();
  }
  return false;
}

size_t CXNProjector::ReadNotify(uint8_t * data, size_t quantity) {
  size_t size = Wire.requestFrom((uint8_t)I2C_SONY_CXNProjector, quantity, true);

  if (0 == size) {
    Serial.println("No Data");
    delay(30);
    return 0;
  } else {
    int i = 0;
    while (Wire.available()) {
      data[i++] = Wire.read();
    }
  }

  Serial.println("read notify:");
  HexDump(Serial, data, 2 + data[1]);
  return size;
}
