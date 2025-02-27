//---------------------------------------------------------------------------
//
//	X68000 EMULATOR "XM6"
//
//	Copyright (C) 2001-2006 ＰＩ．(ytanaka@ipc-tokai.or.jp)
//	Copyright (C) 2014-2020 GIMONS
//
//	XM6i
//	Copyright (C) 2010-2015 isaki@NetBSD.org
//	Copyright (C) 2010 Y.Sugahara
//
//	Imported sava's Anex86/T98Next image and MO format support patch.
//	Imported NetBSD support and some optimisation patch by Rin Okuyama.
//  	Comments translated to english by akuker.
//
//	[ Disk ]
//
//---------------------------------------------------------------------------

#include "os.h"
#include "xm6.h"
#include "filepath.h"
#include "fileio.h"
#include "gpiobus.h"
#include "ctapdriver.h"
#include "cfilesystem.h"
#include "disk.h"

//===========================================================================
//
//	Disk
//
//===========================================================================


//===========================================================================
//
//	Disk Track
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
DiskTrack::DiskTrack()
{
	// Initialization of internal information
	dt.track = 0;
	dt.size = 0;
	dt.sectors = 0;
	dt.raw = FALSE;
	dt.init = FALSE;
	dt.changed = FALSE;
	dt.length = 0;
	dt.buffer = NULL;
	dt.maplen = 0;
	dt.changemap = NULL;
	dt.imgoffset = 0;
}

//---------------------------------------------------------------------------
//
//	Destructor
//
//---------------------------------------------------------------------------
DiskTrack::~DiskTrack()
{
	// Release memory, but do not save automatically
	if (dt.buffer) {
		free(dt.buffer);
		dt.buffer = NULL;
	}
	if (dt.changemap) {
		free(dt.changemap);
		dt.changemap = NULL;
	}
}

//---------------------------------------------------------------------------
//
//	Initialization
//
//---------------------------------------------------------------------------
void DiskTrack::Init(
	int track, int size, int sectors, BOOL raw, off64_t imgoff)
{
	ASSERT(track >= 0);
	ASSERT((size >= 8) && (size <= 11));
	ASSERT((sectors > 0) && (sectors <= 0x100));
	ASSERT(imgoff >= 0);

	// Set Parameters
	dt.track = track;
	dt.size = size;
	dt.sectors = sectors;
	dt.raw = raw;

	// Not initialized (needs to be loaded)
	dt.init = FALSE;

	// Not Changed
	dt.changed = FALSE;

	// Offset to actual data
	dt.imgoffset = imgoff;
}

