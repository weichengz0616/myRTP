#include "rtp.h"
#include "util.h"

#include <stdio.h>
#include <sys/socket.h>
#include <string.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_RTP_SIZE 1461


uint32_t window_size;
uint8_t mode;
uint16_t port;

uint32_t seq_num;//第一个SYN----x
uint32_t send_base;//滑动窗口下限
uint32_t next_seq_num;
int sock;
struct sockaddr_in receiver_addr;
char send_buffer[20480] = {0};
char recv_buffer[20480] = {0};
char ack_flags[20480] = {0};


int make_socket(const char *receiver_ip, uint16_t port);
rtp_header_t make_rtp_header(uint32_t seqnum, uint16_t len, uint8_t flags, int type);
int rtp_send(char *data, uint32_t size);
int rtp_recv(int timeout, int sec, int usec,int flags);
rtp_packet_t make_rtp_packet(uint32_t seqnum, uint32_t* len,uint8_t flags,FILE* fp);


int build_connection(const char* receiver_ip, uint16_t receiver_port);
int transfer_data(const char* filename);
int terminate();


int main(int argc, char **argv)
{
    if (argc != 6)
    {
        LOG_FATAL("Usage: ./sender [receiver ip] [receiver port] [file path] "
                  "[window size] [mode]\n");
    }

    // your code here
    port = atoi(argv[2]);
    window_size = atoi(argv[4]);
    mode = atoi(argv[5]);

    if(build_connection(argv[1],port) < 0)
    {
        return 0;
    }
    if(transfer_data(argv[3]) < 0)
    {
        return 0;
    }
    if(terminate() < 0)
    {
        return 0;
    }

    LOG_DEBUG("Sender: exiting...\n");
    return 0;
}

int make_socket(const char *receiver_ip, uint16_t port)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&receiver_addr, 0, sizeof(receiver_addr));
    receiver_addr.sin_family = AF_INET;
    inet_pton(AF_INET, receiver_ip, &receiver_addr.sin_addr);
    receiver_addr.sin_port = htons(port);

    return 0;
}

rtp_header_t make_rtp_header(uint32_t seqnum, uint16_t len, uint8_t flags, int type)
{
    rtp_header_t header;
    header.seq_num = seqnum;
    header.length = len;
    header.checksum = 0;
    header.flags = flags;
    if(type)//type = 1表示这不是数据包,而是SYN/ACK/FIN
    {
        header.checksum = compute_checksum(&header,sizeof(header));
    }

    return header;
}

int rtp_send(char *data, uint32_t size)
{
    memset(send_buffer, 0, sizeof(send_buffer));
    memcpy(send_buffer, data, size);
    int re = sendto(sock, send_buffer, size, 0,
                    (struct sockaddr *)&receiver_addr, sizeof(receiver_addr));
    return re;
}

int rtp_recv(int timeout, int sec, int usec,int flags)//flags主要用于非阻塞
{
    memset(recv_buffer, 0, sizeof(recv_buffer));

    struct timeval time_out;
    if (timeout)
    { // set time out
        time_out.tv_sec = sec;
        time_out.tv_usec = usec;
        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                       &time_out, sizeof(time_out)) == -1)
        {
            perror("setsockopt failed:");
        }
    }
    // receive
    int re = recvfrom(sock, recv_buffer, sizeof(recv_buffer),
                      flags, NULL, NULL);
    if (timeout)
    {
        // cancel time out
        time_out.tv_sec = 0;
        time_out.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                   &time_out, sizeof(time_out));
    }
    return re;
}



