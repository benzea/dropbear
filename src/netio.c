#include "netio.h"
#include "list.h"
#include "dbutil.h"
#include "session.h"
#include "debug.h"
#include "runopts.h"

#ifdef HAVE_LINUX_VM_SOCKETS_H
void
dropbear_freeaddrinfo(struct addrinfo *ai)
{
	struct addrinfo *next;

	/* For AF_VSOCK, we allocated it ourselves, so free it here as we
	 * cannot be sure that the stock freeaddrinfo free's it in the same
	 * way as we allocated it.
	 */
	if (ai && ai->ai_family != AF_VSOCK)
		return freeaddrinfo(ai);

	for(; ai != NULL;) {
		next = ai->ai_next;
		free(ai);
		ai = next;
	}
}

int dropbear_getaddrinfo(const char *hostname, const char *servname,
	const struct addrinfo *hints, struct addrinfo **res)
{
	const char *vsock = strstr(hostname, "%vsock");
	if (vsock && (hints->ai_family == AF_UNSPEC || hints->ai_family == AF_VSOCK)) {
		struct addrinfo *vsock_res;
		struct sockaddr_vm *vsockaddr;

		vsock_res = calloc(1, sizeof(struct addrinfo) + sizeof(*vsockaddr));
		vsockaddr = (void *)(vsock_res + 1);
		vsock_res->ai_family = AF_VSOCK;
		vsock_res->ai_socktype = hints->ai_socktype;
		vsock_res->ai_addr = (struct sockaddr *)vsockaddr;
		vsock_res->ai_addrlen = sizeof(*vsockaddr);
		vsockaddr->svm_family = AF_VSOCK;
		if (vsock != hostname)
			vsockaddr->svm_cid = atoi(hostname);
		else
			vsockaddr->svm_cid = VMADDR_CID_ANY;
		vsockaddr->svm_port = atoi(servname);

		if (res)
			*res = vsock_res;
		else
			dropbear_freeaddrinfo(vsock_res);

		return 0;
	}

	return getaddrinfo(hostname, servname, hints, res);
}
#else
#define dropbear_freeaddrinfo freeaddrinfo
#define dropbear_getaddrinfo getaddrinfo
#endif


struct dropbear_progress_connection {
	struct addrinfo *res;
	struct addrinfo *res_iter;

	char *remotehost, *remoteport; /* For error reporting */

	connect_callback cb;
	void *cb_data;

	struct Queue *writequeue; /* A queue of encrypted packets to send with TCP fastopen,
								or NULL. */

	int sock;

	char* errstring;
	char *bind_address, *bind_port;
	enum dropbear_prio prio;
};

/* Deallocate a progress connection. Removes from the pending list if iter!=NULL.
Does not close sockets */
static void remove_connect(struct dropbear_progress_connection *c, m_list_elem *iter) {
	if (c->res) {
		/* Only call freeaddrinfo if connection is not AF_UNIX. */
		if (c->res->ai_family != AF_UNIX) {
			freeaddrinfo(c->res);
		} else {
			m_free(c->res);
		}
	}
	m_free(c->remotehost);
	m_free(c->remoteport);
	m_free(c->errstring);
	m_free(c->bind_address);
	m_free(c->bind_port);
	m_free(c);

	if (iter) {
		list_remove(iter);
	}
}

static void cancel_callback(int result, int sock, void* UNUSED(data), const char* UNUSED(errstring)) {
	if (result == DROPBEAR_SUCCESS)
	{
		m_close(sock);
	}
}

void cancel_connect(struct dropbear_progress_connection *c) {
	c->cb = cancel_callback;
	c->cb_data = NULL;
}

