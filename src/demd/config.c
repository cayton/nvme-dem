/*
 * NVMe over Fabrics Distributed Endpoint Manager (NVMe-oF DEM).
 * Copyright (c) 2017-2018 Intel Corporation, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "common.h"

#include "ops.h"
#include "curl.h"

#define CONFIG_ERR " - unable to configure remote target"

static inline char *trtype_str(u8 trtype)
{
	switch (trtype) {
	case NVMF_TRTYPE_RDMA:
		return TRTYPE_STR_RDMA;
	case NVMF_TRTYPE_FC:
		return TRTYPE_STR_FC;
	case NVMF_TRTYPE_TCP:
		return TRTYPE_STR_TCP;
	default:
		return "unknown";
	}
}

static inline char *adrfam_str(u8 adrfam)
{
	switch (adrfam) {
	case NVMF_ADDR_FAMILY_IP4:
		return ADRFAM_STR_IPV4;
	case NVMF_ADDR_FAMILY_IP6:
		return ADRFAM_STR_IPV6;
	case NVMF_ADDR_FAMILY_FC:
		return ADRFAM_STR_FC;
	default:
		return "unknown";
	}
}

static inline u8 to_trtype(char *str)
{
	if (strcmp(str, TRTYPE_STR_RDMA) == 0)
		return NVMF_TRTYPE_RDMA;
	if (strcmp(str, TRTYPE_STR_FC) == 0)
		return NVMF_TRTYPE_FC;
	if (strcmp(str, TRTYPE_STR_TCP) == 0)
		return NVMF_TRTYPE_TCP;
	return 0;
}

static inline u8 to_adrfam(char *str)
{
	if (strcmp(str, ADRFAM_STR_IPV4) == 0)
		return NVMF_ADDR_FAMILY_IP4;
	if (strcmp(str, ADRFAM_STR_IPV6) == 0)
		return NVMF_ADDR_FAMILY_IP6;
	if (strcmp(str, ADRFAM_STR_FC) == 0)
		return NVMF_ADDR_FAMILY_FC;
	return 0;
}

int get_mgmt_mode(char *mode)
{
	int			 mgmt_mode;

	if (strcmp(mode, TAG_OUT_OF_BAND_MGMT) == 0)
		mgmt_mode = OUT_OF_BAND_MGMT;
	else if (strcmp(mode, TAG_IN_BAND_MGMT) == 0)
		mgmt_mode = IN_BAND_MGMT;
	else
		mgmt_mode = LOCAL_MGMT;

	return mgmt_mode;
}

static inline int get_uri(struct target *target, char *uri)
{
	char			*addr = target->oob_iface.address;
	int			 port = target->oob_iface.port;

	return sprintf(uri, "http://%s:%d/", addr, port);
}

static inline struct target *find_target(char *alias)
{
	struct target		*target;

	list_for_each_entry(target, target_list, node)
		if (!strcmp(target->alias, alias))
			return target;
	return NULL;
}

static inline struct subsystem *find_subsys(struct target *target, char *nqn)
{
	struct subsystem	*subsys;

	list_for_each_entry(subsys, &target->subsys_list, node)
		if (!strcmp(subsys->nqn, nqn))
			return subsys;
	return NULL;
}

static inline struct portid *find_portid(struct target *target, int id)
{
	struct portid		*portid;

	list_for_each_entry(portid, &target->portid_list, node)
		if (portid->portid == id)
			return portid;
	return NULL;
}

static inline struct ns *find_ns(struct subsystem *subsys, int nsid)
{
	struct ns		*ns;

	list_for_each_entry(ns, &subsys->ns_list, node)
		if (ns->nsid == nsid)
			return ns;
	return NULL;
}

static int build_set_port_inb(struct target *target,
			      struct nvmf_port_config_page_hdr **_hdr)
{
	struct nvmf_port_config_page_entry *entry;
	struct nvmf_port_config_page_hdr *hdr;
	struct portid		*portid;
	void			*ptr;
	int			 len = 0;
	int			 count = 0;

	list_for_each_entry(portid, &target->portid_list, node)
		count++;

	len = sizeof(hdr) - 1 + (count * sizeof(*entry));
	if (posix_memalign(&ptr, PAGE_SIZE, len)) {
		print_err("no memory for buffer, errno %d", errno);
		return 0;
	}

	hdr = ptr;
	hdr->num_entries = count;

	entry = (struct nvmf_port_config_page_entry *) &hdr->data;
	list_for_each_entry(portid, &target->portid_list, node) {
		if (!portid->valid)
			continue;

		entry->status = 0;

		entry->portid = portid->portid;
		entry->trtype = to_trtype(portid->type);
		entry->adrfam = to_adrfam(portid->family);
		strcpy(entry->traddr, portid->address);
		strcpy(entry->trsvcid, portid->port);
		entry++;
	}

	*_hdr = hdr;

	return len;
}

static int build_set_subsys_inb(struct target *target,
				struct nvmf_subsys_config_page_hdr **_hdr)
{
	struct nvmf_subsys_config_page_entry *entry;
	struct nvmf_subsys_config_page_hdr *hdr;
	struct subsystem	*subsystem;
	struct host		*host;
	char			*hostnqn;
	void			*ptr;
	int			 len = 0;
	int			 count = 0;

	len = sizeof(hdr) - 1;
	list_for_each_entry(subsystem, &target->subsys_list, node) {
		len += sizeof(*entry) - 1;
		count++;
		list_for_each_entry(host, &subsystem->host_list, node)
			len += NVMF_NQN_FIELD_LEN;
	}

	if (posix_memalign(&ptr, PAGE_SIZE, len)) {
		print_err("no memory for buffer, errno %d", errno);
		return 0;
	}

	hdr = ptr;
	hdr->num_entries = count;

	/* TODO do we validate subsystems? */
	entry = (struct nvmf_subsys_config_page_entry *) &hdr->data;
	list_for_each_entry(subsystem, &target->subsys_list, node) {
		entry->status = 0;
		entry->allowallhosts = subsystem->access;
		strcpy(entry->subnqn, subsystem->nqn);
		count = 0;
		hostnqn = (char *) &entry->data;
		list_for_each_entry(host, &subsystem->host_list, node) {
			strcpy(hostnqn, host->nqn);
			count++;
			hostnqn += NVMF_NQN_FIELD_LEN;
		}
		entry->numhosts = count;
		entry = (struct nvmf_subsys_config_page_entry *) hostnqn;
		print_debug("TODO need nsid info too");
	}

	*_hdr = hdr;

	return len;
}

