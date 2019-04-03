/*
 * A syscall used to move pages between two nodes.
 */

#include <linux/sched/mm.h>
#include <linux/cpuset.h>
#include <linux/mempolicy.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/nodemask.h>
#include <linux/rmap.h>
#include <linux/security.h>
#include <linux/swap.h>
#include <linux/syscalls.h>

#include "internal.h"


static unsigned long shrink_lists_node_memcg(pg_data_t *pgdat,
	struct mem_cgroup *memcg, unsigned long nr_to_scan)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);
	enum lru_list lru;

	for_each_evictable_lru(lru) {
		unsigned long nr_to_scan_local = lruvec_size_memcg_node(lru, memcg,
				pgdat->node_id) / 2;
		struct scan_control sc = {.may_unmap = 1, .no_reclaim = 1};
		/*nr_reclaimed += shrink_list(lru, nr_to_scan, lruvec, memcg, sc);*/
		/*
		 * for slow node, we want active list, we start from the top of
		 * the active list. For pages in the bottom of
		 * the inactive list, we can place it to the top of inactive list
		 */
		/*
		 * for fast node, we want inactive list, we start from the bottom of
		 * the inactive list. For pages in the active list, we just keep them.
		 */
		/*
		 * A key question is how many pages to scan each time, and what criteria
		 * to use to move pages between active/inactive page lists.
		 *  */
		if (is_active_lru(lru))
			shrink_active_list(nr_to_scan_local, lruvec, &sc, lru);
		else
			shrink_inactive_list(nr_to_scan_local, lruvec, &sc, lru);
	}
	cond_resched();

	return 0;
}

static int shrink_lists(struct task_struct *p, struct mm_struct *mm,
		const nodemask_t *slow, const nodemask_t *fast, unsigned long nr_to_scan)
{
	struct mem_cgroup *memcg = mem_cgroup_from_task(p);
	int slow_nid, fast_nid;
	int err = 0;

	if (!memcg)
		return 0;
	/* Let's handle simplest situation first */
	if (!(nodes_weight(*slow) == 1 && nodes_weight(*fast) == 1))
		return 0;

	if (memcg == root_mem_cgroup)
		return 0;

	slow_nid = first_node(*slow);
	fast_nid = first_node(*fast);

	/* move pages between page lists in slow node */
	shrink_lists_node_memcg(NODE_DATA(slow_nid), memcg, nr_to_scan);

	/* move pages between page lists in fast node */
	shrink_lists_node_memcg(NODE_DATA(fast_nid), memcg, nr_to_scan);

	return err;
}

SYSCALL_DEFINE6(mm_manage, pid_t, pid, unsigned long, nr_pages,
		unsigned long, maxnode,
		const unsigned long __user *, slow_nodes,
		const unsigned long __user *, fast_nodes,
		int, flags)
{
	const struct cred *cred = current_cred(), *tcred;
	struct task_struct *task;
	struct mm_struct *mm = NULL;
	int err;
	nodemask_t task_nodes;
	nodemask_t *slow;
	nodemask_t *fast;
	NODEMASK_SCRATCH(scratch);

	if (!scratch)
		return -ENOMEM;

	slow = &scratch->mask1;
	fast = &scratch->mask2;

	err = get_nodes(slow, slow_nodes, maxnode);
	if (err)
		goto out;

	err = get_nodes(fast, fast_nodes, maxnode);
	if (err)
		goto out;

	/* Check flags */
	if (flags & ~(
				  MPOL_MF_MOVE|
				  MPOL_MF_MOVE_MT|
				  MPOL_MF_MOVE_DMA|
				  MPOL_MF_MOVE_CONCUR|
				  MPOL_MF_EXCHANGE|
				  MPOL_MF_SHRINK_LISTS|
				  MPOL_MF_MOVE_ALL))
		return -EINVAL;

	/* Find the mm_struct */
	rcu_read_lock();
	task = pid ? find_task_by_vpid(pid) : current;
	if (!task) {
		rcu_read_unlock();
		err = -ESRCH;
		goto out;
	}
	get_task_struct(task);

	err = -EINVAL;
	/*
	 * Check if this process has the right to modify the specified
	 * process. The right exists if the process has administrative
	 * capabilities, superuser privileges or the same
	 * userid as the target process.
	 */
	tcred = __task_cred(task);
	if (!uid_eq(cred->euid, tcred->suid) && !uid_eq(cred->euid, tcred->uid) &&
	    !uid_eq(cred->uid,  tcred->suid) && !uid_eq(cred->uid,  tcred->uid) &&
	    !capable(CAP_SYS_NICE)) {
		rcu_read_unlock();
		err = -EPERM;
		goto out_put;
	}
	rcu_read_unlock();

	err = security_task_movememory(task);
	if (err)
		goto out_put;

	task_nodes = cpuset_mems_allowed(task);
	mm = get_task_mm(task);
	put_task_struct(task);

	if (!mm) {
		err = -EINVAL;
		goto out;
	}
	if (test_bit(MMF_MM_MANAGE, &mm->flags)) {
		mmput(mm);
		goto out;
	} else {
		set_bit(MMF_MM_MANAGE, &mm->flags);
	}

	if (flags & MPOL_MF_SHRINK_LISTS)
		shrink_lists(task, mm, slow, fast, nr_pages);

	clear_bit(MMF_MM_MANAGE, &mm->flags);
	mmput(mm);
out:
	NODEMASK_SCRATCH_FREE(scratch);

	return err;

out_put:
	put_task_struct(task);
	goto out;

}