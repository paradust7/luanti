// Luanti
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2013 celeron55, Perttu Ahola <celeron55@gmail.com>

#include "test.h"

#include "log.h"
#include "porting.h"
#include "settings.h"
#include "util/serialize.h"
#include "network/connection.h"
#include "network/inmemorytransport.h"
#include "network/peerhandler.h"
#include "network/mtp/internal.h"
#include "network/networkexceptions.h"
#include "network/networkpacket.h"

class TestConnection : public TestBase {
public:
	TestConnection()
	{
		if (INTERNET_SIMULATOR == false)
			TestManager::registerTestModule(this);
	}

	const char *getName() { return "TestConnection"; }

	void runTests(IGameDef *gamedef);

	void testNetworkPacketSerialize();
	void testHelpers();
	void testConnectSendReceive();
	void testInMemoryTransport();
};

static TestConnection g_test_instance;

void TestConnection::runTests(IGameDef *gamedef)
{
	TEST(testNetworkPacketSerialize);
	TEST(testHelpers);
	TEST(testConnectSendReceive);
	TEST(testInMemoryTransport);
}

////////////////////////////////////////////////////////////////////////////////

struct Handler : public con::PeerHandler
{
	Handler(const char *a_name) : name(a_name) {}

	void peerAdded(session_t peer_id, const Address &address) override
	{
		infostream << "Handler(" << name << ")::peerAdded(): "
			"id=" << peer_id << std::endl;
		last_id = peer_id;
		count++;
	}

	void peerRemoved(session_t peer_id, bool is_timeout, const Address &address) override
	{
		infostream << "Handler(" << name << ")::peerRemoved(): "
			"id=" << peer_id << ", timeout=" << is_timeout << std::endl;
		last_id = peer_id;
		last_timeout = is_timeout;
		count--;
	}

	s32 count = 0;
	u16 last_id = 0;
	bool last_timeout = false;
	const char *name;
};

void TestConnection::testNetworkPacketSerialize()
{
	const static u8 expected[] = {
		0x00, 0x7b,
		0x00, 0x02, 0xd8, 0x42, 0xdf, 0x9a
	};

	if (sizeof(wchar_t) == 2)
		warningstream << __FUNCTION__ << " may fail on this platform." << std::endl;

	{
		NetworkPacket pkt(123, 0);

		// serializing wide strings should do surrogate encoding, we test that here
		pkt << std::wstring(L"\U00020b9a");

		auto buf = pkt.oldForgePacket();
		UASSERTEQ(int, buf.getSize(), sizeof(expected));
		UASSERT(!memcmp(expected, &buf[0], buf.getSize()));
	}

	{
		NetworkPacket pkt;
		pkt.putRawPacket(expected, sizeof(expected), 0);

		// same for decoding
		std::wstring pkt_s;
		pkt >> pkt_s;

		UASSERT(pkt_s == L"\U00020b9a");

		// Test for out-of-range reads
		UASSERT(pkt.getRemainingBytes() == 0);
		EXCEPTION_CHECK(PacketError, pkt.readLongString());
		EXCEPTION_CHECK(PacketError, pkt.skip((u32)-1));
		pkt.seek(0);
		UASSERTEQ(u32, pkt.getRemainingBytes(), sizeof(expected) - 2 /* u16 command */);
	}
}

void TestConnection::testHelpers()
{
	// Some constants for testing
	u32 proto_id = 0x12345678;
	session_t peer_id = 123;
	u8 channel = 2;
	SharedBuffer<u8> data1(1);
	data1[0] = 100;
	Address a(127,0,0,1, 10);
	const u16 seqnum = 34352;

	con::BufferedPacketPtr p1 = con::makePacket(a, data1,
			proto_id, peer_id, channel);
	/*
		We should now have a packet with this data:
		Header:
			[0] u32 protocol_id
			[4] session_t sender_peer_id
			[6] u8 channel
		Data:
			[7] u8 data1[0]
	*/
	UASSERT(readU32(&p1->data[0]) == proto_id);
	UASSERT(readU16(&p1->data[4]) == peer_id);
	UASSERT(readU8(&p1->data[6]) == channel);
	UASSERT(readU8(&p1->data[7]) == data1[0]);

	//infostream<<"initial data1[0]="<<((u32)data1[0]&0xff)<<std::endl;

	SharedBuffer<u8> p2 = con::makeReliablePacket(data1, seqnum);

	/*infostream<<"p2.getSize()="<<p2.getSize()<<", data1.getSize()="
			<<data1.getSize()<<std::endl;
	infostream<<"readU8(&p2[3])="<<readU8(&p2[3])
			<<" p2[3]="<<((u32)p2[3]&0xff)<<std::endl;
	infostream<<"data1[0]="<<((u32)data1[0]&0xff)<<std::endl;*/

	UASSERT(p2.getSize() == 3 + data1.getSize());
	UASSERT(readU8(&p2[0]) == con::PACKET_TYPE_RELIABLE);
	UASSERT(readU16(&p2[1]) == seqnum);
	UASSERT(readU8(&p2[3]) == data1[0]);
}


