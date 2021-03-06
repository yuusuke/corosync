/*
 * Copyright (c) 2002-2005 MontaVista Software, Inc.
 * Copyright (c) 2006-2013 Red Hat, Inc.
 *
 * All rights reserved.
 *
 * Author: Steven Dake (sdake@redhat.com)
 *         Jan Friesse (jfriesse@redhat.com)
 *
 * This software licensed under BSD license, the text of which follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 * - Neither the name of the MontaVista Software, Inc. nor the names of its
 *   contributors may be used to endorse or promote products derived from this
 *   software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <config.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/param.h>

#include <corosync/swab.h>
#include <qb/qblist.h>
#include <qb/qbdefs.h>
#include <libknet.h>
#include <corosync/totem/totem.h>
#include <corosync/config.h>
#include <corosync/logsys.h>
#include <corosync/icmap.h>

#include "util.h"
#include "totemconfig.h"

#define TOKEN_RETRANSMITS_BEFORE_LOSS_CONST	4
#define TOKEN_TIMEOUT				1000
#define TOKEN_COEFFICIENT			650
#define JOIN_TIMEOUT				50
#define MERGE_TIMEOUT				200
#define DOWNCHECK_TIMEOUT			1000
#define FAIL_TO_RECV_CONST			2500
#define	SEQNO_UNCHANGED_CONST			30
#define MINIMUM_TIMEOUT				(int)(1000/HZ)*3
#define MAX_NETWORK_DELAY			50
#define WINDOW_SIZE				50
#define MAX_MESSAGES				17
#define MISS_COUNT_CONST			5

/* These currently match the defaults in libknet.h */
#define KNET_PING_INTERVAL                      1000
#define KNET_PING_TIMEOUT                       2000
#define KNET_PING_PRECISION                     2048
#define KNET_PONG_COUNT                         2
#define KNET_PMTUD_INTERVAL                     30
#define KNET_DEFAULT_TRANSPORT                  KNET_TRANSPORT_UDP

#define DEFAULT_PORT				5405

static char error_string_response[512];

static void add_totem_config_notification(struct totem_config *totem_config);

static void *totem_get_param_by_name(struct totem_config *totem_config, const char *param_name)
{
	if (strcmp(param_name, "totem.token") == 0)
		return &totem_config->token_timeout;
	if (strcmp(param_name, "totem.token_retransmit") == 0)
		return &totem_config->token_retransmit_timeout;
	if (strcmp(param_name, "totem.hold") == 0)
		return &totem_config->token_hold_timeout;
	if (strcmp(param_name, "totem.token_retransmits_before_loss_const") == 0)
		return &totem_config->token_retransmits_before_loss_const;
	if (strcmp(param_name, "totem.join") == 0)
		return &totem_config->join_timeout;
	if (strcmp(param_name, "totem.send_join") == 0)
		return &totem_config->send_join_timeout;
	if (strcmp(param_name, "totem.consensus") == 0)
		return &totem_config->consensus_timeout;
	if (strcmp(param_name, "totem.merge") == 0)
		return &totem_config->merge_timeout;
	if (strcmp(param_name, "totem.downcheck") == 0)
		return &totem_config->downcheck_timeout;
	if (strcmp(param_name, "totem.fail_recv_const") == 0)
		return &totem_config->fail_to_recv_const;
	if (strcmp(param_name, "totem.seqno_unchanged_const") == 0)
		return &totem_config->seqno_unchanged_const;
	if (strcmp(param_name, "totem.heartbeat_failures_allowed") == 0)
		return &totem_config->heartbeat_failures_allowed;
	if (strcmp(param_name, "totem.max_network_delay") == 0)
		return &totem_config->max_network_delay;
	if (strcmp(param_name, "totem.window_size") == 0)
		return &totem_config->window_size;
	if (strcmp(param_name, "totem.max_messages") == 0)
		return &totem_config->max_messages;
	if (strcmp(param_name, "totem.miss_count_const") == 0)
		return &totem_config->miss_count_const;
	if (strcmp(param_name, "totem.knet_pmtud_interval") == 0)
		return &totem_config->knet_pmtud_interval;
	if (strcmp(param_name, "totem.knet_compression_threshold") == 0)
		return &totem_config->knet_compression_threshold;
	if (strcmp(param_name, "totem.knet_compression_level") == 0)
		return &totem_config->knet_compression_level;
	if (strcmp(param_name, "totem.knet_compression_model") == 0)
		return &totem_config->knet_compression_model;

	return NULL;
}

/*
 * Read key_name from icmap. If key is not found or key_name == delete_key or if allow_zero is false
 * and readed value is zero, default value is used and stored into totem_config.
 */
static void totem_volatile_config_set_uint32_value (struct totem_config *totem_config,
	const char *key_name, const char *deleted_key, unsigned int default_value,
	int allow_zero_value)
{
	char runtime_key_name[ICMAP_KEYNAME_MAXLEN];

	if (icmap_get_uint32(key_name, totem_get_param_by_name(totem_config, key_name)) != CS_OK ||
	    (deleted_key != NULL && strcmp(deleted_key, key_name) == 0) ||
	    (!allow_zero_value && *(uint32_t *)totem_get_param_by_name(totem_config, key_name) == 0)) {
		*(uint32_t *)totem_get_param_by_name(totem_config, key_name) = default_value;
	}

	/*
	 * Store totem_config value to cmap runtime section
	 */
	if (strlen("runtime.config.") + strlen(key_name) >= ICMAP_KEYNAME_MAXLEN) {
		/*
		 * This shouldn't happen
		 */
		return ;
	}

	strcpy(runtime_key_name, "runtime.config.");
	strcat(runtime_key_name, key_name);

	icmap_set_uint32(runtime_key_name, *(uint32_t *)totem_get_param_by_name(totem_config, key_name));
}

static void totem_volatile_config_set_int32_value (struct totem_config *totem_config,
	const char *key_name, const char *deleted_key, int default_value,
	int allow_zero_value)
{
	char runtime_key_name[ICMAP_KEYNAME_MAXLEN];

	if (icmap_get_int32(key_name, totem_get_param_by_name(totem_config, key_name)) != CS_OK ||
	    (deleted_key != NULL && strcmp(deleted_key, key_name) == 0) ||
	    (!allow_zero_value && *(int32_t *)totem_get_param_by_name(totem_config, key_name) == 0)) {
		*(int32_t *)totem_get_param_by_name(totem_config, key_name) = default_value;
	}

	/*
	 * Store totem_config value to cmap runtime section
	 */
	if (strlen("runtime.config.") + strlen(key_name) >= ICMAP_KEYNAME_MAXLEN) {
		/*
		 * This shouldn't happen
		 */
		return ;
	}

	strcpy(runtime_key_name, "runtime.config.");
	strcat(runtime_key_name, key_name);

	icmap_set_int32(runtime_key_name, *(int32_t *)totem_get_param_by_name(totem_config, key_name));
}

static void totem_volatile_config_set_string_value (struct totem_config *totem_config,
	const char *key_name, const char *deleted_key, const char *default_value)
{
	char runtime_key_name[ICMAP_KEYNAME_MAXLEN];
	void **config_value;
	void *old_config_ptr;

	config_value = totem_get_param_by_name(totem_config, key_name);
	old_config_ptr = *config_value;
	if (icmap_get_string(key_name, totem_get_param_by_name(totem_config, key_name)) != CS_OK ||
	    (deleted_key != NULL && strcmp(deleted_key, key_name) == 0)) {

		/* Need to strdup() here so that the free() below works for a default and a configured value */
		*config_value = strdup(default_value);
	}
	free(old_config_ptr);

	/*
	 * Store totem_config value to cmap runtime section
	 */
	if (strlen("runtime.config.") + strlen(key_name) >= ICMAP_KEYNAME_MAXLEN) {
		/*
		 * This shouldn't happen
		 */
		return ;
	}

	strcpy(runtime_key_name, "runtime.config.");
	strcat(runtime_key_name, key_name);

	icmap_set_string(runtime_key_name, (char *)*config_value);
}


/*
 * Read and validate config values from cmap and store them into totem_config. If key doesn't exists,
 * default value is stored. deleted_key is name of key beeing processed by delete operation
 * from cmap. It is considered as non existing even if it can be read. Can be NULL.
 */
