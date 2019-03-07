#define LINUX
#define pr_fmt(fmt) "ipanema[" KBUILD_MODNAME "]: " fmt

#include <linux/delay.h>
#include <linux/ipanema.h>
#include <linux/ipanema_rq.h>
#include <linux/ktime.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/threads.h>

#include "../kernel/sched/monitor.h"


#define ipanema_assert(x) do{if(!(x)) panic("Error in " #x "\n");} while(0)
#define time_to_ticks(x) (ktime_to_ms(x) * HZ / 1000000)
#define ticks_to_time(x) (ms_to_ktime(x * 1000000 / HZ))

static char *name = KBUILD_MODNAME;
static struct ipanema_module *module;

/* #define	RQ_NQS		64		/\* Number of run queues. *\/ */

#define SCHED_SLICE 200
#define SCHED_SLICE_MIN_DIVISOR 8
#define penalty_fork 666
#define INTERRUPT   1
#define REGULAR     2
#define INTERACTIVE 4
#define L2_CACHE  1
#define L3_CACHE  2


struct ule_ipa_process;
struct ule_ipa_core;
struct ule_ipa_sched_domain;
struct ule_ipa_sched_group;


/* definition of protocol states */
struct state_info {
	struct ule_ipa_process *curr; /* private / unshared */
	struct ipanema_rq realtime;
	struct ipanema_rq timeshare;
};


// At least a READY queue is often shared.
// Optimization: use DEFINE_PER_CPU_ALIGNED(type, name) otherwise.
// See include/linux/percpu-defs.h for more information.
DEFINE_PER_CPU_SHARED_ALIGNED(struct state_info, state_info);


/* definition of core's states */
struct core_state_info {
	cpumask_t active_cores;
	cpumask_t idle_cores;
};
static struct core_state_info cstate_info;

struct ule_ipa_process {
	/* process attributes
	 *  specified by the scheduling policy
	 *  in the process = {...} declaration
	 */
	int state; // Internal
	struct task_struct *task; // Internal
	struct task_struct *parent; //system
	int prio;
	int last_core;
	int slice;
	ktime_t rtime;
	ktime_t slptime;
	ktime_t last_blocked;
	ktime_t last_schedule;
};

struct ule_ipa_core {
	/* core attributes
	 *  specified by the scheduling policy
	 *  in the core = {...} declaration
	 */
	enum ipanema_core_state state; // Internal
	cpumask_t *cpuset; // Internal
	int id; // System
	int cload;
	struct ule_ipa_sched_domain *sd;
	bool balanced;
};

struct ule_ipa_sched_group {
	/* group attributes
	 *  specified by the scheduling policy
	 *  in the group = {...} declaration
	 */
	cpumask_t cores;
	int sharing_level;
};

/*
 * Example of topology:
 *
 *    O----------[0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15]
 *    |           /         |              |          \
 *    O----[0 1 2 3]-----[4 5 6 7]----[8 9 10 11]-----[12 13 14 15]
 *    |    /      \      /      \      /       \        /        \
 *    O--[0 1]--[2 3]--[4 5]--[6 7]--[8 9]--[10 11]--[12 13]--[14 15]
 *    |
 *   ule_ipa_topology
 */
struct ule_ipa_sched_domain {
	/* domain attributes
	 *  specified by the scheduling policy
	 *  in the domain = {...} declaration
	 */
	struct list_head siblings;  // Internal, link domains of the same level
	struct ule_ipa_sched_domain *parent; // Internal
	int ___sched_group_idx; // Internal
	spinlock_t lock; // Internal
	int flags; // Internal
	cpumask_t cores; // Internal
	struct ule_ipa_sched_group *groups;
};

static struct list_head *ule_ipa_topology;
static unsigned int ule_ipa_nr_topology_levels;

DEFINE_PER_CPU(struct ule_ipa_core, core);

static void ipa_change_proc(struct ule_ipa_process *proc,
			    struct ule_ipa_process **dst,
			    enum ipanema_state state)
{
	*dst = proc;
	proc->state = state;
	change_state(proc->task, state, task_cpu(proc->task), NULL);
}

