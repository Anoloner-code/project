#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOB_ID_MAX 63
#define BAR_WIDTH 40

typedef struct {
    char id[JOB_ID_MAX + 1];
    int arrival;
    int burst;
    int priority;
    int index;
} Job;

typedef struct {
    Job job;
    int remaining;
    int completion;
} SimJob;

typedef struct {
    int job_index;
    int start;
    int end;
} Segment;

typedef struct {
    Segment *data;
    int size;
    int cap;
} SegmentList;

typedef struct {
    const char *label;
    int *completion;
    double avg_turnaround;
    double avg_waiting;
    double cpu_util;
    SegmentList gantt;
} ScheduleResult;

typedef struct {
    int *data;
    int head;
    int tail;
    int size;
    int cap;
} IntQueue;

/* die - Print error message to stderr and exit.
 * Args: msg - error message
 * Return: does not return */
static void die(const char *msg) {
    fprintf(stderr, "%s\n", msg);
    exit(EXIT_FAILURE);
}

/* xmalloc - Allocate memory; exit on failure.
 * Args: size - number of bytes
 * Return: pointer to allocated memory */
static void *xmalloc(size_t size) {
    void *ptr = malloc(size);

    if (ptr == NULL) {
        die("malloc failed");
    }
    return ptr;
}

/* xrealloc - Reallocate memory; exit on failure.
 * Args: ptr - existing pointer, size - new byte size
 * Return: pointer to reallocated memory */
static void *xrealloc(void *ptr, size_t size) {
    void *next = realloc(ptr, size);

    if (next == NULL) {
        die("realloc failed");
    }
    return next;
}

/* segments_init - Initialise an empty Gantt segment list.
 * Args: list - segment list to initialise
 * Return: void */
static void segments_init(SegmentList *list) {
    list->data = NULL;
    list->size = 0;
    list->cap = 0;
}

/* segments_push - Append a Gantt segment; merges with previous if contiguous and same job.
 * Args: list - segment list, job_index - job index (-1 for idle), start - start time, end - end time
 * Return: void */
static void segments_push(SegmentList *list, int job_index, int start, int end) {
    Segment *last;

    if (start >= end) {
        return;
    }

    if (list->size > 0) {
        last = &list->data[list->size - 1];
        if (last->job_index == job_index && last->end == start) {
            last->end = end;
            return;
        }
    }

    if (list->size == list->cap) {
        int new_cap = (list->cap == 0) ? 8 : list->cap * 2;
        list->data = xrealloc(list->data, (size_t)new_cap * sizeof(Segment));
        list->cap = new_cap;
    }

    list->data[list->size].job_index = job_index;
    list->data[list->size].start = start;
    list->data[list->size].end = end;
    list->size++;
}

/* segments_destroy - Free segment list storage.
 * Args: list - segment list to destroy
 * Return: void */
static void segments_destroy(SegmentList *list) {
    free(list->data);
    list->data = NULL;
    list->size = 0;
    list->cap = 0;
}

/* queue_init - Initialise an integer circular queue.
 * Args: queue - queue to initialise, initial_cap - initial capacity
 * Return: void */
static void queue_init(IntQueue *queue, int initial_cap) {
    queue->cap = (initial_cap > 0) ? initial_cap : 8;
    queue->data = xmalloc((size_t)queue->cap * sizeof(int));
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;
}

/* queue_grow - Double the queue capacity (internal helper).
 * Args: queue - queue to grow
 * Return: void */
static void queue_grow(IntQueue *queue) {
    int *new_data = xmalloc((size_t)queue->cap * 2u * sizeof(int));

    for (int i = 0; i < queue->size; i++) {
        new_data[i] = queue->data[(queue->head + i) % queue->cap];
    }

    free(queue->data);
    queue->data = new_data;
    queue->cap *= 2;
    queue->head = 0;
    queue->tail = queue->size;
}

/* queue_push - Enqueue a value.
 * Args: queue - target queue, value - integer to enqueue
 * Return: void */
static void queue_push(IntQueue *queue, int value) {
    if (queue->size == queue->cap) {
        queue_grow(queue);
    }

    queue->data[queue->tail] = value;
    queue->tail = (queue->tail + 1) % queue->cap;
    queue->size++;
}

/* queue_pop - Dequeue and return the front value; exits on underflow.
 * Args: queue - source queue
 * Return: dequeued integer */
