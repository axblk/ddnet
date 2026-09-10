/* (c) Magnus Auvinen. See licence.txt in the root of the distribution for more information. */
/* If you are missing that file, acquire a complete release at teeworlds.com.                */
#ifndef ENGINE_SHARED_NETWORK_H
#define ENGINE_SHARED_NETWORK_H

#include "stun.h"

#include <base/hash.h>
#include <base/net.h>
#include <base/types.h>

#include <array>
#include <map>
#include <vector>

class CHuffman;
class CNetBan;
typedef struct DdnetNet CNet;
typedef struct DdnetNetEvent CNetEvent;
class CPacker;

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
	NET_CONNLESS_EXTRA_SIZE = 4,
	NET_MAX_CLIENTS = 128,
	NET_MAX_CONSOLE_CLIENTS = 4,

	NET_CHUNKFLAG_VITAL = 1,
	// Not a game message but a step of a map arriving on a QUIC stream of
	// its own: the header (see NetDecodeMapHeader), a piece of the map, its
	// end after the checksum matched, or a failure with the reason as data.
	NET_CHUNKFLAG_MAP_HEADER = 1 << 4,
	NET_CHUNKFLAG_MAP_DATA = 1 << 5,
	NET_CHUNKFLAG_MAP_END = 1 << 6,
	NET_CHUNKFLAG_MAP_FAILED = 1 << 7,
	NET_CHUNKFLAG_MAP = NET_CHUNKFLAG_MAP_HEADER | NET_CHUNKFLAG_MAP_DATA | NET_CHUNKFLAG_MAP_END | NET_CHUNKFLAG_MAP_FAILED,
};

typedef int SECURITY_TOKEN;
// The token of 0.7, as it appears in its connectionless packets.
typedef unsigned int TOKEN;

SECURITY_TOKEN ToSecurityToken(const unsigned char *pData);

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
// Takes the data of a NET_CHUNKFLAG_MAP_HEADER chunk apart.
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

// server side
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

class CNetServer
{
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
		// The client sent its timeout code: after a timeout the slot waits
		// for the code instead of going, see STATE_TIMEOUT.
		bool m_TimeoutProtected = false;
		// When the timeout happened, for the end of the protection.
		int64_t m_TimeoutAt = 0;
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

	bool m_FlushBatch = false;
	bool m_aFlushPending[NET_MAX_CLIENTS] = {};

	void Flush(int ClientId);

public:
	~CNetServer();
	void SetIdentity(const unsigned char (&aSeed)[32]);
	void SetTlsFiles(const char *pCert, const char *pKey);
	// The hash browsers accept the certificate by, the current one or the
	// next; false without WebTransport.
	bool CertificateSha256(bool Next, SHA256_DIGEST *pSha256);
	// The server's own public identity, the 32 bytes clients pin it by;
	// false before the library is open.
	bool Identity(unsigned char (&aIdentity)[32]);
	// Whether the library listens for WebSockets: `sv_websocket`, if they
	// are compiled in. False before the library is open.
	bool AcceptsWebsockets();

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
	// before. CancelMap() stops a map still going out.
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
	const NETADDR *ClientAddr(int ClientId) const { return &m_aPeers[ClientId].m_Address; }
	const std::array<char, NETADDR_MAXSTRSIZE> &ClientAddrString(int ClientId, bool IncludePort) const
	{
		return IncludePort ? m_aPeers[ClientId].m_aAddressStr : m_aPeers[ClientId].m_aAddressStrNoPort;
	}
	// All connections of the network library are authenticated.
	bool HasSecurityToken(int ClientId) const { return true; }
	int NetType() const { return NETTYPE_IPV4 | NETTYPE_IPV6; }
	// The library asks for and hands out 0.7 tokens itself.
	void SendTokenSixup(NETADDR &Addr, SECURITY_TOKEN Token) {}

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

// client side
class CNetClient
{
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
	// The fragment of the connect address: the identity to expect from the
	// server and, for a browser, the certificate hashes to take; empty takes
	// any identity.
	char m_aConnectFragment[256] = "";
	// The host name the connect address came as, without its port; empty
	// for an IP address. A browser connects by the name, since it has to
	// check the certificate against it.
	char m_aConnectHost[128] = "";
	// The identity the QUIC server showed, hex; empty for other transports.
	char m_aServerIdentity[65] = "";

	NETADDR m_BindAddr = {0};

	bool OpenLibrary();
	void CloseLibrary();
	void Reopen();
	void ConnectImpl(const NETADDR *pAddr, int NumAddrs, bool Sixup);
	static bool SendRaw(void *pUser, const NETADDR *pAddr, const void *pData, int Size);

	CStun *m_pStun = nullptr;

public:
	~CNetClient();

	// openness
	bool Open(NETADDR BindAddr);
	void Close();

	// connection state
	void Disconnect(const char *pReason);
	void Connect(const NETADDR *pAddr, int NumAddrs);
	void Connect7(const NETADDR *pAddr, int NumAddrs);
	// What a following Connect() to a QUIC or WebSocket address carries
	// besides the address: the host name it came as (empty for an IP
	// address; a browser connects by the name, since it checks the
	// certificate against it) and the fragment. `identity-sha256=<hex>` or
	// the bare hex pins the server's identity, `cert-sha256=<hex>[,<hex>]`
	// names the certificates a browser takes; empty takes whatever the
	// server shows.
	void SetConnectTarget(const char *pHost, const char *pFragment);
	const char *ServerIdentity() const { return m_aServerIdentity; }
	const char *ConnectHost() const { return m_aConnectHost; }

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
	int NetType() const { return NETTYPE_IPV4 | NETTYPE_IPV6; }
	// The socket is owned by the network library, which recreates it on error.
	bool SocketIsBroken() const { return false; }
	const NETADDR *ServerAddress() const { return &m_ServerAddress; }
	void ConnectAddresses(const NETADDR **ppAddrs, int *pNumAddrs) const
	{
		*ppAddrs = m_aConnectAddrs;
		*pNumAddrs = m_NumConnectAddrs;
	}

	// stun
	void FeedStunServer(NETADDR StunServer);
	void RefreshStun();
	CONNECTIVITY GetConnectivity(int NetType, NETADDR *pGlobalAddr);
};

// The Huffman codec, which demos and ghosts share with the wire, and the
// library's logging.
class CNetBase
{
	static CHuffman ms_Huffman;

public:
	static void Init();
	// Tells the network library how much to log, so that it formats only
	// what some logger would take; called from the update loops.
	static void UpdateLogLevel();
	static int Compress(const void *pData, int DataSize, void *pOutput, int OutputSize);
	static int Decompress(const void *pData, int DataSize, void *pOutput, int OutputSize);
};

#endif