static void ipa_change_queue(struct ule_ipa_process *proc,
			     struct ipanema_rq *rq, enum ipanema_state state)
{
	if (proc->state == IPANEMA_RUNNING)
		ipanema_state(task_cpu(proc->task)).curr = NULL;
	proc->state = state;
	change_state(proc->task, state, task_cpu(proc->task), rq);
}


static void ipa_change_queue_and_core(struct ule_ipa_process *proc,
				      struct ipanema_rq *rq,
				      enum ipanema_state state,
				      struct ule_ipa_core *core)
{
	if (proc->state == IPANEMA_RUNNING)
		ipanema_state(task_cpu(proc->task)).curr = NULL;
	proc->state = state;
	change_state(proc->task, state, core->id, rq);
}

static void set_active_core(struct ule_ipa_core *core, cpumask_t *cores,
			    int state)
{
	core->state = state;
	if (core->cpuset)
		cpumask_clear_cpu(core->id, core->cpuset);
	cpumask_set_cpu(core->id, cores);
	core->cpuset = cores;
}

static void set_sleeping_core(struct ule_ipa_core *core,
			      cpumask_t *cores, int state)
{
	core->state = state;
	if (core->cpuset)
		cpumask_clear_cpu(core->id, core->cpuset);
	cpumask_set_cpu(core->id, cores);
	core->cpuset = cores;
}

static enum ipanema_core_state
ipanema_ule_get_core_state(struct ipanema_policy *policy,
			   struct core_event *e)
{
	return ipanema_core(e->target).state;
}

static int migrate_from_to(struct ule_ipa_core *busiest,
			   struct ule_ipa_core *thief)
{
	struct task_struct *pos, *n;
	struct ule_ipa_process *t;
	int ret;
	unsigned long flags;

	/* Remove tasks from busiest */
	local_irq_save(flags);
	ipanema_lock_core(busiest->id);

	// go through realtime rq
	list_for_each_entry_safe(pos, n,
				 &ipanema_state(busiest->id).realtime.head,
				 ipanema.node_list) {
		t = policy_metadata(pos);
		if (pos->on_cpu)
			continue;
		if (!cpumask_test_cpu(thief->id, &pos->cpus_allowed))
			continue;

		ipa_change_queue_and_core(t, NULL, IPANEMA_MIGRATING, thief);
		busiest->cload--;

		goto unlock_busiest;
	}
	/* go through timeshare */
	list_for_each_entry_safe(pos, n,
				 &ipanema_state(busiest->id).timeshare.head,
				 ipanema.node_list) {
		t = policy_metadata(pos);
		if (pos->on_cpu)
			continue;
		if (!cpumask_test_cpu(thief->id, &pos->cpus_allowed))
			continue;

		ipa_change_queue_and_core(t, NULL, IPANEMA_MIGRATING, thief);
		busiest->cload--;
		goto unlock_busiest;
	}

	t = NULL;

unlock_busiest:
	ipanema_unlock_core(busiest->id);

	if (!t) {
		ret = 0;
		goto end;
	}
	/* Add them to my queue */
	ipanema_lock_core(thief->id);
	if (t->prio == REGULAR)
		ipa_change_queue(t, &ipanema_state(thief->id).timeshare,
				 IPANEMA_READY);
	else
		ipa_change_queue(t, &ipanema_state(thief->id).realtime,
				 IPANEMA_READY);
	thief->cload++;
	ipanema_unlock_core(thief->id);

	ret = 1;
end:
	local_irq_restore(flags);

	return ret;
}

static bool can_steal_core(struct ule_ipa_core *tgt,
			   struct ule_ipa_core *thief)
{
	return !tgt->balanced &&
		!thief->balanced &&
		tgt->cload > thief->cload;
}

static struct ule_ipa_core *select_core(struct ipanema_policy *policy,
					struct ule_ipa_sched_group *sg,
					cpumask_t *stealable_cores)
{
	struct ule_ipa_core *c, *victim = NULL;
	int max_cload = 0;
	int cpu;

	for_each_cpu_and(cpu, stealable_cores, &policy->allowed_cores) {
		c = &ipanema_core(cpu);
		if (c->cload > max_cload) {
			victim = c;
			max_cload = c->cload;
		}
	}

	return victim;
}

