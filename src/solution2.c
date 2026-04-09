#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    char name[ITEM_NAME_MAX + 1];
    unsigned int size_bytes;
} DirectoryEntry;

typedef struct {
    DirectoryEntry *data;
    int size;
    int cap;
} DirectoryRegistry;

typedef struct {
    OpType op;
    int entry_index;
} Operation;

typedef struct {
    Operation *data;
    int size;
    int cap;
} OpList;

typedef struct {
    int active_readers;
    int writer_active;
    int waiting_managers;
    int waiting_supervisors;
    unsigned long read_overlap_epoch;
    pthread_mutex_t mtx;
    pthread_cond_t cv;
} DirectoryState;

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

static DirectoryRegistry g_registry;
static DirectoryState g_directory;
static pthread_mutex_t g_print_mtx = PTHREAD_MUTEX_INITIALIZER;

/* die - Print error message to stderr and exit.
 * Args: msg - error message string
 * Return: does not return */
static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

/* xmalloc - Allocate memory; exit on failure.
 * Args: size - number of bytes
 * Return: pointer to allocated memory, or NULL if size is 0 */
static void *xmalloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    void *ptr = malloc(size);
    if (ptr == NULL) {
        die("malloc failed");
    }
    return ptr;
}

/* print_line - Thread-safe formatted print with newline and flush.
 * Args: fmt - printf format string, ... - format arguments
 * Return: void */
static void print_line(const char *fmt, ...) {
    va_list args;

    pthread_mutex_lock(&g_print_mtx);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    putchar('\n');
    fflush(stdout);
    pthread_mutex_unlock(&g_print_mtx);
}

/* op_list_init - Initialise an empty operation list.
 * Args: list - list to initialise
 * Return: void */
static void op_list_init(OpList *list) {
    list->data = NULL;
    list->size = 0;
    list->cap = 0;
}

/* op_list_push - Append an operation to the list, growing if needed.
 * Args: list - target list, op - operation to append
 * Return: void */
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

/* op_list_destroy - Free the operation list's internal storage.
 * Args: list - list to destroy
 * Return: void */
static void op_list_destroy(OpList *list) {
    free(list->data);
    list->data = NULL;
    list->size = 0;
    list->cap = 0;
}

/* registry_init - Initialise an empty directory registry.
 * Args: registry - registry to initialise
 * Return: void */
static void registry_init(DirectoryRegistry *registry) {
    registry->data = NULL;
    registry->size = 0;
    registry->cap = 0;
}

/* registry_destroy - Free registry internal storage.
 * Args: registry - registry to destroy
 * Return: void */
static void registry_destroy(DirectoryRegistry *registry) {
    free(registry->data);
    registry->data = NULL;
    registry->size = 0;
    registry->cap = 0;
}

/* parse_item_number - Extract the numeric portion from an item name string.
 * Args: item_name - item name (e.g., "item_0042.dat")
 * Return: extracted integer, or 0 if no digits found */
static int parse_item_number(const char *item_name) {
    int value = 0;
    int found_digit = 0;

    for (const char *p = item_name; *p != '\0'; p++) {
        if (*p >= '0' && *p <= '9') {
            found_digit = 1;
            value = value * 10 + (*p - '0');
        }
    }

    return found_digit ? value : 0;
}

/* initial_size_for_item - Compute the initial byte size for a directory entry.
 * Args: item_name - item name string
 * Return: initial size in bytes */
static unsigned int initial_size_for_item(const char *item_name) {
    int item_number = parse_item_number(item_name);
    unsigned int multiplier = 1u << (item_number % 4);
    return 512u * multiplier;
}

/* registry_get_or_add - Look up an item by name; add it if not found.
 * Args: registry - directory registry, item_name - name to look up
 * Return: index of the entry in the registry */