void TestConnection::testConnectSendReceive()
{

	constexpr u32 timeout_ms = 100;

	/*
		Test some real connections

		NOTE: This mostly tests the legacy interface.
	*/

	Handler hand_server("server");
	Handler hand_client("client");

	infostream << "** Creating server Connection" << std::endl;
	con::NetworkOverrides server_overrides;
	server_overrides.timeout = 5.0f;
	server_overrides.bind_port = 30001;
	auto server_con = con::createServer(&hand_server, false, server_overrides);
	con::IConnection &server = *server_con;

	Address server_address = server.getBindAddress();
	if (server_address.isAny()) {
		server_address = Address(127, 0, 0, 1, 30001);
	}

	infostream << "** Creating client Connection" << std::endl;
	con::NetworkOverrides client_overrides;
	client_overrides.timeout = 5.0f;
	auto client_con = con::createClient(&hand_client, false,
			server_address.isIPv6(), client_overrides);
	con::IConnection &client = *client_con;

	UASSERT(hand_server.count == 0);
	UASSERT(hand_client.count == 0);

	sleep_ms(50);

	infostream << "** running client.Connect()" << std::endl;
	client.Connect(server_address);

	sleep_ms(50);

	// Client should not have added client yet
	UASSERT(hand_client.count == 0);

	NetworkPacket pkt;
	infostream << "** running client.Receive()" << std::endl;
	if (client.ReceiveTimeoutMs(&pkt, timeout_ms)) {
		infostream << "** Client received: peer_id=" << pkt.getPeerId()
			<< ", size=" << pkt.getSize() << std::endl;
	}

	// Client should have added server now
	UASSERT(hand_client.count == 1);
	UASSERT(hand_client.last_id == 1);
	// Server should not have added client yet
	UASSERT(hand_server.count == 0);

	sleep_ms(100);

	NetworkPacket pkt1;
	infostream << "** running server.Receive()" << std::endl;
	if (server.ReceiveTimeoutMs(&pkt, timeout_ms)) {
		infostream << "** Server received: peer_id=" << pkt.getPeerId()
			<< ", size=" << pkt.getSize()
			<< std::endl;
	}
	else {
		// No actual data received, but the client has
		// probably been connected
	}

	// Client should be the same
	UASSERT(hand_client.count == 1);
	UASSERT(hand_client.last_id == 1);
	// Server should have the client
	UASSERT(hand_server.count == 1);
	UASSERT(hand_server.last_id >= 2);

	//sleep_ms(50);

	while (client.Connected() == false) {
		NetworkPacket pkt;
		infostream << "** running client.Receive()" << std::endl;
		if (client.TryReceive(&pkt)) {
			infostream << "** Client received: peer_id=" << pkt.getPeerId()
				<< ", size=" << pkt.getSize() << std::endl;
		}
		sleep_ms(50);
	}

	sleep_ms(50);

	NetworkPacket pkt2;
	infostream << "** running server.Receive()" << std::endl;
	if (server.ReceiveTimeoutMs(&pkt, timeout_ms)) {
		infostream << "** Server received: peer_id=" << pkt.getPeerId()
			<< ", size=" << pkt.getSize()
			<< std::endl;
	}

	/*
		Simple send-receive test
	*/
	{
		NetworkPacket pkt(0x4b, 0);
		pkt.putRawString("Hello World !", 14);

		auto sentdata = pkt.oldForgePacket();

		infostream<<"** running client.Send()"<<std::endl;
		client.Send(PEER_ID_SERVER, 0, &pkt, true);

		sleep_ms(50);

		NetworkPacket recvpacket;
		infostream << "** running server.Receive()" << std::endl;
		UASSERT(server.ReceiveTimeoutMs(&recvpacket, timeout_ms));
		infostream << "** Server received: peer_id=" << pkt.getPeerId()
				<< ", size=" << pkt.getSize()
				<< ", data=" << pkt.getRemainingNoCopy()
				<< std::endl;

		auto recvdata = pkt.oldForgePacket();

		UASSERT(memcmp(*sentdata, *recvdata, recvdata.getSize()) == 0);
	}

	const session_t peer_id_client = hand_server.last_id;
	/*
		Send a large packet
	*/
	{
		const int datasize = 30000;

		NetworkPacket pkt(0xff, datasize);
		for (u16 i=0; i<datasize; i++) {
			pkt << static_cast<u8>(i/4);
		}
		// NOTE: There is only one offset counter, hence reset it before reading.
		pkt.seek(0);

		std::string_view raw = pkt.getRemainingNoCopy();
		UASSERTEQ(size_t, raw.size(), datasize);

		infostream << "Sending data (size=" << datasize << "):";
		for (int i = 0; i < datasize && i < 20; i++) {
			if (i % 2 == 0)
				infostream << " ";
			char buf[10];
			porting::mt_snprintf(buf, sizeof(buf), "%.2X",
				((int)raw[i]) & 0xff);
			infostream<<buf;
		}
		if (datasize > 20)
			infostream << "...";
		infostream << std::endl;

		auto sentdata = pkt.oldForgePacket();

		server.Send(peer_id_client, 0, &pkt, true);

		//sleep_ms(3000);

		Buffer<u8> recvdata;
		infostream << "** running client.Receive()" << std::endl;
		session_t peer_id = 132;
		u16 size = 0;
		bool received = false;
		u64 timems0 = porting::getTimeMs();
		for (;;) {
			if (porting::getTimeMs() - timems0 > 5000 || received)
				break;
			NetworkPacket pkt;
			if (client.ReceiveTimeoutMs(&pkt, timeout_ms)) {
				size = pkt.getSize();
				peer_id = pkt.getPeerId();
				recvdata = pkt.oldForgePacket();
				received = true;
			}
			sleep_ms(10);
		}
		UASSERT(received);
		infostream << "** Client received: peer_id=" << peer_id
			<< ", size=" << size << std::endl;

		infostream << "Received data (size=" << size << "): ";
		for (int i = 0; i < size && i < 20; i++) {
			if (i % 2 == 0)
				infostream << " ";
			char buf[10];
			porting::mt_snprintf(buf, sizeof(buf), "%.2X", ((int)(recvdata[i])) & 0xff);
			infostream << buf;
		}
		if (size > 20)
			infostream << "...";
		infostream << std::endl;

		UASSERT(memcmp(*sentdata, *recvdata, recvdata.getSize()) == 0);
		UASSERT(peer_id == PEER_ID_SERVER);
	}

	// Check peer handlers
	UASSERT(hand_client.count == 1);
	UASSERT(hand_client.last_id == 1);
	UASSERT(hand_server.count == 1);
	UASSERT(hand_server.last_id >= 2);
}