static void build_set_port_oob(struct portid *portid, char *buf, int len)
{
	snprintf(buf, len, "{" JSSTR "," JSSTR "," JSSTR "," JSINDX "}",
		 TAG_TYPE, portid->type, TAG_FAMILY, portid->family,
		 TAG_ADDRESS, portid->address, TAG_TRSVCID, portid->port_num);
}

static void build_set_host_oob(char *nqn, char *buf, int len)
{
	snprintf(buf, len, "{" JSSTR "}", TAG_HOSTNQN, nqn);
}

static void build_set_subsys_oob(struct subsystem *subsys, char *buf, int len)
{
	snprintf(buf, len, "{" JSSTR "," JSINDX "}",
		 TAG_SUBNQN, subsys->nqn, TAG_ALLOW_ANY, subsys->access);
}

static void build_set_ns_oob(struct ns *ns, char *buf, int len)
{
	snprintf(buf, len, "{" JSINDX "," JSINDX "," JSINDX "}",
		 TAG_NSID, ns->nsid, TAG_DEVID, ns->devid,
		 TAG_DEVNSID, ns->devns);
}

static void build_set_portid_oob(int portid, char *buf, int len)
{
	snprintf(buf, len, "{" JSINDX "}", TAG_PORTID, portid);
}

static int send_get_config_oob(struct target *target, char *tag, char **buf)
{
	char			 uri[128];
	char			*p = uri;
	int			 len;

	len = get_uri(target, uri);
	p += len;

	strcpy(p, tag);

	return exec_get(uri, buf);
}

static int send_set_port_oob(struct target *target, char *buf, int portid)
{
	char			 uri[128];
	char			*p = uri;
	int			 len;

	len = get_uri(target, uri);
	p += len;

	sprintf(p, URI_PORTID "/%d", portid);

	return exec_post(uri, buf, strlen(buf));
}

static int send_set_config_oob(struct target *target, char *tag, char *buf)
{
	char			 uri[128];
	char			*p = uri;
	int			 len;

	len = get_uri(target, uri);
	p += len;

	strcpy(p, tag);

	return exec_post(uri, buf, strlen(buf));
}

