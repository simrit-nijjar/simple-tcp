# simple-tcp
CPSC 317 - Programming Assignment 04

## Overview

The purpose of this assignment is to give you an opportunity to build a simplified version of a transport service that provides a reliable streamed delivery service similar to TCP. You will use this service to transfer a file from a sender to a receiver.

This protocol, named STCP (Simplified Transport Control Protocol) will support a window based flow-control mechanism, but it will not have congestion control. In addition, unlike TCP, data will only flow from the sender to the receiver (ACKs will flow in the reverse direction). In broad terms the protocol has a setup phase, a data transfer phase, and a shutdown phase. In this assignment you will only have to write the sender side, the receiver has already been written for you.

We believe that the receiver is reasonably robust but it may contain bugs, so don't be surprised if it occasionally crashes if your application isn't working properly.

## SCTP Specification


On the wire, an STCP packet, which is encapsulated in a UDP packet, uses the same header as TCP defined by the following C structure:

```
typedef struct tcpheader {
    unsigned short srcPort;		// Not used (should always be 0)
    unsigned short dstPort;		// Not used (should always be 0)
    unsigned int seqNo;
    unsigned int ackNo;
    unsigned char dataOffset;		// Not used (should always be 5)
    unsigned char flags;
    unsigned short windowSize;
    unsigned short checksum;
    unsigned short urgentPointer;	// Not used (should always be 0)
} tcpheader;
```

Within the `tcpheader` structure the fields have the same meaning as in TCP. A few of the fields are described below:

- The only bits in the flags field that will be used in your implementation are the `SYN`, `FIN`, `RST`, and `ACK`. For convenience, we often refer to a packet that contains a particular flag whose value is 1 by the name of that flag bit. So, we speak of `SYN` packets, `FIN` packets, `RST` packets, and `ACK` packets. However, a packet may have multiple of these flag bit set (for example, an acknowledgement of an initial `SYN` packet will have both the `SYN` and `ACK` flags set.
- The `windowSize` field specifies, in bytes, the receiver's advertised receive window size. This field is only interesting for packets sent by the receiver.
- The `seqNo` and `ackNo` fields are as in TCP. As in TCP, the `SYN` and `FIN` flags count as one byte each.
Your program must be prepared for the situation when the maximum sequence number is reached and the sequence number wraps around (i.e. becomes smaller than its current value.) In addition, just as in TCP the initial sequence number needs to be randomly selected.

- The checksum field is used for corruption checking, and is computed using the standard Internet checksum (a function computing this is provided for you).

During protocol processing the receiver can be in four possible states: `CLOSED`, `LISTEN`, `ESTABLISHED` and `TIME_WAIT`. Initially, it is in the `CLOSED` state and transitions to the `LISTEN` state. While in the `LISTEN` state, the receiver waits for a SYN packet to arrive on the specified port number. When it does, it responds with an ACK, and moves to the `ESTABLISHED` state. After the sender has transmitted all data, it will send a FIN packet to the receiver. Upon receipt of the FIN, the receiver moves to the `TIME_WAIT` state after sending an ACK. Similar to TCP, it remains in `TIME_WAIT` for four seconds before re-entering the `CLOSED` state.

The sender can be in five possible states: `CLOSED`, `SYN_SENT`, `ESTABLISHED`, `CLOSING` and `FIN_WAIT`. Like the receiver, the sender starts in the `CLOSED` state. It initiates a connection by sending a SYN packet (to the receiver's UDP port), thus entering the `SYN_SENT` state.

In the common case in which the SYN is correctly acknowledged, the sender enters the `ESTABLISHED` state and starts transmitting packets. When the sending application (sitting above STCP) is finished generating data, it issues a "close" operation to STCP. This causes the sender to enter the `CLOSING` state. At this point, the sender must still ensure that all buffered data arrives at the receiver reliably, i.e. it must make sure that all data bytes are acknowledged. Upon verification of successful transmission, the sender sends a FIN packet with the appropriate sequence number and enters the `FIN_WAIT` state. Once the FIN packet is acknowledged, the sender re-enters the `CLOSED` state.

The sender must be able to send multiple DATA packets before receiving the corresponding ACK packet. Just how much data is allowed to be sent is based on the value specified in the window field of the most recently received ACK. Once the window limit has been reached, the sender is to wait for new ACK packets until enough bytes are acknowledged and the window has opened to permit the sending, and hence receiving of additional data.

Any packet, including the SYN or FIN packets, that is not acknowledged after a timeout must be retransmitted. You should continue to retransmit packets forever (or until you receive a permanent error from the underlying socket). The retransmission time must take into consideration the actual time when the the oldest unacknowledged packet was sent.(i.e., If some time elapses between when a packet was originally sent and the existence of an ACK is tested, the elapsed time must be deducted from the time to wait.) The timeout period is to be fixed at one second for the first attempt, two seconds for the second attempt, and four seconds for the third and subsequent attempts.

If the sender receives three consecutive ACK packets with the same value for `ackNo`, and a DATA packet with the same `seqNo` has already been sent to the receiver, it is to immediately resend the corresponding DATA packet (similar to TCP's "Fast Retransmission"). Before this packet is sent, though, the sender is to process any ACK packets already in the buffer (i.e. it should call the receiving function waiting for a zero timeout repeatedly until no packet is returned). After the DATA packet is resent, the sender is to wait for the appropriate timeout period (as described above) for a new ACK. No other DATA packets are to be sent until this ACK arrives, including any other packet that may timeout while the sender is waiting for the ACK. If an ACK with a greater `seqNo` is received, the sending process should resume as usual. If new ACKs arrive with the same `seqNo`, these ACKs should be ignored, and should not trigger a new fast retransmission process. Normal retransmissions and timeouts still apply (but only for the affected DATA packet), although the fast retransmission should be handled as if a timeout happened, thus changing the timeout to two seconds.

The SYN transmission also includes the initial sequence number (ISN) of the conversation. The ISN is to be randomly chosen from the valid range of possible sequence numbers. Any following packet will base its `seqNo` field on this number. The ACK to the SYN will have the sequence number equal to ISN + 1, as should the first data packet.

In the event that one party detects that the other is misbehaving, it will reset the connection by sending a RST packet. For example, one easy way in which a receiver can misbehave is to acknowledge a packet that has not yet been transmitted.

Any packet that is received by either the receiver (SYN, FIN, DATA, RST) or the sender (ACK, RST) is to be checked for integrity, by verifying the `checksum` field. If the field has an incorrect value, the packet should be ignored, as if it was not received.

The receiver may change the advertised window at any time by sending a new value for the `windowSize` field in an ACK packet. The sender is responsible for reading this field and updating its internal window to accommodate the data bytes required by the advertised window. This value is initialized by the acknowledgement to the SYN packet, and may change at any time during the established connection. Since the window size can be changed at any time by the receiver, it is conceivable that the sender could reduce the window size by more than the amount of data received. For example, 1000 bytes could be sent but the window size could be reduced by more than 1000 bytes in the coresponding ACK. If such a situation were to occur it is possible that the sender could have in flight more bytes than can fit into this revised window. That is OK provided the sender obeys the new window size and doesn't send any more data.

## Implementation Constraints and Suggestions

You have been provided with an implementation of the STCP receiver process in C; you must write the sender. A skeleton for implementing the sender has been provided and is in the file `sender.c`. Feel free to change the implementation of the main function. However, the functionality of the functions `stp_open`, `stp_send` and `stp_close` is to be exactly as specified below and in the corresponding source code comments.

In the provided skeleton, applications will invoke the sender's functionality by means of the following application programmer's interface (API) consisting of the following three routines:

    stcp_ctrl_blk *stcp_open(char *destination, int sendersPort, int receiversPort);
    int stcp_send(stcp_ctrl_blk *stcp_cb, unsigned char *data, int length);
    int stcp_close(stcp_ctrl_blk *stcp_cb);

The first function is called once by the application at the sending side to open and establish an STCP connection to a remote receiver. The second function is called repeatedly by the application to transmit data across an established connection, and the final function is used by the application when it has no more messages to transmit, and wishes to cleanly close the connection. The application uses the API to call the STCP routines. You will write the definition to these routines and use them to transfer the contents of a file to a remote receiver.

To manage your protocol, you will need a global and central "structure" to help keep track of things like state transitions, sliding windows and buffers. This structure is declared in `stcp_ctrl_blk` and can be changed as needed. You do not need any additional global data structures. Ideally you should have no global data.

A typical protocol implementation would consist of many threads of control, but in this assignment things have been simplified so that a single-threaded approach is sufficient to implement the sender's functionality.

The provided Makefile will compile and build the `sender` application; `sender` is the application that you are developing. The `receiver` application is precompiled. We will provide executables for Linux on Intel processors, and MacOS (with both Apple and Intel processors). You should create a symbolic link from the version of the receiver executable that runs on your hardware to the name `receiver`. For example, if you are running on Linux, do:

    ln -s receiver_linux receiver

On MacOS, to execute a downloaded executable (which the receiver will be) you must first open it in the Finder: navigate to the directory where you have downloaded it, right click on it, and then select Open. MacOS will ask you if you are sure, confirm your intention and then you will be able to run it from any context including the provided scripts.

`receiver` will receive the file that your sender sends to it. It takes three command line arguments, which specify how to interact with the sender and one additional optional argument that specifies how the `receiver` should introduce errors in the sent and received packets (more on this below). This is an application which expects to be sent a file, and uses the receive-side API to call the stcp_routines (provided) to get the file, then writes it to a file called "OutputFile". The `receiver` only accepts one file and then exits.

Your implementation may assume the computer is running the IPv4 protocol. You should **NOT** deal with IPv6 because the receiver only works on IPv4.

If the sender is stuck in the same state for a long time, or crashes in the middle of transmission, the receiver waits for 3 "infinite" timeout durations and then aborts the test with an error.

Except for `sender.c`, you are not allowed to change any other source file. You will only be submitting your `sender.c` file to the autograder.

There is no restriction regarding what the sender prints on the default output stream. You may use it as you wish, for debugging or logging purposes.

Remember that the requested file may contain binary information, so a null byte does not necessarily represent the end of the data (i.e. do not use string functions like strlen, strcpy or strcat to deal with the packets).

The instructors have planned on scheduling a tutorial during the course of the assignment to work on a state-machine representation of STCP. Prefer working on the assignment independently and do not wait for the tutorial.

You may discuss ideas or concepts of this assignment with your classmates, but you must do your work alone. You are not allowed to show your work, even for discussion or explanatory purposes to other classmates. Don't post your code or the code from starter on any public repository, email list, newsgroup or forum.

Your code is to be developed for and run on the department Linux machines (e.g. remote, pender, etc.). You may do your development in any environment you choose, but the autograder testing environment is Linux like the department Linux servers.

Style and comments are part of the evaluation, so keep your code clear, clean and well-documented. Your code should be easy to read, with blank lines delineating blocks of functionality, and avoiding long lines. Use proper comments and define clear names for variables and functions. Use functions to deal with long blocks of code and repeated functionality. Avoid global variables. You are expected to properly close all open resources (sockets, files) and free all dynamically-allocated memory space.

### Testing

To test your code, a receiver has been provided in the set of files you download for this assignment. Note that since UDP is being used packets can always be lost or delivered out of order. However, the chance of that on the department networks or on your own machine is very small. As a result the receiver has functionality to simulate lost packets and out of order packet delivery. The desired "errors" in packet delivery are specified in a "script" file which is provided as the optional fourth argument when the receiver program is executed.

    % ./receiver sourceHost sendPort recvPort [scriptFile] [randomSeed]

Where:

  - `sourceHost` - the name of the host to receive data from;
  - `sendPort` - the port number the sending host will be sending data from;
  - `recvPort` - the port this application will use to receive data on;
  - `script` - the name of a text file that contains instructions on how the sender should drop or delay incoming or outgoing packets.
  - `randomSeed` - an integer to be used as a seed for probabilistic operations like seqNo, packet loss, corruption. If unset, the current time will be used.

The receiver can also be invoked without the sourceHost, sendPort and recvPort options, in which case they will default to "localhost" and two port numbers that are generated based on your Linux user-id.

    % ./receiver [scriptFile]

Here are a few sample `receiver` commands:

    % ./receiver localhost 4562 4182

In this situation the `receiver` is expecting packets to arrive from the local machine. The packets will come from port 4562 and be accepted on port 4182.

    % ./receiver localhost 4562 4182 delaysyn.script

In this situation the `receiver` is expecting packets to arrive from the local machine. The packets will come from port 4562 and be accepted on port 4182. The file `delaysyn.script`, which is included in your starter code, indicates that the initial SYN packet sent to the receiver, and the initial SYN packet that the receiver sends to the sender will be delayed by 900 milliseconds and 300 milliseconds, respectively.

    % ./receiver probpointtwocorrupt.script

In this situation the `receiver` is expecting packets to arrive from the local machine, using the default ports derived from your uid. The script sets the probabily of lost, corrupt, or reordered packets to 0.2 (or 20%).

The command to run the `sender` with the provided main function takes the host name of the receiver, the port to send to, the port to send from, and the file to transfer. A typical sender command would look like the following:

    % ./sender localhost 4562 4182 SendFileName

Note that source and destination port numbers are reversed in the two calls so that the programs plug together naturally. Note also that this behaviour corresponds to the provided skeleton of the sender, **DO NOT CHANGE THIS**.

The `sender` can also be invoked without the sourceHost, `recvPort` and `sendPort` options, in which case they will default to "localhost" and two port numbers that are generated based on your Linux user-id. These port arguments will be reversed so that they match the `receiver` ports if you use the default ports for both commands.

    % ./sender SendFileName
### Simplifications

You do not need to worry about the following aspects of the implementation:

  - Round-trip time (RTT) estimation. Any packet not acknowledged within one, two or four seconds is considered lost and must be retransmitted.
  - Congestion control. You only need to worry about overrunning the receiver's advertised window, but ignore network congestion.
  - The CLOSED state does not need to be implemented at the sender. Upon start-up, just have the sender immediately send a SYN and enter SYN_SENT. Upon being satisfied that the connection is terminated the process will exit.
  - The provided receiver does not implement the FIN_WAIT state. This means that if the sender sends a FIN and its ACK is lost, the receiver will be closed when the next two attempts to send a FIN are made, and the connection will finally be reset. You don't need to worry about this scenario.

On the other hand, don't oversimplify. You will need to be able to deal with:

  - Flow control. Make sure you obey the advertised window constraints and its changes.
  - Multiple data packets in flight. If the window is not full, you are not supposed to wait for an ACK before sending the next packet.
  - Retransmission of packets which have timed out.
  - Sequence number wraparound. Your sender is to choose a random ISN and to deal with the problem of wraparound. Note that the receiver will independently choose its initial sequence number and you will need to deal with wrap around if it chooses an initial sequence number that is near the end of the sequence number space.
  - Fast retransmission

Submissions that do not follow the spirit of the assignment may be penalized. We may also add new test cases in the future to identify such instances. One example is input file handling - reading the same byte from the file multiple times or holding all of the file's contents in memory unnecessarily does not scale.

### Plan of Attack

Don't try to implement this assignment all at once. Instead incrementally build the solution. A suggested strategy is to:

  - Read all the provided .c and .h files to make sure you understand what functions have been provided for you and what you need to implement.
  - Start simple and initially assume that packets are not lost. You may start with a send-and-wait implementation. Even though this may pass a subset of testcases, this is not enough.
  - Start by transferring a file that will fit in one data packet. Work to larger files that will fit in a commonly advertised window. Work to even larger files that require a sequence number wraparound.
  - When sending a file you should send as many packets as possible before waiting for the ACKs. Initially you can only wait for ACKs if the window is full.
  - Check for ACKs without waiting for new ones (use a zero timeout or use non-blocking sockets). You will need to do this to deal with fast retransmission requirements.
  - Only after you have file transfers working should you start dealing with lost packets and out of order packets.
  - Write small pieces of code and test them thoroughly before going to the next stage. For example write and test the code to setup a connection before transferring any data.

## Script Files

Once you have your reliable transport working assuming that the underlying network doesn't drop, re-order, or corrupt any segments, you will want to ensure that it is truly reliable in the face of problems. The script file that you provide as an optional argument to the `receiver` program can inject failures. Each line of the script file can contain a specification of one kind of failure to inject. Look at the provided script files for examples. There are two forms of failures that it can inject.

### Random failures

You can specify a failure probability for incoming or outgoing segments (or both) and for each "type" of segment. The syntax is:

    [drop | corrupt | swap] [in | out] [syn | fin | ack | data] percentage

`in` or `out` specifies whether the segment is incoming or outgoing (from the receiver's point of view). If neither is specified, then segments going either direction will be affected.

`drop`, `corrupt`, or `swap` specifies whether the segment will be dropped (never delivered), corrupted (one bit will be changed, so the checksum should detect this corruption), or swapped (with the next segment).

`syn`, `fin`, `ack`, or `data` specifies the type of segment that should be affected. You can specify different probabilities for each type of segment. A packet is classified into exactly one of these types - the most specific one from [DATA, FIN, SYN, ACK] in decreasing order of specificity. For instance, a FIN-ACK is classified as a FIN-type; an ACK with a payload becomes a DATA.

`percentage` indicates the probability that a given segment will be affected.

To test your receive window management functionality, you can separately specify a probability that the application will consume data slowly using:

    consume percentage

When data is consumed slowly, the `receiver` will advertise a smaller receive window to the sender
Specific failures

You can also specify that a particular segment should be affected. This is done using a specification like:

    [in | out] [syn | fin | ack | data] ordinal-number [delay
          delay-in-milliseconds | drop]

The `in`, `out`, `syn`, `fin`, `ack`, and `data` are as in the random failure specification. The ordinal-number specifies which segment should be affected (segments are numbered starting at 1 in each direction). Affected segments can be delayed for the specified number of milliseconds after which they will be processed normally, or they can be dropped.

## Process Analysis

To get started, draw event response diagrams for at least the following events. The purpose of these diagrams is to give you a clear idea of how to handle the different scenarios. The scenarios to draw the diagrams for are:

  1. A transfer that has no lost packets and completes;
  1. `SYN` packet is lost once;
  1. `ACK` to a `SYN` packet is lost once;
  1. `SYN` packet is lost twice;
  1. Second `DATA` packet is lost once;
  1. `ACK` to second `DATA` packet is lost once;
  1. Second `DATA` packet is lost twice;
  1. Second `DATA` packet is lost three times;
  1. `FIN` packet is lost once;
  1. `ACK` to `FIN` packet is lost once;

All diagrams should include the entire connection, and not just the affected event, e.g. a lost `FIN` diagram should include all the steps since the `SYN` packet was sent. All packets shown in the diagram should include the sequence number used in the packet. Clearly indicate what is the number of data bytes included in each data packet. Your connection should include at least five data packets, and you should consider that the sender is able to send at least three data packets before an `ACK` is received for the first data packet, and the window size is large enough to accommodate this. In cases in which a timeout occurs, clearly specify where the timeout starts and ends.
