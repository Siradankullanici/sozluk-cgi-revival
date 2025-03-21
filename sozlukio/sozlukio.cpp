// sozlukio.cpp : Defines the exported functions for the DLL application.
//

#include "stdafx.h"
#include "sozlukio.h"
#include "RequestCollector.h"
#include "RequestBridge.h"
#include "helper.h"
#include "sapi.h"
#include <stdlib.h>
#include <stdio.h>

extern "C" {
    BOOL WINAPI Service()
    {
        return TRUE;
    }
}

#pragma comment(linker,"/export:Service=_Service@0")


#define SiopCreateWriteBuffer(initial) HlpCreateAutoBuffer(initial)
#define SiopPutIntoWriteBuffer(buf,data,len) HlpWriteIntoAutoBuffer((PAUTO_BUFFER)buf,data,len)
#define SiopDisposeWriteBuffer(buf) HlpDisposeAutoBuffer((PAUTO_BUFFER)buf)



typedef PAUTO_BUFFER PFILE_WRITE_BUFFER;


typedef enum __FILE_TYPE
{
    DictTxt,
    UserTxt,
    IndexTxt,
    ShowTxt,
    AddTxt,
    MaxTxt
}FILE_TYPE;

typedef enum __EXEC_NAME
{
    INDEX,
    ADD,
    VIEW
}EXEC_NAME;

typedef struct __FILE_IO_STATE
{
    HANDLE      realHandle;
    HANDLE      fakeHandle;
    ULONG       status;
    FILE_TYPE   type;
    EXEC_NAME   exeName;
    struct
    {
        LONG    low;
        LONG    high;
    }FilePointer;
    PVOID       tag;
}FILE_IO_STATE,*PFILE_IO_STATE;

#define FIOS_ACTIVE         0x80000000
#define FIOS_DELAY_DISPOSE  0x40000000
#define FIOS_KEEP_TAG       0x20000000

typedef struct __FILE_HANDLE_TABLE
{
    FILE_IO_STATE files[MaxTxt];
}FILE_HANDLE_TABLE, *PFILE_HANDLE_TABLE;

PCHAR SioCachedPostContent = NULL;
ULONG SioCachedPostContentLen = 0;

PCHAR SioCachedEnvContent = NULL;
ULONG SioCachedEnvContentLen = 0;

FILE_HANDLE_TABLE SioHandles = { 0 };

SOZLUK_REQUEST SioRequest = { 0 };

extern CHAR SioExeName[MAX_PATH];

PAUTO_BUFFER SiopSusersOfTheView = NULL;

extern void RciDestroyRequestObject(PSOZLUK_REQUEST request);
extern PVOID HkpAlloc(ULONG size);
extern void HkpFree(PVOID mem);

extern pfnCreateFileA _CreateFileA;
extern pfnReadFile _ReadFile;
extern pfnWriteFile _WriteFile;
extern pfnCloseHandle _CloseHandle;
extern pfnSetFilePointer _SetFilePointer;
extern pfnGetFileSize _GetFileSize;
extern pfnGetEnvironmentVariableA _GetEnvironmentVariableA;


#define SIOLOG(x,...) DLOG(x,__VA_ARGS__)


const PCHAR SiopPaginationHtmlBegin = (const PCHAR)"<br /><form action=\"/cgi-bin/%s\" method=\"GET\">";

const PCHAR SiopPaginationPagerBegin = (const PCHAR)"<br /><select name=\"pagenum\" style=\"vertical-align: middle;\">";

const PCHAR SiopPaginationHtmlEnd = (const PCHAR)"</select><input type=\"image\" src=\"/git.gif\" border=\"0\" style=\"vertical-align: middle; margin-left: 4px;\"></form>";

const PCHAR SiopPaginationItemHtml = (const PCHAR)"<option %s value=\"%d\">%d</option>";


BOOL SiopBuildIndexPaginationHtml
(
    PAUTO_BUFFER indexContent,
    ULONG totalPages,
    ULONG currentPage,
    PCHAR pagerHash,
    PCHAR index
)
{
    CHAR fmtBuf[512] = { 0 };
    INT len;
    ULONG insertIndex;
    PAUTO_BUFFER buf = NULL;

    // No needed to put pager if there is only one page.
    if (totalPages <= 1)
        return TRUE;

    buf = HlpCreateAutoBuffer(strlen(SiopPaginationItemHtml) * totalPages);

    len = sprintf(fmtBuf, (const PCHAR)SiopPaginationHtmlBegin, (const PCHAR)"index.exe");
    HlpWriteIntoAutoBuffer(buf, fmtBuf, len);

    if (strlen(pagerHash) > 0)
    {
        len = sprintf(fmtBuf, (PCHAR)"<input type=\"hidden\" name=\"ph\" value=\"%s\" />", pagerHash);
        HlpWriteIntoAutoBuffer(buf, fmtBuf, len);
    }

    if (strlen(index) > 0)
    {
        len = sprintf(fmtBuf, (PCHAR)"<input type=\"hidden\" name=\"i\" value=\"%s\" />", index);
        HlpWriteIntoAutoBuffer(buf, fmtBuf, len);
    }

    HlpWriteIntoAutoBuffer(buf, (const PCHAR)SiopPaginationPagerBegin, strlen(SiopPaginationPagerBegin));

    for (INT i = 0; i < totalPages; i++)
    {
        memset(fmtBuf, 0, sizeof(fmtBuf));
        len = sprintf(fmtBuf, (const PCHAR)SiopPaginationItemHtml,
            currentPage == i + 1 ? (const PCHAR)"selected" : (const PCHAR)"", i + 1, i + 1);
        HlpWriteIntoAutoBuffer(buf, fmtBuf, len);
    }

    HlpWriteIntoAutoBuffer(buf, (const PCHAR)SiopPaginationHtmlEnd, strlen(SiopPaginationHtmlEnd));

    insertIndex = HlpStrPos((PCHAR)indexContent->buffer, (PCHAR)"</ul>");

    if (insertIndex == (ULONG)-1)
    {
        return FALSE;
    }

    HlpInsertIntoAutoBuffer(indexContent, insertIndex, buf->buffer, buf->pos, 0);

    HlpDisposeAutoBuffer(buf);

    return TRUE;
}

