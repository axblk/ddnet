// SPDX-License-Identifier: (GPL-2.0 OR BSD-2-Clause)
/* Loader and key authority for the DDNet XDP filter.
 *
 * One instance per host. Only one XDP program can be attached to an interface, so
 * this cannot live inside a server process: a host that runs several DDNet servers
 * would have the first one win and the rest fail to start. Keeping it separate also
 * means no server needs CAP_BPF, and the filter survives a server restart.
 *
 * The key is the only thing shared with the servers. It is written to a file rather
 * than pushed over a socket, which removes the ordering problem at startup and works
 * unchanged when a server runs in a container with the file bind mounted.
 */
#include "ddnet_xdp_shared.h"

#include <arpa/inet.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <libgen.h>
#include <linux/if_link.h>
#include <net/if.h>
#include <netdb.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_PORTS DDNET_XDP_MAX_PORTS
#define MAX_MASTERS 32

struct key_file_epoch
{
	uint64_t m_K0;
	uint64_t m_K1;
	uint8_t m_Valid;
	uint8_t m_aPad[7];
};

/* Layout of the key file. The magic and version let a server refuse a format it does
 * not understand instead of deriving tokens nobody will recognise. */
struct key_file
{
	char m_aMagic[DDNET_XDP_KEY_MAGIC_SIZE];
	uint32_t m_Version;
	uint32_t m_Current;
	struct key_file_epoch m_aEpochs[DDNET_XDP_KEY_EPOCHS];
};

struct options
{
	const char *m_pInterface;
	const char *m_pObject;
	const char *m_pKeyPath;
	const char *m_pPinDir;
	const char *m_pKeyGroup;
	uint16_t m_aPorts[MAX_PORTS];
	int m_NumPorts;
	const char *m_apMasters[MAX_MASTERS];
	int m_NumMasters;
	unsigned m_aBudgetPps[DDNET_XDP_NUM_BUDGETS];
	unsigned m_PrefixV4;
	unsigned m_PrefixV6;
	unsigned m_RotateSeconds;
	unsigned m_ArmAfter;
	unsigned m_ConnPps;
	unsigned m_ConnIdleSeconds;
	bool m_CountOnly;
	bool m_StatsOnly;
	bool m_SkbMode;
	bool m_Verbose;
};

static volatile sig_atomic_t s_Stop;

static void on_signal(int Signal)
{
	(void)Signal;
	s_Stop = 1;
}

static void log_error(const char *pFormat, ...)
{
	va_list Args;
	va_start(Args, pFormat);
	fprintf(stderr, "ddnet-xdp: ERROR: ");
	vfprintf(stderr, pFormat, Args);
	fprintf(stderr, "\n");
	va_end(Args);
}

static void log_info(const char *pFormat, ...)
{
	va_list Args;
	va_start(Args, pFormat);
	fprintf(stdout, "ddnet-xdp: ");
	vfprintf(stdout, pFormat, Args);
	fprintf(stdout, "\n");
	fflush(stdout);
	va_end(Args);
}

static int read_random(void *pBuffer, size_t Size)
{
	FILE *pFile = fopen("/dev/urandom", "rb");
	size_t Read;
	if(!pFile)
		return -1;
	Read = fread(pBuffer, 1, Size, pFile);
	fclose(pFile);
	return Read == Size ? 0 : -1;
}

/* Writes the key file in one step, so a server never reads a half written one. */
/* Picks the key state back up from the file this service writes. A restart that
 * started from nothing would leave three of the four epochs invalid, and every
 * running session carries a token derived from one of them: they would all be
 * dropped the moment the port is armed again. The file is the state, so it is
 * also where the state comes from. */
static int read_key_file(const char *pPath, struct key_file *pKeys)
{
	struct key_file Read;
	ssize_t Got;
	int File = open(pPath, O_RDONLY);
	if(File < 0)
		return -1;
	Got = read(File, &Read, sizeof(Read));
	close(File);
	if(Got != (ssize_t)sizeof(Read))
		return -1;
	if(memcmp(Read.m_aMagic, DDNET_XDP_KEY_MAGIC, DDNET_XDP_KEY_MAGIC_SIZE) != 0 ||
		Read.m_Version != 1 || Read.m_Current >= DDNET_XDP_KEY_EPOCHS)
		return -1;
	*pKeys = Read;
	return 0;
}

