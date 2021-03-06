/*  Copyright (C) 2006 thoduv
    Copyright (C) 2006-2007 Theo Berkau
    Copyright (C) 2008-2009 DeSmuME team
    Copyright (C) 2012 DeSmuMEWii team

    This file is part of DeSmuMEWii

    DeSmuMEWii is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    DeSmuMEWii is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with DeSmuMEWii; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <stdlib.h>
#include <string.h>
#include "debug.h"
#include "types.h"
#include "mc.h"
#include "movie.h"
#include "readwrite.h"
#include "NDSSystem.h"

//temporary hack until we have better error reporting facilities
#ifdef _MSC_VER
#include <windows.h>
#endif

#define FW_CMD_READ             0x03
#define FW_CMD_WRITEDISABLE     0x04
#define FW_CMD_READSTATUS       0x05
#define FW_CMD_WRITEENABLE      0x06
#define FW_CMD_PAGEWRITE        0x0A
#define FW_CMD_READ_ID			0x9F

#define BM_CMD_AUTODETECT       0xFF
#define BM_CMD_WRITESTATUS      0x01
#define BM_CMD_WRITELOW         0x02
#define BM_CMD_READLOW          0x03
#define BM_CMD_WRITEDISABLE     0x04
#define BM_CMD_READSTATUS       0x05
#define BM_CMD_WRITEENABLE      0x06
#define BM_CMD_WRITEHIGH        0x0A
#define BM_CMD_READHIGH         0x0B

/* FLASH*/
#define COMM_PAGE_WRITE				0x0A
#define COMM_PAGE_ERASE				0xDB
#define COMM_SECTOR_ERASE			0xD8
#define COMM_CHIP_ERASE				0xC7
#define CARDFLASH_READ_BYTES_FAST	0x0B    /* Not used*/
#define CARDFLASH_DEEP_POWDOWN		0xB9    /* Not used*/
#define CARDFLASH_WAKEUP			0xAB    /* Not used*/

//since r2203 this was 0x00.
//but baby pals proves finally that it should be 0xFF:
//the game reads its initial sound volumes from uninitialized data, and if it is 0, the game will be silent
//if it is 0xFF then the game starts with its sound and music at max, as presumably it is supposed to.
//so in r3303 I finally changed it (no$ appears definitely to initialized to 0xFF)
static const u8 kUninitializedSaveDataValue = 0xFF; 

static const char* kDesmumeSaveCookie = "|-DESMUME SAVE-|";

static const u32 saveSizes[] = {512,			// 4k
								8*1024,			// 64k
								32*1024,		// 512k
								64*1024,		// 1Mbit
								256*1024,		// 2Mbit
								512*1024,		// 4Mbit
								0xFFFFFFFF};
static const u32 saveSizes_count = ARRAY_SIZE(saveSizes);

//the lookup table from user save types to save parameters
static const int save_types[7][2] = {
		{MC_TYPE_AUTODETECT,1},
		{MC_TYPE_EEPROM1,MC_SIZE_4KBITS},
		{MC_TYPE_EEPROM2,MC_SIZE_64KBITS},
		{MC_TYPE_EEPROM2,MC_SIZE_512KBITS},
		{MC_TYPE_FRAM,MC_SIZE_256KBITS},
		{MC_TYPE_FLASH,MC_SIZE_2MBITS},
		{MC_TYPE_FLASH,MC_SIZE_4MBITS}
};

void backup_setManualBackupType(int type)
{
	CommonSettings.manualBackupType = type;
}

void mc_init(memory_chip_t *mc, int type)
{
	mc->com = 0;
	mc->addr = 0;
	mc->addr_shift = 0;
	mc->data = NULL;
	mc->size = 0;
	mc->write_enable = FALSE;
	mc->writeable_buffer = FALSE;
	mc->type = type;
	mc->autodetectsize = 0;

	switch(mc->type)
	{
		case MC_TYPE_EEPROM1:
			mc->addr_size = 1;
			break;
		case MC_TYPE_EEPROM2:
		case MC_TYPE_FRAM:
			mc->addr_size = 2;
			break;
		case MC_TYPE_FLASH:
			mc->addr_size = 3;
			break;
		default: break;
	}
}

