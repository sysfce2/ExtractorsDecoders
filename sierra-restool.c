// Sierra resource archive tool
// ----------------------------
// Valley Bell, written on 2026-01-02

/*
Archive Format (RESOURCE.NNN)
--------------
[loop]
  0Dh bytes - file name (null-terminated, padded with garbage)
  4 bytes   - file data length ll (Little Endian)
  ll bytes  - file data
[loop end]

Hashmap Format (RESOURCE.MAP)
--------------
4 bytes - list of file name hash byte indices
2 bytes - number of resource files n
[loop n times]
  0Dh bytes - resource file name (null-terminated, padded with garbage)
  2 bytes   - number of contained files fc (Little Endian)
  [loop fc times]
    4 bytes - file name hash
    4 bytes - offset in resource file (points to file name string)
  [loop end]
[loop end]

The games use the hashmap file to determine where each file can be read from.
However the archive files are self-contained and don't need the hashmap to be extracted.

Games verified to work:
- The Incredible Machine (archives: RESOURCE.00#, hashmap: RESOURCE.MAP)
- The Adventures of Willy Beamish (archives: VOLUME.00#, hashmap: VOLUME.RMF)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER
#define strdup	_strdup
#endif

#ifdef _WIN32
#include <direct.h>		// for _mkdir()
#define MakeDir(x)	_mkdir(x)
#else
#include <sys/stat.h>	// for mkdir()
#define MakeDir(x)	mkdir(x, 0755)
#endif

#ifdef HAVE_STDINT

#include <stdint.h>
typedef uint8_t		UINT8;
typedef int16_t		INT16;
typedef uint16_t	UINT16;
typedef uint32_t	UINT32;

#else	// ! HAVE_STDINT

typedef unsigned char	UINT8;
typedef signed short	INT16;
typedef unsigned short	UINT16;
typedef unsigned int	UINT32;

#endif	// HAVE_STDINT


typedef struct _file_item
{
	char* fileName;
	UINT32 filePos;
	UINT32 size;
} FILE_ITEM;

typedef struct _file_list
{
	size_t alloc;
	size_t count;
	FILE_ITEM* items;
} FILE_LIST;


static int ParseHashIndices(const char* arg);
static UINT8 ReadFileData(const char* fileName, UINT32* retSize, UINT8** retData);
static size_t GetFileSize(const char* fileName);
static UINT8 WriteFileData(const char* fileName, UINT32 dataLen, const void* data);
static const char* GetFileTitle(const char* filePath);

static void NormalizePath(char* filePath);
static void CreateDirTree(const char* dirPath);
static int ExtractArchive(const char* arcFileName, const char* outFolder);

static FILE_ITEM* AddFileListItem(FILE_LIST* fl);
static void FreeFileList(FILE_LIST* fl);
static void RemoveControlChars(char* str);
static size_t GetColumns(char* line, size_t maxCols, char* colPtrs[], const char* delim);
static char SanitizeFNChar(char c);
static void CopyDOSFileName(char buffer[13], const char* fileTitle);
static int CreateArchive(const char* arcFileName, const char* fileListName);

static int CreateMapFile(const char* mapFileName, size_t arcCnt, const char* arcList[]);

static UINT16 ReadLE16(const UINT8* data);
static UINT32 ReadLE32(const UINT8* data);
static void WriteLE16(UINT8* buffer, UINT16 value);
static void WriteLE32(UINT8* buffer, UINT32 value);


#define MODE_NONE		0x00
#define MODE_EXTRACT	0x01
#define MODE_CREATE		0x02
#define MODE_MAP_RECALC	0x03

static char DOS_FILENAME_CHARS[0x80] = {
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	// 00..0F
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	// 10..1F
	0, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 0, 0,	// 20..2F
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0,	// 30..3F
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,	// 40..4F
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1,	// 50..5F
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,	// 60..6F
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 1, 0,	// 70..7F
};


static UINT8 hashIndices[4] = {0, 1, 6, 7};	// default indices in TIM
static UINT8 hashIdxOverrd = 0;

int main(int argc, char* argv[])
{
	int argbase;
	UINT8 mode;
	
	printf("Sierra resource archive tool\n----------------------------\n");
	if (argc < 3)
	{
		printf("Usage: %s [mode/options] resource.xxx <parameters>\n", argv[0]);
		printf("Mode: (required)\n");
		printf("    -x      extract single archive, parameters: output-folder\n");
		printf("    -c      create single archive, parameters: filelist.txt\n");
		printf("    -m      recreate MAP file, parameters: RES.001 [RES.002 ...]\n");
		printf("            Note: Hash indexes are read from the MAP file when possible\n");
		printf("                  They can be explicitly set using the -b option\n");
		printf("Options:\n");
		printf("    -b n    [mode -m] set hash index bytes, format: 0,1,5,7\n");
		return 0;
	}
	
	argbase = 1;
	mode = MODE_NONE;
	while(argbase < argc && argv[argbase][0] == '-')
	{
		if (argv[argbase][1] == 'x')
		{
			mode = MODE_EXTRACT;
		}
		else if (argv[argbase][1] == 'c')
		{
			mode = MODE_CREATE;
		}
		else if (argv[argbase][1] == 'm')
		{
			mode = MODE_MAP_RECALC;
		}
		else if (argv[argbase][1] == 'b')
		{
			int ret;
			argbase ++;
			if (argbase >= argc)
			{
				printf("Argument incomplete!\n");
				return 1;
			}
			ret = ParseHashIndices(argv[argbase]);
			if (ret)
				return ret;
		}
		else
			break;
		argbase ++;
	}
	switch(mode)
	{
	case MODE_NONE:
		printf("Please specify a mode!\n");
		return 1;
	case MODE_EXTRACT:
		if (argc < argbase + 2)
		{
			printf("Insufficient arguments!\n");
			return 1;
		}
		return ExtractArchive(argv[argbase + 0], argv[argbase + 1]);
	case MODE_CREATE:
		if (argc < argbase + 2)
		{
			printf("Insufficient arguments!\n");
			return 1;
		}
		return CreateArchive(argv[argbase + 0], argv[argbase + 1]);
	case MODE_MAP_RECALC:
		if (argc < argbase + 2)
		{
			printf("Insufficient arguments!\n");
			return 1;
		}
		return CreateMapFile(argv[argbase + 0], argc - (argbase + 1), &argv[argbase + 1]);
	default:
		printf("Unsupported mode!\n");
		return 1;
	}
	
	return 0;
}

static int ParseHashIndices(const char* arg)
{
	UINT8 idx;
	const char* ptr;
	
	ptr = arg;
	for (idx = 0; idx < 4; idx ++)
	{
		char* end;
		long val = strtol(ptr, &end, 0);
		if (end == ptr)
		{
			if (*ptr == '\0')
				printf("Hash indices list incomplete!\n");
			else
				printf("Error parsing value: %s\n", ptr);
			return 1;
		}
		if (val < 0 || val >= 12)
		{
			printf("Hash index %ld is invalid! (must be 0..11)\n", val);
			return 1;
		}
		hashIndices[idx] = (UINT8)val;
		ptr = end;
		if (*ptr != '\0')
		{
			if (*ptr != ',')
			{
				printf("Hash index values must be comma-separated!\n");
				return 1;
			}
			ptr ++;
		}
	}
	
	hashIdxOverrd = 1;
	return 0;
}

static UINT8 ReadFileData(const char* fileName, UINT32* retSize, UINT8** retData)
{
	FILE* hFile;
	UINT32 readBytes;
	
	hFile = fopen(fileName, "rb");
	if (hFile == NULL)
		return 0xFF;
	
	fseek(hFile, 0, SEEK_END);
	*retSize = ftell(hFile);
	if (*retSize > 0x10000000)
		*retSize = 0x10000000;	// limit to 256 MB
	
	*retData = (UINT8*)realloc(*retData, *retSize);
	fseek(hFile, 0, SEEK_SET);
	readBytes = fread(*retData, 0x01, *retSize, hFile);
	
	fclose(hFile);
	return (readBytes == *retSize) ? 0 : 1;
}

static size_t GetFileSize(const char* fileName)
{
	FILE* hFile;
	size_t fileSize;
	
	hFile = fopen(fileName, "rb");
	if (hFile == NULL)
		return (size_t)-1;
	
	fseek(hFile, 0, SEEK_END);
	fileSize = ftell(hFile);
	
	fclose(hFile);
	return fileSize;
}

static UINT8 WriteFileData(const char* fileName, UINT32 dataLen, const void* data)
{
	FILE* hFile;
	UINT32 writtenBytes;
	
	hFile = fopen(fileName, "wb");
	if (hFile == NULL)
		return 0xFF;
	
	writtenBytes = fwrite(data, 1, dataLen, hFile);
	
	fclose(hFile);
	return (writtenBytes == dataLen) ? 0 : 1;
}

static const char* GetFileTitle(const char* filePath)
{
	const char* sepPos1 = strrchr(filePath, '/');
	const char* sepPos2 = strrchr(filePath, '\\');
	const char* dirSepPos;
	
	if (sepPos1 == NULL)
		dirSepPos = sepPos2;
	else if (sepPos2 == NULL)
		dirSepPos = sepPos1;
	else
		dirSepPos = (sepPos1 < sepPos2) ? sepPos2 : sepPos1;
	return (dirSepPos != NULL) ? &dirSepPos[1] : filePath;
}


static void NormalizePath(char* filePath)
{
	for (; *filePath != '\0'; filePath ++)
	{
		if (*filePath == '\\')
			*filePath = '/';
	}
	
	return;
}

static void CreateDirTree(const char* dirPath)
{
	char* tempPath;
	char* dirSepPos;
	char bakChr;
	
	tempPath = strdup(dirPath);
	NormalizePath(tempPath);
	
	dirSepPos = strchr(tempPath, '/');
	while(dirSepPos != NULL)
	{
		bakChr = *dirSepPos;
		*dirSepPos = '\0';
		MakeDir(tempPath);
		*dirSepPos = bakChr;
		dirSepPos = strchr(dirSepPos + 1, '/');
	}
	free(tempPath);
	
	return;
}

static int ExtractArchive(const char* arcFileName, const char* outFolder)
{
	size_t arcSize;
	UINT8* arcData;
	UINT8 retVal;
	char* outPath;
	char* outName;
	FILE* hListFile;
	UINT32 curPos;
	size_t fileCnt;
	size_t curFile;
	
	outPath = (char*)malloc(strlen(outFolder) + 0x11);
	strcpy(outPath, outFolder);
	outName = outPath + strlen(outPath);
	if (outName > outPath)
	{
		char pathEnd = outName[-1];
#ifdef _WIN32
		if (! (pathEnd == '/' || pathEnd == '\\'))
		{
			*outName = '\\';
			outName ++;
		}
#else
		if (pathEnd != '/')
		{
			*outName = '/';
			outName ++;
		}
#endif
	}
	
	arcSize = 0;
	arcData = NULL;
	retVal = ReadFileData(arcFileName, &arcSize, &arcData);
	if (retVal)
	{
		if (retVal == 0xFF)
			printf("Error opening %s!\n", arcFileName);
		else
			printf("Unable to fully read %s!\n", arcFileName);
		return 2;
	}
	
	// count files
	for (fileCnt = 0, curPos = 0x00; curPos < arcSize; fileCnt ++)
	{
		UINT32 fileSize;
		if (curPos + 0x11 > arcSize)
			break;
		fileSize = ReadLE32(&arcData[curPos + 0x0D]);
		curPos += 0x11 + fileSize;
	}
	
	// create directory and text file
	strcpy(outName, "");
	CreateDirTree(outPath);
	
	strcpy(outName, "_fileLst.txt");
	hListFile = fopen(outPath, "wt");
	if (hListFile == NULL)
	{
		printf("Error writing %s!\n", outPath);
		free(arcData);
		return 3;
	}
	
	fprintf(hListFile, "#filename\n");
	printf("%u %s\n", fileCnt, (fileCnt == 1) ? "file" : "files");
	
	// extract everything
	curPos = 0x00;
	for (curFile = 0; curFile < fileCnt; curFile ++)
	{
		const char* fileName = (char*)&arcData[curPos + 0x00];
		UINT32 fileSize = ReadLE32(&arcData[curPos + 0x0D]);
		UINT32 filePos = curPos + 0x11;
		curPos += 0x11 + fileSize;
		
		outName[13] = '\0';
		strncpy(outName, fileName, 13);
		printf("File %u/%u: offset: 0x%06X, size 0x%04X, name: %s\n",
			1 + curFile, fileCnt, filePos, fileSize, outName);
		if (filePos + fileSize > arcSize)
		{
			fileSize = arcSize - filePos;
			printf("Warning! Early archive end - truncating file to %u bytes.\n", fileSize);
		}
		
		fprintf(hListFile, "%s\n", outName);
		retVal = WriteFileData(outPath, fileSize, &arcData[filePos]);
		if (retVal)
		{
			if (retVal == 0xFF)
				printf("Error writing %s!\n", outPath);
			else
				printf("Error writing %s - file incomplete!\n", outPath);
			continue;
		}
	}
	
	fclose(hListFile);
	free(arcData);
	
	printf("Done.\n");
	return 0;
}


static FILE_ITEM* AddFileListItem(FILE_LIST* fl)
{
	if (fl->count >= fl->alloc)
	{
		fl->alloc += 0x10;
		fl->items = (FILE_ITEM*)realloc(fl->items, fl->alloc * sizeof(FILE_ITEM));
	}
	fl->count ++;
	return &fl->items[fl->count - 1];
}

static void FreeFileList(FILE_LIST* fl)
{
	size_t curFile;
	for (curFile = 0; curFile < fl->count; curFile ++)
	{
		free(fl->items[curFile].fileName);
	}
	fl->count = 0;
	fl->alloc = 0;
	free(fl->items);	fl->items = NULL;
	return;
}

static void RemoveControlChars(char* str)
{
	size_t idx = strlen(str);
	
	while(idx > 0 && (unsigned char)str[idx - 1] < 0x20)
		idx --;
	str[idx] = '\0';
	
	return;
}

static size_t GetColumns(char* line, size_t maxCols, char* colPtrs[], const char* delim)
{
	size_t curCol = 0;
	char* token = line;
	while(token != NULL && curCol < maxCols)
	{
		colPtrs[curCol] = token;
		curCol ++;
		
		token = strpbrk(token, "\t");
		if (token != NULL)
		{
			*token = '\0';
			token ++;
		}
	}
	return curCol;
}

static char SanitizeFNChar(char c)
{
	if (c >= 'a' && c <= 'z')
		c -= ('a' - 'A');	// only uppercase file names are allowed
	if (c < 0x80 && DOS_FILENAME_CHARS[c])
		return c;
	else
		return '_';	// replace invalid characters with underscore
}

static void CopyDOSFileName(char buffer[13], const char* fileTitle)
{
	size_t inPos;
	size_t outPos;
	size_t remChars;
	const char* extPos = strrchr(fileTitle, '.');
	
	// generate base name
	inPos = 0;
	outPos = 0;
	for (remChars = 8; remChars > 0; remChars --)
	{
		if (fileTitle[inPos] == '\0' || &fileTitle[inPos] == extPos)
			break;	// stop at end or file extension
		buffer[outPos] = SanitizeFNChar(fileTitle[inPos]);
		inPos ++;	outPos ++;
	}
	if (&fileTitle[inPos] == extPos)
	{
		buffer[outPos] = fileTitle[inPos];	// copy extension dot
		inPos ++;	outPos ++;
		
		// generate file extension
		for (remChars = 3; remChars > 0; remChars --)
		{
			if (fileTitle[inPos] == '\0')
				break;	// stop at end or file extension
			buffer[outPos] = SanitizeFNChar(fileTitle[inPos]);
			inPos ++;	outPos ++;
		}
	}
	buffer[outPos] = '\0';
	return;
}

static int CreateArchive(const char* arcFileName, const char* fileListName)
{
	FILE_LIST fileList;
	FILE* hFile;
	UINT32 lineID;
	char lineStr[0x1000];	// 4096 chars should be enough
	char* outPath;
	char* outName;
	int result;
	size_t curFile;
	UINT8 retVal;
	UINT8 hdrBuf[0x11];
	
	hFile = fopen(fileListName, "rt");
	if (hFile == NULL)
	{
		printf("Error opening %s!\n", fileListName);
		return 3;
	}
	
	// read file list
	fileList.alloc = 0;
	fileList.count = 0;
	fileList.items = NULL;
	lineID = 0;
	while(! feof(hFile))
	{
		FILE_ITEM* fi;
		char* strPtr;
		char* colPtrs[2];
		size_t colCnt;
		
		strPtr = fgets(lineStr, 0x1000, hFile);
		if (strPtr == NULL)
			break;
		lineID ++;
		RemoveControlChars(lineStr);
		if (strlen(lineStr) == 0 || lineStr[0] == '#')
			continue;
		
		colCnt = GetColumns(lineStr, 2, colPtrs, "\t");
		if (colCnt < 1)
			continue;
		
		fi = AddFileListItem(&fileList);
		fi->fileName = strdup(lineStr);
	}
	
	fclose(hFile);
	
	printf("Packing %u %s ...\n", fileList.count, (fileList.count == 1) ? "file" : "files");
	// create buffer for file path of files to be read
	{
		const char* outFTitle = GetFileTitle(fileListName);
		size_t outPLen = outFTitle - fileListName;
		size_t maxFNLen = 0;
		
		// determining maximum file name length first
		for (curFile = 0; curFile < fileList.count; curFile ++)
		{
			FILE_ITEM* fi = &fileList.items[curFile];
			size_t fnLen = strlen(fi->fileName);
			if (maxFNLen < fnLen)
				maxFNLen = fnLen;
		}
		
		// then create buffer for "folder + file name"
		outPath = (char*)malloc(outPLen + maxFNLen + 1);
		strncpy(outPath, fileListName, outPLen);
		outName = outPath + outPLen;
		*outName = '\0';
	}
	
	hFile = fopen(arcFileName, "wb");
	if (hFile == NULL)
	{
		printf("Error writing %s!\n", arcFileName);
		FreeFileList(&fileList);
		return 2;
	}
	
	memset(hdrBuf, 0x00, sizeof(hdrBuf));
	
	// The header buffer entry is NOT properly cleared by Sierra's tool,
	// causing garbage data after the file name terminator.
	// We are replicating the behaviour in TIM archives faithfully by:
	// 1. initializing with the archive's file name
	// 2. keeping the previous file name in the buffer, just writing the new one over
	// ("The Adventures of Willy Beamish" have different garbage.)
	CopyDOSFileName((char*)&hdrBuf[0x00], GetFileTitle(arcFileName));
	
	result = 0;
	// copy file data into archive
	for (curFile = 0; curFile < fileList.count; curFile ++)
	{
		FILE_ITEM* fi = &fileList.items[curFile];
		UINT32 dataSize = 0;
		UINT8* data = NULL;
		
		strcpy(outName, fi->fileName);
		fi->size = GetFileSize(outPath);
		
		CopyDOSFileName((char*)&hdrBuf[0x00], GetFileTitle(fi->fileName));
		WriteLE32(&hdrBuf[0x0D], fi->size);
		
		printf("Writing file %u/%u (%s) ...\n", 1 + curFile, fileList.count, fi->fileName);
		retVal = ReadFileData(outPath, &dataSize, &data);
		if (retVal)
		{
			printf("Unable to read %s!\n", outPath);
			result = 4;
			continue;
		}
		
		fwrite(hdrBuf, 0x11, 1, hFile);
		fwrite(data, 1, dataSize, hFile);
		
		free(data);
	}
	
	fclose(hFile);
	FreeFileList(&fileList);
	
	printf("Done.\n");
	return result;
}


static void CalcFNameHash(UINT8 buffer[4], const char* fileName)
{
	char fnBuf[13];
	size_t pos;
	INT16 fnSum;
	INT16 fnXor;
	INT16 fnFactor;
	UINT32 hashData;
	
	// TIM was confirmed to only take the actual file title into account.
	// (State gets reset on ':' and '\'.)
	fileName = GetFileTitle(fileName);
	
	strncpy(fnBuf, fileName, 13);	// copy + zero-fill
	
	fnSum = 0;
	fnXor = 0;
	for (pos = 0; fnBuf[pos] != '\0'; pos ++)
	{
		char c = fnBuf[pos];
		if (c >= 'a' && c <= 'z')
		{
			fnBuf[pos] ^= 0x20;	// enforce uppercase characters
			c = fnBuf[pos];
		}
		fnSum += (UINT8)c;
		fnXor ^= (UINT8)c;
	}
	fnFactor = fnSum * fnXor;	// result is 16-bit signed
	
	hashData = 0;
	for (pos = 0; pos < 4; pos ++)
	{
		hashData <<= 8;
		hashData += fnBuf[hashIndices[pos]];
	}
	hashData += fnFactor;	// [uint32] += [int16]
	
	WriteLE32(buffer, hashData);
	return;
}

static int CreateMapFile(const char* mapFileName, size_t arcCnt, const char* arcList[])
{
	FILE_LIST fileList;
	FILE* hFile;
	int result;
	size_t curArc;
	UINT8 retVal;
	size_t arcSize;
	UINT8* arcData;
	UINT8 hdrBuf[0x0F];
	
	if (! hashIdxOverrd)
	{
		UINT8 readIdx = 0;
		
		hFile = fopen(mapFileName, "rt");
		if (hFile != NULL)
		{
			UINT8 indexBuf[4];
			UINT8 idx;
			size_t readEl = fread(indexBuf, 1, 4, hFile);
			fclose(hFile);
			
			if (readEl < 4)
			{
				printf("Failed to read existing hash indices.\n");
			}
			else
			{
				readIdx = 1;
				for (idx = 0; idx < 4; idx ++)
				{
					if (indexBuf[idx] >= 12)
					{
						printf("Hash byte index[%u] == %u -> invalid\n", idx, indexBuf[idx]);
						readIdx = 0;
					}
				}
				if (readIdx)
				{
					printf("Using hash indices from existing file.\n");
					memcpy(hashIndices, indexBuf, 4);
				}
			}
		}
		if (! readIdx)
			printf("Falling back to default hash indices.\n");
	}
	printf("Using hash byte indices %u,%u,%u,%u.\n", hashIndices[0], hashIndices[1], hashIndices[2], hashIndices[3]);
	
	hFile = fopen(mapFileName, "wb");
	if (hFile == NULL)
	{
		printf("Error writing %s!\n", mapFileName);
		return 2;
	}
	
	printf("Indexing %u %s ...\n", arcCnt, (arcCnt == 1) ? "archive" : "archives");
	
	memcpy(&hdrBuf[0x00], hashIndices, 4);
	WriteLE16(&hdrBuf[0x04], (UINT16)arcCnt);
	fwrite(hdrBuf, 0x06, 1, hFile);
	
	// replicate the lack of buffer clearing here as well (see CreateArchive)
	memset(hdrBuf, 0x00, sizeof(hdrBuf));
	CopyDOSFileName((char*)&hdrBuf[0x00], GetFileTitle(mapFileName));
	
	fileList.alloc = 0;
	fileList.items = NULL;
	result = 0;
	for (curArc = 0; curArc < arcCnt; curArc ++)
	{
		const char* arcTitle = GetFileTitle(arcList[curArc]);
		UINT32 curPos;
		FILE_ITEM* fi;
		size_t curFile;
		size_t mapLen;
		UINT8* mapData;
		
		arcSize = 0;
		arcData = NULL;
		retVal = ReadFileData(arcList[curArc], &arcSize, &arcData);
		if (retVal)
		{
			if (retVal == 0xFF)
				printf("Error opening %s!\n", arcList[curArc]);
			else
				printf("Unable to fully read %s!\n", arcList[curArc]);
			result = 4;
			continue;
		}
		
		// enumerate files
		fileList.count = 0;
		for (curPos = 0x00; curPos < arcSize; )
		{
			if (curPos + 0x11 > arcSize)
				break;
			fi = AddFileListItem(&fileList);
			fi->fileName = (char*)&arcData[curPos + 0x00];
			fi->filePos = curPos;
			fi->size = ReadLE32(&arcData[curPos + 0x0D]);
			curPos += 0x11 + fi->size;
		}
		
		printf("Writing index for %s (%u %s) ...\n",
			arcTitle, fileList.count, (fileList.count == 1) ? "file" : "files");
		CopyDOSFileName((char*)&hdrBuf[0x00], arcTitle);
		WriteLE16(&hdrBuf[0x0D], (UINT16)fileList.count);
		
		mapLen = fileList.count * 8;
		mapData = (UINT8*)malloc(mapLen);
		curPos = 0x00;
		for (curFile = 0; curFile < fileList.count; curFile ++, curPos += 0x08)
		{
			fi = &fileList.items[curFile];
			CalcFNameHash(&mapData[curPos + 0x00], fi->fileName);
			WriteLE32(&mapData[curPos + 0x04], fi->filePos);
		}
		
		fwrite(hdrBuf, 1, 0x0F, hFile);
		fwrite(mapData, 1, mapLen, hFile);
		
		free(mapData);
		free(arcData);
	}
	fileList.count = 0;	// prevent trying to free file name strings (those were only references)
	
	fclose(hFile);
	FreeFileList(&fileList);
	
	printf("Done.\n");
	return result;
}


static UINT16 ReadLE16(const UINT8* data)
{
	return	(data[0x00] << 0) | (data[0x01] << 8);
}

static UINT32 ReadLE32(const UINT8* data)
{
	return	(data[0x00] <<  0) | (data[0x01] <<  8) |
			(data[0x02] << 16) | (data[0x03] << 24);
}

static void WriteLE16(UINT8* buffer, UINT16 value)
{
	buffer[0x00] = (value >> 0) & 0xFF;
	buffer[0x01] = (value >> 8) & 0xFF;
	return;
}

static void WriteLE32(UINT8* buffer, UINT32 value)
{
	buffer[0x00] = (value >>  0) & 0xFF;
	buffer[0x01] = (value >>  8) & 0xFF;
	buffer[0x02] = (value >> 16) & 0xFF;
	buffer[0x03] = (value >> 24) & 0xFF;
	return;
}