BOOL SiopReplaceEdiContactInfo(PAUTO_BUFFER showTxtData)
{
    ULONG index;

    index = HlpStrPos((PCHAR)showTxtData->buffer, (PCHAR)"mailto:edi@sourtimes.org"); //24
    //http://sourtimes.oguzkartal.net:1999 //36

    if (index == (ULONG)-1)
        return FALSE;

    HlpInsertIntoAutoBuffer(showTxtData, index, (PVOID)"http://sourtimes.oguzkartal.net:1999", 36, 36 - 24);

    return TRUE;
}

BOOL SiopSetEntryWordbreakStyle(PAUTO_BUFFER showTxtData)
{
    ULONG index;

    index = HlpStrPos((PCHAR)showTxtData->buffer, (PCHAR)"%desc");

    if (index == (ULONG)-1)
        return FALSE;

    HlpInsertIntoAutoBuffer(showTxtData, index + 5, (PCHAR)"</p>", 4, 0);
    HlpInsertIntoAutoBuffer(showTxtData, index, (PCHAR)"<p style=\"word-wrap: break-word\">", 33, 0);

    return TRUE;
}

BOOL SiopBuildViewPaginationHtml
(
    PAUTO_BUFFER viewContent,
    UCHAR recordsPerPage,
    ULONG totalPages,
    ULONG currentPage,
    PCHAR baslik,
    UINT baslikId
)
{
    CHAR fmtBuf[512] = { 0 };
    INT len;
    ULONG insertIndex;
    PAUTO_BUFFER buf = NULL;

    // No need to put pager if only one page.
    if (totalPages <= 1)
        return TRUE;

    buf = HlpCreateAutoBuffer(strlen(SiopPaginationItemHtml) * totalPages);

    HlpWriteIntoAutoBuffer(buf, (PCHAR)"<center>", 8);

    len = sprintf(fmtBuf, (const PCHAR)SiopPaginationHtmlBegin, (const PCHAR)"view.exe");
    HlpWriteIntoAutoBuffer(buf, fmtBuf, len);

    if (baslikId > 0)
    {
        len = sprintf(fmtBuf, (PCHAR)"<input type=\"hidden\" name=\"bid\" value=\"%d\" />", baslikId);
        HlpWriteIntoAutoBuffer(buf, fmtBuf, len);
    }

    memset(fmtBuf, 0, sizeof(fmtBuf));
    len = sprintf(fmtBuf, (PCHAR)"<input type=\"hidden\" name=\"bs\" value=\"%s\" />", baslik);
    HlpWriteIntoAutoBuffer(buf, fmtBuf, len);

    HlpWriteIntoAutoBuffer(buf, (const PCHAR)SiopPaginationPagerBegin, strlen(SiopPaginationPagerBegin));

    for (INT i = 0; i < totalPages; i++)
    {
        memset(fmtBuf, 0, sizeof(fmtBuf));
        len = sprintf(fmtBuf, (const PCHAR)SiopPaginationItemHtml,
            currentPage == i + 1 ? (const PCHAR)"selected" : (const PCHAR)"", i + 1, i + 1);
        HlpWriteIntoAutoBuffer(buf, fmtBuf, len);
    }

    HlpWriteIntoAutoBuffer(buf, (const PCHAR)SiopPaginationHtmlEnd, strlen(SiopPaginationHtmlEnd));

    HlpWriteIntoAutoBuffer(buf, (PCHAR)"</center>", 9);

    insertIndex = HlpStrPos((PCHAR)viewContent->buffer, (PCHAR)"<hr ");

    if (insertIndex == (ULONG)-1)
    {
        return FALSE;
    }

    HlpInsertIntoAutoBuffer(viewContent, insertIndex, buf->buffer, buf->pos, 0);

    // Ordered list items start position injection.
    insertIndex = HlpStrPos((PCHAR)viewContent->buffer, (PCHAR)"<ol>");

    memset(fmtBuf, 0, sizeof(fmtBuf));
    sprintf(fmtBuf, (PCHAR)"<ol start=\"%d\">\r\n\r\n", ((currentPage - 1) * recordsPerPage) + 1);

    HlpInsertIntoAutoBuffer(viewContent, insertIndex, fmtBuf, strlen(fmtBuf), 4);

    HlpDisposeAutoBuffer(buf);

    return TRUE;
}

BOOL SiopBuildSuserListForExistenceCheck(PENTRY_VIEW_QUERY_RESULT pvqr)
{
    CHAR buf[128];
    ULONG len;

    if (pvqr->AffectedRecordCount == 0)
        return FALSE;

    SiopSusersOfTheView = HlpCreateAutoBuffer(pvqr->AffectedRecordCount * 32);

    if (!SiopSusersOfTheView)
        return FALSE;

    for (INT i = 0; i < pvqr->AffectedRecordCount; i++)
    {
        len = sprintf(buf, "~%s\r\nfpwd\r\n", pvqr->Entries[i].Suser);
        HlpWriteIntoAutoBuffer(SiopSusersOfTheView, buf, len);
    }


    return TRUE;
}

BOOL SiopDeliverFakeSuserDataForExistenceCheck(PFILE_IO_STATE ioState, LPVOID buf, DWORD readSize, DWORD *readedSize)
{
    ULONG remainSize, copySize;

    if (ioState->FilePointer.low == SiopSusersOfTheView->pos)
    {
        *readedSize = 0;
        return FALSE;
    }

    remainSize = SiopSusersOfTheView->pos - ioState->FilePointer.low;

    if (remainSize >= readSize)
        copySize = readSize;
    else
        copySize = remainSize;

    memcpy(buf, SiopSusersOfTheView->buffer + ioState->FilePointer.low, copySize);


    *readedSize = copySize;
    ioState->FilePointer.low += copySize;

    return TRUE;
}


PCHAR SiopGetCachedRequestContent(ULONG *length)
{
    if (SioCachedPostContentLen > 0)
    {
        *length = SioCachedPostContentLen;
        return SioCachedPostContent;
    }
    else if (SioCachedEnvContentLen > 0)
    {
        *length = SioCachedEnvContentLen;
        return SioCachedEnvContent;
    }

    *length = 0;
    return NULL;
}