int build_connection(const char* receiver_ip, uint16_t receiver_port)
{
    if(make_socket(receiver_ip,receiver_port) != 0)
    {
        return -1;
    }
    seq_num = rand();

    int cnt = 0;
    //第一次握手:发送第一个 SYN
build_send1:
    if(cnt >= 50)//发送了50次都没有受到ACK,直接退出
    {
        LOG_DEBUG("SYN fail\n");
        return -1;
    }
    cnt++;
    rtp_header_t header = make_rtp_header(seq_num,0,RTP_SYN,1);
    int send_num = rtp_send((char*)&header,sizeof(rtp_header_t));
    if(send_num < 0)
    {
        return -1;
    }


    //第二次握手:接收 SYN|ACK
    int recv_num = rtp_recv(1,0,100000,0);//100ms
    if(recv_num < 0 && errno == EWOULDBLOCK)
    {
        LOG_DEBUG("second--timeout\n");
        goto build_send1;
    }
    rtp_header_t recv_header = *(rtp_header_t*)recv_buffer;
    uint32_t tmp_check = recv_header.checksum;
    recv_header.checksum = 0;
    if(recv_header.seq_num != seq_num + 1 || 
        recv_header.flags != (RTP_SYN | RTP_ACK) || 
        tmp_check != compute_checksum(&recv_header,sizeof(rtp_header_t)))
    {
        //ACK包错误,重传
        LOG_DEBUG("second--packet error\n");
        goto build_send1;
    }


    //第三次握手:发送 ACK
    cnt = 0;
    rtp_header_t ack_header = make_rtp_header(seq_num+1,0,RTP_ACK,1);
build_send2:
    if(cnt >= 50)
    {
        return -1;
    }
    cnt++;
    send_num = rtp_send((char*)&ack_header,sizeof(rtp_header_t));
    if(send_num < 0)
    {
        return -1;
    }
    //等待2s
    //这是考虑到receiver可能没收到ACK,可能重传上述SYN|ACK报文,因此2s内若未收到上述报文,则认为成功连接
    recv_num = rtp_recv(1,2,0,0);//2s
    if(recv_num == sizeof(rtp_header_t))
    {
        rtp_header_t recv_header = *(rtp_header_t*)recv_buffer;
        if(recv_header.flags == (RTP_SYN | RTP_ACK))
        {
            goto build_send2;
        }
    }
    

    //认为成功建立连接
    LOG_DEBUG("build connection success\n");
    return 0;
}






rtp_packet_t make_rtp_packet(uint32_t seqnum, uint32_t* len,uint8_t flags,FILE* fp)
{
    char data[MAX_RTP_SIZE];//这个缓冲区=============================待定======================
    uint16_t length = fread(data,1,MAX_RTP_SIZE,fp);//从文件中实际读取的数据
    *len = length;

    rtp_header_t header = make_rtp_header(seqnum,length,0,0);

    rtp_packet_t packet;
    packet.rtp = header;
    memset(packet.payload, 0, sizeof(packet.payload));
    memcpy(packet.payload, data, length);
    packet.rtp.checksum = compute_checksum(&packet,sizeof(header) + length);//注意这里packet的实际大小

    return packet;
}

