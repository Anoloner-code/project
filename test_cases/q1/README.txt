Q1 Sample Test Cases

Each Q1 input file uses the following format:

Line 1:  P
Line 2:  M
Line 3:  N
Line 4:  num_orders
Line 5:  T
Line 6:  token counts for the T token types
Line 7:  token pairs required by each Encoder

Meaning of fields:
- P
  Number of Quantizers / Encoders in the pipeline.

- M
  Capacity of Buffer A.

- N
  Capacity of Buffer B.

- num_orders
  Number of orders to be processed.

- T
  Number of token types.

- token counts
  A line containing T integers. The i-th integer gives the number of available
  tokens of type i.

- token pairs required by each Encoder
  A line containing P token pairs. Each pair (a,b) means the corresponding
  Encoder must acquire one token of type a and one token of type b before
  encoding a packet.

Example:
3
4
4
90
3
2 2 2
(0,1) (1,2) (0,2)

Notes:
1. These files are sample inputs only.
2. Students are responsible for generating and processing the required number of orders.
3. Correct sentinel propagation and thread termination are part of the required behavior.
4. The provided cases are not exhaustive.