static int write_key_file(const char *pPath, const struct key_file *pKeys, const char *pGroup)
{
	char aTemp[512];
	char aDirectory[512];
	int File;
	ssize_t Written;

	if(snprintf(aTemp, sizeof(aTemp), "%s.tmp", pPath) >= (int)sizeof(aTemp))
	{
		log_error("key path is too long: %s", pPath);
		return -1;
	}
	snprintf(aDirectory, sizeof(aDirectory), "%s", pPath);
	if(mkdir(dirname(aDirectory), 0755) != 0 && errno != EEXIST)
	{
		log_error("cannot create %s: %s", aDirectory, strerror(errno));
		return -1;
	}
	/* The key is what makes a token unforgeable, so it must not be world readable.
	 * Servers are expected to run in a group that is granted access to it. */
	File = open(aTemp, O_WRONLY | O_CREAT | O_TRUNC, 0640);
	if(File < 0)
	{
		log_error("cannot write %s: %s", aTemp, strerror(errno));
		return -1;
	}
	if(pGroup)
	{
		// A server usually runs as its own user, and a key only root can read is a
		// key the server cannot derive tokens with.
		const struct group *pEntry = getgrnam(pGroup);
		if(!pEntry)
		{
			log_error("no group '%s'", pGroup);
			close(File);
			unlink(aTemp);
			return -1;
		}
		if(fchown(File, (uid_t)-1, pEntry->gr_gid) != 0)
		{
			log_error("cannot give '%s' access to the key: %s", pGroup, strerror(errno));
			close(File);
			unlink(aTemp);
			return -1;
		}
	}
	Written = write(File, pKeys, sizeof(*pKeys));
	if(Written != (ssize_t)sizeof(*pKeys) || fsync(File) != 0)
	{
		log_error("cannot write %s: %s", aTemp, strerror(errno));
		close(File);
		unlink(aTemp);
		return -1;
	}
	close(File);
	if(rename(aTemp, pPath) != 0)
	{
		log_error("cannot rename %s: %s", aTemp, strerror(errno));
		unlink(aTemp);
		return -1;
	}
	return 0;
}

/* Rotates to the next epoch. The previous one stays valid so that connections which
 * were handed a token under it keep working across the change. */
static int rotate_keys(struct key_file *pKeys, int KeyMap, const char *pPath, const char *pGroup)
{
	const uint32_t Next = (pKeys->m_Current + 1) % DDNET_XDP_KEY_EPOCHS;
	const uint32_t Retire = (Next + DDNET_XDP_KEY_EPOCHS - 2) % DDNET_XDP_KEY_EPOCHS;
	uint32_t Epoch;

	if(read_random(&pKeys->m_aEpochs[Next].m_K0, sizeof(uint64_t)) != 0 ||
		read_random(&pKeys->m_aEpochs[Next].m_K1, sizeof(uint64_t)) != 0)
	{
		log_error("cannot read random bytes for the new key");
		return -1;
	}
	pKeys->m_aEpochs[Next].m_Valid = 1;
	pKeys->m_Current = Next;
	if(Retire != Next)
		pKeys->m_aEpochs[Retire].m_Valid = 0;

	for(Epoch = 0; Epoch < DDNET_XDP_KEY_EPOCHS; Epoch++)
	{
		struct ddnet_xdp_key Value = {};
		Value.m_Key.m_K0 = pKeys->m_aEpochs[Epoch].m_K0;
		Value.m_Key.m_K1 = pKeys->m_aEpochs[Epoch].m_K1;
		Value.m_Valid = pKeys->m_aEpochs[Epoch].m_Valid;
		if(bpf_map_update_elem(KeyMap, &Epoch, &Value, BPF_ANY) != 0)
		{
			log_error("cannot update key %u: %s", Epoch, strerror(errno));
			return -1;
		}
	}
	/* The map is updated before the file, so a server can never derive a token under
	 * a key the filter does not know yet. */
	return write_key_file(pPath, pKeys, pGroup);
}