static void connect_try_next(struct dropbear_progress_connection *c) {
	struct addrinfo *r;
	int err;
	int res = 0;
	int fastopen = 0;
	int retry_errno = EINPROGRESS;
#if DROPBEAR_CLIENT_TCP_FAST_OPEN
	struct msghdr message;
#endif

	for (r = c->res_iter; r; r = r->ai_next)
	{
		dropbear_assert(c->sock == -1);

		c->sock = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
		if (c->sock < 0) {
			continue;
		}

		/* According to the connect(2) manpage it should be testing EAGAIN
		 * rather than EINPROGRESS for unix sockets.
		 */
		retry_errno = r->ai_family == AF_UNIX ? EAGAIN : EINPROGRESS;

		if (c->bind_address || c->bind_port) {
			/* bind to a source port/address */
			struct addrinfo hints;
			struct addrinfo *bindaddr = NULL;
			memset(&hints, 0, sizeof(hints));
			hints.ai_socktype = SOCK_STREAM;
			hints.ai_family = r->ai_family;
			hints.ai_flags = AI_PASSIVE;

			err = dropbear_getaddrinfo(c->bind_address, c->bind_port, &hints, &bindaddr);
			if (err) {
				int len = 100 + strlen(gai_strerror(err));
				m_free(c->errstring);
				c->errstring = (char*)m_malloc(len);
				snprintf(c->errstring, len, "Error resolving bind address '%s' (port %s). %s", 
						c->bind_address, c->bind_port, gai_strerror(err));
				TRACE(("Error resolving bind: %s", gai_strerror(err)))
				close(c->sock);
				c->sock = -1;
				continue;
			}
			res = bind(c->sock, bindaddr->ai_addr, bindaddr->ai_addrlen);
			freeaddrinfo(bindaddr);
			bindaddr = NULL;
			if (res < 0) {
				/* failure */
				int keep_errno = errno;
				int len = 300;
				m_free(c->errstring);
				c->errstring = m_malloc(len);
				snprintf(c->errstring, len, "Error binding local address '%s' (port %s). %s", 
						c->bind_address, c->bind_port, strerror(keep_errno));
				close(c->sock);
				c->sock = -1;
				continue;
			}
		}

		ses.maxfd = MAX(ses.maxfd, c->sock);
		set_sock_nodelay(c->sock);
		set_sock_priority(c->sock, c->prio);
		setnonblocking(c->sock);

#if DROPBEAR_CLIENT_TCP_FAST_OPEN
		fastopen = (c->writequeue != NULL &&
			    (r->ai_family == AF_INET || r->ai_family == AF_INET6);

		if (fastopen) {
			memset(&message, 0x0, sizeof(message));
			message.msg_name = r->ai_addr;
			message.msg_namelen = r->ai_addrlen;
			/* 6 is arbitrary, enough to hold initial packets */
			unsigned int iovlen = 6; /* Linux msg_iovlen is a size_t */
			struct iovec iov[6];
			packet_queue_to_iovec(c->writequeue, iov, &iovlen);
			message.msg_iov = iov;
			message.msg_iovlen = iovlen;
			res = sendmsg(c->sock, &message, MSG_FASTOPEN);
			/* Returns EINPROGRESS if FASTOPEN wasn't available */
			if (res < 0) {
				if (errno != EINPROGRESS) {
					m_free(c->errstring);
					c->errstring = m_strdup(strerror(errno));
					/* Not entirely sure which kind of errors are normal - 2.6.32 seems to 
					return EPIPE for any (nonblocking?) sendmsg(). just fall back */
					TRACE(("sendmsg tcp_fastopen failed, falling back. %s", strerror(errno)));
					/* No kernel MSG_FASTOPEN support. Fall back below */
					fastopen = 0;
					/* Set to NULL to avoid trying again */
					c->writequeue = NULL;
				}
			} else {
				packet_queue_consume(c->writequeue, res);
			}
		}
#endif

		/* Normal connect(), used as fallback for TCP fastopen too */
		if (!fastopen) {
			res = connect(c->sock, r->ai_addr, r->ai_addrlen);
		}

		if (res < 0 && errno != retry_errno) {
			/* failure */
			m_free(c->errstring);
			c->errstring = m_strdup(strerror(errno));
			close(c->sock);
			c->sock = -1;
			continue;
		} else {
			/* new connection was successful, wait for it to complete */
			break;
		}
	}

	if (r) {
		c->res_iter = r->ai_next;
	} else {
		c->res_iter = NULL;
	}
}

/* Connect via TCP to a host. */
struct dropbear_progress_connection *connect_remote(const char* remotehost, const char* remoteport,
	connect_callback cb, void* cb_data,
	const char* bind_address, const char* bind_port, enum dropbear_prio prio)
{
	struct dropbear_progress_connection *c = NULL;
	int err;
	struct addrinfo hints;

	c = m_malloc(sizeof(*c));
	c->remotehost = m_strdup(remotehost);
	c->remoteport = m_strdup(remoteport);
	c->sock = -1;
	c->cb = cb;
	c->cb_data = cb_data;
	c->prio = prio;

	list_append(&ses.conn_pending, c);

#if DROPBEAR_FUZZ
	if (fuzz.fuzzing) {
		c->errstring = m_strdup("fuzzing connect_remote always fails");
		return c;
	}
#endif

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;

	err = dropbear_getaddrinfo(remotehost, remoteport, &hints, &c->res);
	if (err) {
		int len;
		len = 100 + strlen(gai_strerror(err));
		c->errstring = (char*)m_malloc(len);
		snprintf(c->errstring, len, "Error resolving '%s' port '%s'. %s", 
				remotehost, remoteport, gai_strerror(err));
		TRACE(("Error resolving: %s", gai_strerror(err)))
	} else {
		c->res_iter = c->res;
	}
	
	if (bind_address) {
		c->bind_address = m_strdup(bind_address);
	}
	if (bind_port) {
		c->bind_port = m_strdup(bind_port);
	}

	return c;
}


/* Connect to stream local socket. */
struct dropbear_progress_connection *connect_streamlocal(const char* localpath,
	connect_callback cb, void* cb_data, enum dropbear_prio prio)
{
	struct dropbear_progress_connection *c = NULL;
	struct sockaddr_un *sunaddr;

	c = m_malloc(sizeof(*c));
	c->remotehost = m_strdup(localpath);
	c->remoteport = NULL;
	c->sock = -1;
	c->cb = cb;
	c->cb_data = cb_data;
	c->prio = prio;

	list_append(&ses.conn_pending, c);

#if DROPBEAR_FUZZ
	if (fuzz.fuzzing) {
		c->errstring = m_strdup("fuzzing connect_streamlocal always fails");
		return c;
	}
#endif

	if (strlen(localpath) >= sizeof(sunaddr->sun_path)) {
		c->errstring = m_strdup("Stream path too long");
		TRACE(("localpath: %s is too long", localpath));
		return c;
	}

	/*
	 * Fake up a struct addrinfo for AF_UNIX connections.
	 * remove_connect() must check ai_family
	 * and use m_free() not freeaddirinfo() for AF_UNIX.
	 */
	c->res = m_malloc(sizeof(*c->res) + sizeof(*sunaddr));
	c->res->ai_addr = (struct sockaddr *)(c->res + 1);
	c->res->ai_addrlen = sizeof(*sunaddr);
	c->res->ai_family = AF_UNIX;
	c->res->ai_socktype = SOCK_STREAM;
	c->res->ai_protocol = PF_UNSPEC;
	sunaddr = (struct sockaddr_un *)c->res->ai_addr;
	sunaddr->sun_family = AF_UNIX;
	strlcpy(sunaddr->sun_path, localpath, sizeof(sunaddr->sun_path));

	/* Copy to target iter */ 
	c->res_iter = c->res;

	return c;
}

void remove_connect_pending() {
	while (ses.conn_pending.first) {
		struct dropbear_progress_connection *c = ses.conn_pending.first->item;
		remove_connect(c, ses.conn_pending.first);
	}
}


void set_connect_fds(fd_set *writefd) {
	m_list_elem *iter;
	iter = ses.conn_pending.first;
	while (iter) {
		m_list_elem *next_iter = iter->next;
		struct dropbear_progress_connection *c = iter->item;
		/* Set one going */
		while (c->res_iter && c->sock < 0) {
			connect_try_next(c);
		}
		if (c->sock >= 0) {
			FD_SET(c->sock, writefd);
		} else {
			/* Final failure */
			if (!c->errstring) {
				c->errstring = m_strdup("unexpected failure");
			}
			c->cb(DROPBEAR_FAILURE, -1, c->cb_data, c->errstring);
			remove_connect(c, iter);
		}
		iter = next_iter;
	}
}

void handle_connect_fds(const fd_set *writefd) {
	m_list_elem *iter;
	for (iter = ses.conn_pending.first; iter; iter = iter->next) {
		int val;
		socklen_t vallen = sizeof(val);
		struct dropbear_progress_connection *c = iter->item;

		if (c->sock < 0 || !FD_ISSET(c->sock, writefd)) {
			continue;
		}

		TRACE(("handling %s port %s socket %d", c->remotehost, c->remoteport, c->sock));

		if (getsockopt(c->sock, SOL_SOCKET, SO_ERROR, &val, &vallen) != 0) {
			TRACE(("handle_connect_fds getsockopt(%d) SO_ERROR failed: %s", c->sock, strerror(errno)))
			/* This isn't expected to happen - Unix has surprises though, continue gracefully. */
			m_close(c->sock);
			c->sock = -1;
		} else if (val != 0) {
			/* Connect failed */
			TRACE(("connect to %s port %s failed.", c->remotehost, c->remoteport))
			m_close(c->sock);
			c->sock = -1;

			m_free(c->errstring);
			c->errstring = m_strdup(strerror(val));
		} else {
			/* New connection has been established */
			c->cb(DROPBEAR_SUCCESS, c->sock, c->cb_data, NULL);
			remove_connect(c, iter);
			TRACE(("leave handle_connect_fds - success"))
			/* Must return here - remove_connect() invalidates iter */
			return; 
		}
	}
}

void connect_set_writequeue(struct dropbear_progress_connection *c, struct Queue *writequeue) {
	c->writequeue = writequeue;
}

void packet_queue_to_iovec(const struct Queue *queue, struct iovec *iov, unsigned int *iov_count) {
	struct Link *l;
	unsigned int i;
	int len;
	buffer *writebuf;

#ifndef IOV_MAX
	#if (defined(__CYGWIN__) || defined(__GNU__)) && !defined(UIO_MAXIOV)
		#define IOV_MAX 1024
	#elif defined(__sgi)
		#define IOV_MAX 512 
	#else 
		#define IOV_MAX UIO_MAXIOV
	#endif
#endif

	*iov_count = MIN(MIN(queue->count, IOV_MAX), *iov_count);

	for (l = queue->head, i = 0; i < *iov_count; l = l->link, i++)
	{
		writebuf = (buffer*)l->item;
		len = writebuf->len - writebuf->pos;
		dropbear_assert(len > 0);
		TRACE2(("write_packet writev #%d len %d/%d", i,
				len, writebuf->len))
		iov[i].iov_base = buf_getptr(writebuf, len);
		iov[i].iov_len = len;
	}
}

void packet_queue_consume(struct Queue *queue, ssize_t written) {
	buffer *writebuf;
	int len;
	while (written > 0) {
		writebuf = (buffer*)examine(queue);
		len = writebuf->len - writebuf->pos;
		if (len > written) {
			/* partial buffer write */
			buf_incrpos(writebuf, written);
			written = 0;
		} else {
			written -= len;
			dequeue(queue);
			buf_free(writebuf);
		}
	}
}

void set_sock_nodelay(int sock) {
	int val;

	/* disable nagle */
	val = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (void*)&val, sizeof(val));
}

