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

#ifndef RAMCLOUD_TABLETPROFILER_H
#define RAMCLOUD_TABLETPROFILER_H

#include <vector>
#include "Common.h"
#include "Log.h"

namespace RAMCloud {

class Partition {
 public:
    Partition()
        : firstKey(0), lastKey(0), minBytes(0), maxBytes(0),
          minReferents(0), maxReferents(0)
    {
    }
#if TESTING
    // only for testing. too many u64 args make it too error-prone in
    // normal use. this is just an expedient for tests.
    Partition(uint64_t firstKey, uint64_t lastKey, uint64_t minBytes,
              uint64_t maxBytes, uint64_t minReferents, uint64_t maxReferents)
        : firstKey(firstKey), lastKey(lastKey), minBytes(minBytes),
          maxBytes(maxBytes), minReferents(minReferents),
          maxReferents(maxReferents)
    {
    }
#endif
    uint64_t firstKey;          /// the first key of this partition
    uint64_t lastKey;           /// the last key of this partition
    uint64_t minBytes;          /// the min possible bytes in this partition
    uint64_t maxBytes;          /// the max possible bytes in this partition
    uint64_t minReferents;      /// the min possible referents in this partition
    uint64_t maxReferents;      /// the max possible referents in this partition
};
typedef std::vector<Partition> PartitionList;

class TabletProfiler {
  public:
    TabletProfiler();
    ~TabletProfiler();

    // public methods
    void           track(uint64_t key, uint32_t bytes, LogTime time);
    void           untrack(uint64_t key, uint32_t bytes, LogTime time);
    PartitionList* getPartitions(uint64_t maxPartitionBytes,
                                 uint64_t maxPartitionReferents,
                                 uint64_t residualMaxBytes,
                                 uint64_t residualMaxReferents);

    /**
     * Return the maximum number of bytes we can be off by when calculating
     * Partitions. The actual count is always <= what we report, so we can
     * only overestimate by at most this amount of error. We never underestimate.
     */
    static uint64_t
    getMaximumByteError()
    {
        return 2 * ((uint64_t)ceil(64.0 / BITS_PER_LEVEL) - 1) *
            BUCKET_SPLIT_BYTES;
    }

    /**
     * Return the maximum number of referents we can be off by when calculating
     * Partitions. The actual count is always <= what we report, so we can
     * only overestimate by at most this amount of error. We never underestimate.
     */
    static uint64_t
    getMaximumReferentError()
    {
        return 2 * ((uint64_t)ceil(64.0 / BITS_PER_LEVEL) - 1) *
            BUCKET_SPLIT_OBJS;
    }

  PRIVATE:
    /// Bits of key space we shave off each level deeper in the structure.
    /// The first level covers the entire 64-bit range, whereas the next
    /// level only covers 2^(64 - BITS_PER_LEVEL), and so on. This also
    /// affects the number of buckets per Subrange. I.e., there are at most
    /// 2^BITS_PER_LEVEL of them.
    static const int      BITS_PER_LEVEL = 8;

    /// Min bytes to track per bucket before using a child Subrange.
    static const uint64_t BUCKET_SPLIT_BYTES = 8 * 1024 * 1024;

    /// Min referents to track per bucket before using a child Subrange.
    static const uint64_t BUCKET_SPLIT_OBJS  = BUCKET_SPLIT_BYTES / 100;

    /// Max bytes a bucket and its parent can have before they are merged.
    static const uint64_t BUCKET_MERGE_BYTES = BUCKET_SPLIT_BYTES * 3 / 4;

    /// Max referents a bucket and its parent can have before they are merged.
    static const uint64_t BUCKET_MERGE_OBJS  = BUCKET_SPLIT_OBJS  * 3 / 4;

    class PartitionCollector {
      public:
        PartitionCollector(uint64_t maxPartitionBytes,
                           uint64_t maxPartitionReferents,
                           PartitionList* partitions,
                           uint64_t residualMaxBytes,
                           uint64_t residualMaxReferents);
        bool addRangeLeaf(uint64_t firstKey, uint64_t lastKey,
                          uint64_t rangeBytes, uint64_t rangeReferents,
                          uint64_t possibleBytes, uint64_t possibleReferents);
        void addRangeNonLeaf(uint64_t rangeBytes, uint64_t rangeReferents);
        void done();

      PRIVATE:
        PartitionList* partitions;

        void pushCurrentTally(uint64_t lastKey,
                              uint64_t minBytes,
                              uint64_t maxBytes,
                              uint64_t minReferents,
                              uint64_t maxReferents);

