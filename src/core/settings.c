/*
 * $Id$
 *
 * Copyright (c) 2001-2003, Raphael Manfredi, Richard Eckart
 *
 * gtk-gnutella configuration.
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

#include "common.h"

#include <signal.h>
#include <pwd.h>
#include <netdb.h>

#include "settings.h"
#include "search.h"
#include "hosts.h"
#include "upload_stats.h"
#include "sockets.h"
#include "inet.h"
#include "hcache.h"
#include "downloads.h"
#include "share.h"

#include "if/gnet_property.h"
#include "if/gnet_property_priv.h"
#include "if/core/net_stats.h"

#include "if/bridge/c2ui.h"

#include "lib/cq.h"
#include "lib/file.h"
#include "lib/glib-missing.h"
#include "lib/override.h"		/* Must be the last header included */

RCSID("$Id$");

#define debug dbg

static const gchar config_file[] = "config_gnet";
static const gchar ul_stats_file[] = "upload_stats";

#define CONFIG_DIR_MODE	/* 0755 */ \
	(S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)

static gchar *home_dir = NULL;
static gchar *config_dir = NULL;

static prop_set_t *properties = NULL;

/* 
 * For backward compatibility these values are still read, but 
 * no longer written to the config file:
 *
 * Variable                    Changed at       New name
 * ----------------            ---------------- -------------
 * socksv5_user                0.90u 12/05/2002 socks_user
 * socksv5_pass                0.90u 12/05/2002 socks_pass
 * progressbar_bps_in_visible  0.90u 15/05/2002 progressbar_bws_in_visible
 * progressbar_bps_out_visible 0.90u 15/05/2002 progressbar_bws_out_visible
 * progressbar_bps_in_avg      0.90u 15/05/2002 progressbar_bws_in_avg
 * progressbar_bps_out_avg     0.90u 15/05/2002 progressbar_bws_out_avg
 */

static const gchar pidfile[] = "gtk-gnutella.pid";

static void settings_callbacks_init(void);
static void settings_callbacks_shutdown(void);
static void update_servent_uptime(void);

/* ----------------------------------------- */

/*
 * ensure_unicity
 *
 * Look for any existing PID file. If found, look at the pid recorded
 * there and make sure it has died. Abort operations if it hasn't...
 */
static void ensure_unicity(const gchar *file)
{
	FILE *f;
	pid_t pid;
	gulong pid_value; 
	gint error;
	gchar buf[16];

	f = file_fopen_missing(file, "r");
	if (f == NULL)
		return;				/* Assume it's missing if can't be opened */

	buf[0] = '\0';
	if (NULL == fgets(buf, sizeof(buf), f))
		return;
	fclose(f);

	pid_value = gm_atoul(buf, NULL, &error);
	if (error)
		return;				/* Can't read it back correctly */

	pid = pid_value;

	/*
	 * Existence check relies on the existence of signal 0. The kernel
	 * won't actually send anything, but will perform all the existence
	 * checks inherent to the kill() syscall for us...
	 */

	if (-1 == kill(pid, 0)) {
		if (errno != ESRCH)
			g_warning("kill() return unexpected error: %s", g_strerror(errno));
		return;
	}

	fprintf(stderr,
		"You seem to have left another gtk-gnutella running (pid = %ld)\n",
		(glong) pid);
	exit(1);
}

/*
 * save_pid
 *
 * Write our pid to the pidfile.
 */
static void save_pid(const gchar *file)
{
	FILE *f;

	f = file_fopen(file, "w");
	if (f == NULL)
		return;

	fprintf(f, "%lu\n", (gulong) getpid());

	if (0 != fclose(f))
		g_warning("could not flush pidfile \"%s\": %s",
			file, g_strerror(errno));
}

/* ----------------------------------------- */

#if !defined(_SC_PAGE_SIZE) && defined(_SC_PAGESIZE)
#define _SC_PAGE_SIZE _SC_PAGESIZE
#endif

#if defined (_SC_PAGE_SIZE) && defined (_SC_PHYS_PAGES)
static glong settings_getpagesize(void)
{
	glong ret;

	errno = 0;
	ret = sysconf(_SC_PAGE_SIZE);
	if ((glong) -1 == ret && 0 != errno) {
		g_warning("sysconf(_SC_PHYS_PAGES) failed: %s", g_strerror(errno));
		return 0;
	}
	return ret;
}
#else
#define settings_getpagesize() ((glong) getpagesize())
#endif /* _SC_PAGE_SIZE && _SC_PHYS_PAGES */

/* 
 * settings_getphysmemsize:
 *
 * returns the amount of physical RAM in KB, or zero in case of failure
 */
static gulong settings_getphysmemsize(void)
{
#if defined (_SC_PHYS_PAGES)
	guint32 pagesize = settings_getpagesize();
	glong pages;

	errno = 0;
	pages = sysconf(_SC_PHYS_PAGES); 
	if ((glong) -1 == pages && 0 != errno) {
		g_warning("sysconf(_SC_PHYS_PAGES) failed: %s", g_strerror(errno));
		return 0;
	}
	return (pagesize >> 10) * (gulong) pages;
#elif defined(HAS_SYSCTL) && defined(CTL_HW) && defined(HW_USERMEM)
/* There's also HW_PHYSMEM but HW_USERMEM is better for our needs. */
	int mib[2] = { CTL_HW, HW_USERMEM };
	int amount = 0;
	size_t len = sizeof(amount);

	if (-1 == sysctl(mib, 2, &amount, &len, NULL, 0))
		g_warning(
			"settings_getphysmemsize: sysctl() for HW_USERMEM failed: %s",
			g_strerror(errno));
	return amount / 1024;
#else /* !(defined (_SC_PHYS_PAGES) || defined(HAS_SYSCTL)) */
	g_warning("Unable to determine amount of physical RAM");
	return 0;
#endif 	/* _SC_PHYS_PAGES */
}

