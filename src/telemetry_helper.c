#include <thermal/simd/telemetry_helper.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MSR_IA32_APERF
#define MSR_IA32_APERF 0xE8
#endif
#ifndef MSR_IA32_MPERF
#define MSR_IA32_MPERF 0xE7
#endif

#include <thermal/simd/logging.h>
#include <thermal/simd/metrics.h>

#define LOG_COMPONENT "telemetry"
#define INITIAL_BACKOFF_SEC 5
#define MAX_BACKOFF_SEC 600
#define TSD_TEMP_MIN_MILLIC (-50000LL)
#define TSD_TEMP_MAX_MILLIC 150000LL

/* Retry scheduling is elapsed-time state and must not move with CLOCK_REALTIME. */
static time_t tsd_now_seconds(void) {
    struct timespec now = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0) return 0;
    return now.tv_sec;
}

static void schedule_retry(time_t *deadline, int *backoff) {
    if (!deadline || !backoff) return;
    if (*backoff <= 0) *backoff = INITIAL_BACKOFF_SEC;
    int next = *backoff > MAX_BACKOFF_SEC ? MAX_BACKOFF_SEC : *backoff;
    *deadline = tsd_now_seconds() + (time_t)next;
    if (*backoff < MAX_BACKOFF_SEC) {
        int doubled = *backoff * 2;
        *backoff = doubled > MAX_BACKOFF_SEC ? MAX_BACKOFF_SEC : doubled;
    }
}

static void reset_retry(time_t *deadline, int *backoff) {
    if (deadline) *deadline = 0;
    if (backoff) *backoff = INITIAL_BACKOFF_SEC;
}

static int read_text_from_path(const char *path, char *buffer, size_t size) {
    if (!path || !buffer || size < 2) {
        errno = EINVAL;
        return -1;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buffer, (int)size, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    size_t len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r' ||
                       buffer[len - 1] == ' ' || buffer[len - 1] == '\t')) {
        buffer[--len] = '\0';
    }
    return 0;
}

static void lowercase_ascii(char *text) {
    if (!text) return;
    for (char *p = text; *p; ++p) {
        if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    }
}

static int read_long_from_path(const char *path, long long *value) {
    if (!path || !value) {
        errno = EINVAL;
        return -1;
    }
    char buffer[64];
    if (read_text_from_path(path, buffer, sizeof(buffer)) != 0) return -1;
    char *endptr = NULL;
    errno = 0;
    long long parsed = strtoll(buffer, &endptr, 10);
    if (errno != 0 || endptr == buffer || *endptr != '\0') return -1;
    *value = parsed;
    return 0;
}

static int copy_path(char *dest, size_t dest_size, const char *path) {
    if (!dest || !path || dest_size == 0) return -1;
    int written = snprintf(dest, dest_size, "%s", path);
    return written >= 0 && (size_t)written < dest_size ? 0 : -1;
}

static int physical_package_id(int cpu) {
    char path[256];
    int written = snprintf(path, sizeof(path),
                           "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", cpu);
    if (written < 0 || (size_t)written >= sizeof(path)) return -1;
    long long package = -1;
    if (read_long_from_path(path, &package) != 0 || package < 0 || package > 4096) return -1;
    return (int)package;
}


static int physical_package_count(void) {
    DIR *dir = opendir("/sys/devices/system/cpu");
    if (!dir) return 0;
    int packages[256];
    size_t count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "cpu", 3) != 0) continue;
        const char *digits = entry->d_name + 3;
        if (*digits < '0' || *digits > '9') continue;
        char *end = NULL;
        long cpu = strtol(digits, &end, 10);
        if (!end || *end != '\0' || cpu < 0 || cpu > 1048576) continue;
        int package = physical_package_id((int)cpu);
        if (package < 0) continue;
        int seen = 0;
        for (size_t i = 0; i < count; ++i) {
            if (packages[i] == package) { seen = 1; break; }
        }
        if (!seen && count < sizeof(packages) / sizeof(packages[0])) packages[count++] = package;
    }
    closedir(dir);
    return (int)count;
}

static int label_mentions_package(const char *label, int package_id) {
    if (!label || package_id < 0) return 0;
    char needle[64];
    (void)snprintf(needle, sizeof(needle), "package id %d", package_id);
    if (strstr(label, needle)) return 1;
    (void)snprintf(needle, sizeof(needle), "package %d", package_id);
    if (strstr(label, needle)) return 1;
    (void)snprintf(needle, sizeof(needle), "pkg%d", package_id);
    return strstr(label, needle) != NULL;
}

