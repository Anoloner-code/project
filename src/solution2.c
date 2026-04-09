#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "helpers.h"

#define ITEM_NAME_MAX 127

typedef enum {
    ROLE_WORKER,
    ROLE_MANAGER,
    ROLE_SUPERVISOR
} Role;

typedef enum {
    OP_READ,
    OP_WRITE
} OpType;

typedef struct {
    OpType op;
    char item[ITEM_NAME_MAX + 1];
} Operation;

typedef struct {
    Operation *data;
    int size;
    int cap;
} OpList;

typedef struct {
    char name[ITEM_NAME_MAX + 1];
    int active_readers;
    int active_writer;
    int waiting_supervisors;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
} ItemState;

typedef struct {
    ItemState *data;
    int size;
    int cap;
    pthread_mutex_t mtx;
} ItemRegistry;

typedef struct {
    Role role;
    int id;
    OpList *ops;
} ThreadCtx;

static int g_num_workers = 0;
static int g_num_managers = 0;
static int g_num_supervisors = 0;
static int g_num_ops = 0;

static OpList *g_worker_ops = NULL;
static OpList *g_manager_ops = NULL;
static OpList *g_supervisor_ops = NULL;

static ItemRegistry g_registry;
static pthread_mutex_t g_print_mtx = PTHREAD_MUTEX_INITIALIZER;

static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

static void *xmalloc(size_t n) {
    if (n == 0) {
        return NULL;
    }
    void *p = malloc(n);
    if (p == NULL) {
        die("malloc failed");
    }
    return p;
}

static void op_list_init(OpList *list) {
    list->data = NULL;
    list->size = 0;
    list->cap = 0;
}

static void op_list_push(OpList *list, const Operation *op) {
    if (list->size == list->cap) {
        int new_cap = (list->cap == 0) ? 8 : list->cap * 2;
        Operation *new_data = realloc(list->data, (size_t)new_cap * sizeof(Operation));
        if (new_data == NULL) {
            die("realloc failed");
        }
        list->data = new_data;
        list->cap = new_cap;
    }
    list->data[list->size++] = *op;
}

static void op_list_destroy(OpList *list) {
    free(list->data);
    list->data = NULL;
    list->size = 0;
    list->cap = 0;
}

static void registry_init(ItemRegistry *r) {
    r->data = NULL;
    r->size = 0;
    r->cap = 0;
    if (pthread_mutex_init(&r->mtx, NULL) != 0) {
        die("pthread_mutex_init failed");
    }
}

static void registry_destroy(ItemRegistry *r) {
    for (int i = 0; i < r->size; i++) {
        pthread_mutex_destroy(&r->data[i].mtx);
        pthread_cond_destroy(&r->data[i].cv);
    }
    free(r->data);
    pthread_mutex_destroy(&r->mtx);
}

static ItemState *registry_get_or_create(ItemRegistry *r, const char *item) {
    if (pthread_mutex_lock(&r->mtx) != 0) {
        die("pthread_mutex_lock failed");
    }

    for (int i = 0; i < r->size; i++) {
        if (strcmp(r->data[i].name, item) == 0) {
            ItemState *found = &r->data[i];
            pthread_mutex_unlock(&r->mtx);
            return found;
        }
    }

    if (r->size == r->cap) {
        int new_cap = (r->cap == 0) ? 16 : r->cap * 2;
        ItemState *new_data = realloc(r->data, (size_t)new_cap * sizeof(ItemState));
        if (new_data == NULL) {
            pthread_mutex_unlock(&r->mtx);
            die("realloc failed");
        }
        r->data = new_data;
        r->cap = new_cap;
    }

    ItemState *st = &r->data[r->size++];
    memset(st, 0, sizeof(*st));
    strncpy(st->name, item, ITEM_NAME_MAX);
    st->name[ITEM_NAME_MAX] = '\0';
    if (pthread_mutex_init(&st->mtx, NULL) != 0) {
        pthread_mutex_unlock(&r->mtx);
        die("pthread_mutex_init failed");
    }
    if (pthread_cond_init(&st->cv, NULL) != 0) {
        pthread_mutex_unlock(&r->mtx);
        die("pthread_cond_init failed");
    }

    pthread_mutex_unlock(&r->mtx);
    return st;
}

static ItemState *registry_get(ItemRegistry *r, const char *item) {
    if (pthread_mutex_lock(&r->mtx) != 0) {
        die("pthread_mutex_lock failed");
    }

    for (int i = 0; i < r->size; i++) {
        if (strcmp(r->data[i].name, item) == 0) {
            ItemState *found = &r->data[i];
            pthread_mutex_unlock(&r->mtx);
            return found;
        }
    }

    pthread_mutex_unlock(&r->mtx);
    die("item not found in registry");
    return NULL;
}

