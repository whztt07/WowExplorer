/*****************************************************************************/
/* SListFile.cpp                          Copyright (c) Ladislav Zezula 2004 */
/*---------------------------------------------------------------------------*/
/* Description:                                                              */
/*---------------------------------------------------------------------------*/
/*   Date    Ver   Who  Comment                                              */
/* --------  ----  ---  -------                                              */
/* 12.06.04  1.00  Lad  The first version of SListFile.cpp                   */
/*****************************************************************************/

#define __STORMLIB_SELF__
#include "StormLib.h"
#include "StormCommon.h"
#include <assert.h>

//-----------------------------------------------------------------------------
// Listfile entry structure

#define CACHE_BUFFER_SIZE  0x1000       // Size of the cache buffer

struct TListFileCache
{
    HANDLE  hFile;                      // Stormlib file handle
    char  * szMask;                     // Self-relative pointer to file mask
    DWORD   dwFileSize;                 // Total size of the cached file
    DWORD   dwFilePos;                  // Position of the cache in the file
    BYTE  * pBegin;                     // The begin of the listfile cache
    BYTE  * pPos;
    BYTE  * pEnd;                       // The last character in the file cache

    BYTE Buffer[CACHE_BUFFER_SIZE];
//  char MaskBuff[1]                    // Followed by the name mask (if any)
};

//-----------------------------------------------------------------------------
// Local functions (cache)

static bool FreeListFileCache(TListFileCache * pCache)
{
    // Valid parameter check
    if(pCache == NULL)
        return false;

    //
    // Note: don't close the pCache->hFile handle.
    // This has to be done by the creator of the listfile cache
    //

    // Free all allocated buffers
    if(pCache->szMask != NULL)
        STORM_FREE(pCache->szMask);
    STORM_FREE(pCache);
    return true;
}

static TListFileCache * CreateListFileCache(HANDLE hListFile, const char * szMask)
{
    TListFileCache * pCache = NULL;
    size_t nMaskLength = 0;
    DWORD dwBytesRead = 0;
    DWORD dwFileSize;

    // Get the amount of bytes that need to be allocated
    dwFileSize = SFileGetFileSize(hListFile, NULL);
    if(dwFileSize == 0)
        return NULL;

    // Append buffer for name mask, if any
    if(szMask != NULL)
        nMaskLength = strlen(szMask) + 1;

    // Allocate cache for one file block
    pCache = (TListFileCache *)STORM_ALLOC(BYTE, sizeof(TListFileCache) + nMaskLength);
    if(pCache != NULL)
    {
        // Clear the entire structure
        memset(pCache, 0, sizeof(TListFileCache) + nMaskLength);

        // Shall we copy the mask?
        if(szMask != NULL)
        {
            pCache->szMask = (char *)(pCache + 1);
            memcpy(pCache->szMask, szMask, nMaskLength);
        }

        // Load the file cache from the file
        SFileReadFile(hListFile, pCache->Buffer, CACHE_BUFFER_SIZE, &dwBytesRead, NULL);
        if(dwBytesRead != 0)
        {
            // Allocate pointers
            pCache->pBegin = pCache->pPos = &pCache->Buffer[0];
            pCache->pEnd   = pCache->pBegin + dwBytesRead;
            pCache->dwFileSize = dwFileSize;
            pCache->hFile  = hListFile;
        }
        else
        {
            FreeListFileCache(pCache);
            pCache = NULL;
        }
    }

    // Return the cache
    return pCache;
}