static int send_update_subsys_oob(struct target *target, char *subsys,
				  char *tag, char *buf)
{
	char			 uri[128];
	char			*p = uri;
	int			 len;

	len = get_uri(target, uri);
	p += len;

	sprintf(p, URI_SUBSYSTEM "/%s/%s", subsys, tag);

	return exec_post(uri, buf, strlen(buf));
}

static inline int get_inb_nsdevs(struct target *target)
{
	struct nvmf_ns_devices_rsp_page_hdr *nsdevs_hdr;
	struct nvmf_ns_devices_rsp_page_entry *nsdev;
	struct nsdev		*ns_dev;
	int			 i;
	int			 ret;

	ret = send_get_nsdevs(&target->dq, &nsdevs_hdr);
	if (ret) {
		print_err("send get nsdevs INB failed for %s", target->alias);
		goto out1;
	}

	if (!nsdevs_hdr->num_entries) {
		print_err("No NS devices defined for %s", target->alias);
		goto out2;
	}

	nsdev = (struct nvmf_ns_devices_rsp_page_entry *) &nsdevs_hdr->data;

	list_for_each_entry(ns_dev, &target->device_list, node)
		ns_dev->valid = 0;

	for (i = nsdevs_hdr->num_entries; i > 0; i--, nsdev++) {
		list_for_each_entry(ns_dev, &target->device_list, node)
			if (ns_dev->nsdev == nsdev->dev_id &&
			    ns_dev->nsid == nsdev->ns_id) {
				ns_dev->valid = 1;
				goto found;
		}
		if (nsdev->dev_id == 255)
			print_err("New nsdev on %s - nullb0", target->alias);
		else
			print_err("New nsdev on %s - dev id %d nsid %d",
				  target->alias, nsdev->dev_id, nsdev->ns_id);
found:
		continue;
	}

	list_for_each_entry(ns_dev, &target->device_list, node)
		if (!ns_dev->valid) {
			if (nsdev->dev_id == 255)
				print_err("Nsdev not on %s - nullb0",
					  target->alias);
			else
				print_err("Nsdev not on %s - dev id %d nsid %d",
					  target->alias, ns_dev->nsdev,
					  ns_dev->nsid);
		}
out2:
	free(nsdevs_hdr);
out1:
	return ret;
}

static inline int get_inb_xports(struct target *target)
{
	struct nvmf_transports_rsp_page_hdr *xports_hdr;
	struct nvmf_transports_rsp_page_entry *xport;
	struct portid		*portid;
	int			 i, rdma_found;
	int			 ret;

	ret = send_get_xports(&target->dq, &xports_hdr);
	if (ret) {
		print_err("send get xports INB failed for %s", target->alias);
		goto out1;
	}

	if (!xports_hdr->num_entries) {
		print_err("No transports defined for %s", target->alias);
		goto out2;
	}

	xport = (struct nvmf_transports_rsp_page_entry *) &xports_hdr->data;

	list_for_each_entry(portid, &target->portid_list, node)
		portid->valid = 0;

	for (i = xports_hdr->num_entries; i > 0; i--, xport++) {
		rdma_found = 0;
		list_for_each_entry(portid, &target->portid_list, node) {
			if (!strcmp(xport->traddr, portid->address) &&
			    (xport->adrfam == to_adrfam(portid->family)) &&
			    (xport->trtype == to_trtype(portid->type))) {
				portid->valid = 1;
				if (xport->adrfam == NVMF_TRTYPE_RDMA)
					rdma_found = 1;
				else
					goto found;
			}

			if (!strcmp(xport->traddr, portid->address) &&
			    (xport->adrfam == to_adrfam(portid->family)) &&
			    (xport->trtype == NVMF_TRTYPE_RDMA &&
			     to_trtype(portid->type) == NVMF_TRTYPE_TCP)) {
				portid->valid = 1;
				if (!rdma_found)
					rdma_found = 1;
				else
					goto found;
			}
		}
		if (!rdma_found)
			print_err("New transport on %s - %s %s %s",
				  target->alias, trtype_str(xport->trtype),
				  adrfam_str(xport->adrfam), xport->traddr);
found:
		continue;
	}

	list_for_each_entry(portid, &target->portid_list, node)
		if (!portid->valid)
			print_err("Transport not on %s - %s %s %s",
				  target->alias, portid->type,
				  portid->family, portid->address);
out2:
	free(xports_hdr);
out1:
	return ret;
}