static void totem_volatile_config_read (struct totem_config *totem_config, const char *deleted_key)
{
	uint32_t u32;

	totem_volatile_config_set_uint32_value(totem_config, "totem.token_retransmits_before_loss_const", deleted_key,
	    TOKEN_RETRANSMITS_BEFORE_LOSS_CONST, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.token", deleted_key, TOKEN_TIMEOUT, 0);

	if (totem_config->interfaces[0].member_count > 2) {
		u32 = TOKEN_COEFFICIENT;
		icmap_get_uint32("totem.token_coefficient", &u32);
		totem_config->token_timeout += (totem_config->interfaces[0].member_count - 2) * u32;

		/*
		 * Store totem_config value to cmap runtime section
		 */
		icmap_set_uint32("runtime.config.totem.token", totem_config->token_timeout);
	}

	totem_volatile_config_set_uint32_value(totem_config, "totem.max_network_delay", deleted_key, MAX_NETWORK_DELAY, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.window_size", deleted_key, WINDOW_SIZE, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.max_messages", deleted_key, MAX_MESSAGES, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.miss_count_const", deleted_key, MISS_COUNT_CONST, 0);
	totem_volatile_config_set_uint32_value(totem_config, "totem.knet_pmtud_interval", deleted_key, KNET_PMTUD_INTERVAL, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.token_retransmit", deleted_key,
	   (int)(totem_config->token_timeout / (totem_config->token_retransmits_before_loss_const + 0.2)), 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.hold", deleted_key,
	    (int)(totem_config->token_retransmit_timeout * 0.8 - (1000/HZ)), 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.join", deleted_key, JOIN_TIMEOUT, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.consensus", deleted_key,
	    (int)(float)(1.2 * totem_config->token_timeout), 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.merge", deleted_key, MERGE_TIMEOUT, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.downcheck", deleted_key, DOWNCHECK_TIMEOUT, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.fail_recv_const", deleted_key, FAIL_TO_RECV_CONST, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.seqno_unchanged_const", deleted_key,
	    SEQNO_UNCHANGED_CONST, 0);

	totem_volatile_config_set_uint32_value(totem_config, "totem.send_join", deleted_key, 0, 1);

	totem_volatile_config_set_uint32_value(totem_config, "totem.heartbeat_failures_allowed", deleted_key, 0, 1);

	totem_volatile_config_set_uint32_value(totem_config, "totem.knet_compression_threshold", deleted_key, 0, 1);

	totem_volatile_config_set_int32_value(totem_config, "totem.knet_compression_level", deleted_key, 0, 1);

	totem_volatile_config_set_string_value(totem_config, "totem.knet_compression_model", deleted_key, "none");


}

static int totem_volatile_config_validate (
	struct totem_config *totem_config,
	const char **error_string)
{
	static char local_error_reason[512];
	const char *error_reason = local_error_reason;

	if (totem_config->max_network_delay < MINIMUM_TIMEOUT) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The max_network_delay parameter (%d ms) may not be less than (%d ms).",
			totem_config->max_network_delay, MINIMUM_TIMEOUT);
		goto parse_error;
	}

	if (totem_config->token_timeout < MINIMUM_TIMEOUT) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The token timeout parameter (%d ms) may not be less than (%d ms).",
			totem_config->token_timeout, MINIMUM_TIMEOUT);
		goto parse_error;
	}

	if (totem_config->token_retransmit_timeout < MINIMUM_TIMEOUT) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The token retransmit timeout parameter (%d ms) may not be less than (%d ms).",
			totem_config->token_retransmit_timeout, MINIMUM_TIMEOUT);
		goto parse_error;
	}

	if (totem_config->token_hold_timeout < MINIMUM_TIMEOUT) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The token hold timeout parameter (%d ms) may not be less than (%d ms).",
			totem_config->token_hold_timeout, MINIMUM_TIMEOUT);
		goto parse_error;
	}

	if (totem_config->join_timeout < MINIMUM_TIMEOUT) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The join timeout parameter (%d ms) may not be less than (%d ms).",
			totem_config->join_timeout, MINIMUM_TIMEOUT);
		goto parse_error;
	}

	if (totem_config->consensus_timeout < MINIMUM_TIMEOUT) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The consensus timeout parameter (%d ms) may not be less than (%d ms).",
			totem_config->consensus_timeout, MINIMUM_TIMEOUT);
		goto parse_error;
	}

	if (totem_config->consensus_timeout < totem_config->join_timeout) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The consensus timeout parameter (%d ms) may not be less than join timeout (%d ms).",
			totem_config->consensus_timeout, totem_config->join_timeout);
		goto parse_error;
	}

	if (totem_config->merge_timeout < MINIMUM_TIMEOUT) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The merge timeout parameter (%d ms) may not be less than (%d ms).",
			totem_config->merge_timeout, MINIMUM_TIMEOUT);
		goto parse_error;
	}

	if (totem_config->downcheck_timeout < MINIMUM_TIMEOUT) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The downcheck timeout parameter (%d ms) may not be less than (%d ms).",
			totem_config->downcheck_timeout, MINIMUM_TIMEOUT);
		goto parse_error;
	}

	return 0;

parse_error:
	snprintf (error_string_response, sizeof(error_string_response),
		 "parse error in config: %s\n", error_reason);
	*error_string = error_string_response;
	return (-1);

}

static int totem_get_crypto(struct totem_config *totem_config, const char **error_string)
{
	char *str;
	const char *tmp_cipher;
	const char *tmp_hash;
	const char *tmp_model;

	tmp_hash = "none";
	tmp_cipher = "none";
	tmp_model = "none";

	if (icmap_get_string("totem.crypto_model", &str) == CS_OK) {
		if (strcmp(str, "nss") == 0) {
			tmp_model = "nss";
		}
		if (strcmp(str, "openssl") == 0) {
			tmp_model = "openssl";
		}
		free(str);
	} else {
		tmp_model = "nss";
	}

	if (icmap_get_string("totem.crypto_cipher", &str) == CS_OK) {
		if (strcmp(str, "none") == 0) {
			tmp_cipher = "none";
		}
		if (strcmp(str, "aes256") == 0) {
			tmp_cipher = "aes256";
		}
		if (strcmp(str, "aes192") == 0) {
			tmp_cipher = "aes192";
		}
		if (strcmp(str, "aes128") == 0) {
			tmp_cipher = "aes128";
		}
		if (strcmp(str, "3des") == 0) {
			tmp_cipher = "3des";
		}
		free(str);
	}

	if (icmap_get_string("totem.crypto_hash", &str) == CS_OK) {
		if (strcmp(str, "none") == 0) {
			tmp_hash = "none";
		}
		if (strcmp(str, "md5") == 0) {
			tmp_hash = "md5";
		}
		if (strcmp(str, "sha1") == 0) {
			tmp_hash = "sha1";
		}
		if (strcmp(str, "sha256") == 0) {
			tmp_hash = "sha256";
		}
		if (strcmp(str, "sha384") == 0) {
			tmp_hash = "sha384";
		}
		if (strcmp(str, "sha512") == 0) {
			tmp_hash = "sha512";
		}
		free(str);
	}

	if ((strcmp(tmp_cipher, "none") != 0) &&
	    (strcmp(tmp_hash, "none") == 0)) {
		*error_string = "crypto_cipher requires crypto_hash with value other than none";
		return -1;
	}

	if (strcmp(tmp_model, "none") == 0) {
		*error_string = "crypto_model should be 'nss' or 'openssl'";
		return -1;
	}

	free(totem_config->crypto_cipher_type);
	free(totem_config->crypto_hash_type);
	free(totem_config->crypto_model);

	totem_config->crypto_cipher_type = strdup(tmp_cipher);
	totem_config->crypto_hash_type = strdup(tmp_hash);
	totem_config->crypto_model = strdup(tmp_model);

	return 0;
}

