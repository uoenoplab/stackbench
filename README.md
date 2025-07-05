## Opening Up Kernel-Bypass TCP Stacks

Custom source code and detailed hardware and software configuration 
used in the
[paper](https://www.usenix.org/conference/atc25/presentation/awamoto)
titled above.

## Modified Stacks
- src/mtcp
- src/tas

Modifications described in Section 2.
Build process follows the original code.

## Server tool
- src/httpd

nophttpd described in Section 3.

## Client tool
- src/wrk

Improved multicore scalability with coarse-grained stat aggregation described in
Section 3.

## HW and SW configuration
- records/conf 

Filename self-explains the stack and workload.
`wsc` refers to downclocked experiment.
`reflex` refers to IX.

## Misc
- records/perf

Flame graphs mentioned in Section 8.3.
