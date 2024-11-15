/************************************************************************
 * Adapted from a course at Boston University for use in CPSC 317 at UBC
 *
 *
 * The interfaces for the STCP sender (you get to implement them), and a
 * simple application-level routine to drive the sender.
 *
 * This routine reads the data to be transferred over the connection
 * from a file specified and invokes the STCP send functionality to
 * deliver the packets as an ordered sequence of datagrams.
 *
 * Version 2.0
 *
 *
 *************************************************************************/


#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <math.h>
#include <pthread.h>

#include "stcp.h"

#define STCP_SUCCESS 1
#define STCP_ERROR -1

typedef struct {
    int fd;
    int state;
    unsigned int initSeq;
    unsigned int seq;
    unsigned int ack;
    unsigned int latestAck;
    unsigned int windowStart;
    unsigned int windowPos;
    unsigned int windowSize;
    pthread_t *threads;
    pthread_mutex_t* fileLock;
    pthread_mutex_t* logicLock;
} stcp_send_ctrl_blk;
/* ADD ANY EXTRA FUNCTIONS HERE */

typedef struct {
    stcp_send_ctrl_blk* cb;
    unsigned char* data;
    unsigned int length;
    unsigned int seq;
    unsigned int ack;
} thread_data;


/**
 * Validate the checksum of a packet
 * 
 * @param pkt The packet to calculate checksum for in host byte order
 * 
 * @return The calculated checksum, or -1 if the checksum is invalid
 */
int validateChecksum(packet *pkt) {
  unsigned short checksum = ipchecksum(pkt->data, pkt->len);
  if (checksum != 0) return checksum;
  return 1;
}

/**
 * Create a new segment from received data and set header to host byte order
 * 
 * @param data The tcp packet to parse
 * @param len The length of the tcp packet
 * 
 * @return The parsed packet in host byte order
 */
void parsePacket(packet* pkt, unsigned char *data, int len) {
  pkt->len = len;
  if (data != NULL) {
      memcpy(pkt->data, data, len);
  }
  pkt->hdr = (tcpheader *) pkt->data; 
  ntohHdr(pkt->hdr);
}

void initilizePacket(packet *pkt, int flags, unsigned short rwnd, unsigned int seq, unsigned int ack, unsigned char *data, int len) {
  unsigned char *buffer = calloc(1, len + sizeof(tcpheader));
  memcpy(buffer + sizeof(tcpheader), data, len);

  createSegment(pkt, flags, rwnd, seq, ack, buffer, len);
}