DEFINE_SPINLOCK(lb_lock);

static void steal_for_dom(struct ipanema_policy *policy,
			  struct ule_ipa_core *core_31,
			  struct ule_ipa_sched_domain *sd)
{
	cpumask_t stealable_cores;
	struct ule_ipa_core *selected, *c;
	int i;

	/* init bitmaps */
	cpumask_clear(&stealable_cores);

	/* Step 1: can_steal_core() */
	for_each_cpu_and(i, &cstate_info.active_cores, &policy->allowed_cores) {
		c = &ipanema_core(i);
		if (c == core_31)
			continue;
		if (can_steal_core(c, core_31))
			cpumask_set_cpu(i, &stealable_cores);
	}
	if (cpumask_empty(&stealable_cores))
		goto end;

	/* Step 2: select_core() */
	selected = select_core(policy, NULL, &stealable_cores);
	if (!selected)
		goto end;

	/* Step 3: steal_thread() */
	migrate_from_to(selected, core_31);
	selected->balanced = true;
end:
	core_31->balanced = true;
}

DEFINE_PER_CPU(uint32_t, randomval);
/**
 *  As defined in BSD
 */
static uint32_t sched_random(void)
{
	uint32_t *rnd = &get_cpu_var(randomval);
	uint32_t res;

	*rnd = *rnd * 69069 + 5;
	res = *rnd >> 16;
	put_cpu_var(randomval);

	return *rnd >> 16;
}

static struct ule_ipa_core *pickup_core(struct ipanema_policy *policy,
					struct ule_ipa_process *t)
{
	struct ule_ipa_core *c = &ipanema_core(task_cpu(t->task)), *idlest = c;
	struct ule_ipa_sched_domain *sd = c->sd;
	int cpu, min_cload = INT_MAX;

	/* Run interrupt threads on their core */
	if (t->prio == INTERRUPT)
		return &ipanema_core(t->last_core);

	/* Pick up an idle cpu that shares a cache */
	while (sd) {
		if (!(sd->flags & DOMAIN_CACHE))
			goto next;
		for_each_cpu_and(cpu, &sd->cores, &policy->allowed_cores) {
			if (!cpumask_test_cpu(cpu, &t->task->cpus_allowed))
				continue;
			c = &ipanema_core(cpu);
			if (c->cload == 0)
				return c;
		}
	next:
		sd = sd->parent;
	}

	/* default: get idlest cpu */
	for_each_cpu_and(cpu, &policy->allowed_cores, &t->task->cpus_allowed) {
		c = &ipanema_core(cpu);
		if (c->cload < min_cload) {
			min_cload = c->cload;
			idlest = c;
		}
	}

	return idlest;
}

static int ipanema_ule_new_prepare(struct ipanema_policy *policy,
				   struct process_event *e)
{
	struct ule_ipa_process *tgt, *parent;
	struct ule_ipa_core *idlest = NULL;
	struct task_struct *task_15;

	task_15 = e->target;
	tgt = kzalloc(sizeof(struct ule_ipa_process), GFP_ATOMIC);
	if (!tgt)
		return -1;

	policy_metadata(task_15) = tgt;
	tgt->task = task_15;
	if (task_15->parent->policy != SCHED_IPANEMA)
		tgt->parent = NULL;
	else
		tgt->parent = task_15->parent;

	/* find idlest cores on machine */
	idlest = pickup_core(policy, tgt);

	if (tgt->parent) {
		parent = policy_metadata(tgt->parent);
		parent->rtime += ticks_to_time(penalty_fork);
		tgt->prio = parent->prio;
	} else {
		tgt->prio = REGULAR;
	}
	tgt->last_core = idlest->id;

	return idlest->id;
}

static void ipanema_ule_new_place(struct ipanema_policy *policy,
				  struct process_event *e)
{
	struct ule_ipa_process *tgt = policy_metadata(e->target);
	int idlecore_10 = task_cpu(e->target);
	struct ule_ipa_core *c = &ipanema_core(idlecore_10);