static int totem_config_get_ip_version(struct totem_config *totem_config)
{
	int res;
	char *str;

	res = AF_INET;
	if (totem_config->transport_number == TOTEM_TRANSPORT_KNET) {
		res = AF_UNSPEC;
	} else {
		if (icmap_get_string("totem.ip_version", &str) == CS_OK) {
			if (strcmp(str, "ipv4") == 0) {
				res = AF_INET;
			}
			if (strcmp(str, "ipv6") == 0) {
				res = AF_INET6;
			}
			free(str);
		}
	}

	return (res);
}

static uint16_t generate_cluster_id (const char *cluster_name)
{
	int i;
	int value = 0;

	for (i = 0; i < strlen(cluster_name); i++) {
		value <<= 1;
		value += cluster_name[i];
	}

	return (value & 0xFFFF);
}

static int get_cluster_mcast_addr (
		const char *cluster_name,
		unsigned int linknumber,
		int ip_version,
		struct totem_ip_address *res)
{
	uint16_t clusterid;
	char addr[INET6_ADDRSTRLEN + 1];
	int err;

	if (cluster_name == NULL) {
		return (-1);
	}

	clusterid = generate_cluster_id(cluster_name) + linknumber;
	memset (res, 0, sizeof(*res));

	switch (ip_version) {
	case AF_INET:
		snprintf(addr, sizeof(addr), "239.192.%d.%d", clusterid >> 8, clusterid % 0xFF);
		break;
	case AF_INET6:
		snprintf(addr, sizeof(addr), "ff15::%x", clusterid);
		break;
	default:
		/*
		 * Unknown family
		 */
		return (-1);
	}

	err = totemip_parse (res, addr, ip_version);

	return (err);
}

static unsigned int generate_nodeid_for_duplicate_test(
	struct totem_config *totem_config,
	char *addr)
{
	unsigned int nodeid;
	struct totem_ip_address totemip;

	/* AF_INET hard-coded here because auto-generated nodeids
	   are only for IPv4 */
	if (totemip_parse(&totemip, addr, AF_INET) != 0)
		return -1;

	memcpy (&nodeid, &totemip.addr, sizeof (unsigned int));

#if __BYTE_ORDER == __LITTLE_ENDIAN
	nodeid = swab32 (nodeid);
#endif

	if (totem_config->clear_node_high_bit) {
		nodeid &= 0x7FFFFFFF;
	}
	return nodeid;
}

static int check_for_duplicate_nodeids(
	struct totem_config *totem_config,
	const char **error_string)
{
	icmap_iter_t iter;
	icmap_iter_t subiter;
	const char *iter_key;
	int res = 0;
	int retval = 0;
	char tmp_key[ICMAP_KEYNAME_MAXLEN];
	char *ring0_addr=NULL;
	char *ring0_addr1=NULL;
	unsigned int node_pos;
	unsigned int node_pos1;
	unsigned int nodeid;
	unsigned int nodeid1;
	int autogenerated;

	iter = icmap_iter_init("nodelist.node.");
	while ((iter_key = icmap_iter_next(iter, NULL, NULL)) != NULL) {
		res = sscanf(iter_key, "nodelist.node.%u.%s", &node_pos, tmp_key);
		if (res != 2) {
			continue;
		}

		if (strcmp(tmp_key, "ring0_addr") != 0) {
			continue;
		}

		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.nodeid", node_pos);
		autogenerated = 0;
		if (icmap_get_uint32(tmp_key, &nodeid) != CS_OK) {

			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.ring0_addr", node_pos);
			if (icmap_get_string(tmp_key, &ring0_addr) != CS_OK) {
				continue;
			}

			/* Generate nodeid so we can check that auto-generated nodeids don't clash either */
			nodeid = generate_nodeid_for_duplicate_test(totem_config, ring0_addr);
			if (nodeid == -1) {
				continue;
			}
			autogenerated = 1;
		}

		node_pos1 = 0;
		subiter = icmap_iter_init("nodelist.node.");
		while (((iter_key = icmap_iter_next(subiter, NULL, NULL)) != NULL) && (node_pos1 < node_pos)) {
			res = sscanf(iter_key, "nodelist.node.%u.%s", &node_pos1, tmp_key);
			if ((res != 2) || (node_pos1 >= node_pos)) {
				continue;
			}

			if (strcmp(tmp_key, "ring0_addr") != 0) {
				continue;
			}

			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.nodeid", node_pos1);
			if (icmap_get_uint32(tmp_key, &nodeid1) != CS_OK) {

				snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.ring0_addr", node_pos1);
				if (icmap_get_string(tmp_key, &ring0_addr1) != CS_OK) {
					continue;
				}
				nodeid1 = generate_nodeid_for_duplicate_test(totem_config, ring0_addr1);
				if (nodeid1 == -1) {
					continue;
				}
			}

			if (nodeid == nodeid1) {
				retval = -1;
				snprintf (error_string_response, sizeof(error_string_response),
					  "Nodeid %u%s%s%s appears twice in corosync.conf", nodeid,
					  autogenerated?"(autogenerated from ":"",
					  autogenerated?ring0_addr:"",
					  autogenerated?")":"");
				log_printf (LOGSYS_LEVEL_ERROR, error_string_response);
				*error_string = error_string_response;
				break;
			}
		}
		icmap_iter_finalize(subiter);
	}
	icmap_iter_finalize(iter);
	return retval;
}


static int find_local_node_in_nodelist(struct totem_config *totem_config)
{
	icmap_iter_t iter;
	const char *iter_key;
	int res = 0;
	unsigned int node_pos;
	int local_node_pos = -1;
	struct totem_ip_address bind_addr;
	int interface_up, interface_num;
	char tmp_key[ICMAP_KEYNAME_MAXLEN];
	char *node_addr_str;
	struct totem_ip_address node_addr;

	res = totemip_iface_check(&totem_config->interfaces[0].bindnet,
		&bind_addr, &interface_up, &interface_num,
		totem_config->clear_node_high_bit);
	if (res == -1) {
		return (-1);
	}

	iter = icmap_iter_init("nodelist.node.");
	while ((iter_key = icmap_iter_next(iter, NULL, NULL)) != NULL) {
		res = sscanf(iter_key, "nodelist.node.%u.%s", &node_pos, tmp_key);
		if (res != 2) {
			continue;
		}

		if (strcmp(tmp_key, "ring0_addr") != 0) {
			continue;
		}

		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.ring0_addr", node_pos);
		if (icmap_get_string(tmp_key, &node_addr_str) != CS_OK) {
			continue;
		}

		res = totemip_parse (&node_addr, node_addr_str, totem_config->ip_version);
		free(node_addr_str);
		if (res == -1) {
			continue ;
		}

		if (totemip_equal(&bind_addr, &node_addr)) {
			local_node_pos = node_pos;
		}
	}
	icmap_iter_finalize(iter);

	return (local_node_pos);
}

/*
 * Compute difference between two set of totem interface arrays. set1 and set2
 * are changed so for same ring, ip existing in both set1 and set2 are cleared
 * (set to 0), and ips which are only in set1 or set2 remains untouched.
 * totempg_node_add/remove is called.
 */
static void compute_interfaces_diff(struct totem_interface *set1,
	struct totem_interface *set2)
{
	int ring_no, set1_pos, set2_pos;
	struct totem_ip_address empty_ip_address;

	memset(&empty_ip_address, 0, sizeof(empty_ip_address));

	for (ring_no = 0; ring_no < INTERFACE_MAX; ring_no++) {
		if (!set1[ring_no].configured && !set2[ring_no].configured) {
			continue;
		}

		for (set1_pos = 0; set1_pos < set1[ring_no].member_count; set1_pos++) {
			for (set2_pos = 0; set2_pos < set2[ring_no].member_count; set2_pos++) {
				/*
				 * For current ring_no remove all set1 items existing
				 * in set2
				 */
				if (memcmp(&set1[ring_no].member_list[set1_pos],
				    &set2[ring_no].member_list[set2_pos],
				    sizeof(struct totem_ip_address)) == 0) {
					memset(&set1[ring_no].member_list[set1_pos], 0,
					    sizeof(struct totem_ip_address));
					memset(&set2[ring_no].member_list[set2_pos], 0,
					    sizeof(struct totem_ip_address));
				}
			}
		}
	}