u8 *mc_alloc(memory_chip_t *mc, u32 size)
{
	u8 *buffer = new u8[size];
	memset(buffer,0,size);

	delete [] mc->data;
	mc->data = buffer;
	if(!buffer) { return NULL; }
	mc->size = size;
	mc->writeable_buffer = TRUE;

	return buffer;
}

void mc_free(memory_chip_t *mc)
{
    if(mc->data) delete[] mc->data;
    mc_init(mc, 0);
}

void fw_reset_com(memory_chip_t *mc)
{
	if(mc->com == FW_CMD_PAGEWRITE)
	{
		if (mc->fp)
		{
			fseek(mc->fp, 0, SEEK_SET);
			fwrite(mc->data, mc->size, 1, mc->fp);
		}

		mc->write_enable = FALSE;
	}

	mc->com = 0;
}

u8 fw_transfer(memory_chip_t *mc, u8 data)
{
	if(mc->com == FW_CMD_READ || mc->com == FW_CMD_PAGEWRITE) /* check if we are in a command that needs 3 bytes address */
	{
		if(mc->addr_shift > 0)   /* if we got a complete address */
		{
			mc->addr_shift--;
			mc->addr |= data << (mc->addr_shift * 8); /* argument is a byte of address */
		}
		else    /* if we have received 3 bytes of address, proceed command */
		{
			switch(mc->com)
			{
				case FW_CMD_READ:
					if(mc->addr < mc->size)  /* check if we can read */
					{
						data = mc->data[mc->addr];       /* return byte */
						mc->addr++;      /* then increment address */
					}
					break;

				case FW_CMD_PAGEWRITE:
					if(mc->addr < mc->size)
					{
						mc->data[mc->addr] = data;       /* write byte */
						mc->addr++;
					}
					break;
			}

		}
	}
	else if(mc->com == FW_CMD_READSTATUS)
	{
		return (mc->write_enable ? 0x02 : 0x00);
	}
	else if(mc->com == FW_CMD_READ_ID)
	{
		switch(mc->addr)
		{
		//here is an ID string measured from an old ds fat: 62 16 00 (0x62=sanyo)
		//but we chose to use an ST from martin's ds fat string so programs might have a clue as to the firmware size:
		//20 40 12
		case 0: 
			data = 0x20;
			mc->addr=1; 
			break;
		case 1: 
			data = 0x40; //according to gbatek this is the device ID for the flash on someone's ds fat
			mc->addr=2; 
			break;
		case 2: 
			data = 0x12;
			mc->addr = 0; 
			break;
		}
	}
	else	//finally, check if it's a new command
	{
		switch(data)
		{
			case 0: break;	// nothing

			case FW_CMD_READ_ID:
				mc->addr = 0;
				mc->com = FW_CMD_READ_ID;
				break;
			
			case FW_CMD_READ:    //read command
				mc->addr = 0;
				mc->addr_shift = 3;
				mc->com = FW_CMD_READ;
				break;
				
			case FW_CMD_WRITEENABLE:     // enable writing
				if(mc->writeable_buffer) { mc->write_enable = TRUE; }
				break;
				
			case FW_CMD_WRITEDISABLE:    // disable writing
				mc->write_enable = FALSE;
				break;
				
			case FW_CMD_PAGEWRITE:       // write command
				if(mc->write_enable)
				{
					mc->addr = 0;
					mc->addr_shift = 3;
					mc->com = FW_CMD_PAGEWRITE;
				}
				else { data = 0; }
				break;
			
			case FW_CMD_READSTATUS:  //status register command
				mc->com = FW_CMD_READSTATUS;
				break;
				
			default:
				printf("Unhandled FW command: %02X\n", data);
				break;
		}
	}
	
	return data;
}

bool BackupDevice::save_state(EMUFILE* os)
{
	u32 version = 1;
	//v0
	write32le(version,os);
	write32le(write_enable,os);
	write32le(com,os);
	write32le(addr_size,os);
	write32le(addr_counter,os);
	write32le((u32)state,os);
	writebuffer(data,os);
	writebuffer(data_autodetect,os);
	//v1
	write32le(addr,os);
	return true;
}

bool BackupDevice::load_state(EMUFILE* is)
{
	u32 version;
	if(read32le(&version,is)!=1) return false;
	if(version>=0){
		readbool(&write_enable,is);
		read32le(&com,is);
		read32le(&addr_size,is);
		read32le(&addr_counter,is);
		u32 temp;
		read32le(&temp,is);
		state = (STATE)temp;
		readbuffer(data,is);
		readbuffer(data_autodetect,is);
	}
	if(version>=1)
		read32le(&addr,is);


	return true;
}

