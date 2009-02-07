/* ==========================================================================
 * dns.c - Restartable DNS Resolver.
 * --------------------------------------------------------------------------
 * Copyright (c) 2009  William Ahern
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to permit
 * persons to whom the Software is furnished to do so, subject to the
 * following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN
 * NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
 * USE OR OTHER DEALINGS IN THE SOFTWARE.
 * ==========================================================================
 */
#include <stddef.h>	/* offsetof() */
#include <stdlib.h>	/* malloc(3) free(3) rand(3) random(3) arc4random(3) */
#include <stdio.h>	/* FILE fopen(3) fclose(3) getc(3) rewind(3) */

#include <string.h>	/* memcpy(3) strlen(3) memmove(3) memchr(3) memcmp(3) strchr(3) */
#include <strings.h>	/* strcasecmp(3) strncasecmp(3) */

#include <ctype.h>	/* isspace(3) isdigit(3) */

#include <time.h>	/* time_t time(2) */

#include <signal.h>	/* sig_atomic_t */

#include <errno.h>	/* errno */

#include <assert.h>	/* assert(3) */

#include <sys/types.h>	/* socklen_t htons(3) ntohs(3) */
#include <sys/socket.h>	/* AF_INET AF_INET6 AF_UNIX struct sockaddr struct sockaddr_in struct sockaddr_in6 socket(2) */

#if defined(AF_UNIX)
#include <sys/un.h>	/* struct sockaddr_un */
#endif

#include <fcntl.h>	/* F_SETFD F_GETFL F_SETFL O_NONBLOCK fcntl(2) */

#include <unistd.h>	/* gethostname(3) close(2) */

#include <netinet/in.h>	/* struct sockaddr_in struct sockaddr_in6 */

#include <arpa/inet.h>	/* inet_pton(3) inet_ntop(3) */


#include "dns.h"


/*
 * S T A N D A R D  M A C R O S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef MIN
#define MIN(a, b)	(((a) < (b))? (a) : (b))
#endif


#ifndef MAX
#define MAX(a, b)	(((a) > (b))? (a) : (b))
#endif


#ifndef lengthof
#define lengthof(a)	(sizeof (a) / sizeof (a)[0])
#endif


#define MARK	fprintf(stderr, "@@ %s:%d\n", __FILE__, __LINE__);


/*
 * A T O M I C  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static void dns_atomic_fence(void) {
	return;
} /* dns_atomic_fence() */


static unsigned dns_atomic_inc(dns_atomic_t *i) {
	return (*i)++;
} /* dns_atomic_inc() */


static unsigned dns_atomic_dec(dns_atomic_t *i) {
	return (*i)--;
} /* dns_atomic_dec() */


static unsigned dns_atomic_load(dns_atomic_t *i) {
	return *i;
} /* dns_atomic_load() */


static unsigned dns_atomic_store(dns_atomic_t *i, unsigned n) {
	unsigned o;

	o	= dns_atomic_load(i);
	*i	= n;
	return o;
} /* dns_atomic_store() */


/*
 * C R Y P T O  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef DNS_RANDOM
#if defined(HAVE_ARC4RANDOM)	\
 || defined(__OpenBSD__)	\
 || defined(__FreeBSD__)	\
 || defined(__NetBSD__)		\
 || defined(__APPLE__)
#define DNS_RANDOM	arc4random
#elif __linux
#define DNS_RANDOM	random
#else
#define DNS_RANDOM	rand
#endif
#endif

static unsigned dns__random(void) {
	return DNS_RANDOM();
} /* dns__random() */

unsigned (*dns_random)(void)	= &dns__random;


/*
 * U T I L I T Y  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

/*
 * Monotonic Clock
 *
 * WARNING: Strictly speaking, it's not guaranteed to be monotonic. But
 * outside of wierd situations where one thread freezes (say, because a
 * laptop is put to sleep) between setting tmp_tick and assigning tmp_tick
 * to tick, it should never jump back more than a few seconds--basically,
 * the maximum time a process can be involuntarily put to sleep.
 * 
 * The goal is to prevent a system clock reset from putting timer
 * expirations unrevocably/unrecoverably far into the future (like minutes
 * or months).
 */
static time_t dns_now(void) {
	/* XXX: Assumes sizeof (time_t) <= sizeof (sig_atomic_t) */
	static volatile sig_atomic_t last, tick;
	volatile sig_atomic_t tmp_last, tmp_tick;
	time_t now;

	time(&now);

	tmp_last	= last;

	if (now > tmp_last) {
		tmp_tick	= tick;
		tmp_tick	+= now - tmp_last;
		tick		= tmp_tick;
	}

	last	= now;

	return tick;
} /* dns_now() */


static size_t dns_af_len(int af) {
	static const size_t table[32]	= {
		[AF_INET6]	= sizeof (struct sockaddr_in6),
		[AF_INET]	= sizeof (struct sockaddr_in),
#if defined(AF_UNIX)
		[AF_UNIX]	= sizeof (struct sockaddr_un),
#endif
	};

	return table[MIN(af, lengthof(table) - 1)];
} /* dns_af_len() */

#define dns_sa_len(sa)	dns_af_len(((struct sockaddr *)(sa))->sa_family)


#define DNS_SA_NOPORT	&dns_sa_noport;
static unsigned short dns_sa_noport;

static unsigned short *dns_sa_port(int af, void *sa) {

	switch (af) {
	case AF_INET6:
		return &((struct sockaddr_in6 *)sa)->sin6_port;
	case AF_INET:
		return &((struct sockaddr_in *)sa)->sin_port;
	default:
		return DNS_SA_NOPORT;
	}
} /* dns_sa_port() */


static void *dns_sa_addr(int af, void *sa) {
	switch (af) {
	case AF_INET6:
		return &((struct sockaddr_in6 *)sa)->sin6_addr;
	case AF_INET:
		return &((struct sockaddr_in *)sa)->sin_addr;
	default:
		return 0;
	}
} /* dns_sa_addr() */


#if _WIN32
static int dns_inet_pton(int af, const void *src, void *dst) {
	union { struct sockaddr_in sin; struct sockaddr_in6 sin6 } u;

	u.sin.sin_family	= af;

	if (0 != WSAStringToAddressA(src, af, (void *)0, (struct sockaddr *)&u, &(int){ sizeof u; }))
		return -1;

	switch (af) {
	case AF_INET6:
		*(struct in6_addr *)dst	= u.sin6->sin6_addr;

		return 1;
	case AF_INET:
		*(struct in_addr *)dst	= u.sin->sin_addr;

		return 1;
	default:
		return 0;
	}
} /* dns_inet_pton() */

static const char *dns_inet_ntop(int af, const void *src, void *dst, int lim) {
	union { struct sockaddr_in sin; struct sockaddr_in6 sin6 } u;

	u.sin.sin_family	= af;

	switch (af) {
	case AF_INET6:
		u.sin6->sin6_addr	= *(struct in6_addr *)src;
		break;
	case AF_INET:
		u.sin->sin_addr		= *(struct in_addr *)src;

		break;
	default:
		return 0;
	}

	if (0 != WSAAddressToStringA((struct sockaddr *)&u, dns_sa_len(&u), (void *)0, dst, &lim))
		return 0;

	return dst;
} /* dns_inet_ntop() */
#else
#define dns_inet_pton(...)	inet_pton(__VA_ARGS__)
#define dns_inet_ntop(...)	inet_ntop(__VA_ARGS__)
#endif


static void dns_soclose(int fd) {
	if (fd != -1) {
#if _WIN32
		closesocket(fd);
#else
		close(fd);
#endif
	}
} /* dns_soclose() */


#define DNS_SO_MAXTRY	7

static int dns_socket(struct sockaddr *origin, int type, int *error) {
	int flags, fd	= -1;

	if (-1 == (fd = socket(origin->sa_family, type, 0)))
		goto syerr;

	if (-1 == fcntl(fd, F_SETFD, 1))
		goto syerr;

	if (-1 == (flags = fcntl(fd, F_GETFL)))
		goto syerr;

	if (-1 == fcntl(fd, F_SETFL, flags | O_NONBLOCK))
		goto syerr;

	if (origin->sa_family != AF_INET && origin->sa_family != AF_INET6)
		return fd;

	if (*dns_sa_port(origin->sa_family, origin) == 0) {
		struct sockaddr_storage tmp;
		unsigned i, port;

		memcpy(&tmp, origin, dns_sa_len(origin));

		for (i = 0; i < DNS_SO_MAXTRY; i++) {
			port	= 1025 + (dns_random() % 64510);

			*dns_sa_port(tmp.ss_family, &tmp)	= htons(port);

			if (0 == bind(fd, (struct sockaddr *)&tmp, dns_sa_len(&tmp)))
				return fd;
		}
	}
	
	if (0 == bind(fd, origin, dns_sa_len(origin)))
		return fd;

	/* FALL THROUGH */
syerr:
	*error	= errno;

	dns_soclose(fd);

	return -1;
} /* dns_socket() */


