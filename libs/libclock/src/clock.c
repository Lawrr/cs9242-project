#include <stdint.h>

#include <utils/util.h>
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

#define EPIT_CONTROL_REGISTER 0
#define EPIT_STATUS_REGISTER 1
#define EPIT_LOAD_REGISTER 2
#define EPIT_COMPARE_REGISTER 3
#define EPIT_COUNTER_REGISTER 4

#define EPIT_FREQUENCY 66000000

#define DEFAULT_INTERRUPT_TICK 0xFFFFFFFF

static uint32_t *timer_vaddr;

extern const seL4_BootInfo* _boot_info;

static seL4_CPtr _irq_ep;

static timestamp_t current_time = 0;

static timestamp_t load_register_value;

static uint32_t current_id = 1;

struct timer_handler {
    timer_callback_t callback;
    timestamp_t expires;
    struct timer_handler *next;
    int id;
    void *data;
};

static struct timer_handler *handler_head;

static struct timer_irq {
    int irq;
    seL4_IRQHandler cap;
} _timer_irqs[1];

static seL4_CPtr enable_irq(int irq, seL4_CPtr aep) {
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

static struct timer_handler *timer_handler_new(timer_callback_t callback, void *data, uint64_t expires){
    struct timer_handler *h = malloc(sizeof(struct timer_handler));
    if (h == NULL) {
        return NULL;
    }

    h->callback = callback;
    h->data = data;
    h->next = NULL;
    h->expires = current_time + expires;

    if (current_id == 0) {
        current_id = 1;
    }
    h->id = current_id++;

    return h;
}

static void insert(struct timer_handler *to_insert) {
    struct timer_handler *curr = handler_head;
    if (curr == NULL) {
        handler_head = to_insert;
        set_next_timer_interrupt((to_insert->expires - current_time) / 1000);
    } else if (curr->expires > to_insert->expires) {
        to_insert->next = curr;
        handler_head = to_insert;
        set_next_timer_interrupt((to_insert->expires - current_time) / 1000);
    } else {
        bool inserted = FALSE;
        while (curr->next != NULL) {
            if (curr->next->expires > to_insert->expires) {
                to_insert->next = curr->next;
                curr->next = to_insert;
                inserted = TRUE;
            }
            curr = curr->next;
        }

        /* Insert at the end if its the longest */
        if (!inserted) {
            to_insert->next = curr->next;
            curr->next = to_insert;
            inserted = TRUE;
        }
    }
}

static struct timer_handler *remove_head() {
    struct timer_handler *ret = handler_head;
    if (ret == NULL) {
        return NULL;
    }

    handler_head = handler_head->next;
    if (handler_head != NULL) {
        set_next_timer_interrupt((handler_head->expires - current_time) / 1000);
    } else {
        set_next_timer_interrupt(DEFAULT_INTERRUPT_TICK);
    }

    return ret;
}

/* Initialises the timer */
void timer_init(uint32_t *vaddr) {
    timer_vaddr = vaddr;
}

void set_next_timer_interrupt(uint32_t ms) {
    load_register_value = EPIT_FREQUENCY / 1000 * ms;
    timer_vaddr[EPIT_LOAD_REGISTER] = load_register_value;
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
    timer_vaddr[EPIT_CONTROL_REGISTER] = control;

    _timer_irqs[0].irq = EPIT1_IRQ;
    _timer_irqs[0].cap = enable_irq(EPIT1_IRQ, _irq_ep);

    set_next_timer_interrupt(DEFAULT_INTERRUPT_TICK);

    return CLOCK_R_OK;
}

uint32_t register_timer(uint64_t delay, timer_callback_t callback, void *data) {
    struct timer_handler *new_handler = timer_handler_new(callback, data, delay);
    if (new_handler == NULL) {
        return 0;
    }

    insert(new_handler);

    return new_handler->id;
}

int remove_timer(uint32_t id) {
    struct timer_handler *curr = handler_head;
    if (curr == NULL) {
        return CLOCK_R_FAIL;
    }

    while (curr->next != NULL) {
        if (curr->next->id == id) {
            struct timer_handler *del = curr->next;
            curr->next = del->next;
            free(del);
            return CLOCK_R_OK;
        }
        curr = curr->next;
    }

    /* Check if its the very last item on the list */
    // TODO

    return CLOCK_R_FAIL;
}

int timer_interrupt(void) {
    if (_irq_ep == seL4_CapNull) {
        return CLOCK_R_FAIL;
    }
    timer_vaddr[EPIT_STATUS_REGISTER] = 1;
    int err = seL4_IRQHandler_Ack(_timer_irqs[0].cap);

    current_time += load_register_value * 1000000 / EPIT_FREQUENCY;

    /* Handle callback */
    struct timer_handler *handler = remove_head();
    if (handler != NULL) {
        handler->callback(handler->id, handler->data);
        free(handler);
    }

    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    timestamp_t counter = timer_vaddr[EPIT_COUNTER_REGISTER] * 1000000 / EPIT_FREQUENCY;
    return current_time + counter;
}

int stop_timer(void) {
    timer_vaddr[EPIT_CONTROL_REGISTER] &= 0 << EPIT_EN;
    return CLOCK_R_OK;
}
