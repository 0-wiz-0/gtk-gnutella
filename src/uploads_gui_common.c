/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Richard Eckart
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

#include "config.h"

#include <glib.h>
#include "glib-missing.h"   /* For gm_snprintf() */
#include "gnet.h"           /* For gnet_upload_status_t */
#include "misc.h"           /* For g_strlcpy() [GTK1] */
#include "uploads_gui.h"    /* For upload_row_data_t */
#include "gui_property.h"   /* For gui_prop_get_guint32() */

#include "uploads_gui_common.h"
#include "override.h"			/* Must be the last header included */

RCSID("$Id$");

#define IO_STALLED		60		/* If nothing exchanged after that many secs */

/*
 * uploads_gui_progress
 *
 * Returns a floating point value from [0:1] which indicates
 * the total progress of the upload.
 */
gfloat uploads_gui_progress(
	const gnet_upload_status_t *u,
	const upload_row_data_t *data)
{
	gfloat progress = 0.0;
	guint32 requested;
	
	if (u->pos < data->range_start) /* No progress yet */
		return 0.0; 

	switch (u->status) {
    case GTA_UL_HEADERS:
    case GTA_UL_WAITING:
    case GTA_UL_PFSP_WAITING:
    case GTA_UL_ABORTED:
	case GTA_UL_QUEUED:
    case GTA_UL_QUEUE:
    case GTA_UL_QUEUE_WAITING:
	case GTA_UL_PUSH_RECEIVED:
		progress = 0.0;
		break;
    case GTA_UL_CLOSED:
	case GTA_UL_COMPLETE:
		progress = 1.0;
		break;
	case GTA_UL_SENDING:
		requested = data->range_end - data->range_start + 1;
		if (requested != 0) {
			/*
			 * position divided by 1 percentage point, found by dividing
			 * the total size by 100
			 */
			progress = (gfloat)(u->pos - data->range_start) /
				(gfloat)requested;
		} else {
			progress = 0.0;
		}	
		break;
	default:	/* Should never get this far. */ 
		g_assert_not_reached(); 
		break;
	}
	return progress;
}

/*
 * uploads_gui_status_str
 *
 * Returns a pointer to a static buffer containing a string which
 * describes the current status of the upload.
 */
