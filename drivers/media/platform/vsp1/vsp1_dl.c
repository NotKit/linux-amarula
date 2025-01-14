// SPDX-License-Identifier: GPL-2.0+
/*
 * vsp1_dl.c  --  R-Car VSP1 Display List
 *
 * Copyright (C) 2015 Renesas Corporation
 *
 * Contact: Laurent Pinchart (laurent.pinchart@ideasonboard.com)
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/gfp.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include "vsp1.h"
#include "vsp1_dl.h"

#define VSP1_DL_NUM_ENTRIES		256

#define VSP1_DLH_INT_ENABLE		(1 << 1)
#define VSP1_DLH_AUTO_START		(1 << 0)

struct vsp1_dl_header_list {
	u32 num_bytes;
	u32 addr;
} __attribute__((__packed__));

struct vsp1_dl_header {
	u32 num_lists;
	struct vsp1_dl_header_list lists[8];
	u32 next_header;
	u32 flags;
} __attribute__((__packed__));

struct vsp1_dl_entry {
	u32 addr;
	u32 data;
} __attribute__((__packed__));

/**
 * struct vsp1_dl_body - Display list body
 * @list: entry in the display list list of bodies
 * @vsp1: the VSP1 device
 * @entries: array of entries
 * @dma: DMA address of the entries
 * @size: size of the DMA memory in bytes
 * @num_entries: number of stored entries
 */
struct vsp1_dl_body {
	struct list_head list;
	struct vsp1_device *vsp1;

	struct vsp1_dl_entry *entries;
	dma_addr_t dma;
	size_t size;

	unsigned int num_entries;
};

/**
 * struct vsp1_dl_list - Display list
 * @list: entry in the display list manager lists
 * @dlm: the display list manager
 * @header: display list header, NULL for headerless lists
 * @dma: DMA address for the header
 * @body0: first display list body
 * @fragments: list of extra display list bodies
 * @has_chain: if true, indicates that there's a partition chain
 * @chain: entry in the display list partition chain
 * @internal: whether the display list is used for internal purpose
 */
struct vsp1_dl_list {
	struct list_head list;
	struct vsp1_dl_manager *dlm;

	struct vsp1_dl_header *header;
	dma_addr_t dma;

	struct vsp1_dl_body body0;
	struct list_head fragments;

	bool has_chain;
	struct list_head chain;

	bool internal;
};

enum vsp1_dl_mode {
	VSP1_DL_MODE_HEADER,
	VSP1_DL_MODE_HEADERLESS,
};

/**
 * struct vsp1_dl_manager - Display List manager
 * @index: index of the related WPF
 * @mode: display list operation mode (header or headerless)
 * @singleshot: execute the display list in single-shot mode
 * @vsp1: the VSP1 device
 * @lock: protects the free, active, queued, pending and gc_fragments lists
 * @free: array of all free display lists
 * @active: list currently being processed (loaded) by hardware
 * @queued: list queued to the hardware (written to the DL registers)
 * @pending: list waiting to be queued to the hardware
 * @gc_work: fragments garbage collector work struct
 * @gc_fragments: array of display list fragments waiting to be freed
 */
struct vsp1_dl_manager {
	unsigned int index;
	enum vsp1_dl_mode mode;
	bool singleshot;
	struct vsp1_device *vsp1;

	spinlock_t lock;
	struct list_head free;
	struct vsp1_dl_list *active;
	struct vsp1_dl_list *queued;
	struct vsp1_dl_list *pending;

	struct work_struct gc_work;
	struct list_head gc_fragments;
};

/* -----------------------------------------------------------------------------
 * Display List Body Management
 */

/*
 * Initialize a display list body object and allocate DMA memory for the body
 * data. The display list body object is expected to have been initialized to
 * 0 when allocated.
 */
static int vsp1_dl_body_init(struct vsp1_device *vsp1,
			     struct vsp1_dl_body *dlb, unsigned int num_entries,
			     size_t extra_size)
{
	size_t size = num_entries * sizeof(*dlb->entries) + extra_size;

	dlb->vsp1 = vsp1;
	dlb->size = size;

	dlb->entries = dma_alloc_wc(vsp1->bus_master, dlb->size, &dlb->dma,
				    GFP_KERNEL);
	if (!dlb->entries)
		return -ENOMEM;

	return 0;
}

/*
 * Cleanup a display list body and free allocated DMA memory allocated.
 */
