/*
epdNtpClockV1.ino
Copyright (C) 2024-2025 desiFish

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include <SPI.h>
#include "src/EPD_3in52.h"
#include "src/imagedata.h"
#include "src/epdpaint.h"

#include <Wire.h>
#include "RTClib.h"
#include "driver/rtc_io.h" // RTC GPIO library for deep sleep external wakeup

#include <WiFi.h>

#include <NTPClient.h>
#include <WiFiUdp.h>

#include <Preferences.h>
#include <BH1750.h>

// powersave wifi off
#include "esp_wifi.h"

// Define the DS3231 Interrupt pin (will wake-up the ESP32 - must be an RTC GPIO - check Readme for references)
#define BUTTON_PIN_BITMASK(GPIO7) (1ULL << GPIO7) // 2 ^ GPIO_NUMBER in hex
#define CLOCK_INTERRUPT_PIN GPIO_NUM_7            // GPIO 7

RTC_DATA_ATTR bool nightFlag = false; // RTC_DATA_ATTR keeps the variable in RTC memory, so it survives deep sleep
RTC_DATA_ATTR float battLevel = 0.0;  // RTC_DATA_ATTR keeps the variable in RTC memory, so it survives deep sleep

RTC_DS3231 rtc; // ds3231 object

// Set the alarm
DateTime alarm1Time = DateTime(2025, 4, 6, 13, 35, 0); // Set the alarm time (year, month, day, hour, minute, second), no need to change this, will work with any time

#define BATPIN A0 // battery voltage divider connection pin (1M Ohm voltage divider with 104 Capacitor)

// System configuration constants
#define WIFI_CONNECT_TIMEOUT 10000 // Timeout for WiFi connection attempts (ms)
#define BATTERY_LEVEL_SAMPLING 4   // Number of ADC samples for battery voltage averaging
#define LIGHT_SENSOR_TIMEOUT 1000  // Max wait for light sensor reading (ms)
#define DEBUG 0                    // Set to 1 for Serial debugging

// Battery monitoring thresholds (Volts)
#define battChangeThreshold 0.15 // Minimum voltage change to update reading (for removing fluctuations)
#define battUpperLim 3.3         // Upper limit for normal battery operation
// you may need to adjust this value based on your readings to show battery "100%"
#define battHigh 3.3 // Full battery threshold (ideally this should be the resting voltage, i.e. ~3.4V but my ESP32 reads it 3.36V)
#define battLow 2.9  // Low battery threshold

const char *ssid = "SonyBraviaX400";      // Your WiFi SSID
const char *password = "66227617975PsA#"; // Your WiFi password

WiFiUDP ntpUDP;                                         // Create a UDP instance to send and receive NTP packets
NTPClient timeClient(ntpUDP, "in.pool.ntp.org", 19800); // 19800 is offset of India, in.pool.ntp.org is close to India
// Weekday names for display
static const char daysOfTheWeek[7][10] PROGMEM = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
#define COLORED 0   // 0 is black
#define UNCOLORED 1 // 1 is white

UBYTE image[68000];
Epd epd;

byte getWiFiBars()
{
  if (WiFi.status() != WL_CONNECTED)
    return 0;

  int rssi = WiFi.RSSI();
  if (rssi >= -55)
    return 5;
  if (rssi >= -65)
    return 4;
  if (rssi >= -72)
    return 3;
  if (rssi >= -80)
    return 2;
  return 1;
}
/**
 * @brief Measures battery voltage with averaging
 * @return float Actual battery voltage in volts
 * @note Uses multiple samples to reduce noise
 */
float batteryLevel()
{
  uint32_t Vbatt = 0;
  for (int i = 0; i < BATTERY_LEVEL_SAMPLING; i++)
  {
    Vbatt = Vbatt + analogReadMilliVolts(BATPIN); // ADC with correction
    delay(10);
  }
  float Vbattf = 2 * Vbatt / BATTERY_LEVEL_SAMPLING / 1000.0; // attenuation ratio 1/2, mV --> V
  // Serial.println(Vbattf);
  return (Vbattf);
}