        /* residual counts passed in to the constructor, for use in the
           first partition generated */
        uint64_t residualMaxBytes;          /// Initial residual byte count
        uint64_t residualMaxReferents;      /// Initial residual referent count

        /* current tally */
        uint64_t maxPartitionBytes;         /// Current max byte count
        uint64_t maxPartitionReferents;     /// Current max referent count
        uint64_t nextFirstKey;              /// Next firstKey for addRangeLeaf
        uint64_t currentFirstKey;           /// Current first key in partition
        uint64_t currentKnownBytes;         /// Count of exactly known bytes
        uint64_t currentKnownReferents;     /// Count of exactly known referents
        uint64_t previousPossibleBytes;     /// Possible last partition bytes
        uint64_t previousPossibleReferents; /// Possible last partition refs
        bool     isDone;                    /// Is this partition is done?

        DISALLOW_COPY_AND_ASSIGN(PartitionCollector);
    };

    // forward decl
    class Subrange;

    /// A Bucket is used to track the number of bytes and referents within
    /// a contiguous subrange of the key space. Each Bucket may have a
    /// child Subrange, which more precisely tracks that range (i.e. with
    /// more individual Buckets). Note that counts in a parent Bucket
    /// are _not_ reflected in any descendant Buckets.
    struct Bucket {
        Subrange *child;                /// Pointer to child Subrange, or NULL
        uint64_t  totalBytes;           /// Total byte count
        uint64_t  totalReferents;       /// Total referent count
    };

    /// A Subrange is an individual node in our TabletProfiler tree. It tracks
    /// a specific contiguous subrange of the key space using individual
    /// Buckets.
    class Subrange {
      public:
        class BucketHandle {
          public:
            BucketHandle(Subrange *subrange, int bucketIndex);
            Subrange* getSubrange();
            Bucket*   getBucket();
            uint64_t  getFirstKey();
            uint64_t  getLastKey();

            bool operator==(const BucketHandle& other) const {
                return (this->subrange == other.subrange &&
                       this->bucketIndex == other.bucketIndex);
            }

          PRIVATE:
            Subrange* subrange;         /// Pointer to the Subrange referenced
            int       bucketIndex;      /// Index of the Bucket in subrange
        };

        Subrange(BucketHandle parent, uint64_t firstKey, uint64_t lastKey,
                 LogTime time);
       ~Subrange();

        void         track(BucketHandle bh, uint64_t key, uint32_t bytes,
                               LogTime time);
        bool         untrack(BucketHandle bh, uint64_t key, uint32_t bytes,
                                  LogTime time);
        BucketHandle findBucket(uint64_t key, LogTime *time = NULL);
        Bucket*      getBucket(uint32_t bucketIndex);
        uint64_t     getBucketFirstKey(BucketHandle bh);
        uint64_t     getBucketLastKey(BucketHandle bh);
        bool         isBottom();
        bool         partitionWalk(PartitionCollector *pc,
                                   uint64_t parentBytes = 0,
                                   uint64_t parentReferents = 0);
        LogTime      getCreateTime();
        uint64_t     getFirstKey();
        uint64_t     getLastKey();

      PRIVATE:
        BucketHandle parent;            /// Handle to Subrange's parent Bucket
        uint64_t     bucketWidth;       /// Keyspace width of each Bucket
        Bucket      *buckets;           /// Array of Buckets
        uint32_t     numBuckets;        /// Number of Buckets in buckets array
        uint64_t     firstKey;          /// First key of this Subrange
        uint64_t     lastKey;           /// Last key of this Subrange
        uint64_t     totalBytes;        /// Sum of all Buckets' totalBytes
        uint64_t     totalReferents;    /// Sum of all Buckets' totalReferents
        uint32_t     totalChildren;     /// Number of Buckets with child != NULL
        LogTime      createTime;        /// LogTime of track() that created this

        DISALLOW_COPY_AND_ASSIGN(Subrange);
    };

    // private methods
    Subrange::BucketHandle findBucket(uint64_t key, LogTime *time = NULL);

    // TabletProfiler private variables
    Subrange*              root;                /// Root Subrange in our tree
    Subrange::BucketHandle findHint;            /// Optimisation for locality
    LogTime                lastTracked;         /// LogTime of last track() call
    uint64_t               totalTracked;        /// Total tracked referents
    uint64_t               totalTrackedBytes;   /// Total tracked bytes

    DISALLOW_COPY_AND_ASSIGN(TabletProfiler);
};

} // namespace

#endif // !RAMCLOUD_TABLETPROFILER_H
