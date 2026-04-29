// Ides Adv98V "TC0" decoder
// Written by Valley Bell, 2026-04-29
// PANDA HOUSE 'PIYO' decoder
#include <stdio.h>
#include <stdlib.h>
#include <string.h>	// for memcmp/memcpy

typedef unsigned char UINT8;
typedef unsigned short UINT16;

static UINT16 ReadLE16(const UINT8* data);
static void WriteLE16(UINT8* buffer, UINT16 value);
static UINT8 CheckTC0Signature(size_t dataLen, const UINT8* data, size_t tc0Base);
static size_t ScanForKeyCalc(size_t scanLen, const UINT8* scanData);
static void DecodeData(UINT8* dst, const UINT8* src, size_t len, UINT16 keyInit, UINT16 keyMult, UINT16 keyAdd);
static size_t DecodeEXEData(size_t srcLen, UINT8* data);


int main(int argc, char* argv[])
{
	int argbase;
	FILE* hFile;
	size_t dataLen;
	UINT8* data;
	size_t tc0Base;
	unsigned int iteration;
	
	printf("Ides 'TC0' decoder\n------------------\n");
	if (argc < 3)
	{
		printf("Usage: %s ADVBIOS.OVL ADVBIOS_DEC.OVL\n", argv[0]);
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
	dataLen = ftell(hFile);
	if (dataLen > 0x100000)	// 1 MB
		dataLen = 0x100000;
	
	fseek(hFile, 0x00, SEEK_SET);
	data = (UINT8*)malloc(dataLen);
	fread(data, 0x01, dataLen, hFile);
	
	fclose(hFile);
	
	if (!(data[0x00] == 'M' && data[0x01] == 'Z'))	// check "MZ" signature
	{
		free(data);
		printf("Not an EXE file!\n");
		return 1;
	}
	
	tc0Base = ReadLE16(&data[0x08]) * 0x10 + ReadLE16(&data[0x16]) * 0x10 + ReadLE16(&data[0x14]);
	if (!CheckTC0Signature(dataLen, data, tc0Base))
	{
		free(data);
		printf("TC0 signature not found!\n");
		return 1;
	}
	
	for (iteration = 0; ; iteration++)
	{
		tc0Base = ReadLE16(&data[0x08]) * 0x10 + ReadLE16(&data[0x16]) * 0x10 + ReadLE16(&data[0x14]);
		if (!CheckTC0Signature(dataLen, data, tc0Base))
			break;
		
		printf("--- Iteration %u ---\n", 1 + iteration);
		dataLen = DecodeEXEData(dataLen, data);
		printf("\n", iteration);
	}
	
	if (dataLen != (size_t)-1)
	{
		hFile = fopen(argv[argbase + 1], "wb");
		if (hFile == NULL)
		{
			free(data);
			printf("Error opening %s!\n", argv[argbase + 1]);
			return 2;
		}
		fwrite(data, 0x01, dataLen, hFile);
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

static UINT8 CheckTC0Signature(size_t dataLen, const UINT8* data, size_t tc0Base)
{
	if (tc0Base + 0x14 > dataLen)
		return 0;
	return !memcmp(&data[tc0Base + 0x03], "TC0", 0x03);
}

static size_t ScanForKeyCalc(size_t scanLen, const UINT8* scanData)
{
	/* disassembly of the decryption function:
		8B 07           MOV     AX, [BX]    ; key: initial value
		8B 4F 02        MOV     CX, [BX+2]  ; number of bytes to decode
		8B 7F 04        MOV     DI, [BX+4]  ; decryption start pointer (100h == where first byte of the EXE payload)
		BE 11 27        MOV     SI, 2711h   ; key: multiplicator
		__          decode_loop:
		F7 E6           MUL     AX, SI
		05 19 36        ADD     AX, 3619h   ; key: increment
		8B D0           MOV     DX, AX
		D1 C2           ROL     DX, 1
		D1 C2           ROL     DX, 1
		26 30 35        XOR     ES:[DI], DH
		47              INC     DI
		E2 EF           LOOP    decode_loop
	*/
	size_t curPos;
	
	for (curPos = 0x00; curPos <= scanLen - 0x08; curPos ++)
	{
		if (scanData[curPos + 0x00] == 0xBE && scanData[curPos + 0x05] == 0x05)
			return curPos;	// return offset of "MOV SI, n"
	}
	
	return (size_t)-1;
}

static void DecodeData(UINT8* dst, const UINT8* src, size_t len, UINT16 keyInit, UINT16 keyMult, UINT16 keyAdd)
{
	size_t pos;
	UINT16 key = keyInit;
	for (pos = 0x00; pos < len; pos ++)
	{
		UINT8 xorVal;
		key *= keyMult;                 //  MUL     AX, SI  ; AX = low 16 bits of the result
		key += keyAdd;                  //  ADD     AX, 3619h
		xorVal = (key >> 6) & 0xFF;     //  MOV     DX, AX    / ROL     DX, 2
		dst[pos] = src[pos] ^ xorVal;   //  XOR     [DI], DH
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
	size_t tc0Base = baseOfs + initCS * 0x10 + initIP;
	const UINT8* tc0Data = &data[tc0Base];
	size_t keyCalcPos;
	UINT16 keyInit;
	UINT16 keyMult;
	UINT16 keyAdd;
	UINT16 decLen;
	UINT16 newIP;
	size_t newLen;
	UINT16 newLastPageSize;
	UINT16 newPageCount;
	
	/* layout of decode segment in encrypted EXE file:
		00..02: [code] CALL $+14h (jump over TC0 decode block)
		03..05: "TC0" signature (ignored)
		06..07: key initialization value
		08..09: size of data to decode
		0A..0B: decryption start pointer (100h == where first byte of the EXE payload)
		0C..0D: register IP (instruction pointer) -> MZ offset 14h
		0E..0F: register CS (code segment) -> MZ offset 16h
		10..11: register SP (stack pointer) -> MZ offset 10h
		12..13: register SS (stack segment) -> MZ offset 0Eh
		14..  : [code] decoding logic
	*/
	
	// decode program data
	keyInit = ReadLE16(&tc0Data[0x06]);
	keyCalcPos = ScanForKeyCalc(srcLen - tc0Base, tc0Data);
	if (keyCalcPos == (size_t)-1)
	{
		printf("Warning: Unable to parse decoding logic.\n");
		printf("Falling back to default key mult/add values.\n");
		keyMult = 0x2711;
		keyAdd = 0x3619;
	}
	else
	{
		keyMult = ReadLE16(&tc0Data[keyCalcPos + 0x01]);
		keyAdd = ReadLE16(&tc0Data[keyCalcPos + 0x06]);
	}
	decLen = ReadLE16(&tc0Data[0x08]);
	printf("Encrypted bytes: 0x%04X\n", decLen);
	printf("Key init: 0x%04X, mult: 0x%04X, add: 0x%04X\n", keyInit, keyMult, keyAdd);
	DecodeData(&data[baseOfs], &data[baseOfs], decLen, keyInit, keyMult, keyAdd);
	newLen = baseOfs + decLen;
	printf("EXE size: 0x%04X -> 0x%04X bytes\n", (unsigned)srcLen, (unsigned)newLen);
	
	// copy 8086 register values into MZ header
	memcpy(&data[0x14], &tc0Data[0x0C], 0x04);	// copy register CS:IP
	memcpy(&data[0x10], &tc0Data[0x10], 0x02);	// copy register SS
	memcpy(&data[0x0E], &tc0Data[0x12], 0x02);	// copy register SP
	
	// recalculate EXE size
	newPageCount = (UINT16)((newLen + 0x1FF) / 0x200);
	newLastPageSize = (UINT16)(newLen & 0x1FF);
	if (newLastPageSize == 0x000)
		newLastPageSize = 0x200;
	WriteLE16(&data[0x02], newLastPageSize);
	WriteLE16(&data[0x04], newPageCount);
	
	return newLen;
}