BackupDevice::BackupDevice()
{
	reset();
}

//due to unfortunate shortcomings in the emulator architecture,
//at reset-time, we won't have a filename to the .dsv file.
//so the only difference between load_rom (init) and reset is that
//one of them saves the filename
void BackupDevice::load_rom(const char* filename)
{
	this->filename = filename;
	reset();
}

void BackupDevice::movie_mode()
{
	reset();
}

void BackupDevice::reset()
{
	com = 0;
	addr = addr_counter = 0;
	flushPending = false;
	lazyFlushPending = false;
	data.resize(0);
	write_enable = FALSE;
	data_autodetect.resize(0);

	state = DETECTING;
	addr_size = 0;
	loadfile();

	//if the user has requested a manual choice for backup type, and we havent imported a raw save file, then apply it now
	if(state == DETECTING && CommonSettings.manualBackupType != MC_TYPE_AUTODETECT)
	{
		state = RUNNING;
		int savetype = save_types[CommonSettings.manualBackupType][0];
		int savesize = save_types[CommonSettings.manualBackupType][1];
		ensure((u32)savesize); //expand properly if necessary
		data.resize(savesize); //truncate if necessary
		addr_size = addr_size_for_old_save_type(savetype);
		flush();
	}
}

void BackupDevice::close_rom()
{
	flush();
}

void BackupDevice::reset_command()
{
	//for a performance hack, save files are only flushed after each reset command
	//(hopefully, after each page)
	if(flushPending)
	{
		flush();
		flushPending = false;
		lazyFlushPending = false;
	}

	if(state == DETECTING && data_autodetect.size()>0)
	{
		//we can now safely detect the save address size
		u32 autodetect_size = data_autodetect.size();

		printf("Autodetecting with autodetect_size=%d\n",autodetect_size);

		const u8 sm64_sig[] = {0x01,0x80,0x00,0x00};
		if(autodetect_size == 4 && !memcmp(&data_autodetect[0],sm64_sig,4))
		{
			addr_size = 2;
		}
		else //detect based on rules
			switch(autodetect_size)
			{
				case 0:
				case 1:
					printf("Catastrophic error while autodetecting save type.\nIt will need to be specified manually\n");
					#ifdef _MSC_VER
					MessageBox(0,"Catastrophic Error Code: Camel;\nyour save type has not been autodetected correctly;\nplease report to developers",0,0);
					#endif
					addr_size = 1; //choose 1 just to keep the busted savefile from growing too big
					break;
				case 2:
					//the modern typical case for small eeproms
					addr_size = 1;
					break;
				case 3:
					//another modern typical case..
					//but unfortunately we select this case for spider-man 3, when what it meant to do was
					//present the archaic 1+2 case
					//it seems that over the hedge does this also.
					addr_size = 2;
					break;
				case 4:
					//a modern typical case
					addr_size = 3;
					break;
				default:
					//the archaic case: write the address and then some modulo-4 number of bytes
					//why modulo 4? who knows.
					addr_size = autodetect_size & 3;
					break;
			}

		state = RUNNING;
		data_autodetect.resize(0);
		flush();
	}

	com = 0;
}
u8 BackupDevice::data_command(u8 val, int cpu)
{
	if(com == BM_CMD_READLOW || com == BM_CMD_WRITELOW)
	{
		//handle data or address
		if(state == DETECTING)
		{
			if(com == BM_CMD_WRITELOW)
			{
				printf("Unexpected backup device initialization sequence using writes!\n");
			}

			//just buffer the data until we're no longer detecting
			data_autodetect.push_back(val);
			val = 0;
		}
		else
		{
			if(addr_counter<addr_size)
			{
				//continue building address
				addr <<= 8;
				addr |= val;
				addr_counter++;
				//if(addr_counter==addr_size) printf("ADR: %08X\n",addr);
			}
			else
			{
				//why does tomb raider underworld access 0x180 and go clear through to 0x280?
				//should this wrap around at 0 or at 0x100?
				if(addr_size == 1) addr &= 0x1FF;

				//address is complete
				ensure(addr+1);
				if(com == BM_CMD_READLOW)
				{
					val = data[addr];
					//flushPending = true; //is this a good idea? it may slow stuff down, but it is helpful for debugging
					lazyFlushPending = true; //lets do this instead
					//printf("read: %08X\n",addr);
				}
				else
				{
					if(write_enable)
					{
						//printf("WRITE ADR: %08X\n",addr);
						data[addr] = val;
						flushPending = true;
						//printf("writ: %08X\n",addr);
					}
				}
				addr++;
			}
		}
	}
	else if(com == BM_CMD_READSTATUS)
	{
		//handle request to read status
		//LOG("Backup Memory Read Status: %02X\n", mc->write_enable << 1);
		return (write_enable << 1);
	}
	else
	{
		//there is no current command. receive one
		switch(val)
		{
			case 0: break; //??

			case 8:
				printf("COMMAND%c: Unverified Backup Memory command: %02X FROM %08X\n",(cpu==ARMCPU_ARM9)?'9':'7',val, (cpu==ARMCPU_ARM9)?NDS_ARM9.instruct_adr:NDS_ARM7.instruct_adr);
				val = 0xAA;
				break;

			case BM_CMD_WRITEDISABLE:
				write_enable = FALSE;
				break;

			case BM_CMD_READSTATUS:
				com = BM_CMD_READSTATUS;
				break;

			case BM_CMD_WRITEENABLE:
				write_enable = TRUE;
				break;

			case BM_CMD_WRITELOW:
			case BM_CMD_READLOW:
				//printf("XLO: %08X\n",addr);
				com = val;
				addr_counter = 0;
				addr = 0;
				break;

			case BM_CMD_WRITEHIGH:
			case BM_CMD_READHIGH:
				//printf("XHI: %08X\n",addr);
				if(val == BM_CMD_WRITEHIGH) val = BM_CMD_WRITELOW;
				if(val == BM_CMD_READHIGH) val = BM_CMD_READLOW;
				com = val;
				addr_counter = 0;
				addr = 0;
				if(addr_size==1) {
					//"write command that's only available on ST M95040-W that I know of"
					//this makes sense, since this device would only have a 256 bytes address space with writelow
					//and writehigh would allow access to the upper 256 bytes
					//but it was detected in pokemon diamond also during the main save process
					addr = 0x1;
				}
				break;

			default:
				printf("COMMAND%c: Unhandled Backup Memory command: %02X FROM %08X\n",(cpu==ARMCPU_ARM9)?'9':'7',val, (cpu==ARMCPU_ARM9)?NDS_ARM9.instruct_adr:NDS_ARM7.instruct_adr);
				break;
		} //switch(val)
	}
	return val;
}

