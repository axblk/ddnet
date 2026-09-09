/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SHARED_NETWORK_H
#define ENGINE_SHARED_NETWORK_H

#include "ringbuffer.h"
#include "stun.h"

#include <base/hash.h>
#include <base/net.h>
#include <base/types.h>

#include <array>
#include <map>
#include <optional>
#include <vector>

class CHuffman;
class CNetBan;
#ifdef CONF_NETWORKING_QUIC
typedef struct DdnetNet CNet;
typedef struct DdnetNetEvent CNetEvent;
#endif
class CPacker;

/*

CURRENT:
	packet header: 3 bytes
		unsigned char flags_ack; // 6bit flags, 2bit ack
			0.6:   ORNCaaAA
			0.6.5: ORNCT-AA
			0.7:   --NORCAA

		O = flag compression
		R = flag resend
		N = flag connless
		C = flag control
		T = flag token (0.6.5 only not supported by ddnet)
		- = unused, should be zero
		a = should be zero otherwise it messes up the ack number
		A = bit of ack number


		unsigned char ack; // 8 bit ack
		unsigned char num_chunks; // 8 bit chunks

		(unsigned char padding[3])	// 24 bit extra in case it's a connection less packet
									// this is to make sure that it's compatible with the
									// old protocol

	chunk header: 2-3 bytes
		unsigned char flags_size; // 2bit flags, 6 bit size
		unsigned char size_seq; // 4bit size, 4bit seq
		(unsigned char seq;) // 8bit seq, if vital flag is set
*/

enum
{
	NETSENDFLAG_VITAL = 1 << 0,
	NETSENDFLAG_CONNLESS = 1 << 1,
	NETSENDFLAG_FLUSH = 1 << 2,
	NETSENDFLAG_EXTENDED = 1 << 3,
};

enum
{
	NETSTATE_OFFLINE = 0,
	NETSTATE_CONNECTING,
	NETSTATE_ONLINE,
};

enum
{
	NET_MAX_PACKETSIZE = 1400,
	NET_MAX_CONNLESS_PAYLOAD = NET_MAX_PACKETSIZE - 6,
	/**
	 * The maximum size of a chunk within a connection-oriented packet.
	 *
	 * This is 1023 because the size is packed into 10 bits in the chunk header.
	 */
	NET_MAX_CHUNK_SIZE = 1023,
	NET_MAX_CHUNKHEADERSIZE = 3,
	NET_PACKETHEADERSIZE = 3,
	NET_CONNLESS_EXTRA_SIZE = 4,
	NET_MAX_CLIENTS = 128,
	NET_MAX_CONSOLE_CLIENTS = 4,
	NET_MAX_SEQUENCE = 1 << 10,
	NET_MAX_PACKET_CHUNKS = 0xFF,

	NET_PACKETFLAG_UNUSED = 1 << 0,
	NET_PACKETFLAG_TOKEN = 1 << 1,
	NET_PACKETFLAG_CONTROL = 1 << 2,
	NET_PACKETFLAG_CONNLESS = 1 << 3,
	NET_PACKETFLAG_RESEND = 1 << 4,
	NET_PACKETFLAG_COMPRESSION = 1 << 5,
	// NOT SENT VIA THE NETWORK DIRECTLY:
	NET_PACKETFLAG_EXTENDED = 1 << 6,

	NET_CHUNKFLAG_VITAL = 1,
	NET_CHUNKFLAG_RESEND = 2,
	// Not a game message but a step of a map arriving on a QUIC stream of
	// its own: the header (see NetDecodeMapHeader), a piece of the map, its
	// end after the checksum matched, or a failure with the reason as data.
	NET_CHUNKFLAG_MAP_HEADER = 1 << 4,
	NET_CHUNKFLAG_MAP_DATA = 1 << 5,
	NET_CHUNKFLAG_MAP_END = 1 << 6,
	NET_CHUNKFLAG_MAP_FAILED = 1 << 7,
	NET_CHUNKFLAG_MAP = NET_CHUNKFLAG_MAP_HEADER | NET_CHUNKFLAG_MAP_DATA | NET_CHUNKFLAG_MAP_END | NET_CHUNKFLAG_MAP_FAILED,

	NET_CTRLMSG_KEEPALIVE = 0,
	NET_CTRLMSG_CONNECT = 1,
	NET_CTRLMSG_CONNECTACCEPT = 2,
	NET_CTRLMSG_ACCEPT = 3,
	NET_CTRLMSG_CLOSE = 4,

	NET_CONN_BUFFERSIZE = 1024 * 32,

	// Addresses tracked for `sv_connlimit`, evicted least recently used. The limit stops
	// applying once addresses are evicted before they reach `sv_connlimit`.
	NET_CONNLIMIT_IPS = 256,

