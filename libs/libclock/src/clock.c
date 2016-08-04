#include <stdint.h>

#include <clock/clock.h>
#include <cspace/cspace.h>

#define EPIT1_IRQ 88

#define EPIT_CLKSRC 24
#define EPIT_OM 22
#define EPIT_STOPEN 21
#define EPIT_WAITEN 19
#define EPIT_DBGEN 18
#define EPIT_IOVW 17
#define EPIT_SWR 16
#define EPIT_PRESCALAR 4
#define EPIT_RLD 3
#define EPIT_OCIEN 2
#define EPIT_ENMOD 1
#define EPIT_EN 0

#define EPIT_FREQ 66000000

static uint32_t *timer_vaddr;

extern const seL4_BootInfo* _boot_info;

static seL4_CPtr _irq_ep;

static timestamp_t current_time = 0;

static timestamp_t load_register_value;

static struct timer_irq {
    int irq;
    seL4_IRQHandler cap;
} _timer_irqs[1];

static seL4_CPtr
enable_irq(int irq, seL4_CPtr aep) {
    seL4_CPtr cap;
    int err;
    /* Create an IRQ handler */
    cap = cspace_irq_control_get_cap(cur_cspace, seL4_CapIRQControl, irq);
    /* Assign to an end point */
    err = seL4_IRQHandler_SetEndpoint(cap, aep);
    if (err) {
        return NULL;
    }
    /* Ack the handler before continuing */
    err = seL4_IRQHandler_Ack(cap);
    if (err) {
        return NULL;
    }
    return cap;
}

/* Initialises the timer */
void timer_init(uint32_t *vaddr) {
    timer_vaddr = vaddr;
}

void set_next_timer_interrupt(uint32_t ms) {
    load_register_value = EPIT_FREQ / 1000 * ms;
    *(timer_vaddr + 2) = load_register_value;
}

int start_timer(seL4_CPtr interrupt_ep) {
    _irq_ep = interrupt_ep;

    uint32_t control = (1 << EPIT_CLKSRC |
                        3 << EPIT_OM |
                        0 << EPIT_STOPEN |
                        0 << EPIT_WAITEN |
                        0 << EPIT_DBGEN |
                        1 << EPIT_IOVW |
                        0 << EPIT_SWR |
                        0 << EPIT_PRESCALAR |
                        1 << EPIT_RLD |
                        1 << EPIT_OCIEN |
                        1 << EPIT_ENMOD |
                        1 << EPIT_EN);
    *timer_vaddr = control;

    _timer_irqs[0].irq = EPIT1_IRQ;
    _timer_irqs[0].cap = enable_irq(EPIT1_IRQ, _irq_ep);

    set_next_timer_interrupt(1000);

    return CLOCK_R_OK;
}

uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data) {
    return 0;
}

int remove_timer(uint32_t id) {
    return 0;
}

int timer_interrupt(void) {
    if(_irq_ep == seL4_CapNull){
        return CLOCK_R_FAIL;
    }
    *(timer_vaddr + 1) = 1;
    int err = seL4_IRQHandler_Ack(_timer_irqs[0].cap);

    current_time += load_register_value * 1000000/EPIT_FREQ;
    return CLOCK_R_FAIL;
}

timestamp_t time_stamp(void) {
    timestamp_t counter = timer_vaddr[4] * 1000000/EPIT_FREQ;
    return current_time + counter;
}

int stop_timer(void) {
    return 0;
}
