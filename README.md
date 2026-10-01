This is a L2 token passing protocol I designed and (partially) implemented as part of my embedded systems internship. This protocol is the redesign of [v1](https://github.com/AliGhaffarian/silly-proto-v1).

# Overview of the protocol
Silly proto v2 enables different nodes of a rs-485 half duplex network to talk.

Packet layout of the protocol:

```
 0                   8                      16                      24                       31
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                  magic                     |                      len                      |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|               len (cont.)                  |                     flags                     |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|              flags (cont.)                 |                 header checksum               |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|         header checksum (cont.)            |                  data checksum                |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|           data checksum (cont.)            |                      src                      |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                src (cont.)                 |                      src (cont.)              |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                src (cont.)                 |                      dst                      |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                dst (cont.)                 |                    dst (cont.)                |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                dst (cont.)                 |                      master                   |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                master (cont.)              |                      master                   |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                master (cont.)              |                    elem count                 |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|                elem count (cont.)          |             optional object (variable length) |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
|         optional data (variable length)    |
+--+--+--+--+--+--+--+--+--+--+--+--+--+--+--+
```

Explanation of the fields:
- `magic`: A magic number to enable scanning for start of the packet and synchronization
- `len`: Length of the header + data
- `flags`: Control flags, defined in [silly_proto.h](<src/components/silly_proto/include/silly_proto.h>)
- `src`: mac address of the sender
- `dst`: mac address of the destination (needed when passing the token)
- `master`: mac address of master of the network from the `src`'s point of view
- `elem count`: number of silly proto objects encoded inside the packet

Note: A MAC address is not 64bits in length and some space is wasted by encoding it as `uint64_t`, but for the sake of simplicity I disregarded optimization.

# Protocol Diagrams

![protocol diagram](assets/diagrams/silly_proto_v2.svg)

# Notes on Building

I used clang on this project, to enable clang you need to do the following before building:
```sh
export IDF_TOOLCHAIN=clang
export PATH=\"/home/work/.espressif/tools/esp-clang/esp-20.1.1_20250829/esp-clang/bin:$PATH\" # your path is almost certainly different
```

# Implementation Details

The protocol implementation is broken into multiple components:

- Receiver: A FreeRTOS task that reads packet from the given uart port, sends them to the emitter task with no furthur computation
    - Implemented: Yes
- Sender: A FreeRTOS task that waits for data to arrive on it's input queue, and sends the data over the given uart. Sends collision event to the emitter if collision is detected while sending.
    - Implemented: Yes
- Core: Library of silly proto, provides encodings, decoding, and checksum verifications
    - Implemented: Yes
- Token passer: A synchronous logic which passes the token to the next node. Is started automatically when reference count of the token hits zero. Causes the caller to block until the token is passed.
    - Implemented: Yes
- Emitter: A FreeRTOS task that takes input from Sender and Receiver tasks, as well as timer callbacks, translates the input into one or multiple events, publishes the events to the interested subscribers. Provides an API to register callbacks that will be called on the requested hook point.
    - Implemented: Yes
- Slave State Machine: An asynchronous state machine that implements the slave on the node. Subscribes to the emitter, uses Sender to talk.
    - Implemented: Yes, but I'm not confident with the correctness
- Master State Machine: An asynchronous state machine that implements the master on the node. Subscribes to the emitter, uses Sender to talk.
    - Implemented: No
- Node State Machine: The top level structure that holds a reference to all the other tasks, is responsible for starting and initializing everything else, holds shared context between distinct logics, provides implementation of the shared behavior between master and slave, excluding the act of token passing, provides an API to the user to read from and write to both master's and slave's tx and rx queues
    - Implemented: Yes

Note: all nodes implement both master and slave, but the master doesn't act until negotiation happens and the node wins as the master.

There is no unit tests (there are, but only two, and one of them is failing). Since I failed to start testing early, I expect around 7 hours of debugging on the implemented components.

Changes since the v1:
- The target network is half duplex
- Dynamic master negotiation is designed (but not implemented) 
    - Masters maintain a priority bit map of their capabilities, and it is the main factor of the negotiation
    - If no master capable node is up, slaves can take over too
- Token passing is decentralized
- Node addressing is based on the device's MAC address
- The protocol is broken into multiple tasks
- No task directly writes to a uart port, they instead send the data to the Sender/Receiver tasks input queue
- The protocol no longer guarantees reliable data transfer

# Version of Used Software

- ESP-IDF: v6.0.2
