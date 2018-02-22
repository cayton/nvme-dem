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
#include <unistd.h>
#include <errno.h>
#include <curl/curl.h>

#include "jansson.h"
#include "tags.h"

#include "curl.h"
#include "show.h"

enum { DEM = 0, TARGET, HOST, GROUP, END = -1 };
static char *groups[] = { URI_DEM, URI_TARGET, URI_HOST, URI_GROUP };

static char *dem_server = DEFAULT_ADDR;
static char *dem_port = DEFAULT_PORT;
static int prompt_deletes = 1;
static int formatted;

enum { HUMAN = 0, RAW = -1, JSON = 1 };

#define JSSTR		"\"%s\":\"%s\""
#define JSINT		"\"%s\":%d"
#define JSTAG		"\"%s\":"

#define _ADD		"add"
#define _SET		"set"
#define _EDIT		"edit"
#define _DEL		"delete"
#define _RENA		"rename"
#define _GET		"get"
#define _LIST		"list"
#define _GROUP		"group"
#define _TARGET		"target"
#define _SUBSYS		"subsystem"
#define _HOST		"host"
#define _PORT		"portid"
#define _ACL		"acl"
#define _NS		"ns"
#define _REFRESH	"refresh"
#define _LINK		"link"
#define _UNLINK		"unlink"
#define _USAGE		"usage"
#define _INTERFACE	"interface"
#define _MODE		"mode"

#define DELETE_PROMPT	"Are you sure you want to delete "

struct verbs {
	int		(*function)(char *base, int n, char **p);
	int		 target;
	int		 num_args;
	char		*verb;
	char		*object;
	char		*args;
	char		*help;
};

static char *error_str(int ret)
{
	if (ret == -EEXIST)
		return "already exists";

	if (ret == -ENOENT)
		return "not found";

	return "unknown error";
}

static inline int cancel_delete(void)
{
	char			 c;

	c = getchar();

	return (c != 'y' && c != 'Y');
}

static inline int formatted_json(json_t *json)
{
	char			*str;

	str = json_dumps(json, 2);
	if (!str)
		return -EINVAL;

	printf("%s\n", str);

	free(str);

	return 0;
}

/* DEM */

static int dem_config(char *base, int n, char **p)
{
	char			*result;
	json_t			*parent;
	json_error_t		 error;
	int			 ret;

	UNUSED(n);
	UNUSED(p);

	ret = exec_get(base, &result);
	if (ret)
		return ret;

	if (formatted == RAW)
		goto err;

	parent = json_loads(result, JSON_DECODE_ANY, &error);
	if (!parent)
		goto err;

	if (formatted) {
		if (formatted_json(parent))
			goto err;
	} else
		show_config(parent);

	goto out;
err:
	printf("%s\n", result);
out:
	free(result);

	return 0;
}

static int dem_shutdown(char *base, int n, char **p)
{
	char			 url[128];

	UNUSED(n);
	UNUSED(p);

	snprintf(url, sizeof(url), "%s/%s", base, METHOD_SHUTDOWN);

	return exec_post(url, NULL, 0);
}

/* GROUPS */

static int list_group(char *url, int n, char **p)
{
	char			*result;
	json_t			*parent;
	json_error_t		 error;
	int			 ret;

	UNUSED(n);
	UNUSED(p);

	ret = exec_get(url, &result);
	if (ret)
		return ret;

	if (formatted == RAW)
		goto err;

	parent = json_loads(result, JSON_DECODE_ANY, &error);
	if (!parent)
		goto err;

	if (formatted) {
		if (formatted_json(parent))
			goto err;
	} else
		show_group_list(parent);

	goto out;
err:
	printf("%s\n", result);
out:
	free(result);

	return 0;
}

static int add_group(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p++;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	return exec_post(url, NULL, 0);
}

static int get_group(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p;
	char			*result;
	json_t			*parent;
	json_error_t		 error;
	int			 ret;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	ret = exec_get(url, &result);
	if (ret)
		return ret;

	if (formatted == RAW)
		goto err;

	parent = json_loads(result, JSON_DECODE_ANY, &error);
	if (!parent)
		goto err;

	if (formatted) {
		if (formatted_json(parent))
			goto err;
	} else
		show_group_data(parent);

	goto out;
err:
	printf("%s\n", result);
out:
	free(result);

	return 0;
}

