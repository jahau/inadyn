/* libConfuse interface to parse inadyn.conf v2 format
 *
 * Copyright (C) 2014-2015  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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

#include <errno.h>
#include <string.h>
#include <confuse.h>

#include "cache.h"
#include "ddns.h"

/*
 * period        = 600
 * forced-update = 604800
 *
 * provider default@freedns.afraid.org
 * {
 *   wildcard = false
 *   username = example
 *   password = secret
 *   alias    = { "example.homenet.org", "example.afraid.org" }
 * }
 *
 * provider default@dyndns.org
 * {
 *   ssl      = true
 *   username = admin
 *   password = supersecret
 *   alias    = example.dyndns.org
 * }
 */
static LIST_HEAD(head, di) info_list = LIST_HEAD_INITIALIZER(info_list);

/*
 * Convert deprecated 'alias' setting to new 'hostname',
 * same functionality with new name.
 */
static int deprecate_alias(cfg_t *cfg)
{
	size_t i;
	cfg_opt_t *alias, *hostname;

	alias = cfg_getopt(cfg, "alias");
	if (!alias || cfg_opt_size(alias) <= 0)
		return 0;

	hostname = cfg_getopt(cfg, "hostname");
	if (cfg_opt_size(hostname) > 0) {
		cfg_error(cfg, "Both 'hostname' and 'alias' set, cannot convert deprecated 'alias' to 'hostname'");
		return -1;
	}

	cfg_error(cfg, "converting 'alias' to 'hostname'.");
	for (i = 0; i < cfg_opt_size(alias); i++)
		cfg_opt_setnstr(hostname, cfg_opt_getnstr(alias, i), i);

	cfg_free_value(alias);

	return 0;
}

static int validate_period(cfg_t *cfg, cfg_opt_t *opt)
{
	int val = cfg_getint(cfg, opt->name);

	if (val < DDNS_MIN_PERIOD)
		val = DDNS_MIN_PERIOD;
	if (val > DDNS_MAX_PERIOD)
		val = DDNS_MAX_PERIOD;

	return 0;
}

static int validate_hostname(cfg_t *cfg, const char *provider, cfg_opt_t *hostname)
{
	size_t i;

	if (!hostname) {
		cfg_error(cfg, "DDNS hostname setting is missing in provider %s", provider);
		return -1;
	}

	if (!cfg_opt_size(hostname)) {
		cfg_error(cfg, "No hostnames listed in DDNS provider %s", provider);
		return -1;
	}

	for (i = 0; i < cfg_opt_size(hostname); i++) {
		char *name = cfg_opt_getnstr(hostname, i);
		ddns_info_t info;

		if (sizeof(info.alias[0].name) < strlen(name)) {
			cfg_error(cfg, "Too long DDNS hostname (%s) in provider %s", name, provider);
			return -1;
		}
	}

	return 0;
}

/* No need to validate username/password for custom providers */
static int validate_common(cfg_t *cfg, const char *provider, int custom)
{
	if (!plugin_find(provider)) {
		cfg_error(cfg, "Invalid DDNS provider %s", provider);
		return -1;
	}

	if (!custom && !cfg_getstr(cfg, "username")) {
		cfg_error(cfg, "Missing username setting for DDNS provider %s", provider);
		return -1;
	}

	if (!custom && !cfg_getstr(cfg, "password")) {
		cfg_error(cfg, "Missing password setting for DDNS provider %s", provider);
		return -1;
	}

	return deprecate_alias(cfg) ||
		validate_hostname(cfg, provider, cfg_getopt(cfg, "hostname"));
}

static int validate_provider(cfg_t *cfg, cfg_opt_t *opt)
{
	const char *provider;

	cfg = cfg_opt_getnsec(opt, 0);
	provider = cfg_title(cfg);

	if (!provider) {
		cfg_error(cfg, "Missing DDNS provider name");
		return -1;
	}

	return validate_common(cfg, provider, 0);
}

static int validate_custom(cfg_t *cfg, cfg_opt_t *opt)
{
	cfg = cfg_opt_getnsec(opt, 0);
	if (!cfg)
		return -1;

	if (!cfg_getstr(cfg, "ddns-server")) {
		cfg_error(cfg, "Missing 'ddns-server' for custom DDNS provider");
		return -1;
	}

	return validate_common(cfg, "custom", 1);
}

/* server:port => server:80 if port is not given */
static int getserver(const char *server, ddns_name_t *name)
{
	char *str, *ptr;

	if (strlen(server) > sizeof(name->name))
		return 1;

	str = strdup(server);
	if (!str)
		return 1;

	ptr = strchr(str, ':');
	if (ptr) {
		*ptr++ = 0;
		name->port = atonum(ptr);
		if (-1 == name->port)
			name->port = HTTP_DEFAULT_PORT;
	} else {
		name->port = HTTP_DEFAULT_PORT;
	}

	strlcpy(name->name, str, sizeof(name->name));
	free(str);

	return 0;
}

