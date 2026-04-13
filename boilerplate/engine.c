#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
void run_supervisor(const char *rootfs){
    printf("Supervisor started with rootfs: %s\n", rootfs);
    while(1){
    sleep(10);
    }
    }
void start_container(const char *id,const char *rootfs,const char *cmd){
    pid_t pid = fork();
if (pid == 0) {
chroot(rootfs);
chdir("/");
execl(cmd, cmd, NULL);
perror("exec failed");
exit(1);
}else{
    printf("started container %s with PID %d\n",id ,pid);
    }
  }
   void show_ps(){
        
        system( "ls /tmp/jackfruit/containers/");
} 
      void stop_container(const char *id){
      char path[256];
      snprintf(path,sizeof(path), "/tmp/jackfruit/containers/%s/pid", id);
      int fd = open(path,O_RDONLY);
      if(fd < 0){ printf("Container not  found\n"); return;}
      char buf[32]={0};
      read(fd,buf,sizeof(buf));
      close(fd);
      pid_t pid = atoi(buf);
      kill(pid,  SIGTERM);
      char cmd[256];
      snprintf(cmd, sizeof(cmd), "rm-rf /tmp/jackfruit/containers/%s", id);
     system(cmd);
     printf("Container %s stopped\n", id);

      }
    int main(int argc,char *argv[]){
     if(argc <2){
     printf("Usage:\n");
    printf(" engine supervisor <rootfs>\n");
     printf(" engine start <id> <rootfs> <cmd>\n");
     printf(" engine ps\n");
     printf(" engine stop <id>\n");
     return 1;
     }
      if(strcmp(argv[1], "supervisor") == 0){
        run_supervisor(argv[2]);
      }else if(strcmp(argv[1],"start") == 0){
        start_container(argv[2],argv[3],argv[4]);
      }else if(strcmp(argv[1], "ps")== 0){
      show_ps();
      }else if(strcmp(argv[1], "stop") ==0){
      stop_container(argv[2]);
      }else{
            printf("unknown command\n");
            }
            return 0;
            }
    
