/*
 * oslat - OS latency detector
 *
 * Copyright 2020 Red Hat Inc.
 *
 * Authors: Peter Xu <peterx@redhat.com>
 *
 * Some of the utility code based on sysjitter-1.3:
 * Copyright 2010-2015 David Riddoch <david@riddoch.org.uk>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of version 3 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include "rt-utils.h"

static const char* version = "v0.1.3";


#ifdef __GNUC__
# define atomic_inc(ptr)   __sync_add_and_fetch((ptr), 1)
# if defined(__x86_64__)
#  define relax()          __asm__ __volatile__("pause" ::: "memory") 
static inline void frc(uint64_t* pval)
{
    uint32_t low, high;
    /* See rdtsc_ordered() of Linux */
    __asm__ __volatile__("lfence");
    __asm__ __volatile__("rdtsc" : "=a" (low) , "=d" (high));
    *pval = ((uint64_t) high << 32) | low;
}
# elif defined(__i386__)
#  define relax()          __asm__ __volatile__("pause" ::: "memory") 
static inline void frc(uint64_t* pval)
{
    __asm__ __volatile__("rdtsc" : "=A" (*pval));
}
# elif defined(__PPC64__)
#  define relax()          do{}while(0)
static inline void frc(uint64_t* pval)
{
    __asm__ __volatile__("mfspr %0, 268\n" : "=r" (*pval));
}
# else
#  error Need frc() for this platform.
# endif
#else
# error Need to add support for this compiler.
#endif

typedef uint64_t stamp_t;   /* timestamp */
typedef uint64_t cycles_t;  /* number of cycles */

enum command {
    WAIT,
    GO,
    STOP
};

/*
 * We'll have buckets 1us, 2us, ..., (BUCKET_SIZE) us.
 */
#define  BUCKET_SIZE  (32)

struct thread {
    int                  core_i;
    pthread_t            thread_id;

    /* NOTE! this is also how many ticks per us */
    unsigned             cpu_mhz;
    cycles_t             int_total;
    stamp_t              frc_start;
    stamp_t              frc_stop;
    cycles_t             runtime;
    stamp_t             *buckets;
    /* Maximum latency detected */
    stamp_t              maxlat;
    double               average;
    double               variance;
};

struct global {
    /* Configuration. */
    unsigned              runtime_secs;
    unsigned              n_threads;
    struct timeval        tv_start;
    int                   rtprio;
    int                   bucket_size;
    int                   trace_threshold;
    int                   runtime;
    char *                cpu_list;
    char *                app_name;

    /* Mutable state. */
    volatile enum command cmd;
    volatile unsigned     n_threads_started;
    volatile unsigned     n_threads_ready;
    volatile unsigned     n_threads_running;
    volatile unsigned     n_threads_finished;
};

static struct global g;

