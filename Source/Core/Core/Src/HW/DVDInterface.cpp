// Copyright (C) 2003-2009 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/

#include "Common.h" // Common
#include "ChunkFile.h"

#include "StreamADPCM.H" // Core
#include "DVDInterface.h"
#include "../PowerPC/PowerPC.h"
#include "PeripheralInterface.h"
#include "Memmap.h"
#include "Thread.h"
#include "../VolumeHandler.h"

namespace DVDInterface
{

/*
20975: 00000000 DVD (zzz_80146b84 ??, 0x80146bf8) : DVD(r): 0xcc006004
20976: 00000000 DVD (zzz_80146b84 ??, 0x80146c00) : DVD(w): 0x00000000 @ 0xcc006004
20977: 00000000 DVD (DVDLowRead, 0x801448a8) : DVD(w): 0x00000020 @ 0xcc006018
20978: 00000000 DVD (Read, 0x80144744) : DVD(w): 0xa8000000 @ 0xcc006008
20979: 00000000 DVD (Read, 0x80144750) : DVD(w): 0x01094227 @ 0xcc00600c
20980: 00000000 DVD (Read, 0x80144758) : DVD(w): 0x00000020 @ 0xcc006010
20981: 00000000 DVD (Read, 0x8014475c) : DVD(w): 0x8167cc80 @ 0xcc006014
20982: 00000000 DVD (Read, 0x80144760) : DVD(w): 0x00000020 @ 0xcc006018
20983: 00000000 DVD (Read, 0x80144768) : DVD(w): 0x00000003 @ 0xcc00601c
20984: 00000000 DVD: DVD: Read ISO: DVDOffset=0425089c, DMABuffer=0167cc80, SrcLength=00000020, DMALength=00000020
20989: 00000000 DVD (zzz_801442fc ??, 0x80144388) : DVD(r): 0xcc006000
20990: 00000000 DVD (zzz_801442fc ??, 0x801443d8) : DVD(w): 0x0000003a @ 0xcc006000
20992: 00000000 DVD (zzz_801442fc ??, 0x801444d0) : DVD(w): 0x00000000 @ 0xcc006004
20993: 00000000 DVD (zzz_80146e44 ??, 0x80146fcc) : DVD(r): 0xcc006018

After this, Cubivore infinitely calls DVDGetDriveStatus, which does not even
bother to check any DVD regs. Waiting for interrupt?
*/

// internal hardware addresses
enum
{
	DI_STATUS_REGISTER			= 0x00,
	DI_COVER_REGISTER			= 0x04,
	DI_COMMAND_0				= 0x08,
	DI_COMMAND_1				= 0x0C,
	DI_COMMAND_2				= 0x10,
	DI_DMA_ADDRESS_REGISTER		= 0x14,
	DI_DMA_LENGTH_REGISTER		= 0x18, 
	DI_DMA_CONTROL_REGISTER		= 0x1C,
	DI_IMMEDIATE_DATA_BUFFER	= 0x20,
	DI_CONFIG_REGISTER			= 0x24
};


// DVD IntteruptTypes
enum DVDInterruptType
{
	INT_DEINT		= 0,
	INT_TCINT		= 1,
	INT_BRKINT		= 2,
	INT_CVRINT      = 3,
};

// debug commands which may be ORd
enum
{
	STOP_DRIVE	= 0,
	START_DRIVE	= 0x100,
	ACCEPT_COPY	= 0x4000,
	DISC_CHECK	= 0x8000,
};

// DI Status Register
union UDISR	
{
	u32 Hex;
	struct  
	{		
		unsigned BREAK          :  1;		// Stop the Device + Interrupt
		unsigned DEINITMASK     :  1;		// Access Device Error Int Mask
		unsigned DEINT          :  1;		// Access Device Error Int
		unsigned TCINTMASK      :  1;		// Transfer Complete Int Mask
		unsigned TCINT          :  1;		// Transfer Complete Int
		unsigned BRKINTMASK     :  1;
		unsigned BRKINT         :  1;		// w 1: clear brkint
		unsigned                : 25;
	};
	UDISR() {Hex = 0;}
	UDISR(u32 _hex) {Hex = _hex;}
};

// DI Cover Register
union UDICVR
{
	u32 Hex;
	struct  
	{		
		unsigned CVR            :  1;		// 0: Cover closed	1: Cover open
		unsigned CVRINTMASK	    :  1;		// 1: Interrupt enabled;
		unsigned CVRINT         :  1;		// r 1: Interrupt requested w 1: Interrupt clear
		unsigned                : 29;
	};
	UDICVR() {Hex = 0;}
	UDICVR(u32 _hex) {Hex = _hex;}
};

// DI DMA Address Register
union UDIDMAAddressRegister
{
	u32 Hex;
	struct 
	{
		unsigned Address	:	26;
		unsigned			:	6;
	};
};

// DI DMA Address Length Register
union UDIDMAAddressLength
{
	u32 Hex;
	struct 
	{
		unsigned Length		:  26;
		unsigned			:	6;
	};
};

// DI DMA Control Register
union UDIDMAControlRegister
{
	u32 Hex;
	struct 
	{
		unsigned TSTART		:	1;		// w:1 start   r:0 ready
		unsigned DMA		:	1;		// 1: DMA Mode    0: Immediate Mode (can only do Access Register Command)
		unsigned RW			:	1;		// 0: Read Command (DVD to Memory)  1: Write COmmand (Memory to DVD)
		unsigned			:  29;
	};
};

// DI Config Register
union UDIConfigRegister
{
	u32 Hex;
	struct 
	{
		unsigned CONFIG		:	8;
		unsigned			:  24;
	};
	UDIConfigRegister() {Hex = 0;}
	UDIConfigRegister(u32 _hex) {Hex = _hex;}
};

// HACK to allow multi-command debug-mode transfers
struct SDIDebugTransfer 
{
	u32 Address;
	u16 Length;
	bool InProgress;
};

// hardware registers
struct DVDMemStruct
{
	UDISR				StatusReg;
	UDICVR				CoverReg;
	u32					Command[3];
	UDIDMAAddressRegister	DMAAddress;
	UDIDMAAddressLength		DMALength;
	UDIDMAControlRegister	DMAControlReg;
	u32					Immediate;
	UDIConfigRegister	ConfigReg;
	u32					AudioStart;
	u32					AudioPos;
	u32					AudioLength;
	SDIDebugTransfer	DebugTransfer;
};

// STATE_TO_SAVE
DVDMemStruct dvdMem;
u32	 g_ErrorCode = 0x00;
bool g_bDiscInside = false;

Common::CriticalSection dvdread_section;

void DoState(PointerWrap &p)
{
	p.Do(dvdMem);
	p.Do(g_ErrorCode);
	p.Do(g_bDiscInside);
}

void UpdateInterrupts();
void GenerateDVDInterrupt(DVDInterruptType _DVDInterrupt);
void ExecuteCommand(UDIDMAControlRegister& _DMAControlReg);

void Init()
{
	dvdMem.StatusReg.Hex	  = 0;
	dvdMem.CoverReg.Hex		  = 0;
	dvdMem.Command[0]		  = 0;
	dvdMem.Command[1]		  = 0;
	dvdMem.Command[2]		  = 0;
	dvdMem.DMAAddress.Hex	  = 0;
	dvdMem.DMALength.Hex	  = 0;
	dvdMem.DMAControlReg.Hex  = 0;
	dvdMem.Immediate		  = 0;
	dvdMem.ConfigReg.Hex	  = 0;
	dvdMem.AudioStart	      = 0;
	dvdMem.AudioPos           = 0;
	dvdMem.AudioLength        = 0;

//	SetLidOpen(true);
}

void Shutdown()
{

}

void SetDiscInside(bool _DiscInside)
{
    g_bDiscInside = _DiscInside;
}

void SetLidOpen(bool _bOpen)
{
	if (_bOpen)
		dvdMem.CoverReg.CVR = 1;
	else
		dvdMem.CoverReg.CVR = 0;

	GenerateDVDInterrupt(INT_CVRINT);
}

bool IsLidOpen()
{	
	if (dvdMem.CoverReg.CVR)
		return true;
	else
		return false;
}

bool DVDRead(u32 _iDVDOffset, u32 _iRamAddress, u32 _iLength)
{
	// We won't need the crit sec when DTK streaming has been rewritten correctly.
	dvdread_section.Enter();
	bool retval = VolumeHandler::ReadToPtr(Memory::GetPointer(_iRamAddress), _iDVDOffset, _iLength);
	dvdread_section.Leave();
	return retval;
}

bool DVDReadADPCM(u8* _pDestBuffer, u32 _iNumSamples)
{
	if (dvdMem.AudioPos == 0)
	{
		//MessageBox(0,"DVD: Trying to stream from 0", "bah", 0);
		memset(_pDestBuffer, 0, _iNumSamples); // probably __AI_SRC_INIT :P
		return false;
	}
	_iNumSamples &= ~31;
	dvdread_section.Enter();
	VolumeHandler::ReadToPtr(_pDestBuffer, dvdMem.AudioPos, _iNumSamples);
	dvdread_section.Leave();

	//
	// FIX THIS
	//
	// loop check
	//
	dvdMem.AudioPos += _iNumSamples;
	if (dvdMem.AudioPos >= dvdMem.AudioStart + dvdMem.AudioLength)
	{
		dvdMem.AudioPos = dvdMem.AudioStart;
		NGCADPCM::InitFilter();
	}

	//LOG(DVDINTERFACE,"ReadADPCM");
	return true;
}

void Read32(u32& _uReturnValue, const u32 _iAddress)
{
	DEBUG_LOG(DVDINTERFACE, "DVD(r): 0x%08x", _iAddress);

	switch (_iAddress & 0xFFF)
	{
	case DI_STATUS_REGISTER:		_uReturnValue = dvdMem.StatusReg.Hex; return;
	case DI_COVER_REGISTER:			_uReturnValue = dvdMem.CoverReg.Hex; return;
	case DI_COMMAND_0:				_uReturnValue = dvdMem.Command[0]; return;
	case DI_COMMAND_1:				_uReturnValue = dvdMem.Command[1]; return;
	case DI_COMMAND_2:				_uReturnValue = dvdMem.Command[2]; return;
	case DI_DMA_ADDRESS_REGISTER:	_uReturnValue = dvdMem.DMAAddress.Hex; return;
	case DI_DMA_LENGTH_REGISTER:	_uReturnValue = dvdMem.DMALength.Hex; return;
	case DI_DMA_CONTROL_REGISTER:	_uReturnValue = dvdMem.DMAControlReg.Hex; return;
	case DI_IMMEDIATE_DATA_BUFFER:	_uReturnValue = dvdMem.Immediate; return;
	case DI_CONFIG_REGISTER:	
		{
			dvdMem.ConfigReg.Hex = 0x000000FF;
			_uReturnValue = dvdMem.ConfigReg.Hex; 
			return;
		}

	default:
		_dbg_assert_(DVDINTERFACE,0);		
	}

	_uReturnValue = 0;
}

void Write32(const u32 _iValue, const u32 _iAddress)
{
	DEBUG_LOG(DVDINTERFACE,  "DVD(w): 0x%08x @ 0x%08x", _iValue, _iAddress);

	switch (_iAddress & 0x3FF)
	{
	case DI_STATUS_REGISTER:
		{
			UDISR tmpStatusReg(_iValue);

			dvdMem.StatusReg.DEINITMASK	= tmpStatusReg.DEINITMASK;
			dvdMem.StatusReg.TCINTMASK	= tmpStatusReg.TCINTMASK;
			dvdMem.StatusReg.BRKINTMASK	= tmpStatusReg.BRKINTMASK;
			
			if (tmpStatusReg.DEINT)		dvdMem.StatusReg.DEINT = 0;
			if (tmpStatusReg.TCINT)		dvdMem.StatusReg.TCINT = 0;
			if (tmpStatusReg.BRKINT)	dvdMem.StatusReg.BRKINT = 0;

			if (tmpStatusReg.BREAK) 
			{
				_dbg_assert_(DVDINTERFACE, 0);
			}

			UpdateInterrupts();
		}
		break;

	case DI_COVER_REGISTER:	
		{
			UDICVR tmpCoverReg(_iValue);

			dvdMem.CoverReg.CVRINTMASK = tmpCoverReg.CVRINTMASK;

			if (tmpCoverReg.CVRINT)	dvdMem.CoverReg.CVRINT = 0;
			
			UpdateInterrupts();
		}
		break;

	case DI_COMMAND_0:				dvdMem.Command[0] = _iValue; break;
	case DI_COMMAND_1:				dvdMem.Command[1] = _iValue; break;
	case DI_COMMAND_2:				dvdMem.Command[2] = _iValue; break;

	case DI_DMA_ADDRESS_REGISTER:
		{
			dvdMem.DMAAddress.Hex = _iValue; 
			_dbg_assert_(DVDINTERFACE, ((dvdMem.DMAAddress.Hex & 0x1F) == 0));
		}
		break;
	case DI_DMA_LENGTH_REGISTER:	dvdMem.DMALength.Hex  = _iValue; break;
	case DI_DMA_CONTROL_REGISTER:	
		{
			dvdMem.DMAControlReg.Hex = _iValue;
			if (dvdMem.DMAControlReg.TSTART)
			{
				ExecuteCommand(dvdMem.DMAControlReg);
			}
		}
		break;

	case DI_IMMEDIATE_DATA_BUFFER:	dvdMem.Immediate = _iValue; break;
	case DI_CONFIG_REGISTER:
		{	
			UDIConfigRegister tmpConfigReg(_iValue);

			dvdMem.ConfigReg.CONFIG = tmpConfigReg.CONFIG;
		}
		break;

	default:
		_dbg_assert_msg_(DVDINTERFACE, 0, "Write to unknown DI address 0x%08x", _iAddress);
		break;
	}
}

void UpdateInterrupts()
{
	if ((dvdMem.StatusReg.DEINT	& dvdMem.StatusReg.DEINITMASK) ||
		(dvdMem.StatusReg.TCINT	& dvdMem.StatusReg.TCINTMASK)  ||
		(dvdMem.StatusReg.BRKINT & dvdMem.StatusReg.BRKINTMASK) ||
		(dvdMem.CoverReg.CVRINT	& dvdMem.CoverReg.CVRINTMASK))
	{
		CPeripheralInterface::SetInterrupt(CPeripheralInterface::INT_CAUSE_DI, true);
	}
	else
	{
		CPeripheralInterface::SetInterrupt(CPeripheralInterface::INT_CAUSE_DI, false);
	}
}

void GenerateDVDInterrupt(DVDInterruptType _DVDInterrupt)
{
	switch(_DVDInterrupt) 
	{
	case INT_DEINT:		dvdMem.StatusReg.DEINT	= 1; break;
	case INT_TCINT:		dvdMem.StatusReg.TCINT	= 1; break;
	case INT_BRKINT:	dvdMem.StatusReg.BRKINT	= 1; break;
	case INT_CVRINT:	dvdMem.CoverReg.CVRINT	= 1; break;
	}

	UpdateInterrupts();
}

bool m_bStream = false;

void ExecuteCommand(UDIDMAControlRegister& _DMAControlReg)
{
	_dbg_assert_(DVDINTERFACE, _DMAControlReg.RW == 0); // only DVD to Memory

	// Catch multi-command transfers here
	if (dvdMem.DebugTransfer.InProgress)
	{
		dvdMem.DebugTransfer.InProgress = false;
		// If we ever need to actually read/write the drive ram/cache, here would be the place
		// Up to 12bytes can be written at once (dvdMem.Command[0] through dvdMem.Command[2])
		INFO_LOG(DVDINTERFACE, "\t queued cmd: 0x%08x @ 0x%08x NOT IMPLEMENTED",
			dvdMem.Command[0], dvdMem.DebugTransfer.Address);
	}
	else
	{ // The huge switch is in this else!
	switch ((dvdMem.Command[0] & 0xFF000000) >> 24)
	{
	//=========================================================================================================
	// DRIVE INFO (DMA)
	//	Command/Subcommand/Padding <- 12000000
	//	Command0                   <- 0
	//	Command1                   <- 0x20
	//	Command2                   <- Address in ram of the buffer
	//
	//	output buffer:        
	// 	 0000-0001  revisionLevel
	//	 0002-0003  deviceCode 
	//   0004-0007  releaseDate 
	//	 0008-001F  padding(0)
	//=========================================================================================================
	case 0x12:
		{
			// small safety check, dunno if it's needed
			if ((dvdMem.Command[1] == 0) && (dvdMem.DMALength.Length == 0x20))
			{
				u8* driveInfo = Memory::GetPointer(dvdMem.DMAAddress.Address);
				// gives the correct output in GCOS - 06 2001/08 (61)
				// there may be other stuff missing ?
				driveInfo[4] = 0x20;
				driveInfo[5] = 0x01;
				driveInfo[6] = 0x06;
				driveInfo[7] = 0x08;
				driveInfo[8] = 0x61;

				// Just for fun
				INFO_LOG(DVDINTERFACE, "Drive Info: %02x %02x%02x/%02x (%02x)",
					driveInfo[6], driveInfo[4], driveInfo[5], driveInfo[7], driveInfo[8]);
			}
		}
		break;

	//=========================================================================================================
	// SET EXTENSION
	// Apparently the drive needs certain flags set explicitly?
	//=========================================================================================================
	case 0x55:
		INFO_LOG(DVDINTERFACE, "SetExtension %x", _DMAControlReg);
		break;

	//=========================================================================================================
	// READ (DMA)
	//	Command/Subcommand/Padding <- A8000000
	//	Command0                   <- Position on DVD shr 2 
	//	Command1                   <- Length of the read
	//	Command2                   <- Address in ram of the buffer
	//=========================================================================================================
	case 0xA8:
		{	
			if (g_bDiscInside)
			{
				u32 iDVDOffset = dvdMem.Command[1] << 2;
				u32 iSrcLength = dvdMem.Command[2];

				INFO_LOG(DVDINTERFACE, "Read ISO: DVDOffset=%08x, DMABuffer=%08x, SrcLength=%08x, DMALength=%08x",
					iDVDOffset, dvdMem.DMAAddress.Address, iSrcLength, dvdMem.DMALength.Length);
				_dbg_assert_(DVDINTERFACE, iSrcLength == dvdMem.DMALength.Length);

				if (!DVDRead(iDVDOffset, dvdMem.DMAAddress.Address, dvdMem.DMALength.Length))
				{
					PanicAlert("Cant read from DVD_Plugin - DVD-Interface: Fatal Error");
				}
			}
			else
			{
				// there is no disc to read
				_DMAControlReg.TSTART = 0;
				dvdMem.DMALength.Length = 0;
				GenerateDVDInterrupt(INT_DEINT);
				g_ErrorCode = 0x03023A00;
				return;
			}
		}
		break;

	//=========================================================================================================
	// SEEK (Immediate)
	//	Command/Subcommand/Padding <- AB000000
	//	Command0                   <- Position on DVD shr 2 
	//=========================================================================================================
	case 0xAB:
		{
#if MAX_LOGLEVEL >= DEBUG_LEVEL
			u32 offset = dvdMem.Command[1] << 2;
#endif
			DEBUG_LOG(DVDINTERFACE, "Seek: offset=%08x  (ignoring)", offset);
		}		
		break;

	//=========================================================================================================
	// REQUEST ERROR (Immediate)
	//	Command/Subcommand/Padding <- E0000000
	//=========================================================================================================
	case 0xE0:
		ERROR_LOG(DVDINTERFACE, "Requesting error");
		dvdMem.Immediate = g_ErrorCode;
		break;

	//=========================================================================================================
	// AUDIOSTREAM (Immediate)
	//	Command/Subcommand/Padding <- E1??0000    ?? = subcommand
	//	Command0                   <- Position on DVD shr 2 
	//	Command1                   <- Length of the stream
	//=========================================================================================================
	case 0xE1:
		{
			// i dunno if we need this check
			// if (m_bStream)
			// MessageBox(NULL, "dont overwrite a stream while you play it", "FATAL ERROR", MB_OK);

			// subcommand

			// ugly hack to catch the disable command
			if (dvdMem.Command[1]!=0)
			{
#if MAX_LOGLEVEL >= DEBUG_LEVEL
				u8 subCommand = (dvdMem.Command[0] & 0x00FF0000) >> 16;
#endif

				dvdMem.AudioPos = dvdMem.Command[1] << 2;
				dvdMem.AudioStart = dvdMem.AudioPos;
				dvdMem.AudioLength = dvdMem.Command[2];
				NGCADPCM::InitFilter();			

				m_bStream = true;

				DEBUG_LOG(DVDINTERFACE, "DVD(Audio) Stream subcmd = %02x offset = %08x length=%08x",
					subCommand, dvdMem.AudioPos, dvdMem.AudioLength);
			}			
		}		
		break;

	//=========================================================================================================
	// REQUEST AUDIO STATUS (Immediate)
	//	Command/Subcommand/Padding <- E2000000
	//=========================================================================================================
	case 0xE2:
		{
	/*		if (m_bStream)
				dvdMem.Immediate = 1;
			else
				dvdMem.Immediate = 0;*/
		}
		DEBUG_LOG(DVDINTERFACE, "DVD(Audio): Request Audio status");
		break;

	//=========================================================================================================
	// STOP MOTOR (Immediate)
	//	Command/Subcommand/Padding <- E3000000
	//=========================================================================================================
	case 0xE3:
		DEBUG_LOG(DVDINTERFACE, "Stop motor");
		break;

	//=========================================================================================================
	// DVD AUDIO DISABLE (Immediate)`
	//	Command/Subcommand/Padding <- E4000000 (disable)
	//	Command/Subcommand/Padding <- E4010000 (enable)
	//=========================================================================================================
	case 0xE4:
/*		if (((dvdMem.Command[0] & 0x00FF0000) >> 16) == 1)
		{
			m_bStream = true;
			LOG(DVDINTERFACE, "DVD(Audio): Audio enabled");
		}
		else
		{
			m_bStream = false;
			LOG(DVDINTERFACE, "DVD(Audio): Audio disabled");
		}*/
		break;

	//=========================================================================================================
	// SET STATUS
	//=========================================================================================================
	case 0xEE:
		INFO_LOG(DVDINTERFACE, "SetStatus %x", _DMAControlReg);
		break;

	//=========================================================================================================
	// DEBUG COMMANDS
	// Subcommands:
	//  0x00: ?
	//  0x01: read/write memory/cache
	//  0x10: ?
	//  0x11: stop/start/accept copy/disk check CAN BE OR'd!
	//  0x12: jump (jsr) to address
	//=========================================================================================================
	case 0xFE:
		{
			u8 subCommand = (dvdMem.Command[0] & 0x00FF0000) >> 16;
			u16 argument = (u16)(dvdMem.Command[0] & 0x0000FFFF);

			switch (subCommand)
			{
			case 0x01:
				{
					dvdMem.DebugTransfer.Address = dvdMem.Command[1];
					dvdMem.DebugTransfer.Length = dvdMem.Command[2] >> 16; // can be up to 12 bytes

					INFO_LOG(DVDINTERFACE, "Next cmd will %s %i bytes to drive %s @ 0x%08x",
						(argument & 0x100) ? "write" : "read", dvdMem.DebugTransfer.Length,
						(argument & 0x8000) ? "cache" : "mem", dvdMem.DebugTransfer.Address);

					dvdMem.DebugTransfer.InProgress = true;
				}
				break;

			case 0x11:
				char flags[256];
				sprintf(flags, "%s%s%s%s",
					(argument & STOP_DRIVE) ? "StopDrive " : "",
					(argument & START_DRIVE) ? "StartDrive " : "",
					(argument & ACCEPT_COPY) ? "AcceptCopy " : "",
					(argument & DISC_CHECK) ? "DiscCheck" : "");
				INFO_LOG(DVDINTERFACE, "Debug cmd(s): %s", flags);
				break;

			default:
				WARN_LOG(DVDINTERFACE, "Unsupported DVD Drive debug command 0x%08x", dvdMem.Command[0]);
				break;
			}
		}
		break;

	//=========================================================================================================
	// UNLOCK COMMANDS 1: "MATSHITA" 2: "DVD-GAME"
	// LOL
	//=========================================================================================================
	case 0xFF:
		{
			if (dvdMem.Command[0] == 0xFF014D41
				&& dvdMem.Command[1] == 0x54534849
				&& dvdMem.Command[2] == 0x54410200)
			{
				INFO_LOG(DVDINTERFACE, "Unlock test 1 passed");
			}
			else if (dvdMem.Command[0] == 0xFF004456
				&& dvdMem.Command[1] == 0x442D4741
				&& dvdMem.Command[2] == 0x4D450300)
			{
				INFO_LOG(DVDINTERFACE, "Unlock test 2 passed");
			}
			else
			{
				INFO_LOG(DVDINTERFACE, "Unlock test failed");
			}
		}
		break;

	//=========================================================================================================
	// UNKNOWN DVD COMMAND
	//=========================================================================================================
	default:
		PanicAlert("Unknown DVD command %08x - fatal error", dvdMem.Command[0]);
		_dbg_assert_(DVDINTERFACE, 0);
		break;
	}
	} // end of if(dvdMem.DebugTransfer.InProgress)

	// transfer is done
	_DMAControlReg.TSTART = 0;
	dvdMem.DMALength.Length = 0;
	GenerateDVDInterrupt(INT_TCINT);
	g_ErrorCode = 0x00;
}

}  // namespace
