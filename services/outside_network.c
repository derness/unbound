/*
 * services/outside_network.c - implement sending of queries and wait answer.
 *
 * Copyright (c) 2007, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * This file has functions to send queries to authoritative servers and
 * wait for the pending answer events.
 */

#include "services/outside_network.h"
#include "services/listen_dnsport.h"
#include "util/netevent.h"
#include "util/log.h"

#ifdef HAVE_SYS_TYPES_H
#  include <sys/types.h>
#endif
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>

/** compare function of pending rbtree */
static int pending_cmp(const void* key1, const void* key2)
{
	struct pending *p1 = (struct pending*)key1;
	struct pending *p2 = (struct pending*)key2;
	if(p1->id < p2->id)
		return -1;
	if(p1->id > p2->id)
		return 1;
	log_assert(p1->id == p2->id);
	if(p1->addrlen < p2->addrlen)
		return -1;
	if(p1->addrlen > p2->addrlen)
		return 1;
	log_assert(p1->addrlen == p2->addrlen);
	return memcmp(&p1->addr, &p2->addr, p1->addrlen);
}

/** compare function of pending_timeout rbtree */
static int pending_timeout_cmp(const void* key1, const void* key2)
{
	struct pending_timeout *p1 = (struct pending_timeout*)key1;
	struct pending_timeout *p2 = (struct pending_timeout*)key2;
	if(p1->timeout.tv_sec < p2->timeout.tv_sec)
		return -1;
	if(p1->timeout.tv_sec > p2->timeout.tv_sec)
		return 1;
	log_assert(p1->timeout.tv_sec == p2->timeout.tv_sec);
	if(p1->timeout.tv_usec < p2->timeout.tv_usec)
		return -1;
	if(p1->timeout.tv_usec > p2->timeout.tv_usec)
		return 1;
	log_assert(p1->timeout.tv_usec == p2->timeout.tv_usec);
	if(p1 < p2)
		return -1;
	if(p1 > p2)
		return 1;
	return 0;
}

/** callback for incoming udp answers from the network. */
static int outnet_udp_cb(struct comm_point* c, void* my_arg, int error,
	struct comm_reply *reply_info)
{
	log_info("answer cb");
	return 0;
}

/** open another udp port to listen to, every thread has its own range
  * of open ports.
  * @param ifname: on which interface to open the port.
  * @param hints: hints on family and passiveness preset.
  * @param porthint: if not -1, it gives the port to base range on.
  * @return: file descriptor
  */
static int open_udp_port_range(const char* ifname, struct addrinfo* hints,
	int porthint)
{
	struct addrinfo *res = NULL;
	int r, s;
	char portstr[20];
	if(porthint != -1)
		snprintf(portstr, sizeof(portstr), "%d", porthint);

	if((r=getaddrinfo(ifname, ((porthint==-1)?NULL:portstr), hints, 
		&res)) != 0 || !res) {
		log_err("node %s:%s getaddrinfo: %s %s",
			ifname?ifname:"default", (porthint!=-1)?portstr:"eph", 
			gai_strerror(r), r==EAI_SYSTEM?strerror(errno):"");
		return -1;
	}
	s = create_udp_sock(res);
	freeaddrinfo(res);
	return s;
}

/**
 * Create range of UDP ports on the given interface.
 */
static int 
make_udp_range(struct outside_network* outnet, const char* ifname, 
	size_t num_ports, size_t off, int do_ip4, int do_ip6, int porthint)
{
	size_t i;
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_flags = AI_PASSIVE;
	if(ifname)
		hints.ai_flags |= AI_NUMERICHOST;
	hints.ai_family = AF_UNSPEC;
	if(do_ip4 && do_ip6)
		hints.ai_family = AF_UNSPEC;
	else if(do_ip4)
		hints.ai_family = AF_INET;
	else if(do_ip6)
		hints.ai_family = AF_INET6;
	hints.ai_socktype = SOCK_DGRAM;
	for(i=0; i<num_ports; i++) {
		int fd = open_udp_port_range(ifname, &hints, porthint);
		if(fd == -1) {
			return 0;
		}
		if(porthint != -1) 
			porthint++;
		outnet->udp_ports[i+off] = comm_point_create_udp(
			outnet->base, fd, outnet->udp_buff, outnet_udp_cb,
			outnet);
		if(!outnet->udp_ports[i+off]) {
			return 0;
		}
	}
	return 1;
}

