/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __INCLUDE_LINUX_OOM_H
#define __INCLUDE_LINUX_OOM_H


#include <linux/sched/signal.h>
#include <linux/types.h>
#include <linux/nodemask.h>
#include <uapi/linux/oom.h>
#include <linux/sched/coredump.h> /* MMF_* */
#include <linux/mm.h> /* VM_FAULT* */

struct zonelist;
struct notifier_block;
struct mem_cgroup;
struct task_struct;

/*
 * Details of the page allocation that triggered the oom killer that are used to
 * determine what should be killed.
 */
struct oom_control {
	/* Used to determine cpuset */
	struct zonelist *zonelist;

	/* Used to determine mempolicy */
	nodemask_t *nodemask;

	/* Memory cgroup in which oom is invoked, or NULL for global oom */
	struct mem_cgroup *memcg;

	/* Used to determine cpuset and node locality requirement */
	const gfp_t gfp_mask;

	/*
	 * order == -1 means the oom kill is required by sysrq, otherwise only
	 * for display purposes.
	 */
	const int order;

	/*
	 * Only kill positive adj tasks. Used to behave more like Android's
	 * lowmemorykiller.
	 */
	const bool only_positive_adj;

	/* Used by oom implementation, do not set */
	unsigned long totalpages;
	struct task_struct *chosen;
	unsigned long chosen_points;
};

extern struct mutex oom_lock;
extern struct mutex oom_adj_mutex;

static inline void set_current_oom_origin(void)
{
	current->signal->oom_flag_origin = true;
}

static inline void clear_current_oom_origin(void)
{
	current->signal->oom_flag_origin = false;
}

static inline bool oom_task_origin(const struct task_struct *p)
{
	return p->signal->oom_flag_origin;
}

static inline bool tsk_is_oom_victim(struct task_struct * tsk)
{
#ifdef CONFIG_ANDROID_SIMPLE_LMK
	return test_ti_thread_flag(task_thread_info(tsk), TIF_MEMDIE);
#else
	return tsk->signal->oom_mm;
#endif
}

/*
 * Use this helper if tsk->mm != mm and the victim mm needs a special
 * handling. This is guaranteed to stay true after once set.
 */
static inline bool mm_is_oom_victim(struct mm_struct *mm)
{
	return test_bit(MMF_OOM_VICTIM, &mm->flags);
}

/*
 * Checks whether a page fault on the given mm is still reliable.
 * This is no longer true if the oom reaper started to reap the
 * address space which is reflected by MMF_UNSTABLE flag set in
 * the mm. At that moment any !shared mapping would lose the content
 * and could cause a memory corruption (zero pages instead of the
 * original content).
 *
 * User should call this before establishing a page table entry for
 * a !shared mapping and under the proper page table lock.
 *
 * Return 0 when the PF is safe VM_FAULT_SIGBUS otherwise.
 */
static inline vm_fault_t check_stable_address_space(struct mm_struct *mm)
{
	if (unlikely(test_bit(MMF_UNSTABLE, &mm->flags)))
		return VM_FAULT_SIGBUS;
	return 0;
}

bool __oom_reap_task_mm(struct mm_struct *mm);

extern unsigned long oom_badness(struct task_struct *p,
		struct mem_cgroup *memcg, const nodemask_t *nodemask,
		unsigned long totalpages, bool only_positive_adj);

extern bool out_of_memory(struct oom_control *oc);

extern void exit_oom_victim(void);

extern int register_oom_notifier(struct notifier_block *nb);
extern int unregister_oom_notifier(struct notifier_block *nb);

extern bool oom_killer_disable(signed long timeout);
extern void oom_killer_enable(void);

extern struct task_struct *find_lock_task_mm(struct task_struct *p);

extern void dump_tasks(struct mem_cgroup *memcg,
		       const nodemask_t *nodemask);

#ifdef CONFIG_HAVE_USERSPACE_LOW_MEMORY_KILLER
extern bool should_ulmk_retry(gfp_t gfp);
extern void ulmk_update_last_kill(void);
extern void ulmk_watchdog_fn(struct timer_list *t);
extern void ulmk_watchdog_pet(struct timer_list *t);
#else
static inline bool should_ulmk_retry(gfp_t gfp)
{
	return false;
}
static inline void ulmk_update_last_kill(void) {}
static inline void ulmk_watchdog_fn(struct timer_list *t) {}
static inline void ulmk_watchdog_pet(struct timer_list *t) {}
#endif

/* sysctls */
extern int sysctl_oom_dump_tasks;
extern int sysctl_oom_kill_allocating_task;
extern int sysctl_panic_on_oom;
extern int sysctl_reap_mem_on_sigkill;

extern atomic64_t last_oom_jiffies;

/* calls for LMK reaper */
extern void add_to_oom_reaper(struct task_struct *p);
extern void check_panic_on_foreground_kill(struct task_struct *p);
#define ULMK_MAGIC "lmkd"
#endif /* _INCLUDE_LINUX_OOM_H */