/*
 * P A C K E T  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

unsigned dns_p_count(struct dns_packet *P, enum dns_section section) {
	switch (section) {
	case DNS_S_QD:
		return ntohs(dns_header(P)->qdcount);
	case DNS_S_AN:
		return ntohs(dns_header(P)->ancount);
	case DNS_S_NS:
		return ntohs(dns_header(P)->nscount);
	case DNS_S_AR:
		return ntohs(dns_header(P)->arcount);
	case DNS_S_ALL:
		return ntohs(dns_header(P)->qdcount)
		     + ntohs(dns_header(P)->ancount)
		     + ntohs(dns_header(P)->nscount)
		     + ntohs(dns_header(P)->arcount);
	default:
		return 0;
	}
} /* dns_p_count() */


struct dns_packet *dns_p_init(struct dns_packet *P, size_t size) {
	static const struct dns_packet P_initializer;

	assert(size >= 12);	/* Header size */

	*P	= P_initializer;
	P->size	= size - offsetof(struct dns_packet, data);
	P->end	= 12;

	return P;
} /* dns_p_init() */


void dns_p_dictadd(struct dns_packet *P, unsigned short dn) {
	unsigned i;

	for (i = 0; i < lengthof(P->dict); i++) {
		if (!P->dict[i]) {
			P->dict[i]	= dn;

			break;
		}
	}
} /* dns_p_dictadd() */


int dns_p_push(struct dns_packet *P, enum dns_section section, const void *dn, size_t dnlen, enum dns_type type, enum dns_class class, unsigned ttl, const void *any) {
	size_t end	= P->end;
	int error;

	if ((error = dns_d_push(P, dn, dnlen)))
		goto error;

	if (P->size - P->end < 4)
		goto toolong;

	P->data[P->end++]	= 0xff & (type >> 8);
	P->data[P->end++]	= 0xff & (type >> 0);

	P->data[P->end++]	= 0xff & (class >> 8);
	P->data[P->end++]	= 0xff & (class >> 0);

	if (section == DNS_S_QD) {
		dns_header(P)->qdcount	= htons(ntohs(dns_header(P)->qdcount) + 1);

		return 0;
	}

	if (P->size - P->end < 6)
		goto toolong;

	P->data[P->end++]	= 0x7f & (ttl >> 24);
	P->data[P->end++]	= 0xff & (ttl >> 16);
	P->data[P->end++]	= 0xff & (ttl >> 8);
	P->data[P->end++]	= 0xff & (ttl >> 0);

	if ((error = dns_any_push(P, (union dns_any *)any, type)))
		goto error;

	switch (section) {
	case DNS_S_AN:
		dns_header(P)->ancount	= htons(ntohs(dns_header(P)->ancount) + 1);

		break;
	case DNS_S_NS:
		dns_header(P)->nscount	= htons(ntohs(dns_header(P)->nscount) + 1);

		break;
	case DNS_S_AR:
		dns_header(P)->arcount	= htons(ntohs(dns_header(P)->arcount) + 1);

		break;
	default:
		break;
	} /* switch() */

	return 0;
toolong:
	error	= -1;
error:
	P->end	= end;

	return error;
} /* dns_p_push() */


/*
 * D O M A I N  N A M E  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#ifndef DNS_D_MAXPTRS
#define DNS_D_MAXPTRS	127	/* Arbitrary; possible, valid depth is something like packet size / 2 + fudge. */
#endif

static size_t dns_l_expand(unsigned char *dst, size_t lim, unsigned short src, unsigned short *nxt, const unsigned char *data, size_t end) {
	unsigned short len;
	unsigned nptrs	= 0;

retry:
	if (src >= end)
		goto invalid;

	switch (0x03 & (data[src] >> 6)) {
	case 0x00:
		len	= (0x3f & (data[src++]));

		if (end - src < len)
			goto invalid;

		if (lim > 0) {
			memcpy(dst, &data[src], MIN(lim, len));

			dst[MIN(lim - 1, len)]	= '\0';
		}

		*nxt	= src + len;

		return len;
	case 0x01:
		goto invalid;
	case 0x02:
		goto invalid;
	case 0x03:
		if (++nptrs > DNS_D_MAXPTRS)
			goto invalid;

		if (end - src < 2)
			goto invalid;

		src	= ((0x3f & data[src + 0]) << 8)
			| ((0xff & data[src + 1]) << 0);

		goto retry;
	} /* switch() */

	/* NOT REACHED */
invalid:
	*nxt	= end;

	return 0;
} /* dns_l_expand() */


char *dns_d_init(void *dst, size_t lim, const void *src, size_t len, int flags) {
	if (flags & DNS_D_ANCHOR) {
		dns_d_anchor(dst, lim, src, len);
	} else {
		memmove(dst, src, MIN(lim, len));

		if (lim > 0)
			((char *)dst)[MIN(len, lim - 1)]	= '\0';
	}

	return dst;
} /* dns_d_init() */


size_t dns_d_anchor(void *dst, size_t lim, const void *src, size_t len) {
	if (len == 0)
		return 0;

	memmove(dst, src, MIN(lim, len));

	if (((const char *)src)[len - 1] != '.') {
		if (len < lim)
			((char *)dst)[len]	= '.';
		len++;
	}

	if (lim > 0)
		((char *)dst)[MIN(lim - 1, len)]	= '\0';

	return len;
} /* dns_d_anchor() */


size_t dns_d_cleave(void *dst, size_t lim, const void *src, size_t len) {
	const char *dot;

	/* XXX: Skip any leading dot. Handles cleaving root ".". */
	if (len == 0 || !(dot = memchr((const char *)src + 1, '.', len - 1)))
		return 0;

	len	-= dot - (const char *)src;

	/* XXX: Unless root, skip the label's trailing dot. */
	if (len > 1) {
		src	= ++dot;
		len--;
	} else
		src	= dot;

	memmove(dst, src, MIN(lim, len));

	if (lim > 0)
		((char *)dst)[MIN(lim - 1, len)]	= '\0';

	return len;
} /* dns_d_cleave() */


size_t dns_d_comp(void *dst_, size_t lim, const void *src_, size_t len, struct dns_packet *P, int *error) {
	struct { unsigned char *b; size_t p, x; } dst, src;
	unsigned char ch;

	dst.b	= dst_;
	dst.p	= 0;
	dst.x	= 1;

	src.b	= (unsigned char *)src_;
	src.p	= 0;
	src.x	= 0;

	while (src.x < len) {
		ch	= src.b[src.x];

		if (ch == '.') {
			if (dst.p < lim)
				dst.b[dst.p]	= (0x3f & (src.x - src.p));

			dst.p	= dst.x++;
			src.p	= ++src.x;
		} else {
			if (dst.x < lim)
				dst.b[dst.x]	= ch;

			dst.x++;
			src.x++;
		}
	} /* while() */

	if (src.x > src.p) {
		if (dst.p < lim)
			dst.b[dst.p]	= (0x3f & (src.x - src.p));

		dst.p	= dst.x;
	}

	if (dst.p < lim)
		dst.b[dst.p]	= 0x00;

	dst.p++;

#if 1
	if (dst.p < lim) {
		struct { unsigned char label[DNS_D_MAXLABEL + 1]; size_t len; unsigned short p, x, y; } a, b;
		unsigned i;

		a.p	= 0;

		while ((a.len = dns_l_expand(a.label, sizeof a.label, a.p, &a.x, dst.b, lim))) {
			for (i = 0; i < lengthof(P->dict) && P->dict[i]; i++) {
				b.p	= P->dict[i];

				while ((b.len = dns_l_expand(b.label, sizeof b.label, b.p, &b.x, P->data, P->end))) {
					a.y	= a.x;
					b.y	= b.x;

					while (a.len && b.len && 0 == strcasecmp((char *)a.label, (char *)b.label)) {
						a.len = dns_l_expand(a.label, sizeof a.label, a.y, &a.y, dst.b, lim);
						b.len = dns_l_expand(b.label, sizeof b.label, b.y, &b.y, P->data, P->end);
					}

					if (a.len == 0 && b.len == 0 && b.p <= 0x3fff) {
						dst.b[a.p++]	= 0xc0
								| (0x3f & (b.p >> 8));
						dst.b[a.p++]	= (0xff & (b.p >> 0));

						return a.p;
					}

					b.p	= b.x;
				} /* while() */
			} /* for() */

			a.p	= a.x;
		} /* while() */
	} /* if () */
#endif

	return dst.p;
} /* dns_d_comp() */


