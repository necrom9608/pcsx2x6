// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

// ILinkNet: HLE van Sony's i.LINK-socketbibliotheek ("ilsocket", ILSOCK.IRX) over UDP.
// Gebruikt door Namco System 246-linkspellen (o.a. Time Crisis 3 twin) die via
// ILDRV.IRX (iLINK_Server) sceILsock*-functies aanroepen. In plaats van de (niet
// geemuleerde) FireWire-controller gaan de datagrammen als UDP over het LAN.
//
// Instellingen (PCSX2.ini):
//   [ILinkNet]
//   Enabled = true             ; HLE aan/uit
//   Port = 37800               ; UDP-poort (zelfde op beide PC's)
//   PeerIP =                   ; leeg = broadcast 255.255.255.255, anders IP van de andere kast
//   PeerPort =                 ; optioneel, standaard = Port (enkel nodig om 2 emulators op 1 PC te testen)
//   GUID =                     ; optioneel: lage 32 bits van de eigen EUI-64 (hex); leeg = afgeleid van computernaam
//   Verbose = false

#pragma once

#include "common/Pcsx2Types.h"

namespace ILinkNet
{
	// ilsocket-exports (indices uit de importtabel van ILDRV.IRX)
	int Init();      // 4
	int Reset();     // 5
	int Open();      // 8
	int Close();     // 9
	int Bind();      // 10
	int Connect();   // 11
	int Send();      // 12
	int SendTo();    // 13
	int Recv();      // 14
	int RecvFrom();  // 15

	// ilink-exports
	int SbEui64();   // 27
	int SbNodeId();  // 28

	// Aangeroepen vanuit loadcore::RegisterLibraryEntries (adres van thbase::DelayThread bijhouden)
	void OnRegisterLibrary(u32 export_table);

	bool IsEnabled();
	void Shutdown();
} // namespace ILinkNet