	NET_TOKENCACHE_ADDRESSEXPIRY = 64,
	NET_TOKENCACHE_PACKETEXPIRY = 5,
};
enum
{
	NET_TOKEN_MAX = 0xffffffff,
	NET_TOKEN_NONE = NET_TOKEN_MAX,
	NET_TOKEN_MASK = NET_TOKEN_MAX,

	NET_TOKENREQUEST_DATASIZE = 512,
};

typedef int SECURITY_TOKEN;
typedef unsigned int TOKEN;

SECURITY_TOKEN ToSecurityToken(const unsigned char *pData);
void WriteSecurityToken(unsigned char *pData, SECURITY_TOKEN Token);

extern const unsigned char SECURITY_TOKEN_MAGIC[4];

enum
{
	NET_SECURITY_TOKEN_UNKNOWN = -1,
	NET_SECURITY_TOKEN_UNSUPPORTED = 0,
};

typedef int (*NETFUNC_DELCLIENT)(int ClientId, const char *pReason, void *pUser);
typedef int (*NETFUNC_NEWCLIENT_CON)(int ClientId, void *pUser);
typedef int (*NETFUNC_NEWCLIENT)(int ClientId, void *pUser, bool Sixup);
typedef int (*NETFUNC_NEWCLIENT_NOAUTH)(int ClientId, void *pUser);
typedef int (*NETFUNC_CLIENTREJOIN)(int ClientId, void *pUser, bool Sixup, bool VanillaAuth);

// What a map stream announces before the map itself.
struct CNetMapHeader
{
	char m_aName[256];
	unsigned m_Crc;
	uint64_t m_Size;
	SHA256_DIGEST m_Sha256;
};
// Takes the data of a NET_CHUNKFLAG_MAP_HEADER chunk apart. Always false
// without QUIC.
bool NetDecodeMapHeader(const void *pData, int Size, CNetMapHeader *pHeader);

struct CNetChunk
{
	// -1 means that it's a stateless packet
	// 0 on the client means the server
	int m_ClientId;
	NETADDR m_Address; // only used when client_id == -1
	int m_Flags;
	int m_DataSize;
	const void *m_pData;
	// only used if the flags contain NETSENDFLAG_EXTENDED and NETSENDFLAG_CONNLESS
	unsigned char m_aExtraData[NET_CONNLESS_EXTRA_SIZE];

	void AssertSizeSanity() const;
};

class CNetChunkHeader
{
public:
	int m_Flags;
	int m_Size;
	int m_Sequence;

	unsigned char *Pack(unsigned char *pData, int Split = 4) const;
	unsigned char *Unpack(unsigned char *pData, int Split = 4);
};

class CNetChunkResend
{
public:
	int m_Flags;
	int m_DataSize;
	unsigned char *m_pData;

	int m_Sequence;
	int64_t m_LastSendTime;
	int64_t m_FirstSendTime;
};

class CNetPacketConstruct
{
public:
	int m_Flags;
	int m_Ack;
	int m_NumChunks;
	int m_DataSize;
	unsigned char m_aChunkData[NET_MAX_PACKETSIZE - NET_PACKETHEADERSIZE];
	unsigned char m_aExtraData[NET_CONNLESS_EXTRA_SIZE];
};

enum class CONNECTIVITY
{
	UNKNOWN,
	CHECKING,
	UNREACHABLE,
	REACHABLE,
	ADDRESS_KNOWN,
};

class CStun
{
public:
	// Sends a datagram as it is; STUN goes over the socket everything else
	// uses, whoever owns it.
	typedef bool (*FSendRaw)(void *pUser, const NETADDR *pAddr, const void *pData, int Size);

private:
	class CProtocol
	{
		int m_Index;
		FSendRaw m_pfnSend;
		void *m_pUser;
		CStunData m_Stun;
		bool m_HaveStunServer = false;
		NETADDR m_StunServer;
		bool m_HaveAddr = false;
		NETADDR m_Addr;
		int64_t m_LastResponse = -1;
		int64_t m_NextTry = -1;
		int m_NumUnsuccessfulTries = -1;

	public:
		CProtocol(int Index, FSendRaw pfnSend, void *pUser);
		void FeedStunServer(NETADDR StunServer);
		void Refresh();
		void Update();
		bool OnPacket(NETADDR Addr, unsigned char *pData, int DataSize);
		CONNECTIVITY GetConnectivity(NETADDR *pGlobalAddr);
	};
	CProtocol m_aProtocols[2];

public:
	CStun(FSendRaw pfnSend, void *pUser);
	void FeedStunServer(NETADDR StunServer);
	void Refresh();
	void Update();
	bool OnPacket(NETADDR Addr, unsigned char *pData, int DataSize);
	CONNECTIVITY GetConnectivity(int NetType, NETADDR *pGlobalAddr);
};

