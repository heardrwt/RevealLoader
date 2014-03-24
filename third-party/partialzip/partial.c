#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <zlib.h>
#include <libgen.h>

#include "common.h"
#include "partial/partial.h"

static size_t dummyReceive(void* data, size_t size, size_t nmemb, void* info) {
	return size * nmemb;
}

static size_t receiveCentralDirectoryEnd(void* data, size_t size, size_t nmemb, ZipInfo* info) {
	memcpy(info->centralDirectoryEnd + info->centralDirectoryEndRecvd, data, size * nmemb);
	info->centralDirectoryEndRecvd += size * nmemb;
	return size * nmemb;
}

static size_t receiveCentralDirectory(void* data, size_t size, size_t nmemb, ZipInfo* info) {
	memcpy(info->centralDirectory + info->centralDirectoryRecvd, data, size * nmemb);
	info->centralDirectoryRecvd += size * nmemb;
	return size * nmemb;
}

static void flipFiles(ZipInfo* info)
{
	char* cur = info->centralDirectory;

	unsigned int i;
	for(i = 0; i < info->centralDirectoryDesc->CDEntries; i++)
	{
		CDFile* candidate = (CDFile*) cur;
		FLIPENDIANLE(candidate->signature);
		FLIPENDIANLE(candidate->version);
		FLIPENDIANLE(candidate->versionExtract);
		// FLIPENDIANLE(candidate->flags);
		FLIPENDIANLE(candidate->method);
		FLIPENDIANLE(candidate->modTime);
		FLIPENDIANLE(candidate->modDate);
		// FLIPENDIANLE(candidate->crc32);
		FLIPENDIANLE(candidate->compressedSize);
		FLIPENDIANLE(candidate->size);
		FLIPENDIANLE(candidate->lenFileName);
		FLIPENDIANLE(candidate->lenExtra);
		FLIPENDIANLE(candidate->lenComment);
		FLIPENDIANLE(candidate->diskStart);
		// FLIPENDIANLE(candidate->internalAttr);
		// FLIPENDIANLE(candidate->externalAttr);
		FLIPENDIANLE(candidate->offset);

		cur += sizeof(CDFile) + candidate->lenFileName + candidate->lenExtra + candidate->lenComment;
	}
}