// Reloads the cache. Returns number of characters
// that has been loaded into the cache.
static DWORD ReloadListFileCache(TListFileCache * pCache)
{
    DWORD dwBytesToRead;
    DWORD dwBytesRead = 0;

    // Only do something if the cache is empty
    if(pCache->pPos >= pCache->pEnd)
    {
        // Move the file position forward
        pCache->dwFilePos += CACHE_BUFFER_SIZE;
        if(pCache->dwFilePos >= pCache->dwFileSize)
            return 0;

        // Get the number of bytes remaining
        dwBytesToRead = pCache->dwFileSize - pCache->dwFilePos;
        if(dwBytesToRead > CACHE_BUFFER_SIZE)
            dwBytesToRead = CACHE_BUFFER_SIZE;

        // Load the next data chunk to the cache
        SFileSetFilePointer(pCache->hFile, pCache->dwFilePos, NULL, FILE_BEGIN);
        SFileReadFile(pCache->hFile, pCache->Buffer, CACHE_BUFFER_SIZE, &dwBytesRead, NULL);

        // If we didn't read anything, it might mean that the block
        // of the file is not available (in case of partial MPQs).
        // We stop reading the file at this point, because the rest
        // of the listfile is unreliable
        if(dwBytesRead == 0)
            return 0;

        // Set the buffer pointers
        pCache->pBegin =
        pCache->pPos = &pCache->Buffer[0];
        pCache->pEnd = pCache->pBegin + dwBytesRead;
    }

    return dwBytesRead;
}

static size_t ReadListFileLine(TListFileCache * pCache, char * szLine, int nMaxChars)
{
    char * szLineBegin = szLine;
    char * szLineEnd = szLine + nMaxChars - 1;
    char * szExtraString = NULL;
    
    // Skip newlines, spaces, tabs and another non-printable stuff
    for(;;)
    {
        // If we need to reload the cache, do it
        if(pCache->pPos == pCache->pEnd)
        {
            if(ReloadListFileCache(pCache) == 0)
                break;
        }

        // If we found a non-whitespace character, stop
        if(*pCache->pPos > 0x20)
            break;

        // Skip the character
        pCache->pPos++;
    }

    // Copy the remaining characters
    while(szLine < szLineEnd)
    {
        // If we need to reload the cache, do it now and resume copying
        if(pCache->pPos == pCache->pEnd)
        {
            if(ReloadListFileCache(pCache) == 0)
                break;
        }

        // If we have found a newline, stop loading
        if(*pCache->pPos == 0x0D || *pCache->pPos == 0x0A)
            break;

        // Blizzard listfiles can also contain information about patch:
        // Pass1\Files\MacOS\unconditional\user\Background Downloader.app\Contents\Info.plist~Patch(Data#frFR#base-frFR,1326)
        if(*pCache->pPos == '~')
            szExtraString = szLine;

        // Copy the character
        *szLine++ = *pCache->pPos++;
    }

    // Terminate line with zero
    *szLine = 0;

    // If there was extra string after the file name, clear it
    if(szExtraString != NULL)
    {
        if(szExtraString[0] == '~' && szExtraString[1] == 'P')
        {
            szLine = szExtraString;
            *szExtraString = 0;
        }
    }

    // Return the length of the line
    return (szLine - szLineBegin);
}

static int CompareFileNodes(const void * p1, const void * p2) 
{
    char * szFileName1 = *(char **)p1;
    char * szFileName2 = *(char **)p2;

    return _stricmp(szFileName1, szFileName2);
}

static int WriteListFileLine(
    TMPQFile * hf,
    const char * szLine)
{
    char szNewLine[2] = {0x0D, 0x0A};
    size_t nLength = strlen(szLine);
    int nError;

    nError = SFileAddFile_Write(hf, szLine, (DWORD)nLength, MPQ_COMPRESSION_ZLIB);
    if(nError != ERROR_SUCCESS)
        return nError;

    return SFileAddFile_Write(hf, szNewLine, sizeof(szNewLine), MPQ_COMPRESSION_ZLIB);
}

//-----------------------------------------------------------------------------
// Local functions (listfile nodes)

// Adds a name into the list of all names. For each locale in the MPQ,
// one entry will be created
// If the file name is already there, does nothing.
static int SListFileCreateNodeForAllLocales(TMPQArchive * ha, const char * szFileName)
{
    TMPQHeader * pHeader = ha->pHeader;
    TFileEntry * pFileEntry;
    TMPQHash * pFirstHash;
    TMPQHash * pHash;
    bool bNameEntryCreated = false;

    // If we have HET table, use that one
    if(ha->pHetTable != NULL)
    {
        pFileEntry = GetFileEntryAny(ha, szFileName);
        if(pFileEntry != NULL)
        {
            // Allocate file name for the file entry
            AllocateFileName(pFileEntry, szFileName);
            bNameEntryCreated = true;
        }

        return ERROR_SUCCESS;
    }

    // If we have hash table, we use it
    if(bNameEntryCreated == false && ha->pHashTable != NULL)
    {
        // Look for the first hash table entry for the file
        pFirstHash = pHash = GetFirstHashEntry(ha, szFileName);

        // Go while we found something
        while(pHash != NULL)
        {
            // Is it a valid file table index ?
            if(pHash->dwBlockIndex < pHeader->dwBlockTableSize)
            {
                // Allocate file name for the file entry
                AllocateFileName(ha->pFileTable + pHash->dwBlockIndex, szFileName);
                bNameEntryCreated = true;
            }

            // Now find the next language version of the file
            pHash = GetNextHashEntry(ha, pFirstHash, pHash);
        }
    }

    return ERROR_CAN_NOT_COMPLETE;
}

