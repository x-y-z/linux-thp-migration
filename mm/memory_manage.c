/*
 * A syscall used to move pages between two nodes.
 */

#include <linux/sched/mm.h>
#include <linux/cpuset.h>
#include <linux/mempolicy.h>
#include <linux/memcontrol.h>
#include <linux/migrate.h>
#include <linux/exchange.h>
#include <linux/mm_inline.h>
#include <linux/nodemask.h>
#include <linux/rmap.h>
#include <linux/security.h>
#include <linux/swap.h>
#include <linux/syscalls.h>

#include "internal.h"

int migration_batch_size = 16;

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
		enum migrate_mode mode, int batch_size)
{
	bool migrate_concur = mode & MIGRATE_CONCUR;
	bool unlimited_batch_size = (batch_size <=0 || !migrate_concur);
	int num = 0;
	int from_nid = -1;
	int err;

	if (list_empty(page_list))
		return num;

	while (!list_empty(page_list)) {
		LIST_HEAD(batch_page_list);
		int i;

		/* it should move all pages to batch_page_list if !migrate_concur */
		for (i = 0; i < batch_size || unlimited_batch_size; i++) {
			struct page *item = list_first_entry_or_null(page_list, struct page, lru);
			if (!item)
				break;
			list_move(&item->lru, &batch_page_list);
		}

		from_nid = page_to_nid(list_first_entry(&batch_page_list, struct page, lru));

		if (migrate_concur)
			err = migrate_pages_concur(&batch_page_list, alloc_new_node_page,
				NULL, nid, mode, MR_SYSCALL);
		else
			err = migrate_pages(&batch_page_list, alloc_new_node_page,
				NULL, nid, mode, MR_SYSCALL);

		if (err) {
			struct page *page;

			list_for_each_entry(page, &batch_page_list, lru)
				num += hpage_nr_pages(page);

			putback_movable_pages(&batch_page_list);
		}
	}
	pr_debug("%d pages failed to migrate from %d to %d\n",
		num, from_nid, nid);
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

static int add_pages_to_exchange_list(struct list_head *from_pagelist,
	struct list_head *to_pagelist, struct exchange_page_info *info_list,
	struct list_head *exchange_list, unsigned long info_list_size)
{
	unsigned long info_list_index = 0;
	LIST_HEAD(failed_from_list);
	LIST_HEAD(failed_to_list);

	while (!list_empty(from_pagelist) && !list_empty(to_pagelist)) {
		struct page *from_page, *to_page;
		struct exchange_page_info *one_pair = &info_list[info_list_index];
		int rc;

		from_page = list_first_entry_or_null(from_pagelist, struct page, lru);
		to_page = list_first_entry_or_null(to_pagelist, struct page, lru);

		if (!from_page || !to_page)
			break;

		if (!thp_migration_supported() && PageTransHuge(from_page)) {
			lock_page(from_page);
			rc = split_huge_page_to_list(from_page, &from_page->lru);
			unlock_page(from_page);
			if (rc) {
				list_move(&from_page->lru, &failed_from_list);
				continue;
			}
		}

		if (!thp_migration_supported() && PageTransHuge(to_page)) {
			lock_page(to_page);
			rc = split_huge_page_to_list(to_page, &to_page->lru);
			unlock_page(to_page);
			if (rc) {
				list_move(&to_page->lru, &failed_to_list);
				continue;
			}
		}

		if (hpage_nr_pages(from_page) != hpage_nr_pages(to_page)) {
			if (!(hpage_nr_pages(from_page) == 1 && hpage_nr_pages(from_page) == HPAGE_PMD_NR)) {
				list_del(&from_page->lru);
				list_add(&from_page->lru, &failed_from_list);
			}
			if (!(hpage_nr_pages(to_page) == 1 && hpage_nr_pages(to_page) == HPAGE_PMD_NR)) {
				list_del(&to_page->lru);
				list_add(&to_page->lru, &failed_to_list);
			}
			continue;
		}

		/* Exclude file-backed pages, exchange it concurrently is not
		 * implemented yet. */
		if (page_mapping(from_page)) {
			list_del(&from_page->lru);
			list_add(&from_page->lru, &failed_from_list);
			continue;
		}
		if (page_mapping(to_page)) {
			list_del(&to_page->lru);
			list_add(&to_page->lru, &failed_to_list);
			continue;
		}

		list_del(&from_page->lru);
		list_del(&to_page->lru);

		one_pair->from_page = from_page;
		one_pair->to_page = to_page;

		list_add_tail(&one_pair->list, exchange_list);

		info_list_index++;
		if (info_list_index >= info_list_size)
			break;
	}
	list_splice(&failed_from_list, from_pagelist);
	list_splice(&failed_to_list, to_pagelist);