unsigned short dns_d_skip(unsigned short src, struct dns_packet *P) {
	unsigned short len;

	while (src < P->end) {
		switch (0x03 & (P->data[src] >> 6)) {
		case 0x00:	/* FOLLOWS */
			len	= (0x3f & P->data[src++]);

			if (0 == len) {
/* success ==> */		return src;
			} else if (P->end - src > len) {
				src	+= len;

				break;
			} else
				goto invalid;

			/* NOT REACHED */
		case 0x01:	/* RESERVED */
			goto invalid;
		case 0x02:	/* RESERVED */
			goto invalid;
		case 0x03:	/* POINTER */
			if (P->end - src < 2)
				goto invalid;

			src	+= 2;

/* success ==> */	return src;
		} /* switch() */
	} /* while() */

invalid:
	return P->end;
} /* dns_d_skip() */


#include <stdio.h>

size_t dns_d_expand(void *dst, size_t lim, unsigned short src, struct dns_packet *P, int *error) {
	size_t dstp	= 0;
	unsigned nptrs	= 0;
	unsigned char len;

	while (src < P->end) {
		switch ((0x03 & (P->data[src] >> 6))) {
		case 0x00:	/* FOLLOWS */
			len	= (0x3f & P->data[src]);

			if (0 == len) {
				if (dstp == 0) {
					if (dstp < lim)
						((unsigned char *)dst)[dstp]	= '.';

					dstp++;
				}

				/* NUL terminate */
				if (lim > 0)
					((unsigned char *)dst)[MIN(dstp, lim - 1)]	= '\0';

/* success ==> */		return dstp;
			}

			src++;

			if (P->end - src < len)
				goto toolong;

			if (dstp < lim)
				memcpy(&((unsigned char *)dst)[dstp], &P->data[src], MIN(len, lim - dstp));

			src	+= len;
			dstp	+= len;

			if (dstp < lim)
				((unsigned char *)dst)[dstp]	= '.';

			dstp++;

			nptrs	= 0;

			continue;
		case 0x01:	/* RESERVED */
			goto reserved;
		case 0x02:	/* RESERVED */
			goto reserved;
		case 0x03:	/* POINTER */
			if (++nptrs > DNS_D_MAXPTRS)
				goto toolong;

			if (P->end - src < 2)
				goto toolong;

			src	= ((0x3f & P->data[src + 0]) << 8)
				| ((0xff & P->data[src + 1]) << 0);

			continue;
		} /* switch() */
	} /* while() */

toolong:
	*error	= -1;

	if (lim > 0)
		((unsigned char *)dst)[MIN(dstp, lim - 1)]	= '\0';

	return 0;
reserved:
	*error	= -1;

	if (lim > 0)
		((unsigned char *)dst)[MIN(dstp, lim - 1)]	= '\0';

	return 0;
} /* dns_d_expand() */


int dns_d_push(struct dns_packet *P, const void *dn, size_t len) {
	size_t lim	= P->size - P->end;
	int error;

	len	= dns_d_comp(&P->data[P->end], lim, dn, len, P, &error);

	if (len == 0)
		return error;
	if (len > lim)
		return -1;

	dns_p_dictadd(P, P->end);

	P->end	+= len;

	return 0;
} /* dns_d_push() */


/*
 * R E S O U R C E  R E C O R D  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

int dns_rr_copy(struct dns_packet *P, struct dns_rr *rr, struct dns_packet *Q) {
	unsigned char dn[DNS_D_MAXNAME + 1];
	union dns_any any;
	size_t len;
	int error;

	if (0 == (len = dns_d_expand(dn, sizeof dn, rr->dn.p, Q, &error)))
		return error;
	else if (len >= sizeof dn)
		return -1;

	if (rr->section != DNS_S_QD && (error = dns_any_parse(dns_any_init(&any, sizeof any), rr, Q)))
		return error;

	return dns_p_push(P, rr->section, dn, len, rr->type, rr->class, rr->ttl, &any);
} /* dns_rr_copy() */


int dns_rr_parse(struct dns_rr *rr, unsigned short src, struct dns_packet *P) {
	unsigned short p	= src;

	if (src >= P->end)
		return -1;

	rr->dn.p	= p;
	rr->dn.len	= (p = dns_d_skip(p, P)) - rr->dn.p;

	if (P->end - p < 4)
		return -1;

	rr->type	= ((0xff & P->data[p + 0]) << 8)
			| ((0xff & P->data[p + 1]) << 0);

	rr->class	= ((0xff & P->data[p + 2]) << 8)
			| ((0xff & P->data[p + 3]) << 0);

	p	+= 4;

	if (src == 12) {
		rr->ttl		= 0;
		rr->rd.p	= 0;
		rr->rd.len	= 0;

		return 0;
	}

	if (P->end - p < 4)
		return -1;

	rr->ttl		= ((0x7f & P->data[p + 0]) << 24)
			| ((0xff & P->data[p + 1]) << 16)
			| ((0xff & P->data[p + 2]) << 8)
			| ((0xff & P->data[p + 3]) << 0);

	p	+= 4;

	if (P->end - p < 2)
		return -1;

	rr->rd.len	= ((0xff & P->data[p + 0]) << 8)
			| ((0xff & P->data[p + 1]) << 0);
	rr->rd.p	= p + 2;

	p	+= 2;

	if (P->end - p < rr->rd.len)
		return -1;

	return 0;
} /* dns_rr_parse() */


static unsigned short dns_rr_len(const struct dns_rr *rr, struct dns_packet *P) {
	size_t len	= 0;

	len	+= rr->dn.len;
	len	+= 4;

	if (rr->dn.p == 12)
		return len;

	len	+= 4;
	len	+= 2;
	len	+= rr->rd.len;

	return len;
} /* dns_rr_len() */


unsigned short dns_rr_skip(unsigned short src, struct dns_packet *P) {
	struct dns_rr rr;

	if (0 != dns_rr_parse(&rr, src, P))
		return P->end;

	return src + dns_rr_len(&rr, P);
} /* dns_rr_skip() */


#include <stdio.h>

struct dns_rr_i *dns_rr_i_init(struct dns_rr_i *i) {
	static const struct dns_rr_i i_initializer = {
		.state	= DNS_RR_I_STATE_INITIALIZER
	};

	i->state	= i_initializer.state;

	return i;
} /* dns_rr_i_init() */


unsigned dns_rr_grep(struct dns_rr *rr, unsigned lim, struct dns_rr_i *i, struct dns_packet *P, int *error) {
	char dn[DNS_D_MAXNAME + 1];
	unsigned count	= 0;

	while (i->state.next < P->end) {
		if (i->state.index >= dns_p_count(P, i->state.section)) {
			if (DNS_S_AR < (i->state.section <<= 1))
				break;

			i->state.index		= 0;

			continue;
		}

		if ((*error = dns_rr_parse(rr, i->state.next, P)))
			goto error;

		rr->section	= i->state.section;
		i->state.next	+= dns_rr_len(rr, P);
		i->state.index++;

		if (i->section && !(rr->section & i->section))
			continue;

		if (i->type && rr->type != i->type && i->type != DNS_T_ALL)
			continue;

		if (i->class && rr->class != i->class && i->class != DNS_C_ANY)
			continue;

		if (i->name) {
			if (sizeof dn <= dns_d_expand(dn, sizeof dn, rr->dn.p, P, error))
				goto error;

			if (0 != strcasecmp(dn, i->name))
				continue;
		}

		rr++;

		if (++count < lim)
			continue;

		return count;
	} /* while() */

	return count;
error:
	return count;
} /* dns_rr_grep() */


#if 0
int dns_rr_next(struct dns_rr *rr, enum dns_section sections, struct dns_packet *P, unsigned long *state) {
	unsigned short src	= 0xffff & (*state >> 0);
	unsigned short i	= 0x0fff & (*state >> 16);
	unsigned short mask	= 0x000f & (*state >> 28);
	unsigned short count	= 0;
	int section, error;

	if (src >= P->end)
		return -1;

nextrr:
	if (0 == (sections &= ~mask))
		return -1;

	if (sections & DNS_S_QD) {
		count	= 0x0fff & ntohs(dns_header(P)->qdcount);
		section	= DNS_S_QD;
	} else if (sections & DNS_S_AN) {
		count	= 0x0fff & ntohs(dns_header(P)->ancount);
		section	= DNS_S_AN;
	} else if (sections & DNS_S_NS) {
		count	= 0x0fff & ntohs(dns_header(P)->nscount);
		section	= DNS_S_NS;
	} else if (sections & DNS_S_AR) {
		count	= 0x0fff & ntohs(dns_header(P)->arcount);
		section	= DNS_S_AR;
	} else
		return -1;

	if (i >= count) {
		mask	|= section;
		*state	|= ((0x000f & mask) << 28);
		i	= 0;

		goto nextrr;
	}

	if (i == 0) {
		unsigned short skip	= 0;

		switch (section) {
		case DNS_S_AR:
			skip	+= ntohs(dns_header(P)->nscount);
		case DNS_S_NS:
			skip	+= ntohs(dns_header(P)->ancount);
		case DNS_S_AN:
			skip	+= ntohs(dns_header(P)->qdcount);
		case DNS_S_QD:
			break;
		}

		src	= 12;

		while (skip--)
			src	= dns_rr_skip(src, P);
	}

	if ((error = dns_rr_parse(rr, src, P)))
		return error;

	rr->section	= section;

	src	+= dns_rr_len(rr, P);
	i++;

	*state	= ((0xffff & src)  << 0)
		| ((0x0fff & i)    << 16)
		| ((0x000f & mask) << 28);

	return 0;
} /* dns_rr_next() */
#endif


