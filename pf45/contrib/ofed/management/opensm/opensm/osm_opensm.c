/*
 * Copyright (c) 2004-2008 Voltaire, Inc. All rights reserved.
 * Copyright (c) 2002-2006 Mellanox Technologies LTD. All rights reserved.
 * Copyright (c) 1996-2003 Intel Corporation. All rights reserved.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */

/*
 * Abstract:
 *    Implementation of osm_opensm_t.
 * This object represents the opensm super object.
 * This object is part of the opensm family of objects.
 */

#if HAVE_CONFIG_H
#  include <config.h>
#endif				/* HAVE_CONFIG_H */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <complib/cl_dispatcher.h>
#include <complib/cl_passivelock.h>
#include <vendor/osm_vendor_api.h>
#include <opensm/osm_version.h>
#include <opensm/osm_base.h>
#include <opensm/osm_opensm.h>
#include <opensm/osm_log.h>
#include <opensm/osm_subnet.h>
#include <opensm/osm_sm.h>
#include <opensm/osm_vl15intf.h>
#include <opensm/osm_event_plugin.h>

struct routing_engine_module {
	const char *name;
	int (*setup) (struct osm_routing_engine *, osm_opensm_t *);
};

extern int osm_ucast_minhop_setup(struct osm_routing_engine *, osm_opensm_t *);
extern int osm_ucast_updn_setup(struct osm_routing_engine *, osm_opensm_t *);
extern int osm_ucast_file_setup(struct osm_routing_engine *, osm_opensm_t *);
extern int osm_ucast_ftree_setup(struct osm_routing_engine *, osm_opensm_t *);
extern int osm_ucast_lash_setup(struct osm_routing_engine *, osm_opensm_t *);
extern int osm_ucast_dor_setup(struct osm_routing_engine *, osm_opensm_t *);

const static struct routing_engine_module routing_modules[] = {
	{"minhop", osm_ucast_minhop_setup},
	{"updn", osm_ucast_updn_setup},
	{"file", osm_ucast_file_setup},
	{"ftree", osm_ucast_ftree_setup},
	{"lash", osm_ucast_lash_setup},
	{"dor", osm_ucast_dor_setup},
	{NULL, NULL}
};

/**********************************************************************
 **********************************************************************/
const char *osm_routing_engine_type_str(IN osm_routing_engine_type_t type)
{
	switch (type) {
	case OSM_ROUTING_ENGINE_TYPE_NONE:
		return "none";
	case OSM_ROUTING_ENGINE_TYPE_MINHOP:
		return "minhop";
	case OSM_ROUTING_ENGINE_TYPE_UPDN:
		return "updn";
	case OSM_ROUTING_ENGINE_TYPE_FILE:
		return "file";
	case OSM_ROUTING_ENGINE_TYPE_FTREE:
		return "ftree";
	case OSM_ROUTING_ENGINE_TYPE_LASH:
		return "lash";
	case OSM_ROUTING_ENGINE_TYPE_DOR:
		return "dor";
	default:
		break;
	}
	return "unknown";
}

/**********************************************************************
 **********************************************************************/
osm_routing_engine_type_t osm_routing_engine_type(IN const char *str)
{
	/* For legacy reasons, consider a NULL pointer and the string
	 * "null" as the minhop routing engine.
	 */
	if (!str || !strcasecmp(str, "null")
	    || !strcasecmp(str, "minhop"))
		return OSM_ROUTING_ENGINE_TYPE_MINHOP;
	else if (!strcasecmp(str, "none"))
		return OSM_ROUTING_ENGINE_TYPE_NONE;
	else if (!strcasecmp(str, "updn"))
		return OSM_ROUTING_ENGINE_TYPE_UPDN;
	else if (!strcasecmp(str, "file"))
		return OSM_ROUTING_ENGINE_TYPE_FILE;
	else if (!strcasecmp(str, "ftree"))
		return OSM_ROUTING_ENGINE_TYPE_FTREE;
	else if (!strcasecmp(str, "lash"))
		return OSM_ROUTING_ENGINE_TYPE_LASH;
	else if (!strcasecmp(str, "dor"))
		return OSM_ROUTING_ENGINE_TYPE_DOR;
	else
		return OSM_ROUTING_ENGINE_TYPE_UNKNOWN;
}

