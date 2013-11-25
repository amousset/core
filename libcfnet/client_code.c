/*
   Copyright (C) CFEngine AS

   This file is part of CFEngine 3 - written and maintained by CFEngine AS.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the
   Free Software Foundation; version 3.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA

  To the extent this program is licensed as part of the Enterprise
  versions of CFEngine, the applicable Commercial Open Source License
  (COSL) may apply to this file if you as a licensee so wish it. See
  included file COSL.txt.
*/

#include <cfnet.h>                                 /* struct ConnectionInfo */
#include <client_code.h>
#include <communication.h>
#include <connection_info.h>
#include <classic.h>                  /* RecvSocketStream */
#include <net.h>                      /* SendTransaction,ReceiveTransaction */
#include <tls_client.h>               /* TLSTry */
#include <tls_generic.h>              /* TLSVerifyPeer */
#include <dir.h>
#include <unix.h>
#include <dir_priv.h>
#include <client_protocol.h>
#include <crypto.h>
#include <logging.h>
#include <files_hashes.h>
#include <files_copy.h>
#include <mutex.h>
#include <rlist.h>
#include <policy.h>
#include <item_lib.h>
#include <files_lib.h>
#include <string_lib.h>
#include <misc_lib.h>                                   /* ProgrammingError */

#include <lastseen.h>                                           /* LastSaw */


typedef struct
{
    char *server;
    AgentConnection *conn;
    int busy;
} ServerItem;

#define CFENGINE_SERVICE "cfengine"

#define RECVTIMEOUT 30 /* seconds */

#define CF_COULD_NOT_CONNECT -2

/* With this lock we ensure we read the list head atomically, but we don't
 * guarantee anything about the queue's contents. It should be OK since we
 * never remove elements from the queue, only prepend to the head.*/
static pthread_mutex_t cft_serverlist = PTHREAD_ERRORCHECK_MUTEX_INITIALIZER_NP;

static void NewClientCache(Stat *data, AgentConnection *conn);
static void CacheServerConnection(AgentConnection *conn, const char *server);
static void MarkServerOffline(const char *server);
static AgentConnection *GetIdleConnectionToServer(const char *server);
static bool ServerOffline(const char *server);
static void FlushFileStream(int sd, int toget);
static int CacheStat(const char *file, struct stat *statbuf, const char *stattype, AgentConnection *conn);
/**
  @param err Set to 0 on success, -1 no server responce, -2 authentication failure.
  */
static AgentConnection *ServerConnection(const char *server, FileCopy fc, int *err, int s);

int TryConnect(AgentConnection *conn, struct timeval *tvp, struct sockaddr *cinp, int cinpSz);


ProtocolVersion SELECTED_PROTOCOL = CF_PROTOCOL_TLS; /* TODO command line / body common control policy option */


/**
 * Initialize client's network library.
 */
bool cfnet_init()
{
    CryptoInitialize();

    if (TLSClientInitialize())
        return true;
    else
        return false;
}

static Seq *GetGlobalServerList(void)
{
    /* Only ip address strings are stored in this list, so don't put any
     * hostnames. TODO convert to list of (sockaddr_storage *) to enforce this. */
    static Seq *server_list = NULL;
    if (!server_list)
    {
        server_list = SeqNew(100, free);
    }
    return server_list;
}

static int FSWrite(const char *destination, int dd, const char *buf, size_t n_write)
{
    const void *cur = buf;
    const void *end = buf + n_write;

    while (cur < end)
    {
        const void *skip_span = MemSpan(cur, 0, end - cur);
        if (skip_span > cur)
        {
            if (lseek(dd, skip_span - cur, SEEK_CUR) < 0)
            {
                Log(LOG_LEVEL_ERR, "Copy failed (no space?) while copying to '%s' from network '%s'", destination, GetErrorStr());
                return false;
            }

            cur = skip_span;
        }

        const void *copy_span = MemSpanInverse(cur, 0, end - cur);
        if (copy_span > cur)
        {
            if (FullWrite(dd, cur, copy_span - cur) < 0)
            {
                Log(LOG_LEVEL_ERR, "Copy failed (no space?) while copying to '%s' from network '%s'", destination, GetErrorStr());
                return false;
            }

            cur = copy_span;
        }
    }

    return true;
}

void DetermineCfenginePort()
{
    struct servent *server;

    errno = 0;
    if ((server = getservbyname(CFENGINE_SERVICE, "tcp")) == NULL)
    {
        if (errno == 0)
        {
            Log(LOG_LEVEL_VERBOSE, "No registered cfengine service, using default");
        }
        else
        {
            Log(LOG_LEVEL_VERBOSE, "Unable to query services database, using default. (getservbyname: %s)", GetErrorStr());
        }
        snprintf(STR_CFENGINEPORT, 15, "5308");
        SHORT_CFENGINEPORT = htons((unsigned short) 5308);
    }
    else
    {
        snprintf(STR_CFENGINEPORT, 15, "%u", ntohs(server->s_port));
        SHORT_CFENGINEPORT = server->s_port;
    }

    Log(LOG_LEVEL_VERBOSE, "Setting cfengine default port to %u, '%s'", ntohs(SHORT_CFENGINEPORT), STR_CFENGINEPORT);
}

/*********************************************************************/

AgentConnection *NewServerConnection(FileCopy fc, bool background, int *err, int s)
{
    AgentConnection *conn = NULL;
    Rlist *rp = NULL;

    for (rp = fc.servers; rp != NULL; rp = rp->next)
    {
        const char *servername = RlistScalarValue(rp);

        if (ServerOffline(servername))
        {
            continue;
        }

        if (background)
        {
            ThreadLock(&cft_serverlist);
            Seq *srvlist_tmp = GetGlobalServerList();
            ThreadUnlock(&cft_serverlist);

            /* TODO not return NULL if >= CFA_MAXTREADS ? */
            /* TODO RlistLen is O(n) operation. */
            if (SeqLength(srvlist_tmp) < CFA_MAXTHREADS)
            {
                /* If background connection was requested, then don't cache it
                 * in SERVERLIST since it will be closed right afterwards. */
                conn = ServerConnection(servername, fc, err, s);
                return conn;
            }
        }
        else
        {
            conn = GetIdleConnectionToServer(servername);
            if (conn != NULL)
            {
                *err = 0;
                return conn;
            }

            /* This is first usage, need to open */
            conn = ServerConnection(servername, fc, err, s);
            if (conn != NULL)
            {
                CacheServerConnection(conn, servername);
                return conn;
            }

            /* This server failed, trying next in list. */
            Log(LOG_LEVEL_INFO, "Unable to establish connection with %s",
                servername);
            MarkServerOffline(servername);
        }
    }

    Log(LOG_LEVEL_ERR, "Unable to establish any connection with server.");
    return NULL;
}

/**
 * @return 1 success, 0 auth/ID error, -1 other error
 */
