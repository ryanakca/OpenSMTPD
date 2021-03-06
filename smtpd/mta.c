/*	$OpenBSD$	*/

/*
 * Copyright (c) 2008 Pierre-Yves Ritschard <pyr@openbsd.org>
 * Copyright (c) 2008 Gilles Chehade <gilles@poolp.org>
 * Copyright (c) 2009 Jacek Masiulaniec <jacekm@dobremiasto.net>
 * Copyright (c) 2012 Eric Faurot <eric@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>

#include <ctype.h>
#include <err.h>
#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <inttypes.h>
#include <netdb.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "smtpd.h"
#include "log.h"

#define MAXERROR_PER_HOST	4

#define DELAY_CHECK_SOURCE	1
#define DELAY_CHECK_SOURCE_SLOW	10
#define DELAY_CHECK_SOURCE_FAST 0
#define DELAY_CHECK_LIMIT	5

#define	DELAY_QUADRATIC		1
#define DELAY_ROUTE_BASE	200
#define DELAY_ROUTE_MAX		(3600 * 4)

static void mta_imsg(struct mproc *, struct imsg *);
static void mta_shutdown(void);
static void mta_sig_handler(int, short, void *);

static void mta_query_mx(struct mta_relay *);
static void mta_query_secret(struct mta_relay *);
static void mta_query_preference(struct mta_relay *);
static void mta_query_source(struct mta_relay *);
static void mta_on_mx(void *, void *, void *);
static void mta_on_secret(struct mta_relay *, const char *);
static void mta_on_preference(struct mta_relay *, int, int);
static void mta_on_source(struct mta_relay *, struct mta_source *);
static void mta_on_timeout(struct runq *, void *);
static void mta_connect(struct mta_connector *);
static void mta_route_enable(struct mta_route *);
static void mta_route_disable(struct mta_route *, int, int);
static void mta_drain(struct mta_relay *);
static void mta_flush(struct mta_relay *, int, const char *);
static struct mta_route *mta_find_route(struct mta_connector *, time_t, int*,
    time_t*);
static void mta_log(const struct mta_envelope *, const char *, const char *,
    const char *, const char *);

SPLAY_HEAD(mta_relay_tree, mta_relay);
static struct mta_relay *mta_relay(struct envelope *);
static void mta_relay_ref(struct mta_relay *);
static void mta_relay_unref(struct mta_relay *);
static int mta_relay_cmp(const struct mta_relay *, const struct mta_relay *);
SPLAY_PROTOTYPE(mta_relay_tree, mta_relay, entry, mta_relay_cmp);

SPLAY_HEAD(mta_host_tree, mta_host);
static struct mta_host *mta_host(const struct sockaddr *);
static void mta_host_ref(struct mta_host *);
static void mta_host_unref(struct mta_host *);
static int mta_host_cmp(const struct mta_host *, const struct mta_host *);
SPLAY_PROTOTYPE(mta_host_tree, mta_host, entry, mta_host_cmp);

SPLAY_HEAD(mta_domain_tree, mta_domain);
static struct mta_domain *mta_domain(char *, int);
#if 0
static void mta_domain_ref(struct mta_domain *);
#endif
static void mta_domain_unref(struct mta_domain *);
static int mta_domain_cmp(const struct mta_domain *, const struct mta_domain *);
SPLAY_PROTOTYPE(mta_domain_tree, mta_domain, entry, mta_domain_cmp);

SPLAY_HEAD(mta_source_tree, mta_source);
static struct mta_source *mta_source(const struct sockaddr *);
static void mta_source_ref(struct mta_source *);
static void mta_source_unref(struct mta_source *);
static const char *mta_source_to_text(struct mta_source *);
static int mta_source_cmp(const struct mta_source *, const struct mta_source *);
SPLAY_PROTOTYPE(mta_source_tree, mta_source, entry, mta_source_cmp);

static struct mta_connector *mta_connector(struct mta_relay *,
    struct mta_source *);
static void mta_connector_free(struct mta_connector *);
static const char *mta_connector_to_text(struct mta_connector *);

SPLAY_HEAD(mta_route_tree, mta_route);
static struct mta_route *mta_route(struct mta_source *, struct mta_host *);
static void mta_route_ref(struct mta_route *);
static void mta_route_unref(struct mta_route *);
static const char *mta_route_to_text(struct mta_route *);
static int mta_route_cmp(const struct mta_route *, const struct mta_route *);
SPLAY_PROTOTYPE(mta_route_tree, mta_route, entry, mta_route_cmp);

static struct mta_relay_tree		relays;
static struct mta_domain_tree		domains;
static struct mta_host_tree		hosts;
static struct mta_source_tree		sources;
static struct mta_route_tree		routes;

static struct tree wait_mx;
static struct tree wait_preference;
static struct tree wait_secret;
static struct tree wait_source;

static struct runq *runq_relay;
static struct runq *runq_connector;
static struct runq *runq_route;
static struct runq *runq_hoststat;

static time_t	max_seen_conndelay_route;
static time_t	max_seen_discdelay_route;

#define	HOSTSTAT_EXPIRE_DELAY	(4 * 3600)
struct hoststat {
	char			 name[SMTPD_MAXHOSTNAMELEN];
	time_t			 tm;
	char			 error[SMTPD_MAXLINESIZE];
	struct tree		 deferred;
};
static struct dict hoststat;

void mta_hoststat_update(const char *, const char *);
void mta_hoststat_cache(const char *, uint64_t);
void mta_hoststat_uncache(const char *, uint64_t);
void mta_hoststat_reschedule(const char *);
static void mta_hoststat_remove_entry(struct hoststat *);


void
mta_imsg(struct mproc *p, struct imsg *imsg)
{
	struct mta_relay	*relay;
	struct mta_task		*task;
	struct mta_domain	*domain;
	struct mta_route	*route;
	struct mta_mx		*mx, *imx;
	struct hoststat		*hs;
	struct mta_envelope	*e;
	struct sockaddr_storage	 ss;
	struct envelope		 evp;
	struct msg		 m;
	const char		*secret;
	const char		*hostname;
	uint64_t		 reqid;
	time_t			 t;
	char			 buf[SMTPD_MAXLINESIZE];
	int			 dnserror, preference, v, status;
	void			*iter;
	uint64_t		 u64;

	if (p->proc == PROC_QUEUE) {
		switch (imsg->hdr.type) {

		case IMSG_MTA_TRANSFER:
			m_msg(&m, imsg);
			m_get_envelope(&m, &evp);
			m_end(&m);

			relay = mta_relay(&evp);

			TAILQ_FOREACH(task, &relay->tasks, entry)
				if (task->msgid == evpid_to_msgid(evp.id))
					break;

			if (task == NULL) {
				task = xmalloc(sizeof *task, "mta_task");
				TAILQ_INIT(&task->envelopes);
				task->relay = relay;
				relay->ntask += 1;
				TAILQ_INSERT_TAIL(&relay->tasks, task, entry);
				task->msgid = evpid_to_msgid(evp.id);
				if (evp.sender.user[0] || evp.sender.domain[0])
					snprintf(buf, sizeof buf, "%s@%s",
					    evp.sender.user, evp.sender.domain);
				else
					buf[0] = '\0';
				task->sender = xstrdup(buf, "mta_task:sender");
				stat_increment("mta.task", 1);
			}

			e = xcalloc(1, sizeof *e, "mta_envelope");
			e->id = evp.id;
			e->creation = evp.creation;
			snprintf(buf, sizeof buf, "%s@%s",
			    evp.dest.user, evp.dest.domain);
			e->dest = xstrdup(buf, "mta_envelope:dest");
			snprintf(buf, sizeof buf, "%s@%s",
			    evp.rcpt.user, evp.rcpt.domain);
			if (strcmp(buf, e->dest))
				e->rcpt = xstrdup(buf, "mta_envelope:rcpt");
			e->task = task;

			TAILQ_INSERT_TAIL(&task->envelopes, e, entry);
			log_debug("debug: mta: received evp:%016" PRIx64
			    " for <%s>", e->id, e->dest);

			stat_increment("mta.envelope", 1);

			mta_drain(relay);
			mta_relay_unref(relay); /* from here */
			return;

		case IMSG_QUEUE_MESSAGE_FD:
			mta_session_imsg(p, imsg);
			return;
		}
	}

	if (p->proc == PROC_LKA) {
		switch (imsg->hdr.type) {

		case IMSG_LKA_SECRET:
			m_msg(&m, imsg);
			m_get_id(&m, &reqid);
			m_get_string(&m, &secret);
			m_end(&m);
			relay = tree_xpop(&wait_secret, reqid);
			mta_on_secret(relay, secret[0] ? secret : NULL);
			return;

		case IMSG_LKA_SOURCE:
			m_msg(&m, imsg);
			m_get_id(&m, &reqid);
			m_get_int(&m, &status);
			if (status == LKA_OK)
				m_get_sockaddr(&m, (struct sockaddr*)&ss);
			m_end(&m);

			relay = tree_xpop(&wait_source, reqid);
			mta_on_source(relay, (status == LKA_OK) ?
			    mta_source((struct sockaddr *)&ss) : NULL);
			return;

		case IMSG_LKA_HELO:
			mta_session_imsg(p, imsg);
			return;

		case IMSG_DNS_HOST:
			m_msg(&m, imsg);
			m_get_id(&m, &reqid);
			m_get_sockaddr(&m, (struct sockaddr*)&ss);
			m_get_int(&m, &preference);
			m_end(&m);
			domain = tree_xget(&wait_mx, reqid);
			mx = xcalloc(1, sizeof *mx, "mta: mx");
			mx->host = mta_host((struct sockaddr*)&ss);
			mx->preference = preference;
			TAILQ_FOREACH(imx, &domain->mxs, entry) {
				if (imx->preference > mx->preference) {
					TAILQ_INSERT_BEFORE(imx, mx, entry);
					return;
				}
			}
			TAILQ_INSERT_TAIL(&domain->mxs, mx, entry);
			return;

		case IMSG_DNS_HOST_END:
			m_msg(&m, imsg);
			m_get_id(&m, &reqid);
			m_get_int(&m, &dnserror);
			m_end(&m);
			domain = tree_xpop(&wait_mx, reqid);
			domain->mxstatus = dnserror;
			if (domain->mxstatus == DNS_OK) {
				log_debug("debug: MXs for domain %s:",
				    domain->name);
				TAILQ_FOREACH(mx, &domain->mxs, entry)
					log_debug("	%s preference %i",
					    sa_to_text(mx->host->sa),
					    mx->preference);
			}
			else {
				log_debug("debug: Failed MX query for %s:",
				    domain->name);
			}
			waitq_run(&domain->mxs, domain);
			return;

		case IMSG_DNS_MX_PREFERENCE:
			m_msg(&m, imsg);
			m_get_id(&m, &reqid);
			m_get_int(&m, &dnserror);
			if (dnserror == 0)
				m_get_int(&m, &preference);
			m_end(&m);

			relay = tree_xpop(&wait_preference, reqid);
			mta_on_preference(relay, dnserror, preference);
			return;

		case IMSG_DNS_PTR:
			mta_session_imsg(p, imsg);
			return;

		case IMSG_LKA_SSL_INIT:
			mta_session_imsg(p, imsg);
			return;

		case IMSG_LKA_SSL_VERIFY:
			mta_session_imsg(p, imsg);
			return;
		}
	}

	if (p->proc == PROC_PARENT) {
		switch (imsg->hdr.type) {
		case IMSG_CTL_VERBOSE:
			m_msg(&m, imsg);
			m_get_int(&m, &v);
			m_end(&m);
			log_verbose(v);
			return;

		case IMSG_CTL_PROFILE:
			m_msg(&m, imsg);
			m_get_int(&m, &v);
			m_end(&m);
			profiling = v;
			return;
		}
	}

	if (p->proc == PROC_CONTROL) {
		switch (imsg->hdr.type) {
			
		case IMSG_CTL_RESUME_ROUTE:
			u64 = *((uint64_t *)imsg->data);
			if (u64)
				log_debug("resuming route: %llu",
				    (unsigned long long)u64);
			else
				log_debug("resuming all routes");
			SPLAY_FOREACH(route, mta_route_tree, &routes) {
				if (u64 && route->id != u64)
					continue;
				mta_route_enable(route);
				if (u64)
					break;
			}
			return;

		case IMSG_CTL_MTA_SHOW_ROUTES:
			SPLAY_FOREACH(route, mta_route_tree, &routes) {
				v = runq_pending(runq_route, NULL, route, &t);
				snprintf(buf, sizeof(buf),
				    "%llu. %s %c%c%c%c nconn=%zu penalty=%i timeout=%s",
				    (unsigned long long)route->id,
				    mta_route_to_text(route),
				    route->flags & ROUTE_NEW ? 'N' : '-',
				    route->flags & ROUTE_DISABLED ? 'D' : '-',
				    route->flags & ROUTE_RUNQ ? 'Q' : '-',
				    route->flags & ROUTE_KEEPALIVE ? 'K' : '-',
				    route->nconn,
				    route->penalty,
				    v ? duration_to_text(t - time(NULL)) : "-");
				m_compose(p, IMSG_CTL_MTA_SHOW_ROUTES,
				    imsg->hdr.peerid, 0, -1,
				    buf, strlen(buf) + 1);
			}
			m_compose(p, IMSG_CTL_MTA_SHOW_ROUTES, imsg->hdr.peerid,
			    0, -1, NULL, 0);
			return;
		case IMSG_CTL_MTA_SHOW_HOSTSTATS:
			iter = NULL;
			while (dict_iter(&hoststat, &iter, &hostname,
				(void **)&hs)) {
				snprintf(buf, sizeof(buf),
				    "%s|%llu|%s",
				    hostname, (unsigned long long) hs->tm,
				    hs->error);
				m_compose(p, IMSG_CTL_MTA_SHOW_HOSTSTATS,
				    imsg->hdr.peerid, 0, -1,
				    buf, strlen(buf) + 1);
			}
			m_compose(p, IMSG_CTL_MTA_SHOW_HOSTSTATS,
			    imsg->hdr.peerid,
			    0, -1, NULL, 0);
			return;
		}
	}

	errx(1, "mta_imsg: unexpected %s imsg", imsg_to_str(imsg->hdr.type));
}