#define TEST(x)                                 \
    do {                                        \
        if( ! (x) )                             \
            test_fail(#x, __LINE__);            \
    } while( 0 )

#define TEST0(x)  TEST((x) == 0)

static void test_fail(const char* what, int line)
{
    fprintf(stderr, "ERROR:\n");
    fprintf(stderr, "ERROR: TEST(%s)\n", what);
    fprintf(stderr, "ERROR: at line %d\n", line);
    fprintf(stderr, "ERROR: errno=%d (%s)\n", errno, strerror(errno));
    fprintf(stderr, "ERROR:\n");
    exit(1);
}

static int move_to_core(int core_i)
{
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(core_i, &cpus);
    return sched_setaffinity(0, sizeof(cpus), &cpus);
}

static cycles_t __measure_cpu_hz(void)
{
    struct timeval tvs, tve;
    stamp_t s, e;
    double sec;

    frc(&s);
    e = s;
    gettimeofday(&tvs, NULL);
    while( e - s < 1000000 )
        frc(&e);
    gettimeofday(&tve, NULL);
    sec = tve.tv_sec - tvs.tv_sec + (tve.tv_usec - tvs.tv_usec) / 1e6;
    return (cycles_t) ((e - s) / sec);
}

static unsigned measure_cpu_mhz(void)
{
    cycles_t m, mprev, d;

    mprev = __measure_cpu_hz();
    do {
        m = __measure_cpu_hz();
        if( m > mprev )  d = m - mprev;
        else             d = mprev - m;
        mprev = m;
    } while( d > m / 1000 );

    return (unsigned) (m / 1000000);
}

static void thread_init(struct thread* t)
{
    t->cpu_mhz = measure_cpu_mhz();
    t->maxlat = 0;
    TEST(t->buckets = calloc(1, sizeof(t->buckets[0]) * g.bucket_size));
}

static float cycles_to_sec(const struct thread* t, uint64_t cycles)
{
    return cycles / (t->cpu_mhz * 1e6);
}

static void insert_bucket(struct thread *t, stamp_t value)
{
    int index, us;

    index = value / t->cpu_mhz;
    assert(index >= 0);
    us = index + 1;
    assert(us > 0);

    if (g.trace_threshold && us >= g.trace_threshold) {
        char *line = "%s: Trace threshold (%d us) triggered with %u us!  "
            "Stopping the test.\n";
        tracemark(line, g.app_name, g.trace_threshold, us);
        err_quit(line, g.app_name, g.trace_threshold, us);
    }

    /* Update max latency */
    if (us > t->maxlat) {
        t->maxlat = us;
    }

    /* Too big the jitter; put into the last bucket */
    if (index >= g.bucket_size) {
        index = g.bucket_size - 1;
    }

    t->buckets[index]++;
    if (t->buckets[index] == 0) {
        printf("Bucket %d overflowed\n", index);
        exit(1);
    }
}

static void doit(struct thread* t)
{
    stamp_t ts1, ts2;

    frc(&ts2);
    do {
        frc(&ts1);
        insert_bucket(t, ts1 - ts2);
        frc(&ts2);
        insert_bucket(t, ts2 - ts1);
    } while (g.cmd == GO);
}

static int set_fifo_prio(int prio)
{
    struct sched_param param;

    memset(&param, 0, sizeof(param));
    param.sched_priority = prio;
    return sched_setscheduler(0, SCHED_FIFO, &param);
}

static void* thread_main(void* arg)
{
    /* Important thing to note here is that once we start bashing the CPU, we
     * need to keep doing so to prevent the core from changing frequency or
     * dropping into a low power state.
     */
    struct thread* t = arg;

    /* Alloc memory in the thread itself after setting affinity to get the
     * best chance of getting numa-local memory.  Doesn't matter so much for
     * the "struct thread" since we expect that to stay cache resident.
     */
    TEST(move_to_core(t->core_i) == 0);
    if (g.rtprio)
        TEST(set_fifo_prio(g.rtprio) == 0);

    /* Don't bash the cpu until all threads have got going. */
    atomic_inc(&g.n_threads_started);
    while( g.cmd == WAIT )
        usleep(1000);

    thread_init(t);

    /* Ensure we all start at the same time. */
    atomic_inc(&g.n_threads_running);
    while( g.n_threads_running != g.n_threads )
        relax();

    frc(&t->frc_start);
    doit(t);
    frc(&t->frc_stop);

    t->runtime = t->frc_stop - t->frc_start;

    /* Wait for everyone to finish so we don't disturb them by exiting and
     * waking the main thread.
     */
    atomic_inc(&g.n_threads_finished);
    while( g.n_threads_finished != g.n_threads )
        relax();

    return NULL;
}

#define putfield(label, val, fmt, end) do {     \
        printf("%12s:\t", label);               \
        for (i = 0; i < g.n_threads; ++i)       \
            printf(" %"fmt, val);               \
        printf("%s\n", end);                    \
    } while (0)

void calculate(struct thread *t)
{
    int i, j;
    double sum, tmp;
    uint64_t count;

    for (i = 0; i < g.n_threads; ++i) {
        /* Calculate average */
        sum = count = 0;
        for (j = 0; j < g.bucket_size; j++) {
            sum += 1.0 * t[i].buckets[j] * (j+1);
            count += t[i].buckets[j];
        }
        t[i].average = sum / count;

        /* Calculate variance */
        sum = 0;
        for (j = 0; j < g.bucket_size; j++) {
            tmp = (j+1 - t[i].average);
            tmp *= tmp;
            sum += tmp * t[i].buckets[j];
        }
        t[i].variance = sqrt(sum / count);
    }
}

