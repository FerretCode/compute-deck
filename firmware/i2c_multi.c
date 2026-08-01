#include "i2c_multi.h"
#include <stdio.h>
#include "hardware/irq.h"

#define CLK_DIV 16

static i2c_multi_t *i2c_multi;

static void (*receive_handler)(uint8_t data, bool is_address) = NULL;
static void (*request_handler)(uint8_t address) = NULL;
static void (*stop_handler)(uint8_t length) = NULL;

static inline void i2c_slave_program_init(PIO pio, uint sm, uint offset, uint sda_pin, uint scl_pin, uint start_offset, uint jmp_pin);
static inline void byte_handler_pio(void);
static inline void stop_handler_pio(void);
static inline uint8_t transpond_byte(uint8_t byte);

// Opcodes for out exec
#define OP_WAIT_SCL_LOW  0x2020 // wait 0 gpio 0
#define OP_WAIT_SCL_HIGH 0x20a0 // wait 1 gpio 0
#define OP_STRETCH_SCL   0xe081 // set pins 0 side 1
#define OP_ACK_SDA_LOW   0xe083 // set pins 0 side 3
#define OP_RELEASE_SCL   0xe082 // set pins 0 side 2
#define OP_RELEASE_BUS   0xe080 // set pins 0 side 0
#define OP_IRQ_WAIT_0    0xc020 // irq wait 0
#define OP_JMP(addr)     (0x0000 | (addr))
#define OP_JMP_PIN(addr) (0x00c0 | (addr))

void i2c_multi_init(PIO pio, uint sda_pin, uint scl_pin) {
    pio_clear_instruction_memory(pio);
    i2c_multi = (i2c_multi_t *)malloc(sizeof(i2c_multi_t));
    i2c_multi->pio = pio;
    i2c_multi->status = I2C_IDLE;
    i2c_multi->sda_pin = sda_pin;
    i2c_multi->scl_pin = scl_pin;
    i2c_multi->bytes_count = 0;
    i2c_multi_disable_all_addresses();
    i2c_multi->buffer = NULL;
    i2c_multi->buffer_start = NULL;
    uint pio_irq0 = (pio == pio0 ? PIO0_IRQ_0 : PIO1_IRQ_0);
    uint pio_irq1 = (pio == pio0 ? PIO0_IRQ_1 : PIO1_IRQ_1);
    i2c_multi->length = -1;
    pio_gpio_init(pio, sda_pin);
    pio_gpio_init(pio, scl_pin);
    
    uint offset = pio_add_program(pio, &i2c_slave_program);
    i2c_multi->offset_read = offset;
    i2c_multi->offset_write = offset;
    i2c_multi->offset_start = offset;
    i2c_multi->offset_stop = offset;

    i2c_multi->sm_start = pio_claim_unused_sm(pio, true);
    i2c_slave_program_init(pio, i2c_multi->sm_start, offset, sda_pin, scl_pin, i2c_slave_offset_start_loop, scl_pin);
    
    i2c_multi->sm_stop = pio_claim_unused_sm(pio, true);
    i2c_slave_program_init(pio, i2c_multi->sm_stop, offset, sda_pin, scl_pin, i2c_slave_offset_stop_loop, scl_pin);
    
    i2c_multi->sm_read = pio_claim_unused_sm(pio, true);
    i2c_slave_program_init(pio, i2c_multi->sm_read, offset, sda_pin, scl_pin, i2c_slave_offset_read_byte, sda_pin);
    
    i2c_multi->sm_write = pio_claim_unused_sm(pio, true);
    i2c_slave_program_init(pio, i2c_multi->sm_write, offset, sda_pin, scl_pin, i2c_slave_offset_write_byte, sda_pin);

    printf("PIO SMs initialized at offset %u\n", offset);

    // Default ACK sequence: Wait SCL Low then Stretch
    pio_sm_put(pio, i2c_multi->sm_read, (OP_STRETCH_SCL << 16) | OP_WAIT_SCL_LOW);
    pio_sm_put(pio, i2c_multi->sm_read, (OP_IRQ_WAIT_0 << 16) | 0xa042);
               
    irq_set_exclusive_handler(pio_irq0, byte_handler_pio);
    irq_set_enabled(pio_irq0, true);
    irq_set_exclusive_handler(pio_irq1, stop_handler_pio);
    irq_set_enabled(pio_irq1, true);
}