static int cfg_getserver(cfg_t *cfg, char *server, ddns_name_t *name)
{
	const char *str;

	str = cfg_getstr(cfg, server);
	if (!str)
		return 1;

	return getserver(str, name);
}

static int set_provider_opts(cfg_t *cfg, ddns_info_t *info, int custom)
{
	size_t j;
	const char *str;
	ddns_system_t *system;

	if (custom)
		str = "custom";
	else
		str = cfg_title(cfg);

	system = plugin_find(str);
	if (!system) {
		logit(LOG_ERR, "Cannot find an DDNS plugin for provider '%s'", str);
		return 1;
	}

	info->system = system;

	if (getserver(system->checkip_name, &info->checkip_name))
		goto error;
	if (strlen(system->checkip_url) > sizeof(info->checkip_url))
		goto error;
	strlcpy(info->checkip_url, system->checkip_url, sizeof(info->checkip_url));

	if (getserver(system->server_name, &info->server_name))
		goto error;
	if (strlen(system->server_url) > sizeof(info->server_url))
		goto error;
	strlcpy(info->server_url, system->server_url, sizeof(info->server_url));

	info->wildcard = cfg_getbool(cfg, "wildcard");
	info->ssl_enabled = cfg_getbool(cfg, "ssl");
	str = cfg_getstr(cfg, "username");
	if (str && strlen(str) <= sizeof(info->creds.username))
		strlcpy(info->creds.username, str, sizeof(info->creds.username));
	str = cfg_getstr(cfg, "password");
	if (str && strlen(str) <= sizeof(info->creds.password))
		strlcpy(info->creds.password, str, sizeof(info->creds.password));

	for (j = 0; j < cfg_size(cfg, "hostname"); j++) {
		size_t pos = info->alias_count;

		str = cfg_getnstr(cfg, "hostname", j);
		if (!str)
			continue;

		strlcpy(info->alias[pos].name, str, sizeof(info->alias[pos].name));
		info->alias_count++;
	}

	if (custom) {
		info->append_myip = cfg_getbool(cfg, "append-myip");

		cfg_getserver(cfg, "checkip-server", &info->checkip_name);
		str = cfg_getstr(cfg, "checkip-path");
		if (str && strlen(str) <= sizeof(info->checkip_url))
			strlcpy(info->checkip_url, str, sizeof(info->checkip_url));

		cfg_getserver(cfg, "ddns-server", &info->server_name);
		str = cfg_getstr(cfg, "ddns-path");
		if (str && strlen(str) <= sizeof(info->server_url))
			strlcpy(info->server_url, str, sizeof(info->server_url));

		for (j = 0; j < cfg_size(cfg, "ddns-response"); j++) {
			size_t pos = info->server_response_num;

			str = cfg_getnstr(cfg, "ddns-response", j);
			if (!str)
				continue;

			if (info->server_response_num >= NELEMS(info->server_response)) {
				logit(LOG_WARNING, "Skipping response '%s', only %d custom responses supported",
				      str, NELEMS(info->server_response));
				continue;
			}

			strlcpy(info->server_response[pos], str, sizeof(info->server_response[pos]));
			info->server_response_num++;
		}

		/* Default check, if no configured custom response string(s) */
		if (!cfg_size(cfg, "ddns-response")) {
			for (j = 0; j < NELEMS(info->server_response); j++) {
				if (!generic_responses[j])
					break;

				strlcpy(info->server_response[j], generic_responses[j], sizeof(info->server_response[j]));
				info->server_response_num++;
			}
		}
	}

	return 0;

error:
	logit(LOG_ERR, "Failed setting up %s DDNS provider, skipping.", str);
	return 1;
}

static int create_provider(cfg_t *cfg, int custom)
{
	ddns_info_t *info;

	info = calloc(1, sizeof(*info));
	if (!info) {
		logit(LOG_ERR, "Failed allocating memory for provider %s", cfg_title(cfg));
		return 1;
	}

	http_construct(&info->checkip);
	http_construct(&info->server);
	if (set_provider_opts(cfg, info, custom))
		return 1;

	LIST_INSERT_HEAD(&info_list, info, link);
	return 0;
}

ddns_info_t *conf_info_iterator(int first)
{
	static ddns_info_t *ptr = NULL;

	if (first) {
		ptr = LIST_FIRST(&info_list);
		return ptr;
	}

	if (!ptr || ptr == LIST_END(&info_list))
		return NULL;

	ptr = LIST_NEXT(ptr, link);
	return ptr;
}

