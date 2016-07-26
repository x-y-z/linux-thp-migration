/*
 * This implements parallel page copy function through multi threaded
 * work queues.
 *
 * Zi Yan <ziy@nvidia.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.
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
unsigned int nr_copythreads = 8;

struct copy_info {
	struct work_struct copy_work;
	char *to;
	char *from;
	unsigned long chunk_size;
};

static void copy_pages(char *vto, char *vfrom, unsigned long size)
{
	memcpy(vto, vfrom, size);
}

static void copythread(struct work_struct *work)
{
	struct copy_info *info = (struct copy_info *) work;

	copy_pages(info->to, info->from, info->chunk_size);
}

int copy_pages_mthread(struct page *to, struct page *from, int nr_pages)
{
	unsigned int node = page_to_nid(to);
	const struct cpumask *cpumask = cpumask_of_node(node);
	struct copy_info *work_items;
	char *vto, *vfrom;
	unsigned long i, cthreads, cpu, chunk_size;
	int cpu_id_list[32] = {0};

	cthreads = nr_copythreads;
	cthreads = min_t(unsigned int, cthreads, cpumask_weight(cpumask));
	cthreads = (cthreads / 2) * 2;
	work_items = kcalloc(cthreads, sizeof(struct copy_info), GFP_KERNEL);
	if (!work_items)
		return -ENOMEM;

	i = 0;
	for_each_cpu(cpu, cpumask) {
		if (i >= cthreads)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	vfrom = kmap(from);
	vto = kmap(to);
	chunk_size = PAGE_SIZE * nr_pages / cthreads;

	for (i = 0; i < cthreads; ++i) {
		INIT_WORK((struct work_struct *) &work_items[i], copythread);

		work_items[i].to = vto + i * chunk_size;
		work_items[i].from = vfrom + i * chunk_size;
		work_items[i].chunk_size = chunk_size;

		queue_work_on(cpu_id_list[i], system_highpri_wq,
					  (struct work_struct *) &work_items[i]);
	}

	for (i = 0; i < cthreads; ++i)
		flush_work((struct work_struct *) &work_items[i]);

	kunmap(to);
	kunmap(from);
	kfree(work_items);
	return 0;
}

int copy_page_lists_mthread(struct page **to, struct page **from, int nr_pages) 
{
	int err = 0;
	unsigned int cthreads, node = page_to_nid(*to);
	int i;
	struct copy_info *work_items;
	int nr_pages_per_page = hpage_nr_pages(*from);
	const struct cpumask *cpumask = cpumask_of_node(node);
	int cpu_id_list[32] = {0};
	int cpu;

	cthreads = nr_copythreads;
	cthreads = min_t(unsigned int, cthreads, cpumask_weight(cpumask));
	cthreads = (cthreads / 2) * 2;
	cthreads = min_t(unsigned int, nr_pages, cthreads);

	work_items = kzalloc(sizeof(struct copy_info)*nr_pages,
						 GFP_KERNEL);
	if (!work_items)
		return -ENOMEM;

	i = 0;
	for_each_cpu(cpu, cpumask) {
		if (i >= cthreads)
			break;
		cpu_id_list[i] = cpu;
		++i;
	}

	for (i = 0; i < nr_pages; ++i) {
		int thread_idx = i % cthreads;

		INIT_WORK((struct work_struct *)&work_items[i], 
				  copythread);

		work_items[i].to = kmap(to[i]);
		work_items[i].from = kmap(from[i]);
		work_items[i].chunk_size = PAGE_SIZE * hpage_nr_pages(from[i]);

		BUG_ON(nr_pages_per_page != hpage_nr_pages(from[i]));
		BUG_ON(nr_pages_per_page != hpage_nr_pages(to[i]));


		queue_work_on(cpu_id_list[thread_idx], 
					  system_highpri_wq, 
					  (struct work_struct *)&work_items[i]);
	}

	/* Wait until it finishes  */
	for (i = 0; i < cthreads; ++i)
		flush_work((struct work_struct *) &work_items[i]);

	for (i = 0; i < nr_pages; ++i) {
			kunmap(to[i]);
			kunmap(from[i]);
	}

	kfree(work_items);

	return err;
}
