/* 
 ****************************************************************************
 * (C) 2005 - Grzegorz Milos - Intel Research Cambridge
 ****************************************************************************
 *
 *        File: sched.c
 *      Author: Grzegorz Milos
 *     Changes: Robert Kaiser
 *              
 *        Date: Aug 2005
 * 
 * Environment: Xen Minimal OS
 * Description: simple scheduler for Mini-Os
 *
 * The scheduler is non-preemptive (cooperative), and schedules according 
 * to Round Robin algorithm.
 *
 ****************************************************************************
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
 * DEALINGS IN THE SOFTWARE.
 */

#include <mini-os/os.h>
#include <mini-os/hypervisor.h>
#include <mini-os/time.h>
#include <mini-os/mm.h>
#include <mini-os/types.h>
#include <mini-os/lib.h>
#include <mini-os/xmalloc.h>
#include <mini-os/sched.h>
#include <mini-os/semaphore.h>

#include <sys/queue.h>

TAILQ_HEAD(thread_list, thread);

struct thread *idle_thread = NULL;
static struct thread_list exited_threads = TAILQ_HEAD_INITIALIZER(exited_threads);
static struct thread_list thread_list = TAILQ_HEAD_INITIALIZER(thread_list);
static int threads_started;

struct thread *main_thread;

void inline print_runqueue(void)
{
    struct thread *th;
    TAILQ_FOREACH(th, &thread_list, thread_list)
    {
        printk("   Thread \"%s\", runnable=%d\n", th->name, is_runnable(th));
    }
    printk("\n");
}

static void (*scheduler_hook)(void *, void *);

void switch_threads(struct thread *prev, struct thread *next)
{

    if (scheduler_hook)
	scheduler_hook(prev->cookie, next->cookie);
    arch_switch_threads(prev, next);
}

void schedule(void)
{
    struct thread *prev, *next, *thread, *tmp;
    unsigned long flags;

    prev = current;
    local_irq_save(flags); 

    if (in_callback) {
        printk("Must not call schedule() from a callback\n");
        BUG();
    }
    if (flags) {
        printk("Must not call schedule() with IRQs disabled\n");
        BUG();
    }

    do {
        /* Examine all threads.
           Find a runnable thread, but also wake up expired ones and find the
           time when the next timeout expires, else use 10 seconds. */
        s_time_t now = NOW();
        s_time_t min_wakeup_time = now + SECONDS(10);
        next = NULL;
        TAILQ_FOREACH_SAFE(thread, &thread_list, thread_list, tmp)
        {
            if (!is_runnable(thread) && thread->wakeup_time != 0LL)
            {
                if (thread->wakeup_time <= now) {
		    thread->flags |= THREAD_TIMEDOUT;
                    wake(thread);
                } else if (thread->wakeup_time < min_wakeup_time)
                    min_wakeup_time = thread->wakeup_time;
            }
            if(is_runnable(thread)) 
            {
                next = thread;
                /* Put this thread on the end of the list */
                TAILQ_REMOVE(&thread_list, thread, thread_list);
                TAILQ_INSERT_TAIL(&thread_list, thread, thread_list);
                break;
            }
        }
        if (next)
            break;
        /* block until the next timeout expires, or for 10 secs, whichever comes first */
        block_domain(min_wakeup_time);
        /* handle pending events if any */
        force_evtchn_callback();
    } while(1);
    local_irq_restore(flags);
    /* Interrupting the switch is equivalent to having the next thread
       inturrupted at the return instruction. And therefore at safe point. */
    if(prev != next) switch_threads(prev, next);

    TAILQ_FOREACH_SAFE(thread, &exited_threads, thread_list, tmp)
    {
        if(thread != prev)
        {
            TAILQ_REMOVE(&exited_threads, thread, thread_list);
	    if ((thread->flags & THREAD_EXTSTACK) == 0)
                free_pages(thread->stack, STACK_SIZE_PAGE_ORDER);
            xfree(thread);
        }
    }
}