static void
mta_sig_handler(int sig, short event, void *p)
{
	switch (sig) {
	case SIGINT:
	case SIGTERM:
		mta_shutdown();
		break;
	default:
		fatalx("mta_sig_handler: unexpected signal");
	}
}

static void
mta_shutdown(void)
{
	log_info("info: mail transfer agent exiting");
	_exit(0);
}

pid_t
mta(void)
{
	pid_t		 pid;
	struct passwd	*pw;
	struct event	 ev_sigint;
	struct event	 ev_sigterm;

	switch (pid = fork()) {
	case -1:
		fatal("mta: cannot fork");
	case 0:
		break;
	default:
		return (pid);
	}

	purge_config(PURGE_EVERYTHING);

	if ((pw = getpwnam(SMTPD_USER)) == NULL)
		fatalx("unknown user " SMTPD_USER);

	if (chroot(PATH_CHROOT) == -1)
		fatal("mta: chroot");
	if (chdir("/") == -1)
		fatal("mta: chdir(\"/\")");

	config_process(PROC_MTA);

	if (setgroups(1, &pw->pw_gid) ||
	    setresgid(pw->pw_gid, pw->pw_gid, pw->pw_gid) ||
	    setresuid(pw->pw_uid, pw->pw_uid, pw->pw_uid))
		fatal("mta: cannot drop privileges");

	SPLAY_INIT(&relays);
	SPLAY_INIT(&domains);
	SPLAY_INIT(&hosts);
	SPLAY_INIT(&sources);
	SPLAY_INIT(&routes);

	tree_init(&wait_secret);
	tree_init(&wait_mx);
	tree_init(&wait_preference);
	tree_init(&wait_source);
	dict_init(&hoststat);

	imsg_callback = mta_imsg;
	event_init();

	runq_init(&runq_relay, mta_on_timeout);
	runq_init(&runq_connector, mta_on_timeout);
	runq_init(&runq_route, mta_on_timeout);
	runq_init(&runq_hoststat, mta_on_timeout);

	signal_set(&ev_sigint, SIGINT, mta_sig_handler, NULL);
	signal_set(&ev_sigterm, SIGTERM, mta_sig_handler, NULL);
	signal_add(&ev_sigint, NULL);
	signal_add(&ev_sigterm, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGHUP, SIG_IGN);

	config_peer(PROC_PARENT);
	config_peer(PROC_QUEUE);
	config_peer(PROC_LKA);
	config_peer(PROC_CONTROL);
	config_done();

	if (event_dispatch() < 0)
		fatal("event_dispatch");
	mta_shutdown();

	return (0);
}

