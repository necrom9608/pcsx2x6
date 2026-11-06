// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// Zie ILinkNet.h. Gereconstrueerd uit ILDRV.IRX (ildrv.c, Jun 21 2001) en TC3LOAD (nmILsocket.c):
//   sceILsockInit(int nsock, int pktsize)
//   sceILsockOpen(int domain=60, int type=2 (DGRAM), int proto)      -> sock
//   sceILsockBind(int sock, ilsock_addr* a, int len=16)
//   sceILsockConnect(int sock, ilsock_addr* a, int len=16)
//   sceILsockSend(int sock, void* buf, int len, int flags)              -> len
//   sceILsockSendTo(int sock, void* buf, int len, int flags, ilsock_addr* a, int alen)
//   sceILsockRecv(int sock, void* buf, int len, int flags)              -> len   (blokkeert)
//   sceILsockRecvFrom(int sock, void* buf, int len, int flags, ilsock_addr* from, int* fromlen)
//   sce1394SbEui64(u32 out[2]) -> 0 ; sce1394SbNodeId() -> node id
// ilsock_addr (16 bytes): u8 len, u8 family(60), u16 port (big-endian), u8 eui[8] (big-endian), u8 pad[4]

#include "common/Console.h"

#ifdef _WIN32
#include "common/RedtapeWindows.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12) // niet altijd in mstcpip.h (o.a. met WIN32_LEAN_AND_MEAN)
#endif
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ILinkNet.h"
#include "Host.h"
#include "IopMem.h"
#include "R3000A.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <deque>
#include <map>
#include <string>
#include <vector>

#define A0 (psxRegs.GPR.n.a0)
#define A1 (psxRegs.GPR.n.a1)
#define A2 (psxRegs.GPR.n.a2)
#define A3 (psxRegs.GPR.n.a3)
#define V0 (psxRegs.GPR.n.v0)
#define SP (psxRegs.GPR.n.sp)
#define RA (psxRegs.GPR.n.ra)
#define PC (psxRegs.pc)

namespace ILinkNet
{
#ifdef _WIN32
	using sock_t = SOCKET;
	static constexpr sock_t BAD_SOCK = INVALID_SOCKET;
	static void CloseSock(sock_t s) { closesocket(s); }
#else
	using sock_t = int;
	static constexpr sock_t BAD_SOCK = -1;
	static void CloseSock(sock_t s) { close(s); }
#endif

	static constexpr u32 MAGIC = 0x314C4354; // "TCL1"
	static constexpr u32 EUI_HI = 0x08004607; // vendor-deel zoals de originele GUID
	static constexpr u32 BCAST = 0xFFFFFFFF;
	static constexpr int MAX_SOCKS = 16;
	static constexpr size_t MAX_QUEUE = 256;
	static constexpr size_t MAX_PAYLOAD = 2048;

#pragma pack(push, 1)
	struct WireHdr
	{
		u32 magic;
		u32 src_hi, src_lo;
		u32 dst_hi, dst_lo;
		u16 src_port, dst_port;
	};
#pragma pack(pop)

	struct Packet
	{
		u32 src_hi, src_lo;
		u16 src_port;
		std::vector<u8> data;
	};

	struct Sock
	{
		bool used = false;
		u16 port = 0;
		bool connected = false;
		u32 peer_hi = 0, peer_lo = 0;
		u16 peer_port = 0;
		std::deque<Packet> queue;
	};

	struct Pending
	{
		u32 a0, a1, a2, a3, ra;
	};

	static bool s_cfg_loaded = false;
	static bool s_enabled = true;
	static bool s_verbose = false;
	static int s_udp_port = 37800;
	static int s_peer_port = 37800;
	static std::string s_peer_ip;
	static u32 s_my_lo = 0;

	static sock_t s_udp = BAD_SOCK;
	static bool s_net_failed = false;
	static std::array<Sock, MAX_SOCKS> s_socks;
	static std::map<u64, sockaddr_in> s_peers; // EUI -> IP (geleerd uit ontvangen pakketten)
	static std::map<u32, Pending> s_pending;   // sp -> bewaarde argumenten van een blokkerende Recv
	static u32 s_delay_thread = 0;              // thbase::DelayThread (export 33)
	static u16 s_next_ephemeral = 0x8000;
	static u64 s_tx_count = 0, s_rx_count = 0;
	static bool s_logged_peer = false;

	// ------------------------------------------------------------------------------------------
	static u32 Fnv1a(const std::string& s)
	{
		u32 h = 0x811C9DC5;
		for (unsigned char c : s)
		{
			h ^= c;
			h *= 0x01000193;
		}
		return h;
	}