	for (ring_no = 0; ring_no < INTERFACE_MAX; ring_no++) {
		for (set1_pos = 0; set1_pos < set1[ring_no].member_count; set1_pos++) {
			/*
			 * All items which remained in set1 doesn't exists in set2 any longer so
			 * node has to be removed.
			 */
			if (memcmp(&set1[ring_no].member_list[set1_pos], &empty_ip_address, sizeof(empty_ip_address)) != 0) {
				log_printf(LOGSYS_LEVEL_DEBUG,
					"removing dynamic member %s for ring %u",
					totemip_print(&set1[ring_no].member_list[set1_pos]),
					ring_no);

				totempg_member_remove(&set1[ring_no].member_list[set1_pos], ring_no);
			}
		}
		if (!set2[ring_no].configured) {
			continue;
		}
		for (set2_pos = 0; set2_pos < set2[ring_no].member_count; set2_pos++) {
			/*
			 * All items which remained in set2 doesn't existed in set1 so this is no node
			 * and has to be added.
			 */
			if (memcmp(&set2[ring_no].member_list[set2_pos], &empty_ip_address, sizeof(empty_ip_address)) != 0) {
				log_printf(LOGSYS_LEVEL_DEBUG,
					"adding dynamic member %s for ring %u",
					totemip_print(&set2[ring_no].member_list[set2_pos]),
					ring_no);

				totempg_member_add(&set2[ring_no].member_list[set2_pos], ring_no);
			}
		}
	}
}

/*
 * Reconfigure links in totempg. Sets new local IP address and adds params for new links.
 */
static void reconfigure_links(struct totem_config *totem_config)
{
	int i;
	char tmp_key[ICMAP_KEYNAME_MAXLEN];
	char *addr_string;
	struct totem_ip_address local_ip;
	int err;
	unsigned int local_node_pos = find_local_node_in_nodelist(totem_config);

	for (i = 0; i<INTERFACE_MAX; i++) {
		if (!totem_config->interfaces[i].configured) {
			continue;
		}

		log_printf(LOGSYS_LEVEL_INFO, "Configuring link %d\n", i);

		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.ring%u_addr", local_node_pos, i);
		if (icmap_get_string(tmp_key, &addr_string) != CS_OK) {
			continue;
		}

		err = totemip_parse(&local_ip, addr_string, AF_UNSPEC);
		if (err != 0) {
			continue;
		}
		local_ip.nodeid = totem_config->node_id;

		/* In case this is a new link, fill in the defaults if there was no interface{} section for it */
		if (!totem_config->interfaces[i].knet_link_priority)
			totem_config->interfaces[i].knet_link_priority = 1;
		if (!totem_config->interfaces[i].knet_ping_interval)
			totem_config->interfaces[i].knet_ping_interval = KNET_PING_INTERVAL;
		if (!totem_config->interfaces[i].knet_ping_timeout)
			totem_config->interfaces[i].knet_ping_timeout = KNET_PING_TIMEOUT;
		if (!totem_config->interfaces[i].knet_ping_precision)
			totem_config->interfaces[i].knet_ping_precision = KNET_PING_PRECISION;
		if (!totem_config->interfaces[i].knet_pong_count)
			totem_config->interfaces[i].knet_pong_count = KNET_PONG_COUNT;
		if (!totem_config->interfaces[i].knet_transport)
			totem_config->interfaces[i].knet_transport = KNET_TRANSPORT_UDP;
		if (!totem_config->interfaces[i].ip_port)
			totem_config->interfaces[i].ip_port = DEFAULT_PORT;

		totempg_iface_set(&local_ip, totem_config->interfaces[i].ip_port, i);
	}
}

static void put_nodelist_members_to_config(struct totem_config *totem_config, int reload)
{
	icmap_iter_t iter, iter2;
	const char *iter_key, *iter_key2;
	int res = 0;
	unsigned int node_pos;
	char tmp_key[ICMAP_KEYNAME_MAXLEN];
	char tmp_key2[ICMAP_KEYNAME_MAXLEN];
	char *node_addr_str;
	int member_count;
	unsigned int linknumber = 0;
	int i, j;
	struct totem_interface *new_interfaces = NULL;

	if (reload) {
		/*
		 * We need to compute diff only for reload. Also for initial configuration
		 * not all totem structures are initialized so corosync will crash during
		 * member_add/remove
		 */
		new_interfaces = malloc (sizeof (struct totem_interface) * INTERFACE_MAX);
		assert(new_interfaces != NULL);
	}

	/* Clear out nodelist so we can put the new one in if needed */
	for (i = 0; i < INTERFACE_MAX; i++) {
		for (j = 0; j < PROCESSOR_COUNT_MAX; j++) {
			memset(&totem_config->interfaces[i].member_list[j], 0, sizeof(struct totem_ip_address));
		}
		totem_config->interfaces[i].member_count = 0;
	}

	iter = icmap_iter_init("nodelist.node.");
	while ((iter_key = icmap_iter_next(iter, NULL, NULL)) != NULL) {
		res = sscanf(iter_key, "nodelist.node.%u.%s", &node_pos, tmp_key);
		if (res != 2) {
			continue;
		}

		if (strcmp(tmp_key, "ring0_addr") != 0) {
			continue;
		}

		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.", node_pos);
		iter2 = icmap_iter_init(tmp_key);
		while ((iter_key2 = icmap_iter_next(iter2, NULL, NULL)) != NULL) {
			unsigned int nodeid;

			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.nodeid", node_pos);
			if (icmap_get_uint32(tmp_key, &nodeid) != CS_OK) {
			}

			res = sscanf(iter_key2, "nodelist.node.%u.ring%u%s", &node_pos, &linknumber, tmp_key2);
			if (res != 3 || strcmp(tmp_key2, "_addr") != 0) {
				continue;
			}

			if (icmap_get_string(iter_key2, &node_addr_str) != CS_OK) {
				continue;
			}

			member_count = totem_config->interfaces[linknumber].member_count;

			res = totemip_parse(&totem_config->interfaces[linknumber].member_list[member_count],
						node_addr_str, totem_config->ip_version);
			if (res != -1) {
				totem_config->interfaces[linknumber].member_list[member_count].nodeid = nodeid;
				totem_config->interfaces[linknumber].member_count++;
			}

			totem_config->interfaces[linknumber].configured = 1;
			free(node_addr_str);
		}

		icmap_iter_finalize(iter2);
	}

	icmap_iter_finalize(iter);

	if (reload) {
		log_printf(LOGSYS_LEVEL_DEBUG, "About to reconfigure links from nodelist.\n");
		reconfigure_links(totem_config);

		memcpy(new_interfaces, totem_config->interfaces, sizeof (struct totem_interface) * INTERFACE_MAX);

		compute_interfaces_diff(totem_config->orig_interfaces, new_interfaces);

		free(new_interfaces);
	}
}

static void nodelist_dynamic_notify(
	int32_t event,
	const char *key_name,
	struct icmap_notify_value new_val,
	struct icmap_notify_value old_val,
	void *user_data)
{
	int res;
	unsigned int ring_no;
	unsigned int member_no;
	char tmp_str[ICMAP_KEYNAME_MAXLEN];
	uint8_t reloading;
	struct totem_config *totem_config = (struct totem_config *)user_data;

	/*
	* If a full reload is in progress then don't do anything until it's done and
	* can reconfigure it all atomically
	*/
	if (icmap_get_uint8("config.totemconfig_reload_in_progress", &reloading) == CS_OK && reloading) {
		return ;
	}

	res = sscanf(key_name, "nodelist.node.%u.ring%u%s", &member_no, &ring_no, tmp_str);
	if (res != 3)
		return ;

	if (strcmp(tmp_str, "_addr") != 0) {
		return;
	}

	put_nodelist_members_to_config(totem_config, 1);
}


/*
 * Tries to find node (node_pos) in config nodelist which address matches any
 * local interface. Address can be stored in ring0_addr or if ipaddr_key_prefix is not NULL
 * key with prefix ipaddr_key is used (there can be multiuple of them)
 * This function differs  * from find_local_node_in_nodelist because it doesn't need bindnetaddr,
 * but doesn't work when bind addr is network address (so IP must be exact
 * match).
 *
 * Returns 1 on success (address was found, node_pos is then correctly set) or 0 on failure.
 */