void SiopCleanUnwantedCharsFromTheBaslik(PREQUEST_KEYVALUE pKv, UCHAR level)
{
    ULONG length;

    if (!strcmp(pKv->key, "word") && level == RL_AFTER_ENCODE)
    {
        length = pKv->valueLength;

        length = HlpRemoveString(pKv->value, length, (PCHAR)"$amp;");
        length = HlpRemoveString(pKv->value, length, (PCHAR)"$eq;");
        length = HlpRemoveString(pKv->value, length, (PCHAR)"$lt;");
        length = HlpRemoveString(pKv->value, length, (PCHAR)"$gt;");
        length = HlpRemoveString(pKv->value, length, (PCHAR)"$percnt;");
        length = HlpRemoveString(pKv->value, length, (PCHAR)"$plus;");

        pKv->valueLength = length;
    }
}

ULONG SioGetRequestValue(const PCHAR key, PCHAR buffer, ULONG bufSize)
{
    ULONG vlen = 0;

    for (LONG i = 0; i < SioRequest.KvCount; i++)
    {
        if (!_stricmp(key, SioRequest.KvList[i].key))
        {
            if (bufSize < SioRequest.KvList[i].valueLength + 1)
            {
                SIOLOG("buffer size not enough");
                return 0;
            }

            strcpy(buffer, SioRequest.KvList[i].value);
            vlen = SioRequest.KvList[i].valueLength;

            break;
        }
    }

    return vlen;
}

BOOL SioGetRequestValueAsInt(const PCHAR key, INT *pval)
{
    char buf[32] = { 0 };
    
    if (!SioGetRequestValue(key, buf, sizeof(buf)))
        return FALSE;

    *pval = (INT)atoi(buf);
    return TRUE;
}

PFILE_IO_STATE SioGetIoStateByFileType(FILE_TYPE type)
{
    INT index = (INT)type;

    if (index < 0)
        return NULL;

    if (index >= MaxTxt)
        return NULL;

    return &SioHandles.files[index];
}

PFILE_IO_STATE SioGetIoStateByHandle(HANDLE handle)
{
    for (int i = 0; i < MaxTxt; i++)
    {
        if (SioHandles.files[i].realHandle == handle ||
            SioHandles.files[i].fakeHandle == handle)
        {
            return &SioHandles.files[i];
        }
    }

    return NULL;
}


PFILE_IO_STATE SiopReserveIoState(FILE_TYPE type)
{
    PVOID tag = NULL;
    PFILE_IO_STATE ioState;

    if (type >= MaxTxt)
        return NULL;

    ioState = &SioHandles.files[type];

    if (ioState->status & FIOS_ACTIVE)
        return ioState;

    if (ioState->status & FIOS_KEEP_TAG)
        tag = ioState->tag;

    RtlZeroMemory(ioState, sizeof(FILE_IO_STATE));
    ioState->type = type;

    if (tag)
    {
        ioState->tag = tag;
        ioState->status |= FIOS_KEEP_TAG;
    }

    if (!_stricmp(SioExeName, "index.exe"))
        ioState->exeName = INDEX;
    else if (!_stricmp(SioExeName, "add.exe"))
        ioState->exeName = ADD;
    else if (!_stricmp(SioExeName, "view.exe"))
        ioState->exeName = VIEW;

    ioState->status |= FIOS_ACTIVE;

    return ioState;
}

void SioDisposeIoState(PFILE_IO_STATE state)
{
    state->FilePointer.low = 0;
    state->FilePointer.high = 0;
    state->realHandle = NULL;
    state->fakeHandle = NULL;
    state->tag = NULL;
    state->status = 0;
}

BOOL SioHandleCreateFileForViewEntry(PFILE_IO_STATE state)
{
    ULONG len;
    PENTRY_VIEW_QUERY_RESULT pvqr;
    CHAR baslik[128] = { 0 };
    CHAR latest[2] = { 0 };
    UINT baslikId = 0;
    INT pageNumber = -1;

    if (SioRequest.KvCount > 0)
    {
        len = SioGetRequestValue((const PCHAR)"baslik", baslik, sizeof(baslik));

        if (len > MAX_BASLIK_LEN)
            return FALSE;

        SioGetRequestValueAsInt((const PCHAR)"bid", (INT*)&baslikId);
        SioGetRequestValueAsInt((const PCHAR)"pagenum", &pageNumber);

        if (len == 0 && baslikId == 0)
        {
            // Default unnamed key-value, like view.exe?BASLIK 
            len = SioGetRequestValue((const PCHAR)"", baslik, sizeof(baslik));
            if (len == 0)
                return FALSE;
        }

        if (len > 0)
            HlpUrlDecodeAsAscii(baslik, strlen(baslik));

        SioGetRequestValue((const PCHAR)"latest", latest, sizeof(latest));
    }
    else
    {
        if (SiopGetCachedRequestContent(&len))
        {
            if (len + 1 > sizeof(baslik))
                return FALSE;

            strncpy(baslik, SiopGetCachedRequestContent(&len), len);

            if (HlpUrlDecodeAsAscii(baslik, len) > 50)
                return FALSE;
        }
    }

    pvqr = (PENTRY_VIEW_QUERY_RESULT)HlpAlloc(sizeof(ENTRY_VIEW_QUERY_RESULT));

    if (!pvqr)
    {
        return FALSE;
    }

    SzGetEntriesByBaslik(baslik, pageNumber, baslikId, latest[0] == '1', pvqr);

    state->tag = pvqr;

    return TRUE;
}

BOOL SioHandleCreateFileForIndexAndSearch(PFILE_IO_STATE state)
{
    PINDEX_QUERY_RESULT pqr;
    CHAR iVal[16] = { 0 };
    CHAR searchTerm[512] = { 0 };
    CHAR dateBegin[32] = { 0 };
    CHAR dateEnd[32] = { 0 };
    CHAR suser[64] = { 0 };
    CHAR pagerHash[36] = { 0 };

    INT pageNum = -1;

    SioGetRequestValue((const PCHAR)"i", iVal, sizeof(iVal));
    SioGetRequestValue((const PCHAR)"search", searchTerm, sizeof(searchTerm));
    SioGetRequestValue((const PCHAR)"date", dateBegin, sizeof(dateBegin));
    SioGetRequestValue((const PCHAR)"todate", dateEnd, sizeof(dateEnd));
    SioGetRequestValue((const PCHAR)"author", suser, sizeof(suser));
    SioGetRequestValueAsInt((const PCHAR)"pagenum", &pageNum);
    SioGetRequestValue((const PCHAR)"ph", pagerHash, sizeof(pagerHash));

    pqr = (PINDEX_QUERY_RESULT)HlpAlloc(sizeof(INDEX_QUERY_RESULT));

    if (!pqr)
        return FALSE;

    if (!SzSearch(iVal, searchTerm, suser, dateBegin, dateEnd, pagerHash, pageNum, pqr))
    {
        SIOLOG("pagenum: %d, pagerhash: %s", pageNum, pagerHash);
        SIOLOG("i=%s, search=%s, author=%s", iVal, searchTerm, suser);
        SIOLOG("fd: %s, td: %s", dateBegin, dateEnd);
        HlpFree(pqr);
        return FALSE;
    }

    if (strlen(iVal) > 0)
        strcpy_s(pqr->index, sizeof(pqr->index), iVal);

    state->tag = pqr;

    if (pqr->TotalPageCount > 1)
    {
        // Mark dict.txt's Io object as delayed dispose so that we don't
        // destroy the entry query result attached to this Io object.
        state->status |= FIOS_DELAY_DISPOSE;
    }

    return TRUE;
}

