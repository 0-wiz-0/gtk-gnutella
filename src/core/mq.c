/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @file
 *
 * Message queues, common code between TCP and UDP sending stacks.
 */

#include "common.h"

RCSID("$Id$");

#include "nodes.h"
#include "mq.h"
#include "pmsg.h"
#include "gmsg.h"
#include "gnet_stats.h"

#include "lib/walloc.h"
#include "lib/cq.h"

#include "if/gnet_property_priv.h"

#include "lib/override.h"		/* Must be the last header included */

static void qlink_free(mqueue_t *q);
static void mq_update_flowc(mqueue_t *q);
static gboolean make_room_header(
	mqueue_t *q, gchar *header, guint prio, gint needed, gint *offset);
static void mq_swift_timer(cqueue_t *cq, gpointer obj);

/*
 * Polymorphic operations.
 */

#define MQ_PUTQ(o,m)		((o)->ops->putq((o), (m)))

/**
 * Free queue and all enqueued messages.
 *
 * Since the message queue is the top of the network TX stack,
 * calling mq_free() recursively requests freeing to lower layers.
 */
void
mq_free(mqueue_t *q)
{
	GList *l;
	gint n;

	tx_free(q->tx_drv);		/* Get rid of lower layers */

	for (n = 0, l = q->qhead; l; l = g_list_next(l)) {
		n++;
		pmsg_free((pmsg_t *) l->data);
	}

	g_assert(n == q->count);

	if (q->qlink)
		qlink_free(q);

	if (q->swift_ev)
		cq_cancel(callout_queue, q->swift_ev);

	g_list_free(q->qhead);
	wfree(q, sizeof(*q));
}

/**
 * Remove link from message queue and return the previous item.
 * The `size' parameter refers to the size of the removed message.
 *
 * The underlying message is freed and the size information on the
 * queue is updated, but not the flow-control information.
 */
static GList *
mq_rmlink_prev(mqueue_t *q, GList *l, gint size)
{
	GList *prev = g_list_previous(l);

	q->qhead = g_list_remove_link(q->qhead, l);
	if (q->qtail == l)
		q->qtail = prev;
	q->size -= size;
	q->count--;

	pmsg_free((pmsg_t *) l->data);
	g_list_free_1(l);

	return prev;
}

/**
 * A "swift" checkpoint was reached.
 */
