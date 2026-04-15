#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sched.h>
#include <sys/ioctl.h>
#include "monitor_ioctl.h"

/* ─── tunables ───────────────────────────────────────────── */
#define MAX_CONTAINERS  16
#define STACK_SIZE      (1024*1024)
#define SOCK_PATH       "/tmp/engine.sock"
#define LOG_DIR         "/tmp/engine_logs"
#define BUF_SLOTS       1024
#define BUF_MSG_MAX     4096
#define DEFAULT_SOFT    40
#define DEFAULT_HARD    64

/* ─── container state ────────────────────────────────────── */
typedef enum { ST_EMPTY=0, ST_STARTING, ST_RUNNING,
               ST_STOPPED, ST_KILLED } CState;

static const char *state_name(CState s) {
    switch(s){
        case ST_STARTING: return "starting";
        case ST_RUNNING:  return "running";
        case ST_STOPPED:  return "stopped";
        case ST_KILLED:   return "killed";
        default:          return "empty";
    }
}

/* ─── log entry ──────────────────────────────────────────── */
typedef struct {
    char cid[64];
    char msg[BUF_MSG_MAX];
    int  sentinel;
} LogEntry;

/* ─── bounded buffer ─────────────────────────────────────── */
typedef struct {
    LogEntry       slots[BUF_SLOTS];
    int            head, tail, count, shutdown;
    pthread_mutex_t mu;
    pthread_cond_t  not_full, not_empty;
} BBuf;

static BBuf g_buf;

static void bbuf_init(void) {
    pthread_mutex_init(&g_buf.mu, NULL);
    pthread_cond_init(&g_buf.not_full, NULL);
    pthread_cond_init(&g_buf.not_empty, NULL);
}

static void bbuf_push(const char *cid, const char *msg, int sentinel) {
    pthread_mutex_lock(&g_buf.mu);
    while (g_buf.count == BUF_SLOTS && !g_buf.shutdown)
        pthread_cond_wait(&g_buf.not_full, &g_buf.mu);
    if (!g_buf.shutdown) {
        LogEntry *e = &g_buf.slots[g_buf.tail];
        strncpy(e->cid, cid, 63);
        strncpy(e->msg, msg, BUF_MSG_MAX-1);
        e->sentinel = sentinel;
        g_buf.tail = (g_buf.tail+1) % BUF_SLOTS;
        g_buf.count++;
        pthread_cond_signal(&g_buf.not_empty);
    }
    pthread_mutex_unlock(&g_buf.mu);
}

static int bbuf_pop(LogEntry *out) {
    pthread_mutex_lock(&g_buf.mu);
    while (g_buf.count == 0 && !g_buf.shutdown)
        pthread_cond_wait(&g_buf.not_empty, &g_buf.mu);
    if (g_buf.count == 0) { pthread_mutex_unlock(&g_buf.mu); return -1; }
    *out = g_buf.slots[g_buf.head];
    g_buf.head = (g_buf.head+1) % BUF_SLOTS;
    g_buf.count--;
    pthread_cond_signal(&g_buf.not_full);
    pthread_mutex_unlock(&g_buf.mu);
    return 0;
}

static void bbuf_shutdown(void) {
    pthread_mutex_lock(&g_buf.mu);
    g_buf.shutdown = 1;
    pthread_cond_broadcast(&g_buf.not_empty);
    pthread_cond_broadcast(&g_buf.not_full);
    pthread_mutex_unlock(&g_buf.mu);
}

/* ─── container metadata ─────────────────────────────────── */
typedef struct {
    char   id[64];
    char   rootfs[256];
    char   command[1024];
    pid_t  pid;
    time_t start_time;
    CState state;
    int    soft_mib, hard_mib, nice_val;
    char   logfile[256];
    int    exit_status;
    int    stop_requested;
    int    pipe_read_fd;
    pthread_t producer_tid;
} Container;

static Container       g_con[MAX_CONTAINERS];
static pthread_mutex_t g_meta_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t       g_consumer_tid;
static volatile int    g_running = 1;

