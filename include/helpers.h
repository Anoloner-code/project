#ifndef HELPERS_H
#define HELPERS_H

typedef enum {
    OP_Q1_QUANTIZE,
    OP_Q1_ENCODE,
    OP_Q1_LOG,
    OP_Q2_WORKER_READ,
    OP_Q2_MANAGER_HANDLE,
    OP_Q2_SUPERVISOR_UPDATE
} SimOp;

/* Sleep for the given number of milliseconds. */
void delay_ms(unsigned int ms);

/* Simulate processing time for a specific operation type. */
void simulate_work(SimOp op);

#endif