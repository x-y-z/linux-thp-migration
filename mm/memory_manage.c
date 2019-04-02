/*
 * A syscall used to move pages between two nodes.
 */

#include <linux/sched/mm.h>
#include <linux/cpuset.h>
#include <linux/mempolicy.h>
#include <linux/memcontrol.h>
#include <linux/migrate.h>
#include <linux/mm_inline.h>
#include <linux/nodemask.h>
#include <linux/rmap.h>
#include <linux/security.h>
#include <linux/swap.h>
#include <linux/syscalls.h>

#include "internal.h"

enum isolate_action {
	ISOLATE_COLD_PAGES = 1,
	ISOLATE_HOT_PAGES,
	ISOLATE_HOT_AND_COLD_PAGES,
};

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

static unsigned long isolate_pages_from_lru_list(pg_data_t *pgdat,
		struct mem_cgroup *memcg, unsigned long nr_pages,
		struct list_head *base_page_list,
		struct list_head *huge_page_list,
		unsigned long *nr_taken_base_page,
		unsigned long *nr_taken_huge_page,
		enum isolate_action action)
{
	struct lruvec *lruvec = mem_cgroup_lruvec(pgdat, memcg);
	enum lru_list lru;
	unsigned long nr_all_taken = 0;

	if (nr_pages == ULONG_MAX)
		nr_pages = memcg_size_node(memcg, pgdat->node_id);

	lru_add_drain_all();

	for_each_evictable_lru(lru) {
		unsigned long nr_scanned, nr_taken;
		int file = is_file_lru(lru);
		struct scan_control sc = {.may_unmap = 1};

		if (action == ISOLATE_COLD_PAGES && is_active_lru(lru))
			continue;
		if (action == ISOLATE_HOT_PAGES && !is_active_lru(lru))
			continue;

		spin_lock_irq(&pgdat->lru_lock);

		/* Isolate base pages */
		sc.isolate_only_base_page = 1;
		nr_taken = isolate_lru_pages(nr_pages, lruvec, base_page_list,
					&nr_scanned, &sc, lru);
		/* Isolate huge pages */
		sc.isolate_only_base_page = 0;
		sc.isolate_only_huge_page = 1;
		nr_taken += isolate_lru_pages(nr_pages - nr_scanned, lruvec,
					huge_page_list, &nr_scanned, &sc, lru);

		__mod_node_page_state(pgdat, NR_ISOLATED_ANON + file, nr_taken);

		spin_unlock_irq(&pgdat->lru_lock);

		nr_all_taken += nr_taken;

		if (nr_all_taken > nr_pages)
			break;
	}

	return nr_all_taken;
}

static int migrate_to_node(struct list_head *page_list, int nid,
		enum migrate_mode mode)
{
	bool migrate_concur = mode & MIGRATE_CONCUR;
	int num = 0;
	int from_nid;
	int err;

	if (list_empty(page_list))
		return num;

	from_nid = page_to_nid(list_first_entry(page_list, struct page, lru));

	if (migrate_concur)
		err = migrate_pages_concur(page_list, alloc_new_node_page,
			NULL, nid, mode, MR_SYSCALL);
	else
		err = migrate_pages(page_list, alloc_new_node_page,
			NULL, nid, mode, MR_SYSCALL);

	if (err) {
		struct page *page;

		list_for_each_entry(page, page_list, lru)
			num += hpage_nr_pages(page);
		pr_debug("%d pages failed to migrate from %d to %d\n",
			num, from_nid, nid);

		putback_movable_pages(page_list);
	}
	return num;
}

static inline int _putback_overflow_pages(unsigned long max_nr_pages,
		struct list_head *page_list, unsigned long *nr_remaining_pages)
{
	struct page *page;
	LIST_HEAD(putback_list);

	if (list_empty(page_list))
		return max_nr_pages;

	*nr_remaining_pages = 0;
	/* in case we need to drop the whole list */
	page = list_first_entry(page_list, struct page, lru);
	if (max_nr_pages <= (2 * hpage_nr_pages(page))) {
		max_nr_pages = 0;
		putback_movable_pages(page_list);
		goto out;
	}

