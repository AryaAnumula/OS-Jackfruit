#ifndef CONTAINER_H
#define CONTAINER_H

#include <time.h>
#include <sys/types.h>
#include <pthread.h>

#define MAX_CONTAINERS  16
#define MAX_ID_LEN      64
#define MAX_PATH_LEN    256

/* ------------------------------------------------------------------ */
/*  Container lifecycle state                                           */
/* ------------------------------------------------------------------ */
typedef enum {
    STATE_STARTING        = 0,
    STATE_RUNNING         = 1,
    STATE_STOPPED         = 2,   /* clean exit or manual stop          */
    STATE_KILLED          = 3,   /* killed by supervisor (SIGKILL)     */
    STATE_HARD_LIMIT_KILLED = 4  /* killed by kernel memory monitor    */
} ContainerState;

static inline const char *state_str(ContainerState s) {
    switch (s) {
        case STATE_STARTING:          return "starting";
        case STATE_RUNNING:           return "running";
        case STATE_STOPPED:           return "stopped";
        case STATE_KILLED:            return "killed";
        case STATE_HARD_LIMIT_KILLED: return "hard_limit_killed";
        default:                      return "unknown";
    }
}

/* ------------------------------------------------------------------ */
/*  Per-container metadata                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    char           id[MAX_ID_LEN];
    pid_t          host_pid;
    time_t         start_time;
    ContainerState state;
    int            soft_mib;
    int            hard_mib;
    char           log_path[MAX_PATH_LEN];
    char           rootfs[MAX_PATH_LEN];
    int            exit_code;       /* set after child exits            */
    int            exit_signal;     /* non-zero if killed by signal     */
    int            stop_requested;  /* set to 1 before sending SIGTERM  */
    int            in_use;          /* slot occupied?                   */

    /* pipe fds: container writes stdout/stderr → supervisor reads     */
    int            pipe_stdout[2];
    int            pipe_stderr[2];

    /* producer thread for this container's output                     */
    pthread_t      producer_tid;
    int            producer_running;
} ContainerMeta;

#endif /* CONTAINER_H */