int get_inb_config(struct target *target)
{
	int			 ret;

	ret = get_inb_nsdevs(target);
	if (ret)
		return ret;

	return get_inb_xports(target);
}

int config_target_inb(struct target *target)
{
	struct nvmf_port_config_page_hdr   *port_hdr = NULL;
	struct nvmf_subsys_config_page_hdr *subsys_hdr = NULL;
	int			 len;
	int			 ret = 0;

	len = build_set_port_inb(target, &port_hdr);
	if (!len)
		print_err("build set port INB failed for %s", target->alias);
	else {
		ret = send_set_port_config(&target->dq, len, port_hdr);
		if (ret) {
			print_err("send set port INB failed for %s",
				  target->alias);
			goto out;
		}
	}

	len = build_set_subsys_inb(target, &subsys_hdr);
	if (!len)
		print_err("build set subsys INB failed for %s", target->alias);
	else {
		ret = send_set_subsys_config(&target->dq, len, subsys_hdr);
		if (ret)
			print_err("send set subsys INB failed for %s",
				  target->alias);
	}

	if (port_hdr)
		free(port_hdr);
out:
	if (subsys_hdr)
		free(subsys_hdr);

	return ret;
}

int add_group(char *group, char *resp)
{
	return add_json_group(group, resp);
}

int update_group(char *group, char *data, char *resp)
{
	return update_json_group(group, data, resp);
}

int set_group_member(char *group, char *data, char *tag, char *parent_tag,
		     char *resp)
{
	return set_json_group_member(group, data, tag, parent_tag, resp);
}

int del_group_member(char *group, char *member, char *tag, char *parent_tag,
		     char *resp)
{
	return del_json_group_member(group, member, tag, parent_tag, resp);
}

int del_group(char *group, char *resp)
{
	return del_json_group(group, resp);
}

int add_host(char *host, char *resp)
{
	int			 ret;

	ret = add_json_group(host, resp);

	return ret;
}

int update_host(char *host, char *data, char *resp)
{
	int			 ret;
	char			 hostnqn[MAX_NQN_SIZE + 1];

	ret = update_json_host(host, data, resp, hostnqn);

	print_debug("TODO walk subsys if nqn changed");
	/* if host name changes, we need to walk all subsystems to
	 * delete the old host and create the new host
	 */

	return ret;
}

static int send_del_host_inb(struct target *target, char *hostnqn)
{
	UNUSED(hostnqn);
	UNUSED(target);

	print_debug("TODO need to send INB del host");

	return 0;
}

static int send_del_host_oob(struct target *target, char *hostnqn)
{
	char			 uri[128];
	char			*p = uri;
	int			 len;

	len = get_uri(target, p);
	p += len;

	sprintf(p, URI_HOST "/%s", hostnqn);

	return exec_delete(uri);
}

static int send_del_acl_inb(struct subsystem *subsys, struct host *host)
{
	UNUSED(host);
	UNUSED(subsys);

	print_debug("TODO need to send INB del acl");

	return 0;
}

static int send_del_acl_oob(struct subsystem *subsys, struct host *host)
{
	char			 uri[128];
	char			*p = uri;
	int			 len;

	len = get_uri(subsys->target, p);
	p += len;

	sprintf(p, URI_SUBSYSTEM "/%s/" URI_HOST "/%s", subsys->nqn, host->nqn);

	return exec_delete(uri);
}

static void _del_acl(struct subsystem *subsys, struct host *host)
{
	struct target *target = subsys->target;

	if (target->mgmt_mode == IN_BAND_MGMT)
		send_del_acl_inb(subsys, host);
	else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
		send_del_acl_oob(subsys, host);

	list_del(&host->node);
}

static void _del_host(struct target  *target, char *hostnqn)
{
	if (target->mgmt_mode == IN_BAND_MGMT)
		send_del_host_inb(target, hostnqn);
	else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
		send_del_host_oob(target, hostnqn);
}

int del_host(char *hostnqn, char *resp)
{
	struct target		*target;
	struct subsystem	*subsys;
	struct host		*host;
	int			 ret;

	ret = del_json_host(hostnqn, resp);
	if (ret)
		return ret;

	list_for_each_entry(target, target_list, node) {
		list_for_each_entry(subsys, &target->subsys_list, node)
			list_for_each_entry(host, &subsys->host_list, node)
				if (!strcmp(host->nqn, hostnqn)) {
					_del_acl(subsys, host);
					break;
				}
		_del_host(target, hostnqn);
	}

	return 0;
}

