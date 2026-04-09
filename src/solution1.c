#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <semaphore.h>
#include <errno.h>

#include "helpers.h"

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "Error: %s\n", msg); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define CHECK_SYS(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "Error: %s: %s\n", msg, strerror(errno)); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

#define SEM_WAIT(s) do { \
    int _rc; \
    do { _rc = sem_wait(s); } while (_rc == -1 && errno == EINTR); \
    CHECK_SYS(_rc == 0, "sem_wait failed"); \
} while (0)

#define SEM_POST(s) CHECK_SYS(sem_post(s) == 0, "sem_post failed")

typedef struct {
    int order_id, raw_value, token_type, is_sentinel;
} RawPacket;

typedef struct {
    int order_id, encoded_value, is_sentinel;
} EncodedPacket;

typedef struct {
    void *arr;
    int cap, in, out;
    sem_t empty, full, mutex;
} Buffer;

typedef struct {
    int T;
    int *available;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
} TokenPool;

static int P, M, N, num_orders, T;
static int *token_init_cnt, *tA, *tB;
static Buffer bufferA, bufferB;
static TokenPool pool;
static pthread_t *quant_threads, *enc_threads, logger_tid;
static int *q_ids, *e_ids;
static int next_order_id = 0;
static pthread_mutex_t next_order_mtx = PTHREAD_MUTEX_INITIALIZER;

static int parse_int(const char *s) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    CHECK(s[0] != '\0' && *end == '\0', "parse_int: invalid format");
    CHECK(v >= -2147483648L && v <= 2147483647L, "parse_int: out of range");
    return (int)v;
}

static void buf_init(Buffer *b, int cap, int elem_size) {
    b->cap = cap;
    b->in = b->out = 0;
    b->arr = malloc(cap * elem_size);
    CHECK(b->arr != NULL, "malloc failed");
    CHECK_SYS(sem_init(&b->empty, 0, cap) == 0, "sem_init empty failed");
    CHECK_SYS(sem_init(&b->full, 0, 0) == 0, "sem_init full failed");
    CHECK_SYS(sem_init(&b->mutex, 0, 1) == 0, "sem_init mutex failed");
}

static void buf_destroy(Buffer *b) {
    sem_destroy(&b->empty);
    sem_destroy(&b->full);
    sem_destroy(&b->mutex);
    free(b->arr);
}

static void buf_put(Buffer *b, void *item, int elem_size) {
    SEM_WAIT(&b->empty);
    SEM_WAIT(&b->mutex);
    memcpy((char *)b->arr + b->in * elem_size, item, elem_size);
    b->in = (b->in + 1) % b->cap;
    SEM_POST(&b->mutex);
    SEM_POST(&b->full);
}

static void buf_get(Buffer *b, void *item, int elem_size) {
    SEM_WAIT(&b->full);
    SEM_WAIT(&b->mutex);
    memcpy(item, (char *)b->arr + b->out * elem_size, elem_size);
    b->out = (b->out + 1) % b->cap;
    SEM_POST(&b->mutex);
    SEM_POST(&b->empty);
}

static void token_pool_init(TokenPool *tp, int types, const int *init_cnt) {
    tp->T = types;
    tp->available = malloc(types * sizeof(int));
    CHECK(tp->available != NULL, "malloc failed");
    for (int i = 0; i < types; i++) tp->available[i] = init_cnt[i];
    CHECK(pthread_mutex_init(&tp->mtx, NULL) == 0, "pthread_mutex_init failed");
    CHECK(pthread_cond_init(&tp->cv, NULL) == 0, "pthread_cond_init failed");
}

static void token_pool_destroy(TokenPool *tp) {
    pthread_mutex_destroy(&tp->mtx);
    pthread_cond_destroy(&tp->cv);
    free(tp->available);
}

static void acquire_tokens(TokenPool *tp, int a, int b) {
    pthread_mutex_lock(&tp->mtx);
    while (tp->available[a] <= 0 || tp->available[b] <= 0) {
        pthread_cond_wait(&tp->cv, &tp->mtx);
    }
    tp->available[a]--;
    tp->available[b]--;
    pthread_mutex_unlock(&tp->mtx);
}

static void release_tokens(TokenPool *tp, int a, int b) {
    pthread_mutex_lock(&tp->mtx);
    tp->available[a]++;
    tp->available[b]++;
    pthread_cond_broadcast(&tp->cv);
    pthread_mutex_unlock(&tp->mtx);
}

void *quantizer_thread(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&next_order_mtx);
        int oid = next_order_id;
        if (oid >= num_orders) {
            pthread_mutex_unlock(&next_order_mtx);
            break;
        }
        next_order_id++;
        pthread_mutex_unlock(&next_order_mtx);

        simulate_work(OP_Q1_QUANTIZE);
        RawPacket p = {oid, oid + 1, oid % T, 0};
        buf_put(&bufferA, &p, sizeof(RawPacket));
    }
    return NULL;
}