ZipInfo* PartialZipInit(const char* url)
{
	ZipInfo* info = (ZipInfo*) malloc(sizeof(ZipInfo));
	info->url = strdup(url);
	info->centralDirectoryRecvd = 0;
	info->centralDirectoryEndRecvd = 0;
	info->centralDirectoryDesc = NULL;

	info->hCurl = curl_easy_init();

	curl_easy_setopt(info->hCurl, CURLOPT_URL, info->url);
	curl_easy_setopt(info->hCurl, CURLOPT_FOLLOWLOCATION, 1);
	curl_easy_setopt(info->hCurl, CURLOPT_NOBODY, 1);
	curl_easy_setopt(info->hCurl, CURLOPT_WRITEFUNCTION, dummyReceive);

	if(strncmp(info->url, "file://", 7) == 0)
	{
		char path[1024];
		strcpy(path, info->url + 7);
		char* filePath = (char*) curl_easy_unescape(info->hCurl, path, 0,  NULL);
		FILE* f = fopen(filePath, "rb");
		if(!f)
		{
			curl_free(filePath);
			curl_easy_cleanup(info->hCurl);
			free(info->url);
			free(info);

			return NULL;
		}

		fseek(f, 0, SEEK_END);
		info->length = ftell(f);
		fclose(f);

		curl_free(filePath);
	}
	else
	{
		curl_easy_perform(info->hCurl);

		double dFileLength;
		curl_easy_getinfo(info->hCurl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &dFileLength);
		info->length = dFileLength;
	}

	char sRange[100];
	uint64_t start;

	if(info->length > (0xffff + sizeof(EndOfCD)))
		start = info->length - 0xffff - sizeof(EndOfCD);
	else
		start = 0;

	uint64_t end = info->length - 1;

	sprintf(sRange, "%" PRIu64 "-%" PRIu64, start, end);

	curl_easy_setopt(info->hCurl, CURLOPT_WRITEFUNCTION, receiveCentralDirectoryEnd);
	curl_easy_setopt(info->hCurl, CURLOPT_WRITEDATA, info);
	curl_easy_setopt(info->hCurl, CURLOPT_RANGE, sRange);
	curl_easy_setopt(info->hCurl, CURLOPT_HTTPGET, 1);

	curl_easy_perform(info->hCurl);

	char* cur;
	for(cur = info->centralDirectoryEnd; cur < (info->centralDirectoryEnd + (end - start - 1)); cur++)
	{
		EndOfCD* candidate = (EndOfCD*) cur;
		uint32_t signature = candidate->signature;
		FLIPENDIANLE(signature);
		if(signature == 0x06054b50)
		{
			uint16_t lenComment = candidate->lenComment;
			FLIPENDIANLE(lenComment);
			if((cur + lenComment + sizeof(EndOfCD)) == (info->centralDirectoryEnd + info->centralDirectoryEndRecvd))
			{
				FLIPENDIANLE(candidate->diskNo);
				FLIPENDIANLE(candidate->CDDiskNo);
				FLIPENDIANLE(candidate->CDDiskEntries);
				FLIPENDIANLE(candidate->CDEntries);
				FLIPENDIANLE(candidate->CDSize);
				FLIPENDIANLE(candidate->CDOffset);
				FLIPENDIANLE(candidate->lenComment);
				info->centralDirectoryDesc = candidate;
				break;
			}
		}

	}

	if(info->centralDirectoryDesc)
	{
		info->centralDirectory = malloc(info->centralDirectoryDesc->CDSize);
		start = info->centralDirectoryDesc->CDOffset;
		end = start + info->centralDirectoryDesc->CDSize - 1;
		sprintf(sRange, "%" PRIu64 "-%" PRIu64, start, end);
		curl_easy_setopt(info->hCurl, CURLOPT_WRITEFUNCTION, receiveCentralDirectory);
		curl_easy_setopt(info->hCurl, CURLOPT_WRITEDATA, info);
		curl_easy_setopt(info->hCurl, CURLOPT_RANGE, sRange);
		curl_easy_setopt(info->hCurl, CURLOPT_HTTPGET, 1);
		curl_easy_perform(info->hCurl);

		flipFiles(info);

		return info;
	}
	else 
	{
		curl_easy_cleanup(info->hCurl);
		free(info->url);
		free(info);
		return NULL;
	}
}

CDFile* PartialZipFindFile(ZipInfo* info, const char* fileName)
{
	char* cur = info->centralDirectory;
	unsigned int i;
	size_t fileNameLength = strlen(fileName);
	for(i = 0; i < info->centralDirectoryDesc->CDEntries; i++)
	{
		CDFile* candidate = (CDFile*) cur;
		const char* curFileName = cur + sizeof(CDFile);

		if(fileNameLength == candidate->lenFileName && strncmp(fileName, curFileName, candidate->lenFileName) == 0)
			return candidate;

		cur += sizeof(CDFile) + candidate->lenFileName + candidate->lenExtra + candidate->lenComment;
	}

	return NULL;
}

CDFile* PartialZipListFiles(ZipInfo* info)
{
	char* cur = info->centralDirectory;
	unsigned int i;
	for(i = 0; i < info->centralDirectoryDesc->CDEntries; i++)
	{
		CDFile* candidate = (CDFile*) cur;
		const char* curFileName = cur + sizeof(CDFile);
		char* myFileName = (char*) malloc(candidate->lenFileName + 1);
		memcpy(myFileName, curFileName, candidate->lenFileName);
		myFileName[candidate->lenFileName] = '\0';

		printf("%s: method: %d, compressed size: %d, size: %d\n", myFileName, candidate->method,
				candidate->compressedSize, candidate->size);

		free(myFileName);

		cur += sizeof(CDFile) + candidate->lenFileName + candidate->lenExtra + candidate->lenComment;
	}

	return NULL;
}

