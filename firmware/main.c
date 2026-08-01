#include "hardware/pio.h"
#include "i2c_multi.h"
#include "pico/stdlib.h"
#include <stdio.h>

#define I2C_ADDR 0x1F

const uint COLS[5] = {2, 3, 4, 5, 6};
const uint ROWS[7] = {7, 8, 9, 10, 11, 12, 13};

const char keymap[7][5] = {
    {'Q', 'E', 'R', 'U', 'O'},      // ROW1
    {'W', 'S', 'G', 'H', 'L'},      // ROW2
    {'\x01', 'D', 'T', 'Y', 'I'},   // ROW3 (\x01 = sym)
    {'A', 'P', '\x02', '\r', '\b'}, // ROW4 (\x02 = shift-R, \r = CR, \b = bksp)
    {'\x03', 'X', 'V', 'B', '$'},   // ROW5 (\x03 = alt)
    {' ', 'Z', 'C', 'N', 'M'},      // ROW6
    {'\x04', '\x05', 'F', 'J', 'K'}, // ROW7 (\x04 = mic, \x05 = shift-L)
};

static uint8_t last_keycode = 0;
static uint8_t i2c_buffer[1] = {0};

void i2c_request_handler(uint8_t address) {
  printf("I2C Request Handler: address=0x%02X, keycode=0x%02X\n", address,
         last_keycode);

  i2c_buffer[0] = last_keycode;
  last_keycode = 0;
}

int main(void) {
  stdio_init_all();
  sleep_ms(2000);
  printf("Starting Q10 Keyboard Firmware (CANARY)...\n");

  for (int i = 0; i < 7; i++) {
    gpio_init(ROWS[i]);
    gpio_set_dir(ROWS[i], GPIO_IN);
    gpio_pull_up(ROWS[i]);
  }

  for (int i = 0; i < 5; i++) {
    gpio_init(COLS[i]);
    gpio_set_dir(COLS[i], GPIO_IN);
    gpio_disable_pulls(COLS[i]);
  }

  // initialize PIO-based I2C slave with swapped pins (SDA=1, SCL=0) due to a
  // hardware bug
  gpio_pull_up(0);
  gpio_pull_up(1);
  i2c_multi_init(pio0, 1, 0);
  i2c_multi_enable_address(I2C_ADDR);
  i2c_multi_set_request_handler(i2c_request_handler);
  i2c_multi_set_write_buffer(i2c_buffer);

  while (true) {
    for (int c = 0; c < 5; c++) {

      gpio_set_dir(COLS[c], GPIO_OUT);
      gpio_put(COLS[c], 0);
      sleep_us(10); // allow signal to settle

      for (int r = 0; r < 7; r++) {
        if (!gpio_get(ROWS[r])) { // active low = pressed
          char pressed = keymap[r][c];

          last_keycode = (uint8_t)pressed;

          printf("CANARY: Key pressed: '%c' (Col GPIO%d, Row GPIO%d)\n", pressed,
                 COLS[c], ROWS[r]);

          while (!gpio_get(ROWS[r]))
            ; // wait for release

          sleep_ms(10); // debounce
        }
      }

      gpio_set_dir(COLS[c], GPIO_IN);
      gpio_disable_pulls(COLS[c]);
    }

    sleep_ms(10);
  }
}
