#pragma once

#include <M5StickCPlus2.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t nvsMutex;
extern SemaphoreHandle_t tamaMutex;


// Typedefs for display compatibility
#define TFT_eSPI LovyanGFX
#define TFT_eSprite LGFX_Sprite

// RTC structures compatibility
struct RTC_TimeTypeDef {
  uint8_t Hours;
  uint8_t Minutes;
  uint8_t Seconds;
};

struct RTC_DateTypeDef {
  uint8_t WeekDay;
  uint8_t Month;
  uint8_t Date;
  uint16_t Year;
};

class RTC_Compat {
public:
  void GetTime(RTC_TimeTypeDef* time) {
    auto t = ::M5.Rtc.getTime();
    time->Hours = t.hours;
    time->Minutes = t.minutes;
    time->Seconds = t.seconds;
  }
  void GetDate(RTC_DateTypeDef* date) {
    auto dt = ::M5.Rtc.getDateTime();
    date->WeekDay = dt.date.weekDay;
    date->Month = dt.date.month;
    date->Date = dt.date.date;
    date->Year = dt.date.year;
  }
  void SetTime(RTC_TimeTypeDef* time) {
    auto dt = ::M5.Rtc.getDateTime();
    ::M5.Rtc.setDateTime( { { dt.date.year, dt.date.month, dt.date.date, dt.date.weekDay },
                            { (int8_t)time->Hours, (int8_t)time->Minutes, (int8_t)time->Seconds } } );
  }
  void SetDate(RTC_DateTypeDef* date) {
    auto dt = ::M5.Rtc.getDateTime();
    ::M5.Rtc.setDateTime( { { (int16_t)date->Year, (int8_t)date->Month, (int8_t)date->Date, (int8_t)date->WeekDay },
                            { dt.time.hours, dt.time.minutes, dt.time.seconds } } );
  }
};

class IMU_Compat {
public:
  int Init() {
    return ::M5.Imu.init();
  }
  bool getAccelData(float* ax, float* ay, float* az) {
    return ::M5.Imu.getAccelData(ax, ay, az);
  }
};

class Beep_Compat {
public:
  void begin() {}
  void update() {}
  void tone(uint16_t freq, uint16_t dur) {
    ::M5.Speaker.tone(freq, dur);
  }
};

class AXP192_Compat {
public:
  void ScreenBreath(uint8_t brightness) {
    uint8_t val = (uint8_t)(brightness * 2.55f);
    if (val < 4) val = 4;  // avoid complete black-out, SetLDO2 handles off
    ::M5.Display.setBrightness(val);
  }
  void SetLDO2(bool state) {
    if (state) {
      // Handled by subsequent ScreenBreath calls
    } else {
      ::M5.Display.setBrightness(0);
    }
  }
  void PowerOff() {
    ::M5.Power.powerOff();
  }
  float GetBatVoltage() {
    return (float)::M5.Power.getBatteryVoltage() / 1000.0f;
  }
  float GetBatCurrent() {
    return (float)::M5.Power.getBatteryCurrent();
  }
  int GetBatLevel() {
    return (int)::M5.Power.getBatteryLevel();
  }
  float GetVBusVoltage() {
    auto t = ::M5.Power.getType();
    if (t == m5::Power_Class::pmic_axp2101) return ::M5.Power.Axp2101.getVBUSVoltage();
    if (t == m5::Power_Class::pmic_axp192)  return ::M5.Power.Axp192.getVBUSVoltage();
    auto chg = ::M5.Power.isCharging();
    if (chg == m5::Power_Class::is_charging) return 5.0f;
    float vbat = (float)::M5.Power.getBatteryVoltage() / 1000.0f;
    return (vbat > 4.1f) ? 5.0f : 0.0f;
  }
  float GetTempInAXP192() {
    auto t = ::M5.Power.getType();
    if (t == m5::Power_Class::pmic_axp2101) return ::M5.Power.Axp2101.getInternalTemperature();
    if (t == m5::Power_Class::pmic_axp192)  return ::M5.Power.Axp192.getInternalTemperature();
    return -999.0f;
  }
  uint8_t GetBtnPress() {
    if (::M5.BtnPWR.wasClicked()) {
      return 0x02;
    }
    return 0;
  }
};

class M5StickCPlus_Compat {
public:
  M5GFX &Lcd = ::M5.Display;
  IMU_Compat Imu;
  RTC_Compat Rtc;
  m5::Button_Class &BtnA = ::M5.BtnA;
  m5::Button_Class &BtnB = ::M5.BtnB;
  m5::Mic_Class     &Mic  = ::M5.Mic;
  
  AXP192_Compat Axp;
  Beep_Compat Beep;

  void begin() {
    Serial.begin(115200);
    delay(10);
    Serial.println("M5StickCPlus_Compat: beginning...");
    auto cfg = ::M5.config();
    ::M5.begin(cfg);
    ::M5.Display.setBrightness(128);
  }

  void update() {
    ::M5.update();
  }
};

inline M5StickCPlus_Compat M5_compat;
#define M5 M5_compat