	static std::string HostName()
	{
		char buf[256] = {};
#ifdef _WIN32
		DWORD n = sizeof(buf);
		if (!GetComputerNameA(buf, &n))
			return "pcsx2";
#else
		if (gethostname(buf, sizeof(buf) - 1) != 0)
			return "pcsx2";
#endif
		return buf;
	}

	static void LoadConfig()
	{
		if (s_cfg_loaded)
			return;
		s_cfg_loaded = true;
		s_enabled = Host::GetBaseBoolSettingValue("ILinkNet", "Enabled", true);
		s_verbose = Host::GetBaseBoolSettingValue("ILinkNet", "Verbose", false);
		s_udp_port = Host::GetBaseIntSettingValue("ILinkNet", "Port", 37800);
		s_peer_port = Host::GetBaseIntSettingValue("ILinkNet", "PeerPort", s_udp_port);
		s_peer_ip = Host::GetBaseStringSettingValue("ILinkNet", "PeerIP", "");
		const std::string guid = Host::GetBaseStringSettingValue("ILinkNet", "GUID", "");
		if (!guid.empty())
			s_my_lo = static_cast<u32>(std::strtoul(guid.c_str(), nullptr, 16));
		else
			s_my_lo = Fnv1a(HostName());
		if (s_my_lo == 0 || s_my_lo == BCAST)
			s_my_lo ^= 0x5A5A5A5A;
		Console.WriteLn("ILinkNet: %s, UDP-poort %d, peer %s, eigen GUID %08x:%08x",
			s_enabled ? "aan" : "uit", s_udp_port, s_peer_ip.empty() ? "(broadcast)" : s_peer_ip.c_str(), EUI_HI, s_my_lo);
	}

	bool IsEnabled()
	{
		LoadConfig();
		return s_enabled;
	}