void* stcp_send_segment(void* arg) {
    // Cast the argument to the correct type (stcp_send_ctrl_blk and buffer info)
    stcp_send_ctrl_blk* cb = ((thread_data*)arg)->cb;
    unsigned char* data = ((thread_data*)arg)->data;
    int length = ((thread_data*)arg)->length;
    unsigned int seq = ((thread_data*)arg)->seq;
    unsigned int ack = ((thread_data*)arg)->ack;

    int fd = cb->fd; 

    pthread_mutex_lock(cb->logicLock);  
    // Allocate and copy data
    packet *pkt = calloc(1, sizeof(packet));
    packet *pktRcv = calloc(1, sizeof(packet));

    logLog("checkpoint", "Sending packet with seq %u", seq);

    // Prepare the segment and set all necessary fields
    initilizePacket(pkt, ACK, STCP_MAXWIN, seq, ack, data, length);

    // Display packet details for debugging
    dump('s', pkt, pkt->len);


    pthread_mutex_unlock(cb->logicLock);

    // Convert header fields to network byte order afterward
    htonHdr(pkt->hdr);

    // Calculate checksum on header + payload in host byte order
    pkt->hdr->checksum = ipchecksum(pkt->data, pkt->len);

    // Setup buffer to receive incoming packet
    unsigned char *buffer = calloc(1, sizeof(tcpheader));
    int timeout = STCP_INITIAL_TIMEOUT;


    // Lock if previous packets have not been sent yet
    while ((int) minus32(cb->windowPos, seq) < 0) {
      logLog("excess", "(seq %u) Waiting for previous packets to finish sending", seq);
    }

    // Lock if window is full
    unsigned int currWindowPos = minus32(seq, cb->windowStart);
    while ((int) minus32(currWindowPos, cb->windowSize) > 0) {
      logLog("excess", "(seq %u) Waiting for window to move", seq);
    }

    int sent = 0;

    // Loop to send and wait for ACK
    while (1) {        
        logLog("debug", "current window position: %u", cb->windowPos);
        logLog("info", "Sending packet with seq %u", seq);

        // If largest ack is more than the expected ack, then break
        if ((int) minus32(cb->latestAck, seq + length) >= 0) {
            logLog("success", "Received CUMUL ACK from receiver! Wanted Ack: %u :: Latest Ack: %u", seq + length, cb->latestAck);
            break;
        }


        // Send the packet and await response
        pthread_mutex_lock(cb->fileLock);
        write(fd, pkt, pkt->len);

        // Wait for ACK
        int lenRcv = readWithTimeout(fd, buffer, timeout);

        if (sent == 0) {
            cb->windowPos = plus32(seq, length);
            sent = 1;
        }

        // Update timeout
        timeout = stcpNextTimeout(timeout);
        pthread_mutex_unlock(cb->fileLock);



        if (lenRcv == STCP_READ_TIMED_OUT) {
            logLog("failure", "Timed out waiting for (?ACK %u) from receiver", seq + length);
            continue;
        }

        // Process received packet
        parsePacket(pktRcv, buffer, lenRcv);
        tcpheader* hdrRcv = pktRcv->hdr;

        pthread_mutex_lock(cb->logicLock);
        dump('r', pktRcv, pktRcv->len);
        pthread_mutex_unlock(cb->logicLock);

        // Verify checksum
        int checkSum = validateChecksum(pktRcv);
        if (!checkSum) {
            logLog("failure", "Invalid Checksum -> Received %u but expected %u", checkSum, hdrRcv->checksum);
            continue;
        }

        // Additional ACK checks
        if (hdrRcv->flags != ACK) {
            logLog("failure", "Invalid flags -> Received %u but expected %u", hdrRcv->flags, ACK);
            continue;
        }
        
        pthread_mutex_lock(cb->logicLock);
        if (hdrRcv->ackNo <= seq) {
            logLog("failure", "Invalid Ack -> Received %u but expected %u", hdrRcv->ackNo, seq + length);
            continue;
        }
        cb->windowStart = hdrRcv->ackNo;
        if (minus32(hdrRcv->ackNo, cb->latestAck) > 0) {
          cb->latestAck = hdrRcv->ackNo;
          cb->windowSize = hdrRcv->windowSize;  
        }
        pthread_mutex_unlock(cb->logicLock);


        logLog("success", "Received valid ACK from receiver! Seq: %u :: Ack: %u", hdrRcv->seqNo, hdrRcv->ackNo);
        break;
    }

    free(pkt);
    free(buffer);
    free(pktRcv);
    free(arg);

    return NULL;
}



/*
 * Send STCP. This routine is to send all the data (len bytes).  If more
 * than MSS bytes are to be sent, the routine breaks the data into multiple
 * packets. It will keep sending data until the send window is full or all
 * the data has been sent. At which point it reads data from the network to,
 * hopefully, get the ACKs that open the window. You will need to be careful
 * about timing your packets and dealing with the last piece of data.
 *
 * Your sender program will spend almost all of its time in either this
 * function or in tcp_close().  All input processing (you can use the
 * function readWithTimeout() defined in stcp.c to receive segments) is done
 * as a side effect of the work of this function (and stcp_close()).
 *
 * The function returns STCP_SUCCESS on success, or STCP_ERROR on error.
 */
