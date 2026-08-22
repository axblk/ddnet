#include <base/mem.h>
#include <base/net.h>
#include <base/secure.h>

#include <engine/shared/network.h>
#include <engine/shared/packer.h>
#include <engine/shared/protocol.h>

#include <gtest/gtest.h>

#include <chrono>
#include <vector>

using namespace std::chrono_literals;

class CLegacyWireCapture : public testing::Test
{
	NETSOCKET m_ReceiveSocket = nullptr;
	NETSOCKET m_SendSocket = nullptr;
	NETADDR m_Target = {};

protected:
	static constexpr SECURITY_TOKEN TOKEN_VALUE = 0x12345678;
	static constexpr int ACK = 0x123;

	void SetUp() override
	{
		NETADDR BindAddress = {};
		BindAddress.type = NETTYPE_IPV4;
		do
		{
			BindAddress.port = secure_rand_below(65535 - 1024) + 1024;
		} while(!(m_ReceiveSocket = net_udp_create(BindAddress)));
		NETADDR SendAddress = {};
		SendAddress.type = NETTYPE_IPV4;
		m_SendSocket = net_udp_create(SendAddress);
		ASSERT_NE(m_SendSocket, nullptr);
		ASSERT_FALSE(net_addr_from_str(&m_Target, "127.0.0.1"));
		m_Target.port = BindAddress.port;
	}

	void TearDown() override
	{
		if(m_ReceiveSocket)
			net_udp_close(m_ReceiveSocket);
		if(m_SendSocket)
			net_udp_close(m_SendSocket);
	}

	std::vector<unsigned char> Receive()
	{
		if(net_socket_read_wait(m_ReceiveSocket, 10s) != 1)
		{
			ADD_FAILURE() << "timed out waiting for loopback UDP capture";
			return {};
		}
		NETADDR Address;
		unsigned char *pData;
		const int Size = net_udp_recv(m_ReceiveSocket, &Address, &pData);
		if(Size <= 0)
		{
			ADD_FAILURE() << "failed to receive loopback UDP capture";
			return {};
		}
		return {pData, pData + Size};
	}

	std::vector<unsigned char> CaptureControl(int Control, const void *pExtra, int ExtraSize)
	{
		CNetBase::SendControlMsg(m_SendSocket, &m_Target, ACK, Control, pExtra, ExtraSize, TOKEN_VALUE);
		return Receive();
	}

	std::vector<unsigned char> CaptureMessage(const CPacker &Message, int ChunkFlags, int Sequence)
	{
		CNetPacketConstruct Packet = {};
		Packet.m_Ack = ACK;
		Packet.m_NumChunks = 1;
		CNetChunkHeader Header;
		Header.m_Flags = ChunkFlags;
		Header.m_Size = Message.Size();
		Header.m_Sequence = Sequence;
		unsigned char *pData = Header.Pack(Packet.m_aChunkData);
		mem_copy(pData, Message.Data(), Message.Size());
		Packet.m_DataSize = pData + Message.Size() - Packet.m_aChunkData;
		CNetBase::SendPacket(m_SendSocket, &m_Target, &Packet, TOKEN_VALUE);
		return Receive();
	}
};

TEST_F(CLegacyWireCapture, ConnectAndDisconnectGolden)
{
	const std::vector<unsigned char> Connect = {
		0x11, 0x23, 0x00, NET_CTRLMSG_CONNECT, 'T', 'K', 'E', 'N', 0x12, 0x34, 0x56, 0x78};
	EXPECT_EQ(CaptureControl(NET_CTRLMSG_CONNECT, SECURITY_TOKEN_MAGIC, sizeof(SECURITY_TOKEN_MAGIC)), Connect);

	static constexpr char REASON[] = "bye";
	const std::vector<unsigned char> Disconnect = {
		0x11, 0x23, 0x00, NET_CTRLMSG_CLOSE, 'b', 'y', 'e', 0x00, 0x12, 0x34, 0x56, 0x78};
	EXPECT_EQ(CaptureControl(NET_CTRLMSG_CLOSE, REASON, sizeof(REASON)), Disconnect);
}