void i2c_multi_set_write_buffer(uint8_t *buffer) {
    i2c_multi->buffer = buffer;
    i2c_multi->buffer_start = buffer;
}

void i2c_multi_set_receive_handler(i2c_multi_receive_handler_t handler) { receive_handler = handler; }
void i2c_multi_set_request_handler(i2c_multi_request_handler_t handler) { request_handler = handler; }
void i2c_multi_set_stop_handler(i2c_multi_stop_handler_t handler) { stop_handler = handler; }

void i2c_multi_enable_address(uint8_t address) { i2c_multi->address[address / 32] |= 1 << (address % 32); }
void i2c_multi_disable_address(uint8_t address) { i2c_multi->address[address / 32] &= ~(1 << (address % 32)); }

void i2c_multi_enable_all_addresses() {
    for(int i=0; i<4; i++) i2c_multi->address[i] = 0xFFFFFFFF;
}

void i2c_multi_disable_all_addresses() {
    for(int i=0; i<4; i++) i2c_multi->address[i] = 0;
}

bool i2c_multi_is_address_enabled(uint8_t address) { return i2c_multi->address[address / 32] & (1 << (address % 32)); }

void i2c_multi_disable(void) {
    pio_sm_set_enabled(i2c_multi->pio, i2c_multi->sm_read, false);
    pio_sm_set_enabled(i2c_multi->pio, i2c_multi->sm_write, false);
    pio_sm_set_enabled(i2c_multi->pio, i2c_multi->sm_start, false);
    pio_sm_set_enabled(i2c_multi->pio, i2c_multi->sm_stop, false);
    pio_sm_clear_fifos(i2c_multi->pio, i2c_multi->sm_read);
    pio_sm_clear_fifos(i2c_multi->pio, i2c_multi->sm_write);
    gpio_set_input_enabled(i2c_multi->sda_pin, true);
    gpio_set_input_enabled(i2c_multi->scl_pin, true);
    i2c_multi->bytes_count = 0;
    i2c_multi->status = I2C_IDLE;
    i2c_multi->buffer = i2c_multi->buffer_start;
}

void i2c_multi_restart(void) {
    i2c_multi_disable();
    pio_sm_restart(i2c_multi->pio, i2c_multi->sm_start);
    pio_sm_restart(i2c_multi->pio, i2c_multi->sm_stop);
    pio_sm_restart(i2c_multi->pio, i2c_multi->sm_read);
    pio_sm_restart(i2c_multi->pio, i2c_multi->sm_write);
    pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_STRETCH_SCL << 16) | OP_WAIT_SCL_LOW);
    pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_IRQ_WAIT_0 << 16) | 0xa042);
    pio_sm_set_enabled(i2c_multi->pio, i2c_multi->sm_read, true);
    pio_sm_set_enabled(i2c_multi->pio, i2c_multi->sm_write, true);
    pio_sm_set_enabled(i2c_multi->pio, i2c_multi->sm_start, true);
    pio_sm_set_enabled(i2c_multi->pio, i2c_multi->sm_stop, true);
}