static void
mq_swift_checkpoint(mqueue_t *q, gboolean initial)
{
	gint elapsed = q->swift_elapsed;	/* Elapsed since we were scheduled */
	gint target_to_lowmark;
	gint flushed_till_next_timer;
	gint added_till_next_timer;
	gfloat period_ratio;
	gint added;
	gint needed;
	gint extra;

	g_assert(q->flags & MQ_FLOWC);
	g_assert(q->size > q->lowat);	/* Or we would have left FC */

	q->swift_ev = NULL;				/* Event fired, we may not reinstall it */

	/*
	 * For next period, the elapsed time will be...
	 */

	q->swift_elapsed = node_flowc_swift_period(q->node) * 1000;

	/*
	 * Compute target to reach the low watermark, and then the amount we
	 * will have flushed by the time we reach the next timer, at the
	 * present TX rate, as well as the data that will have been added to
	 * the queue.
	 */

	period_ratio = (gfloat) q->swift_elapsed / (gfloat) elapsed;
	target_to_lowmark = q->size - q->lowat;
	added = q->size - q->last_size + q->flowc_written;

	flushed_till_next_timer = (gint) (q->flowc_written * period_ratio);
	added_till_next_timer = added <= 0 ? 0 : (gint) (added * period_ratio);

	/*
	 * Now compute the amount of bytes we need to forcefully drop to be
	 * able to leave flow-control when the next timer fires...
	 */

	extra = target_to_lowmark -
		(flushed_till_next_timer - added_till_next_timer);

	if (extra <= 0) {
		/*
		 * We should be able to fully flush the queue by next timer at the
		 * present average fill and flushing rates.  So needed could be 0.
		 * However, to account for the bursty nature of the traffic,
		 * take a margin...
		 */

		needed = target_to_lowmark / 3;
	} else {
		/*
		 * We won't be able to reach the low watermark at the present rates.
		 * We need to remove the extra traffic present in the queue, plus
		 * add a margin: we assume we'll only be able to flush 75% of what
		 * we flushed currently.
		 */

		needed = extra + flushed_till_next_timer / 4;
	}

	if (initial) {
		/*
		 * First time we're in "swift" mode.
		 *
		 * Purge pending queries, since they are getting quite old.
		 * Leave our queries in for now (they have hops=0).
		 */

		q->header.function = GTA_MSG_SEARCH;
		q->header.hops = 1;
		q->header.ttl = max_ttl;

		if (needed > 0)
			make_room_header(q, (gchar*) &q->header, PMSG_P_DATA, needed, NULL);

		/*
		 * Whether or not we were able to make enough room at this point
		 * is not important, for the initial checkpoint.  Indeed, since
		 * we are now in "swift" mode, more query messages will be dropped
		 * at the next iteration, since we'll start dropping query hits,
		 * and hits are more prioritary than queries.
		 */

	} else {
		gint ttl;

		/*
		 * We're going to drop query hits...
		 *
		 * We start with the lowest prioritary query hit: low hops count
		 * and high TTL, and we progressively increase until we can drop
		 * the amount we need to drop.
		 *
		 * Note that we will never be able to drop the partially written
		 * message at the tail of the queue, even if it is less prioritary
		 * than our comparison point.
		 */

		q->header.function = GTA_MSG_SEARCH_RESULTS;

		/*
		 * Loop until we reach hops=hard_ttl_limit or we have finished
		 * removing enough data from the queue.
		 */

		for (ttl = hard_ttl_limit; needed > 0 && ttl >= 0; ttl--) {
			gint old_size = q->size;

			q->header.hops = hard_ttl_limit - ttl;
			q->header.ttl = ttl;

			if (
				make_room_header(q, (gchar*) &q->header, PMSG_P_DATA,
					needed, NULL)
			)
				break;

			needed -= (old_size - q->size);		/* Amount we removed */
		}
	}

	mq_update_flowc(q);		/* May cause us to leave "swift" mode */

	/*
	 * Re-install for next time, if still in "swift" mode.
	 *
	 * Subsequent calls after the initial call all go through
	 * mq_swift_timer() anyway.
	 */

	if (q->flags & MQ_SWIFT) {
		q->flowc_written = 0;
		q->last_size = q->size;
		q->swift_ev = cq_insert(callout_queue,
			q->swift_elapsed, mq_swift_timer, q);
	}
}

/**
 * Callout queue callback: periodic "swift" mode timer.
 */
static void
mq_swift_timer(cqueue_t *unused_cq, gpointer obj)
{
	mqueue_t *q = (mqueue_t *) obj;

	(void) unused_cq;
	g_assert((q->flags & (MQ_FLOWC|MQ_SWIFT)) == (MQ_FLOWC|MQ_SWIFT));

	mq_swift_checkpoint(q, FALSE);
}

/**
 * Callout queue callback invoked when the queue must enter "swift" mode.
 */
static void
mq_enter_swift(cqueue_t *unused_cq, gpointer obj)
{
	mqueue_t *q = (mqueue_t *) obj;

	(void) unused_cq;
	g_assert((q->flags & (MQ_FLOWC|MQ_SWIFT)) == MQ_FLOWC);

	q->flags |= MQ_SWIFT;

	node_tx_swift_changed(q->node);
	mq_swift_checkpoint(q, TRUE);
}

/**
 * Called when the message queue first enters flow-control.
 */