static void vsp1_dl_body_cleanup(struct vsp1_dl_body *dlb)
{
	dma_free_wc(dlb->vsp1->bus_master, dlb->size, dlb->entries, dlb->dma);
}

/**
 * vsp1_dl_fragment_alloc - Allocate a display list fragment
 * @vsp1: The VSP1 device
 * @num_entries: The maximum number of entries that the fragment can contain
 *
 * Allocate a display list fragment with enough memory to contain the requested
 * number of entries.
 *
 * Return a pointer to a fragment on success or NULL if memory can't be
 * allocated.
 */
struct vsp1_dl_body *vsp1_dl_fragment_alloc(struct vsp1_device *vsp1,
					    unsigned int num_entries)
{
	struct vsp1_dl_body *dlb;
	int ret;

	dlb = kzalloc(sizeof(*dlb), GFP_KERNEL);
	if (!dlb)
		return NULL;

	ret = vsp1_dl_body_init(vsp1, dlb, num_entries, 0);
	if (ret < 0) {
		kfree(dlb);
		return NULL;
	}

	return dlb;
}

/**
 * vsp1_dl_fragment_free - Free a display list fragment
 * @dlb: The fragment
 *
 * Free the given display list fragment and the associated DMA memory.
 *
 * Fragments must only be freed explicitly if they are not added to a display
 * list, as the display list will take ownership of them and free them
 * otherwise. Manual free typically happens at cleanup time for fragments that
 * have been allocated but not used.
 *
 * Passing a NULL pointer to this function is safe, in that case no operation
 * will be performed.
 */
void vsp1_dl_fragment_free(struct vsp1_dl_body *dlb)
{
	if (!dlb)
		return;

	vsp1_dl_body_cleanup(dlb);
	kfree(dlb);
}

/**
 * vsp1_dl_fragment_write - Write a register to a display list fragment
 * @dlb: The fragment
 * @reg: The register address
 * @data: The register value
 *
 * Write the given register and value to the display list fragment. The maximum
 * number of entries that can be written in a fragment is specified when the
 * fragment is allocated by vsp1_dl_fragment_alloc().
 */
void vsp1_dl_fragment_write(struct vsp1_dl_body *dlb, u32 reg, u32 data)
{
	dlb->entries[dlb->num_entries].addr = reg;
	dlb->entries[dlb->num_entries].data = data;
	dlb->num_entries++;
}

/* -----------------------------------------------------------------------------
 * Display List Transaction Management
 */

static struct vsp1_dl_list *vsp1_dl_list_alloc(struct vsp1_dl_manager *dlm)
{
	struct vsp1_dl_list *dl;
	size_t header_size;
	int ret;

	dl = kzalloc(sizeof(*dl), GFP_KERNEL);
	if (!dl)
		return NULL;

	INIT_LIST_HEAD(&dl->fragments);
	dl->dlm = dlm;

	/*
	 * Initialize the display list body and allocate DMA memory for the body
	 * and the optional header. Both are allocated together to avoid memory
	 * fragmentation, with the header located right after the body in
	 * memory.
	 */
	header_size = dlm->mode == VSP1_DL_MODE_HEADER
		    ? ALIGN(sizeof(struct vsp1_dl_header), 8)
		    : 0;

	ret = vsp1_dl_body_init(dlm->vsp1, &dl->body0, VSP1_DL_NUM_ENTRIES,
				header_size);
	if (ret < 0) {
		kfree(dl);
		return NULL;
	}

	if (dlm->mode == VSP1_DL_MODE_HEADER) {
		size_t header_offset = VSP1_DL_NUM_ENTRIES
				     * sizeof(*dl->body0.entries);

		dl->header = ((void *)dl->body0.entries) + header_offset;
		dl->dma = dl->body0.dma + header_offset;

		memset(dl->header, 0, sizeof(*dl->header));
		dl->header->lists[0].addr = dl->body0.dma;
	}

	return dl;
}

static void vsp1_dl_list_free(struct vsp1_dl_list *dl)
{
	vsp1_dl_body_cleanup(&dl->body0);
	list_splice_init(&dl->fragments, &dl->dlm->gc_fragments);
	kfree(dl);
}

/**
 * vsp1_dl_list_get - Get a free display list
 * @dlm: The display list manager
 *
 * Get a display list from the pool of free lists and return it.
 *
 * This function must be called without the display list manager lock held.
 */