	static bool EnsureNet()
	{
		if (s_udp != BAD_SOCK)
			return true;
		if (s_net_failed)
			return false;
#ifdef _WIN32
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
		s_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
		if (s_udp == BAD_SOCK)
		{
			Console.Error("ILinkNet: socket() mislukt");
			s_net_failed = true;
			return false;
		}
		int one = 1;
		setsockopt(s_udp, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof(one));
		setsockopt(s_udp, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
#ifdef _WIN32
		u_long nb = 1;
		ioctlsocket(s_udp, FIONBIO, &nb);
		BOOL no_reset = FALSE;
		DWORD ret = 0;
		WSAIoctl(s_udp, SIO_UDP_CONNRESET, &no_reset, sizeof(no_reset), nullptr, 0, &ret, nullptr, nullptr);
#else
		fcntl(s_udp, F_SETFL, fcntl(s_udp, F_GETFL, 0) | O_NONBLOCK);
#endif
		sockaddr_in addr = {};
		addr.sin_family = AF_INET;
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
		addr.sin_port = htons(static_cast<u16>(s_udp_port));
		if (bind(s_udp, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0)
		{
			Console.Error("ILinkNet: bind op UDP-poort %d mislukt (draait er al een emulator?)", s_udp_port);
			CloseSock(s_udp);
			s_udp = BAD_SOCK;
			s_net_failed = true;
			return false;
		}
		Console.WriteLn("ILinkNet: luistert op UDP-poort %d", s_udp_port);
		return true;
	}

	void Shutdown()
	{
		if (s_udp != BAD_SOCK)
			CloseSock(s_udp);
		s_udp = BAD_SOCK;
		s_net_failed = false;
		for (Sock& s : s_socks)
			s = Sock{};
		s_pending.clear();
	}

	static Sock* GetSock(u32 id)
	{
		if (id >= MAX_SOCKS || !s_socks[id].used)
			return nullptr;
		return &s_socks[id];
	}

	static Sock* FindByPort(u16 port)
	{
		for (Sock& s : s_socks)
			if (s.used && s.port == port)
				return &s;
		return nullptr;
	}

	// ilsock_addr uit IOP-geheugen lezen
	static void ReadAddr(u32 addr, u16& port, u32& hi, u32& lo)
	{
		u8 b[12];
		for (int i = 0; i < 12; i++)
			b[i] = iopMemRead8(addr + i);
		port = static_cast<u16>((b[2] << 8) | b[3]);
		hi = (u32(b[4]) << 24) | (u32(b[5]) << 16) | (u32(b[6]) << 8) | b[7];
		lo = (u32(b[8]) << 24) | (u32(b[9]) << 16) | (u32(b[10]) << 8) | b[11];
	}

	static void WriteAddr(u32 addr, u16 port, u32 hi, u32 lo)
	{
		const u8 b[16] = {16, 60, u8(port >> 8), u8(port), u8(hi >> 24), u8(hi >> 16), u8(hi >> 8), u8(hi),
			u8(lo >> 24), u8(lo >> 16), u8(lo >> 8), u8(lo), 0, 0, 0, 0};
		for (int i = 0; i < 16; i++)
			iopMemWrite8(addr + i, b[i]);
	}

	static void WriteGuest(u32 addr, const u8* data, u32 len)
	{
		if (iopMemSafeWriteBytes(addr, data, len))
			psxCpu->Clear(addr, (len + 3) / 4);
		else
			for (u32 i = 0; i < len; i++)
				iopMemWrite8(addr + i, data[i]);
	}

	static void Poll()
	{
		if (!EnsureNet())
			return;
		u8 buf[sizeof(WireHdr) + MAX_PAYLOAD];
		for (int guard = 0; guard < 512; guard++)
		{
			sockaddr_in from = {};
#ifdef _WIN32
			int flen = sizeof(from);
			const int n = recvfrom(s_udp, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen);
#else
			socklen_t flen = sizeof(from);
			const int n = static_cast<int>(recvfrom(s_udp, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &flen));
#endif
			if (n <= 0)
				break;
			if (n < static_cast<int>(sizeof(WireHdr)))
				continue;
			WireHdr h;
			std::memcpy(&h, buf, sizeof(h));
			if (h.magic != MAGIC)
				continue;
			if (h.src_hi == EUI_HI && h.src_lo == s_my_lo)
				continue; // eigen broadcast
			const bool for_me = (h.dst_hi == BCAST && h.dst_lo == BCAST) || (h.dst_hi == EUI_HI && h.dst_lo == s_my_lo);
			if (!for_me)
				continue;

			s_peers[(u64(h.src_hi) << 32) | h.src_lo] = from;
			if (!s_logged_peer)
			{
				char ip[64] = {};
				inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
				Console.WriteLn("ILinkNet: eerste pakket van andere kast %08x:%08x (%s)", h.src_hi, h.src_lo, ip);
				s_logged_peer = true;
			}

			Sock* s = FindByPort(h.dst_port);
			if (!s)
				continue;
			if (s->queue.size() >= MAX_QUEUE)
				s->queue.pop_front();
			Packet p;
			p.src_hi = h.src_hi;
			p.src_lo = h.src_lo;
			p.src_port = h.src_port;
			p.data.assign(buf + sizeof(WireHdr), buf + n);
			s->queue.push_back(std::move(p));
			s_rx_count++;
		}
	}

	static int SendPacket(const Sock& s, u32 dst_hi, u32 dst_lo, u16 dst_port, u32 buf, u32 len)
	{
		if (!EnsureNet())
			return -1;
		if (len > MAX_PAYLOAD)
			len = MAX_PAYLOAD;
		std::vector<u8> out(sizeof(WireHdr) + len);
		WireHdr h = {MAGIC, EUI_HI, s_my_lo, dst_hi, dst_lo, s.port, dst_port};
		std::memcpy(out.data(), &h, sizeof(h));
		if (!iopMemSafeReadBytes(buf, out.data() + sizeof(h), len))
			for (u32 i = 0; i < len; i++)
				out[sizeof(h) + i] = iopMemRead8(buf + i);

		sockaddr_in to = {};
		to.sin_family = AF_INET;
		to.sin_port = htons(static_cast<u16>(s_peer_port));
		const auto it = s_peers.find((u64(dst_hi) << 32) | dst_lo);
		if (it != s_peers.end())
		{
			to.sin_addr = it->second.sin_addr;
		}
		else if (!s_peer_ip.empty())
		{
			inet_pton(AF_INET, s_peer_ip.c_str(), &to.sin_addr);
		}
		else
		{
			to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
		}
		sendto(s_udp, reinterpret_cast<const char*>(out.data()), static_cast<int>(out.size()), 0,
			reinterpret_cast<const sockaddr*>(&to), sizeof(to));
		s_tx_count++;
		if (s_verbose && (s_tx_count % 600) == 1)
			Console.WriteLn("ILinkNet: tx=%llu rx=%llu", s_tx_count, s_rx_count);
		return static_cast<int>(len);
	}

	static int Return(s32 value)
	{
		V0 = static_cast<u32>(value);
		PC = RA;
		return 1;
	}

	// ------------------------------------------------------------------------------------------
	int Init()
	{
		if (!IsEnabled())
			return 0;
		for (Sock& s : s_socks)
			s = Sock{};
		s_pending.clear();
		EnsureNet();
		Console.WriteLn("ILinkNet: sceILsockInit(%d, %d)", A0, A1);
		return Return(0);
	}

	int Reset()
	{
		if (!IsEnabled())
			return 0;
		return Return(0);
	}

	int Open()
	{
		if (!IsEnabled())
			return 0;
		for (int i = 0; i < MAX_SOCKS; i++)
		{
			if (!s_socks[i].used)
			{
				s_socks[i] = Sock{};
				s_socks[i].used = true;
				if (s_verbose)
					Console.WriteLn("ILinkNet: open(%d,%d,%d) -> %d", A0, A1, A2, i);
				return Return(i);
			}
		}
		return Return(-1);
	}

	int Close()
	{
		if (!IsEnabled())
			return 0;
		if (Sock* s = GetSock(A0))
			*s = Sock{};
		return Return(0);
	}

	int Bind()
	{
		if (!IsEnabled())
			return 0;
		Sock* s = GetSock(A0);
		if (!s)
			return Return(-1);
		u16 port;
		u32 hi, lo;
		ReadAddr(A1, port, hi, lo);
		if (port == 0)
			port = s_next_ephemeral++;
		s->port = port;
		Console.WriteLn("ILinkNet: bind(sock %d, poort 0x%04x)", A0, port);
		return Return(0);
	}

	int Connect()
	{
		if (!IsEnabled())
			return 0;
		Sock* s = GetSock(A0);
		if (!s)
			return Return(-1);
		ReadAddr(A1, s->peer_port, s->peer_hi, s->peer_lo);
		s->connected = true;
		Console.WriteLn("ILinkNet: connect(sock %d -> %08x:%08x poort 0x%04x)", A0, s->peer_hi, s->peer_lo, s->peer_port);
		return Return(0);
	}

	int Send()
	{
		if (!IsEnabled())
			return 0;
		Sock* s = GetSock(A0);
		if (!s || !s->connected)
			return Return(-1);
		return Return(SendPacket(*s, s->peer_hi, s->peer_lo, s->peer_port, A1, A2));
	}

	int SendTo()
	{
		if (!IsEnabled())
			return 0;
		Sock* s = GetSock(A0);
		if (!s)
			return Return(-1);
		const u32 addr = iopMemRead32(SP + 16);
		u16 port;
		u32 hi, lo;
		ReadAddr(addr, port, hi, lo);
		return Return(SendPacket(*s, hi, lo, port, A1, A2));
	}

	// Gemeenschappelijke ontvangst; blokkeert door de IOP-thread via DelayThread(1 ms) te laten
	// wachten en daarna de oorspronkelijke 'jal' opnieuw uit te voeren.
	static int DoRecv(bool with_from)
	{
		const auto pend = s_pending.find(SP);
		if (pend != s_pending.end())
		{
			if (pend->second.ra == RA)
			{
				A0 = pend->second.a0;
				A1 = pend->second.a1;
				A2 = pend->second.a2;
				A3 = pend->second.a3;
			}
			s_pending.erase(pend);
		}

		Poll();
		Sock* s = GetSock(A0);
		if (!s)
			return Return(-1);

		if (s->queue.empty())
		{
			const u32 jal_site = RA - 8;
			const u32 insn = iopMemRead32(jal_site);
			if (s_delay_thread != 0 && (insn >> 26) == 3)
			{
				s_pending[SP] = Pending{A0, A1, A2, A3, RA};
				A0 = 1000;      // 1 ms
				RA = jal_site;  // na DelayThread opnieuw sceILsockRecv aanroepen
				PC = s_delay_thread;
				return 1;
			}
			return Return(-1);
		}

		Packet p = std::move(s->queue.front());
		s->queue.pop_front();
		const u32 n = std::min<u32>(A2, static_cast<u32>(p.data.size()));
		WriteGuest(A1, p.data.data(), n);
		if (with_from)
		{
			const u32 from = iopMemRead32(SP + 16);
			const u32 fromlen = iopMemRead32(SP + 20);
			if (from)
				WriteAddr(from, p.src_port, p.src_hi, p.src_lo);
			if (fromlen)
				iopMemWrite32(fromlen, 16);
		}
		return Return(static_cast<s32>(n));
	}

	int Recv()
	{
		if (!IsEnabled())
			return 0;
		return DoRecv(false);
	}

	int RecvFrom()
	{
		if (!IsEnabled())
			return 0;
		return DoRecv(true);
	}

	int SbEui64()
	{
		if (!IsEnabled())
			return 0;
		iopMemWrite32(A0, EUI_HI);
		iopMemWrite32(A0 + 4, s_my_lo);
		return Return(0);
	}

	int SbNodeId()
	{
		if (!IsEnabled())
			return 0;
		return Return(0xFFC0);
	}

	void OnRegisterLibrary(u32 export_table)
	{
		// export-tabel: +12 naam[8], +20 functiepointers
		char name[9] = {};
		for (int i = 0; i < 8; i++)
			name[i] = static_cast<char>(iopMemRead8(export_table + 12 + i));
		if (std::strncmp(name, "thbase", 8) == 0)
		{
			s_delay_thread = iopMemRead32(export_table + 20 + 33 * 4);
			if (s_verbose)
				Console.WriteLn("ILinkNet: thbase::DelayThread op 0x%08x", s_delay_thread);
		}
	}
} // namespace ILinkNet
