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
MODULE_AUTHOR("container-runtime");
MODULE_DESCRIPTION("Container memory monitor");

/* ─── per-container entry ────────────────────────────────── */
struct monitor_entry {
    pid_t  pid;
    long   soft_bytes;
    long   hard_bytes;
    int    soft_warned;
    struct list_head list;
};

static LIST_HEAD(g_list);
static DEFINE_MUTEX(g_mutex);
static struct timer_list g_timer;

/* ─── RSS helper ─────────────────────────────────────────── */
static long get_rss_bytes(struct task_struct *task)
{
    long rss = 0;
    if (task && task->mm) {
        rss = get_mm_rss(task->mm);
        rss <<= PAGE_SHIFT;   /* pages -> bytes */
    }
    return rss;
}

/* ─── timer callback: check all monitored containers ──────── */
static void monitor_timer_cb(struct timer_list *t)
{
    struct monitor_entry *entry, *tmp;

    mutex_lock(&g_mutex);
    list_for_each_entry_safe(entry, tmp, &g_list, list) {
        struct pid    *pid_struct = find_get_pid(entry->pid);
        struct task_struct *task  = pid_task(pid_struct, PIDTYPE_PID);
        put_pid(pid_struct);

        /* remove stale entries */
        if (!task) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        long rss = get_rss_bytes(task);

        if (rss > entry->hard_bytes) {
            printk(KERN_WARNING
                "container_monitor: PID %d exceeded hard limit "
                "(%ld MiB > %ld MiB) — sending SIGKILL\n",
                entry->pid,
                rss >> 20,
                entry->hard_bytes >> 20);
            send_sig(SIGKILL, task, 1);

        } else if (rss > entry->soft_bytes && !entry->soft_warned) {
            printk(KERN_WARNING
                "container_monitor: PID %d exceeded soft limit "
                "(%ld MiB > %ld MiB) — warning\n",
                entry->pid,
                rss >> 20,
                entry->soft_bytes >> 20);
            entry->soft_warned = 1;
        }
    }
    mutex_unlock(&g_mutex);

    /* reschedule every 1 second */
    mod_timer(&g_timer, jiffies + HZ);
}

/* ─── ioctl handler ──────────────────────────────────────── */
static long monitor_ioctl(struct file *f, unsigned int cmd,
                           unsigned long arg)
{
    struct monitor_params params;

    switch (cmd) {

    case MONITOR_REGISTER_PID:
        if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
            return -EFAULT;

        struct monitor_entry *e = kzalloc(sizeof(*e), GFP_KERNEL);
        if (!e) return -ENOMEM;

        e->pid        = params.pid;
        e->soft_bytes = (long)params.soft_mib * 1024 * 1024;
        e->hard_bytes = (long)params.hard_mib * 1024 * 1024;
        e->soft_warned = 0;

        mutex_lock(&g_mutex);
        list_add(&e->list, &g_list);
        mutex_unlock(&g_mutex);

        printk(KERN_INFO
            "container_monitor: registered PID %d "
            "(soft %d MiB, hard %d MiB)\n",
            params.pid, params.soft_mib, params.hard_mib);
        return 0;

    case MONITOR_UNREGISTER_PID:
        if (copy_from_user(&params, (void __user *)arg, sizeof(params)))
            return -EFAULT;

        mutex_lock(&g_mutex);
        struct monitor_entry *cur, *tmp;
        list_for_each_entry_safe(cur, tmp, &g_list, list) {
            if (cur->pid == params.pid) {
                list_del(&cur->list);
                kfree(cur);
                break;
            }
        }
        mutex_unlock(&g_mutex);

        printk(KERN_INFO
            "container_monitor: unregistered PID %d\n", params.pid);
        return 0;

    default:
        return -ENOTTY;
    }
}

/* ─── file ops ───────────────────────────────────────────── */
static const struct file_operations monitor_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

static struct miscdevice monitor_dev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = "container_monitor",
    .fops  = &monitor_fops,
};

/* ─── init / exit ────────────────────────────────────────── */
static int __init monitor_init(void)
{
    int ret = misc_register(&monitor_dev);
    if (ret) {
        printk(KERN_ERR "container_monitor: failed to register device\n");
        return ret;
    }

    timer_setup(&g_timer, monitor_timer_cb, 0);
    mod_timer(&g_timer, jiffies + HZ);

    printk(KERN_INFO "container_monitor: loaded, /dev/container_monitor ready\n");
    return 0;
}

static void __exit monitor_exit(void)
{
    timer_delete_sync(&g_timer);

    mutex_lock(&g_mutex);
    struct monitor_entry *cur, *tmp;
    list_for_each_entry_safe(cur, tmp, &g_list, list) {
        list_del(&cur->list);
        kfree(cur);
    }
    mutex_unlock(&g_mutex);

    misc_deregister(&monitor_dev);
    printk(KERN_INFO "container_monitor: unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);