class CNetConnection
{
	// TODO: is this needed because this needs to be aware of
	// the ack sequencing number and is also responsible for updating
	// that. this should be fixed.
	friend class CPacketChunkUnpacker;

public:
	enum class EState
	{
		OFFLINE,
		WANT_TOKEN,
		CONNECT,
		PENDING,
		ONLINE,
		ERROR,
	};

	SECURITY_TOKEN m_SecurityToken;

private:
	unsigned short m_Sequence;
	unsigned short m_Ack;
	unsigned short m_PeerAck;
	EState m_State = EState::OFFLINE;
	int m_RemoteClosed;
	bool m_BlockCloseMsg;
	bool m_UnknownSeq;

	CStaticRingBuffer<CNetChunkResend, NET_CONN_BUFFERSIZE> m_Buffer;

	int64_t m_LastUpdateTime;
	int64_t m_LastRecvTime;
	int64_t m_LastSendTime;
	int64_t m_LastResendTime;
	bool m_ResendRequested;

	char m_aErrorString[256];

	CNetPacketConstruct m_Construct;

	NETADDR m_aConnectAddrs[16];
	int m_NumConnectAddrs;
	NETADDR m_PeerAddr;
	NETSOCKET m_Socket;
	NETSTATS m_Stats;

	std::array<char, NETADDR_MAXSTRSIZE> m_aPeerAddrStr;
	std::array<char, NETADDR_MAXSTRSIZE> m_aPeerAddrStrNoPort;
	// client 0.7
	static TOKEN GenerateToken7(const NETADDR *pPeerAddr);
	class CNetBase *m_pNetBase;
	bool IsSixup() const { return m_Sixup; }

	//
	bool IsPeerAddress(const NETADDR &Addr) const;
	void SetPeerAddr(const NETADDR *pAddr);
	void ClearPeerAddr();
	void ResetStats();
	void SetError(const char *pString);
	void AckChunks(int Ack);

	int QueueChunkEx(int Flags, int DataSize, const void *pData, int Sequence);
	void SendConnect();
	void SendControl(int ControlMsg, const void *pExtra, int ExtraSize);
	void SendControlWithToken7(int ControlMsg, SECURITY_TOKEN ResponseToken);
	void ResendChunk(CNetChunkResend *pResend);
	void Resend();
	void AnswerResendRequest(int64_t Now);

public:
	bool m_TimeoutProtected;
	bool m_TimeoutSituation;

	void SetToken7(TOKEN Token);

	void Reset();
	void Init(NETSOCKET Socket, bool BlockCloseMsg);
	int Connect(const NETADDR *pAddr, int NumAddrs);
	int Connect7(const NETADDR *pAddr, int NumAddrs);
	void Disconnect(const char *pReason);

	int Update();
	int Flush();

	int Feed(CNetPacketConstruct *pPacket, NETADDR *pAddr, SECURITY_TOKEN SecurityToken = NET_SECURITY_TOKEN_UNSUPPORTED, SECURITY_TOKEN ResponseToken = NET_SECURITY_TOKEN_UNSUPPORTED);
	int QueueChunk(int Flags, int DataSize, const void *pData);

	const char *ErrorString();
	void SignalResend();
	EState State() const { return m_State; }
	const NETADDR *PeerAddress() const { return &m_PeerAddr; }
	const std::array<char, NETADDR_MAXSTRSIZE> &PeerAddressString(bool IncludePort) const
	{
		return IncludePort ? m_aPeerAddrStr : m_aPeerAddrStrNoPort;
	}
	void ConnectAddresses(const NETADDR **ppAddrs, int *pNumAddrs) const
	{
		*ppAddrs = m_aConnectAddrs;
		*pNumAddrs = m_NumConnectAddrs;
	}

	void ResetErrorString() { m_aErrorString[0] = 0; }
	const char *ErrorString() const { return m_aErrorString; }

	// Needed for GotProblems in NetClient
	int64_t LastRecvTime() const { return m_LastRecvTime; }
	int64_t ConnectTime() const { return m_LastUpdateTime; }

	int AckSequence() const { return m_Ack; }
	int SeqSequence() const { return m_Sequence; }
	int SecurityToken() const { return m_SecurityToken; }
	CStaticRingBuffer<CNetChunkResend, NET_CONN_BUFFERSIZE> *ResendBuffer() { return &m_Buffer; }

