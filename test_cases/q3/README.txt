Q3 Sample Test Cases

Each line in a Q3 input file represents one job:

<job_id> <arrival_time> <burst_time>

Fields:
- <job_id>
  A string identifier of the job.

- <arrival_time>
  A non-negative integer representing the job arrival time.

- <burst_time>
  A positive integer representing the CPU time required by the job.

Example:
J1 0 5
J2 1 3
J3 2 8

Notes:
1. These files are sample inputs only.
2. Q3 should be simulated using logical time derived from the input.
3. Real-time sleep/delay must not be used in Q3.
4. The provided cases are not exhaustive.
5. Some older sample files may still include a fourth legacy field; solution3 ignores it for backward compatibility.