//---------------------------------------------------------------------------
//
//	Load
//
//---------------------------------------------------------------------------
BOOL DiskTrack::Load(const Filepath& path)
{
	Fileio fio;
	int i;

	// Not needed if already loaded
	if (dt.init) {
		ASSERT(dt.buffer);
		ASSERT(dt.changemap);
		return TRUE;
	}

	// Calculate offset (previous tracks are considered to
    // hold 256 sectors)
	off64_t offset = ((off64_t)dt.track << 8);
	if (dt.raw) {
		ASSERT(dt.size == 11);
		offset *= 0x930;
		offset += 0x10;
	} else {
		offset <<= dt.size;
	}

	// Add offset to real image
	offset += dt.imgoffset;

	// Calculate length (data size of this track)
	int length = dt.sectors << dt.size;

	// Allocate buffer memory
	ASSERT((dt.size >= 8) && (dt.size <= 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= 0x100));

	if (dt.buffer == NULL) {
                if (posix_memalign((void **)&dt.buffer, 512, ((length + 511) / 512) * 512)) {
                        LOGWARN("%s posix_memalign failed", __PRETTY_FUNCTION__);
                }
		dt.length = length;
	}

	if (!dt.buffer) {
		return FALSE;
	}

	// Reallocate if the buffer length is different
	if (dt.length != (DWORD)length) {
		free(dt.buffer);
		if (posix_memalign((void **)&dt.buffer, 512, ((length + 511) / 512) * 512)) {
                  LOGWARN("%s posix_memalign failed", __PRETTY_FUNCTION__);  
                }
		dt.length = length;
	}

	// Reserve change map memory
	if (dt.changemap == NULL) {
		dt.changemap = (BOOL *)malloc(dt.sectors * sizeof(BOOL));
		dt.maplen = dt.sectors;
	}

	if (!dt.changemap) {
		return FALSE;
	}

	// Reallocate if the buffer length is different
	if (dt.maplen != (DWORD)dt.sectors) {
		free(dt.changemap);
		dt.changemap = (BOOL *)malloc(dt.sectors * sizeof(BOOL));
		dt.maplen = dt.sectors;
	}

	// Clear changemap
	memset(dt.changemap, 0x00, dt.sectors * sizeof(BOOL));

	// Read from File
	if (!fio.OpenDIO(path, Fileio::ReadOnly)) {
		return FALSE;
	}
	if (dt.raw) {
		// Split Reading
		for (i = 0; i < dt.sectors; i++) {
			// Seek
			if (!fio.Seek(offset)) {
				fio.Close();
				return FALSE;
			}

			// Read
			if (!fio.Read(&dt.buffer[i << dt.size], 1 << dt.size)) {
				fio.Close();
				return FALSE;
			}

			// Next offset
			offset += 0x930;
		}
	} else {
		// Continuous reading
		if (!fio.Seek(offset)) {
			fio.Close();
			return FALSE;
		}
		if (!fio.Read(dt.buffer, length)) {
			fio.Close();
			return FALSE;
		}
	}
	fio.Close();

	// Set a flag and end normally
	dt.init = TRUE;
	dt.changed = FALSE;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Save
//
//---------------------------------------------------------------------------
BOOL DiskTrack::Save(const Filepath& path)
{
	int i;
	int j;
	int total;

	// Not needed if not initialized
	if (!dt.init) {
		return TRUE;
	}

	// Not needed unless changed
	if (!dt.changed) {
		return TRUE;
	}

	// Need to write
	ASSERT(dt.buffer);
	ASSERT(dt.changemap);
	ASSERT((dt.size >= 8) && (dt.size <= 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= 0x100));

	// Writing in RAW mode is not allowed
	ASSERT(!dt.raw);

	// Calculate offset (previous tracks are considered to hold 256
	off64_t offset = ((off64_t)dt.track << 8);
	offset <<= dt.size;

	// Add offset to real image
	offset += dt.imgoffset;

	// Calculate length per sector
	int length = 1 << dt.size;

	// Open file
	Fileio fio;
	if (!fio.Open(path, Fileio::ReadWrite)) {
		return FALSE;
	}

	// Partial write loop
	for (i = 0; i < dt.sectors;) {
		// If changed
		if (dt.changemap[i]) {
			// Initialize write size
			total = 0;

			// Seek
			if (!fio.Seek(offset + ((off64_t)i << dt.size))) {
				fio.Close();
				return FALSE;
			}

			// Consectutive sector length
			for (j = i; j < dt.sectors; j++) {
				// end when interrupted
				if (!dt.changemap[j]) {
					break;
				}

				// Add one sector
				total += length;
			}

			// Write
			if (!fio.Write(&dt.buffer[i << dt.size], total)) {
				fio.Close();
				return FALSE;
			}

			// To unmodified sector
			i = j;
		} else {
			// Next Sector
			i++;
		}
	}

	// Close
	fio.Close();

	// Drop the change flag and exit
	memset(dt.changemap, 0x00, dt.sectors * sizeof(BOOL));
	dt.changed = FALSE;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Read Sector
//
//---------------------------------------------------------------------------
BOOL DiskTrack::Read(BYTE *buf, int sec) const
{
	ASSERT(buf);
	ASSERT((sec >= 0) & (sec < 0x100));

	LOGTRACE("%s reading sector: %d", __PRETTY_FUNCTION__,sec);
	// Error if not initialized
	if (!dt.init) {
		return FALSE;
	}

	// // Error if the number of sectors exceeds the valid number
	if (sec >= dt.sectors) {
		return FALSE;
	}

	// Copy
	ASSERT(dt.buffer);
	ASSERT((dt.size >= 8) && (dt.size <= 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= 0x100));
	memcpy(buf, &dt.buffer[(off64_t)sec << dt.size], (off64_t)1 << dt.size);

	// Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Write Sector
//
//---------------------------------------------------------------------------
BOOL DiskTrack::Write(const BYTE *buf, int sec)
{
	ASSERT(buf);
	ASSERT((sec >= 0) & (sec < 0x100));
	ASSERT(!dt.raw);

	// Error if not initialized
	if (!dt.init) {
		return FALSE;
	}

	// // Error if the number of sectors exceeds the valid number
	if (sec >= dt.sectors) {
		return FALSE;
	}

	// Calculate offset and length
	int offset = sec << dt.size;
	int length = 1 << dt.size;

	// Compare
	ASSERT(dt.buffer);
	ASSERT((dt.size >= 8) && (dt.size <= 11));
	ASSERT((dt.sectors > 0) && (dt.sectors <= 0x100));
	if (memcmp(buf, &dt.buffer[offset], length) == 0) {
		// 同じものを書き込もうとしているので、正常終了
		return TRUE;
	}

	// Copy, change
	memcpy(&dt.buffer[offset], buf, length);
	dt.changemap[sec] = TRUE;
	dt.changed = TRUE;

	// Success
	return TRUE;
}

//===========================================================================
//
//	Disk Cache
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
DiskCache::DiskCache(
	const Filepath& path, int size, int blocks, off64_t imgoff)
{
	int i;

	ASSERT((size >= 8) && (size <= 11));
	ASSERT(blocks > 0);
	ASSERT(imgoff >= 0);

	// Cache work
	for (i = 0; i < CacheMax; i++) {
		cache[i].disktrk = NULL;
		cache[i].serial = 0;
	}

	// Other
	serial = 0;
	sec_path = path;
	sec_size = size;
	sec_blocks = blocks;
	cd_raw = FALSE;
	imgoffset = imgoff;
}

//---------------------------------------------------------------------------
//
//	Destructor
//
//---------------------------------------------------------------------------
DiskCache::~DiskCache()
{
	// Clear the track
	Clear();
}

//---------------------------------------------------------------------------
//
//	RAW Mode Setting
//
//---------------------------------------------------------------------------
void DiskCache::SetRawMode(BOOL raw)
{
	ASSERT(sec_size == 11);

	// Configuration
	cd_raw = raw;
}

//---------------------------------------------------------------------------
//
//	Save
//
//---------------------------------------------------------------------------
BOOL DiskCache::Save()
{
	// Save track
	for (int i = 0; i < CacheMax; i++) {
		// Is it a valid track?
		if (cache[i].disktrk) {
			// Save
			if (!cache[i].disktrk->Save(sec_path)) {
				return FALSE;
			}
		}
	}

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Get disk cache information
//
//---------------------------------------------------------------------------
BOOL DiskCache::GetCache(int index, int& track, DWORD& aserial) const
{
	ASSERT((index >= 0) && (index < CacheMax));

	// FALSE if unused
	if (!cache[index].disktrk) {
		return FALSE;
	}

	// Set track and serial
	track = cache[index].disktrk->GetTrack();
	aserial = cache[index].serial;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Clear
//
//---------------------------------------------------------------------------
void DiskCache::Clear()
{
	// Free the cache
	for (int i = 0; i < CacheMax; i++) {
		if (cache[i].disktrk) {
			delete cache[i].disktrk;
			cache[i].disktrk = NULL;
		}
	}
}

//---------------------------------------------------------------------------
//
//	Sector Read
//
//---------------------------------------------------------------------------
BOOL DiskCache::Read(BYTE *buf, int block)
{
	ASSERT(sec_size != 0);

	// Update first
	Update();

	// Calculate track (fixed to 256 sectors/track)
	int track = block >> 8;

	// Get the track data
	DiskTrack *disktrk = Assign(track);
	if (!disktrk) {
		return FALSE;
	}

	// Read the track data to the cache
	return disktrk->Read(buf, (BYTE)block);
}

//---------------------------------------------------------------------------
//
//	Sector write
//
//---------------------------------------------------------------------------
BOOL DiskCache::Write(const BYTE *buf, int block)
{
	ASSERT(sec_size != 0);

	// Update first
	Update();

	// Calculate track (fixed to 256 sectors/track)
	int track = block >> 8;

	// Get that track data
	DiskTrack *disktrk = Assign(track);
	if (!disktrk) {
		return FALSE;
	}

	// Write the data to the cache
	return disktrk->Write(buf, (BYTE)block);
}

//---------------------------------------------------------------------------
//
//	Track Assignment
//
//---------------------------------------------------------------------------
DiskTrack* DiskCache::Assign(int track)
{
	ASSERT(sec_size != 0);
	ASSERT(track >= 0);

	// First, check if it is already assigned
	for (int i = 0; i < CacheMax; i++) {
		if (cache[i].disktrk) {
			if (cache[i].disktrk->GetTrack() == track) {
				// Track match
				cache[i].serial = serial;
				return cache[i].disktrk;
			}
		}
	}

	// Next, check for empty
	for (int i = 0; i < CacheMax; i++) {
		if (!cache[i].disktrk) {
			// Try loading
			if (Load(i, track)) {
				// Success loading
				cache[i].serial = serial;
				return cache[i].disktrk;
			}

			// Load failed
			return NULL;
		}
	}

	// Finally, find the youngest serial number and delete it

	// Set index 0 as candidate c
	DWORD s = cache[0].serial;
	int c = 0;

	// Compare candidate with serial and update to smaller one
	for (int i = 0; i < CacheMax; i++) {
		ASSERT(cache[i].disktrk);

		// Compare and update the existing serial
		if (cache[i].serial < s) {
			s = cache[i].serial;
			c = i;
		}
	}

	// Save this track
	if (!cache[c].disktrk->Save(sec_path)) {
		return NULL;
	}

	// Delete this track
	DiskTrack *disktrk = cache[c].disktrk;
	cache[c].disktrk = NULL;

	// Load
	if (Load(c, track, disktrk)) {
		// Successful loading
		cache[c].serial = serial;
		return cache[c].disktrk;
	}

	// Load failed
	return NULL;
}

//---------------------------------------------------------------------------
//
//	Load cache
//
//---------------------------------------------------------------------------
BOOL DiskCache::Load(int index, int track, DiskTrack *disktrk)
{
	ASSERT((index >= 0) && (index < CacheMax));
	ASSERT(track >= 0);
	ASSERT(!cache[index].disktrk);

	// Get the number of sectors on this track
	int sectors = sec_blocks - (track << 8);
	ASSERT(sectors > 0);
	if (sectors > 0x100) {
		sectors = 0x100;
	}

	// Create a disk track
	if (disktrk == NULL) {
		disktrk = new DiskTrack();
	}

	// Initialize disk track
	disktrk->Init(track, sec_size, sectors, cd_raw, imgoffset);

	// Try loading
	if (!disktrk->Load(sec_path)) {
		// 失敗
		delete disktrk;
		return FALSE;
	}

	// Allocation successful, work set
	cache[index].disktrk = disktrk;

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Update serial number
//
//---------------------------------------------------------------------------
void DiskCache::Update()
{
	// Update and do nothing except 0
	serial++;
	if (serial != 0) {
		return;
	}

	// Clear serial of all caches (loop in 32bit)
	for (int i = 0; i < CacheMax; i++) {
		cache[i].serial = 0;
	}
}


//===========================================================================
//
//	Disk
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
Disk::Disk(std::string id)
{
	assert(id.length() == 4);

	disk.id = id;

	// Work initialization
	disk.ready = FALSE;
	disk.writep = FALSE;
	disk.readonly = FALSE;
	disk.removable = FALSE;
	disk.lock = FALSE;
	disk.attn = FALSE;
	disk.reset = FALSE;
	disk.size = 0;
	disk.blocks = 0;
	disk.lun = 0;
	disk.code = 0;
	disk.dcache = NULL;
	disk.imgoffset = 0;

	// Other
	cache_wb = TRUE;
}

//---------------------------------------------------------------------------
//
//	Destructor
//
//---------------------------------------------------------------------------
Disk::~Disk()
{
	// Save disk cache
	if (disk.ready) {
		// Only if ready...
		if (disk.dcache) {
			disk.dcache->Save();
		}
	}

	// Clear disk cache
	if (disk.dcache) {
		delete disk.dcache;
		disk.dcache = NULL;
	}
}

//---------------------------------------------------------------------------
//
//	Reset
//
//---------------------------------------------------------------------------
void Disk::Reset()
{
	// no lock, no attention, reset
	disk.lock = FALSE;
	disk.attn = FALSE;
	disk.reset = TRUE;
}

//---------------------------------------------------------------------------
//
//	Retrieve the disk's ID
//
//---------------------------------------------------------------------------
const std::string& Disk::GetID() const
{ 
	return disk.id; 
}


//---------------------------------------------------------------------------
//
//	Get cache writeback mode
//
//---------------------------------------------------------------------------
bool Disk::IsCacheWB()
{ 
	return cache_wb; 
}
 
//---------------------------------------------------------------------------
//
//	Set cache writeback mode
//
//---------------------------------------------------------------------------
void Disk::SetCacheWB(BOOL enable) 
{ 
	cache_wb = enable; 
}

//---------------------------------------------------------------------------
//
//	Device type checks
//
//---------------------------------------------------------------------------

bool Disk::IsSASI() const
{
	return disk.id == "SAHD";
}

bool Disk::IsSCSI() const
{
	return disk.id == "SCHD";
}

bool Disk::IsCdRom() const
{
	return disk.id == "SCCD";
}

bool Disk::IsMo() const
{
	return disk.id == "SCMO";
}

bool Disk::IsBridge() const
{
	return disk.id == "SCBR";
}

bool Disk::IsDaynaPort() const
{
	return disk.id == "SCDP";
}

bool Disk::IsNuvolink() const
{
	return disk.id == "SCNL";
}

//---------------------------------------------------------------------------
//
//	Open
//  * Call as a post-process after successful opening in a derived class
//
//---------------------------------------------------------------------------
BOOL Disk::Open(const Filepath& path, BOOL /*attn*/)
{
	ASSERT((disk.size >= 8) && (disk.size <= 11));
	ASSERT(disk.blocks > 0);

	// Ready
	disk.ready = TRUE;

	// Cache initialization
	ASSERT(!disk.dcache);
	disk.dcache =
		new DiskCache(path, disk.size, disk.blocks, disk.imgoffset);

	// Can read/write open
	Fileio fio;
	if (fio.Open(path, Fileio::ReadWrite)) {
		// Write permission, not read only
		disk.writep = FALSE;
		disk.readonly = FALSE;
		fio.Close();
	} else {
		// Write protected, read only
		disk.writep = TRUE;
		disk.readonly = TRUE;
	}

	// Not locked
	disk.lock = FALSE;

	// Save path
	diskpath = path;

	// Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	Eject
//
//---------------------------------------------------------------------------
void Disk::Eject(BOOL force)
{
	// Can only be ejected if it is removable
	if (!disk.removable) {
		return;
	}

	// If you're not ready, you don't need to eject
	if (!disk.ready) {
		return;
	}

	// Must be unlocked if there is no force flag
	if (!force) {
		if (disk.lock) {
			return;
		}
	}

	// Remove disk cache
	disk.dcache->Save();
	delete disk.dcache;
	disk.dcache = NULL;

	// Not ready, no attention
	disk.ready = FALSE;
	disk.writep = FALSE;
	disk.readonly = FALSE;
	disk.attn = FALSE;
}

//---------------------------------------------------------------------------
//
//	Write Protected
//
//---------------------------------------------------------------------------
void Disk::WriteP(BOOL writep)
{
	// be ready
	if (!disk.ready) {
		return;
	}

	// Read Only, protect only
	if (disk.readonly) {
		ASSERT(disk.writep);
		return;
	}

	// Write protect flag setting
	disk.writep = writep;
}

//---------------------------------------------------------------------------
//
//	Get Path
//
//---------------------------------------------------------------------------
void Disk::GetPath(Filepath& path) const
{
	path = diskpath;
}

//---------------------------------------------------------------------------
//
//	Flush
//
//---------------------------------------------------------------------------
BOOL Disk::Flush()
{
	// Do nothing if there's nothing cached
	if (!disk.dcache) {
		return TRUE;
	}

	// Save cache
	return disk.dcache->Save();
}

//---------------------------------------------------------------------------
//
//	Check Ready
//
//---------------------------------------------------------------------------
BOOL Disk::CheckReady()
{
	// Not ready if reset
	if (disk.reset) {
		disk.code = DISK_DEVRESET;
		disk.reset = FALSE;
		LOGTRACE("%s Disk in reset", __PRETTY_FUNCTION__);
		return FALSE;
	}

	// Not ready if it needs attention
	if (disk.attn) {
		disk.code = DISK_ATTENTION;
		disk.attn = FALSE;
		LOGTRACE("%s Disk in needs attention", __PRETTY_FUNCTION__);
		return FALSE;
	}

	// Return status if not ready
	if (!disk.ready) {
		disk.code = DISK_NOTREADY;
		LOGTRACE("%s Disk not ready", __PRETTY_FUNCTION__);
		return FALSE;
	}

	// Initialization with no error
	disk.code = DISK_NOERROR;
	LOGTRACE("%s Disk is ready!", __PRETTY_FUNCTION__);

	return TRUE;
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//	*You need to be successful at all times
//
//---------------------------------------------------------------------------
int Disk::Inquiry(
	const DWORD* /*cdb*/, BYTE* /*buf*/, DWORD /*major*/, DWORD /*minor*/)
{
	// default is INQUIRY failure
	disk.code = DISK_INVALIDCMD;
	return 0;
}

//---------------------------------------------------------------------------
//
//	REQUEST SENSE
//	*SASI is a separate process
//
//---------------------------------------------------------------------------
int Disk::RequestSense(const DWORD *cdb, BYTE *buf)
{
	ASSERT(cdb);
	ASSERT(buf);

	// Return not ready only if there are no errors
	if (disk.code == DISK_NOERROR) {
		if (!disk.ready) {
			disk.code = DISK_NOTREADY;
		}
	}

	// Size determination (according to allocation length)
	int size = (int)cdb[4];
	LOGTRACE("%s size of data = %d", __PRETTY_FUNCTION__, size);
	ASSERT((size >= 0) && (size < 0x100));

	// For SCSI-1, transfer 4 bytes when the size is 0
    // (Deleted this specification for SCSI-2)
	if (size == 0) {
		size = 4;
	}

	// Clear the buffer
	memset(buf, 0, size);

	// Set 18 bytes including extended sense data

	// Current error
	buf[0] = 0x70;

	buf[2] = (BYTE)(disk.code >> 16);
	buf[7] = 10;
	buf[12] = (BYTE)(disk.code >> 8);
	buf[13] = (BYTE)disk.code;

	// Clear the code
	disk.code = 0x00;

	return size;
}

//---------------------------------------------------------------------------
//
//	MODE SELECT check
//	*Not affected by disk.code
//
//---------------------------------------------------------------------------
int Disk::SelectCheck(const DWORD *cdb)
{
	ASSERT(cdb);

	// Error if save parameters are set instead of SCSIHD
	if (!IsSCSI()) {
		// Error if save parameters are set
		if (cdb[1] & 0x01) {
			disk.code = DISK_INVALIDCDB;
			return 0;
		}
	}

	// Receive the data specified by the parameter length
	int length = (int)cdb[4];
	return length;
}


//---------------------------------------------------------------------------
//
//	MODE SELECT(10) check
//	* Not affected by disk.code
//
//---------------------------------------------------------------------------
int Disk::SelectCheck10(const DWORD *cdb)
{
	ASSERT(cdb);

	// Error if save parameters are set instead of SCSIHD
	if (!IsSCSI()) {
		if (cdb[1] & 0x01) {
			disk.code = DISK_INVALIDCDB;
			return 0;
		}
	}

	// Receive the data specified by the parameter length
	DWORD length = cdb[7];
	length <<= 8;
	length |= cdb[8];
	if (length > 0x800) {
		length = 0x800;
	}

	return (int)length;
}

//---------------------------------------------------------------------------
//
//	MODE SELECT
//	* Not affected by disk.code
//
//---------------------------------------------------------------------------
BOOL Disk::ModeSelect(
	const DWORD* /*cdb*/, const BYTE *buf, int length)
{
	ASSERT(buf);
	ASSERT(length >= 0);

	// cannot be set
	disk.code = DISK_INVALIDPRM;

	return FALSE;
}

//---------------------------------------------------------------------------
//
//	MODE SENSE
//	*Not affected by disk.code
//
//---------------------------------------------------------------------------
int Disk::ModeSense(const DWORD *cdb, BYTE *buf)
{
	BOOL valid;
	BOOL change;
	int ret;

	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x1a);

	// Get length, clear buffer
	int length = (int)cdb[4];
	ASSERT((length >= 0) && (length < 0x100));
	memset(buf, 0, length);

	// Get changeable flag
	if ((cdb[2] & 0xc0) == 0x40) {
		change = TRUE;
	} else {
		change = FALSE;
	}

	// Get page code (0x00 is valid from the beginning)
	int page = cdb[2] & 0x3f;
	if (page == 0x00) {
		valid = TRUE;
	} else {
		valid = FALSE;
	}

	// Basic information
	int size = 4;

	// MEDIUM TYPE
	if (IsMo()) {
		buf[1] = 0x03; // optical reversible or erasable
	}

	// DEVICE SPECIFIC PARAMETER
	if (disk.writep) {
		buf[2] = 0x80;
	}

	// add block descriptor if DBD is 0
	if ((cdb[1] & 0x08) == 0) {
		// Mode parameter header
		buf[3] = 0x08;

		// Only if ready
		if (disk.ready) {
			// Block descriptor (number of blocks)
			buf[5] = (BYTE)(disk.blocks >> 16);
			buf[6] = (BYTE)(disk.blocks >> 8);
			buf[7] = (BYTE)disk.blocks;

			// Block descriptor (block length)
			size = 1 << disk.size;
			buf[9] = (BYTE)(size >> 16);
			buf[10] = (BYTE)(size >> 8);
			buf[11] = (BYTE)size;
		}

		// size
		size = 12;
	}

	// Page code 1(read-write error recovery)
	if ((page == 0x01) || (page == 0x3f)) {
		size += AddError(change, &buf[size]);
		valid = TRUE;
	}

	// Page code 3(format device)
	if ((page == 0x03) || (page == 0x3f)) {
		size += AddFormat(change, &buf[size]);
		valid = TRUE;
	}

	// Page code 4(drive parameter)
	if ((page == 0x04) || (page == 0x3f)) {
		size += AddDrive(change, &buf[size]);
		valid = TRUE;
	}

	// Page code 6(optical)
	if (IsMo()) {
		if ((page == 0x06) || (page == 0x3f)) {
			size += AddOpt(change, &buf[size]);
			valid = TRUE;
		}
	}

	// Page code 8(caching)
	if ((page == 0x08) || (page == 0x3f)) {
		size += AddCache(change, &buf[size]);
		valid = TRUE;
	}

	// Page code 13(CD-ROM)
	if (IsCdRom()) {
		if ((page == 0x0d) || (page == 0x3f)) {
			size += AddCDROM(change, &buf[size]);
			valid = TRUE;
		}
	}

	// Page code 14(CD-DA)
	if (IsCdRom()) {
		if ((page == 0x0e) || (page == 0x3f)) {
			size += AddCDDA(change, &buf[size]);
			valid = TRUE;
		}
	}

	// Page (vendor special)
	ret = AddVendor(page, change, &buf[size]);
	if (ret > 0) {
		size += ret;
		valid = TRUE;
	}

	// final setting of mode data length
	buf[0] = (BYTE)(size - 1);

	// Unsupported page
	if (!valid) {
		disk.code = DISK_INVALIDCDB;
		return 0;
	}

	// MODE SENSE success
	disk.code = DISK_NOERROR;
	return length;
}

//---------------------------------------------------------------------------
//
//	MODE SENSE(10)
//	*Not affected by disk.code
//
//---------------------------------------------------------------------------
int Disk::ModeSense10(const DWORD *cdb, BYTE *buf)
{
	BOOL valid;
	BOOL change;
	int ret;

	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x5a);

	// Get length, clear buffer
	int length = cdb[7];
	length <<= 8;
	length |= cdb[8];
	if (length > 0x800) {
		length = 0x800;
	}
	ASSERT((length >= 0) && (length < 0x800));
	memset(buf, 0, length);

	// Get changeable flag
	if ((cdb[2] & 0xc0) == 0x40) {
		change = TRUE;
	} else {
		change = FALSE;
	}

	// Get page code (0x00 is valid from the beginning)
	int page = cdb[2] & 0x3f;
	if (page == 0x00) {
		valid = TRUE;
	} else {
		valid = FALSE;
	}

	// Basic Information
	int size = 4;
	if (disk.writep) {
		buf[2] = 0x80;
	}

	// add block descriptor if DBD is 0
	if ((cdb[1] & 0x08) == 0) {
		// Mode parameter header
		buf[3] = 0x08;

		// Only if ready
		if (disk.ready) {
			// Block descriptor (number of blocks)
			buf[5] = (BYTE)(disk.blocks >> 16);
			buf[6] = (BYTE)(disk.blocks >> 8);
			buf[7] = (BYTE)disk.blocks;

			// Block descriptor (block length)
			size = 1 << disk.size;
			buf[9] = (BYTE)(size >> 16);
			buf[10] = (BYTE)(size >> 8);
			buf[11] = (BYTE)size;
		}

		// Size
		size = 12;
	}

	// Page code 1(read-write error recovery)
	if ((page == 0x01) || (page == 0x3f)) {
		size += AddError(change, &buf[size]);
		valid = TRUE;
	}

	// Page code 3(format device)
	if ((page == 0x03) || (page == 0x3f)) {
		size += AddFormat(change, &buf[size]);
		valid = TRUE;
	}

	// Page code 4(drive parameter)
	if ((page == 0x04) || (page == 0x3f)) {
		size += AddDrive(change, &buf[size]);
		valid = TRUE;
	}

	// ペPage code 6(optical)
	if (IsMo()) {
		if ((page == 0x06) || (page == 0x3f)) {
			size += AddOpt(change, &buf[size]);
			valid = TRUE;
		}
	}

	// Page code 8(caching)
	if ((page == 0x08) || (page == 0x3f)) {
		size += AddCache(change, &buf[size]);
		valid = TRUE;
	}

	// Page code 13(CD-ROM)
	if (IsCdRom()) {
		if ((page == 0x0d) || (page == 0x3f)) {
			size += AddCDROM(change, &buf[size]);
			valid = TRUE;
		}
	}

	// Page code 14(CD-DA)
	if (IsCdRom()) {
		if ((page == 0x0e) || (page == 0x3f)) {
			size += AddCDDA(change, &buf[size]);
			valid = TRUE;
		}
	}

	// Page (vendor special)
	ret = AddVendor(page, change, &buf[size]);
	if (ret > 0) {
		size += ret;
		valid = TRUE;
	}

	// final setting of mode data length
	buf[0] = (BYTE)(size - 1);

	// Unsupported page
	if (!valid) {
		disk.code = DISK_INVALIDCDB;
		return 0;
	}

	// MODE SENSE success
	disk.code = DISK_NOERROR;
	return length;
}

//---------------------------------------------------------------------------
//
//	Add error page
//
//---------------------------------------------------------------------------
int Disk::AddError(BOOL change, BYTE *buf)
{
	ASSERT(buf);

	// Set the message length
	buf[0] = 0x01;
	buf[1] = 0x0a;

	// No changeable area
	if (change) {
		return 12;
	}

	// Retry count is 0, limit time uses internal default value
	return 12;
}

//---------------------------------------------------------------------------
//
//	Add format page
//
//---------------------------------------------------------------------------
int Disk::AddFormat(BOOL change, BYTE *buf)
{
	int size;

	ASSERT(buf);

	// Set the message length
	buf[0] = 0x80 | 0x03;
	buf[1] = 0x16;

	// Show the number of bytes in the physical sector as changeable
	// (though it cannot be changed in practice)
	if (change) {
		buf[0xc] = 0xff;
		buf[0xd] = 0xff;
		return 24;
	}

	if (disk.ready) {
		// Set the number of tracks in one zone to 8 (TODO)
		buf[0x3] = 0x08;

		// Set sector/track to 25 (TODO)
		buf[0xa] = 0x00;
		buf[0xb] = 0x19;

		// Set the number of bytes in the physical sector
		size = 1 << disk.size;
		buf[0xc] = (BYTE)(size >> 8);
		buf[0xd] = (BYTE)size;
	}

	// Set removable attribute
	if (disk.removable) {
		buf[20] = 0x20;
	}

	return 24;
}

//---------------------------------------------------------------------------
//
//	Add drive page
//
//---------------------------------------------------------------------------
int Disk::AddDrive(BOOL change, BYTE *buf)
{
	ASSERT(buf);

	// Set the message length
	buf[0] = 0x04;
	buf[1] = 0x16;

	// No changeable area
	if (change) {
		return 24;
	}

	if (disk.ready) {
		// Set the number of cylinders (total number of blocks
        // divided by 25 sectors/track and 8 heads)
		DWORD cylinder = disk.blocks;
		cylinder >>= 3;
		cylinder /= 25;
		buf[0x2] = (BYTE)(cylinder >> 16);
		buf[0x3] = (BYTE)(cylinder >> 8);
		buf[0x4] = (BYTE)cylinder;

		// Fix the head at 8
		buf[0x5] = 0x8;
	}

	return 24;
}

//---------------------------------------------------------------------------
//
//	Add option
//
//---------------------------------------------------------------------------
int Disk::AddOpt(BOOL change, BYTE *buf)
{
	ASSERT(buf);

	// Set the message length
	buf[0] = 0x06;
	buf[1] = 0x02;

	// No changeable area
	if (change) {
		return 4;
	}

	// Do not report update blocks
	return 4;
}

//---------------------------------------------------------------------------
//
//	Add Cache Page
//
//---------------------------------------------------------------------------
int Disk::AddCache(BOOL change, BYTE *buf)
{
	ASSERT(buf);

	// Set the message length
	buf[0] = 0x08;
	buf[1] = 0x0a;

	// No changeable area
	if (change) {
		return 12;
	}

	// Only read cache is valid, no prefetch
	return 12;
}

//---------------------------------------------------------------------------
//
//	Add CDROM Page
//
//---------------------------------------------------------------------------
int Disk::AddCDROM(BOOL change, BYTE *buf)
{
	ASSERT(buf);

	// Set the message length
	buf[0] = 0x0d;
	buf[1] = 0x06;

	// No changeable area
	if (change) {
		return 8;
	}

	// 2 seconds for inactive timer
	buf[3] = 0x05;

	// MSF multiples are 60 and 75 respectively
	buf[5] = 60;
	buf[7] = 75;

	return 8;
}

//---------------------------------------------------------------------------
//
//	CD-DAページ追加
//
//---------------------------------------------------------------------------
int Disk::AddCDDA(BOOL change, BYTE *buf)
{
	ASSERT(buf);

	// Set the message length
	buf[0] = 0x0e;
	buf[1] = 0x0e;

	// No changeable area
	if (change) {
		return 16;
	}

	// Audio waits for operation completion and allows
	// PLAY across multiple tracks
	return 16;
}

//---------------------------------------------------------------------------
//
//	Add special vendor page
//
//---------------------------------------------------------------------------
int Disk::AddVendor(int /*page*/, BOOL /*change*/, BYTE *buf)
{
	ASSERT(buf);

	return 0;
}

//---------------------------------------------------------------------------
//
//	READ DEFECT DATA(10)
//	*Not affected by disk.code
//
//---------------------------------------------------------------------------
int Disk::ReadDefectData10(const DWORD *cdb, BYTE *buf)
{
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x37);

	// Get length, clear buffer
	DWORD length = cdb[7];
	length <<= 8;
	length |= cdb[8];
	if (length > 0x800) {
		length = 0x800;
	}
	ASSERT((length >= 0) && (length < 0x800));
	memset(buf, 0, length);

	// P/G/FORMAT
	buf[1] = (cdb[1] & 0x18) | 5;
	buf[3] = 8;

	buf[4] = 0xff;
	buf[5] = 0xff;
	buf[6] = 0xff;
	buf[7] = 0xff;

	buf[8] = 0xff;
	buf[9] = 0xff;
	buf[10] = 0xff;
	buf[11] = 0xff;

	// no list
	disk.code = DISK_NODEFECT;
	return 4;
}

//---------------------------------------------------------------------------
//
//	Command
//
//---------------------------------------------------------------------------

//---------------------------------------------------------------------------
//
//	TEST UNIT READY
//
//---------------------------------------------------------------------------
BOOL Disk::TestUnitReady(const DWORD* /*cdb*/)
{
	// Status check
	if (!CheckReady()) {
		return FALSE;
	}

	// TEST UNIT READY Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	REZERO UNIT
//
//---------------------------------------------------------------------------
BOOL Disk::Rezero(const DWORD* /*cdb*/)
{
	// Status check
	if (!CheckReady()) {
		return FALSE;
	}

	// REZERO Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	FORMAT UNIT
//	*Opcode $06 for SASI, Opcode $04 for SCSI
//
//---------------------------------------------------------------------------
BOOL Disk::Format(const DWORD *cdb)
{
	// Status check
	if (!CheckReady()) {
		return FALSE;
	}

	// FMTDATA=1 is not supported (but OK if there is no DEFECT LIST)
	if ((cdb[1] & 0x10) != 0 && cdb[4] != 0) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// FORMAT Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	REASSIGN BLOCKS
//
//---------------------------------------------------------------------------
BOOL Disk::Reassign(const DWORD* /*cdb*/)
{
	// Status check
	if (!CheckReady()) {
		return FALSE;
	}

	// REASSIGN BLOCKS Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	READ
//
//---------------------------------------------------------------------------
int Disk::Read(const DWORD *cdb, BYTE *buf, DWORD block)
{
	ASSERT(buf);

	// Status check
	if (!CheckReady()) {
		return 0;
	}

	// Error if the total number of blocks is exceeded
	if (block >= disk.blocks) {
		disk.code = DISK_INVALIDLBA;
		return 0;
	}

	// leave it to the cache
	if (!disk.dcache->Read(buf, block)) {
		disk.code = DISK_READFAULT;
		return 0;
	}

	//  Success
	return (1 << disk.size);
}

//---------------------------------------------------------------------------
//
//	WRITE check
//
//---------------------------------------------------------------------------
int Disk::WriteCheck(DWORD block)
{
	// Status check
	if (!CheckReady()) {
		return 0;
	}

	// Error if the total number of blocks is exceeded
	if (block >= disk.blocks) {
		return 0;
	}

	// Error if write protected
	if (disk.writep) {
		disk.code = DISK_WRITEPROTECT;
		return 0;
	}

	//  Success
	return (1 << disk.size);
}

//---------------------------------------------------------------------------
//
//	WRITE
//
//---------------------------------------------------------------------------
BOOL Disk::Write(const DWORD *cdb, const BYTE *buf, DWORD block)
{
	ASSERT(buf);

	LOGTRACE("%s", __PRETTY_FUNCTION__);
	// Error if not ready
	if (!disk.ready) {
		disk.code = DISK_NOTREADY;
		return FALSE;
	}

	// Error if the total number of blocks is exceeded
	if (block >= disk.blocks) {
		disk.code = DISK_INVALIDLBA;
		return FALSE;
	}

	// Error if write protected
	if (disk.writep) {
		disk.code = DISK_WRITEPROTECT;
		return FALSE;
	}

	// Leave it to the cache
	if (!disk.dcache->Write(buf, block)) {
		disk.code = DISK_WRITEFAULT;
		return FALSE;
	}

	//  Success
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	SEEK
//	*Does not check LBA (SASI IOCS)
//
//---------------------------------------------------------------------------
BOOL Disk::Seek(const DWORD* /*cdb*/)
{
	// Status check
	if (!CheckReady()) {
		return FALSE;
	}

	// SEEK Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	ASSIGN
//
//---------------------------------------------------------------------------
BOOL Disk::Assign(const DWORD* /*cdb*/)
{
	// Status check
	if (!CheckReady()) {
		return FALSE;
	}

	//  Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	SPECIFY
//
//---------------------------------------------------------------------------
BOOL Disk::Specify(const DWORD* /*cdb*/)
{
	// Status check
	if (!CheckReady()) {
		return FALSE;
	}

	//  Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	START STOP UNIT
//
//---------------------------------------------------------------------------
BOOL Disk::StartStop(const DWORD *cdb)
{
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x1b);

	// Look at the eject bit and eject if necessary
	if (cdb[4] & 0x02) {
		if (disk.lock) {
			// Cannot be ejected because it is locked
			disk.code = DISK_PREVENT;
			return FALSE;
		}

		// Eject
		Eject(FALSE);
	}

	// OK
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	SEND DIAGNOSTIC
//
//---------------------------------------------------------------------------
BOOL Disk::SendDiag(const DWORD *cdb)
{
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x1d);

	// Do not support PF bit
	if (cdb[1] & 0x10) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// Do not support parameter list
	if ((cdb[3] != 0) || (cdb[4] != 0)) {
		disk.code = DISK_INVALIDCDB;
		return FALSE;
	}

	// Always successful
	disk.code = DISK_NOERROR;
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	PREVENT/ALLOW MEDIUM REMOVAL
//
//---------------------------------------------------------------------------
BOOL Disk::Removal(const DWORD *cdb)
{
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x1e);

	// Status check
	if (!CheckReady()) {
		return FALSE;
	}

	// Set Lock flag
	if (cdb[4] & 0x01) {
		disk.lock = TRUE;
	} else {
		disk.lock = FALSE;
	}

	// REMOVAL Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	READ CAPACITY
//
//---------------------------------------------------------------------------
int Disk::ReadCapacity(const DWORD* /*cdb*/, BYTE *buf)
{
	DWORD blocks;
	DWORD length;

	ASSERT(buf);

	// Buffer clear
	memset(buf, 0, 8);

	// Status check
	if (!CheckReady()) {
		return 0;
	}

	if (disk.blocks <= 0) {
		LOGWARN("%s Capacity not available, medium may not be present", __PRETTY_FUNCTION__);

		return -1;
	}

	// Create end of logical block address (disk.blocks-1)
	blocks = disk.blocks - 1;
	buf[0] = (BYTE)(blocks >> 24);
	buf[1] = (BYTE)(blocks >> 16);
	buf[2] = (BYTE)(blocks >> 8);
	buf[3] = (BYTE)blocks;

	// Create block length (1 << disk.size)
	length = 1 << disk.size;
	buf[4] = (BYTE)(length >> 24);
	buf[5] = (BYTE)(length >> 16);
	buf[6] = (BYTE)(length >> 8);
	buf[7] = (BYTE)length;

	// return the size
	return 8;
}

//---------------------------------------------------------------------------
//
//	VERIFY
//
//---------------------------------------------------------------------------
BOOL Disk::Verify(const DWORD *cdb)
{
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x2f);

	// Get parameters
	DWORD record = cdb[2];
	record <<= 8;
	record |= cdb[3];
	record <<= 8;
	record |= cdb[4];
	record <<= 8;
	record |= cdb[5];
	DWORD blocks = cdb[7];
	blocks <<= 8;
	blocks |= cdb[8];

	// Status check
	if (!CheckReady()) {
		return 0;
	}

	// Parameter check
	if (disk.blocks < (record + blocks)) {
		disk.code = DISK_INVALIDLBA;
		return FALSE;
	}

	//  Success
	return TRUE;
}

//---------------------------------------------------------------------------
//
//	READ TOC
//
//---------------------------------------------------------------------------
int Disk::ReadToc(const DWORD *cdb, BYTE *buf)
{
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x43);
	ASSERT(buf);

	// This command is not supported
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO
//
//---------------------------------------------------------------------------
BOOL Disk::PlayAudio(const DWORD *cdb)
{
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x45);

	// This command is not supported
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO MSF
//
//---------------------------------------------------------------------------
BOOL Disk::PlayAudioMSF(const DWORD *cdb)
{
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x47);

	// This command is not supported
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

//---------------------------------------------------------------------------
//
//	PLAY AUDIO TRACK
//
//---------------------------------------------------------------------------
BOOL Disk::PlayAudioTrack(const DWORD *cdb)
{
	ASSERT(cdb);
	ASSERT(cdb[0] == 0x48);

	// This command is not supported
	disk.code = DISK_INVALIDCMD;
	return FALSE;
}