void settings_init(void)
{
    struct passwd *pwd = NULL;
	guint32 maxfd = (guint32) sysconf(_SC_OPEN_MAX);
	gulong memory = settings_getphysmemsize();
	guint32 amount = (guint32) memory;
	struct rlimit lim;
	char *path = NULL;

	g_message("detected amount of physical RAM: %lu KB", memory);
	g_message("process can use %u file descriptors", maxfd);

	if (-1 != getrlimit(RLIMIT_DATA, &lim)) {
		guint32 maxdata = lim.rlim_cur >> 10;
		amount = MIN(amount, maxdata);		/* For our purposes */
	}

    properties = gnet_prop_init();

	gnet_prop_set_guint32_val(PROP_SYS_NOFILE, maxfd);
	gnet_prop_set_guint32_val(PROP_SYS_PHYSMEM, amount);

	config_dir = g_strdup(getenv("GTK_GNUTELLA_DIR"));
	memset((gchar *) guid, 0, sizeof(guid));

	pwd = getpwuid(getuid());

	if (pwd && pwd->pw_dir)
		home_dir = g_strdup(pwd->pw_dir);
	else
		home_dir = g_strdup(getenv("HOME"));

	if (!home_dir)
		g_warning(_("Can't find your home directory!"));

	if (!config_dir) {
		if (home_dir) {
			config_dir = make_pathname(home_dir, ".gtk-gnutella");
		} else
			g_warning(_("No home directory: prefs will not be saved!"));
	}

	if (NULL == config_dir || '\0' == config_dir[0])
		goto no_config_dir;

	if (!is_directory(config_dir)) {
		g_warning(_("creating configuration directory \"%s\""), config_dir);

		if (mkdir(config_dir, CONFIG_DIR_MODE) == -1) {
			g_warning("mkdir(\"%s\") failed: \"%s\"",
				config_dir, g_strerror(errno));
			goto no_config_dir;
#if 0
			G_FREE_NULL(config_dir);
			config_dir = NULL;
#endif
		}
	}


	g_assert(NULL != config_dir);
	/* Ensure we're the only instance running */

	path = make_pathname(config_dir, pidfile);
	ensure_unicity(path);
	save_pid(path);
	G_FREE_NULL(path);

		/* Parse the configuration */
	prop_load_from_file(properties, config_dir, config_file);
    
	path = make_pathname(config_dir, ul_stats_file);

	upload_stats_load_history(path);	/* Loads the upload statistics */
	G_FREE_NULL(path);
	

	/* watch for filter_file defaults */

	if (hard_ttl_limit < max_ttl) {
		*(guint32 *) &hard_ttl_limit = max_ttl;
		g_warning("hard_ttl_limit was too small, adjusted to %u",
			hard_ttl_limit);
	}

	/* Flow control depends on this being not too small */
	if (node_sendqueue_size < 1.5 * settings_max_msg_size()) {
		*(guint32 *) &node_sendqueue_size =
			(guint32) (1.5 * settings_max_msg_size());
		g_warning("node_sendqueue_size was too small, adjusted to %u",
			node_sendqueue_size);
	}

    settings_callbacks_init();
	return;

no_config_dir: 
	g_warning(_("Cannot proceed without valid configuration directory"));
	exit(EXIT_FAILURE); /* g_error() would dump core, that's ugly. */
}

/*
 * settings_config_dir
 *
 * Get the config directory
 */
const gchar *settings_config_dir(void)
{
	g_assert(NULL != config_dir);
	return (const gchar *) config_dir;
}

/*
 * settings_home_dir
 *
 * Gets the home dir.
 */
const gchar *settings_home_dir(void)
{
	g_assert(NULL != home_dir);
	return (const gchar *) home_dir;
}

/*
 * settings_remove_pidfile:
 *
 * Remove pidfile.
 */
static void settings_remove_pidfile(void)
{
	char *path;

	path = make_pathname(config_dir, pidfile);
	g_return_if_fail(NULL != path);
	if (-1 == unlink(path))
		g_warning("could not remove pidfile \"%s\": %s",
			path, g_strerror(errno));
	G_FREE_NULL(path);
}

/*
 * settings_ip_changed:
 *
 * This routine is called when we determined that our IP was no longer the
 * one we computed.  We base this on some headers sent back when we handshake
 * with other nodes, and as a result, cannot trust the information.
 *
 * What we do henceforth is trust 3 successive indication that our IP changed,
 * provided we get the same information each time.
 *
 *		--RAM, 13/01/2002
 *
 * ``peer_ip'' is the IP address of peer which reported the new IP address.
 * There must be 3 peers from 3 different /16 networks before a change is
 * accepted. Otherwise, it would be very easy to confuse GTKG by connecting
 * 3 times in a row and submitting a *wrong* IP address.
 *
 *		--cbiere, 2004-08-01
 */