	void ResumeConnection(const NETADDR *pAddr, int Sequence, int Ack, SECURITY_TOKEN SecurityToken, CStaticRingBuffer<CNetChunkResend, NET_CONN_BUFFERSIZE> *pResendBuffer, bool Sixup);

	// anti spoof
	void DirectInit(const NETADDR &Addr, SECURITY_TOKEN SecurityToken, SECURITY_TOKEN Token, bool Sixup);
	void SetUnknownSeq() { m_UnknownSeq = true; }
	void SetSequence(int Sequence) { m_Sequence = Sequence; }

	bool m_Sixup;
	SECURITY_TOKEN m_Token;
};

class CConsoleNetConnection
{
public:
	enum class EState
	{
		OFFLINE,
		ONLINE,
		ERROR,
	};

private:
	EState m_State;

	NETADDR m_PeerAddr;
	std::array<char, NETADDR_MAXSTRSIZE> m_aPeerAddrStr;
	NETSOCKET m_Socket;

	char m_aBuffer[NET_MAX_PACKETSIZE];
	int m_BufferOffset;

	char m_aErrorString[256];

	bool m_LineEndingDetected;
	char m_aLineEnding[3];

	void SetPeerAddr(const NETADDR *pAddr);
	void ClearPeerAddr();

public:
	int Init(NETSOCKET Socket, const NETADDR *pAddr);
	void Disconnect(const char *pReason);

	EState State() const { return m_State; }
	const NETADDR *PeerAddress() const { return &m_PeerAddr; }
	const std::array<char, NETADDR_MAXSTRSIZE> &PeerAddressString() const { return m_aPeerAddrStr; }
	const char *ErrorString() const { return m_aErrorString; }

	void Reset();
	int Update();
	int Send(const char *pLine);
	int Recv(char *pLine, int MaxLength);
};

/**
 * Accepts a non-control packet containing one or more chunks and unpacks each chunk individually.
 * After a packet has been fed into the unpacker by calling @link FeedPacket @endlink, all chunks have
 * to be unpacked by calling @link UnpackNextChunk @endlink until the function returns `false`, before
 * the unpacker can be fed another packet.
 */
class CPacketChunkUnpacker
{
public:
	void FeedPacket(const NETADDR &Addr, const CNetPacketConstruct &Packet, CNetConnection *pConnection, int ClientId);
	bool UnpackNextChunk(CNetChunk *pChunk);
	void Reset();

private:
	bool m_Valid = false;
	NETADDR m_Addr;
	CNetConnection *m_pConnection;
	int m_CurrentChunk;
	// offset of the next chunk header in m_Data.m_aChunkData
	int m_CurrentOffset;
	int m_ClientId;
	CNetPacketConstruct m_Data;
};

// server side
#ifdef CONF_NETWORKING_QUIC
// Formats the address the network library binds to.
void BindAddrStr(const NETADDR &BindAddr, char *pBuffer, size_t BufferSize);
// What a connectionless packet from the library came over, by the scheme
// of its address.
enum class ENetConnless
{
	NONE,
	TW06,
	TW07,
	// A datagram as it is, which is how STUN answers arrive.
	RAW,
};
// The address a connectionless packet came from, out of the library's URL.
ENetConnless NetConnlessAddr(const char *pUrl, NETADDR *pAddr);
// Sends a connectionless chunk through the library: 0.7 for an address
// flagged NETTYPE_TW7, with the extended header when the chunk asks for it,
// and to everyone on the link for a broadcast address.
void NetSendConnless(CNet *pNet, const CNetChunk *pChunk);
#endif

class CNetServer
{
#ifdef CONF_NETWORKING_QUIC
	struct CPeer
	{
		enum
		{
			STATE_NONE,
			STATE_CONNECTED,
			STATE_TIMEOUT,
			STATE_TIMEOUT_CLEARED,
		};

		int m_State = STATE_NONE;
		// Stores a mapping from client IDs to peer IDs of the network library.
		// The opposite mapping is stored in the userdata of the library.
		uint64_t m_Id = -1;
		bool m_TimeoutProtected = false;
		// Connected over QUIC rather than 0.6 or 0.7 over UDP.
		bool m_Quic = false;
		NETADDR m_Address = {0};
		std::array<char, NETADDR_MAXSTRSIZE> m_aAddressStr = {};
		std::array<char, NETADDR_MAXSTRSIZE> m_aAddressStrNoPort = {};

		void Reset();
		void SetAddress(const NETADDR &Addr);
	};

	CNet *m_pNet = nullptr;
	CNetEvent *m_pNetEvent = nullptr;
	unsigned char m_aBuffer[NET_MAX_PACKETSIZE] = {0};