int stcp_send(stcp_send_ctrl_blk *cb, unsigned char* data, int length) {
  logLog("body", "(seq %i) '%s'", cb->seq, data);
  // Allocate memory for the thread argument struct

  unsigned int seq = cb->seq + 2;
  unsigned int initSeq = cb->initSeq;
  unsigned int index = ceil((minus32(seq, initSeq)) / STCP_MSS);

  thread_data* send_arg = malloc(sizeof(thread_data));
  send_arg->cb = cb;
  send_arg->data = data;
  send_arg->length = length;
  send_arg->seq = cb->seq;
  send_arg->ack = cb->ack;

  cb->seq = plus32(cb->seq, length);
  // cb->ack = plus32(cb->ack, 1);

  // Create a new thread for sending the data

  logLog("thread", "Creating thread %d", index);
  if (pthread_create(&(cb->threads[index]), NULL, stcp_send_segment, send_arg) != 0) {
      logPerror("Error creating thread for sending data.");
  }
  return STCP_SUCCESS;
}

/*
 * Open the sender side of the STCP connection. Returns the pointer to
 * a newly allocated control block containing the basic information
 * about the connection. Returns NULL if an error happened.
 *
 * If you use udp_open() it will use connect() on the UDP socket
 * then all packets then sent and received on the given file
 * descriptor go to and are received from the specified host. Reads
 * and writes are still completed in a datagram unit size, but the
 * application does not have to do the multiplexing and
 * demultiplexing. This greatly simplifies things but restricts the
 * number of "connections" to the number of file descriptors and isn't
 * very good for a pure request response protocol like DNS where there
 * is no long term relationship between the client and server.
 */
stcp_send_ctrl_blk * stcp_open(char *destination, int sendersPort, int receiversPort) {

    logLog("init", "Sending from port %d to <%s, %d>", sendersPort, destination, receiversPort);
    int fd = udp_open(destination, receiversPort, sendersPort);
    (void) fd;

    /* YOUR CODE HERE */

    // Check for failure to open connection
    if (fd < 0) {
        logPerror("Failed to open connection");
        return NULL;
    }

    // Initialize control block
    stcp_send_ctrl_blk *cb = calloc(1, sizeof(stcp_send_ctrl_blk));

    cb->fd = fd;
    cb->windowSize = STCP_MAXWIN;

    logLog("init", "Sending initial SYN pack to receiver");

    // Initializing the SYN packet
    packet *pktSent = calloc(1, sizeof(packet));
    createSegment(pktSent, SYN, STCP_MAXWIN, 30, 0, NULL, 0);
    
    dump('s', pktSent, sizeof(tcpheader));


    htonHdr(pktSent->hdr);
    pktSent->hdr->checksum = ipchecksum(pktSent->hdr, sizeof(tcpheader));

    // Setup buffer to receive incoming packet
    unsigned char *buf = calloc(1, STCP_MTU);
    packet *pktRcv = calloc(1, sizeof(packet));

    int res;
    int timeout = STCP_INITIAL_TIMEOUT;

    cb->state = STCP_SENDER_SYN_SENT;

    // Try to send the initial SYN packet
    while (cb->state == STCP_SENDER_SYN_SENT) {
      // Send the SYN packet
      write(fd, pktSent, sizeof(tcpheader));



      // Wait for the SYN-ACK packet
      res = readWithTimeout(fd, buf, timeout);
      timeout = stcpNextTimeout(timeout);

      // Handle error cases
      if (res == STCP_READ_TIMED_OUT) {
        logPerror("Timed out waiting for SYN-ACK from receiver");
        continue;
      } else if (res == STCP_READ_PERMANENT_FAILURE) {
        logPerror("Failed to read SYN-ACK from receiver");
        return NULL;
      }

      // Process the received packet
      parsePacket(pktRcv, buf, res);
      logLog("init", "Receiving SYN-ACK from receiver");
      dump('r', pktRcv, pktRcv->len);

      tcpheader *hdrRcv = pktRcv->hdr;

      // Verify checksum
      int checksum = validateChecksum(pktRcv);
      if (!checksum) {
        logLog("init", "Invalid checksum -> Received %x but expected %x", checksum, hdrRcv->checksum);
        continue;
      }

      // Verify correct flags
        if (hdrRcv->flags != (ACK | SYN)) {
          logLog("init", "Invalid flags -> Received %x but expected %x", hdrRcv->flags, ACK | SYN);
          continue;
        }

      // Initialize the control block
      logLog("success", "Received SYN-ACK from receiver. Syn: %u :: Ack: %u", hdrRcv->seqNo, hdrRcv->ackNo);
      cb->state = STCP_SENDER_ESTABLISHED;
      cb->ack = (unsigned int) hdrRcv->seqNo + 1;
      cb->seq = hdrRcv->ackNo;
      cb->initSeq = hdrRcv->ackNo;
      cb->latestAck = hdrRcv->ackNo;
      cb->windowSize = hdrRcv->windowSize;
      cb->windowStart = hdrRcv->ackNo;
      cb->windowPos = hdrRcv->ackNo;
      
      cb->state = STCP_SENDER_ESTABLISHED;
    }

    // Free memory and return control block
    free(buf);
    free(pktSent);
    free(pktRcv);


    return cb;
}


