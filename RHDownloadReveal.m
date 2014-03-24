//
//  RHDownloadReveal.m
//  RHDownloadReveal
//
//  Created by Richard Heard on 21/03/2014.
//  Copyright (c) 2014 Richard Heard. All rights reserved.
//

#include <unistd.h>
#include <sys/stat.h>

#include "common.h"
#include <partial/partial.h>
char endianness = IS_LITTLE_ENDIAN;

#ifdef __OBJC__
#import <Foundation/Foundation.h>
#endif

//download libReveal using partialzip

NSString *downloadURL = @"http://download.revealapp.com/Reveal.app.zip";
NSString *zipPath = @"Reveal.app/Contents/SharedSupport/iOS-Libraries/libReveal.dylib";

NSString *folder = @"/Library/RHRevealLoader";
NSString *filename = @"libReveal.dylib";

struct partialFile {
    unsigned char *pos;
    size_t fileSize;
    size_t downloadedBytes;
};


size_t data_callback(ZipInfo* info, CDFile* file, unsigned char *buffer, size_t size, void *userInfo) {
    struct partialFile *pfile = (struct partialFile *)userInfo;
	memcpy(pfile->pos, buffer, size);
	pfile->pos += size;
    pfile->downloadedBytes += size;
    
    printf("Downloaded %zu/%zu bytes. %g%% complete.\n", pfile->downloadedBytes, pfile->fileSize, ((float)pfile->downloadedBytes/(float)pfile->fileSize) * 100.f);
    
    return size;
}

int main(int argc, const char *argv[], const char *envp[]){
    NSString *libraryPath = [folder stringByAppendingPathComponent:filename];
    
    if (argc > 1 && strcmp(argv[1], "upgrade") != 0) {
        printf("CYDIA upgrade, nuking existing %s\n", [libraryPath UTF8String]);
        [[NSFileManager defaultManager] removeItemAtPath:libraryPath error:nil];
    }
    
    if (![[NSFileManager defaultManager] fileExistsAtPath:libraryPath]) {
        //download libReveal.dylib
        
        
        ZipInfo* info = PartialZipInit([downloadURL UTF8String]);
        if(!info) {
            printf("Cannot find %s\n", [downloadURL UTF8String]);
            return 0;
        }
        
        CDFile *file = PartialZipFindFile(info, [zipPath UTF8String]);
        if(!file) {
            printf("Cannot find %s in %s\n", [zipPath UTF8String], [downloadURL UTF8String]);
            return 0;
        }
        
        int dataLen = file->size;

        unsigned char *data = malloc(dataLen+1);
        struct partialFile pfile = (struct partialFile){data, dataLen, 0};
        
        PartialZipGetFile(info, file, data_callback, &pfile);
        *(pfile.pos) = '\0';
        
        PartialZipRelease(info);
        
        NSData *dylibData = [NSData dataWithBytes:data length:dataLen];
        
        if (![[NSFileManager defaultManager] createDirectoryAtPath:folder withIntermediateDirectories:YES attributes:nil error:nil]){
            printf("Failed to create folder %s\n", [folder UTF8String]);
            return 0;
        }

        if (![dylibData writeToFile:libraryPath atomically:YES]){
            printf("Failed to write file to path %s\n", [libraryPath UTF8String]);
            return 0;
        }
        
        free(data);
        printf("Successfully downloaded %s to path %s\n", [downloadURL UTF8String], [libraryPath UTF8String]);
    
    } else {
        printf("libReveal.dylib already exists at path %s\n", [libraryPath UTF8String]);
    }
    
	return 0;
}