/*
 * Local error on the given source.
 */
void
mta_source_error(struct mta_relay *relay, struct mta_route *route, const char *e)
{
	struct mta_connector	*c;

	/*
	 * Remember the source as broken for this connector.
	 */
	c = mta_connector(relay, route->src);
	if (!(c->flags & CONNECTOR_ERROR_SOURCE))
		log_info("smtp-out: Error on %s: %s",
		    mta_route_to_text(route), e);
	c->flags |= CONNECTOR_ERROR_SOURCE;
}

/*
 * TODO:
 * Currently all errors are reported on the host itself.  Technically,
 * it should depend on the error, and it would be probably better to report
 * it at the connector level.  But we would need to have persistent routes
 * for that.  Hosts are "naturally" persisted, as they are referenced from
 * the MX list on the domain.
 * Also, we need a timeout on that.
 */
void
mta_route_error(struct mta_relay *relay, struct mta_route *route)
{
	route->dst->nerror++;

	if (route->dst->flags & HOST_IGNORE)
		return;

	if (route->dst->nerror > MAXERROR_PER_HOST) {
		log_info("smtp-out: Too many errors on host %s: ignoring this MX",
		    mta_host_to_text(route->dst));
		route->dst->flags |= HOST_IGNORE;
	}
}

void
mta_route_ok(struct mta_relay *relay, struct mta_route *route)
{
	struct mta_connector	*c;

	if (!(route->flags & ROUTE_NEW))
		return;

	log_debug("debug: mta-routing: route %s is now valid.",
	    mta_route_to_text(route));

	route->flags &= ~ROUTE_NEW;

	c = mta_connector(relay, route->src);
	mta_connect(c);
}

void
mta_route_down(struct mta_relay *relay, struct mta_route *route)
{
	mta_route_disable(route, 2, ROUTE_DISABLED_SMTP);
}

void
mta_route_collect(struct mta_relay *relay, struct mta_route *route)
{
	struct mta_connector	*c;

	log_debug("debug: mta_route_collect(%s)",
	    mta_route_to_text(route));

	relay->nconn -= 1;
	relay->domain->nconn -= 1;
	route->nconn -= 1;
	route->src->nconn -= 1;
	route->dst->nconn -= 1;
	route->lastdisc = time(NULL);

	/* First connection failed */
	if (route->flags & ROUTE_NEW)
		mta_route_disable(route, 2, ROUTE_DISABLED_NET);

	c = mta_connector(relay, route->src);
	c->nconn -= 1;
	mta_connect(c);
	mta_route_unref(route); /* from mta_find_route() */
	mta_relay_unref(relay); /* from mta_connect() */
}

struct mta_task *
mta_route_next_task(struct mta_relay *relay, struct mta_route *route)
{
	struct mta_task	*task;

	if ((task = TAILQ_FIRST(&relay->tasks))) {
		TAILQ_REMOVE(&relay->tasks, task, entry);
		relay->ntask -= 1;
		task->relay = NULL;
	}

	return (task);
}

void
mta_delivery_log(struct mta_envelope *e, const char *source, const char *relay,
    int delivery, const char *status)
{
	if (delivery == IMSG_DELIVERY_OK) {
		mta_log(e, "Ok", source, relay, status);
	}
	else if (delivery == IMSG_DELIVERY_TEMPFAIL) {
		mta_log(e, "TempFail", source, relay, status);
	}
	else if (delivery == IMSG_DELIVERY_PERMFAIL) {
		mta_log(e, "PermFail", source, relay, status);
	}
	else if (delivery == IMSG_DELIVERY_LOOP) {
		mta_log(e, "PermFail", source, relay, "Loop detected");
	}
	else
		errx(1, "bad delivery");
}

void
mta_delivery_notify(struct mta_envelope *e, int delivery, const char *status,
    uint32_t penalty)
{
	if (delivery == IMSG_DELIVERY_OK) {
		queue_ok(e->id);
	}
	else if (delivery == IMSG_DELIVERY_TEMPFAIL) {
		queue_tempfail(e->id, penalty, status);
	}
	else if (delivery == IMSG_DELIVERY_PERMFAIL) {
		queue_permfail(e->id, status);
	}
	else if (delivery == IMSG_DELIVERY_LOOP) {
		queue_loop(e->id);
	}
	else
		errx(1, "bad delivery");
}

void
mta_delivery(struct mta_envelope *e, const char *source, const char *relay,
    int delivery, const char *status, uint32_t penalty)
{
	mta_delivery_log(e, source, relay, delivery, status);
	mta_delivery_notify(e, delivery, status, penalty);
}

static void
mta_query_mx(struct mta_relay *relay)
{
	uint64_t	id;

	if (relay->status & RELAY_WAIT_MX)
		return;

	log_debug("debug: mta: querying MX for %s...",
	    mta_relay_to_text(relay));

	if (waitq_wait(&relay->domain->mxs, mta_on_mx, relay)) {
		id = generate_uid();
		tree_xset(&wait_mx, id, relay->domain);
		if (relay->domain->flags)
			dns_query_host(id, relay->domain->name);
		else
			dns_query_mx(id, relay->domain->name);
		relay->domain->lastmxquery = time(NULL);
	}
	relay->status |= RELAY_WAIT_MX;
	mta_relay_ref(relay);
}

static void
mta_query_limits(struct mta_relay *relay)
{
	if (relay->status & RELAY_WAIT_LIMITS)
		return;

	relay->limits = dict_get(env->sc_limits_dict, relay->domain->name);
	if (relay->limits == NULL)
		relay->limits = dict_get(env->sc_limits_dict, "default");

	if (max_seen_conndelay_route < relay->limits->conndelay_route)
		max_seen_conndelay_route = relay->limits->conndelay_route;
	if (max_seen_discdelay_route < relay->limits->discdelay_route)
		max_seen_discdelay_route = relay->limits->discdelay_route;
}

static void
mta_query_secret(struct mta_relay *relay)
{
	if (relay->status & RELAY_WAIT_SECRET)
		return;

	log_debug("debug: mta: querying secret for %s...",
	    mta_relay_to_text(relay));

	tree_xset(&wait_secret, relay->id, relay);
	relay->status |= RELAY_WAIT_SECRET;

	m_create(p_lka, IMSG_LKA_SECRET, 0, 0, -1);
	m_add_id(p_lka, relay->id);
	m_add_string(p_lka, relay->authtable);
	m_add_string(p_lka, relay->authlabel);
	m_close(p_lka);

	mta_relay_ref(relay);
}

static void
mta_query_preference(struct mta_relay *relay)
{
	if (relay->status & RELAY_WAIT_PREFERENCE)
		return;

	log_debug("debug: mta: querying preference for %s...",
	    mta_relay_to_text(relay));

	tree_xset(&wait_preference, relay->id, relay);
	relay->status |= RELAY_WAIT_PREFERENCE;
	dns_query_mx_preference(relay->id, relay->domain->name,
		relay->backupname);
	mta_relay_ref(relay);
}

static void
mta_query_source(struct mta_relay *relay)
{
	log_debug("debug: mta: querying source for %s...",
	    mta_relay_to_text(relay));

	relay->sourceloop += 1;

	if (relay->sourcetable == NULL) {
		/*
		 * This is a recursive call, but it only happens once, since
		 * another source will not be queried immediatly.
		 */
		mta_relay_ref(relay);
		mta_on_source(relay, mta_source(NULL));
		return;
	}

	m_create(p_lka, IMSG_LKA_SOURCE, 0, 0, -1);
	m_add_id(p_lka, relay->id);
	m_add_string(p_lka, relay->sourcetable);
	m_close(p_lka);

	tree_xset(&wait_source, relay->id, relay);
	relay->status |= RELAY_WAIT_SOURCE;
	mta_relay_ref(relay);
}