static void
mq_enter_flowc(mqueue_t *q)
{
	/*
	 * We're installing an event that will fire once the grace period is
	 * exhausted: this will bring us into "swift" mode, unless the event
	 * is cancelled because we're leaving flow-control.
	 */

	g_assert(q->swift_ev == NULL);
	g_assert(q->node != NULL);
	g_assert(!(q->flags & (MQ_FLOWC|MQ_SWIFT)));
	g_assert(q->size >= q->hiwat);

	q->flags |= MQ_FLOWC;			/* Above wartermark, raise */
	q->flowc_written = 0;
	q->last_size = q->size;
	q->swift_elapsed = node_flowc_swift_grace(q->node) * 1000;
	q->swift_ev =
		cq_insert(callout_queue, q->swift_elapsed, mq_enter_swift, q);

	node_tx_enter_flowc(q->node);	/* Signal flow control */

	if (dbg > 4)
		printf("entering FLOWC for node %s (%d bytes queued)\n",
			node_ip(q->node), q->size);
}

/**
 * Leaving flow-control state.
 */
static void
mq_leave_flowc(mqueue_t *q)
{
	g_assert(q->flags & MQ_FLOWC);

	if (dbg > 4)
		printf("leaving %s for node %s (%d bytes queued)\n",
			(q->flags & MQ_SWIFT) ? "SWIFT" : "FLOWC",
			node_ip(q->node), q->size);

	q->flags &= ~(MQ_FLOWC|MQ_SWIFT);	/* Under low watermark, clear */
	if (q->qlink)
		qlink_free(q);

	if (q->swift_ev) {
		cq_cancel(callout_queue, q->swift_ev);
		q->swift_ev = NULL;
	}

	node_tx_leave_flowc(q->node);	/* Signal end flow control */
}

/**
 * Update flow-control indication for queue.
 * Invoke node "callbacks" when crossing a watermark boundary.
 *
 * We define three levels: no flow-control, in warn zone, in flow-control.
 */
static void
mq_update_flowc(mqueue_t *q)
{
	if (q->flags & MQ_FLOWC) {
		if (q->size <= q->lowat) {
			mq_leave_flowc(q);
			q->flags &= ~MQ_WARNZONE;		/* no flow-control */
		}
	} else if (q->size >= q->hiwat) {
		mq_enter_flowc(q);
		q->flags |= MQ_WARNZONE;			/* in flow-control */
	} else if (q->size >= q->lowat) {
		if (!(q->flags & MQ_WARNZONE)) {
			q->flags |= MQ_WARNZONE;		/* in warn zone */
			node_tx_enter_warnzone(q->node);
		}
	} else if (q->flags & MQ_WARNZONE) {
		q->flags &= ~MQ_WARNZONE;			/* no flow-control */
		node_tx_leave_warnzone(q->node);
	}
}

/**
 * Remove all unsent messages from the queue.
 */
void
mq_clear(mqueue_t *q)
{
	GList *l;

	g_assert(q);

	if (q->count == 0)
		return;					/* Queue is empty */

	while ((l = q->qhead)) {
		pmsg_t *mb = (pmsg_t *) l->data;

		/*
		 * Break if we started to write this message, i.e. if we read
		 * some data out of it.
		 */

		if (!pmsg_is_unread(mb))
			break;

		(void) mq_rmlink_prev(q, l, pmsg_size(mb));
	}

	g_assert(q->count >= 0 && q->count <= 1);	/* At most one message */

	if (q->qlink)
		qlink_free(q);

	mq_update_flowc(q);

	/*
	 * Queue was not empty (hence enabled). If we removed all its
	 * messages, we must disable it: there is nothing more to service.
	 */

	if (q->count == 0) {
		tx_srv_disable(q->tx_drv);
		node_tx_service(q->node, FALSE);
	}
}

/**
 * Forbid further writes to the queue.
 */