static size_t dns__print10(void *dst, size_t lim, size_t off, unsigned n) {
	unsigned cp	= off;
	unsigned d	= 1000000;
	unsigned ch;

	while (d) {
		if ((ch = n / d) || cp > off) {
			n	-= ch * d;

			if (cp < lim)
				((unsigned char *)dst)[cp]	= '0' + ch;

			cp++;
		}

		d	/= 10;
	}

	return cp - off;
} /* dns__print10() */


static size_t dns__printchar(void *dst, size_t lim, size_t cp, unsigned char ch) {
	if (cp < lim)
		((unsigned char *)dst)[cp]	= ch;

	return 1;
} /* dns__printchar() */


static size_t dns__printstring(void *dst, size_t lim, size_t cp, const void *src, size_t len) {
	if (cp < lim)
		memcpy(&((unsigned char *)dst)[cp], src, MIN(len, lim - cp));

	return len;
} /* dns__printstring() */

#define dns__printstring5(a, b, c, d, e)	dns__printstring((a), (b), (c), (d), (e))
#define dns__printstring4(a, b, c, d)		dns__printstring((a), (b), (c), (d), strlen((d)))
#define dns__printstring(...)			DNS_PP_CALL(DNS_PP_XPASTE(dns__printstring, DNS_PP_NARG(__VA_ARGS__)), __VA_ARGS__)


static void dns__printnul(void *dst, size_t lim, size_t off) {
	if (lim > 0)
		((unsigned char *)dst)[MIN(off, lim - 1)]	= '\0';
} /* dns__printnul() */


size_t dns_rr_print(void *dst, size_t lim, struct dns_rr *rr, struct dns_packet *P, int *error) {
	union dns_any any;
	size_t cp, n, rdlen;
	void *rd;

	cp	= 0;

	if (rr->section == DNS_S_QD)
		cp	+= dns__printchar(dst, lim, cp, ';');

	if (0 == (n = dns_d_expand(&((unsigned char *)dst)[cp], (cp < lim)? lim - cp : 0, rr->dn.p, P, error)))
		return 0;

	cp	+= n;

	if (rr->section != DNS_S_QD) {
		cp	+= dns__printchar(dst, lim, cp, ' ');
		cp	+= dns__print10(dst, lim, cp, rr->ttl);
	}

	cp	+= dns__printchar(dst, lim, cp, ' ');
	cp	+= dns__printstring(dst, lim, cp, dns_strclass(rr->class), strlen(dns_strclass(rr->class)));
	cp	+= dns__printchar(dst, lim, cp, ' ');
	cp	+= dns__printstring(dst, lim, cp, dns_strtype(rr->type), strlen(dns_strtype(rr->type)));

	if (rr->section == DNS_S_QD)
		goto epilog;

	cp	+= dns__printchar(dst, lim, cp, ' ');

	if ((*error = dns_any_parse(dns_any_init(&any, sizeof any), rr, P)))
		return 0;

	if (cp < lim) {
		rd	= &((unsigned char *)dst)[cp];
		rdlen	= lim - cp;
	} else {
		rd	= 0;
		rdlen	= 0;
	}

	cp	+= dns_any_print(rd, rdlen, &any, rr->type);

epilog:
	dns__printnul(dst, lim, cp);

	return cp;
} /* dns_rr_print() */


int dns_a_parse(struct dns_a *a, struct dns_rr *rr, struct dns_packet *P) {
	unsigned long addr;

	if (rr->rd.len != 4)
		return -1;

	addr	= ((0xff & P->data[rr->rd.p + 0]) << 24)
		| ((0xff & P->data[rr->rd.p + 1]) << 16)
		| ((0xff & P->data[rr->rd.p + 2]) << 8)
		| ((0xff & P->data[rr->rd.p + 3]) << 0);

	a->addr.s_addr	= htonl(addr);

	return 0;
} /* dns_a_parse() */


int dns_a_push(struct dns_packet *P, struct dns_a *a) {
	unsigned long addr;

	if (P->size - P->end < 6)
		return -1;

	P->data[P->end++]	= 0x00;
	P->data[P->end++]	= 0x04;

	addr	= ntohl(a->addr.s_addr);

	P->data[P->end++]	= 0xff & (addr >> 24);
	P->data[P->end++]	= 0xff & (addr >> 16);
	P->data[P->end++]	= 0xff & (addr >> 8);
	P->data[P->end++]	= 0xff & (addr >> 0);

	return 0;
} /* dns_a_push() */


size_t dns_a_print(void *dst, size_t lim, struct dns_a *a) {
	char addr[INET_ADDRSTRLEN + 1]	= "0.0.0.0";
	size_t len;

	dns_inet_ntop(AF_INET, &a->addr, addr, sizeof addr);

	dns__printnul(dst, lim, (len = dns__printstring(dst, lim, 0, addr)));

	return len;
} /* dns_a_print() */


int dns_aaaa_parse(struct dns_aaaa *aaaa, struct dns_rr *rr, struct dns_packet *P) {
	if (rr->rd.len != sizeof aaaa->addr.s6_addr)
		return -1;

	memcpy(aaaa->addr.s6_addr, &P->data[rr->rd.p], sizeof aaaa->addr.s6_addr);

	return 0;
} /* dns_aaaa_parse() */


int dns_aaaa_push(struct dns_packet *P, struct dns_aaaa *aaaa) {
	if (P->size - P->end < 2 + sizeof aaaa->addr.s6_addr)
		return -1;

	P->data[P->end++]	= 0x00;
	P->data[P->end++]	= 0x10;

	memcpy(&P->data[P->end], aaaa->addr.s6_addr, sizeof aaaa->addr.s6_addr);

	P->end	+= sizeof aaaa->addr.s6_addr;

	return 0;
} /* dns_aaaa_push() */


size_t dns_aaaa_print(void *dst, size_t lim, struct dns_aaaa *aaaa) {
	char addr[INET6_ADDRSTRLEN + 1]	= "::";
	size_t len;

	dns_inet_ntop(AF_INET6, &aaaa->addr, addr, sizeof addr);

	dns__printnul(dst, lim, (len = dns__printstring(dst, lim, 0, addr)));

	return len;
} /* dns_aaaa_print() */


int dns_mx_parse(struct dns_mx *mx, struct dns_rr *rr, struct dns_packet *P) {
	size_t len;
	int error;

	if (rr->rd.len < 3)
		return -1;

	mx->preference	= (0xff00 & (P->data[rr->rd.p + 0] << 8))
			| (0x00ff & (P->data[rr->rd.p + 1] << 0));

	len	= dns_d_expand(mx->host, sizeof mx->host, rr->rd.p + 2, P, &error);

	if (len == 0)
		return error;
	if (len >= sizeof mx->host)
		return -1;

	return 0;
} /* dns_mx_parse() */


int dns_mx_push(struct dns_packet *P, struct dns_mx *mx) {
	size_t end, len;
	int error;

	if (P->size - P->end < 5)
		return -1;

	end	= P->end;
	P->end	+= 2;

	P->data[P->end++]	= 0xff & (mx->preference >> 8);
	P->data[P->end++]	= 0xff & (mx->preference >> 0);

	if ((error = dns_d_push(P, mx->host, strlen(mx->host))))
		goto error;

	len	= P->end - end - 2;

	P->data[end + 0]	= 0xff & (len >> 8);
	P->data[end + 1]	= 0xff & (len >> 0);

	return 0;
error:
	P->end	= end;

	return error;
} /* dns_mx_push() */


size_t dns_mx_print(void *dst, size_t lim, struct dns_mx *mx) {
	size_t cp	= 0;

	cp	+= dns__print10(dst, lim, cp, mx->preference);
	cp	+= dns__printchar(dst, lim, cp, ' ');
	cp	+= dns__printstring(dst, lim, cp, mx->host, strlen(mx->host));

	dns__printnul(dst, lim, cp);

	return cp;
} /* dns_mx_print() */


int dns_ns_parse(struct dns_ns *ns, struct dns_rr *rr, struct dns_packet *P) {
	size_t len;
	int error;

	len	= dns_d_expand(ns->host, sizeof ns->host, rr->rd.p, P, &error);

	if (len == 0)
		return error;
	if (len >= sizeof ns->host)
		return -1;

	return 0;
} /* dns_ns_parse() */