unsigned char* PartialZipCopyFileName(ZipInfo* info, CDFile* file)
{
	const unsigned char* curFileName = (const unsigned char*)file + sizeof(CDFile);
	unsigned char* result = malloc(file->lenFileName + 1);
	memcpy(result, curFileName, file->lenFileName);
	result[file->lenFileName] = '\0';
	return result;
}

typedef struct ReceiveBodyData* ReceiveDataBodyDataRef;

typedef struct ReceiveBodyFileData {
	CDFile *file;
	union {
		LocalFile localFile;
		char localFileBytes[sizeof(LocalFile)];
	};
	size_t rangeSize;
	size_t bytesLeftInLocalFile;
	size_t bytesLeftBeforeData;
	size_t bytesLeftInData;
	void (*startBodyCallback)(ReceiveDataBodyDataRef);
	size_t (*processBodyCallback)(unsigned char*, size_t, ReceiveDataBodyDataRef);
	void (*finishBodyCallback)(ReceiveDataBodyDataRef);
} ReceiveBodyFileData;

typedef enum {
	MultipartDecodeStateInHeader,
	MultipartDecodeStateFoundFirstCR,
	MultipartDecodeStateFoundFirstNL,
	MultipartDecodeStateFoundSecondCR,
	MultipartDecodeStateInBody,
} MultipartDecodeState;

typedef struct ReceiveBodyData {
	ZipInfo *info;
	PartialZipGetFileCallback callback;
	void *userInfo;
	ReceiveBodyFileData *currentFile;
	bool foundMultipartBoundary;
	MultipartDecodeState multipartDecodeState;
	size_t bytesRemainingInRange;
	z_stream stream; // Reused for each compressed stream
} ReceiveDataBodyData;

// Uncompressed File Data

static void startBodyUncompressed(ReceiveDataBodyDataRef bodyData)
{
}

static size_t receiveDataBodyUncompressed(unsigned char* data, size_t size, ReceiveDataBodyDataRef bodyData)
{
	return bodyData->callback(bodyData->info, bodyData->currentFile->file, data, size, bodyData->userInfo);
}

static void finishBodyUncompressed(ReceiveDataBodyDataRef bodyData)
{
}

// ZLIB Compressed File Data

#define ZLIB_BUFFER_SIZE 16384

static void startBodyZLIBCompressed(ReceiveDataBodyDataRef bodyData)
{
	bodyData->stream.zalloc = Z_NULL;
	bodyData->stream.zfree = Z_NULL;
	bodyData->stream.opaque = Z_NULL;
	bodyData->stream.avail_in = 0;
	bodyData->stream.next_in = NULL;
	inflateInit2(&bodyData->stream, -MAX_WBITS);
}

static size_t receiveDataBodyZLIBCompressed(unsigned char* data, size_t size, ReceiveDataBodyDataRef bodyData)
{
	ReceiveBodyFileData *fileData = bodyData->currentFile;
	bodyData->stream.next_in = data;
	bodyData->stream.avail_in = size;
	unsigned char buffer[ZLIB_BUFFER_SIZE];
	do {
		bodyData->stream.next_out = buffer;
		bodyData->stream.avail_out = ZLIB_BUFFER_SIZE;
		int err = inflate(&bodyData->stream, Z_NO_FLUSH);
		if (bodyData->stream.avail_out != ZLIB_BUFFER_SIZE) {
			size_t new_bytes = ZLIB_BUFFER_SIZE - bodyData->stream.avail_out;
			size_t bytes_read = bodyData->callback(bodyData->info, fileData->file, buffer, new_bytes, bodyData->userInfo);
			if (bytes_read != new_bytes) {
				// Abort if callback doesn't read all data
				return 0;
			}
		}
		switch (err) {
			case Z_BUF_ERROR:
			case Z_OK:
				continue;
			case Z_STREAM_END:
				return size;
			default:
				// Abort if there's some sort of zlib stream error
				return 0;
		}
	} while (bodyData->stream.avail_out == 0);
	return size;
}

