/* Copyright (c) 2011-2012 Stanford University
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

#include "ReplicaManager.h"
#include "ReplicatedSegment.h"
#include "ShortMacros.h"
#include "TaskManager.h"

namespace RAMCloud {

// --- ReplicatedSegment ---

/**
 * Create a ReplicatedSegment.  Only called by ReplicaManager.
 *
 * \param taskManager
 *      The ReplicaManager's work queue, this is added to it when schedule()
 *      is called.
 * \param tracker
 *      The tracker used to find backups and track replica distribution
 *      stats.
 * \param backupSelector
 *      Used to choose where to store replicas. Shared among ReplicatedSegments.
 * \param writeRpcsInFlight
 *      Number of outstanding write rpcs to backups across all
 *      ReplicatedSegments.  Used to throttle write rpcs.
 * \param dataMutex
 *      Mutex which protects all ReplicaManager state; shared with the
 *      ReplicaManager and all other ReplicatedSegments.
 * \param deleter
 *      Deletes this when this determines it is no longer needed.
 * \param masterId
 *      The server id of the master whose log this segment belongs to.
 * \param segmentId
 *      ID for the segment, must match the segmentId given by the log module.
 * \param data
 *      The start of raw bytes of the in-memory log segment to be replicated.
 * \param openLen
 *      Bytes to send atomically to backups with the open segment rpc.
 * \param numReplicas
 *      Number of replicas of this segment that must be maintained.
 * \param maxBytesPerWriteRpc
 *      Maximum bytes to send in a single write rpc; can help latency of
 *      GetRecoveryDataRequests by unclogging backups a bit.
 */
ReplicatedSegment::ReplicatedSegment(TaskManager& taskManager,
                                     BackupTracker& tracker,
                                     BaseBackupSelector& backupSelector,
                                     Deleter& deleter,
                                     uint32_t& writeRpcsInFlight,
                                     std::mutex& dataMutex,
                                     ServerId masterId,
                                     uint64_t segmentId,
                                     const void* data,
                                     uint32_t openLen,
                                     uint32_t numReplicas,
                                     uint32_t maxBytesPerWriteRpc)
    : Task(taskManager)
    , tracker(tracker)
    , backupSelector(backupSelector)
    , deleter(deleter)
    , writeRpcsInFlight(writeRpcsInFlight)
    , dataMutex(dataMutex)
    , masterId(masterId)
    , segmentId(segmentId)
    , data(data)
    , openLen(openLen)
    , maxBytesPerWriteRpc(maxBytesPerWriteRpc)
    , queued(true, openLen, false)
    , freeQueued(false)
    , followingSegment(NULL)
    , precedingSegmentCloseAcked(true)
    , listEntries()
    , replicas(numReplicas)
{
    schedule(); // schedule to replicate the opening data
}

ReplicatedSegment::~ReplicatedSegment()
{
    assert(!isScheduled());
}

/**
 * Request the eventual freeing all known replicas of a segment from its
 * backups.  The caller's ReplicatedSegment pointer is invalidated upon the
 * return of this function.  After the return of this call all outstanding
 * write rpcs for this segment are guaranteed to have completed so the log
 * memory associated with this segment is free for reuse.  This implies that
 * this call can spin waiting for write rpcs, though, it tries to be
 * friendly to concurrent operations by releasing and reacquiring the
 * internal ReplicaManager lock each time it checks rpcs for completion.
 *
 * Currently, there is no public interface to ensure enqueued free
 * operations have completed.
 */
void
ReplicatedSegment::free()
{
    TEST_LOG("%lu, %lu", *masterId, segmentId);

    // The order is important and rather subtle here:
    // First, mark the segment as queued for freeing.
    // Then make sure not to return to the caller before any outstanding
    // write request has finished.
    // If the segment isn't marked free first then new write requests for
    // other replicas may get started as we wait to reap the outstanding
    // writeRpc.  This can cause the length of time the lock is held to
    // stretch out.

    Tub<Lock> lock;
    lock.construct(dataMutex);
    freeQueued = true;

    checkAgain:
    foreach (auto& replica, replicas) {
        if (!replica || !replica->writeRpc)
            continue;
        taskManager.proceed();
        // Release and reacquire the lock; this gives other operations
        // a chance to slip in while this thread waits for all write
        // rpcs to finish up.
        lock.construct(dataMutex);
        goto checkAgain;
    }

    schedule();
}

