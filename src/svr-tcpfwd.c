/*
 * Dropbear SSH
 * 
 * Copyright (c) 2002,2003 Matt Johnston
 * Copyright (c) 2004 by Mihnea Stoenescu
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE. */

#include "includes.h"
#include "ssh.h"
#include "tcpfwd.h"
#include "dbutil.h"
#include "session.h"
#include "buffer.h"
#include "packet.h"
#include "listener.h"
#include "runopts.h"
#include "auth.h"
#include "netio.h"

#if !DROPBEAR_SVR_REMOTETCPFWD

/* This is better than SSH_MSG_UNIMPLEMENTED */
void recv_msg_global_request_remotetcp() {
	unsigned int wantreply = 0;

	TRACE(("recv_msg_global_request_remotetcp: remote tcp forwarding not compiled in"))

	buf_eatstring(ses.payload);
	wantreply = buf_getbool(ses.payload);
	if (wantreply) {
		send_msg_request_failure();
	}
}

/* */
#endif /* !DROPBEAR_SVR_REMOTETCPFWD */

static int svr_cancelremotetcp(void);
static int svr_remotetcpreq(int *allocated_listen_port);
static int newtcpdirect(struct Channel * channel);
static int newstreamlocal(struct Channel * channel);

#if DROPBEAR_SVR_REMOTETCPFWD
static const struct ChanType svr_chan_tcpremote = {
	"forwarded-tcpip",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

/* At the moment this is completely used for tcp code (with the name reflecting
 * that). If new request types are added, this should be replaced with code
 * similar to the request-switching in chansession.c */
void recv_msg_global_request_remotetcp() {

	char* reqname = NULL;
	unsigned int namelen;
	unsigned int wantreply = 0;
	int ret = DROPBEAR_FAILURE;

	TRACE(("enter recv_msg_global_request_remotetcp"))

	reqname = buf_getstring(ses.payload, &namelen);
	wantreply = buf_getbool(ses.payload);

	if (svr_opts.noremotetcp || !svr_pubkey_allows_tcpfwd()) {
		TRACE(("leave recv_msg_global_request_remotetcp: remote tcp forwarding disabled"))
		goto out;
	}

	if (namelen > MAX_NAME_LEN) {
		TRACE(("name len is wrong: %d", namelen))
		goto out;
	}

	if (strcmp("tcpip-forward", reqname) == 0) {
		int allocated_listen_port = 0;
		ret = svr_remotetcpreq(&allocated_listen_port);
		/* client expects-port-number-to-make-use-of-server-allocated-ports */
		if (DROPBEAR_SUCCESS == ret) {
			CHECKCLEARTOWRITE();
			buf_putbyte(ses.writepayload, SSH_MSG_REQUEST_SUCCESS);
			buf_putint(ses.writepayload, allocated_listen_port);
			encrypt_packet();
			wantreply = 0; /* avoid out: below sending another reply */
		}
	} else if (strcmp("cancel-tcpip-forward", reqname) == 0) {
		ret = svr_cancelremotetcp();
	} else {
		TRACE(("reqname isn't tcpip-forward: '%s'", reqname))
	}

out:
	if (wantreply) {
		if (ret == DROPBEAR_SUCCESS) {
			send_msg_request_success();
		} else {
			send_msg_request_failure();
		}
	}

	m_free(reqname);

	TRACE(("leave recv_msg_global_request"))
}

static int matchtcp(const void* typedata1, const void* typedata2) {

	const struct TCPListener *info1 = (struct TCPListener*)typedata1;
	const struct TCPListener *info2 = (struct TCPListener*)typedata2;

	return (info1->listenport == info2->listenport)
			&& (info1->chantype == info2->chantype)
			&& (strcmp(info1->listenaddr, info2->listenaddr) == 0);
}

static int svr_cancelremotetcp() {

	int ret = DROPBEAR_FAILURE;
	char * bindaddr = NULL;
	unsigned int addrlen;
	unsigned int port;
	struct Listener * listener = NULL;
	struct TCPListener tcpinfo;

	TRACE(("enter cancelremotetcp"))

	bindaddr = buf_getstring(ses.payload, &addrlen);
	if (addrlen > MAX_HOST_LEN) {
		TRACE(("addr len too long: %d", addrlen))
		goto out;
	}

	port = buf_getint(ses.payload);

	tcpinfo.sendaddr = NULL;
	tcpinfo.sendport = 0;
	tcpinfo.listenaddr = bindaddr;
	tcpinfo.listenport = port;
	listener = get_listener(CHANNEL_ID_TCPFORWARDED, &tcpinfo, matchtcp);
	if (listener) {
		remove_listener( listener );
		ret = DROPBEAR_SUCCESS;
	}

out:
	m_free(bindaddr);
	TRACE(("leave cancelremotetcp"))
	return ret;
}

static int svr_remotetcpreq(int *allocated_listen_port) {

	int ret = DROPBEAR_FAILURE;
	char * request_addr = NULL;
	unsigned int addrlen;
	struct TCPListener *tcpinfo = NULL;
	unsigned int port;
	struct Listener *listener = NULL;

	TRACE(("enter remotetcpreq"))

	request_addr = buf_getstring(ses.payload, &addrlen);
	if (addrlen > MAX_HOST_LEN) {
		TRACE(("addr len too long: %d", addrlen))
		goto out;
	}

	port = buf_getint(ses.payload);

	if (port != 0) {
		if (port < 1 || port > 65535) {
			TRACE(("invalid port: %d", port))
			goto out;
		}

		if (!ses.allowprivport && port < IPPORT_RESERVED) {
			TRACE(("can't assign port < 1024 for non-root"))
			goto out;
		}
	}

	tcpinfo = (struct TCPListener*)m_malloc(sizeof(struct TCPListener));
	tcpinfo->sendaddr = NULL;
	tcpinfo->sendport = 0;
	tcpinfo->listenport = port;
	tcpinfo->chantype = &svr_chan_tcpremote;
	tcpinfo->tcp_type = forwarded;
	tcpinfo->interface = svr_opts.interface;

	tcpinfo->request_listenaddr = request_addr;
	if (!opts.listen_fwd_all || (strcmp(request_addr, "localhost") == 0) ) {
		/* NULL means "localhost only" */
		tcpinfo->listenaddr = NULL;
	}
	else
	{
		tcpinfo->listenaddr = m_strdup(request_addr);
	}

	ret = listen_tcpfwd(tcpinfo, &listener);
	if (DROPBEAR_SUCCESS == ret) {
		tcpinfo->listenport = get_sock_port(listener->socks[0]);
		*allocated_listen_port = tcpinfo->listenport;
	}

out:
	if (ret == DROPBEAR_FAILURE) {
		/* we only free it if a listener wasn't created, since the listener
		 * has to remember it if it's to be cancelled */
		m_free(request_addr);
		m_free(tcpinfo);
	}

	TRACE(("leave remotetcpreq"))

	return ret;
}

#endif /* DROPBEAR_SVR_REMOTETCPFWD */

#if DROPBEAR_SVR_LOCALTCPFWD

const struct ChanType svr_chan_tcpdirect = {
	"direct-tcpip",
	newtcpdirect, /* init */
	NULL, /* checkclose */
	NULL, /* reqhandler */
	NULL, /* closehandler */
	NULL /* cleanup */
};

/* Called upon creating a new direct tcp channel (ie we connect out to an
 * address */
static int newtcpdirect(struct Channel * channel) {

	char* desthost = NULL;
	unsigned int destport;
	char* orighost = NULL;
	unsigned int origport;
	char portstring[NI_MAXSERV];
	unsigned int len;
	int err = SSH_OPEN_ADMINISTRATIVELY_PROHIBITED;

	TRACE(("newtcpdirect channel %d", channel->index))

	if (svr_opts.nolocaltcp || !svr_pubkey_allows_tcpfwd()) {
		TRACE(("leave newtcpdirect: local tcp forwarding disabled"))
		goto out;
	}

	desthost = buf_getstring(ses.payload, &len);
	if (len > MAX_HOST_LEN) {
		TRACE(("leave newtcpdirect: desthost too long"))
		goto out;
	}

	destport = buf_getint(ses.payload);
	
	orighost = buf_getstring(ses.payload, &len);
	if (len > MAX_HOST_LEN) {
		TRACE(("leave newtcpdirect: orighost too long"))
		goto out;
	}

	origport = buf_getint(ses.payload);

	/* best be sure */
#ifdef HAVE_LINUX_VM_SOCKETS_H
	if (strstr(orighost, "%vsock") != NULL) {} else
#endif
	if (origport > 65535 || destport > 65535) {
		TRACE(("leave newtcpdirect: port > 65535"))
		goto out;
	}

	if (!svr_pubkey_allows_local_tcpfwd(desthost, destport)) {
		TRACE(("leave newtcpdirect: local tcp forwarding not permitted to requested destination"));
		goto out;
	}

	snprintf(portstring, sizeof(portstring), "%u", destport);
	channel->conn_pending = connect_remote(desthost, portstring, channel_connect_done,
		channel, NULL, NULL, DROPBEAR_PRIO_NORMAL);

	err = SSH_OPEN_IN_PROGRESS;

out:
	m_free(desthost);
	m_free(orighost);
	TRACE(("leave newtcpdirect: err %d", err))
	return err;
}

#endif /* DROPBEAR_SVR_LOCALTCPFWD */


#if DROPBEAR_SVR_LOCALSTREAMFWD

const struct ChanType svr_chan_streamlocal = {
	"direct-streamlocal@openssh.com",
	newstreamlocal, /* init */
	NULL, /* checkclose */
	NULL, /* reqhandler */
	NULL, /* closehandler */
	NULL /* cleanup */
};

/* Called upon creating a new stream local channel (ie we connect out to an
 * address */
static int newstreamlocal(struct Channel * channel) {

	/*
	https://cvsweb.openbsd.org/cgi-bin/cvsweb/src/usr.bin/ssh/PROTOCOL#rev1.30

	byte		SSH_MSG_CHANNEL_OPEN
	string		"direct-streamlocal@openssh.com"
	uint32		sender channel
	uint32		initial window size
	uint32		maximum packet size
	string		socket path
	string		reserved
	uint32		reserved
	*/

	char* destsocket = NULL;
	unsigned int len;
	int err = SSH_OPEN_ADMINISTRATIVELY_PROHIBITED;

	TRACE(("streamlocal channel %d", channel->index))

	if (svr_opts.nolocaltcp || !svr_pubkey_allows_tcpfwd()) {
		TRACE(("leave newstreamlocal: local unix forwarding disabled"))
		goto out;
	}

	destsocket = buf_getstring(ses.payload, &len);
	if (len > MAX_HOST_LEN) {
		TRACE(("leave streamlocal: destsocket too long"))
		goto out;
	}

	channel->conn_pending = connect_streamlocal(destsocket, channel_connect_done,
		channel, DROPBEAR_PRIO_NORMAL);

	err = SSH_OPEN_IN_PROGRESS;

out:
	m_free(destsocket);
	TRACE(("leave streamlocal: err %d", err))
	return err;
}

#endif /* DROPBEAR_SVR_LOCALSTREAMFWD */