void conf_info_cleanup(void)
{
	ddns_info_t *ptr, *tmp;

	LIST_FOREACH_SAFE(ptr, &info_list, link, tmp) {
		if (ptr->creds.encoded_password)
			free(ptr->creds.encoded_password);
		LIST_REMOVE(ptr, link);
		free(ptr);
	}
}

cfg_t *conf_parse_file(char *file, ddns_t *ctx)
{
	int ret = 0;
	size_t i;
	cfg_opt_t provider_opts[] = {
		CFG_STR     ("username",     NULL, CFGF_NONE),
		CFG_STR     ("password",     NULL, CFGF_NONE),
		CFG_STR_LIST("hostname",     NULL, CFGF_NONE),
		CFG_STR_LIST("alias",        NULL, CFGF_DEPRECATED),
		CFG_BOOL    ("ssl",          cfg_false, CFGF_NONE),
		CFG_BOOL    ("wildcard",     cfg_false, CFGF_NONE),
		CFG_END()
	};
	cfg_opt_t custom_opts[] = {
		/* Same as a general provider */
		CFG_STR     ("username",     NULL, CFGF_NONE),
		CFG_STR     ("password",     NULL, CFGF_NONE),
		CFG_STR_LIST("hostname",     NULL, CFGF_NONE),
		CFG_STR_LIST("alias",        NULL, CFGF_DEPRECATED),
		CFG_BOOL    ("ssl",          cfg_false, CFGF_NONE),
		CFG_BOOL    ("wildcard",     cfg_false, CFGF_NONE),
		/* Custom settings */
		CFG_BOOL    ("append-myip",    cfg_false, CFGF_NONE),
		CFG_STR     ("ddns-server",    NULL, CFGF_NONE),
		CFG_STR     ("ddns-path",      NULL, CFGF_NONE),
		CFG_STR_LIST("ddns-response",  NULL, CFGF_NONE),
		CFG_STR     ("checkip-server", NULL, CFGF_NONE), /* Syntax:  name:port */
		CFG_STR     ("checkip-path",   NULL, CFGF_NONE), /* Default: "/" */
		CFG_END()
	};
	cfg_opt_t opts[] = {
		CFG_BOOL("fake-address",  cfg_false, CFGF_NONE),
		CFG_STR ("cache-dir",	  DEFAULT_CACHE_DIR, CFGF_NONE),
		CFG_INT ("period",	  DDNS_DEFAULT_PERIOD, CFGF_NONE),
		CFG_INT ("iterations",    DDNS_DEFAULT_ITERATIONS, CFGF_NONE),
		CFG_INT ("forced-update", DDNS_FORCED_UPDATE_PERIOD, CFGF_NONE),
		CFG_STR ("iface",         NULL, CFGF_NONE),
		CFG_SEC ("provider",      provider_opts, CFGF_MULTI | CFGF_TITLE),
		CFG_SEC ("custom",        custom_opts, CFGF_MULTI | CFGF_TITLE),
		CFG_END()
	};
	cfg_t *cfg;

	cfg = cfg_init(opts, CFGF_IGNORE_UNKNOWN);
	if (!cfg) {
		logit(LOG_ERR, "Failed initializing configuration file parser: %m");
		return NULL;
	}

	/* Validators */
	cfg_set_validate_func(cfg, "period", validate_period);
	cfg_set_validate_func(cfg, "provider", validate_provider);
	cfg_set_validate_func(cfg, "custom", validate_custom);

	switch (cfg_parse(cfg, file)) {
	case CFG_FILE_ERROR:
		logit(LOG_ERR, "Cannot read configuration file %s: %m", file);
		return NULL;

	case CFG_PARSE_ERROR:
		logit(LOG_ERR, "Error parsing configuration file %s: %m", file);
		return NULL;

	case CFG_SUCCESS:
		break;
	}

	/* Set global options */
	ctx->normal_update_period_sec = cfg_getint(cfg, "period");
	ctx->error_update_period_sec  = DDNS_ERROR_UPDATE_PERIOD;
	ctx->forced_update_period_sec = cfg_getint(cfg, "forced-update");
	if (once)
		ctx->total_iterations = 1;
	else
		ctx->total_iterations = cfg_getint(cfg, "iterations");

	cache_dir                     = cfg_getstr(cfg, "cache-dir");
	ctx->forced_update_fake_addr  = cfg_getbool(cfg, "fake-address");

	/* Command line --iface=IFNAME takes precedence */
	if (!iface)
		iface                 = cfg_getstr(cfg, "iface");

	for (i = 0; i < cfg_size(cfg, "provider"); i++)
		ret |= create_provider(cfg_getnsec(cfg, "provider", i), 0);

	for (i = 0; i < cfg_size(cfg, "custom"); i++)
		ret |= create_provider(cfg_getnsec(cfg, "custom", i), 1);

	if (ret)
		return NULL;

	return cfg;
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