void
mq_shutdown(mqueue_t *q)
{
	g_assert(q);

	q->flags |= MQ_DISCARD;
}

/**
 * Compare two pointers to links based on their relative priorities, then
 * based on their held Gnutella messages.
 * -- qsort() callback
 */
static gint
qlink_cmp(const void *lp1, const void *lp2)
{
	const pmsg_t *m1 = (const pmsg_t *) (*(const GList * const *) lp1)->data;
	const pmsg_t *m2 = (const pmsg_t *) (*(const GList * const *) lp2)->data;

	if (pmsg_prio(m1) == pmsg_prio(m2))
		return gmsg_cmp(pmsg_start(m1), pmsg_start(m2));

	return pmsg_prio(m1) < pmsg_prio(m2) ? -1 : +1;
}

/**
 * Create the `qlink' sorted array of queued items.
 */
static void
qlink_create(mqueue_t *q)
{
	GList **qlink;
	GList *l;
	gint n;

	g_assert(q->qlink == NULL);

	qlink = q->qlink = (GList **) g_malloc(q->count * sizeof(GList *));

	/*
	 * Prepare sorting of queued messages.
	 *
	 * What's sorted is queue links, but the comparison factor is the
	 * gmsg_cmp() routine to compare the Gnutella messages.
	 */

	for (l = q->qhead, n = 0; l && n < q->count; l = g_list_next(l), n++)
		qlink[n] = l;

	if (l || n != q->count)
		g_warning("BUG: queue count of %d for 0x%lx is wrong (found %d)",
			q->count, (gulong) q, n);

	/*
	 * We use `n' and not `q->count' in case the warning above is emitted,
	 * in which case we have garbage after the `n' first items.
	 */

	q->qlink_count = n;
	qsort(qlink, n, sizeof(GList *), qlink_cmp);
}

/**
 * Free the `qlink' sorted array of queued items.
 */
static void
qlink_free(mqueue_t *q)
{
	g_assert(q->qlink);

	G_FREE_NULL(q->qlink);
	q->qlink_count = 0;
}

/**
 * Insert linkable `l' within the sorted qlink array of linkables for the queue,
 * before the position indicated by `hint'.
 */
static void
qlink_insert_before(mqueue_t *q, gint hint, GList *l)
{
	g_assert(hint >= 0 && hint < q->qlink_count);
	g_assert(qlink_cmp(&q->qlink[hint], &l) >= 0);	/* `hint' >= `l' */

	/*
	 * Lookup before the message for a NULL entry that we could fill.
	 */

	if (hint > 0 && q->qlink[hint-1] == NULL) {
		q->qlink[hint-1] = l;
		return;
	}

	/*
	 * Extend the array then, and insert right at `hint' in the new array.
	 */

	q->qlink = g_realloc(q->qlink, ++q->qlink_count * sizeof(GList *));

	memmove(&q->qlink[hint+1], &q->qlink[hint],
		(q->qlink_count - hint - 1) * sizeof(GList *));	/* Shift right */

	q->qlink[hint] = l;
}

/**
 * Insert linkable `l' within the sorted qlink array of linkables.
 */