/**
 * Request the eventual close of the replicas of a segment on its backups;
 * please read the documentation for this function carefully.
 *
 * Once close() is called the only valid operation on the segment is free();
 * no further write() calls are permitted.  The caller cannot ensure that the
 * closed status of the segment is reflected durably in its replicas without
 * getting creative; this class takes care of that detail for callers.
 *
 * The timing of when a close is replicated for a segment relative to
 * open and write requests for the following segment affects the integrity
 * of the log during recovery.  During log cleaning and unit testing this
 * ordering isn't important (see Log cleaning and unit testing below).
 *
 * Normal operation:
 *
 * \a followingSegment is used to enforce a safe ordering of operations issued
 * to backups; therefore, its correct use is critical to ensure:
 *  1) That log is not mistakenly detected as incomplete during recovery, and
 *  2) That all data loss is detected during recovery.
 *
 * For a log transitioning from a full head segment s1 to a new, empty
 * head segment s2 the caller must guarantee:
 *  1) s1.close(s2) is called (\a followingSegment is the new head), and
 *  2) No call to s2.write(...) precedes the call to s1.close(s2).
 *
 * Explanation of the problems which can occur:
 *
 * Problem 1.
 * If the coordinator cannot find an open log segment during recovery it
 * has no way of knowing if it has all of the log (any number of segments
 * from the head may have been lost).  Because of this it is critical that
 * there always be at least one segment durably marked as open on backups.
 * Call this the open-before-close rule.  \a followingSegment allows an easy
 * check to make sure that the new head segment in the log is durably open
 * before issuing any close rpcs for old head segment.
 * Not obeying open-before-close threatens the integrity of the entire log
 * during recovery.
 *
 * Problem 2.
 * If log data is (even durably) stored in an open segment while other
 * segments which precede it in the log are still open the data may not be
 * detected as lost if it is lost.  This is because if all the replicas for
 * the segment with the data in it are lost the coordinator will still
 * conclude it has recovered the entire log since it was able to find an
 * open segment (and thus the head of the log).
 * Call this the no-write-before-preceding-close rule; not obeying this rule
 * can result in loss of data acknowledged to applications as durable after
 * a recovery.
 *
 * Together these two rules transitively create the following flow during
 * normal operation for any two segments s1 and s2 which follows s1:
 * s2 is durably opened -> s1 is durably closed -> writes are issued for s2.
 * This cycle repeats as segments are added to the log.
 *
 * Internally, \a followingSegment is sufficient to ensure this ordering.
 * See #followingSegment for an explanation of the checks performed.
 *
 * Log cleaning and unit testing:
 *
 * During log cleaning many segments at a time are allocated and written
 * (writes for different cleaned segments can be interleaved) and sync() is
 * called explicitly at the end to ensure all writes on them have been
 * completed before they are added to the log.  Since they are spliced into
 * the log atomically as part of another open segment they do not (and cannot
 * obey these extra ordering constraints).  To bypass these constraints the
 * log cleaner can simply pass NULL is for \a followingSegment.  Similarly,
 * since unit tests should almost always pass NULL to avoid these extra
 * ordering checks.
 *
 * Cleaned log segment replicas can appear as open during recovery without
 * issue (neither this class or the caller are expected to wait for those
 * segments to be durably closed).  This is because the system will not
 * consider segments without a digest to be the head of the log and a cleaned
 * replica can only be considered part of the log if it was named in a log
 * digest.  Cleaned segment replicas are simply sync()'ed before being
 * spliced into the log to ensure all the data is durable.
 *
 * \param followingSegment
 *      For a normal log segment this is a pointer to the ReplicatedSegment
 *      which logically will follow this segment in the log.  Used to check
 *      ordering constraints of backup replication operations, see above.
 *      Pass NULL for log cleaning or during unit testing to bypass the
 *      ordering constraints.
 */
void
ReplicatedSegment::close(ReplicatedSegment* followingSegment)
{
    Lock _(dataMutex);
    TEST_LOG("%lu, %lu, %lu", *masterId, segmentId, followingSegment ?
                                            followingSegment->segmentId : 0);

    // immutable after close
    assert(!queued.close);
    queued.close = true;
    this->followingSegment = followingSegment;
    if (followingSegment) {
        if (followingSegment->openLen != followingSegment->queued.bytes) {
            LOG(ERROR, "Caller provided followingSegment to request "
                       "enforcement of close-segment-before-write-to-next, "
                       "but the following segment has already writes queued "
                       "before close was called");
        }
        followingSegment->precedingSegmentCloseAcked = false;
    }
    LOG(DEBUG, "Segment %lu closed (length %d)", segmentId, queued.bytes);
    ++metrics->master.segmentCloseCount;

    schedule();
}