/* ─── clone context ──────────────────────────────────────── */
typedef struct {
    char rootfs[256];
    char command[1024];   /* full shell command string */
    int  nice_val;
    int  pipe_write_fd;
} CloneCtx;

static CloneCtx g_clonectx;




/* ─── kernel monitor helpers ─────────────────────────────── */
static void register_with_monitor(pid_t pid, int soft_mib, int hard_mib) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) return;
    struct monitor_params p = { pid, soft_mib, hard_mib };
    ioctl(fd, MONITOR_REGISTER_PID, &p);
    close(fd);
}

static void unregister_from_monitor(pid_t pid) {
    int fd = open("/dev/container_monitor", O_RDWR);
    if (fd < 0) return;
    struct monitor_params p = { pid, 0, 0 };
    ioctl(fd, MONITOR_UNREGISTER_PID, &p);
    close(fd);
}

/* ─── child fn (runs inside clone) ──────────────────────── */
static int child_fn(void *arg) {
    (void)arg;
    CloneCtx *ctx = &g_clonectx;

    /* redirect stdout+stderr to pipe */
    dup2(ctx->pipe_write_fd, STDOUT_FILENO);
    dup2(ctx->pipe_write_fd, STDERR_FILENO);
    close(ctx->pipe_write_fd);

    sethostname("container", 9);

    if (mount(NULL, "/", NULL, MS_REC|MS_PRIVATE, NULL) != 0)
        perror("mount private");

    if (chroot(ctx->rootfs) != 0) { perror("chroot"); return 1; }
    chdir("/");

    mount("proc", "/proc", "proc", 0, NULL);

    if (ctx->nice_val != 0)
        setpriority(PRIO_PROCESS, 0, ctx->nice_val);

    /* always run via /bin/sh -c so full shell commands work */
    char *argv[] = { "/bin/sh", "-c", ctx->command, NULL };
    execv("/bin/sh", argv);
    perror("execv");
    return 1;
}

/* ─── producer thread ────────────────────────────────────── */
typedef struct { char cid[64]; int fd; } ProdArg;

static void *producer_fn(void *arg) {
    ProdArg *pa = arg;
    char  cid[64]; strncpy(cid, pa->cid, 63);
    int   fd = pa->fd;
    free(pa);

    char tmp[BUF_MSG_MAX];
    ssize_t n;
    while ((n = read(fd, tmp, sizeof(tmp)-1)) > 0) {
        tmp[n] = '\0';
        bbuf_push(cid, tmp, 0);
    }
    bbuf_push(cid, "", 1);
    close(fd);
    return NULL;
}

/* ─── consumer thread ────────────────────────────────────── */
static void *consumer_fn(void *arg) {
    (void)arg;
    LogEntry e;
    while (bbuf_pop(&e) == 0) {
        if (e.sentinel) continue;
        char logfile[256] = "";
        pthread_mutex_lock(&g_meta_mu);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (g_con[i].state != ST_EMPTY &&
                strcmp(g_con[i].id, e.cid) == 0) {
                strncpy(logfile, g_con[i].logfile, 255);
                break;
            }
        }
        pthread_mutex_unlock(&g_meta_mu);
        if (logfile[0]) {
            FILE *f = fopen(logfile, "a");
            if (f) { fputs(e.msg, f); fclose(f); }
        }
    }
    return NULL;
}

/* ─── SIGCHLD handler ────────────────────────────────────── */
static void sigchld_handler(int sig) {
    (void)sig;
    int status; pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&g_meta_mu);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (g_con[i].pid == pid) {
                unregister_from_monitor(g_con[i].pid);
g_con[i].exit_status = status;
                if (g_con[i].stop_requested) {
                    g_con[i].state = ST_STOPPED;
                } else if (WIFSIGNALED(status) &&
                           WTERMSIG(status) == SIGKILL) {
                    g_con[i].state = ST_KILLED;
                } else {
                    g_con[i].state = ST_STOPPED;
                }
                break;
            }
        }
        pthread_mutex_unlock(&g_meta_mu);
    }
}

/* ─── SIGTERM/SIGINT handler ─────────────────────────────── */
static void sigterm_handler(int sig) {
    (void)sig;
    g_running = 0;
}