struct vsp1_dl_list *vsp1_dl_list_get(struct vsp1_dl_manager *dlm)
{
	struct vsp1_dl_list *dl = NULL;
	unsigned long flags;

	spin_lock_irqsave(&dlm->lock, flags);

	if (!list_empty(&dlm->free)) {
		dl = list_first_entry(&dlm->free, struct vsp1_dl_list, list);
		list_del(&dl->list);

		/*
		 * The display list chain must be initialised to ensure every
		 * display list can assert list_empty() if it is not in a chain.
		 */
		INIT_LIST_HEAD(&dl->chain);
	}

	spin_unlock_irqrestore(&dlm->lock, flags);

	return dl;
}

/* This function must be called with the display list manager lock held.*/
static void __vsp1_dl_list_put(struct vsp1_dl_list *dl)
{
	struct vsp1_dl_list *dl_child;

	if (!dl)
		return;

	/*
	 * Release any linked display-lists which were chained for a single
	 * hardware operation.
	 */
	if (dl->has_chain) {
		list_for_each_entry(dl_child, &dl->chain, chain)
			__vsp1_dl_list_put(dl_child);
	}

	dl->has_chain = false;

	/*
	 * We can't free fragments here as DMA memory can only be freed in
	 * interruptible context. Move all fragments to the display list
	 * manager's list of fragments to be freed, they will be
	 * garbage-collected by the work queue.
	 */
	if (!list_empty(&dl->fragments)) {
		list_splice_init(&dl->fragments, &dl->dlm->gc_fragments);
		schedule_work(&dl->dlm->gc_work);
	}

	dl->body0.num_entries = 0;

	list_add_tail(&dl->list, &dl->dlm->free);
}

/**
 * vsp1_dl_list_put - Release a display list
 * @dl: The display list
 *
 * Release the display list and return it to the pool of free lists.
 *
 * Passing a NULL pointer to this function is safe, in that case no operation
 * will be performed.
 */
void vsp1_dl_list_put(struct vsp1_dl_list *dl)
{
	unsigned long flags;

	if (!dl)
		return;

	spin_lock_irqsave(&dl->dlm->lock, flags);
	__vsp1_dl_list_put(dl);
	spin_unlock_irqrestore(&dl->dlm->lock, flags);
}

/**
 * vsp1_dl_list_write - Write a register to the display list
 * @dl: The display list
 * @reg: The register address
 * @data: The register value
 *
 * Write the given register and value to the display list. Up to 256 registers
 * can be written per display list.
 */
void vsp1_dl_list_write(struct vsp1_dl_list *dl, u32 reg, u32 data)
{
	vsp1_dl_fragment_write(&dl->body0, reg, data);
}

/**
 * vsp1_dl_list_add_fragment - Add a fragment to the display list
 * @dl: The display list
 * @dlb: The fragment
 *
 * Add a display list body as a fragment to a display list. Registers contained
 * in fragments are processed after registers contained in the main display
 * list, in the order in which fragments are added.
 *
 * Adding a fragment to a display list passes ownership of the fragment to the
 * list. The caller must not touch the fragment after this call, and must not
 * free it explicitly with vsp1_dl_fragment_free().
 *
 * Fragments are only usable for display lists in header mode. Attempt to
 * add a fragment to a header-less display list will return an error.
 */
int vsp1_dl_list_add_fragment(struct vsp1_dl_list *dl,
			      struct vsp1_dl_body *dlb)
{
	/* Multi-body lists are only available in header mode. */
	if (dl->dlm->mode != VSP1_DL_MODE_HEADER)
		return -EINVAL;

	list_add_tail(&dlb->list, &dl->fragments);
	return 0;
}

/**
 * vsp1_dl_list_add_chain - Add a display list to a chain
 * @head: The head display list
 * @dl: The new display list
 *
 * Add a display list to an existing display list chain. The chained lists
 * will be automatically processed by the hardware without intervention from
 * the CPU. A display list end interrupt will only complete after the last
 * display list in the chain has completed processing.
 *
 * Adding a display list to a chain passes ownership of the display list to
 * the head display list item. The chain is released when the head dl item is
 * put back with __vsp1_dl_list_put().
 *
 * Chained display lists are only usable in header mode. Attempts to add a
 * display list to a chain in header-less mode will return an error.
 */
int vsp1_dl_list_add_chain(struct vsp1_dl_list *head,
			   struct vsp1_dl_list *dl)
{
	/* Chained lists are only available in header mode. */
	if (head->dlm->mode != VSP1_DL_MODE_HEADER)
		return -EINVAL;