static void finishBodyZLIBCompressed(ReceiveDataBodyDataRef bodyData)
{
	unsigned char buffer[ZLIB_BUFFER_SIZE];
	int err;
	do {
		bodyData->stream.next_out = buffer;
		bodyData->stream.avail_out = ZLIB_BUFFER_SIZE;
		err = inflate(&bodyData->stream, Z_SYNC_FLUSH);
		if (bodyData->stream.avail_out != ZLIB_BUFFER_SIZE) {
			size_t new_bytes = ZLIB_BUFFER_SIZE - bodyData->stream.avail_out;
			size_t bytes_read = bodyData->callback(bodyData->info, bodyData->currentFile->file, buffer, new_bytes, bodyData->userInfo);
			if (bytes_read != new_bytes) {
				// Abort if callback doesn't read all data
				break;
			}
		}
	} while (err == Z_OK);
	inflateEnd(&bodyData->stream);
}

// Single part responses (single range)

static size_t receiveDataBodySingle(unsigned char* data, size_t size, size_t nmemb, ReceiveDataBodyDataRef bodyData)
{
	size_t byteCount = size * nmemb;
	ReceiveBodyFileData *pFileData = bodyData->currentFile;
	// Read LocalFile header
	if (pFileData->bytesLeftInLocalFile) {
		if (byteCount < pFileData->bytesLeftInLocalFile) {
			memcpy(&pFileData->localFileBytes[sizeof(LocalFile) - pFileData->bytesLeftInLocalFile], data, byteCount);
			pFileData->bytesLeftInLocalFile -= byteCount;
			return size * nmemb;
		}
		memcpy(&pFileData->localFileBytes[sizeof(LocalFile) - pFileData->bytesLeftInLocalFile], data, pFileData->bytesLeftInLocalFile);
		data += pFileData->bytesLeftInLocalFile;
		byteCount -= pFileData->bytesLeftInLocalFile;
		pFileData->bytesLeftInLocalFile = 0;
		FLIPENDIANLE(pFileData->localFile.signature);
		FLIPENDIANLE(pFileData->localFile.versionExtract);
		// FLIPENDIANLE(pFileData->localFile.flags);
		FLIPENDIANLE(pFileData->localFile.method);
		FLIPENDIANLE(pFileData->localFile.modTime);
		FLIPENDIANLE(pFileData->localFile.modDate);
		// FLIPENDIANLE(pFileData->localFile.crc32);
		FLIPENDIANLE(pFileData->localFile.compressedSize);
		FLIPENDIANLE(pFileData->localFile.size);
		FLIPENDIANLE(pFileData->localFile.lenFileName);
		FLIPENDIANLE(pFileData->localFile.lenExtra);
		pFileData->bytesLeftBeforeData = pFileData->localFile.lenFileName + pFileData->localFile.lenExtra;
	}
	// Read file name and extra bytes before body
	if (pFileData->bytesLeftBeforeData) {
		if (byteCount < pFileData->bytesLeftBeforeData) {
			pFileData->bytesLeftBeforeData -= byteCount;
			return size * nmemb;
		}
		data += pFileData->bytesLeftBeforeData;
		byteCount -= pFileData->bytesLeftBeforeData;
		pFileData->bytesLeftBeforeData = 0;
		pFileData->startBodyCallback(bodyData);
	}
	// Read body
	if (pFileData->bytesLeftInData) {
		if (byteCount < pFileData->bytesLeftInData) {
			if (pFileData->processBodyCallback(data, byteCount, bodyData) != byteCount)
				return 0;
			pFileData->bytesLeftInData -= byteCount;
			return size * nmemb;
		}
		if (pFileData->processBodyCallback(data, pFileData->bytesLeftInData, bodyData) != pFileData->bytesLeftInData)
			return 0;
		pFileData->finishBodyCallback(bodyData);
		pFileData->bytesLeftInData = 0;
	}
	return size * nmemb;
}

