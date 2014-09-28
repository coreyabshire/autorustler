#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <algorithm>

#include "car/radio.h"
#include "gpio/gpio.h"
#include "gps/sirf.h"
#include "imu/imu.h"
#include "lcd/lcd.h"
#include "ui/uistate.h"

volatile UIState uistate;

void handle_sigint(int signo) { uistate.done = true; }

const int BUTTON_RED = 24;     // GPIO24 -> red button
const int BUTTON_BLACK = 23;   // GPIO23 -> black button

extern void* GPSThread(void* data);
extern void* IMUThread(void* data);
extern bool StartCamera();

void InitButtons() {
  INP_GPIO(BUTTON_RED);
  INP_GPIO(BUTTON_BLACK);
  GPIO_PULL = 2;
  usleep(10);
  // clock on GPIO 23 & 24 (bit 23 & 24 set)
  GPIO_PULLCLK0 = (1 << BUTTON_RED) | (1 << BUTTON_BLACK);
  usleep(10);
  GPIO_PULL = 0;
  GPIO_PULLCLK0 = 0;
}

#if 1
const uint8_t dither_matrix[8][8] = {
  {  4, 192,  51, 239,  16, 204,  63, 251},
  {129,  67, 177, 114, 141,  78, 188, 126},
  { 35, 224,  20, 208,  47, 235,  31, 220},
  {161,  98, 145,  82, 173, 110, 157,  94},
  { 12, 200,  59, 247,   8, 196,  55, 243},
  {137,  75, 184, 122, 133,  71, 180, 118},
  { 43, 231,  27, 216,  39, 228,  24, 212},
  {169, 106, 153,  90, 165, 102, 149,  86}
};
#else  // gamma-corrected? this is way too dark
const uint8_t dither_matrix[8][8] = {
  { 38, 224, 123, 248,  72, 230, 135, 253},
  {187, 139, 216, 177, 195, 149, 222, 185},
  {104, 240,  79, 232, 118, 246,  98, 238},
  {207, 165, 197, 153, 214, 174, 205, 162},
  { 63, 228, 131, 251,  52, 226, 127, 250},
  {192, 146, 220, 182, 190, 142, 218, 179},
  {114, 244,  93, 236, 109, 242,  86, 234},
  {211, 171, 202, 159, 209, 168, 200, 156}
};
#endif


int main() {
  memset(const_cast<UIState*>(&uistate), 0, sizeof(uistate));

  if (!gpio_init()) {
    fprintf(stderr, "gpio init fail\n");
    return 1;
  }

  InitButtons();

  LCD lcd;
  if (!lcd.Init()) {
    fprintf(stderr, "lcd init fail\n");
    return 1;
  }

  int i2c_fd = open("/dev/i2c-1", O_RDWR);
  if (i2c_fd == -1) {
    perror("/dev/i2c-1");
    return 1;
  }

  // TODO: launch IMU/radio thread

  int gps_fd = sirf_open();
  if (gps_fd == -1) {
    fprintf(stderr, "GPS serial port init fail\n");
    return 1;
  }
  pthread_t gps_thread;
  pthread_create(&gps_thread, NULL, GPSThread,
                 reinterpret_cast<void*>(gps_fd));

  pthread_t imu_thread;
  pthread_create(&imu_thread, NULL, IMUThread,
                 reinterpret_cast<void*>(i2c_fd));

  if (!StartCamera()) {
    fprintf(stderr, "cam init fail\n");
    return 1;
  }

  signal(SIGINT, handle_sigint);
  signal(SIGTERM, handle_sigint);

  uint8_t screen[84*6];

  char buf[84];
  int frameno = 0;
  while (!uistate.done) {
    memset(screen, 0, sizeof(screen));

    frameno++;
    int dither_xor = (frameno&1) << 1;
    // draw camera frame
    for (int y = 0; y < 48; y += 8) {
      int idxout = (y>>3)*84;
      int idxin = y*64;
      for (int x = 0; x < 64; x++) {
        uint8_t pxl = 0;
        for (int i = 0; i < 8; i++) {
          if (uistate.cam_preview[idxin + 64*i] <=
              dither_matrix[i ^ dither_xor][x&7]) {
            pxl |= (1 << i);
          }
        }
        idxin++;
        screen[idxout++] = pxl;
      }
    }

    {
      // bottom right: battery voltage
      snprintf(buf, sizeof(buf), "%0.2fV", uistate.vbat);
      LCD::WriteString(53, 40, buf, screen);
    }

    {
      // first line: button / recording state
      if (GET_GPIO(BUTTON_BLACK) == 0) {
        screen[65] = 0x1c;
        screen[66] = 0x3e;
        screen[67] = 0x3e;
        screen[68] = 0x3e;
        screen[69] = 0x1c;
      }
      if (GET_GPIO(BUTTON_RED) == 0) {
        screen[71] = 0x1c;
        screen[72] = 0x3e;
        screen[73] = 0x3e;
        screen[74] = 0x3e;
        screen[75] = 0x1c;
      }
    }

    {
      // 2nd line: throttle and steering
      int thr = std::max(0, (uistate.rc_state.throttle - 70)/5);
      int str = std::max(0, (uistate.rc_state.steering - 70)/5);
      if (thr > 10) {
        for (int i = 10; i <= thr; i++)
          screen[84 + 64 + i] |= 0x03;
      } else {
        for (int i = 10; i >= thr; i--)
          screen[84 + 64 + i] |= 0x03;
      }
      screen[84 + 83 - str] |= 0x0c;
      screen[84 + 82 - str] |= 0x0c;
    }

    {
      // rest of 2nd line and 1/2 of 3rd line: gyro axes
      int gx = std::min(19, std::max(0, 10 + uistate.imu_state.gyro_x / 200));
      int gy = std::min(19, std::max(0, 10 + uistate.imu_state.gyro_y / 200));
      int gz = std::min(19, std::max(0, 10 + uistate.imu_state.gyro_z / 200));
      screen[84 + 63 + gx] |= 0xc0;
      screen[84 + 64 + gx] |= 0xc0;
      screen[2*84 + 63 + gy] |= 0x03;
      screen[2*84 + 64 + gy] |= 0x03;
      screen[2*84 + 63 + gz] |= 0x0c;
      screen[2*84 + 64 + gz] |= 0x0c;
    }

    {
      // 4th line: circle indicators for compass and acceleration
      // TODO
    }

    {
      // 6th line: gps + compass (arrow indicating home direction / north?)
      // HDOP / SVs?
      // GPS icon at upper right?
      // yeah!

      // SVs: 5..6 = 1 bar, 7 2 bar, 9+ 3 bars?
      //     x
      //   x x
      // x x x
      // x x x
      if (uistate.gps_SVs >= 5) screen[79] |= 0x0c;
      if (uistate.gps_SVs >= 7) screen[81] |= 0x0e;
      if (uistate.gps_SVs >= 9) screen[83] |= 0x0f;
    }

    lcd.GotoXY(0, 0);
    lcd.Draw(screen, 84*6);
    usleep(50000);
  }

  memset(screen, 0, sizeof(screen));
  lcd.GotoXY(0, 0);
  lcd.Draw(screen, 84*6);

  return 0;
}
