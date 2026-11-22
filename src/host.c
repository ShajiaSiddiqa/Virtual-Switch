#define _POSIX_C_SOURCE 200809L
#include "checked.h"
#include "frame_io.h"
#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <sys/time.h>
#include <unistd.h>

#define COMMAND_SIZE (FRAME_MAX_PAYLOAD + 128)
static volatile sig_atomic_t stopped;
static volatile sig_atomic_t signal_fd=-1;
static void stop_host(int number)
{
    (void)number;
    int saved=errno;
    stopped=1;
    if(signal_fd>=0) {
        const char byte=1;
        ssize_t result;
        do { result=write((int)signal_fd,&byte,1); } while(result<0 && errno==EINTR);
        /* EAGAIN means a wake byte is already pending; stopped is authoritative. */
        (void)result;
    }
    errno=saved;
}
static int number(const char *text,long maximum)
{
    if(text==NULL || *text=='\0') return -1;
    char *end;errno=0;long value=strtol(text,&end,10);
    return errno || end==text || *end || value<0 || value>maximum ? -1 : (int)value;
}
static int parse_mac(const char *text,uint8_t mac[FRAME_MAC_SIZE])
{
    if(text==NULL || strlen(text)!=17) return -1;
    for(size_t i=0;i<FRAME_MAC_SIZE;i++) {
        size_t offset=i*3;
        if(i<5 && text[offset+2]!=':') return -1;
        unsigned int value=0;
        for(size_t j=0;j<2;j++) {
            unsigned char c=(unsigned char)text[offset+j];
            unsigned int digit;
            if(c>='0' && c<='9') digit=c-'0';
            else if(c>='a' && c<='f') digit=c-'a'+10;
            else if(c>='A' && c<='F') digit=c-'A'+10;
            else return -1;
            value=value*16+digit;
        }
        mac[i]=(uint8_t)value;
    }
    return 0;
}
static int display(const FrameHeader *header,const uint8_t *payload)
{
    if(printf("FRAME src=%02x:%02x:%02x:%02x:%02x:%02x dst=%02x:%02x:%02x:%02x:%02x:%02x vlan=%u priority=%u length=%u text=",
        header->src[0],header->src[1],header->src[2],header->src[3],header->src[4],header->src[5],
        header->dst[0],header->dst[1],header->dst[2],header->dst[3],header->dst[4],header->dst[5],
        (unsigned)header->vlan,(unsigned)header->priority,(unsigned)header->length)<0) return -1;
    for(size_t i=0;i<header->length;i++) {
        unsigned char byte=payload[i];
        if(byte>=32 && byte<=126 && byte!='\\') {
            if(putchar(byte)==EOF) return -1;
        } else if(printf("\\x%02x",byte)<0) return -1;
    }
    return putchar('\n')==EOF || fflush(stdout)==EOF ? -1 : 0;
}
static char *token(char **text)
{
    char *start=*text;
    while(*start==' ' || *start=='\t') start++;
    if(*start=='\0') {*text=start;return NULL;}
    char *end=start;
    while(*end && *end!=' ' && *end!='\t') end++;
    if(*end) *end++='\0';
    *text=end;
    return start;
}
/* recv N exits after N total frames, including any received before the command. */
static int command(int fd,char *line,const uint8_t *source,uint64_t received,
                    uint64_t *goal,int *input_open)
{
    char *cursor=line;
    char *verb=token(&cursor);
    if(verb==NULL) return 0;
    if(strcmp(verb,"quit")==0) {
        if(token(&cursor)!=NULL) {errno=EINVAL;return -1;}
        stopped=1;return 0;
    }
    if(strcmp(verb,"help")==0) {
        if(puts("send DST VLAN TEXT | sendp DST VLAN PRIORITY TEXT | recv N | quit")<0 || fflush(stdout)==EOF)
            return -1;
        return 0;
    }
    if(strcmp(verb,"recv")==0) {
        int count=number(token(&cursor),1000000);
        if(count<1 || token(&cursor)!=NULL || UINT64_MAX-received<(uint64_t)count) {
            errno=EINVAL;return -1;
        }
        *goal=(uint64_t)count;
        *input_open=0;
        if(received>=*goal) stopped=1;
        return 0;
    }
    int priority_command=strcmp(verb,"sendp")==0;
    if(!priority_command && strcmp(verb,"send")!=0) {errno=EINVAL;return -1;}
    FrameHeader header={0};
    char *destination=token(&cursor);
    int vlan=number(token(&cursor),4094);
    int priority=priority_command ? number(token(&cursor),2) : 1;
    if(parse_mac(destination,header.dst)!=0 || vlan<1 || priority<0) {errno=EINVAL;return -1;}
    while(*cursor==' ' || *cursor=='\t') cursor++;
    size_t length=strlen(cursor);
    if(length>FRAME_MAX_PAYLOAD) {errno=EMSGSIZE;return -1;}
    header.length=(uint16_t)length;header.vlan=(uint16_t)vlan;header.priority=(uint8_t)priority;
    memcpy(header.src,source,FRAME_MAC_SIZE);
    if(frame_write(fd,&header,(const uint8_t *)cursor)!=0) return -1;
    if(printf("SENT length=%zu vlan=%d priority=%d\n",length,vlan,priority)<0 || fflush(stdout)==EOF)
        return -1;
    return 0;
}
static int session_wait(int fd,const uint8_t *source,int wake_fd)
{
    uint8_t wire[FRAME_HEADER_SIZE+FRAME_MAX_PAYLOAD];
    size_t used=0,needed=FRAME_HEADER_SIZE;
    FrameHeader header={0};
    char line[COMMAND_SIZE];size_t line_used=0;
    uint64_t received=0,goal=0;
    int input_open=1;
    while(!stopped) {
        struct pollfd events[3]={{.fd=fd,.events=POLLIN},
            {.fd=input_open ? STDIN_FILENO : -1,.events=POLLIN},
            {.fd=wake_fd,.events=POLLIN}};
        int ready=poll(events,3,-1);
        if(ready<0) {if(errno==EINTR) continue;return -1;}
        if(stopped || events[2].revents) return 0;
        if(events[0].revents & POLLNVAL) {errno=EBADF;return -1;}
        if(events[0].revents & (POLLIN|POLLHUP|POLLERR)) {
            /* Bound a receive burst so continuous traffic cannot starve stdin. */
            for(unsigned int reads=0;reads<64 && !stopped;reads++) {
                ssize_t count=recv(fd,wire+used,needed-used,MSG_DONTWAIT);
                if(count<0) {
                    if(errno==EINTR) {if(stopped) return 0;continue;}
                    if(errno==EAGAIN || errno==EWOULDBLOCK) break;
                    return -1;
                }
                if(count==0) {
                    if(used || (goal && received<goal)) {errno=EPROTO;return -1;}
                    return 0;
                }
                used+=(size_t)count;
                if(used==FRAME_HEADER_SIZE && needed==FRAME_HEADER_SIZE) {
                    if(frame_deserialize(wire,FRAME_HEADER_SIZE,&header)!=0) {errno=EPROTO;return -1;}
                    needed=FRAME_HEADER_SIZE+header.length;
                }
                if(used==needed) {
                    if(display(&header,wire+FRAME_HEADER_SIZE)!=0) return -1;
                    if(received==UINT64_MAX) {errno=EOVERFLOW;return -1;}
                    received++;
                    used=0;needed=FRAME_HEADER_SIZE;
                    if(goal && received>=goal) return 0;
                }
            }
        }
        if(input_open && events[1].revents & (POLLIN|POLLHUP|POLLERR|POLLNVAL)) {
            char byte;
            ssize_t count=read(STDIN_FILENO,&byte,1);
            if(count<0) {if(errno==EINTR) continue;return -1;}
            if(count==0) {
                if(line_used) {
                    line[line_used]='\0';
                    if(command(fd,line,source,received,&goal,&input_open)!=0) return -1;
                }
                if(!goal) return 0;
                input_open=0;
            } else if(byte=='\n') {
                if(line_used && line[line_used-1]=='\r') line_used--;
                line[line_used]='\0';
                if(command(fd,line,source,received,&goal,&input_open)!=0) return -1;
                line_used=0;
            } else {
                if(byte=='\0' || line_used==sizeof(line)-1) {errno=EMSGSIZE;return -1;}
                line[line_used++]=byte;
            }
        }
    }
    return 0;
}
static int session(int fd,const uint8_t *source)
{
    int wake[2];
    if(pipe(wake)<0) return -1;
    for(int i=0;i<2;i++) {
        int flags=fcntl(wake[i],F_GETFL);
        if(flags<0 || fcntl(wake[i],F_SETFL,flags|O_NONBLOCK)<0) {
            int saved=errno;
            vsw_posix(close(wake[0]),"close wake reader");
            vsw_posix(close(wake[1]),"close wake writer");
            errno=saved;
            return -1;
        }
    }
    /* A signal before this assignment sets stopped. After it, the pipe keeps
     * the wakeup readable even if the signal arrives just before poll(). */
    signal_fd=wake[1];
    int result=session_wait(fd,source,wake[0]);
    int saved=errno;
    sigset_t blocked;
    if(sigemptyset(&blocked)<0 || sigaddset(&blocked,SIGINT)<0 ||
       sigaddset(&blocked,SIGTERM)<0 || sigprocmask(SIG_BLOCK,&blocked,NULL)<0)
        vsw_posix(-1,"block host signals during cleanup");
    signal_fd=-1;
    vsw_posix(close(wake[0]),"close wake reader");
    vsw_posix(close(wake[1]),"close wake writer");
    errno=saved;
    return result;
}