// Multi-part responses

#define BOUNDARY_HEADER_PREFIX "Content-Type: multipart/byteranges; boundary="

static size_t receiveHeaderMulti(unsigned char* data, size_t size, size_t nmemb, ReceiveDataBodyDataRef bodyData)
{
	size_t byteCount = size * nmemb;
	if (byteCount > sizeof(BOUNDARY_HEADER_PREFIX)) {
		if (memcmp(data, BOUNDARY_HEADER_PREFIX, sizeof(BOUNDARY_HEADER_PREFIX)-1) == 0) {
			bodyData->foundMultipartBoundary = true;
		}
	}
	return byteCount;
}

static size_t receiveDataBodyMulti(unsigned char* data, size_t size, size_t nmemb, ReceiveDataBodyDataRef bodyData)
{
	// Check if server Returned a multipart/byteranges
	if (!bodyData->foundMultipartBoundary)
		return 0;
	size_t byteCount = size * nmemb;
	MultipartDecodeState state = bodyData->multipartDecodeState;
	do {
		switch (state) {
			case MultipartDecodeStateInHeader:
				if (*data == '\r')
					state = MultipartDecodeStateFoundFirstCR;
				byteCount--;
				data++;
				break;
			case MultipartDecodeStateFoundFirstCR:
				state = (*data == '\n') ? MultipartDecodeStateFoundFirstNL : MultipartDecodeStateInHeader;
				byteCount--;
				data++;
				break;
			case MultipartDecodeStateFoundFirstNL:
				state = (*data == '\r') ? MultipartDecodeStateFoundSecondCR : MultipartDecodeStateInHeader;
				byteCount--;
				data++;
				break;
			case MultipartDecodeStateFoundSecondCR:
				if (*data == '\n') {
					state = MultipartDecodeStateInBody;
					bodyData->bytesRemainingInRange = bodyData->currentFile->rangeSize;
				} else {
					state = MultipartDecodeStateInHeader;
				}
				byteCount--;
				data++;
				break;
			case MultipartDecodeStateInBody: {
				if (byteCount < bodyData->bytesRemainingInRange) {
					if (receiveDataBodySingle(data, 1, byteCount, bodyData) != byteCount)
						return 0;
					bodyData->bytesRemainingInRange -= byteCount;
					byteCount = 0;
				} else {
					if (receiveDataBodySingle(data, 1, bodyData->bytesRemainingInRange, bodyData) != bodyData->bytesRemainingInRange)
						return 0;
					byteCount -= bodyData->bytesRemainingInRange;
					data += bodyData->bytesRemainingInRange;
					bodyData->bytesRemainingInRange = 0;
					bodyData->currentFile++;
					state = MultipartDecodeStateInHeader;
				}
				break;
			}
		}
	} while(byteCount);
	bodyData->multipartDecodeState = state;
	return size * nmemb;
}


static uint64_t PartialZipFileMaximumEndpoint(ZipInfo* info, CDFile* file)
{
	char* cur = info->centralDirectory;
	size_t result = info->centralDirectoryDesc->CDOffset;
	size_t fileOffset = file->offset;
	unsigned int i;
	for(i = 0; i < info->centralDirectoryDesc->CDEntries; i++) {
		CDFile* candidate = (CDFile*) cur;
		size_t candidateOffset = candidate->offset;
		if ((candidateOffset > fileOffset) && (candidateOffset < result))
			result = candidateOffset;
		cur += sizeof(CDFile) + candidate->lenFileName + candidate->lenExtra + candidate->lenComment;
	}

	return result;
}