static void
qlink_insert(mqueue_t *q, GList *l)
{
	gint low = 0;
	gint high = q->qlink_count - 1;
	GList **qlink = q->qlink;

	/*
	 * If `qlink' is empty, create a slot for the new entry.
	 */

	if (high < 0) {
		g_assert(q->count == 1);		/* `l' is already part of the queue */
		q->qlink = g_realloc(q->qlink, ++q->qlink_count * sizeof(GList *));
		q->qlink[0] = l;
		return;
	}

	/*
	 * If lower than the beginning, insert at the head.
	 */

	if (qlink[low] != NULL && qlink_cmp(&l, &qlink[low]) <= 0) {
		qlink_insert_before(q, low, l);
		return;
	}

	/*
	 * If higher than the tail, insert at the tail.
	 */

	if (qlink[high] != NULL && qlink_cmp(&l, &qlink[high]) >= 0) {
		q->qlink = g_realloc(q->qlink, ++q->qlink_count * sizeof(GList *));
		q->qlink[q->qlink_count - 1] = l;
		return;
	}

	/*
	 * The array is sorted, so we're going to use a dichotomic search
	 * to find the right insertion point.  However, there can be NULLified
	 * entries in the array, so this is not a plain dichotomic search.
	 */

	while (low <= high) {
		gint mid = low + (high - low) / 2;
		gint c;

		/*
		 * If we end up in a NULL spot, inspect the [low, high] range.
		 */

		if (qlink[mid] == NULL) {
			gint n;
			gint lowest_non_null = -1;
			gint highest_non_null = -1;

			/*
			 * Go back towards `low' to find a non-NULL entry.
			 */

			for (n = mid - 1; n >= low; n--) {
				if (qlink[n] != NULL) {
					lowest_non_null = n;
					break;
				}
			}

			/*
			 * Go forward towards `high' to find a non-NULL entry.
			 */

			for (n = mid + 1; n <= high; n++) {
				if (qlink[n] != NULL) {
					highest_non_null = n;
					break;
				}
			}

			/*
			 * If both `lowest_non_null' and `highest_non_null' are -1, we
			 * have only NULLs, and the boundaries are NULL as well.
			 */

			if (lowest_non_null == -1) {
				if (highest_non_null == -1) {
					qlink[mid] = l;
					return;
				}
				low = mid + 1;
				continue;
			} else if (highest_non_null == -1) {
				high = mid - 1;
				continue;
			}

			/*
			 * We know that the final insertion point will be after `low'
			 * and before `high'.  If there are only NULL entries between
			 * the two, we're done...
			 */

			if (lowest_non_null <= low + 1 && highest_non_null >= high - 1) {
				qlink[mid] = l;			/* Insert at the middle of the range */
				return;
			}

			if (qlink_cmp(&l, &qlink[lowest_non_null]) < 0) {
				high = lowest_non_null - 1;
				continue;
			}

			if (qlink_cmp(&l, &qlink[highest_non_null]) > 0) {
				low = highest_non_null + 1;
				continue;
			}

			/*
			 * `lowest_non_null' <= `l' <= `highest_non_null'
			 */

			low = lowest_non_null + 1;
			high = highest_non_null - 1;

			continue;
		}

		/*
		 * Regular dichotomic case.
		 */

		c = qlink_cmp(&qlink[mid], &l);

		if (c == 0) {
			qlink_insert_before(q, mid, l);
			return;
		} else if (c < 0)
			low = mid + 1;
		else
			high = mid - 1;
	}

	if (qlink[low] == NULL) {
		qlink[low] = l;
		return;
	}

	qlink_insert_before(q, low, l);
}

/**
 * Remove the entry in the `qlink' linkable array.
 */
static void
qlink_remove(mqueue_t *q, GList *l)
{
	GList **qlink = q->qlink;
	gint n = q->qlink_count;

	g_assert(qlink);
	g_assert(n > 0);

	/*
	 * If more entries in `qlink' than 3 times the amount of queued messages,
	 * we have too many NULL in the array.
	 */

	if  (n > q->count * 3) {
		GList **dest = qlink;
		gint copied = 0;
		gboolean found = FALSE;

		while (n-- > 0) {
			GList *entry = *qlink++;;
			if (l == entry || entry == NULL) {
				if (entry == l)
					found = TRUE;
				continue;
			}
			*dest++ = entry;
			copied++;
		}

		q->qlink_count = copied;

		g_assert(found);

	} else {
		while (n-- > 0) {
			if (l == *qlink++) {
				*(--qlink) = NULL;
				return;
			}
		}

		g_assert(0);		/* Must have been found */
	}
}