int main(int argc,char *argv[])
{
    uint8_t source[FRAME_MAC_SIZE];
    int port=argc==4 ? number(argv[2],65535) : -1;
    if(argc!=4 || port<1 || parse_mac(argv[3],source)!=0) {
        vsw_posix(fprintf(stderr,"Usage: %s <numeric_ipv4> <tcp_port:1-65535> <mac>\n",argv[0]), "fprintf");return EXIT_FAILURE;
    }
    struct sockaddr_in address={.sin_family=AF_INET,.sin_port=htons((uint16_t)port)};
    if(inet_pton(AF_INET,argv[1],&address.sin_addr)!=1) {
        vsw_posix(fprintf(stderr,"Invalid numeric IPv4 address\n"), "fprintf");return EXIT_FAILURE;
    }
    if(signal(SIGPIPE,SIG_IGN)==SIG_ERR) {perror("ignore SIGPIPE");return EXIT_FAILURE;}
    struct sigaction action={.sa_handler=stop_host};
    if(sigemptyset(&action.sa_mask)<0 || sigaction(SIGINT,&action,NULL)<0 || sigaction(SIGTERM,&action,NULL)<0) {
        perror("host signals");return EXIT_FAILURE;
    }
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0) {perror("socket");return EXIT_FAILURE;}
    int status=EXIT_FAILURE;
    const struct timeval timeout={.tv_sec=3};
    if(setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout))<0) {perror("send timeout");goto cleanup;}
    if(connect(fd,(struct sockaddr *)&address,sizeof(address))<0) {perror("connect");goto cleanup;}
    if(printf("CONNECTED mac=%s\n",argv[3])<0 || fflush(stdout)==EOF) {perror("CONNECTED");goto cleanup;}
    if(session(fd,source)<0) {perror("host session");goto cleanup;}
    status=EXIT_SUCCESS;
cleanup:
    if(shutdown(fd,SHUT_RDWR)<0 && errno!=ENOTCONN) {perror("shutdown");status=EXIT_FAILURE;}
    if(close(fd)<0) {perror("close");status=EXIT_FAILURE;}
    return status;
}