static int send_set_subsys_inb(struct subsystem *subsys)
{
	UNUSED(subsys);

	print_debug("TODO need to send INB set subsys");

	return 0;
}

static int send_set_subsys_oob(struct subsystem *subsys)
{
	struct target		*target = subsys->target;
	char			*alias = target->alias;
	char			 buf[256];
	int			 ret;

	build_set_subsys_oob(subsys, buf, sizeof(buf));

	ret = send_set_config_oob(target, URI_SUBSYSTEM, buf);
	if (ret)
		print_err("set subsys OOB failed for %s", alias);

	return ret;
}

int set_subsys(char *alias, char *nqn, char *data, char *resp)
{
	struct target		*target;
	struct subsystem	*subsys;
	struct portid		*portid;
	struct subsystem	 new_ss;
	char			 buf[256];
	int			 ret;

	memset(&new_ss, 0, sizeof(new_ss));

	ret = set_json_subsys(alias, nqn, data, resp, &new_ss);
	if (ret)
		return ret;

	resp += strlen(resp);

	target = find_target(alias);
	if (!target)
		return -ENOENT;

	if (!nqn) {
		subsys = new_subsys(target, new_ss.nqn);
		if (!subsys)
			return -ENOMEM;

		subsys->access =  new_ss.access;
	} else {
		subsys = find_subsys(target, nqn);
		if (!subsys)
			return -ENOENT;

		if (target->mgmt_mode != LOCAL_MGMT &&
		    (strcmp(nqn, new_ss.nqn) ||
		     subsys->access != new_ss.access)) {
			print_debug("TODO delete old ss bofore send_set calls");
			strcpy(subsys->nqn, new_ss.nqn);
			subsys->access = new_ss.access;
		}
	}

	if (target->mgmt_mode == LOCAL_MGMT)
		return 0;

	if (target->mgmt_mode == IN_BAND_MGMT)
		ret = send_set_subsys_inb(subsys);
	else
		ret = send_set_subsys_oob(subsys);

	if (ret || nqn) {
		sprintf(resp, CONFIG_ERR);
		return ret;
	}

	list_for_each_entry(portid, &target->portid_list, node) {
		/* TODO this only works for OOB */
		build_set_portid_oob(portid->portid, buf, sizeof(buf));

		ret = send_update_subsys_oob(target, subsys->nqn, URI_PORTID,
					     buf);
		if (ret)
			print_err("set subsys portid OOB failed: %s", alias);
	}

	return ret;
}

static int send_del_subsys_inb(struct subsystem *subsys)
{
	UNUSED(subsys);

	print_debug("TODO need to send INB del subsys");

	return 0;
}

static int send_del_subsys_oob(struct subsystem *subsys)
{
	char			 uri[128];
	char			*p = uri;
	int			 len;

	len = get_uri(subsys->target, p);
	p += len;

	sprintf(p, URI_SUBSYSTEM "/%s", subsys->nqn);

	return exec_delete(uri);
}

int del_subsys(char *alias, char *nqn, char *resp)
{
	struct subsystem	*subsys;
	struct target		*target;
	int			 ret;

	ret = del_json_subsys(alias, nqn, resp);
	if (ret)
		goto out;

	target = find_target(alias);
	if (!target)
		goto out;

	subsys = find_subsys(target, nqn);
	if (!subsys)
		goto out;

	if (target->mgmt_mode == IN_BAND_MGMT)
		ret = send_del_subsys_inb(subsys);
	else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
		ret = send_del_subsys_oob(subsys);

	list_del(&subsys->node);
out:
	return ret;
}

/* PORTID */

static int send_del_portid_inb(struct target *target, struct portid *portid)
{
	UNUSED(target);
	UNUSED(portid);

	print_debug("TODO need to send INB del portid");

	return 0;
}

static int send_del_portid_oob(struct target *target, struct portid *portid)
{
	char			 uri[128];
	char			*p = uri;
	int			 len;

	len = get_uri(target, p);
	p += len;

	sprintf(p, URI_PORTID "/%d", portid->portid);

	return exec_delete(uri);
}