static int set_group(char *url, int n, char **p)
{
	char			 data[256];
	char			*group = *p++;
	int			 len;

	UNUSED(n);

	len = snprintf(data, sizeof(data), "{" JSSTR "}", TAG_NAME, group);

	return exec_put(url, data, len);
}

static int del_group(char *base, int n, char **p)
{
	char			 url[128];
	int			 i;

	if (prompt_deletes) {
		if (n > 1)
			printf(DELETE_PROMPT "these " TAG_GROUP "s? (N/y) ");
		else
			printf(DELETE_PROMPT TAG_GROUP " '%s'? (N/y) ", *p);

		if (cancel_delete())
			return 0;
	}

	for (i = 0; i < n; i++) {
		snprintf(url, sizeof(url), "%s/%s", base, p[i]);
		exec_delete(url);
	}

	return 0;
}

static int rename_group(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*old = *p++;
	char			*new = *p;
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, old);

	len = snprintf(data, sizeof(data), "{" JSSTR" }", TAG_NAME, new);

	return exec_patch(url, data, len);
}

/* TARGETS */

static int list_target(char *url, int n, char **p)
{
	char			*result;
	json_t			*parent;
	json_error_t		 error;
	int			 ret;

	UNUSED(n);
	UNUSED(p);

	ret = exec_get(url, &result);
	if (ret)
		return ret;

	if (formatted == RAW)
		goto err;

	parent = json_loads(result, JSON_DECODE_ANY, &error);
	if (!parent)
		goto err;

	if (formatted) {
		if (formatted_json(parent))
			goto err;
	} else
		show_target_list(parent, 0);

	goto out;
err:
	printf("%s\n", result);
out:
	free(result);

	return 0;
}

static int add_target(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p++;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	return exec_post(url, NULL, 0);
}

static int get_target(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p;
	char			*result;
	json_t			*parent;
	json_error_t		 error;
	int			 ret;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	ret = exec_get(url, &result);
	if (ret)
		return ret;

	if (formatted == RAW)
		goto err;

	parent = json_loads(result, JSON_DECODE_ANY, &error);
	if (!parent)
		goto err;

	if (formatted) {
		if (formatted_json(parent))
			goto err;
	} else
		show_target_data(parent);

	goto out;
err:
	printf("%s\n", result);
out:
	free(result);

	return 0;
}

static int set_target(char *base, int n, char **p)
{
	char			 data[256];
	char			*alias = *p;
	int			 len;

	UNUSED(n);

	len = snprintf(data, sizeof(data), "{" JSSTR "}", TAG_ALIAS, alias);

	return exec_put(base, data, len);
}

static int set_interface(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*alias = *p++;
	char			*family = *p++;
	char			*address = *p++;
	int			 port = atoi(*p);
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	len = snprintf(data, sizeof(data),
		     "{" JSTAG "{" JSSTR "," JSSTR "," JSINT "}}",
		     TAG_INTERFACE, TAG_IFFAMILY, family,
		     TAG_IFADDRESS, address, TAG_IFPORT, port);

	return exec_patch(url, data, len);
}

static int set_mode(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			 mode[32];
	char			*alias = *p++;
	char			*val = *p;
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	if (val[0] == 'o')
		strcpy(mode, TAG_OUT_OF_BAND_MGMT);
	else if (val[0] == 'i')
		strcpy(mode, TAG_IN_BAND_MGMT);
	else if (val[0] == 'l')
		strcpy(mode, TAG_LOCAL_MGMT);
	else
		return -EINVAL;

	len = snprintf(data, sizeof(data), "{" JSSTR "}", TAG_MGMT_MODE, mode);

	return exec_patch(url, data, len);
}

static int set_refresh(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*alias = *p++;
	int			 refresh = atoi(*p);
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	len = snprintf(data, sizeof(data), "{" JSINT "}", TAG_REFRESH, refresh);

	return exec_patch(url, data, len);
}

