/*
 * Exchange page copy routine.
 *
 * Copyright 2019 by NVIDIA.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Authors: Zi Yan <ziy@nvidia.com>
 *
 */
#include <linux/highmem.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/freezer.h>

/*
 * nr_copythreads can be the highest number of threads for given node
 * on any architecture. The actual number of copy threads will be
 * limited by the cpumask weight of the target node.
 */
extern unsigned int limit_mt_num;

struct copy_page_info {
	struct work_struct copy_page_work;
	char *to;
	char *from;
	unsigned long chunk_size;
};

static void exchange_page_routine(char *to, char *from, unsigned long chunk_size)
{
	u64 tmp;
	int i;

	for (i = 0; i < chunk_size; i += sizeof(tmp)) {
		tmp = *((u64*)(from + i));
		*((u64*)(from + i)) = *((u64*)(to + i));
		*((u64*)(to + i)) = tmp;
	}
}

static void exchange_page_work_queue_thread(struct work_struct *work)
{
	struct copy_page_info *my_work = (struct copy_page_info*)work;

	exchange_page_routine(my_work->to,
							  my_work->from,
							  my_work->chunk_size);
}

int exchange_page_mthread(struct page *to, struct page *from, int nr_pages)
{
	int total_mt_num = limit_mt_num;
	int to_node = page_to_nid(to);
	int i;
	struct copy_page_info *work_items;
	char *vto, *vfrom;
	unsigned long chunk_size;
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;

	total_mt_num = min_t(unsigned int, total_mt_num,
						 cpumask_weight(per_node_cpumask));

	if (total_mt_num > 1)
		total_mt_num = (total_mt_num / 2) * 2;

	if (total_mt_num > 32 || total_mt_num < 1)
		return -ENODEV;

	work_items = kzalloc(sizeof(struct copy_page_info)*total_mt_num,
						 GFP_KERNEL);
	if (!work_items)
		return -ENOMEM;

	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	/* XXX: assume no highmem  */
	vfrom = kmap(from);
	vto = kmap(to);
	chunk_size = PAGE_SIZE*nr_pages / total_mt_num;

	for (i = 0; i < total_mt_num; ++i) {
		INIT_WORK((struct work_struct *)&work_items[i],
				exchange_page_work_queue_thread);

		work_items[i].to = vto + i * chunk_size;
		work_items[i].from = vfrom + i * chunk_size;
		work_items[i].chunk_size = chunk_size;

		queue_work_on(cpu_id_list[i],
					  system_highpri_wq,
					  (struct work_struct *)&work_items[i]);
	}

	/* Wait until it finishes  */
	flush_workqueue(system_highpri_wq);

	kunmap(to);
	kunmap(from);

	kfree(work_items);

	return 0;
}

int exchange_page_lists_mthread(struct page **to, struct page **from, int nr_pages)
{
	int err = 0;
	unsigned int total_mt_num = limit_mt_num;
	int to_node = page_to_nid(*to);
	int i;
	struct copy_page_info *work_items;
	const struct cpumask *per_node_cpumask = cpumask_of_node(to_node);
	int cpu_id_list[32] = {0};
	int cpu;


	total_mt_num = min_t(unsigned int, total_mt_num,
						 cpumask_weight(per_node_cpumask));
	total_mt_num = min_t(int, nr_pages, total_mt_num);

	if (total_mt_num > 32 || total_mt_num < 1)
		return -ENODEV;

	work_items = kzalloc(sizeof(struct copy_page_info)*nr_pages,
						 GFP_KERNEL);
	if (!work_items)
		return -ENOMEM;


	i = 0;
	for_each_cpu(cpu, per_node_cpumask) {
		if (i >= total_mt_num)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	for (i = 0; i < nr_pages; ++i) {
		int thread_idx = i % total_mt_num;

		INIT_WORK((struct work_struct *)&work_items[i], exchange_page_work_queue_thread);

		/* XXX: assume no highmem  */
		work_items[i].to = kmap(to[i]);
		work_items[i].from = kmap(from[i]);
		work_items[i].chunk_size = PAGE_SIZE * hpage_nr_pages(from[i]);

		BUG_ON(hpage_nr_pages(to[i]) != hpage_nr_pages(from[i]));

		queue_work_on(cpu_id_list[thread_idx], system_highpri_wq, (struct work_struct *)&work_items[i]);
	}

	/* Wait until it finishes  */
	flush_workqueue(system_highpri_wq);

	for (i = 0; i < nr_pages; ++i) {
			kunmap(to[i]);
			kunmap(from[i]);
	}

	kfree(work_items);

	return err;
}

