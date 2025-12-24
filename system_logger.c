#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <errno.h>
#include <signal.h>
#include <sys/syslog.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <sys/statfs.h>

#define LOG_INTERVAL 5
#define MAX_LINE_LEN 256
#define CONFIG_FILE "/var/lib/system_logger/config.conf"
#define LOG_FILE "/var/log/system_logger.log"
#define MAX_CONFIG_LINE 512
#define MAX_PATH_LEN 512

static FILE *log_file = NULL;
static int log_interval = LOG_INTERVAL;
static int inotify_fd = -1;
static int use_syslog = 1;

typedef struct {
    char path[MAX_PATH_LEN];
    int wd;
    time_t last_check;
} watch_dir_t;

static watch_dir_t watch_dirs[] = {
    {"/etc", -1, 0},
    {"/var/log", -1, 0},
    {"/tmp", -1, 0}
};
#define NUM_WATCH_DIRS (sizeof(watch_dirs) / sizeof(watch_dirs[0]))

static const char* get_username(void) {
    const char *user = getenv("USER");
    return user ? user : (getenv("USERNAME") ? getenv("USERNAME") : "unknown");
}

int read_config(void) {
    FILE *config = fopen(CONFIG_FILE, "r");
    if (!config) return 0;
    
    char line[MAX_CONFIG_LINE];
    while (fgets(line, sizeof(line), config)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';
        if (len == 0 || line[0] == '#') continue;
        
        if (strncmp(line, "LOG_INTERVAL=", 13) == 0) {
            int interval = atoi(line + 13);
            if (interval > 0 && interval <= 3600) log_interval = interval;
        } else if (strncmp(line, "USE_SYSLOG=", 11) == 0) {
            use_syslog = (atoi(line + 11) != 0);
        }
    }
    fclose(config);
    return 0;
}

int open_log_file(void) {
    log_file = fopen(LOG_FILE, "a");
    if (!log_file) {
        fprintf(stderr, "Error opening file %s: %s\n", LOG_FILE, strerror(errno));
        return -1;
    }
    setvbuf(log_file, NULL, _IOLBF, 0);
    return 0;
}

void close_log_file(void) {
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
}

void log_message(const char *username, const char *message, int priority) {
    char time_str[64];
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
    
    if (log_file) {
        const char *level_str = "INFO";
        if (priority == LOG_WARNING) level_str = "WARNING";
        else if (priority == LOG_ERR) level_str = "ERROR";
        else if (priority == LOG_DEBUG) level_str = "DEBUG";
        fprintf(log_file, "[%s] [%s] [%s] %s\n", time_str, level_str, username, message);
        fflush(log_file);
    }
    
    if (use_syslog) {
        syslog(priority, "[%s] %s", username, message);
    }
}

void log_uptime(const char *username) {
    FILE *file = fopen("/proc/uptime", "r");
    if (!file) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Error reading /proc/uptime: %s", strerror(errno));
        log_message(username, msg, LOG_WARNING);
        return;
    }
    
    double uptime_seconds;
    if (fscanf(file, "%lf", &uptime_seconds) == 1) {
        int days = (int)(uptime_seconds / 86400);
        int hours = (int)((uptime_seconds - days * 86400) / 3600);
        int minutes = (int)((uptime_seconds - days * 86400 - hours * 3600) / 60);
        char msg[256];
        snprintf(msg, sizeof(msg), "Uptime: %d days, %d hours, %d minutes (%.0f seconds)", 
                days, hours, minutes, uptime_seconds);
        log_message(username, msg, LOG_INFO);
    }
    fclose(file);
}

void log_free_inodes(const char *username) {
    struct statfs fs_info;
    if (statfs("/", &fs_info) == 0) {
        unsigned long long free_inodes = fs_info.f_bavail;
        unsigned long long total_inodes = fs_info.f_files;
        char msg[256];
        snprintf(msg, sizeof(msg), "Free inodes: %llu out of %llu", 
                free_inodes, total_inodes);
        log_message(username, msg, LOG_INFO);
    } else {
        char msg[256];
        snprintf(msg, sizeof(msg), "Error getting inode information: %s", strerror(errno));
        log_message(username, msg, LOG_WARNING);
    }
}

void log_network_connections(const char *username) {
    FILE *file = fopen("/proc/net/tcp", "r");
    if (!file) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Error opening /proc/net/tcp: %s", strerror(errno));
        log_message(username, msg, LOG_WARNING);
        return;
    }
    
    char line[MAX_LINE_LEN];
    int connection_count = 0, established_count = 0;
    
    if (fgets(line, sizeof(line), file)) {
        while (fgets(line, sizeof(line), file)) {
            connection_count++;
            if (strstr(line, ": 01 ") != NULL) established_count++;
        }
    }
    fclose(file);
    
    char msg[256];
    snprintf(msg, sizeof(msg), "TCP network connections: total %d, established %d", 
            connection_count, established_count);
    log_message(username, msg, LOG_INFO);
}

