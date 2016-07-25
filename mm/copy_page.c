/*
 * Use DMA engine to copy page data
 *
 * Zi Yan
 *
 */

#include <linux/highmem.h>
#include <linux/dmaengine.h>
#include <linux/dma-mapping.h>

#define NUM_AVAIL_DMA_CHAN 16


int use_all_dma_chans = 0;
int limit_dma_chans = NUM_AVAIL_DMA_CHAN;


struct dma_chan *copy_chan[NUM_AVAIL_DMA_CHAN] = {0};
struct dma_device *copy_dev[NUM_AVAIL_DMA_CHAN] = {0};



#ifdef CONFIG_PROC_SYSCTL
int sysctl_dma_page_migration(struct ctl_table *table, int write,
				 void __user *buffer, size_t *lenp,
				 loff_t *ppos)
{
	int err = 0;
	int use_all_dma_chans_prior_val = use_all_dma_chans;
	dma_cap_mask_t copy_mask;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	err = proc_dointvec_minmax(table, write, buffer, lenp, ppos);

	if (err < 0)
		return err;
	if (write) {
		/* Grab all DMA channels  */
		if (use_all_dma_chans_prior_val == 0 && use_all_dma_chans == 1) {
			int i;

			dma_cap_zero(copy_mask);
			dma_cap_set(DMA_MEMCPY, copy_mask);

			dmaengine_get();
			for (i = 0; i < NUM_AVAIL_DMA_CHAN; ++i) {
				if (!copy_chan[i]) {
					copy_chan[i] = dma_request_channel(copy_mask, NULL, NULL);
				}
				if (!copy_chan[i]) {
					pr_err("%s: cannot grab channel: %d\n", __func__, i);
					continue;
				}

				copy_dev[i] = copy_chan[i]->device;

				if (!copy_dev[i]) {
					pr_err("%s: no device: %d\n", __func__, i);
					continue;
				}
			}

		} 
		/* Release all DMA channels  */
		else if (use_all_dma_chans_prior_val == 1 && use_all_dma_chans == 0) {
			int i;

			for (i = 0; i < NUM_AVAIL_DMA_CHAN; ++i) {
				if (copy_chan[i]) {
					dma_release_channel(copy_chan[i]);
					copy_chan[i] = NULL;
					copy_dev[i] = NULL;
				}
			}

			dmaengine_put();
		}

		if (err)
			use_all_dma_chans = use_all_dma_chans_prior_val;
	}
	return err;
}

#endif

static int copy_page_dma_once(struct page *to, struct page *from, int nr_pages)
{
	static struct dma_chan *copy_chan = NULL;
	struct dma_device *device = NULL;
	struct dma_async_tx_descriptor *tx = NULL;
	dma_cookie_t cookie;
	enum dma_ctrl_flags flags = 0;
	struct dmaengine_unmap_data *unmap = NULL;
	dma_cap_mask_t mask;
	int ret_val = 0;

	
	dma_cap_zero(mask);
	dma_cap_set(DMA_MEMCPY, mask);

	dmaengine_get();

	copy_chan = dma_request_channel(mask, NULL, NULL);

	if (!copy_chan) {
		pr_err("%s: cannot get a channel\n", __func__);
		ret_val = -1;
		goto no_chan;
	}

	device = copy_chan->device;

	if (!device) {
		pr_err("%s: cannot get a device\n", __func__);
		ret_val = -2;
		goto release;
	}
		
	unmap = dmaengine_get_unmap_data(device->dev, 2, GFP_NOWAIT);

	if (!unmap) {
		pr_err("%s: cannot get unmap data\n", __func__);
		ret_val = -3;
		goto release;
	}

	unmap->to_cnt = 1;
	unmap->addr[0] = dma_map_page(device->dev, from, 0, PAGE_SIZE*nr_pages,
					  DMA_TO_DEVICE);
	unmap->from_cnt = 1;
	unmap->addr[1] = dma_map_page(device->dev, to, 0, PAGE_SIZE*nr_pages,
					  DMA_FROM_DEVICE);
	unmap->len = PAGE_SIZE*nr_pages;

	tx = device->device_prep_dma_memcpy(copy_chan, 
						unmap->addr[1],
						unmap->addr[0], unmap->len,
						flags);

	if (!tx) {
		pr_err("%s: null tx descriptor\n", __func__);
		ret_val = -4;
		goto unmap_dma;
	}

	cookie = tx->tx_submit(tx);

	if (dma_submit_error(cookie)) {
		pr_err("%s: submission error\n", __func__);
		ret_val = -5;
		goto unmap_dma;
	}

	if (dma_sync_wait(copy_chan, cookie) != DMA_COMPLETE) {
		pr_err("%s: dma does not complete properly\n", __func__);
		ret_val = -6;
	}

unmap_dma:
	dmaengine_unmap_put(unmap);
release:
	if (copy_chan) {
		dma_release_channel(copy_chan);
	}
no_chan:
	dmaengine_put();

	return ret_val;
}