int dns_ns_push(struct dns_packet *P, struct dns_ns *ns) {
	size_t end, len;
	int error;

	if (P->size - P->end < 3)
		return -1;

	end	= P->end;
	P->end	+= 2;

	if ((error = dns_d_push(P, ns->host, strlen(ns->host))))
		goto error;

	len	= P->end - end - 2;

	P->data[end + 0]	= 0xff & (len >> 8);
	P->data[end + 1]	= 0xff & (len >> 0);

	return 0;
error:
	P->end	= end;

	return error;
} /* dns_ns_push() */


size_t dns_ns_print(void *dst, size_t lim, struct dns_ns *ns) {
	size_t cp;

	cp	= dns__printstring(dst, lim, 0, ns->host, strlen(ns->host));

	dns__printnul(dst, lim, cp);

	return cp;
} /* dns_ns_print() */


int dns_cname_parse(struct dns_cname *cname, struct dns_rr *rr, struct dns_packet *P) {
	return dns_ns_parse((struct dns_ns *)cname, rr, P);
} /* dns_cname_parse() */


int dns_cname_push(struct dns_packet *P, struct dns_cname *cname) {
	return dns_ns_push(P, (struct dns_ns *)cname);
} /* dns_cname_push() */


size_t dns_cname_print(void *dst, size_t lim, struct dns_cname *cname) {
	return dns_ns_print(dst, lim, (struct dns_ns *)cname);
} /* dns_cname_print() */


struct dns_txt *dns_txt_init(struct dns_txt *txt, size_t size) {
	assert(size > offsetof(struct dns_txt, data));

	txt->size	= size - offsetof(struct dns_txt, data);
	txt->len	= 0;

	return txt;
} /* dns_txt_init() */


int dns_txt_parse(struct dns_txt *txt, struct dns_rr *rr, struct dns_packet *P) {
	struct { unsigned char *b; size_t p, end; } dst, src;
	unsigned n;

	dst.b	= txt->data;
	dst.p	= 0;
	dst.end	= txt->size;

	src.b	= P->data;
	src.p	= rr->rd.p;
	src.end	= src.p + rr->rd.len;

	while (src.p < src.end) {
		n	= 0xff & P->data[src.p++];

		if (src.end - src.p < n || dst.end - dst.p < n)
			return -1;

		memcpy(&dst.b[dst.p], &src.b[src.p], n);

		dst.p	+= n;
		src.p	+= n;
	}

	txt->len	= dst.p;

	return 0;
} /* dns_txt_parse() */


int dns_txt_push(struct dns_packet *P, struct dns_txt *txt) {
	struct { unsigned char *b; size_t p, end; } dst, src;
	unsigned n;

	dst.b	= P->data;
	dst.p	= P->end;
	dst.end	= P->size;

	src.b	= txt->data;
	src.p	= 0;
	src.end	= txt->len;

	if (dst.end - dst.p < 2)
		return -1;

	n	= txt->len + 1 + (txt->len / 256);

	dst.b[dst.p++]	= 0xff & (n >> 8);
	dst.b[dst.p++]	= 0xff & (n >> 0);

	while (src.p < src.end) {
		n	= 0xff & (src.end - src.p);

		if (dst.p >= dst.end)
			return -1;

		dst.b[dst.p++]	= n;

		if (dst.end - dst.p < n)
			return -1;

		memcpy(&dst.b[dst.p], &src.b[src.p], n);

		dst.p	+= n;
		src.p	+= n;
	}

	P->end	= dst.p;

	return 0;
} /* dns_txt_push() */


size_t dns_txt_print(void *dst_, size_t lim, struct dns_txt *txt) {
	struct { unsigned char *b; size_t p, end; } dst, src;
	unsigned ch;

	dst.b	= dst_;
	dst.end	= lim;
	dst.p	= 0;

	src.b	= txt->data;
	src.end	= txt->len;
	src.p	= 0;

	dst.p	+= dns__printchar(dst.b, dst.end, dst.p, '"');

	while (src.p < src.end) {
		ch	= src.b[src.p];

		if (0 == (src.p++ % 256)) {
			dst.p	+= dns__printchar(dst.b, dst.end, dst.p, '"');
			dst.p	+= dns__printchar(dst.b, dst.end, dst.p, ' ');
			dst.p	+= dns__printchar(dst.b, dst.end, dst.p, '"');
		}

		if (ch < 32 || ch > 126 || ch == '"' || ch == '\\') {
			dst.p	+= dns__printchar(dst.b, dst.end, dst.p, '\\');
			dst.p	+= dns__print10(dst.b, dst.end, dst.p, ch);
		} else {
			dst.p	+= dns__printchar(dst.b, dst.end, dst.p, ch);
		}
	}

	dst.p	+= dns__printchar(dst.b, dst.end, dst.p, '"');

	dns__printnul(dst.b, dst.end, dst.p);

	return dst.p;
} /* dns_txt_print() */


static const struct {
	enum dns_type type;
	const char *name;
	int (*parse)();
	int (*push)();
	size_t (*print)();
} dns_rrtypes[]	= {
	{ DNS_T_A,     "A",     &dns_a_parse,     &dns_a_push,     &dns_a_print     },
	{ DNS_T_AAAA,  "AAAA",  &dns_aaaa_parse,  &dns_aaaa_push,  &dns_aaaa_print  },
	{ DNS_T_MX,    "MX",    &dns_mx_parse,    &dns_mx_push,    &dns_mx_print    },
	{ DNS_T_NS,    "NS",    &dns_ns_parse,    &dns_ns_push,    &dns_ns_print    },
	{ DNS_T_CNAME, "CNAME", &dns_cname_parse, &dns_cname_push, &dns_cname_print },
	{ DNS_T_TXT,   "TXT",   &dns_txt_parse,   &dns_txt_push,   &dns_txt_print   },
}; /* dns_rrtypes[] */


union dns_any *dns_any_init(union dns_any *any, size_t size) {
	return (union dns_any *)dns_txt_init(&any->rdata, size);
} /* dns_any_init() */


int dns_any_parse(union dns_any *any, struct dns_rr *rr, struct dns_packet *P) {
	unsigned i;

	for (i = 0; i < lengthof(dns_rrtypes); i++) {
		if (dns_rrtypes[i].type == rr->type)
			return dns_rrtypes[i].parse(any, rr, P);
	}

	if (rr->rd.len > any->rdata.size)
		return -1;

	memcpy(any->rdata.data, &P->data[rr->rd.p], rr->rd.len);
	any->rdata.len	= rr->rd.len;

	return 0;
} /* dns_any_parse() */


int dns_any_push(struct dns_packet *P, union dns_any *any, enum dns_type type) {
	unsigned i;

	for (i = 0; i < lengthof(dns_rrtypes); i++) {
		if (dns_rrtypes[i].type == type)
			return dns_rrtypes[i].push(P, any);
	}

	if (P->size - P->end < any->rdata.len + 2)
		return -1;

	P->data[P->end++]	= 0xff & (any->rdata.len >> 8);
	P->data[P->end++]	= 0xff & (any->rdata.len >> 0);

	memcpy(&P->data[P->end], any->rdata.data, any->rdata.len);
	P->end	+= any->rdata.len;

	return 0;
} /* dns_any_push() */


size_t dns_any_print(void *dst_, size_t lim, union dns_any *any, enum dns_type type) {
	struct { unsigned char *b; size_t p, end; } dst, src;
	unsigned i, ch;

	for (i = 0; i < lengthof(dns_rrtypes); i++) {
		if (dns_rrtypes[i].type == type)
			return dns_rrtypes[i].print(dst_, lim, any);
	}

	dst.b	= dst_;
	dst.end	= lim;
	dst.p	= 0;

	src.b	= any->rdata.data;
	src.end	= any->rdata.len;
	src.p	= 0;

	dst.p	+= dns__printchar(dst.b, dst.end, dst.p, '"');

	while (src.p < src.end) {
		ch	= src.b[src.p++];

		dst.p	+= dns__printchar(dst.b, dst.end, dst.p, '\\');
		dst.p	+= dns__print10(dst.b, dst.end, dst.p, ch);
	}

	dst.p	+= dns__printchar(dst.b, dst.end, dst.p, '"');

	dns__printnul(dst.b, dst.end, dst.p);

	return dst.p;
} /* dns_any_print() */


/*
 * R E S O L V . C O N F  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct dns_resolv_conf *dns_resconf_open(int *error) {
	static const struct dns_resolv_conf resconf_initializer
		= { .lookup = "bf", .options = { .ndots = 1, },
		    .interface = { .ss_family = AF_INET }, };
	struct dns_resolv_conf *resconf;

	if (!(resconf = malloc(sizeof *resconf)))
		goto syerr;

	*resconf	= resconf_initializer;

	if (0 != gethostname(resconf->search[0], sizeof resconf->search[0]))
		goto syerr;

	dns_d_anchor(resconf->search[0], sizeof resconf->search[0], resconf->search[0], strlen(resconf->search[0]));
	dns_d_cleave(resconf->search[0], sizeof resconf->search[0], resconf->search[0], strlen(resconf->search[0]));

	/*
	 * XXX: If gethostname() returned a string without any label
	 *      separator, then search[0][0] should be NUL.
	 */

	dns_resconf_acquire(resconf);

	return resconf;