#if DROPBEAR_SERVER_TCP_FAST_OPEN
void set_listen_fast_open(int sock) {
	int qlen = MAX(MAX_UNAUTH_PER_IP, 5);
	if (setsockopt(sock, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen)) != 0) {
		TRACE(("set_listen_fast_open failed for socket %d: %s", sock, strerror(errno)))
	}
}

#endif

void set_sock_priority(int sock, enum dropbear_prio prio) {

	int rc;
	int val;

#if DROPBEAR_FUZZ
	if (fuzz.fuzzing) {
		TRACE(("fuzzing skips set_sock_prio"))
		return;
	}
#endif
	/* Don't log ENOTSOCK errors so that this can harmlessly be called
	 * on a client '-J' proxy pipe */

	if (opts.disable_ip_tos == 0) {
#ifdef IP_TOS
	/* Set the DSCP field for outbound IP packet priority.
	rfc4594 has some guidance to meanings.

	We set AF21 as "Low-Latency" class for interactive (tty session,
	also handshake/setup packets). Other traffic is left at the default.

	OpenSSH at present uses AF21/CS1, rationale
	https://cvsweb.openbsd.org/src/usr.bin/ssh/readconf.c#rev1.284

	Old Dropbear/OpenSSH and Debian/Ubuntu OpenSSH (at Jan 2022) use
	IPTOS_LOWDELAY/IPTOS_THROUGHPUT

	DSCP constants are from Linux headers, applicable to other platforms
	such as macos.
	*/
	if (prio == DROPBEAR_PRIO_LOWDELAY) {
		val = 0x48; /* IPTOS_DSCP_AF21 */
	} else {
		val = 0; /* default */
	}
#if defined(IPPROTO_IPV6) && defined(IPV6_TCLASS)
	rc = setsockopt(sock, IPPROTO_IPV6, IPV6_TCLASS, (void*)&val, sizeof(val));
	if (rc < 0 && errno != ENOTSOCK) {
		TRACE(("Couldn't set IPV6_TCLASS (%s)", strerror(errno)));
	}
#endif
	rc = setsockopt(sock, IPPROTO_IP, IP_TOS, (void*)&val, sizeof(val));
	if (rc < 0 && errno != ENOTSOCK) {
		TRACE(("Couldn't set IP_TOS (%s)", strerror(errno)));
	}
#endif /* IP_TOS */
	}

#ifdef HAVE_LINUX_PKT_SCHED_H
	/* Set scheduling priority within the local Linux network stack */
	if (prio == DROPBEAR_PRIO_LOWDELAY) {
		val = TC_PRIO_INTERACTIVE;
	} else {
		val = 0;
	}
	/* linux specific, sets QoS class. see tc-prio(8) */
	rc = setsockopt(sock, SOL_SOCKET, SO_PRIORITY, (void*) &val, sizeof(val));
	if (rc < 0 && errno != ENOTSOCK) {
		TRACE(("Couldn't set SO_PRIORITY (%s)", strerror(errno)))
    }
#endif

}

