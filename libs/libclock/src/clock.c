#include <stdint.h>

#include <utils/util.h>
#include <clock/clock.h>
#include <cspace/cspace.h>

#define EPIT1_IRQ 88
#define EPIT2_IRQ 89

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

#define EPIT_FREQUENCY 66000000 /* One second */

#define INTERRUPT_THRESHOLD 1000 /* In terms of microseconds */
#define DEFAULT_INTERRUPT_TICK 3960000000 /* In terms of frequency */
#define MAX_INTERRUPT_TICK 0xFFFFFFFF /* In terms of frequency */

extern const seL4_BootInfo* _boot_info;

/* EPIT timer registers */
static volatile struct epit_timer {
    uint32_t control;
    uint32_t status;
    uint32_t load;
    uint32_t compare;
    uint32_t counter;
};

/* Stores a registered timer */
static struct timer_handler {
    int id;
    timer_callback_t callback;
    timestamp_t expire_time;
    timestamp_t delay;
    void *data;
    struct timer_handler *next;
};

static struct timer_irq {
    int irq;
    seL4_IRQHandler cap;
} _timer_irqs[2];

static struct epit_timer *epit1_timer;
static struct epit_timer *epit2_timer;

static seL4_CPtr _irq_ep;

static timestamp_t current_time = 0;

static timestamp_t load_register_value;

static uint32_t current_id = 1;

/* Head of linked list of timer_handlers - sorted by expire_time */
static struct timer_handler *handler_head = NULL;

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

static void handler_queue_destroy(void) {
    struct timer_handler *next; 
    struct timer_handler *handler = handler_head;
    while (handler != NULL) {
        next = handler->next;
        free(handler);
        handler = next;
    }
}

/* Creates a new timer_handler */
static struct timer_handler *timer_handler_new(timer_callback_t callback, void *data, uint64_t delay){
    struct timer_handler *h = malloc(sizeof(struct timer_handler));
    if (h == NULL) {
        return NULL;
    }

    /* Set values */
    h->callback = callback;
    h->data = data;
    h->next = NULL;
    h->delay = delay;
    h->expire_time = time_stamp() + delay;

    /* id 0 indicates error, so we cannot use it */
    if (current_id == 0) {
        current_id = 1;
    }
    h->id = current_id++;

    return h;
}

static uint64_t frequency_to_microseconds(uint64_t time) {
    return time * 1000000 / EPIT_FREQUENCY;
}

static uint64_t microseconds_to_frequency(uint64_t time) {
    return time * EPIT_FREQUENCY / 1000000;
}

/* Inserts a timer_handler into the linked list */
static void insert(struct timer_handler *to_insert) {
    struct timer_handler *curr = handler_head;

    if (curr == NULL) {
        /* Linked list is currently empty */
        handler_head = to_insert;
        /* Set new interrupt time */
        set_timer_interrupt(to_insert->expire_time - time_stamp());
    } else if (curr->expire_time > to_insert->expire_time) {
        /* New timer inserted to front */
        to_insert->next = curr;
        handler_head = to_insert;
        /* Set new interrupt time */
        set_timer_interrupt(to_insert->expire_time - time_stamp());
    } else {
        /* New timer inserted somewhere in the middle (or end) of the list */
        bool inserted = FALSE;
        while (curr->next != NULL && !inserted) {
            if (curr->next->expire_time > to_insert->expire_time) {
                to_insert->next = curr->next;
                curr->next = to_insert;
                inserted = TRUE;
            }
            curr = curr->next;
        }

        /* Insert at the end if it has not been inserted yet */
        if (!inserted) {
            to_insert->next = curr->next;
            curr->next = to_insert;
        }
    }
}

/* Remove head of linked list and returns it */
static struct timer_handler *remove_head() {
    struct timer_handler *ret = handler_head;
    if (ret == NULL) {
        return NULL;
    }

    handler_head = handler_head->next;
    if (handler_head != NULL) {
        /* Set new interrupt time of next timer */
        set_timer_interrupt(handler_head->expire_time - time_stamp());
    } else {
        /* Else set default interrupt time */
        set_timer_interrupt(DEFAULT_INTERRUPT_TICK);
    }

    return ret;
}

/* Sets the next timer interrupt time */
void set_timer_interrupt(timestamp_t us) {
    load_register_value = microseconds_to_frequency(us);
    /* Limit at DEFAULT_INTERRUPT_TICK */
    if (load_register_value > DEFAULT_INTERRUPT_TICK) {
        load_register_value = DEFAULT_INTERRUPT_TICK;
    }
    /* Update current time */
    current_time += frequency_to_microseconds(epit1_timer->load - epit1_timer->counter);

    /* Set new load value */
    epit1_timer->load = load_register_value;
}