	head->has_chain = true;
	list_add_tail(&dl->chain, &head->chain);
	return 0;
}

static void vsp1_dl_list_fill_header(struct vsp1_dl_list *dl, bool is_last)
{
	struct vsp1_dl_manager *dlm = dl->dlm;
	struct vsp1_dl_header_list *hdr = dl->header->lists;
	struct vsp1_dl_body *dlb;
	unsigned int num_lists = 0;

	/*
	 * Fill the header with the display list bodies addresses and sizes. The
	 * address of the first body has already been filled when the display
	 * list was allocated.
	 */

	hdr->num_bytes = dl->body0.num_entries
		       * sizeof(*dl->header->lists);

	list_for_each_entry(dlb, &dl->fragments, list) {
		num_lists++;
		hdr++;

		hdr->addr = dlb->dma;
		hdr->num_bytes = dlb->num_entries
			       * sizeof(*dl->header->lists);
	}

	dl->header->num_lists = num_lists;

	if (!list_empty(&dl->chain) && !is_last) {
		/*
		 * If this display list's chain is not empty, we are on a list,
		 * and the next item is the display list that we must queue for
		 * automatic processing by the hardware.
		 */
		struct vsp1_dl_list *next = list_next_entry(dl, chain);

		dl->header->next_header = next->dma;
		dl->header->flags = VSP1_DLH_AUTO_START;
	} else if (!dlm->singleshot) {
		/*
		 * if the display list manager works in continuous mode, the VSP
		 * should loop over the display list continuously until
		 * instructed to do otherwise.
		 */
		dl->header->next_header = dl->dma;
		dl->header->flags = VSP1_DLH_INT_ENABLE | VSP1_DLH_AUTO_START;
	} else {
		/*
		 * Otherwise, in mem-to-mem mode, we work in single-shot mode
		 * and the next display list must not be started automatically.
		 */
		dl->header->flags = VSP1_DLH_INT_ENABLE;
	}
}

static bool vsp1_dl_list_hw_update_pending(struct vsp1_dl_manager *dlm)
{
	struct vsp1_device *vsp1 = dlm->vsp1;

	if (!dlm->queued)
		return false;

	/*
	 * Check whether the VSP1 has taken the update. In headerless mode the
	 * hardware indicates this by clearing the UPD bit in the DL_BODY_SIZE
	 * register, and in header mode by clearing the UPDHDR bit in the CMD
	 * register.
	 */
	if (dlm->mode == VSP1_DL_MODE_HEADERLESS)
		return !!(vsp1_read(vsp1, VI6_DL_BODY_SIZE)
			  & VI6_DL_BODY_SIZE_UPD);
	else
		return !!(vsp1_read(vsp1, VI6_CMD(dlm->index))
			  & VI6_CMD_UPDHDR);
}

static void vsp1_dl_list_hw_enqueue(struct vsp1_dl_list *dl)
{
	struct vsp1_dl_manager *dlm = dl->dlm;
	struct vsp1_device *vsp1 = dlm->vsp1;

	if (dlm->mode == VSP1_DL_MODE_HEADERLESS) {
		/*
		 * In headerless mode, program the hardware directly with the
		 * display list body address and size and set the UPD bit. The
		 * bit will be cleared by the hardware when the display list
		 * processing starts.
		 */
		vsp1_write(vsp1, VI6_DL_HDR_ADDR(0), dl->body0.dma);
		vsp1_write(vsp1, VI6_DL_BODY_SIZE, VI6_DL_BODY_SIZE_UPD |
			   (dl->body0.num_entries * sizeof(*dl->header->lists)));
	} else {
		/*
		 * In header mode, program the display list header address. If
		 * the hardware is idle (single-shot mode or first frame in
		 * continuous mode) it will then be started independently. If
		 * the hardware is operating, the VI6_DL_HDR_REF_ADDR register
		 * will be updated with the display list address.
		 */
		vsp1_write(vsp1, VI6_DL_HDR_ADDR(dlm->index), dl->dma);
	}
}