// Calls Receive() on the connection until the condition is true, or a few
// seconds have passed. Any data received is ignored.
template <typename F>
static bool receiveUntil(con::IConnection &con, F condition)
{
	const u64 start = porting::getTimeMs();
	while (!condition()) {
		if (porting::getTimeMs() - start > 5000)
			return false;
		NetworkPacket pkt;
		con.ReceiveTimeoutMs(&pkt, 10);
	}
	return true;
}

void TestConnection::testInMemoryTransport()
{
	constexpr u32 timeout_ms = 1000;
	const Address server_address(127, 0, 0, 1, 30000);

	Handler hand_server("server");
	Handler hand_client("client");

	con::NetworkOverrides overrides;
	overrides.transport = "memory";
	std::unique_ptr<con::IConnection> server = con::createServer(
			&hand_server, true, overrides);

	// Connects the client, and lets both handlers see the new peers
	const auto connect = [&] (con::IConnection &client) {
		const s32 client_count = hand_client.count;
		const s32 server_count = hand_server.count;
		client.Connect(server_address);
		UASSERT(client.Connected());
		UASSERT(receiveUntil(client, [&] { return hand_client.count == client_count + 1; }));
		UASSERT(receiveUntil(*server, [&] { return hand_server.count == server_count + 1; }));
	};

	// Returns a new client connected to the server
	const auto connect_client = [&] () {
		std::unique_ptr<con::IConnection> client = con::createClient(
				&hand_client, true, false, overrides);
		connect(*client);
		return client;
	};

	std::unique_ptr<con::IConnection> client = connect_client();
	UASSERTEQ(s32, hand_client.count, 1);
	UASSERTEQ(u16, hand_client.last_id, PEER_ID_SERVER);
	UASSERT(client->getRemoteAddress() == server_address);
	UASSERT(client->getPeerStat(PEER_ID_SERVER, con::AVG_RTT) == 0.0f);
	const session_t client_id = hand_server.last_id;
	UASSERT(client_id >= 2);

	// Packets arrive in order, regardless of channel and reliability
	for (u16 i = 0; i < 100; i++) {
		NetworkPacket pkt(0x10, 2);
		pkt << i;
		client->Send(PEER_ID_SERVER, i % 3, &pkt, i % 2 == 0);
	}
	for (u16 i = 0; i < 100; i++) {
		NetworkPacket pkt;
		UASSERT(server->ReceiveTimeoutMs(&pkt, timeout_ms));
		UASSERTEQ(session_t, pkt.getPeerId(), client_id);
		UASSERTEQ(u16, pkt.getCommand(), 0x10);
		u16 j;
		pkt >> j;
		UASSERTEQ(u16, j, i);
	}

	// Large packets are not a problem, even unreliable ones
	{
		const u32 datasize = 1000000;
		NetworkPacket pkt(0xff, datasize);
		for (u32 i = 0; i < datasize; i++)
			pkt << static_cast<u8>(i * 7);
		auto sentdata = pkt.oldForgePacket();
		server->Send(client_id, 1, &pkt, false);

		NetworkPacket recvpkt;
		UASSERT(client->ReceiveTimeoutMs(&recvpkt, timeout_ms));
		UASSERTEQ(session_t, recvpkt.getPeerId(), PEER_ID_SERVER);
		auto recvdata = recvpkt.oldForgePacket();
		UASSERTEQ(size_t, recvdata.getSize(), sentdata.getSize());
		UASSERT(memcmp(*sentdata, *recvdata, recvdata.getSize()) == 0);
	}

	// The server kicks the client. Packets sent before are still delivered.
	{
		NetworkPacket pkt(0x20, 0);
		server->Send(client_id, 0, &pkt, true);
		server->DisconnectPeer(client_id);
		UASSERT(receiveUntil(*server, [&] { return hand_server.count == 0; }));
		UASSERT(!hand_server.last_timeout);

		NetworkPacket recvpkt;
		UASSERT(client->ReceiveTimeoutMs(&recvpkt, timeout_ms));
		UASSERTEQ(u16, recvpkt.getCommand(), 0x20);
		UASSERT(receiveUntil(*client, [&] { return hand_client.count == 0; }));
		UASSERT(!hand_client.last_timeout);
		UASSERT(!client->Connected());
	}

	// The client connects again, then leaves without disconnecting
	connect(*client);
	client.reset();
	UASSERT(receiveUntil(*server, [&] { return hand_server.count == 0; }));
	UASSERT(hand_server.last_timeout);
	hand_client.count = 0;

	// The client disconnects, then leaves
	client = connect_client();
	client->Disconnect();
	client.reset();
	UASSERT(receiveUntil(*server, [&] { return hand_server.count == 0; }));
	UASSERT(!hand_server.last_timeout);
	hand_client.count = 0;

	// Only one client at a time. Another one fails like a timed out connect.
	client = connect_client();
	{
		Handler hand_second("second client");
		auto second = con::createClient(
				&hand_second, true, false, overrides);
		second->Connect(server_address);
		UASSERT(receiveUntil(*second, [&] { return hand_second.last_timeout; }));
		UASSERTEQ(s32, hand_second.count, 0);
		UASSERT(!second->Connected());
	}
	UASSERT(client->Connected());

	// The server leaves without disconnecting. What it sent before still arrives.
	{
		NetworkPacket pkt(0x21, 0);
		server->Send(hand_server.last_id, 0, &pkt, true);
	}
	server.reset();
	{
		NetworkPacket recvpkt;
		UASSERT(client->ReceiveTimeoutMs(&recvpkt, timeout_ms));
		UASSERTEQ(u16, recvpkt.getCommand(), 0x21);
	}
	UASSERT(receiveUntil(*client, [&] { return hand_client.count == 0; }));
	UASSERT(hand_client.last_timeout);
	UASSERT(!client->Connected());
	client.reset();

	// Without a server, connecting fails like a timed out connect.
	// Nothing is passed on while only one side is there.
	{
		Handler hand_lonely("lonely client");
		Handler hand_late("late server");
		auto lonely = con::createClient(
				&hand_lonely, true, false, overrides);
		lonely->Connect(server_address);
		UASSERT(receiveUntil(*lonely, [&] { return hand_lonely.last_timeout; }));
		UASSERTEQ(s32, hand_lonely.count, 0);
		NetworkPacket pkt(0x30, 0);
		lonely->Send(PEER_ID_SERVER, 0, &pkt, true);

		auto late = con::createServer(&hand_late, true, overrides);
		lonely->Send(PEER_ID_SERVER, 0, &pkt, true);
		NetworkPacket recvpkt;
		UASSERT(!late->ReceiveTimeoutMs(&recvpkt, 100));
		UASSERTEQ(s32, hand_late.count, 0);
		UASSERT(!lonely->Connected());

		// There can only be one in-memory server
		EXCEPTION_CHECK(con::ConnectionException,
				con::createServer(&hand_late, true, overrides));
	}
}