int TLSConnect(ConnectionInfo *conn_info, bool trust_server,
               const char *ipaddr, const char *username)
{
    int ret;

    ret = TLSTry(conn_info);
    if (ret == -1)
    {
        return -1;
    }

    /* TODO fix, we identify hub user with our own username, because
     * we store key filenames as "user-key.pub" and we need a
     * username. We might as well hard-code root... */
    ret = TLSVerifyPeer(conn_info, ipaddr, username);

    if (ret == -1)                                      /* error */
    {
        return -1;
    }

    if (ret == 1)
    {
        Log(LOG_LEVEL_INFO,
            "Server is TRUSTED, received key %s MATCHES stored one.",
            ConnectionInfoPrintableKeyHash(conn_info));
    }
    else   /* ret == 0 */
    {
        Log(LOG_LEVEL_WARNING, "%s: Server's public key is UNKNOWN!",
            ConnectionInfoPrintableKeyHash(conn_info));

        if (trust_server)             /* We're most probably bootstrapping. */
        {
            Log(LOG_LEVEL_WARNING,
                "%s: Explicitly trusting this key from now on.",
                ConnectionInfoPrintableKeyHash(conn_info));
            SavePublicKey(username, ConnectionInfoPrintableKeyHash(conn_info),
                          KeyRSA(ConnectionInfoKey(conn_info)));
        }
        else
        {
            Log(LOG_LEVEL_ERR, "TRUST FAILED, WARNING: possible MAN IN THE MIDDLE attack!");
            Log(LOG_LEVEL_ERR, "Rebootstrap the client if you really want to start trusting this new key.");
            return -1;
        }
    }

    /* TLS CONNECTION IS ESTABLISHED, negotiate protocol version. */
    ret = TLSClientNegotiateProtocol(conn_info);
    if (ret <= 0)
    {
        return -1;
    }

    /* We continue by sending identification data. */
    ret = TLSClientSendIdentity(conn_info, username);
    if (ret == -1)
    {
        return -1;
    }

    /* Server might hang up here, after we sent identification! We
     * must get the "OK WELCOME" message for everything to be OK. */
    char line[1024] = "";
    ret = TLSRecvLine(ConnectionInfoSSL(conn_info), line, sizeof(line));
    if (ret <= 0 ||
        (strncmp(line, "OK WELCOME", strlen("OK WELCOME")) != 0))
    {
        return 0;
    }

    return 1;
}

/*****************************************************************************/

static AgentConnection *ServerConnection(const char *server, FileCopy fc, int *err, int s)
{
    AgentConnection *conn = NULL;
    int ret;
    *err = 0;

#if !defined(__MINGW32__)
    signal(SIGPIPE, SIG_IGN);
#endif /* !__MINGW32__ */

#if !defined(__MINGW32__)
    static sigset_t signal_mask;
    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &signal_mask, NULL);
#endif

    conn = NewAgentConn(server, false);

/* username of the client - say root from Windows */

#ifdef __MINGW32__
    snprintf(conn->username, CF_SMALLBUF, "root");
#else
    /* FIXME: username is local */
    GetCurrentUserName(conn->username, CF_SMALLBUF);
#endif /* !__MINGW32__ */

    /* TODO fix, this was supposed to check if the connection is cached (open
     * or unreachable). However conn was just alloc'd so it's always INVALID */
    if (ConnectionInfoSocket(conn->conn_info) == SOCKET_INVALID)
    {
        if (-1 == s)
        {
            if (!ServerConnect(conn, server, fc))
            {
                Log(LOG_LEVEL_INFO, "No server is responding on this port");
                DisconnectServer(conn, false);
                *err = -1;
                return NULL;
            }

            if (ConnectionInfoSocket(conn->conn_info) < 0)                      /* INVALID or OFFLINE socket */
            {
                UnexpectedError("ServerConnect() succeeded but socket descriptor is %d!",
                                ConnectionInfoSocket(conn->conn_info));
                *err = -1;
                return NULL;
            }
        }
        else
        {
            /*
             * In this mode the connection is already opened, most likely
             * because of Call Collect.
             * We don't need to connect to the server, we just need to populate
             * the required structures.
             */
        }

        switch (SELECTED_PROTOCOL)
        {
        case CF_PROTOCOL_TLS:

            ret = TLSConnect(conn->conn_info, fc.trustkey,
                             conn->remoteip, conn->username);

            if (ret == -1)                                      /* Error */
            {
                DisconnectServer(conn, false);
                *err = -1;
                return NULL;
            }
            else if (ret == 0)                             /* Auth/ID error */
            {
                    DisconnectServer(conn, false);
                    errno = EPERM;
                    *err = -2;
                    return NULL;
            }
            assert(ret == 1);
            ConnectionInfoSetProtocolVersion(conn->conn_info, CF_PROTOCOL_TLS);
            ConnectionInfoSetConnectionStatus(conn->conn_info, CF_CONNECTION_ESTABLISHED);
            LastSaw1(conn->remoteip, ConnectionInfoPrintableKeyHash(conn->conn_info),
                     LAST_SEEN_ROLE_CONNECT);
            break;

        case CF_PROTOCOL_CLASSIC:

            ConnectionInfoSetProtocolVersion(conn->conn_info, CF_PROTOCOL_CLASSIC);
            conn->encryption_type = CfEnterpriseOptions();

            if (!IdentifyAgent(conn->conn_info))
            {
                Log(LOG_LEVEL_ERR, "Id-authentication for '%s' failed", VFQNAME);
                errno = EPERM;
                DisconnectServer(conn, false);
                *err = -2; // auth err
                return NULL;
            }

            if (!AuthenticateAgent(conn, fc.trustkey))
            {
                Log(LOG_LEVEL_ERR, "Authentication dialogue with '%s' failed", server);
                errno = EPERM;
                DisconnectServer(conn, false);
                *err = -2; // auth err
                return NULL;
            }
            ConnectionInfoSetConnectionStatus(conn->conn_info, CF_CONNECTION_ESTABLISHED);
            break;

        default:
            ProgrammingError("ServerConnection: ProtocolVersion %d!",
                             SELECTED_PROTOCOL);
        }
        conn->authenticated = true;
    }

    return conn;
}

/*********************************************************************/

void DisconnectServer(AgentConnection *conn, int partial)
{
    if (!partial)
    {
        /* Socket needs to be closed even after SSL_shutdown. */
        if (ConnectionInfoSocket(conn->conn_info) >= 0)                  /* Not INVALID or OFFLINE */
        {
            if (ConnectionInfoProtocolVersion(conn->conn_info) == CF_PROTOCOL_TLS &&
                ConnectionInfoSSL(conn->conn_info) != NULL)
            {
                SSL_shutdown(ConnectionInfoSSL(conn->conn_info));
            }

            cf_closesocket(ConnectionInfoSocket(conn->conn_info));
            Log(LOG_LEVEL_INFO, "Connection to %s is closed", conn->remoteip);
        }
    }
    DeleteAgentConn(conn, partial);
}

/*********************************************************************/

