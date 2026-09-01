#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int32_t dx;
    int32_t dy;
    int32_t dz;
    uint8_t rotation;
} Observation;

typedef struct {
    int32_t x_min;
    int32_t x_max;
    int32_t y_min;
    int32_t y_max;
    int32_t z_min;
    int32_t z_max;
} SearchRange;

typedef struct {
    uint64_t paid_source_columns;
    uint64_t generated_source_blocks;
    uint64_t prefix_survivors;
    uint64_t verifier_evaluations;
    uint64_t matches;
} ScanStats;

typedef struct {
    int32_t x;
    int32_t y;
    int32_t z;
} Match;

typedef struct {
    Match *data;
    size_t count;
    size_t capacity;
} MatchList;

typedef struct {
    const char *pattern_path;
    SearchRange range;
    int64_t tile_size;
    int threads;
    bool print_matches;
} Options;

typedef struct {
    const Observation *observations;
    size_t observation_count;
    const SearchRange *range;
    int64_t tile_size;
    int64_t work_begin;
    int64_t work_end;
    bool fast_path;
    ScanStats stats;
    MatchList matches;
    int status;
} Worker;

_Static_assert(sizeof(Observation) == 16, "Observation ABI");
_Static_assert(offsetof(Observation, rotation) == 12, "Observation ABI");
_Static_assert(sizeof(SearchRange) == 24, "SearchRange ABI");
_Static_assert(sizeof(ScanStats) == 40, "ScanStats ABI");

uint8_t fas_vanilla3_rotation(int32_t x, int32_t y, int32_t z);
int32_t fas_scan_worker(const Observation *observations, size_t observation_count,
                        const SearchRange *range, int64_t tile_size,
                        int64_t z_begin, int64_t z_end, uint8_t *context,
                        ScanStats *stats);
int32_t fas_scan_general_worker(const Observation *observations,
                                size_t observation_count,
                                const SearchRange *range, int64_t x_begin,
                                int64_t x_end, uint8_t *context,
                                ScanStats *stats);

uint8_t *fas_allocate_memory(size_t size, size_t alignment) {
    if (alignment <= _Alignof(max_align_t)) return malloc(size);
    if (size > SIZE_MAX - alignment + 1) return NULL;
    size_t rounded = (size + alignment - 1) & ~(alignment - 1);
    return aligned_alloc(alignment, rounded);
}

void fas_release_memory(uint8_t *value) {
    free(value);
}

static void usage(const char *program, int status) {
    FILE *stream = status == 0 ? stdout : stderr;
    fprintf(stream,
            "Usage: %s PATTERN [options]\n\n"
            "Pattern lines are: dx dy dz rotation.\n"
            "Arbitrary dy is supported.\n"
            "Ranges are half-open and origin-Y height may contain at most 64 values.\n\n"
            "Options:\n"
            "  --size N          centered N x N X/Z plane\n"
            "  --x-min N         default -5000\n"
            "  --x-max N         default  5000\n"
            "  --z-min N         default -5000\n"
            "  --z-max N         default  5000\n"
            "  --y-min N         default -60\n"
            "  --y-max N         default 1\n"
            "  --tile-size N     cache tile edge, default 1024, max 1024\n"
            "  --threads N       pthread workers, default 1\n"
            "  --quiet           do not print individual matches\n"
            "  --help            show this help\n",
            program);
    exit(status);
}

