/* Copyright (c) 2011 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * \file
 * This file defines the ServerList class.
 */

#ifndef RAMCLOUD_SERVERLIST_H
#define RAMCLOUD_SERVERLIST_H

#include "ServiceMask.h"
#include "ServerId.h"
#include "SpinLock.h"
#include "Transport.h"
#include "Tub.h"

namespace RAMCloud {

/// Forward decl.
class ServerTrackerInterface;

/**
 * Exception type thrown by the ServerList class.
 */
struct ServerListException : public Exception {
    ServerListException(const CodeLocation& where, std::string msg)
        : Exception(where, msg) {}
};

/**
 * A ServerList maintains a mapping of coordinator-allocated ServerIds to
 * the service locators that address particular servers. Here a "server"
 * is not a physical machine, but rather a specific instance of a RAMCloud
 * server process.
 *
 * The intent is to have a single ServerList per process. If a module wishes
 * to keep track of changes to the ServerList (i.e. be notified when machines
 * are added or removed), then it may register its own private ServerTracker
 * with the master ServerList. The tracker will be fed updates whenever servers
 * come or go. The tracker also provides a convenient way to associate their
 * own per-server state with ServerIds that they're using or keeping track of.
 *
 * The ServerList is re-entrant. It is expected that one or more threads will
 * feed it updates from the coordinator while others register or unregister
 * ServerTrackers.
 */
class ServerList {
  PUBLIC:
    /**
     * Information about a particular server in the serverList vector.
     * This information is disseminated as part of ServerChanges to listening
     * trackers and replicated there for fast, lock-free access.
     */
    class ServerDetails {
      PUBLIC:
        /**
         * Create an instance where all fields are invalid. Used to 'zero-out'
         * serverList entries which aren't currently associated with a server.
         */
        ServerDetails()
            : serverId()
            , serviceLocator()
            , services()
            , expectedReadMBytesPerSec()
        {}

        /**
         * Create an instance where only #serverId is valid. Used to represent
         * the details of a SERVER_REMOVED event.
         */
        explicit ServerDetails(ServerId id)
            : serverId(id)
            , serviceLocator()
            , services()
            , expectedReadMBytesPerSec()
        {}

        /**
         * Create an instance which represents an active server in the cluster.
         * All fields are valid.
         */
        ServerDetails(ServerId id,
                      const string& locator,
                      ServiceMask services)
            : serverId(id)
            , serviceLocator(locator)
            , services(services)
            , expectedReadMBytesPerSec()
        {}

        ServerDetails(ServerId id,
                      const string& locator,
                      ServiceMask services,
                      uint32_t expectedReadMBytesPerSec)
            : serverId(id)
            , serviceLocator(locator)
            , services(services)
            , expectedReadMBytesPerSec(expectedReadMBytesPerSec)
        {}

        /// ServerId associated with this index in the serverList.
        ServerId serverId;

        /// Service locator associated with this serverId in the serverList.
        string serviceLocator;

        /// Which services are supported by the process at #serverId.
        ServiceMask services;

        /// Disk bandwidth of the backup server in MB/s, if
        /// services.has(BACKUP_SERVICE), invalid otherwise.
        uint32_t expectedReadMBytesPerSec;
    };

    ServerList();
    ~ServerList();
    void add(ServerId id, const string& locator,
             ServiceMask services, uint32_t expectedReadMBytesPerSec);
    void remove(ServerId id);
    string getLocator(ServerId id);
    string toString(ServerId serverId);
    Transport::SessionRef getSession(ServerId id);
    uint32_t size();
    ServerId operator[](uint32_t indexNumber);
    bool contains(ServerId serverId);
    uint64_t getVersion();
    void setVersion(uint64_t newVersion);
    void registerTracker(ServerTrackerInterface& tracker);
    void unregisterTracker(ServerTrackerInterface& tracker);

  PRIVATE:
    /// Slots in the server list.
    std::vector<Tub<ServerDetails>> serverList;

    /// Version number of this list, as dictated by the coordinator. Used to
    /// tell if the list is out of date, and if so, by how many additions or
    /// removals.
    uint64_t version;

    /// ServerTrackers that have registered with us and will receive updates
    /// regarding additions or removals from this list.
    std::vector<ServerTrackerInterface*> trackers;

    /// Mutex to protect the class. Simultaneous access can occur due to
    /// ServerTrackers registering/unregistering while adds and removes
    /// are being handled.
    SpinLock mutex;

    DISALLOW_COPY_AND_ASSIGN(ServerList);
};

} // namespace RAMCloud

#endif // !RAMCLOUD_SERVERLIST_H
