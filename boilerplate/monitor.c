#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/sched.h>
#include <linux/pid.h>
#include <linux/mm.h>
#include <linux/uaccess.h>
#include <linux/signal.h>
#include "monitor_ioctl.h"

MODULE_LICENSE("GPL");

struct entry {
    pid_t pid;
    long soft, hard;
    struct list_head list;
};

static LIST_HEAD(list_head);
static DEFINE_MUTEX(lock);
static struct timer_list timer;

/* ---------- timer ---------- */
static void check(struct timer_list *t){
    struct entry *e;

    mutex_lock(&lock);

    list_for_each_entry(e,&list_head,list){
        struct task_struct *task = pid_task(find_vpid(e->pid),PIDTYPE_PID);
        if(!task) continue;

        long rss = get_mm_rss(task->mm)<<PAGE_SHIFT;

        if(rss > e->hard){
            send_sig(SIGKILL,task,1);
        }
    }

    mutex_unlock(&lock);
    mod_timer(&timer,jiffies+HZ);
}

/* ---------- ioctl ---------- */
static long ioctl_fn(struct file *f,unsigned int cmd,unsigned long arg){
    struct monitor_params p;

    copy_from_user(&p,(void*)arg,sizeof(p));

    struct entry *e = kmalloc(sizeof(*e),GFP_KERNEL);
    e->pid = p.pid;
    e->soft = p.soft_mib*1024*1024;
    e->hard = p.hard_mib*1024*1024;

    mutex_lock(&lock);
    list_add(&e->list,&list_head);
    mutex_unlock(&lock);

    return 0;
}

static struct file_operations fops = {
    .unlocked_ioctl = ioctl_fn,
};

static struct miscdevice dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "container_monitor",
    .fops = &fops
};

static int __init init_mod(void){
    misc_register(&dev);
    timer_setup(&timer,check,0);
    mod_timer(&timer,jiffies+HZ);
    return 0;
}

static void __exit exit_mod(void){
    misc_deregister(&dev);
    del_timer(&timer);
}

module_init(init_mod);
module_exit(exit_mod);