/**
 * @brief Enables WiFi with power-optimized settings
 * @note Includes timeout and CPU frequency management
 */
void enableWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  unsigned long startAttemptTime = millis();
  while (WiFi.status() != WL_CONNECTED &&
         millis() - startAttemptTime < WIFI_CONNECT_TIMEOUT)
  {
    delay(100);
  }

  if (WiFi.status() != WL_CONNECTED)
  {
    disableWiFi();
  }
}

/**
 * @brief Disables WiFi and other wireless interfaces to save power
 */
void disableWiFi()
{
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  esp_wifi_stop();
  btStop();
}

/**
 * @brief Updates RTC time from NTP server if necessary
 * If an update is needed and WiFi is connected, it fetches the current time
 * from an NTP server and updates the RTC.
 *
 * @return bool Returns true if the time was successfully updated, false otherwise
 * @note Requires an active WiFi connection to function
 */
bool autoTimeUpdate()
{
  enableWiFi();
  if (WiFi.status() == WL_CONNECTED)
  {
    timeClient.begin();
    if (timeClient.update() && timeClient.isTimeSet())
    {
      time_t rawtime = timeClient.getEpochTime();
      struct tm *ti = localtime(&rawtime);

      uint16_t year = ti->tm_year + 1900;
      uint8_t month = ti->tm_mon + 1;
      uint8_t day = ti->tm_mday;

      rtc.adjust(DateTime(year, month, day,
                          timeClient.getHours(),
                          timeClient.getMinutes(),
                          timeClient.getSeconds()));

      if (DEBUG)
      {
        Serial.println("RTC updated: " + String(year) + "-" +
                       String(month) + "-" + String(day));
      }
      return true;
    }
    else
      return false;
  }
  else
    return false;
}

/**
 * @brief Pads single digit numbers with leading zero
 * @param num Number to pad
 * @return String Padded number as string
 */
String padNum(int num)
{
  return (num < 10 ? "0" : "") + String(num);
}

/**
 * @brief Main setup function - runs once at startup/wake
 * @note Device enters deep sleep after completing operations
 */