	return info_list_index;
}

static unsigned long exchange_pages_between_nodes(unsigned long nr_from_pages,
	unsigned long nr_to_pages, struct list_head *from_page_list,
	struct list_head *to_page_list, int batch_size,
	bool huge_page, enum migrate_mode mode)
{
	struct exchange_page_info *info_list;
	unsigned long info_list_size = min_t(unsigned long,
		nr_from_pages, nr_to_pages) / (huge_page?HPAGE_PMD_NR:1);
	unsigned long added_size = 0;
	bool migrate_concur = mode & MIGRATE_CONCUR;
	LIST_HEAD(exchange_list);

	/* non concurrent does not need to split into batches  */
	if (!migrate_concur || batch_size <= 0)
		batch_size = info_list_size;

	/* prepare for huge page split  */
	if (!thp_migration_supported() && huge_page) {
		batch_size = batch_size * HPAGE_PMD_NR;
		info_list_size = info_list_size * HPAGE_PMD_NR;
	}

	info_list = kvzalloc(sizeof(struct exchange_page_info)*batch_size,
			GFP_KERNEL);
	if (!info_list)
		return 0;

	while (!list_empty(from_page_list) && !list_empty(to_page_list)) {
		unsigned long nr_added_pages;
		INIT_LIST_HEAD(&exchange_list);

		nr_added_pages = add_pages_to_exchange_list(from_page_list, to_page_list,
			info_list, &exchange_list, batch_size);

		/*
		 * Nothing to exchange, we bail out.
		 *
		 * In case from_page_list and to_page_list both only have file-backed
		 * pages left */
		if (!nr_added_pages)
			break;

		added_size += nr_added_pages;

		VM_BUG_ON(added_size > info_list_size);

		if (migrate_concur)
			exchange_pages_concur(&exchange_list, mode, MR_SYSCALL);
		else
			exchange_pages(&exchange_list, mode, MR_SYSCALL);

		memset(info_list, 0, sizeof(struct exchange_page_info)*batch_size);
	}

	kvfree(info_list);

	return info_list_size;
}

static int do_mm_manage(struct task_struct *p, struct mm_struct *mm,
		const nodemask_t *slow, const nodemask_t *fast,
		unsigned long nr_pages, int flags)
{
	bool migrate_mt = flags & MPOL_MF_MOVE_MT;
	bool migrate_concur = flags & MPOL_MF_MOVE_CONCUR;
	bool migrate_dma = flags & MPOL_MF_MOVE_DMA;
	bool move_hot_and_cold_pages = flags & MPOL_MF_MOVE_ALL;
	bool migrate_exchange_pages = flags & MPOL_MF_EXCHANGE;
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

		if (migrate_exchange_pages) {
			unsigned long nr_exchange_pages;

			/*
			 * base pages can include file-backed ones, we do not handle them
			 * at the moment
			 */
			if (!thp_migration_supported()) {
				nr_exchange_pages =  exchange_pages_between_nodes(nr_isolated_slow_base_pages,
					nr_isolated_fast_base_pages, &slow_base_page_list,
					&fast_base_page_list, migration_batch_size, false, mode);

				nr_isolated_fast_base_pages -= nr_exchange_pages;
			}

			/* THP page exchange */
			nr_exchange_pages =  exchange_pages_between_nodes(nr_isolated_slow_huge_pages,
				nr_isolated_fast_huge_pages, &slow_huge_page_list,
				&fast_huge_page_list, migration_batch_size, true, mode);

			/* split THP above, so we do not need to multiply the counter */
			if (!thp_migration_supported())
				nr_isolated_fast_huge_pages -= nr_exchange_pages;
			else
				nr_isolated_fast_huge_pages -= nr_exchange_pages * HPAGE_PMD_NR;

			goto migrate_out;
		} else {
migrate_out:
		/* Migrate pages to slow node */
		/* No multi-threaded migration for base pages */
		nr_isolated_fast_base_pages -=
			migrate_to_node(&fast_base_page_list, slow_nid,
				mode & ~MIGRATE_MT, migration_batch_size);

		nr_isolated_fast_huge_pages -=
			migrate_to_node(&fast_huge_page_list, slow_nid, mode,
				migration_batch_size);
		}
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
		migrate_to_node(&slow_base_page_list, fast_nid, mode & ~MIGRATE_MT,
				migration_batch_size);

	nr_isolated_slow_huge_pages -=
		migrate_to_node(&slow_huge_page_list, fast_nid, mode,
				migration_batch_size);

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