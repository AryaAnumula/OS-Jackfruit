/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define MONITOR_DEVICE "/dev/container_monitor"

/* ================================================================== */
/* Type Definitions                                                    */
/* ================================================================== */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED,
    CONTAINER_HARD_LIMIT_KILLED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    int pipe_read_fd;
    pthread_t producer_tid;
    int producer_running;
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int pipe_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct {
    int  read_fd;
    char id[CONTAINER_ID_LEN];
    bounded_buffer_t *buf;
} producer_arg_t;

/* ================================================================== */
/* Function Prototypes                                                 */
/* ================================================================== */

static void usage(const char *prog);
static int run_supervisor(const char *rootfs);
static int cmd_start(int argc, char *argv[]);
static int cmd_run(int argc, char *argv[]);
static int cmd_ps(void);
static int cmd_logs(int argc, char *argv[]);
static int cmd_stop(int argc, char *argv[]);

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid, unsigned long soft, unsigned long hard);
int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid);
static void reap_zombies(supervisor_ctx_t *ctx);

static supervisor_ctx_t *g_ctx = NULL;
static volatile sig_atomic_t g_reap_needed = 0;

/* ================================================================== */
/* Reaper Logic (Task 1)                                               */
/* ================================================================== */

static void reap_zombies(supervisor_ctx_t *ctx) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = ctx->containers;
        while (c) {
            if (c->host_pid == pid) {
                if (WIFEXITED(status)) {
                    c->exit_code = WEXITSTATUS(status);
                    c->state = CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    c->exit_signal = WTERMSIG(status);
                    if (c->stop_requested) {
                        c->state = CONTAINER_STOPPED;
                    } else if (WTERMSIG(status) == SIGKILL) {
                        c->state = CONTAINER_HARD_LIMIT_KILLED;
                    } else {
                        c->state = CONTAINER_KILLED;
                    }
                }
                if (ctx->monitor_fd >= 0) unregister_from_monitor(ctx->monitor_fd, c->id, pid);
                break;
            }
            c = c->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }
    g_reap_needed = 0;
}

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static void usage(const char *prog) {
    fprintf(stderr, "Usage:\n  %s supervisor <base-rootfs>\n  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n  %s ps\n  %s logs <id>\n  %s stop <id>\n", prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag __attribute__((unused)), const char *value, unsigned long *target_bytes) {
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') return -1;
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc, char *argv[], int start_index) {
    for (int i = start_index; i < argc; i += 2) {
        if (i + 1 >= argc) return -1;
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1], &req->soft_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1], &req->hard_limit_bytes) != 0) return -1;
        } else if (strcmp(argv[i], "--nice") == 0) {
            req->nice_value = atoi(argv[i+1]);
        }
    }
    return 0;
}

static const char *state_to_string(container_state_t state) {
    switch (state) {
        case CONTAINER_STARTING: return "starting";
        case CONTAINER_RUNNING:  return "running";
        case CONTAINER_STOPPED:  return "stopped";
        case CONTAINER_KILLED:   return "killed";
        case CONTAINER_EXITED:  return "exited";
        case CONTAINER_HARD_LIMIT_KILLED:   return "hard_limit_killed";
        default: return "unknown";
    }
}

/* ================================================================== */
/* Bounded buffer logic                                               */
/* ================================================================== */

static int bounded_buffer_init(bounded_buffer_t *buffer) {
    memset(buffer, 0, sizeof(*buffer));
    pthread_mutex_init(&buffer->mutex, NULL);
    pthread_cond_init(&buffer->not_empty, NULL);
    pthread_cond_init(&buffer->not_full, NULL);
    return 0;
}

__attribute__((unused)) static void bounded_buffer_destroy(bounded_buffer_t *buffer) {
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer) {
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    if (buffer->shutting_down) { pthread_mutex_unlock(&buffer->mutex); return -1; }
    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;
    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item) {
    pthread_mutex_lock(&buffer->mutex);
    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    if (buffer->count == 0 && buffer->shutting_down) { pthread_mutex_unlock(&buffer->mutex); return 1; }
    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;
    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ================================================================== */
/* Logging Threads                                                   */
/* ================================================================== */

void *logging_thread(void *arg) {
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char path[PATH_MAX];
    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        FILE *f = fopen(path, "a");
        if (f) { 
            if (fwrite(item.data, 1, item.length, f) != item.length) {
                /* Log write error handle if needed */
            }
            fclose(f); 
        }
    }
    return NULL;
}