	c->cload++;
	smp_wmb();
	if (tgt->prio == REGULAR)
		ipa_change_queue_and_core(tgt,
					  &ipanema_state(c->id).timeshare,
					  IPANEMA_READY, c);
	else
		ipa_change_queue_and_core(tgt,
					  &ipanema_state(c->id).realtime,
					  IPANEMA_READY, c);
}

static void ipanema_ule_new_end(struct ipanema_policy *policy,
				struct process_event *e)
{
	pr_info("[%d] post new on core %d\n",
		       e->target->pid, e->target->cpu);
}

static void ipanema_ule_detach(struct ipanema_policy *policy,
			       struct process_event *e)
/* need to free the process metadata memory */
{
	struct ule_ipa_process *tgt = policy_metadata(e->target);
	struct ule_ipa_core *c = &ipanema_core(task_cpu(tgt->task));

	ipa_change_queue(tgt, NULL, IPANEMA_TERMINATED);
	smp_wmb();
	c->cload--;
	kfree(tgt);
}

static void update_rtime(struct ule_ipa_process *t)
{
	t->rtime = ktime_sub(ktime_get(), t->last_schedule);
}

static void ipanema_ule_tick(struct ipanema_policy *policy,
			     struct process_event *e)
{
	struct ule_ipa_process *tgt = policy_metadata(e->target);

	tgt->slice--;
	if (tgt->slice <= 0) {
		update_rtime(tgt);
		ipa_change_queue(tgt,
				 &ipanema_state(task_cpu(tgt->task)).timeshare,
				 IPANEMA_READY_TICK);
	}
}

static void ipanema_ule_yield(struct ipanema_policy *policy,
			      struct process_event *e)
{
	struct ule_ipa_process *tgt = policy_metadata(e->target);

	update_rtime(tgt);
	ipa_change_queue(tgt, &ipanema_state(task_cpu(tgt->task)).timeshare,
			 IPANEMA_READY);
}

static void ipanema_ule_block(struct ipanema_policy *policy,
			      struct process_event *e)
{
	struct ule_ipa_process * tgt = policy_metadata(e->target);
	struct ule_ipa_core *c = &ipanema_core(task_cpu(e->target));

	tgt->last_blocked = ktime_get();
	ipa_change_queue(tgt, NULL, IPANEMA_BLOCKED);
	smp_wmb();
	c->cload--;
}

static bool update_realtime(struct ule_ipa_process *t)
{
	/* Computation is more complex in FreeBSD :) */
	if (ktime_after(t->slptime, t->rtime))
		t->prio = INTERACTIVE;
	else if (t->prio != INTERRUPT)
		t->prio = REGULAR;

	return (t->prio == INTERACTIVE) || (t->prio == INTERRUPT);
}

static int ipanema_ule_unblock_prepare(struct ipanema_policy *policy,
				       struct process_event *e)
{
	struct task_struct *task_15 = e->target;
	struct ule_ipa_process *p = policy_metadata(task_15);
	struct ule_ipa_core *idlest = NULL;

	idlest = pickup_core(policy, p);
	p->slptime = ktime_sub(ktime_get(), p->last_blocked);

	return idlest->id;
}

static void ipanema_ule_unblock_place(struct ipanema_policy *policy,
				      struct process_event *e)
{
	struct ule_ipa_process *tgt = policy_metadata(e->target);
	int idlecore_11 = task_cpu(e->target);
	struct ule_ipa_core *c = &ipanema_core(idlecore_11);

	c->cload++;
	smp_wmb();
	if (update_realtime(tgt))
		ipa_change_queue_and_core(tgt,
					  &ipanema_state(idlecore_11).realtime,
					  IPANEMA_READY, c);
	else
		ipa_change_queue_and_core(tgt,
					  &ipanema_state(idlecore_11).timeshare,
					  IPANEMA_READY, c);
}

static void ipanema_ule_unblock_end(struct ipanema_policy *policy,
				    struct process_event *e)
{
	pr_info("[%d] post unblock on core %d\n", e->target->pid,
		       e->target->cpu);
}

