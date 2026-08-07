# CS 5523 Replicated Shared List Project (Luis Saenz/khe699)

## Program status
Fully completed implementation in C using POSIX sockets and POSIX pthreads. 

The system runs with at least 3 server processes and 3 client processes. Each server maintains a replica of the same shared list. Clients may connect to any server and issue update operations. At the end of a normal run, all servers print identical final list contents.

## Files
- `server.c` - multithreaded replicated list server
- `client.c` - random workload client plus small admin commands for printing/shutdown
- `common.c`, `common.h` - shared socket/protocol helpers
- `Makefile` - build file
- `run_demo.sh` - convenience script for running 3 servers and 3 clients

## Synchronization strategy
Each server process is multithreaded. A new pthread is created for each accepted TCP connection.

Shared data is protected as follows:
1. The replicated list is protected by `list_mutex`.
2. The pending ordered message queue and delivery metadata are protected by `order_mutex`.
3. Threads waiting for a particular global sequence number use `order_cond`.
4. On the sequencer server, sequence-number assignment is protected by `seq_mutex`.

Invalid list positions are handled gracefully. Invalid `insert`, `delete`, or `replace` operations are ignored and logged instead of crashing the server.

## Ordering mechanism
This implementation uses a sequencer based totally ordered multicast protocol.
- Server `0` is the sequencer.
- A client may contact any server.
- If the contacted server is not server `0`, it forwards the update request to server `0`.
- Server `0` assigns a monotonically increasing global sequence number.
- Server `0` broadcasts `ORDER <seq> <operation>` to every server, including itself.
- Every server stores ordered messages in a pending queue and applies only the next expected sequence number.

This means that even if ordered messages arrive out of order, each replica applies them in the same global order.

## Compile
To compile/clean, use `make` and `make clean` as normal.

## Run manually
Open three terminals and start three servers. The port for server `i` is `base_port + i`.

```bash
./server 0 5000 3
./server 1 5000 3
./server 2 5000 3
```

Then run three clients, each generating 50 operations:

```bash
./client 1 5000 3 50 &
./client 2 5000 3 50 &
./client 3 5000 3 50 &
wait
```

After the clients finish, tell all servers to print their final lists:

```bash
./client --print-all 5000 3
```

Then shut down all servers:

```bash
./client --shutdown-all 5000 3
```

Each server prints both required formats:

```text
Indexed format:
0:Operating 1:systems 2:coordinate ...

Sentence format:
Operating systems coordinate multiple processes ...
```

Additionally, you could run it with the demonstration script included. Simply run `./run_demo.sh`


Generated values look like `C1_W17`, meaning client 1's operation 17. The demonstration script will print out the order of operations for each server, and they should all match up. This was so that its easier to debug it, and compare the lists of each server, but to meet the requirements of the assignment, the words in the list do appear in the output alongside the operations.

## Help received
I did not recieve any help from other classmates. I did refrence `The Linux Programming Interface` book as well as the Linux `man` pages.

## Comments or suggestions
None from me. 