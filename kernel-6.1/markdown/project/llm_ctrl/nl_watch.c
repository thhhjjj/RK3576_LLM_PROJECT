/* SPDX-License-Identifier: GPL-2.0 */
 /*
 * Userspace listener for llm_ctrl genetlink OVERTEMP multicast.
 * Static-linked for Buildroot; run before lowering threshold_c.
 *
 * NLA_* helpers are defined locally: glibc UAPI does not export them
 * as macros the way the kernel tree does.
 */

#include <errno.h>
#include <linux/genetlink.h>
#include <linux/netlink.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "llm_ctrl.h"

#ifndef NLA_ALIGNTO
#define NLA_ALIGNTO 4
#endif
#ifndef NLA_ALIGN
#define NLA_ALIGN(len) (((len) + NLA_ALIGNTO - 1) & ~(NLA_ALIGNTO - 1))
#endif
#ifndef NLA_HDRLEN
#define NLA_HDRLEN ((int)NLA_ALIGN(sizeof(struct nlattr)))
#endif
#ifndef NLA_OK
#define NLA_OK(nla, len) \
	((len) >= (int)sizeof(struct nlattr) && \
	 (nla)->nla_len >= sizeof(struct nlattr) && \
	 (nla)->nla_len <= (len))
#endif
#ifndef NLA_NEXT
#define NLA_NEXT(nla, attrlen) \
	((attrlen) -= NLA_ALIGN((nla)->nla_len), \
	 (struct nlattr *)(((char *)(nla)) + NLA_ALIGN((nla)->nla_len)))
#endif
#ifndef NLA_DATA
#define NLA_DATA(nla) ((void *)(((char *)(nla)) + NLA_HDRLEN))
#endif
#ifndef NLA_PAYLOAD
#define NLA_PAYLOAD(nla) ((int)((nla)->nla_len - NLA_HDRLEN))
#endif

static int nl_send(int fd, struct nlmsghdr *nlh)
{
	struct sockaddr_nl addr = { .nl_family = AF_NETLINK };

	return sendto(fd, nlh, nlh->nlmsg_len, 0,
		      (struct sockaddr *)&addr, sizeof(addr));
}

static int resolve_family(int fd, int *family_id, int *mcast_id)
{
	char buf[512];
	struct nlmsghdr *nlh;
	struct genlmsghdr *gh;
	struct nlattr *na;
	int len, rem;
	uint16_t id = 0;
	uint32_t grp = 0;
	int got_id = 0, got_grp = 0;

	memset(buf, 0, sizeof(buf));
	nlh = (struct nlmsghdr *)buf;
	nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
	nlh->nlmsg_type = GENL_ID_CTRL;
	nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
	nlh->nlmsg_seq = 1;
	nlh->nlmsg_pid = getpid();

	gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
	gh->cmd = CTRL_CMD_GETFAMILY;
	gh->version = 1;

	na = (struct nlattr *)(buf + NLMSG_ALIGN(nlh->nlmsg_len));
	na->nla_type = CTRL_ATTR_FAMILY_NAME;
	na->nla_len = NLA_HDRLEN + strlen(LLM_CTRL_GENL_NAME) + 1;
	memcpy(NLA_DATA(na), LLM_CTRL_GENL_NAME, strlen(LLM_CTRL_GENL_NAME) + 1);
	nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + NLA_ALIGN(na->nla_len);

	if (nl_send(fd, nlh) < 0)
		return -1;

	len = recv(fd, buf, sizeof(buf), 0);
	if (len < 0)
		return -1;

	for (nlh = (struct nlmsghdr *)buf; NLMSG_OK(nlh, len);
	     nlh = NLMSG_NEXT(nlh, len)) {
		if (nlh->nlmsg_type == NLMSG_ERROR)
			return -1;
		if (nlh->nlmsg_type != GENL_ID_CTRL)
			continue;

		gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
		na = (struct nlattr *)((char *)gh + GENL_HDRLEN);
		rem = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);

		for (; NLA_OK(na, rem); na = NLA_NEXT(na, rem)) {
			if (na->nla_type == CTRL_ATTR_FAMILY_ID) {
				id = *(uint16_t *)NLA_DATA(na);
				got_id = 1;
			} else if (na->nla_type == CTRL_ATTR_MCAST_GROUPS) {
				struct nlattr *grp_na = NLA_DATA(na);
				int grp_rem = NLA_PAYLOAD(na);

				for (; NLA_OK(grp_na, grp_rem);
				     grp_na = NLA_NEXT(grp_na, grp_rem)) {
					struct nlattr *a = NLA_DATA(grp_na);
					int a_rem = NLA_PAYLOAD(grp_na);
					const char *name = NULL;
					uint32_t gid = 0;

					for (; NLA_OK(a, a_rem);
					     a = NLA_NEXT(a, a_rem)) {
						if (a->nla_type == CTRL_ATTR_MCAST_GRP_NAME)
							name = NLA_DATA(a);
						else if (a->nla_type == CTRL_ATTR_MCAST_GRP_ID)
							gid = *(uint32_t *)NLA_DATA(a);
					}
					if (name &&
					    strcmp(name, LLM_CTRL_MCGRP_EVENTS) == 0) {
						grp = gid;
						got_grp = 1;
					}
				}
			}
		}
	}

	if (!got_id || !got_grp)
		return -1;
	*family_id = id;
	*mcast_id = (int)grp;
	return 0;
}

