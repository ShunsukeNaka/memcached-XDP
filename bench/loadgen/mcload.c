// mcload: memcached の GET 用の簡易負荷ツール (UDP / TCP)。
//
// - 接続 1 本につきスレッド 1 本。各スレッドはリクエストを 1 つ送って応答を待つ
//   クローズドループで動く (同時に処理中のリクエスト数 = 接続数)。
// - -i を指定すると、1 リクエストごとにその間隔を空ける (低負荷のレイテンシ計測用)。
// - キーは key:0000000000 〜 key:<nkeys-1> から一様乱数で選ぶ (scripts/seed_db.sh の形式)。
// - レイテンシは送信直前から応答を受け取り終わるまでを CLOCK_MONOTONIC で測る。
//
// UDP のリクエストには 8 バイトのフレームヘッダを付ける:
//   request id (2) | sequence number (2) | total datagrams (2) | reserved (2)
// memcached は大きな応答を 1400 バイトごとに分けて返すので、total 個そろうまで受信する。

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define HIST_RES_NS 100ULL                       // ヒストグラムの分解能
#define HIST_MAX_NS (20ULL * 1000 * 1000)        // これを超えたら最後のバケットに入れる
#define HIST_BUCKETS (HIST_MAX_NS / HIST_RES_NS + 1)
#define RESP_BUF 65536
#define UDP_HDR 8

enum proto { PROTO_UDP, PROTO_TCP };

struct config {
    const char *host;
    int port;
    enum proto proto;
    int conns;
    double duration_s;
    double warmup_s;
    uint64_t nkeys;
    uint64_t interval_ns;
    bool csv;
};

struct worker {
    pthread_t tid;
    int id;
    const struct config *cfg;
    uint64_t *hist;
    uint64_t requests, hits, misses, errors;
    uint64_t sum_ns, max_ns;
};

static atomic_bool g_measuring;
static atomic_bool g_stop;

static inline uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void sleep_until(uint64_t t) {
    struct timespec ts = {.tv_sec = t / 1000000000ULL, .tv_nsec = t % 1000000000ULL};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {
    }
}

static uint64_t xorshift64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    return *s = x;
}