void settings_ip_changed(guint32 new_ip, guint32 peer_ip)
{
	static guint32 last_ip_seen = 0;
	static guint same_ip_count = 0;
	static guint32 peers[3];
	guint i;

	g_assert(!force_local_ip);		/* Must be called when IP isn't forced */
	g_assert(new_ip != 0);			/* The new IP must be valid */

	peer_ip &= 0xffff0000;		/* One vote per /16 network; host byteorder! */
	for (i = 0; i < G_N_ELEMENTS(peers); i++) {
		if (peers[i] == peer_ip)
			return;
	}

	if (new_ip != last_ip_seen) {
		last_ip_seen = new_ip;
		same_ip_count = 1;
		peers[0] = peer_ip;
		return;
	}

	g_assert(same_ip_count > 0 && same_ip_count < G_N_ELEMENTS(peers));
	peers[same_ip_count] = peer_ip;

	if (++same_ip_count < G_N_ELEMENTS(peers))
		return;

	last_ip_seen = 0;
	same_ip_count = 0;
	memset(peers, 0, sizeof peers);

	if (new_ip == local_ip)
		return;

    gnet_prop_set_guint32_val(PROP_LOCAL_IP, new_ip);
}

/*
 * settings_max_msg_size:
 *
 * Maximum message payload size we are configured to handle.
 */
guint32 settings_max_msg_size(void)
{
	/*
	 * Today, they are fixed at config time, but they will be set via
	 * GUI tomorrow, so the max size is not fixed in time.
	 *				--RAM, 15/09/2001
	 *
	 * They can be changed via the GUI as of today...
	 *				-- RAM, 24/12/2003
	 */

	guint32 maxsize;

	maxsize = MAX(search_queries_kick_size, search_answers_kick_size);
	maxsize = MAX(maxsize, other_messages_kick_size);

	return maxsize;
}

/*
 * settings_ask_for_property
 *
 * Ask them to set a property to be able to run.
 */
void settings_ask_for_property(gchar *name, gchar *value)
{
	extern void gtk_gnutella_exit(gint);

	fprintf(stderr, "\n*** ANCIENT VERSION DETECTED! ***\n\n");
	fprintf(stderr,
		"Sorry, this program is too ancient to run without\n"
		"an explicit user action: please edit the file\n\n"
		"\t%s%s%s\n\n"
		"and set the variable \"%s\" to\n\"%s\".\n\n"
		"You will then be able to run this version forever, but\n"
		"please consider upgrading, as Gnutella is an evolving\n"
		"network, where ancient versions are less performant, if\n"
		"not harmful!\n\n",
		config_dir, G_DIR_SEPARATOR_S, config_file, name, value);
	fprintf(stderr, "*** EXITING ***\n\n");

	gtk_gnutella_exit(1);
}

/*
 * settings_shutdown
 *
 * Called at exit time to flush the property files.
 */
void settings_shutdown(void)
{
	update_servent_uptime();
    settings_callbacks_shutdown();

    prop_save_to_file(properties, config_dir, config_file);
}

/*
 * settings_save_if_dirty
 *
 * Save settings if dirty.
 */
void settings_save_if_dirty(void)
{
    prop_save_to_file_if_dirty(properties, config_dir, config_file);
}

/*
 * settings_close:
 *
 * Finally free all memory allocated. Call after settings_shutdown.
 */
void settings_close(void)
{
	settings_remove_pidfile();
    gnet_prop_shutdown();

	if (home_dir)
		G_FREE_NULL(home_dir);
	if (config_dir)
		G_FREE_NULL(config_dir);
}

void gnet_get_bw_stats(gnet_bw_source type, gnet_bw_stats_t *s)
{
    g_assert(s != NULL);

    switch(type) {
    case BW_GNET_IN:
        s->enabled  = bws_gin_enabled;
        s->current  = bsched_bps(bws.gin);
        s->average  = bsched_avg_bps(bws.gin);
        s->limit    = bws.gin->bw_per_second;
        break;
    case BW_GNET_OUT:
        s->enabled  = bws_gout_enabled;
        s->current  = bsched_bps(bws.gout);
        s->average  = bsched_avg_bps(bws.gout);
        s->limit    = bws.gout->bw_per_second;
        break;
    case BW_GNET_UDP_IN:
        s->enabled  = bws_gin_enabled;
        s->current  = bsched_bps(bws.gin_udp);
        s->average  = bsched_avg_bps(bws.gin_udp);
        s->limit    = bws.gin_udp->bw_per_second;
        break;
    case BW_GNET_UDP_OUT:
        s->enabled  = bws_gout_enabled;
        s->current  = bsched_bps(bws.gout_udp);
        s->average  = bsched_avg_bps(bws.gout_udp);
        s->limit    = bws.gout_udp->bw_per_second;
        break;
    case BW_HTTP_IN:
        s->enabled  = bws_in_enabled;
        s->current  = bsched_bps(bws.in);
        s->average  = bsched_avg_bps(bws.in);
        s->limit    = bws.in->bw_per_second;
        break;
    case BW_HTTP_OUT:
        s->enabled  = bws_out_enabled;
        s->current  = bsched_bps(bws.out);
        s->average  = bsched_avg_bps(bws.out);
        s->limit    = bws.out->bw_per_second;
        break;
    case BW_LEAF_IN:
        s->enabled  = bws_glin_enabled;
        s->current  = bsched_bps(bws.glin);
        s->average  = bsched_avg_bps(bws.glin);
        s->limit    = bws.glin->bw_per_second;
        break;
    case BW_LEAF_OUT:
        s->enabled  = bws_glout_enabled;
        s->current  = bsched_bps(bws.glout);
        s->average  = bsched_avg_bps(bws.glout);
        s->limit    = bws.glout->bw_per_second;
        break;
    };
}

/***
 *** Internal helpers.
 ***/

/*
 * get_average_ip_lifetime
 *
 * Compute the EMA of the IP address lifetime up to now, but do not
 * update the property.
 */
