/*
 * buffers.h
 *
 * Copyright (C) 2009 - Pierre-Marc Fournier (pierre-marc dot fournier at polymtl dot ca)
 * Copyright (C) 2008 - Mathieu Desnoyers (mathieu.desnoyers@polymtl.ca)
 *
 */

#ifndef _UST_BUFFERS_H
#define _UST_BUFFERS_H

#include <kcompat/kref.h>
#include <assert.h>
#include "channels.h"
#include "tracerconst.h"
#include "tracercore.h"
#include "header-inline.h"
#include <usterr.h>

/***** SHOULD BE REMOVED ***** */

/*
 * BUFFER_TRUNC zeroes the subbuffer offset and the subbuffer number parts of
 * the offset, which leaves only the buffer number.
 */
#define BUFFER_TRUNC(offset, chan) \
	((offset) & (~((chan)->alloc_size-1)))
#define BUFFER_OFFSET(offset, chan) ((offset) & ((chan)->alloc_size - 1))
#define SUBBUF_OFFSET(offset, chan) ((offset) & ((chan)->subbuf_size - 1))
#define SUBBUF_ALIGN(offset, chan) \
	(((offset) + (chan)->subbuf_size) & (~((chan)->subbuf_size - 1)))
#define SUBBUF_TRUNC(offset, chan) \
	((offset) & (~((chan)->subbuf_size - 1)))
#define SUBBUF_INDEX(offset, chan) \
	(BUFFER_OFFSET((offset), chan) >> (chan)->subbuf_size_order)

/*
 * Tracks changes to rchan/rchan_buf structs
 */
#define UST_CHANNEL_VERSION		8

/**************************************/

struct commit_counters {
	local_t cc;
	local_t cc_sb;			/* Incremented _once_ at sb switch */
};

struct ust_buffer {
	/* First 32 bytes cache-hot cacheline */
	local_t offset;			/* Current offset in the buffer */
	struct commit_counters *commit_count;	/* Commit count per sub-buffer */
	atomic_long_t consumed;		/*
					 * Current offset in the buffer
					 * standard atomic access (shared)
					 */
	unsigned long last_tsc;		/*
					 * Last timestamp written in the buffer.
					 */
	/* End of first 32 bytes cacheline */
	atomic_long_t active_readers;	/*
					 * Active readers count
					 * standard atomic access (shared)
					 */
	local_t events_lost;
	local_t corrupted_subbuffers;
	/* one byte is written to this pipe when data is available, in order
           to wake the consumer */
	/* portability: Single byte writes must be as quick as possible. The kernel-side
	   buffer must be large enough so the writer doesn't block. From the pipe(7)
           man page: Since linux 2.6.11, the pipe capacity is 65536 bytes. */
	int data_ready_fd_write;
	/* the reading end of the pipe */
	int data_ready_fd_read;

	unsigned int finalized;
//ust//	struct timer_list switch_timer; /* timer for periodical switch */
	unsigned long switch_timer_interval; /* 0 = unset */

	struct ust_channel *chan;

	struct kref kref;
	void *buf_data;
	size_t buf_size;
	int shmid;
	unsigned int cpu;

	/* commit count per subbuffer; must be at end of struct */
	local_t commit_seq[0] ____cacheline_aligned;
} ____cacheline_aligned;

/*
 * A switch is done during tracing or as a final flush after tracing (so it
 * won't write in the new sub-buffer).
 * FIXME: make this message clearer
 */
enum force_switch_mode { FORCE_ACTIVE, FORCE_FLUSH };

extern int ltt_reserve_slot_lockless_slow(struct ust_trace *trace,
		struct ust_channel *ltt_channel, void **transport_data,
		size_t data_size, size_t *slot_size, long *buf_offset, u64 *tsc,
		unsigned int *rflags, int largest_align, int cpu);

extern void ltt_force_switch_lockless_slow(struct ust_buffer *buf,
		enum force_switch_mode mode);


static __inline__ void ust_buffers_do_copy(void *dest, const void *src, size_t len)
{
	union {
		const void *src;
		const u8 *src8;
		const u16 *src16;
		const u32 *src32;
		const u64 *src64;
	} u = { .src = src };

	switch (len) {
	case 0: break;
	case 1: *(u8 *)dest = *u.src8;
		break;
	case 2: *(u16 *)dest = *u.src16;
		break;
	case 4: *(u32 *)dest = *u.src32;
		break;
	case 8: *(u64 *)dest = *u.src64;
		break;
	default:
		memcpy(dest, src, len);
	}
}

static __inline__ void *ust_buffers_offset_address(struct ust_buffer *buf, size_t offset)
{
	return ((char *)buf->buf_data)+offset;
}