/* from openssh/canohost.c avoid premature-optimization */
int get_sock_port(int sock) {
	struct sockaddr_storage from;
	socklen_t fromlen;
	char strport[NI_MAXSERV];
	int r;

	/* Get IP address of client. */
	fromlen = sizeof(from);
	memset(&from, 0, sizeof(from));
	if (getsockname(sock, (struct sockaddr *)&from, &fromlen) < 0) {
		TRACE(("getsockname failed: %d", errno))
		return 0;
	}

#ifdef HAVE_LINUX_VM_SOCKETS_H
	if (from.ss_family == AF_VSOCK)
		return ((struct sockaddr_vm *)&from)->svm_port;
#endif

	/* Work around Linux IPv6 weirdness */
	if (from.ss_family == AF_INET6)
		fromlen = sizeof(struct sockaddr_in6);

	/* Non-inet sockets don't have a port number. */
	if (from.ss_family != AF_INET && from.ss_family != AF_INET6)
		return 0;

	/* Return port number. */
	if ((r = getnameinfo((struct sockaddr *)&from, fromlen, NULL, 0,
	    strport, sizeof(strport), NI_NUMERICSERV)) != 0) {
		TRACE(("netio.c/get_sock_port/getnameinfo NI_NUMERICSERV failed: %d", r))
	}
	return atoi(strport);
}