time_t get_average_ip_lifetime(time_t now)
{
	guint32 current_ip_stamp;
	guint32 average_ip_uptime;
	time_t lifetime = 0;

	gnet_prop_get_guint32_val(PROP_CURRENT_IP_STAMP, &current_ip_stamp);
	gnet_prop_get_guint32_val(PROP_AVERAGE_IP_UPTIME, &average_ip_uptime);

	if (current_ip_stamp) {
		lifetime = now - current_ip_stamp;
		if (lifetime < 0)
			lifetime = 0;
	}

	/*
	 * The average lifetime is computed as an EMA on 3 terms.
	 * The smoothing factor sm=2/(3+1) is therefore 0.5.
	 */

	average_ip_uptime += (lifetime >> 1) - (average_ip_uptime >> 1);

	return average_ip_uptime;
}

/*
 * update_address_lifetime
 *
 * Called whenever the IP address we advertise changed.
 * Update the average uptime for a given IP address.
 */
static void update_address_lifetime(void)
{
	static guint32 old_ip = 0;
	gboolean force_local_ip;
	guint32 current_ip;
	guint32 current_ip_stamp;
	time_t now;

	gnet_prop_get_boolean_val(PROP_FORCE_LOCAL_IP, &force_local_ip);
	if (force_local_ip)
		gnet_prop_get_guint32_val(PROP_FORCED_LOCAL_IP, &current_ip);
	else
		gnet_prop_get_guint32_val(PROP_LOCAL_IP, &current_ip);

	if (old_ip == 0) {				/* First time */
		old_ip = current_ip;
		gnet_prop_get_guint32_val(PROP_CURRENT_IP_STAMP, &current_ip_stamp);
		if (current_ip_stamp == 0) {
			now = time(NULL);
			gnet_prop_set_guint32_val(PROP_CURRENT_IP_STAMP, (guint32) now);
		}
	}

	if (old_ip == current_ip)
		return;

	/*
	 * IP address changed, update lifetime information.
	 */

	now = time(NULL);
	old_ip = current_ip;

	gnet_prop_get_guint32_val(PROP_CURRENT_IP_STAMP, &current_ip_stamp);

	if (current_ip_stamp)
		gnet_prop_set_guint32_val(PROP_AVERAGE_IP_UPTIME,
			get_average_ip_lifetime(now));

	gnet_prop_set_guint32_val(PROP_CURRENT_IP_STAMP, (guint32) now);
}

/*
 * get_average_servent_uptime
 *
 * Compute the EMA of the averate servent uptime, up to now, but do not
 * update the property.
 */
time_t get_average_servent_uptime(time_t now)
{
	time_t start_stamp;
	gint32 uptime, val;
	guint32 avg_servent_uptime;

	gnet_prop_get_guint32_val(PROP_AVERAGE_SERVENT_UPTIME, &avg_servent_uptime);
	gnet_prop_get_guint32_val(PROP_START_STAMP, &val);
	
	start_stamp = val;
	uptime = delta_time(now, start_stamp);
	if (uptime < 0)
		uptime = 0;

	/*
	 * The average uptime is computed as an EMA on 7 terms.
	 * The smoothing factor sm=2/(7+1) is therefore 0.25.
	 */

	avg_servent_uptime += (uptime >> 2) - (avg_servent_uptime >> 2);

	return avg_servent_uptime;
}

/*
 * update_servent_uptime
 *
 * Called at shutdown time to update the average_uptime property before
 * saving the properties to disk.
 */
static void update_servent_uptime(void)
{
	time_t now = time(NULL);

	gnet_prop_set_guint32_val(PROP_AVERAGE_SERVENT_UPTIME,
		get_average_servent_uptime(now));
}

/***
 *** Callbacks
 ***/
static gboolean up_connections_changed(property_t prop)
{
    guint32 up_connections;
    guint32 max_connections;
    
    gnet_prop_get_guint32_val(PROP_UP_CONNECTIONS, &up_connections);
    gnet_prop_get_guint32_val(PROP_MAX_CONNECTIONS, &max_connections);

    if (up_connections > max_connections)
        gnet_prop_set_guint32_val(PROP_MAX_CONNECTIONS, up_connections);

    return FALSE;
}

static gboolean max_connections_changed(property_t prop)
{
    guint32 up_connections;
    guint32 max_connections;
    
    gnet_prop_get_guint32_val(PROP_UP_CONNECTIONS, &up_connections);
    gnet_prop_get_guint32_val(PROP_MAX_CONNECTIONS, &max_connections);

    if (up_connections > max_connections)
        gnet_prop_set_guint32_val(PROP_UP_CONNECTIONS, max_connections);

    return FALSE;
}

static gboolean max_hosts_cached_changed(property_t prop)
{
    hcache_prune(HCACHE_FRESH_ANY);

    return FALSE;
}

static gboolean max_ultra_hosts_cached_changed(property_t prop)
{
    hcache_prune(HCACHE_FRESH_ULTRA);

    return FALSE;
}

static gboolean max_bad_hosts_cached_changed(property_t prop)
{
    hcache_prune(HCACHE_BUSY);
    hcache_prune(HCACHE_TIMEOUT);
    hcache_prune(HCACHE_UNSTABLE);

    return FALSE;
}

static gboolean enable_udp_changed(property_t prop)
{
	gboolean enabled;

    gnet_prop_get_boolean_val(prop, &enabled);

	if (enabled) {
		if (s_udp_listen == NULL)
			s_udp_listen = socket_udp_listen(0, listen_port);
		node_udp_enable();
	} else {
		if (s_udp_listen) {
			socket_free(s_udp_listen);
			s_udp_listen = NULL;
		}
		node_udp_disable();
	}

	return FALSE;
}