	NETADDR m_Address = {0};
	CNetBan *m_pNetBan = nullptr;
	int m_MaxClients = NET_MAX_CLIENTS;
	int m_MaxClientsPerIp = NET_MAX_CLIENTS;

	NETFUNC_NEWCLIENT m_pfnNewClient = nullptr;
	NETFUNC_NEWCLIENT_NOAUTH m_pfnNewClientNoAuth = nullptr;
	NETFUNC_DELCLIENT m_pfnDelClient = nullptr;
	NETFUNC_CLIENTREJOIN m_pfnClientRejoin = nullptr;
	void *m_pUser = nullptr;

	unsigned char m_aSecurityTokenSeed[16] = {0};

	// Next client ID to try.
	int m_NextClientId = 0;

	CPeer m_aPeers[NET_MAX_CLIENTS];

	// The Ed25519 seed the library identifies the server with, random per
	// start unless one was set before Open.
	unsigned char m_aIdentity[32] = {0};
	bool m_HasIdentity = false;
	// PEM files with the certificate chain and key shown to browsers;
	// empty for a certificate the library makes itself.
	char m_aTlsCert[IO_MAX_PATH_LENGTH] = "";
	char m_aTlsKey[IO_MAX_PATH_LENGTH] = "";

	// The maps SetMap() gave the library, kept to give them again after
	// Reopen().
	struct CMap
	{
		char m_aName[256];
		unsigned m_Crc;
		SHA256_DIGEST m_Sha256;
		std::vector<unsigned char> m_vData;
	};
	std::map<int, CMap> m_Maps;

	bool OpenLibrary();
	void Reopen();
	bool SetMapImpl(int MapId, const CMap &Map);
#else
	struct CSlot
	{
	public:
		CNetConnection m_Connection;
	};

	struct CSpamConn
	{
		NETADDR m_Addr;
		// start of the timespan the connections are counted in
		int64_t m_Time;
		// last connection, only used to pick the entry to evict
		int64_t m_LastSeen;
		int m_Conns;
	};

	NETADDR m_Address;
	NETSOCKET m_Socket;
	CNetBan *m_pNetBan;
	CSlot m_aSlots[NET_MAX_CLIENTS];
	int m_MaxClients = NET_MAX_CLIENTS;
	int m_MaxClientsPerIp;

	NETFUNC_NEWCLIENT m_pfnNewClient;
	NETFUNC_NEWCLIENT_NOAUTH m_pfnNewClientNoAuth;
	NETFUNC_DELCLIENT m_pfnDelClient;
	NETFUNC_CLIENTREJOIN m_pfnClientRejoin;
	void *m_pUser;

	unsigned char m_aSecurityTokenSeed[16];

	// vanilla connect flood detection
	int64_t m_VConnFirst;
	int m_VConnNum;

	// budgets for work unauthenticated peers can request, reset in Update():
	// `m_NumRecvPackets` per Recv() batch, the others per second
	int m_NumRecvPackets = 0;
	int64_t m_BudgetStart = 0;
	int m_NumPreConnDecompress = 0;
	int m_NumBanReplies = 0;

	CSpamConn m_aSpamConns[NET_CONNLIMIT_IPS] = {};

	CPacketChunkUnpacker m_PacketChunkUnpacker;
	CNetPacketConstruct m_RecvBuffer;

	void OnTokenCtrlMsg(NETADDR &Addr, int ControlMsg, const CNetPacketConstruct &Packet, int Slot);
	int OnSixupCtrlMsg(NETADDR &Addr, CNetChunk *pChunk, int ControlMsg, const CNetPacketConstruct &Packet, SECURITY_TOKEN &ResponseToken, SECURITY_TOKEN Token, int Slot);
	void OnPreConnMsg(NETADDR &Addr, CNetPacketConstruct &Packet, int Slot);
	bool ClientExists(const NETADDR &Addr) { return GetClientSlot(Addr) != -1; }
	int GetClientSlot(const NETADDR &Addr);
	void SendControl(NETADDR &Addr, int ControlMsg, const void *pExtra, int ExtraSize, SECURITY_TOKEN SecurityToken);

	int TryAcceptClient(NETADDR &Addr, SECURITY_TOKEN SecurityToken, int Slot, bool VanillaAuth = false, bool Sixup = false, SECURITY_TOKEN Token = 0);
	int NumClientsWithAddr(NETADDR Addr);
	bool Connlimit(NETADDR Addr);
	void SendMsgs(NETADDR &Addr, const CPacker **ppMsgs, int Num);
#endif

	bool m_FlushBatch = false;
	bool m_aFlushPending[NET_MAX_CLIENTS] = {};

