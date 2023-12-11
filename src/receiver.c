/*
代码将建立连接和传输数据分开
即认为: 在特定时间收到特定类型数据. 比如,在数据传输阶段收到的一定是数据包,而不是SYN/ACK/FIN
但是,是否可能出现如下情况:
sender在建立连接阶段发送过多次SYN/ACK, 但receiver在数据传输阶段又接收到其中某个
对于sender的数据传输阶段也可能出现这样的情况


*/

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

uint32_t seq_num;   // 第一次收到的SYN-----x
uint32_t recv_base; // 滑动窗口下限
uint32_t next_seq_num;
int sock;
struct sockaddr_in send_addr; // 对方
struct sockaddr_in lst_addr;  // 监听

char send_buffer[200] = {0}; // 只会发送头部报文
char recv_buffer[20480] = {0};
char ack_flags[80000] = {0};

// cache
// ack_flag = 1 代表 有效位
rtp_packet_t cache[80000];

int make_socket(uint16_t port);
rtp_header_t make_rtp_header(uint32_t seqnum, uint16_t len, uint8_t flags, int type);
int rtp_send(char *data, uint32_t size);
int rtp_recv(int timeout, int sec, int usec, int flags);

int build_connection(uint16_t listen_port);
int transfer_data(const char *filename);

int main(int argc, char **argv)
{
    if (argc != 5)
    {
        LOG_FATAL("Usage: ./receiver [listen port] [file path] [window size] "
                  "[mode]\n");
    }

    // your code here
    port = atoi(argv[1]);
    window_size = atoi(argv[3]);
    mode = atoi(argv[4]);

    if (build_connection(port) < 0)
    {
        return 0;
    }
    if (transfer_data(argv[2]) < 0)
    {
        return 0;
    }

    LOG_DEBUG("Receiver: exiting...\n\n\n\n\n\n");
    return 0;
}

int make_socket(uint16_t port)
{
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    memset(&lst_addr, 0, sizeof(lst_addr));
    lst_addr.sin_family = AF_INET;
    lst_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    lst_addr.sin_port = htons(port);
    bind(sock, (struct sockaddr *)&lst_addr, sizeof(lst_addr));

    LOG_DEBUG("%d\n", port);

    return 0;
}

rtp_header_t make_rtp_header(uint32_t seqnum, uint16_t len, uint8_t flags, int type)
{
    rtp_header_t header;
    header.seq_num = seqnum;
    header.length = len;
    header.checksum = 0;
    header.flags = flags;
    if (type) // type = 1表示这不是数据包,而是SYN/ACK/FIN
    {
        header.checksum = compute_checksum(&header, sizeof(header));
    }

    return header;
}

int rtp_send(char *data, uint32_t size)
{
    memset(send_buffer, 0, sizeof(send_buffer));
    memcpy(send_buffer, data, size);
    int re = sendto(sock, send_buffer, size, 0,
                    (struct sockaddr *)&send_addr, sizeof(send_addr));
    return re;
}

int rtp_recv(int timeout, int sec, int usec, int flags) // flags主要用于非阻塞
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
    //!!!!!!!!!! 必须要么两个NULL，要么两个都不为NULL
    // 两个NULL的话,len必须初始化
    // 我靠
    socklen_t len = sizeof(send_addr);
    int re = recvfrom(sock, recv_buffer, sizeof(recv_buffer),
                      flags, (struct sockaddr *)&send_addr, &len);
    // char tmp[20];
    // inet_ntop(AF_INET,&send_addr.sin_addr,tmp,20);
    // LOG_DEBUG("send addr:  %s:%d\n",tmp,ntohs(send_addr.sin_port));
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

int build_connection(uint16_t listen_port)
{
    // 初始化
    make_socket(listen_port);

    // 第一次握手:接收 SYN报文
build_recv:
    int recv_num = rtp_recv(1, 5, 0, 0);
    if (recv_num < 0 && errno == EWOULDBLOCK)
    {
        // 连接超时 5s 退出
        LOG_DEBUG("SYN timeout\n");
        return -1;
    }
    rtp_header_t syn = *(rtp_header_t *)recv_buffer;
    uint32_t check = syn.checksum;
    syn.checksum = 0;
    seq_num = syn.seq_num;
    if (syn.flags != RTP_SYN || check != compute_checksum(&syn, sizeof(rtp_header_t)))
    {
        // 包错误 重新接收
        goto build_recv;
    }

    // 第二次握手:发送 SYN|ACK报文
    LOG_DEBUG("second hand\n");
    rtp_header_t syn_ack = make_rtp_header(seq_num + 1, 0, RTP_SYN | RTP_ACK, 1);
build_send1:
    rtp_send((char *)&syn_ack, sizeof(rtp_header_t));

    // 第三次握手:接收 ACK报文
    LOG_DEBUG("third hand\n");
    recv_num = rtp_recv(1, 0, 100000, 0);
    if (recv_num < 0 && errno == EWOULDBLOCK) // 超时
    {
        LOG_DEBUG("third--timeout\n");
        goto build_send1;
    }
    rtp_header_t ack = *(rtp_header_t *)recv_buffer;
    check = ack.checksum;
    ack.checksum = 0;
    if (ack.flags != RTP_ACK ||
        ack.seq_num != seq_num + 1 ||
        check != compute_checksum(&ack, sizeof(rtp_header_t))) // 包错误
    {
        LOG_DEBUG("third--packet error\n");
        goto build_send1;
    }

    LOG_DEBUG("success\n");
    return 0;
}

