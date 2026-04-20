// ================= monitor.c (README-READY) =================

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/jiffies.h>
#include <linux/version.h>
#include <linux/interrupt.h>
#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

// ================= DATA STRUCTURE =================
// Each container is tracked using a linked-list node

struct monitor_entry {
    pid_t pid;
    char container_id[32];

    unsigned long soft_limit;
    unsigned long hard_limit;

    int soft_triggered;   // ensures soft warning only once

    struct list_head list;
};

// ================= GLOBAL STATE =================

// Linked list of monitored containers
static LIST_HEAD(monitor_list);

// Mutex chosen (instead of spinlock) because:
// - ioctl runs in process context (can sleep)
// - safer for memory allocation paths
static DEFINE_MUTEX(monitor_lock);

// ================= DEVICE STATE =================

static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

// ================= RSS HELPER =================

static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

// ================= EVENT HANDLERS =================

static void log_soft_limit_event(const char *id, pid_t pid,
                                unsigned long limit, long rss)
{
    printk(KERN_WARNING
        "[container_monitor] SOFT LIMIT id=%s pid=%d rss=%ld limit=%lu\n",
        id, pid, rss, limit);
}

static void kill_process(const char *id, pid_t pid,
                         unsigned long limit, long rss)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
        "[container_monitor] HARD LIMIT id=%s pid=%d rss=%ld limit=%lu\n",
        id, pid, rss, limit);
}

// ================= TIMER CALLBACK =================

static void timer_callback(struct timer_list *t)
{
    struct monitor_entry *entry, *tmp;

    mutex_lock(&monitor_lock);

    // Safe iteration (supports deletion during traversal)
    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {

        long rss = get_rss_bytes(entry->pid);

        // Process no longer exists
        if (rss < 0) {
            list_del(&entry->list);
            kfree(entry);
            continue;
        }

        // Soft limit check (trigger once)
        if (!entry->soft_triggered && rss > entry->soft_limit) {
            log_soft_limit_event(entry->container_id,
                                 entry->pid,
                                 entry->soft_limit,
                                 rss);
            entry->soft_triggered = 1;
        }

        // Hard limit check (kill + remove)
        if (rss > entry->hard_limit) {
            kill_process(entry->container_id,
                         entry->pid,
                         entry->hard_limit,
                         rss);

            list_del(&entry->list);
            kfree(entry);
        }
    }

    mutex_unlock(&monitor_lock);

    // Re-arm timer
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

// ================= IOCTL =================

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
        return -EFAULT;

    // ---------- REGISTER ----------
    if (cmd == MONITOR_REGISTER) {

        struct monitor_entry *entry;

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid = req.pid;
        
        strncpy(entry->container_id, req.container_id,
        sizeof(entry->container_id) - 1);
entry->container_id[sizeof(entry->container_id) - 1] = '\0';

entry->soft_limit = req.soft_limit_bytes;
entry->hard_limit = req.hard_limit_bytes;
entry->soft_triggered = 0;

        mutex_lock(&monitor_lock);
        list_add(&entry->list, &monitor_list);
        mutex_unlock(&monitor_lock);

        return 0;
    }

    // ---------- UNREGISTER ----------
    if (cmd == MONITOR_UNREGISTER) {

        struct monitor_entry *entry, *tmp;
        int found = 0;

        mutex_lock(&monitor_lock);

        list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
            if (entry->pid == req.pid) {
                list_del(&entry->list);
                kfree(entry);
                found = 1;
                break;
            }
        }

        mutex_unlock(&monitor_lock);

        return found ? 0 : -ENOENT;
    }

    return -EINVAL;
}

// ================= FOPS =================

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

// ================= INIT =================

static int __init monitor_init(void)
{
    alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);

    cl = class_create(DEVICE_NAME);
    
    
    device_create(cl, NULL, dev_num, NULL, DEVICE_NAME);

    cdev_init(&c_dev, &fops);
    cdev_add(&c_dev, dev_num, 1);

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Loaded\n");
    return 0;
}

// ================= EXIT =================

static void __exit monitor_exit(void)
{
    struct monitor_entry *entry, *tmp;

    timer_shutdown_sync(&monitor_timer);

    mutex_lock(&monitor_lock);

    list_for_each_entry_safe(entry, tmp, &monitor_list, list) {
        list_del(&entry->list);
        kfree(entry);
    }

    mutex_unlock(&monitor_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Unloaded\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Container Memory Monitor");
