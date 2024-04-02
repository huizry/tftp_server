#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <string.h>

#define TRUE 1
#define FALSE 0
// Max size of a TFTP packet.
#define MAXBUFFER 516
// Opcodes
#define RRQ 1
#define WRQ 2
#define DATA 3
#define ACK 4
#define ERR 5

// Handle alarm.
void almHndlr(int sig) {
    int stat;
    wait(&stat);
}

// Send error packet.
void sendErr(int val, int sd, struct sockaddr_in clt) {
  char errPkt[5] = {0};
  uint16_t errOpcNet = htons(5);
  uint16_t errNum = htons(val);

  memcpy(errPkt, &errOpcNet, 2);
  memcpy(errPkt + 2, &errNum, 2);

  if(sendto(sd, errPkt, 5, 0, (struct sockaddr *) &clt, sizeof(clt)) == -1) {
    perror("sendto() failed.\n");
    exit(EXIT_FAILURE);
  }
}

// Receive Read Request.
void recvRRQ(struct sockaddr_in clt, int chdSd, char* buffer) {
  // Open file for reading.
  int fd = open(buffer + 2, O_RDONLY);
  if (fd == -1) {
    sendErr(1, chdSd, clt);
    exit(EXIT_FAILURE);
  }

  int blkNum = 1;
  char flBuf[512];
  char dataPkt[516];
  uint16_t dataOpcNet = htons(3);
  int len = sizeof(clt);

  while(TRUE) {
    // Read file.
    int rc = read(fd, flBuf, 512);
    if(rc == 0) {
      break;
    }

    // Send data packet.
    uint16_t blkNumNet = htons(blkNum);
    memcpy(dataPkt, &dataOpcNet, 2);
    memcpy(dataPkt + 2, &blkNumNet, 2);
    memcpy(dataPkt + 4, flBuf, rc);
    sendto(chdSd, dataPkt, 4 + rc, 0, (struct sockaddr *) &clt, sizeof(clt));

    // Wait for ack packet.
    recvfrom(chdSd, buffer, MAXBUFFER, 0, (struct sockaddr *) &clt, (socklen_t *) &len);

    // Stop read request after finish reading.
    if(rc < 512) {
      break;
    }

    ++blkNum;
  }
}

// Receive Write Request.
void recvWRQ(struct sockaddr_in clt, int chdSd, char* buffer) {
  // Open file for writing.
  int fd = open(buffer + 2, O_WRONLY | O_CREAT | O_TRUNC, 0660);
  if (fd == -1) {
    sendErr(1, chdSd, clt);
    exit(EXIT_FAILURE);
  }

  int blkNum = 0;
  char ackPkt[4];
  uint16_t dataOpcNet = htons(4);
  int len = sizeof(clt);

  while(TRUE) {
    // Send ack packet.
    uint16_t blkNumNet = htons(blkNum);
    memcpy(ackPkt, &dataOpcNet, 2);
    memcpy(ackPkt + 2, &blkNumNet, 2);
    sendto(chdSd, ackPkt, 4, 0, (struct sockaddr *) &clt, sizeof(clt));

    // Wait for data packet.
    int n = recvfrom(chdSd, buffer, MAXBUFFER, 0, (struct sockaddr *) &clt, (socklen_t *) &len);

    int wc = write(fd, buffer + 4, n - 4);
    ++blkNum;

    // Stop write request after finish writing. Send final ack packet.
    if(wc < 512) {
      uint16_t blkNumNet = htons(blkNum);
      memcpy(ackPkt + 2, &blkNumNet, 2);
      sendto(chdSd, ackPkt, 4, 0, (struct sockaddr *) &clt, sizeof(clt));
      break;
    }
  }
}

int main(int argc, char *argv[]) {
  // Check correct # of command line arguments.
  if(argc != 3) {
    perror("# of arguments != 3.\n");
    exit(EXIT_FAILURE);
  }

  int port = atoi(argv[1]) + 1;

  int sd;
  sd = socket(AF_INET, SOCK_DGRAM, 0);
  if(sd == -1) {
    perror("socket() failed\n");
    exit(EXIT_FAILURE);
  }

  // Create UDP server.
  struct sockaddr_in svr;
  svr.sin_family = AF_INET;
  svr.sin_addr.s_addr = htonl(INADDR_ANY);
  svr.sin_port = htons(atoi(argv[1]));

  // Bind to a port number.
  if(bind(sd, (struct sockaddr *) &svr, sizeof(svr)) < 0) {
    perror("bind() failed\n");
    exit(EXIT_FAILURE);
  }

  int length = sizeof(svr);
  if (getsockname(sd, (struct sockaddr *) &svr, (socklen_t *) &length) < 0) {
    perror("getsockname() failed\n");
    exit(EXIT_FAILURE);
  }
  printf("UDP server at port number (host order) %d\n", ntohs(svr.sin_port));

  int n;
  char buffer[MAXBUFFER];
  struct sockaddr_in clt;
  int len = sizeof(clt);

  unsigned short int opcode;

  signal(SIGCHLD, almHndlr);

  while(TRUE) {
    // Receive packet from TFTP client.
    n = recvfrom(sd, buffer, MAXBUFFER, 0, (struct sockaddr *) &clt, (socklen_t *) &len);

    if (n == -1) {
      perror("recvfrom() failed\n");
      exit(EXIT_FAILURE);
    }
    else {
      // Get opcode.
      unsigned short int * opcode_ptr;
      opcode_ptr = (unsigned short int *)buffer;
      opcode = ntohs(*opcode_ptr);

      if(opcode != RRQ && opcode != WRQ) {
        sendErr(5, sd, clt);
        exit(EXIT_FAILURE);
      }
      else {
        // Parent process.
        if(fork() == 0) {
          close(sd);
          break;
        }
        // Child process.
        else {
          // New port for each child.
          ++port;
        }
      }
    }
  }

  // Construct child socket.
  int chdSd;
  chdSd = socket(AF_INET, SOCK_DGRAM, 0);
  if(chdSd == -1) {
    perror("socket() failed\n");
    exit(EXIT_FAILURE);
  }

  struct sockaddr_in chdSvr;
  chdSvr.sin_family = AF_INET;
  chdSvr.sin_addr.s_addr = htonl(INADDR_ANY);
  chdSvr.sin_port = htons(port);

  if(bind(chdSd, (struct sockaddr *) &chdSvr, sizeof(chdSvr)) < 0) {
    perror("bind() failed\n");
    exit(EXIT_FAILURE);
  }

  if(opcode == RRQ) {
    recvRRQ(clt, chdSd, buffer);
  }
  else {
    recvWRQ(clt, chdSd, buffer);
  }
  close(chdSd);
  exit(EXIT_SUCCESS);
}