	void Flush(int ClientId);

public:
#ifdef CONF_NETWORKING_QUIC
	~CNetServer();
	void SetIdentity(const unsigned char (&aSeed)[32]);
	void SetTlsFiles(const char *pCert, const char *pKey);
	// The hash browsers accept the certificate by, the current one or the
	// next; false without WebTransport.
	bool CertificateSha256(bool Next, SHA256_DIGEST *pSha256);
	// The server's own public identity, the 32 bytes clients pin it by;
	// false before the library is open.
	bool Identity(unsigned char (&aIdentity)[32]);
#endif

	int SetCallbacks(NETFUNC_NEWCLIENT pfnNewClient, NETFUNC_DELCLIENT pfnDelClient, void *pUser);
	int SetCallbacks(NETFUNC_NEWCLIENT pfnNewClient, NETFUNC_NEWCLIENT_NOAUTH pfnNewClientNoAuth, NETFUNC_CLIENTREJOIN pfnClientRejoin, NETFUNC_DELCLIENT pfnDelClient, void *pUser);

	//
	bool Open(NETADDR BindAddr, CNetBan *pNetBan, int MaxClients, int MaxClientsPerIp);
	void Close();

	//
	int Recv(CNetChunk *pChunk, SECURITY_TOKEN *pResponseToken);
	int Send(CNetChunk *pChunk);
	void Update();
	// Block for at most `Microseconds`, returning early when a packet arrives.
	void Wait(uint64_t Microseconds);

	// Maps for QUIC clients, which get them whole on a stream of their own
	// instead of in NETMSG_MAP_DATA chunks. SetMap() keeps a copy under the
	// ID; SendMap() starts sending it to the client and is false for a
	// client that is not connected over QUIC, which gets the chunks as
	// before. CancelMap() stops a map still going out. Without QUIC these
	// do nothing.
	void SetMap(int MapId, const char *pName, unsigned Crc, const SHA256_DIGEST &Sha256, const void *pData, unsigned Size);
	bool SendMap(int ClientId, int MapId);
	void CancelMap(int ClientId);

	// While a flush batch is open, sends requesting MSGFLAG_FLUSH only queue
	// their chunk and mark the connection; EndFlushBatch() then flushes each
	// marked connection once. Used to coalesce the flushes triggered while
	// draining a burst of incoming packets into one packet per recipient.
	void BeginFlushBatch() { m_FlushBatch = true; }
	void EndFlushBatch();

	//
	void Drop(int ClientId, const char *pReason);

	// status requests
	NETADDR Address() const { return m_Address; }
	CNetBan *NetBan() const { return m_pNetBan; }
	int MaxClients() const { return m_MaxClients; }
#ifdef CONF_NETWORKING_QUIC
	const NETADDR *ClientAddr(int ClientId) const { return &m_aPeers[ClientId].m_Address; }
	const std::array<char, NETADDR_MAXSTRSIZE> &ClientAddrString(int ClientId, bool IncludePort) const
	{
		return IncludePort ? m_aPeers[ClientId].m_aAddressStr : m_aPeers[ClientId].m_aAddressStrNoPort;
	}
	// All connections of the network library are authenticated.
	bool HasSecurityToken(int ClientId) const { return true; }
	// The socket is owned by the network library, so it cannot be used directly.
	NETSOCKET Socket() const { return nullptr; }
	int NetType() const { return NETTYPE_IPV4 | NETTYPE_IPV6; }
	// The library asks for and hands out 0.7 tokens itself.
	void SendTokenSixup(NETADDR &Addr, SECURITY_TOKEN Token) {}
#else
	const NETADDR *ClientAddr(int ClientId) const { return m_aSlots[ClientId].m_Connection.PeerAddress(); }
	const std::array<char, NETADDR_MAXSTRSIZE> &ClientAddrString(int ClientId, bool IncludePort) const { return m_aSlots[ClientId].m_Connection.PeerAddressString(IncludePort); }
	bool HasSecurityToken(int ClientId) const { return m_aSlots[ClientId].m_Connection.SecurityToken() != NET_SECURITY_TOKEN_UNSUPPORTED; }
	NETSOCKET Socket() const { return m_Socket; }
	int NetType() const { return net_socket_type(m_Socket); }
	void SendTokenSixup(NETADDR &Addr, SECURITY_TOKEN Token);
#endif

	//
	void SetMaxClientsPerIp(int Max);
	bool HasErrored(int ClientId);
	void ResumeOldConnection(int ClientId, int OrigId);
	void IgnoreTimeouts(int ClientId);

	void ResetErrorString(int ClientId);
	const char *ErrorString(int ClientId);

	// Answers a 0.7 connectionless packet, with the token it asked to be
	// answered with.
	void SendConnlessSixup(const NETADDR *pAddr, const void *pData, int DataSize, SECURITY_TOKEN ResponseToken);