void i2c_multi_remove(void) {
    pio_set_irq0_source_enabled(i2c_multi->pio, pis_interrupt0, false);
    pio_set_irq1_source_enabled(i2c_multi->pio, pis_interrupt1, false);
    pio_clear_instruction_memory(i2c_multi->pio);
    pio_sm_unclaim(i2c_multi->pio, i2c_multi->sm_start);
    pio_sm_unclaim(i2c_multi->pio, i2c_multi->sm_stop);
    pio_sm_unclaim(i2c_multi->pio, i2c_multi->sm_read);
    pio_sm_unclaim(i2c_multi->pio, i2c_multi->sm_write);
    gpio_set_input_enabled(i2c_multi->sda_pin, true);
    gpio_set_input_enabled(i2c_multi->scl_pin, true);
    free(i2c_multi);
}

void i2c_multi_fixed_length(int16_t length) { i2c_multi->length = length; }

static inline void i2c_slave_program_init(PIO pio, uint sm, uint offset, uint sda_pin, uint scl_pin, uint start_offset, uint jmp_pin) {
    pio_sm_config c = i2c_slave_program_get_default_config(offset);
    sm_config_set_in_pins(&c, sda_pin);
    sm_config_set_out_pins(&c, sda_pin, 1);
    sm_config_set_set_pins(&c, scl_pin, 2); 
    sm_config_set_sideset_pins(&c, scl_pin); 
    sm_config_set_jmp_pin(&c, jmp_pin);
    sm_config_set_clkdiv(&c, CLK_DIV);
    sm_config_set_in_shift(&c, true, false, 32); 
    sm_config_set_out_shift(&c, true, true, 32);
    if(sm == i2c_multi->sm_write) sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    
    pio_sm_init(pio, sm, offset + start_offset, &c);
    pio_sm_set_enabled(pio, sm, true);
    
    if(sm == i2c_multi->sm_stop) {
        pio_set_irq1_source_enabled(pio, pis_interrupt1, true);
        pio_interrupt_clear(pio, 1);
    }
    if(sm == i2c_multi->sm_read) {
        pio_set_irq0_source_enabled(pio, pis_interrupt0, true);
        pio_interrupt_clear(pio, 0);
    }
}