struct outside_network* 
outside_network_create(struct comm_base *base, size_t bufsize, 
	size_t num_ports, const char** ifs, int num_ifs, int do_ip4, 
	int do_ip6, int port_base)
{
	struct outside_network* outnet = (struct outside_network*)
		calloc(1, sizeof(struct outside_network));
	int k;
	if(!outnet) {
		log_err("malloc failed");
		return NULL;
	}
	outnet->base = base;
	outnet->num_udp = (num_ifs?num_ports * num_ifs:num_ports);
	if(	!(outnet->udp_buff = ldns_buffer_new(bufsize)) ||
		!(outnet->udp_ports = (struct comm_point **)calloc(
			outnet->num_udp, sizeof(struct comm_point*))) ||
		!(outnet->pending = rbtree_create(pending_cmp)) ||
		!(outnet->pending_timeout = rbtree_create(
			pending_timeout_cmp))) {
		log_err("malloc failed");
		outside_network_delete(outnet);
		return NULL;
	}
	if(num_ifs == 0) {
		if(!make_udp_range(outnet, NULL, num_ports, 0, do_ip4, 
			do_ip6, port_base)) {
			outside_network_delete(outnet);
			return NULL;
		}
	}
	else for(k=0; k<num_ifs; k++) {
		if(!make_udp_range(outnet, ifs[k], num_ports, k*num_ports, 
			do_ip4, do_ip6, port_base)) {
			outside_network_delete(outnet);
			return NULL;
		}
	}

	return outnet;
}

void outside_network_delete(struct outside_network* outnet)
{
	if(!outnet)
		return;
	/* check every element, since we can be called on malloc error */
	if(outnet->pending) {
		struct pending *p, *np;
		p = (struct pending*)rbtree_first(outnet->pending);
		while(p) {
			np = (struct pending*)rbtree_next((rbnode_t*)p);
			pending_delete(NULL, p);
			p = np;
		}
		free(outnet->pending);
	}
	if(outnet->pending_timeout) {
		log_assert(outnet->pending_timeout->count == 0);
		free(outnet->pending_timeout);
	}
	if(outnet->udp_buff)
		ldns_buffer_free(outnet->udp_buff);
	if(outnet->udp_ports) {
		size_t i;
		for(i=0; i<outnet->num_udp; i++) {
			if(outnet->udp_ports[i])
				comm_point_delete(outnet->udp_ports[i]);
		}
		free(outnet->udp_ports);
	}
	free(outnet);
}

void pending_delete(struct outside_network* outnet, struct pending* p)
{
	if(!p)
		return;
	if(outnet) {
		(void)rbtree_delete(outnet->pending, p->node.key);
		(void)rbtree_delete(outnet->pending_timeout, 
			p->timeout->node.key);
	}
	free(p->timeout);
	free(p);
}

void pending_udp_query(struct outside_network* outnet, ldns_buffer* packet, 
	struct sockaddr_storage* addr, socklen_t addrlen, int timeout)
{
	/* choose a random outgoing port and interface */
	/* uses lousy random() function. TODO: entropy source. */
	double precho = (double)random() * (double)outnet->num_udp / 
		((double)RAND_MAX + 1.0);
	int chosen = (int)precho;
	/* don't trust in perfect double rounding */
	if(chosen < 0) chosen = 0;
	if(chosen >= (int)outnet->num_udp) chosen = (int)outnet->num_udp-1;
}
