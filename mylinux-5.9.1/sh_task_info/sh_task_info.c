#include<linux/kernel.h>
#include<linux/init.h>
#include<linux/sched.h>
#include<linux/syscalls.h>

SYSCALL_DEFINE2(sh_task_info, long, pid, char*, path) {
    char buff[512];
    char id[65];

    pit_t p = pid;

    struct task_struct *st;
    st = find_task_by_vpid(p);

    long cur_state = st->state;
    int cpu_num = st->on_cpu;
    int last_cpu_num = st->recent_used_cpu;
    int exit_state = st->exit_state;
    int priority = st->prio;

    printk(KERN_INFO "Task PID: %d \n Priority: %d \n CPU number : %d \n last : %d \n exit : %d \n preo: %d\n", st->state,  st->on_cpu, st->recent_used_cpu, st->exit_state, st->prio);

    sprintf(id, "%d", p);
    sprintf(id, "%d", p);
    sprintf(id, "%d", p);
    sprintf(id, "%d", p);
    sprintf(id, "%d", p);

    sprintf(id, "%d", p);
    long copied = strncpy_from_user(buff, path, sizeof(buff));

    int b = strlen(buff);
    
    struct file *fil;
    int y = 00700;

    fil = file_open(buff,O_CREAT, y);
    filp_close(fil, NULL);
    y =0;
    fil =  file_open(buff,O_WRONLY, y);

    long var = kernel_write(fil, buff, b , 0);

    printk(KERN_INFO "output new syscall called with %s %s \n", id, buff);
    filp_close(fil, NULL);
    return 0;

}
