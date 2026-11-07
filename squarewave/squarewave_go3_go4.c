/**
 * Copyright (c) 2020 Raspberry Pi (Trading) Ltd.
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pio.h"
#include "squarewave.pio.h"

#define TRIGGER_GPIO 15
#define OUTPUT_GPIO 0
#define MAX_DELAYS 100

// Match the instruction paths in squarewave.pio.
// The delay starts AFTER WAIT detects the input, so GPIO input latency is extra.
// This is determined by the pio program itself, how many lines of code we have to go through before we change the output
static const uint32_t delay_overhead_cycles = 2;
static const uint32_t width_overhead_cycles = 3;

static uint32_t ns_to_cycles(uint32_t ns, uint32_t clock_hz) {
    // Use 64-bit multiplication and round to the nearest whole PIO cycle.
    return (uint32_t)(((uint64_t)ns * clock_hz + 500000000ULL) / 1000000000ULL);
}

static void queue_frame(PIO pio, uint sm, uint32_t delay_cycles,
                        uint32_t width_cycles, uint32_t pulses_per_frame) {
    pio_interrupt_clear(pio, 0);

    // The three PULL instructions in PIO read these in this exact order.
    // GO3: PIO uses count - 1, so a user count of N produces N pulses.
    pio_sm_put_blocking(pio, sm, pulses_per_frame - 1);
    pio_sm_put_blocking(pio, sm, delay_cycles - delay_overhead_cycles);
    pio_sm_put_blocking(pio, sm, width_cycles - width_overhead_cycles);
}

int main(void) {
    stdio_init_all();
    while (!stdio_usb_connected()) sleep_ms(10);

    // set up the trigger_gpio such that it waits for a rising edge before it triggers
    gpio_init(TRIGGER_GPIO);
    gpio_set_dir(TRIGGER_GPIO, GPIO_IN);
    gpio_pull_down(TRIGGER_GPIO);

    PIO pio = pio0;
    uint sm = pio_claim_unused_sm(pio, true);
    uint offset = pio_add_program(pio, &squarewave_program);
    pio_sm_config config = squarewave_program_get_default_config(offset);
    sm_config_set_set_pins(&config, OUTPUT_GPIO, 1);
    sm_config_set_in_pins(&config, TRIGGER_GPIO); // WAIT PIN 0 uses this input.
    sm_config_set_clkdiv(&config, 1.0f);         // PIO clock equals system clock.
    pio_gpio_init(pio, OUTPUT_GPIO);
    pio_sm_init(pio, sm, offset, &config);
    pio_sm_set_consecutive_pindirs(pio, sm, OUTPUT_GPIO, 1, true);
    pio_sm_exec(pio, sm, pio_encode_set(pio_pins, 0));

    const uint32_t clock_hz = clock_get_hz(clk_sys);
    printf("\nDelay-list test: user-selected pulses and pause per frame\n");
    printf("PIO clock: %" PRIu32 " Hz. Enter timings in whole nanoseconds.\n",
           clock_hz);
    printf("Reported delays exclude GPIO synchronization/WAIT detection latency.\n");

    uint32_t pulse_width_ns;
    printf("Pulse width (ns): ");
    fflush(stdout);
    if (scanf("%" SCNu32, &pulse_width_ns) != 1) {
        printf("Invalid pulse width. Reset to try again.\n");
        return 1;
    }
    uint32_t width_cycles = ns_to_cycles(pulse_width_ns, clock_hz);
    if (width_cycles < width_overhead_cycles) {
        printf("Pulse width is too short. Reset to try again.\n");
        return 1;
    }
    printf("  Using %" PRIu32 " cycles (%.2f ns).\n",
           width_cycles, (double)width_cycles * 1e9 / clock_hz);

    // GO3: Use the same pulse count for every delay value in this run.
    int32_t pulses_per_frame;
    printf("Pulses per delay: ");
    fflush(stdout);
    if (scanf("%" SCNd32, &pulses_per_frame) != 1 || pulses_per_frame <= 0) {
        printf("Enter a positive pulse count. Reset to try again.\n");
        return 1;
    }

    // GO4: Use the same pause after every frame in continuous test mode.
    int32_t pause_between_frames_us;
    printf("Pause between frames (us): ");
    fflush(stdout);
    if (scanf("%" SCNd32, &pause_between_frames_us) != 1 ||
        pause_between_frames_us < 0) {
        printf("Enter a non-negative pause. Reset to try again.\n");
        return 1;
    }

    uint32_t delay_count;
    printf("How many delta-t values (1-100): ");
    fflush(stdout);
    if (scanf("%" SCNu32, &delay_count) != 1 ||
        delay_count == 0 || delay_count > MAX_DELAYS) {
        printf("Enter a count from 1 to 100. Reset to try again.\n");
        return 1;
    }

    uint32_t delay_cycles[MAX_DELAYS];
    for (uint32_t i = 0; i < delay_count; ++i) {
        uint32_t delay_ns;
        printf("Delta-t %" PRIu32 " (ns): ", i + 1);
        fflush(stdout);
        if (scanf("%" SCNu32, &delay_ns) != 1) {
            printf("Invalid delay. Reset to try again.\n");
            return 1;
        }
        delay_cycles[i] = ns_to_cycles(delay_ns, clock_hz);
        if (delay_cycles[i] < delay_overhead_cycles) {
            printf("Delay is too short. Reset to try again.\n");
            return 1;
        }
        printf("  Using %" PRIu32 " cycles (%.2f ns).\n",
               delay_cycles[i], (double)delay_cycles[i] * 1e9 / clock_hz);
    }

    printf("Armed: input GPIO %d, output GPIO %d; %" PRId32
           " pulses per delay; %" PRId32 " us between frames.\n",
           TRIGGER_GPIO, OUTPUT_GPIO, pulses_per_frame, pause_between_frames_us);
    fflush(stdout);

    printf("Continuous test mode. Reset the Pico to enter new parameters.\n");
    fflush(stdout);

    // PIO begins at PULL BLOCK and stays there until we queue a frame.
    pio_sm_set_enabled(pio, sm, true);

    // Repeat the whole delay list so the waveform can be observed continuously.
    // Every output pulse still needs a new rising edge on the input GPIO.
    while (true) {
        for (uint32_t i = 0; i < delay_count; ++i) {
            queue_frame(pio, sm, delay_cycles[i], width_cycles, pulses_per_frame);

            // Wait until PIO finishes the selected number of output pulses.
            while (!pio_interrupt_get(pio, 0)) tight_loop_contents();

            // Pause after every frame, including the last one in the list.
            // After the last frame, the next loop starts again at delta-t 1.
            sleep_us(pause_between_frames_us);
        }
    }
}
