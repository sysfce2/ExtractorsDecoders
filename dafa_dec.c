// "Dragoon Armor For Adult" DUNGEON.EXE decoder
// Written by Valley Bell, 2024-04-01
// This removes the first layer of EXE protection - a rotating XOR over most of the EXE file.
// based on PANDA HOUSE 'PIYO' decoder
#include <stdio.h>
#include <stdlib.h>
#include <string.h>	// for memcmp/memcpy

typedef unsigned char UINT8;
typedef unsigned short UINT16;
typedef signed short INT16;
typedef unsigned int UINT32;

static UINT16 ReadLE16(const UINT8* data);
static void WriteLE16(UINT8* buffer, UINT16 value);
static UINT8 CheckDecryptionCode(size_t dataLen, const UINT8* data);
static void DecodeData(UINT8* dst, const UINT8* src, size_t len, UINT16 keyInit, UINT16 keyMult, UINT16 keyAdd);
static size_t DecodeEXEData(size_t srcLen, UINT8* data);


int main(int argc, char* argv[])
{
	int argbase;
	FILE* hFile;
	size_t srcLen;
	size_t decLen;
	UINT8* data;
	
	printf("DUNGEON.EXE decoder\n-------------------\n");
	if (argc < 3)
	{
		printf("Usage: %s DUNGEON.EXE DECODED.EXE\n", argv[0]);
		printf("Removes the EXE protection of DUNGEON.EXE\n");
		printf("from \"Dragoon Armor For Adult\" by FairyTale\n");
		return 0;
	}
	
	argbase = 1;
	hFile = fopen(argv[argbase + 0], "rb");
	if (hFile == NULL)
	{
		printf("Error opening file!\n");
		return 1;
	}
	
	fseek(hFile, 0x00, SEEK_END);
	srcLen = ftell(hFile);
	if (srcLen > 0x100000)	// 1 MB
		srcLen = 0x100000;
	
	fseek(hFile, 0x00, SEEK_SET);
	data = (UINT8*)malloc(srcLen);
	fread(data, 0x01, srcLen, hFile);
	
	fclose(hFile);
	
	if (!(data[0x00] == 'M' && data[0x01] == 'Z'))	// check "MZ" signature
	{
		free(data);
		printf("Not an EXE file!\n");
		return 1;
	}
	decLen = DecodeEXEData(srcLen, data);
	
	if (decLen != (size_t)-1)
	{
		hFile = fopen(argv[argbase + 1], "wb");
		if (hFile == NULL)
		{
			free(data);
			printf("Error opening %s!\n", argv[argbase + 1]);
			return 2;
		}
		fwrite(data, 0x01, decLen, hFile);
		fclose(hFile);
		
		printf("Done.\n");
	}
	
	free(data);
	
#ifdef _DEBUG
	getchar();
#endif
	
	return 0;
}

static UINT16 ReadLE16(const UINT8* data)
{
	return (data[0x01] << 8) | (data[0x00] << 0);
}

static void WriteLE16(UINT8* buffer, UINT16 value)
{
	buffer[0x00] = (value >> 0) & 0xFF;
	buffer[0x01] = (value >> 8) & 0xFF;
	
	return;
}

static UINT8 CheckDecryptionCode(size_t dataLen, const UINT8* data)
{
	/* disassembly of the decryption function:
		B8 BE 59        MOV     AX, 59BEh   ; key: initial value
		BB AB 37        MOV     BX, 37ABh   ; key: multiplicator
		BF 00 01        MOV     DI, 100h    ; data start pointer (100h == where first byte of the EXE payload)
		B9 13 76        MOV     CX, 7613h   ; number of bytes to decode
		__          decode_loop:
		F7 E3           MUL     AX, BX
		05 A1 31        ADD     AX, 31A1h   ; key: increment
		30 15           XOR     [DI], DL
		47              INC     DI
		E2 F6           LOOP    decode_loop
		E9 86 FE        JMP     real_start
	*/
	if (dataLen < 0x19)
		return 0;
	return (data[0x00] == 0xB8 &&
		data[0x03] == 0xBB &&
		data[0x06] == 0xBF &&
		data[0x09] == 0xB9 &&
		data[0x0C] == 0xF7 && data[0x0D] == 0xE3 &&
		data[0x0E] == 0x05 &&
		data[0x11] == 0x30 && data[0x12] == 0x15 &&
		data[0x13] == 0x47 &&
		data[0x14] == 0xE2 && data[0x15] == 0xF6 &&
		data[0x16] == 0xE9);
}

