/* Copyright (c) 2010-2011 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "TestUtil.h"
#include "MockFastTransport.h"
#include "MockSyscall.h"
#include "Tub.h"
#include "UdpDriver.h"

namespace RAMCloud {
class UdpDriverTest : public ::testing::Test {
  public:
    string exceptionMessage;
    ServiceLocator *serverLocator;
    IpAddress *serverAddress;
    UdpDriver *server;
    UdpDriver *client;
    MockSyscall* sys;
    Syscall *savedSyscall;
    TestLog::Enable* logEnabler;
    MockFastTransport *clientTransport, *serverTransport;

    UdpDriverTest()
        : exceptionMessage()
        , serverLocator(NULL)
        , serverAddress(NULL)
        , server(NULL)
        , client(NULL)
        , sys(NULL)
        , savedSyscall(NULL)
        , logEnabler(NULL)
        , clientTransport(NULL)
        , serverTransport(NULL)
    {
        savedSyscall = UdpDriver::sys;
        sys = new MockSyscall();
        UdpDriver::sys = sys;
        exceptionMessage = "no exception";
        serverLocator = new ServiceLocator("udp: host=localhost, port=8100");
        serverAddress = new IpAddress(*serverLocator);
        server = new UdpDriver(serverLocator);
        client = new UdpDriver;
        logEnabler = new TestLog::Enable();
        clientTransport = new MockFastTransport(client);
        serverTransport = new MockFastTransport(server);
    }

    ~UdpDriverTest() {
        delete serverLocator;
        serverLocator = NULL;
        delete serverAddress;
        serverAddress = NULL;
        // Note: deleting the transport deletes the driver implicitly.
        if (serverTransport != NULL) {
            delete serverTransport;
            serverTransport = NULL;
            server = NULL;
        }
        delete clientTransport;
        clientTransport = NULL;
        client = NULL;
        delete sys;
        sys = NULL;
        UdpDriver::sys = savedSyscall;
        delete logEnabler;
    }

    void sendMessage(UdpDriver *driver, IpAddress *address,
            const char *header, const char *payload) {
        Buffer message;
        Buffer::Chunk::appendToBuffer(&message, payload,
                                      downCast<uint32_t>(strlen(payload)));
        Buffer::Iterator iterator(message);
        driver->sendPacket(address, header, downCast<uint32_t>(strlen(header)),
                           &iterator);
    }

    // Used to wait for data to arrive on a driver by invoking the
    // dispatcher's polling loop; gives up if a long time goes by with
    // no data.
    const char *receivePacket(MockFastTransport *transport) {
        transport->packetData.clear();
        uint64_t start = Cycles::rdtsc();
        while (true) {
            Context::get().dispatch->poll();
            if (transport->packetData.size() != 0) {
                return transport->packetData.c_str();
            }
            if (Cycles::toSeconds(Cycles::rdtsc() - start) > .1) {
                return "no packet arrived";
            }
        }
    }

  private:
    DISALLOW_COPY_AND_ASSIGN(UdpDriverTest);
};

TEST_F(UdpDriverTest, basics) {
    // Send a packet from a client-style driver to a server-style
    // driver.
    Buffer message;
    const char *testString = "This is a sample message";
    Buffer::Chunk::appendToBuffer(&message, testString,
            downCast<uint32_t>(strlen(testString)));
    Buffer::Iterator iterator(message);
    client->sendPacket(serverAddress, "header:", 7, &iterator);
    EXPECT_STREQ("header:This is a sample message",
            receivePacket(serverTransport));

    // Send a response back in the other direction.
    message.reset();
    Buffer::Chunk::appendToBuffer(&message, "response", 8);
    Buffer::Iterator iterator2(message);
    server->sendPacket(serverTransport->sender, "h:", 2, &iterator2);
    EXPECT_STREQ("h:response", receivePacket(clientTransport));
}

TEST_F(UdpDriverTest, constructor_errorInSocketCall) {
    sys->socketErrno = EPERM;
    try {
        UdpDriver server2(serverLocator);
    } catch (DriverException& e) {
        exceptionMessage = e.message;
    }
    EXPECT_EQ("UdpDriver couldn't create socket: "
                "Operation not permitted", exceptionMessage);
}

TEST_F(UdpDriverTest, constructor_socketInUse) {
    try {
        UdpDriver server2(serverLocator);
    } catch (DriverException& e) {
        exceptionMessage = e.message;
    }
    EXPECT_EQ("UdpDriver couldn't bind to locator "
            "'udp: host=localhost, port=8100': Address already in use",
            exceptionMessage);
}

TEST_F(UdpDriverTest, destructor_closeSocket) {
    // If the socket isn't closed, we won't be able to create another
    // UdpDriver that binds to the same socket.
    delete serverTransport;
    serverTransport = NULL;
    try {
        server = new UdpDriver(serverLocator);
        serverTransport = new MockFastTransport(server);
    } catch (DriverException& e) {
        exceptionMessage = e.message;
    }
    EXPECT_EQ("no exception", exceptionMessage);
}

TEST_F(UdpDriverTest, sendPacket_headerEmpty) {
    Buffer message;
    Buffer::Chunk::appendToBuffer(&message, "xyzzy", 5);
    Buffer::Iterator iterator(message);
    client->sendPacket(serverAddress, "", 0, &iterator);
    EXPECT_STREQ("xyzzy", receivePacket(serverTransport));
}

TEST_F(UdpDriverTest, sendPacket_payloadEmpty) {
    Buffer message;
    Buffer::Chunk::appendToBuffer(&message, "xyzzy", 5);
    Buffer::Iterator iterator(message);
    client->sendPacket(serverAddress, "header:", 7, &iterator);
    EXPECT_STREQ("header:xyzzy", receivePacket(serverTransport));
}

TEST_F(UdpDriverTest, sendPacket_multipleChunks) {
    Buffer message;
    Buffer::Chunk::appendToBuffer(&message, "xyzzy", 5);
    Buffer::Chunk::appendToBuffer(&message, "0123456789", 10);
    Buffer::Chunk::appendToBuffer(&message, "abc", 3);
    Buffer::Iterator iterator(message, 1, 23);
    client->sendPacket(serverAddress, "header:", 7, &iterator);
    EXPECT_STREQ("header:yzzy0123456789abc",
            receivePacket(serverTransport));
}

TEST_F(UdpDriverTest, sendPacket_errorInSend) {
    sys->sendmsgErrno = EPERM;
    Buffer message;
    Buffer::Chunk::appendToBuffer(&message, "xyzzy", 5);
    Buffer::Iterator iterator(message);
    try {
        client->sendPacket(serverAddress, "header:", 7, &iterator);
    } catch (DriverException& e) {
        exceptionMessage = e.message;
    }
    EXPECT_EQ("UdpDriver error sending to socket: "
            "Operation not permitted", exceptionMessage);
}

TEST_F(UdpDriverTest, ReadHandler_errorInRecv) {
    sys->recvfromErrno = EPERM;
    Driver::Received received;
    try {
        server->readHandler->handleFileEvent(
                Dispatch::FileEvent::READABLE);
    } catch (DriverException& e) {
        exceptionMessage = e.message;
    }
    EXPECT_EQ("UdpDriver error receiving from socket: "
            "Operation not permitted", exceptionMessage);
}

TEST_F(UdpDriverTest, ReadHandler_noPacketAvailable) {
    server->readHandler->handleFileEvent(
            Dispatch::FileEvent::READABLE);
    EXPECT_EQ("", serverTransport->packetData);
}

TEST_F(UdpDriverTest, ReadHandler_multiplePackets) {
    sendMessage(client, serverAddress, "header:", "first");
    sendMessage(client, serverAddress, "header:", "second");
    sendMessage(client, serverAddress, "header:", "third");
    EXPECT_STREQ("header:first", receivePacket(serverTransport));
    EXPECT_STREQ("header:second", receivePacket(serverTransport));
    EXPECT_STREQ("header:third", receivePacket(serverTransport));
}

}  // namespace RAMCloud