static void
mta_on_mx(void *tag, void *arg, void *data)
{
	struct mta_domain	*domain = data;
	struct mta_relay	*relay = arg;

	log_debug("debug: mta: ... got mx (%p, %s, %s)",
	    tag, domain->name, mta_relay_to_text(relay));

	switch (domain->mxstatus) {
	case DNS_OK:
		break;
	case DNS_RETRY:
		relay->fail = IMSG_DELIVERY_TEMPFAIL;
		relay->failstr = "Temporary failure in MX lookup";
		break;
	case DNS_EINVAL:
		relay->fail = IMSG_DELIVERY_PERMFAIL;
		relay->failstr = "Invalid domain name";
		break;
	case DNS_ENONAME:
		relay->fail = IMSG_DELIVERY_PERMFAIL;
		relay->failstr = "Domain does not exist";
		break;
	case DNS_ENOTFOUND:
		relay->fail = IMSG_DELIVERY_TEMPFAIL;
		relay->failstr = "No MX found for domain";
		break;
	default:
		fatalx("bad DNS lookup error code");
		break;
	}

	if (domain->mxstatus)
		log_info("smtp-out: Failed to resolve MX for %s: %s",
		    mta_relay_to_text(relay), relay->failstr);

	relay->status &= ~RELAY_WAIT_MX;
	mta_drain(relay);
	mta_relay_unref(relay); /* from mta_drain() */
}

static void
mta_on_secret(struct mta_relay *relay, const char *secret)
{
	log_debug("debug: mta: ... got secret for %s: %s",
	    mta_relay_to_text(relay), secret);

	if (secret)
		relay->secret = strdup(secret);

	if (relay->secret == NULL) {
		log_warnx("warn: Failed to retrieve secret "
			    "for %s", mta_relay_to_text(relay));
		relay->fail = IMSG_DELIVERY_TEMPFAIL;
		relay->failstr = "Could not retrieve credentials";
	}

	relay->status &= ~RELAY_WAIT_SECRET;
	mta_drain(relay);
	mta_relay_unref(relay); /* from mta_query_secret() */
}

static void
mta_on_preference(struct mta_relay *relay, int dnserror, int preference)
{
	if (dnserror) {
		log_warnx("warn: Couldn't find backup preference for %s",
		    mta_relay_to_text(relay));
		relay->backuppref = INT_MAX;
	}
	else {
		log_debug("debug: mta: ... got preference for %s: %i, %i",
		    mta_relay_to_text(relay), dnserror, preference);
		relay->backuppref = preference;
	}

	relay->status &= ~RELAY_WAIT_PREFERENCE;
	mta_drain(relay);
	mta_relay_unref(relay); /* from mta_query_preference() */
}

static void
mta_on_source(struct mta_relay *relay, struct mta_source *source)
{
	struct mta_connector	*c;
	void			*iter;
	int			 delay, errmask;

	log_debug("debug: mta: ... got source for %s: %s",
	    mta_relay_to_text(relay), source ? mta_source_to_text(source) : "NULL");

	relay->lastsource = time(NULL);
	delay = DELAY_CHECK_SOURCE_SLOW;

	if (source) {
		c = mta_connector(relay, source);
		if (c->flags & CONNECTOR_NEW) {
			c->flags &= ~CONNECTOR_NEW;
			delay = DELAY_CHECK_SOURCE;
		}
		mta_connect(c);
		if ((c->flags & CONNECTOR_ERROR) == 0)
			relay->sourceloop = 0;
		else
			delay = DELAY_CHECK_SOURCE_FAST;
		mta_source_unref(source); /* from constructor */
	}
	else {
		log_warnx("warn: Failed to get source address"
			    "for %s", mta_relay_to_text(relay));
	}

	if (tree_count(&relay->connectors) == 0) {
		relay->fail = IMSG_DELIVERY_TEMPFAIL;
		relay->failstr = "Could not retrieve source address";
	}
	if (tree_count(&relay->connectors) < relay->sourceloop) {
		relay->fail = IMSG_DELIVERY_TEMPFAIL;
		relay->failstr = "No valid route to remote MX";

		errmask = 0;
		iter = NULL;
		while (tree_iter(&relay->connectors, &iter, NULL, (void **)&c))
			errmask |= c->flags;

		if (errmask & CONNECTOR_ERROR_ROUTE_SMTP)
			relay->failstr = "Destination seem to reject all mails";
		else if (errmask & CONNECTOR_ERROR_ROUTE_NET)
			relay->failstr = "Network error on destination MXs";
		else if (errmask & CONNECTOR_ERROR_MX)
			relay->failstr = "No MX found for destination";
		else if (errmask & CONNECTOR_ERROR_FAMILY)
			relay->failstr = "Address family mismatch on destination MXs";
		else
			relay->failstr = "No valid route to destination";	
	}

	relay->nextsource = relay->lastsource + delay;
	relay->status &= ~RELAY_WAIT_SOURCE;
	mta_drain(relay);
	mta_relay_unref(relay); /* from mta_query_source() */
}

static void
mta_connect(struct mta_connector *c)
{
	struct mta_route	*route;
	struct mta_limits	*l = c->relay->limits;
	int			 limits;
	time_t			 nextconn, now;

    again:

	log_debug("debug: mta: connecting with %s", mta_connector_to_text(c));

	/* Do not connect if this connector has an error. */
	if (c->flags & CONNECTOR_ERROR) {
		log_debug("debug: mta: connector error");
		return;
	}

	if (c->flags & CONNECTOR_WAIT) {
		log_debug("debug: mta: canceling connector timeout");
		runq_cancel(runq_connector, NULL, c);
	}

	/* No job. */
	if (c->relay->ntask == 0) {
		log_debug("debug: mta: no task for connector");
		return;
	}

	/* Do not create more connections than necessary */
	if ((c->relay->nconn_ready >= c->relay->ntask) ||
	    (c->relay->nconn > 2 && c->relay->nconn >= c->relay->ntask / 2)) {
		log_debug("debug: mta: enough connections already");
		return;
	}

	limits = 0;
	nextconn = now = time(NULL);

	if (c->relay->domain->lastconn + l->conndelay_domain > nextconn) {
		log_debug("debug: mta: cannot use domain %s before %llus",
		    c->relay->domain->name,
		    (unsigned long long) c->relay->domain->lastconn + l->conndelay_domain - now);
		nextconn = c->relay->domain->lastconn + l->conndelay_domain;
	}
	if (c->relay->domain->nconn >= l->maxconn_per_domain) {
		log_debug("debug: mta: hit domain limit");
		limits |= CONNECTOR_LIMIT_DOMAIN;
	}

	if (c->source->lastconn + l->conndelay_source > nextconn) {
		log_debug("debug: mta: cannot use source %s before %llus",
		    mta_source_to_text(c->source),
		    (unsigned long long) c->source->lastconn + l->conndelay_source - now);
		nextconn = c->source->lastconn + l->conndelay_source;
	}
	if (c->source->nconn >= l->maxconn_per_source) {
		log_debug("debug: mta: hit source limit");
		limits |= CONNECTOR_LIMIT_SOURCE;
	}

	if (c->lastconn + l->conndelay_connector > nextconn) {
		log_debug("debug: mta: cannot use %s before %llus",
		    mta_connector_to_text(c),
		    (unsigned long long) c->lastconn + l->conndelay_connector - now);
		nextconn = c->lastconn + l->conndelay_connector;
	}
	if (c->nconn >= l->maxconn_per_connector) {
		log_debug("debug: mta: hit connector limit");
		limits |= CONNECTOR_LIMIT_CONN;
	}

	if (c->relay->lastconn + l->conndelay_relay > nextconn) {
		log_debug("debug: mta: cannot use %s before %llus",
		    mta_relay_to_text(c->relay),
		    (unsigned long long) c->relay->lastconn + l->conndelay_relay - now);
		nextconn = c->relay->lastconn + l->conndelay_relay;
	}
	if (c->relay->nconn >= l->maxconn_per_relay) {
		log_debug("debug: mta: hit relay limit");
		limits |= CONNECTOR_LIMIT_RELAY;
	}

	/* We can connect now, find a route */
	if (!limits && nextconn <= now)
		route = mta_find_route(c, now, &limits, &nextconn);
	else
		route = NULL;

	/* No route */
	if (route == NULL) {

		if (c->flags & CONNECTOR_ERROR) {
			/* XXX we might want to clear this flag later */
			log_debug("debug: mta-routing: no route available for %s: errors on connector",
			    mta_connector_to_text(c));
			return;
		}
		else if (limits) {
			log_debug("debug: mta-routing: no route available for %s: limits reached",
			    mta_connector_to_text(c));
			nextconn = now + DELAY_CHECK_LIMIT;
		}
		else {
			log_debug("debug: mta-routing: no route available for %s: must wait a bit",
			    mta_connector_to_text(c));
		}
		log_debug("debug: mta: retrying to connect on %s in %llus...",
		    mta_connector_to_text(c),
		    (unsigned long long) nextconn - time(NULL));
		c->flags |= CONNECTOR_WAIT;
		runq_schedule(runq_connector, nextconn, NULL, c);
		return;
	}

	log_debug("debug: mta-routing: spawning new connection on %s",
		    mta_route_to_text(route));

	c->nconn += 1;
	c->lastconn = time(NULL);

	c->relay->nconn += 1;
	c->relay->lastconn = c->lastconn;
	c->relay->domain->nconn += 1;
	c->relay->domain->lastconn = c->lastconn;
	route->nconn += 1;
	route->lastconn = c->lastconn;
	route->src->nconn += 1;
	route->src->lastconn = c->lastconn;
	route->dst->nconn += 1;
	route->dst->lastconn = c->lastconn;

	mta_session(c->relay, route);	/* this never fails synchronously */
	mta_relay_ref(c->relay);

    goto again;
}