	// anti spoof
	SECURITY_TOKEN GetGlobalToken();
	SECURITY_TOKEN GetToken(const NETADDR &Addr);
	SECURITY_TOKEN GetVanillaToken(const NETADDR &Addr);
};

class CNetConsole
{
	struct CSlot
	{
		CConsoleNetConnection m_Connection;
	};

	NETSOCKET m_Socket;
	CNetBan *m_pNetBan;
	CSlot m_aSlots[NET_MAX_CONSOLE_CLIENTS];

	NETFUNC_NEWCLIENT_CON m_pfnNewClient;
	NETFUNC_DELCLIENT m_pfnDelClient;
	void *m_pUser;

public:
	void SetCallbacks(NETFUNC_NEWCLIENT_CON pfnNewClient, NETFUNC_DELCLIENT pfnDelClient, void *pUser);

	//
	bool Open(NETADDR BindAddr, CNetBan *pNetBan);
	void Close();

	//
	int Recv(char *pLine, int MaxLength, int *pClientId = nullptr);
	int Send(int ClientId, const char *pLine);
	void Update();

	//
	int AcceptClient(NETSOCKET Socket, const NETADDR *pAddr);
	void Drop(int ClientId, const char *pReason);

	// status requests
	const NETADDR *ClientAddr(int ClientId) const { return m_aSlots[ClientId].m_Connection.PeerAddress(); }
	const std::array<char, NETADDR_MAXSTRSIZE> &ClientAddrString(int ClientId) const { return m_aSlots[ClientId].m_Connection.PeerAddressString(); }
	CNetBan *NetBan() const { return m_pNetBan; }
};

class CNetTokenCache
{
public:
	void Init(NETSOCKET Socket);
	void SendPacketConnless(CNetChunk *pChunk);
	void FetchToken(NETADDR *pAddr);
	void AddToken(const NETADDR *pAddr, TOKEN Token);
	TOKEN GetToken(const NETADDR *pAddr);
	TOKEN GenerateToken();
	void Update();

private:
	class CConnlessPacketInfo
	{
	public:
		NETADDR m_Addr;
		int m_DataSize;
		unsigned char m_aData[NET_MAX_CONNLESS_PAYLOAD];
		int64_t m_Expiry;
	};

	class CAddressInfo
	{
	public:
		NETADDR m_Addr;
		TOKEN m_Token;
		int64_t m_Expiry;
	};

	NETSOCKET m_Socket;
	std::vector<CAddressInfo> m_TokenCache;
	std::vector<CConnlessPacketInfo> m_ConnlessPackets;
};

// client side
class CNetClient
{
#ifdef CONF_NETWORKING_QUIC
	CNet *m_pNet = nullptr;
	CNetEvent *m_pNetEvent = nullptr;
	unsigned char m_aBuffer[NET_MAX_PACKETSIZE] = {0};
	int m_State = NETSTATE_OFFLINE;
	int64_t m_PeerId = -1;
	NETADDR m_ServerAddress = {0};
	// Addresses passed to the last Connect(), reported back by ConnectAddresses().
	NETADDR m_aConnectAddrs[16] = {{}};
	int m_NumConnectAddrs = 0;
	char m_aErrorString[256] = {0};
	// The identity to expect from a QUIC server, hex; empty takes any.
	char m_aConnectIdentity[65] = "";
	// The identity the QUIC server showed, hex; empty for other transports.
	char m_aServerIdentity[65] = "";

	NETADDR m_BindAddr = {0};

	bool OpenLibrary();
	void CloseLibrary();
	void Reopen();
	void ConnectImpl(const NETADDR *pAddr, int NumAddrs, bool Sixup);
	static bool SendRaw(void *pUser, const NETADDR *pAddr, const void *pData, int Size);
#else
	static bool SendRaw(void *pUser, const NETADDR *pAddr, const void *pData, int Size);
	CNetConnection m_Connection;
	CPacketChunkUnpacker m_PacketChunkUnpacker;
	CNetPacketConstruct m_RecvBuffer;
	CNetTokenCache m_TokenCache;

	NETSOCKET m_Socket = nullptr;
#endif

	CStun *m_pStun = nullptr;

public:
#ifdef CONF_NETWORKING_QUIC
	~CNetClient();
#endif

	// openness
	bool Open(NETADDR BindAddr);
	void Close();