syerr:
	*error	= errno;

	free(resconf);

	return 0;
} /* dns_resconf_open() */


void dns_resconf_close(struct dns_resolv_conf *resconf) {
	if (!resconf || 1 != dns_resconf_release(resconf))
		return /* void */;

	free(resconf);
} /* dns_resconf_close() */


unsigned dns_resconf_acquire(struct dns_resolv_conf *resconf) {
	return dns_atomic_inc(&resconf->_.refcount);
} /* dns_resconf_acquire() */


unsigned dns_resconf_release(struct dns_resolv_conf *resconf) {
	return dns_atomic_dec(&resconf->_.refcount);
} /* dns_resconf_release() */


enum dns_resconf_keyword {
	DNS_RESCONF_NAMESERVER,
	DNS_RESCONF_DOMAIN,
	DNS_RESCONF_SEARCH,
	DNS_RESCONF_LOOKUP,
	DNS_RESCONF_FILE,
	DNS_RESCONF_BIND,
	DNS_RESCONF_OPTIONS,
	DNS_RESCONF_EDNS0,
	DNS_RESCONF_NDOTS,
	DNS_RESCONF_RECURSIVE,
	DNS_RESCONF_INTERFACE,
}; /* enum dns_resconf_keyword */ 

static enum dns_resconf_keyword dns_resconf_keyword(const char *word) {
	static const char *words[]	= {
		[DNS_RESCONF_NAMESERVER]	= "nameserver",
		[DNS_RESCONF_DOMAIN]		= "domain",
		[DNS_RESCONF_SEARCH]		= "search",
		[DNS_RESCONF_LOOKUP]		= "lookup",
		[DNS_RESCONF_FILE]		= "file",
		[DNS_RESCONF_BIND]		= "bind",
		[DNS_RESCONF_OPTIONS]		= "options",
		[DNS_RESCONF_EDNS0]		= "edns0",
		[DNS_RESCONF_RECURSIVE]		= "recursive",
		[DNS_RESCONF_INTERFACE]		= "interface",
	};
	unsigned i;

	for (i = 0; i < lengthof(words); i++) {
		if (words[i] && 0 == strcasecmp(words[i], word))
			return i;
	}

	if (0 == strncasecmp(word, "ndots:", sizeof "ndots:" - 1))
		return DNS_RESCONF_NDOTS;

	return 0;
} /* dns_resconf_keyword() */

#define dns_resconf_issep(ch)	(isspace(ch) || (ch) == ',')
#define dns_resconf_iscom(ch)	((ch) == '#' || (ch) == ';')

int dns_resconf_loadfile(struct dns_resolv_conf *resconf, FILE *fp) {
	unsigned sa_count	= 0;
	char words[6][DNS_D_MAXNAME + 1];
	unsigned wp, wc, i, j, n;
	int ch, af;
	struct sockaddr *sa;

	rewind(fp);

	do {
		memset(words, '\0', sizeof words);
		wp	= 0;
		wc	= 0;

		while (EOF != (ch = getc(fp)) && ch != '\n') {
			if (dns_resconf_issep(ch)) {
				if (wp > 0) {
					wp	= 0;

					if (++wc >= lengthof(words))
						goto skip;
				}
			} else if (dns_resconf_iscom(ch)) {
skip:
				do {
					ch	= getc(fp);
				} while (ch != EOF && ch != '\n');

				break;
			} else {
				dns__printchar(words[wc], sizeof words[wc], wp, ch);
				wp++;
			}
		}

		if (wp > 0)
			wc++;

		if (wc < 2)
			continue;

		switch (dns_resconf_keyword(words[0])) {
		case DNS_RESCONF_NAMESERVER:
			if (sa_count >= lengthof(resconf->nameserver))
				continue;

			af	= (strchr(words[1], ':'))? AF_INET6 : AF_INET;
			sa	= (struct sockaddr *)&resconf->nameserver[sa_count];

			if (1 != dns_inet_pton(af, words[1], dns_sa_addr(af, sa)))
				continue;

			*dns_sa_port(af, sa)	= htons(53);
			sa->sa_family		= af;

			sa_count++;

			break;
		case DNS_RESCONF_DOMAIN:
		case DNS_RESCONF_SEARCH:
			memset(resconf->search, '\0', sizeof resconf->search);

			for (i = 1, j = 0; i < wc && j < lengthof(resconf->search); i++, j++)
				dns_d_anchor(resconf->search[j], sizeof resconf->search[j], words[i], strlen(words[i]));

			break;
		case DNS_RESCONF_LOOKUP:
			for (i = 1, j = 0; i < wc && j < lengthof(resconf->lookup); i++) {
				switch (dns_resconf_keyword(words[i])) {
				case DNS_RESCONF_FILE:
					resconf->lookup[j++]	= 'f';

					break;
				case DNS_RESCONF_BIND:
					resconf->lookup[j++]	= 'b';

					break;
				default:
					break;
				} /* switch() */
			} /* for() */

			break;
		case DNS_RESCONF_OPTIONS:
			for (i = 1; i < wc; i++) {
				switch (dns_resconf_keyword(words[i])) {
				case DNS_RESCONF_EDNS0:
					resconf->options.edns0	= 1;

					break;
				case DNS_RESCONF_NDOTS:
					for (j = sizeof "ndots:" - 1, n = 0; isdigit((int)words[i][j]); j++) {
						n	*= 10;
						n	+= words[i][j] - '0';
					} /* for() */

					resconf->options.ndots	= n;

					break;
				case DNS_RESCONF_RECURSIVE:
					resconf->options.recursive	= 1;

					break;
				default:
					break;
				} /* switch() */
			} /* for() */

			break;
		case DNS_RESCONF_INTERFACE:
			for (i = 0, n = 0; isdigit((int)words[2][i]); i++) {
				n	*= 10;
				n	+= words[2][i] - '0';
			}

			dns_resconf_setiface(resconf, words[1], n);

			break;
		default:
			break;
		} /* switch() */
	} while (ch != EOF);

	return 0;
} /* dns_resconf_loadfile() */


int dns_resconf_loadpath(struct dns_resolv_conf *resconf, const char *path) {
	FILE *fp;
	int error;

	if (!(fp = fopen(path, "r")))
		return errno;

	error	= dns_resconf_loadfile(resconf, fp);

	fclose(fp);

	return error;
} /* dns_resconf_loadpath() */


int dns_resconf_setiface(struct dns_resolv_conf *resconf, const char *addr, unsigned short port) {
	int af	= (strchr(addr, ':'))? AF_INET6 : AF_INET;

	if (1 != dns_inet_pton(af, addr, dns_sa_addr(af, &resconf->interface)))
		return errno;

	*dns_sa_port(af, &resconf->interface)	= htons(port);
	resconf->interface.ss_family		= af;

	return 0;
} /* dns_resconf_setiface() */


size_t dns_resconf_search(void *dst, size_t lim, const void *qname, size_t qlen, struct dns_resolv_conf *resconf, unsigned long *state) {
	unsigned srchi		= 0xff & (*state >> 8);
	unsigned ndots		= 0xff & (*state >> 16);
	unsigned slen, len	= 0;
	const char *qp, *qe;

//	assert(0xff > lengthof(resconf->search));

	switch (0xff & *state) {
	case 0:
		qp	= qname;
		qe	= qp + qlen;

		while ((qp = memchr(qp, '.', qe - qp)))
			{ ndots++; qp++; }

		++*state;

		if (ndots >= resconf->options.ndots) {
			len	= dns_d_anchor(dst, lim, qname, qlen);

			break;
		}

		/* FALL THROUGH */
	case 1:
		if (srchi < lengthof(resconf->search) && (slen = strlen(resconf->search[srchi]))) {
			len	= dns__printstring(dst, lim, 0, qname, qlen);
			len	= dns_d_anchor(dst, lim, dst, len);
			len	+= dns__printstring(dst, lim, len, resconf->search[srchi], slen);

			srchi++;

			break;
		}

		++*state;

		/* FALL THROUGH */
	case 2:
		++*state;

		if (ndots < resconf->options.ndots) {
			len	= dns_d_anchor(dst, lim, qname, qlen);

			break;
		}

		/* FALL THROUGH */
	default:
		break;
	} /* switch() */

	dns__printnul(dst, lim, len);

	*state	= ((0xff & *state) << 0)
		| ((0xff & srchi) << 8)
		| ((0xff & ndots) << 16);

	return len;
} /* dns_resconf_search() */