static void
mta_on_timeout(struct runq *runq, void *arg)
{
	struct mta_connector	*connector = arg;
	struct mta_relay	*relay = arg;
	struct mta_route	*route = arg;
	struct hoststat		*hs = arg;

	if (runq == runq_relay) {
		log_debug("debug: mta: ... timeout for %s",
		    mta_relay_to_text(relay));
		relay->status &= ~RELAY_WAIT_CONNECTOR;
		mta_drain(relay);
		mta_relay_unref(relay); /* from mta_drain() */
	}
	else if (runq == runq_connector) {
		log_debug("debug: mta: ... timeout for %s",
		    mta_connector_to_text(connector));
		connector->flags &= ~CONNECTOR_WAIT;
		mta_connect(connector);
	}
	else if (runq == runq_route) {
		route->flags &= ~ROUTE_RUNQ;
		mta_route_enable(route);
		mta_route_unref(route);
	}
	else if (runq == runq_hoststat) {
		log_debug("debug: mta: ... timeout for hoststat %s",
			hs->name);
		mta_hoststat_remove_entry(hs);
		free(hs);
	}
}

static void
mta_route_disable(struct mta_route *route, int penalty, int reason)
{
	unsigned long long	delay;

	route->penalty += penalty;
	route->lastpenalty = time(NULL);
	delay = (unsigned long long)DELAY_ROUTE_BASE * route->penalty * route->penalty;
	if (delay > DELAY_ROUTE_MAX)
		delay = DELAY_ROUTE_MAX;
#if 0
	delay = 60;
#endif

	log_info("smtp-out: Disabling route %s for %llus",
	    mta_route_to_text(route), delay);

	if (route->flags & ROUTE_DISABLED) {
		runq_cancel(runq_route, NULL, route);
		mta_route_unref(route); /* from last call to here */
	}
	route->flags |= reason & ROUTE_DISABLED;
	runq_schedule(runq_route, time(NULL) + delay, NULL, route);
	mta_route_ref(route);
}

static void
mta_route_enable(struct mta_route *route)
{
	if (route->flags & ROUTE_DISABLED) {
		log_info("smtp-out: Enabling route %s",
		    mta_route_to_text(route));
		route->flags &= ~ROUTE_DISABLED;
		route->flags |= ROUTE_NEW;
	}
	
	if (route->penalty) {
#if DELAY_QUADRATIC
		route->penalty -= 1;
		route->lastpenalty = time(NULL);
#else
		route->penalty = 0;
#endif
	}
}

static void
mta_drain(struct mta_relay *r)
{
	char			 buf[64];

	log_debug("debug: mta: draining %s "
	    "refcount=%i, ntask=%zu, nconnector=%zu, nconn=%zu", 
	    mta_relay_to_text(r),
	    r->refcount, r->ntask, tree_count(&r->connectors), r->nconn);

	/*
	 * All done.
	 */
	if (r->ntask == 0) {
		log_debug("debug: mta: all done for %s", mta_relay_to_text(r));
		return;
	}

	/*
	 * If we know that this relay is failing flush the tasks.
	 */
	if (r->fail) {
		mta_flush(r, r->fail, r->failstr);
		return;
	}

	/* Query secret if needed. */
	if (r->flags & RELAY_AUTH && r->secret == NULL)
		mta_query_secret(r);

	/* Query our preference if needed. */
	if (r->backupname && r->backuppref == -1)
		mta_query_preference(r);

	/* Query the domain MXs if needed. */
	if (r->domain->lastmxquery == 0)
		mta_query_mx(r);

	/* Query the limits if needed. */
	if (r->limits == NULL)
		mta_query_limits(r);

	/* Wait until we are ready to proceed. */
	if (r->status & RELAY_WAITMASK) {
		buf[0] = '\0';
		if (r->status & RELAY_WAIT_MX)
			strlcat(buf, " MX", sizeof buf);
		if (r->status & RELAY_WAIT_PREFERENCE)
			strlcat(buf, " preference", sizeof buf);
		if (r->status & RELAY_WAIT_SECRET)
			strlcat(buf, " secret", sizeof buf);
		if (r->status & RELAY_WAIT_SOURCE)
			strlcat(buf, " source", sizeof buf);
		if (r->status & RELAY_WAIT_CONNECTOR)
			strlcat(buf, " connector", sizeof buf);
		log_debug("debug: mta: %s waiting for%s",
		    mta_relay_to_text(r), buf);
		return;
	}

	/*
	 * We have pending task, and it's maybe time too try a new source.
	 */
	if (r->nextsource <= time(NULL))
		mta_query_source(r);
	else {
		log_debug("debug: mta: scheduling relay %s in %llus...",
		    mta_relay_to_text(r),
		    (unsigned long long) r->nextsource - time(NULL));
		runq_schedule(runq_relay, r->nextsource, NULL, r);
		r->status |= RELAY_WAIT_CONNECTOR;
		mta_relay_ref(r);
	}
}

static void
mta_flush(struct mta_relay *relay, int fail, const char *error)
{
	struct mta_envelope	*e;
	struct mta_task		*task;
	const char     		*domain;
	void			*iter;
	struct mta_connector	*c;
	size_t			 n;
	size_t			 r;

	log_debug("debug: mta_flush(%s, %i, \"%s\")",
	    mta_relay_to_text(relay), fail, error);

	if (fail != IMSG_DELIVERY_TEMPFAIL && fail != IMSG_DELIVERY_PERMFAIL)
		errx(1, "unexpected delivery status %i", fail);

	n = 0;
	while ((task = TAILQ_FIRST(&relay->tasks))) {
		TAILQ_REMOVE(&relay->tasks, task, entry);
		while ((e = TAILQ_FIRST(&task->envelopes))) {
			TAILQ_REMOVE(&task->envelopes, e, entry);
			mta_delivery(e, NULL, relay->domain->name, fail, error, 0);
			
			/*
			 * host was suspended, cache envelope id in hoststat tree
			 * so that it can be retried when a delivery succeeds for
			 * that domain.
			 */
			domain = strchr(e->dest, '@');
			if (fail == IMSG_DELIVERY_TEMPFAIL && domain) {
				r = 0;
				iter = NULL;
				while (tree_iter(&relay->connectors, &iter,
					NULL, (void **)&c)) {
					if (c->flags & CONNECTOR_ERROR_ROUTE)
						r++;
				}
				if (tree_count(&relay->connectors) == r)
					mta_hoststat_cache(domain+1, e->id);
			}

			free(e->dest);
			free(e->rcpt);
			free(e);
			n++;
		}
		free(task->sender);
		free(task);
	}

	stat_decrement("mta.task", relay->ntask);
	stat_decrement("mta.envelope", n);
	relay->ntask = 0;
}

/*
 * Find a route to use for this connector
 */