void setup()
{
  if (DEBUG)
    Serial.begin(115200);
  disableWiFi(); // Initialize peripherals with power-optimized settings

  pinMode(BATPIN, INPUT);
  Wire.begin();
  Wire.setClock(400000);    // Set I2C clock speed to 400kHz
  analogReadResolution(12); // Set ADC resolution to 12 bits

  BH1750 lightMeter(0x23); // Initalize light sensor
  Preferences pref;        // preference library object
  byte errFlag = 0;        // Error flag for various error messages
  byte wifiBars = 0;       // 0 = not connected, 1-5 = signal level
  bool rtcOK = rtc.begin();

  if (!rtcOK)
  {
    if (DEBUG)
      Serial.println("Couldn't find RTC");
    errFlag |= 2;
  }
  else
  {
    // We don't need the 32K Pin, so disable it
    rtc.disable32K();
    // Set alarm 1, 2 flag to false (so alarm 1, 2 didn't happen so far)
    // if not done, this easily leads to problems, as both register aren't reset on reboot/recompile
    rtc.clearAlarm(1);
    rtc.clearAlarm(2);
    // Stop oscillating signals at SQW Pin otherwise setAlarm1 will fail
    rtc.writeSqwPinMode(DS3231_OFF);
    // Turn off alarm 2 (in case it isn't off already)
    // again, this isn't done at reboot, so a previously set alarm could easily go overlooked
    rtc.disableAlarm(2);
    // Schedule an alarm
    if (!rtc.setAlarm1(alarm1Time, DS3231_A1_Second))
    { // this mode triggers the alarm when the minutes match
      if (DEBUG)
        Serial.println("Error, alarm wasn't set!");
    }
    else
    {
      if (DEBUG)
        Serial.println("Alarm will happen at specified time");
    }
  }

  if (lightMeter.begin(BH1750::ONE_TIME_HIGH_RES_MODE))
  {
    if (DEBUG)
      Serial.println(F("BH1750 Advanced begin"));
  }
  else
  {
    if (DEBUG)
      Serial.println(F("Error initialising BH1750"));
    errFlag |= 1;
  }

  float lux = 0;
  if ((errFlag & 1) == 0)
  {
    unsigned long lightStart = millis();
    while (!lightMeter.measurementReady(false) && millis() - lightStart < LIGHT_SENSOR_TIMEOUT)
    {
      yield();
    }

    if (lightMeter.measurementReady(false))
    {
      lux = lightMeter.readLightLevel();
      if (DEBUG)
      {
        Serial.print("Light: ");
        Serial.print(lux);
        Serial.println(" lx");
      }
    }
    else
    {
      if (DEBUG)
        Serial.println(F("Light sensor timeout"));
      errFlag |= 1;
      lux = 1;
    }
  }
  else
    lux = 1; // if light sensor error, set lux to 1 so that it doesn't go to night mode
  String percentStr = "";
  if (lux == 0)
  {
    if (!nightFlag)
    { // prevents unnecessary redrawing of same thing
      nightFlag = true;
      if (epd.Init() != 0)
      {
        if (DEBUG)
          Serial.println("e-Paper init failed");
        return;
      }
      epd.Clear();
      showMsg("SLEEPING o_o");
    }
  }
  else
  {
    nightFlag = false;

    DateTime now = DateTime(2000, 1, 1, 0, 0, 0);
    bool timeNeedsUpdate = false;

    if (rtcOK)
    {
      pref.begin("database", false); // Open Preferences with namespace "database"

      if (!pref.isKey("timeNeedsUpdate")) // create key:value pairs
        pref.putBool("timeNeedsUpdate", true);
      timeNeedsUpdate = pref.getBool("timeNeedsUpdate", false);

      now = rtc.now();
      if ((now.year() == 1970) || rtc.lostPower()) // if RTC lost power or not set
        timeNeedsUpdate = true;

      // Get the current day
      byte currentDay = now.day();

      // Check if we need to update time (every 15 days)
      if (!pref.isKey("lastCheckedDay")) // create key:value pairs
        pref.putUChar("lastCheckedDay", 0);
      byte lastCheckedDay = pref.getUChar("lastCheckedDay", 0);
      byte daysPassed = (currentDay - lastCheckedDay + 31) % 31;

      if ((daysPassed >= 15) || timeNeedsUpdate) // check if 15 days passed or force update
      {
        if (DEBUG)
          Serial.println("Updating time from NTP server");
        bool timeUpdated = autoTimeUpdate(); // Update time from NTP server
        wifiBars = getWiFiBars();
        if (timeUpdated)
        {
          if (DEBUG)
            Serial.println("Time updated");
          timeNeedsUpdate = false;
        }
        else
        {
          if (DEBUG)
            Serial.println("Time Not updated");
        }
        disableWiFi(); // Turn off WiFi to save power
        pref.putBool("timeNeedsUpdate", timeNeedsUpdate);
        pref.putUChar("lastCheckedDay", currentDay); // Update last checked day
      }
      else
      {
        if (DEBUG)
          Serial.println("Time already updated");
      }

      pref.end();      // Close the preferences
      now = rtc.now(); // Get the current time again after potential update
    }

    // Battery level handling
    float newBattLevel = batteryLevel();
    battLevel = (newBattLevel < battLevel) ? newBattLevel : ((newBattLevel - battLevel) >= battChangeThreshold || newBattLevel > battUpperLim) ? newBattLevel
                                                                                                                                               : battLevel; // Update battLevel if it has changed significantly or is above upper limit
    byte percent = constrain(((battLevel - battLow) / (battHigh - battLow)) * 100, 0, 100);                                                                 // Calculate percentage based on battHigh and battLow

    if (battLevel >= 3.7) // LiFePO4 battery has max voltage of 3.6V anything larger than this means external power
      percentStr = "USB";
    else
      percentStr = String(percent) + "%";

    String dateString = "--/--/----";
    String timeString = "--:--";
    String tempString = "--";
    String week = "CLOCK";
    String notificationMsg = "";

    if (rtcOK)
    {
      byte tempHour = now.twelveHour();      // Get the hour in 12-hour format
      byte temp = int(rtc.getTemperature()); // Get the temperature in Celsius, rounded to the nearest integer

      dateString = padNum(now.day()) + "/" + padNum(now.month()) + "/" + String(now.year());     // Format date string
      timeString = padNum(tempHour) + ":" + padNum(now.minute()) + (now.isPM() ? " PM" : " AM"); // Format time string
      tempString = String(padNum(temp));
      week = daysOfTheWeek[now.dayOfTheWeek()];
    }

    if (timeNeedsUpdate) // If time needs to be updated
      notificationMsg = "TIME SYNC ERR";

    if (percent <= 5) // Battery level is low
      notificationMsg = "BATTERY LOW";

    if (epd.Init() != 0) // Initialize e-Paper display
    {
      if (DEBUG)
        Serial.println("e-Paper init failed");
      return;
    }
    if (DEBUG)
      Serial.println("e-Paper initialized");

    if (errFlag == 1) // If there was an error with the light sensor
      notificationMsg = "LUX ERROR";
    else if (errFlag == 2) // If there was an error with the RTC
      notificationMsg = "RTC ERROR";
    else if (errFlag == 3) // If there was an error with both the light sensor and RTC
      notificationMsg = "ALL ERROR";
    epd.Clear();
    showTime(week, timeString, dateString, String(battLevel) + "V", percentStr, percent /*for battery icon*/, tempString, notificationMsg, wifiBars);
  }
  // Go to sleep now
  if (DEBUG)
  {
    Serial.println("Going to sleep now");
    Serial.flush();
  }
  // Configure external wake-up
  esp_sleep_enable_ext1_wakeup(BUTTON_PIN_BITMASK(CLOCK_INTERRUPT_PIN), ESP_EXT1_WAKEUP_ANY_LOW);
  // Configure pullup/downs via RTCIO to tie wakeup pins to inactive level during deepsleep.
  // The RTC SQW pin is active low, CHECK README for references on Deep Sleep
  rtc_gpio_pulldown_dis(CLOCK_INTERRUPT_PIN);
  rtc_gpio_pullup_en(CLOCK_INTERRUPT_PIN);
  if (percentStr == "USB") // if external power is connected, don't go to sleep
  {
    if (DEBUG)
      Serial.println("External power connected, staying awake");
  }
  else
    esp_deep_sleep_start(); // Enter deep sleep mode
}