static void dns_resconf_dump(struct dns_resolv_conf *resconf, FILE *fp) {
	unsigned i;
	int af;

	for (i = 0; i < lengthof(resconf->nameserver) && (af = resconf->nameserver[i].ss_family) != AF_UNSPEC; i++) {
		char addr[INET6_ADDRSTRLEN + 1]	= "[INVALID]";

		dns_inet_ntop(af, dns_sa_addr(af, &resconf->nameserver[i]), addr, sizeof addr);

		fprintf(fp, "nameserver %s\n", addr);
	}


	fprintf(fp, "search");

	for (i = 0; i < lengthof(resconf->search) && resconf->search[i][0]; i++)
		fprintf(fp, " %s", resconf->search[i]);

	fputc('\n', fp);


	fprintf(fp, "lookup");

	for (i = 0; i < lengthof(resconf->lookup) && resconf->lookup[i]; i++) {
		switch (resconf->lookup[i]) {
		case 'b':
			fprintf(fp, " bind"); break;
		case 'f':
			fprintf(fp, " file"); break;
		}
	}

	fputc('\n', fp);


	fprintf(fp, "options ndots:%d", resconf->options.ndots);

	if (resconf->options.edns0)
		fprintf(fp, " edns0");
	if (resconf->options.recursive)
		fprintf(fp, " recursive");
	
	fputc('\n', fp);


	if ((af = resconf->interface.ss_family) != AF_UNSPEC) {
		char addr[INET6_ADDRSTRLEN + 1]	= "[INVALID]";

		dns_inet_ntop(af, dns_sa_addr(af, &resconf->interface), addr, sizeof addr);

		fprintf(fp, "interface %s %hu\n", addr, ntohs(*dns_sa_port(af, &resconf->interface)));
	}

	return /* void */;
} /* dns_resconf_dump() */


/*
 * H I N T  S E R V E R  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct dns_hints_soa {
	unsigned char zone[DNS_D_MAXNAME + 1];
	
	struct {
		struct sockaddr_storage ss;

		struct {
			dns_atomic_t saved, effective, ttl;
		} pri;

		dns_atomic_t nlost;
	} addrs[16];

	unsigned count;

	struct dns_hints_soa *next;
}; /* struct dns_hints_soa */


struct dns_hints {
	dns_atomic_t refcount;

	struct dns_hints_soa *head;
}; /* struct dns_hints */


struct dns_hints *dns_hints_open(int *error) {
	static const struct dns_hints H_initializer;
	struct dns_hints *H;

	if (!(H = malloc(sizeof *H)))
		goto syerr;

	*H	= H_initializer;

	dns_hints_acquire(H);

	return H;
syerr:
	*error	= errno;

	free(H);

	return 0;
} /* dns_hints_open() */


void dns_hints_close(struct dns_hints *H) {
	struct dns_hints_soa *soa, *nxt;

	if (!H || 1 != dns_hints_release(H))
		return /* void */;

	for (soa = H->head; soa; soa = nxt) {
		nxt	= soa->next;

		free(soa);
	}

	free(H);

	return /* void */;
} /* dns_hints_close() */


unsigned dns_hints_acquire(struct dns_hints *H) {
	return dns_atomic_inc(&H->refcount);
} /* dns_hints_acquire() */


unsigned dns_hints_release(struct dns_hints *H) {
	return dns_atomic_dec(&H->refcount);
} /* dns_hints_release() */


static struct dns_hints_soa *dns_hints_fetch(struct dns_hints *H, const char *zone) {
	struct dns_hints_soa *soa;

	for (soa = H->head; soa; soa = soa->next) {
		if (0 == strcasecmp(zone, (char *)soa->zone))
			return soa;
	}

	return 0;
} /* dns_hints_fetch() */


int dns_hints_insert(struct dns_hints *H, const char *zone, const struct sockaddr *sa, unsigned priority) {
	static const struct dns_hints_soa soa_initializer;
	struct dns_hints_soa *soa;
	unsigned i;

	if (!(soa = dns_hints_fetch(H, zone))) {
		if (!(soa = malloc(sizeof *soa)))
			return errno;

		*soa	= soa_initializer;

		dns__printstring(soa->zone, sizeof soa->zone, 0, zone);

		soa->next	= H->head;
		H->head		= soa->next;
	}

	i	= soa->count % lengthof(soa->addrs);

	memcpy(&soa->addrs[i].ss, sa, dns_sa_len(sa));

	dns_atomic_store(&soa->addrs[i].pri.effective, MAX(1, priority));
	dns_atomic_store(&soa->addrs[i].pri.saved, MAX(1, priority));

	if (soa->count < lengthof(soa->addrs))
		soa->count++;

	return 0;
} /* dns_hints_insert() */


unsigned dns_hints_insert_resconf(struct dns_hints *H, const struct dns_resolv_conf *resconf, int *error) {
	unsigned i, n;

	for (i = 0, n = 0; i < lengthof(resconf->nameserver) && resconf->nameserver[i].ss_family != AF_UNSPEC; i++) {
		if ((*error = dns_hints_insert(H, ".", (struct sockaddr *)&resconf->nameserver[i], n + 1)))
			break;

		n++;
	}

	return n;
} /* dns_hints_insert_resconf() */


/*
 * FIXME: This code (might need to / should) be refactored to ensure that
 * it can never lead to something undesirable, like an infinite loop in
 * dns_hints_i_grep() or dns_hints_i_ffwd().
 *
 * The hints structure might be the single most important (perhaps only)
 * critical section. It is highly desirable to keep a shared state regarding
 * unreachable servers (i.e. the effective priority), especially of caching
 * servers if we're not recursive. All other shared data can be considered
 * immutable after internal and applied initialization.
 */
void dns_hints_update(struct dns_hints *H, const char *zone, const struct sockaddr *sa, int nice) {
	struct dns_hints_soa *soa;
	unsigned long i, nlost, ttl;
	time_t now	= dns_now();

	if (!(soa = dns_hints_fetch(H, zone)))
		return /* void */;

	for (i = 0; i < soa->count; i++) {
		if (sa->sa_family == soa->addrs[i].ss.ss_family && 0 == memcmp(&soa->addrs[i].ss, sa, dns_sa_len(sa))) {
			if (nice < 0) {
				nlost	= 1 + dns_atomic_inc(&soa->addrs[i].nlost);

				dns_atomic_store(&soa->addrs[i].pri.effective, 0);

				dns_atomic_store(&soa->addrs[i].pri.ttl, now + MIN(60, 3 * nlost));
			} else if (nice > 0) {
				goto reset;
			}
		} else {
			ttl	= dns_atomic_load(&soa->addrs[i].pri.ttl);

			if (ttl > 0 && ttl < now) {
reset:
				dns_atomic_store(&soa->addrs[i].pri.effective, dns_atomic_load(&soa->addrs[i].pri.saved));
				dns_atomic_store(&soa->addrs[i].pri.ttl, 0);
				dns_atomic_store(&soa->addrs[i].nlost, 0);
			}
		}
	}

	return /* void */;
} /* dns_hints_update() */


struct dns_hints_i *dns_hints_i_init(struct dns_hints_i *i) {
	static const struct dns_hints_i i_initializer;

	i->state	= i_initializer.state;

	return i;
} /* dns_hints_i_init() */


static int dns_hints_i_ffwd(struct dns_hints_i *i, struct dns_hints_soa *soa) {
	unsigned min, max;
	int j, found;

	do {
		while (i->state.p < i->state.end) {
			j = i->state.p++ % soa->count;

			if (dns_atomic_load(&soa->addrs[j].pri.effective) == i->state.priority)
				return j;
		}

		/* Scan for next priority */
		min	= ++i->state.priority;
		max	= -1;
		found	= 0;

		for (j = 0; j < soa->count; j++) {
			unsigned pri	= dns_atomic_load(&soa->addrs[j].pri.effective);

			if (pri >= min && pri <= max) {
				i->state.priority	= pri;
				max			= pri;
				found			= 1;
			}
		}

		i->state.p	= 0xff & dns_random();
		i->state.end	= i->state.p + soa->count;
	} while (found);

	return -1;
} /* dns_hints_i_ffwd() */


unsigned dns_hints_grep(struct sockaddr **sa, socklen_t *sa_len, unsigned lim, struct dns_hints_i *i, struct dns_hints *H) {
	struct dns_hints_soa *soa;
	unsigned n;
	int j;

	if (!(soa = dns_hints_fetch(H, i->zone)))
		return 0;

	n	= 0;

	while (n < lim && -1 != (j = dns_hints_i_ffwd(i, soa))) {
		*sa	= (struct sockaddr *)&soa->addrs[j].ss;
		*sa_len	= dns_sa_len(*sa);

		sa++;
		sa_len++;
		n++;
	}

	return n;
} /* dns_hints_grep() */


/*
 * R E S O L V E R  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

struct dns_resolver {
	int fd;

	struct dns_resolv_conf *resconf;
	struct dns_hints *hints;

	dns_atomic_t refcount;

	unsigned char qname[DNS_D_MAXNAME];
}; /* struct dns_resolver */