// Saves the whole listfile into the MPQ.
int SListFileSaveToMpq(TMPQArchive * ha)
{
    TFileEntry * pFileTableEnd = ha->pFileTable + ha->dwFileTableSize;
    TFileEntry * pFileEntry;
    TMPQFile * hf = NULL;
    char * szPrevItem;
    char ** SortTable = NULL;
    DWORD dwFileSize = 0;
    size_t nFileNodes = 0;
    size_t i;
    int nError = ERROR_SUCCESS;

    // Allocate the table for sorting listfile
    SortTable = STORM_TEMP_ALLOC(char*, ha->dwFileTableSize);
    if(SortTable == NULL)
        return ERROR_NOT_ENOUGH_MEMORY;

    // Construct the sort table
    // Note: in MPQs with multiple locale versions of the same file,
    // this code causes adding multiple listfile entries.
    // Since those MPQs were last time used in Starcraft,
    // we leave it as it is.
    for(pFileEntry = ha->pFileTable; pFileEntry < pFileTableEnd; pFileEntry++)
    {
        // Only take existing items
        if((pFileEntry->dwFlags & MPQ_FILE_EXISTS) && pFileEntry->szFileName != NULL)
        {
            // Ignore pseudo-names
            if(!IsPseudoFileName(pFileEntry->szFileName, NULL) && !IsInternalMpqFileName(pFileEntry->szFileName))
            {
                SortTable[nFileNodes++] = pFileEntry->szFileName;
            }
        }
    }

    // Sort the table
    qsort(SortTable, nFileNodes, sizeof(char *), CompareFileNodes);

    // Now parse the table of file names again - remove duplicates
    // and count file size.
    if(nFileNodes != 0)
    {
        // Count the 0-th item
        dwFileSize += (DWORD)strlen(SortTable[0]) + 2;
        szPrevItem = SortTable[0];
        
        // Count all next items
        for(i = 1; i < nFileNodes; i++)
        {
            // If the item is the same like the last one, skip it
            if(_stricmp(SortTable[i], szPrevItem))
            {
                dwFileSize += (DWORD)strlen(SortTable[i]) + 2;
                szPrevItem = SortTable[i];
            }
        }

        // Determine the flags for (listfile)
        if(ha->dwFileFlags1 == 0)
            ha->dwFileFlags1 = GetDefaultSpecialFileFlags(ha, dwFileSize);

        // Create the listfile in the MPQ
        nError = SFileAddFile_Init(ha, LISTFILE_NAME,
                                       0,
                                       dwFileSize,
                                       LANG_NEUTRAL,
                                       ha->dwFileFlags1 | MPQ_FILE_REPLACEEXISTING,
                                      &hf);
        // Add all file names
        if(nError == ERROR_SUCCESS)
        {
            // Each name is followed by newline ("\x0D\x0A")
            szPrevItem = SortTable[0];
            nError = WriteListFileLine(hf, SortTable[0]);

            // Count all next items
            for(i = 1; i < nFileNodes; i++)
            {
                // If the item is the same like the last one, skip it
                if(_stricmp(SortTable[i], szPrevItem))
                {
                    WriteListFileLine(hf, SortTable[i]);
                    szPrevItem = SortTable[i];
                }
            }
        }
    }
    else
    {
        // Create the listfile in the MPQ
        dwFileSize = (DWORD)strlen(LISTFILE_NAME) + 2;
        nError = SFileAddFile_Init(ha, LISTFILE_NAME,
                                       0,
                                       dwFileSize,
                                       LANG_NEUTRAL,
                                       MPQ_FILE_ENCRYPTED | MPQ_FILE_COMPRESS | MPQ_FILE_REPLACEEXISTING,
                                      &hf);

        // Just add "(listfile)" there
        if(nError == ERROR_SUCCESS)
        {
            WriteListFileLine(hf, LISTFILE_NAME);
        }
    }

    // Finalize the file in the MPQ
    if(hf != NULL)
    {
        SFileAddFile_Finish(hf);
    }
    
    // Free buffers
    if(nError == ERROR_SUCCESS)
        ha->dwFlags &= ~MPQ_FLAG_INV_LISTFILE;
    if(SortTable != NULL)
        STORM_TEMP_FREE(SortTable);
    return nError;
}