/**
 * Remove from the queue enough messages that are less prioritary than
 * the current one, so as to make sure we can enqueue it.
 *
 * If `offset' is not null, it may be set with the offset within qlink where
 * the message immediately more prioritary than `mb' can be found.  It is
 * up to the caller to initialize it with -1 and check whether it has been
 * set.
 *
 * Returns TRUE if we were able to make enough room.
 */
static gboolean
make_room(mqueue_t *q, pmsg_t *mb, gint needed, gint *offset)
{
	gchar *header = pmsg_start(mb);
	guint prio = pmsg_prio(mb);

	return make_room_header(q, header, prio, needed, offset);
}

/**
 * Same as make_room(), but we are not given a "pmsg_t" as a comparison
 * point but a Gnutella header and a message priority explicitly.
 */
static gboolean
make_room_header(
	mqueue_t *q, gchar *header, guint prio, gint needed, gint *offset)
{
	gint n;
	gint dropped = 0;				/* Amount of messages dropped */

	g_assert(needed > 0);

	if (dbg > 5)
		printf("%s try to make room for %d bytes in queue 0x%lx (node %s)\n",
			(q->flags & MQ_SWIFT) ? "SWIFT" : "FLOWC",
			needed, (gulong) q, node_ip(q->node));

	if (q->qhead == NULL)			/* Queue is empty */
		return FALSE;

	if (q->qlink == NULL)			/* No cached sorted queue links */
		qlink_create(q);

	g_assert(q->qlink);

	/*
	 * Traverse the sorted links and prune as many messages as necessary.
	 * Note that we try to prune at least one byte more than needed, hence
	 * we stay in the loop even when needed reaches 0.
	 *
	 * To avoid freeing the qlink array every time we drop something, we
	 * write NULLs at the entries we drop (which are then ignored
	 * in the loop below).  When we break out because we found a more
	 * prioritary message, we remember the index in the array and return it
	 * to the caller.  If the message is finally inserted in the queue,
	 * we can insert its GList link right before that index.
	 *
	 * This adds some complexity but it will avoid millions of calls to
	 * qlink_cmp(), which is costly.
	 *
	 * The qlink array is freed when we leave flow control.  During FC, we
	 * need to find the messages we're removing after writing them to the
	 * network, so we can NULLify the corresponding slot in the qlink array.
	 */

	for (n = 0; needed >= 0 && n < q->qlink_count; n++) {
		GList *item = q->qlink[n];
		pmsg_t *cmb;
		gchar *cmb_start;
		gint cmb_size;

		/*
		 * If slot was NULLified, skip it.
		 */

		if (item == NULL)
			continue;

		cmb = (pmsg_t *) item->data;
		cmb_start = pmsg_start(cmb);

		/*
		 * Any partially written message, however unimportant, cannot be
		 * removed or we'd break the flow of messages.
		 */

		if (pmsg_read_base(cmb) != cmb_start)	/* Started to write it  */
			continue;

		/*
		 * If we reach a message equally or more important than the message
		 * we're trying to enqueue, then we haven't removed enough.  Stop!
		 *
		 * This is the only case where we don't necessarily attempt to prune
		 * more than requested, i.e. we'll return TRUE if needed == 0.
		 * (it's necessarily >= 0 if we're in the loop)
		 */

		if (gmsg_cmp(cmb_start, header) >= 0) {
			if (offset != NULL)
				*offset = n;
			break;
		}

		/*
		 * If we reach a message whose priority is higher than ours, stop.
		 * A less prioritary message cannot supersede a higher priority one,
		 * even if its embedded Gnet message is deemed less important.
		 */

		if (pmsg_prio(cmb) > prio) {
			if (offset != NULL)
				*offset = n;
			break;
		}

		/*
		 * Drop message.
		 */

		if (dbg > 4)
			gmsg_log_dropped(pmsg_start(cmb),
				"to %s node %s, in favor of %s",
				(q->flags & MQ_SWIFT) ? "SWIFT" : "FLOWC",
				node_ip(q->node), gmsg_infostr(header));

		gnet_stats_count_flowc(pmsg_start(cmb));
		cmb_size = pmsg_size(cmb);

		needed -= cmb_size;
		(void) mq_rmlink_prev(q, q->qlink[n], cmb_size);
		q->qlink[n] = NULL;
		dropped++;
	}

	if (dropped)
		node_add_txdrop(q->node, dropped);	/* Dropped during TX */

	if (dbg > 5)
		printf("%s end purge: %d bytes (count=%d) for node %s, need=%d\n",
			(q->flags & MQ_SWIFT) ? "SWIFT" : "FLOWC",
			q->size, q->count, node_ip(q->node), needed);

	/*
	 * In case we emptied the whole queue, disable servicing.
	 *
	 * This should only happen rarely, but it is conceivable if we
	 * get a message larger than the queue size and yet more prioritary
	 * than everything.  We'd empty the queue, and then call node_bye(),
	 * which would send the message immediately (mq_clear() would have no
	 * effect on an empty queue) and we would have an impossible condition:
	 * an empty queue with servicing enabled.  A recipe for disaster, as
	 * it breaks assertions.
	 *
	 * We know the queue was enabled because it was not empty initially
	 * if we reached that point, or we'd have cut processing at entry.
	 *
	 *		--RAM, 22/03/2002
	 */

	mq_update_flowc(q);		/* Perhaps we dropped enough to leave FC? */

	if (q->count == 0) {
		tx_srv_disable(q->tx_drv);
		node_tx_service(q->node, FALSE);
	}

	return needed <= 0;		/* Can be 0 if we broke out loop above */
}

