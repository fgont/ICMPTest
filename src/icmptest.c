
#include <stdlib.h>
#include <string.h>
//#include <unistd.h>
#include <stdio.h>

#include <stdbool.h>
#include <sys/socket.h>
#include <poll.h>

#include <arpa/inet.h>

#include <netinet/ip.h>
#include <netinet/ip_icmp.h>

#include <time.h>
#include <pthread.h>

#include<signal.h>

#ifdef __linux
#include <linux/types.h>	// required for linux/errqueue.h
#include <linux/errqueue.h>	// SO_EE_ORIGIN_ICMP
#endif

#include "iphelper.h"
#include "sockethelper.h"


#define MAXBUFLEN 2048
#define MAX_LISTEN_SOCKETS 10

int lines; //Just to gracefully hanle SIGINT

struct test_config{
    
    struct sockaddr_storage remoteAddr;
    struct sockaddr_storage localAddr;
    int sockfd;
    int icmpSocket;

    int numSentPkts;
    int numRcvdPkts;
    int numSentProbe;
    int numRcvdICMP;
    void (*data_handler)(struct test_config *, struct sockaddr *, 
                         unsigned char *, int);

    void (*icmp_handler)(struct test_config *, struct sockaddr *, 
                         int);
};

static void data_handler(struct test_config *config, struct sockaddr *saddr, 
                         unsigned char *buf, int len){

    config->numRcvdPkts++;
    printf("\r \033[7C RX: %i ", config->numRcvdPkts);
}


static void icmp_handler(struct test_config *config, 
                         struct sockaddr *saddr, 
                         int icmpType){
    
    char src_str[INET6_ADDRSTRLEN];

    config->numRcvdICMP++;
    printf("\r \033[35C RX ICMP: %i ",config->numRcvdICMP);
    printf("\033[%iB",config->numRcvdICMP);
    printf("\n  <-  %s (ICMP type:%i)\033[K",
           inet_ntop(AF_INET, saddr, src_str,INET_ADDRSTRLEN),
           icmpType);
    printf("\033[%iA",config->numRcvdICMP);

    //For graefull SIGINT
    lines = config->numRcvdICMP;
}

static void *sendData(struct test_config *config)
{
    struct timespec timer;
    struct timespec remaining;

    //How fast? Pretty fast..
    timer.tv_sec = 0;
    //timer.tv_nsec = 50000000;
    timer.tv_nsec = 5000000;
    
    for(;;) {
        nanosleep(&timer, &remaining);
        config->numSentPkts++;
        printf("\rTX: %i", config->numSentPkts);
        fflush(stdout);
        sendPacket(config->sockfd,
                       (uint8_t *)"ICMP_Test",
                       9,
                       (struct sockaddr *)&config->remoteAddr,
                       false,
                       0);
    }
}

static void *socketListen(void *ptr){
    struct pollfd ufds[MAX_LISTEN_SOCKETS];
    struct test_config *config = (struct test_config *)ptr;
    struct sockaddr_storage their_addr;
    unsigned char buf[MAXBUFLEN];
    socklen_t addr_len;
    int rv;
    int numbytes;
    int i;
    int numSockets = 0;
    int keyLen = 16;
    char md5[keyLen];

    //Normal send/recieve RTP socket..
    ufds[0].fd = config->sockfd;
    ufds[0].events = POLLIN | POLLERR;
    numSockets++;

    //Listen on the ICMP socket if it exists
    if(config->icmpSocket != 0){
        ufds[1].fd = config->icmpSocket;
        ufds[1].events = POLLIN | POLLERR;
        numSockets++;
    }
    addr_len = sizeof their_addr;
    
    while(1){
        rv = poll(ufds, numSockets, -1);
        if (rv == -1) {
            perror("poll"); // error occurred in poll()
        } else if (rv == 0) {
            printf("Timeout occurred! (Should not happen)\n");
        } else {
            // check for events on s1:
            for(i=0;i<numSockets;i++){
                if (ufds[i].revents & POLLIN) {
                    if(i == 0){
                        if ((numbytes = recvfrom(config->sockfd, buf, 
                                                 MAXBUFLEN , 0, 
                                                 (struct sockaddr *)&their_addr, &addr_len)) == -1) {
                            perror("recvfrom");
                            exit(1);
                        }
                        config->data_handler(config, (struct sockaddr *)&their_addr, buf, numbytes);
                    }
                    if(i == 1){//This is the ICMP socket
                        struct ip *ip_packet, *inner_ip_packet;
                        struct icmp *icmp_packet;
                        
                        if ((numbytes = recvfrom(config->icmpSocket, buf, 
                                                 MAXBUFLEN , 0, 
                                                 (struct sockaddr *)&their_addr, &addr_len)) == -1) {
                            perror("recvfrom");
                            exit(1);
                        }
                        //Try to get something out of the potential ICMP packet
                        ip_packet = (struct ip *) &buf;
                        icmp_packet = (struct icmp *) (buf + (ip_packet->ip_hl << 2));
                        inner_ip_packet = &icmp_packet->icmp_ip;
                                                
                        config->icmp_handler(config, (struct sockaddr *)&ip_packet->ip_src, icmp_packet->icmp_type);
                    }
                }
#ifdef __linux
                if (ufds[0].revents & POLLERR) {
                    //Do stuff with msghdr
                    struct msghdr msg;
                    struct sockaddr_in response;		// host answered IP_RECVERR
                    char control_buf[1500];
                    struct iovec iov;
                    char buf[1500];
                    struct cmsghdr *cmsg;

                    memset(&msg, 0, sizeof(msg));
                    msg.msg_name = &response;			// host
                    msg.msg_namelen = sizeof(response);
                    msg.msg_control = control_buf;
                    msg.msg_controllen = sizeof(control_buf);
                    iov.iov_base = buf;
                    iov.iov_len = sizeof(buf);
                    msg.msg_iov = &iov;
                    msg.msg_iovlen = 1;
               
                    if (recvmsg(config->sockfd, &msg, MSG_ERRQUEUE ) == -1) {
			//Ignore for now. Will get it later..
                        continue;
                    }    
                    for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
                         cmsg = CMSG_NXTHDR(&msg,cmsg)) {
                        if (cmsg->cmsg_level == IPPROTO_IP && cmsg->cmsg_type == IP_RECVERR){
                            struct sock_extended_err *ee;
                            ee = (struct sock_extended_err *) CMSG_DATA(cmsg);
                            
                            if (ee->ee_origin == SO_EE_ORIGIN_ICMP) {
                                //Have mercy.. Must fix this
                                struct sockaddr_in *addr = &((struct sockaddr_in*)SO_EE_OFFENDER(ee))->sin_addr;
                                
                                config->icmp_handler(config, 
                                                     (struct sockaddr*)addr,
                                                     ee->ee_type);
                            }
                        }
                    }
                }
#endif
            }
        }
    }
 }
    
    
    
    