/*
 * Last TSC comparison functions. Check if the current TSC overflows
 * LTT_TSC_BITS bits from the last TSC read. Reads and writes last_tsc
 * atomically.
 */

/* FIXME: does this test work properly? */
#if (BITS_PER_LONG == 32)
static __inline__ void save_last_tsc(struct ust_buffer *ltt_buf,
					u64 tsc)
{
	ltt_buf->last_tsc = (unsigned long)(tsc >> LTT_TSC_BITS);
}

static __inline__ int last_tsc_overflow(struct ust_buffer *ltt_buf,
					u64 tsc)
{
	unsigned long tsc_shifted = (unsigned long)(tsc >> LTT_TSC_BITS);

	if (unlikely((tsc_shifted - ltt_buf->last_tsc)))
		return 1;
	else
		return 0;
}
#else
static __inline__ void save_last_tsc(struct ust_buffer *ltt_buf,
					u64 tsc)
{
	ltt_buf->last_tsc = (unsigned long)tsc;
}

static __inline__ int last_tsc_overflow(struct ust_buffer *ltt_buf,
					u64 tsc)
{
	if (unlikely((tsc - ltt_buf->last_tsc) >> LTT_TSC_BITS))
		return 1;
	else
		return 0;
}
#endif

static __inline__ void ltt_reserve_push_reader(
		struct ust_channel *rchan,
		struct ust_buffer *buf,
		long offset)
{
	long consumed_old, consumed_new;

	do {
		consumed_old = atomic_long_read(&buf->consumed);
		/*
		 * If buffer is in overwrite mode, push the reader consumed
		 * count if the write position has reached it and we are not
		 * at the first iteration (don't push the reader farther than
		 * the writer). This operation can be done concurrently by many
		 * writers in the same buffer, the writer being at the farthest
		 * write position sub-buffer index in the buffer being the one
		 * which will win this loop.
		 * If the buffer is not in overwrite mode, pushing the reader
		 * only happens if a sub-buffer is corrupted.
		 */
		if (unlikely((SUBBUF_TRUNC(offset, buf->chan)
		   - SUBBUF_TRUNC(consumed_old, buf->chan))
		   >= rchan->alloc_size))
			consumed_new = SUBBUF_ALIGN(consumed_old, buf->chan);
		else
			return;
	} while (unlikely(atomic_long_cmpxchg(&buf->consumed, consumed_old,
			consumed_new) != consumed_old));
}

static __inline__ void ltt_vmcore_check_deliver(
		struct ust_buffer *buf,
		long commit_count, long idx)
{
	local_set(&buf->commit_seq[idx], commit_count);
}

static __inline__ void ltt_check_deliver(struct ust_channel *chan,
		struct ust_buffer *buf,
		long offset, long commit_count, long idx)
{
	long old_commit_count = commit_count - chan->subbuf_size;

	/* Check if all commits have been done */
	if (unlikely((BUFFER_TRUNC(offset, chan)
			>> chan->n_subbufs_order)
			- (old_commit_count
			   & chan->commit_count_mask) == 0)) {
		/*
		 * If we succeeded in updating the cc_sb, we are delivering
		 * the subbuffer. Deals with concurrent updates of the "cc"
		 * value without adding a add_return atomic operation to the
		 * fast path.
		 */
		if (likely(local_cmpxchg(&buf->commit_count[idx].cc_sb,
					 old_commit_count, commit_count)
			   == old_commit_count)) {
			int result;

			/*
			 * Set noref flag for this subbuffer.
			 */
//ust//			ltt_set_noref_flag(rchan, buf, idx);
			ltt_vmcore_check_deliver(buf, commit_count, idx);

			/* wakeup consumer */
			result = write(buf->data_ready_fd_write, "1", 1);
			if(result == -1) {
				PERROR("write (in ltt_relay_buffer_flush)");
				ERR("this should never happen!");
			}
		}
	}
}

static __inline__ int ltt_poll_deliver(struct ust_channel *chan, struct ust_buffer *buf)
{
	long consumed_old, consumed_idx, commit_count, write_offset;

	consumed_old = atomic_long_read(&buf->consumed);
	consumed_idx = SUBBUF_INDEX(consumed_old, buf->chan);
	commit_count = local_read(&buf->commit_count[consumed_idx].cc_sb);
	/*
	 * No memory barrier here, since we are only interested
	 * in a statistically correct polling result. The next poll will
	 * get the data is we are racing. The mb() that ensures correct
	 * memory order is in get_subbuf.
	 */
	write_offset = local_read(&buf->offset);

	/*
	 * Check that the subbuffer we are trying to consume has been
	 * already fully committed.
	 */

	if (((commit_count - chan->subbuf_size)
	     & chan->commit_count_mask)
	    - (BUFFER_TRUNC(consumed_old, buf->chan)
	       >> chan->n_subbufs_order)
	    != 0)
		return 0;

	/*
	 * Check that we are not about to read the same subbuffer in
	 * which the writer head is.
	 */
	if ((SUBBUF_TRUNC(write_offset, buf->chan)
	   - SUBBUF_TRUNC(consumed_old, buf->chan))
	   == 0)
		return 0;

	return 1;

}