static int queue_pop(IntQueue *queue) {
    int value;

    if (queue->size == 0) {
        die("queue underflow");
    }

    value = queue->data[queue->head];
    queue->head = (queue->head + 1) % queue->cap;
    queue->size--;
    return value;
}

/* queue_empty - Check if the queue has no elements.
 * Args: queue - queue to check
 * Return: 1 if empty, 0 otherwise */
static int queue_empty(const IntQueue *queue) {
    return queue->size == 0;
}

/* queue_destroy - Free queue storage.
 * Args: queue - queue to destroy
 * Return: void */
static void queue_destroy(IntQueue *queue) {
    free(queue->data);
    queue->data = NULL;
    queue->head = 0;
    queue->tail = 0;
    queue->size = 0;
    queue->cap = 0;
}

/* compare_jobs_by_arrival - qsort comparator: order by arrival time, then input index.
 * Args: lhs, rhs - pointers to Job structs
 * Return: negative/zero/positive per qsort convention */
static int compare_jobs_by_arrival(const void *lhs, const void *rhs) {
    const Job *a = lhs;
    const Job *b = rhs;

    if (a->arrival != b->arrival) {
        return (a->arrival < b->arrival) ? -1 : 1;
    }
    return (a->index < b->index) ? -1 : (a->index > b->index);
}

/* parse_jobs - Read job definitions from a file.
 * Args: path - input file path, jobs_out - output pointer to allocated Job array
 * Return: number of jobs read */
static int parse_jobs(const char *path, Job **jobs_out) {
    FILE *fp = fopen(path, "r");
    Job *jobs = NULL;
    int size = 0;
    int cap = 0;

    if (fp == NULL) {
        die("fopen failed");
    }

    for (;;) {
        Job job;
        int scanned = fscanf(fp, " %63s %d %d %d", job.id, &job.arrival, &job.burst, &job.priority);

        if (scanned == EOF) {
            break;
        }
        if (scanned != 4) {
            fclose(fp);
            free(jobs);
            die("invalid job line");
        }
        if (job.arrival < 0 || job.burst <= 0 || job.priority < 0) {
            fclose(fp);
            free(jobs);
            die("job values out of range");
        }

        if (size == cap) {
            int new_cap = (cap == 0) ? 8 : cap * 2;
            jobs = xrealloc(jobs, (size_t)new_cap * sizeof(Job));
            cap = new_cap;
        }

        job.index = size;
        jobs[size++] = job;
    }

    fclose(fp);
    *jobs_out = jobs;
    return size;
}

/* make_sim_jobs - Create simulation state array from parsed jobs.
 * Args: jobs - source job array, count - number of jobs
 * Return: allocated SimJob array with remaining times set to burst */
static SimJob *make_sim_jobs(const Job *jobs, int count) {
    SimJob *sim_jobs = xmalloc((size_t)count * sizeof(SimJob));

    for (int i = 0; i < count; i++) {
        sim_jobs[i].job = jobs[i];
        sim_jobs[i].remaining = jobs[i].burst;
        sim_jobs[i].completion = -1;
    }

    return sim_jobs;
}

/* total_burst_time - Sum burst times of all jobs.
 * Args: jobs - job array, count - number of jobs
 * Return: total burst time */
static int total_burst_time(const Job *jobs, int count) {
    int total = 0;

    for (int i = 0; i < count; i++) {
        total += jobs[i].burst;
    }
    return total;
}

/* finalize_metrics - Compute avg turnaround, avg waiting, and CPU utilisation.
 * Args: result - output result, jobs - original jobs, sim_jobs - simulation state,
 *       count - job count, total_burst - sum of all burst times
 * Return: void */
static void finalize_metrics(ScheduleResult *result, const Job *jobs, const SimJob *sim_jobs, int count, int total_burst) {
    double total_turnaround = 0.0;
    double total_waiting = 0.0;
    int makespan = 0;

    result->completion = (count > 0) ? xmalloc((size_t)count * sizeof(int)) : NULL;

    for (int i = 0; i < count; i++) {
        int completion = sim_jobs[i].completion;
        int turnaround = completion - jobs[i].arrival;
        int waiting = turnaround - jobs[i].burst;

        result->completion[i] = completion;
        total_turnaround += turnaround;
        total_waiting += waiting;
        if (completion > makespan) {
            makespan = completion;
        }
    }

    if (count == 0) {
        result->avg_turnaround = 0.0;
        result->avg_waiting = 0.0;
        result->cpu_util = 0.0;
        return;
    }

    result->avg_turnaround = total_turnaround / count;
    result->avg_waiting = total_waiting / count;
    result->cpu_util = (makespan == 0) ? 0.0 : (100.0 * (double)total_burst) / (double)makespan;
}

