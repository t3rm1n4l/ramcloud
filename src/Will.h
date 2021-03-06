/* Copyright (c) 2010 Stanford University
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

#ifndef RAMCLOUD_WILL_H
#define RAMCLOUD_WILL_H

#include <vector>
#include "Common.h"
#include "Tablets.pb.h"
#include "TabletProfiler.h"

namespace RAMCloud {

class Will {
  public:
    Will(ProtoBuf::Tablets& tablets, uint64_t maxBytesPerPartition,
         uint64_t maxReferentsPerPartition);
    void serialize(ProtoBuf::Tablets& will);
    void debugDump();

  PRIVATE:
    /// Each entry in the Will describes a key range for a particular
    /// Tablet and is assigned to a partition. All entries for a
    /// particular partition must meet the total byte and referent
    /// contraints given to the constructor.
    struct WillEntry {
        uint64_t partitionId;
        uint64_t tableId;
        uint64_t firstKey;
        uint64_t lastKey;
        uint64_t minBytes;
        uint64_t maxBytes;
        uint64_t minReferents;
        uint64_t maxReferents;
    };
    typedef std::vector<WillEntry> WillList;

    /* current partition state */
    uint64_t currentId;             /// current partitionId we're working on
    uint64_t currentMaxBytes;       /// current max bytes in this partition
    uint64_t currentMaxReferents;   /// current max referents in this partition

    /// current number of TabletProfiler partitions added since the
    /// last currentId increment
    uint64_t currentCount;

    /* parameters dictating partition sizes */
    uint64_t maxBytesPerPartition;      /// max bytes allowed in a partition
    uint64_t maxReferentsPerPartition;  /// max referents allowed in a partition

    /// list tablets, ordered by partition
    WillList entries;

    void     addTablet(const ProtoBuf::Tablets::Tablet& tablet);
    void     addPartition(Partition& partition,
                          const ProtoBuf::Tablets::Tablet& tablet);

    friend class WillBenchmark;

    DISALLOW_COPY_AND_ASSIGN(Will);
};

} // namespace

#endif // !RAMCLOUD_WILL_H