/* A master is given by name as often as by address, and a name can point at
 * several addresses and change over time, so it is resolved rather than parsed, and
 * resolved again now and then. */
static int add_master(int MapV4, int MapV6, const char *pName)
{
	struct addrinfo Hints = {};
	struct addrinfo *pResults, *pEntry;
	const uint8_t One = 1;
	int Added = 0;

	Hints.ai_family = AF_UNSPEC;
	Hints.ai_socktype = SOCK_DGRAM;
	if(getaddrinfo(pName, NULL, &Hints, &pResults) != 0)
	{
		log_error("cannot resolve master %s", pName);
		return -1;
	}
	for(pEntry = pResults; pEntry; pEntry = pEntry->ai_next)
	{
		if(pEntry->ai_family == AF_INET)
		{
			const struct sockaddr_in *pAddress = (const struct sockaddr_in *)pEntry->ai_addr;
			if(bpf_map_update_elem(MapV4, &pAddress->sin_addr.s_addr, &One, BPF_ANY) == 0)
				Added++;
		}
		else if(pEntry->ai_family == AF_INET6)
		{
			const struct sockaddr_in6 *pAddress = (const struct sockaddr_in6 *)pEntry->ai_addr;
			if(bpf_map_update_elem(MapV6, &pAddress->sin6_addr, &One, BPF_ANY) == 0)
				Added++;
		}
	}
	freeaddrinfo(pResults);
	if(Added == 0)
	{
		log_error("master %s resolved to nothing usable", pName);
		return -1;
	}
	return 0;
}

/* A rate is stored as the time one token takes, because splitting a rate over prefix
 * buckets and CPUs would otherwise round down to nothing. */
static uint64_t ns_per_token(unsigned Pps, int NumCpus)
{
	uint64_t Share;
	if(Pps == 0)
		return 0;
	Share = (uint64_t)Pps;
	return (1000000000ULL * DDNET_XDP_PREFIX_BUCKETS * (uint64_t)NumCpus) / Share;
}

static void usage(const char *pName)
{
	fprintf(stderr,
		"usage: %s -i INTERFACE -p PORT [-p PORT ...] [options]\n"
		"\n"
		"  -i, --interface NAME   interface to attach to (the host one, not a veth)\n"
		"  -p, --port PORT        UDP port of a DDNet server, repeatable\n"
		"  -o, --object PATH      ddnet_xdp_kern.o (default: next to this binary)\n"
		"  -k, --key PATH         key file to write (default: %s)\n"
		"  -g, --key-group GROUP  group that may read the key, for servers running as\n"
		"                         another user; without it only root can read it\n"
		"      --legacy-pps N     budget for 0.6 that cannot be verified (default 10000)\n"
		"      --serverinfo-pps N budget for server info requests (default 2000)\n"
		"      --handshake-pps N  budget for handshakes passed to the server (default 5000)\n"
		"      --newconn-pps N    budget for QUIC connection attempts (default 2000)\n"
		"                         every budget is spread over source prefixes; 0 is unlimited\n"
		"  -m, --master NAME      master server, address or name, never budgeted; repeatable\n"
		"      --prefix4 N        IPv4 aggregation prefix (default 24)\n"
		"      --prefix6 N        IPv6 aggregation prefix (default 56)\n"
		"      --rotate N         key rotation interval in seconds (default 21600)\n"
		"      --arm-after N      verified packets before a port starts dropping (default 100)\n"
		"      --conntrack[=PPS]  remember 0.6 connections seen handshaking and give\n"
		"                         each one its own budget (default 200 when given)\n"
		"      --conn-idle N      forget a 0.6 connection after N idle seconds (default 60)\n"
		"      --count-only       never drop, only count\n"
		"      --pin-dir PATH     where to pin the maps (default: %s)\n"
		"      --stats            print the counters of a running instance and exit\n"
		"      --skb              attach in generic mode instead of driver mode\n"
		"  -v, --verbose          print counters every second\n",
		pName, DDNET_XDP_DEFAULT_KEY_PATH, DDNET_XDP_DEFAULT_PIN_DIR);
}