static int del_target(char *base, int n, char **p)
{
	char			 url[128];
	int			 i;

	if (prompt_deletes) {
		if (n > 1)
			printf(DELETE_PROMPT "these " TAG_TARGET "s? (N/y) ");
		else
			printf(DELETE_PROMPT TAG_TARGET " '%s'? (N/y) ", *p);

		if (cancel_delete())
			return 0;
	}

	for (i = 0; i < n; i++) {
		snprintf(url, sizeof(url), "%s/%s", base, p[i]);
		exec_delete(url);
	}

	return 0;
}

static int rename_target(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*old = *p++;
	char			*new = *p;
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, old);

	len = snprintf(data, sizeof(data), "{" JSSTR "}", TAG_ALIAS, new);

	return exec_patch(url, data, len);
}

static int refresh_target(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s", base, alias, METHOD_REFRESH);

	return exec_post(url, NULL, 0);
}

/* GROUP LINK */

static int link_target(char *base, int n, char **p)
{
	char			 url[128];
	char			*target = *p++;
	char			*group = *p;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s/%s", base, group, URI_TARGET,
		 target);

	return exec_post(url, NULL, 0);
}

static int unlink_target(char *base, int n, char **p)
{
	char			 url[128];
	char			*target = *p++;
	char			*group = *p;

	UNUSED(n);
	if (prompt_deletes) {
		printf(DELETE_PROMPT "%s '%s' from %s '%s'? (N/y) ",
		       TAG_TARGET, target, TAG_GROUP, group);
		if (cancel_delete())
			return 0;
	}

	snprintf(url, sizeof(url), "%s/%s/%s/%s",
		 base, group, URI_TARGET, target);

	return exec_delete(url);
}

static int usage_target(char *base, int n, char **p)
{
	char			 url[128];
	char			*result;
	char			*alias = *p;
	json_t			*parent;
	json_error_t		 error;
	int			 ret;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s", base, alias, URI_USAGE);

	ret = exec_get(url, &result);
	if (ret)
		return ret;

	if (formatted == RAW)
		goto err;

	parent = json_loads(result, JSON_DECODE_ANY, &error);
	if (!parent)
		goto err;

	if (formatted) {
		if (formatted_json(parent))
			goto err;
	} else
		show_usage_data(parent);

	goto out;
err:
	printf("%s\n", result);
out:
	free(result);

	return 0;
}

/* SUBSYSTEMS */

static int add_subsys(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p++;
	char			*nqn = *p++;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s/%s",
		 base, alias, URI_SUBSYSTEM, nqn);

	return exec_post(url, NULL, 0);
}

static int set_subsys(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*alias = *p++;
	char			*nqn = *p++;
	int			 allow_any = atoi(*p);
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s", base, alias, URI_SUBSYSTEM);

	len = snprintf(data, sizeof(data), "{" JSSTR "," JSINT "}",
		       TAG_SUBNQN, nqn, TAG_ALLOW_ANY, allow_any);

	return exec_put(url, data, len);
}

static int edit_subsys(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*alias = *p++;
	char			*nqn = *p++;
	int			 allow_any = atoi(*p);
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s/%s",
		 base, alias, URI_SUBSYSTEM, nqn);

	len = snprintf(data, sizeof(data), "{" JSINT "}",
		       TAG_ALLOW_ANY, allow_any);

	return exec_put(url, data, len);
}

static int del_subsys(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p++;
	int			 i;

	if (prompt_deletes) {
		printf(DELETE_PROMPT "these %s from %s '%s'? (N/y) ",
		       TAG_SUBSYSTEMS, TAG_TARGET, alias);
		if (cancel_delete())
			return 0;
	}

	for (i = 0, n--; i < n; i++) {
		snprintf(url, sizeof(url), "%s/%s/%s/%s", base, alias,
			 URI_SUBSYSTEM, p[i]);
		exec_delete(url);
	}

	return 0;
}

static int rename_subsys(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*alias = *p++;
	char			*old = *p++;
	char			*new = *p;
	int			 len;

	UNUSED(n);
	UNUSED(p);

	snprintf(url, sizeof(url), "%s/%s/%s/%s",
		 base, alias, URI_SUBSYSTEM, old);

	len = snprintf(data, sizeof(data), "{" JSSTR "}", TAG_SUBNQN, new);

	return exec_patch(url, data, len);
}

