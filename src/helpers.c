#include "helpers.h"

#include <time.h>
#include <errno.h>

void delay_ms(unsigned int ms) {
    struct timespec req, rem;
    req.tv_sec = ms / 1000;
    req.tv_nsec = (long)(ms % 1000) * 1000000L;

    while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
        req = rem;
    }
}

void simulate_work(SimOp op) {
    switch (op) {
        case OP_Q1_QUANTIZE:
            delay_ms(20);
            break;
        case OP_Q1_ENCODE:
            delay_ms(30);
            break;
        case OP_Q1_LOG:
            delay_ms(10);
            break;
        case OP_Q2_WORKER_READ:
            delay_ms(20);
            break;
        case OP_Q2_MANAGER_HANDLE:
            delay_ms(30);
            break;
        case OP_Q2_SUPERVISOR_UPDATE:
            delay_ms(40);
            break;
        default:
            delay_ms(10);
            break;
    }
}