static int parse_options(int argc, char **argv, struct options *pOptions)
{
	static const struct option s_aLong[] = {
		{"interface", required_argument, NULL, 'i'},
		{"port", required_argument, NULL, 'p'},
		{"object", required_argument, NULL, 'o'},
		{"key", required_argument, NULL, 'k'},
		{"master", required_argument, NULL, 'm'},
		{"legacy-pps", required_argument, NULL, 1},
		{"newconn-pps", required_argument, NULL, 2},
		{"serverinfo-pps", required_argument, NULL, 16},
		{"handshake-pps", required_argument, NULL, 17},
		{"prefix4", required_argument, NULL, 3},
		{"prefix6", required_argument, NULL, 4},
		{"rotate", required_argument, NULL, 5},
		{"arm-after", required_argument, NULL, 6},
		{"conntrack", optional_argument, NULL, 12},
		{"conn-idle", required_argument, NULL, 13},
		{"count-only", no_argument, NULL, 7},
		{"key-group", required_argument, NULL, 11},
		{"pin-dir", required_argument, NULL, 9},
		{"stats", no_argument, NULL, 10},
		{"skb", no_argument, NULL, 8},
		{"verbose", no_argument, NULL, 'v'},
		{"help", no_argument, NULL, 'h'},
		{NULL, 0, NULL, 0},
	};
	int Option;

	pOptions->m_pKeyPath = DDNET_XDP_DEFAULT_KEY_PATH;
	pOptions->m_pPinDir = DDNET_XDP_DEFAULT_PIN_DIR;
	/* Ordered by how much a loss hurts, see the enum. Legacy 0.6 is what a flood
	 * looks like and gets the least; handshakes are players trying to get in and get
	 * enough that they usually do. */
	pOptions->m_aBudgetPps[DDNET_XDP_BUDGET_LEGACY] = 10000;
	pOptions->m_aBudgetPps[DDNET_XDP_BUDGET_CONNLESS] = 2000;
	pOptions->m_aBudgetPps[DDNET_XDP_BUDGET_HANDSHAKE] = 5000;
	pOptions->m_aBudgetPps[DDNET_XDP_BUDGET_NEWCONN] = 2000;
	pOptions->m_PrefixV4 = 24;
	pOptions->m_PrefixV6 = 56;
	pOptions->m_RotateSeconds = 21600;
	pOptions->m_ArmAfter = 100;
	pOptions->m_ConnIdleSeconds = 60;

	while((Option = getopt_long(argc, argv, "i:p:o:k:g:m:vh", s_aLong, NULL)) != -1)
	{
		switch(Option)
		{
		case 'i': pOptions->m_pInterface = optarg; break;
		case 'p':
			if(pOptions->m_NumPorts >= MAX_PORTS)
			{
				log_error("at most %d ports", MAX_PORTS);
				return -1;
			}
			pOptions->m_aPorts[pOptions->m_NumPorts++] = (uint16_t)atoi(optarg);
			break;
		case 'o': pOptions->m_pObject = optarg; break;
		case 'k': pOptions->m_pKeyPath = optarg; break;
		case 'g': pOptions->m_pKeyGroup = optarg; break;
		case 'm':
			if(pOptions->m_NumMasters >= MAX_MASTERS)
			{
				log_error("at most %d masters", MAX_MASTERS);
				return -1;
			}
			pOptions->m_apMasters[pOptions->m_NumMasters++] = optarg;
			break;
		case 1: pOptions->m_aBudgetPps[DDNET_XDP_BUDGET_LEGACY] = (unsigned)atoi(optarg); break;
		case 2: pOptions->m_aBudgetPps[DDNET_XDP_BUDGET_NEWCONN] = (unsigned)atoi(optarg); break;
		case 16: pOptions->m_aBudgetPps[DDNET_XDP_BUDGET_CONNLESS] = (unsigned)atoi(optarg); break;
		case 17: pOptions->m_aBudgetPps[DDNET_XDP_BUDGET_HANDSHAKE] = (unsigned)atoi(optarg); break;
		case 3: pOptions->m_PrefixV4 = (unsigned)atoi(optarg); break;
		case 4: pOptions->m_PrefixV6 = (unsigned)atoi(optarg); break;
		case 5: pOptions->m_RotateSeconds = (unsigned)atoi(optarg); break;
		case 6: pOptions->m_ArmAfter = (unsigned)atoi(optarg); break;
		case 7: pOptions->m_CountOnly = true; break;
		case 12: pOptions->m_ConnPps = optarg ? (unsigned)atoi(optarg) : 200; break;
		case 13: pOptions->m_ConnIdleSeconds = (unsigned)atoi(optarg); break;
		case 9: pOptions->m_pPinDir = optarg; break;
		case 11: pOptions->m_pKeyGroup = optarg; break;
		case 10: pOptions->m_StatsOnly = true; break;
		case 8: pOptions->m_SkbMode = true; break;
		case 'v': pOptions->m_Verbose = true; break;
		default: return -1;
		}
	}
	if(pOptions->m_StatsOnly)
		return 0;
	if(!pOptions->m_pInterface || pOptions->m_NumPorts == 0)
		return -1;
	return 0;
}