static inline void byte_handler_pio(void) {
    uint8_t received = 0;
    bool is_address = false;
    i2c_multi->bytes_count++;
    
    if (i2c_multi->status != I2C_WRITE) {
        received = transpond_byte(pio_sm_get_blocking(i2c_multi->pio, i2c_multi->sm_read) >> 24);
        printf("IRQ0: byte=0x%02X, status=%d\n", received, i2c_multi->status);
    }

    if (i2c_multi->status == I2C_IDLE) {
        if (!i2c_multi_is_address_enabled(received >> 1)) {
            // NACK: Release bus and back to start
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_JMP(i2c_multi->offset_read + i2c_slave_offset_read_byte) << 16) | OP_RELEASE_BUS);
            // Re-setup default for next address
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_STRETCH_SCL << 16) | OP_WAIT_SCL_LOW);
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_IRQ_WAIT_0 << 16) | 0xa042);
            i2c_multi->status = I2C_IDLE; i2c_multi->bytes_count = 0;
            pio_interrupt_clear(i2c_multi->pio, 0);
            return;
        }
        i2c_multi->status = (received & 1) ? I2C_WRITE : I2C_READ;
        is_address = true;
    }

    if (i2c_multi->status == I2C_READ) {
        // ACK sequence: SDA low, Release SCL, Wait SCL High, Wait SCL Low, Back to Loop
        pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_RELEASE_SCL << 16) | OP_ACK_SDA_LOW);
        pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_WAIT_SCL_LOW << 16) | OP_WAIT_SCL_HIGH);
        pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_JMP(i2c_multi->offset_read + i2c_slave_offset_read_loop) << 16) | OP_RELEASE_BUS);
        // Re-setup default
        pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_STRETCH_SCL << 16) | OP_WAIT_SCL_LOW);
        pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_IRQ_WAIT_0 << 16) | 0xa042);
        if (receive_handler) receive_handler(is_address ? received >> 1 : received, is_address);
    } 
    else if (i2c_multi->status == I2C_WRITE) {
        if (is_address) {
            if (request_handler) request_handler(received >> 1);
            uint8_t value = transpond_byte(i2c_multi->buffer ? *i2c_multi->buffer++ : 0);
            // ACK address
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_RELEASE_SCL << 16) | OP_ACK_SDA_LOW);
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_WAIT_SCL_LOW << 16) | OP_WAIT_SCL_HIGH);
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_JMP(i2c_multi->offset_read + i2c_slave_offset_read_byte) << 16) | OP_RELEASE_BUS);
            // Re-setup read SM default
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_read, (OP_STRETCH_SCL << 16) | OP_WAIT_SCL_LOW);
            pio_sm_put_blocking(i2c_multi->pio, i2c_multi->sm_read, (OP_IRQ_WAIT_0 << 16) | 0xa042);
            // Trigger write SM
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_write, value);
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_write, (OP_STRETCH_SCL << 16) | OP_WAIT_SCL_LOW);
            pio_sm_put(i2c_multi->pio, i2c_multi->sm_write, (OP_IRQ_WAIT_0 << 16) | 0xa042);
        } else {
            // Master ACK check
            if (gpio_get(i2c_multi->sda_pin) || (i2c_multi->length > 0 && i2c_multi->bytes_count >= i2c_multi->length + 1)) {
                pio_sm_exec(i2c_multi->pio, i2c_multi->sm_write, OP_JMP(i2c_multi->offset_write + i2c_slave_offset_write_byte));
                i2c_multi->status = I2C_IDLE; i2c_multi->bytes_count = 0;
            } else {
                uint8_t value = transpond_byte(i2c_multi->buffer ? *i2c_multi->buffer++ : 0);
                pio_sm_put(i2c_multi->pio, i2c_multi->sm_write, (OP_WAIT_SCL_LOW << 16) | OP_RELEASE_BUS);
                pio_sm_put(i2c_multi->pio, i2c_multi->sm_write, value);
                pio_sm_put(i2c_multi->pio, i2c_multi->sm_write, (OP_STRETCH_SCL << 16) | OP_WAIT_SCL_LOW);
                pio_sm_put(i2c_multi->pio, i2c_multi->sm_write, (OP_IRQ_WAIT_0 << 16) | 0xa042);
            }
        }
    }
    pio_interrupt_clear(i2c_multi->pio, 0);
}

static inline void stop_handler_pio(void) {
    if (i2c_multi->status != I2C_IDLE) {
        printf("IRQ1: Stop/Restart, status=%d, count=%d\n", i2c_multi->status, i2c_multi->bytes_count);
    }
    pio_interrupt_clear(i2c_multi->pio, 1);
    if (i2c_multi->status == I2C_IDLE) return;
    pio_sm_exec(i2c_multi->pio, i2c_multi->sm_read, OP_JMP(i2c_multi->offset_read + i2c_slave_offset_read_byte));
    pio_sm_exec(i2c_multi->pio, i2c_multi->sm_write, OP_JMP(i2c_multi->offset_write + i2c_slave_offset_write_byte));
    pio_sm_clear_fifos(i2c_multi->pio, i2c_multi->sm_read);
    pio_sm_clear_fifos(i2c_multi->pio, i2c_multi->sm_write);
    i2c_multi->buffer = i2c_multi->buffer_start;
    if (stop_handler) stop_handler(i2c_multi->bytes_count - 1);
    i2c_multi->bytes_count = 0;
    i2c_multi->status = I2C_IDLE;
}

static inline uint8_t transpond_byte(uint8_t byte) {
    uint8_t transponded = ((byte & 0x1) << 7) | (((byte & 0x2) >> 1) << 6) | (((byte & 0x4) >> 2) << 5) |
                          (((byte & 0x8) >> 3) << 4) | (((byte & 0x10) >> 4) << 3) | (((byte & 0x20) >> 5) << 2) |
                          (((byte & 0x40) >> 6) << 1) | (((byte & 0x80) >> 7));
    return transponded;
}