static void vsp1_dl_list_commit_continuous(struct vsp1_dl_list *dl)
{
	struct vsp1_dl_manager *dlm = dl->dlm;

	/*
	 * If a previous display list has been queued to the hardware but not
	 * processed yet, the VSP can start processing it at any time. In that
	 * case we can't replace the queued list by the new one, as we could
	 * race with the hardware. We thus mark the update as pending, it will
	 * be queued up to the hardware by the frame end interrupt handler.
	 *
	 * If a display list is already pending we simply drop it as the new
	 * display list is assumed to contain a more recent configuration. It is
	 * an error if the already pending list has the internal flag set, as
	 * there is then a process waiting for that list to complete. This
	 * shouldn't happen as the waiting process should perform proper
	 * locking, but warn just in case.
	 */
	if (vsp1_dl_list_hw_update_pending(dlm)) {
		WARN_ON(dlm->pending && dlm->pending->internal);
		__vsp1_dl_list_put(dlm->pending);
		dlm->pending = dl;
		return;
	}

	/*
	 * Pass the new display list to the hardware and mark it as queued. It
	 * will become active when the hardware starts processing it.
	 */
	vsp1_dl_list_hw_enqueue(dl);

	__vsp1_dl_list_put(dlm->queued);
	dlm->queued = dl;
}

static void vsp1_dl_list_commit_singleshot(struct vsp1_dl_list *dl)
{
	struct vsp1_dl_manager *dlm = dl->dlm;

	/*
	 * When working in single-shot mode, the caller guarantees that the
	 * hardware is idle at this point. Just commit the head display list
	 * to hardware. Chained lists will be started automatically.
	 */
	vsp1_dl_list_hw_enqueue(dl);

	dlm->active = dl;
}

void vsp1_dl_list_commit(struct vsp1_dl_list *dl, bool internal)
{
	struct vsp1_dl_manager *dlm = dl->dlm;
	struct vsp1_dl_list *dl_child;
	unsigned long flags;

	if (dlm->mode == VSP1_DL_MODE_HEADER) {
		/* Fill the header for the head and chained display lists. */
		vsp1_dl_list_fill_header(dl, list_empty(&dl->chain));

		list_for_each_entry(dl_child, &dl->chain, chain) {
			bool last = list_is_last(&dl_child->chain, &dl->chain);

			vsp1_dl_list_fill_header(dl_child, last);
		}
	}

	dl->internal = internal;

	spin_lock_irqsave(&dlm->lock, flags);

	if (dlm->singleshot)
		vsp1_dl_list_commit_singleshot(dl);
	else
		vsp1_dl_list_commit_continuous(dl);

	spin_unlock_irqrestore(&dlm->lock, flags);
}

/* -----------------------------------------------------------------------------
 * Display List Manager
 */

/**
 * vsp1_dlm_irq_frame_end - Display list handler for the frame end interrupt
 * @dlm: the display list manager
 *
 * Return a set of flags that indicates display list completion status.
 *
 * The VSP1_DL_FRAME_END_COMPLETED flag indicates that the previous display list
 * has completed at frame end. If the flag is not returned display list
 * completion has been delayed by one frame because the display list commit
 * raced with the frame end interrupt. The function always returns with the flag
 * set in header mode as display list processing is then not continuous and
 * races never occur.
 *
 * The VSP1_DL_FRAME_END_INTERNAL flag indicates that the previous display list
 * has completed and had been queued with the internal notification flag.
 * Internal notification is only supported for continuous mode.
 */
unsigned int vsp1_dlm_irq_frame_end(struct vsp1_dl_manager *dlm)
{
	unsigned int flags = 0;

	spin_lock(&dlm->lock);

	/*
	 * The mem-to-mem pipelines work in single-shot mode. No new display
	 * list can be queued, we don't have to do anything.
	 */
	if (dlm->singleshot) {
		__vsp1_dl_list_put(dlm->active);
		dlm->active = NULL;
		flags |= VSP1_DL_FRAME_END_COMPLETED;
		goto done;
	}

	/*
	 * If the commit operation raced with the interrupt and occurred after
	 * the frame end event but before interrupt processing, the hardware
	 * hasn't taken the update into account yet. We have to skip one frame
	 * and retry.
	 */
	if (vsp1_dl_list_hw_update_pending(dlm))
		goto done;

	/*
	 * The device starts processing the queued display list right after the
	 * frame end interrupt. The display list thus becomes active.
	 */
	if (dlm->queued) {
		if (dlm->queued->internal)
			flags |= VSP1_DL_FRAME_END_INTERNAL;
		dlm->queued->internal = false;

		__vsp1_dl_list_put(dlm->active);
		dlm->active = dlm->queued;
		dlm->queued = NULL;
		flags |= VSP1_DL_FRAME_END_COMPLETED;
	}

	/*
	 * Now that the VSP has started processing the queued display list, we
	 * can queue the pending display list to the hardware if one has been
	 * prepared.
	 */
	if (dlm->pending) {
		vsp1_dl_list_hw_enqueue(dlm->pending);
		dlm->queued = dlm->pending;
		dlm->pending = NULL;
	}

done:
	spin_unlock(&dlm->lock);

	return flags;
}