static bool parse_i64(const char *text, int64_t *result) {
    char *end = NULL;
    errno = 0;
    long long value = strtoll(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return false;
    *result = (int64_t)value;
    return true;
}

static int32_t option_i32(const char *option, const char *text) {
    int64_t value;
    if (!parse_i64(text, &value) || value < INT32_MIN || value > INT32_MAX) {
        fprintf(stderr, "error: invalid integer for %s: %s\n", option, text);
        exit(1);
    }
    return (int32_t)value;
}

static int64_t option_i64(const char *option, const char *text) {
    int64_t value;
    if (!parse_i64(text, &value)) {
        fprintf(stderr, "error: invalid integer for %s: %s\n", option, text);
        exit(1);
    }
    return value;
}

static Options parse_options(int argc, char **argv) {
    if (argc < 2) usage(argv[0], 2);
    Options options = {
        .pattern_path = NULL,
        .range = {-5000, 5000, -60, 1, -5000, 5000},
        .tile_size = 1024,
        .threads = 1,
        .print_matches = true,
    };
    bool size_set = false;
    int64_t requested_size = 0;
    for (int index = 1; index < argc; ++index) {
        const char *argument = argv[index];
        if (strcmp(argument, "--help") == 0) usage(argv[0], 0);
        if (strcmp(argument, "--quiet") == 0) {
            options.print_matches = false;
            continue;
        }
        if (strncmp(argument, "--", 2) != 0) {
            if (options.pattern_path != NULL) {
                fprintf(stderr, "error: only one pattern file is allowed\n");
                exit(1);
            }
            options.pattern_path = argument;
            continue;
        }
        if (++index == argc) {
            fprintf(stderr, "error: missing value after %s\n", argument);
            exit(1);
        }
        const char *value = argv[index];
        if (strcmp(argument, "--size") == 0) {
            requested_size = option_i64(argument, value);
            if (requested_size < 1 || requested_size > INT32_MAX) {
                fprintf(stderr, "error: --size must be in [1, INT32_MAX]\n");
                exit(1);
            }
            size_set = true;
        } else if (strcmp(argument, "--x-min") == 0) {
            options.range.x_min = option_i32(argument, value);
        } else if (strcmp(argument, "--x-max") == 0) {
            options.range.x_max = option_i32(argument, value);
        } else if (strcmp(argument, "--z-min") == 0) {
            options.range.z_min = option_i32(argument, value);
        } else if (strcmp(argument, "--z-max") == 0) {
            options.range.z_max = option_i32(argument, value);
        } else if (strcmp(argument, "--y-min") == 0) {
            options.range.y_min = option_i32(argument, value);
        } else if (strcmp(argument, "--y-max") == 0) {
            options.range.y_max = option_i32(argument, value);
        } else if (strcmp(argument, "--tile-size") == 0) {
            options.tile_size = option_i64(argument, value);
        } else if (strcmp(argument, "--threads") == 0) {
            int64_t threads = option_i64(argument, value);
            if (threads < 1 || threads > INT_MAX) {
                fprintf(stderr, "error: --threads must be in [1, INT_MAX]\n");
                exit(1);
            }
            options.threads = (int)threads;
        } else {
            fprintf(stderr, "error: unknown option: %s\n", argument);
            exit(1);
        }
    }
    if (options.pattern_path == NULL) {
        fprintf(stderr, "error: a pattern file is required\n");
        exit(1);
    }
    if (size_set) {
        int64_t minimum = -(requested_size / 2);
        int64_t maximum = minimum + requested_size;
        options.range.x_min = options.range.z_min = (int32_t)minimum;
        options.range.x_max = options.range.z_max = (int32_t)maximum;
    }
    return options;
}

static void append_observation(Observation **data, size_t *count,
                               size_t *capacity, Observation value) {
    if (*count == *capacity) {
        size_t next = *capacity == 0 ? 32 : *capacity * 2;
        Observation *replacement = realloc(*data, next * sizeof(*replacement));
        if (replacement == NULL) {
            fprintf(stderr, "error: allocation failed\n");
            exit(1);
        }
        *data = replacement;
        *capacity = next;
    }
    (*data)[(*count)++] = value;
}

static Observation *read_pattern(const char *path, size_t *count) {
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        fprintf(stderr, "error: cannot open pattern file: %s\n", path);
        exit(1);
    }
    char *line = NULL;
    size_t line_capacity = 0;
    size_t line_number = 0;
    size_t capacity = 0;
    Observation *observations = NULL;
    while (getline(&line, &line_capacity, file) >= 0) {
        ++line_number;
        char *comment = strchr(line, '#');
        if (comment != NULL) *comment = '\0';
        char extra;
        long long dx, dy, dz, rotation;
        int fields = sscanf(line, " %lld %lld %lld %lld %c", &dx, &dy, &dz,
                            &rotation, &extra);
        if (fields == EOF) continue;
        if (fields != 4) {
            fprintf(stderr, "error: %s:%zu: expected: dx dy dz rotation\n",
                    path, line_number);
            exit(1);
        }
        if (dx < INT32_MIN || dx > INT32_MAX || dy < INT32_MIN ||
            dy > INT32_MAX || dz < INT32_MIN || dz > INT32_MAX ||
            rotation < 0 || rotation > 3) {
            fprintf(stderr, "error: %s:%zu: value is outside its supported range\n",
                    path, line_number);
            exit(1);
        }
        append_observation(&observations, count, &capacity,
                           (Observation){(int32_t)dx, (int32_t)dy,
                                         (int32_t)dz, (uint8_t)rotation});
    }
    free(line);
    fclose(file);
    return observations;
}