int totem_config_find_local_addr_in_nodelist(struct totem_config *totem_config, const char *ipaddr_key_prefix, unsigned int *node_pos)
{
	struct qb_list_head addrs;
	struct totem_ip_if_address *if_addr;
	icmap_iter_t iter, iter2;
	const char *iter_key, *iter_key2;
	struct qb_list_head *list;
	const char *ipaddr_key;
	int ip_version;
	struct totem_ip_address node_addr;
	char *node_addr_str;
	int node_found = 0;
	int res = 0;
	char tmp_key[ICMAP_KEYNAME_MAXLEN];

	if (totemip_getifaddrs(&addrs) == -1) {
		return 0;
	}

	ip_version = totem_config_get_ip_version(totem_config);

	iter = icmap_iter_init("nodelist.node.");

	while ((iter_key = icmap_iter_next(iter, NULL, NULL)) != NULL) {
		res = sscanf(iter_key, "nodelist.node.%u.%s", node_pos, tmp_key);
		if (res != 2) {
			continue;
		}

		if (strcmp(tmp_key, "ring0_addr") != 0) {
			continue;
		}

		if (icmap_get_string(iter_key, &node_addr_str) != CS_OK) {
			continue ;
		}

		free(node_addr_str);

		/*
		 * ring0_addr found -> let's iterate thru ipaddr_key_prefix
		 */
		snprintf(tmp_key, sizeof(tmp_key), "nodelist.node.%u.%s", *node_pos,
		    (ipaddr_key_prefix != NULL ? ipaddr_key_prefix : "ring0_addr"));

		iter2 = icmap_iter_init(tmp_key);
		while ((iter_key2 = icmap_iter_next(iter2, NULL, NULL)) != NULL) {
			/*
			 * ring0_addr must be exact match, not prefix
			 */
			ipaddr_key = (ipaddr_key_prefix != NULL ? iter_key2 : tmp_key);
			if (icmap_get_string(ipaddr_key, &node_addr_str) != CS_OK) {
				continue ;
			}

			if (totemip_parse(&node_addr, node_addr_str, ip_version) == -1) {
				free(node_addr_str);
				continue ;
			}
			free(node_addr_str);

			/*
			 * Try to match ip with if_addrs
			 */
			node_found = 0;
			qb_list_for_each(list, &(addrs)) {
				if_addr = qb_list_entry(list, struct totem_ip_if_address, list);

				if (totemip_equal(&node_addr, &if_addr->ip_addr)) {
					node_found = 1;
					break;
				}
			}

			if (node_found) {
				break ;
			}
		}

		icmap_iter_finalize(iter2);

		if (node_found) {
			break ;
		}
	}

	icmap_iter_finalize(iter);
	totemip_freeifaddrs(&addrs);

	return (node_found);
}