/* ─── spawn container ────────────────────────────────────── */
static int spawn_container(const char *id, const char *rootfs,
                            const char *cmd, int soft, int hard,
                            int nice_val, char *errbuf) {
    pthread_mutex_lock(&g_meta_mu);
    int slot = -1;
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_con[i].state == ST_EMPTY || 
    g_con[i].state == ST_STOPPED || 
    g_con[i].state == ST_KILLED) { slot = i; break; }

    }
   for (int i = 0; i < MAX_CONTAINERS; i++) {
    if ((g_con[i].state == ST_RUNNING || g_con[i].state == ST_STARTING) &&
        strcmp(g_con[i].id, id) == 0) {
            pthread_mutex_unlock(&g_meta_mu);
            if (errbuf) snprintf(errbuf,256,"ERROR: id '%s' already running",id);
            return -1;
        }
    }
    if (slot < 0) {
        pthread_mutex_unlock(&g_meta_mu);
        if (errbuf) snprintf(errbuf,256,"ERROR: max containers reached");
        return -1;
    }

    Container *c = &g_con[slot];
    memset(c, 0, sizeof(*c));
    strncpy(c->id,      id,     63);
    strncpy(c->rootfs,  rootfs, 255);
    strncpy(c->command, cmd,    1023);
    c->soft_mib   = soft;
    c->hard_mib   = hard;
    c->nice_val   = nice_val;
    c->state      = ST_STARTING;
    c->start_time = time(NULL);
    snprintf(c->logfile, 255, "%s/%s.log", LOG_DIR, id);
    pthread_mutex_unlock(&g_meta_mu);

    /* pipe */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        if (errbuf) snprintf(errbuf,256,"ERROR: pipe: %s",strerror(errno));
        return -1;
    }
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC);

    /* fill clone context */
    pthread_mutex_lock(&g_meta_mu);
    strncpy(g_clonectx.rootfs,  rootfs, 255);
    strncpy(g_clonectx.command, cmd,    1023);
    g_clonectx.nice_val      = nice_val;
    g_clonectx.pipe_write_fd = pipefd[1];
    pthread_mutex_unlock(&g_meta_mu);

    char *stack = malloc(STACK_SIZE);
    if (!stack) { close(pipefd[0]); close(pipefd[1]); return -1; }

    pid_t pid = clone(child_fn, stack + STACK_SIZE,
                      CLONE_NEWPID|CLONE_NEWUTS|CLONE_NEWNS|SIGCHLD,
                      NULL);
    if (pid < 0) {
        free(stack);
        close(pipefd[0]); close(pipefd[1]);
        if (errbuf) snprintf(errbuf,256,"ERROR: clone: %s",strerror(errno));
        return -1;
    }
    close(pipefd[1]);

    pthread_mutex_lock(&g_meta_mu);
    c->pid          = pid;
    c->state        = ST_RUNNING;
    c->pipe_read_fd = pipefd[0];
    pthread_mutex_unlock(&g_meta_mu);

    register_with_monitor(pid, soft, hard);
ProdArg *pa = malloc(sizeof(ProdArg));
    strncpy(pa->cid, id, 63);
    pa->fd = pipefd[0];
    pthread_create(&c->producer_tid, NULL, producer_fn, pa);

    free(stack);
    return pid;
}

/* ─── cmd: ps ────────────────────────────────────────────── */
static void cmd_ps(int client_fd) {
    char buf[4096] = "";
    char line[512];
    snprintf(line, sizeof(line),
        "%-12s %-8s %-10s %-8s %-8s %s\n",
        "ID","PID","STATE","SOFT","HARD","LOGFILE");
    strncat(buf, line, sizeof(buf)-strlen(buf)-1);

    pthread_mutex_lock(&g_meta_mu);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_con[i].state == ST_EMPTY) continue;
        snprintf(line, sizeof(line),
            "%-12s %-8d %-10s %-8d %-8d %s\n",
            g_con[i].id, g_con[i].pid,
            state_name(g_con[i].state),
            g_con[i].soft_mib, g_con[i].hard_mib,
            g_con[i].logfile);
        strncat(buf, line, sizeof(buf)-strlen(buf)-1);
    }
    pthread_mutex_unlock(&g_meta_mu);

    write(client_fd, buf, strlen(buf));
}