/*
 * Make sure all the outstanding data has been transmitted and
 * acknowledged, and then initiate closing the connection. This
 * function is also responsible for freeing and closing all necessary
 * structures that were not previously freed, including the control
 * block itself.
 *
 * Returns STCP_SUCCESS on success or STCP_ERROR on error.
 */
int stcp_close(stcp_send_ctrl_blk *cb) {
    /* YOUR CODE HERE */

    // Send FIN packet to receiver
    packet *pkt = calloc(1, sizeof(packet));
    createSegment(pkt, FIN, STCP_MAXWIN, cb->seq, cb->ack, NULL, 0);
    htonHdr(pkt->hdr);
    pkt->hdr->checksum = ipchecksum(pkt->data, pkt->len);

    // Setup buffer to receive incoming packet
    unsigned char *buffer = calloc(1, sizeof(tcpheader));
    packet *pktRcv = calloc(1, sizeof(packet));

    int timeout = STCP_INITIAL_TIMEOUT;

    cb->state = STCP_SENDER_FIN_WAIT;

    // Loop to send and wait for ACK
    while (cb->state == STCP_SENDER_FIN_WAIT) {
        logLog("finish", "Sending FIN packet to receiver");

        // Send the packet and await response
        write(cb->fd, pkt, pkt->len);
        int lenRcv = readWithTimeout(cb->fd, buffer, timeout);
        timeout = stcpNextTimeout(timeout);

        if (lenRcv == STCP_READ_TIMED_OUT) {
            logLog("finish", "Timed out waiting for ACK from receiver");
            continue;
        } else if (lenRcv == STCP_READ_PERMANENT_FAILURE) {
            logPerror("Failed to read SYN-ACK from receiver");
            return STCP_ERROR;
        }

        // Process received packet
        parsePacket(pktRcv, buffer, lenRcv);
        tcpheader* hdrRcv = pktRcv->hdr;

        // Verify checksum
        int checkSum = validateChecksum(pktRcv);
        if (!checkSum) {
            logLog("failure", "Invalid Checksum -> %x is not 0", checkSum);
            continue;
        }

        // Additional ACK checks
        if (hdrRcv->flags != (ACK | FIN)) {
            logLog("failure", "Invalid flags -> Received %x but expected %x", hdrRcv->flags, ACK);
            continue;
        }
        if (hdrRcv->ackNo != cb->seq + 1) {
            logLog("failure", "Invalid Ack -> Received %d but expected %d", hdrRcv->ackNo, cb->seq + 1);
            continue;
        }

        logLog("success", "Received valid ACK from receiver!");
        cb->state = STCP_SENDER_CLOSED;
    }

    free(pkt);
    free(buffer);
    free(pktRcv);

    pthread_mutex_destroy(cb->fileLock);
    pthread_mutex_destroy(cb->logicLock);
    
    free(cb);

    return STCP_SUCCESS;
}
/*
 * Return a port number based on the uid of the caller.  This will
 * with reasonably high probability return a port number different from
 * that chosen for other uses on the undergraduate Linux systems.
 *
 * This port is used if ports are not specified on the command line.
 */