/**********************************************************************
 **********************************************************************/
static void append_routing_engine(osm_opensm_t *osm,
				  struct osm_routing_engine *routing_engine)
{
	struct osm_routing_engine *r;

	routing_engine->next = NULL;

	if (!osm->routing_engine_list) {
		osm->routing_engine_list = routing_engine;
		return;
	}

	r = osm->routing_engine_list;
	while (r->next)
		r = r->next;

	r->next = routing_engine;
}

static void setup_routing_engine(osm_opensm_t *osm, const char *name)
{
	struct osm_routing_engine *re;
	const struct routing_engine_module *m;

	for (m = routing_modules; m->name && *m->name; m++) {
		if (!strcmp(m->name, name)) {
			re = malloc(sizeof(struct osm_routing_engine));
			if (!re) {
				OSM_LOG(&osm->log, OSM_LOG_VERBOSE,
					"memory allocation failed\n");
				return;
			}
			memset(re, 0, sizeof(struct osm_routing_engine));

			re->name = m->name;
			if (m->setup(re, osm)) {
				OSM_LOG(&osm->log, OSM_LOG_VERBOSE,
					"setup of routing"
					" engine \'%s\' failed\n", name);
				return;
			}
			OSM_LOG(&osm->log, OSM_LOG_DEBUG,
				"\'%s\' routing engine set up\n", re->name);
			append_routing_engine(osm, re);
			return;
		}
	}

	OSM_LOG(&osm->log, OSM_LOG_ERROR,
		"cannot find or setup routing engine \'%s\'", name);
}

static void setup_routing_engines(osm_opensm_t *osm, const char *engine_names)
{
	char *name, *str, *p;

	if (!engine_names || !*engine_names) {
		setup_routing_engine(osm, "minhop");
		return;
	}

	str = strdup(engine_names);
	name = strtok_r(str, ", \t\n", &p);
	while (name && *name) {
		setup_routing_engine(osm, name);
		name = strtok_r(NULL, ", \t\n", &p);
	}
	free(str);

	if (!osm->routing_engine_list)
		setup_routing_engine(osm, "minhop");
}

/**********************************************************************
 **********************************************************************/
void osm_opensm_construct(IN osm_opensm_t * const p_osm)
{
	memset(p_osm, 0, sizeof(*p_osm));
	p_osm->osm_version = OSM_VERSION;
	osm_subn_construct(&p_osm->subn);
	osm_sm_construct(&p_osm->sm);
	osm_sa_construct(&p_osm->sa);
	osm_db_construct(&p_osm->db);
	osm_mad_pool_construct(&p_osm->mad_pool);
	osm_vl15_construct(&p_osm->vl15);
	osm_log_construct(&p_osm->log);
}

/**********************************************************************
 **********************************************************************/
static void destroy_routing_engines(osm_opensm_t *osm)
{
	struct osm_routing_engine *r, *next;

	next = osm->routing_engine_list;
	while (next) {
		r = next;
		next = r->next;
		if (r->delete)
			r->delete(r->context);
		free(r);
	}
}

/**********************************************************************
 **********************************************************************/
static void destroy_plugins(osm_opensm_t *osm)
{
	osm_epi_plugin_t *p;
	/* remove from the list, and destroy it */
	while (!cl_is_qlist_empty(&osm->plugin_list)){
		p = (osm_epi_plugin_t *)cl_qlist_remove_head(&osm->plugin_list);
		/* plugin is responsible for freeing its own resources */
		osm_epi_destroy(p);
	}
}