static gboolean listen_port_changed(property_t prop)
{
	static guint32 old_listen_port = (guint32) -1;
	gboolean random_port = FALSE;
    guint32 listen_port;
	guint num_tried = 0;
	guint32 tried[65536 / (8 * sizeof(guint32))]; /* Use bits as bool flags */

    gnet_prop_get_guint32_val(prop, &listen_port);

	/*
	 * If port did not change values, do nothing.
	 */

	if (listen_port == old_listen_port)
		return FALSE;

	if (old_listen_port != (guint32) -1) {
		inet_firewalled();			/* Assume we're firewalled on port change */
		inet_udp_firewalled();
	}

	random_port = listen_port == 0;
	memset(tried, 0, sizeof tried);
	memset(tried, 0xff, 1024 / 8); /* Mark ports below 1024 as already tried */

	do {
		if (random_port) {
			guint32 i, b, r;

			listen_port = r = random_value(65535 - 1024) + 1024;
			/* Check whether this port was tried before */
			do {
				i = r / (8 * sizeof tried[0]);
				b = 1 << (r % (8 * sizeof tried[0]));
				if ((tried[i] & b) == 0) {
					tried[i] |= b;
					listen_port = r;
					break;
				}
				r = (r + 101) & 0xffff;
			} while (r != listen_port);

			g_assert(listen_port > 1023);
		}
	
		old_listen_port = listen_port;
	
		/*
		 * Close old ports.
		 */
	
		if (s_tcp_listen) {
			socket_free(s_tcp_listen);
			s_tcp_listen = NULL;
		}

		if (s_udp_listen) {
			socket_free(s_udp_listen);
			s_udp_listen = NULL;
		}
	
		/*
		 * If the new port != 0, open the new port
		 */
	
		if (listen_port)
			s_tcp_listen = socket_tcp_listen(0, listen_port, SOCK_TYPE_CONTROL);

		/*
		 * If UDP is enabled, also listen on the same UDP port.
		 */

		if (enable_udp) {
			s_udp_listen = socket_udp_listen(0, listen_port);
			if (random_port && s_udp_listen == NULL) {
				socket_free(s_tcp_listen);
				s_tcp_listen = NULL;
			}
		}
	} while (random_port && s_tcp_listen == NULL && ++num_tried < 65535 - 1024);
	
    /*
     * If socket allocation failed, reset the property
     */

    if ((s_tcp_listen == NULL) && (listen_port != 0)) {
		if (random_port) {
			old_listen_port = (guint32) -1;
			listen_port = 0;
		} else
        	old_listen_port = listen_port = 0;
		
        gnet_prop_set_guint32_val(prop, listen_port);
        return TRUE;
    }

	if (random_port)
		gnet_prop_set_guint32_val(prop, listen_port);

    return FALSE;
}

static gboolean bw_http_in_enabled_changed(property_t prop)
{
    gboolean val;

    gnet_prop_get_boolean_val(prop, &val);

    if (val)
        bsched_enable(bws.in);
    else
        bsched_disable(bws.in);

    return FALSE;
}

static gboolean bw_http_out_enabled_changed(property_t prop)
{
    gboolean val;

    gnet_prop_get_boolean_val(prop, &val);

    if (val)
        bsched_enable(bws.out);
    else
        bsched_disable(bws.out);

    return FALSE;
}

static gboolean bw_gnet_in_enabled_changed(property_t prop)
{
    gboolean val;

    gnet_prop_get_boolean_val(prop, &val);

    if (val)
        bsched_enable(bws.gin);
    else
        bsched_disable(bws.gin);

    return FALSE;
}

static gboolean bw_gnet_out_enabled_changed(property_t prop)
{
    gboolean val;

    gnet_prop_get_boolean_val(prop, &val);

    if (val)
        bsched_enable(bws.gout);
    else
        bsched_disable(bws.gout);

    return FALSE;
}

static gboolean bw_gnet_lin_enabled_changed(property_t prop)
{
    gboolean val;

    gnet_prop_get_boolean_val(prop, &val);

    if (val)
        bsched_enable(bws.glin);
    else
        bsched_disable(bws.glin);

    return FALSE;
}

static gboolean bw_gnet_lout_enabled_changed(property_t prop)
{
    gboolean val;

    gnet_prop_get_boolean_val(prop, &val);

    if (val)
        bsched_enable(bws.glout);
    else
        bsched_disable(bws.glout);

    return FALSE;
}

static gboolean node_sendqueue_size_changed(property_t prop)
{
    guint32 val;
    guint32 min = 1.5 * settings_max_msg_size();

    gnet_prop_get_guint32_val(PROP_NODE_SENDQUEUE_SIZE, &val);

    if (val < min) {
        gnet_prop_set_guint32_val(PROP_NODE_SENDQUEUE_SIZE, min);
        return TRUE;
    }
    
    return FALSE;
}

static gboolean scan_extensions_changed(property_t prop)
{
    gchar *s = gnet_prop_get_string(prop, NULL, 0);

    parse_extensions(s);

    G_FREE_NULL(s);

    return FALSE;
}

static gboolean file_path_changed(property_t prop)
{
    gchar *s = gnet_prop_get_string(prop, NULL, 0);

	g_assert(s != NULL);

    if (!is_directory(s)) {
		gchar *path;

        if (prop == PROP_SAVE_FILE_PATH) {
			prop_def_t *def = gnet_prop_get_def(prop);
			path = g_strdup(home_dir ? home_dir : *def->data.string.def);
			prop_free_def(def);
		} else
			path = gnet_prop_get_string(PROP_SAVE_FILE_PATH, NULL, 0);
    
        g_warning("property \"%s\": directory %s is not available, "
            "using %s instead", gnet_prop_name(prop), s, path); 

        gnet_prop_set_string(prop, path);

		G_FREE_NULL(path);
        G_FREE_NULL(s);
        return TRUE;
    }

    G_FREE_NULL(s);
    return FALSE;
}

