# Updated Submission Notes

This README supplements the existing top-level `README.txt` and reflects the updated requirements previously applied to Questions 1, 2, and 3.

## How to Compile

The project uses the provided `makefile`.

Build Question 1:
```sh
make pipeline
```

Build Question 2:
```sh
make pipeline2
```

Build Question 3:
```sh
make pipeline3
```

## How to Run

Question 1:
```sh
./pipeline test_cases/q1/case1.txt
```

Question 2:
```sh
./pipeline2 test_cases/q2/case1.txt
```

Question 3:
```sh
./pipeline3 test_cases/q3/case1.txt
```

## Notes for Updated Requirements

### Question 1

- The packet-level `token_type` field was removed to match the updated specification.
- Encoders still use their fixed per-thread token requirements `(tA_i, tB_i)`.
- The pipeline uses bounded buffers with semaphores and a clean sentinel-based shutdown protocol.

### Question 2

- The synchronization policy implemented is:
  `Supervisor (write) > Manager (write) > Worker (read)`
- Workers may read concurrently only when no writer is active and no waiting higher-priority writer blocks them.
- Managers and Supervisors both require exclusive access.

### Question 3

- The priority field is no longer required by the updated specification.
- The implemented scheduling set is:
  - FCFS
  - Preemptive SJF
  - RR `(q=3)`
  - RR `(q=6)`
  - MLFQ
- The removed `Priority (Preemptive)` algorithm is not included.

## Known Limitations / Incomplete Sections

- The top-level `makefile` does not provide one command that builds all three programs at once. Use the individual targets `make pipeline`, `make pipeline2`, and `make pipeline3`.
- Some bundled sample materials in the repository may still reflect older wording from earlier specifications. In particular, some older Q3 sample input files still include a legacy fourth column. `solution3` accepts that extra field for backward compatibility and ignores it.
- Question 2 is concurrent, so the exact print order can vary between runs. The required behaviors should still appear, but the output does not attempt to force one exact sample trace.
- This repository currently contains code and test inputs only. It does not include the written report answers for the report questions.

## Validation Summary

The updated solutions were checked as follows:

- Q1 builds and runs on the bundled `q1` cases, producing the expected number of logger lines.
- Q2 builds and demonstrates concurrent reads, worker blocking behind a waiting supervisor, and supervisor priority over managers.
- Q3 builds and runs with the revised algorithm set and updated input handling.