/* PORTID */

static int set_portid(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*alias = *p++;
	int			 portid = atoi(*p++);
	char			*type = *p++;
	char			*family = *p++;
	char			*address = *p++;
	int			 trsvcid = atoi(*p);
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s", base, alias, URI_PORTID);

	len = snprintf(data, sizeof(data),
		       "{" JSINT "," JSSTR "," JSSTR "," JSSTR "," JSINT "}",
		       TAG_PORTID, portid, TAG_TYPE, type, TAG_FAMILY, family,
		       TAG_ADDRESS, address, TAG_TRSVCID, trsvcid);

	return exec_put(url, data, len);
}

static int edit_portid(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*alias = *p++;
	int			 portid = atoi(*p++);
	char			*type = *p++;
	char			*family = *p++;
	char			*address = *p++;
	int			 trsvcid = atoi(*p);
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s/%d", base, alias, URI_PORTID,
		 portid);

	len = snprintf(data, sizeof(data),
		       "{" JSSTR "," JSSTR "," JSSTR "," JSINT "}",
		       TAG_TYPE, type, TAG_FAMILY, family,
		       TAG_ADDRESS, address, TAG_TRSVCID, trsvcid);

	return exec_put(url, data, len);
}

static int del_portid(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p++;
	int			 i;

	if (prompt_deletes) {
		printf(DELETE_PROMPT "these %s from %s '%s'? (N/y) ",
		       TAG_PORTIDS, TAG_TARGET, alias);
		if (cancel_delete())
			return 0;
	}

	for (i = 0, n--; i < n; i++) {
		snprintf(url, sizeof(url), "%s/%s/%s/%s", base, alias,
			 URI_PORTID, p[i]);
		exec_delete(url);
	}

	return 0;
}

/* ACL */

static int set_acl(char *base, int n, char **p)
{
	char			 data[384];
	char			 url[128];
	char			*alias = *p++;
	char			*ss = *p++;
	char			*host = *p;
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s/%s/%s", base, alias,
		 URI_SUBSYSTEM, ss, URI_HOST);

	len = snprintf(data, sizeof(data), "{" JSSTR "}", TAG_ALIAS, host);

	return exec_put(url, data, len);
}

static int del_acl(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p++;
	char			*ss = *p++;
	int			 i;

	if (prompt_deletes) {
		printf(DELETE_PROMPT "these %s from %s '%s' on %s '%s'? (N/y) ",
		       TAG_HOSTS, TAG_SUBSYSTEM, ss, TAG_TARGET, alias);
		if (cancel_delete())
			return 0;
	}

	for (i = 0, n -= 2; i < n; i++) {
		snprintf(url, sizeof(url), "%s/%s/%s/%s/%s/%s", base, alias,
			 URI_SUBSYSTEM, ss, URI_HOST, p[i]);
		exec_delete(url);
	}

	return 0;
}

/* NAMESPACES */

static int set_ns(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*alias = *p++;
	char			*subsys = *p++;
	int			 nsid = atoi(*p++);
	int			 devid = atoi(*p++);
	int			 devnsid = atoi(*p);
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s/%s/%s", base, alias,
		 URI_SUBSYSTEM, subsys, URI_NSID);

	len = snprintf(data, sizeof(data), "{" JSINT "," JSINT "," JSINT "}",
		       TAG_NSID, nsid, TAG_DEVID, devid, TAG_DEVNSID, devnsid);

	return exec_put(url, data, len);
}

static int del_ns(char *base, int n, char **p)
{
	char			 base_url[128];
	char			 url[128];
	char			*alias = *p++;
	char			*subsys = *p++;
	int			 i;

	if (prompt_deletes) {
		printf(DELETE_PROMPT
		       "these %s from %s '%s' on %s '%s'? (N/y) ",
		       TAG_NSIDS, TAG_SUBSYSTEM, subsys, TAG_TARGET, alias);
		if (cancel_delete())
			return 0;
	}

	snprintf(base_url, sizeof(base_url), "%s/%s/%s/%s/%s",
		base, alias, URI_SUBSYSTEM, subsys, URI_NSID);

	for (i = 0, n -= 2; i < n; i++) {
		snprintf(url, sizeof(url), "%s/%d", base_url, atoi(p[i]));
		exec_delete_ex(url, NULL, 0);
	}

	return 0;
}