int transfer_data(const char* filename)
{
    //初始化
    FILE* fp = fopen(filename,"r");
    struct stat filestat;
    stat(filename,&filestat);
    uint32_t filelen = filestat.st_size;

    uint32_t packet_num = filelen / MAX_RTP_SIZE + 1;//RTP数据包的个数
    send_base = seq_num + 1;
    next_seq_num = seq_num + 1;

    LOG_DEBUG("init: seq_num:%d send_base:%d next_seq_num:%d\n",(int)seq_num,(int)send_base,(int)next_seq_num);

    //计时器
    int timer_fd;
    struct itimerspec timer_spec;
    timer_spec.it_value.tv_sec = 0;
    timer_spec.it_value.tv_nsec = 100000000;//100ms
    timer_spec.it_interval.tv_sec = 0;
    timer_spec.it_interval.tv_nsec = 0;
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);//非阻塞

    


    //发送数据主循环
    LOG_DEBUG("transfer--go to main loop...\n");
    int first = 1;
    while(1)
    {
        //文件数据发送完毕
        if(send_base > seq_num + packet_num)
        {
            break;
        }



        //send 在窗口内
        //而且要在文件大小内!!!!!!
        if(next_seq_num >= send_base && 
            next_seq_num < send_base + window_size && 
            next_seq_num <= seq_num + packet_num)
        {
            //GBN or SR
            //SR:mode==1 且 检查当前next_seq_num已经确认 ====> 直接下一个循环
            if(!(mode == 1 && ack_flags[next_seq_num - seq_num - 1] == 1))
            {
                //注意这里的文件定位,细节
                fseek(fp,(next_seq_num - seq_num - 1) * MAX_RTP_SIZE,SEEK_SET);
                uint32_t len;//payload实际大小
                rtp_packet_t packet = make_rtp_packet(next_seq_num,&len,0,fp);

            

                int send_num = rtp_send((char*)&packet,len + sizeof(rtp_header_t));
                if(send_num < 0)
                {
                    LOG_DEBUG("send error\n");
                    return -1;
                }
                LOG_DEBUG("send===seq_num:%d    sendbase:%d    payload len:%d=======================\n",next_seq_num,send_base,len);
            }

            //不管发不发包,都要自增
            //SR 不用再发送确认的包,但是要++next_seq_num
            next_seq_num++;
            
            
            
            if(first)
            {
                first = 0;
                //第一次启动计时器
                timerfd_settime(timer_fd,0,&timer_spec,NULL);
            }
        }
        



        //receive 非阻塞,因此可能一次性接收到多个ACK包
        int recv_len = rtp_recv(0,0,0,MSG_DONTWAIT);
        int recv_cnt = 0;//记录多少个ACK包
        //LOG_DEBUG("recv_len:%d\n",recv_len);
        //注意sizeof无符号,recvlen超时返回-1
        //我靠
        while(recv_len > 0 && recv_len >= sizeof(rtp_header_t))
        {
            //处理ACK包
            rtp_header_t ack = *(rtp_header_t*)(recv_buffer + recv_cnt * sizeof(rtp_header_t));
            uint32_t check = ack.checksum;
            ack.checksum = 0;

            if(ack.flags == RTP_ACK && check == compute_checksum(&ack,sizeof(rtp_header_t)))
            {
                //GBN 处理seqnum
                //seqnum为接收方期待的下一个seqnum,这说明小于seqnum的全部被确认了,直接更新sendbase
                if(mode == 0)
                {
                    if(send_base < ack.seq_num)
                    {
                        //更新计时器
                        timerfd_settime(timer_fd,0,&timer_spec,NULL);
                        send_base = ack.seq_num;
                        next_seq_num = send_base;//?????注意
                    }
                }
                //SR
                //seqnum为接收方确认的seq_num
                else
                {
                    LOG_DEBUG("recv===seq_num:%d    sendbase:%d    =======\n",ack.seq_num,send_base);
                    //更新ack_flags
                    ack_flags[ack.seq_num - seq_num - 1] = 1;

                    //说明sendbase的报文被接收了,需要更新
                    if(send_base == ack.seq_num)
                    {
                        while(1)
                        {
                            //依次递增send_base,检查是否被确认
                            if(ack_flags[send_base - seq_num - 1] == 1)
                            {
                                send_base++;
                                if(send_base > seq_num + packet_num)
                                    break;
                            }
                            else
                            {
                                break;
                            }
                        }
                        //!!!!!!!!!!注意更新next_seq_num!!!!!!!!!!!!
                        next_seq_num = send_base;
                        //更新计时器
                        timerfd_settime(timer_fd,0,&timer_spec,NULL);
                    }
                }
                
            }

            recv_cnt++;
            recv_len -= sizeof(rtp_header_t);
        }



        //处理定时器超时
        uint64_t timeout;
        int tmp = read(timer_fd,&timeout,sizeof(timeout));
        if(tmp > 0)//超时重传
        {
            LOG_DEBUG("time===timeout next_seq_num:%d send_base:%d\n",next_seq_num,send_base);
            
            //更新了nextseqnum就相当于进入重传
            //此处GBN / SR统一处理了
            next_seq_num = send_base;
            timerfd_settime(timer_fd,0,&timer_spec,NULL);
        }
    }


    //结尾清理
    timer_spec.it_value.tv_nsec = 0;
    timerfd_settime(timer_fd,0,&timer_spec,NULL);
    close(timer_fd);
    fclose(fp);
    return 0;
}





int terminate()
{
    //第一次挥手
    int cnt = 0;
terminate_send:
    if(cnt >= 50)//发送了50次都没有受到ACK,直接退出
    {
        return -1;
    }
    cnt++;
    rtp_header_t fin = make_rtp_header(next_seq_num,0,RTP_FIN,1);
    rtp_send((char*)&fin, sizeof(rtp_header_t));


    //第二次挥手
    int recv_num = rtp_recv(1,0,100000,0);//100ms
    if(recv_num != sizeof(rtp_header_t))
    {
        if(errno == EWOULDBLOCK)//超时重传
        {
            goto terminate_send;
        }
    }
    rtp_header_t recv_header = *(rtp_header_t*)recv_buffer;
    uint32_t tmp_check = recv_header.checksum;
    recv_header.checksum = 0;
    if(recv_header.seq_num != next_seq_num || 
        recv_header.flags != (RTP_FIN | RTP_ACK) || 
        tmp_check != compute_checksum(&recv_header,sizeof(rtp_header_t)))
    {
        //ACK包错误,重传
        goto terminate_send;
    }

    return 0;
}