static void validate(const Observation *observations, size_t count,
                     const Options *options) {
    const SearchRange *range = &options->range;
    if (count == 0 || count > 255) {
        fprintf(stderr, "error: pattern must contain between 1 and 255 observations\n");
        exit(1);
    }
    if (range->x_min >= range->x_max || range->y_min >= range->y_max ||
        range->z_min >= range->z_max) {
        fprintf(stderr, "error: all search ranges must be non-empty\n");
        exit(1);
    }
    if ((int64_t)range->y_max - range->y_min > 64) {
        fprintf(stderr, "error: origin-Y height may contain at most 64 values\n");
        exit(1);
    }
    if (range->x_min == INT32_MIN || range->z_min == INT32_MIN) {
        fprintf(stderr, "error: X/Z ranges may not contain INT32_MIN\n");
        exit(1);
    }
    if (options->tile_size < 1 || options->tile_size > 1024) {
        fprintf(stderr, "error: tile size must be in [1,1024]\n");
        exit(1);
    }
    for (size_t index = 0; index < count; ++index) {
        const Observation *observation = &observations[index];
        int64_t y_min = (int64_t)range->y_min + observation->dy;
        int64_t y_max = (int64_t)range->y_max - 1 + observation->dy;
        int64_t x_min = (int64_t)range->x_min + observation->dx;
        int64_t x_max = (int64_t)range->x_max - 1 + observation->dx;
        int64_t z_min = (int64_t)range->z_min + observation->dz;
        int64_t z_max = (int64_t)range->z_max - 1 + observation->dz;
        if (y_min < INT32_MIN || y_max > INT32_MAX) {
            fprintf(stderr, "error: Y range plus observation offsets exceeds int32\n");
            exit(1);
        }
        if (x_min < -INT32_MAX || x_max > INT32_MAX ||
            z_min < -INT32_MAX || z_max > INT32_MAX) {
            fprintf(stderr, "error: range plus observation offsets exceeds int32\n");
            exit(1);
        }
    }
}

bool fas_record_match(uint8_t *context, int32_t x, int32_t y, int32_t z) {
    Worker *worker = (Worker *)context;
    MatchList *matches = &worker->matches;
    if (matches->count == matches->capacity) {
        size_t next = matches->capacity == 0 ? 16 : matches->capacity * 2;
        Match *replacement = realloc(matches->data, next * sizeof(*replacement));
        if (replacement == NULL) return false;
        matches->data = replacement;
        matches->capacity = next;
    }
    matches->data[matches->count++] = (Match){x, y, z};
    return true;
}

static void *run_worker(void *argument) {
    Worker *worker = argument;
    if (worker->fast_path) {
        worker->status = fas_scan_worker(
            worker->observations, worker->observation_count, worker->range,
            worker->tile_size, worker->work_begin, worker->work_end,
            (uint8_t *)worker, &worker->stats);
    } else {
        worker->status = fas_scan_general_worker(
            worker->observations, worker->observation_count, worker->range,
            worker->work_begin, worker->work_end, (uint8_t *)worker,
            &worker->stats);
    }
    return NULL;
}

static int compare_matches(const void *left_pointer, const void *right_pointer) {
    const Match *left = left_pointer;
    const Match *right = right_pointer;
    if (left->z != right->z) return left->z < right->z ? -1 : 1;
    if (left->x != right->x) return left->x < right->x ? -1 : 1;
    if (left->y != right->y) return left->y < right->y ? -1 : 1;
    return 0;
}

static double seconds_between(struct timespec begin, struct timespec end) {
    return (double)(end.tv_sec - begin.tv_sec) +
           (double)(end.tv_nsec - begin.tv_nsec) / 1000000000.0;
}