int main(void)
{
	int fd;
	struct sockaddr_nl local = { .nl_family = AF_NETLINK };
	int family_id = 0, mcast_id = 0;
	char buf[512];
	int len;

	fd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
	if (fd < 0) {
		perror("socket");
		return 1;
	}

	local.nl_pid = getpid();
	if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0) {
		perror("bind");
		return 1;
	}

	if (resolve_family(fd, &family_id, &mcast_id) < 0) {
		fprintf(stderr,
			"resolve llm_ctrl family failed (is module loaded?)\n");
		return 1;
	}

	if (setsockopt(fd, SOL_NETLINK, NETLINK_ADD_MEMBERSHIP,
		       &mcast_id, sizeof(mcast_id)) < 0) {
		perror("NETLINK_ADD_MEMBERSHIP");
		return 1;
	}

	printf("listening llm_ctrl overtemp (family=%d mcast=%d)\n",
	       family_id, mcast_id);
	fflush(stdout);

	for (;;) {
		len = recv(fd, buf, sizeof(buf), 0);
		if (len < 0) {
			if (errno == EINTR)
				continue;
			perror("recv");
			return 1;
		}

		{
			struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
			int left = len;

			for (; NLMSG_OK(nlh, left); nlh = NLMSG_NEXT(nlh, left)) {
				struct genlmsghdr *gh;
				struct nlattr *na;
				int rem;
				int temp = -1, thr = -1;

				if (nlh->nlmsg_type != family_id)
					continue;
				gh = (struct genlmsghdr *)NLMSG_DATA(nlh);
				if (gh->cmd != LLM_CTRL_CMD_OVERTEMP)
					continue;

				na = (struct nlattr *)((char *)gh + GENL_HDRLEN);
				rem = nlh->nlmsg_len - NLMSG_LENGTH(GENL_HDRLEN);
				for (; NLA_OK(na, rem); na = NLA_NEXT(na, rem)) {
					if (na->nla_type == LLM_CTRL_A_TEMP_C)
						temp = *(int *)NLA_DATA(na);
					else if (na->nla_type == LLM_CTRL_A_THRESHOLD_C)
						thr = *(int *)NLA_DATA(na);
				}
				printf("OVERTEMP temp_c=%d threshold_c=%d\n",
				       temp, thr);
				fflush(stdout);
			}
		}
	}
}