/*
 * returns 0 if reserve ok, or 1 if the slow path must be taken.
 */
static __inline__ int ltt_relay_try_reserve(
		struct ust_channel *chan,
		struct ust_buffer *buf,
		size_t data_size,
		u64 *tsc, unsigned int *rflags, int largest_align,
		long *o_begin, long *o_end, long *o_old,
		size_t *before_hdr_pad, size_t *size)
{
	*o_begin = local_read(&buf->offset);
	*o_old = *o_begin;

	*tsc = trace_clock_read64();

//ust// #ifdef CONFIG_LTT_VMCORE
//ust// 	prefetch(&buf->commit_count[SUBBUF_INDEX(*o_begin, rchan)]);
//ust// 	prefetch(&buf->commit_seq[SUBBUF_INDEX(*o_begin, rchan)]);
//ust// #else
//ust// 	prefetchw(&buf->commit_count[SUBBUF_INDEX(*o_begin, rchan)]);
//ust// #endif
	if (last_tsc_overflow(buf, *tsc))
		*rflags = LTT_RFLAG_ID_SIZE_TSC;

	if (unlikely(SUBBUF_OFFSET(*o_begin, buf->chan) == 0))
		return 1;

	*size = ust_get_header_size(chan,
				*o_begin, data_size,
				before_hdr_pad, *rflags);
	*size += ltt_align(*o_begin + *size, largest_align) + data_size;
	if (unlikely((SUBBUF_OFFSET(*o_begin, buf->chan) + *size)
		     > buf->chan->subbuf_size))
		return 1;

	/*
	 * Event fits in the current buffer and we are not on a switch
	 * boundary. It's safe to write.
	 */
	*o_end = *o_begin + *size;

	if (unlikely((SUBBUF_OFFSET(*o_end, buf->chan)) == 0))
		/*
		 * The offset_end will fall at the very beginning of the next
		 * subbuffer.
		 */
		return 1;

	return 0;
}

static __inline__ int ltt_reserve_slot(struct ust_trace *trace,
		struct ust_channel *chan, void **transport_data,
		size_t data_size, size_t *slot_size, long *buf_offset, u64 *tsc,
		unsigned int *rflags, int largest_align, int cpu)
{
	struct ust_buffer *buf = chan->buf[cpu];
	long o_begin, o_end, o_old;
	size_t before_hdr_pad;

	/*
	 * Perform retryable operations.
	 */
	/* FIXME: make this rellay per cpu? */
	if (unlikely(__get_cpu_var(ltt_nesting) > 4)) {
		local_inc(&buf->events_lost);
		return -EPERM;
	}

	if (unlikely(ltt_relay_try_reserve(chan, buf,
			data_size, tsc, rflags,
			largest_align, &o_begin, &o_end, &o_old,
			&before_hdr_pad, slot_size)))
		goto slow_path;

	if (unlikely(local_cmpxchg(&buf->offset, o_old, o_end) != o_old))
		goto slow_path;

	/*
	 * Atomically update last_tsc. This update races against concurrent
	 * atomic updates, but the race will always cause supplementary full TSC
	 * events, never the opposite (missing a full TSC event when it would be
	 * needed).
	 */
	save_last_tsc(buf, *tsc);

	/*
	 * Push the reader if necessary
	 */
	ltt_reserve_push_reader(chan, buf, o_end - 1);

	/*
	 * Clear noref flag for this subbuffer.
	 */
//ust//	ltt_clear_noref_flag(chan, buf, SUBBUF_INDEX(o_end - 1, chan));

	*buf_offset = o_begin + before_hdr_pad;
	return 0;
slow_path:
	return ltt_reserve_slot_lockless_slow(trace, chan,
		transport_data, data_size, slot_size, buf_offset, tsc,
		rflags, largest_align, cpu);
}

/*
 * Force a sub-buffer switch for a per-cpu buffer. This operation is
 * completely reentrant : can be called while tracing is active with
 * absolutely no lock held.
 *
 * Note, however, that as a local_cmpxchg is used for some atomic
 * operations, this function must be called from the CPU which owns the buffer
 * for a ACTIVE flush.
 */
static __inline__ void ltt_force_switch(struct ust_buffer *buf,
		enum force_switch_mode mode)
{
	return ltt_force_switch_lockless_slow(buf, mode);
}