void *encoder_thread(void *arg) {
    int id = *(int *)arg;
    int a = tA[id], b = tB[id];

    while (1) {
        RawPacket rp;
        buf_get(&bufferA, &rp, sizeof(RawPacket));

        if (rp.is_sentinel) {
            EncodedPacket sp = {-1, 0, 1};
            buf_put(&bufferB, &sp, sizeof(EncodedPacket));
            break;
        }

        acquire_tokens(&pool, a, b);
        simulate_work(OP_Q1_ENCODE);
        EncodedPacket ep = {rp.order_id, rp.raw_value * 2 + a + b, 0};
        release_tokens(&pool, a, b);

        buf_put(&bufferB, &ep, sizeof(EncodedPacket));
    }
    return NULL;
}

void *logger_thread(void *arg) {
    (void)arg;
    int seen_sentinels = 0;
    while (seen_sentinels < P) {
        EncodedPacket ep;
        buf_get(&bufferB, &ep, sizeof(EncodedPacket));
        if (ep.is_sentinel) {
            seen_sentinels++;
        } else {
            simulate_work(OP_Q1_LOG);
            printf("[Logger] order_id=%d encoded=%d\n", ep.order_id, ep.encoded_value);
            fflush(stdout);
        }
    }
    return NULL;
}

static void parse_config(int argc, char **argv) {
    if (argc == 2) {
        FILE *fp = fopen(argv[1], "r");
        CHECK_SYS(fp != NULL, "fopen failed");
        CHECK(fscanf(fp, "%d %d %d %d %d", &P, &M, &N, &num_orders, &T) == 5, "fscanf");
        
        token_init_cnt = malloc(T * sizeof(int));
        tA = malloc(P * sizeof(int));
        tB = malloc(P * sizeof(int));
        CHECK(token_init_cnt && tA && tB, "malloc failed");

        for (int i = 0; i < T; i++)
            CHECK(fscanf(fp, "%d", &token_init_cnt[i]) == 1, "fscanf token");
        for (int i = 0; i < P; i++)
            CHECK(fscanf(fp, " ( %d , %d )", &tA[i], &tB[i]) == 2, "fscanf pair");
        fclose(fp);
    } else {
        CHECK(argc >= 6, "too few arguments");
        P = parse_int(argv[1]);
        M = parse_int(argv[2]);
        N = parse_int(argv[3]);
        num_orders = parse_int(argv[4]);
        T = parse_int(argv[5]);
        CHECK(argc == 1 + 5 + T + 2 * P, "argument count");

        token_init_cnt = malloc(T * sizeof(int));
        tA = malloc(P * sizeof(int));
        tB = malloc(P * sizeof(int));
        CHECK(token_init_cnt && tA && tB, "malloc failed");

        for (int i = 0; i < T; i++)
            token_init_cnt[i] = parse_int(argv[6 + i]);
        for (int i = 0; i < P; i++) {
            tA[i] = parse_int(argv[6 + T + 2*i]);
            tB[i] = parse_int(argv[6 + T + 2*i + 1]);
        }
    }

    CHECK(P > 0 && M > 0 && N > 0 && T > 0 && num_orders >= 0, "invalid params");
    for (int i = 0; i < T; i++)
        CHECK(token_init_cnt[i] >= 0, "negative token count");
    for (int i = 0; i < P; i++) {
        CHECK(tA[i] >= 0 && tA[i] < T && tB[i] >= 0 && tB[i] < T, "token index out of range");
        CHECK(tA[i] != tB[i], "tA_i must not equal tB_i");
    }
}

int main(int argc, char **argv) {
    parse_config(argc, argv);

    buf_init(&bufferA, M, sizeof(RawPacket));
    buf_init(&bufferB, N, sizeof(EncodedPacket));
    token_pool_init(&pool, T, token_init_cnt);

    quant_threads = malloc(P * sizeof(pthread_t));
    enc_threads = malloc(P * sizeof(pthread_t));
    q_ids = malloc(P * sizeof(int));
    e_ids = malloc(P * sizeof(int));
    CHECK(quant_threads && enc_threads && q_ids && e_ids, "malloc failed");

    for (int i = 0; i < P; i++) {
        q_ids[i] = i;
        CHECK(pthread_create(&quant_threads[i], NULL, quantizer_thread, &q_ids[i]) == 0, "pthread_create quantizer");
    }
    for (int i = 0; i < P; i++) {
        e_ids[i] = i;
        CHECK(pthread_create(&enc_threads[i], NULL, encoder_thread, &e_ids[i]) == 0, "pthread_create encoder");
    }
    CHECK(pthread_create(&logger_tid, NULL, logger_thread, NULL) == 0, "pthread_create logger");

    for (int i = 0; i < P; i++)
        CHECK(pthread_join(quant_threads[i], NULL) == 0, "pthread_join quantizer");

    for (int i = 0; i < P; i++) {
        RawPacket s = {-1, 0, -1, 1};
        buf_put(&bufferA, &s, sizeof(RawPacket));
    }

    for (int i = 0; i < P; i++)
        CHECK(pthread_join(enc_threads[i], NULL) == 0, "pthread_join encoder");
    CHECK(pthread_join(logger_tid, NULL) == 0, "pthread_join logger");

    buf_destroy(&bufferA);
    buf_destroy(&bufferB);
    token_pool_destroy(&pool);
    free(token_init_cnt);
    free(tA);
    free(tB);
    free(quant_threads);
    free(enc_threads);
    free(q_ids);
    free(e_ids);

    return 0;
}