void loop()
{
  // This will never run
}

/**
 * @brief Displays Sleep message
 * @param msg Message to display in string format
 */
void showMsg(String msg)
{
  epd.display_NUM(EPD_3IN52_WHITE);
  epd.lut_GC();
  epd.refresh();
  epd.SendCommand(0x50);
  epd.SendData(0x17);
  delay(100);

  Paint paint(image, 240, 360); // width should be the multiple of 8
  paint.SetRotate(3);           // Top right (0,0)
  paint.Clear(COLORED);
  paint.DrawStringAt(10, 100, msg.c_str(), &Font48, UNCOLORED);

  epd.display_part(paint.GetImage(), 0, 0, paint.GetWidth(), paint.GetHeight());
  epd.lut_GC();
  epd.refresh();
  if (DEBUG)
    Serial.println("sleep......");
  delay(100);
  epd.sleep();
  if (DEBUG)
    Serial.println("end showMsg");
}

/**
 * @brief Draws WiFi signal strength dots on the display, filled for connected bars and outlined for disconnected bars. If no connection, an 'x' is displayed.
 * @param paint Paint object for drawing
 * @param wifiBars Number of WiFi signal bars (0-5)
 */
void drawWiFiDots(Paint &paint, byte wifiBars)
{
  int x = 330;
  int y = 8;

  for (byte i = 0; i < 5; i++)
  {
    int dotX = x + (i * 6);
    if (wifiBars > i)
      paint.DrawFilledCircle(dotX, y, 2, COLORED);
    else
      paint.DrawCircle(dotX, y, 2, COLORED);
  }

  if (wifiBars == 0)
  {
    paint.DrawStringAt(x + 9, 3, "x", &Font12, COLORED);
  }
}