/* Reads one counter, summed over the CPUs it is kept on. */
static uint64_t counter(int StatsMap, int NumCpus, uint64_t *pScratch, uint32_t Index)
{
	uint64_t Total = 0;
	int Cpu;
	if(bpf_map_lookup_elem(StatsMap, &Index, pScratch) != 0)
		return 0;
	for(Cpu = 0; Cpu < NumCpus; Cpu++)
		Total += pScratch[Cpu];
	return Total;
}

/* Prints what was classified how, per port. `pPrevious` holds the totals of the last
 * call, if any, so the rate can be shown next to the total; the interesting number
 * during an attack is what is arriving now, not what has arrived since boot. */
static void print_stats(int StatsMap, int NumCpus, const struct options *pOptions,
	const uint16_t *pPorts, int NumPorts, uint64_t *pPrevious, double Seconds)
{
	uint64_t *pScratch = calloc(NumCpus, sizeof(uint64_t));
	int Index;

	if(!pScratch)
		return;
	for(Index = 0; Index < NumPorts; Index++)
	{
		uint32_t Class;
		bool Any = false;
		for(Class = 0; Class < DDNET_XDP_NUM_CLASSES; Class++)
		{
			uint64_t aTotals[DDNET_XDP_NUM_VERDICTS];
			uint32_t Verdict;
			bool AnyClass = false;
			for(Verdict = 0; Verdict < DDNET_XDP_NUM_VERDICTS; Verdict++)
			{
				aTotals[Verdict] = counter(StatsMap, NumCpus, pScratch,
					DDNET_XDP_STATS_INDEX(Index, Class, Verdict));
				AnyClass = AnyClass || aTotals[Verdict] != 0;
			}
			if(!AnyClass)
				continue;
			if(!Any)
			{
				printf("port %u\n", pPorts[Index]);
				Any = true;
			}
			printf("  %-20s", DDNET_XDP_CLASS_NAMES[Class]);
			for(Verdict = 0; Verdict < DDNET_XDP_NUM_VERDICTS; Verdict++)
			{
				printf("  %s %12llu", DDNET_XDP_VERDICT_NAMES[Verdict],
					(unsigned long long)aTotals[Verdict]);
				if(pPrevious && Seconds > 0)
				{
					const uint32_t Slot = DDNET_XDP_STATS_INDEX(Index, Class, Verdict);
					printf(" (%9.0f/s)", (double)(aTotals[Verdict] - pPrevious[Slot]) / Seconds);
					pPrevious[Slot] = aTotals[Verdict];
				}
			}
			printf("\n");
		}
	}
	(void)pOptions;
	free(pScratch);
}