static struct mta_route *
mta_find_route(struct mta_connector *c, time_t now, int *limits,
    time_t *nextconn)
{
	struct mta_route	*route, *best;
	struct mta_limits	*l = c->relay->limits;
	struct mta_mx		*mx;
	int			 level, limit_host, limit_route;
	int			 family_mismatch, seen, suspended_route;
	time_t			 tm;

	log_debug("debug: mta-routing: searching new route for %s...",
	    mta_connector_to_text(c));

	tm = 0;
	limit_host = 0;
	limit_route = 0;
	suspended_route = 0;
	family_mismatch = 0;
	level = -1;
	best = NULL;
	seen = 0;

	TAILQ_FOREACH(mx, &c->relay->domain->mxs, entry) {
		/*
		 * New preference level
		 */		
		if (mx->preference > level) {
#ifndef IGNORE_MX_PREFERENCE
			/*
			 * Use the current best MX if found.
			 */
			if (best)
				break;

			/*
			 * No candidate found.  There are valid MXs at this
			 * preference level but they reached their limit, or
			 * we can't connect yet.
			 */
			if (limit_host || limit_route || tm)
				break;

			/*
			 *  If we are a backup MX, do not relay to MXs with
			 *  a greater preference value.
			 */
			if (c->relay->backuppref >= 0 &&
			    mx->preference >= c->relay->backuppref)
				break;

			/*
			 * Start looking at MXs on this preference level.
			 */ 
#endif
			level = mx->preference;
		}

		if (mx->host->flags & HOST_IGNORE)
			continue;

		/* Found a possibly valid mx */
		seen++;

		if ((c->source->sa &&
		     c->source->sa->sa_family != mx->host->sa->sa_family) ||
		    (l->family && l->family != mx->host->sa->sa_family)) {
			log_debug("debug: mta-routing: skipping host %s: AF mismatch",
			    mta_host_to_text(mx->host));
			family_mismatch = 1;
			continue;
		}

		if (mx->host->nconn >= l->maxconn_per_host) {
			log_debug("debug: mta-routing: skipping host %s: too many connections",
			    mta_host_to_text(mx->host));
			limit_host = 1;
			continue;
		}

		if (mx->host->lastconn + l->conndelay_host > now) {
			log_debug("debug: mta-routing: skipping host %s: cannot use before %llus",
			    mta_host_to_text(mx->host),
			    (unsigned long long) mx->host->lastconn + l->conndelay_host - now);
			if (tm == 0 || mx->host->lastconn + l->conndelay_host < tm)
				tm = mx->host->lastconn + l->conndelay_host;
			continue;
		}

		route = mta_route(c->source, mx->host);

		if (route->flags & ROUTE_DISABLED) {
			log_debug("debug: mta-routing: skipping route %s: suspend",
			    mta_route_to_text(route));
			suspended_route |= route->flags & ROUTE_DISABLED;
			mta_route_unref(route); /* from here */
			continue;
		}

		if (route->nconn && (route->flags & ROUTE_NEW)) {
			log_debug("debug: mta-routing: skipping route %s: not validated yet",
			    mta_route_to_text(route));
			limit_route = 1;
			mta_route_unref(route); /* from here */
			continue;
		}

		if (route->nconn >= l->maxconn_per_route) {
			log_debug("debug: mta-routing: skipping route %s: too many connections",
			    mta_route_to_text(route));
			limit_route = 1;
			mta_route_unref(route); /* from here */
			continue;
		}

		if (route->lastconn + l->conndelay_route > now) {
			log_debug("debug: mta-routing: skipping route %s: cannot use before %llus (delay after connect)",
			    mta_route_to_text(route),
			    (unsigned long long) route->lastconn + l->conndelay_route - now);
			if (tm == 0 || route->lastconn + l->conndelay_route < tm)
				tm = route->lastconn + l->conndelay_route;
			mta_route_unref(route); /* from here */
			continue;
		}

		if (route->lastdisc + l->discdelay_route > now) {
			log_debug("debug: mta-routing: skipping route %s: cannot use before %llus (delay after disconnect)",
			    mta_route_to_text(route),
			    (unsigned long long) route->lastdisc + l->discdelay_route - now);
			if (tm == 0 || route->lastdisc + l->discdelay_route < tm)
				tm = route->lastdisc + l->discdelay_route;
			mta_route_unref(route); /* from here */
			continue;
		}

		/* Use the route with the lowest number of connections. */
		if (best && route->nconn >= best->nconn) {
			log_debug("debug: mta-routing: skipping route %s: current one is better",
			    mta_route_to_text(route));
			mta_route_unref(route); /* from here */
			continue;
		}

		if (best)
			mta_route_unref(best); /* from here */
		best = route;
		log_debug("debug: mta-routing: selecting candidate route %s",
		    mta_route_to_text(route));
	}

	if (best)
		return (best);

	/* Order is important */
	if (seen == 0) {
		log_info("smtp-out: No MX found for %s",
		    mta_connector_to_text(c));
		c->flags |= CONNECTOR_ERROR_MX;
	}
	else if (limit_route) {
		log_debug("debug: mta: hit route limit");
		*limits |= CONNECTOR_LIMIT_ROUTE;
	}
	else if (limit_host) {
		log_debug("debug: mta: hit host limit");
		*limits |= CONNECTOR_LIMIT_HOST;
	}
	else if (tm) {
		if (tm > *nextconn)
			*nextconn = tm;
	}
	else if (family_mismatch) {
		log_info("smtp-out: Address family mismatch on %s",
		    mta_connector_to_text(c));
		c->flags |= CONNECTOR_ERROR_FAMILY;
	}
	else if (suspended_route) {
		log_info("smtp-out: No valid route for %s",
		    mta_connector_to_text(c));
		if (suspended_route & ROUTE_DISABLED_NET)
			c->flags |= CONNECTOR_ERROR_ROUTE_NET;
		if (suspended_route & ROUTE_DISABLED_SMTP)
			c->flags |= CONNECTOR_ERROR_ROUTE_SMTP;
	}

	return (NULL);
}

static void
mta_log(const struct mta_envelope *evp, const char *prefix, const char *source,
    const char *relay, const char *status)
{
	log_info("relay: %s for %016" PRIx64 ": session=%016"PRIx64", "
	    "from=<%s>, to=<%s>, rcpt=<%s>, source=%s, "
	    "relay=%s, delay=%s, stat=%s",
	    prefix,
	    evp->id,
	    evp->session,
	    evp->task->sender,
	    evp->dest,
	    evp->rcpt ? evp->rcpt : "-",
	    source ? source : "-",
	    relay,
	    duration_to_text(time(NULL) - evp->creation),
	    status);
}

static struct mta_relay *
mta_relay(struct envelope *e)
{
	struct mta_relay	 key, *r;

	bzero(&key, sizeof key);

	if (e->agent.mta.relay.flags & RELAY_BACKUP) {
		key.domain = mta_domain(e->dest.domain, 0);
		key.backupname = e->agent.mta.relay.hostname;
	} else if (e->agent.mta.relay.hostname[0]) {
		key.domain = mta_domain(e->agent.mta.relay.hostname, 1);
		key.flags |= RELAY_MX;
	} else {
		key.domain = mta_domain(e->dest.domain, 0);
		key.flags |= RELAY_TLS_OPTIONAL;
	}

	key.flags |= e->agent.mta.relay.flags;
	key.port = e->agent.mta.relay.port;
	key.cert = e->agent.mta.relay.cert;
	if (!key.cert[0])
		key.cert = NULL;
	key.authtable = e->agent.mta.relay.authtable;
	if (!key.authtable[0])
		key.authtable = NULL;
	key.authlabel = e->agent.mta.relay.authlabel;
	if (!key.authlabel[0])
		key.authlabel = NULL;
	key.sourcetable = e->agent.mta.relay.sourcetable;
	if (!key.sourcetable[0])
		key.sourcetable = NULL;
	key.helotable = e->agent.mta.relay.helotable;
	if (!key.helotable[0])
		key.helotable = NULL;

	if ((r = SPLAY_FIND(mta_relay_tree, &relays, &key)) == NULL) {
		r = xcalloc(1, sizeof *r, "mta_relay");
		TAILQ_INIT(&r->tasks);
		r->id = generate_uid();
		r->flags = key.flags;
		r->domain = key.domain;
		r->backupname = key.backupname ?
		    xstrdup(key.backupname, "mta: backupname") : NULL;
		r->backuppref = -1;
		r->port = key.port;
		r->cert = key.cert ? xstrdup(key.cert, "mta: cert") : NULL;
		if (key.authtable)
			r->authtable = xstrdup(key.authtable, "mta: authtable");
		if (key.authlabel)
			r->authlabel = xstrdup(key.authlabel, "mta: authlabel");
		if (key.sourcetable)
			r->sourcetable = xstrdup(key.sourcetable,
			    "mta: sourcetable");
		if (key.helotable)
			r->helotable = xstrdup(key.helotable,
			    "mta: helotable");
		SPLAY_INSERT(mta_relay_tree, &relays, r);
		stat_increment("mta.relay", 1);
	} else {
		mta_domain_unref(key.domain); /* from here */
	}

	r->refcount++;
	return (r);
}

static void
mta_relay_ref(struct mta_relay *r)
{
	r->refcount++;
}