static int get_slice(struct ule_ipa_process *t)
{
	struct ule_ipa_core *c = &ipanema_core(task_cpu(t->task));
	int nb_threads = c->cload;

	if (nb_threads > SCHED_SLICE_MIN_DIVISOR)
		return SCHED_SLICE / SCHED_SLICE_MIN_DIVISOR;
	if (nb_threads == 0) {
		pr_warn("%s(): cpu%d: nr_threads = 0...\n",
			__func__, task_cpu(t->task));
		return SCHED_SLICE;
	}
	return SCHED_SLICE / nb_threads;
}

static void ipanema_ule_schedule(struct ipanema_policy *policy,
				 unsigned int cpu)
{
	struct task_struct *task_20 = NULL;
	struct ule_ipa_process *p;

	task_20 = ipanema_first_task(&ipanema_state(cpu).realtime);
	if (!task_20) {
		task_20 = ipanema_first_task(&ipanema_state(cpu).timeshare);
		if (!task_20)
			return;
	}

	p = policy_metadata(task_20);
	p->last_schedule = ktime_get();
	p->last_core = cpu;
	p->slice = get_slice(p);

	ipa_change_proc(p, &ipanema_state(cpu).curr, IPANEMA_RUNNING);
}

static void ipanema_ule_core_entry(struct ipanema_policy *policy,
				   struct core_event *e)
{
	struct ule_ipa_core *tgt = &per_cpu(core, e->target);

	tgt->balanced = false;
	set_active_core(tgt, &cstate_info.active_cores, IPANEMA_ACTIVE_CORE);
}

static void ipanema_ule_core_exit(struct ipanema_policy *policy,
				  struct core_event *e)
{
	struct ule_ipa_core *tgt = &per_cpu(core, e->target);

	set_sleeping_core(tgt, &cstate_info.idle_cores, IPANEMA_IDLE_CORE);
}

static void ipanema_ule_newly_idle(struct ipanema_policy *policy,
				   struct core_event *e)
{
	struct ule_ipa_core *c = &ipanema_core(e->target);
	struct ule_ipa_sched_domain *sd;

	for (sd = c->sd; sd; sd = sd->parent) {
		if (!(sd->flags & DOMAIN_CACHE))
			continue;

		steal_for_dom(policy, c, sd);
		if (c->cload > 0)
			break;
	}
}

static void ipanema_ule_enter_idle(struct ipanema_policy *policy,
				   struct core_event *e)
{
	struct ule_ipa_core *tgt = &per_cpu(core, e->target);

	set_sleeping_core(tgt, &cstate_info.idle_cores, IPANEMA_IDLE_CORE);
}

static void ipanema_ule_exit_idle(struct ipanema_policy *policy,
				  struct core_event *e)
{
	struct ule_ipa_core * tgt = &per_cpu(core, e->target);
	set_active_core(tgt, &cstate_info.active_cores, IPANEMA_ACTIVE_CORE);
}

static const int balance_interval = 128;
static int balance_ticks = 1;
static int balance_nr = 0;

static void ipanema_ule_balancing(struct ipanema_policy *policy,
				  struct core_event *e)
{
	struct ule_ipa_core *c, *idlest;
	int cpu = e->target;
	unsigned long flags;

	/* Generated if synchronized keyword is used */
	if (!spin_trylock_irqsave(&lb_lock, flags))
		return;

	if (--balance_ticks)
		goto end;

	balance_ticks = max(balance_interval / 2, 1) +
		(sched_random() % balance_interval);
	balance_nr++;

	for_each_cpu(cpu, &policy->allowed_cores) {
		c = &ipanema_core(cpu);
		c->balanced = false;
	}

next:
	idlest = NULL;
	for_each_cpu(cpu, &policy->allowed_cores) {
		c = &ipanema_core(cpu);
		if (c->balanced)
			continue;
		if (!idlest || c->cload < idlest->cload)
			idlest = c;
	}

	if (idlest) {
		steal_for_dom(policy, idlest, NULL);
		goto next;
	}

end:
	/* Generated if synchronized keyword is used */
	spin_unlock_irqrestore(&lb_lock, flags);
}

static int ipanema_ule_init(struct ipanema_policy * policy)
{
	return 0;
}

static bool ipanema_ule_attach(struct ipanema_policy *policy,
			       struct task_struct *_fresh_14, char *command)
{
	return true;
}