int cf_remote_stat(char *file, struct stat *buf, char *stattype, bool encrypt, AgentConnection *conn)
/* If a link, this reads readlink and sends it back in the same
   package. It then caches the value for each copy command */
{
    char sendbuffer[CF_BUFSIZE];
    char recvbuffer[CF_BUFSIZE];
    char in[CF_BUFSIZE], out[CF_BUFSIZE];
    int ret, tosend, cipherlen;
    time_t tloc;

    memset(recvbuffer, 0, CF_BUFSIZE);

    if (strlen(file) > CF_BUFSIZE - 30)
    {
        Log(LOG_LEVEL_ERR, "Filename too long");
        return -1;
    }

    ret = CacheStat(file, buf, stattype, conn);

    if (ret != 1)
    {
        return ret;
    }

    if ((tloc = time((time_t *) NULL)) == -1)
    {
        Log(LOG_LEVEL_ERR, "Couldn't read system clock");
    }

    sendbuffer[0] = '\0';

    /* We encrypt only for CLASSIC protocol. The TLS protocol is always over
     * encrypted layer, so it does not support encrypted (S*) commands. */
    encrypt = encrypt && (ConnectionInfoProtocolVersion(conn->conn_info) == CF_PROTOCOL_CLASSIC);

    if (encrypt)
    {
        if (conn->session_key == NULL)
        {
            Log(LOG_LEVEL_ERR, "Cannot do encrypted copy without keys (use cf-key)");
            return -1;
        }

        snprintf(in, CF_BUFSIZE - 1, "SYNCH %jd STAT %s", (intmax_t) tloc, file);
        cipherlen = EncryptString(conn->encryption_type, in, out, conn->session_key, strlen(in) + 1);
        snprintf(sendbuffer, CF_BUFSIZE - 1, "SSYNCH %d", cipherlen);
        memcpy(sendbuffer + CF_PROTO_OFFSET, out, cipherlen);
        tosend = cipherlen + CF_PROTO_OFFSET;
    }
    else
    {
        snprintf(sendbuffer, CF_BUFSIZE, "SYNCH %jd STAT %s", (intmax_t) tloc, file);
        tosend = strlen(sendbuffer);
    }

    if (SendTransaction(conn->conn_info, sendbuffer, tosend, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_INFO, "Transmission failed/refused talking to %.255s:%.255s. (stat: %s)",
            conn->this_server, file, GetErrorStr());
        return -1;
    }

    if (ReceiveTransaction(conn->conn_info, recvbuffer, NULL) == -1)
    {
        return -1;
    }

    if (strstr(recvbuffer, "unsynchronized"))
    {
        Log(LOG_LEVEL_ERR, "Clocks differ too much to do copy by date (security) '%s'", recvbuffer + 4);
        return -1;
    }

    if (BadProtoReply(recvbuffer))
    {
        Log(LOG_LEVEL_VERBOSE, "Server returned error '%s'", recvbuffer + 4);
        errno = EPERM;
        return -1;
    }

    if (OKProtoReply(recvbuffer))
    {
        Stat cfst;

        // use intmax_t here to provide enough space for large values coming over the protocol
        intmax_t d1, d2, d3, d4, d5, d6, d7, d8, d9, d10, d11, d12 = 0, d13 = 0;
        ret = sscanf(recvbuffer, "OK: "
               "%1" PRIdMAX     // 01 cfst.cf_type
               " %5" PRIdMAX    // 02 cfst.cf_mode
               " %14" PRIdMAX   // 03 cfst.cf_lmode
               " %14" PRIdMAX   // 04 cfst.cf_uid
               " %14" PRIdMAX   // 05 cfst.cf_gid
               " %18" PRIdMAX   // 06 cfst.cf_size
               " %14" PRIdMAX   // 07 cfst.cf_atime
               " %14" PRIdMAX   // 08 cfst.cf_mtime
               " %14" PRIdMAX   // 09 cfst.cf_ctime
               " %1" PRIdMAX    // 10 cfst.cf_makeholes
               " %14" PRIdMAX   // 11 cfst.cf_ino
               " %14" PRIdMAX   // 12 cfst.cf_nlink
               " %18" PRIdMAX,  // 13 cfst.cf_dev
               &d1, &d2, &d3, &d4, &d5, &d6, &d7, &d8, &d9, &d10, &d11, &d12, &d13);

        if (ret < 13)
        {
            Log(LOG_LEVEL_ERR, "Cannot read SYNCH reply from '%s', only %d/13 items parsed", conn->remoteip, ret );
            return -1;
        }

        cfst.cf_type = (FileType) d1;
        cfst.cf_mode = (mode_t) d2;
        cfst.cf_lmode = (mode_t) d3;
        cfst.cf_uid = (uid_t) d4;
        cfst.cf_gid = (gid_t) d5;
        cfst.cf_size = (off_t) d6;
        cfst.cf_atime = (time_t) d7;
        cfst.cf_mtime = (time_t) d8;
        cfst.cf_ctime = (time_t) d9;
        cfst.cf_makeholes = (char) d10;
        cfst.cf_ino = d11;
        cfst.cf_nlink = d12;
        cfst.cf_dev = (dev_t)d13;

        /* Use %?d here to avoid memory overflow attacks */

        memset(recvbuffer, 0, CF_BUFSIZE);

        if (ReceiveTransaction(conn->conn_info, recvbuffer, NULL) == -1)
        {
            return -1;
        }

        if (strlen(recvbuffer) > 3)
        {
            cfst.cf_readlink = xstrdup(recvbuffer + 3);
        }
        else
        {
            cfst.cf_readlink = NULL;
        }

        switch (cfst.cf_type)
        {
        case FILE_TYPE_REGULAR:
            cfst.cf_mode |= (mode_t) S_IFREG;
            break;
        case FILE_TYPE_DIR:
            cfst.cf_mode |= (mode_t) S_IFDIR;
            break;
        case FILE_TYPE_CHAR_:
            cfst.cf_mode |= (mode_t) S_IFCHR;
            break;
        case FILE_TYPE_FIFO:
            cfst.cf_mode |= (mode_t) S_IFIFO;
            break;
        case FILE_TYPE_SOCK:
            cfst.cf_mode |= (mode_t) S_IFSOCK;
            break;
        case FILE_TYPE_BLOCK:
            cfst.cf_mode |= (mode_t) S_IFBLK;
            break;
        case FILE_TYPE_LINK:
            cfst.cf_mode |= (mode_t) S_IFLNK;
            break;
        }

        cfst.cf_filename = xstrdup(file);
        cfst.cf_server = xstrdup(conn->this_server);
        cfst.cf_failed = false;

        if (cfst.cf_lmode != 0)
        {
            cfst.cf_lmode |= (mode_t) S_IFLNK;
        }

        NewClientCache(&cfst, conn);

        if ((cfst.cf_lmode != 0) && (strcmp(stattype, "link") == 0))
        {
            buf->st_mode = cfst.cf_lmode;
        }
        else
        {
            buf->st_mode = cfst.cf_mode;
        }

        buf->st_uid = cfst.cf_uid;
        buf->st_gid = cfst.cf_gid;
        buf->st_size = cfst.cf_size;
        buf->st_mtime = cfst.cf_mtime;
        buf->st_ctime = cfst.cf_ctime;
        buf->st_atime = cfst.cf_atime;
        buf->st_ino = cfst.cf_ino;
        buf->st_dev = cfst.cf_dev;
        buf->st_nlink = cfst.cf_nlink;

        return 0;
    }

    Log(LOG_LEVEL_ERR, "Transmission refused or failed statting '%s', got '%s'", file, recvbuffer);
    errno = EPERM;
    return -1;
}

