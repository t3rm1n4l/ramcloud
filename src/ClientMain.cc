/* Copyright (c) 2009-2011 Stanford University
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

#include <stdio.h>
#include <string.h>
#include <getopt.h>
#include <assert.h>

#include "ClusterMetrics.h"
#include "Cycles.h"
#include "ShortMacros.h"
#include "Crc32C.h"
#include "ObjectFinder.h"
#include "OptionParser.h"
#include "RamCloud.h"
#include "Tub.h"

using namespace RAMCloud;

/*
 * If true, add the table and object ids to every object, calculate and
 * append a checksum, and verify the whole package when recovery is done.
 * The crc is the first 4 bytes of the object. The tableId and objectId
 * are the last 16 bytes.
 */
bool verify = false;

/*
 * Speed up recovery insertion with the single-shot FillWithTestData RPC.
 */
bool fillWithTestData = false;

int
main(int argc, char *argv[])
try
{
    int count, removeCount;
    uint32_t objectDataSize;
    uint32_t tableCount;
    uint32_t skipCount;
    uint64_t b;

    // need external context to set log levels with OptionParser
    Context context(true);
    Context::Guard _(context);

    OptionsDescription clientOptions("Client");
    clientOptions.add_options()
        ("fast,f",
         ProgramOptions::bool_switch(&fillWithTestData),
         "Use a single fillWithTestData rpc to insert recovery objects.")
        ("tables,t",
         ProgramOptions::value<uint32_t>(&tableCount)->
            default_value(1),
         "The number of tables to create with number objects on the master.")
        ("skip,k",
         ProgramOptions::value<uint32_t>(&skipCount)->
            default_value(1),
         "The number of empty tables to create per real table."
         "An enormous hack to create partitions on the crashed master.")
        ("number,n",
         ProgramOptions::value<int>(&count)->
            default_value(1024),
         "The number of values to insert.")
        ("removals,r",
         ProgramOptions::value<int>(&removeCount)->default_value(0),
         "The number of values inserted to remove (creating tombstones).")
        ("size,s",
         ProgramOptions::value<uint32_t>(&objectDataSize)->
            default_value(1024),
         "Number of bytes to insert per object during insert phase.")
        ("verify,v",
         ProgramOptions::bool_switch(&verify),
         "Verify the contents of all objects after recovery completes.");

    OptionParser optionParser(clientOptions, argc, argv);
    Context::get().transportManager->setTimeout(
            optionParser.options.getTransportTimeout());

    LOG(NOTICE, "client: Connecting to %s",
        optionParser.options.getCoordinatorLocator().c_str());

    RamCloud client(context,
                    optionParser.options.getCoordinatorLocator().c_str());

    b = Cycles::rdtsc();
    client.createTable("test");
    uint32_t table;
    table = client.openTable("test");
    LOG(NOTICE, "create+open table took %lu ticks", Cycles::rdtsc() - b);

    b = Cycles::rdtsc();
    client.ping(optionParser.options.getCoordinatorLocator().c_str(),
                12345, 100000000);
    LOG(NOTICE, "coordinator ping took %lu ticks", Cycles::rdtsc() - b);

    b = Cycles::rdtsc();
    client.ping(table, 42, 12345, 100000000);
    LOG(NOTICE, "master ping took %lu ticks", Cycles::rdtsc() - b);

    b = Cycles::rdtsc();
    client.write(table, 42, "Hello, World!", 14);
    LOG(NOTICE, "write took %lu ticks", Cycles::rdtsc() - b);

    b = Cycles::rdtsc();
    const char *value = "0123456789012345678901234567890"
        "123456789012345678901234567890123456789";
    client.write(table, 43, value, downCast<uint32_t>(strlen(value) + 1));
    LOG(NOTICE, "write took %lu ticks", Cycles::rdtsc() - b);

    Buffer buffer;
    b = Cycles::rdtsc();
    uint32_t length;

    client.read(table, 43, &buffer);
    LOG(NOTICE, "read took %lu ticks", Cycles::rdtsc() - b);

    length = buffer.getTotalLength();
    LOG(NOTICE, "Got back [%s] len %u",
        static_cast<const char*>(buffer.getRange(0, length)),
        length);

    client.read(table, 42, &buffer);
    LOG(NOTICE, "read took %lu ticks", Cycles::rdtsc() - b);
    length = buffer.getTotalLength();
    LOG(NOTICE, "Got back [%s] len %u",
        static_cast<const char*>(buffer.getRange(0, length)),
        length);

    b = Cycles::rdtsc();
    uint64_t id = 0xfffffff;
    id = client.create(table, "Hello, World?", 14);
    LOG(NOTICE, "insert took %lu ticks", Cycles::rdtsc() - b);
    LOG(NOTICE, "Got back [%lu] id", id);

    b = Cycles::rdtsc();
    client.read(table, id, &buffer);
    LOG(NOTICE, "read took %lu ticks", Cycles::rdtsc() - b);
    length = buffer.getTotalLength();
    LOG(NOTICE, "Got back [%s] len %u",
        static_cast<const char*>(buffer.getRange(0, length)),
        length);

    char val[objectDataSize];
    memset(val, 0xcc, objectDataSize);
    id = 0xfffffff;

    LOG(NOTICE, "Performing %u inserts of %u byte objects",
        count, objectDataSize);
    uint64_t* ids = static_cast<uint64_t*>(malloc(sizeof(ids[0]) * count));
    b = Cycles::rdtsc();
    for (int j = 0; j < count; j++)
        ids[j] = client.create(table, val, downCast<uint32_t>(strlen(val) + 1));
    LOG(NOTICE, "%d inserts took %lu ticks", count, Cycles::rdtsc() - b);
    LOG(NOTICE, "avg insert took %lu ticks", (Cycles::rdtsc() - b) / count);

    LOG(NOTICE, "Reading one of the objects just inserted");
    client.read(table, ids[0], &buffer);

    LOG(NOTICE, "Performing %u removals of objects just inserted", removeCount);
    for (int j = 0; j < count && j < removeCount; j++)
            client.remove(table, ids[j]);

    client.dropTable("test");

    return 0;
} catch (RAMCloud::ClientException& e) {
    fprintf(stderr, "RAMCloud exception: %s\n", e.str().c_str());
    return 1;
} catch (RAMCloud::Exception& e) {
    fprintf(stderr, "RAMCloud exception: %s\n", e.str().c_str());
    return 1;
}