static void write_summary(struct thread* t)
{
    int i, j;
    char bucket_name[64];

    calculate(t);

    putfield("Core", t[i].core_i, "d", "");
    putfield("CPU Freq", t[i].cpu_mhz, "u", " (Mhz)");

    for (j = 0; j < g.bucket_size; j++) {
        snprintf(bucket_name, sizeof(bucket_name), "%03d (us)", j+1);
        putfield(bucket_name, t[i].buckets[j], PRIu64, "");
    }

    putfield("Max Latency", t[i].maxlat, PRIu64, " (us)");
    putfield("Average", t[i].average, ".2lf", " (us)");
    putfield("Variance", t[i].variance, ".2lf", " (us)");
    putfield("Duration", cycles_to_sec(&(t[i]), t[i].runtime),
             ".3f", " (sec)");
    printf("\n");
}

static void run_expt(struct thread* threads, int runtime_secs)
{
    int i;

    g.runtime_secs = runtime_secs;
    g.n_threads_started = 0;
    g.n_threads_ready = 0;
    g.n_threads_running = 0;
    g.n_threads_finished = 0;
    g.cmd = WAIT;

    for( i = 0; i < g.n_threads; ++i )
        TEST0(pthread_create(&(threads[i].thread_id), NULL,
                             thread_main, &(threads[i])));
    while( g.n_threads_started != g.n_threads )
        usleep(1000);
    gettimeofday(&g.tv_start, NULL);
    g.cmd = GO;

    alarm(runtime_secs);

    /* Go to sleep until the threads have done their stuff. */
    for( i = 0; i < g.n_threads; ++i )
        pthread_join(threads[i].thread_id, NULL);
}

static void cleanup_expt(struct thread* threads)
{
    int i;

    for( i = 0; i < g.n_threads; ++i ) {
        free(threads[i].buckets);
        threads[i].buckets = NULL;
    }
}

static void handle_alarm(int code)
{
    g.cmd = STOP;
}

const char *helpmsg =
    "Usage: %s [options]\n"
    "\n"
    "This is an OS latency detector by running busy loops on specified cores.\n"
    "Please run this tool using root.\n"
    "\n"
    "Available options:\n"
    "\n"
    "  -b, --bucket-size      Specify the number of the buckets (4-1024)\n"
    "  -t, --runtime          Specify test duration, e.g., 60, 20m, 2H\n"
    "                         (m/M: minutes, h/H: hours, d/D: days)\n"
    "  -c, --cpu-list         Specify CPUs to run on, e.g. '1,3,5,7-15'\n"
    "  -f, --rtprio           Using SCHED_FIFO priority (1-99)\n"
    "  -T, --trace-threshold  Stop the test when threshold triggered (in us),\n"
    "                         print a marker in ftrace and stop ftrace too.\n"
    ;

static void usage(void)
{
    printf(helpmsg, g.app_name);
    exit(1);
}

/* TODO: use libnuma? */
static int parse_cpu_list(char *cpu_list, cpu_set_t *cpu_set)
{
    struct bitmask *cpu_mask;
    int i, n_cores;

    n_cores = sysconf(_SC_NPROCESSORS_CONF);

    if (!cpu_list) {
        for (i = 0; i < n_cores; i++)
            CPU_SET(i, cpu_set);
        return n_cores;
    }

    cpu_mask = numa_parse_cpustring_all(cpu_list);
    if (cpu_mask) {
        for (i = 0; i < n_cores; i++) {
            if (numa_bitmask_isbitset(cpu_mask, i)) {
                CPU_SET(i, cpu_set);
            }
        }
        numa_bitmask_free(cpu_mask);
    } else {
        warn("Unknown cpu-list: %s, using all available cpus\n", cpu_list);
        for (i = 0; i < n_cores; i++)
            CPU_SET(i, cpu_set);
    }

    return n_cores;
}