/* HOSTS */

static int list_host(char *url, int n, char **p)
{
	char			*result;
	json_t			*parent;
	json_error_t		 error;
	int			 ret;

	UNUSED(n);
	UNUSED(p);

	ret = exec_get(url, &result);
	if (ret)
		return ret;

	if (formatted == RAW)
		goto err;

	parent = json_loads(result, JSON_DECODE_ANY, &error);
	if (!parent)
		goto err;

	if (formatted) {
		if (formatted_json(parent))
			goto err;
	} else
		show_host_list(parent, 0);

	goto out;
err:
	printf("%s\n", result);
out:
	free(result);

	return 0;
}

static int add_host(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p++;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	return exec_post(url, NULL, 0);
}


static int get_host(char *base, int n, char **p)
{
	char			 url[128];
	char			*alias = *p;
	char			*result;
	json_t			*parent;
	json_error_t		 error;
	int			 ret;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	ret = exec_get(url, &result);
	if (ret)
		return ret;

	if (formatted == RAW)
		goto err;

	parent = json_loads(result, JSON_DECODE_ANY, &error);
	if (!parent)
		goto err;

	if (formatted) {
		if (formatted_json(parent))
			goto err;
	} else
		show_host_data(parent);

	goto out;
err:
	printf("%s\n", result);
out:
	free(result);

	return 0;
}

static int set_host(char *url, int n, char **p)
{
	char			 data[256];
	char			*alias = *p++;
	char			*nqn = *p++;
	int			 len;

	UNUSED(n);

	len = snprintf(data, sizeof(data), "{" JSSTR "," JSSTR "}",
		       TAG_ALIAS, alias, TAG_HOSTNQN, nqn);

	return exec_put(url, data, len);
}

static int edit_host(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*alias = *p++;
	char			*nqn = *p++;
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, alias);

	len = snprintf(data, sizeof(data), "{" JSSTR "}", TAG_HOSTNQN, nqn);

	return exec_put(url, data, len);
}

static int del_host(char *base, int n, char **p)
{
	char			 url[128];
	int			 i;

	if (prompt_deletes) {
		if (n > 1)
			printf(DELETE_PROMPT "these " TAG_HOST "s? (N/y) ");
		else
			printf(DELETE_PROMPT TAG_HOST " '%s'? (N/y) ", *p);

		if (cancel_delete())
			return 0;
	}

	for (i = 0; i < n; i++) {
		snprintf(url, sizeof(url), "%s/%s", base, p[i]);
		exec_delete(url);
	}

	return 0;
}

static int rename_host(char *base, int n, char **p)
{
	char			 url[128];
	char			 data[256];
	char			*old = *p++;
	char			*new = *p;
	int			 len;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s", base, old);

	len = snprintf(data, sizeof(data), "{" JSSTR "}", TAG_ALIAS, new);

	return exec_patch(url, data, len);
}

/* GROUP LINK */

static int link_host(char *base, int n, char **p)
{
	char			 url[128];
	char			*host = *p++;
	char			*group = *p;

	UNUSED(n);

	snprintf(url, sizeof(url), "%s/%s/%s/%s", base, group, URI_HOST, host);

	return exec_post(url, NULL, 0);
}

static int unlink_host(char *base, int n, char **p)
{
	char			 url[128];
	char			*host = *p++;
	char			*group = *p;

	UNUSED(n);

	if (prompt_deletes) {
		printf(DELETE_PROMPT "%s '%s' from %s '%s'? (N/y) ",
		       TAG_HOST, host, TAG_GROUP, group);
		if (cancel_delete())
			return 0;
	}

	snprintf(url, sizeof(url), "%s/%s/%s/%s", base, group, URI_HOST, host);

	return exec_delete(url);
}