/* Hardware Setup */
void vsp1_dlm_setup(struct vsp1_device *vsp1)
{
	u32 ctrl = (256 << VI6_DL_CTRL_AR_WAIT_SHIFT)
		 | VI6_DL_CTRL_DC2 | VI6_DL_CTRL_DC1 | VI6_DL_CTRL_DC0
		 | VI6_DL_CTRL_DLE;

	/*
	 * The DRM pipeline operates with display lists in Continuous Frame
	 * Mode, all other pipelines use manual start.
	 */
	if (vsp1->drm)
		ctrl |= VI6_DL_CTRL_CFM0 | VI6_DL_CTRL_NH0;

	vsp1_write(vsp1, VI6_DL_CTRL, ctrl);
	vsp1_write(vsp1, VI6_DL_SWAP, VI6_DL_SWAP_LWS);
}

void vsp1_dlm_reset(struct vsp1_dl_manager *dlm)
{
	unsigned long flags;

	spin_lock_irqsave(&dlm->lock, flags);

	__vsp1_dl_list_put(dlm->active);
	__vsp1_dl_list_put(dlm->queued);
	__vsp1_dl_list_put(dlm->pending);

	spin_unlock_irqrestore(&dlm->lock, flags);

	dlm->active = NULL;
	dlm->queued = NULL;
	dlm->pending = NULL;
}

/*
 * Free all fragments awaiting to be garbage-collected.
 *
 * This function must be called without the display list manager lock held.
 */
static void vsp1_dlm_fragments_free(struct vsp1_dl_manager *dlm)
{
	unsigned long flags;

	spin_lock_irqsave(&dlm->lock, flags);

	while (!list_empty(&dlm->gc_fragments)) {
		struct vsp1_dl_body *dlb;

		dlb = list_first_entry(&dlm->gc_fragments, struct vsp1_dl_body,
				       list);
		list_del(&dlb->list);

		spin_unlock_irqrestore(&dlm->lock, flags);
		vsp1_dl_fragment_free(dlb);
		spin_lock_irqsave(&dlm->lock, flags);
	}

	spin_unlock_irqrestore(&dlm->lock, flags);
}

static void vsp1_dlm_garbage_collect(struct work_struct *work)
{
	struct vsp1_dl_manager *dlm =
		container_of(work, struct vsp1_dl_manager, gc_work);

	vsp1_dlm_fragments_free(dlm);
}

struct vsp1_dl_manager *vsp1_dlm_create(struct vsp1_device *vsp1,
					unsigned int index,
					unsigned int prealloc)
{
	struct vsp1_dl_manager *dlm;
	unsigned int i;

	dlm = devm_kzalloc(vsp1->dev, sizeof(*dlm), GFP_KERNEL);
	if (!dlm)
		return NULL;

	dlm->index = index;
	dlm->mode = index == 0 && !vsp1->info->uapi
		  ? VSP1_DL_MODE_HEADERLESS : VSP1_DL_MODE_HEADER;
	dlm->singleshot = vsp1->info->uapi;
	dlm->vsp1 = vsp1;

	spin_lock_init(&dlm->lock);
	INIT_LIST_HEAD(&dlm->free);
	INIT_LIST_HEAD(&dlm->gc_fragments);
	INIT_WORK(&dlm->gc_work, vsp1_dlm_garbage_collect);

	for (i = 0; i < prealloc; ++i) {
		struct vsp1_dl_list *dl;

		dl = vsp1_dl_list_alloc(dlm);
		if (!dl)
			return NULL;

		list_add_tail(&dl->list, &dlm->free);
	}

	return dlm;
}

void vsp1_dlm_destroy(struct vsp1_dl_manager *dlm)
{
	struct vsp1_dl_list *dl, *next;

	if (!dlm)
		return;

	cancel_work_sync(&dlm->gc_work);

	list_for_each_entry_safe(dl, next, &dlm->free, list) {
		list_del(&dl->list);
		vsp1_dl_list_free(dl);
	}

	vsp1_dlm_fragments_free(dlm);
}