/*********************************************************************/

Item *RemoteDirList(const char *dirname, bool encrypt, AgentConnection *conn)
{
    char sendbuffer[CF_BUFSIZE];
    char recvbuffer[CF_BUFSIZE];
    char in[CF_BUFSIZE];
    char out[CF_BUFSIZE];
    int n, cipherlen = 0, tosend;
    char *sp;
    Item *files = NULL;
    Item *ret = NULL;

    if (strlen(dirname) > CF_BUFSIZE - 20)
    {
        Log(LOG_LEVEL_ERR, "Directory name too long");
        return NULL;
    }

    /* We encrypt only for CLASSIC protocol. The TLS protocol is always over
     * encrypted layer, so it does not support encrypted (S*) commands. */
    encrypt = encrypt && (ConnectionInfoProtocolVersion(conn->conn_info) == CF_PROTOCOL_CLASSIC);

    if (encrypt)
    {
        if (conn->session_key == NULL)
        {
            Log(LOG_LEVEL_ERR, "Cannot do encrypted copy without keys (use cf-key)");
            return NULL;
        }

        snprintf(in, CF_BUFSIZE, "OPENDIR %s", dirname);
        cipherlen = EncryptString(conn->encryption_type, in, out, conn->session_key, strlen(in) + 1);
        snprintf(sendbuffer, CF_BUFSIZE - 1, "SOPENDIR %d", cipherlen);
        memcpy(sendbuffer + CF_PROTO_OFFSET, out, cipherlen);
        tosend = cipherlen + CF_PROTO_OFFSET;
    }
    else
    {
        snprintf(sendbuffer, CF_BUFSIZE, "OPENDIR %s", dirname);
        tosend = strlen(sendbuffer);
    }

    if (SendTransaction(conn->conn_info, sendbuffer, tosend, CF_DONE) == -1)
    {
        return NULL;
    }

    while (true)
    {
        if ((n = ReceiveTransaction(conn->conn_info, recvbuffer, NULL)) == -1)
        {
            return NULL;
        }

        if (n == 0)
        {
            break;
        }

        if (encrypt)
        {
            memcpy(in, recvbuffer, n);
            DecryptString(conn->encryption_type, in, recvbuffer, conn->session_key, n);
        }

        if (FailedProtoReply(recvbuffer))
        {
            Log(LOG_LEVEL_INFO, "Network access to '%s:%s' denied", conn->this_server, dirname);
            return NULL;
        }

        if (BadProtoReply(recvbuffer))
        {
            Log(LOG_LEVEL_INFO, "%s", recvbuffer + 4);
            return NULL;
        }

        for (sp = recvbuffer; *sp != '\0'; sp++)
        {
            Item *ip;

            if (strncmp(sp, CFD_TERMINATOR, strlen(CFD_TERMINATOR)) == 0)       /* End transmission */
            {
                return ret;
            }

            ip = xcalloc(1, sizeof(Item));
            ip->name = (char *) AllocateDirentForFilename(sp);

            if (files == NULL)  /* First element */
            {
                ret = ip;
                files = ip;
            }
            else
            {
                files->next = ip;
                files = ip;
            }

            while (*sp != '\0')
            {
                sp++;
            }
        }
    }

    return ret;
}

/*********************************************************************/

static void NewClientCache(Stat *data, AgentConnection *conn)
{
    Stat *sp = xmemdup(data, sizeof(Stat));
    sp->next = conn->cache;
    conn->cache = sp;
}

const Stat *ClientCacheLookup(AgentConnection *conn, const char *server_name, const char *file_name)
{
    for (const Stat *sp = conn->cache; sp != NULL; sp = sp->next)
    {
        if (strcmp(server_name, sp->cf_server) == 0 && strcmp(file_name, sp->cf_filename) == 0)
        {
            return sp;
        }
    }

    return NULL;
}

int CompareHashNet(const char *file1, const char *file2, bool encrypt, AgentConnection *conn)
{
    static unsigned char d[EVP_MAX_MD_SIZE + 1];
    char *sp, sendbuffer[CF_BUFSIZE], recvbuffer[CF_BUFSIZE], in[CF_BUFSIZE], out[CF_BUFSIZE];
    int i, tosend, cipherlen;

    HashFile(file2, d, CF_DEFAULT_DIGEST);

    memset(recvbuffer, 0, CF_BUFSIZE);

    /* We encrypt only for CLASSIC protocol. The TLS protocol is always over
     * encrypted layer, so it does not support encrypted (S*) commands. */
    encrypt = encrypt && (ConnectionInfoProtocolVersion(conn->conn_info) == CF_PROTOCOL_CLASSIC);

    if (encrypt)
    {
        snprintf(in, CF_BUFSIZE, "MD5 %s", file1);

        sp = in + strlen(in) + CF_SMALL_OFFSET;

        for (i = 0; i < CF_DEFAULT_DIGEST_LEN; i++)
        {
            *sp++ = d[i];
        }

        cipherlen =
            EncryptString(conn->encryption_type, in, out, conn->session_key,
                          strlen(in) + CF_SMALL_OFFSET + CF_DEFAULT_DIGEST_LEN);
        snprintf(sendbuffer, CF_BUFSIZE, "SMD5 %d", cipherlen);
        memcpy(sendbuffer + CF_PROTO_OFFSET, out, cipherlen);
        tosend = cipherlen + CF_PROTO_OFFSET;
    }
    else
    {
        snprintf(sendbuffer, CF_BUFSIZE, "MD5 %s", file1);
        sp = sendbuffer + strlen(sendbuffer) + CF_SMALL_OFFSET;

        for (i = 0; i < CF_DEFAULT_DIGEST_LEN; i++)
        {
            *sp++ = d[i];
        }

        tosend = strlen(sendbuffer) + CF_SMALL_OFFSET + CF_DEFAULT_DIGEST_LEN;
    }

    if (SendTransaction(conn->conn_info, sendbuffer, tosend, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_ERR, "Failed send. (SendTransaction: %s)", GetErrorStr());
        return false;
    }

    if (ReceiveTransaction(conn->conn_info, recvbuffer, NULL) == -1)
    {
        Log(LOG_LEVEL_ERR, "Failed receive. (ReceiveTransaction: %s)", GetErrorStr());
        Log(LOG_LEVEL_VERBOSE,  "No answer from host, assuming checksum ok to avoid remote copy for now...");
        return false;
    }

    if (strcmp(CFD_TRUE, recvbuffer) == 0)
    {
        return true;            /* mismatch */
    }
    else
    {
        return false;
    }

/* Not reached */
}

/*********************************************************************/