static void DecodeData(UINT8* dst, const UINT8* src, size_t len, UINT16 keyInit, UINT16 keyMult, UINT16 keyAdd)
{
	size_t pos;
	UINT32 key = keyInit;
	for (pos = 0x00; pos < len; pos ++)
	{
		UINT8 xorVal;
		key *= keyMult;                 //  MUL     AX, BX  ; AX = low 16 bits of the result
		xorVal = (key >> 16) & 0xFF;    //                  ; DX = high 16 bits of the result
		dst[pos] = src[pos] ^ xorVal;   //  XOR     [DI], DL
		key += keyAdd;                  //  ADD     AX, 31A1h
		key &= 0xFFFF;
	}
	return;
}

/* MZ EXE file header structure:
	Pos     Len     Description
	00      02      "MZ" signature
	02      02      number of bytes in last 512-byte page
	04      02      number of 512-byte pages (includes partial past page)
	06      02      number of relocation entries
	08      02      header size in 16-byte paragraphs
	0A      02      additional allocation: minimum number of paragraphs
	0C      02      additional allocation: maximum number of paragraphs (often 0FFFFh)
	0E      02      initial value of SS register
	10      02      initial value of SP register
	12      02      checksum (can be 0)
	14      02      initial value of IP register
	16      02      initial value of CS register
	18      02      offset of relocation table
	1A      02      overlay number (usually 0)
*/
static size_t DecodeEXEData(size_t srcLen, UINT8* data)
{
	UINT16 hdrSize = ReadLE16(&data[0x08]);	// in paragraphs of 0x10 bytes
	UINT16 initIP = ReadLE16(&data[0x14]);
	UINT16 initCS = ReadLE16(&data[0x16]);
	size_t baseOfs = hdrSize * 0x10;
	size_t codePos = baseOfs + initCS * 0x10 + initIP;
	const UINT8* decCode = &data[codePos];
	UINT16 keyInit;
	UINT16 keyMult;
	UINT16 keyAdd;
	UINT16 decLen;
	UINT16 newIP;
	size_t newLen;
	UINT16 newLastPageSize;
	UINT16 newPageCount;
	
	if (!CheckDecryptionCode(srcLen - codePos, decCode))
	{
		printf("Unsupported encryption code!\n");
		return (size_t)-1;
	}
	
	// decode program data
	keyInit = ReadLE16(&decCode[0x01]);
	keyMult = ReadLE16(&decCode[0x04]);
	keyAdd = ReadLE16(&decCode[0x0F]);
	decLen = ReadLE16(&decCode[0x0A]);
	newIP = (initIP + 0x19) + (INT16)ReadLE16(&decCode[0x17]);	// calculate original regIP from JMP instruction
	printf("Encrypted bytes: 0x%04X\n", decLen);
	printf("Key init: 0x%04X, mult: 0x%04X, add: 0x%04X\n", keyInit, keyMult, keyAdd);
	DecodeData(&data[baseOfs], &data[baseOfs], decLen, keyInit, keyMult, keyAdd);
	newLen = codePos;
	printf("regIP: 0x%04X -> 0x%04X\n", initIP, newIP);
	printf("EXE size: 0x%04X -> 0x%04X bytes\n", (unsigned)srcLen, (unsigned)newLen);
	
	// adjust 8086 register values in MZ header
	// register CS stays the same
	WriteLE16(&data[0x14], newIP);	// copy register IP
	// register SS/SP stay the same
	
	// recalculate EXE size
	newPageCount = (UINT16)((newLen + 0x1FF) / 0x200);
	newLastPageSize = (UINT16)(newLen & 0x1FF);
	if (newLastPageSize == 0x000)
		newLastPageSize = 0x200;
	WriteLE16(&data[0x02], newLastPageSize);
	WriteLE16(&data[0x04], newPageCount);
	
	return newLen;
}