/* Listen on address:port. 
 * Special cases are address of "" listening on everything,
 * and address of NULL listening on localhost only.
 * Returns the number of sockets bound on success, or -1 on failure. On
 * failure, if errstring wasn't NULL, it'll be a newly malloced error
 * string.*/
int dropbear_listen(const char* address, const char* port,
		int *socks, unsigned int sockcount, char **errstring, int *maxfd, const char* interface) {

	struct addrinfo hints, *res = NULL, *res0 = NULL;
	int err;
	unsigned int nsock;
	int val;
	int sock;
	int allocated_lport = 0;
	
	TRACE(("enter dropbear_listen"))

#if DROPBEAR_FUZZ
	if (fuzz.fuzzing) {
		return fuzz_dropbear_listen(address, port, socks, sockcount, errstring, maxfd);
	}
#endif
	
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC; /* TODO: let them flag v4 only etc */
	hints.ai_socktype = SOCK_STREAM;

	/* for calling getaddrinfo:
	 address == NULL and !AI_PASSIVE: local loopback
	 address == NULL and AI_PASSIVE: all interfaces
	 address != NULL: whatever the address says */
	if (!address) {
		TRACE(("dropbear_listen: local loopback"))
	} else {
		if (address[0] == '\0') {
			if (interface) {
				TRACE(("dropbear_listen: %s", interface))
			} else {
				TRACE(("dropbear_listen: all interfaces"))
			}
			address = NULL;
		}
		hints.ai_flags = AI_PASSIVE;
	}

	err = dropbear_getaddrinfo(address, port, &hints, &res0);

	if (err) {
		if (errstring != NULL && *errstring == NULL) {
			int len;
			len = 20 + strlen(gai_strerror(err));
			*errstring = (char*)m_malloc(len);
			snprintf(*errstring, len, "Error resolving: %s", gai_strerror(err));
		}
		if (res0) {
			dropbear_freeaddrinfo(res0);
			res0 = NULL;
		}
		TRACE(("leave dropbear_listen: failed resolving"))
		return -1;
	}

	/* When listening on server-assigned-port 0
	 * the assigned ports may differ for address families (v4/v6)
	 * causing problems for tcpip-forward.
	 * Caller can do a get_socket_address to discover assigned-port
	 * hence, use same port for all address families */
	allocated_lport = 0;
	nsock = 0;
	for (res = res0; res != NULL && nsock < sockcount;
			res = res->ai_next) {
		if (allocated_lport > 0) {
			if (AF_INET == res->ai_family) {
				((struct sockaddr_in *)res->ai_addr)->sin_port =
					htons(allocated_lport);
			} else if (AF_INET6 == res->ai_family) {
				((struct sockaddr_in6 *)res->ai_addr)->sin6_port =
					htons(allocated_lport);
#ifdef HAVE_LINUX_VM_SOCKETS_H
			} else if (AF_VSOCK == res->ai_family) {
				((struct sockaddr_vm *)res->ai_addr)->svm_port =
					allocated_lport;
#endif
			}
		}

		/* Get a socket */
		socks[nsock] = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		sock = socks[nsock]; /* For clarity */
		if (sock < 0) {
			err = errno;
			TRACE(("socket() failed"))
			continue;
		}

		/* Various useful socket options */
		val = 1;
		/* set to reuse, quick timeout */
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (void*) &val, sizeof(val));

#ifdef SO_BINDTODEVICE
		if(interface && setsockopt(sock, SOL_SOCKET, SO_BINDTODEVICE, interface, strlen(interface)) < 0) {
			dropbear_log(LOG_WARNING, "Couldn't set SO_BINDTODEVICE");
			TRACE(("Failed setsockopt with errno failure, %d %s", errno, strerror(errno)))
		}
#endif

#if defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
		if (res->ai_family == AF_INET6) {
			int on = 1;
			if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, 
						&on, sizeof(on)) == -1) {
				dropbear_log(LOG_WARNING, "Couldn't set IPV6_V6ONLY");
			}
		}