//guarantees that the data buffer has room enough for the specified number of bytes
void BackupDevice::ensure(u32 addr)
{
	u32 size = data.size();
	if(size<addr)
	{
		data.resize(addr);
		for(u32 i=size;i<addr;i++)
			data[i] = kUninitializedSaveDataValue;
	}
}


u32 BackupDevice::addr_size_for_old_save_size(int bupmem_size)
{
	switch(bupmem_size) {
		case MC_SIZE_4KBITS:
			return 1;
		case MC_SIZE_64KBITS:
		case MC_SIZE_256KBITS:
		case MC_SIZE_512KBITS:
			return 2;
		case MC_SIZE_1MBITS:
		case MC_SIZE_2MBITS:
		case MC_SIZE_4MBITS:
		case MC_SIZE_8MBITS:
		case MC_SIZE_16MBITS:
		case MC_SIZE_64MBITS:
			return 3;
		default:
			return 0xFFFFFFFF;
	}
}

u32 BackupDevice::addr_size_for_old_save_type(int bupmem_type)
{
	switch(bupmem_type)
	{
		case MC_TYPE_EEPROM1:
			return 1;
		case MC_TYPE_EEPROM2:
		case MC_TYPE_FRAM:
			return 2;
		case MC_TYPE_FLASH:
			return 3;
		default:
			return 0xFFFFFFFF;
	}
}


void BackupDevice::load_old_state(u32 addr_size, u8* data, u32 datasize)
{
	state = RUNNING;
	this->addr_size = addr_size;
	this->data.resize(datasize);
	memcpy(&this->data[0],data,datasize);

	//dump back out as a dsv, just to keep things sane
	flush();
}

//======================================================================= no$GBA
//=======================================================================
//=======================================================================