BOOL SiopReadStdinPostContent()
{
    BOOL result;
    PCHAR temp;
    CHAR buf[128] = { 0 };
    ULONG realContentLength, readLen;
    HANDLE stdinHandle = GetStdHandle(STD_INPUT_HANDLE);

    //call original env to read real length
    _GetEnvironmentVariableA("CONTENT_LENGTH", buf, sizeof(buf));

    realContentLength = strtoul(buf, NULL, 10);

    if (realContentLength == 0)
        return FALSE;
    
    SioCachedPostContent = (PCHAR)HlpAlloc(realContentLength + 1);

    if (!SioCachedPostContent)
    {
        SIOLOG("alloc error for %lu bytes",realContentLength+1);
        return FALSE;
    }

    SioCachedPostContentLen = realContentLength;

    result = _ReadFile(stdinHandle, SioCachedPostContent, realContentLength, &readLen, NULL);

    DASSERT(SioCachedPostContentLen == readLen);
    
    if (result)
    {
        readLen = HlpReEncodeAsAscii(
            SioCachedPostContent, 
            SioCachedPostContentLen, 
            &temp, 
            SiopCleanUnwantedCharsFromTheBaslik
        );

        if (SioCachedPostContent != temp)
        {
            HlpFree(SioCachedPostContent);
            SioCachedPostContent = temp;
        }

        SioCachedPostContentLen = readLen;
    }
    else
    {
        SIOLOG("CGI Post content could not be read. Err: %lu", GetLastError());

        HlpFree(SioCachedPostContent);
        SioCachedPostContent = NULL;
        SioCachedPostContentLen = 0;
    }

    return result;
}

BOOL SioHandleReadFileForStdin(LPVOID buffer, DWORD readSize, DWORD *readedSize, LPOVERLAPPED olap)
{
    BOOL result = FALSE;
    DWORD readLen = 0;

    if (!SioCachedPostContent)
    {
        if (!SiopReadStdinPostContent())
            return FALSE;
    }

    if (readedSize)
        *readedSize = SioCachedPostContentLen;

    if (buffer)
        RtlCopyMemory(buffer, SioCachedPostContent, SioCachedPostContentLen);

    result = TRUE;

    return result;
}

BOOL SioHandleReadFileForUser(PFILE_IO_STATE state, LPVOID buf, DWORD readSize, DWORD* readedSize)
{
    BOOL result = FALSE;
    PCHAR psz;
    CHAR nick[64] = { 0 };
    CHAR password[64] = { 0 };
    ULONG ulen = 0, plen = 0;
    DWORD readLen = 0;

    if (state->FilePointer.low == 0)
    {
        if (state->exeName == VIEW)
        {
            SiopDeliverFakeSuserDataForExistenceCheck(state, buf, readSize, readedSize);
            return TRUE;
        }

        ulen = SioGetRequestValue((const PCHAR)"nick", nick, sizeof(nick));
        plen = SioGetRequestValue((const PCHAR)"password", password, sizeof(password));

        if (ulen > 0 && plen > 0)
        {
            ulen = HlpUrlDecodeAsAscii(nick, ulen);
            plen = HlpUrlDecodeAsAscii(password, plen);

            CharLowerA(nick);
            CharLowerA(password);

            // this is authentication for entry addition process
            if (!SzAuthSuser(nick, password))
            {
                if (readedSize)
                    *readedSize = 0;
                SIOLOG("Auth failed for %s", nick);
                return TRUE;
            }

            psz = (PCHAR)buf;
            *psz = '~'; // set nickname mark first
            readLen = 1;
            strcpy(psz + readLen, nick);
            readLen += ulen;
            strcpy(psz + readLen, "\r\n");
            readLen += 2;
            strcpy(psz + readLen, password);
            readLen += plen;
            strcpy(psz + readLen, "\r\n");
            readLen += 2;
            *readedSize = readLen;
            state->FilePointer.low += readLen;
            SIOLOG("nickname and pwd passed to the backend (%s)", nick);
            result = TRUE;
        }
        else
        {
            *readedSize = 0;
            result = TRUE;
        }
    }
    else
    {
        if (state->exeName == VIEW)
        {
            SiopDeliverFakeSuserDataForExistenceCheck(state, buf, readSize, readedSize);
            return TRUE;
        }
        *readedSize = 0;
        result = TRUE;
    }

    return result;
}

BOOL SiopPrepareEntryForReadSequence(PSOZLUK_ENTRY_CONTEXT entry)
{
    ULONG len;

    if (entry->Baslik[0] == '~')
        return FALSE;

    len = strlen(entry->Baslik);

    memmove(entry->Baslik + 1, entry->Baslik, len);
    entry->Baslik[0] = '~';
    len++;

    strcpy(entry->Baslik + len, "\r\n");

    strcat(entry->Suser, "\r\n");
    strcat(entry->Date, "\r\n");
    strcat(entry->Desc, "\r\n");

    entry->DescLength += 2;


    return TRUE;
}

#define ERS_LASTWRITE_MASK      0x7FFFFFF0
#define ERS_PARTIALCHECK_MASK   0x80000000
#define ERS_INDEX_MASK          0x0000000F

#define MAKE_ERS(lastWrPos, index) (((DWORD)(lastWrPos) << 4) | ERS_PARTIALCHECK_MASK | ((DWORD)index))