static gboolean shared_dirs_paths_changed(property_t prop)
{
    gchar *s = gnet_prop_get_string(prop, NULL, 0);

    if (!shared_dirs_parse(s)) {
        G_FREE_NULL(s);
        shared_dirs_update_prop();
        return TRUE;
    } else {
        G_FREE_NULL(s);
        return FALSE;
    }
}

static gboolean local_netmasks_string_changed(property_t prop)
{
    gchar *s = gnet_prop_get_string(prop, NULL, 0);

    parse_netmasks(s);

    G_FREE_NULL(s);

    return FALSE;
}

static gboolean hard_ttl_limit_changed(property_t prop)
{
    guint32 hard_ttl_limit;
    guint32 max_ttl;

    gnet_prop_get_guint32_val(PROP_HARD_TTL_LIMIT, &hard_ttl_limit);
    gnet_prop_get_guint32_val(PROP_MAX_TTL, &max_ttl);

    if (hard_ttl_limit < max_ttl)
        gnet_prop_set_guint32_val(PROP_MAX_TTL, hard_ttl_limit);

    return FALSE;
}

static gboolean max_ttl_changed(property_t prop)
{
    guint32 hard_ttl_limit;
    guint32 max_ttl;

    gnet_prop_get_guint32_val(PROP_HARD_TTL_LIMIT, &hard_ttl_limit);
    gnet_prop_get_guint32_val(PROP_MAX_TTL, &max_ttl);

    if (hard_ttl_limit < max_ttl)
        gnet_prop_set_guint32_val(PROP_HARD_TTL_LIMIT, max_ttl);

    return FALSE;
}

static gboolean bw_http_in_changed(property_t prop)
{
    guint32 val;

    gnet_prop_get_guint32(prop, &val, 0, 1);
    bsched_set_bandwidth(bws.in, val);

	gnet_prop_get_guint32_val(PROP_CURRENT_PEERMODE, &val);
	bsched_set_peermode(val);

    return FALSE;
}

static gboolean bw_http_out_changed(property_t prop)
{
    guint32 val;

    gnet_prop_get_guint32(prop, &val, 0, 1);
    bsched_set_bandwidth(bws.out, val);

	gnet_prop_get_guint32_val(PROP_CURRENT_PEERMODE, &val);
	bsched_set_peermode(val);
    
    return FALSE;
}

static gboolean bw_gnet_in_changed(property_t prop)
{
    guint32 val;

    gnet_prop_get_guint32(prop, &val, 0, 1);
    bsched_set_bandwidth(bws.gin, val / 2);
    bsched_set_bandwidth(bws.gin_udp, val / 2);
    
	gnet_prop_get_guint32_val(PROP_CURRENT_PEERMODE, &val);
	bsched_set_peermode(val);

    return FALSE;
}

static gboolean bw_gnet_out_changed(property_t prop)
{
    guint32 val;

    gnet_prop_get_guint32(prop, &val, 0, 1);
    bsched_set_bandwidth(bws.gout, val / 2);
    bsched_set_bandwidth(bws.gout_udp, val / 2);
    
	gnet_prop_get_guint32_val(PROP_CURRENT_PEERMODE, &val);
	bsched_set_peermode(val);

    return FALSE;
}

static gboolean bw_gnet_lin_changed(property_t prop)
{
    guint32 val;

    gnet_prop_get_guint32(prop, &val, 0, 1);
    bsched_set_bandwidth(bws.glin, val);
    
	gnet_prop_get_guint32_val(PROP_CURRENT_PEERMODE, &val);
	bsched_set_peermode(val);

    return FALSE;
}

static gboolean bw_gnet_lout_changed(property_t prop)
{
    guint32 val;

    gnet_prop_get_guint32(prop, &val, 0, 1);
    bsched_set_bandwidth(bws.glout, val);
    
	gnet_prop_get_guint32_val(PROP_CURRENT_PEERMODE, &val);
	bsched_set_peermode(val);

    return FALSE;
}

static gboolean bw_allow_stealing_changed(property_t prop)
{
	gboolean val;

	gnet_prop_get_boolean_val(prop, &val);

	if (val)
		bsched_config_steal_http_gnet();
	else
		bsched_config_steal_gnet();

	return FALSE;
}

static gboolean node_online_mode_changed(property_t prop)
{
	gboolean val;

	gnet_prop_get_boolean_val(prop, &val);
	node_set_online_mode(val);

    return FALSE;
}

static gboolean lib_debug_changed(property_t prop)
{
/* XXX -- common_dbg is no longer a property! --RAM */
#if 0
	gnet_prop_get_guint32_val(prop, &common_dbg);
#endif
    return FALSE;
}

static gboolean force_local_ip_changed(property_t prop)
{
	update_address_lifetime();
    return FALSE;
}

static gboolean local_ip_changed(property_t prop)
{
	update_address_lifetime();
    return FALSE;
}

static gboolean configured_peermode_changed(property_t prop)
{
    guint32 val;
	gboolean forced = FALSE;

    gnet_prop_get_guint32_val(prop, &val);

	/*
	 * We don't allow them to be anything but a leaf node if they are
	 * firewalled.  We even restrict the "normal" mode, which is to be
	 * avoided anyway, and will be removed in a future release.
	 *		--RAM, 2004-09-19
	 */

	switch (val) {
	case NODE_P_NORMAL:
	case NODE_P_ULTRA:
		if (is_firewalled) {
			val = NODE_P_AUTO;
			forced = TRUE;
			g_warning("must run as a leaf when TCP-firewalled");
			gcu_statusbar_warning(
				_("Can only run as a leaf when TCP-firewalled"));
		}
		break;
	default:
		break;
	}

	if (val == NODE_P_AUTO) {
		if (connected_nodes() > 0)		/* Already connected */
			return forced;				/* Keep our current operating mode */
		val = NODE_P_LEAF;				/* Force leaf mode */
		/* FALL THROUGH */
	}

	gnet_prop_set_guint32_val(PROP_CURRENT_PEERMODE, val);

    return forced;
}

