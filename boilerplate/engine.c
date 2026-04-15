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

#define MAX_CONTAINERS 16
#define STACK_SIZE (1024*1024)
#define SOCK_PATH "/tmp/engine.sock"
#define LOG_DIR "/tmp/engine_logs"

typedef enum { ST_EMPTY=0, ST_RUNNING, ST_STOPPED } CState;

typedef struct {
    char id[64];
    pid_t pid;
    CState state;
    char logfile[256];
    int pipe_fd;
    pthread_t tid;
} Container;

static Container g_con[MAX_CONTAINERS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;

/* ---------- log thread ---------- */
static void *reader(void *arg) {
    Container *c = arg;
    char buf[1024];
    FILE *f = fopen(c->logfile,"a");

    while (1) {
        int n = read(c->pipe_fd, buf, sizeof(buf)-1);
        if (n <= 0) break;
        buf[n]=0;
        fputs(buf,f);
        fflush(f);
    }
    fclose(f);
    return NULL;
}

/* ---------- child ---------- */
static int child(void *arg) {
    int *fd = arg;
    dup2(fd[1],1);
    dup2(fd[1],2);
    close(fd[0]);
    close(fd[1]);

    execl("/bin/sh","sh","-c","/bin/ls",NULL);
    return 0;
}

/* ---------- start ---------- */
void start_container() {
    int fd[2];
    pipe(fd);

    char *stack = malloc(STACK_SIZE);

    pid_t pid = clone(child, stack+STACK_SIZE,
        CLONE_NEWPID|SIGCHLD, fd);

    Container *c = &g_con[0];
    strcpy(c->id,"c1");
    c->pid = pid;
    c->state = ST_RUNNING;
    c->pipe_fd = fd[0];
    sprintf(c->logfile,"%s/c1.log",LOG_DIR);

    pthread_create(&c->tid,NULL,reader,c);
}

/* ---------- main ---------- */
int main(int argc,char *argv[]) {

    mkdir(LOG_DIR,0755);

    if(argc<2){
        printf("usage: ./engine supervisor\n");
        return 0;
    }

    if(strcmp(argv[1],"supervisor")==0){

        start_container();
        wait(NULL);
    }

    return 0;
}