int ipanema_ule_free_metadata(struct ipanema_policy *policy)
{
	kfree(policy->data);
	return 0;
}

int ipanema_ule_can_be_default(struct ipanema_policy *policy)
{
	return 1;
}

struct ipanema_module_routines ipanema_ule_routines =
{
	.get_core_state = ipanema_ule_get_core_state,
	.new_prepare = ipanema_ule_new_prepare,
	.new_place = ipanema_ule_new_place,
	.new_end = ipanema_ule_new_end,
	.tick    = ipanema_ule_tick,
	.yield   = ipanema_ule_yield,
	.block   = ipanema_ule_block,
	.unblock_prepare
	= ipanema_ule_unblock_prepare,
	.unblock_place
	= ipanema_ule_unblock_place,
	.unblock_end
	= ipanema_ule_unblock_end,
	.terminate
	= ipanema_ule_detach,
	.schedule= ipanema_ule_schedule,
	.newly_idle
	= ipanema_ule_newly_idle,
	.enter_idle
	= ipanema_ule_enter_idle,
	.exit_idle
	= ipanema_ule_exit_idle,
	.balancing_select
	= ipanema_ule_balancing,
	.core_entry
	= ipanema_ule_core_entry,
	.core_exit
	= ipanema_ule_core_exit,
	.init    = ipanema_ule_init,
	.free_metadata
	= ipanema_ule_free_metadata,
	.can_be_default
	= ipanema_ule_can_be_default,
	.attach  = ipanema_ule_attach
};

static int init_topology(void)
{
	struct topology_level *t = per_cpu(topology_levels, 0);
	size_t size;
	int i;

	ule_ipa_nr_topology_levels = 0;

	while (t) {
		ule_ipa_nr_topology_levels++;
		t = t->next;
	}

	size = ule_ipa_nr_topology_levels * sizeof(struct list_head);
	ule_ipa_topology = kzalloc(size, GFP_KERNEL);
	if (!ule_ipa_topology) {
		ule_ipa_nr_topology_levels = 0;
		return -ENOMEM;
	}

	for (i = 0; i < ule_ipa_nr_topology_levels; i++) {
		INIT_LIST_HEAD(ule_ipa_topology + i);
	}

	return 0;
}

static void destroy_scheduling_domains(void)
{
	struct ule_ipa_sched_domain *sd, *tmp;
	int i;

	for (i = 0; i < ule_ipa_nr_topology_levels; i++) {
		list_for_each_entry_safe(sd, tmp, ule_ipa_topology + i,
					 siblings) {
			list_del(&sd->siblings);
			kfree(sd->groups);
			kfree(sd);
		}
	}

	kfree(ule_ipa_topology);
}

static int create_scheduling_domains(unsigned int cpu)
{
	struct topology_level *t = per_cpu(topology_levels, cpu);
	struct ule_ipa_core *c = &ipanema_core(cpu);
	size_t sd_size = sizeof(struct ule_ipa_sched_domain);
	unsigned int level = 0;
	struct ule_ipa_sched_domain *sd, *lower_sd = NULL;
	bool seen;

	c->sd = NULL;

	while (t) {
		/* if cpu is present in current level */
		seen = false;
		list_for_each_entry(sd, ule_ipa_topology + level, siblings) {
			if (cpumask_test_cpu(cpu, &sd->cores)) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			sd = kzalloc(sd_size, GFP_KERNEL);
			if (!sd)
				goto err;
			INIT_LIST_HEAD(&sd->siblings);
			sd->parent = NULL;
			sd->___sched_group_idx = 0;
			sd->groups = NULL;
			cpumask_copy(&sd->cores, &t->cores);
			sd->flags = t->flags;
			spin_lock_init(&sd->lock);
			list_add_tail(&sd->siblings,
				      ule_ipa_topology + level);
		}
		if (lower_sd)
			lower_sd->parent = sd;
		else
			c->sd = sd;

		if (seen)
			break;

		lower_sd = sd;
		t = t->next;
		level++;
	}

	return 0;

err:
	destroy_scheduling_domains();
	return -ENOMEM;
}