static const char *role_name(Role role) {
    switch (role) {
        case ROLE_WORKER:
            return "Worker";
        case ROLE_MANAGER:
            return "Manager";
        case ROLE_SUPERVISOR:
            return "Supervisor";
        default:
            return "Unknown";
    }
}

static void print_done(Role role, int id, const char *op_name, const char *item) {
    pthread_mutex_lock(&g_print_mtx);
    printf("[%s-%d] %s %s\n", role_name(role), id, op_name, item);
    fflush(stdout);
    pthread_mutex_unlock(&g_print_mtx);
}

static void perform_worker_read(int id, ItemState *st, const char *item) {
    pthread_mutex_lock(&st->mtx);
    while (st->active_writer || st->waiting_supervisors > 0) {
        pthread_cond_wait(&st->cv, &st->mtx);
    }
    st->active_readers++;
    pthread_mutex_unlock(&st->mtx);

    simulate_work(OP_Q2_WORKER_READ);

    pthread_mutex_lock(&st->mtx);
    st->active_readers--;
    if (st->active_readers == 0) {
        pthread_cond_broadcast(&st->cv);
    }
    pthread_mutex_unlock(&st->mtx);

    print_done(ROLE_WORKER, id, "READ", item);
}

static void perform_manager_write(int id, ItemState *st, const char *item) {
    pthread_mutex_lock(&st->mtx);
    while (st->active_writer || st->active_readers > 0) {
        pthread_cond_wait(&st->cv, &st->mtx);
    }
    st->active_writer = 1;
    pthread_mutex_unlock(&st->mtx);

    simulate_work(OP_Q2_MANAGER_HANDLE);

    pthread_mutex_lock(&st->mtx);
    st->active_writer = 0;
    pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mtx);

    print_done(ROLE_MANAGER, id, "WRITE", item);
}

static void perform_supervisor_write(int id, ItemState *st, const char *item) {
    pthread_mutex_lock(&st->mtx);
    st->waiting_supervisors++;
    while (st->active_writer || st->active_readers > 0) {
        pthread_cond_wait(&st->cv, &st->mtx);
    }
    st->waiting_supervisors--;
    st->active_writer = 1;
    pthread_mutex_unlock(&st->mtx);

    simulate_work(OP_Q2_SUPERVISOR_UPDATE);

    pthread_mutex_lock(&st->mtx);
    st->active_writer = 0;
    pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mtx);

    print_done(ROLE_SUPERVISOR, id, "WRITE", item);
}

static void *role_thread_main(void *arg) {
    ThreadCtx *ctx = (ThreadCtx *)arg;
    for (int i = 0; i < ctx->ops->size; i++) {
        Operation *op = &ctx->ops->data[i];
        ItemState *st = registry_get(&g_registry, op->item);

        if (ctx->role == ROLE_WORKER) {
            if (op->op != OP_READ) {
                die("Worker has non-READ operation");
            }
            perform_worker_read(ctx->id, st, op->item);
        } else if (ctx->role == ROLE_MANAGER) {
            if (op->op != OP_WRITE) {
                die("Manager has non-WRITE operation");
            }
            perform_manager_write(ctx->id, st, op->item);
        } else {
            if (op->op != OP_WRITE) {
                die("Supervisor has non-WRITE operation");
            }
            perform_supervisor_write(ctx->id, st, op->item);
        }
    }
    return NULL;
}

static void init_role_op_arrays(void) {
    g_worker_ops = xmalloc((size_t)g_num_workers * sizeof(OpList));
    g_manager_ops = xmalloc((size_t)g_num_managers * sizeof(OpList));
    g_supervisor_ops = xmalloc((size_t)g_num_supervisors * sizeof(OpList));

    for (int i = 0; i < g_num_workers; i++) op_list_init(&g_worker_ops[i]);
    for (int i = 0; i < g_num_managers; i++) op_list_init(&g_manager_ops[i]);
    for (int i = 0; i < g_num_supervisors; i++) op_list_init(&g_supervisor_ops[i]);
}

static void destroy_role_op_arrays(void) {
    for (int i = 0; i < g_num_workers; i++) op_list_destroy(&g_worker_ops[i]);
    for (int i = 0; i < g_num_managers; i++) op_list_destroy(&g_manager_ops[i]);
    for (int i = 0; i < g_num_supervisors; i++) op_list_destroy(&g_supervisor_ops[i]);
    free(g_worker_ops);
    free(g_manager_ops);
    free(g_supervisor_ops);
}