int del_portid(char *alias, int id, char *resp)
{
	struct target		*target;
	struct portid		*portid;
	int			 ret;

	ret = del_json_portid(alias, id, resp);
	if (ret)
		goto out;

	target = find_target(alias);
	if (!target)
		goto out;

	portid = find_portid(target, id);
	if (!portid)
		goto out;

	if (target->mgmt_mode == IN_BAND_MGMT)
		ret = send_del_portid_inb(target, portid);
	else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
		ret = send_del_portid_oob(target, portid);

	list_del(&portid->node);
out:
	return ret;
}

static int send_set_portid_inb(struct target *target, struct portid *portid)
{
	UNUSED(target);
	UNUSED(portid);

	print_debug("TODO need to send INB set portid");

	return 0;
}

static int send_set_portid_oob(struct target *target, struct portid *portid)
{
	int			ret;
	char			buf[256];

	build_set_port_oob(portid, buf, sizeof(buf));

	ret = send_set_port_oob(target, buf, portid->portid);
	if (ret)
		print_err("set port OOB failed for %s", target->alias);

	return ret;
}

int set_portid(char *alias, int id, char *data, char *resp)
{
	int			 ret;
	struct portid		 portid;
	struct target		*target;

	memset(&portid, 0, sizeof(portid));

	ret = set_json_portid(alias, id, data, resp, &portid);
	if (ret)
		goto out;

	resp += strlen(resp);

	target = find_target(alias);

	if (!target) {
		ret = -ENOENT;
		sprintf(resp, " - internal error: target not found");
		goto out;
	}

	if ((portid.portid != id) && id) {
		print_debug("TODO delete old port first");
		/* disconnect all subsystems, delete old port */
	}

	if (target->mgmt_mode == IN_BAND_MGMT)
		ret = send_set_portid_inb(target, &portid);
	else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
		ret = send_set_portid_oob(target, &portid);

	if (ret)
		sprintf(resp, CONFIG_ERR);

	print_debug("TODO: Connect all subsystems");
out:
	return ret;
}

/* NAMESPACE */

static int send_set_ns_inb(struct subsystem *subsys, struct ns *ns)
{
	UNUSED(subsys);
	UNUSED(ns);

	print_debug("TODO need to send INB set ns");

	return 0;
}

static int send_set_ns_oob(struct subsystem *subsys, struct ns *ns)
{
	char			 uri[128];
	char			 buf[256];
	char			*p = uri;
	int			 len;

	build_set_ns_oob(ns, buf, sizeof(buf));

	len = get_uri(subsys->target, p);
	p += len;

	sprintf(p, URI_SUBSYSTEM "/%s/" URI_NAMESPACE "/%d",
		subsys->nqn, ns->nsid);

	return exec_post(uri, buf, strlen(buf));
}

int set_ns(char *alias, char *nqn, char *data, char *resp)
{
	struct subsystem	*subsys;
	struct target		*target;
	struct ns		*ns;
	struct ns		 result;
	int			 ret;

	memset(&ns, 0, sizeof(ns));

	ret = set_json_ns(alias, nqn, data, resp, &result);
	if (ret)
		goto out;

	resp += strlen(resp);

	target = find_target(alias);
	if (!target)
		goto out;

	subsys = find_subsys(target, nqn);
	if (!subsys)
		goto out;

	ns = find_ns(subsys, result.nsid);
	if (!ns)
		ns = &result;
	else
		print_debug("TODO update ns structure");

	if (target->mgmt_mode == IN_BAND_MGMT)
		ret = send_set_ns_inb(subsys, ns);
	else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
		ret = send_set_ns_oob(subsys, ns);

	if (ret)
		sprintf(resp, CONFIG_ERR);
out:
	return ret;
}

static int send_del_ns_inb(struct subsystem *subsys, struct ns *ns)
{
	UNUSED(subsys);
	UNUSED(ns);

	print_debug("TODO need to send INB del ns");

	return 0;
}

static int send_del_ns_oob(struct subsystem *subsys, struct ns *ns)
{
	char			 uri[128];
	char			*p = uri;
	int			 len;

	len = get_uri(subsys->target, p);
	p += len;

	sprintf(p, URI_SUBSYSTEM "/%s/" URI_NAMESPACE "/%d",
		subsys->nqn, ns->nsid);

	return exec_delete(uri);
}