DWORD SiopFillReadBufferWithEntry(PCHAR buffer, DWORD bufferSize, PSOZLUK_ENTRY_CONTEXT entry, DWORD state, DWORD *pWritten)
{
    struct
    {
        PCHAR str;
        ULONG len;
    }parts[4];

    DWORD partLen,tmp;
    DWORD written = 0, partialWritten = 0;
    PCHAR buf = buffer;
    DWORD index=0,lastWritePos=0;
    
    parts[0].str = entry->Baslik;
    parts[1].str = entry->Suser;
    parts[2].str = entry->Date;
    parts[3].str = entry->Desc;

    if (state & ERS_PARTIALCHECK_MASK)
    {
        lastWritePos = (state & ERS_LASTWRITE_MASK) >> 4;
        index = state & ERS_INDEX_MASK;
    }

    for (int i = (int)index; i < 4; i++)
    {
        partialWritten = 0;

        if (i < 3)
        {
            parts[i].len = strlen(parts[i].str);
            partLen = parts[i].len;
        }
        else
        {
            parts[i].len = entry->DescLength;
            partLen = entry->DescLength;
        }

        if (lastWritePos > 0)
        {
            tmp = partLen - lastWritePos;

            partLen = tmp;

            if (partLen > bufferSize)
            {
                strncpy(buf, parts[i].str + lastWritePos, bufferSize);
                lastWritePos += bufferSize;

                state = MAKE_ERS(lastWritePos, i);
                *pWritten = bufferSize;

                return state;
            }

            strncpy(buf, parts[i].str + lastWritePos, partLen);
            written += partLen;
            buf += partLen;

            lastWritePos = 0;

            continue;
        }

        if (written + partLen > bufferSize)
        {
            tmp = bufferSize - written;

            //
            strncpy(buf, parts[i].str, tmp);
            written += tmp;

            state = MAKE_ERS(tmp, i);

            *pWritten = written;
            return state;
        }

        strncpy(buf, parts[i].str,parts[i].len);
        written += parts[i].len;
        
        buf += parts[i].len;
    }
    
    *pWritten = written;

    return 0;
}

BOOL SioDispatchAutoBufferContent(PFILE_IO_STATE state, LPVOID buf, DWORD readSize, DWORD *readedSize)
{
    DWORD readAmount;

    PAUTO_BUFFER iobuf = (PAUTO_BUFFER)state->tag;

    if (!iobuf)
        return FALSE;

    if (state->FilePointer.low == iobuf->pos)
    {
        *readedSize = 0;
        return TRUE;
    }

    if (readSize > iobuf->pos - state->FilePointer.low)
        readAmount = iobuf->pos - state->FilePointer.low;
    else
        readAmount = readSize;

    memcpy(buf, iobuf->buffer + state->FilePointer.low,readAmount);

    state->FilePointer.low += readAmount;

    *readedSize = readAmount;
    return TRUE;
}



DWORD SiopDeliverEntryContent
(
    PSOZLUK_ENTRY_CONTEXT entries, 
    DWORD *pIndex, 
    LONG recordCount,
    DWORD state,
    PCHAR buf,
    DWORD readSize,
    DWORD *readedSize
)
{
    PSOZLUK_ENTRY_CONTEXT entry;
    DWORD index = *pIndex;
    DWORD written = 0, totalWritten = 0;

    for (int i = index; i < recordCount; i++)
    {
        entry = &entries[index];

        SiopPrepareEntryForReadSequence(entry);

        state = SiopFillReadBufferWithEntry(buf, readSize, entry, state, &written);

        totalWritten += written;

        buf  += written;
        readSize -= written;

        if (state & ERS_PARTIALCHECK_MASK)
        {
            //Hmm there is remained part which is caused by lack of buffer size.
            break;
        }

        if (entry->RepCount > 0)
        {
            entry->RepCount--;

            if (entry->RepCount == 0)
            {
                index++;
            }
        }
        else
            index++;

    }

    *pIndex = index;
    *readedSize = totalWritten;

    return state;
}

BOOL SioHandleViewReadFileForDictionary(PFILE_IO_STATE state, LPVOID buf, DWORD readSize, DWORD *readedSize)
{
    PENTRY_VIEW_QUERY_RESULT pvqr = (PENTRY_VIEW_QUERY_RESULT)state->tag;
    PCHAR pbuf;
    DWORD writtenState=0;
    DWORD totalWritten = 0, written = 0;

    pbuf = (PCHAR)buf;

    if (pvqr == NULL)
    {
        *readedSize = 0;
        return TRUE;
    }

    if (pvqr->AffectedRecordCount == 0 || pvqr->AffectedRecordCount == pvqr->Status.RecordIndex)
    {
        *readedSize = 0;
        return TRUE;
    }

    pvqr->Status.State = SiopDeliverEntryContent(
        pvqr->Entries,
        &pvqr->Status.RecordIndex,
        pvqr->AffectedRecordCount,
        pvqr->Status.State,
        pbuf, readSize, readedSize);

    return TRUE;
}

BOOL SioHandleIndexReadFileForDictionary(PFILE_IO_STATE state, LPVOID buf, DWORD readSize, DWORD *readedSize)
{
    PCHAR pbuf;
    PINDEX_QUERY_RESULT pqr = (PINDEX_QUERY_RESULT)state->tag;
    DWORD totalWritten=0, written = 0;

    pbuf = (PCHAR)buf;

    if (pqr == NULL)
    {
        *readedSize = 0;
        return TRUE;
    }

    if (pqr->AffectedLogicalRecordCount == 0 || pqr->AffectedLogicalRecordCount == pqr->Status.RecordIndex)
    {
        *readedSize = 0;
        return TRUE;
    }

    pqr->Status.State = SiopDeliverEntryContent(
        pqr->Entries,
        &pqr->Status.RecordIndex,
        pqr->AffectedLogicalRecordCount,
        pqr->Status.State,
        pbuf,
        readSize,
        readedSize
    );


    return TRUE;
    
}