/* Initialises the timer */
void timer_init(void *epit1_vaddr, void *epit2_vaddr) {
    epit1_timer = epit1_vaddr;
    epit2_timer = epit2_vaddr;

    uint32_t control = (1 << EPIT_CLKSRC |
                        3 << EPIT_OM |
                        1 << EPIT_STOPEN |
                        1 << EPIT_WAITEN |
                        1 << EPIT_DBGEN |
                        1 << EPIT_IOVW |
                        0 << EPIT_SWR |
                        0 << EPIT_PRESCALAR |
                        1 << EPIT_RLD |
                        1 << EPIT_OCIEN |
                        0 << EPIT_ENMOD |
                        0 << EPIT_EN);
    epit1_timer->control = control;
    epit2_timer->control = control;
}

int start_timer(seL4_CPtr interrupt_ep) {
    /* Destroy all handler */
    handler_queue_destroy();

    _irq_ep = interrupt_ep;

    _timer_irqs[0].irq = EPIT1_IRQ;
    _timer_irqs[0].cap = enable_irq(EPIT1_IRQ, _irq_ep);

    _timer_irqs[1].irq = EPIT2_IRQ;
    _timer_irqs[1].cap = enable_irq(EPIT2_IRQ, _irq_ep);

    epit1_timer->control |= 1 << EPIT_EN;
    epit2_timer->control |= 1 << EPIT_EN;

    set_timer_interrupt(DEFAULT_INTERRUPT_TICK);
    epit2_timer->load = MAX_INTERRUPT_TICK;

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
    /* None in list */
    if (curr == NULL) {
        return CLOCK_R_FAIL;
    }

    /* One in list */
    struct timer_handler *prev = curr;
    curr = curr->next;
    if (curr == NULL) {
        if (prev->id == id) {
            handler_head = NULL;
            free(prev);
            return CLOCK_R_OK;
        } else {
            return CLOCK_R_FAIL;
        }
    }

    /* Multiple items in list */
    while (curr->next != NULL) {
        if (curr->next->id == id) {
            struct timer_handler *del = curr->next;
            curr->next = del->next;
            free(del);
            return CLOCK_R_OK;
        }
        prev = curr;
        curr = curr->next;
    }

    /* Check if its the very last item on the list */
    if (curr->id == id) {
        prev->next = NULL;
        free(curr);
        return CLOCK_R_OK;
    }

    return CLOCK_R_FAIL;
}

int timer_interrupt(void) {
    int err;

    if (_irq_ep == seL4_CapNull) {
        return CLOCK_R_FAIL;
    }
    
    /* Keep track of current time */
    current_time += frequency_to_microseconds(load_register_value);

    /* Handle callback */
    struct timer_handler *handler = handler_head;

    /* if the queue is empty, we are using default tick */
    if (handler_head == NULL) {
        /* Acknowledge */
        epit1_timer->status = 1;
        err = seL4_IRQHandler_Ack(_timer_irqs[0].cap);
        if (err) {
            return CLOCK_R_FAIL;
        }
        return CLOCK_R_OK;
    }

    /* Set new timer interrupt for timers with long delays */
    if (microseconds_to_frequency(handler->delay) > DEFAULT_INTERRUPT_TICK) {
        set_timer_interrupt(handler->expire_time - time_stamp());
    }

    while (handler != NULL && handler->expire_time - INTERRUPT_THRESHOLD <= time_stamp()) {
        remove_head();
        handler->callback(handler->id, handler->data);
        free(handler);
        handler = handler_head;
    }
    
    /* Acknowledge */
    epit1_timer->status = 1;
    err = seL4_IRQHandler_Ack(_timer_irqs[0].cap);
    if (err) {
        return CLOCK_R_FAIL;
    }
    return CLOCK_R_OK;
}

timestamp_t time_stamp(void) {
    timestamp_t counter = frequency_to_microseconds(epit1_timer->load - epit1_timer->counter);
    return current_time + counter;
}

int stop_timer(void) {
    /* Disable timer */
    epit1_timer->control |= 0 << EPIT_EN;
    epit2_timer->control |= 0 << EPIT_EN;

    /* Destroy all handler */
    handler_queue_destroy();

    return CLOCK_R_OK;
}