#endif
		set_sock_nodelay(sock);

		if (bind(sock, res->ai_addr, res->ai_addrlen) < 0) {
			err = errno;
			close(sock);
			TRACE(("bind(%s) failed", port))
			continue;
		}

		if (listen(sock, DROPBEAR_LISTEN_BACKLOG) < 0) {
			err = errno;
			close(sock);
			TRACE(("listen() failed"))
			continue;
		}

		if (0 == allocated_lport) {
			allocated_lport = get_sock_port(sock);
		}

		*maxfd = MAX(*maxfd, sock);
		nsock++;
	}

	if (res0) {
		dropbear_freeaddrinfo(res0);
		res0 = NULL;
	}

	if (nsock == 0) {
		if (errstring != NULL && *errstring == NULL) {
			int len;
			len = 20 + strlen(strerror(err));
			*errstring = (char*)m_malloc(len);
			snprintf(*errstring, len, "Error listening: %s", strerror(err));
		}
		TRACE(("leave dropbear_listen: failure, %s", strerror(err)))
		return -1;
	}

	TRACE(("leave dropbear_listen: success, %d socks bound", nsock))
	return nsock;
}

void get_socket_address(int fd, char **local_host, char **local_port,
						char **remote_host, char **remote_port, int host_lookup)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;