BOOL SioAddNewEntry(PFILE_IO_STATE state)
{
    PFILE_WRITE_BUFFER pWrBuffer;
    BOOL result;
    DWORD written = 0;

    SOZLUK_ENTRY_CONTEXT sec;
    memset(&sec, 0, sizeof(sec));

    pWrBuffer = (PFILE_WRITE_BUFFER)state->tag;

    if (!pWrBuffer)
    {
        SIOLOG("invalid fwb !");
        return FALSE;
    }


    SIOLOG("Building entry context from IO Buffer");

    if (!HlpBuildEntryContextFromWriteData((PAUTO_BUFFER)pWrBuffer, &sec))
        return FALSE;


    SIOLOG("new entry for %s (%s)", sec.Baslik, sec.Suser);

    if (!SzAddEntry(&sec))
    {
        result = FALSE;
        goto oneWay;
    }

    result = TRUE;

    SioGetIoStateByFileType(AddTxt)->status |= FIOS_KEEP_TAG;
    SioGetIoStateByFileType(AddTxt)->tag = (LPVOID)sec.BaslikId;

oneWay:

    HlpFree(sec.Desc);

    return result;
}

HANDLE WINAPI Hook_CreateFileA(
    __in     LPCSTR lpFileName,
    __in     DWORD dwDesiredAccess,
    __in     DWORD dwShareMode,
    __in_opt LPSECURITY_ATTRIBUTES lpSecurityAttributes,
    __in     DWORD dwCreationDisposition,
    __in     DWORD dwFlagsAndAttributes,
    __in_opt HANDLE hTemplateFile
)
{
    HANDLE handle;
    PFILE_IO_STATE fileIoHandle = NULL;
    PAUTO_BUFFER iobuf;
    BOOL createOriginalFile = FALSE;
    CHAR buf[256];
    CHAR fileName[MAX_PATH] = { 0 };

    RcReadRequest(&SioRequest);

    HlpGetFileNameFromPath((PCHAR)lpFileName, fileName, sizeof(fileName));

    if (!_stricmp(fileName, "dict.txt"))
    {
        fileIoHandle = SiopReserveIoState(DictTxt);
    }
    else if (!_stricmp(fileName, "user.txt"))
    {
        fileIoHandle = SiopReserveIoState(UserTxt);
    }
    else if (!_stricmp(fileName, "show.txt"))
    {
        fileIoHandle = SiopReserveIoState(ShowTxt);
        createOriginalFile = TRUE;
    }
    else if (!_stricmp(fileName, "index.txt"))
    {
        fileIoHandle = SiopReserveIoState(IndexTxt);
        createOriginalFile = TRUE;
    }
    else if (!_stricmp(fileName, "add.txt"))
    {
        fileIoHandle = SiopReserveIoState(AddTxt);
        createOriginalFile = TRUE;
    }

    if (createOriginalFile || fileIoHandle == NULL)
    {
        handle = _CreateFileA(
            lpFileName,
            dwDesiredAccess,
            dwShareMode,
            lpSecurityAttributes,
            dwCreationDisposition,
            dwFlagsAndAttributes,
            hTemplateFile
        );

        if (!fileIoHandle)
            return handle;
        else
            fileIoHandle->realHandle = handle;
    }

    if (fileIoHandle->type == DictTxt)
    {
        if (fileIoHandle->exeName == INDEX)
        {
            SioHandleCreateFileForIndexAndSearch(fileIoHandle);
        }
        else if (fileIoHandle->exeName == ADD)
        {
            fileIoHandle->tag = SiopCreateWriteBuffer(256);
        }
    }
    else if (fileIoHandle->type == AddTxt)
    {
        ULONG slen = 0, baslikId = 0;
        LONG index = 0;

        iobuf = HlpCreateAutoBuffer(GetFileSize(fileIoHandle->realHandle, NULL) + 1);

        if (!iobuf)
        {
            SioDisposeIoState(fileIoHandle);
            return fileIoHandle->realHandle;
        }

        _ReadFile(fileIoHandle->realHandle, iobuf->buffer, iobuf->size, &iobuf->pos, NULL);

        baslikId = (ULONG)fileIoHandle->tag;
        fileIoHandle->status &= ~FIOS_KEEP_TAG;

        fileIoHandle->tag = iobuf;

        index = HlpStrPos((PCHAR)iobuf->buffer, (PCHAR)"%queryword");

        if (index >= 0)
        {
            iobuf->pos = HlpRemoveString((PCHAR)iobuf->buffer, iobuf->pos, (PCHAR)"%queryword");

            slen = SioGetRequestValue((const PCHAR)"word", buf, sizeof(buf));

            HlpInsertIntoAutoBuffer(iobuf, index, buf, slen, 0);

            index += slen;

            slen = sprintf(buf, "&bid=%lu&latest=1", baslikId);

            HlpInsertIntoAutoBuffer(iobuf, index, buf, slen, 0);

            //first try to get VIEW.EXE?baslik 
        }

    }
    else if (fileIoHandle->type == IndexTxt)
    {
        PFILE_IO_STATE dictIo;
        PINDEX_QUERY_RESULT entryListCtx;

        dictIo = &SioHandles.files[DictTxt];

        //Check dict.txt's Io state 
        //there is any pageable content.
        //If so, we can intercept the index.txt template file to
        //put our pager html, otherwise the index.txt's io
        //sequence continues their natural workflow.
        if (dictIo->status & FIOS_ACTIVE)
        {
            entryListCtx = (PINDEX_QUERY_RESULT)dictIo->tag;

            iobuf = HlpCreateAutoBuffer(GetFileSize(fileIoHandle->realHandle, NULL) + 1);
            fileIoHandle->tag = iobuf;

            //read entire index.txt template content to our buffer
            _ReadFile(fileIoHandle->realHandle, iobuf->buffer, iobuf->size, &iobuf->pos, NULL);

            //and inject pager html
            SiopBuildIndexPaginationHtml(
                iobuf,
                entryListCtx->TotalPageCount,
                entryListCtx->CurrentPageNumber,
                entryListCtx->PagerHash,
                entryListCtx->index
            );
        }
    }
    else if (fileIoHandle->type == ShowTxt)
    {
        PFILE_IO_STATE dictIo;
        PENTRY_VIEW_QUERY_RESULT entryListCtx;

        dictIo = SiopReserveIoState(DictTxt);

        iobuf = HlpCreateAutoBuffer(GetFileSize(fileIoHandle->realHandle, NULL) + 1);
        fileIoHandle->tag = iobuf;

        _ReadFile(fileIoHandle->realHandle, iobuf->buffer, iobuf->size, &iobuf->pos, NULL);

        SiopReplaceEdiContactInfo(iobuf);

        if (SioHandleCreateFileForViewEntry(dictIo))
        {
            entryListCtx = (PENTRY_VIEW_QUERY_RESULT)dictIo->tag;

            SiopBuildSuserListForExistenceCheck(entryListCtx);

            SiopSetEntryWordbreakStyle(iobuf);

            SiopBuildViewPaginationHtml(
                iobuf,
                entryListCtx->RecordsPerPage,
                entryListCtx->TotalPageCount,
                entryListCtx->CurrentPageNumber,
                entryListCtx->Baslik,
                entryListCtx->BaslikId
            );
        }
    }

    fileIoHandle->fakeHandle = fileIoHandle;
    fileIoHandle->FilePointer.low = 0;
    fileIoHandle->FilePointer.high = 0;

    //return fake handle
    return fileIoHandle;
}