	// connection state
	void Disconnect(const char *pReason);
	void Connect(const NETADDR *pAddr, int NumAddrs);
	void Connect7(const NETADDR *pAddr, int NumAddrs);
	// The identity a following Connect() to a QUIC address expects, hex;
	// empty takes whatever the server shows. Without QUIC there is nothing to
	// expect.
	void SetConnectIdentity(const char *pIdentity);
#ifdef CONF_NETWORKING_QUIC
	const char *ServerIdentity() const { return m_aServerIdentity; }
#else
	const char *ServerIdentity() const { return ""; }
#endif

	// communication
	int Recv(CNetChunk *pChunk, SECURITY_TOKEN *pResponseToken, bool Sixup);
	int Send(CNetChunk *pChunk);
	// Block for at most `Microseconds`, returning early when a packet arrives.
	void Wait(uint64_t Microseconds);

	// pumping
	void Update();
	int Flush();

	void ResetErrorString();

	// error and state
	int State() const;
	bool GotProblems(int64_t MaxLatency) const;
	const char *ErrorString() const;
#ifdef CONF_NETWORKING_QUIC
	int NetType() const { return NETTYPE_IPV4 | NETTYPE_IPV6; }
	// The socket is owned by the network library, which recreates it on error.
	bool SocketIsBroken() const { return false; }
	const NETADDR *ServerAddress() const { return &m_ServerAddress; }
	void ConnectAddresses(const NETADDR **ppAddrs, int *pNumAddrs) const
	{
		*ppAddrs = m_aConnectAddrs;
		*pNumAddrs = m_NumConnectAddrs;
	}
#else
	int NetType() const { return net_socket_type(m_Socket); }
	bool SocketIsBroken() const { return m_Socket != nullptr && net_udp_is_broken(m_Socket); }
	const NETADDR *ServerAddress() const { return m_Connection.PeerAddress(); }
	void ConnectAddresses(const NETADDR **ppAddrs, int *pNumAddrs) const { m_Connection.ConnectAddresses(ppAddrs, pNumAddrs); }
#endif

	// stun
	void FeedStunServer(NETADDR StunServer);
	void RefreshStun();
	CONNECTIVITY GetConnectivity(int NetType, NETADDR *pGlobalAddr);
};

// TODO: both, fix these. This feels like a junk class for stuff that doesn't fit anywhere
class CNetBase
{
	static IOHANDLE ms_DataLogSent;
	static IOHANDLE ms_DataLogRecv;
	static CHuffman ms_Huffman;

public:
	static void OpenLog(IOHANDLE DataLogSent, IOHANDLE DataLogRecv);
	static void CloseLog();
	static void Init();
#ifdef CONF_NETWORKING_QUIC
	// Tells the network library how much to log, so that it formats only
	// what some logger would take; called from the update loops.
	static void UpdateLogLevel();
#endif
	static int Compress(const void *pData, int DataSize, void *pOutput, int OutputSize);
	static int Decompress(const void *pData, int DataSize, void *pOutput, int OutputSize);

	static bool IsValidConnectionOrientedPacket(const CNetPacketConstruct *pPacket);

	static void SendControlMsg(NETSOCKET Socket, NETADDR *pAddr, int Ack, int ControlMsg, const void *pExtra, int ExtraSize, SECURITY_TOKEN SecurityToken, bool Sixup = false);
	static void SendControlMsgWithToken7(NETSOCKET Socket, NETADDR *pAddr, TOKEN Token, int Ack, int ControlMsg, TOKEN MyToken, bool Extended);
	static void SendPacketConnless(NETSOCKET Socket, NETADDR *pAddr, const void *pData, int DataSize, bool Extended, unsigned char aExtra[NET_CONNLESS_EXTRA_SIZE]);
	static void SendPacketConnlessWithToken7(NETSOCKET Socket, NETADDR *pAddr, const void *pData, int DataSize, SECURITY_TOKEN Token, SECURITY_TOKEN ResponseToken);
	static void SendPacket(NETSOCKET Socket, NETADDR *pAddr, CNetPacketConstruct *pPacket, SECURITY_TOKEN SecurityToken, bool Sixup = false);

	static std::optional<int> UnpackPacketFlags(unsigned char *pBuffer, int Size);
	// `AllowDecompression` false rejects compressed packets instead of decompressing them,
	// decompression being the most expensive part of receiving a packet. `pDecompressed` is
	// set when decompression was attempted, successfully or not.
	static int UnpackPacket(unsigned char *pBuffer, int Size, CNetPacketConstruct *pPacket, bool &Sixup, bool AllowDecompression, SECURITY_TOKEN *pSecurityToken = nullptr, SECURITY_TOKEN *pResponseToken = nullptr, bool *pDecompressed = nullptr);

	// The backroom is ack-NET_MAX_SEQUENCE/2. Used for knowing if we acked a packet or not
	static bool IsSeqInBackroom(int Seq, int Ack);
};

#endif