void osm_opensm_destroy(IN osm_opensm_t * const p_osm)
{
	/* in case of shutdown through exit proc - no ^C */
	osm_exit_flag = TRUE;

	/*
	 * First of all, clear the is_sm bit.
	 */
	if (p_osm->sm.mad_ctrl.h_bind)
		osm_vendor_set_sm(p_osm->sm.mad_ctrl.h_bind, FALSE);

#ifdef ENABLE_OSM_PERF_MGR
	/* Shutdown the PerfMgr */
	osm_perfmgr_shutdown(&p_osm->perfmgr);
#endif				/* ENABLE_OSM_PERF_MGR */

	/* shut down the SA
	 * - unbind from QP1 messages
	 */
	osm_sa_shutdown(&p_osm->sa);

	/* shut down the SM
	 * - make sure the SM sweeper thread exited
	 * - unbind from QP0 messages
	 */
	osm_sm_shutdown(&p_osm->sm);

	/* cleanup all messages on VL15 fifo that were not sent yet */
	osm_vl15_shutdown(&p_osm->vl15, &p_osm->mad_pool);

	/* shut down the dispatcher - so no new messages cross */
	cl_disp_shutdown(&p_osm->disp);

	/* dump SA DB */
	osm_sa_db_file_dump(p_osm);

	/* do the destruction in reverse order as init */
	destroy_plugins(p_osm);
	destroy_routing_engines(p_osm);
	osm_sa_destroy(&p_osm->sa);
	osm_sm_destroy(&p_osm->sm);
#ifdef ENABLE_OSM_PERF_MGR
	osm_perfmgr_destroy(&p_osm->perfmgr);
#endif				/* ENABLE_OSM_PERF_MGR */
	osm_db_destroy(&p_osm->db);
	osm_vl15_destroy(&p_osm->vl15, &p_osm->mad_pool);
	osm_mad_pool_destroy(&p_osm->mad_pool);
	osm_vendor_delete(&p_osm->p_vendor);
	osm_subn_destroy(&p_osm->subn);
	cl_disp_destroy(&p_osm->disp);
#ifdef HAVE_LIBPTHREAD
	pthread_cond_destroy(&p_osm->stats.cond);
	pthread_mutex_destroy(&p_osm->stats.mutex);
#else
	cl_event_destroy(&p_osm->stats.event);
#endif
	close_node_name_map(p_osm->node_name_map);

	cl_plock_destroy(&p_osm->lock);

	osm_log_destroy(&p_osm->log);
}

static void load_plugins(osm_opensm_t *osm, const char *plugin_names)
{
	osm_epi_plugin_t *epi;
	char *p_names, *name, *p;

	p_names = strdup(plugin_names);
	name = strtok_r(p_names, " \t\n", &p);
	while (name && *name) {
		epi = osm_epi_construct(osm, name);
		if (!epi)
			osm_log(&osm->log, OSM_LOG_ERROR,
				"cannot load plugin \'%s\'\n", name);
		else
			cl_qlist_insert_tail(&osm->plugin_list, &epi->list);
		name = strtok_r(NULL, " \t\n", &p);
	}
	free(p_names);
}

/**********************************************************************
 **********************************************************************/