BOOL WINAPI Hook_ReadFile(
    __in        HANDLE hFile,
    __out_bcount_part_opt(nNumberOfBytesToRead, *lpNumberOfBytesRead) __out_data_source(FILE) LPVOID lpBuffer,
    __in        DWORD nNumberOfBytesToRead,
    __out_opt   LPDWORD lpNumberOfBytesRead,
    __inout_opt LPOVERLAPPED lpOverlapped
)
{
    PFILE_IO_STATE ioState;
    BOOL result = FALSE;

    if (hFile == GetStdHandle(STD_INPUT_HANDLE))
    {
        //the sozluk-cgi trying to read CGI Post content from the stdin.
        //we have to read first and cache it 'cuz stdin acts as a named pipe
        //so we cant seek on it to re-read. 
        
        if (!SioHandleReadFileForStdin(lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead,lpOverlapped))
            goto continueApi;

        return TRUE;
    }

    ioState = SioGetIoStateByHandle(hFile);

    if (ioState)
    {
        switch (ioState->type)
        {
        case DictTxt:
        {
            if (ioState->exeName == INDEX)
            {
                result = SioHandleIndexReadFileForDictionary(
                    ioState, lpBuffer, 
                    nNumberOfBytesToRead, lpNumberOfBytesRead
                );

            }
            else if (ioState->exeName == VIEW)
            {

                result = SioHandleViewReadFileForDictionary(
                    ioState, lpBuffer,
                    nNumberOfBytesToRead, lpNumberOfBytesRead
                );
            }

            break;
        }
        case UserTxt:
            result = SioHandleReadFileForUser(ioState,lpBuffer,nNumberOfBytesToRead,lpNumberOfBytesRead);
            break;
        case IndexTxt:
        case ShowTxt:
        case AddTxt:
        {
            result = SioDispatchAutoBufferContent(ioState, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead);

            if (!result)
            {
                hFile = ioState->realHandle;
                goto continueApi;
            }

            break;
        }
        }

        return result;
    }

continueApi:

    return _ReadFile(hFile, lpBuffer, nNumberOfBytesToRead, lpNumberOfBytesRead, lpOverlapped);
}

BOOL WINAPI Hook_WriteFile(
    __in        HANDLE hFile,
    __in_bcount_opt(nNumberOfBytesToWrite) LPCVOID lpBuffer,
    __in        DWORD nNumberOfBytesToWrite,
    __out_opt   LPDWORD lpNumberOfBytesWritten,
    __inout_opt LPOVERLAPPED lpOverlapped
)
{
    PFILE_IO_STATE ioState;
    BOOL result = FALSE;

    ioState = SioGetIoStateByHandle(hFile);

    if (ioState)
    {
        
        switch (ioState->type)
        {
        case DictTxt:
        {
            result = SiopPutIntoWriteBuffer((PFILE_WRITE_BUFFER)ioState->tag, (LPVOID)lpBuffer, nNumberOfBytesToWrite);

            if (result)
            {
                ioState->FilePointer.low += nNumberOfBytesToWrite;
                *lpNumberOfBytesWritten = nNumberOfBytesToWrite;
            }

            break;
        }
        case UserTxt: 
            break;

        }

        return result;
    }

continueApi:

    return _WriteFile(hFile, lpBuffer, nNumberOfBytesToWrite, lpNumberOfBytesWritten, lpOverlapped);
}


BOOL WINAPI Hook_CloseHandle(
    __in        HANDLE hObject
)
{
    PFILE_IO_STATE ioState;
    BOOL result = FALSE;

    ioState = SioGetIoStateByHandle(hObject);

    if (ioState)
    {

        if (ioState->type == DictTxt)
        {
            if (ioState->exeName == INDEX)
            {
                if (ioState->status & FIOS_DELAY_DISPOSE)
                {
                    //Ok. we have to keep dict.txt's resources for now.
                    SIOLOG("Index query result for dict resource disposition delayed");
                    return TRUE;
                }

                //otherwise we are free to dispose its resources.
                SioDisposeIndexQueryResult((PINDEX_QUERY_RESULT)ioState->tag,TRUE);
            }
            else if (ioState->exeName == ADD)
            {
                if (ioState->tag)
                {
                    //set add.txt's iostate's baslikid
                    SioAddNewEntry(ioState);
                    SiopDisposeWriteBuffer((PFILE_WRITE_BUFFER)ioState->tag);
                    ioState->tag = NULL;
                }

            }
            else if (ioState->exeName == VIEW)
            {
                //Dispose 
                SioDisposeViewQueryResult((PENTRY_VIEW_QUERY_RESULT)ioState->tag, TRUE);
            }
        }
        else if (ioState->type == IndexTxt)
        {
            //
            PFILE_IO_STATE dictIoState = &SioHandles.files[DictTxt];

            /*
            First we must check dict.txt's flag to know delayed dispose.
            If marked as postpone the dispose,
            we had used the dict.txt's query result to build pager
            and now we no longer need the index.txt template file also.
            we can destroy dict.txt's resources either.
            */
            if (dictIoState->status & FIOS_DELAY_DISPOSE)
            {
                //unmark delay disposing and call the close
                dictIoState->status &= ~FIOS_DELAY_DISPOSE;
                Hook_CloseHandle((HANDLE)dictIoState);
            }

            //if there were no pageable content the tag field is empty.
            //that means index.txt's file io ops was gone natural.
            if (ioState->tag)
                HlpDisposeAutoBuffer((PAUTO_BUFFER)ioState->tag);
        }
        else if (ioState->type == AddTxt || ioState->type == ShowTxt)
            HlpDisposeAutoBuffer((PAUTO_BUFFER)ioState->tag);

        if (ioState->realHandle)
            _CloseHandle(ioState->realHandle);

        SioDisposeIoState(ioState);
        return TRUE;
    }

    return _CloseHandle(hObject);
}