static int parse_runtime(const char *str)
{
    char *endptr;
    int v = strtol(str, &endptr, 10);

    printf("SCANNED: %d, ENDPTR: %d\n", v, *endptr);

    if (!*endptr) {
        return v;
    }

    switch (*endptr) {
    case 'd':
    case 'D':
        /* Days */
        v *= 24;
    case 'h':
    case 'H':
        /* Hours */
        v *= 60;
    case 'm':
    case 'M':
        /* Minutes */
        v *= 60;
    case 's':
    case 'S':
        /* Seconds */
        break;
    default:
        printf("Unknown runtime suffix: %s\n", endptr);
        v = 0;
        break;
    }

    return v;
}

/* Process commandline options */
static void parse_options(int argc, char *argv[])
{
    while (1) {
		static struct option options[] = {
			{ "bucket-size", required_argument, NULL, 'b' },
			{ "cpu-list", required_argument, NULL, 'c' },
			{ "runtime", required_argument, NULL, 't' },
			{ "rtprio", required_argument, NULL, 'f' },
			{ "help", no_argument, NULL, 'h' },
			{ "trace-threshold", required_argument, NULL, 'T' },
			{ NULL, 0, NULL, 0 },
		};
		int c = getopt_long(argc, argv, "b:c:f:ht:T:", options, NULL);

		if (c == -1)
			break;

		switch (c) {
        case 'b':
            g.bucket_size = strtol(optarg, NULL, 10);
            if (g.bucket_size > 1024 || g.bucket_size <= 4) {
                printf("Illegal bucket size: %s (should be: 4-1024)\n",
                       optarg);
                exit(1);
            }
            break;
        case 'c':
            g.cpu_list = strdup(optarg);
            break;
        case 't':
            g.runtime = parse_runtime(optarg);
            if (!g.runtime) {
                printf("Illegal runtime: %s\n", optarg);
                exit(1);
            }
            break;
        case 'f':
            g.rtprio = strtol(optarg, NULL, 10);
            if (g.rtprio < 1 || g.rtprio > 99) {
                printf("Illegal RT priority: %s (should be: 1-99)\n", optarg);
                exit(1);
            }
            break;
        case 'T':
            g.trace_threshold = strtol(optarg, NULL, 10);
            if (g.trace_threshold <= 0) {
                printf("Parameter --trace-threshold needs to be positive\n");
                exit(1);
            }
            enable_trace_mark();
            break;
        default:
            usage();
            break;
		}
	}
}

void dump_globals(void)
{
    printf("Total runtime: \t\t%d seconds\n", g.runtime);
    printf("Thread priority: \t");
    if (g.rtprio) {
        printf("SCHED_FIFO:%d\n", g.rtprio);
    } else {
        printf("default\n");
    }
    printf("CPU list: \t\t%s\n", g.cpu_list ?: "(all cores)");
    printf("\n");
}

int main(int argc, char* argv[])
{
    struct thread* threads;
    int i, n_cores;
    cpu_set_t cpu_set;

    CPU_ZERO(&cpu_set);

    g.app_name = argv[0];
    g.rtprio = 0;
    g.bucket_size = BUCKET_SIZE;
    g.runtime = 1;

    printf("\nVersion: %s\n\n", version);

    parse_options(argc, argv);

    TEST(mlockall(MCL_CURRENT | MCL_FUTURE) == 0);

    n_cores = parse_cpu_list(g.cpu_list, &cpu_set);

    TEST( threads = calloc(1, CPU_COUNT(&cpu_set) * sizeof(threads[0])) );
    for( i = 0; i < n_cores; ++i )
        if (CPU_ISSET(i, &cpu_set) && move_to_core(i) == 0)
            threads[g.n_threads++].core_i = i;

    if (CPU_ISSET(0, &cpu_set) && g.rtprio) {
        printf("WARNING: Running SCHED_FIFO workload on CPU 0 "
               "may hang the main thread\n");
    }

    TEST(move_to_core(0) == 0);

    signal(SIGALRM, handle_alarm);
    signal(SIGINT, handle_alarm);
    signal(SIGTERM, handle_alarm);

    dump_globals();

    printf("Pre-heat for 1 seconds...\n");
    run_expt(threads, 1);
    cleanup_expt(threads);
    printf("Test starts...\n");
    run_expt(threads, g.runtime);
    printf("Test completed.\n\n");

    write_summary(threads);

    if (g.cpu_list) {
        free(g.cpu_list);
        g.cpu_list = NULL;
    }

    return 0;
}