static int build_groups(struct ule_ipa_sched_domain *sd,
			unsigned int lvl)
{
	struct ule_ipa_sched_domain *sdl;
	struct ule_ipa_sched_group *sg = NULL;
	int n = 0;

	list_for_each_entry(sdl, &ule_ipa_topology[lvl - 1], siblings) {
		if (cpumask_subset(&sdl->cores, &sd->cores)) {
			n++;
			sg = krealloc(sg,
				      n * sizeof(struct ule_ipa_sched_group),
				      GFP_KERNEL);
			if (!sg)
				goto err;

			cpumask_copy(&sg[n - 1].cores, &sdl->cores);
		}
	}

	sd->___sched_group_idx = n;
	sd->groups = sg;

	return 0;

err:
	destroy_scheduling_domains();
	return -ENOMEM;
}

static int build_lower_groups(struct ule_ipa_sched_domain *sd)
{
	int cpu, n, i = 0;

	n = cpumask_weight(&sd->cores);
	sd->groups = kzalloc(n * sizeof(struct ule_ipa_sched_group),
			     GFP_KERNEL);
	if (!sd->groups)
		goto fail;
	sd->___sched_group_idx = n;

	for_each_cpu(cpu, &sd->cores) {
		cpumask_clear(&sd->groups[i].cores);
		cpumask_set_cpu(cpu, &sd->groups[i].cores);
		i++;
	}

	return 0;

fail:
	destroy_scheduling_domains();
	return -ENOMEM;
}

/* Scheduling domains must be up to date for all CPUs */
static int create_scheduling_groups(void)
{
	struct ule_ipa_sched_domain *sd = NULL;
	int i, ret;

	for (i = ule_ipa_nr_topology_levels - 1; i > 0; i--) {
		list_for_each_entry(sd, &ule_ipa_topology[i], siblings) {
			ret = build_groups(sd, i);
			if (ret)
				goto fail;
		}
	}

	list_for_each_entry(sd, ule_ipa_topology, siblings) {
		ret = build_lower_groups(sd);
		if (ret)
			goto fail;
	}

	return 0;

fail:
	destroy_scheduling_domains();
	return -ENOMEM;
}

static void build_hierarchy(void)
{
	int cpu;

	init_topology();

	/* if unicore, don't build hierarchy */
	if (!ule_ipa_nr_topology_levels)
		return;

	/* create hierarchy for all cpus */
	for_each_possible_cpu(cpu) {
		create_scheduling_domains(cpu);
	}
	create_scheduling_groups();
}

static int proc_show(struct seq_file *s, void *p)
{
	long cpu = (long) s->private;
	struct task_struct *pos, *n;
	struct ule_ipa_process *pr, *curr_proc;
	struct ule_ipa_sched_domain *sd = ipanema_core(cpu).sd;
	int i;

	ipanema_lock_core(cpu);
	pr = ipanema_state(cpu).curr;
	seq_printf(s, "CPU: %ld\n", cpu);
	seq_printf(s, "RUNNING (policy): %d\n",
		   pr ? pr->task->pid : -1);
	n = per_cpu(ipanema_current, cpu);
	seq_printf(s, "RUNNING (runtime): %d\n", n ? n->pid : -1);
	seq_printf(s, "-------------------------------\n");

	seq_printf(s, "READY[realtime]:\n");
	seq_printf(s, "rq: ");
	list_for_each_entry(pos, &ipanema_state(cpu).realtime.head,
			    ipanema.node_list) {
		curr_proc = policy_metadata(pos);
		seq_printf(s, "%d -> ", pos->pid);
	}
	seq_printf(s, "\n");
	seq_printf(s, "nr_tasks = %d\n",
		   ipanema_state(cpu).realtime.nr_tasks);

	seq_printf(s, "-------------------------------\n");
	seq_printf(s, "READY[timeshare]:\n");
	seq_printf(s, "rq: ");
	list_for_each_entry(pos, &ipanema_state(cpu).timeshare.head,
			    ipanema.node_list) {
		curr_proc = policy_metadata(pos);
		seq_printf(s, "%d -> ", pos->pid);
	}
	seq_printf(s, "\n");
	seq_printf(s, "nr_tasks = %d\n",
		   ipanema_state(cpu).timeshare.nr_tasks);

	seq_printf(s, "-------------------------------\n");
	seq_printf(s, "cload = %d\n", ipanema_core(cpu).cload);
	seq_printf(s, "balance_nr = %d\n", balance_nr);

	seq_printf(s, "\nTopology:\n");
	while (sd) {
		seq_printf(s, "[%*pbl]: ", cpumask_pr_args(&sd->cores));
		for (i = 0; i < sd->___sched_group_idx; i++)
			seq_printf(s, "{%*pbl}",
				   cpumask_pr_args(&sd->groups[i].cores));
		seq_printf(s, "\n");
		sd = sd->parent;
	}

	ipanema_unlock_core(cpu);

	return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
	long cpu;

	if (!kstrtol(file->f_path.dentry->d_iname, 10, &cpu))
		return single_open(file, proc_show, (void *)cpu);
	return -ENOENT;
}

