#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event_loop.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "lis3dh.h"
#include "http.h"

#define LOW 0
#define HIGH 1

#define LED_PIN GPIO_NUM_5  //Blue led pin
#define INT_PIN GPIO_NUM_15 //Interrupt GPIO pin

#define ACCEL 0x19 //I2C device address
#define CTRL_REG1 0x20 //Data rate selection and X, Y, Z axis enable register
#define CTRL_REG2 0x21 //High pass filter selection
#define CTRL_REG3 0x22 //Control interrupts
#define CTRL_REG4 0x23 //BDU
#define CTRL_REG5 0x24 //FIFO enable / latch interrupts

#define INT1_CFG 0x30 //Interrupt 1 config
#define INT1_SRC 0x31 //Interrupt status - read in order to reset the latch
#define INT1_THS 0x32 //Define threshold in mg to trigger interrupt
#define INT1_DURATION 0x33 //Define duration for the interrupt to be recognised (not sure about this one)

//Read this value to set the reference values against which accel values are compared when calculating a threshold interrupt
//Also called the REFERENCE register in the datasheet
#define HP_FILTER_RESET 0x26

#define REG_X 0x28
#define REG_Y 0x2A
#define REG_Z 0x2C

#define uS_TO_S_FACTOR 1000000  /* Conversion factor for micro seconds to seconds */
#define NOTIFICATION_DELAY 360  /* Sleep for this amount of time after detecting machine on before sending a notification */

#define ACTIVE_THRESHOLD 240 //Trigger machine on after 4 minutes of vibration
#define INACTIVE_THRESHOLD 60

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR bool sendNotification = false;

volatile unsigned short blink_delay = 1000;

float getAccel(int16_t accel) {
  return (float)accel / 16000;
}

void printAccel(int vibrations) {
  accel_values accel = read_acceleration();

  printf("Read: %f, %f, %f, %d\n", getAccel(accel.x), getAccel(accel.y), getAccel(accel.z), vibrations);

  fflush(stdout);
}

void init_accelerometer()
{

  printf("Initing accel\n");

  init_i2c_device();

  vTaskDelay(1000);
  printAccel(0);

  printf("CTRL_REG1\n");
  write_reg(CTRL_REG1, 0x47); //Set data rate to 50hz. Enable X, Y and Z axes

  //1 - BDU: Block data update. This ensures that both the high and the low bytes for each 16bit represent the same sample
  //0 - BLE: Big/little endian. Set to little endian
  //00 - FS1-FS0: Full scale selection. 00 represents +-2g
  //1 - HR: High resolution mode enabled.
  //00 - ST1-ST0: Self test. Disabled
  //0 - SIM: SPI serial interface mode. Default is 0.
  printf("CTRL_REG4\n");
  write_reg(CTRL_REG4, 0x88);

  // 2 Write 09h into CTRL_REG2 // High-pass filter enabled on data and interrupt1
  printf("CTRL_REG2\n");
  write_reg(CTRL_REG2, 0x09);

  //3 Write 40h into CTRL_REG3 // Interrupt driven to INT1 pad
  printf("CTRL_REG3\n");
  write_reg(CTRL_REG3, 0x40);

  //4 Write 00h into CTRL_REG4 // FS = 2 g
  //5 Write 08h into CTRL_REG5 // Interrupt latched
  printf("CTRL_REG5\n");
  write_reg(CTRL_REG5, 0x08);

  // Threshold as a multiple of 16mg. 4 * 16 = 64mg
  printf("INT1_THS\n");
  write_reg(INT1_THS, 0x06);

  // Duration = 0 not quite sure what this does yet
  printf("INT1_DURATION\n");
  write_reg(INT1_DURATION, 0x00);

  // Read the reference register to set the reference acceleration values against which
  // we compare current values for interrupt generation
  // 8 Read HP_FILTER_RESET
  printf("HP_FILTER_RESET\n");
  read_reg(HP_FILTER_RESET);

  // 9 Write 2Ah into INT1_CFG // Configure interrupt when any of the X, Y or Z axes exceeds (rather than stay below) the threshold
  printf("INT1_CFG\n");
  write_reg(INT1_CFG, 0x2a);

  vTaskDelay(1000);
  printAccel(0);
}

void blink_task(void *pvParameter)
{
  while (1) {
    gpio_set_level(LED_PIN, HIGH);
    vTaskDelay(100);
    gpio_set_level(LED_PIN, LOW);
    vTaskDelay(blink_delay * 2);
  }
}