/* make_empty_result - Create a zeroed ScheduleResult with the given label.
 * Args: label - algorithm name string
 * Return: initialised ScheduleResult */
static ScheduleResult make_empty_result(const char *label) {
    ScheduleResult result;

    result.label = label;
    result.completion = NULL;
    result.avg_turnaround = 0.0;
    result.avg_waiting = 0.0;
    result.cpu_util = 0.0;
    segments_init(&result.gantt);
    return result;
}

/* sjf_better - Comparator for SJF: prefer shorter remaining, earlier arrival, lower index.
 * Args: candidate, best - SimJob pointers to compare
 * Return: 1 if candidate should preempt best, 0 otherwise */
static int sjf_better(const SimJob *candidate, const SimJob *best) {
    if (candidate->remaining != best->remaining) {
        return candidate->remaining < best->remaining;
    }
    if (candidate->job.arrival != best->job.arrival) {
        return candidate->job.arrival < best->job.arrival;
    }
    return candidate->job.index < best->job.index;
}

/* priority_better - Comparator for Priority: prefer lower priority number, then SJF ties.
 * Args: candidate, best - SimJob pointers to compare
 * Return: 1 if candidate should preempt best, 0 otherwise */
static int priority_better(const SimJob *candidate, const SimJob *best) {
    if (candidate->job.priority != best->job.priority) {
        return candidate->job.priority < best->job.priority;
    }
    if (candidate->remaining != best->remaining) {
        return candidate->remaining < best->remaining;
    }
    if (candidate->job.arrival != best->job.arrival) {
        return candidate->job.arrival < best->job.arrival;
    }
    return candidate->job.index < best->job.index;
}

/* next_arrival_time - Find the earliest arrival after current_time among incomplete jobs.
 * Args: sim_jobs - simulation array, count - job count, current_time - current clock
 * Return: next arrival time, or INT_MAX if none */
static int next_arrival_time(const SimJob *sim_jobs, int count, int current_time) {
    int next = INT_MAX;

    for (int i = 0; i < count; i++) {
        if (sim_jobs[i].remaining > 0 && sim_jobs[i].job.arrival > current_time && sim_jobs[i].job.arrival < next) {
            next = sim_jobs[i].job.arrival;
        }
    }
    return next;
}

/* select_ready_job - Choose the best ready job at current_time using the given comparator.
 * Args: sim_jobs - simulation array, count - job count, current_time - current clock,
 *       better - comparator function
 * Return: index of selected job, or -1 if none ready */
static int select_ready_job(const SimJob *sim_jobs, int count, int current_time,
    int (*better)(const SimJob *, const SimJob *)) {
    int best_index = -1;

    for (int i = 0; i < count; i++) {
        if (sim_jobs[i].remaining <= 0 || sim_jobs[i].job.arrival > current_time) {
            continue;
        }
        if (best_index == -1 || better(&sim_jobs[i], &sim_jobs[best_index])) {
            best_index = i;
        }
    }

    return best_index;
}

/* simulate_fcfs - Run First-Come-First-Served scheduling simulation.
 * Args: jobs - job array, count - job count, total_burst - sum of burst times
 * Return: ScheduleResult with Gantt chart and metrics */
static ScheduleResult simulate_fcfs(const Job *jobs, int count, int total_burst) {
    Job *ordered = NULL;
    SimJob *sim_jobs = make_sim_jobs(jobs, count);
    ScheduleResult result = make_empty_result("FCFS");
    int current_time = 0;

    if (count > 0) {
        ordered = xmalloc((size_t)count * sizeof(Job));
        memcpy(ordered, jobs, (size_t)count * sizeof(Job));
        qsort(ordered, (size_t)count, sizeof(Job), compare_jobs_by_arrival);
    }

    for (int i = 0; i < count; i++) {
        int idx = ordered[i].index;
        int start = current_time;
        int end;

        if (current_time < jobs[idx].arrival) {
            segments_push(&result.gantt, -1, current_time, jobs[idx].arrival);
            start = jobs[idx].arrival;
        }

        end = start + jobs[idx].burst;
        segments_push(&result.gantt, idx, start, end);
        sim_jobs[idx].remaining = 0;
        sim_jobs[idx].completion = end;
        current_time = end;
    }

    finalize_metrics(&result, jobs, sim_jobs, count, total_burst);
    free(ordered);
    free(sim_jobs);
    return result;
}