static int no_gba_unpackSAV(void *in_buf, u32 fsize, void *out_buf, u32 &size)
{
	const char no_GBA_HEADER_ID[] = "NocashGbaBackupMediaSavDataFile";
	const char no_GBA_HEADER_SRAM_ID[] = "SRAM";
	u8	*src = (u8 *)in_buf;
	u8	*dst = (u8 *)out_buf;
	u32 src_pos = 0;
	u32 dst_pos = 0;
	u8	cc = 0;
	u32	size_unpacked = 0;
	u32	size_packed = 0;
	u32	compressMethod = 0;

	if (fsize < 0x50) return (1);

	for (int i = 0; i < 0x1F; i++)
	{
		if (src[i] != no_GBA_HEADER_ID[i]) return (2);
	}
	if (src[0x1F] != 0x1A) return (2);
	for (int i = 0; i < 0x4; i++)
	{
		if (src[i+0x40] != no_GBA_HEADER_SRAM_ID[i]) return (2);
	}

	compressMethod = *((u32*)(src+0x44));

	if (compressMethod == 0)	// unpacked
	{
		size_unpacked = *((u32*)(src+0x48));
		src_pos = 0x4C;
		for (u32 i = 0; i < size_unpacked; i++)
		{
			dst[dst_pos++] = src[src_pos++];
		}
		size = dst_pos;
		return (0);
	}

	if (compressMethod == 1)	// packed (method 1)
	{
		size_packed = *((u32*)(src+0x48));
		size_unpacked = *((u32*)(src+0x4C));

		src_pos = 0x50;
		while (true)
		{
			cc = src[src_pos++];
			
			if (cc == 0)
			{
				size = dst_pos;
				return (0);
			}

			else if (cc == 0x80)
			{
				u16 tsize = *((u16*)(src+src_pos+1));
				for (int t = 0; t < tsize; t++)
					dst[dst_pos++] = src[src_pos];
				src_pos += 3;
				continue;
			}
			
			else if (cc > 0x80)		// repeat
			{
				cc -= 0x80;
				for (int t = 0; t < cc; t++)
					dst[dst_pos++] = src[src_pos];
				src_pos++;
				continue;
			}
			// copy
			for (int t = 0; t < cc; t++)
				dst[dst_pos++] = src[src_pos++];
		}
		size = dst_pos;
		return (0);
	}
	return (200);
}

static u32 no_gba_savTrim(void *buf, u32 size)
{
	u32 rows = size / 16;
	u32 pos = (size - 16);
	u8 *src = (u8*)buf;

	for (unsigned int i = 0; i < rows; i++, pos -= 16)
	{
		if (src[pos] == 0xFF)
		{
			for (int t = 0; t < 16; t++)
			{
				if (src[pos+t] != 0xFF) return (pos+16);
			}
		}
		else
		{
			return (pos+16);
		}
	}
	return (size);
}

static u32 no_gba_fillLeft(u32 size)
{
	for (u32 i = 1; i < ARRAY_SIZE(save_types); i++)
	{
		if (size <= (u32)save_types[i][1])
			return (size + (save_types[i][1] - size));
	}
	return size;
}

bool BackupDevice::load_no_gba(const char *fname)
{
	FILE	*fsrc = fopen(fname, "rb");
	u8		*in_buf = NULL;
	u8		*out_buf = NULL;

	if (fsrc)
	{
		u32 fsize = 0;
		fseek(fsrc, 0, SEEK_END);
		fsize = ftell(fsrc);
		fseek(fsrc, 0, SEEK_SET);
		//printf("Open %s file (size %i bytes)\n", fname, fsize);

		in_buf = new u8 [fsize];

		if (fread(in_buf, 1, fsize, fsrc) == fsize)
		{
			out_buf = new u8 [8 * 1024 * 1024 / 8];
			u32 size = 0;

			memset(out_buf, 0xFF, 8 * 1024 * 1024 / 8);
			if (no_gba_unpackSAV(in_buf, fsize, out_buf, size) == 0)
			{
				//printf("New size %i byte(s)\n", size);
				size = no_gba_savTrim(out_buf, size);
				//printf("--- new size after trim %i byte(s)\n", size);
				size = no_gba_fillLeft(size);
				//printf("--- new size after fill %i byte(s)\n", size);
				raw_applyUserSettings(size);
				data.resize(size);
				for (u32 tt = 0; tt < size; tt++)
					data[tt] = out_buf[tt];

				//dump back out as a dsv, just to keep things sane
				flush();
				printf("---- Loaded no$GBA save\n");

				if (in_buf) delete [] in_buf;
				if (out_buf) delete [] out_buf;
				fclose(fsrc);
				return true;
			}
			if (out_buf) delete [] out_buf;
		}
		if (in_buf) delete [] in_buf;
		fclose(fsrc);
	}

	return false;
}