TEST_F(CLegacyWireCapture, SnapshotInputMapAndRconGolden)
{
	CPacker Snapshot;
	Snapshot.Reset();
	Snapshot.AddInt((NETMSG_SNAPSINGLE << 1) | 1);
	Snapshot.AddInt(123);
	Snapshot.AddInt(2);
	Snapshot.AddInt(0x01020304);
	Snapshot.AddInt(4);
	static constexpr unsigned char SNAPSHOT_DATA[] = {0xde, 0xad, 0xbe, 0xef};
	Snapshot.AddRaw(SNAPSHOT_DATA, sizeof(SNAPSHOT_DATA));
	const std::vector<unsigned char> SnapshotGolden = {
		0x01, 0x23, 0x01, 0x00, 0x0d,
		0x0f, 0xbb, 0x01, 0x02, 0x84, 0x8c, 0x90, 0x10, 0x04, 0xde, 0xad, 0xbe, 0xef,
		0x12, 0x34, 0x56, 0x78};
	EXPECT_EQ(CaptureMessage(Snapshot, 0, 0), SnapshotGolden);

	CPacker Input;
	Input.Reset();
	Input.AddInt((NETMSG_INPUT << 1) | 1);
	Input.AddInt(100);
	Input.AddInt(101);
	Input.AddInt(8);
	Input.AddInt(0x11223344);
	Input.AddInt(-7);
	const std::vector<unsigned char> InputGolden = {
		0x01, 0x23, 0x01, 0x00, 0x0c,
		0x21, 0xa4, 0x01, 0xa5, 0x01, 0x08, 0x84, 0xcd, 0x91, 0x92, 0x02, 0x46,
		0x12, 0x34, 0x56, 0x78};
	EXPECT_EQ(CaptureMessage(Input, 0, 0), InputGolden);

	CPacker Map;
	Map.Reset();
	Map.AddInt((NETMSG_MAP_DATA << 1) | 1);
	Map.AddInt(1);
	Map.AddInt(0x123456);
	Map.AddInt(2);
	Map.AddInt(4);
	static constexpr unsigned char MAP_DATA[] = {0xca, 0xfe, 0xba, 0xbe};
	Map.AddRaw(MAP_DATA, sizeof(MAP_DATA));
	const std::vector<unsigned char> MapGolden = {
		0x01, 0x23, 0x01, 0x40, 0x0c, 0x05,
		0x07, 0x01, 0x96, 0xd1, 0x91, 0x01, 0x02, 0x04, 0xca, 0xfe, 0xba, 0xbe,
		0x12, 0x34, 0x56, 0x78};
	EXPECT_EQ(CaptureMessage(Map, NET_CHUNKFLAG_VITAL, 5), MapGolden);

	CPacker Rcon;
	Rcon.Reset();
	Rcon.AddInt((NETMSG_RCON_CMD << 1) | 1);
	Rcon.AddString("status");
	const std::vector<unsigned char> RconGolden = {
		0x01, 0x23, 0x01, 0x40, 0x08, 0x06,
		0x23, 's', 't', 'a', 't', 'u', 's', 0x00,
		0x12, 0x34, 0x56, 0x78};
	EXPECT_EQ(CaptureMessage(Rcon, NET_CHUNKFLAG_VITAL, 6), RconGolden);
}

TEST(Net, Ipv4AndIpv6Work)
{
	NETADDR Bindaddr = {};
	NETSOCKET Socket1;
	NETSOCKET Socket2;

	Bindaddr.type = NETTYPE_IPV4 | NETTYPE_IPV6;
	Socket2 = net_udp_create(Bindaddr);
	do
	{
		Bindaddr.port = secure_rand_below(65535 - 1024) + 1024;
	} while(!(Socket1 = net_udp_create(Bindaddr)));

	NETADDR LocalhostV4;
	NETADDR LocalhostV6;
	NETADDR TargetV4;
	NETADDR TargetV6;
	ASSERT_FALSE(net_addr_from_str(&LocalhostV4, "127.0.0.1"));
	ASSERT_FALSE(net_addr_from_str(&LocalhostV6, "[::1]"));
	TargetV4 = LocalhostV4;
	TargetV6 = LocalhostV6;
	TargetV4.port = Bindaddr.port;
	TargetV6.port = Bindaddr.port;

	NETADDR Addr;
	unsigned char *pData;

	EXPECT_EQ(net_udp_send(Socket2, &TargetV4, "abc", 3), 3);

	EXPECT_EQ(net_socket_read_wait(Socket1, 10s), 1);
	ASSERT_EQ(net_udp_recv(Socket1, &Addr, &pData), 3);
	Addr.port = 0;
	EXPECT_EQ(Addr, LocalhostV4);
	EXPECT_EQ(mem_comp(pData, "abc", 3), 0);

	EXPECT_EQ(net_udp_send(Socket2, &TargetV6, "def", 3), 3);

	EXPECT_EQ(net_socket_read_wait(Socket1, 10s), 1);
	ASSERT_EQ(net_udp_recv(Socket1, &Addr, &pData), 3);
	Addr.port = 0;
	EXPECT_EQ(Addr, LocalhostV6);
	EXPECT_EQ(mem_comp(pData, "def", 3), 0);

	net_udp_close(Socket1);
	net_udp_close(Socket2);
}