/**
 * Wait for the durable replication (meaning at least durably buffered on
 * backups) of data starting at the beginning of the segment up through \a
 * offset bytes (non-inclusive).  Also implies the data will be recovered in
 * the case that the master crashes (provided warnings on
 * ReplicatedSegment::close are obeyed).  Note, this method can wait forever if
 * \a offset bytes are never enqueued for replication.
 *
 * This must be called after any openSegment() or ReplicatedSegment::write()
 * calls where the operation must be immediately durable (though, keep in mind,
 * host failures could have eliminated some replicas even as sync returns).
 *
 * \param offset
 *      The number of bytes of the segment that must be replicated before the
 *      call will return.
 */
void
ReplicatedSegment::sync(uint32_t offset)
{
    CycleCounter<RawMetric> _(&metrics->master.replicaManagerTicks);
    TEST_LOG("syncing");

    while (true) {
        Lock lock(dataMutex);
        if (getAcked().bytes >= offset)
            break;
        taskManager.proceed();
    }
}

/**
 * Request the eventual replication of data ending at \a offset non-inclusive 
 * on a set backups for durability.  Guarantees that no replica will see this
 * write until it has seen all previous writes on this segment.  sync() must
 * be called after write() calls where the operation must be durable.
 *
 * \pre
 *      All previous segments have been closed (at least locally).
 * \param offset
 *      The number of bytes into the segment to replicate.
 */
void
ReplicatedSegment::write(uint32_t offset)
{
    Lock _(dataMutex);
    TEST_LOG("%lu, %lu, %u", *masterId, segmentId, offset);

    // immutable after close
    assert(!queued.close);
    // offset monotonically increases
    assert(offset >= queued.bytes);
    queued.bytes = offset;

    schedule();
}

// - private -

/**
 * Check replication state and make progress in restoring invariants;
 * generally don't invoke this directly, instead use schedule().
 *
 * This method must be called (indirectly via schedule()) when the
 * state of this ReplicatedSegment changes in a non-trivial way in order to
 * ensure that replication invariants hold and to start work in response,
 * if needed.
 *
 * schedule() is called in three cases:
 * 1) A cluster membership change may have affected replication invariants
 *    for this segment.
 * 2) An action by the log module requires some work to be done (e.g. more
 *    replication, freeing replicas).
 * 3) An action done during performTask() itself requires future work (e.g.
 *    work couldn't be completed yet or work generated some new work
 *    which won't be done until a future time).
 */
void
ReplicatedSegment::performTask()
{
    if (freeQueued) {
        foreach (auto& replica, replicas)
            performFree(replica);
        if (!isScheduled()) // Everything is freed, destroy ourself.
            deleter.destroyAndFreeReplicatedSegment(this);
    } else {
        foreach (auto& replica, replicas)
            performWrite(replica);
        assert(isSynced() || isScheduled());
    }
}

/**
 * Make progress, if possible, in freeing a known replica of a segment
 * regardless of what state the replica is in (both locally and remotely).
 * If future work is required this method automatically re-schedules this
 * segment for future attention from the ReplicaManager.
 * \pre freeQueued must be true, otherwise behavior is undefined.
 */
void
ReplicatedSegment::performFree(Tub<Replica>& replica)
{
    /*
     * Internally this method is written as a set of nested
     * if-with-unqualified-else clauses (sometimes the else is implicit) with
     * explicit returns at the end of each block.  This repeatedly splits the
     * segment states between two cases until exactly one of the cases is
     * executed.  This makes it easy ensure all cases are covered and which
     * case a particular state will fall into.  performWrite() is written
     * is a similar style for the same reason.
     */
    if (!replica) {
        // Do nothing is there was no replica, no need to reschedule.
        return;
    }

    if (replica->freeRpc) {
        // A free rpc is outstanding to the backup storing this replica.
        if (replica->freeRpc->isReady()) {
            // Request is finished, clean up the state.
            try {
                (*replica->freeRpc)();
            } catch (TransportException& e) {
                // Retry, if it down the server list will let us know.
                LOG(WARNING,
                    "Failure freeing replica on backup, retrying: %s",
                    e.what());
                replica->freeRpc.destroy();
                schedule();
                return;
            }
            replica.destroy();
            // Free completed, no need to reschedule.
            return;
        } else {
            // Request is not yet finished, stay scheduled to wait on it.
            schedule();
            return;
        }
    } else {
        // No free rpc is outstanding.
        if (replica->writeRpc) {
            // Cannot issue free, a write is outstanding. Make progress on it.
            performWrite(replica);
            // Stay scheduled even if synced since we have to do free still.
            schedule();
            return;
        } else {
            // Issue a free rpc for this replica, reschedule to wait on it.
            replica->freeRpc.construct(replica->client, masterId, segmentId);
            schedule();
            return;
        }
    }
    assert(false); // Unreachable by construction.
}

/**
 * Make progress, if possible, in durably writing segment data to a particular
 * replica.  If future work is required this method automatically re-schedules
 * this segment for future attention from the ReplicaManager.
 */