#if DROPBEAR_FUZZ
	if (fuzz.fuzzing) {
		fuzz_get_socket_address(fd, local_host, local_port, remote_host, remote_port, host_lookup);
		return;
	}
#endif
	
	if (local_host || local_port) {
		addrlen = sizeof(addr);
		if (getsockname(fd, (struct sockaddr*)&addr, &addrlen) < 0) {
			dropbear_exit("Failed socket address: %s", strerror(errno));
		}
		getaddrstring(&addr, local_host, local_port, host_lookup);		
	}
	if (remote_host || remote_port) {
		addrlen = sizeof(addr);
		if (getpeername(fd, (struct sockaddr*)&addr, &addrlen) < 0) {
			dropbear_exit("Failed socket address: %s", strerror(errno));
		}
		getaddrstring(&addr, remote_host, remote_port, host_lookup);		
	}
}

/* Return a string representation of the socket address passed. The return
 * value is allocated with malloc() */
void getaddrstring(struct sockaddr_storage* addr, 
			char **ret_host, char **ret_port,
			int host_lookup) {

	char host[NI_MAXHOST+1], serv[NI_MAXSERV+1];
	unsigned int len;
	int ret;
	
	int flags = NI_NUMERICSERV | NI_NUMERICHOST;

#if !DO_HOST_LOOKUP
	host_lookup = 0;
#endif

#ifdef HAVE_LINUX_VM_SOCKETS_H
	if (addr->ss_family == AF_VSOCK) {
		struct sockaddr_vm *vsockaddr = (void *)addr;

		snprintf(host, NI_MAXHOST, "%u%%vsock", vsockaddr->svm_cid);
		if (ret_host)
			*ret_host = m_strdup(host);
		snprintf(serv, NI_MAXSERV, "%u", vsockaddr->svm_port);
		if (ret_port)
			*ret_port = m_strdup(serv);

		return;
	}
#endif

	if (host_lookup) {
		flags = NI_NUMERICSERV;
	}

	len = sizeof(struct sockaddr_storage);
	/* Some platforms such as Solaris 8 require that len is the length
	 * of the specific structure. Some older linux systems (glibc 2.1.3
	 * such as debian potato) have sockaddr_storage.__ss_family instead
	 * but we'll ignore them */
#ifdef HAVE_STRUCT_SOCKADDR_STORAGE_SS_FAMILY
	if (addr->ss_family == AF_INET) {
		len = sizeof(struct sockaddr_in);
	}
#ifdef AF_INET6
	if (addr->ss_family == AF_INET6) {
		len = sizeof(struct sockaddr_in6);
	}
#endif
#endif

	ret = getnameinfo((struct sockaddr*)addr, len, host, sizeof(host)-1, 
			serv, sizeof(serv)-1, flags);

	if (ret != 0) {
		if (host_lookup) {
			/* On some systems (Darwin does it) we get EINTR from getnameinfo
			 * somehow. Eew. So we'll just return the IP, since that doesn't seem
			 * to exhibit that behaviour. */
			getaddrstring(addr, ret_host, ret_port, 0);
			return;
		} else {
			/* if we can't do a numeric lookup, something's gone terribly wrong */
			dropbear_exit("Failed lookup: %s", gai_strerror(ret));
		}
	}

	if (ret_host) {
		*ret_host = m_strdup(host);
	}
	if (ret_port) {
		*ret_port = m_strdup(serv);
	}
}