/* simulate_preemptive - Run a preemptive scheduling simulation (SJF or Priority).
 * Args: jobs - job array, count - job count, total_burst - sum of burst times,
 *       label - algorithm name, better - comparator for job selection
 * Return: ScheduleResult with Gantt chart and metrics */
static ScheduleResult simulate_preemptive(const Job *jobs, int count, int total_burst,
    const char *label, int (*better)(const SimJob *, const SimJob *)) {
    SimJob *sim_jobs = make_sim_jobs(jobs, count);
    ScheduleResult result = make_empty_result(label);
    int current_time = 0;
    int completed = 0;

    while (completed < count) {
        int selected = select_ready_job(sim_jobs, count, current_time, better);

        if (selected == -1) {
            int next = next_arrival_time(sim_jobs, count, current_time);

            if (next == INT_MAX) {
                break;
            }
            segments_push(&result.gantt, -1, current_time, next);
            current_time = next;
            continue;
        }

        segments_push(&result.gantt, selected, current_time, current_time + 1);
        sim_jobs[selected].remaining--;
        current_time++;

        if (sim_jobs[selected].remaining == 0) {
            sim_jobs[selected].completion = current_time;
            completed++;
        }
    }

    finalize_metrics(&result, jobs, sim_jobs, count, total_burst);
    free(sim_jobs);
    return result;
}

/* enqueue_arrivals - Add all jobs arriving at or before current_time to the ready queue.
 * Args: queue - ready queue, ordered - arrival-sorted jobs, count - job count,
 *       next_arrival_idx - pointer to next index to check, current_time - current clock
 * Return: void */
static void enqueue_arrivals(IntQueue *queue, const Job *ordered, int count, int *next_arrival_idx, int current_time) {
    while (*next_arrival_idx < count && ordered[*next_arrival_idx].arrival <= current_time) {
        queue_push(queue, ordered[*next_arrival_idx].index);
        (*next_arrival_idx)++;
    }
}

/* simulate_rr - Run Round-Robin scheduling simulation with the given time quantum.
 * Args: jobs - job array, count - job count, total_burst - sum of burst times,
 *       quantum - time slice, label - algorithm name
 * Return: ScheduleResult with Gantt chart and metrics */
static ScheduleResult simulate_rr(const Job *jobs, int count, int total_burst, int quantum, const char *label) {
    Job *ordered = NULL;
    SimJob *sim_jobs = make_sim_jobs(jobs, count);
    ScheduleResult result = make_empty_result(label);
    IntQueue queue;
    int current_time = 0;
    int completed = 0;
    int next_arrival_idx = 0;
    int current_job = -1;
    int slice_used = 0;

    queue_init(&queue, count + 1);

    if (count > 0) {
        ordered = xmalloc((size_t)count * sizeof(Job));
        memcpy(ordered, jobs, (size_t)count * sizeof(Job));
        qsort(ordered, (size_t)count, sizeof(Job), compare_jobs_by_arrival);
    }

    while (completed < count) {
        enqueue_arrivals(&queue, ordered, count, &next_arrival_idx, current_time);

        if (current_job == -1) {
            if (queue_empty(&queue)) {
                if (next_arrival_idx >= count) {
                    break;
                }
                segments_push(&result.gantt, -1, current_time, ordered[next_arrival_idx].arrival);
                current_time = ordered[next_arrival_idx].arrival;
                enqueue_arrivals(&queue, ordered, count, &next_arrival_idx, current_time);
            }

            current_job = queue_pop(&queue);
            slice_used = 0;
        }

        segments_push(&result.gantt, current_job, current_time, current_time + 1);
        sim_jobs[current_job].remaining--;
        current_time++;
        slice_used++;

        enqueue_arrivals(&queue, ordered, count, &next_arrival_idx, current_time);

        if (sim_jobs[current_job].remaining == 0) {
            sim_jobs[current_job].completion = current_time;
            current_job = -1;
            slice_used = 0;
            completed++;
        } else if (slice_used == quantum) {
            queue_push(&queue, current_job);
            current_job = -1;
            slice_used = 0;
        }
    }

    finalize_metrics(&result, jobs, sim_jobs, count, total_burst);
    free(ordered);
    free(sim_jobs);
    queue_destroy(&queue);
    return result;
}