void
ReplicatedSegment::performWrite(Tub<Replica>& replica)
{
    if (replica && replica->acked == queued) {
        // If this replica is synced no further work is needed for now.
        return;
    }

    if (!replica) {
        // This replica does not exist yet. Choose a backup and send the open.
        // Happens for a new segment or if a replica was known to be lost.
        if (writeRpcsInFlight == MAX_WRITE_RPCS_IN_FLIGHT) {
            schedule();
            return;
        }
        ServerId conflicts[replicas.numElements - 1];
        uint32_t numConflicts = 0;
        foreach (auto& conflictingReplica, replicas) {
            if (conflictingReplica)
                conflicts[numConflicts++] = conflictingReplica->backupId;
            assert(numConflicts < replicas.numElements);
        }
        ServerId backupId;
        BackupWriteRpc::Flags flags = BackupWriteRpc::OPEN;
        if (replicaIsPrimary(replica)) {
            backupId = backupSelector.selectPrimary(numConflicts, conflicts);
            flags = BackupWriteRpc::OPENPRIMARY;
        } else {
            backupId = backupSelector.selectSecondary(numConflicts, conflicts);
        }
        Transport::SessionRef session = tracker.getSession(backupId);
        replica.construct(backupId, session);
        replica->writeRpc.construct(replica->client, masterId, segmentId,
                                    0, data, openLen, flags);
        ++writeRpcsInFlight;
        replica->sent.open = true;
        replica->sent.bytes = openLen;
        schedule();
        return;
    }

    if (replica->writeRpc) {
        // This backup has a write request outstanding to a backup.
        if (replica->writeRpc->isReady()) {
            // Wait for it to complete if it is ready.
            try {
                (*replica->writeRpc)();
                replica->acked = replica->sent;
                if (replica->sent.close && followingSegment) {
                    followingSegment->precedingSegmentCloseAcked = true;
                    // Don't poke at potentially non-existent segments later.
                    followingSegment = NULL;
                }
            } catch (TransportException& e) {
                // Retry, if it is down the server list will let us know.
                replica->sent = replica->acked;
                LOG(WARNING,
                    "Failure writing replica on backup, retrying: %s",
                    e.what());
            }
            replica->writeRpc.destroy();
            --writeRpcsInFlight;
            if (replica->acked != queued)
                schedule();
            if (!replica->acked.open) {
                // If there was a TransportException then it may be
                // that the open hasn't been acknowledged yet. In that
                // case we reset to a state where the open will be retried.
                replica.destroy();
            }
            return;
        } else {
            // Request is not yet finished, stay scheduled to wait on it.
            schedule();
            return;
        }
    } else {
        // No outstanding write but not yet synced.
        if (replica->sent < queued) {
            // Some part of the data hasn't been sent yet.  Send it.
            assert(!replica->freeRpc);
            assert(!replica->sent.close);

            if (!precedingSegmentCloseAcked) {
                // This segment must wait to send write rpcs until the
                // preceding segment in the log sets precedingSegmentCloseAcked
                // to true.  The goal is to prevent data written in this
                // segment from being undetectably lost in the case that all
                // replicas of it are lost. See #precedingSegmentCloseAcked.

                schedule();
                return;
            }

            uint32_t offset = replica->sent.bytes;
            uint32_t length = queued.bytes - replica->sent.bytes;
            BackupWriteRpc::Flags flags = queued.close ?
                                            BackupWriteRpc::CLOSE :
                                            BackupWriteRpc::NONE;

            // Breaks atomicity of log entries, but it could happen anyway
            // if a segment gets partially written to disk.
            if (length > maxBytesPerWriteRpc) {
                length = maxBytesPerWriteRpc;
                flags = BackupWriteRpc::NONE;
            }

            if (flags == BackupWriteRpc::CLOSE) {
                // Do not send a closing write rpc for this replica until
                // some other segment later in the log has been durably
                // opened.  This ensures that the coordinator will find
                // an open segment during recovery which lets it know
                // the entire log has been found (that is, log isn't missing
                // some head segments).
                if (followingSegment) {
                    if (!followingSegment->getAcked().open) {
                        schedule();
                        return;
                    }
                }
            }

            const char* src = static_cast<const char*>(data) + offset;
            replica->writeRpc.construct(replica->client, masterId, segmentId,
                                        offset, src, length, flags);
            replica->sent.bytes += length;
            replica->sent.close = (flags == BackupWriteRpc::CLOSE);
            schedule();
            return;
        } else {
            // Replica not synced, no rpc outstanding, but all data was sent.
            // Impossible in the one in-flight rpc per replica case.
            assert(false);
            return;
        }
    }
    assert(false); // Unreachable by construction
}

} // namespace RAMCloud