bool BackupDevice::save_no_gba(const char* fname)
{
	FILE* outf = fopen(fname,"wb");
	if(!outf) return false;
	u32 size = data.size();
	u32 padSize = pad_up_size(size);
	if(data.size()>0)
		fwrite(&data[0],1,size,outf);
	for(u32 i=size;i<padSize;i++)
		fputc(0xFF,outf);

	if (padSize < 512 * 1024)
	{
		for(u32 i=padSize; i<512 * 1024; i++)
			fputc(0xFF,outf);
	}
	fclose(outf);
	return true;
}
//======================================================================= end
//=======================================================================
//======================================================================= no$GBA


void BackupDevice::loadfile()
{

	if(filename.length() ==0) return; //No sense crashing if no filename supplied

	EMUFILE_FILE* inf = new EMUFILE_FILE(filename.c_str(),"rb");
	if(inf->fail())
	{
		delete inf;
		//no dsv found; we need to try auto-importing a file with .sav extension
		printf("DeSmuME .dsv save file not found. Trying to load an old raw .sav file.\n");
		
		//change extension to sav
		char tmp[MAX_PATH];
		strcpy(tmp,filename.c_str());
		tmp[strlen(tmp)-3] = 0;
		strcat(tmp,"sav");

		inf = new EMUFILE_FILE(tmp,"rb");
		if(inf->fail())
		{
			delete inf;
			printf("Missing save file %s\n",filename.c_str());
			return;
		}
		delete inf;

		if (!load_no_gba(tmp))
			load_raw(tmp);
	}
	else
	{
		//scan for desmume save footer
		const s32 cookieLen = (s32)strlen(kDesmumeSaveCookie);
		char *sigbuf = new char[cookieLen];
		inf->fseek(-cookieLen, SEEK_END);
		inf->fread(sigbuf,cookieLen);
		int cmp = memcmp(sigbuf,kDesmumeSaveCookie,cookieLen);
		delete[] sigbuf;
		if(cmp)
		{
			//maybe it is a misnamed raw save file. try loading it that way
			printf("Not a DeSmuME .dsv save file. Trying to load as raw.\n");
			delete inf;
			if (!load_no_gba(filename.c_str()))
				load_raw(filename.c_str());
			return;
		}
		//desmume format
		inf->fseek(-cookieLen, SEEK_END);
		inf->fseek(-4, SEEK_CUR);
		u32 version = 0xFFFFFFFF;
		read32le(&version,inf);
		if(version!=0) {
			printf("Unknown save file format\n");
			return;
		}
		inf->fseek(-24, SEEK_CUR);
		struct {
			u32 size,padSize,type,addr_size,mem_size;
		} info;
		read32le(&info.size,inf);
		read32le(&info.padSize,inf);
		read32le(&info.type,inf);
		read32le(&info.addr_size,inf);
		read32le(&info.mem_size,inf);

		//establish the save data
		data.resize(info.size);
		inf->fseek(0, SEEK_SET);
		if(info.size>0)
			inf->fread(&data[0],info.size); //read all the raw data we have
		state = RUNNING;
		addr_size = info.addr_size;
		//none of the other fields are used right now

		delete inf;
	}
}

bool BackupDevice::save_raw(const char* filename)
{
	FILE* outf = fopen(filename,"wb");
	if(!outf) return false;
	u32 size = data.size();
	u32 padSize = pad_up_size(size);
	if(data.size()>0)
		fwrite(&data[0],1,size,outf);
	for(u32 i=size;i<padSize;i++)
		fputc(kUninitializedSaveDataValue,outf);
	fclose(outf);
	return true;
}

u32 BackupDevice::pad_up_size(u32 startSize)
{
	u32 size = startSize;
	u32 ctr=0;
	while(ctr<saveSizes_count && size > saveSizes[ctr]) ctr++;
	u32 padSize = saveSizes[ctr];
	if(padSize == 0xFFFFFFFF)
	{
		printf("PANIC! Couldn't pad up save size. Refusing to pad.\n");
		padSize = startSize;
	}
		return padSize;
}