int EncryptCopyRegularFileNet(const char *source, const char *dest, off_t size, AgentConnection *conn)
{
    int dd, blocksize = 2048, n_read = 0, towrite, plainlen, more = true, finlen, cnt = 0;
    int tosend, cipherlen = 0;
    char *buf, in[CF_BUFSIZE], out[CF_BUFSIZE], workbuf[CF_BUFSIZE], cfchangedstr[265];
    unsigned char iv[32] =
        { 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8 };
    long n_read_total = 0;
    EVP_CIPHER_CTX crypto_ctx;

    snprintf(cfchangedstr, 255, "%s%s", CF_CHANGEDSTR1, CF_CHANGEDSTR2);

    if ((strlen(dest) > CF_BUFSIZE - 20))
    {
        Log(LOG_LEVEL_ERR, "Filename too long");
        return false;
    }

    unlink(dest);                /* To avoid link attacks */

    if ((dd = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL | O_BINARY, 0600)) == -1)
    {
        Log(LOG_LEVEL_ERR,
            "NetCopy to destination '%s:%s' security - failed attempt to exploit a race? (Not copied). (open: %s)",
            conn->this_server, dest, GetErrorStr());
        unlink(dest);
        return false;
    }

    if (size == 0)
    {
        // No sense in copying an empty file
        close(dd);
        return true;
    }

    workbuf[0] = '\0';
    EVP_CIPHER_CTX_init(&crypto_ctx);

    snprintf(in, CF_BUFSIZE - CF_PROTO_OFFSET, "GET dummykey %s", source);
    cipherlen = EncryptString(conn->encryption_type, in, out, conn->session_key, strlen(in) + 1);
    snprintf(workbuf, CF_BUFSIZE, "SGET %4d %4d", cipherlen, blocksize);
    memcpy(workbuf + CF_PROTO_OFFSET, out, cipherlen);
    tosend = cipherlen + CF_PROTO_OFFSET;

/* Send proposition C0 - query */

    if (SendTransaction(conn->conn_info, workbuf, tosend, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_ERR, "Couldn't send data. (SendTransaction: %s)", GetErrorStr());
        close(dd);
        return false;
    }

    buf = xmalloc(CF_BUFSIZE + sizeof(int));

    n_read_total = 0;

    while (more)
    {
        if ((cipherlen = ReceiveTransaction(conn->conn_info, buf, &more)) == -1)
        {
            free(buf);
            return false;
        }

        cnt++;

        /* If the first thing we get is an error message, break. */

        if ((n_read_total == 0) && (strncmp(buf + CF_INBAND_OFFSET, CF_FAILEDSTR, strlen(CF_FAILEDSTR)) == 0))
        {
            Log(LOG_LEVEL_INFO, "Network access to '%s:%s' denied", conn->this_server, source);
            close(dd);
            free(buf);
            return false;
        }

        if (strncmp(buf + CF_INBAND_OFFSET, cfchangedstr, strlen(cfchangedstr)) == 0)
        {
            Log(LOG_LEVEL_INFO, "Source '%s:%s' changed while copying", conn->this_server, source);
            close(dd);
            free(buf);
            return false;
        }

        EVP_DecryptInit_ex(&crypto_ctx, CfengineCipher(CfEnterpriseOptions()), NULL, conn->session_key, iv);

        if (!EVP_DecryptUpdate(&crypto_ctx, workbuf, &plainlen, buf, cipherlen))
        {
            close(dd);
            free(buf);
            return false;
        }

        if (!EVP_DecryptFinal_ex(&crypto_ctx, workbuf + plainlen, &finlen))
        {
            close(dd);
            free(buf);
            return false;
        }

        towrite = n_read = plainlen + finlen;

        n_read_total += n_read;

        if (!FSWrite(dest, dd, workbuf, towrite))
        {
            Log(LOG_LEVEL_ERR, "Local disk write failed copying '%s:%s' to '%s:%s'",
                conn->this_server, source, dest, GetErrorStr());
            if (conn)
            {
                conn->error = true;
            }
            free(buf);
            unlink(dest);
            close(dd);
            EVP_CIPHER_CTX_cleanup(&crypto_ctx);
            return false;
        }
    }

    /* If the file ends with a `hole', something needs to be written at
       the end.  Otherwise the kernel would truncate the file at the end
       of the last write operation. Write a null character and truncate
       it again.  */

    if (ftruncate(dd, n_read_total) < 0)
    {
        Log(LOG_LEVEL_ERR, "Copy failed (no space?) while copying '%s' from network '%s'",
            dest, GetErrorStr());
        free(buf);
        unlink(dest);
        close(dd);
        EVP_CIPHER_CTX_cleanup(&crypto_ctx);
        return false;
    }

    close(dd);
    free(buf);
    EVP_CIPHER_CTX_cleanup(&crypto_ctx);
    return true;
}

