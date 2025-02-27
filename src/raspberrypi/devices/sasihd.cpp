//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//
//	Copyright (C) 2001-2006 ＰＩ．(ytanaka@ipc-tokai.or.jp)
//	Copyright (C) 2014-2020 GIMONS
//  	Copyright (C) akuker
//
//  	Licensed under the BSD 3-Clause License. 
//  	See LICENSE file in the project root folder.
//
//  	[ SASI hard disk ]
//
//---------------------------------------------------------------------------
#include "sasihd.h"
#include "xm6.h"
#include "fileio.h"


//===========================================================================
//
//	SASI Hard Disk
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
SASIHD::SASIHD() : Disk("SAHD")
{
}

//---------------------------------------------------------------------------
//
//	Reset
//
//---------------------------------------------------------------------------
void SASIHD::Reset()
{
	// Unlock, clear attention
	disk.lock = FALSE;
	disk.attn = FALSE;

	// Reset, clear the code
	disk.reset = FALSE;
	disk.code = 0x00;
}

//---------------------------------------------------------------------------
//
//	Open
//
//---------------------------------------------------------------------------
BOOL SASIHD::Open(const Filepath& path, BOOL /*attn*/)
{
	ASSERT(!disk.ready);

	// Open as read-only
	Fileio fio;
	if (!fio.Open(path, Fileio::ReadOnly)) {
		return FALSE;
	}

	// Get file size
	off64_t size = fio.GetFileSize();
	fio.Close();

	#if defined(USE_MZ1F23_1024_SUPPORT)
	// For MZ-2500 / MZ-2800 MZ-1F23 (SASI 20M / sector size 1024) only
	// 20M(22437888 BS=1024 C=21912)
	if (size == 0x1566000) {
		// Sector size and number of blocks
		disk.size = 10;
		disk.blocks = (DWORD)(size >> 10);

		// Call the base class
		return Disk::Open(path);
	}
	#endif	// USE_MZ1F23_1024_SUPPORT

	#if defined(REMOVE_FIXED_SASIHD_SIZE)
	// Must be in 256-byte units
	if (size & 0xff) {
		return FALSE;
	}

	// 10MB or more
	if (size < 0x9f5400) {
		return FALSE;
	}

	// Limit to about 512MB
	if (size > 512 * 1024 * 1024) {
		return FALSE;
	}
	#else
	// 10MB, 20MB, 40MBのみ
	switch (size) {
		// 10MB (10441728 BS=256 C=40788)
		case 0x9f5400:
			break;

		// 20MB (20748288 BS=256 C=81048)
		case 0x13c9800:
			break;

		// 40MB (41496576 BS=256 C=162096)
		case 0x2793000:
			break;

		// Other (Not supported )
		default:
			return FALSE;
	}
	#endif	// REMOVE_FIXED_SASIHD_SIZE

	// Sector size and number of blocks
	disk.size = 8;
	disk.blocks = (DWORD)(size >> 8);

	// Call the base class
	return Disk::Open(path);
}

//---------------------------------------------------------------------------
//
//	REQUEST SENSE
//
//---------------------------------------------------------------------------
int SASIHD::RequestSense(const DWORD *cdb, BYTE *buf)
{
	ASSERT(cdb);
	ASSERT(buf);

	// Size decision
	int size = (int)cdb[4];
	ASSERT(size >= 0 && size < 0x100);

	// Transfer 4 bytes when size 0 (Shugart Associates System Interface specification)
	if (size == 0) {
		size = 4;
	}

	// SASI fixed to non-extended format
	memset(buf, 0, size);
	buf[0] = (BYTE)(disk.code >> 16);
	buf[1] = (BYTE)(disk.lun << 5);

	// Clear the code
	disk.code = 0x00;

	return size;
}
