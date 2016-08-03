#include <stdint.h>

#include <clock/clock.h>

int start_timer(seL4_CPtr interrupt_ep) {
    return 0;
}

uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data) {
    return 0;
}

int remove_timer(uint32_t id) {
    return 0;
}

int timer_interrupt(void) {
    return 0;
}

timestamp_t time_stamp(void) {
    return 0;
}

int stop_timer(void) {
    return 0;
}

/*
 * TODO: get #include working
 *
 * 1. Map device
 * 2. Put settings into virtual address (from mapping)
 */
