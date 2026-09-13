#include "hardware/i2c.h"
#include "pico/i2c_slave.h"
#include "pico/stdlib.h"
#include <stdio.h>

#define I2C_ADDR 0x1F
#define I2C_BAUDRATE (100 * 1000)
#define I2C_SDA_PIN 0
#define I2C_SCL_PIN 1

#define DEBOUNCE_MS 10

const uint COLS[5] = {2, 3, 4, 5, 6};
const uint ROWS[7] = {7, 8, 9, 10, 11, 12, 13};

// \x01 = sym, \x02 = shift-R, \x03 = alt, \x04 = mic/super, \x05 = shift-L,
// \x06 = mute, \x07 = ctrl, \x08 = host_alt, \x0a = caps_lock, \x7f = delete,
// \x11 = up, \x12 = left, \x13 = down, \x14 = right
const char keymap[7][5] = {
    {'Q', 'E', 'R', 'U', 'O'},       // ROW1
    {'W', 'S', 'G', 'H', 'L'},       // ROW2
    {'\x01', 'D', 'T', 'Y', 'I'},    // ROW3
    {'A', 'P', '\x02', '\r', '\b'},  // ROW4
    {'\x03', 'X', 'V', 'B', '\x06'}, // ROW5 (\x06 = mute, alt gives $)
    {' ', 'Z', 'C', 'N', 'M'},       // ROW6
    {'\x04', '\x05', 'F', 'J', 'K'}, // ROW7 (\x04 = mic, alt gives 0)
};

// modifiers keep working while alt is held
const char keymap_alt[7][5] = {
    {'#', '2', '3', '_', '+'},      // ROW1: Q E R U O
    {'1', '4', '/', ':', '"'},      // ROW2: W S G H L
    {'\x01', '5', '(', ')', '-'},   // ROW3: sym D T Y I
    {'*', '@', '\x02', '\r', '\b'}, // ROW4: A P shift-R CR bksp
    {'\x03', '8', '?', '!', '$'},   // ROW5: alt X V B mute
    {' ', '7', '9', '\'', '.'},     // ROW6: space Z C N M
    {'0', '\x05', '6', ';', '`'},   // ROW7: mic shift-L F J K
};

// sym layer: shortcuts for missing modifier keys, navigation,
// and special symbols. '\0' means fall through to the lower keymap
const char keymap_sym[7][5] = {
    {'\x1b', '[', ']', 0, '='},    // ROW1: Q=Esc, E=[, R=], O==
    {'\x11', '\x13', '}', '^', 0}, // ROW2: W=Up, S=Down, G=}, H=^
    {0, '\x14', '\t', 0, 0},       // ROW3: D=Right, T=Tab
    {'\x12', '%', '\x0a', '\r',
     '\x7f'},                       // ROW4: A=Left, P=%, shift-R=Caps, bksp=Del
    {'\x08', '>', '\\', 0, '\x06'}, // ROW5: alt=HostAlt, X=>, V=\, mute=Mute
    {' ', '<', '\x07', '~', '|'},   // ROW6: Z=<, C=Ctrl, N=~, M=|
    {'\x04', '\x0a', '{', 0, 0},    // ROW7: mic=Super, shift-L=Caps, F={
};

static uint8_t last_keycode = 0;
static bool alt_held = false;
static bool sym_held = false;

static bool key_raw[7][5] = {0};
static bool key_debounced[7][5] = {0};
static uint32_t last_change_ms[7][5] = {0};

static void i2c_slave_handler(i2c_inst_t *i2c, i2c_slave_event_t event) {
    switch (event) {
    case I2C_SLAVE_REQUEST:
        i2c_get_hw(i2c)->data_cmd = last_keycode;
        last_keycode = 0;

        break;
    case I2C_SLAVE_RECEIVE:
        (void)i2c_get_hw(i2c)->data_cmd;

        break;
    case I2C_SLAVE_FINISH:
        break;
    }
}

static void handle_key_transition(int r, int c, bool pressed) {
    char base = keymap[r][c];

    if (base == '\x03') { // alt
        alt_held = pressed;
    } else if (base == '\x01') { // sym
        sym_held = pressed;
    }

    if (!pressed) {
        return;
    }

    char resolved;
    if (sym_held && keymap_sym[r][c] != '\0') {
        resolved = keymap_sym[r][c];
    } else if (alt_held) {
        resolved = keymap_alt[r][c];
    } else {
        resolved = base;
    }

    last_keycode = (uint8_t)resolved;

    printf("Key pressed: '%c' (Row%d, Col%d, alt=%d, sym=%d)\n", resolved, r, c,
           alt_held, sym_held);
}

int main(void) {
    stdio_init_all();
    sleep_ms(2000);
    printf("Starting Q10 Keyboard Firmware...\n");

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

    i2c_init(i2c0, I2C_BAUDRATE);
    gpio_set_function(I2C_SDA_PIN, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL_PIN, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA_PIN);
    gpio_pull_up(I2C_SCL_PIN);
    i2c_slave_init(i2c0, I2C_ADDR, i2c_slave_handler);

    while (true) {
        for (int c = 0; c < 5; c++) {
            gpio_set_dir(COLS[c], GPIO_OUT);
            gpio_put(COLS[c], 0);
            sleep_us(10); // allow signal to settle

            uint32_t now = to_ms_since_boot(get_absolute_time());

            for (int r = 0; r < 7; r++) {
                bool pressed = !gpio_get(ROWS[r]); // active low

                if (pressed != key_raw[r][c]) {
                    key_raw[r][c] = pressed;
                    last_change_ms[r][c] = now;
                }

                if ((now - last_change_ms[r][c]) >= DEBOUNCE_MS &&
                    key_raw[r][c] != key_debounced[r][c]) {
                    key_debounced[r][c] = key_raw[r][c];

                    handle_key_transition(r, c, key_debounced[r][c]);
                }
            }

            gpio_set_dir(COLS[c], GPIO_IN);
            gpio_disable_pulls(COLS[c]);
        }

        sleep_ms(1);
    }
}