ib_api_status_t
osm_opensm_init(IN osm_opensm_t * const p_osm,
		IN const osm_subn_opt_t * const p_opt)
{
	ib_api_status_t status;

	/* Can't use log macros here, since we're initializing the log */
	osm_opensm_construct(p_osm);

	if (p_opt->daemon)
		p_osm->log.daemon = 1;

	status = osm_log_init_v2(&p_osm->log, p_opt->force_log_flush,
				 p_opt->log_flags, p_opt->log_file,
				 p_opt->log_max_size, p_opt->accum_log_file);
	if (status != IB_SUCCESS)
		return (status);

	/* If there is a log level defined - add the OSM_VERSION to it */
	osm_log(&p_osm->log,
		osm_log_get_level(&p_osm->log) & (OSM_LOG_SYS ^ 0xFF), "%s\n",
		p_osm->osm_version);
	/* Write the OSM_VERSION to the SYS_LOG */
	osm_log(&p_osm->log, OSM_LOG_SYS, "%s\n", p_osm->osm_version);	/* Format Waived */

	OSM_LOG(&p_osm->log, OSM_LOG_FUNCS, "[\n");	/* Format Waived */

	status = cl_plock_init(&p_osm->lock);
	if (status != IB_SUCCESS)
		goto Exit;

#ifdef HAVE_LIBPTHREAD
	pthread_mutex_init(&p_osm->stats.mutex, NULL);
	pthread_cond_init(&p_osm->stats.cond, NULL);
#else
	status = cl_event_init(&p_osm->stats.event, FALSE);
	if (status != IB_SUCCESS)
		goto Exit;
#endif

	if (p_opt->single_thread) {
		OSM_LOG(&p_osm->log, OSM_LOG_INFO,
			"Forcing single threaded dispatcher\n");
		status = cl_disp_init(&p_osm->disp, 1, "opensm");
	} else {
		/*
		 * Normal behavior is to initialize the dispatcher with
		 * one thread per CPU, as specified by a thread count of '0'.
		 */
		status = cl_disp_init(&p_osm->disp, 0, "opensm");
	}
	if (status != IB_SUCCESS)
		goto Exit;

	status = osm_subn_init(&p_osm->subn, p_osm, p_opt);
	if (status != IB_SUCCESS)
		goto Exit;

	p_osm->p_vendor =
	    osm_vendor_new(&p_osm->log, p_opt->transaction_timeout);
	if (p_osm->p_vendor == NULL) {
		status = IB_INSUFFICIENT_RESOURCES;
		goto Exit;
	}

	status = osm_mad_pool_init(&p_osm->mad_pool);
	if (status != IB_SUCCESS)
		goto Exit;

	status = osm_vl15_init(&p_osm->vl15, p_osm->p_vendor,
			       &p_osm->log, &p_osm->stats,
			       p_opt->max_wire_smps);
	if (status != IB_SUCCESS)
		goto Exit;

	/* the DB is in use by the SM and SA so init before */
	status = osm_db_init(&p_osm->db, &p_osm->log);
	if (status != IB_SUCCESS)
		goto Exit;

	status = osm_sm_init(&p_osm->sm, &p_osm->subn, &p_osm->db,
			     p_osm->p_vendor, &p_osm->mad_pool, &p_osm->vl15,
			     &p_osm->log, &p_osm->stats, &p_osm->disp,
			     &p_osm->lock);

	if (status != IB_SUCCESS)
		goto Exit;

	status = osm_sa_init(&p_osm->sm, &p_osm->sa, &p_osm->subn,
			     p_osm->p_vendor, &p_osm->mad_pool, &p_osm->log,
			     &p_osm->stats, &p_osm->disp, &p_osm->lock);

	if (status != IB_SUCCESS)
		goto Exit;

	cl_qlist_init(&p_osm->plugin_list);

	if (p_opt->event_plugin_name)
		load_plugins(p_osm, p_opt->event_plugin_name);

#ifdef ENABLE_OSM_PERF_MGR
	status = osm_perfmgr_init(&p_osm->perfmgr, p_osm, p_opt);
	if (status != IB_SUCCESS)
		goto Exit;
#endif				/* ENABLE_OSM_PERF_MGR */

	setup_routing_engines(p_osm, p_opt->routing_engine_names);

	p_osm->routing_engine_used = OSM_ROUTING_ENGINE_TYPE_NONE;

	p_osm->node_name_map = open_node_name_map(p_opt->node_name_map_name);

Exit:
	OSM_LOG(&p_osm->log, OSM_LOG_FUNCS, "]\n");	/* Format Waived */
	return (status);
}

/**********************************************************************
 **********************************************************************/
ib_api_status_t
osm_opensm_bind(IN osm_opensm_t * const p_osm, IN const ib_net64_t guid)
{
	ib_api_status_t status;

	OSM_LOG_ENTER(&p_osm->log);

	status = osm_sm_bind(&p_osm->sm, guid);
	if (status != IB_SUCCESS)
		goto Exit;

	status = osm_sa_bind(&p_osm->sa, guid);
	if (status != IB_SUCCESS)
		goto Exit;

#ifdef ENABLE_OSM_PERF_MGR
	status = osm_perfmgr_bind(&p_osm->perfmgr, guid);
	if (status != IB_SUCCESS)
		goto Exit;
#endif				/* ENABLE_OSM_PERF_MGR */

Exit:
	OSM_LOG_EXIT(&p_osm->log);
	return (status);
}

/**********************************************************************
 **********************************************************************/
void osm_opensm_report_event(osm_opensm_t *osm, osm_epi_event_id_t event_id,
			     void *event_data)
{
	cl_list_item_t *item;

	for (item = cl_qlist_head(&osm->plugin_list);
	     item != cl_qlist_end(&osm->plugin_list);
	     item = cl_qlist_next(item)) {
		osm_epi_plugin_t *p = (osm_epi_plugin_t *)item;
		if (p->impl->report)
			p->impl->report(p->plugin_data, event_id, event_data);
	}
}