static int registry_get_or_add(DirectoryRegistry *registry, const char *item_name) {
    for (int i = 0; i < registry->size; i++) {
        if (strcmp(registry->data[i].name, item_name) == 0) {
            return i;
        }
    }

    if (registry->size == registry->cap) {
        int new_cap = (registry->cap == 0) ? 16 : registry->cap * 2;
        DirectoryEntry *new_data = realloc(registry->data, (size_t)new_cap * sizeof(DirectoryEntry));
        if (new_data == NULL) {
            die("realloc failed");
        }
        registry->data = new_data;
        registry->cap = new_cap;
    }

    DirectoryEntry *entry = &registry->data[registry->size];
    strncpy(entry->name, item_name, ITEM_NAME_MAX);
    entry->name[ITEM_NAME_MAX] = '\0';
    entry->size_bytes = initial_size_for_item(item_name);
    registry->size++;
    return registry->size - 1;
}

/* registry_get_entry - Retrieve a directory entry by index.
 * Args: registry - directory registry, entry_index - zero-based index
 * Return: pointer to the DirectoryEntry */
static DirectoryEntry *registry_get_entry(DirectoryRegistry *registry, int entry_index) {
    if (entry_index < 0 || entry_index >= registry->size) {
        die("invalid directory entry index");
    }
    return &registry->data[entry_index];
}

/* directory_state_init - Initialise directory concurrency state (mutex + cond var).
 * Args: state - state to initialise
 * Return: void */
static void directory_state_init(DirectoryState *state) {
    memset(state, 0, sizeof(*state));
    if (pthread_mutex_init(&state->mtx, NULL) != 0) {
        die("pthread_mutex_init failed");
    }
    if (pthread_cond_init(&state->cv, NULL) != 0) {
        die("pthread_cond_init failed");
    }
}

/* directory_state_destroy - Destroy directory concurrency state.
 * Args: state - state to destroy
 * Return: void */
static void directory_state_destroy(DirectoryState *state) {
    pthread_mutex_destroy(&state->mtx);
    pthread_cond_destroy(&state->cv);
}

/* init_role_op_arrays - Allocate and initialise per-thread operation lists for each role.
 * Args: none (uses globals g_num_workers, g_num_managers, g_num_supervisors)
 * Return: void */
static void init_role_op_arrays(void) {
    g_worker_ops = xmalloc((size_t)g_num_workers * sizeof(OpList));
    g_manager_ops = xmalloc((size_t)g_num_managers * sizeof(OpList));
    g_supervisor_ops = xmalloc((size_t)g_num_supervisors * sizeof(OpList));

    for (int i = 0; i < g_num_workers; i++) {
        op_list_init(&g_worker_ops[i]);
    }
    for (int i = 0; i < g_num_managers; i++) {
        op_list_init(&g_manager_ops[i]);
    }
    for (int i = 0; i < g_num_supervisors; i++) {
        op_list_init(&g_supervisor_ops[i]);
    }
}

/* destroy_role_op_arrays - Free all per-thread operation lists.
 * Args: none
 * Return: void */
static void destroy_role_op_arrays(void) {
    for (int i = 0; i < g_num_workers; i++) {
        op_list_destroy(&g_worker_ops[i]);
    }
    for (int i = 0; i < g_num_managers; i++) {
        op_list_destroy(&g_manager_ops[i]);
    }
    for (int i = 0; i < g_num_supervisors; i++) {
        op_list_destroy(&g_supervisor_ops[i]);
    }

    free(g_worker_ops);
    free(g_manager_ops);
    free(g_supervisor_ops);
    g_worker_ops = NULL;
    g_manager_ops = NULL;
    g_supervisor_ops = NULL;
}