int CopyRegularFileNet(const char *source, const char *dest, off_t size, bool encrypt, AgentConnection *conn)
{
    int dd, buf_size, n_read = 0, toget, towrite;
    int done = false, tosend, value;
    char *buf, workbuf[CF_BUFSIZE], cfchangedstr[265];

    off_t n_read_total = 0;
    EVP_CIPHER_CTX crypto_ctx;

    /* We encrypt only for CLASSIC protocol. The TLS protocol is always over
     * encrypted layer, so it does not support encrypted (S*) commands. */
    encrypt = encrypt && (ConnectionInfoProtocolVersion(conn->conn_info) == CF_PROTOCOL_CLASSIC);

    if (encrypt)
    {
        return EncryptCopyRegularFileNet(source, dest, size, conn);
    }

    snprintf(cfchangedstr, 255, "%s%s", CF_CHANGEDSTR1, CF_CHANGEDSTR2);

    if ((strlen(dest) > CF_BUFSIZE - 20))
    {
        Log(LOG_LEVEL_ERR, "Filename too long");
        return false;
    }

    unlink(dest);                /* To avoid link attacks */

    if ((dd = open(dest, O_WRONLY | O_CREAT | O_TRUNC | O_EXCL | O_BINARY, 0600)) == -1)
    {
        Log(LOG_LEVEL_ERR,
            "NetCopy to destination '%s:%s' security - failed attempt to exploit a race? (Not copied) (open: %s)",
            conn->this_server, dest, GetErrorStr());
        unlink(dest);
        return false;
    }

    workbuf[0] = '\0';

    buf_size = 2048;

/* Send proposition C0 */

    snprintf(workbuf, CF_BUFSIZE, "GET %d %s", buf_size, source);
    tosend = strlen(workbuf);

    if (SendTransaction(conn->conn_info, workbuf, tosend, CF_DONE) == -1)
    {
        Log(LOG_LEVEL_ERR, "Couldn't send data");
        close(dd);
        return false;
    }

    buf = xmalloc(CF_BUFSIZE + sizeof(int));    /* Note CF_BUFSIZE not buf_size !! */
    n_read_total = 0;

    Log(LOG_LEVEL_VERBOSE, "Copying remote file '%s:%s', expecting %jd bytes",
          conn->this_server, source, (intmax_t)size);

    while (!done)
    {
        if ((size - n_read_total) >= buf_size)
        {
            toget = towrite = buf_size;
        }
        else if (size != 0)
        {
            towrite = (size - n_read_total);
            toget = towrite;
        }
        else
        {
            toget = towrite = 0;
        }

        /* Stage C1 - receive */
        switch(ConnectionInfoProtocolVersion(conn->conn_info))
        {
        case CF_PROTOCOL_CLASSIC:
            n_read = RecvSocketStream(ConnectionInfoSocket(conn->conn_info), buf, toget);
            break;
        case CF_PROTOCOL_TLS:
            n_read = TLSRecv(ConnectionInfoSSL(conn->conn_info), buf, toget);
            break;
        default:
            UnexpectedError("CopyRegularFileNet: ProtocolVersion %d!",
                            ConnectionInfoProtocolVersion(conn->conn_info));
            n_read = -1;
        }

        if (n_read == -1)
        {
            /* This may happen on race conditions,
             * where the file has shrunk since we asked for its size in SYNCH ... STAT source */

            Log(LOG_LEVEL_ERR, "Error in client-server stream (has %s:%s shrunk?)", conn->this_server, source);
            close(dd);
            free(buf);
            return false;
        }

        /* If the first thing we get is an error message, break. */

        if ((n_read_total == 0) && (strncmp(buf, CF_FAILEDSTR, strlen(CF_FAILEDSTR)) == 0))
        {
            Log(LOG_LEVEL_INFO, "Network access to '%s:%s' denied", conn->this_server, source);
            close(dd);
            free(buf);
            return false;
        }

        if (strncmp(buf, cfchangedstr, strlen(cfchangedstr)) == 0)
        {
            Log(LOG_LEVEL_INFO, "Source '%s:%s' changed while copying", conn->this_server, source);
            close(dd);
            free(buf);
            return false;
        }

        value = -1;

        /* Check for mismatch between encryption here and on server - can lead to misunderstanding */

        sscanf(buf, "t %d", &value);

        if ((value > 0) && (strncmp(buf + CF_INBAND_OFFSET, "BAD: ", 5) == 0))
        {
            Log(LOG_LEVEL_INFO, "Network access to cleartext '%s:%s' denied",
                conn->this_server, source);
            close(dd);
            free(buf);
            return false;
        }

        if (!FSWrite(dest, dd, buf, n_read))
        {
            Log(LOG_LEVEL_ERR, "Local disk write failed copying '%s:%s' to '%s'. (FSWrite: %s)",
                conn->this_server, source, dest, GetErrorStr());
            if (conn)
            {
                conn->error = true;
            }
            free(buf);
            unlink(dest);
            close(dd);
            FlushFileStream(ConnectionInfoSocket(conn->conn_info), size - n_read_total);
            EVP_CIPHER_CTX_cleanup(&crypto_ctx);
            return false;
        }

        n_read_total += towrite;        /* n_read; */

        if (n_read_total >= size)        /* Handle EOF without closing socket */
        {
            done = true;
        }
    }

    /* If the file ends with a `hole', something needs to be written at
       the end.  Otherwise the kernel would truncate the file at the end
       of the last write operation. Write a null character and truncate
       it again.  */

    if (ftruncate(dd, n_read_total) < 0)
    {
        Log(LOG_LEVEL_ERR, "Copy failed (no space?) while copying '%s' from network '%s'",
            dest, GetErrorStr());
        free(buf);
        unlink(dest);
        close(dd);
        FlushFileStream(ConnectionInfoSocket(conn->conn_info), size - n_read_total);
        return false;
    }

    close(dd);
    free(buf);
    return true;
}


/*********************************************************************/
/* Level 2                                                           */
/*********************************************************************/

int ServerConnect(AgentConnection *conn, const char *host, FileCopy fc)
{
    short shortport;
    char strport[CF_MAXVARSIZE] = { 0 };
    struct timeval tv = { 0 };

    if (fc.portnumber == 0)
    {
        shortport = SHORT_CFENGINEPORT;
        strncpy(strport, STR_CFENGINEPORT, CF_MAXVARSIZE);
    }
    else
    {
        shortport = htons(fc.portnumber);
        snprintf(strport, CF_MAXVARSIZE, "%u", (int) fc.portnumber);
    }

    Log(LOG_LEVEL_VERBOSE,
        "Set cfengine port number to '%s' = %hu",
          strport, ntohs(shortport));

    if ((fc.timeout == (short) CF_NOINT) || (fc.timeout <= 0))
    {
        tv.tv_sec = CONNTIMEOUT;
    }
    else
    {
        tv.tv_sec = fc.timeout;
    }

    Log(LOG_LEVEL_VERBOSE, "Set connection timeout to %jd",
          (intmax_t) tv.tv_sec);
    tv.tv_usec = 0;

    struct addrinfo query = { 0 }, *response, *ap;
    struct addrinfo query2 = { 0 }, *response2, *ap2;
    int err, connected = false;

    memset(&query, 0, sizeof(query));
    query.ai_family = fc.force_ipv4 ? AF_INET : AF_UNSPEC;
    query.ai_socktype = SOCK_STREAM;

    if ((err = getaddrinfo(host, strport, &query, &response)) != 0)
    {
        Log(LOG_LEVEL_INFO,
              "Unable to find host or service: (%s/%s): %s",
              host, strport, gai_strerror(err));
        return false;
    }

    for (ap = response; ap != NULL; ap = ap->ai_next)
    {
        /* Convert address to string. */
        char txtaddr[CF_MAX_IP_LEN] = "";
        getnameinfo(ap->ai_addr, ap->ai_addrlen,
                    txtaddr, sizeof(txtaddr),
                    NULL, 0, NI_NUMERICHOST);
        Log(LOG_LEVEL_VERBOSE, "Connecting to host %s (address %s) on port %s",
              host, txtaddr, strport);

        ConnectionInfoSetSocket(conn->conn_info, socket(ap->ai_family, ap->ai_socktype, ap->ai_protocol));
        if (ConnectionInfoSocket(conn->conn_info) == -1)
        {
            Log(LOG_LEVEL_ERR, "Couldn't open a socket. (socket: %s)", GetErrorStr());
            continue;
        }

        /* Bind socket to specific interface, if requested. */
        if (BINDINTERFACE[0] != '\0')
        {
            memset(&query2, 0, sizeof(query2));
            query2.ai_family = fc.force_ipv4 ? AF_INET : AF_UNSPEC;
            query2.ai_socktype = SOCK_STREAM;
            /* returned address is for bind() */
            query2.ai_flags = AI_PASSIVE;

            err = getaddrinfo(BINDINTERFACE, NULL, &query2, &response2);
            if ((err) != 0)
            {
                Log(LOG_LEVEL_ERR,
                    "Unable to lookup interface '%s' to bind. (getaddrinfo: %s)",
                      BINDINTERFACE, gai_strerror(err));
                cf_closesocket(ConnectionInfoSocket(conn->conn_info));
                ConnectionInfoSetSocket(conn->conn_info, SOCKET_INVALID);
                freeaddrinfo(response2);
                freeaddrinfo(response);
                return false;
            }

            for (ap2 = response2; ap2 != NULL; ap2 = ap2->ai_next)
            {
                if (bind(ConnectionInfoSocket(conn->conn_info), ap2->ai_addr, ap2->ai_addrlen) == 0)
                {
                    break;
                }
            }
            freeaddrinfo(response2);
        }

        if (TryConnect(conn, &tv, ap->ai_addr, ap->ai_addrlen))
        {
            Log(LOG_LEVEL_INFO, "Connected to %s on port %s",
                txtaddr, strport);

            assert(sizeof(conn->remoteip) >= sizeof(txtaddr));
            strcpy(conn->remoteip, txtaddr);
            conn->family = ap->ai_family;
            connected = true;
            break;
        }
    }

    if (!connected)
    {
        if (ConnectionInfoSocket(conn->conn_info) >= 0)                 /* not INVALID or OFFLINE socket */
        {
            cf_closesocket(ConnectionInfoSocket(conn->conn_info));
            ConnectionInfoSetSocket(conn->conn_info, SOCKET_INVALID);
        }
    }

    if (response != NULL)
    {
        freeaddrinfo(response);
    }

    if (!connected)
    {
        Log(LOG_LEVEL_VERBOSE, "Unable to connect to server %s: %s", host, GetErrorStr());
        return false;
    }
    return true;
}