/* segment_name - Return the job id string for a Gantt segment, or "idle".
 * Args: segment - Gantt segment, jobs - job array
 * Return: name string */
static const char *segment_name(const Segment *segment, const Job *jobs) {
    return (segment->job_index == -1) ? "idle" : jobs[segment->job_index].id;
}

/* print_gantt - Print a Gantt chart to stdout.
 * Args: gantt - segment list, jobs - job array
 * Return: void */
static void print_gantt(const SegmentList *gantt, const Job *jobs) {
    if (gantt->size == 0) {
        printf("Gantt: | |\n");
        printf("Time: 0\n");
        return;
    }

    printf("Gantt: ");
    for (int i = 0; i < gantt->size; i++) {
        printf("| %s ", segment_name(&gantt->data[i], jobs));
    }
    printf("|\n");

    printf("Time: %d", gantt->data[0].start);
    for (int i = 0; i < gantt->size; i++) {
        printf(" %d", gantt->data[i].end);
    }
    printf("\n");
}

/* print_metrics - Print per-job metrics table and averages.
 * Args: result - schedule result, jobs - job array, count - job count
 * Return: void */
static void print_metrics(const ScheduleResult *result, const Job *jobs, int count) {
    printf("%-8s %-8s %-8s %-12s %-12s %-8s\n",
        "Job", "Arrival", "Burst", "Completion", "Turnaround", "Waiting");

    for (int i = 0; i < count; i++) {
        int completion = result->completion[i];
        int turnaround = completion - jobs[i].arrival;
        int waiting = turnaround - jobs[i].burst;

        printf("%-8s %-8d %-8d %-12d %-12d %-8d\n",
            jobs[i].id,
            jobs[i].arrival,
            jobs[i].burst,
            completion,
            turnaround,
            waiting);
    }

    printf("Average Turnaround Time : %.2f\n", result->avg_turnaround);
    printf("Average Waiting Time : %.2f\n", result->avg_waiting);
    printf("CPU Utilisation : %.2f%%\n", result->cpu_util);
}

/* print_schedule - Print Gantt chart and metrics for one algorithm.
 * Args: result - schedule result, jobs - job array, count - job count
 * Return: void */
static void print_schedule(const ScheduleResult *result, const Job *jobs, int count) {
    printf("=== %s ===\n", result->label);
    print_gantt(&result->gantt, jobs);
    print_metrics(result, jobs, count);
    printf("\n");
}

/* scaled_bar_length - Compute bar chart width proportional to value/max_value.
 * Args: value - data point, max_value - maximum data point
 * Return: character width for the bar */
static int scaled_bar_length(double value, double max_value) {
    if (max_value <= 0.0 || value <= 0.0) {
        return 0;
    }

    return (int)((value / max_value) * BAR_WIDTH + 0.5);
}

/* print_comparison - Print comparative analysis table and bar chart across algorithms.
 * Args: results - array of schedule results, count - number of results
 * Return: void */
static void print_comparison(const ScheduleResult *results, int count) {
    double max_wait = 0.0;

    printf("=== Comparative Analysis ===\n");
    printf("%-24s %-20s %-18s %-12s\n",
        "Algorithm", "Avg Turnaround Time", "Avg Waiting Time", "CPU Util%");

    for (int i = 0; i < count; i++) {
        if (results[i].avg_waiting > max_wait) {
            max_wait = results[i].avg_waiting;
        }

        printf("%-24s %-20.2f %-18.2f %.2f%%\n",
            results[i].label,
            results[i].avg_turnaround,
            results[i].avg_waiting,
            results[i].cpu_util);
    }

    printf("\n=== Avg Waiting Time Bar Chart ===\n");
    for (int i = 0; i < count; i++) {
        int width = scaled_bar_length(results[i].avg_waiting, max_wait);

        printf("%-24s | ", results[i].label);
        for (int j = 0; j < width; j++) {
            putchar('#');
        }
        printf(" %.2f\n", results[i].avg_waiting);
    }
}

/* destroy_result - Free a ScheduleResult's dynamically allocated memory.
 * Args: result - result to destroy
 * Return: void */
static void destroy_result(ScheduleResult *result) {
    free(result->completion);
    result->completion = NULL;
    segments_destroy(&result->gantt);
}

/* simulate_mlfq - Run Multi-Level Feedback Queue scheduling simulation.
 *                 Level 1: RR q=4, Level 2: RR q=8, Level 3: FCFS (run to completion).
 *                 Jobs start at Level 1 and are demoted when they exhaust their quantum.
 * Args: jobs - job array, count - job count, total_burst - sum of burst times
 * Return: ScheduleResult with Gantt chart and metrics */