/* parse_input - Read and parse the Q2 input file (thread counts + operation list).
 * Args: path - file path
 * Return: void; populates global arrays and registry */
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

    registry_init(&g_registry);
    init_role_op_arrays();

    for (int i = 0; i < g_num_ops; i++) {
        char role_ch = '\0';
        int id = -1;
        char op_str[16];
        char item_name[ITEM_NAME_MAX + 1];
        Operation op;

        if (fscanf(fp, " %c %d %15s %127s", &role_ch, &id, op_str, item_name) != 4) {
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

        op.entry_index = registry_get_or_add(&g_registry, item_name);

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

/* perform_worker_read - Execute a worker's read operation with priority-aware blocking.
 * Args: worker_id - worker thread index, entry - directory entry to read
 * Return: void */
static void perform_worker_read(int worker_id, DirectoryEntry *entry) {
    int waiting_message_printed = 0;
    unsigned long start_overlap_epoch;
    int concurrent_read = 0;
    unsigned int observed_size;

    pthread_mutex_lock(&g_directory.mtx);
    while (g_directory.writer_active || g_directory.waiting_supervisors > 0 || g_directory.waiting_managers > 0) {
        if (!waiting_message_printed && g_directory.waiting_supervisors > 0) {
            print_line("[Worker-%d] [worker blocked: supervisor pending] waiting...", worker_id);
            waiting_message_printed = 1;
        }
        pthread_cond_wait(&g_directory.cv, &g_directory.mtx);
    }

    start_overlap_epoch = g_directory.read_overlap_epoch;
    if (g_directory.active_readers > 0) {
        g_directory.read_overlap_epoch++;
        concurrent_read = 1;
    }
    g_directory.active_readers++;
    observed_size = entry->size_bytes;
    pthread_mutex_unlock(&g_directory.mtx);

    simulate_work(OP_Q2_WORKER_READ);

    pthread_mutex_lock(&g_directory.mtx);
    if (!concurrent_read && g_directory.read_overlap_epoch != start_overlap_epoch) {
        concurrent_read = 1;
    }
    g_directory.active_readers--;
    if (g_directory.active_readers == 0) {
        pthread_cond_broadcast(&g_directory.cv);
    }
    pthread_mutex_unlock(&g_directory.mtx);

    if (concurrent_read) {
        print_line("[Worker-%d] [concurrent read] FILE: %s SIZE: %u bytes", worker_id, entry->name, observed_size);
    } else {
        print_line("[Worker-%d] FILE: %s SIZE: %u bytes", worker_id, entry->name, observed_size);
    }
}

/* perform_manager_write - Execute a manager's exclusive write operation.
 * Args: manager_id - manager thread index, entry - directory entry to update
 * Return: void */
static void perform_manager_write(int manager_id, DirectoryEntry *entry) {
    int waiting_message_printed = 0;
    unsigned int old_size;
    unsigned int new_size;

    pthread_mutex_lock(&g_directory.mtx);
    g_directory.waiting_managers++;
    while (g_directory.writer_active || g_directory.active_readers > 0 || g_directory.waiting_supervisors > 0) {
        if (!waiting_message_printed) {
            print_line("[Manager-%d] waiting for write lock", manager_id);
            waiting_message_printed = 1;
        }
        pthread_cond_wait(&g_directory.cv, &g_directory.mtx);
    }

    g_directory.waiting_managers--;
    g_directory.writer_active = 1;
    old_size = entry->size_bytes;
    pthread_mutex_unlock(&g_directory.mtx);

    print_line("[Manager-%d] acquired write lock", manager_id);
    simulate_work(OP_Q2_MANAGER_HANDLE);

    new_size = (old_size > UINT_MAX / 2u) ? UINT_MAX : old_size * 2u;

    pthread_mutex_lock(&g_directory.mtx);
    entry->size_bytes = new_size;
    g_directory.writer_active = 0;
    pthread_cond_broadcast(&g_directory.cv);
    pthread_mutex_unlock(&g_directory.mtx);

    print_line("[Manager-%d] updated %s → %u bytes", manager_id, entry->name, new_size);
}

/* perform_supervisor_write - Execute a supervisor's high-priority exclusive write.
 * Args: supervisor_id - supervisor thread index, entry - directory entry to update
 * Return: void */
static void perform_supervisor_write(int supervisor_id, DirectoryEntry *entry) {
    int preempts_manager = 0;
    unsigned int old_size;
    unsigned int new_size;

    pthread_mutex_lock(&g_directory.mtx);
    g_directory.waiting_supervisors++;
    while (g_directory.writer_active || g_directory.active_readers > 0) {
        pthread_cond_wait(&g_directory.cv, &g_directory.mtx);
    }

    preempts_manager = (g_directory.waiting_managers > 0);
    g_directory.waiting_supervisors--;
    g_directory.writer_active = 1;
    old_size = entry->size_bytes;
    pthread_mutex_unlock(&g_directory.mtx);

    if (preempts_manager) {
        print_line("[Supervisor-%d] [supervisor preempts manager] acquired write lock", supervisor_id);
    } else {
        print_line("[Supervisor-%d] acquired write lock", supervisor_id);
    }

    simulate_work(OP_Q2_SUPERVISOR_UPDATE);

    new_size = (old_size > UINT_MAX / 2u) ? UINT_MAX : old_size * 2u;

    pthread_mutex_lock(&g_directory.mtx);
    entry->size_bytes = new_size;
    g_directory.writer_active = 0;
    pthread_cond_broadcast(&g_directory.cv);
    pthread_mutex_unlock(&g_directory.mtx);

    print_line("[Supervisor-%d] updated %s → %u bytes", supervisor_id, entry->name, new_size);
}

/* role_thread_main - Generic thread entry point; dispatches operations by role.
 * Args: arg - pointer to ThreadCtx with role, id, and operation list
 * Return: NULL */
static void *role_thread_main(void *arg) {
    ThreadCtx *ctx = (ThreadCtx *)arg;

    for (int i = 0; i < ctx->ops->size; i++) {
        Operation *op = &ctx->ops->data[i];
        DirectoryEntry *entry = registry_get_entry(&g_registry, op->entry_index);

        if (ctx->role == ROLE_WORKER) {
            perform_worker_read(ctx->id, entry);
        } else if (ctx->role == ROLE_MANAGER) {
            perform_manager_write(ctx->id, entry);
        } else {
            perform_supervisor_write(ctx->id, entry);
        }
    }

    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <q2_input_file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    parse_input(argv[1]);
    directory_state_init(&g_directory);

    int total_threads = g_num_workers + g_num_managers + g_num_supervisors;
    pthread_t *threads = xmalloc((size_t)total_threads * sizeof(pthread_t));
    ThreadCtx *contexts = xmalloc((size_t)total_threads * sizeof(ThreadCtx));
    int next_thread = 0;

    for (int i = 0; i < g_num_workers; i++) {
        contexts[next_thread].role = ROLE_WORKER;
        contexts[next_thread].id = i;
        contexts[next_thread].ops = &g_worker_ops[i];
        if (pthread_create(&threads[next_thread], NULL, role_thread_main, &contexts[next_thread]) != 0) {
            die("pthread_create worker failed");
        }
        next_thread++;
    }

    for (int i = 0; i < g_num_managers; i++) {
        contexts[next_thread].role = ROLE_MANAGER;
        contexts[next_thread].id = i;
        contexts[next_thread].ops = &g_manager_ops[i];
        if (pthread_create(&threads[next_thread], NULL, role_thread_main, &contexts[next_thread]) != 0) {
            die("pthread_create manager failed");
        }
        next_thread++;
    }

    for (int i = 0; i < g_num_supervisors; i++) {
        contexts[next_thread].role = ROLE_SUPERVISOR;
        contexts[next_thread].id = i;
        contexts[next_thread].ops = &g_supervisor_ops[i];
        if (pthread_create(&threads[next_thread], NULL, role_thread_main, &contexts[next_thread]) != 0) {
            die("pthread_create supervisor failed");
        }
        next_thread++;
    }

    for (int i = 0; i < total_threads; i++) {
        if (pthread_join(threads[i], NULL) != 0) {
            die("pthread_join failed");
        }
    }

    free(threads);
    free(contexts);
    destroy_role_op_arrays();
    directory_state_destroy(&g_directory);
    registry_destroy(&g_registry);

    return EXIT_SUCCESS;
}
