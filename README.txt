A helper module (helpers.c / helpers.h) is provided for simulated processing delays.

The helper provides:
- delay_ms(unsigned int ms): sleep for a specified number of milliseconds
- simulate_work(SimOp op): simulate the processing time of a specific operation type

For Q1 and Q2, students may use simulate_work(...) when modeling active work,
such as quantization, encoding, logging, directory reading, manager handling,
and directory updates.

These delays are part of the simulation workload only. They must not be used as
a substitute for proper synchronization. In particular, students should avoid
placing unnecessary delays inside critical sections protected by mutex locks.

For Q3, real-time delay must not be used. Scheduling behavior should be simulated
using logical time derived from the input.