static void *producer_thread(void *arg) {
    producer_arg_t *pa = (producer_arg_t *)arg;
    log_item_t item;
    ssize_t n;
    memset(&item, 0, sizeof(item));
    snprintf(item.container_id, sizeof(item.container_id), "%s", pa->id);
    while ((n = read(pa->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(pa->buf, &item);
        memset(item.data, 0, LOG_CHUNK_SIZE);
    }
    close(pa->read_fd);
    free(pa);
    return NULL;
}

/* ================================================================== */
/* Container Entrypoint (Task 1)                                       */
/* ================================================================== */

int child_fn(void *arg) {
    child_config_t *cfg = (child_config_t *)arg;
    if (dup2(cfg->pipe_write_fd, STDOUT_FILENO) < 0) perror("dup2 stdout");
    if (dup2(cfg->pipe_write_fd, STDERR_FILENO) < 0) perror("dup2 stderr");
    close(cfg->pipe_write_fd);

    if (sethostname(cfg->id, strlen(cfg->id)) < 0) perror("sethostname");
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0) perror("mount private");

    if (chroot(cfg->rootfs) < 0) { perror("chroot"); return 1; }
    if (chdir("/") < 0) { perror("chdir"); return 1; }
    
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        fprintf(stderr, "warning: mount /proc failed: %s\n", strerror(errno));

    if (cfg->nice_value != 0) {
        if (nice(cfg->nice_value) == -1 && errno != 0) perror("nice");
    }

    char *argv_exec[] = { "/bin/sh", "-c", cfg->command, NULL };
    execv("/bin/sh", argv_exec);
    return 1;
}

/* ================================================================== */
/* ioctl helpers                                                       */
/* ================================================================== */

int register_with_monitor(int monitor_fd, const char *container_id, pid_t host_pid, unsigned long soft, unsigned long hard) {
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft;
    req.hard_limit_bytes = hard;
    snprintf(req.container_id, sizeof(req.container_id), "%s", container_id);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req);
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid) {
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    snprintf(req.container_id, sizeof(req.container_id), "%s", container_id);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req);
}

/* ================================================================== */
/* Signal Handlers                                                   */
/* ================================================================== */

static void handle_sigchld(int sig) { (void)sig; g_reap_needed = 1; }
static void handle_sigterm(int sig) { (void)sig; if (g_ctx) g_ctx->should_stop = 1; }

/* ================================================================== */
/* do_start_container                                                  */
/* ================================================================== */

static int do_start_container(supervisor_ctx_t *ctx, const control_request_t *req) {
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *existing = ctx->containers;
    while (existing) {
        if (strcmp(existing->id, req->container_id) == 0 && existing->state == CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock); return -1;
        }
        existing = existing->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return -1; }

    child_config_t *cfg = malloc(sizeof(child_config_t));
    snprintf(cfg->id, sizeof(cfg->id), "%s", req->container_id);
    snprintf(cfg->rootfs, sizeof(cfg->rootfs), "%s", req->rootfs);
    snprintf(cfg->command, sizeof(cfg->command), "%s", req->command);
    cfg->nice_value = req->nice_value;
    cfg->pipe_write_fd = pipefd[1];

    char *stack = malloc(STACK_SIZE);
    pid_t pid = clone(child_fn, stack + STACK_SIZE, CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD, cfg);
    close(pipefd[1]);

    container_record_t *rec = calloc(1, sizeof(container_record_t));
    snprintf(rec->id, sizeof(rec->id), "%s", req->container_id);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->pipe_read_fd = pipefd[0];
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    if (ctx->monitor_fd >= 0) register_with_monitor(ctx->monitor_fd, rec->id, pid, rec->soft_limit_bytes, rec->hard_limit_bytes);

    producer_arg_t *pa = malloc(sizeof(producer_arg_t));
    snprintf(pa->id, sizeof(pa->id), "%s", req->container_id);
    pa->read_fd = pipefd[0];
    pa->buf = &ctx->log_buffer;
    pthread_create(&rec->producer_tid, NULL, producer_thread, pa);
    rec->producer_running = 1;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);
    free(stack);
    return 0;
}

/* ================================================================== */
/* Command Dispatcher                                                */
/* ================================================================== */

