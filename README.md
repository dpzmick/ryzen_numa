# 2 socket xeon, even on socket 0, odd on socket 1

```
fast tests
( 0-> 2) writer  14.13 GiB/s
( 0-> 2) reader  14.13 GiB/s
( 2-> 4) writer  13.98 GiB/s
( 2-> 4) reader  13.98 GiB/s
( 4-> 6) writer  13.58 GiB/s
( 4-> 6) reader  13.58 GiB/s
( 1-> 3) writer  13.30 GiB/s
( 1-> 3) reader  13.30 GiB/s
( 3-> 5) writer  13.69 GiB/s
( 3-> 5) reader  13.69 GiB/s
( 5-> 7) writer  13.65 GiB/s
( 5-> 7) reader  13.65 GiB/s

slow tests
( 0-> 3) writer  10.58 GiB/s
( 0-> 3) reader  10.56 GiB/s
( 3-> 2) writer  11.22 GiB/s
( 3-> 2) reader  11.19 GiB/s
( 2-> 5) writer  11.34 GiB/s
( 2-> 5) reader  11.28 GiB/s
( 5-> 4) writer  11.36 GiB/s
( 5-> 4) reader  11.32 GiB/s
( 4-> 7) writer  11.01 GiB/s
( 4-> 7) reader  10.99 GiB/s
( 7-> 6) writer  11.35 GiB/s
( 7-> 6) reader  11.31 GiB/s
```

# single socket amd ryzen 3800x
Core complex 0: Cores 0, 1, 2, 3
Core complex 1: Cores 4, 5, 6, 7

```
fast tests
(0->1) writer  10.57 GiB/s
(0->1) reader  10.57 GiB/s
(1->2) writer  10.62 GiB/s
(1->2) reader  10.62 GiB/s
(2->3) writer  10.51 GiB/s
(2->3) reader  10.51 GiB/s
(4->5) writer  10.16 GiB/s
(4->5) reader  10.16 GiB/s
(5->6) writer  10.17 GiB/s
(5->6) reader  10.17 GiB/s
(6->7) writer  10.26 GiB/s
(6->7) reader  10.26 GiB/s

slow tests
(0->4) writer   9.37 GiB/s
(0->4) reader   9.37 GiB/s
(4->1) writer   9.21 GiB/s
(4->1) reader   9.21 GiB/s
(1->5) writer   9.32 GiB/s
(1->5) reader   9.31 GiB/s
(5->2) writer   9.37 GiB/s
(5->2) reader   9.36 GiB/s
(2->6) writer   9.32 GiB/s
(2->6) reader   9.31 GiB/s
(6->3) writer   9.18 GiB/s
(6->3) reader   9.17 GiB/s
(3->7) writer   9.28 GiB/s
(3->7) reader   9.28 GiB/s
```