/**
 * Put message in this queue.
 */
static void
mq_puthere(mqueue_t *q, pmsg_t *mb, gint msize)
{
	gint needed;
	gint qlink_offset = -1;
	GList *new = NULL;
	gboolean make_room_called = FALSE;
	gboolean has_normal_prio = (pmsg_prio(mb) == PMSG_P_DATA);

	/*
	 * If we're flow-controlled and the message can be dropped, acccept it
	 * if we can manage to make room for at least the size of the message,
	 * otherwise drop it.
	 */

	if (
		(q->flags & MQ_FLOWC) &&
		has_normal_prio &&
		gmsg_can_drop(pmsg_start(mb), msize) &&
		((make_room_called = TRUE)) &&			/* Call make_room() once only */
		!make_room(q, mb, msize, &qlink_offset)
	) {
		g_assert(pmsg_is_unread(mb));			/* Not partially written */
		if (dbg > 4)
			gmsg_log_dropped(pmsg_start(mb),
				"to FLOWC node %s, %d bytes queued",
				node_ip(q->node), q->size);

		gnet_stats_count_flowc(pmsg_start(mb));
		pmsg_free(mb);
		node_inc_txdrop(q->node);		/* Dropped during TX */
		return;
	}

	/*
	 * If enqueuing of message will make the queue larger than its maximum
	 * size, then remove from the queue messages that are less important
	 * than this one.
	 */

	needed = q->size + msize - q->maxsize;

	if (
		needed > 0 &&
		(make_room_called || !make_room(q, mb, needed, &qlink_offset))
	) {
		/*
		 * Close the connection only if the message is a prioritary one
		 * and yet there is no less prioritary message to remove!
		 *
		 * Otherwise, we simply drop the message and pray no havoc will
		 * result (like loosing a QRP PATCH message in the sequence).
		 *
		 *		--RAM, 18/01/2003
		 */

		g_assert(pmsg_is_unread(mb));			/* Not partially written */

		gnet_stats_count_flowc(pmsg_start(mb));

		if (has_normal_prio) {
			if (dbg > 4)
				gmsg_log_dropped(pmsg_start(mb),
					"to FLOWC node %s, %d bytes queued [FULL]",
					node_ip(q->node), q->size);

			node_inc_txdrop(q->node);		/* Dropped during TX */
		} else {
			if (dbg > 4)
				gmsg_log_dropped(pmsg_start(mb),
					"to FLOWC node %s, %d bytes queued [KILLING]",
					node_ip(q->node), q->size);

			node_bye(q->node, 502, "Send queue reached %d bytes", q->maxsize);
		}

		pmsg_free(mb);
		return;
	}

	g_assert(q->size + msize <= q->maxsize);

	/*
	 * Enqueue message.
	 *
	 * A normal priority message (the large majority of messages we deal with)
	 * is always enqueued at the head: messsages are read from the tail, i.e.
	 * it is a FIFO queue.
	 *
	 * A higher priority message needs to be inserted at the right place,
	 * near the *tail* but after any partially sent message, and of course
	 * after all enqueued messages with the same priority.
	 */

	if (has_normal_prio) {
		new = q->qhead = g_list_prepend(q->qhead, mb);
		if (q->qtail == NULL)
			q->qtail = q->qhead;
	} else {
		GList *l;
		guint prio = pmsg_prio(mb);
		gboolean inserted = FALSE;

		/*
		 * Unfortunately, there's no g_list_insert_after() or equivalent,
		 * so we break the GList encapsulation.
		 */

		for (l = q->qtail; l; l = l->prev) {
			pmsg_t *m = (pmsg_t *) l->data;

			if (
				pmsg_is_unread(m) &&			/* Not partially written */
				pmsg_prio(m) < prio				/* Reached insert point */
			) {
				/*
				 * Insert after current item, which is less prioritary than
				 * we are, then leave the loop.
				 */

				new = g_list_alloc();

				new->data = mb;
				new->prev = l;
				new->next = l->next;

				if (l->next)
					l->next->prev = new;
				else {
					g_assert(l == q->qtail);	/* Inserted at tail */
					q->qtail = new;				/* New tail */
				}
				l->next = new;

				inserted = TRUE;
				break;
			}
		}

		/*
		 * If we haven't inserted anything, then we've reached the
		 * head of the list.
		 */

		if (!inserted) {
			g_assert(l == NULL);

			new = q->qhead = g_list_prepend(q->qhead, mb);
			if (q->qtail == NULL)
				q->qtail = q->qhead;
		}
	}

	q->size += msize;
	q->count++;

	/*
	 * If `qlink' is not NULL, insert `new' within it.
	 *
	 * We have two options: we called make_room() and have `qlink_offset'
	 * set to something other than -1.  In that case, this is the offset
	 * of the message more prioritary that us.
	 *
	 * We have not called make_room() yet: we need to scan the `qlink' array
	 * to find the proper insertion place.
	 */

	if (q->qlink) {			/* Inserted something, `qlink' is stale */
		g_assert(new != NULL);

		if (qlink_offset != -1)
			qlink_insert_before(q, qlink_offset, new);
		else
			qlink_insert(q, new);
	}

	/*
	 * Update flow control indication, and enable node.
	 */

	mq_update_flowc(q);
	tx_srv_enable(q->tx_drv);

	if (q->count == 1)
		node_tx_service(q->node, TRUE);		/* Only on first message queued */
}

/**
 * Enqueue message, which becomes owned by the queue.
 */
void
mq_putq(mqueue_t *q, pmsg_t *mb)
{
	MQ_PUTQ(q, mb);
}

static const struct mq_cops mq_cops = {
	mq_puthere,				/* puthere */
	qlink_remove,			/* qlink_remove */
	mq_rmlink_prev,			/* rmlink_prev */
	mq_update_flowc,		/* update_flowc */
};

/**
 * Get common operations.
 */
const struct mq_cops *
mq_get_cops(void)
{
	return &mq_cops;
}

/* vi: set ts=4: */