/* Reads the counters of a running instance through the pinned maps, without loading
 * or attaching anything. */
static int show_pinned_stats(const struct options *pOptions)
{
	char aPath[512];
	int StatsMap, PortMap;
	uint16_t aPorts[MAX_PORTS];
	int NumPorts = 0;
	uint16_t Port = 0, Next;
	int NumCpus = libbpf_num_possible_cpus();
	uint16_t aByIndex[MAX_PORTS] = {};

	snprintf(aPath, sizeof(aPath), "%s/ddnet_stats", pOptions->m_pPinDir);
	StatsMap = bpf_obj_get(aPath);
	if(StatsMap < 0)
	{
		log_error("cannot open %s: %s", aPath, strerror(errno));
		log_error("is ddnet-xdp running, and did it pin its maps there?");
		return 1;
	}
	snprintf(aPath, sizeof(aPath), "%s/ddnet_ports", pOptions->m_pPinDir);
	PortMap = bpf_obj_get(aPath);
	if(PortMap < 0)
	{
		log_error("cannot open %s: %s", aPath, strerror(errno));
		close(StatsMap);
		return 1;
	}

	/* The pinned map is the only place that knows which ports are guarded, so the
	 * port list does not have to be repeated on the command line. */
	while(bpf_map_get_next_key(PortMap, NumPorts == 0 ? NULL : &Port, &Next) == 0 && NumPorts < MAX_PORTS)
	{
		struct ddnet_xdp_port Entry;
		Port = Next;
		if(bpf_map_lookup_elem(PortMap, &Port, &Entry) == 0 && Entry.m_Index < MAX_PORTS)
			aByIndex[Entry.m_Index] = Port;
		NumPorts++;
	}
	for(int Index = 0; Index < MAX_PORTS; Index++)
		aPorts[Index] = aByIndex[Index];

	print_stats(StatsMap, NumCpus, pOptions, aPorts, MAX_PORTS, NULL, 0);
	close(PortMap);
	close(StatsMap);
	return 0;
}