/*********************************************************************/

static bool ServerOffline(const char *server)
{
    char ipaddr[CF_MAX_IP_LEN];
    if (Hostname2IPString(ipaddr, server, sizeof(ipaddr)) == -1)
    {
        /* Ignore the error for now, resolution
           will probably fail again soon... TODO return true? */
        return false;
    }

    ThreadLock(&cft_serverlist);
    Seq *srvlist_tmp = GetGlobalServerList();
    ThreadUnlock(&cft_serverlist);

    for (size_t i = 0; i < SeqLength(srvlist_tmp); i++)
    {
        ServerItem *svp = SeqAt(srvlist_tmp, i);

        if (svp == NULL)
        {
            ProgrammingError("SERVERLIST had NULL ServerItem!");
        }

        if (strcmp(ipaddr, svp->server) == 0)
        {
            if (svp->conn == NULL)
            {
                ProgrammingError("ServerOffline:"
                                 " NULL connection in SERVERLIST for %s!",
                                 ipaddr);
            }

            if (ConnectionInfoSocket(svp->conn->conn_info) == CF_COULD_NOT_CONNECT)
                return true;
            else
                return false;
        }
    }

    return false;
}

static AgentConnection *GetIdleConnectionToServer(const char *server)
{
    char ipaddr[CF_MAX_IP_LEN];
    if (Hostname2IPString(ipaddr, server, sizeof(ipaddr)) == -1)
    {
        Log(LOG_LEVEL_WARNING,
            "GetIdleConnectionToServer: could not resolve '%s'", server);
        return NULL;
    }

    // TODO: How does this locking help anything? This is not a copy
    ThreadLock(&cft_serverlist);
    Seq *srvlist_tmp = GetGlobalServerList();
    ThreadUnlock(&cft_serverlist);

    for (size_t i = 0; i < SeqLength(srvlist_tmp); i++)
    {
        ServerItem *svp = SeqAt(srvlist_tmp, i);

        if (svp == NULL)
        {
            ProgrammingError("SERVERLIST had NULL ServerItem!");
        }

        if ((strcmp(ipaddr, svp->server) == 0))
        {
            if (svp->conn == NULL)
            {
                ProgrammingError("GetIdleConnectionToServer:"
                                 " NULL connection in SERVERLIST for %s!",
                                 ipaddr);
            }

            if (svp->busy)
            {
                Log(LOG_LEVEL_VERBOSE, "GetIdleConnectionToServer:"
                    " connection to '%s' seems to be active...",
                    ipaddr);
            }
            else if (ConnectionInfoSocket(svp->conn->conn_info) == CF_COULD_NOT_CONNECT)
            {
                Log(LOG_LEVEL_VERBOSE, "GetIdleConnectionToServer:"
                    " connection to '%s' is marked as offline...",
                    ipaddr);
            }
            else if (ConnectionInfoSocket(svp->conn->conn_info) > 0)
            {
                Log(LOG_LEVEL_VERBOSE, "GetIdleConnectionToServer:"
                    " found connection to %s already open and ready.",
                    ipaddr);
                svp->busy = true;
                return svp->conn;
            }
            else
            {
                Log(LOG_LEVEL_VERBOSE,
                    " connection to '%s' is in unknown state %d...",
                    ipaddr, ConnectionInfoSocket(svp->conn->conn_info));
            }
        }
    }

    Log(LOG_LEVEL_VERBOSE, "GetIdleConnectionToServer:"
        " no existing connection to '%s' is established...", ipaddr);
    return NULL;
}

/*********************************************************************/

void ServerNotBusy(AgentConnection *conn)
{
    ThreadLock(&cft_serverlist);
    Seq *srvlist_tmp = GetGlobalServerList();
    ThreadUnlock(&cft_serverlist);

    for (size_t i = 0; i < SeqLength(srvlist_tmp); i++)
    {
        ServerItem *svp = SeqAt(srvlist_tmp, i);

        if (svp->conn == conn)
        {
            svp->busy = false;
            Log(LOG_LEVEL_VERBOSE, "Existing connection just became free...");
            return;
        }
    }
    ProgrammingError("ServerNotBusy: No connection found!");
}

/*********************************************************************/

static void MarkServerOffline(const char *server)
/* Unable to contact the server so don't waste time trying for
   other connections, mark it offline */
{
    AgentConnection *conn = NULL;

    char ipaddr[CF_MAX_IP_LEN];
    if (Hostname2IPString(ipaddr, server, sizeof(ipaddr)) == -1)
    {
        Log(LOG_LEVEL_ERR,
            "MarkServerOffline: could not resolve '%s'", server);
        return;
    }

    Seq *srvlist_tmp = GetGlobalServerList();

    for (size_t i = 0; i < SeqLength(srvlist_tmp); i++)
    {
        ServerItem *svp = SeqAt(srvlist_tmp, i);
        if (svp == NULL)
        {
            ProgrammingError("SERVERLIST had NULL ServerItem!");
        }

        conn = svp->conn;
        if (strcmp(ipaddr, svp->server) == 0)
        /* TODO assert conn->remoteip == svp->server? Why do we need both? */
        {
            /* Found it, mark offline */
            ConnectionInfoSetSocket(conn->conn_info, CF_COULD_NOT_CONNECT);
            return;
        }
    }

    /* If no existing connection, create one and mark it unconnectable. */
    ServerItem *svp = xmalloc(sizeof(*svp));
    svp->server = xstrdup(ipaddr);
    svp->busy = false;
    svp->conn = NewAgentConn(ipaddr, false);
    ConnectionInfoSetProtocolVersion(svp->conn->conn_info, CF_PROTOCOL_CLASSIC);
    ConnectionInfoSetConnectionStatus(svp->conn->conn_info, CF_CONNECTION_NOT_ESTABLISHED);
    ConnectionInfoSetSocket(svp->conn->conn_info, CF_COULD_NOT_CONNECT);

    ThreadLock(&cft_serverlist);
    SeqAppend(srvlist_tmp, svp);
    ThreadUnlock(&cft_serverlist);
}

/*********************************************************************/