static int temperature_candidate_score(const char *driver, const char *label, int package_id) {
    int score = 0;
    if (driver) {
        if (strstr(driver, "x86_pkg_temp")) score = 120;
        else if (strstr(driver, "coretemp")) score = 110;
        else if (strstr(driver, "k10temp")) score = 110;
        else if (strstr(driver, "zenpower")) score = 105;
        else if (strstr(driver, "cpu")) score = 50;
    }
    if (label) {
        if (label_mentions_package(label, package_id)) score += 150;
        else if (strstr(label, "package")) score += 100;
        else if (strstr(label, "pkg")) score += 90;
        else if (strstr(label, "tctl")) score += 85;
        else if (strstr(label, "tdie")) score += 80;
        else if (strstr(label, "cpu")) score += 50;
        else if (strstr(label, "core")) score += 10;
    }
    return score;
}

typedef struct {
    char path[256];
    int score;
    int explicit_package_match;
    int ambiguous;
} temp_candidate_state_t;

static void consider_temp_candidate(const char *path,
                                    int score,
                                    int explicit_package_match,
                                    temp_candidate_state_t *best) {
    if (!path || !best || access(path, R_OK) != 0) return;
    long long value = 0;
    if (read_long_from_path(path, &value) != 0 || value < TSD_TEMP_MIN_MILLIC || value > TSD_TEMP_MAX_MILLIC) {
        return;
    }

    if (score > best->score ||
        (score == best->score && explicit_package_match > best->explicit_package_match)) {
        if (copy_path(best->path, sizeof(best->path), path) == 0) {
            best->score = score;
            best->explicit_package_match = explicit_package_match;
            best->ambiguous = 0;
        }
        return;
    }

    if (score == best->score && explicit_package_match == best->explicit_package_match &&
        best->path[0] != '\0' && strcmp(path, best->path) != 0) {
        best->ambiguous = 1;
    }
}

static void scan_thermal_zones(int package_id, temp_candidate_state_t *best) {
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) return;
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) != 0) continue;
        char type_path[320];
        char temp_path[320];
        int n1 = snprintf(type_path, sizeof(type_path), "/sys/class/thermal/%s/type", entry->d_name);
        int n2 = snprintf(temp_path, sizeof(temp_path), "/sys/class/thermal/%s/temp", entry->d_name);
        if (n1 < 0 || n2 < 0 || (size_t)n1 >= sizeof(type_path) || (size_t)n2 >= sizeof(temp_path)) continue;
        char type[128];
        if (read_text_from_path(type_path, type, sizeof(type)) != 0) continue;
        lowercase_ascii(type);
        int score = temperature_candidate_score(type, type, package_id);
        if (score >= 50) {
            consider_temp_candidate(temp_path, score, label_mentions_package(type, package_id), best);
        }
    }
    closedir(dir);
}

static int parse_hwmon_temp_input(const char *name, unsigned int *index) {
    if (!name || !index || strncmp(name, "temp", 4) != 0) return 0;
    const char *cursor = name + 4;
    if (*cursor < '0' || *cursor > '9') return 0;
    unsigned long value = 0;
    while (*cursor >= '0' && *cursor <= '9') {
        value = value * 10UL + (unsigned long)(*cursor - '0');
        if (value > 9999UL) return 0;
        ++cursor;
    }
    if (strcmp(cursor, "_input") != 0) return 0;
    *index = (unsigned int)value;
    return 1;
}