static gboolean current_peermode_changed(property_t prop)
{
    guint32 val;

    gnet_prop_get_guint32_val(prop, &val);
	node_current_peermode_changed((node_peer_t) val);

    return FALSE;
}

static gboolean download_rx_size_changed(property_t prop)
{
    guint32 val;

    gnet_prop_get_guint32_val(prop, &val);
	download_set_socket_rx_size(val * 1024);

	return FALSE;
}

static gboolean node_rx_size_changed(property_t prop)
{
    guint32 val;

    gnet_prop_get_guint32_val(prop, &val);
	node_set_socket_rx_size(val * 1024);

	return FALSE;
}

/*
 * Automatically reset properties have a callout queue entry associated
 * with them.  When the entry fires, the property is cleared.  Each time
 * the property is set, the callout entry is reactivated some time in the
 * future.
 */

static gpointer ev_file_descriptor_shortage = NULL;
static gpointer ev_file_descriptor_runout = NULL;

#define RESET_PROP_TM	(10*60*1000)	/* 10 minutes in ms */

/*
 * reset_property_cb	-- callout queue callback
 *
 * Reset the property.
 */
static void reset_property_cb(cqueue_t *cq, gpointer obj)
{
	property_t prop = (property_t) obj;

	switch (prop) {
	case PROP_FILE_DESCRIPTOR_SHORTAGE:
		ev_file_descriptor_shortage = NULL;
		break;
	case PROP_FILE_DESCRIPTOR_RUNOUT:
		ev_file_descriptor_runout = NULL;
		break;
	default:
		g_error("unhandled property #%d", prop);
		break;
	}

	gnet_prop_set_boolean_val(prop, FALSE);
}

static gboolean file_descriptor_x_changed(property_t prop)
{
	gboolean state;
	gpointer *ev = NULL;

	gnet_prop_get_boolean_val(prop, &state);
	if (!state)
		return FALSE;

	/*
	 * Property is set to TRUE: arm callback to reset it in 10 minutes.
	 */

	switch (prop) {
	case PROP_FILE_DESCRIPTOR_SHORTAGE:
		ev = &ev_file_descriptor_shortage;
		break;
	case PROP_FILE_DESCRIPTOR_RUNOUT:
		ev = &ev_file_descriptor_runout;
		break;
	default:
		g_error("unhandled property #%d", prop);
		break;
	}

	g_assert(ev != NULL);

	if (*ev == NULL)
		*ev = cq_insert(callout_queue, RESET_PROP_TM, reset_property_cb,
			GUINT_TO_POINTER(prop));
	else
		cq_resched(callout_queue, *ev, RESET_PROP_TM);

    return FALSE;
}

/*
 * This is only necessary to migrate the old PROP_PROXY_IP to
 * PROP_PROXY_HOSTNAME and should be removed in a future release.
 *    -- cbiere, 2004-06-29
 */
static gboolean proxy_ip_changed(property_t prop)
{
	guint32 ip;

	gnet_prop_get_guint32_val(prop, &ip);
	if (ip) {
		gchar *hostname = gnet_prop_get_string(PROP_PROXY_HOSTNAME, NULL, 0);

		if (hostname[0] == '\0')
			gnet_prop_set_string(PROP_PROXY_HOSTNAME, ip_to_gchar(ip));
    	G_FREE_NULL(hostname);
	}
    return FALSE;
}

/***
 *** Property-to-callback map
 ***/

typedef struct prop_map {
    property_t prop;            /* property handle */
    prop_changed_listener_t cb; /* callback function */
    gboolean init;              /* init widget with current value */
} prop_map_t;

#define IGNORE NULL