/*
 * Function	- Called when command given
 * Target	- Entity to be accesed
 * Num Args	- If <0 varibale # args allowed and indicates minimum args
 * Verb		- Action to be performed
 * Object	- Sub-entity action addresses
 * Args		- Can be NULL, fixed, or variable based on "Num Args"
 * Help		- Message describing what this verb does to this object
 */
static struct verbs verb_list[] = {
	/* DEM */
	{ dem_config,	 DEM,     0, "config",   NULL, NULL,
	  "show dem configuration including interfaces" },
	{ dem_shutdown,	 DEM,     0, "shutdown", NULL, NULL,
	  "signal the dem to shutdown" },

	/* GROUPS */
	{ list_group,	 GROUP,   0, _LIST, _GROUP,  NULL,
	  "list all groups" },
	{ add_group,	 GROUP,   1, _ADD,  _GROUP, "<name>",
	  "create a group (using POST)" },
	{ set_group,	 GROUP,   1, _SET,  _GROUP, "<name>",
	  "create a group (using PUT)" },
	{ get_group,	 GROUP,   1, _GET,  _GROUP, "<name>",
	  "show the targets and hosts associated with a group" },
	{ del_group,	 GROUP,   -1, _DEL,  _GROUP, "<name> {<name> ...}",
	  "delete group(s) and all associated links" },
	{ rename_group,	 GROUP,   2, _RENA, _GROUP, "<old> <new>",
	  "rename a group (using PATCH)" },

	/* TARGETS */
	{ list_target,	 TARGET,  0, _LIST, _TARGET,  NULL,
	  "list all targets" },
	{ add_target,	 TARGET,  1, _ADD,  _TARGET, "<alias>",
	  "create a target (using POST)" },
	{ set_target,	 TARGET,  1, _SET,  _TARGET, "<alias>",
	  "create a target (using PUT)" },
	{ get_target,	 TARGET,  1, _GET,  _TARGET, "<alias>",
	  "show the specified target e.g. ports, subsys's, and ns's" },
	{ del_target,	 TARGET,  -1, _DEL,  _TARGET, "<alias> {<alias> ...}",
	  "delete target(s) and all associated subsystems and port ids" },
	{ set_mode,	 TARGET,  2, _SET,  _MODE, "<alias> <mode>",
	  "update a target's management mode: out-of-band, in-band, or local" },
	{ set_interface, TARGET,  4, _SET,  _INTERFACE,
	  "<alias> <family> <address> <port>",
	  "update a target's out-of-band interface" },
	{ set_refresh,	 TARGET,  2, _SET,  _REFRESH, "<alias> <refresh>",
	  "update a target's refresh rate in minutes" },
	{ rename_target, TARGET,  2, _RENA, _TARGET, "<old> <new>",
	  "rename a target in the specified group (using PATCH)" },
	{ refresh_target, TARGET, 1, "refresh", _TARGET, "<alias>",
	  "signal the dem to refresh the log pages of a target" },
	{ usage_target,	 TARGET,  1, "usage", _TARGET, "<alias>",
	  "get usage for subsystems of a target" },
	{ link_target,	 GROUP,   2, _LINK,  _TARGET, "<alias> <group>",
	  "link a target to a group (using PUT)" },
	{ unlink_target, GROUP,   2, _UNLINK,  _TARGET, "<alias> <group>",
	  "unlink a target from a group (using PUT)" },

	/* SUBSYSTEMS */
	{ add_subsys,	 TARGET,  2, _ADD,  _SUBSYS, "<alias> <subnqn>",
	  "create a subsystem on a target (using POST)" },
	{ set_subsys,	 TARGET,  3, _SET,  _SUBSYS,
	  "<alias> <subnqn> <allow_any>",
	  "create a subsystem on a target (using PUT)" },
	{ edit_subsys,	 TARGET,  3, _EDIT,  _SUBSYS,
	  "<alias> <subnqn> <allow_any>",
	  "update an existing subsystem on a target (using PUT)" },
	{ del_subsys,	 TARGET, -2, _DEL,  _SUBSYS,
	  "<alias> <subnqn> {<subnqn> ...}",
	  "delete subsystems from a target (one at a time)" },
	{ rename_subsys, TARGET,  3, _RENA, _SUBSYS,
	  "<alias> <subnqn> <newnqn>",
	  "rename a subsustem on a target (using PATCH)" },