void BackupDevice::lazy_flush()
{
	if(flushPending || lazyFlushPending)
	{
		lazyFlushPending = flushPending = false;
		flush();
	}
}

void BackupDevice::flush()
{

	EMUFILE* outf = new EMUFILE_FILE(filename.c_str(),"wb");
	if(!outf->fail())
	{
		if(data.size()>0)
			outf->fwrite(&data[0],data.size());
		
		//write the footer. we use a footer so that we can maximize the chance of the
		//save file being recognized as a raw save file by other emulators etc.
		
		//first, pad up to the next largest known save size.
		u32 size = data.size();
		u32 padSize = pad_up_size(size);

		for(u32 i=size;i<padSize;i++)
			outf->fputc(kUninitializedSaveDataValue);

		//this is just for humans to read
		outf->fprintf("|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:");

		//and now the actual footer
		write32le(size,outf); //the size of data that has actually been written
		write32le(padSize,outf); //the size we padded it to
		write32le(0,outf); //save memory type
		write32le(addr_size,outf);
		write32le(0,outf); //save memory size
		write32le(0,outf); //version number
		outf->fprintf("%s", kDesmumeSaveCookie); //this is what we'll use to recognize the desmume format save

		delete outf;
	}
	else
	{
		delete outf;
		printf("Unable to open savefile %s\n", filename.c_str());
	}
}

void BackupDevice::raw_applyUserSettings(u32& size)
{
	//respect the user's choice of backup memory type
	if(CommonSettings.manualBackupType == MC_TYPE_AUTODETECT){
		addr_size = addr_size_for_old_save_size(size);

	}
	else
	{
		int savetype = save_types[CommonSettings.manualBackupType][0];
		int savesize = save_types[CommonSettings.manualBackupType][1];
		addr_size = addr_size_for_old_save_type(savetype);
		if((u32)savesize<size) size = savesize;
	}

	state = RUNNING;
}


bool BackupDevice::load_raw(const char* filename)
{
	FILE* inf = fopen(filename,"rb");
	fseek(inf, 0, SEEK_END);
	u32 size = (u32)ftell(inf);
	fseek(inf, 0, SEEK_SET);

	raw_applyUserSettings(size);

	data.resize(size);
	fread(&data[0],1,size,inf);
	fclose(inf);

	//dump back out as a dsv, just to keep things sane
	flush();

	return true;
}


bool BackupDevice::load_duc(const char* filename)
{
	u32 size;
	char id[16];
	FILE* file = fopen(filename, "rb");
	if(file == NULL)
		return false;

	fseek(file, 0, SEEK_END);
	size = (u32)ftell(file) - 500;
	fseek(file, 0, SEEK_SET);
	
	// Make sure we really have the right file
	fread((void *)id, sizeof(char), 16, file);

	if (memcmp(id, "ARDS000000000001", 16) != 0)
	{
		printf("Not recognized as a valid DUC file\n");
		fclose(file);
		return false;
	}
	// Skip the rest of the header since we don't need it
	fseek(file, 500, SEEK_SET);
	
	raw_applyUserSettings(size);
	
	ensure((u32)size);
	
	fread(&data[0],1,size,file);
	fclose(file);
	
	//choose
	
	flush();
	
	return true;

}

bool BackupDevice::load_movie(EMUFILE* is) {

	const s32 cookieLen = (s32)strlen(kDesmumeSaveCookie);

	is->fseek(-cookieLen, SEEK_END);
	is->fseek(-4, SEEK_CUR);

	u32 version = 0xFFFFFFFF;
	is->fread((char*)&version,4);
	if(version!=0) {
		printf("Unknown save file format\n");
		return false;
	}
	is->fseek(-24, SEEK_CUR);

	struct{
		u32 size,padSize,type,addr_size,mem_size;
	}info;

	is->fread((char*)&info.size,4);
	is->fread((char*)&info.padSize,4);
	is->fread((char*)&info.type,4);
	is->fread((char*)&info.addr_size,4);
	is->fread((char*)&info.mem_size,4);

	//establish the save data
	data.resize(info.size);
	is->fseek(0, SEEK_SET);
	if(info.size>0)
		is->fread((char*)&data[0],info.size);

	state = RUNNING;
	addr_size = info.addr_size;
	//none of the other fields are used right now

	return true;
}