int transfer_data(const char *filename)
{
    FILE *fp = fopen(filename, "w");
    if (!fp)
    {
        LOG_DEBUG("fopen:%s fail\n", filename);
        return -1;
    }
    recv_base = seq_num + 1;
    next_seq_num = seq_num + 1; // GBN 希望接收到的下一个seq_num

    LOG_DEBUG("transfer--go to main loop\n");
    while (1)
    {
        int recv_num = rtp_recv(1, 5, 0, 0);
        if (recv_num < 0 && errno == EWOULDBLOCK)
        {
            fclose(fp);
            LOG_DEBUG("transfer--timeout\n");
            return -1; // 超时直接退出
        }

        // 此时接收的数据可能有多个数据报文,依次解析
        char *tmp_recv_buffer = recv_buffer;
        while (recv_num > 0)
        {
            LOG_DEBUG("main loop recv_num:%d\n", recv_num);
            rtp_header_t *header_p = (rtp_header_t *)tmp_recv_buffer;
            uint32_t check = header_p->checksum;
            header_p->checksum = 0;

            LOG_DEBUG("flags:%d seq:%d recv_base:%d\n", header_p->flags, header_p->seq_num, recv_base);

            // 处理数据报文
            if (header_p->flags == 0 &&
                header_p->seq_num >= recv_base && header_p->seq_num < recv_base + window_size &&
                check == compute_checksum(header_p, header_p->length + sizeof(rtp_header_t)))
            {
                // 拿到一个正确的包
                LOG_DEBUG("get correct packet--seq_num:%d\n", header_p->seq_num);

                // GBN
                if (mode == 0)
                {
                    // GBN
                    // 检查是不是希望的
                    if (header_p->seq_num == next_seq_num)
                    {
                        // 写入文件
                        LOG_DEBUG("write file:%s len:%d\n", filename, header_p->length);
                        fwrite(tmp_recv_buffer + sizeof(rtp_header_t), 1, header_p->length, fp);

                        next_seq_num++;
                        recv_base++;
                        // 标记这个报文已经确认
                        ack_flags[header_p->seq_num - seq_num - 1] = 1;
                    }

                    // 收到的报文已经确认过
                    if (ack_flags[header_p->seq_num - seq_num - 1] == 1)
                    {
                        // GBN
                        // 发送ACK
                        rtp_header_t ack = make_rtp_header(next_seq_num, 0, RTP_ACK, 1);
                        rtp_send((char *)&ack, sizeof(rtp_header_t));
                    }
                }
                // SR
                else
                {
                    LOG_DEBUG("cach====seq_num:%d   recv_base:%d\n", header_p->seq_num, recv_base);
                    ack_flags[header_p->seq_num - seq_num - 1] = 1;

                    // 缓存
                    cache[header_p->seq_num - seq_num - 1] = *(rtp_packet_t *)header_p;

                    // 发送ACK
                    LOG_DEBUG("ACK send seq_num1:%d\n\n", header_p->seq_num);
                    rtp_header_t ack = make_rtp_header(header_p->seq_num, 0, RTP_ACK, 1);
                    rtp_send((char *)&ack, sizeof(rtp_header_t));
                }
            }
            // 在窗口之外, 但是已经收到过
            else if (header_p->flags == 0 &&
                     check == compute_checksum(header_p, header_p->length + sizeof(rtp_header_t)))
            {
                if (ack_flags[header_p->seq_num - seq_num - 1] == 1)
                {
                    LOG_DEBUG("ACK send seq_num2:%d\n\n", header_p->seq_num);
                    rtp_header_t ack = make_rtp_header(header_p->seq_num, 0, RTP_ACK, 1);
                    rtp_send((char *)&ack, sizeof(rtp_header_t));
                }
            }


            // 检查FIN报文
            if (header_p->flags == RTP_FIN &&
                check == compute_checksum(header_p, sizeof(rtp_header_t)))
            {
                rtp_header_t fin_ack = make_rtp_header(header_p->seq_num, 0, RTP_FIN | RTP_ACK, 1);
                rtp_send((char *)&fin_ack, sizeof(rtp_header_t));

                LOG_DEBUG("get FIN\n");
                fclose(fp);
                return 0;
            }


            // SR 写文件、更新recv_base
            if (mode == 1)
            {
                while (ack_flags[recv_base - seq_num - 1] == 1)
                {
                    LOG_DEBUG("write... %d\n", recv_base);
                    fwrite((char *)(cache[recv_base - seq_num - 1].payload), 1,
                           cache[recv_base - seq_num - 1].rtp.length, fp);
                    recv_base++;
                }
                LOG_DEBUG("write over\n");
            }


            // 更新recv_buffer
            tmp_recv_buffer = tmp_recv_buffer + (header_p->length + sizeof(rtp_header_t));
            recv_num = recv_num - (header_p->length + sizeof(rtp_header_t));
        }
    }
}