static struct file_operations proc_fops = {
	.owner   = THIS_MODULE,
	.open    = proc_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release
};

static int proc_topo_show(struct seq_file *s, void *p)
{
	int i;
	struct ule_ipa_sched_domain *sd;

	for (i = 0; i < ule_ipa_nr_topology_levels; i++) {
		seq_printf(s, "Level %d: ", i);
		list_for_each_entry(sd, ule_ipa_topology + i, siblings) {
			seq_printf(s, "[%*pbl]", cpumask_pr_args(&sd->cores));
		}
		seq_printf(s, "\n");
	}

	return 0;
}

static int proc_topo_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_topo_show, NULL);
}

static struct file_operations proc_topo_fops = {
	.owner   = THIS_MODULE,
	.open    = proc_topo_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = single_release,
};

int init_module(void)
{
	int res, cpu;
	struct proc_dir_entry *procdir = NULL;
	char procbuf[10];

	/* Initialize scheduler variables with non-const value (function call) */
	for_each_possible_cpu(cpu) {
		ipanema_core(cpu).id = cpu;
		/* FIXME init of core variables of the user */
		ipanema_core(cpu).cload = 0;
		/* allocation of ipanema rqs */
		init_ipanema_rq(&ipanema_state(cpu).realtime, FIFO, cpu,
				IPANEMA_READY, NULL);
		init_ipanema_rq(&ipanema_state(cpu).timeshare, FIFO, cpu,
				IPANEMA_READY, NULL);
	}

	/* build hierarchy with topology */
	build_hierarchy();

	/* Allocate & setup the ipanema_module */
	module = kzalloc(sizeof(struct ipanema_module), GFP_KERNEL);
	if (!module) {
		res = -ENOMEM;
		goto end;
	}
	strncpy(module->name, name, MAX_POLICY_NAME_LEN);
	module->routines = &ipanema_ule_routines;
	module->kmodule = THIS_MODULE;

	/* Register module to the runtime */
	res = ipanema_add_module(module);
	if (res)
		goto clean_module;

	/*
	 * Create /proc/cfs/<cpu> files and /proc/cfs/topology file
	 * If file creation fails, module insertion does not
	 */
	procdir = proc_mkdir(name, ipa_procdir);
	if (!procdir)
		pr_err("%s: /proc/%s creation failed\n", name, name);
	for_each_possible_cpu(cpu) {
		scnprintf(procbuf, 10, "%d", cpu);
		if (!proc_create(procbuf, 0444, procdir, &proc_fops))
			pr_err("%s: /proc/%s/%s creation failed\n",
			       name, name, procbuf);
	}
	if (!proc_create("topology", 0444, procdir, &proc_topo_fops))
		pr_err("%s: /proc/%s/topology creation failed\n",
		       name, name);

	return 0;

clean_module:
	kfree(module);
end:
	return res;
}

void cleanup_module(void)
{
	int res;

	remove_proc_subtree(name, ipa_procdir);

	res = ipanema_remove_module(module);
	if (res) {
		pr_err("Cleanup failed (%d)\n", res);
		return;
	}

	destroy_scheduling_domains();
	kfree(module);
}

MODULE_AUTHOR("RedhaCC");
MODULE_DESCRIPTION(KBUILD_MODNAME" scheduling policy");
MODULE_LICENSE("GPL");