const gchar *uploads_gui_status_str(
    const gnet_upload_status_t *u, const upload_row_data_t *data)
{
	static gchar tmpstr[256];

	if (u->pos < data->range_start)
		return "No output yet..."; /* Never wrote anything yet */

    switch (u->status) {
    case GTA_UL_PUSH_RECEIVED:
        return "Got push, connecting back...";

    case GTA_UL_COMPLETE:
		if (u->last_update != data->start_date) {
	        guint32 requested = data->range_end - data->range_start + 1;
			guint32 spent = u->last_update - data->start_date;
            gfloat rate = (requested / 1024.0) / spent;
			gm_snprintf(tmpstr, sizeof(tmpstr),
				"Completed (%.1f k/s) %s", rate, short_time(spent));
		} else
			g_strlcpy(tmpstr, "Completed (< 1s)", sizeof(tmpstr));
        break;

    case GTA_UL_SENDING:
		{
			gint slen;
			gfloat rate = u->bps / 1024.0;

			/* Time Remaining at the current rate, in seconds  */
			guint32 tr = (data->range_end + 1 - u->pos) / u->avg_bps;

			slen = gm_snprintf(tmpstr, sizeof(tmpstr), "%.02f%% ", 
				uploads_gui_progress(u, data) * 100.0);

			if (time((time_t *) NULL) - u->last_update > IO_STALLED)
				slen += gm_snprintf(&tmpstr[slen], sizeof(tmpstr)-slen,
					"(stalled) ");
			else
				slen += gm_snprintf(&tmpstr[slen], sizeof(tmpstr)-slen,
					"(%.1f k/s) ", rate);

			gm_snprintf(&tmpstr[slen], sizeof(tmpstr)-slen,
				"TR: %s", short_time(tr));
		} 
		break;

    case GTA_UL_HEADERS:
        return "Waiting for headers...";

    case GTA_UL_WAITING:
        return "Waiting for further request...";

    case GTA_UL_PFSP_WAITING:
        return "Unavailable range, waiting retry...";

    case GTA_UL_ABORTED:
        return "Transmission aborted";

    case GTA_UL_CLOSED:
        return "Transmission complete";

	case GTA_UL_QUEUED:
		{
			extern gint max_uploads;
			extern gint running_uploads;
		
			/*
			 * Status: GTA_UL_QUEUED. When PARQ is enabled, and all upload
			 * slots are full an upload is placed into the PARQ-upload. Clients
			 * supporting Queue 0.1 and 1.0 will get an active slot. We
			 * probably want to display this information
			 *		-- JA, 06/02/2003
			 */
			if (u->parq_position <= max_uploads - running_uploads) {
				/* position 1 should always get an upload slot */
				if (u->parq_retry > 0)
					gm_snprintf(tmpstr, sizeof(tmpstr),
						"Waiting [%d] (slot %d / %d) %ds, lifetime: %s", 
						u->parq_queue_no,
						u->parq_position,
						u->parq_size,
						u->parq_retry, 
						short_time(u->parq_lifetime));
				else
					gm_snprintf(tmpstr, sizeof(tmpstr),
						"Waiting [%d] (slot %d / %d) lifetime: %s", 
						u->parq_queue_no,
						u->parq_position,
						u->parq_size,
						short_time(u->parq_lifetime));
			} else {
				if (u->parq_retry > 0)
					gm_snprintf(tmpstr, sizeof(tmpstr),
						"Queued [%d] (slot %d / %d) %ds, lifetime: %s", 
						u->parq_queue_no,
						u->parq_position,
						u->parq_size,
						u->parq_retry, 
						short_time(u->parq_lifetime));
				else
					gm_snprintf(tmpstr, sizeof(tmpstr),
						"Queued [%d] (slot %d / %d) lifetime: %s", 
						u->parq_queue_no,
						u->parq_position,
						u->parq_size,
						short_time(u->parq_lifetime));
			}
			break;
		}
    case GTA_UL_QUEUE:
        /*
         * PARQ wants to inform a client that action from the client its side
         * is wanted. So it is trying to connect back.
         *      -- JA, 15/04/2003 
         */
        return "Sending QUEUE, connecting back...";

    case GTA_UL_QUEUE_WAITING:
        /*
         * PARQ made a connect back because some action from the client is 
         * wanted. The connection is established and now waiting for some action
         *      -- JA, 15/04/2003
         */
		return "Sent QUEUE, waiting for headers...";
	
    default:
        g_assert_not_reached();
	}

    return tmpstr;
}

/*
 * upload_should_remove
 * 
 * Returns whether the entry for the upload `ul' should be removed 
 * from the UI with respect to the configured behaviour.
 */
gboolean upload_should_remove(time_t now, const upload_row_data_t *ul) 
{
	guint32 grace;

	g_assert(NULL != ul);

	switch (ul->status) {
	case GTA_UL_COMPLETE:
		{
			gboolean val;

			gui_prop_get_boolean_val(PROP_AUTOCLEAR_COMPLETED_UPLOADS, &val);
			gnet_prop_get_guint32_val(PROP_ENTRY_REMOVAL_TIMEOUT, &grace);

			if (delta_time(now, ul->last_update) <= grace)
				return FALSE;

			return val;
		}
		break;
	case GTA_UL_CLOSED:
	case GTA_UL_ABORTED:
		{
			gboolean val;

			gui_prop_get_boolean_val(PROP_AUTOCLEAR_FAILED_UPLOADS, &val);
			gnet_prop_get_guint32_val(PROP_ENTRY_REMOVAL_TIMEOUT, &grace);

			if (delta_time(now, ul->last_update) <= grace)
				return FALSE;

			return val;
		}
		break;
	default:
		break;
	}
	
	return FALSE;
}

/* vi: set ts=4: */