int main(int argc, char **argv) {
    Options options = parse_options(argc, argv);
    size_t observation_count = 0;
    Observation *observations = read_pattern(options.pattern_path,
                                             &observation_count);
    validate(observations, observation_count, &options);

    bool fast_path = options.range.y_min >= -60 && options.range.y_max <= 1;
    for (size_t index = 0; index < observation_count; ++index) {
        if (observations[index].dy != 0 && observations[index].dy != 1) {
            fast_path = false;
        }
    }

    int64_t canonical_z_max = 0;
    int64_t positive_z = (int64_t)options.range.z_max - 1;
    int64_t negative_z = -(int64_t)options.range.z_min;
    if (positive_z > canonical_z_max) canonical_z_max = positive_z;
    if (negative_z > canonical_z_max) canonical_z_max = negative_z;
    const int64_t band_size = 64;
    int64_t canonical_z_count = canonical_z_max + 1;
    int64_t band_count = (canonical_z_count + band_size - 1) / band_size;
    int64_t work_count = fast_path ? band_count :
                         (int64_t)options.range.x_max - options.range.x_min;
    int worker_count = options.threads;
    if ((int64_t)worker_count > work_count) worker_count = (int)work_count;
    if (worker_count < 1) worker_count = 1;

    Worker *workers = calloc((size_t)worker_count, sizeof(*workers));
    pthread_t *threads = calloc((size_t)worker_count, sizeof(*threads));
    if (workers == NULL || threads == NULL) {
        fprintf(stderr, "error: allocation failed\n");
        return 1;
    }
    for (int index = 0; index < worker_count; ++index) {
        workers[index].observations = observations;
        workers[index].observation_count = observation_count;
        workers[index].range = &options.range;
        workers[index].tile_size = options.tile_size;
        workers[index].fast_path = fast_path;
        if (fast_path) {
            int64_t band_begin = band_count * index / worker_count;
            int64_t band_end = band_count * (index + 1) / worker_count;
            workers[index].work_begin = band_begin * band_size;
            workers[index].work_end = band_end * band_size;
            if (workers[index].work_end > canonical_z_count) {
                workers[index].work_end = canonical_z_count;
            }
        } else {
            workers[index].work_begin = (int64_t)options.range.x_min +
                work_count * index / worker_count;
            workers[index].work_end = (int64_t)options.range.x_min +
                work_count * (index + 1) / worker_count;
        }
    }

    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    if (worker_count == 1) {
        run_worker(&workers[0]);
    } else {
        int created = 0;
        for (; created < worker_count - 1; ++created) {
            int error = pthread_create(&threads[created], NULL, run_worker,
                                       &workers[created]);
            if (error != 0) {
                fprintf(stderr, "error: pthread_create: %s\n", strerror(error));
                break;
            }
        }
        if (created == worker_count - 1) run_worker(&workers[worker_count - 1]);
        for (int index = 0; index < created; ++index) {
            pthread_join(threads[index], NULL);
        }
        if (created != worker_count - 1) return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &finished);

    ScanStats stats = {0};
    MatchList matches = {0};
    for (int index = 0; index < worker_count; ++index) {
        if (workers[index].status != 0) {
            fprintf(stderr, "error: Fas worker failed with status %d\n",
                    workers[index].status);
            return 1;
        }
        stats.paid_source_columns += workers[index].stats.paid_source_columns;
        stats.generated_source_blocks += workers[index].stats.generated_source_blocks;
        stats.prefix_survivors += workers[index].stats.prefix_survivors;
        stats.verifier_evaluations += workers[index].stats.verifier_evaluations;
        stats.matches += workers[index].stats.matches;
        if (workers[index].matches.count != 0) {
            size_t next_count = matches.count + workers[index].matches.count;
            Match *replacement = realloc(matches.data, next_count * sizeof(*replacement));
            if (replacement == NULL) {
                fprintf(stderr, "error: allocation failed\n");
                return 1;
            }
            matches.data = replacement;
            memcpy(matches.data + matches.count, workers[index].matches.data,
                   workers[index].matches.count * sizeof(*matches.data));
            matches.count = next_count;
        }
    }
    if (matches.count > 1)
        qsort(matches.data, matches.count, sizeof(*matches.data), compare_matches);
    if (options.print_matches) {
        for (size_t index = 0; index < matches.count; ++index) {
            printf("%" PRId32 " %" PRId32 " %" PRId32 "\n",
                   matches.data[index].x, matches.data[index].y,
                   matches.data[index].z);
        }
    }

    uint64_t width = (uint64_t)((int64_t)options.range.x_max - options.range.x_min);
    uint64_t depth = (uint64_t)((int64_t)options.range.z_max - options.range.z_min);
    uint64_t height = (uint64_t)((int64_t)options.range.y_max - options.range.y_min);
    __uint128_t columns_wide = (__uint128_t)width * depth;
    __uint128_t positions_wide = columns_wide * height;
    if (columns_wide > UINT64_MAX || positions_wide > UINT64_MAX) {
        fprintf(stderr, "error: candidate count exceeds uint64\n");
        return 1;
    }
    uint64_t candidate_columns = (uint64_t)columns_wide;
    uint64_t candidate_positions = (uint64_t)positions_wide;
    double elapsed = seconds_between(started, finished);
    double rate = elapsed == 0.0 ? 0.0 :
                  (double)candidate_positions / elapsed / 1000000.0;
    fprintf(stderr,
            "threads=%d\n"
            "threads_requested=%d\n"
            "elapsed_seconds=%.3f\n"
            "candidate_columns=%" PRIu64 "\n"
            "candidate_positions=%" PRIu64 "\n"
            "paid_source_columns=%" PRIu64 "\n"
            "generated_source_blocks=%" PRIu64 "\n"
            "prefix_survivors=%" PRIu64 "\n"
            "verifier_evaluations=%" PRIu64 "\n"
            "matches=%" PRIu64 "\n"
            "million_positions_per_second=%.3f\n",
            worker_count, options.threads, elapsed, candidate_columns,
            candidate_positions,
            stats.paid_source_columns, stats.generated_source_blocks,
            stats.prefix_survivors, stats.verifier_evaluations, stats.matches,
            rate);

    for (int index = 0; index < worker_count; ++index) {
        free(workers[index].matches.data);
    }
    free(matches.data);
    free(threads);
    free(workers);
    free(observations);
    return 0;
}
