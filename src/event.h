/*
 * $Id$
 *
 * Copyright (c) 2002-2003, Richard Eckart
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

#ifndef _event_h_
#define _event_h_

#include "common.h"
#include "ui_core_interface_event_defs.h"

void real_event_destroy(struct event *evt);

void event_add_subscriber(
    struct event *evt, GCallback cb, frequency_t t, guint32 interval);

void event_remove_subscriber(struct event *evt, GCallback cb);

guint event_subscriber_count(struct event *evt);
gboolean event_subscriber_active(struct event *evt);

/*
 * T_VETO:   breaks trigger chain as soon as a subscriber returns 
 *           a value != 0.
 *
 * T_NORMAL: will call all subscribers in the chain. Use for 
 *           callbacks with a void return type.
 */
#define T_VETO(sig, ...) if((*((sig)s->cb))(__VA_ARGS__)) break;
#define T_NORMAL(sig, ...) (*((sig)s->cb))(__VA_ARGS__);

#define event_trigger(ev, type) G_STMT_START {                     \
    GSList *sl;                                                    \
    time_t now = time(NULL);                                       \
	event_t *evt = (ev);										   \
                                                                   \
    for (                                                          \
        sl = evt->subscribers;                                     \
        sl != NULL;                                                \
        sl = g_slist_next(sl)                                      \
    ) {                                                            \
        gboolean t;                                                \
        struct subscriber *s = (struct subscriber *) sl->data;     \
                                                                   \
        t = s->f_interval == 0;                                    \
        if (!t) {                                                  \
            switch (s->f_type) {                                   \
            case FREQ_UPDATES:                                     \
                t = (evt->triggered_count % s->f_interval) == 0;   \
                break;                                             \
            case FREQ_SECS:                                        \
                t = (guint32) delta_time(now, s->last_call)		   \
						> s->f_interval;   						   \
                break;                                             \
            default:                                               \
                g_assert_not_reached();                            \
            }                                                      \
        }                                                          \
                                                                   \
        if (t) {                                                   \
           s->last_call = now;                                     \
           type                                                    \
        }                                                          \
    }                                                              \
                                                                   \
    evt->triggered_count ++;                                       \
} G_STMT_END



struct event_table {
    GHashTable *events;
};

struct event_table *event_table_new(void);

#define event_table_destroy(t) G_STMT_START {                      \
    real_event_table_destroy(t);                                   \
    G_FREE_NULL(t);                                                \
} G_STMT_END
void real_event_table_destroy(struct event_table *t, gboolean cleanup);


void event_table_add_event(struct event_table *t, struct event *evt);

void event_table_remove_event(struct event_table *t, struct event *evt);

inline void event_table_remove_all(struct event_table *t);

/* vi: set ts=4: */
#endif	/* _event_h_ */
