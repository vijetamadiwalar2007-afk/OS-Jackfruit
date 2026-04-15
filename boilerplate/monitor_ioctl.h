#ifndef MONITOR_IOCTL_H
#define MONITOR_IOCTL_H

#include <linux/ioctl.h>

struct monitor_params {
    int pid;
    int soft_mib;
    int hard_mib;
};

#define MONITOR_REGISTER_PID _IOW('a','a',struct monitor_params)
#define MONITOR_UNREGISTER_PID _IOW('a','b',struct monitor_params)

#endif