void done(){
    printf("\033[%iB",lines);
    printf("\nDONE!\n");
    exit(0);
}

int main(int argc, char **argv)
{
    struct test_config config;
    pthread_t sendDataThread;
    pthread_t listenThread;

    int i;
    signal(SIGINT, done);
    memset(&config, 0, sizeof(config));
    config.icmp_handler = icmp_handler;
    config.data_handler = data_handler;

    if(!getRemoteIpAddr((struct sockaddr *)&config.remoteAddr, 
                            argv[2], 
                        3478)){
        printf("Error getting remote IPaddr");
        exit(1);
        }
    
    if(!getLocalInterFaceAddrs( (struct sockaddr *)&config.localAddr, 
                                argv[1], 
                                config.remoteAddr.ss_family, 
                                IPv6_ADDR_NORMAL, 
                                false)){
        printf("Error local getting IPaddr on %s\n", argv[1]);
        exit(1);
    }
    /* Setting up UDP socket and a ICMP sockhandle */
    config.sockfd = createLocalUDPSocket(config.remoteAddr.ss_family, (struct sockaddr *)&config.localAddr, 0);

    if(config.remoteAddr.ss_family == AF_INET)
        config.icmpSocket=socket(config.remoteAddr.ss_family, SOCK_DGRAM, IPPROTO_ICMP);
    else
        config.icmpSocket=socket(config.remoteAddr.ss_family, SOCK_DGRAM, IPPROTO_ICMPV6);

    if (config.icmpSocket < 0) {
#ifdef __linux
        config.icmpSocket=socket(config.remoteAddr.ss_family, SOCK_RAW, IPPROTO_ICMP);
        if (config.icmpSocket < 0) {
            //No privileges to run raw sockets. Use old socket and recieve ICMP with POLLERR
            //Warning: Remeber to get them out of the queue with readmsg() otherwise kernel
            //will close down socket?
            int val = 1;
            if (setsockopt (config.sockfd, SOL_IP, IP_RECVERR, &val, sizeof (val)) < 0){
                perror ("setsockopt IP_RECVERR");
                exit(1);
            }
        }
#else
        perror("ICMP socket");
        exit(1);
#endif
    }
    
    //Start and listen to the sockets.
    pthread_create(&listenThread, NULL, (void *)socketListen, &config);
    
    //Start a thread that sends packet to the destination (Simulate RTP)
    pthread_create(&sendDataThread, NULL, (void *)sendData, &config);
    
    //Send a probe ackets with increasing TTLs
    for(i=1;i<15;i++){
        sleep(1);
        config.numSentProbe++;
        printf("\r \033[16C Probe: %i (ttl:%i)", config.numSentProbe, i);
        fflush(stdout);
        sendPacket(config.sockfd,
                   (uint8_t *)"ICMP_Test_TTL",
                   13,
                   (struct sockaddr *)&config.remoteAddr,
                   false,
                   i);
        
    }
    
    //Just wait a bit
    sleep(1);
    done();
}
