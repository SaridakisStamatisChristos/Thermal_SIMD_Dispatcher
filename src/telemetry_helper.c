#include <thermal/simd/telemetry_helper.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <dirent.h>
#include <time.h>

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

static time_t tsd_now_seconds(void) {
    time_t now = time(NULL);
    if (now < 0) {
        return 0;
    }
    return now;
}

static void schedule_retry(time_t *deadline, int *backoff) {
    if (!deadline || !backoff) {
        return;
    }
    if (*backoff <= 0) {
        *backoff = INITIAL_BACKOFF_SEC;
    }
    int next = *backoff;
    if (next > MAX_BACKOFF_SEC) {
        next = MAX_BACKOFF_SEC;
    }
    time_t now = tsd_now_seconds();
    *deadline = now + (time_t)next;
    if (*backoff < MAX_BACKOFF_SEC) {
        int new_backoff = *backoff * 2;
        if (new_backoff > MAX_BACKOFF_SEC) {
            new_backoff = MAX_BACKOFF_SEC;
        }
        *backoff = new_backoff;
    }
}

static void reset_retry(time_t *deadline, int *backoff) {
    if (deadline) {
        *deadline = 0;
    }
    if (backoff) {
        *backoff = INITIAL_BACKOFF_SEC;
    }
}

static int reopen_msr(tsd_telemetry_helper_t *helper, int emit_events) {
    if (!helper) {
        return -1;
    }
    char msr_path[128];
    int written = snprintf(msr_path, sizeof(msr_path), "/dev/cpu/%d/msr", helper->cpu);
    if (written < 0 || (size_t)written >= sizeof(msr_path)) {
        return -1;
    }
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
    if (emit_events) {
        tsd_log_debug(LOG_COMPONENT, "event=telemetry_sensor state=pending sensor=msr errno=%d", errno);
    }
    return -1;
}

static int read_long_from_path(const char *path, long long *value) {
    if (!path || !value) {
        errno = EINVAL;
        return -1;
    }
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    char buffer[64];
    if (!fgets(buffer, sizeof(buffer), fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    char *endptr = NULL;
    errno = 0;
    long long parsed = strtoll(buffer, &endptr, 10);
    if (errno != 0 || endptr == buffer) {
        return -1;
    }
    *value = parsed;
    return 0;
}

static int detect_temp_sensor(tsd_telemetry_helper_t *helper) {
    if (!helper) {
        return 0;
    }
    helper->temp_available = 0;
    helper->temp_path[0] = '\0';
    DIR *dir = opendir("/sys/class/thermal");
    if (!dir) {
        return 0;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "thermal_zone", 12) != 0) {
            continue;
        }
        char type_path[256];
        int written = snprintf(type_path, sizeof(type_path), "/sys/class/thermal/%s/type", entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(type_path)) {
            continue;
        }
        FILE *type_fp = fopen(type_path, "r");
        if (!type_fp) {
            continue;
        }
        char type_buf[128];
        if (!fgets(type_buf, sizeof(type_buf), type_fp)) {
            fclose(type_fp);
            continue;
        }
        fclose(type_fp);
        for (char *p = type_buf; *p; ++p) {
            if (*p >= 'A' && *p <= 'Z') {
                *p = (char)(*p - 'A' + 'a');
            }
        }
        if (strstr(type_buf, "pkg") == NULL && strstr(type_buf, "package") == NULL &&
            strstr(type_buf, "cpu") == NULL) {
            continue;
        }
        written = snprintf(helper->temp_path, sizeof(helper->temp_path),
                           "/sys/class/thermal/%s/temp", entry->d_name);
        if (written < 0 || (size_t)written >= sizeof(helper->temp_path)) {
            helper->temp_path[0] = '\0';
            continue;
        }
        if (access(helper->temp_path, R_OK) == 0) {
            helper->temp_available = 1;
            closedir(dir);
            return 1;
        }
    }
    closedir(dir);
    return 0;
}