/* ─── cmd: logs ──────────────────────────────────────────── */
static void cmd_logs(int client_fd, const char *id) {
    char logfile[256] = "";
    pthread_mutex_lock(&g_meta_mu);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_con[i].state != ST_EMPTY &&
            strcmp(g_con[i].id, id) == 0) {
            strncpy(logfile, g_con[i].logfile, 255);
            break;
        }
    }
    pthread_mutex_unlock(&g_meta_mu);

    if (!logfile[0]) {
        char msg[] = "ERROR: container not found\n";
        write(client_fd, msg, strlen(msg));
        return;
    }
    FILE *f = fopen(logfile, "r");
    if (!f) {
        char msg[] = "ERROR: log file not found (no output yet?)\n";
        write(client_fd, msg, strlen(msg));
        return;
    }
    char tmp[1024];
    while (fgets(tmp, sizeof(tmp), f))
        write(client_fd, tmp, strlen(tmp));
    fclose(f);
}

/* ─── cmd: stop ──────────────────────────────────────────── */
static void cmd_stop(int client_fd, const char *id) {
    pid_t pid = -1;
    pthread_mutex_lock(&g_meta_mu);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_con[i].state == ST_RUNNING &&
            strcmp(g_con[i].id, id) == 0) {
            g_con[i].stop_requested = 1;
            pid = g_con[i].pid;
            break;
        }
    }
    pthread_mutex_unlock(&g_meta_mu);

    if (pid < 0) {
        char msg[] = "ERROR: container not running\n";
        write(client_fd, msg, strlen(msg));
        return;
    }
    kill(pid, SIGTERM);
    char msg[128];
    snprintf(msg, sizeof(msg), "OK: sent SIGTERM to %s (pid %d)\n", id, pid);
    write(client_fd, msg, strlen(msg));
}

/* ─── cmd: start ─────────────────────────────────────────── */
static void cmd_start(int client_fd, char *args) {
    char id[64]="", rootfs[256]="", cmd[1024]="";
    int soft=DEFAULT_SOFT, hard=DEFAULT_HARD, nice_val=0;

    char *tok = strtok(args, " \t\n");
    if (tok) { strncpy(id,     tok, 63);   tok = strtok(NULL," \t\n"); }
    if (tok) { strncpy(rootfs, tok, 255);  tok = strtok(NULL," \t\n"); }
    if (tok) { strncpy(cmd,    tok, 1023); tok = strtok(NULL," \t\n"); }

    /* collect remaining tokens — flags or extra cmd args */
    char full_cmd[1024] = "";
    strncpy(full_cmd, cmd, 1023);

    while (tok) {
        if (strcmp(tok,"--soft-mib")==0) {
            tok = strtok(NULL," \t\n");
            if (tok) soft = atoi(tok);
        } else if (strcmp(tok,"--hard-mib")==0) {
            tok = strtok(NULL," \t\n");
            if (tok) hard = atoi(tok);
        } else if (strcmp(tok,"--nice")==0) {
            tok = strtok(NULL," \t\n");
            if (tok) nice_val = atoi(tok);
        } else {
            /* part of the shell command (e.g. -c "echo hello") */
            strncat(full_cmd, " ", sizeof(full_cmd)-strlen(full_cmd)-1);
            strncat(full_cmd, tok, sizeof(full_cmd)-strlen(full_cmd)-1);
        }
        tok = strtok(NULL," \t\n");
    }

    if (!id[0] || !rootfs[0] || !full_cmd[0]) {
        char msg[] = "ERROR: usage: start <id> <rootfs> <cmd>\n";
        write(client_fd, msg, strlen(msg));
        return;
    }

    char errbuf[256] = "";
    int pid = spawn_container(id, rootfs, full_cmd, soft, hard, nice_val, errbuf);
    if (pid < 0) {
        write(client_fd, errbuf, strlen(errbuf));
        write(client_fd, "\n", 1);
    } else {
        char msg[128];
        snprintf(msg, sizeof(msg), "OK: started %s pid=%d\n", id, pid);
        write(client_fd, msg, strlen(msg));
    }
}