static void dispatch(supervisor_ctx_t *ctx, const control_request_t *req, int client_fd) {
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    switch (req->kind) {
        case CMD_START:
        case CMD_RUN: {
            if (do_start_container(ctx, req) < 0) {
                resp.status = -1;
                snprintf(resp.message, sizeof(resp.message), "ERROR: failed to start '%s'\n", req->container_id);
            } else {
                resp.status = 0;
                snprintf(resp.message, sizeof(resp.message), "OK: Container started\n");
            }
            if (write(client_fd, &resp, sizeof(resp)) < 0) perror("write");
            break;
        }
        case CMD_PS: {
            char buf[4096]; int off = 0;
            off += snprintf(buf+off, sizeof(buf)-off, "%-16s %-8s %-20s %-10s %-10s %-10s\n", "ID","PID","STARTED","SOFT(MiB)","HARD(MiB)","STATE");
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *c = ctx->containers;
            while (c) {
                char tbuf[32]; struct tm *t = localtime(&c->started_at);
                strftime(tbuf, 32, "%Y-%m-%d %H:%M:%S", t);
                off += snprintf(buf+off, sizeof(buf)-off, "%-16s %-8d %-20s %-10lu %-10lu %-10s\n", c->id, c->host_pid, tbuf, c->soft_limit_bytes >> 20, c->hard_limit_bytes >> 20, state_to_string(c->state));
                c = c->next;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp.status = 0;
            snprintf(resp.message, sizeof(resp.message), "%.255s", buf);
            if (write(client_fd, &resp, sizeof(resp)) < 0) perror("write");
            break;
        }
        case CMD_LOGS: {
            char path[PATH_MAX] = {0};
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *c = ctx->containers;
            while (c) { if (strcmp(c->id, req->container_id) == 0) { snprintf(path, sizeof(path), "%s", c->log_path); break; } c = c->next; }
            pthread_mutex_unlock(&ctx->metadata_lock);
            if (path[0] == '\0') { resp.status = -1; snprintf(resp.message, sizeof(resp.message), "ERROR: not found\n"); }
            else { resp.status = 0; snprintf(resp.message, sizeof(resp.message), "LOG:\n"); }
            if (write(client_fd, &resp, sizeof(resp)) < 0) perror("write");
            if (path[0] != '\0') {
                FILE *f = fopen(path, "r");
                if (f) { char line[512]; while (fgets(line, 512, f)) if (write(client_fd, line, strlen(line)) < 0) break; fclose(f); }
            }
            break;
        }
        case CMD_STOP: {
            pid_t target = -1;
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *c = ctx->containers;
            while (c) { if (strcmp(c->id, req->container_id) == 0 && c->state == CONTAINER_RUNNING) { c->stop_requested = 1; target = c->host_pid; break; } c = c->next; }
            pthread_mutex_unlock(&ctx->metadata_lock);
            if (target > 0) { kill(target, SIGTERM); resp.status = 0; snprintf(resp.message, sizeof(resp.message), "OK\n"); }
            else { resp.status = -1; snprintf(resp.message, sizeof(resp.message), "ERROR: not running\n"); }
            if (write(client_fd, &resp, sizeof(resp)) < 0) perror("write");
            break;
        }
        default: break;
    }
}

/* ================================================================== */
/* Main Loop                                                         */
/* ================================================================== */

static int run_supervisor(const char *rootfs) {
    supervisor_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    g_ctx = &ctx;
    pthread_mutex_init(&ctx.metadata_lock, NULL);
    bounded_buffer_init(&ctx.log_buffer);
    mkdir(LOG_DIR, 0755);
    ctx.monitor_fd = open(MONITOR_DEVICE, O_RDWR);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX; snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);
    unlink(CONTROL_PATH); bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(ctx.server_fd, 8); fcntl(ctx.server_fd, F_SETFL, O_NONBLOCK);
    struct sigaction sa; memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigchld; sa.sa_flags = SA_RESTART | SA_NOCLDSTOP; sigaction(SIGCHLD, &sa, NULL);
    sa.sa_handler = handle_sigterm; sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL);
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    fprintf(stderr, "[supervisor] ready. rootfs=%s\n", rootfs);
    while (!ctx.should_stop) {
        if (g_reap_needed) reap_zombies(&ctx);
        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd >= 0) {
            control_request_t req;
            if (read(client_fd, &req, sizeof(req)) == sizeof(req)) dispatch(&ctx, &req, client_fd);
            close(client_fd);
        } else { usleep(50000); }
    }
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    return 0;
}

int main(int argc, char *argv[]) {
    if (argc < 2) { usage(argv[0]); return 1; }
    if (strcmp(argv[1], "supervisor") == 0) return run_supervisor(argv[2]);
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run") == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps") == 0) return cmd_ps();
    if (strcmp(argv[1], "logs") == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop") == 0) return cmd_stop(argc, argv);
    usage(argv[0]); return 1;
}

static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 1;
    struct sockaddr_un addr; memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX; snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", CONTROL_PATH);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(fd); return 1; }
    if (write(fd, req, sizeof(*req)) != (ssize_t)sizeof(*req)) { close(fd); return 1; }
    shutdown(fd, SHUT_WR);
    control_response_t resp;
    if (read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp)) {
        printf("%s", resp.message);
        char buf[512]; ssize_t m;
        while ((m = read(fd, buf, sizeof(buf))) > 0) if (write(STDOUT_FILENO, buf, (size_t)m) < 0) break;
    }
    close(fd); return 0;
}

static int cmd_start(int argc, char *argv[]) {
    control_request_t req;
    if (argc < 5) return 1;
    memset(&req, 0, sizeof(req)); req.kind = CMD_START;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    snprintf(req.rootfs, sizeof(req.rootfs), "%s", argv[3]);
    snprintf(req.command, sizeof(req.command), "%s", argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT; req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[]) {
    control_request_t req;
    if (argc < 5) return 1;
    memset(&req, 0, sizeof(req)); req.kind = CMD_RUN;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    snprintf(req.rootfs, sizeof(req.rootfs), "%s", argv[3]);
    snprintf(req.command, sizeof(req.command), "%s", argv[4]);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT; req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void) {
    control_request_t req;
    memset(&req, 0, sizeof(req)); req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[]) {
    control_request_t req;
    if (argc < 3) return 1;
    memset(&req, 0, sizeof(req)); req.kind = CMD_LOGS;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[]) {
    control_request_t req;
    if (argc < 3) return 1;
    memset(&req, 0, sizeof(req)); req.kind = CMD_STOP;
    snprintf(req.container_id, sizeof(req.container_id), "%s", argv[2]);
    return send_control_request(&req);
}