static int open_socket(const struct config *cfg) {
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(cfg->port)};
    if (inet_pton(AF_INET, cfg->host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid host: %s\n", cfg->host);
        return -1;
    }
    int fd = socket(AF_INET, cfg->proto == PROTO_UDP ? SOCK_DGRAM : SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    if (cfg->proto == PROTO_TCP) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

// 戻り値: 1 = ヒット, 0 = ミス, -1 = エラー (タイムアウト・不正な応答)
static int classify(const char *buf, size_t len) {
    if (len >= 5 && memcmp(buf, "VALUE", 5) == 0) return 1;
    if (len == 5 && memcmp(buf, "END\r\n", 5) == 0) return 0;
    return -1;
}

static int do_udp_get(int fd, uint16_t req_id, const char *req, size_t req_len, char *buf) {
    char pkt[UDP_HDR + 300];
    uint16_t hdr[4] = {htons(req_id), 0, htons(1), 0};
    memcpy(pkt, hdr, UDP_HDR);
    memcpy(pkt + UDP_HDR, req, req_len);
    if (send(fd, pkt, UDP_HDR + req_len, 0) < 0) return -1;

    // 応答は seq 順に届くとは限らないので、seq * 1400 の位置に置いて組み立てる。
    // 1400 は memcached の UDP_MAX_PAYLOAD_SIZE (ヘッダを除いた 1 データグラムの最大長)。
    char dgram[UDP_HDR + 1400];
    size_t total_len = 0;
    int expected = -1, got = 0;
    while (expected < 0 || got < expected) {
        ssize_t n = recv(fd, dgram, sizeof(dgram), 0);
        if (n < UDP_HDR) return -1;  // タイムアウト (パケット落ち) など
        uint16_t rh[4];
        memcpy(rh, dgram, UDP_HDR);
        if (ntohs(rh[0]) != req_id) continue;  // 前のリクエストの遅れた応答は捨てる
        int seq = ntohs(rh[1]), total = ntohs(rh[2]);
        size_t plen = (size_t)n - UDP_HDR;
        size_t off = (size_t)seq * 1400;
        if (total <= 0 || seq >= total || off + plen > RESP_BUF) return -1;
        memcpy(buf + off, dgram + UDP_HDR, plen);
        if (seq == total - 1) total_len = off + plen;
        expected = total;
        got++;
    }
    return classify(buf, total_len);
}

static int do_tcp_get(int fd, const char *req, size_t req_len, char *buf) {
    if (send(fd, req, req_len, 0) != (ssize_t)req_len) return -1;
    size_t len = 0;
    for (;;) {
        ssize_t n = recv(fd, buf + len, RESP_BUF - len, 0);
        if (n <= 0) return -1;
        len += (size_t)n;
        // seed_db.sh が作る値には "END\r\n" が含まれないので、末尾だけ見れば十分。
        if (len >= 5 && memcmp(buf + len - 5, "END\r\n", 5) == 0) break;
        if (len == RESP_BUF) return -1;
    }
    return classify(buf, len);
}

static void *worker_main(void *arg) {
    struct worker *w = arg;
    const struct config *cfg = w->cfg;
    char *buf = malloc(RESP_BUF);
    int fd = open_socket(cfg);
    if (fd < 0 || !buf) {
        w->errors++;
        free(buf);
        return NULL;
    }
    uint64_t rng = 0x9E3779B97F4A7C15ULL ^ ((uint64_t)w->id * 0x100000001B3ULL) ^ now_ns();
    uint16_t req_id = (uint16_t)(w->id * 7919);
    char req[300];
    uint64_t next = now_ns();

    while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
        if (cfg->interval_ns) {
            sleep_until(next);
            next += cfg->interval_ns;
        }
        uint64_t key = xorshift64(&rng) % cfg->nkeys;
        int req_len = snprintf(req, sizeof(req), "get key:%010llu\r\n", (unsigned long long)key);

        uint64_t t0 = now_ns();
        int r = cfg->proto == PROTO_UDP ? do_udp_get(fd, ++req_id, req, (size_t)req_len, buf)
                                        : do_tcp_get(fd, req, (size_t)req_len, buf);
        uint64_t lat = now_ns() - t0;

        bool measuring = atomic_load_explicit(&g_measuring, memory_order_relaxed);
        if (r < 0) {
            if (measuring) w->errors++;
            if (cfg->proto == PROTO_TCP) break;  // TCP はストリームがずれるので続けない
            continue;
        }
        if (!measuring) continue;
        w->requests++;
        if (r == 1) w->hits++; else w->misses++;
        w->sum_ns += lat;
        if (lat > w->max_ns) w->max_ns = lat;
        uint64_t b = lat / HIST_RES_NS;
        w->hist[b < HIST_BUCKETS ? b : HIST_BUCKETS - 1]++;
    }
    close(fd);
    free(buf);
    return NULL;
}

static double percentile_us(const uint64_t *hist, uint64_t count, double p) {
    uint64_t target = (uint64_t)(p * (double)count);
    if (target >= count) target = count - 1;
    uint64_t acc = 0;
    for (uint64_t b = 0; b < HIST_BUCKETS; b++) {
        acc += hist[b];
        if (acc > target) return (double)((b + 1) * HIST_RES_NS) / 1000.0;  // バケットの上端
    }
    return (double)HIST_MAX_NS / 1000.0;
}

static void usage(const char *prog) {
    fprintf(stderr,
            "usage: %s [options]\n"
            "  -s HOST       server IPv4 address (default 127.0.0.1)\n"
            "  -p PORT       server port (default 11211)\n"
            "  -P udp|tcp    protocol (default udp)\n"
            "  -c CONNS      connections = threads (default 1)\n"
            "  -d SECONDS    measurement duration (default 10)\n"
            "  -w SECONDS    warmup before measurement (default 1)\n"
            "  -k NKEYS      key space key:0..NKEYS-1 (default 100000)\n"
            "  -i USEC       interval between requests per connection, 0 = as fast as possible (default 0)\n"
            "  --csv         print one CSV line (header with --csv-header)\n"
            "  --csv-header  print the CSV header and exit\n",
            prog);
}

static const char *CSV_HEADER =
    "proto,conns,interval_us,duration_s,requests,errors,hits,misses,throughput_rps,"
    "avg_us,p50_us,p90_us,p99_us,p999_us,max_us";

int main(int argc, char **argv) {
    struct config cfg = {
        .host = "127.0.0.1", .port = 11211, .proto = PROTO_UDP, .conns = 1,
        .duration_s = 10, .warmup_s = 1, .nkeys = 100000, .interval_ns = 0, .csv = false,
    };
    static const struct option longopts[] = {
        {"csv", no_argument, NULL, 'C'},
        {"csv-header", no_argument, NULL, 'H'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "s:p:P:c:d:w:k:i:h", longopts, NULL)) != -1) {
        switch (opt) {
        case 's': cfg.host = optarg; break;
        case 'p': cfg.port = atoi(optarg); break;
        case 'P':
            if (strcmp(optarg, "udp") == 0) cfg.proto = PROTO_UDP;
            else if (strcmp(optarg, "tcp") == 0) cfg.proto = PROTO_TCP;
            else { usage(argv[0]); return 2; }
            break;
        case 'c': cfg.conns = atoi(optarg); break;
        case 'd': cfg.duration_s = atof(optarg); break;
        case 'w': cfg.warmup_s = atof(optarg); break;
        case 'k': cfg.nkeys = strtoull(optarg, NULL, 10); break;
        case 'i': cfg.interval_ns = (uint64_t)(atof(optarg) * 1000.0); break;
        case 'C': cfg.csv = true; break;
        case 'H': puts(CSV_HEADER); return 0;
        default: usage(argv[0]); return opt == 'h' ? 0 : 2;
        }
    }
    if (cfg.conns < 1 || cfg.nkeys < 1 || cfg.duration_s <= 0) {
        usage(argv[0]);
        return 2;
    }

    struct worker *ws = calloc((size_t)cfg.conns, sizeof(*ws));
    for (int i = 0; i < cfg.conns; i++) {
        ws[i].id = i + 1;
        ws[i].cfg = &cfg;
        ws[i].hist = calloc(HIST_BUCKETS, sizeof(uint64_t));
        if (!ws[i].hist) { perror("calloc"); return 1; }
        pthread_create(&ws[i].tid, NULL, worker_main, &ws[i]);
    }

    sleep_until(now_ns() + (uint64_t)(cfg.warmup_s * 1e9));
    atomic_store(&g_measuring, true);
    uint64_t t_start = now_ns();
    sleep_until(t_start + (uint64_t)(cfg.duration_s * 1e9));
    atomic_store(&g_measuring, false);
    double elapsed = (double)(now_ns() - t_start) / 1e9;
    atomic_store(&g_stop, true);

    uint64_t *hist = calloc(HIST_BUCKETS, sizeof(uint64_t));
    uint64_t req = 0, hits = 0, misses = 0, errs = 0, sum = 0, max = 0;
    for (int i = 0; i < cfg.conns; i++) {
        pthread_join(ws[i].tid, NULL);
        req += ws[i].requests; hits += ws[i].hits; misses += ws[i].misses;
        errs += ws[i].errors; sum += ws[i].sum_ns;
        if (ws[i].max_ns > max) max = ws[i].max_ns;
        for (uint64_t b = 0; b < HIST_BUCKETS; b++) hist[b] += ws[i].hist[b];
    }

    const char *pname = cfg.proto == PROTO_UDP ? "udp" : "tcp";
    double rps = (double)req / elapsed;
    double avg = req ? (double)sum / (double)req / 1000.0 : 0;
    double p50 = req ? percentile_us(hist, req, 0.50) : 0;
    double p90 = req ? percentile_us(hist, req, 0.90) : 0;
    double p99 = req ? percentile_us(hist, req, 0.99) : 0;
    double p999 = req ? percentile_us(hist, req, 0.999) : 0;
    double maxus = (double)max / 1000.0;

    if (cfg.csv) {
        printf("%s,%d,%.1f,%.2f,%llu,%llu,%llu,%llu,%.0f,%.2f,%.1f,%.1f,%.1f,%.1f,%.1f\n", pname,
               cfg.conns, (double)cfg.interval_ns / 1000.0, elapsed, (unsigned long long)req,
               (unsigned long long)errs, (unsigned long long)hits, (unsigned long long)misses, rps,
               avg, p50, p90, p99, p999, maxus);
    } else {
        printf("proto=%s conns=%d interval=%.1fus duration=%.2fs\n", pname, cfg.conns,
               (double)cfg.interval_ns / 1000.0, elapsed);
        printf("requests=%llu errors=%llu hits=%llu misses=%llu\n", (unsigned long long)req,
               (unsigned long long)errs, (unsigned long long)hits, (unsigned long long)misses);
        printf("throughput=%.0f req/s\n", rps);
        printf("latency(us): avg=%.2f p50=%.1f p90=%.1f p99=%.1f p99.9=%.1f max=%.1f\n", avg, p50,
               p90, p99, p999, maxus);
    }
    return errs && !req ? 1 : 0;
}