static void
mta_relay_unref(struct mta_relay *relay)
{
	struct mta_connector	*c;

	if (--relay->refcount)
		return;

	log_debug("debug: mta: freeing %s", mta_relay_to_text(relay));
	SPLAY_REMOVE(mta_relay_tree, &relays, relay);

	while ((tree_poproot(&relay->connectors, NULL, (void**)&c)))
		mta_connector_free(c);

	free(relay->authlabel);
	free(relay->authtable);
	free(relay->backupname);
	free(relay->cert);
	free(relay->helotable);
	free(relay->secret);
	free(relay->sourcetable);

	mta_domain_unref(relay->domain); /* from constructor */
	free(relay);
	stat_decrement("mta.relay", 1);
}

const char *
mta_relay_to_text(struct mta_relay *relay)
{
	static char	 buf[1024];
	char		 tmp[32];
	const char	*sep = ",";

	snprintf(buf, sizeof buf, "[relay:%s", relay->domain->name);

	if (relay->port) {
		strlcat(buf, sep, sizeof buf);
		snprintf(tmp, sizeof tmp, "port=%i", (int)relay->port);
		strlcat(buf, tmp, sizeof buf);
	}

	if (relay->flags & RELAY_STARTTLS) {
		strlcat(buf, sep, sizeof buf);
		strlcat(buf, "starttls", sizeof buf);
	}

	if (relay->flags & RELAY_SMTPS) {
		strlcat(buf, sep, sizeof buf);
		strlcat(buf, "smtps", sizeof buf);
	}

	if (relay->flags & RELAY_AUTH) {
		strlcat(buf, sep, sizeof buf);
		strlcat(buf, "auth=", sizeof buf);
		strlcat(buf, relay->authtable, sizeof buf);
		strlcat(buf, ":", sizeof buf);
		strlcat(buf, relay->authlabel, sizeof buf);
	}

	if (relay->cert) {
		strlcat(buf, sep, sizeof buf);
		strlcat(buf, "cert=", sizeof buf);
		strlcat(buf, relay->cert, sizeof buf);
	}

	if (relay->flags & RELAY_MX) {
		strlcat(buf, sep, sizeof buf);
		strlcat(buf, "mx", sizeof buf);
	}

	if (relay->flags & RELAY_BACKUP) {
		strlcat(buf, sep, sizeof buf);
		strlcat(buf, "backup=", sizeof buf);
		strlcat(buf, relay->backupname, sizeof buf);
	}

	if (relay->sourcetable) {
		strlcat(buf, sep, sizeof buf);
		strlcat(buf, "sourcetable=", sizeof buf);
		strlcat(buf, relay->sourcetable, sizeof buf);
	}

	strlcat(buf, "]", sizeof buf);

	return (buf);
}

static int
mta_relay_cmp(const struct mta_relay *a, const struct mta_relay *b)
{
	int	r;

	if (a->domain < b->domain)
		return (-1);
	if (a->domain > b->domain)
		return (1);

	if (a->flags < b->flags)
		return (-1);
	if (a->flags > b->flags)
		return (1);

	if (a->port < b->port)
		return (-1);
	if (a->port > b->port)
		return (1);

	if (a->authtable == NULL && b->authtable)
		return (-1);
	if (a->authtable && b->authtable == NULL)
		return (1);
	if (a->authtable && ((r = strcmp(a->authtable, b->authtable))))
		return (r);
	if (a->authlabel && ((r = strcmp(a->authlabel, b->authlabel))))
		return (r);
	if (a->sourcetable == NULL && b->sourcetable)
		return (-1);
	if (a->sourcetable && b->sourcetable == NULL)
		return (1);
	if (a->sourcetable && ((r = strcmp(a->sourcetable, b->sourcetable))))
		return (r);

	if (a->cert == NULL && b->cert)
		return (-1);
	if (a->cert && b->cert == NULL)
		return (1);
	if (a->cert && ((r = strcmp(a->cert, b->cert))))
		return (r);

	if (a->backupname && ((r = strcmp(a->backupname, b->backupname))))
		return (r);

	return (0);
}

SPLAY_GENERATE(mta_relay_tree, mta_relay, entry, mta_relay_cmp);

static struct mta_host *
mta_host(const struct sockaddr *sa)
{
	struct mta_host		key, *h;
	struct sockaddr_storage	ss;

	memmove(&ss, sa, sa->sa_len);
	key.sa = (struct sockaddr*)&ss;
	h = SPLAY_FIND(mta_host_tree, &hosts, &key);

	if (h == NULL) {
		h = xcalloc(1, sizeof(*h), "mta_host");
		h->sa = xmemdup(sa, sa->sa_len, "mta_host");
		SPLAY_INSERT(mta_host_tree, &hosts, h);
		stat_increment("mta.host", 1);
	}

	h->refcount++;
	return (h);
}

static void
mta_host_ref(struct mta_host *h)
{
	h->refcount++;
}

static void
mta_host_unref(struct mta_host *h)
{
	if (--h->refcount)
		return;

	SPLAY_REMOVE(mta_host_tree, &hosts, h);
	free(h->sa);
	free(h->ptrname);
	free(h);
	stat_decrement("mta.host", 1);
}

const char *
mta_host_to_text(struct mta_host *h)
{
	static char buf[1024];

	if (h->ptrname)
		snprintf(buf, sizeof buf, "%s (%s)",
		    sa_to_text(h->sa), h->ptrname);
	else
		snprintf(buf, sizeof buf, "%s", sa_to_text(h->sa));

	return (buf);
}

static int
mta_host_cmp(const struct mta_host *a, const struct mta_host *b)
{
	if (a->sa->sa_len < b->sa->sa_len)
		return (-1);
	if (a->sa->sa_len > b->sa->sa_len)
		return (1);
	return (memcmp(a->sa, b->sa, a->sa->sa_len));
}

SPLAY_GENERATE(mta_host_tree, mta_host, entry, mta_host_cmp);

static struct mta_domain *
mta_domain(char *name, int flags)
{
	struct mta_domain	key, *d;

	key.name = name;
	key.flags = flags;
	d = SPLAY_FIND(mta_domain_tree, &domains, &key);

	if (d == NULL) {
		d = xcalloc(1, sizeof(*d), "mta_domain");
		d->name = xstrdup(name, "mta_domain");
		d->flags = flags;
		TAILQ_INIT(&d->mxs);
		SPLAY_INSERT(mta_domain_tree, &domains, d);
		stat_increment("mta.domain", 1);
	}

	d->refcount++;
	return (d);
}

#if 0
static void
mta_domain_ref(struct mta_domain *d)
{
	d->refcount++;
}
#endif

static void
mta_domain_unref(struct mta_domain *d)
{
	struct mta_mx	*mx;

	if (--d->refcount)
		return;

	while ((mx = TAILQ_FIRST(&d->mxs))) {
		TAILQ_REMOVE(&d->mxs, mx, entry);
		mta_host_unref(mx->host); /* from IMSG_DNS_HOST */
		free(mx);
	}

	SPLAY_REMOVE(mta_domain_tree, &domains, d);
	free(d->name);
	free(d);
	stat_decrement("mta.domain", 1);
}

static int
mta_domain_cmp(const struct mta_domain *a, const struct mta_domain *b)
{
	if (a->flags < b->flags)
		return (-1);
	if (a->flags > b->flags)
		return (1);
	return (strcasecmp(a->name, b->name));
}

SPLAY_GENERATE(mta_domain_tree, mta_domain, entry, mta_domain_cmp);

static struct mta_source *
mta_source(const struct sockaddr *sa)
{
	struct mta_source	key, *s;
	struct sockaddr_storage	ss;

	if (sa) {
		memmove(&ss, sa, sa->sa_len);
		key.sa = (struct sockaddr*)&ss;
	} else
		key.sa = NULL;
	s = SPLAY_FIND(mta_source_tree, &sources, &key);

	if (s == NULL) {
		s = xcalloc(1, sizeof(*s), "mta_source");
		if (sa)
			s->sa = xmemdup(sa, sa->sa_len, "mta_source");
		SPLAY_INSERT(mta_source_tree, &sources, s);
		stat_increment("mta.source", 1);
	}

	s->refcount++;
	return (s);
}

static void
mta_source_ref(struct mta_source *s)
{
	s->refcount++;
}

static void
mta_source_unref(struct mta_source *s)
{
	if (--s->refcount)
		return;

	SPLAY_REMOVE(mta_source_tree, &sources, s);
	free(s->sa);
	free(s);
	stat_decrement("mta.source", 1);
}