	/* PORTID */
	{ set_portid,	 TARGET,  6, _SET,  _PORT,
	  "<alias> <portid> <trtype> <adrfam> <traddr> <trsrvid>",
	  "create a port on a target (using PUT)" },
	{ edit_portid,	 TARGET,  6, _EDIT,  _PORT,
	  "<alias> <portid> <trtype> <adrfam> <traddr> <trsrvid>",
	  "update a port on a target (using PUT)" },
	{ del_portid,	 TARGET, -2, _DEL,  _PORT,
	  "<alias> <portid> {<portid> ...}",
	  "delete ports from a target (one at a time)" },

	/* ACL */
	{ set_acl,	 TARGET,  3, _SET,  _ACL,
	  "<target_alias> <subnqn> <host_alias>",
	  "allow a host access to a subsystem/target (using PUT)" },
	{ del_acl,	 TARGET, -3, _DEL,  _ACL,
	  "<target_alias> <subnqn> <host_alias> {<host_alias> ...}",
	  "delete hosts from access to a subsystem/target" },

	/* NAMESPACES */
	{ set_ns,	 TARGET,  5, _SET,  _NS,
	  "<alias> <subnqn> <nsid> <devid> <devnsid>",
	  "create a namespace in a subsys on a target (using PUT)" },
	{ del_ns,	 TARGET, -3, _DEL,  _NS,
	  "<alias> <subnqn> <nsid> {<nsid> ...}",
	  "delete namespaces from a subsys on a target (one at a time)" },

	/* HOSTS */
	{ list_host,	 HOST,  0, _LIST, _HOST,  NULL,
	  "list all hosts" },
	{ add_host,	 HOST,  1, _ADD,  _HOST, "<alias>",
	  "create a host (using POST)" },
	{ get_host,	 HOST,  1, _GET,  _HOST, "<alias>",
	  "show the specified host" },
	{ set_host,	 HOST,  2, _SET,  _HOST, "<alias> <hostnqn>",
	  "create a host (using PUT)" },
	{ edit_host,	 HOST,  2, _EDIT,  _HOST, "<alias> <hostnqn>",
	  "update a host (using PUT)" },
	{ del_host,	 HOST,  -1, _DEL,  _HOST, "<alias> {<alias> ...}",
	  "delete hosts (one at a time)" },
	{ rename_host,	 HOST,  2, _RENA, _HOST, "<alias> <new_alias>",
	  "rename a host (using PATCH)" },
	{ link_host,     GROUP, 2, _LINK,  _HOST, "<alias> <group>",
	  "link a host to a group (using PUT)" },
	{ unlink_host,   GROUP, 2, _UNLINK,  _HOST, "<alias> <group>",
	  "unlink a host from a group (using PUT)" },

	{ NULL,		 END,   0,  NULL,  NULL,   NULL, "" },
};

/* utility functions */

static void show_help(char *prog, char *msg, char *opt)
{
	struct verbs		*p;
	int			target = END;

	if (msg) {
		if (opt)
			printf("Error: %s '%s'\n", msg, opt);
		else
			printf("Error: %s\n", msg);
		printf("use -? or --help to show full help\n");
		return;
	}

	printf("Usage: %s {options} <verb> {object} {value ...}\n", prog);
	printf("options: %s\n",
	       "{-f} {-c} {-j|-r} {-s <server>} {-p <port>}");
	printf("  -f -- force - do not verify deletes\n");
	printf("  -c -- enable debug of curl commands being sent\n");
	printf("  -j -- show output in json mode (less human readable)\n");
	printf("  -r -- show output in raw mode (unformatted)\n");
	printf("  -s -- specify server (default %s)\n", DEFAULT_ADDR);
	printf("  -p -- specify port (default %s)\n", DEFAULT_PORT);
	printf("\n");
	printf("  verb : list | get | add | set | rename");
	printf(" | delete | link | unlink\n");
	printf("	 | refresh | config | shutdown\n");
	printf("       : shorthand verbs may be used (first 3 characters)\n");
	printf("object : group | target | subsystem | portid | acl\n");
	printf("	 | ns | host\n");
	printf("       : shorthand objects may be used (first character)\n");
	printf("allow_any -- 0 : use host list\n");
	printf("	  -- 1 : allow any host to access a subsystem\n");
	printf("\n");

	for (p = verb_list; p->verb; p++) {
		if (target != p->target) {
			target = p->target;
			printf("%s commands:\n", groups[target]);
		}
		printf("  dem %s", p->verb);
		if (p->object)
			printf(" %s", p->object);
		if (p->args)
			printf(" %s", p->args);
		printf("\n");
		if (p->help)
			printf("    - %s\n", p->help);
	}
}