static int copy_page_dma_always(struct page *to, struct page *from, int nr_pages)
{
	struct dma_async_tx_descriptor *tx[NUM_AVAIL_DMA_CHAN] = {0};
	dma_cookie_t cookie[NUM_AVAIL_DMA_CHAN];
	enum dma_ctrl_flags flags[NUM_AVAIL_DMA_CHAN] = {0};
	struct dmaengine_unmap_data *unmap[NUM_AVAIL_DMA_CHAN] = {0};
	int ret_val = 0;
	int total_available_chans = NUM_AVAIL_DMA_CHAN;
	int i;
	size_t page_offset;

	for (i = 0; i < NUM_AVAIL_DMA_CHAN; ++i) {
		if (!copy_chan[i]) {
			total_available_chans = i;
		}
	}
	if (total_available_chans != NUM_AVAIL_DMA_CHAN) {
		pr_err("%d channels are missing", NUM_AVAIL_DMA_CHAN - total_available_chans);
	}

	total_available_chans = min_t(int, total_available_chans, limit_dma_chans);

	/* round down to closest 2^x value  */
	total_available_chans = 1<<ilog2(total_available_chans);

	if ((nr_pages != 1) && (nr_pages % total_available_chans != 0))
		return -5;
	
	for (i = 0; i < total_available_chans; ++i) {
		unmap[i] = dmaengine_get_unmap_data(copy_dev[i]->dev, 2, GFP_NOWAIT);
		if (!unmap[i]) {
			pr_err("%s: no unmap data at chan %d\n", __func__, i);
			ret_val = -3;
			goto unmap_dma;
		}
	}

	for (i = 0; i < total_available_chans; ++i) {
		if (nr_pages == 1) {
			page_offset = PAGE_SIZE / total_available_chans;

			unmap[i]->to_cnt = 1;
			unmap[i]->addr[0] = dma_map_page(copy_dev[i]->dev, from, page_offset*i,
							  page_offset,
							  DMA_TO_DEVICE);
			unmap[i]->from_cnt = 1;
			unmap[i]->addr[1] = dma_map_page(copy_dev[i]->dev, to, page_offset*i,
							  page_offset,
							  DMA_FROM_DEVICE);
			unmap[i]->len = page_offset;
		} else {
			page_offset = nr_pages / total_available_chans;

			unmap[i]->to_cnt = 1;
			unmap[i]->addr[0] = dma_map_page(copy_dev[i]->dev, 
								from + page_offset*i, 
								0,
								PAGE_SIZE*page_offset,
								DMA_TO_DEVICE);
			unmap[i]->from_cnt = 1;
			unmap[i]->addr[1] = dma_map_page(copy_dev[i]->dev, 
								to + page_offset*i, 
								0,
								PAGE_SIZE*page_offset,
								DMA_FROM_DEVICE);
			unmap[i]->len = PAGE_SIZE*page_offset;
		}
	}

	for (i = 0; i < total_available_chans; ++i) {
		tx[i] = copy_dev[i]->device_prep_dma_memcpy(copy_chan[i], 
							unmap[i]->addr[1],
							unmap[i]->addr[0], 
							unmap[i]->len,
							flags[i]);
		if (!tx[i]) {
			pr_err("%s: no tx descriptor at chan %d\n", __func__, i);
			ret_val = -4;
			goto unmap_dma;
		}
	}

	for (i = 0; i < total_available_chans; ++i) {
		cookie[i] = tx[i]->tx_submit(tx[i]);

		if (dma_submit_error(cookie[i])) {
			pr_err("%s: submission error at chan %d\n", __func__, i);
			ret_val = -5;
			goto unmap_dma;
		}
					
		dma_async_issue_pending(copy_chan[i]);
	}

	for (i = 0; i < total_available_chans; ++i) {
		if (dma_sync_wait(copy_chan[i], cookie[i]) != DMA_COMPLETE) {
			ret_val = -6;
			pr_err("%s: dma does not complete at chan %d\n", __func__, i);
		}
	}

unmap_dma:

	for (i = 0; i < total_available_chans; ++i) {
		if (unmap[i])
			dmaengine_unmap_put(unmap[i]);
	}

	return ret_val;
}

int copy_page_dma(struct page *to, struct page *from, int nr_pages)
{
	BUG_ON(hpage_nr_pages(from) != nr_pages);
	BUG_ON(hpage_nr_pages(to) != nr_pages);

	if (!use_all_dma_chans) {
		return copy_page_dma_once(to, from, nr_pages);
	} 

	return copy_page_dma_always(to, from, nr_pages);
}