static ScheduleResult simulate_mlfq(const Job *jobs, int count, int total_burst) {
    Job *ordered = NULL;
    SimJob *sim_jobs = make_sim_jobs(jobs, count);
    ScheduleResult result = make_empty_result("MLFQ");
    IntQueue q1, q2, q3;
    int *level;
    int current_time = 0;
    int completed = 0;
    int next_arrival_idx = 0;
    int current_job = -1;
    int slice_used = 0;
    int current_quantum = 0;

    queue_init(&q1, count + 1);
    queue_init(&q2, count + 1);
    queue_init(&q3, count + 1);
    level = xmalloc((size_t)count * sizeof(int));
    for (int i = 0; i < count; i++) {
        level[i] = 1;
    }

    if (count > 0) {
        ordered = xmalloc((size_t)count * sizeof(Job));
        memcpy(ordered, jobs, (size_t)count * sizeof(Job));
        qsort(ordered, (size_t)count, sizeof(Job), compare_jobs_by_arrival);
    }

    while (completed < count) {
        while (next_arrival_idx < count && ordered[next_arrival_idx].arrival <= current_time) {
            int idx = ordered[next_arrival_idx].index;
            queue_push(&q1, idx);
            level[idx] = 1;
            next_arrival_idx++;
        }

        if (current_job == -1) {
            if (!queue_empty(&q1)) {
                current_job = queue_pop(&q1);
                current_quantum = 4;
            } else if (!queue_empty(&q2)) {
                current_job = queue_pop(&q2);
                current_quantum = 8;
            } else if (!queue_empty(&q3)) {
                current_job = queue_pop(&q3);
                current_quantum = sim_jobs[current_job].remaining;
            } else {
                if (next_arrival_idx >= count) {
                    break;
                }
                segments_push(&result.gantt, -1, current_time, ordered[next_arrival_idx].arrival);
                current_time = ordered[next_arrival_idx].arrival;
                continue;
            }
            slice_used = 0;
        }

        segments_push(&result.gantt, current_job, current_time, current_time + 1);
        sim_jobs[current_job].remaining--;
        current_time++;
        slice_used++;

        while (next_arrival_idx < count && ordered[next_arrival_idx].arrival <= current_time) {
            int idx = ordered[next_arrival_idx].index;
            queue_push(&q1, idx);
            level[idx] = 1;
            next_arrival_idx++;
        }

        if (sim_jobs[current_job].remaining == 0) {
            sim_jobs[current_job].completion = current_time;
            current_job = -1;
            slice_used = 0;
            completed++;
        } else if (slice_used == current_quantum) {
            int cur_level = level[current_job];
            if (cur_level < 3) {
                level[current_job] = cur_level + 1;
            }
            if (level[current_job] == 2) {
                queue_push(&q2, current_job);
            } else {
                queue_push(&q3, current_job);
            }
            current_job = -1;
            slice_used = 0;
        }
    }

    finalize_metrics(&result, jobs, sim_jobs, count, total_burst);
    free(ordered);
    free(sim_jobs);
    free(level);
    queue_destroy(&q1);
    queue_destroy(&q2);
    queue_destroy(&q3);
    return result;
}

int main(int argc, char **argv) {
    Job *jobs = NULL;
    ScheduleResult results[6];
    int count;
    int total_burst;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <input-file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    count = parse_jobs(argv[1], &jobs);
    total_burst = total_burst_time(jobs, count);

    results[0] = simulate_fcfs(jobs, count, total_burst);
    results[1] = simulate_preemptive(jobs, count, total_burst, "SJF (Preemptive)", sjf_better);
    results[2] = simulate_preemptive(jobs, count, total_burst, "Priority (Preemptive)", priority_better);
    results[3] = simulate_rr(jobs, count, total_burst, 3, "RR (q=3)");
    results[4] = simulate_rr(jobs, count, total_burst, 6, "RR (q=6)");
    results[5] = simulate_mlfq(jobs, count, total_burst);

    for (int i = 0; i < 6; i++) {
        print_schedule(&results[i], jobs, count);
    }
    print_comparison(results, 6);

    for (int i = 0; i < 6; i++) {
        destroy_result(&results[i]);
    }

    free(jobs);
    return EXIT_SUCCESS;
}