int main(int argc, char **argv)
{
	struct options Options = {};
	struct bpf_object *pObject = NULL;
	struct bpf_program *pProgram;
	struct key_file Keys = {};
	char aObjectPath[512];
	int InterfaceIndex;
	int ConfigMap, KeyMap, PortMap, MasterV4Map, MasterV6Map, StatsMap;
	int ProgramFd;
	int NumCpus;
	int Result = 1;
	int Index;
	unsigned AttachFlags;
	uint32_t Zero = 0;
	struct ddnet_xdp_config Config = {};
	static uint64_t s_aPrevious[DDNET_XDP_STATS_ENTRIES];
	uint64_t *aPrevious = s_aPrevious;
	time_t LastRotate;

	if(parse_options(argc, argv, &Options) != 0)
	{
		usage(argv[0]);
		return 2;
	}

	if(Options.m_StatsOnly)
		return show_pinned_stats(&Options);

	InterfaceIndex = if_nametoindex(Options.m_pInterface);
	if(InterfaceIndex == 0)
	{
		log_error("no interface %s: %s", Options.m_pInterface, strerror(errno));
		return 1;
	}

	NumCpus = libbpf_num_possible_cpus();
	if(NumCpus <= 0)
	{
		log_error("cannot determine the number of CPUs");
		return 1;
	}

	if(Options.m_pObject)
	{
		snprintf(aObjectPath, sizeof(aObjectPath), "%s", Options.m_pObject);
	}
	else
	{
		char aSelf[512];
		ssize_t Length = readlink("/proc/self/exe", aSelf, sizeof(aSelf) - 1);
		if(Length <= 0)
		{
			log_error("cannot find myself, pass --object");
			return 1;
		}
		aSelf[Length] = '\0';
		snprintf(aObjectPath, sizeof(aObjectPath), "%s/ddnet_xdp_kern.o", dirname(aSelf));
	}

	pObject = bpf_object__open_file(aObjectPath, NULL);
	if(!pObject)
	{
		log_error("cannot open %s: %s", aObjectPath, strerror(errno));
		return 1;
	}
	if(bpf_object__load(pObject) != 0)
	{
		log_error("cannot load %s: %s", aObjectPath, strerror(errno));
		log_error("run as root, and check the verifier log above");
		goto out;
	}

	pProgram = bpf_object__find_program_by_name(pObject, "ddnet_xdp_filter");
	if(!pProgram)
	{
		log_error("the object has no ddnet_xdp_filter program");
		goto out;
	}
	ProgramFd = bpf_program__fd(pProgram);

	ConfigMap = bpf_object__find_map_fd_by_name(pObject, "ddnet_config");
	KeyMap = bpf_object__find_map_fd_by_name(pObject, "ddnet_keys");
	PortMap = bpf_object__find_map_fd_by_name(pObject, "ddnet_ports");
	MasterV4Map = bpf_object__find_map_fd_by_name(pObject, "ddnet_master_v4");
	MasterV6Map = bpf_object__find_map_fd_by_name(pObject, "ddnet_master_v6");
	StatsMap = bpf_object__find_map_fd_by_name(pObject, "ddnet_stats");
	if(ConfigMap < 0 || KeyMap < 0 || PortMap < 0 || MasterV4Map < 0 || MasterV6Map < 0 || StatsMap < 0)
	{
		log_error("the object is missing a map");
		goto out;
	}

	for(Index = 0; Index < DDNET_XDP_NUM_BUDGETS; Index++)
	{
		Config.m_aBudgets[Index].m_NsPerToken = ns_per_token(Options.m_aBudgetPps[Index], NumCpus);
		Config.m_aBudgets[Index].m_Burst = Options.m_aBudgetPps[Index] ? 32 : 0;
	}
	Config.m_PrefixV4 = Options.m_PrefixV4;
	Config.m_PrefixV6 = Options.m_PrefixV6;
	/* Not divided over prefixes or CPUs: this budget belongs to one connection, and
	 * its packets all arrive on whichever queue its 4-tuple hashes to. */
	Config.m_ConnNsPerToken = Options.m_ConnPps ? 1000000000ULL / Options.m_ConnPps : 0;
	Config.m_ConnBurst = Options.m_ConnPps ? 64 : 0;
	Config.m_ConnIdleNs = (uint64_t)Options.m_ConnIdleSeconds * 1000000000ULL;
	if(bpf_map_update_elem(ConfigMap, &Zero, &Config, BPF_ANY) != 0)
	{
		log_error("cannot write the configuration: %s", strerror(errno));
		goto out;
	}

	for(Index = 0; Index < Options.m_NumPorts; Index++)
	{
		struct ddnet_xdp_port Port = {};
		Port.m_Index = (uint8_t)Index;
		if(bpf_map_update_elem(PortMap, &Options.m_aPorts[Index], &Port, BPF_ANY) != 0)
		{
			log_error("cannot add port %u: %s", Options.m_aPorts[Index], strerror(errno));
			goto out;
		}
	}
	for(Index = 0; Index < Options.m_NumMasters; Index++)
	{
		if(add_master(MasterV4Map, MasterV6Map, Options.m_apMasters[Index]) != 0)
			goto out;
	}

	if(read_key_file(Options.m_pKeyPath, &Keys) == 0)
	{
		log_info("continuing from key epoch %u in %s", Keys.m_Current, Options.m_pKeyPath);
	}
	else
	{
		memcpy(Keys.m_aMagic, DDNET_XDP_KEY_MAGIC, DDNET_XDP_KEY_MAGIC_SIZE);
		Keys.m_Version = 1;
		Keys.m_Current = DDNET_XDP_KEY_EPOCHS - 1;
	}
	if(rotate_keys(&Keys, KeyMap, Options.m_pKeyPath, Options.m_pKeyGroup) != 0)
		goto out;
	LastRotate = time(NULL);

	AttachFlags = Options.m_SkbMode ? XDP_FLAGS_SKB_MODE : XDP_FLAGS_DRV_MODE;
	if(bpf_xdp_attach(InterfaceIndex, ProgramFd, AttachFlags, NULL) != 0)
	{
		if(Options.m_SkbMode)
		{
			log_error("cannot attach to %s: %s", Options.m_pInterface, strerror(errno));
			goto out;
		}
		/* Driver mode is where the saving is, because the packet is dropped before
		 * an skb exists. Generic mode still works and is better than nothing. */
		log_info("driver mode refused, falling back to generic mode");
		AttachFlags = XDP_FLAGS_SKB_MODE;
		if(bpf_xdp_attach(InterfaceIndex, ProgramFd, AttachFlags, NULL) != 0)
		{
			log_error("cannot attach to %s: %s", Options.m_pInterface, strerror(errno));
			goto out;
		}
	}

	/* Pinning lets `--stats` and bpftool look at the counters at any time, and keeps
	 * the maps alive if this process is replaced. */
	if(bpf_object__pin_maps(pObject, Options.m_pPinDir) != 0)
		log_info("could not pin the maps under %s (%s), --stats will not work",
			Options.m_pPinDir, strerror(errno));

	log_info("attached to %s in %s mode, %d ports, key in %s",
		Options.m_pInterface, AttachFlags == XDP_FLAGS_DRV_MODE ? "driver" : "generic",
		Options.m_NumPorts, Options.m_pKeyPath);
	if(Options.m_ConnPps)
		log_info("remembering 0.6 connections, %u packets per second each, forgotten after %u idle seconds",
			Options.m_ConnPps, Options.m_ConnIdleSeconds);
	else
		log_info("not remembering 0.6 connections, they share the unverified budget");
	if(Options.m_CountOnly)
		log_info("counting only, nothing will be dropped");
	else
		log_info("a port starts dropping after %u verified packets", Options.m_ArmAfter);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	time_t LastResolve = time(NULL);
	while(!s_Stop)
	{
		const time_t Now = time(NULL);
		sleep(1);

		if(Options.m_NumMasters && Now - LastResolve >= 3600)
		{
			/* Addresses are added, never removed: a master that moved keeps its old
			 * address accepted until restart, which is the harmless direction. */
			LastResolve = Now;
			for(Index = 0; Index < Options.m_NumMasters; Index++)
				add_master(MasterV4Map, MasterV6Map, Options.m_apMasters[Index]);
		}

		if(!Options.m_CountOnly)
		{
			for(Index = 0; Index < Options.m_NumPorts; Index++)
			{
				struct ddnet_xdp_port Port = {};
				if(bpf_map_lookup_elem(PortMap, &Options.m_aPorts[Index], &Port) != 0)
					continue;
				if(Port.m_Armed || Port.m_Verified < Options.m_ArmAfter)
					continue;
				/* The port has proven that the server on it derives tokens the same
				 * way, so dropping is now safe for it. A port whose server does not
				 * is left passing everything instead of being cut off. */
				Port.m_Armed = 1;
				if(bpf_map_update_elem(PortMap, &Options.m_aPorts[Index], &Port, BPF_ANY) == 0)
					log_info("port %u armed after %llu verified packets",
						Options.m_aPorts[Index], (unsigned long long)Port.m_Verified);
			}
		}

		if(Options.m_RotateSeconds && Now - LastRotate >= (time_t)Options.m_RotateSeconds)
		{
			if(rotate_keys(&Keys, KeyMap, Options.m_pKeyPath, Options.m_pKeyGroup) != 0)
				break;
			LastRotate = Now;
			log_info("rotated to key epoch %u", Keys.m_Current);
		}

		if(Options.m_Verbose)
			print_stats(StatsMap, NumCpus, &Options, Options.m_aPorts, Options.m_NumPorts, aPrevious, 1.0);
	}

	log_info("detaching");
	bpf_xdp_detach(InterfaceIndex, AttachFlags, NULL);
	bpf_object__unpin_maps(pObject, Options.m_pPinDir);
	print_stats(StatsMap, NumCpus, &Options, Options.m_aPorts, Options.m_NumPorts, NULL, 0);
	Result = 0;

out:
	if(pObject)
		bpf_object__close(pObject);
	return Result;
}