int del_ns(char *alias, char *nqn, int nsid, char *resp)
{
	struct subsystem	*subsys;
	struct target		*target;
	struct ns		*ns;
	int			 ret;

	ret = del_json_ns(alias, nqn, nsid, resp);
	if (ret)
		goto out;

	target = find_target(alias);
	if (!target)
		goto out;

	subsys = find_subsys(target, nqn);
	if (!subsys)
		goto out;

	ns = find_ns(subsys, nsid);
	if (!ns)
		goto out;

	if (target->mgmt_mode == IN_BAND_MGMT)
		ret = send_del_ns_inb(subsys, ns);
	else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
		ret = send_del_ns_oob(subsys, ns);

	list_del(&ns->node);
out:
	return ret;
}

/* TARGET */

static void notify_hosts(void)
{
	/* TODO Walk list of interested hosts and update them */
}

static int get_oob_nsdevs(struct target *target)
{
	char			*nsdevs;
	int			 ret;

	ret = send_get_config_oob(target, URI_NSDEV, &nsdevs);
	if (ret) {
		print_err("send get nsdevs OOB failed for %s", target->alias);
		return ret;
	}

	print_debug("TODO store info in target structure");

	free(nsdevs);

	return 0;
}

static int get_oob_xports(struct target *target)
{
	char			*xports;
	int			 ret;

	ret = send_get_config_oob(target, URI_INTERFACE, &xports);
	if (ret) {
		print_err("send get xports OOB failed for %s", target->alias);
		return ret;
	}

	print_debug("TODO store info in target structure");

	free(xports);

	return 0;
}

int get_oob_config(struct target *target)
{
	int			 ret;

	ret = get_oob_nsdevs(target);
	if (ret)
		return ret;

	return get_oob_xports(target);
}

static void config_subsys_oob(struct target *target, struct subsystem *subsys)
{
	struct ns		*ns;
	struct host		*host;
	struct portid		*portid;
	char			*alias = target->alias;
	char			*nqn = subsys->nqn;
	char			 buf[256];
	int			 ret;

	build_set_subsys_oob(subsys, buf, sizeof(buf));

	ret = send_set_config_oob(target, URI_SUBSYSTEM, buf);
	if (ret) {
		print_err("set subsys OOB failed for %s", alias);
		return;
	}

	list_for_each_entry(ns, &subsys->ns_list, node) {
		build_set_ns_oob(ns, buf, sizeof(buf));

		ret = send_update_subsys_oob(target, nqn, URI_NAMESPACE, buf);
		if (ret)
			print_err("set subsys ns OOB failed for %s", alias);
	}

	list_for_each_entry(host, &subsys->host_list, node) {
		build_set_host_oob(host->nqn, buf, sizeof(buf));

		ret = send_set_config_oob(target, URI_HOST, buf);
		if (ret) {
			print_err("set host OOB failed for %s", alias);
			continue;
		}

		ret = send_update_subsys_oob(target, nqn, URI_HOST, buf);
		if (ret)
			print_err("set subsys acl OOB failed for %s", alias);
	}

	list_for_each_entry(portid, &target->portid_list, node) {
		build_set_portid_oob(portid->portid, buf, sizeof(buf));

		ret = send_update_subsys_oob(target, nqn, URI_PORTID, buf);
		if (ret)
			print_err("set subsys portid OOB failed for %s", alias);
	}
}

int config_target_oob(struct target *target)
{
	struct portid		*portid;
	struct subsystem	*subsys;

	list_for_each_entry(portid, &target->portid_list, node)
		send_set_portid_oob(target, portid);

	list_for_each_entry(subsys, &target->subsys_list, node)
		config_subsys_oob(target, subsys);

	return 0;
}

int del_target(char *alias, char *resp)
{
	struct target		*target;
	struct subsystem	*subsys;
	struct portid		*portid;
	struct host		*host;

	int			 ret;

	ret = del_json_target(alias, resp);
	if (ret)
		goto out;

	target = find_target(alias);
	if (!target)
		goto out;

	list_for_each_entry(subsys, &target->subsys_list, node) {
		if (target->mgmt_mode == IN_BAND_MGMT)
			ret = send_del_subsys_inb(subsys);
		else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
			ret = send_del_subsys_oob(subsys);

		list_for_each_entry(host, &subsys->host_list, node) {
			if (target->mgmt_mode == IN_BAND_MGMT)
				send_del_host_inb(target, host->nqn);
			else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
				send_del_host_oob(target, host->nqn);
		}
	}

	list_for_each_entry(portid, &target->portid_list, node) {
		if (target->mgmt_mode == IN_BAND_MGMT)
			ret = send_del_portid_inb(target, portid);
		else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
			ret = send_del_portid_oob(target, portid);
	}

	list_del(&target->node);
out:
	return ret;
}