static void config_convert_nodelist_to_interface(struct totem_config *totem_config)
{
	int res = 0;
	unsigned int node_pos;
	char tmp_key[ICMAP_KEYNAME_MAXLEN];
	char tmp_key2[ICMAP_KEYNAME_MAXLEN];
	char *node_addr_str;
	unsigned int linknumber = 0;
	icmap_iter_t iter;
	const char *iter_key;

	if (totem_config_find_local_addr_in_nodelist(totem_config, NULL, &node_pos)) {
		/*
		 * We found node, so create interface section
		 */
		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.", node_pos);
		iter = icmap_iter_init(tmp_key);
		while ((iter_key = icmap_iter_next(iter, NULL, NULL)) != NULL) {
			res = sscanf(iter_key, "nodelist.node.%u.ring%u%s", &node_pos, &linknumber, tmp_key2);
			if (res != 3 || strcmp(tmp_key2, "_addr") != 0) {
				continue ;
			}

			if (icmap_get_string(iter_key, &node_addr_str) != CS_OK) {
				continue;
			}

			snprintf(tmp_key2, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.bindnetaddr", linknumber);
			icmap_set_string(tmp_key2, node_addr_str);
			free(node_addr_str);
		}
		icmap_iter_finalize(iter);
	}
}

static int get_interface_params(struct totem_config *totem_config,
				const char **error_string, uint64_t *warnings,
				int reload)
{
	int res = 0;
	unsigned int linknumber = 0;
	int member_count = 0;
	int i;
	icmap_iter_t iter, member_iter;
	const char *iter_key;
	const char *member_iter_key;
	char linknumber_key[ICMAP_KEYNAME_MAXLEN];
	char tmp_key[ICMAP_KEYNAME_MAXLEN];
	uint8_t u8;
	uint32_t u32;
	char *str;
	char *cluster_name = NULL;

	if (reload) {
		for (i=0; i<INTERFACE_MAX; i++) {
			totem_config->interfaces[i].configured = 0;
		}
	}
	if (icmap_get_string("totem.cluster_name", &cluster_name) != CS_OK) {
		cluster_name = NULL;
	}

	iter = icmap_iter_init("totem.interface.");
	while ((iter_key = icmap_iter_next(iter, NULL, NULL)) != NULL) {
		res = sscanf(iter_key, "totem.interface.%[^.].%s", linknumber_key, tmp_key);
		if (res != 2) {
			continue;
		}

		if (strcmp(tmp_key, "bindnetaddr") != 0 && totem_config->transport_number == TOTEM_TRANSPORT_UDP) {
			continue;
		}

		member_count = 0;
		linknumber = atoi(linknumber_key);

		if (linknumber >= INTERFACE_MAX) {
			free(cluster_name);

			snprintf (error_string_response, sizeof(error_string_response),
			    "parse error in config: interface ring number %u is bigger than allowed maximum %u\n",
			    linknumber, INTERFACE_MAX - 1);

			*error_string = error_string_response;
			return -1;
		}

		/* These things are only valid for the initial read */
		if (!reload) {
			/*
			 * Get the bind net address
			 */
			if (icmap_get_string(iter_key, &str) == CS_OK) {
				res = totemip_parse (&totem_config->interfaces[linknumber].bindnet, str,
						     totem_config->ip_version);
				free(str);
			}

			/*
			 * Get interface multicast address
			 */
			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.mcastaddr", linknumber);
			if (icmap_get_string(tmp_key, &str) == CS_OK) {
				res = totemip_parse (&totem_config->interfaces[linknumber].mcast_addr, str, totem_config->ip_version);
				free(str);
			} else {
				/*
				 * User not specified address -> autogenerate one from cluster_name key
				 * (if available). Return code is intentionally ignored, because
				 * udpu doesn't need mcastaddr and validity of mcastaddr for udp is
				 * checked later anyway.
				 */
				(void)get_cluster_mcast_addr (cluster_name,
							      linknumber,
							      totem_config->ip_version,
							      &totem_config->interfaces[linknumber].mcast_addr);
			}

			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.broadcast", linknumber);
			if (icmap_get_string(tmp_key, &str) == CS_OK) {
				if (strcmp (str, "yes") == 0) {
					totem_config->broadcast_use = 1;
				}
				free(str);
			}
		}

		/* These things are only valid for the initial read OR a newly-defined link */
		if (!reload || (totem_config->interfaces[linknumber].configured == 0)) {

			/*
			 * Get mcast port
			 */
			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.mcastport", linknumber);
			if (icmap_get_uint16(tmp_key, &totem_config->interfaces[linknumber].ip_port) != CS_OK) {
				if (totem_config->broadcast_use) {
					totem_config->interfaces[linknumber].ip_port = DEFAULT_PORT + (2 * linknumber);
				} else {
					totem_config->interfaces[linknumber].ip_port = DEFAULT_PORT;
				}
			}

			/*
			 * Get the TTL
			 */
			totem_config->interfaces[linknumber].ttl = 1;

			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.ttl", linknumber);

			if (icmap_get_uint8(tmp_key, &u8) == CS_OK) {
				totem_config->interfaces[linknumber].ttl = u8;
			}

			totem_config->interfaces[linknumber].knet_transport = KNET_DEFAULT_TRANSPORT;
			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.knet_transport", linknumber);
			if (icmap_get_string(tmp_key, &str) == CS_OK) {
				if (strcmp(str, "sctp") == 0) {
					totem_config->interfaces[linknumber].knet_transport = KNET_TRANSPORT_SCTP;
				}
				else if (strcmp(str, "udp") == 0) {
					totem_config->interfaces[linknumber].knet_transport = KNET_TRANSPORT_UDP;
				}
				else {
					*error_string = "Unrecognised knet_transport. expected 'udp' or 'sctp'";
					return -1;
				}
			}
		}
		totem_config->interfaces[linknumber].configured = 1;

		/*
		 * Get the knet link params
		 */
		totem_config->interfaces[linknumber].knet_link_priority = 1;
		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.knet_link_priority", linknumber);

		if (icmap_get_uint8(tmp_key, &u8) == CS_OK) {
			totem_config->interfaces[linknumber].knet_link_priority = u8;
		}

		totem_config->interfaces[linknumber].knet_ping_interval = KNET_PING_INTERVAL;
		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.knet_ping_interval", linknumber);
		if (icmap_get_uint32(tmp_key, &u32) == CS_OK) {
			totem_config->interfaces[linknumber].knet_ping_interval = u32;
		}
		totem_config->interfaces[linknumber].knet_ping_timeout = KNET_PING_TIMEOUT;
		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.knet_ping_timeout", linknumber);
		if (icmap_get_uint32(tmp_key, &u32) == CS_OK) {
			totem_config->interfaces[linknumber].knet_ping_timeout = u32;
		}
		totem_config->interfaces[linknumber].knet_ping_precision = KNET_PING_PRECISION;
		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.knet_ping_precision", linknumber);
		if (icmap_get_uint32(tmp_key, &u32) == CS_OK) {
			totem_config->interfaces[linknumber].knet_ping_precision = u32;
		}
		totem_config->interfaces[linknumber].knet_pong_count = KNET_PONG_COUNT;
		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.knet_pong_count", linknumber);
		if (icmap_get_uint32(tmp_key, &u32) == CS_OK) {
			totem_config->interfaces[linknumber].knet_pong_count = u32;
		}

		snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.member.", linknumber);
		member_iter = icmap_iter_init(tmp_key);
		while ((member_iter_key = icmap_iter_next(member_iter, NULL, NULL)) != NULL) {
			if (member_count == 0) {
				if (icmap_get_string("nodelist.node.0.ring0_addr", &str) == CS_OK) {
					free(str);
					*warnings |= TOTEM_CONFIG_WARNING_MEMBERS_IGNORED;
					break;
				} else {
					*warnings |= TOTEM_CONFIG_WARNING_MEMBERS_DEPRECATED;
				}
			}

			if (icmap_get_string(member_iter_key, &str) == CS_OK) {
				res = totemip_parse (&totem_config->interfaces[linknumber].member_list[member_count++],
						str, totem_config->ip_version);
			}
		}
		icmap_iter_finalize(member_iter);

		totem_config->interfaces[linknumber].member_count = member_count;

	}
	icmap_iter_finalize(iter);

	return 0;
}

extern int totem_config_read (
	struct totem_config *totem_config,
	const char **error_string,
	uint64_t *warnings)
{
	int res = 0;
	char *str, *ring0_addr_str;
	char tmp_key[ICMAP_KEYNAME_MAXLEN];
	uint16_t u16;
	int i;
	int local_node_pos;
	int nodeid_set;

	*warnings = 0;

	memset (totem_config, 0, sizeof (struct totem_config));
	totem_config->interfaces = malloc (sizeof (struct totem_interface) * INTERFACE_MAX);
	if (totem_config->interfaces == 0) {
		*error_string = "Out of memory trying to allocate ethernet interface storage area";
		return -1;
	}

	totem_config->transport_number = TOTEM_TRANSPORT_KNET;
	if (icmap_get_string("totem.transport", &str) == CS_OK) {
		if (strcmp (str, "udpu") == 0) {
			totem_config->transport_number = TOTEM_TRANSPORT_UDPU;
		}

		if (strcmp (str, "udp") == 0) {
			totem_config->transport_number = TOTEM_TRANSPORT_UDP;
		}

		if (strcmp (str, "knet") == 0) {
			totem_config->transport_number = TOTEM_TRANSPORT_KNET;
		}

		free(str);
	}

	memset (totem_config->interfaces, 0,
		sizeof (struct totem_interface) * INTERFACE_MAX);

	strcpy (totem_config->link_mode, "passive");

	icmap_get_uint32("totem.version", (uint32_t *)&totem_config->version);

	if (totem_get_crypto(totem_config, error_string) != 0) {
		return -1;
	}

	if (icmap_get_string("totem.link_mode", &str) == CS_OK) {
		if (strlen(str) >= TOTEM_LINK_MODE_BYTES) {
			*error_string = "totem.link_mode is too long";
			free(str);

			return -1;
		}
		strcpy (totem_config->link_mode, str);
		free(str);
	}

	icmap_get_uint32("totem.nodeid", &totem_config->node_id);

	totem_config->clear_node_high_bit = 0;
	if (icmap_get_string("totem.clear_node_high_bit", &str) == CS_OK) {
		if (strcmp (str, "yes") == 0) {
			totem_config->clear_node_high_bit = 1;
		}
		free(str);
	}

	icmap_get_uint32("totem.threads", &totem_config->threads);

	icmap_get_uint32("totem.netmtu", &totem_config->net_mtu);

	totem_config->ip_version = totem_config_get_ip_version(totem_config);

	if (icmap_get_string("totem.interface.0.bindnetaddr", &str) != CS_OK) {
		/*
		 * We were not able to find ring 0 bindnet addr. Try to use nodelist informations
		 */
		config_convert_nodelist_to_interface(totem_config);
	} else {
		if (icmap_get_string("nodelist.node.0.ring0_addr", &ring0_addr_str) == CS_OK) {
			/*
			 * Both bindnetaddr and ring0_addr are set.
			 * Log warning information, and use nodelist instead
			 */
			*warnings |= TOTEM_CONFIG_BINDNETADDR_NODELIST_SET;

			config_convert_nodelist_to_interface(totem_config);

			free(ring0_addr_str);
		}

		free(str);
	}

	/*
	 * Broadcast option is global but set in interface section,
	 * so reset before processing interfaces.
	 */
	totem_config->broadcast_use = 0;

	res = get_interface_params(totem_config, error_string, warnings, 0);
	if (res < 0) {
		return res;
	}

	/*
	 * Use broadcast is global, so if set, make sure to fill mcast addr correctly
	 * broadcast is only supported for UDP so just do interface 0;
	 */
	if (totem_config->broadcast_use) {
		totemip_parse (&totem_config->interfaces[0].mcast_addr,
			       "255.255.255.255", 0);
	}


	/*
	 * Store automatically generated items back to icmap only for UDP
	 */
	if (totem_config->transport_number == TOTEM_TRANSPORT_UDP) {
		for (i = 0; i < INTERFACE_MAX; i++) {
			if (!totem_config->interfaces[i].configured) {
				continue;
			}
			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.mcastaddr", i);
			if (icmap_get_string(tmp_key, &str) == CS_OK) {
				free(str);
			} else {
				str = (char *)totemip_print(&totem_config->interfaces[i].mcast_addr);
				icmap_set_string(tmp_key, str);
			}

			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "totem.interface.%u.mcastport", i);
			if (icmap_get_uint16(tmp_key, &u16) != CS_OK) {
				icmap_set_uint16(tmp_key, totem_config->interfaces[i].ip_port);
			}
		}
	}

	/*
	 * Check existence of nodelist
	 */
	if (icmap_get_string("nodelist.node.0.ring0_addr", &str) == CS_OK) {
		free(str);
		/*
		 * find local node
		 */
		local_node_pos = find_local_node_in_nodelist(totem_config);
		if (local_node_pos != -1) {
			icmap_set_uint32("nodelist.local_node_pos", local_node_pos);

			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.nodeid", local_node_pos);

			nodeid_set = (totem_config->node_id != 0);
			if (icmap_get_uint32(tmp_key, &totem_config->node_id) == CS_OK && nodeid_set) {
				*warnings |= TOTEM_CONFIG_WARNING_TOTEM_NODEID_IGNORED;
			}
			if ((totem_config->transport_number == TOTEM_TRANSPORT_KNET) && (!totem_config->node_id)) {
				*error_string = "With knet, you must specify nodeid for current node";
				return -1;
			}

			/*
			 * Make localnode ring0_addr read only, so we can be sure that local
			 * node never changes. If rebinding to other IP would be in future
			 * supported, this must be changed and handled properly!
			 */
			snprintf(tmp_key, ICMAP_KEYNAME_MAXLEN, "nodelist.node.%u.ring0_addr", local_node_pos);
			icmap_set_ro_access(tmp_key, 0, 1);
			icmap_set_ro_access("nodelist.local_node_pos", 0, 1);
		}

		put_nodelist_members_to_config(totem_config, 0);
	}

	/*
	 * Get things that might change in the future (and can depend on totem_config->interfaces);
	 */
	totem_volatile_config_read(totem_config, NULL);

	icmap_set_uint8("config.totemconfig_reload_in_progress", 0);

	add_totem_config_notification(totem_config);

	return 0;
}