static void parse_input(const char *path) {
    FILE *fp = fopen(path, "r");
    if (fp == NULL) {
        die("fopen failed");
    }

    if (fscanf(fp, "%d %d %d %d", &g_num_workers, &g_num_managers, &g_num_supervisors, &g_num_ops) != 4) {
        fclose(fp);
        die("invalid config line");
    }

    if (g_num_workers < 0 || g_num_managers < 0 || g_num_supervisors < 0 || g_num_ops < 0) {
        fclose(fp);
        die("negative values in config");
    }

    init_role_op_arrays();

    for (int i = 0; i < g_num_ops; i++) {
        char role_ch = '\0';
        int id = -1;
        char op_str[16];
        char item[ITEM_NAME_MAX + 1];
        Operation op;

        if (fscanf(fp, " %c %d %15s %127s", &role_ch, &id, op_str, item) != 4) {
            fclose(fp);
            die("invalid operation line");
        }

        if (strcmp(op_str, "READ") == 0) {
            op.op = OP_READ;
        } else if (strcmp(op_str, "WRITE") == 0) {
            op.op = OP_WRITE;
        } else {
            fclose(fp);
            die("unknown operation type");
        }

        strncpy(op.item, item, ITEM_NAME_MAX);
        op.item[ITEM_NAME_MAX] = '\0';

        if (role_ch == 'W') {
            if (id < 0 || id >= g_num_workers) {
                fclose(fp);
                die("worker id out of range");
            }
            if (op.op != OP_READ) {
                fclose(fp);
                die("worker can only READ");
            }
            op_list_push(&g_worker_ops[id], &op);
        } else if (role_ch == 'M') {
            if (id < 0 || id >= g_num_managers) {
                fclose(fp);
                die("manager id out of range");
            }
            if (op.op != OP_WRITE) {
                fclose(fp);
                die("manager can only WRITE");
            }
            op_list_push(&g_manager_ops[id], &op);
        } else if (role_ch == 'S') {
            if (id < 0 || id >= g_num_supervisors) {
                fclose(fp);
                die("supervisor id out of range");
            }
            if (op.op != OP_WRITE) {
                fclose(fp);
                die("supervisor can only WRITE");
            }
            op_list_push(&g_supervisor_ops[id], &op);
        } else {
            fclose(fp);
            die("unknown role");
        }
    }

    fclose(fp);
}

static void preregister_items(void) {
    for (int i = 0; i < g_num_workers; i++) {
        for (int j = 0; j < g_worker_ops[i].size; j++) {
            registry_get_or_create(&g_registry, g_worker_ops[i].data[j].item);
        }
    }

    for (int i = 0; i < g_num_managers; i++) {
        for (int j = 0; j < g_manager_ops[i].size; j++) {
            registry_get_or_create(&g_registry, g_manager_ops[i].data[j].item);
        }
    }

    for (int i = 0; i < g_num_supervisors; i++) {
        for (int j = 0; j < g_supervisor_ops[i].size; j++) {
            registry_get_or_create(&g_registry, g_supervisor_ops[i].data[j].item);
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <q2_input_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    parse_input(argv[1]);
    registry_init(&g_registry);
    preregister_items();

    int total_threads = g_num_workers + g_num_managers + g_num_supervisors;
    pthread_t *threads = xmalloc((size_t)total_threads * sizeof(pthread_t));
    ThreadCtx *ctx = xmalloc((size_t)total_threads * sizeof(ThreadCtx));
    int t = 0;

    for (int i = 0; i < g_num_workers; i++) {
        ctx[t].role = ROLE_WORKER;
        ctx[t].id = i;
        ctx[t].ops = &g_worker_ops[i];
        if (pthread_create(&threads[t], NULL, role_thread_main, &ctx[t]) != 0) {
            die("pthread_create worker failed");
        }
        t++;
    }

    for (int i = 0; i < g_num_managers; i++) {
        ctx[t].role = ROLE_MANAGER;
        ctx[t].id = i;
        ctx[t].ops = &g_manager_ops[i];
        if (pthread_create(&threads[t], NULL, role_thread_main, &ctx[t]) != 0) {
            die("pthread_create manager failed");
        }
        t++;
    }

    for (int i = 0; i < g_num_supervisors; i++) {
        ctx[t].role = ROLE_SUPERVISOR;
        ctx[t].id = i;
        ctx[t].ops = &g_supervisor_ops[i];
        if (pthread_create(&threads[t], NULL, role_thread_main, &ctx[t]) != 0) {
            die("pthread_create supervisor failed");
        }
        t++;
    }

    for (int i = 0; i < total_threads; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            die("pthread_join failed");
        }
    }

    free(threads);
    free(ctx);
    registry_destroy(&g_registry);
    destroy_role_op_arrays();

    return EXIT_SUCCESS;
}