static int SFileAddArbitraryListFile(
    TMPQArchive * ha,
    HANDLE hListFile)
{
    TListFileCache * pCache = NULL;
    size_t nLength;
    char szFileName[MAX_PATH];

    // Create the listfile cache for that file
    pCache = CreateListFileCache(hListFile, NULL);
    if(pCache != NULL)
    {
        // Load the node list. Add the node for every locale in the archive
        while((nLength = ReadListFileLine(pCache, szFileName, sizeof(szFileName))) > 0)
            SListFileCreateNodeForAllLocales(ha, szFileName);

        // Delete the cache
        FreeListFileCache(pCache);
    }
    
    return (pCache != NULL) ? ERROR_SUCCESS : ERROR_FILE_CORRUPT;
}

static int SFileAddExternalListFile(
    TMPQArchive * ha,
    HANDLE hMpq,
    const char * szListFile)
{
    HANDLE hListFile;
    int nError = ERROR_SUCCESS;

    // Open the external list file
    if(SFileOpenFileEx(hMpq, szListFile, SFILE_OPEN_LOCAL_FILE, &hListFile))
    {
        // Add the data from the listfile to MPQ
        nError = SFileAddArbitraryListFile(ha, hListFile);
        SFileCloseFile(hListFile);
    }
    return nError;
}

static int SFileAddInternalListFile(
    TMPQArchive * ha,
    HANDLE hMpq)
{
    TMPQArchive * haMpq = (TMPQArchive *)hMpq;
    TMPQHash * pFirstHash;
    TMPQHash * pHash;
    HANDLE hListFile;
    LCID lcSaveLocale = lcFileLocale;
    int nError = ERROR_SUCCESS;

    // If there is hash table, we need to support multiple listfiles
    // with different locales (BrooDat.mpq)
    if(haMpq->pHashTable != NULL)
    {
        pFirstHash = pHash = GetFirstHashEntry(haMpq, LISTFILE_NAME);
        while(nError == ERROR_SUCCESS && pHash != NULL)
        {
            // Set the prefered locale to that from list file
            SFileSetLocale(pHash->lcLocale);
            if(SFileOpenFileEx(hMpq, LISTFILE_NAME, 0, &hListFile))
            {
                // Add the data from the listfile to MPQ
                nError = SFileAddArbitraryListFile(ha, hListFile);
                SFileCloseFile(hListFile);
            }
            
            // Restore the original locale
            SFileSetLocale(lcSaveLocale);

            // Move to the next hash
            pHash = GetNextHashEntry(haMpq, pFirstHash, pHash);
        }
    }
    else
    {
        // Open the external list file
        if(SFileOpenFileEx(hMpq, LISTFILE_NAME, 0, &hListFile))
        {
            // Add the data from the listfile to MPQ
            // The function also closes the listfile handle
            nError = SFileAddArbitraryListFile(ha, hListFile);
            SFileCloseFile(hListFile);
        }
    }

    // Return the result of the operation
    return nError;
}

//-----------------------------------------------------------------------------
// File functions