static void CacheServerConnection(AgentConnection *conn, const char *server)
/* First time we open a connection, so store it */
{
    char ipaddr[CF_MAX_IP_LEN];
    if (Hostname2IPString(ipaddr, server, sizeof(ipaddr)) == -1)
    {
        Log(LOG_LEVEL_ERR, "Could not resolve '%s'", server);
        return;
    }

    ServerItem *svp = xmalloc(sizeof(*svp));
    svp->server = xstrdup(ipaddr);
    svp->busy = true;
    svp->conn = conn;

    ThreadLock(&cft_serverlist);
    SeqAppend(GetGlobalServerList(), svp);
    ThreadUnlock(&cft_serverlist);
}

/*********************************************************************/

static int CacheStat(const char *file, struct stat *statbuf, const char *stattype, AgentConnection *conn)
{
    Stat *sp;

    for (sp = conn->cache; sp != NULL; sp = sp->next)
    {
        if ((strcmp(conn->this_server, sp->cf_server) == 0) && (strcmp(file, sp->cf_filename) == 0))
        {
            if (sp->cf_failed)  /* cached failure from cfopendir */
            {
                errno = EPERM;
                return -1;
            }

            if ((strcmp(stattype, "link") == 0) && (sp->cf_lmode != 0))
            {
                statbuf->st_mode = sp->cf_lmode;
            }
            else
            {
                statbuf->st_mode = sp->cf_mode;
            }

            statbuf->st_uid = sp->cf_uid;
            statbuf->st_gid = sp->cf_gid;
            statbuf->st_size = sp->cf_size;
            statbuf->st_atime = sp->cf_atime;
            statbuf->st_mtime = sp->cf_mtime;
            statbuf->st_ctime = sp->cf_ctime;
            statbuf->st_ino = sp->cf_ino;
            statbuf->st_dev = sp->cf_dev;
            statbuf->st_nlink = sp->cf_nlink;

            return 0;
        }
    }

    return 1;
}

/*********************************************************************/

static void FlushFileStream(int sd, int toget)
{
    int i;
    char buffer[2];

    Log(LOG_LEVEL_INFO, "Flushing rest of file...%d bytes", toget);

    for (i = 0; i < toget; i++)
    {
        recv(sd, buffer, 1, 0); /* flush to end of current file */
    }
}

/*********************************************************************/

void ConnectionsInit(void)
{
    ThreadLock(&cft_serverlist);
    SeqClear(GetGlobalServerList());
    ThreadUnlock(&cft_serverlist);
}

/*********************************************************************/

/* No locking taking place in here, so make sure you've finalised all threads
 * before calling this one! */
void ConnectionsCleanup(void)
{
    Seq *srvlist_tmp = GetGlobalServerList();

    for (size_t i = 0; i < SeqLength(srvlist_tmp); i++)
    {
        ServerItem *svp = SeqAt(srvlist_tmp, i);
        if (svp == NULL)
        {
            ProgrammingError("SERVERLIST had NULL ServerItem!");
        }
        if (svp->conn == NULL)
        {
            ProgrammingError("ConnectionsCleanup:"
                             "NULL connection in SERVERLIST!");
        }

        DisconnectServer(svp->conn, false);
    }

    SeqClear(srvlist_tmp);
}

/*********************************************************************/

#if !defined(__MINGW32__)

#if defined(__hpux) && defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
// HP-UX GCC type-pun warning on FD_SET() macro:
// While the "fd_set" type is defined in /usr/include/sys/_fd_macros.h as a
// struct of an array of "long" values in accordance with the XPG4 standard's
// requirements, the macros for the FD operations "pretend it is an array of
// int32_t's so the binary layout is the same for both Narrow and Wide
// processes," as described in _fd_macros.h. In the FD_SET, FD_CLR, and
// FD_ISSET macros at line 101, the result is cast to an "__fd_mask *" type,
// which is defined as int32_t at _fd_macros.h:82.
//
// This conflict between the "long fds_bits[]" array in the XPG4-compliant
// fd_set structure, and the cast to an int32_t - not long - pointer in the
// macros, causes a type-pun warning if -Wstrict-aliasing is enabled.
// The warning is merely a side effect of HP-UX working as designed,
// so it can be ignored.
#endif

int TryConnect(AgentConnection *conn, struct timeval *tvp, struct sockaddr *cinp, int cinpSz)
/** 
 * Tries a nonblocking connect and then restores blocking if
 * successful. Returns true on success, false otherwise.
 * NB! Do not use recv() timeout - see note below.
 **/
{
    int res;
    long arg;
    struct sockaddr_in emptyCin = { 0 };

    if (!cinp)
    {
        cinp = (struct sockaddr *) &emptyCin;
        cinpSz = sizeof(emptyCin);
    }

    /* set non-blocking socket */
    arg = fcntl(ConnectionInfoSocket(conn->conn_info), F_GETFL, NULL);

    if (fcntl(ConnectionInfoSocket(conn->conn_info), F_SETFL, arg | O_NONBLOCK) == -1)
    {
        Log(LOG_LEVEL_ERR, "Could not set socket to non-blocking mode. (fcntl: %s)", GetErrorStr());
    }

    res = connect(ConnectionInfoSocket(conn->conn_info), cinp, (socklen_t) cinpSz);

    if (res < 0)
    {
        if (errno == EINPROGRESS)
        {
            fd_set myset;
            int valopt;
            socklen_t lon = sizeof(int);

            FD_ZERO(&myset);

            FD_SET(ConnectionInfoSocket(conn->conn_info), &myset);

            /* now wait for connect, but no more than tvp.sec */
            res = select(ConnectionInfoSocket(conn->conn_info) + 1, NULL, &myset, NULL, tvp);
            if (getsockopt(ConnectionInfoSocket(conn->conn_info), SOL_SOCKET, SO_ERROR, (void *) (&valopt), &lon) != 0)
            {
                Log(LOG_LEVEL_ERR, "Could not check connection status. (getsockopt: %s)", GetErrorStr());
                return false;
            }

            if (valopt || (res <= 0))
            {
                Log(LOG_LEVEL_INFO, "Error connecting to server (timeout): (getsockopt: %s)", GetErrorStr());
                return false;
            }
        }
        else
        {
            Log(LOG_LEVEL_INFO, "Error connecting to server. (connect: %s)", GetErrorStr());
            return false;
        }
    }

    /* connection suceeded; return to blocking mode */

    if (fcntl(ConnectionInfoSocket(conn->conn_info), F_SETFL, arg) == -1)
    {
        Log(LOG_LEVEL_ERR, "Could not set socket to blocking mode. (fcntl: %s)", GetErrorStr());
    }

    if (SetReceiveTimeout(ConnectionInfoSocket(conn->conn_info), tvp) == -1)
    {
        Log(LOG_LEVEL_ERR, "Could not set socket timeout. (SetReceiveTimeout: %s)", GetErrorStr());
    }

    return true;
}

#if defined(__hpux) && defined(__GNUC__)
#pragma GCC diagnostic warning "-Wstrict-aliasing"
#endif

#endif /* !defined(__MINGW32__) */