DWORD WINAPI Hook_SetFilePointer(
    __in        HANDLE hFile,
    __in        LONG lDistanceToMove,
    __inout_opt PLONG lpDistanceToMoveHigh,
    __in        DWORD dwMoveMethod
)
{
    PFILE_IO_STATE ioState;
    DWORD result = 0;
    LONG low=0, high=0;

    low = lDistanceToMove;

    if (lpDistanceToMoveHigh)
        high = *lpDistanceToMoveHigh;

    ioState = SioGetIoStateByHandle(hFile);

    if (ioState)
    {
        switch (dwMoveMethod)
        {
        case FILE_BEGIN:
            ioState->FilePointer.low = low;
            ioState->FilePointer.high = high;
            break;
        case FILE_CURRENT:
        case FILE_END:
            ioState->FilePointer.low += low;
            ioState->FilePointer.high += high;
            break;
        }

        if (lpDistanceToMoveHigh)
            *lpDistanceToMoveHigh = ioState->FilePointer.high;

        return ioState->FilePointer.low;
    }

    return _SetFilePointer(hFile, lDistanceToMove, lpDistanceToMoveHigh, dwMoveMethod);
}

DWORD WINAPI Hook_GetFileSize(
    __in        HANDLE  hFile,
    __out_opt   LPDWORD lpFileSizeHigh
)
{
    SIOLOG("GetFileSize");
    
    return _GetFileSize(hFile, lpFileSizeHigh);
}


DWORD WINAPI Hook_GetEnvironmentVariableA(
    __in_opt LPCSTR lpName,
    __out_ecount_part_opt(nSize, return +1) LPSTR lpBuffer,
    __in DWORD nSize
)
{
    PCHAR temp;
    CHAR tmp[1];
    DWORD len;
    BOOL isViewExe, isQsInternal;

    isQsInternal = _stricmp(lpName, "QUERY_STRING_INTERNAL") == 0;
    isViewExe = _stricmp(SioExeName, "view.exe") == 0;

    if (!_stricmp(lpName, "QUERY_STRING") || isQsInternal)
    {
        if (!SioCachedEnvContent)
        {
            len = _GetEnvironmentVariableA(lpName, tmp, sizeof(tmp));

            if (len > 0)
            {
                SioCachedEnvContent = (PCHAR)HlpAlloc(len);

                if (!SioCachedEnvContent)
                    goto continueWithApi;

                SioCachedEnvContentLen = _GetEnvironmentVariableA(lpName, SioCachedEnvContent, len);

                if (SioCachedEnvContentLen > 0)
                {
                    len = HlpReEncodeAsAscii(SioCachedEnvContent, SioCachedEnvContentLen, &temp, SiopCleanUnwantedCharsFromTheBaslik);

                    if (temp != SioCachedEnvContent)
                    {
                        HlpFree(SioCachedEnvContent);
                        SioCachedEnvContent = temp;
                    }

                    SioCachedEnvContentLen = len;

                    if (isViewExe)
                    {
                        RcReadRequest(&SioRequest);
                    }
                }
            }
        }

        if (isViewExe && !isQsInternal)
        {
            len = SioGetRequestValue((const PCHAR)"bs", lpBuffer, nSize);
            if (len > 0)
                return len;
            return SioGetRequestValue((const PCHAR)"", lpBuffer, nSize);
        }

        if (nSize < SioCachedEnvContentLen + 1)
            return SioCachedEnvContentLen + 1;

        memcpy(lpBuffer, SioCachedEnvContent, SioCachedEnvContentLen);

        return SioCachedEnvContentLen;
    }
    else if (!_stricmp(lpName, "CONTENT_LENGTH"))
    {
        //Before returing the content-length value to the cgi executable
        //we have to process the post content to making sure input content
        //can be handle correctly by the sozluk-cgi.
        if (!SioCachedPostContentLen)
        {
            if (!SiopReadStdinPostContent())
                return 0;
        }

        memset(lpBuffer, 0, nSize);
        _ultoa(SioCachedPostContentLen, lpBuffer, 10);
        return strlen(lpBuffer);
    }

continueWithApi:
    return _GetEnvironmentVariableA(lpName, lpBuffer, nSize);
}



BOOL SioDisposeIndexQueryResult(PINDEX_QUERY_RESULT pqr, BOOL freeObject)
{
    if (pqr)
    {
        for (int i = 0; i < pqr->AffectedLogicalRecordCount; i++)
        {
            SioFreeEntryContext(&pqr->Entries[i], FALSE);
        }

        HlpFree(pqr->Entries);

        if (freeObject)
            HlpFree(pqr);
    }
    else
        return FALSE;

    return TRUE;
}

BOOL SioDisposeViewQueryResult(PENTRY_VIEW_QUERY_RESULT pvqr, BOOL freeObject)
{
    if (pvqr)
    {
        for (int i = 0; i < pvqr->AffectedRecordCount; i++)
        {
            SioFreeEntryContext(&pvqr->Entries[i], FALSE);
        }

        HlpFree(pvqr->Entries);

        if (freeObject)
            HlpFree(pvqr);
    }
    else
        return FALSE;

    return TRUE;
}

BOOL SioFreeEntryContext(PSOZLUK_ENTRY_CONTEXT pec, BOOL freeObject)
{
    if (pec->Desc != NULL)
    {
        if (pec->Desc != pec->DescFixedBuf)
            HlpFree(pec->Desc);
    }

    if (freeObject)
        HlpFree(pec);

    return TRUE;
}



void SiopReleaseGlobalResources()
{
    RbCloseBridge();

    if (SiopSusersOfTheView)
        HlpDisposeAutoBuffer(SiopSusersOfTheView);

    if (SioCachedEnvContent)
        HlpFree(SioCachedEnvContent);

    if (SioCachedPostContent)
        HlpFree(SioCachedPostContent);

    RciDestroyRequestObject(&SioRequest);


    for (int i = 0; i < MaxTxt; i++)
    {
        if (SioHandles.files[i].status & FIOS_ACTIVE)
            SIOLOG("fid %d still active.", i);

        if (SioHandles.files[i].tag)
            SIOLOG("fid %d has tag", i);
    }

}