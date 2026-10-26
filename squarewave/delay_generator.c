#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <inttypes.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "delay_generator.pio.h"

// pins and things
#define SYNC_GPIO 15
#define GLOBAL_GPIO 14
#define OUTPUT_GPIO 0
#define MAX_DELAYS 100

// parametrizable values
static const uint32_t delay_ns[] = {50, 100, 150};
static const uint32_t pulse_width_ns = 40;
static const uint32_t triggers_per_delay = 10000;
static const uint32_t readout_pause_us = 1000;
static const uint32_t list_iterations = 2;
static const uint32_t rest_between_iterations_ms = 50;

#define DELAY_COUNT (sizeof(delay_ns) / sizeof(delay_ns[0]))
_Static_assert(DELAY_COUNT <= MAX_DELAYS, "Too many delays sorry");

// converts ns to clock cycles (i think)
static uint32_t ns_to_cycles(uint32_t ns, uint32_t hz) {
    return (uint32_t)(((uint64_t)ns * hz + 500000000ULL) / 1000000000ULL);
}

static bool wait_for_global_rising_edge(void) {
    // wait for low, then rising edge
    while (gpio_get(GLOBAL_GPIO)) tight_loop_contents();
    while (!gpio_get(GLOBAL_GPIO)) tight_loop_contents();
    return true;
}

static bool start_burst(PIO pio, uint sm, uint offset, uint32_t pulses,
                        uint32_t requested_delay_ns, uint32_t requested_width_ns,
                        uint32_t clk_hz) {
    uint32_t delay_cycles = ns_to_cycles(requested_delay_ns, clk_hz);
    uint32_t width_cycles = ns_to_cycles(requested_width_ns, clk_hz);
    if (pulses == 0 || delay_cycles < 2 || width_cycles < 2) return false;

    // stop then restart at the beginning of the assembly program.
    // clear any leftover FIFO words and prior flags.
    pio_sm_set_enabled(pio, sm, false);
    pio_sm_clear_fifos(pio, sm);
    pio_sm_restart(pio, sm);
    pio_sm_exec(pio, sm, pio_encode_jmp(offset));
    pio_interrupt_clear(pio, 0);

    // after the WAIT 1 instruction completes:
    // delay to output high ~= delay_loop_count + 2 PIO cycles.
    // high duration = width_loop_count + 2 PIO cycles.
    pio_sm_put_blocking(pio, sm, pulses - 1);
    pio_sm_put_blocking(pio, sm, delay_cycles - 2);
    pio_sm_put_blocking(pio, sm, width_cycles - 2);
    pio_sm_set_enabled(pio, sm, true);
    return true;
}

int main(void) {
    stdio_init_all();
    gpio_init(SYNC_GPIO);
    gpio_set_dir(SYNC_GPIO, GPIO_IN);
    gpio_pull_down(SYNC_GPIO);
    gpio_init(GLOBAL_GPIO);
    gpio_set_dir(GLOBAL_GPIO, GPIO_IN);
    gpio_pull_down(GLOBAL_GPIO);

    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &delay_generator_program);
    pio_sm_config c = delay_generator_program_get_default_config(offset);
    sm_config_set_set_pins(&c, OUTPUT_GPIO, 1);
    sm_config_set_clkdiv(&c, 1.0f);
    pio_gpio_init(pio, OUTPUT_GPIO);
    pio_sm_init(pio, sm, offset, &c);
    pio_sm_set_consecutive_pindirs(pio, sm, OUTPUT_GPIO, 1, true);
    pio_sm_set_enabled(pio, sm, false);
    const uint32_t hz = clock_get_hz(clk_sys);

    // the delay and width are in terms of PIO clock cycles.
    // for 150 MHz, 1 cycle = 6.667 ns
    while (true) {
        wait_for_global_rising_edge();
        for (uint32_t iter = 0; iter < list_iterations; ++iter) {
            for (size_t i = 0; i < DELAY_COUNT; ++i) {
                if (!start_burst(pio, sm, offset, triggers_per_delay,
                                 delay_ns[i], pulse_width_ns, hz)) {
                    printf("Invalid timing/count for delay %u ns\n", delay_ns[i]);
                    while (true) tight_loop_contents();
                }
                // the PIO sets IRQ flag 0 only after exactly N pulses.
                // if fewer triggers arrive, this intentionally waits forever.
                while (!pio_interrupt_get(pio, 0)) tight_loop_contents();
                pio_sm_set_enabled(pio, sm, false);
                pio_sm_exec(pio, sm, pio_encode_set(pio_pins, 0));
                sleep_us(readout_pause_us);
            }
            if (iter + 1 < list_iterations) sleep_ms(rest_between_iterations_ms);
        }
    }
}