struct thread *
create_thread(const char *name, void *cookie,
	void (*function)(void *), void *data, void *stack)
{
    struct thread *thread;
    unsigned long flags;
    /* Call architecture specific setup. */
    thread = arch_create_thread(name, function, data, stack);
    /* Not runable, not exited, not sleeping */
    thread->flags = 0;
    thread->wakeup_time = 0LL;
    thread->lwp = NULL;
    thread->cookie = cookie;
    set_runnable(thread);
    local_irq_save(flags);
    TAILQ_INSERT_TAIL(&thread_list, thread, thread_list);
    local_irq_restore(flags);
    return thread;
}

struct join_waiter {
    struct thread *jw_thread;
    struct thread *jw_wanted;
    TAILQ_ENTRY(join_waiter) jw_entries;
};
static TAILQ_HEAD(, join_waiter) joinwq = TAILQ_HEAD_INITIALIZER(joinwq);

void exit_thread(void)
{
    unsigned long flags;
    struct thread *thread = current;
    struct join_waiter *jw_iter;

    /* if joinable, gate until we are allowed to exit */
    local_irq_save(flags);
    while (thread->flags & THREAD_MUSTJOIN) {
        thread->flags |= THREAD_JOINED;
        local_irq_restore(flags);

        /* see if the joiner is already there */
        TAILQ_FOREACH(jw_iter, &joinwq, jw_entries) {
            if (jw_iter->jw_wanted == thread) {
                wake(jw_iter->jw_thread);
                break;
            }
        }
        block(thread);
        schedule();
        local_irq_save(flags);
    }

    /* interrupts still disabled ... */

    /* Remove from the thread list */
    TAILQ_REMOVE(&thread_list, thread, thread_list);
    clear_runnable(thread);
    /* Put onto exited list */
    TAILQ_INSERT_HEAD(&exited_threads, thread, thread_list);
    local_irq_restore(flags);

    /* Schedule will free the resources */
    while(1)
    {
        schedule();
        printk("schedule() returned!  Trying again\n");
    }
}

/* hmm, all of the interfaces here are namespaced "backwards" ... */
void join_thread(struct thread *joinable)
{
    struct join_waiter jw;
    struct thread *thread = get_current();
    unsigned long flags;

    local_irq_save(flags);
    ASSERT(joinable->flags & THREAD_MUSTJOIN);
    /* wait for exiting thread to hit thread_exit() */
    while (!(joinable->flags & THREAD_JOINED)) {
        local_irq_restore(flags);

        jw.jw_thread = thread;
        jw.jw_wanted = joinable;
        TAILQ_INSERT_TAIL(&joinwq, &jw, jw_entries);
        block(thread);
        schedule();
        TAILQ_REMOVE(&joinwq, &jw, jw_entries);

        local_irq_save(flags);
    }

    /* signal exiting thread that we have seen it and it may now exit */
    ASSERT(joinable->flags & THREAD_JOINED);
    joinable->flags &= ~THREAD_MUSTJOIN;
    local_irq_restore(flags);

    wake(joinable);
}

void block(struct thread *thread)
{
    thread->wakeup_time = 0LL;
    clear_runnable(thread);
}

static int
dosleep(s_time_t wakeuptime)
{
    struct thread *thread = get_current();
    int rv;

    thread->wakeup_time = wakeuptime;
    thread->flags &= ~THREAD_TIMEDOUT;
    clear_runnable(thread);
    schedule();

    rv = !!(thread->flags & THREAD_TIMEDOUT);
    thread->flags &= ~THREAD_TIMEDOUT;
    return rv;
}

int msleep(uint32_t millisecs)
{

    return dosleep(NOW() + MILLISECS(millisecs));
}

int absmsleep(uint32_t millisecs)
{

    return dosleep(MILLISECS(millisecs));
}

void wake(struct thread *thread)
{
    thread->wakeup_time = 0LL;
    set_runnable(thread);
}

void idle_thread_fn(void *unused)
{
    threads_started = 1;
    while (1) {
        block(current);
        schedule();
    }
}

void init_sched(void)
{
    printk("Initialising scheduler\n");

    idle_thread = create_thread("Idle", NULL, idle_thread_fn, NULL, NULL);
}

void set_sched_hook(void (*f)(void *, void *))
{

    scheduler_hook = f;
}

struct thread *init_mainlwp(void *cookie)
{

    current->cookie = cookie;
    return current;
}

/*
 * Local variables:
 * mode: C
 * c-set-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