static prop_map_t property_map[] = {
    {
        PROP_NODE_SENDQUEUE_SIZE,
        node_sendqueue_size_changed,
        TRUE
    },
    {
        PROP_SEARCH_QUERIES_KICK_SIZE,
        node_sendqueue_size_changed,
        TRUE
    },
    {
        PROP_SEARCH_ANSWERS_KICK_SIZE,
        node_sendqueue_size_changed,
        TRUE
    },
    { 
        PROP_UP_CONNECTIONS, 
        up_connections_changed, 
        TRUE 
    },
    {
        PROP_MAX_CONNECTIONS, 
        max_connections_changed, 
        TRUE 
    },
    {
        PROP_MAX_HOSTS_CACHED, 
        max_hosts_cached_changed, 
        TRUE 
    },
    {
        PROP_MAX_ULTRA_HOSTS_CACHED, 
        max_ultra_hosts_cached_changed, 
        TRUE 
	},
    {
        PROP_MAX_BAD_HOSTS_CACHED, 
        max_bad_hosts_cached_changed, 
        TRUE 
	},
    {
        PROP_LISTEN_PORT, 
        listen_port_changed, 
        TRUE
    },
    {
        PROP_BW_HTTP_IN_ENABLED, 
        bw_http_in_enabled_changed, 
        FALSE
    },
    {
        PROP_BW_HTTP_OUT_ENABLED, 
        bw_http_out_enabled_changed, 
        FALSE
    },
    {
        PROP_BW_GNET_IN_ENABLED, 
        bw_gnet_in_enabled_changed, 
        FALSE
    },
    {
        PROP_BW_GNET_OUT_ENABLED, 
        bw_gnet_out_enabled_changed, 
        FALSE
    },
    {
        PROP_BW_GNET_LEAF_IN_ENABLED, 
        bw_gnet_lin_enabled_changed, 
        FALSE
    },
    {
        PROP_BW_GNET_LEAF_OUT_ENABLED, 
        bw_gnet_lout_enabled_changed, 
        FALSE
    },
    {
        PROP_SCAN_EXTENSIONS,
        scan_extensions_changed,
        TRUE
    },
    {
        PROP_SAVE_FILE_PATH,
        file_path_changed,
        TRUE
    },
    {
        PROP_MOVE_FILE_PATH,
        file_path_changed,
        TRUE
    },
    {
        PROP_BAD_FILE_PATH,
        file_path_changed,
        TRUE
    },
    {
        PROP_SHARED_DIRS_PATHS,
        shared_dirs_paths_changed,
        TRUE
    },
    {
        PROP_LOCAL_NETMASKS_STRING,
        local_netmasks_string_changed,
        TRUE
    },
    {
        PROP_HARD_TTL_LIMIT,
        hard_ttl_limit_changed,
        TRUE
    },
    {
        PROP_MAX_TTL,
        max_ttl_changed,
        TRUE
    },
    {
        PROP_BW_HTTP_IN,
        bw_http_in_changed,
        FALSE
    },
    {
        PROP_BW_HTTP_OUT,
        bw_http_out_changed,
        FALSE
    },
    {
        PROP_BW_GNET_IN,
        bw_gnet_in_changed,
        FALSE
    },
    {
        PROP_BW_GNET_OUT,
        bw_gnet_out_changed,
        FALSE
    },
    {
        PROP_BW_GNET_LIN,
        bw_gnet_lin_changed,
        FALSE
    },
    {
        PROP_BW_GNET_LOUT,
        bw_gnet_lout_changed,
        FALSE
    },
    {
        PROP_BW_ALLOW_STEALING,
        bw_allow_stealing_changed,
        FALSE
    },
	{
		PROP_ONLINE_MODE,
		node_online_mode_changed,
		TRUE						/* Need to call callback at init time */
	},
    {
        PROP_LIB_DEBUG,
        lib_debug_changed,
        TRUE
    },
	{
		PROP_FORCE_LOCAL_IP,
		force_local_ip_changed,
		TRUE,
	},
	{
		PROP_LOCAL_IP,
		local_ip_changed,
		TRUE,
	},
	{
		PROP_CONFIGURED_PEERMODE,
		configured_peermode_changed,
		TRUE,
	},
	{
		PROP_CURRENT_PEERMODE,
		current_peermode_changed,
		TRUE,
	},
	{
		PROP_DOWNLOAD_RX_SIZE,
		download_rx_size_changed,
		TRUE,
	},
	{
		PROP_NODE_RX_SIZE,
		node_rx_size_changed,
		TRUE,
	},
	{
		PROP_FILE_DESCRIPTOR_SHORTAGE,
		file_descriptor_x_changed,
		FALSE,
	},
	{
		PROP_FILE_DESCRIPTOR_RUNOUT,
		file_descriptor_x_changed,
		FALSE,
	},
	{
		PROP_PROXY_IP,
		proxy_ip_changed,
		TRUE,
	},
	{
		PROP_ENABLE_UDP,
		enable_udp_changed,
		FALSE,				/* UDP socket inited via listen_port_changed() */
	},
};

/***
 *** Control functions
 ***/

#define PROPERTY_MAP_SIZE G_N_ELEMENTS(property_map)

static gboolean init_list[GNET_PROPERTY_NUM];

static void settings_callbacks_init(void)
{
    guint n;

    for (n = 0; n < GNET_PROPERTY_NUM; n ++)
        init_list[n] = FALSE;

    if (debug >= 2) {
        printf("settings_callbacks_init: property_map size: %u\n", 
            (guint) PROPERTY_MAP_SIZE);
    }

    for (n = 0; n < PROPERTY_MAP_SIZE; n ++) {
        property_t prop = property_map[n].prop;
        guint32 idx = prop - GNET_PROPERTY_MIN;

        if (!init_list[idx])
           init_list[idx] = TRUE;
        else
            g_warning("settings_callbacks_init:" 
                " property %d already mapped", n);

        if (property_map[n].cb != IGNORE) {
            gnet_prop_add_prop_changed_listener(
                property_map[n].prop,
                property_map[n].cb,
                property_map[n].init);
        } else if (debug >= 10) {
            printf("settings_callbacks_init: property ignored: %s\n",
				gnet_prop_name(prop));
        }
    }

    if (debug >= 1) {
        for (n = 0; n < GNET_PROPERTY_NUM; n++) {
            if (!init_list[n])
                printf("settings_callbacks_init: unmapped property: %s\n",
					gnet_prop_name(n+GNET_PROPERTY_MIN)); 
        }
    }
}

static void settings_callbacks_shutdown(void)
{
    guint n;

	if (ev_file_descriptor_shortage != NULL) {
		cq_cancel(callout_queue, ev_file_descriptor_shortage);
		ev_file_descriptor_shortage = NULL;
	}
	if (ev_file_descriptor_runout != NULL) {
		cq_cancel(callout_queue, ev_file_descriptor_runout);
		ev_file_descriptor_runout = NULL;
	}
	
    for (n = 0; n < PROPERTY_MAP_SIZE; n ++) {
        if (property_map[n].cb != IGNORE) {
            gnet_prop_remove_prop_changed_listener(
                property_map[n].prop,
                property_map[n].cb);
        }
    }
}

/* vi: set ts=4: */