static int detect_cpufreq_paths(tsd_telemetry_helper_t *helper) {
    if (!helper) {
        return 0;
    }
    char path[256];
    int written = snprintf(path, sizeof(path),
                           "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", helper->cpu);
    if (written < 0 || (size_t)written >= sizeof(path) || access(path, R_OK) != 0) {
        helper->freq_sysfs_available = 0;
        helper->freq_cur_path[0] = '\0';
        helper->freq_max_khz = 0;
        return 0;
    }
    written = snprintf(helper->freq_cur_path, sizeof(helper->freq_cur_path), "%s", path);
    if (written < 0 || (size_t)written >= sizeof(helper->freq_cur_path)) {
        helper->freq_sysfs_available = 0;
        helper->freq_cur_path[0] = '\0';
        helper->freq_max_khz = 0;
        return 0;
    }
    long long max_khz = 0;
    written = snprintf(path, sizeof(path),
                       "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq", helper->cpu);
    if (read_long_from_path(path, &max_khz) != 0 || max_khz <= 0) {
        written = snprintf(path, sizeof(path),
                           "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", helper->cpu);
        if (written < 0 || (size_t)written >= sizeof(path) ||
            read_long_from_path(path, &max_khz) != 0 || max_khz <= 0) {
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

static int read_msr64(int fd, off_t offset, uint64_t *value) {
    if (fd < 0 || !value) {
        errno = EINVAL;
        return -1;
    }
    uint64_t tmp = 0;
    ssize_t n = pread(fd, &tmp, sizeof(tmp), offset);
    if (n != (ssize_t)sizeof(tmp)) {
        return -1;
    }
    *value = tmp;
    return 0;
}

int tsd_telemetry_helper_init(tsd_telemetry_helper_t *helper, int cpu) {
    if (!helper) {
        errno = EINVAL;
        return -1;
    }
    memset(helper, 0, sizeof(*helper));
    helper->cpu = cpu;
    helper->msr_fd = -1;
    reset_retry(&helper->temp_retry_deadline, &helper->temp_backoff_seconds);
    reset_retry(&helper->freq_retry_deadline, &helper->freq_backoff_seconds);
    reset_retry(&helper->msr_retry_deadline, &helper->msr_backoff_seconds);
    detect_temp_sensor(helper);
    if (helper->temp_available) {
        reset_retry(&helper->temp_retry_deadline, &helper->temp_backoff_seconds);
    }
    if (detect_cpufreq_paths(helper)) {
        reset_retry(&helper->freq_retry_deadline, &helper->freq_backoff_seconds);
    }

    if (reopen_msr(helper, 0) != 0) {
        tsd_metrics_increment(TSD_METRIC_TELEMETRY_MSR_DROPS);
        tsd_log_warn(LOG_COMPONENT, "event=telemetry_sensor state=degraded sensor=msr errno=%d", errno);
    }
    return 0;
}

void tsd_telemetry_helper_destroy(tsd_telemetry_helper_t *helper) {
    if (!helper) {
        return;
    }
    if (helper->msr_fd >= 0) {
        close(helper->msr_fd);
        helper->msr_fd = -1;
    }
}

static void sample_temperature(tsd_telemetry_helper_t *helper, tsd_telemetry_sample_t *out) {
    if (!helper || !out) {
        return;
    }
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
        if (!helper->temp_available) {
            return;
        }
    }
    long long temp_value = 0;
    char path_copy[256];
    strncpy(path_copy, helper->temp_path, sizeof(path_copy));
    path_copy[sizeof(path_copy) - 1] = '\0';
    if (read_long_from_path(helper->temp_path, &temp_value) == 0) {
        out->temp_available = 1;
        out->package_temp_millic = (int32_t)temp_value;
    } else {
        helper->temp_available = 0;
        tsd_metrics_increment(TSD_METRIC_TELEMETRY_TEMP_DROPS);
        tsd_log_warn(LOG_COMPONENT, "event=telemetry_sensor state=degraded sensor=temp path=%s errno=%d", path_copy, errno);
        helper->temp_path[0] = '\0';
        schedule_retry(&helper->temp_retry_deadline, &helper->temp_backoff_seconds);
    }
}

static void sample_msr_ratio(tsd_telemetry_helper_t *helper, tsd_telemetry_sample_t *out) {
    if (!helper || !out) {
        return;
    }
    if (!helper->msr_available || helper->msr_fd < 0) {
        time_t now = tsd_now_seconds();
        if (helper->msr_retry_deadline == 0 || now >= helper->msr_retry_deadline) {
            reopen_msr(helper, 1);
        }
        if (!helper->msr_available || helper->msr_fd < 0) {
            return;
        }
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
            out->freq_ratio_milli = (uint32_t)(num / delta_mperf);
            out->freq_ratio_available = 1;
        }
    }
    helper->last_aperf = aperf;
    helper->last_mperf = mperf;
}

static void sample_cpufreq_ratio(tsd_telemetry_helper_t *helper, tsd_telemetry_sample_t *out) {
    if (!helper || !out) {
        return;
    }
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
        if (!helper->freq_sysfs_available) {
            return;
        }
    }
    long long cur_khz = 0;
    if (read_long_from_path(helper->freq_cur_path, &cur_khz) != 0 || cur_khz <= 0) {
        helper->freq_sysfs_available = 0;
        tsd_metrics_increment(TSD_METRIC_TELEMETRY_FREQ_DROPS);
        tsd_log_warn(LOG_COMPONENT, "event=telemetry_sensor state=degraded sensor=cpufreq path=%s errno=%d", helper->freq_cur_path, errno);
        helper->freq_cur_path[0] = '\0';
        schedule_retry(&helper->freq_retry_deadline, &helper->freq_backoff_seconds);
        return;
    }
    __uint128_t num = (__uint128_t)cur_khz * 1000u;
    out->freq_ratio_milli = (uint32_t)(num / helper->freq_max_khz);
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
    if (!out->freq_ratio_available) {
        sample_cpufreq_ratio(helper, out);
    }
    return 0;
}