static void scan_hwmon(int package_id, temp_candidate_state_t *best) {
    DIR *root = opendir("/sys/class/hwmon");
    if (!root) return;
    struct dirent *hw = NULL;
    while ((hw = readdir(root)) != NULL) {
        if (strncmp(hw->d_name, "hwmon", 5) != 0) continue;
        char base[320];
        int bn = snprintf(base, sizeof(base), "/sys/class/hwmon/%s", hw->d_name);
        if (bn < 0 || (size_t)bn >= sizeof(base)) continue;

        char name_path[384];
        (void)snprintf(name_path, sizeof(name_path), "%s/name", base);
        char driver[128] = {0};
        if (read_text_from_path(name_path, driver, sizeof(driver)) != 0) continue;
        lowercase_ascii(driver);
        int driver_score = temperature_candidate_score(driver, NULL, package_id);
        if (driver_score == 0) continue;

        DIR *dir = opendir(base);
        if (!dir) continue;
        struct dirent *entry = NULL;
        while ((entry = readdir(dir)) != NULL) {
            unsigned int index = 0;
            if (!parse_hwmon_temp_input(entry->d_name, &index)) continue;
            char input_path[384];
            char label_path[384];
            int in = snprintf(input_path, sizeof(input_path), "%s/%s", base, entry->d_name);
            int ln = snprintf(label_path, sizeof(label_path), "%s/temp%u_label", base, index);
            if (in < 0 || ln < 0 || (size_t)in >= sizeof(input_path) || (size_t)ln >= sizeof(label_path)) continue;
            char label[128] = {0};
            if (read_text_from_path(label_path, label, sizeof(label)) == 0) lowercase_ascii(label);
            int score = driver_score + temperature_candidate_score(NULL, label, package_id);
            consider_temp_candidate(input_path, score, label_mentions_package(label, package_id), best);
        }
        closedir(dir);
    }
    closedir(root);
}

static int detect_temp_sensor(tsd_telemetry_helper_t *helper) {
    if (!helper) return 0;
    helper->temp_available = 0;
    helper->temp_path[0] = '\0';

    temp_candidate_state_t best = {{0}, -1, 0, 0};
    int package_id = physical_package_id(helper->cpu);
    scan_thermal_zones(package_id, &best);
    scan_hwmon(package_id, &best);

    if (best.score < 0 || best.path[0] == '\0') return 0;

    const int package_count = physical_package_count();
    if (package_count > 1 && (package_id < 0 || !best.explicit_package_match)) {
        tsd_log_warn(LOG_COMPONENT,
                     "multi-package host requires explicit package-labelled thermal authority; cpu=%d package=%d packages=%d",
                     helper->cpu, package_id, package_count);
        return 0;
    }

    /*
     * On multi-package hosts an unlabeled top-scoring package sensor is not a
     * safe package identity. Equal candidates are common with AMD k10temp/
     * zenpower. Prefer absence of a thermal safety signal over silently using
     * another socket's temperature.
     */
    if (package_id >= 0 && best.ambiguous && !best.explicit_package_match) {
        tsd_log_warn(LOG_COMPONENT,
                     "ambiguous package temperature sensors for cpu=%d package=%d; refusing unsafe selection",
                     helper->cpu, package_id);
        return 0;
    }

    if (copy_path(helper->temp_path, sizeof(helper->temp_path), best.path) != 0) return 0;
    helper->temp_available = 1;
    tsd_log_debug(LOG_COMPONENT, "selected package temperature sensor cpu=%d package=%d score=%d path=%s",
                  helper->cpu, package_id, best.score, helper->temp_path);
    return 1;
}

static int detect_cpufreq_paths(tsd_telemetry_helper_t *helper) {
    if (!helper) return 0;
    char path[256];
    int written = snprintf(path, sizeof(path),
                           "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", helper->cpu);
    if (written < 0 || (size_t)written >= sizeof(path) || access(path, R_OK) != 0) {
        helper->freq_sysfs_available = 0;
        helper->freq_cur_path[0] = '\0';
        helper->freq_max_khz = 0;
        return 0;
    }
    if (copy_path(helper->freq_cur_path, sizeof(helper->freq_cur_path), path) != 0) return 0;

    long long max_khz = 0;
    written = snprintf(path, sizeof(path),
                       "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", helper->cpu);
    if (written < 0 || (size_t)written >= sizeof(path) || read_long_from_path(path, &max_khz) != 0 || max_khz <= 0) {
        written = snprintf(path, sizeof(path),
                           "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", helper->cpu);
        if (written < 0 || (size_t)written >= sizeof(path) || read_long_from_path(path, &max_khz) != 0 || max_khz <= 0) {
            helper->freq_sysfs_available = 0;
            helper->freq_max_khz = 0;
            helper->freq_cur_path[0] = '\0';
            return 0;
        }
    }
    helper->freq_sysfs_available = 1;
    helper->freq_max_khz = (uint64_t)max_khz;
    return 1;
}