/*
 * for flight recording. must be called after relay_commit.
 * This function increments the subbuffers's commit_seq counter each time the
 * commit count reaches back the reserve offset (module subbuffer size). It is
 * useful for crash dump.
 */
#ifdef CONFIG_LTT_VMCORE
static __inline__ void ltt_write_commit_counter(struct rchan_buf *buf,
		struct ltt_channel_buf_struct *ltt_buf,
		long idx, long buf_offset, long commit_count, size_t data_size)
{
	long offset;
	long commit_seq_old;

	offset = buf_offset + data_size;

	/*
	 * SUBBUF_OFFSET includes commit_count_mask. We can simply
	 * compare the offsets within the subbuffer without caring about
	 * buffer full/empty mismatch because offset is never zero here
	 * (subbuffer header and event headers have non-zero length).
	 */
	if (unlikely(SUBBUF_OFFSET(offset - commit_count, buf->chan)))
		return;

	commit_seq_old = local_read(&ltt_buf->commit_seq[idx]);
	while (commit_seq_old < commit_count)
		commit_seq_old = local_cmpxchg(&ltt_buf->commit_seq[idx],
					 commit_seq_old, commit_count);
}
#else
static __inline__ void ltt_write_commit_counter(struct ust_buffer *buf,
		long idx, long buf_offset, long commit_count, size_t data_size)
{
}
#endif

/*
 * Atomic unordered slot commit. Increments the commit count in the
 * specified sub-buffer, and delivers it if necessary.
 *
 * Parameters:
 *
 * @ltt_channel : channel structure
 * @transport_data: transport-specific data
 * @buf_offset : offset following the event header.
 * @data_size : size of the event data.
 * @slot_size : size of the reserved slot.
 */
static __inline__ void ltt_commit_slot(
		struct ust_channel *chan,
		struct ust_buffer *buf, long buf_offset,
		size_t data_size, size_t slot_size)
{
	long offset_end = buf_offset;
	long endidx = SUBBUF_INDEX(offset_end - 1, chan);
	long commit_count;

#ifdef LTT_NO_IPI_BARRIER
	smp_wmb();
#else
	/*
	 * Must write slot data before incrementing commit count.
	 * This compiler barrier is upgraded into a smp_mb() by the IPI
	 * sent by get_subbuf().
	 */
	barrier();
#endif
	local_add(slot_size, &buf->commit_count[endidx].cc);
	/*
	 * commit count read can race with concurrent OOO commit count updates.
	 * This is only needed for ltt_check_deliver (for non-polling delivery
	 * only) and for ltt_write_commit_counter. The race can only cause the
	 * counter to be read with the same value more than once, which could
	 * cause :
	 * - Multiple delivery for the same sub-buffer (which is handled
	 *   gracefully by the reader code) if the value is for a full
	 *   sub-buffer. It's important that we can never miss a sub-buffer
	 *   delivery. Re-reading the value after the local_add ensures this.
	 * - Reading a commit_count with a higher value that what was actually
	 *   added to it for the ltt_write_commit_counter call (again caused by
	 *   a concurrent committer). It does not matter, because this function
	 *   is interested in the fact that the commit count reaches back the
	 *   reserve offset for a specific sub-buffer, which is completely
	 *   independent of the order.
	 */
	commit_count = local_read(&buf->commit_count[endidx].cc);

	ltt_check_deliver(chan, buf, offset_end - 1, commit_count, endidx);
	/*
	 * Update data_size for each commit. It's needed only for extracting
	 * ltt buffers from vmcore, after crash.
	 */
	ltt_write_commit_counter(buf, endidx, buf_offset, commit_count, data_size);
}

void _ust_buffers_write(struct ust_buffer *buf, size_t offset,
        const void *src, size_t len, ssize_t cpy);

static __inline__ int ust_buffers_write(struct ust_buffer *buf, size_t offset,
        const void *src, size_t len)
{
	size_t cpy;
	size_t buf_offset = BUFFER_OFFSET(offset, buf->chan);

	assert(buf_offset < buf->chan->subbuf_size*buf->chan->subbuf_cnt);

	cpy = min_t(size_t, len, buf->buf_size - buf_offset);
	ust_buffers_do_copy(buf->buf_data + buf_offset, src, cpy);

	if (unlikely(len != cpy))
		_ust_buffers_write(buf, buf_offset, src, len, cpy);
	return len;
}

int ust_buffers_get_subbuf(struct ust_buffer *buf, long *consumed);
int ust_buffers_put_subbuf(struct ust_buffer *buf, unsigned long uconsumed_old);

#endif /* _UST_BUFFERS_H */