/* ─── supervisor main loop ───────────────────────────────── */
static void run_supervisor(const char *base_rootfs) {
    (void)base_rootfs;

    mkdir(LOG_DIR, 0755);

    bbuf_init();
    pthread_create(&g_consumer_tid, NULL, consumer_fn, NULL);

    struct sigaction sa = {0};
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART|SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);
    unlink(SOCK_PATH);
    bind(srv, (struct sockaddr*)&addr, sizeof(addr));
    listen(srv, 8);
    fcntl(srv, F_SETFL, O_NONBLOCK);

    printf("[supervisor] listening on %s\n", SOCK_PATH);
    fflush(stdout);

    while (g_running) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) { usleep(10000); continue; }

        char req[2048] = "";
        ssize_t n = read(cli, req, sizeof(req)-1);
        if (n <= 0) { close(cli); continue; }
        req[n] = '\0';
        req[strcspn(req,"\n")] = '\0';

        if      (strncmp(req,"ps",2)==0)     cmd_ps(cli);
        else if (strncmp(req,"logs ",5)==0)  cmd_logs(cli, req+5);
        else if (strncmp(req,"stop ",5)==0)  cmd_stop(cli, req+5);
        else if (strncmp(req,"start ",6)==0) cmd_start(cli, req+6);
        else {
            char msg[] = "ERROR: unknown command\n";
            write(cli, msg, strlen(msg));
        }
        close(cli);
    }

    printf("[supervisor] shutting down\n");
    pthread_mutex_lock(&g_meta_mu);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (g_con[i].state == ST_RUNNING) {
            g_con[i].stop_requested = 1;
            kill(g_con[i].pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&g_meta_mu);

    sleep(1);
    bbuf_shutdown();
    pthread_join(g_consumer_tid, NULL);

    close(srv);
    unlink(SOCK_PATH);
}

/* ─── CLI client ─────────────────────────────────────────── */
static int cli_send(const char *msg) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path)-1);

    if (connect(fd,(struct sockaddr*)&addr,sizeof(addr)) != 0) {
        fprintf(stderr,
            "ERROR: cannot connect to supervisor at %s\n"
            "       Is it running? Try: sudo ./engine supervisor ./rootfs-base\n",
            SOCK_PATH);
        close(fd);
        return 1;
    }
    write(fd, msg, strlen(msg));
    write(fd, "\n", 1);

    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf)-1)) > 0) {
        buf[n] = '\0';
        fputs(buf, stdout);
    }
    close(fd);
    return 0;
}

/* ─── main ───────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  engine supervisor <base-rootfs>\n"
            "  engine start <id> <rootfs> <cmd> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  engine ps\n"
            "  engine logs <id>\n"
            "  engine stop <id>\n");
        return 1;
    }

    if (strcmp(argv[1],"supervisor")==0) {
        if (argc < 3) {
            fprintf(stderr,"Usage: engine supervisor <base-rootfs>\n");
            return 1;
        }
        run_supervisor(argv[2]);
        return 0;
    }

    if (strcmp(argv[1],"ps")==0) {
        return cli_send("ps");
    }
    if (strcmp(argv[1],"logs")==0 && argc>=3) {
        char msg[256];
        snprintf(msg, sizeof(msg), "logs %s", argv[2]);
        return cli_send(msg);
    }
    if (strcmp(argv[1],"stop")==0 && argc>=3) {
        char msg[256];
        snprintf(msg, sizeof(msg), "stop %s", argv[2]);
        return cli_send(msg);
    }
    if (strcmp(argv[1],"start")==0 && argc>=5) {
        char msg[2048];
        int off = snprintf(msg, sizeof(msg), "start %s %s %s",
                           argv[2], argv[3], argv[4]);
        for (int i = 5; i < argc && off < (int)sizeof(msg)-32; i++)
            off += snprintf(msg+off, sizeof(msg)-off, " %s", argv[i]);
        return cli_send(msg);
    }

    fprintf(stderr, "ERROR: unknown command '%s'\n", argv[1]);
    return 1;
}