static int reopen_msr(tsd_telemetry_helper_t *helper, int emit_events) {
    if (!helper) return -1;
    char msr_path[128];
    int written = snprintf(msr_path, sizeof(msr_path), "/dev/cpu/%d/msr", helper->cpu);
    if (written < 0 || (size_t)written >= sizeof(msr_path)) return -1;
    int fd = open(msr_path, O_RDONLY);
    if (fd >= 0) {
        helper->msr_fd = fd;
        helper->msr_available = 1;
        reset_retry(&helper->msr_retry_deadline, &helper->msr_backoff_seconds);
        if (emit_events) {
            tsd_metrics_increment(TSD_METRIC_TELEMETRY_MSR_RECOVERIES);
            tsd_log_info(LOG_COMPONENT, "event=telemetry_sensor state=recovered sensor=msr path=%s", msr_path);
        }
        return 0;
    }
    helper->msr_fd = -1;
    helper->msr_available = 0;
    schedule_retry(&helper->msr_retry_deadline, &helper->msr_backoff_seconds);
    if (emit_events) tsd_log_debug(LOG_COMPONENT, "event=telemetry_sensor state=pending sensor=msr errno=%d", errno);
    return -1;
}

static int read_msr64(int fd, off_t offset, uint64_t *value) {
    if (fd < 0 || !value) {
        errno = EINVAL;
        return -1;
    }
    uint64_t tmp = 0;
    ssize_t n = pread(fd, &tmp, sizeof(tmp), offset);
    if (n != (ssize_t)sizeof(tmp)) return -1;
    *value = tmp;
    return 0;
}

int tsd_telemetry_helper_init(tsd_telemetry_helper_t *helper, int cpu) {
    if (!helper || cpu < 0) {
        errno = EINVAL;
        return -1;
    }
    memset(helper, 0, sizeof(*helper));
    helper->cpu = cpu;
    helper->msr_fd = -1;
    reset_retry(&helper->temp_retry_deadline, &helper->temp_backoff_seconds);
    reset_retry(&helper->freq_retry_deadline, &helper->freq_backoff_seconds);
    reset_retry(&helper->msr_retry_deadline, &helper->msr_backoff_seconds);
    if (detect_temp_sensor(helper)) reset_retry(&helper->temp_retry_deadline, &helper->temp_backoff_seconds);
    if (detect_cpufreq_paths(helper)) reset_retry(&helper->freq_retry_deadline, &helper->freq_backoff_seconds);
    if (reopen_msr(helper, 0) != 0) {
        tsd_metrics_increment(TSD_METRIC_TELEMETRY_MSR_DROPS);
        tsd_log_warn(LOG_COMPONENT, "event=telemetry_sensor state=degraded sensor=msr errno=%d", errno);
    }
    return 0;
}

void tsd_telemetry_helper_destroy(tsd_telemetry_helper_t *helper) {
    if (!helper) return;
    if (helper->msr_fd >= 0) close(helper->msr_fd);
    helper->msr_fd = -1;
    helper->msr_available = 0;
}

static void sample_temperature(tsd_telemetry_helper_t *helper, tsd_telemetry_sample_t *out) {
    if (!helper || !out) return;
    if (!helper->temp_available || helper->temp_path[0] == '\0') {
        time_t now = tsd_now_seconds();
        if (helper->temp_retry_deadline == 0 || now >= helper->temp_retry_deadline) {
            if (detect_temp_sensor(helper)) {
                reset_retry(&helper->temp_retry_deadline, &helper->temp_backoff_seconds);
                tsd_metrics_increment(TSD_METRIC_TELEMETRY_TEMP_RECOVERIES);
                tsd_log_info(LOG_COMPONENT, "event=telemetry_sensor state=recovered sensor=temp path=%s", helper->temp_path);
            } else {
                schedule_retry(&helper->temp_retry_deadline, &helper->temp_backoff_seconds);
            }
        }
        if (!helper->temp_available) return;
    }

    long long temp_value = 0;
    char path_copy[256];
    (void)copy_path(path_copy, sizeof(path_copy), helper->temp_path);
    if (read_long_from_path(helper->temp_path, &temp_value) == 0 &&
        temp_value >= TSD_TEMP_MIN_MILLIC && temp_value <= TSD_TEMP_MAX_MILLIC) {
        out->temp_available = 1;
        out->package_temp_millic = (int32_t)temp_value;
        return;
    }

    helper->temp_available = 0;
    tsd_metrics_increment(TSD_METRIC_TELEMETRY_TEMP_DROPS);
    tsd_log_warn(LOG_COMPONENT, "event=telemetry_sensor state=degraded sensor=temp path=%s errno=%d", path_copy, errno);
    helper->temp_path[0] = '\0';
    schedule_retry(&helper->temp_retry_deadline, &helper->temp_backoff_seconds);
}