int totem_config_validate (
	struct totem_config *totem_config,
	const char **error_string)
{
	static char local_error_reason[512];
	char parse_error[512];
	const char *error_reason = local_error_reason;
	int i,j;
	uint32_t u32;
	int num_configured = 0;
	unsigned int interface_max = INTERFACE_MAX;



	for (i = 0; i < INTERFACE_MAX; i++) {
		if (totem_config->interfaces[i].configured) {
			num_configured++;
		}
	}
	if (num_configured == 0) {
		error_reason = "No interfaces defined";
		goto parse_error;
	}

	/* Check we found a local node address */
	if (icmap_get_uint32("nodelist.local_node_pos", &u32) != CS_OK) {
		error_reason = "No valid address found for local host";
		goto parse_error;
	}

	for (i = 0; i < INTERFACE_MAX; i++) {
		/*
		 * Some error checking of parsed data to make sure its valid
		 */

		struct totem_ip_address null_addr;

		if (!totem_config->interfaces[i].configured) {
			continue;
		}

		memset (&null_addr, 0, sizeof (struct totem_ip_address));

		if ((totem_config->transport_number == TOTEM_TRANSPORT_UDP) &&
			memcmp (&totem_config->interfaces[i].mcast_addr, &null_addr,
				sizeof (struct totem_ip_address)) == 0) {
			error_reason = "No multicast address specified";
			goto parse_error;
		}

		if (totem_config->interfaces[i].ip_port == 0) {
			error_reason = "No multicast port specified";
			goto parse_error;
		}

		if (totem_config->interfaces[i].ttl > 255) {
			error_reason = "Invalid TTL (should be 0..255)";
			goto parse_error;
		}
		if (totem_config->transport_number != TOTEM_TRANSPORT_UDP &&
		    totem_config->interfaces[i].ttl != 1) {
			error_reason = "Can only set ttl on multicast transport types";
			goto parse_error;
		}
		if (totem_config->interfaces[i].knet_link_priority > 255) {
			error_reason = "Invalid link priority (should be 0..255)";
			goto parse_error;
		}
		if (totem_config->transport_number != TOTEM_TRANSPORT_KNET &&
		    totem_config->interfaces[i].knet_link_priority != 1) {
			error_reason = "Can only set link priority on knet transport type";
			goto parse_error;
		}

		if (totem_config->interfaces[i].mcast_addr.family == AF_INET6 &&
			totem_config->node_id == 0) {

			error_reason = "An IPV6 network requires that a node ID be specified.";
			goto parse_error;
		}

		if (totem_config->broadcast_use == 0 && totem_config->transport_number == TOTEM_TRANSPORT_UDP) {
			if (totem_config->interfaces[i].mcast_addr.family != totem_config->interfaces[i].bindnet.family) {
				error_reason = "Multicast address family does not match bind address family";
				goto parse_error;
			}

			if (totemip_is_mcast (&totem_config->interfaces[i].mcast_addr) != 0) {
				error_reason = "mcastaddr is not a correct multicast address.";
				goto parse_error;
			}
		}
		/* Verify that all nodes on the same knet link have the same IP family */
		for (j=1; j<totem_config->interfaces[i].member_count; j++) {
			if (totem_config->interfaces[i].configured) {
				if (totem_config->interfaces[i].member_list[j].family !=
				    totem_config->interfaces[i].member_list[0].family) {
					snprintf (local_error_reason, sizeof(local_error_reason),
						  "Nodes for link %d have different IP families", i);
					goto parse_error;
				}
			}
		}
	}

	if (totem_config->version != 2) {
		error_reason = "This totem parser can only parse version 2 configurations.";
		goto parse_error;
	}

	if (totem_volatile_config_validate(totem_config, error_string) == -1) {
		return (-1);
	}

	if (check_for_duplicate_nodeids(totem_config, error_string) == -1) {
		return (-1);
	}

	/*
	 * KNET Link values validation
	 */
	if (strcmp (totem_config->link_mode, "active") &&
	        strcmp (totem_config->link_mode, "rr") &&
		strcmp (totem_config->link_mode, "passive")) {
		snprintf (local_error_reason, sizeof(local_error_reason),
			"The Knet link mode \"%s\" specified is invalid.  It must be active, passive or rr.\n", totem_config->link_mode);
		goto parse_error;
	}

	/* Only Knet does multiple interfaces */
	if (totem_config->transport_number != TOTEM_TRANSPORT_KNET) {
		interface_max = 1;
	}

	if (interface_max < num_configured) {
		snprintf (parse_error, sizeof(parse_error),
			  "%d is too many configured interfaces for non-Knet transport.",
			  num_configured);
		error_reason = parse_error;
		goto parse_error;
	}

	/* Only knet allows crypto */
	if (totem_config->transport_number != TOTEM_TRANSPORT_KNET) {
		if ((strcmp(totem_config->crypto_cipher_type, "none") != 0) ||
		    (strcmp(totem_config->crypto_hash_type, "none") != 0)) {

			snprintf (parse_error, sizeof(parse_error),
				  "crypto_cipher & crypto_hash are only valid for the Knet transport.");
			error_reason = parse_error;
			goto parse_error;
		}
	}

	if (totem_config->net_mtu == 0) {
		if (totem_config->transport_number == TOTEM_TRANSPORT_KNET) {
			totem_config->net_mtu = KNET_MAX_PACKET_SIZE;
		}
		else {
			totem_config->net_mtu = 1500;
		}
	}

	return 0;

parse_error:
	snprintf (error_string_response, sizeof(error_string_response),
		 "parse error in config: %s\n", error_reason);
	*error_string = error_string_response;
	return (-1);

}

static int read_keyfile (
	const char *key_location,
	struct totem_config *totem_config,
	const char **error_string)
{
	int fd;
	int res;
	int saved_errno;
	char error_str[100];
	const char *error_ptr;

	fd = open (key_location, O_RDONLY);
	if (fd == -1) {
		error_ptr = qb_strerror_r(errno, error_str, sizeof(error_str));
		snprintf (error_string_response, sizeof(error_string_response),
			"Could not open %s: %s\n",
			 key_location, error_ptr);
		goto parse_error;
	}

	res = read (fd, totem_config->private_key, TOTEM_PRIVATE_KEY_LEN_MAX);
	saved_errno = errno;
	close (fd);

	if (res == -1) {
		error_ptr = qb_strerror_r (saved_errno, error_str, sizeof(error_str));
		snprintf (error_string_response, sizeof(error_string_response),
			"Could not read %s: %s\n",
			 key_location, error_ptr);
		goto parse_error;
	}

	if (res < TOTEM_PRIVATE_KEY_LEN_MIN) {
		snprintf (error_string_response, sizeof(error_string_response),
			"Could only read %d bits of minimum %u bits from %s.\n",
			 res * 8, TOTEM_PRIVATE_KEY_LEN_MIN * 8, key_location);
		goto parse_error;
	}

	totem_config->private_key_len = res;

	return 0;

parse_error:
	*error_string = error_string_response;
	return (-1);
}