	list_for_each_entry(page, page_list, lru) {
		int nr_pages = hpage_nr_pages(page);
		/* drop just one more page to avoid using up free space  */
		if (max_nr_pages <= (2 * nr_pages)) {
			max_nr_pages = 0;
			break;
		}
		max_nr_pages -= nr_pages;
		*nr_remaining_pages += nr_pages;
	}

	/* we did not scan all pages in page_list, we need to put back some */
	if (&page->lru != page_list) {
		list_cut_position(&putback_list, page_list, &page->lru);
		putback_movable_pages(page_list);
		list_splice(&putback_list, page_list);
	}
out:
	return max_nr_pages;
}

static int putback_overflow_pages(unsigned long max_nr_base_pages,
		unsigned long max_nr_huge_pages,
		long nr_free_pages,
		struct list_head *base_page_list,
		struct list_head *huge_page_list,
		unsigned long *nr_base_pages,
		unsigned long *nr_huge_pages)
{
	if (nr_free_pages < 0) {
		if ((-nr_free_pages) > max_nr_base_pages) {
			nr_free_pages += max_nr_base_pages;
			max_nr_base_pages = 0;
		}

		if ((-nr_free_pages) > max_nr_huge_pages) {
			nr_free_pages = 0;
			max_nr_base_pages = 0;
		}
	}
	/*
	 * counting pages in page lists and substract the number from max_nr_*
	 * when max_nr_* go to zero, drop the remaining pages
	 */
	max_nr_huge_pages += _putback_overflow_pages(nr_free_pages/2 + max_nr_base_pages,
			base_page_list, nr_base_pages);
	return _putback_overflow_pages(nr_free_pages/2 + max_nr_huge_pages,
			huge_page_list, nr_huge_pages);
}