static void sample_msr_ratio(tsd_telemetry_helper_t *helper, tsd_telemetry_sample_t *out) {
    if (!helper || !out) return;
    if (!helper->msr_available || helper->msr_fd < 0) {
        time_t now = tsd_now_seconds();
        if (helper->msr_retry_deadline == 0 || now >= helper->msr_retry_deadline) reopen_msr(helper, 1);
        if (!helper->msr_available || helper->msr_fd < 0) return;
    }
    uint64_t aperf = 0;
    uint64_t mperf = 0;
    if (read_msr64(helper->msr_fd, MSR_IA32_APERF, &aperf) != 0 ||
        read_msr64(helper->msr_fd, MSR_IA32_MPERF, &mperf) != 0) {
        close(helper->msr_fd);
        helper->msr_fd = -1;
        helper->msr_available = 0;
        tsd_metrics_increment(TSD_METRIC_TELEMETRY_MSR_DROPS);
        tsd_log_warn(LOG_COMPONENT, "event=telemetry_sensor state=degraded sensor=msr cpu=%d errno=%d", helper->cpu, errno);
        schedule_retry(&helper->msr_retry_deadline, &helper->msr_backoff_seconds);
        return;
    }
    if (helper->last_mperf != 0 && mperf > helper->last_mperf && aperf >= helper->last_aperf) {
        uint64_t delta_aperf = aperf - helper->last_aperf;
        uint64_t delta_mperf = mperf - helper->last_mperf;
        if (delta_mperf > 0) {
            __uint128_t num = (__uint128_t)delta_aperf * 1000u;
            uint64_t ratio = (uint64_t)(num / delta_mperf);
            out->freq_ratio_milli = ratio > UINT32_MAX ? UINT32_MAX : (uint32_t)ratio;
            out->freq_ratio_available = 1;
        }
    }
    helper->last_aperf = aperf;
    helper->last_mperf = mperf;
}

static void sample_cpufreq_ratio(tsd_telemetry_helper_t *helper, tsd_telemetry_sample_t *out) {
    if (!helper || !out) return;
    if (!helper->freq_sysfs_available || helper->freq_cur_path[0] == '\0' || helper->freq_max_khz == 0) {
        time_t now = tsd_now_seconds();
        if (helper->freq_retry_deadline == 0 || now >= helper->freq_retry_deadline) {
            if (detect_cpufreq_paths(helper)) {
                reset_retry(&helper->freq_retry_deadline, &helper->freq_backoff_seconds);
                tsd_metrics_increment(TSD_METRIC_TELEMETRY_FREQ_RECOVERIES);
                tsd_log_info(LOG_COMPONENT, "event=telemetry_sensor state=recovered sensor=cpufreq path=%s", helper->freq_cur_path);
            } else {
                schedule_retry(&helper->freq_retry_deadline, &helper->freq_backoff_seconds);
            }
        }
        if (!helper->freq_sysfs_available) return;
    }
    long long cur_khz = 0;
    if (read_long_from_path(helper->freq_cur_path, &cur_khz) != 0 || cur_khz <= 0) {
        helper->freq_sysfs_available = 0;
        tsd_metrics_increment(TSD_METRIC_TELEMETRY_FREQ_DROPS);
        tsd_log_warn(LOG_COMPONENT, "event=telemetry_sensor state=degraded sensor=cpufreq path=%s errno=%d",
                     helper->freq_cur_path, errno);
        helper->freq_cur_path[0] = '\0';
        schedule_retry(&helper->freq_retry_deadline, &helper->freq_backoff_seconds);
        return;
    }
    __uint128_t num = (__uint128_t)cur_khz * 1000u;
    uint64_t ratio = (uint64_t)(num / helper->freq_max_khz);
    out->freq_ratio_milli = ratio > UINT32_MAX ? UINT32_MAX : (uint32_t)ratio;
    out->freq_ratio_available = 1;
}

int tsd_telemetry_helper_sample(tsd_telemetry_helper_t *helper, tsd_telemetry_sample_t *out) {
    if (!helper || !out) {
        errno = EINVAL;
        return -1;
    }
    memset(out, 0, sizeof(*out));
    sample_temperature(helper, out);
    sample_msr_ratio(helper, out);
    if (!out->freq_ratio_available) sample_cpufreq_ratio(helper, out);
    return (out->temp_available || out->freq_ratio_available) ? 0 : -1;
}