// Displays time, battery info. First para is week in const char, then time in hh:mm am/pm, then date in dd/mm/yyyy, then battlevel in X.YZV, percent in XY%
/**
 * @brief Updates display with time and status information
 * @param w Weekday string
 * @param timeString Formatted time string
 * @param dateString Formatted date string
 * @param battLevelS Battery voltage string
 * @param percentStr Battery percentage string
 * @param percent Battery percentage value for icon
 * @param temp Temperature string
 * @param notificationMsg Message shown in the notification area
 * @param wifiBars WiFi signal level, 0 means disconnected
 * @note Implements power-efficient display update strategy
 */
void showTime(String w, String timeString, String dateString,
              String battLevelS, String percentStr, byte percent, String temp, String notificationMsg, byte wifiBars)
{
  epd.display_NUM(EPD_3IN52_WHITE);
  epd.lut_GC();
  epd.refresh();
  epd.SendCommand(0x50);
  epd.SendData(0x17);
  delay(100);

  Paint paint(image, 240, 360); // width should be the multiple of 8
  paint.SetRotate(3);           // Top right (0,0)
  paint.Clear(UNCOLORED);

  int statusX = 10;
  if (percentStr != "USB")
  {
    // Battery icon outline
    paint.DrawRectangle(statusX + 2, 4, statusX + 18, 12, COLORED);
    paint.DrawRectangle(statusX, 6, statusX + 2, 10, COLORED);

    // Battery fill level
    byte fillX = statusX + 17; // Default to empty (rightmost position)

    if (percent >= 95)
      fillX = statusX + 3; // Full
    else if (percent >= 85)
      fillX = statusX + 5; // Full-Med
    else if (percent >= 70)
      fillX = statusX + 7; // Med
    else if (percent >= 50)
      fillX = statusX + 9; // Med-half
    else if (percent >= 30)
      fillX = statusX + 11; // Half
    else if (percent >= 10)
      fillX = statusX + 13; // Low-half
    else if (percent >= 5)
      fillX = statusX + 15; // Low
    else if (percent >= 0)
      paint.DrawStringAt(statusX + 7, 2, "x", &Font12, COLORED); // Critical

    paint.DrawFilledRectangle(fillX, 4, statusX + 17, 11, COLORED);
    statusX += 28;
  }

  paint.DrawStringAt(statusX, 4, percentStr.c_str(), &Font12, COLORED);
  statusX += (percentStr.length() * Font12.Width) + 8;
  paint.DrawStringAt(statusX, 4, battLevelS.c_str(), &Font12, COLORED);
  drawWiFiDots(paint, wifiBars);
  if (notificationMsg.length() > 0)
  {
    int notificationX = (360 - (notificationMsg.length() * Font12.Width)) / 2;
    paint.DrawStringAt(notificationX, 5, notificationMsg.c_str(), &Font12, COLORED);
  }
  int stringWidth = w.length() * Font48.Width;
  int newStartPos = (360 - stringWidth) / 2; // Center position calculation
  paint.DrawStringAt(newStartPos, 30, w.c_str(), &Font48, COLORED);

  paint.DrawStringAt(20, 100, timeString.c_str(), &Font48, COLORED);
  paint.DrawStringAt(270, 100, temp.c_str(), &Font48, COLORED);
  paint.DrawStringAt(316, 91, "o", &Font24, COLORED);
  paint.DrawStringAt(325, 100, "C", &Font48, COLORED);

  stringWidth = dateString.length() * Font48.Width;
  newStartPos = (360 - stringWidth) / 2; // Center position calculation
  paint.DrawStringAt(newStartPos, 170, dateString.c_str(), &Font48, COLORED);

  epd.display_part(paint.GetImage(), 0, 0, paint.GetWidth(), paint.GetHeight());
  epd.lut_GC();
  epd.refresh();
  if (DEBUG)
    Serial.println("sleep......");
  delay(100);
  epd.sleep();
  if (DEBUG)
    Serial.println("end");
}