struct dns_resolver *dns_r_open(struct dns_resolv_conf *resconf, struct dns_hints *hints, int *error) {
	static const struct dns_resolver R_initializer
		= { .fd = -1, .refcount = 1, };
	struct dns_resolver *R;

	if (!(R = malloc(sizeof *R)))
		goto syerr;

	*R	= R_initializer;

	if (-1 == (R->fd = dns_socket((struct sockaddr *)&resconf->interface, SOCK_DGRAM, error)))
		goto error;

	dns_resconf_acquire(resconf);
	R->resconf	= resconf;

	dns_hints_acquire(hints);
	R->hints	= hints;

	return R;
syerr:
	*error	= errno;
error:
	dns_r_close(R);

	return 0;
} /* dns_r_open() */


void dns_r_close(struct dns_resolver *R) {
	if (!R || 1 < dns_r_release(R))
		return;

	dns_soclose(R->fd);

	dns_hints_close(R->hints);
	dns_resconf_close(R->resconf);

	free(R);
} /* dns_r_close() */


unsigned dns_r_acquire(struct dns_resolver *R) {
	return dns_atomic_inc(&R->refcount);
} /* dns_r_acquire() */


unsigned dns_r_release(struct dns_resolver *R) {
	return dns_atomic_dec(&R->refcount);
} /* dns_r_release() */


/*
 * M I S C E L L A N E O U S  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

const char *(dns_strsection)(enum dns_section section, void *dst, size_t lim) {
	switch (section) {
	case DNS_S_QD:
		dns__printstring(dst, lim, 0, "QUESTION");

		break;
	case DNS_S_AN:
		dns__printstring(dst, lim, 0, "ANSWER");

		break;
	case DNS_S_NS:
		dns__printstring(dst, lim, 0, "AUTHORITY");

		break;
	case DNS_S_AR:
		dns__printstring(dst, lim, 0, "ADDITIONAL");

		break;
	default:
		dns__printnul(dst, lim, dns__print10(dst, lim, 0, 0xffff & section));

		break;
	} /* switch (class) */

	return dst;
} /* dns_strsection() */


const char *(dns_strclass)(enum dns_class class, void *dst, size_t lim) {
	switch (class) {
	case DNS_C_IN:
		dns__printstring(dst, lim, 0, "IN");

		break;
	default:
		dns__printnul(dst, lim, dns__print10(dst, lim, 0, 0xffff & class));

		break;
	} /* switch (class) */

	return dst;
} /* dns_strclass() */


const char *(dns_strtype)(enum dns_type type, void *dst, size_t lim) {
	unsigned i;

	for (i = 0; i < lengthof(dns_rrtypes); i++) {
		if (dns_rrtypes[i].type == type) {
			dns__printstring(dst, lim, 0, dns_rrtypes[i].name);

			return dst;
		}
	}

	dns__printnul(dst, lim, dns__print10(dst, lim, 0, 0xffff & type));

	return dst;
} /* dns_strtype() */


/*
 * C O M M A N D - L I N E / R E G R E S S I O N  R O U T I N E S
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>

#include <ctype.h>

#include <err.h>


struct {
	struct {
		const char *path[8];
		unsigned count;
	} resconf;

	int verbose;
} MAIN;


void dump(const unsigned char *src, size_t len, FILE *fp) {
	static const unsigned char hex[]	= "0123456789abcdef";
	static const unsigned char tmpl[]	= "                                                    |                |\n";
	unsigned char ln[sizeof tmpl];
	const unsigned char *sp, *se;
	unsigned char *h, *g;
	unsigned i, n;

	sp	= src;
	se	= sp + len;

	while (sp < se) {
		memcpy(ln, tmpl, sizeof ln);

		h	= &ln[2];
		g	= &ln[53];

		for (n = 0; n < 2; n++) {
			for (i = 0; i < 8 && se - sp > 0; i++, sp++) {
				h[0]	= hex[0x0f & (*sp >> 4)];
				h[1]	= hex[0x0f & (*sp >> 0)];
				h	+= 3;

				*g++	= (isgraph(*sp))? *sp : '.';
			}

			h++;
		}

		fputs((char *)ln, fp);
	}

	return /* void */;
} /* dump() */


static void panic(const char *fmt, ...) {
	va_list ap;

	va_start(ap, fmt);

	verrx(EXIT_FAILURE, fmt, ap);
} /* panic() */


static struct dns_resolv_conf *resconf(void) {
	static struct dns_resolv_conf *resconf;
	const char *path;
	int error, i;

	if (resconf)
		return resconf;

	if (!(resconf = dns_resconf_open(&error)))
		panic("dns_resconf_open: %s", strerror(error));

	if (!MAIN.resconf.count)
		MAIN.resconf.path[MAIN.resconf.count++]	= "/etc/resolv.conf";

	for (i = 0; i < MAIN.resconf.count; i++) {
		path	= MAIN.resconf.path[i];

		if (0 == strcmp(path, "-"))
			error	= dns_resconf_loadfile(resconf, stdin);
		else
			error	= dns_resconf_loadpath(resconf, path);

		if (error)
			panic("%s: %s", path, strerror(error));
	}

	return resconf;
} /* resconf() */


static int parse_packet(int argc, char *argv[]) {
	struct dns_packet *P	= dns_p_new(512);
	struct dns_packet *Q	= dns_p_new(512);
	enum dns_section section;
	struct dns_rr rr;
	int error;
	union dns_any any;
	char pretty[sizeof any * 2];
	size_t len;

	P->end	= fread(P->data, 1, P->size, stdin);

	section	= 0;

	dns_rr_foreach(&rr, P) {
		if (section != rr.section)
			fprintf(stdout, ";; [%s]\n", dns_strsection(rr.section));

		if ((len = dns_rr_print(pretty, sizeof pretty, &rr, P, &error)))
			fprintf(stdout, "%s\n", pretty);

		dns_rr_copy(Q, &rr, P);

		section	= rr.section;
	}

	fputc('\n', stdout);

	section	= 0;

#if 0
	dns_rr_foreach(&rr, Q, .name = "ns8.yahoo.com.") {
#else
	struct dns_rr rrset[32];
	struct dns_rr_i *rri	= dns_rr_i_new(.name = dns_d_new("ns8.yahoo.com", DNS_D_ANCHOR));
	unsigned rrcount	= dns_rr_grep(rrset, lengthof(rrset), rri, Q, &error);

	for (unsigned i = 0; i < rrcount; i++) {
		rr	= rrset[i];
#endif
		if (section != rr.section)
			fprintf(stdout, ";; [%s]\n", dns_strsection(rr.section));

		if ((len = dns_rr_print(pretty, sizeof pretty, &rr, Q, &error)))
			fprintf(stdout, "%s\n", pretty);

		section	= rr.section;
	}

	if (MAIN.verbose) {
		fprintf(stderr, "orig:%zu\n", P->end);
		dump(P->data, P->end, stdout);

		fprintf(stderr, "copy:%zu\n", Q->end);
		dump(Q->data, Q->end, stdout);
	}

	return 0;
} /* parse_packet() */


static int parse_domain(int argc, char *argv[]) {
	char *dn;

	dn	= (argc)? argv[0] : "f.l.google.com";

	printf("[%s]\n", dn);

	dn	= dns_d_new(dn);

	do {
		puts(dn);
	} while (dns_d_cleave(dn, strlen(dn) + 1, dn, strlen(dn)));

	return 0;
} /* parse_domain() */


static int parse_resconf(int argc, char *argv[]) {
	dns_resconf_dump(resconf(), stdout);

	return 0;
} /* parse_resconf() */


static int search_list(int argc, char *argv[]) {
	const char *qname	= (argc)? argv[0] : "f.l.google.com";
	unsigned long i		= 0;
	char name[DNS_D_MAXNAME + 1];

	printf("[%s]\n", qname);

	while (dns_resconf_search(name, sizeof name, qname, strlen(qname), resconf(), &i))
		puts(name);

	return 0;
} /* search_list() */


int main(int argc, char **argv) {
	static const struct { const char *cmd; int (*run)(); } cmds[]
		= {{ "parse-packet",	&parse_packet	},
		   { "parse-domain",	&parse_domain	},
		   { "parse-resconf",	&parse_resconf	},
		   { "search-list",	&search_list	}};
	extern int optind;
	extern char *optarg;
	int ch, i;

	while (-1 != (ch = getopt(argc, argv, "c:v"))) {
		switch (ch) {
		case 'c':
			assert(MAIN.resconf.count < lengthof(MAIN.resconf.path));

			MAIN.resconf.path[MAIN.resconf.count++]	= optarg;

			break;
		case 'v':
			MAIN.verbose	= 1;

			break;
		default:
			return 1;
		} /* switch() */
	} /* while() */

	argc	-= optind;
	argv	+= optind;

	for (i = 0; i < lengthof(cmds) && argv[0]; i++) {
		if (0 == strcmp(cmds[i].cmd, argv[0]))
			return cmds[i].run(--argc, ++argv);
	}

	return 0;
} /* main() */
