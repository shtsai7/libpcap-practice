#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<netinet/ip.h>
#include<netinet/udp.h>
#include<arpa/inet.h>
#include<netinet/if_ether.h>  // for ETH_P_IP
#include<ifaddrs.h>           // for geting interface addresses
#include<print_packet.h>      // for printing packets

#define INTERFACE_NUM 5       // assume there are at most five interfaces on a host

void error(const char *msg);
void printHex(char *buffer, int recvlen);
char* skipEthHdr(char *buffer);
int getProtocol(char *packet);
void handleUDP(char *packet);
void handleIPinIP(char *packet);
void handleOSPF(char *packet);
int isForward(char *packet);
void forwardUDP(char *packet);

// This array "address" is used to store the ip addresses
// of the host's interfaces
struct sockaddr_in *address[INTERFACE_NUM];  
int addrNum;

int main (int argc, char *argv[]) {
  int sockfd;
  struct sockaddr_in cli_addr, serv_addr;
  socklen_t clilen = sizeof(cli_addr);
  int recvlen;

  // initialize receive buffer
  char *buffer;
  buffer = (char *) malloc(2048);
  memset(buffer, 0, 2048);

  // create ethernet socket
  // use ETH_P_IP will receive all IP packets to this server, along with their Ethernet headers
  sockfd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_IP)); 
  if (sockfd < 0) {
    error("ERROR socket");
  }
  
  // get host's interfaces addresses, and store them in "address"
  addrNum = 0;
  struct ifaddrs *tmp;
  getifaddrs(&tmp);
  while (tmp) {
    if (tmp->ifa_addr && tmp->ifa_addr->sa_family == AF_INET) {
      struct sockaddr_in *pAddr = (struct sockaddr_in *) tmp->ifa_addr;
      address[addrNum] = pAddr;
      addrNum++;
    }
    tmp = tmp->ifa_next;
  }
  freeifaddrs(tmp);
  
  int count = 0;   // used for keeping track of the number of packets received
  while (1) {
    printf("waiting for packet ...\n");
    
    recvlen = recvfrom(sockfd, buffer, 2047, 0, (struct sockaddr *) &cli_addr, &clilen);
    if (recvlen > 0) {
      printf("Packet %d, %d bytes, ",count, recvlen);

      char *packet = skipEthHdr(buffer);  // all packets received contain ethernet headers, skip ethernet headers here
      int protocol = getProtocol(packet); // reads the protocol value from the IP header
      printf("Protocol = %d\n", protocol);

      switch (protocol){	
      case 1:
	printf("This is an ICMP packet\n\n");
      case 4:
	handleIPinIP(packet);
	break;
      case 17:
	handleUDP(packet);
	break;
      case 89:
	handleOSPF(packet);
	break;
	
      /*  // ignore TCP packets for now, b/c need to deal with TCP handshakes
      case 6:
        handleTCP(packet);
        break;
      */
	
      default:
	printf("Other packet");
      }
	
    }
    memset(buffer, 0 ,2048);    // reset receive buffer
    recvlen = 0;               
    printf("\n");
    count++;
  }

  free(buffer);
  close(sockfd);
  return 0;
}

char* skipEthHdr(char *buffer) {
  // given a pointer to a received packet,
  // this function skips its ethernet header
  // and returns a pointer to the start of its IP header
  
  int ethhdr_len = (int) sizeof(struct ethhdr);
  return buffer + ethhdr_len;
}

int getProtocol(char *packet) {
  // given a pointer to a received packet (Ethernet header included),
  // this function returns the protocol number in the IP header
  struct ip *iphdr = (struct ip *) packet;
  return iphdr->ip_p;
}

void handleUDP(char *packet) {
  // this function handles UDP packet by printing the information it contains
  int ip_len = printIPheader(packet);
  printUDPheader(packet, ip_len);
}

void handleIPinIP(char *packet) {
  // this function handles IP-in-IP packet by printing the information it contains
  
  // print outer header
  int ip_len1 = printIPheader(packet);
  int protocol = getProtocol(packet + ip_len1);

  // only handles UDP packets for now, will add others
  if (protocol == 17) {
    handleUDP(packet + ip_len1);
    if (isForward(packet) > 0) {
      printf("Forwarding this packet...\n");
      forwardUDP(packet + ip_len1);
    }
  }
}

void handleOSPF(char *packet) {
  // this function handles OSPF packet
  // for now it just prints a message, haven't test it yet
  printf("This is a OSPF packet\n");
}

int isForward(char *packet) {
  // check if the outer destination address of an IP-in-IP packet
  // matches one of the IP addresses of the host's interfaces
  // If so, the host is the forward server, and it needs to forward
  // the encapsulated packet to the destination
  struct ip *iphdr = (struct ip*) packet;
  uint32_t dst_addr = iphdr->ip_dst.s_addr;
  for (int i = 0; i < addrNum; i++) {
    if (address[i]->sin_addr.s_addr == dst_addr) {
      return 1;
    }
  }
  return 0;
}

void forwardUDP(char *packet) {
  // Call this function when receives a UDP packet that is 
  // encapsulated in an IP-in-IP packet.
  // Decapsulate the UDP packet and forward it to its destination.

  int sockfd;
  int one = 1;
  const int *val = &one;
  struct sockaddr_in serv_addr;

  char *newpacket = (char *)malloc(2048);
  memset(newpacket,0,2048);

  // create UDP socket
  sockfd = socket(AF_INET, SOCK_RAW, IPPROTO_UDP);
  if (sockfd < 0) {
      error("ERROR socket openning");
  } 
  if (setsockopt(sockfd, IPPROTO_IP, IP_HDRINCL, val, sizeof(one))<0) {
      error("ERROR setting socket option");
  }

  struct ip *iphdr = (struct ip*) packet;

  short iphdr_len = (iphdr->ip_hl)*4;
  short ip_len = ntohs(iphdr->ip_len);
  struct udphdr *udp_hdr = (struct udphdr*) (packet+iphdr_len);
  memcpy(newpacket, packet, ip_len);
  
  // fill in destination address 
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port = udp_hdr->uh_dport;
  serv_addr.sin_addr = iphdr->ip_dst;
  
  // send
  int sendlen;
  sendlen = sendto(sockfd, newpacket, ip_len, 0, (struct sockaddr *) &serv_addr, sizeof(serv_addr));
  if (sendlen < 0) {
      error("ERROR sendto failed");
  } else {
      printf("Successfully forward packet\n");
  }
 
  close(sockfd);
  free(newpacket);
}

void printHex(char *buffer, int recvlen) {
  // this function prints the packet in Hex value
  // for debugging and testing purpose
  
  printf("Here is the Hex format of the packet\n");
  for (int i = 0; i < recvlen; i++) {
    printf("%x ", buffer[i]);
  }
  printf("\n\n");
}

void error(const char *msg) {
  perror(msg);
  exit(1);
}