static const char *
mta_source_to_text(struct mta_source *s)
{
	static char buf[1024];

	if (s->sa == NULL)
		return "[]";
	snprintf(buf, sizeof buf, "%s", sa_to_text(s->sa));
	return (buf);
}

static int
mta_source_cmp(const struct mta_source *a, const struct mta_source *b)
{
	if (a->sa == NULL)
		return ((b->sa == NULL) ? 0 : -1);
	if (b->sa == NULL)
		return (1);
	if (a->sa->sa_len < b->sa->sa_len)
		return (-1);
	if (a->sa->sa_len > b->sa->sa_len)
		return (1);
	return (memcmp(a->sa, b->sa, a->sa->sa_len));
}

SPLAY_GENERATE(mta_source_tree, mta_source, entry, mta_source_cmp);

static struct mta_connector *
mta_connector(struct mta_relay *relay, struct mta_source *source)
{
	struct mta_connector	*c;

	c = tree_get(&relay->connectors, (uintptr_t)(source));
	if (c == NULL) {
		c = xcalloc(1, sizeof(*c), "mta_connector");
		c->relay = relay;
		c->source = source;
		c->flags |= CONNECTOR_NEW;
		mta_source_ref(source);
		tree_xset(&relay->connectors, (uintptr_t)(source), c);
		stat_increment("mta.connector", 1);
		log_debug("debug: mta: new %s", mta_connector_to_text(c));
	}

	return (c);
}

static void
mta_connector_free(struct mta_connector *c)
{
	log_debug("debug: mta: freeing %s",
	    mta_connector_to_text(c));

	if (c->flags & CONNECTOR_WAIT) {
		log_debug("debug: mta: canceling timeout for %s",
		    mta_connector_to_text(c));
		runq_cancel(runq_connector, NULL, c);
	}
	mta_source_unref(c->source); /* from constructor */
	free(c);

	stat_decrement("mta.connector", 1);
}

static const char *
mta_connector_to_text(struct mta_connector *c)
{
	static char buf[1024];

	snprintf(buf, sizeof buf, "[connector:%s->%s,0x%x]",
	    mta_source_to_text(c->source),
	    mta_relay_to_text(c->relay),
	    c->flags);
	return (buf);
}

static struct mta_route *
mta_route(struct mta_source *src, struct mta_host *dst)
{
	struct mta_route	key, *r;
	static uint64_t		rid = 0;

	key.src = src;
	key.dst = dst;
	r = SPLAY_FIND(mta_route_tree, &routes, &key);

	if (r == NULL) {
		r = xcalloc(1, sizeof(*r), "mta_route");
		r->src = src;
		r->dst = dst;
		r->flags |= ROUTE_NEW;
		r->id = ++rid;
		SPLAY_INSERT(mta_route_tree, &routes, r);
		mta_source_ref(src);
		mta_host_ref(dst);
		stat_increment("mta.route", 1);
	}
	else if (r->flags & ROUTE_RUNQ) {
		log_debug("debug: mta: mta_route_ref(): canceling runq for route %s",
		    mta_route_to_text(r));
		r->flags &= ~(ROUTE_RUNQ | ROUTE_KEEPALIVE);
		runq_cancel(runq_route, NULL, r);
		r->refcount--; /* from mta_route_unref() */
	}

	r->refcount++;
	return (r);
}

static void
mta_route_ref(struct mta_route *r)
{
	r->refcount++;
}

static void
mta_route_unref(struct mta_route *r)
{
	time_t	sched, now;
	int	delay;

	if (--r->refcount)
		return;

	/*
	 * Nothing references this route, but we might want to keep it alive
	 * for a while.
	 */
	now = time(NULL);
	sched = 0;

	if (r->penalty) {
#if DELAY_QUADRATIC
		delay = DELAY_ROUTE_BASE * r->penalty * r->penalty;
#else
		delay = 15 * 60;
#endif
		if (delay > DELAY_ROUTE_MAX)
			delay = DELAY_ROUTE_MAX;
		sched = r->lastpenalty + delay;
		log_debug("debug: mta: mta_route_unref(): keeping route %s alive for %llus (penalty %i)",
		    mta_route_to_text(r), (unsigned long long) sched - now, r->penalty);
	} else if (!(r->flags & ROUTE_KEEPALIVE)) {
		if (r->lastconn + max_seen_conndelay_route > now)
			sched = r->lastconn + max_seen_conndelay_route;
		if (r->lastdisc + max_seen_discdelay_route > now &&
		    r->lastdisc + max_seen_discdelay_route < sched)
			sched = r->lastdisc + max_seen_discdelay_route;

		if (sched > now)
			log_debug("debug: mta: mta_route_unref(): keeping route %s alive for %llus (imposed delay)",
			    mta_route_to_text(r), (unsigned long long) sched - now);
	}

	if (sched > now) {
		r->flags |= ROUTE_RUNQ;
		runq_schedule(runq_route, sched, NULL, r);
		r->refcount++;
		return;
	}

	log_debug("debug: mta: ma_route_unref(): really discarding route %s",
	    mta_route_to_text(r));

	SPLAY_REMOVE(mta_route_tree, &routes, r);
	mta_source_unref(r->src); /* from constructor */
	mta_host_unref(r->dst); /* from constructor */
	free(r);
	stat_decrement("mta.route", 1);
}

static const char *
mta_route_to_text(struct mta_route *r)
{
	static char	buf[1024];

	snprintf(buf, sizeof buf, "%s <-> %s",
	    mta_source_to_text(r->src),
	    mta_host_to_text(r->dst));

	return (buf);
}

static int
mta_route_cmp(const struct mta_route *a, const struct mta_route *b)
{
	if (a->src < b->src)
		return (-1);
	if (a->src > b->src)
		return (1);

	if (a->dst < b->dst)
		return (-1);
	if (a->dst > b->dst)
		return (1);

	return (0);
}

SPLAY_GENERATE(mta_route_tree, mta_route, entry, mta_route_cmp);


/* hoststat errors are not critical, we do best effort */
void
mta_hoststat_update(const char *host, const char *error)
{
	struct hoststat	*hs = NULL;
	char		 buf[SMTPD_MAXHOSTNAMELEN];
	time_t		 tm;

	if (! lowercase(buf, host, sizeof buf))
		return;

	tm = time(NULL);
	hs = dict_get(&hoststat, buf);
	if (hs == NULL) {
		hs = calloc(1, sizeof *hs);
		if (hs == NULL)
			return;
		tree_init(&hs->deferred);
		runq_schedule(runq_hoststat, tm+HOSTSTAT_EXPIRE_DELAY, NULL, hs);
	}
	strlcpy(hs->name, buf, sizeof hs->name);
	strlcpy(hs->error, error, sizeof hs->error);
	hs->tm = time(NULL);
	dict_set(&hoststat, buf, hs);

	runq_cancel(runq_hoststat, NULL, hs);
	runq_schedule(runq_hoststat, tm+HOSTSTAT_EXPIRE_DELAY, NULL, hs);
}

void
mta_hoststat_cache(const char *host, uint64_t evpid)
{
	struct hoststat	*hs = NULL;
	char buf[SMTPD_MAXHOSTNAMELEN];

	if (! lowercase(buf, host, sizeof buf))
		return;

	hs = dict_get(&hoststat, buf);
	if (hs == NULL)
		return;

	tree_set(&hs->deferred, evpid, NULL);
}

void
mta_hoststat_uncache(const char *host, uint64_t evpid)
{
	struct hoststat	*hs = NULL;
	char buf[SMTPD_MAXHOSTNAMELEN];

	if (! lowercase(buf, host, sizeof buf))
		return;

	hs = dict_get(&hoststat, buf);
	if (hs == NULL)
		return;

	tree_pop(&hs->deferred, evpid);
}

void
mta_hoststat_reschedule(const char *host)
{
	struct hoststat	*hs = NULL;
	char		 buf[SMTPD_MAXHOSTNAMELEN];
	uint64_t	 evpid;

	if (! lowercase(buf, host, sizeof buf))
		return;

	hs = dict_get(&hoststat, buf);
	if (hs == NULL)
		return;

	while (tree_poproot(&hs->deferred, &evpid, NULL)) {
		m_compose(p_queue, IMSG_MTA_SCHEDULE, 0, 0, -1,
		    &evpid, sizeof evpid);
	}
}

static void
mta_hoststat_remove_entry(struct hoststat *hs)
{
	while (tree_poproot(&hs->deferred, NULL, NULL))
		;
	dict_pop(&hoststat, hs->name);
	runq_cancel(runq_hoststat, NULL, hs);
}