int totem_config_keyread (
	struct totem_config *totem_config,
	const char **error_string)
{
	int got_key = 0;
	char *key_location = NULL;
	int res;
	size_t key_len;

	memset (totem_config->private_key, 0, sizeof(totem_config->private_key));
	totem_config->private_key_len = 0;

	if (strcmp(totem_config->crypto_cipher_type, "none") == 0 &&
	    strcmp(totem_config->crypto_hash_type, "none") == 0) {
		return (0);
	}

	/* cmap may store the location of the key file */
	if (icmap_get_string("totem.keyfile", &key_location) == CS_OK) {
		res = read_keyfile(key_location, totem_config, error_string);
		free(key_location);
		if (res)  {
			goto key_error;
		}
		got_key = 1;
	} else { /* Or the key itself may be in the cmap */
		if (icmap_get("totem.key", NULL, &key_len, NULL) == CS_OK) {
			if (key_len > sizeof(totem_config->private_key)) {
				sprintf(error_string_response, "key is too long");
				goto key_error;
			}
			if (key_len < TOTEM_PRIVATE_KEY_LEN_MIN) {
				sprintf(error_string_response, "key is too short");
				goto key_error;
			}
			if (icmap_get("totem.key", totem_config->private_key, &key_len, NULL) == CS_OK) {
				totem_config->private_key_len = key_len;
				got_key = 1;
			} else {
				sprintf(error_string_response, "can't load private key");
				goto key_error;
			}
		}
	}

	/* In desperation we read the default filename */
	if (!got_key) {
		const char *filename = getenv("COROSYNC_TOTEM_AUTHKEY_FILE");
		if (!filename)
			filename = COROSYSCONFDIR "/authkey";
		res = read_keyfile(filename, totem_config, error_string);
		if (res)
			goto key_error;

	}

	return (0);

key_error:
	*error_string = error_string_response;
	return (-1);

}

static void debug_dump_totem_config(const struct totem_config *totem_config)
{

	log_printf(LOGSYS_LEVEL_DEBUG, "Token Timeout (%d ms) retransmit timeout (%d ms)",
	    totem_config->token_timeout, totem_config->token_retransmit_timeout);
	log_printf(LOGSYS_LEVEL_DEBUG, "token hold (%d ms) retransmits before loss (%d retrans)",
	    totem_config->token_hold_timeout, totem_config->token_retransmits_before_loss_const);
	log_printf(LOGSYS_LEVEL_DEBUG, "join (%d ms) send_join (%d ms) consensus (%d ms) merge (%d ms)",
	    totem_config->join_timeout, totem_config->send_join_timeout, totem_config->consensus_timeout,
	    totem_config->merge_timeout);
	log_printf(LOGSYS_LEVEL_DEBUG, "downcheck (%d ms) fail to recv const (%d msgs)",
	    totem_config->downcheck_timeout, totem_config->fail_to_recv_const);
	log_printf(LOGSYS_LEVEL_DEBUG,
	    "seqno unchanged const (%d rotations) Maximum network MTU %d",
	    totem_config->seqno_unchanged_const, totem_config->net_mtu);
	log_printf(LOGSYS_LEVEL_DEBUG,
	    "window size per rotation (%d messages) maximum messages per rotation (%d messages)",
	    totem_config->window_size, totem_config->max_messages);
	log_printf(LOGSYS_LEVEL_DEBUG, "missed count const (%d messages)", totem_config->miss_count_const);
	log_printf(LOGSYS_LEVEL_DEBUG, "heartbeat_failures_allowed (%d)",
	    totem_config->heartbeat_failures_allowed);
	log_printf(LOGSYS_LEVEL_DEBUG, "max_network_delay (%d ms)", totem_config->max_network_delay);
}

static void totem_change_notify(
	int32_t event,
	const char *key_name,
	struct icmap_notify_value new_val,
	struct icmap_notify_value old_val,
	void *user_data)
{
	struct totem_config *totem_config = (struct totem_config *)user_data;
	uint32_t *param;
	uint8_t reloading;
	const char *deleted_key = NULL;
	const char *error_string;

	/*
	 * If a full reload is in progress then don't do anything until it's done and
	 * can reconfigure it all atomically
	 */
	if (icmap_get_uint8("config.reload_in_progress", &reloading) == CS_OK && reloading)
		return;

	param = totem_get_param_by_name((struct totem_config *)user_data, key_name);
	/*
	 * Process change only if changed key is found in totem_config (-> param is not NULL)
	 * or for special key token_coefficient. token_coefficient key is not stored in
	 * totem_config, but it is used for computation of token timeout.
	 */
	if (!param && strcmp(key_name, "totem.token_coefficient") != 0)
		return;

	/*
	 * Values other than UINT32 are not supported, or needed (yet)
	 */
	switch (event) {
	case ICMAP_TRACK_DELETE:
		deleted_key = key_name;
		break;
	case ICMAP_TRACK_ADD:
	case ICMAP_TRACK_MODIFY:
		deleted_key = NULL;
		break;
	default:
		break;
	}

	totem_volatile_config_read (totem_config, deleted_key);
	log_printf(LOGSYS_LEVEL_DEBUG, "Totem related config key changed. Dumping actual totem config.");
	debug_dump_totem_config(totem_config);
	if (totem_volatile_config_validate(totem_config, &error_string) == -1) {
		log_printf (LOGSYS_LEVEL_ERROR, "%s", error_string);
		/*
		 * TODO: Consider corosync exit and/or load defaults for volatile
		 * values. For now, log error seems to be enough
		 */
	}
}

static void totem_reload_notify(
	int32_t event,
	const char *key_name,
	struct icmap_notify_value new_val,
	struct icmap_notify_value old_val,
	void *user_data)
{
	struct totem_config *totem_config = (struct totem_config *)user_data;
	uint32_t local_node_pos;
	const char *error_string;
	uint64_t warnings;

	/* Reload has completed */
	if (*(uint8_t *)new_val.data == 0) {

		totem_config->orig_interfaces = malloc (sizeof (struct totem_interface) * INTERFACE_MAX);
		assert(totem_config->orig_interfaces != NULL);
		memcpy(totem_config->orig_interfaces, totem_config->interfaces, sizeof (struct totem_interface) * INTERFACE_MAX);

		get_interface_params(totem_config, &error_string, &warnings, 1);
		put_nodelist_members_to_config (totem_config, 1);
		totem_volatile_config_read (totem_config, NULL);
		log_printf(LOGSYS_LEVEL_DEBUG, "Configuration reloaded. Dumping actual totem config.");
		debug_dump_totem_config(totem_config);
		if (totem_volatile_config_validate(totem_config, &error_string) == -1) {
			log_printf (LOGSYS_LEVEL_ERROR, "%s", error_string);
			/*
			 * TODO: Consider corosync exit and/or load defaults for volatile
			 * values. For now, log error seems to be enough
			 */
		}

		/* Reinstate the local_node_pos */
		local_node_pos = find_local_node_in_nodelist(totem_config);
		if (local_node_pos != -1) {
			icmap_set_uint32("nodelist.local_node_pos", local_node_pos);
		}

		/* Reconfigure network params as appropriate */
		totempg_reconfigure();

		free(totem_config->orig_interfaces);

		icmap_set_uint8("config.totemconfig_reload_in_progress", 0);
	} else {
		icmap_set_uint8("config.totemconfig_reload_in_progress", 1);
	}
}

static void add_totem_config_notification(struct totem_config *totem_config)
{
	icmap_track_t icmap_track;

	icmap_track_add("totem.",
		ICMAP_TRACK_ADD | ICMAP_TRACK_DELETE | ICMAP_TRACK_MODIFY | ICMAP_TRACK_PREFIX,
		totem_change_notify,
		totem_config,
		&icmap_track);

	icmap_track_add("config.reload_in_progress",
		ICMAP_TRACK_ADD | ICMAP_TRACK_MODIFY,
		totem_reload_notify,
		totem_config,
		&icmap_track);

	icmap_track_add("nodelist.node.",
		ICMAP_TRACK_ADD | ICMAP_TRACK_DELETE | ICMAP_TRACK_MODIFY | ICMAP_TRACK_PREFIX,
		nodelist_dynamic_notify,
		(void *)totem_config,
		&icmap_track);
}
