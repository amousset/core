/*
  Copyright 2019 Northern.tech AS

  This file is part of CFEngine 3 - written and maintained by Northern.tech AS.

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

#ifndef CFENGINE_PROTOCOL_H
#define CFENGINE_PROTOCOL_H

#include <cfnet.h>
#include <sequence.h>

static inline bool ProtocolIsKnown(const ProtocolVersion p)
{
    return ((p > CF_PROTOCOL_UNDEFINED) && (p <= CF_PROTOCOL_LATEST));
}

static inline bool ProtocolIsTLS(const ProtocolVersion p)
{
    return ((p >= CF_PROTOCOL_TLS) && (p <= CF_PROTOCOL_LATEST));
}

static inline bool ProtocolIsTooNew(const ProtocolVersion p)
{
    return (p > CF_PROTOCOL_LATEST);
}

static inline bool ProtocolIsUndefined(const ProtocolVersion p)
{
    return (p <= CF_PROTOCOL_UNDEFINED);
}

static inline bool ProtocolIsClassic(const ProtocolVersion p)
{
    return (p == CF_PROTOCOL_CLASSIC);
}

/**
 * Returns CF_PROTOCOL_TLS or CF_PROTOCOL_CLASSIC (or CF_PROTOCOL_UNDEFINED)
 * Maps all versions using TLS to CF_PROTOCOL_TLS for convenience
 * in switch statements.
 */
static inline ProtocolVersion ProtocolClassicOrTLS(const ProtocolVersion p)
{
    if (ProtocolIsTLS(p))
    {
        return CF_PROTOCOL_TLS;
    }
    if (ProtocolIsClassic(p))
    {
        return CF_PROTOCOL_CLASSIC;
    }
    return CF_PROTOCOL_UNDEFINED;
}

/**
 * Parses the version string sent over network to enum, e.g. "CFE_v1" -> 1
 */
ProtocolVersion ParseProtocolVersionNetwork(const char *s);

/**
 * Parses the version string set in policy to enum, e.g. "classic" -> 1
 */
ProtocolVersion ParseProtocolVersionPolicy(const char *s);

// Old name for compatibility (enterprise), TODO remove:
#define ProtocolVersionParse ParseProtocolVersionPolicy

/**
 * Receives a directory listing from a remote host.
 *
 * The server will use "/var/cfengine" as working directory if absolute path
 * is not specified. The server sends the directory entries as a string
 * separated by NUL-bytes, and ending with the magic string CFD_TERMINATOR.
 *
 * The function shall fail if connection is not established, or if the server
 * gives a bad response (denoted by a message preceded by "BAD").
 *
 * @param [in] conn  The connection to use
 * @param [in] path  Path on remote host
 * @return A sequence of filenames in the requested directory on success, NULL
 *         on failure.
 *
 * Example (for printing each directory entry):
 * @code
 *     AgentConnection *conn = ServerConnection("127.0.0.1", "666", ...);
 *     Seq *dir = ProtocolOpenDir(conn, "masterfiles");
 *     for (int i = 0; i < SeqLength(dir); i++)
 *     {
 *         char *entry = SeqAt(i);
 *         printf("%s\n", entry);
 *     }
 * @endcode
 *
 * In the protocol, this will look like this on the server side:
 *     Received: OPENDIR masterfiles
 *     Translated to: OPENDIR /var/cfengine/masterfiles
 *     Sends string:
 *     ".\0..\0cfe_internal\0cf_promises_release_id\0...
 *      ...templates\0update.cf\0" CFD_TERMINATOR
 */
Seq *ProtocolOpenDir(AgentConnection *conn, const char *path);

#endif
