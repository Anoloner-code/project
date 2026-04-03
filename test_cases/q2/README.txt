Q2 input format

Each Q2 test case begins with one configuration line:

<num_workers> <num_managers> <num_supervisors> <num_ops>

This is followed by exactly <num_ops> operation lines, each in the form:

<role> <id> <op> <item>

Where:
- <role> is one of:
  W   Worker
  M   Manager
  S   Supervisor

- <id> is the zero-based thread index within that role.
  Examples:
  W 0 means Worker-0
  M 1 means Manager-1
  S 0 means Supervisor-0

- <op> is one of:
  READ
  WRITE

- <item> is the target directory entry, for example:
  item_0001.dat

Semantics:
- Workers perform READ operations.
- Managers perform WRITE operations.
- Supervisors also perform WRITE operations, but they have higher priority than Workers.
- The exact interleaving of printed thread messages may vary across runs because the program is concurrent.

Students are responsible for validating:
- concurrent reads on the same item,
- exclusion between readers and writers,
- exclusion between writers,
- priority handling for Supervisor operations.

The provided cases are sample inputs only. They do not cover all edge cases.