int set_interface(char *target, char *data, char *resp)
{
	int			 ret;

	ret = set_json_interface(target, data, resp);

	print_debug("TODO store info in target structure\n");

	return ret;
}

int add_target(char *alias, char *resp)
{
	return add_json_target(alias, resp);
}

int update_target(char *alias, char *data, char *resp)
{
	struct target		 result;
	struct target		 *target;
	int			 ret;

	memset(&result, 0, sizeof(result));

	ret = update_json_target(alias, data, resp, &result);
	if (ret)
		return ret;

	if (!alias) {
		target = alloc_target(result.alias);
		if (!target)
			ret = -ENOMEM;
		else
			target->mgmt_mode = result.mgmt_mode;
	} else {
		target = find_target(alias);
		if (strcmp(result.alias, alias))
			strcpy(target->alias, result.alias);
		target->mgmt_mode = result.mgmt_mode;
		target->refresh = result.refresh;
		if (target->mgmt_mode == OUT_OF_BAND_MGMT) {
			target->oob_iface.port = result.oob_iface.port;
			strcpy(target->oob_iface.address,
			       result.oob_iface.address);
		}
	}

	return ret;
}

static int send_set_acl_inb(struct subsystem *subsys, struct host *host)
{
	UNUSED(host);
	UNUSED(subsys);

	print_debug("TODO need to send INB set acl");

	return 0;
}

static int send_set_acl_oob(struct subsystem *subsys, struct host *host)
{
	char			 uri[128];
	char			 buf[128];
	char			*p = uri;
	int			 len;
	int			 ret;

	len = get_uri(subsys->target, p);
	p += len;

	strcpy(p, URI_HOST);

	build_set_host_oob(host->nqn, buf, sizeof(buf));

	ret = exec_post(uri, buf, strlen(buf));
	if (ret)
		return ret;

	sprintf(p, URI_SUBSYSTEM "/%s/" URI_HOST, subsys->nqn);

	return exec_post(uri, buf, strlen(buf));
}

int set_acl(char *alias, char *subnqn, char *hostnqn, char *data, char *resp)
{
	struct target		*target;
	struct subsystem	*subsys;
	struct host		*host;
	char			 nqn[MAX_STRING + 1];
	int			 ret;

	ret = set_json_acl(alias, subnqn, hostnqn, data, resp, nqn);
	if (ret)
		goto out;

	resp += strlen(resp);

	target = find_target(alias);
	if (!target)
		goto out;

	subsys = find_subsys(target, subnqn);
	if (!subsys)
		goto out;

	if (!hostnqn)
		hostnqn = nqn;

	list_for_each_entry(host, &subsys->host_list, node)
		if (!strcmp(host->nqn, hostnqn))
			goto out;

	host = malloc(sizeof(*host));
	if (!host)
		return -ENOMEM;

	strcpy(host->nqn, hostnqn);

	if (target->mgmt_mode == IN_BAND_MGMT)
		ret = send_set_acl_inb(subsys, host);
	else if (target->mgmt_mode  == OUT_OF_BAND_MGMT)
		ret = send_set_acl_oob(subsys, host);

	if (ret)
		sprintf(resp, CONFIG_ERR);

	list_add_tail(&host->node, &subsys->host_list);
out:
	// TODO on error, should delete the json peice
	return ret;
}

int del_acl(char *alias, char *nqn, char *hostnqn, char *resp)
{
	struct target		*target;
	struct subsystem	*subsys;
	struct host		*host;
	int			 ret;

	ret = del_json_acl(alias, nqn, hostnqn, resp);
	if (ret)
		goto out;

	target = find_target(alias);
	if (!target)
		goto out;

	subsys = find_subsys(target, nqn);
	if (!subsys)
		goto out;

	list_for_each_entry(host, &subsys->host_list, node)
		if (!strcmp(host->nqn, hostnqn)) {
			_del_acl(subsys, host);
			break;
		}
out:
	return ret;
}