static int do_mm_manage(struct task_struct *p, struct mm_struct *mm,
		const nodemask_t *slow, const nodemask_t *fast,
		unsigned long nr_pages, int flags)
{
	bool migrate_mt = flags & MPOL_MF_MOVE_MT;
	bool migrate_concur = flags & MPOL_MF_MOVE_CONCUR;
	bool migrate_dma = flags & MPOL_MF_MOVE_DMA;
	bool move_hot_and_cold_pages = flags & MPOL_MF_MOVE_ALL;
	struct mem_cgroup *memcg = mem_cgroup_from_task(p);
	int err = 0;
	unsigned long nr_isolated_slow_pages;
	unsigned long nr_isolated_slow_base_pages = 0;
	unsigned long nr_isolated_slow_huge_pages = 0;
	unsigned long nr_isolated_fast_pages;
	/* in case no migration from to node, we migrate all isolated pages from
	 * slow node  */
	unsigned long nr_isolated_fast_base_pages = ULONG_MAX;
	unsigned long nr_isolated_fast_huge_pages = ULONG_MAX;
	unsigned long max_nr_pages_fast_node, nr_pages_fast_node;
	unsigned long nr_pages_slow_node, nr_active_pages_slow_node;
	long nr_free_pages_fast_node;
	int slow_nid, fast_nid;
	enum migrate_mode mode = MIGRATE_SYNC |
		(migrate_mt ? MIGRATE_MT : MIGRATE_SINGLETHREAD) |
		(migrate_dma ? MIGRATE_DMA : MIGRATE_SINGLETHREAD) |
		(migrate_concur ? MIGRATE_CONCUR : MIGRATE_SINGLETHREAD);
	enum isolate_action isolate_action =
		move_hot_and_cold_pages?ISOLATE_HOT_AND_COLD_PAGES:ISOLATE_HOT_PAGES;
	LIST_HEAD(slow_base_page_list);
	LIST_HEAD(slow_huge_page_list);

	if (!memcg)
		return 0;
	/* Let's handle simplest situation first */
	if (!(nodes_weight(*slow) == 1 && nodes_weight(*fast) == 1))
		return 0;

	/* Only work on specific cgroup not the global root */
	if (memcg == root_mem_cgroup)
		return 0;

	slow_nid = first_node(*slow);
	fast_nid = first_node(*fast);

	max_nr_pages_fast_node = memcg_max_size_node(memcg, fast_nid);
	nr_pages_fast_node = memcg_size_node(memcg, fast_nid);
	nr_active_pages_slow_node = active_inactive_size_memcg_node(memcg,
			slow_nid, true);
	nr_pages_slow_node = memcg_size_node(memcg, slow_nid);

	nr_free_pages_fast_node = max_nr_pages_fast_node - nr_pages_fast_node;

	/* do not migrate in more pages than fast node can hold */
	nr_pages = min_t(unsigned long, max_nr_pages_fast_node, nr_pages);
	/* do not migrate away more pages than slow node has */
	nr_pages = min_t(unsigned long, nr_pages_slow_node, nr_pages);

	/* if fast node has enough space, migrate all possible pages in slow node */
	if (nr_pages != ULONG_MAX &&
		nr_free_pages_fast_node > 0 &&
		nr_active_pages_slow_node < nr_free_pages_fast_node) {
		isolate_action = ISOLATE_HOT_AND_COLD_PAGES;
	}

	nr_isolated_slow_pages = isolate_pages_from_lru_list(NODE_DATA(slow_nid),
			memcg, nr_pages, &slow_base_page_list, &slow_huge_page_list,
			&nr_isolated_slow_base_pages, &nr_isolated_slow_huge_pages,
			isolate_action);

	if (max_nr_pages_fast_node != ULONG_MAX &&
		(nr_free_pages_fast_node < 0 ||
		 nr_free_pages_fast_node < nr_isolated_slow_pages)) {
		LIST_HEAD(fast_base_page_list);
		LIST_HEAD(fast_huge_page_list);

		nr_isolated_fast_base_pages = 0;
		nr_isolated_fast_huge_pages = 0;
		/* isolate pages on fast node to make space */
		nr_isolated_fast_pages = isolate_pages_from_lru_list(NODE_DATA(fast_nid),
			memcg,
			nr_isolated_slow_pages - nr_free_pages_fast_node,
			&fast_base_page_list, &fast_huge_page_list,
			&nr_isolated_fast_base_pages, &nr_isolated_fast_huge_pages,
			move_hot_and_cold_pages?ISOLATE_HOT_AND_COLD_PAGES:ISOLATE_COLD_PAGES);

		/* Migrate pages to slow node */
		/* No multi-threaded migration for base pages */
		nr_isolated_fast_base_pages -=
			migrate_to_node(&fast_base_page_list, slow_nid, mode & ~MIGRATE_MT);

		nr_isolated_fast_huge_pages -=
			migrate_to_node(&fast_huge_page_list, slow_nid, mode);
	}

	if (nr_isolated_fast_base_pages != ULONG_MAX &&
		nr_isolated_fast_huge_pages != ULONG_MAX)
		putback_overflow_pages(nr_isolated_fast_base_pages,
				nr_isolated_fast_huge_pages, nr_free_pages_fast_node,
				&slow_base_page_list, &slow_huge_page_list,
				&nr_isolated_slow_base_pages,
				&nr_isolated_slow_huge_pages);

	/* Migrate pages to fast node */
	/* No multi-threaded migration for base pages */
	nr_isolated_slow_base_pages -=
		migrate_to_node(&slow_base_page_list, fast_nid, mode & ~MIGRATE_MT);

	nr_isolated_slow_huge_pages -=
		migrate_to_node(&slow_huge_page_list, fast_nid, mode);

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

	if (flags & MPOL_MF_MOVE)
		err = do_mm_manage(task, mm, slow, fast, nr_pages, flags);

	clear_bit(MMF_MM_MANAGE, &mm->flags);
	mmput(mm);
out:
	NODEMASK_SCRATCH_FREE(scratch);

	return err;

out_put:
	put_task_struct(task);
	goto out;

}