// Adds a listfile into the MPQ archive.
int WINAPI SFileAddListFile(HANDLE hMpq, const char * szListFile)
{
    TMPQArchive * ha = (TMPQArchive *)hMpq;
    int nError = ERROR_SUCCESS;

    // Add the listfile for each MPQ in the patch chain
	TMPQArchive* halist[MAX_PATCH_NUM + 1] = {0};
	halist[0] = ha;
	if (ha != NULL)
	{
		DWORD nCount = 1;
		for (DWORD i = 0; i < ha->dwPatchCount; ++i)
		{
			halist[1+i] = ha->haPatchList[i];
			++nCount;
		}

		for (DWORD i = 0; i < nCount; ++i)
		{
			ha = halist[i];

			if(szListFile != NULL)
				SFileAddExternalListFile(ha, hMpq, szListFile);
			else
				SFileAddInternalListFile(ha, hMpq);

			// Also, add three special files to the listfile:
			// (listfile) itself, (attributes) and (signature)
			SListFileCreateNodeForAllLocales(ha, LISTFILE_NAME);
			SListFileCreateNodeForAllLocales(ha, SIGNATURE_NAME);
			SListFileCreateNodeForAllLocales(ha, ATTRIBUTES_NAME);
		}
	}

    return nError;
}

//-----------------------------------------------------------------------------
// Enumerating files in listfile

HANDLE WINAPI SListFileFindFirstFile(HANDLE hMpq, const char * szListFile, const char * szMask, SFILE_FIND_DATA * lpFindFileData)
{
    TListFileCache * pCache = NULL;
    HANDLE hListFile = NULL;
    size_t nLength = 0;
    DWORD dwSearchScope = SFILE_OPEN_LOCAL_FILE;
    int nError = ERROR_SUCCESS;

    // Initialize the structure with zeros
    memset(lpFindFileData, 0, sizeof(SFILE_FIND_DATA));

    // If the szListFile is NULL, it means we have to open internal listfile
    if(szListFile == NULL)
    {
        // Use SFILE_OPEN_ANY_LOCALE for listfile. This will allow us to load
        // the listfile even if there is only non-neutral version of the listfile in the MPQ
        dwSearchScope = SFILE_OPEN_ANY_LOCALE;
        szListFile = LISTFILE_NAME;
    }

    // Open the local/internal listfile
    if(!SFileOpenFileEx(hMpq, szListFile, dwSearchScope, &hListFile))
        nError = GetLastError();

    // Load the listfile to cache
    if(nError == ERROR_SUCCESS)
    {
        pCache = CreateListFileCache(hListFile, szMask);
        if(pCache == NULL)
            nError = ERROR_FILE_CORRUPT;
    }

    // Perform file search
    if(nError == ERROR_SUCCESS)
    {
        for(;;)
        {
            // Read the (next) line
            nLength = ReadListFileLine(pCache, lpFindFileData->cFileName, sizeof(lpFindFileData->cFileName));
            if(nLength == 0)
            {
                nError = ERROR_NO_MORE_FILES;
                break;
            }

            // If some mask entered, check it
            if(CheckWildCard(lpFindFileData->cFileName, pCache->szMask))
                break;                
        }
    }

    // Cleanup & exit
    if(nError != ERROR_SUCCESS)
    {
        memset(lpFindFileData, 0, sizeof(SFILE_FIND_DATA));
        SetLastError(nError);
    }

    if(pCache != NULL)
        FreeListFileCache(pCache);
    if(hListFile != NULL)
        SFileCloseFile(hListFile);
    return (HANDLE)pCache;
}

bool WINAPI SListFileFindNextFile(HANDLE hFind, SFILE_FIND_DATA * lpFindFileData)
{
    TListFileCache * pCache = (TListFileCache *)hFind;
    size_t nLength;
    bool bResult = false;
    int nError = ERROR_SUCCESS;

    for(;;)
    {
        // Read the (next) line
        nLength = ReadListFileLine(pCache, lpFindFileData->cFileName, sizeof(lpFindFileData->cFileName));
        if(nLength == 0)
        {
            nError = ERROR_NO_MORE_FILES;
            break;
        }

        // If some mask entered, check it
        if(CheckWildCard(lpFindFileData->cFileName, pCache->szMask))
        {
            bResult = true;
            break;
        }
    }

    if(nError != ERROR_SUCCESS)
        SetLastError(nError);
    return bResult;
}

bool WINAPI SListFileFindClose(HANDLE hFind)
{
    return FreeListFileCache((TListFileCache *)hFind);
}