int init_directory_monitoring(void) {
    inotify_fd = inotify_init();
    if (inotify_fd < 0) return -1;
    
    for (size_t i = 0; i < NUM_WATCH_DIRS; i++) {
        int wd = inotify_add_watch(inotify_fd, watch_dirs[i].path, 
                                   IN_MODIFY | IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO);
        if (wd >= 0) {
            watch_dirs[i].wd = wd;
            watch_dirs[i].last_check = time(NULL);
        }
    }
    return 0;
}

void check_directory_changes(const char *username) {
    if (inotify_fd < 0) return;
    
    fd_set read_fds;
    struct timeval timeout = {0, 0};
    FD_ZERO(&read_fds);
    FD_SET(inotify_fd, &read_fds);
    
    if (select(inotify_fd + 1, &read_fds, NULL, NULL, &timeout) > 0) {
        char buffer[4096];
        ssize_t length = read(inotify_fd, buffer, sizeof(buffer));
        
        if (length > 0) {
            int i = 0;
            while (i < length) {
                struct inotify_event *event = (struct inotify_event *)&buffer[i];
                
                for (size_t j = 0; j < NUM_WATCH_DIRS; j++) {
                    if (watch_dirs[j].wd == event->wd) {
                        if (event->len > 0 && strcmp(event->name, "system_logger.log") == 0) break;
                        
                        const char *event_type = "modification";
                        if (event->mask & IN_CREATE) event_type = "creation";
                        else if (event->mask & IN_DELETE) event_type = "deletion";
                        else if (event->mask & IN_MODIFY) event_type = "modification";
                        else if (event->mask & IN_MOVED_FROM) event_type = "moved from";
                        else if (event->mask & IN_MOVED_TO) event_type = "moved to";
                        
                        char msg[1024];
                        if (event->len > 0) {
                            snprintf(msg, sizeof(msg), "%s: %s of file %s", 
                                    watch_dirs[j].path, event_type, event->name);
                        } else {
                            snprintf(msg, sizeof(msg), "%s: %s", 
                                    watch_dirs[j].path, event_type);
                        }
                        log_message(username, msg, LOG_INFO);
                        break;
                    }
                }
                i += sizeof(struct inotify_event) + event->len;
            }
        }
    }
}

void check_directory_changes_periodic(const char *username) {
    static time_t last_check = 0;
    time_t now = time(NULL);
    if (now - last_check < 30) return;
    last_check = now;
    
    for (size_t i = 0; i < NUM_WATCH_DIRS; i++) {
        struct stat st;
        if (stat(watch_dirs[i].path, &st) == 0) {
            if (strcmp(watch_dirs[i].path, "/var/log") == 0) {
                struct stat log_st;
                char log_path[MAX_PATH_LEN + 32];
                snprintf(log_path, sizeof(log_path), "%s/system_logger.log", watch_dirs[i].path);
                if (stat(log_path, &log_st) == 0 && st.st_mtime == log_st.st_mtime && watch_dirs[i].last_check > 0) {
                    watch_dirs[i].last_check = st.st_mtime;
                    continue;
                }
            }
            
            if (watch_dirs[i].last_check > 0 && st.st_mtime > watch_dirs[i].last_check) {
                char msg[1024];
                snprintf(msg, sizeof(msg), "Changes detected in directory: %s", watch_dirs[i].path);
                log_message(username, msg, LOG_INFO);
            }
            watch_dirs[i].last_check = st.st_mtime;
        }
    }
}

void signal_handler(int sig) {
    const char *username = get_username();
    if (sig == SIGTERM || sig == SIGINT) {
        log_message(username, "Termination signal received. Program is stopping.", LOG_INFO);
        if (inotify_fd >= 0) close(inotify_fd);
        close_log_file();
        if (use_syslog) closelog();
        exit(0);
    }
}

int main(int argc __attribute__((unused)), char *argv[] __attribute__((unused))) {
    const char *username = get_username();
    char message[512];
    
    signal(SIGTERM, signal_handler);
    signal(SIGINT, signal_handler);
    
    if (use_syslog) openlog("system_logger", LOG_PID | LOG_CONS, LOG_DAEMON);
    read_config();
    
    if (open_log_file() != 0) {
        fprintf(stderr, "Failed to open log file. Program is terminating.\n");
        return 1;
    }
    
    if (init_directory_monitoring() < 0) {
        snprintf(message, sizeof(message), "Failed to initialize directory monitoring: %s", strerror(errno));
        log_message(username, message, LOG_WARNING);
    }
    
    log_message(username, "------------------------------", LOG_INFO);
    log_message(username, "Logging program started", LOG_INFO);
    
    if (geteuid() == 0) {
        log_message(username, "Program is running with root privileges", LOG_INFO);
    } else {
        snprintf(message, sizeof(message), "Program is running as user (UID: %d)", getuid());
        log_message(username, message, LOG_INFO);
    }
    
    snprintf(message, sizeof(message), "Logging interval: %d seconds", log_interval);
    log_message(username, message, LOG_INFO);
    
    while (1) {
        log_uptime(username);
        log_network_connections(username);
        log_free_inodes(username);
        check_directory_changes(username);
        check_directory_changes_periodic(username);
        sleep(log_interval);
    }
    
    close_log_file();
    if (use_syslog) closelog();
    return 0;
}