float battery_voltage() {
  int adc_value = adc1_get_voltage(ADC1_CHANNEL_7);
  float adc_voltage = adc_value * 3.3 / 4096;
  float battery_voltage = adc_voltage * 2;
  return battery_voltage;
}

void sleep(void *args) {
  printf("Let's wait for a bit.\n");
  vTaskDelay(15000);
  printf("MACHINE ON! Sleeping before sending notification.\n");
  fflush(stdout);
  sendNotification = true;
  //Wait for 6 minutes and then send a notification?
  esp_deep_sleep_enable_timer_wakeup(60 * uS_TO_S_FACTOR);
  esp_deep_sleep_start();
}

void check_vibrations(void *pvParameter)
{
  init_accelerometer();

  printf("Accelerometer enabled\n");

  unsigned int time_active = 0;
  unsigned int time_inactive = 0;
  while (1) {
    uint8_t interrupt_src = read_reg(INT1_SRC);
    //the 7th bit is the IA or "interrupt active" bit
    if (interrupt_src & 0x40) {
      time_active++;

      if (time_inactive > 10) {
        time_inactive -= 5;
      } else {
        time_inactive = 0;
      }

      printf("Active. A: %d\n", time_active);
      if (time_active == ACTIVE_THRESHOLD) {
        printf("MACHINE ON! Sleeping before sending notification.\n");
        fflush(stdout);
        sendNotification = true;
        //Wait for 6 minutes and then send a notification?
        esp_deep_sleep_enable_timer_wakeup(NOTIFICATION_DELAY * uS_TO_S_FACTOR);
        esp_deep_sleep_start();
      }
    } else {
      time_inactive++;
      //Decrease the time active so we account for quiet periods
      if (time_active > 0) {
        time_active--;
      }
      printf("Inactive. A: %d, IA: %d\n", time_active, time_inactive);
      if (time_inactive == INACTIVE_THRESHOLD) {
        printf("MACHINE OFF! Sleeping until next interrupt.\n");
        fflush(stdout);
        //No activity seen for a few seconds. Enable deep sleep with interrupt trigger
        esp_deep_sleep_enable_ext0_wakeup(INT_PIN, HIGH);
        esp_deep_sleep_start();
      }
    }
    vTaskDelay(1000);
  }

  vTaskDelete(NULL);
}

void print_wakeup_reason(){
  esp_deep_sleep_wakeup_cause_t wakeup_reason;

  wakeup_reason = esp_deep_sleep_get_wakeup_cause();

  switch(wakeup_reason)
  {
    case 1  : printf("Wakeup caused by external signal using RTC_IO\n"); break;
    case 2  : printf("Wakeup caused by external signal using RTC_CNTL\n"); break;
    case 3  : printf("Wakeup caused by timer\n"); break;
    case 4  : printf("Wakeup caused by touchpad\n"); break;
    case 5  : printf("Wakeup caused by ULP program\n"); break;
    default :
      printf("Wakeup was not caused by deep sleep: %d\n", wakeup_reason);
      break;
  }
  fflush(stdout);
}

static const char *TAG = "washing";

void app_main()
{
  nvs_flash_init();

  //Increment boot number and print it every reboot
  ++bootCount;
  printf("Boot number: %d\n", bootCount);

  gpio_set_direction(LED_PIN, GPIO_MODE_OUTPUT);

  //Print the wakeup reason for ESP32
  print_wakeup_reason();

  sendNotification = true;
  if (sendNotification) {
    printf("Turning on wifi\n");
    initialise_wifi();
    xTaskCreate(&http_get_task, "http_get_task", 8192, NULL, 5, NULL);
    sendNotification = false;
  } else {
    //Required for battery monitoring
    // adc1_config_width(ADC_WIDTH_12Bit);
    // adc1_config_channel_atten(ADC1_CHANNEL_7, ADC_ATTEN_11db);

    //TODO reduce stack size of blink task
    xTaskCreate(&blink_task, "blink_task", 2048, NULL, 5, NULL);
    xTaskCreate(&check_vibrations, "check_vibrations", 2048, NULL, 5, NULL);
    // xTaskCreate(&sleep, "sleep", 2048, NULL, 5, NULL);
    // xTaskCreate(&interrupt_task, "interrupt_task", 2048, NULL, 5, NULL);
  }
}