static struct verbs *find_verb(char *verb, char *object)
{
	struct verbs		*p;
	int			 verb_len = strlen(verb);
	int			 object_len = (object) ? strlen(object) : 0;

	for (p = verb_list; p->verb; p++) {
		if (strcmp(verb, p->verb)) {
			if (verb_len == 3 && strncmp(verb, p->verb, 3) == 0)
				; // short version of verb matches
			else
				continue;
		}
		if (!p->object)
			return p;
		if (!object)
			break;
		if (strcmp(object, p->object) == 0)
			return p;
		if (object_len == 1 && object[0] == p->object[0])
			return p; // short version of object matches
	}

	return NULL;
}

static int valid_arguments(char *prog, int argc, int target, int expect)
{
	const int		 base = (target == DEM) ? 1 : 2;
	int			 ret = 0;

	if (expect < 0) {
		expect = -expect; // get absolute value
		if (argc < base  + expect) {
			show_help(prog, "missing attrs", NULL);
			ret = -1;
		}
	} else if (argc < base + expect) {
		show_help(prog, "missing attrs", NULL);
		ret = -1;
	} else if (argc > base + expect) {
		show_help(prog, "extra attrs", NULL);
		ret = -1;
	}

	return ret;
}

int main(int argc, char *argv[])
{
	struct verbs		*p;
	char			**args;
	char			**opts;
	int			 n;
	int			 opt;
	int			 ret = -1;
	char			 url[128];
	int			 debug_curl;

	if (argc <= 1 || strcmp(argv[1], "--help") == 0) {
		show_help(argv[0], NULL, NULL);
		return 0;
	}

	formatted = HUMAN;
	debug_curl = 0;

	while ((opt = getopt(argc, argv, "?jcrfs:p:")) != -1) {
		switch (opt) {
		case 'c':
			debug_curl = 1;
			break;
		case 'r':
			formatted = RAW;
			break;
		case 'j':
			formatted = JSON;
			break;
		case 'f':
			prompt_deletes = 0;
			break;
		case 's':
			dem_server = optarg;
			break;
		case 'p':
			dem_port = optarg;
			break;
		case '?':
			show_help(argv[0], NULL, NULL);
			return 0;
		default:
			show_help(argv[0], "Unknown option", NULL);
			return 1;
		}
	}

	args = &argv[optind];
	argc -= optind;

	if (argc == 0) {
		show_help(argv[0], "missing verb/object set", NULL);
		return -1;
	}

	p = find_verb(args[0], (argc <= 1) ? NULL : args[1]);
	if (!p) {
		show_help(argv[0], "unknown verb/object set", NULL);
		return -1;
	}

	if (valid_arguments(argv[0], argc, p->target, p->num_args))
		return -1;

	if (init_curl(debug_curl))
		return -1;

	if (p->target == DEM)
		snprintf(url, sizeof(url), "http://%s:%s/%s",
			 dem_server, dem_port, groups[DEM]);
	else
		snprintf(url, sizeof(url), "http://%s:%s/%s",
			 dem_server, dem_port, groups[p->target]);

	if (argc <= 1)
		opts = args;
	else {
		argc -= 2;
		opts = &args[2];
	}

	ret = p->function(url, argc, opts);
	if (ret < 0) {
		n = (strcmp(p->verb, _RENA) == 0 && ret == -EEXIST) ? 4 : 3;
		printf("Error: %s: %s '%s' %s\n",
		       argv[0], args[1], args[n], error_str(ret));
	}

	cleanup_curl();

	return ret;
}