int getDefaultPort() {
    uid_t uid = getuid();
    int port = (uid % (32768 - 512) * 2) + 1024;
    assert(port >= 1024 && port <= 65535 - 1);
    return port;
}

/*
 * This application is to invoke the send-side functionality.
 */
int main(int argc, char **argv) {
    stcp_send_ctrl_blk *cb;

    char *destinationHost;
    int receiversPort, sendersPort;
    char *filename = NULL;
    int file;
    /* You might want to change the size of this buffer to test how your
     * code deals with different packet sizes.
     */
    unsigned char buffer[STCP_MSS];
    int num_read_bytes;

    logConfig("sender", "failure,success,finish,init,sender,thread,checkpoint,debug");
    // logConfig("sender", "failure,success,finish,init,sender,thread,checkpoint");
    // logConfig("sender", "failure,success,finish,init,sender,thread");
    // logConfig("sender", "failure,success,finish,init,sender");
    // logConfig("sender", "failure,success,finish,init");
    // logConfig("sender", "failure,success,finish");
    // logConfig("sender", "");
    /* Verify that the arguments are right */
    if (argc > 5 || argc == 1) {
        fprintf(stderr, "usage: sender DestinationIPAddress/Name receiveDataOnPort sendDataToPort filename\n");
        fprintf(stderr, "or   : sender filename\n");
        exit(1);
    }
    if (argc == 2) {
        filename = argv[1];
        argc--;
    }

    // Extract the arguments
    destinationHost = argc > 1 ? argv[1] : "localhost";
    receiversPort = argc > 2 ? atoi(argv[2]) : getDefaultPort();
    sendersPort = argc > 3 ? atoi(argv[3]) : getDefaultPort() + 1;
    if (argc > 4) filename = argv[4];

    /* Open file for transfer */
    file = open(filename, O_RDONLY);
    if (file < 0) {
        logPerror(filename);
        exit(1);
    }

    /*
     * Open connection to destination.  If stcp_open succeeds the
     * control block should be correctly initialized.
     */
    cb = stcp_open(destinationHost, sendersPort, receiversPort);
    if (cb == NULL) {
        /* YOUR CODE HERE */
        logPerror("Failed to open connection");
        exit(1);
    }

    pthread_mutex_t file_mutex;
    pthread_mutex_t logic_mutex;
    pthread_mutex_init(&file_mutex, NULL);
    pthread_mutex_init(&logic_mutex, NULL);
    cb->fileLock = &file_mutex;
    cb->logicLock = &logic_mutex;

    /* Start to send data in file via STCP to remote receiver. Chop up
     * the file into pieces as large as max packet size and transmit
     * those pieces.
     */
    struct stat st;

    fstat(file, &st);
    const unsigned int numPackets = ceil(st.st_size / STCP_MSS) + 1;

    logLog("debug", "number of total packets: %d", numPackets);

    pthread_t threads[numPackets];
    cb->threads = threads;

    while (1) {
        num_read_bytes = read(file, buffer, sizeof(buffer));

        /* Break when EOF is reached */
        if (num_read_bytes <= 0)
            break;

        if (stcp_send(cb, buffer, num_read_bytes) == STCP_ERROR) {
            /* YOUR CODE HERE */
            logPerror("!!!!!!!!!!!!!!!!!!!!! Failed to send data");
        }
    }

    cb->state = STCP_SENDER_CLOSING;

    for (int i = 0; i < numPackets; i++) {
        int result = pthread_join(threads[i], NULL);
        if (result != 0) {
            logLog("failure", "Failed to join thread with error code %d", result);
            exit(1);
        } else {
          logLog("success", "Thread %d has finished", i);
        }
    }

    logLog("success", "!! All threads have finished !!");

    /* Close the connection to remote receiver */
    if (stcp_close(cb) == STCP_ERROR) {
        /* YOUR CODE HERE */
        logPerror("Failed to close connection");
        exit(1);
    }

    return 0;
}