bool PartialZipGetFiles(ZipInfo* info, CDFile* files[], size_t count, PartialZipGetFileCallback callback, void *userInfo)
{
	if (!count)
		return true;

	ReceiveBodyFileData *fileData = malloc(sizeof(ReceiveBodyFileData) * count);
	curl_easy_setopt(info->hCurl, CURLOPT_HTTPGET, 1);

	// Apply Range string
	size_t i;
	size_t rangeLength = 0;
	for (i = 0; i < count; i++) {
		uint64_t start = files[i]->offset;
		uint64_t end = PartialZipFileMaximumEndpoint(info, files[i]);
		fileData[i].rangeSize = end - start;
		char temp[100];
		rangeLength += sprintf(temp, "%" PRIu64 "%" PRIu64, start, end) + 2;
	}
	char range[rangeLength+1];
	char *rangeCurrent = range;
	for (i = 0; i < count; i++) {
		uint64_t start = files[i]->offset;
		uint64_t end = start + fileData[i].rangeSize - 1;
		rangeCurrent += sprintf(rangeCurrent, "%" PRIu64 "-%" PRIu64 ",", start, end);
	}
	rangeCurrent[-1] = '\0';
	curl_easy_setopt(info->hCurl, CURLOPT_RANGE, range);
	
	// Build file data for each file
	for (i = 0; i < count; i++) {
		fileData[i].file = files[i];
		fileData[i].bytesLeftInLocalFile = sizeof(LocalFile);
		fileData[i].bytesLeftInData = files[i]->compressedSize;
		switch (files[i]->method) {
			case 8:
				fileData[i].startBodyCallback = startBodyZLIBCompressed;
				fileData[i].processBodyCallback = receiveDataBodyZLIBCompressed;
				fileData[i].finishBodyCallback = finishBodyZLIBCompressed;
				break;
			default:
				fileData[i].startBodyCallback = startBodyUncompressed;
				fileData[i].processBodyCallback = receiveDataBodyUncompressed;
				fileData[i].finishBodyCallback = finishBodyUncompressed;
				break;
		}
	}

	// Build body data
	ReceiveDataBodyData bodyData;
	bodyData.info = info;
	bodyData.callback = callback;
	bodyData.userInfo = userInfo;
	bodyData.currentFile = &fileData[0];
	
	// Make request
	curl_easy_setopt(info->hCurl, CURLOPT_WRITEDATA, &bodyData);
	int curl_result;
	if (count == 1) {
		curl_easy_setopt(info->hCurl, CURLOPT_WRITEFUNCTION, receiveDataBodySingle);
		curl_easy_setopt(info->hCurl, CURLOPT_HEADERDATA, NULL);
		curl_easy_setopt(info->hCurl, CURLOPT_HEADERFUNCTION, NULL);
		curl_result = curl_easy_perform(info->hCurl);
		free(fileData);
	} else {
		curl_easy_setopt(info->hCurl, CURLOPT_WRITEFUNCTION, receiveDataBodyMulti);
		curl_easy_setopt(info->hCurl, CURLOPT_HEADERDATA, &bodyData);
		curl_easy_setopt(info->hCurl, CURLOPT_HEADERFUNCTION, receiveHeaderMulti);
		bodyData.foundMultipartBoundary = false;
		bodyData.multipartDecodeState = MultipartDecodeStateInHeader;
		curl_result = curl_easy_perform(info->hCurl);
		free(fileData);
		if (!bodyData.foundMultipartBoundary) {
			// Multi-part failed, fallback to a single request per file
			for (i = 0; i < count; i++)
				if (!PartialZipGetFiles(info, &files[i], 1, callback, userInfo))
					return false;
			return true;
		}
	}

	return curl_result == 0;
}

bool PartialZipGetFile(ZipInfo* info, CDFile* file, PartialZipGetFileCallback callback, void *userInfo)
{
	CDFile *files[1] = { file };
	return PartialZipGetFiles(info, files, 1, callback, userInfo);
}

void PartialZipRelease(ZipInfo* info)
{
	curl_easy_cleanup(info->hCurl);
	free(info->centralDirectory);
	free(info->url);
	free(info);

	curl_global_cleanup();
}

