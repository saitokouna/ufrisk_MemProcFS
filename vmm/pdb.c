// pdb.h : implementation related to parsing of program databases (PDB) files
//         used for debug symbols and automatic retrieval from the Microsoft
//         Symbol Server. (Windows exclusive functionality).
//
// (c) Ulf Frisk, 2019-2021
// Author: Ulf Frisk, pcileech@frizk.net
//

#include "pdb.h"
#include "pe.h"
#include "util.h"
#include "vmmwindef.h"
#include "vmmwininit.h"
#include <winreg.h>
#include <io.h>
#define _NO_CVCONST_H
#include <dbghelp.h>

#define VMMWIN_PDB_LOAD_ADDRESS_STEP    0x10000000;
#define VMMWIN_PDB_LOAD_ADDRESS_BASE    0x0000511f'00000000;
#define VMMWIN_PDB_FAKEPROCHANDLE       (HANDLE)0x00005fed'6fed7fed
#define VMMWIN_PDB_WARN_DEFAULT         "WARNING: Functionality may be limited. Extended debug information disabled.\n"

typedef struct tdPDB_ENTRY {
    OB ObHdr;
    QWORD qwHash;
    QWORD vaModuleBase;
    LPSTR szModuleName;
    LPSTR szName;
    BYTE pbGUID[16];
    DWORD dwAge;
    DWORD cbModuleSize;
    // load data below
    BOOL fLoadFailed;
    LPSTR szPath;
    QWORD qwLoadAddress;
} PDB_ENTRY, *PPDB_ENTRY;

const LPSTR szVMMWIN_PDB_FUNCTIONS[] = {
    "SymGetOptions",
    "SymSetOptions",
    "SymInitialize",
    "SymCleanup",
    "SymFindFileInPath",
    "SymLoadModuleEx",
    "SymUnloadModule64",
    "SymEnumSymbols",
    "SymEnumTypesByName",
    "SymGetTypeFromName",
    "SymGetTypeInfo",
    "SymGetTypeInfoEx",
    "SymFromAddr",
};

typedef struct tdVMMWIN_PDB_FUNCTIONS {
    DWORD(*SymGetOptions)(VOID);
    DWORD(*SymSetOptions)(_In_ DWORD SymOptions);
    BOOL(*SymInitialize)(_In_ HANDLE hProcess, _In_opt_ PCSTR UserSearchPath, _In_ BOOL fInvadeProcess);
    BOOL(*SymCleanup)(_In_ HANDLE hProcess);
    BOOL(*SymFindFileInPath)(_In_ HANDLE hprocess, _In_opt_ PCSTR SearchPath, _In_ PCSTR FileName, _In_opt_ PVOID id, _In_ DWORD two, _In_ DWORD three, _In_ DWORD flags, _Out_writes_(MAX_PATH + 1) PSTR FoundFile, _In_opt_ PFINDFILEINPATHCALLBACK callback, _In_opt_ PVOID context);
    DWORD64(*SymLoadModuleEx)(_In_ HANDLE hProcess, _In_opt_ HANDLE hFile, _In_opt_ PCSTR ImageName, _In_opt_ PCSTR ModuleName, _In_ DWORD64 BaseOfDll, _In_ DWORD DllSize, _In_opt_ PMODLOAD_DATA Data, _In_opt_ DWORD Flags);
    BOOL(*SymUnloadModule64)(_In_ HANDLE hProcess, _In_ DWORD64 BaseOfDll);
    BOOL(*SymEnumSymbols)(_In_ HANDLE hProcess, _In_ ULONG64 BaseOfDll, _In_opt_ PCSTR Mask, _In_ PSYM_ENUMERATESYMBOLS_CALLBACK EnumSymbolsCallback, _In_opt_ PVOID UserContext);
    BOOL(*SymEnumTypesByName)(_In_ HANDLE hProcess, _In_ ULONG64 BaseOfDll, _In_opt_ PCSTR mask, _In_ PSYM_ENUMERATESYMBOLS_CALLBACK EnumSymbolsCallback, _In_opt_ PVOID UserContext);
    BOOL(*SymGetTypeFromName)(_In_ HANDLE hProcess, _In_ ULONG64 BaseOfDll, _In_ PCSTR Name, _Inout_ PSYMBOL_INFO);
    BOOL(*SymGetTypeInfo)(_In_ HANDLE hProcess, _In_ DWORD64 ModBase, _In_ ULONG TypeId, _In_ IMAGEHLP_SYMBOL_TYPE_INFO GetType, _Out_ PVOID pInfo);
    BOOL(*SymGetTypeInfoEx)(_In_ HANDLE hProcess, _In_ DWORD64 ModBase, _Inout_ PIMAGEHLP_GET_TYPE_INFO_PARAMS Params);
    BOOL(*SymFromAddr)(_In_ HANDLE hProcess, _In_ DWORD64 Address, _Out_ PDWORD64 Displacement, _Out_ PSYMBOL_INFO Symbol);
} VMMWIN_PDB_FUNCTIONS, *PVMMWIN_PDB_FUNCTIONS;

typedef struct tdVMMWIN_PDB_CONTEXT {
    BOOL fDisabled;
    HANDLE hSym;
    HMODULE hModuleSymSrv;
    HMODULE hModuleDbgHelp;
    CRITICAL_SECTION Lock;
    POB_MAP pmPdbByHash;
    POB_MAP pmPdbByModule;
    QWORD qwLoadAddressNext;
    union {
        VMMWIN_PDB_FUNCTIONS pfn;
        QWORD vafn[sizeof(VMMWIN_PDB_FUNCTIONS) / sizeof(PVOID)];
    };
} VMMWIN_PDB_CONTEXT, *PVMMWIN_PDB_CONTEXT;

typedef struct tdVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS {
    PHANDLE phEventThreadStarted;
    BOOL fPdbInfo;
    PE_CODEVIEW_INFO PdbInfo;
} VMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS, *PVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS;

QWORD PDB_HashPdb(_In_ LPSTR szPdbName, _In_reads_(16) PBYTE pbPdbGUID, _In_ DWORD dwPdbAge)
{
    QWORD qwHash = 0;
    qwHash = Util_HashStringA(szPdbName);
    qwHash = dwPdbAge + ((qwHash >> 13) | (qwHash << 51));
    qwHash = *(PQWORD)pbPdbGUID + ((qwHash >> 13) | (qwHash << 51));
    qwHash = *(PQWORD)(pbPdbGUID + 8) + ((qwHash >> 13) | (qwHash << 51));
    return qwHash;
}

DWORD PDB_HashModuleName(_In_ LPSTR szModuleName)
{
    DWORD i, c, dwHash = 0;
    WCHAR wszBuffer[MAX_PATH];
    c = Util_PathFileNameFix_Registry(wszBuffer, szModuleName, NULL, 0, 0, TRUE);
    for(i = 0; i < c; i++) {
        dwHash = ((dwHash >> 13) | (dwHash << 19)) + wszBuffer[i];
    }
    return dwHash;
}

VOID PDB_CallbackCleanup_ObPdbEntry(PPDB_ENTRY pOb)
{
    LocalFree(pOb->szModuleName);
    LocalFree(pOb->szName);
    LocalFree(pOb->szPath);
}

/*
* Add a module to the PDB database and return its handle.
* NB! The PDB for the added module won't be loaded until required.
* -- vaModuleBase
* -- cbModuleSize = optional size of the module (required if using GetSymbolFromAddress functionality).
* -- szModuleName
* -- szPdbName
* -- pbPdbGUID
* -- dwPdbAge
* -- return = The PDB handle on success (no need to close handle); or zero on fail.
*/
PDB_HANDLE PDB_AddModuleEntry(_In_ QWORD vaModuleBase, _In_opt_ DWORD cbModuleSize, _In_ LPSTR szModuleName, _In_ LPSTR szPdbName, _In_reads_(16) PBYTE pbPdbGUID, _In_ DWORD dwPdbAge)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry;
    QWORD qwPdbHash;
    if(!ctx) { return 0; }
    qwPdbHash = PDB_HashPdb(szPdbName, pbPdbGUID, dwPdbAge);
    EnterCriticalSection(&ctx->Lock);
    if(!ObMap_ExistsKey(ctx->pmPdbByHash, qwPdbHash)) {
        pObPdbEntry = Ob_Alloc(OB_TAG_PDB_ENTRY, LMEM_ZEROINIT, sizeof(PDB_ENTRY), PDB_CallbackCleanup_ObPdbEntry, NULL);
        if(!pObPdbEntry) {
            LeaveCriticalSection(&ctx->Lock);
            return 0;
        }
        pObPdbEntry->dwAge = dwPdbAge;
        pObPdbEntry->qwHash = qwPdbHash;
        memcpy(pObPdbEntry->pbGUID, pbPdbGUID, 16);
        pObPdbEntry->szName = Util_StrDupA(szPdbName);
        pObPdbEntry->szModuleName = Util_StrDupA(szModuleName);
        pObPdbEntry->vaModuleBase = vaModuleBase;
        pObPdbEntry->cbModuleSize = cbModuleSize;
        ObMap_Push(ctx->pmPdbByHash, qwPdbHash, pObPdbEntry);
        ObMap_Push(ctx->pmPdbByModule, PDB_HashModuleName(szModuleName), pObPdbEntry);
        Ob_DECREF(pObPdbEntry);
    }
    LeaveCriticalSection(&ctx->Lock);
    return qwPdbHash;
}

/*
* Retrieve a PDB handle given a process and module base address. If the handle
* is not found in the database an attempt to automatically add it is performed.
* NB! Only one PDB with the same base address may exist regardless of process.
* NB! The PDB for the added module won't be loaded until required.
* -- pProcess
* -- vaModuleBase
* -- return = The PDB handle on success (no need to close handle); or zero on fail.
*/
PDB_HANDLE PDB_GetHandleFromModuleAddress(_In_ PVMM_PROCESS pProcess, _In_ QWORD vaModuleBase)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry = 0;
    PE_CODEVIEW_INFO CodeViewInfo = { 0 };
    DWORD i, iMax;
    QWORD qwPdbHash, cbModuleSize;
    CHAR szModuleName[MAX_PATH], *szPdbText;
    if(!ctx) { return 0; }
    // find: module base address already in .pdb database.
    for(i = 0, iMax = ObMap_Size(ctx->pmPdbByHash); i < iMax; i++) {
        if((pObPdbEntry = ObMap_GetByIndex(ctx->pmPdbByHash, i))) {
            if(vaModuleBase == pObPdbEntry->vaModuleBase) {
                qwPdbHash = pObPdbEntry->qwHash;
                Ob_DECREF_NULL(&pObPdbEntry);
                return qwPdbHash;
            }
            Ob_DECREF_NULL(&pObPdbEntry);
        }
    }
    // retrieve codeview and add to .pdb database.
    if(!(cbModuleSize = PE_GetSize(pProcess, vaModuleBase)) || (cbModuleSize > 0x04000000)) { return 0; }
    if(!PE_GetCodeViewInfo(pProcess, vaModuleBase, NULL, &CodeViewInfo)) { return 0; }
    strcpy_s(szModuleName, MAX_PATH, CodeViewInfo.CodeView.PdbFileName);
    if((szPdbText = strstr(szModuleName, ".pdb"))) {
        szPdbText[0] = 0;
    }
    return PDB_AddModuleEntry(
        vaModuleBase,
        (DWORD)cbModuleSize,
        szModuleName,
        CodeViewInfo.CodeView.PdbFileName,
        CodeViewInfo.CodeView.Guid,
        CodeViewInfo.CodeView.Age
    );
}

/*
* Retrieve a PDB handle from an already added module.
* NB! If multiple modules exists with the same name the 1st module to be added
*     is returned.
* -- szModuleName
* -- return = The PDB handle on success (no need to close handle); or zero on fail.
*/
PDB_HANDLE PDB_GetHandleFromModuleName(_In_ LPSTR szModuleName)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry;
    QWORD qwHashPdb;
    DWORD dwHashModule;
    if(!ctx || ctx->fDisabled) { return 0; }
    if(!szModuleName || !strcmp("nt", szModuleName)) {
        szModuleName = "ntoskrnl";
    }
    dwHashModule = PDB_HashModuleName(szModuleName);
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByModule, dwHashModule))) { return 0; }
    qwHashPdb = pObPdbEntry->fLoadFailed ? 0 : pObPdbEntry->qwHash;
    Ob_DECREF(pObPdbEntry);
    return qwHashPdb;
}

/*
* Ensure that the PDB_ENTRY have its symbols loaded into memory.
* NB! this function must be called in a single-threaded context!
* -- pPdbEntry
* -- return
*/
_Success_(return)
BOOL PDB_LoadEnsureEx(_In_ PPDB_ENTRY pPdbEntry)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    CHAR szPdbPath[MAX_PATH + 1];
    if(!ctx || pPdbEntry->fLoadFailed) { return FALSE; }
    if(pPdbEntry->qwLoadAddress) { return TRUE; }
    if(!ctx->pfn.SymFindFileInPath(ctx->hSym, NULL, pPdbEntry->szName, pPdbEntry->pbGUID, pPdbEntry->dwAge, 0, SSRVOPT_GUIDPTR, szPdbPath, NULL, NULL)) { goto fail; }
    pPdbEntry->szPath = Util_StrDupA(szPdbPath);
    pPdbEntry->qwLoadAddress = ctx->pfn.SymLoadModuleEx(ctx->hSym, NULL, szPdbPath, NULL, ctx->qwLoadAddressNext, pPdbEntry->cbModuleSize, NULL, 0);
    ctx->qwLoadAddressNext += VMMWIN_PDB_LOAD_ADDRESS_STEP;
    if(!pPdbEntry->szPath || !pPdbEntry->qwLoadAddress) { goto fail; }
    return TRUE;
fail:
    pPdbEntry->fLoadFailed = TRUE;
    return FALSE;
}

/*
* Ensure that the PDB_HANDLE have its symbols loaded into memory.
* -- hPDB
* -- return
*/
_Success_(return)
BOOL PDB_LoadEnsure(_In_opt_ PDB_HANDLE hPDB)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry;
    BOOL fResult;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl"); }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    EnterCriticalSection(&ctx->Lock);
    fResult = PDB_LoadEnsureEx(pObPdbEntry);
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

/*
* Return the module name given a PDB handle.
* -- hPDB
* -- szModuleName = buffer to receive module name upon success.
* -- return
*/
_Success_(return)
BOOL PDB_GetModuleName(_In_opt_ PDB_HANDLE hPDB, _Out_writes_(MAX_PATH) LPSTR szModuleName)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl"); }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    strncpy_s(szModuleName, MAX_PATH, pObPdbEntry->szModuleName, MAX_PATH - 1);
    Ob_DECREF(pObPdbEntry);
    return TRUE;
}

/*
* Callback function for PDB_GetSymbolOffset() / SymEnumSymbols()
*/
BOOL PDB_GetSymbolOffset_Callback(_In_ PSYMBOL_INFO pSymInfo, _In_ ULONG SymbolSize, _In_ PDWORD pdwSymbolOffset)
{
    if(pSymInfo->Address - pSymInfo->ModBase < 0x10000000) {
        *pdwSymbolOffset = (DWORD)(pSymInfo->Address - pSymInfo->ModBase);
    }
    return FALSE;
}

/*
* Query the PDB for the offset of a symbol.
* -- hPDB
* -- szSymbolName
* -- pdwSymbolOffset
* -- return
*/
_Success_(return)
BOOL PDB_GetSymbolOffset(_In_opt_ PDB_HANDLE hPDB, _In_ LPSTR szSymbolName, _Out_ PDWORD pdwSymbolOffset)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    SYMBOL_INFO SymbolInfo = { 0 };
    PPDB_ENTRY pObPdbEntry = NULL;
    BOOL fResult = FALSE;
    *pdwSymbolOffset = 0;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl"); }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    EnterCriticalSection(&ctx->Lock);
    if(!PDB_LoadEnsureEx(pObPdbEntry)) { goto fail; }
    if(ctxVmm->f32) {
        // 32-bit: use slower algo since it's working ...
        if(!ctx->pfn.SymEnumSymbols(ctx->hSym, pObPdbEntry->qwLoadAddress, szSymbolName, PDB_GetSymbolOffset_Callback, pdwSymbolOffset)) { goto fail; }
        if(!*pdwSymbolOffset) { goto fail; }
    } else {
        // 64-bit: use faster algo
        SymbolInfo.SizeOfStruct = sizeof(SYMBOL_INFO);
        if(!ctx->pfn.SymGetTypeFromName(ctx->hSym, pObPdbEntry->qwLoadAddress, szSymbolName, &SymbolInfo)) { goto fail; }
        *pdwSymbolOffset = (DWORD)(SymbolInfo.Address - SymbolInfo.ModBase);
    }
    fResult = TRUE;
fail:
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

/*
* Query the PDB for the offset of a symbol and return its virtual address. If
* szSymbolName contains wildcard '?*' characters and matches multiple symbols
* the virtual address of the 1st symbol is returned.
* -- hPDB
* -- szSymbolName = wildcard symbol name
* -- pvaSymbolAddress
* -- return
*/
_Success_(return)
BOOL PDB_GetSymbolAddress(_In_opt_ PDB_HANDLE hPDB, _In_ LPSTR szSymbolName, _Out_ PQWORD pvaSymbolAddress)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry = NULL;
    DWORD cbSymbolOffset;
    BOOL fResult = FALSE;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl"); }
    if(!PDB_GetSymbolOffset(hPDB, szSymbolName, &cbSymbolOffset)) { return FALSE; }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    *pvaSymbolAddress = pObPdbEntry->vaModuleBase + cbSymbolOffset;
    Ob_DECREF(pObPdbEntry);
    return TRUE;
}

/*
* Query the PDB for the closest symbol name given an offset from the module
* base address.
* -- hPDB
* -- dwSymbolOffset = the offset from the module base to query.
* -- szSymbolName = buffer to receive the name of the symbol.
* -- pdwSymbolDisplacement = displacement from the beginning of the symbol.
* -- return
*/
_Success_(return)
BOOL PDB_GetSymbolFromOffset(_In_opt_ PDB_HANDLE hPDB, _In_ DWORD dwSymbolOffset, _Out_writes_opt_(MAX_PATH) LPSTR szSymbolName, _Out_opt_ PDWORD pdwSymbolDisplacement)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    SYMBOL_INFO_PACKAGE SymbolInfo = { 0 };
    QWORD cch, qwDisplacement;
    PPDB_ENTRY pObPdbEntry = NULL;
    BOOL fResult = FALSE;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl"); }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    EnterCriticalSection(&ctx->Lock);
    if(!PDB_LoadEnsureEx(pObPdbEntry)) { goto fail; }
    SymbolInfo.si.SizeOfStruct = sizeof(SYMBOL_INFO);
    SymbolInfo.si.MaxNameLen = MAX_SYM_NAME;
    if(!ctx->pfn.SymFromAddr(ctx->hSym, pObPdbEntry->qwLoadAddress + dwSymbolOffset, &qwDisplacement, &SymbolInfo.si)) { goto fail; }
    if(szSymbolName) {
        cch = min(MAX_PATH - 1, SymbolInfo.si.NameLen);
        memcpy(szSymbolName, SymbolInfo.si.Name, cch);
        szSymbolName[cch] = 0;
    }
    if(pdwSymbolDisplacement) {
        *pdwSymbolDisplacement = (DWORD)qwDisplacement;
    }
    fResult = TRUE;
fail:
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

/*
* Read memory at the PDB acquired symbol offset. If szSymbolName contains
* wildcard '?*' characters and matches multiple symbols the offset of the
* 1st symbol is used to read the memory.
* -- hPDB
* -- szSymbolName = wildcard symbol name
* -- pProcess
* -- pb
* -- cb
* -- return
*/
_Success_(return)
BOOL PDB_GetSymbolPBYTE(_In_opt_ PDB_HANDLE hPDB, _In_ LPSTR szSymbolName, _In_ PVMM_PROCESS pProcess, _Out_writes_(cb) PBYTE pb, _In_ DWORD cb)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PPDB_ENTRY pObPdbEntry = NULL;
    DWORD dwSymbolOffset;
    BOOL fResult;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl"); }
    if(!PDB_GetSymbolOffset(hPDB, szSymbolName, &dwSymbolOffset)) { return FALSE; }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    fResult = VmmRead(pProcess, pObPdbEntry->vaModuleBase + dwSymbolOffset, pb, cb);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

/*
* Query the PDB for the size of a type. If szTypeName contains wildcard '?*'
* characters and matches multiple types the size of the 1st type is returned.
* -- hPDB
* -- szTypeName = wildcard type name
* -- pdwTypeSize
* -- return
*/
_Success_(return)
BOOL PDB_GetTypeSize(_In_opt_ PDB_HANDLE hPDB, _In_ LPSTR szTypeName, _Out_ PDWORD pdwTypeSize)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    SYMBOL_INFO SymbolInfo = { 0 };
    PPDB_ENTRY pObPdbEntry = NULL;
    BOOL fResult = FALSE;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl"); }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    EnterCriticalSection(&ctx->Lock);
    if(!PDB_LoadEnsureEx(pObPdbEntry)) { goto fail; }
    SymbolInfo.SizeOfStruct = sizeof(SYMBOL_INFO);
    fResult = ctx->pfn.SymGetTypeFromName(ctx->hSym, pObPdbEntry->qwLoadAddress, szTypeName, &SymbolInfo) && SymbolInfo.Size;
    *pdwTypeSize = SymbolInfo.Size;
fail:
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

_Success_(return)
BOOL PDB_GetTypeSizeShort(_In_opt_ PDB_HANDLE hPDB, _In_ LPSTR szTypeName, _Out_ PWORD pwTypeSize)
{
    DWORD dwTypeSize;
    if(!PDB_GetTypeSize(hPDB, szTypeName, &dwTypeSize) || (dwTypeSize > 0xffff)) { return FALSE; }
    if(pwTypeSize) { *pwTypeSize = (WORD)dwTypeSize; }
    return TRUE;
}

/*
* Callback function for PDB_GetTypeChildOffset()
*/
BOOL PDB_GetTypeChildOffset_Callback(_In_ PSYMBOL_INFO pSymInfo, _In_ ULONG SymbolSize, _In_ PDWORD pdwTypeId)
{
    *pdwTypeId = pSymInfo->Index;
    pSymInfo->Index;
    return FALSE;
}

/*
* Query the PDB for the offset of a child inside a type - often inside a struct.
* If szTypeName contains wildcard '?*' characters and matches multiple types the
* first type is queried for children. The child name must match exactly.
* -- hPDB
* -- szTypeName = wildcard type name.
* -- wszTypeChildName = exact match of child name.
* -- pdwTypeOffset = offset relative to type base.
* -- return
*/
_Success_(return)
BOOL PDB_GetTypeChildOffset(_In_opt_ PDB_HANDLE hPDB, _In_ LPSTR szTypeName, _In_ LPWSTR wszTypeChildName, _Out_ PDWORD pdwTypeOffset)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    BOOL fResult = FALSE;
    LPWSTR wszTypeChildSymName;
    PPDB_ENTRY pObPdbEntry = NULL;
    DWORD dwTypeId, cTypeChildren, iTypeChild;
    TI_FINDCHILDREN_PARAMS *pFindChildren = NULL;
    if(!ctx || ctx->fDisabled || !hPDB) { return FALSE; }
    if(hPDB == PDB_HANDLE_KERNEL) { hPDB = PDB_GetHandleFromModuleName("ntoskrnl"); }
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    EnterCriticalSection(&ctx->Lock);
    if(!PDB_LoadEnsureEx(pObPdbEntry)) { goto fail; }
    if(!ctx->pfn.SymEnumTypesByName(ctx->hSym, pObPdbEntry->qwLoadAddress, szTypeName, PDB_GetTypeChildOffset_Callback, &dwTypeId) || !dwTypeId) { goto fail; }
    if(!ctx->pfn.SymGetTypeInfo(ctx->hSym, pObPdbEntry->qwLoadAddress, dwTypeId, TI_GET_CHILDRENCOUNT, &cTypeChildren) || !cTypeChildren) { goto fail; }
    if(!(pFindChildren = LocalAlloc(LMEM_ZEROINIT, sizeof(TI_FINDCHILDREN_PARAMS) + cTypeChildren * sizeof(ULONG)))) { goto fail; }
    pFindChildren->Count = cTypeChildren;
    if(!ctx->pfn.SymGetTypeInfo(ctx->hSym, pObPdbEntry->qwLoadAddress, dwTypeId, TI_FINDCHILDREN, pFindChildren)) { goto fail; }
    for(iTypeChild = 0; iTypeChild < cTypeChildren; iTypeChild++) {
        if(!ctx->pfn.SymGetTypeInfo(ctx->hSym, pObPdbEntry->qwLoadAddress, pFindChildren->ChildId[iTypeChild], TI_GET_SYMNAME, &wszTypeChildSymName)) { continue; }
        if(!wcscmp(wszTypeChildName, wszTypeChildSymName)) {
            if(ctx->pfn.SymGetTypeInfo(ctx->hSym, pObPdbEntry->qwLoadAddress, pFindChildren->ChildId[iTypeChild], TI_GET_OFFSET, pdwTypeOffset)) {
                LocalFree(wszTypeChildSymName);
                fResult = TRUE;
                break;
            }
        }
        LocalFree(wszTypeChildSymName);
    }
fail:
    LocalFree(pFindChildren);
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObPdbEntry);
    return fResult;
}

_Success_(return)
BOOL PDB_GetTypeChildOffsetShort(_In_opt_ PDB_HANDLE hPDB, _In_ LPSTR szTypeName, _In_ LPWSTR wszTypeChildName, _Out_ PWORD pwTypeOffset)
{
    DWORD dwTypeOffset;
    if(!PDB_GetTypeChildOffset(hPDB, szTypeName, wszTypeChildName, &dwTypeOffset) || (dwTypeOffset > 0xffff)) { return FALSE; }
    if(pwTypeOffset) { *pwTypeOffset = (WORD)dwTypeOffset; }
    return TRUE;
}



//-----------------------------------------------------------------------------
// INITIALIZATION/REFRESH/CLOSE FUNCTIONALITY BELOW:
//-----------------------------------------------------------------------------

/*
* Cleanup the PDB sub-system. This should ideally be done on Vmm Close().
*/
VOID PDB_Close()
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    if(!ctx) { return; }
    ctxVmm->pPdbContext = NULL;
    EnterCriticalSection(&ctx->Lock);
    LeaveCriticalSection(&ctx->Lock);
    DeleteCriticalSection(&ctx->Lock);
    if(ctx->hSym) {
        ctx->pfn.SymCleanup(ctx->hSym);
    }
    Ob_DECREF(ctx->pmPdbByHash);
    Ob_DECREF(ctx->pmPdbByModule);
    if(ctx->hModuleDbgHelp) { FreeLibrary(ctx->hModuleDbgHelp); }
    if(ctx->hModuleSymSrv) { FreeLibrary(ctx->hModuleSymSrv); }
    ZeroMemory(ctx, sizeof(VMMWIN_PDB_CONTEXT));
    ctxMain->pdb.fInitialized = FALSE;
}

/*
* 
*/
_Success_(return)
BOOL PDB_Initialize_Async_Kernel_ScanForPdbInfo(_In_ PVMM_PROCESS pSystemProcess, _Out_ PPE_CODEVIEW_INFO pCodeViewInfo)
{
    PBYTE pb = NULL;
    DWORD i, cbRead;
    PPE_CODEVIEW pPdb;
    ZeroMemory(pCodeViewInfo, sizeof(PE_CODEVIEW_INFO));
    if(!ctxVmm->kernel.vaBase) { return FALSE; }
    if(!(pb = LocalAlloc(0, 0x00800000))) { return FALSE; }
    VmmReadEx(pSystemProcess, ctxVmm->kernel.vaBase, pb, 0x00800000, &cbRead, VMM_FLAG_ZEROPAD_ON_FAIL);
    // 1: search for pdb debug information adn extract offset of PsInitialSystemProcess
    for(i = 0; i < 0x00800000 - sizeof(PE_CODEVIEW); i += 4) {
        pPdb = (PPE_CODEVIEW)(pb + i);
        if(pPdb->Signature == 0x53445352) {
            if(pPdb->Age > 0x20) { continue; }
            if(memcmp("nt", pPdb->PdbFileName, 2)) { continue; }
            if(memcmp(".pdb", pPdb->PdbFileName + 8, 5)) { continue; }
            pCodeViewInfo->SizeCodeView = 4 + 16 + 4 + 12;
            pCodeViewInfo->CodeView.Signature = pPdb->Signature;
            memcpy(pCodeViewInfo->CodeView.Guid, pPdb->Guid, 16);
            pCodeViewInfo->CodeView.Age = pPdb->Age;
            memcpy(pCodeViewInfo->CodeView.PdbFileName, pPdb->PdbFileName, 12);
            LocalFree(pb);
            return TRUE;
        }
    }
    LocalFree(pb);
    return FALSE;
}

VOID PDB_Initialize_WaitComplete()
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    if(ctx && ctxMain->pdb.fEnable) {
        EnterCriticalSection(&ctx->Lock);
        LeaveCriticalSection(&ctx->Lock);
    }
}

/*
* Asynchronous initialization of the PDB for the kernel. This is done async
* since it may take some time to load the PDB from the Microsoft Symbol server.
* Once this initializtion is successfully completed the fDisabled flag will be
* removed - allowing other threads to use the PDB subsystem.
* If the initialization fails it's assume the PDB system should be disabled.
* -- hEventThreadStart
* -- return
*/
DWORD PDB_Initialize_Async_Kernel_ThreadProc(LPVOID lpParameter)
{
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS pKernelParameters = (PVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS)lpParameter;
    DWORD dwReturnStatus = 0;
    PVMM_PROCESS pObSystemProcess = NULL;
    PPDB_ENTRY pObKernelEntry = NULL;
    QWORD qwPdbHash;
    if(!ctx) { return 0; }
    EnterCriticalSection(&ctx->Lock);
    SetEvent(*pKernelParameters->phEventThreadStarted);
    if(!(pObSystemProcess = VmmProcessGet(4))) { goto fail; }
    pKernelParameters->fPdbInfo = pKernelParameters->fPdbInfo ||
        PE_GetCodeViewInfo(pObSystemProcess, ctxVmm->kernel.vaBase, NULL, &pKernelParameters->PdbInfo) ||
        PDB_Initialize_Async_Kernel_ScanForPdbInfo(pObSystemProcess, &pKernelParameters->PdbInfo);
    if(!pKernelParameters->fPdbInfo) {
        vmmprintf("%s         Reason: Unable to locate debugging information in kernel image.\n", VMMWIN_PDB_WARN_DEFAULT);
        goto fail;
    }
    qwPdbHash = PDB_AddModuleEntry(ctxVmm->kernel.vaBase, (DWORD)ctxVmm->kernel.cbSize, "ntoskrnl", pKernelParameters->PdbInfo.CodeView.PdbFileName, pKernelParameters->PdbInfo.CodeView.Guid, pKernelParameters->PdbInfo.CodeView.Age);
    pObKernelEntry = ObMap_GetByKey(ctx->pmPdbByHash, qwPdbHash);
    if(!pObKernelEntry) {
        vmmprintf("%s         Reason: Failed creating initial PDB entry.\n", VMMWIN_PDB_WARN_DEFAULT);
        goto fail;
    }
    if(!PDB_LoadEnsureEx(pObKernelEntry)) {
        vmmprintf("%s         Reason: Unable to download kernel symbols to cache from Symbol Server.\n", VMMWIN_PDB_WARN_DEFAULT);
        goto fail;
    }
    vmmprintfvv_fn("Initialization of debug symbol .pdb functionality completed.\n    [ %s ]\n", ctxMain->pdb.szSymbolPath);
    ctx->fDisabled = FALSE;
    dwReturnStatus = 1;
    // fall-through to fail for cleanup
fail:
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObKernelEntry);
    Ob_DECREF(pObSystemProcess);
    LocalFree(pKernelParameters);
    return dwReturnStatus;
}

VOID PDB_Initialize_InitialValues()
{
    HKEY hKey;
    DWORD cbData, dwEnableSymbols, dwEnableSymbolServer;
    // 1: try load values from registry
    if(!ctxMain->pdb.fInitialized) {
        ctxMain->pdb.fEnable = 1;
        ctxMain->pdb.fServerEnable = !ctxMain->cfg.fDisableSymbolServerOnStartup;
    }
    ctxMain->pdb.szLocal[0] = 0;
    ctxMain->pdb.szServer[0] = 0;
    dwEnableSymbols = ctxMain->pdb.fEnable ? 1 : 0;
    dwEnableSymbolServer = ctxMain->pdb.fServerEnable ? 1 : 0;
    if(ERROR_SUCCESS == RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\UlfFrisk\\MemProcFS", 0, KEY_READ, &hKey)) {
        cbData = _countof(ctxMain->pdb.szLocal) - 1;
        RegQueryValueExA(hKey, "SymbolCache", NULL, NULL, (PBYTE)ctxMain->pdb.szLocal, &cbData);
        if(cbData < 3) { ctxMain->pdb.szLocal[0] = 0; }
        cbData = _countof(ctxMain->pdb.szServer) - 1;
        RegQueryValueExA(hKey, "SymbolServer", NULL, NULL, (PBYTE)ctxMain->pdb.szServer, &cbData);
        if(cbData < 3) { ctxMain->pdb.szServer[0] = 0; }
        if(ctxMain->pdb.fEnable) {
            cbData = sizeof(DWORD);
            RegQueryValueExA(hKey, "SymbolEnable", NULL, NULL, (PBYTE)&dwEnableSymbols, &cbData);
        }
        if(ctxMain->pdb.fServerEnable) {
            cbData = sizeof(DWORD);
            RegQueryValueExA(hKey, "SymbolServerEnable", NULL, NULL, (PBYTE)&dwEnableSymbolServer, &cbData);
        }
        RegCloseKey(hKey);
    }
    // 2: set default values (if not already loaded from registry)
    if(!ctxMain->pdb.szLocal[0]) {
        Util_GetPathDll(ctxMain->pdb.szLocal, ctxVmm->hModuleVmm);
        strncat_s(ctxMain->pdb.szLocal, _countof(ctxMain->pdb.szLocal), "Symbols", _TRUNCATE);
    }
    if(!ctxMain->pdb.szServer[0]) {
        strncpy_s(ctxMain->pdb.szServer, _countof(ctxMain->pdb.szServer), "https://msdl.microsoft.com/download/symbols", _TRUNCATE);
    }
    // 3: set final values
    ctxMain->pdb.fEnable = (dwEnableSymbols == 1);
    ctxMain->pdb.fServerEnable = (dwEnableSymbolServer == 1);
    strncpy_s(ctxMain->pdb.szSymbolPath, _countof(ctxMain->pdb.szSymbolPath), "srv*", _TRUNCATE);
    strncat_s(ctxMain->pdb.szSymbolPath, _countof(ctxMain->pdb.szSymbolPath), ctxMain->pdb.szLocal, _TRUNCATE);
    if(ctxMain->pdb.fServerEnable) {
        strncat_s(ctxMain->pdb.szSymbolPath, _countof(ctxMain->pdb.szSymbolPath), "*", _TRUNCATE);
        strncat_s(ctxMain->pdb.szSymbolPath, _countof(ctxMain->pdb.szSymbolPath), ctxMain->pdb.szServer, _TRUNCATE);
    }
    ctxMain->pdb.fInitialized = TRUE;
}

/*
* Update the PDB configuration. The PDB syb-system will be reloaded on
* configuration changes - which may cause a short interruption for any
* caller.
*/
VOID PDB_ConfigChange()
{
    HKEY hKey;
    CHAR szLocalPath[MAX_PATH] = { 0 };
    // update new values in registry
    if(ERROR_SUCCESS == RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\UlfFrisk\\MemProcFS", 0, NULL, 0, KEY_ALL_ACCESS, NULL, &hKey, NULL)) {
        Util_GetPathDll(szLocalPath, ctxVmm->hModuleVmm);
        if(strncmp(szLocalPath, ctxMain->pdb.szLocal, strlen(szLocalPath) - 1) && !_access_s(ctxMain->pdb.szLocal, 06)) {
            RegSetValueExA(hKey, "SymbolCache", 0, REG_SZ, (PBYTE)ctxMain->pdb.szLocal, (DWORD)strlen(ctxMain->pdb.szLocal));
        } else {
            RegSetValueExA(hKey, "SymbolCache", 0, REG_SZ, (PBYTE)"", 0);
        }
        if((!strncmp("http://", ctxMain->pdb.szServer, 7) || !strncmp("https://", ctxMain->pdb.szServer, 8)) && !strstr(ctxMain->pdb.szServer, "msdl.microsoft.com")) {
            RegSetValueExA(hKey, "SymbolServer", 0, REG_SZ, (PBYTE)ctxMain->pdb.szServer, (DWORD)strlen(ctxMain->pdb.szServer));
        } else {
            RegSetValueExA(hKey, "SymbolServer", 0, REG_SZ, (PBYTE)"", 0);
        }
        RegCloseKey(hKey);
    }
    // refresh values and reload!
    EnterCriticalSection(&ctxVmm->LockMaster);
    PDB_Close();
    PDB_Initialize(NULL, FALSE);
    LeaveCriticalSection(&ctxVmm->LockMaster);
}

/*
* Initialize the PDB sub-system. This should ideally be done on Vmm Init()
*/
VOID PDB_Initialize(_In_opt_ PPE_CODEVIEW_INFO pPdbInfoOpt, _In_ BOOL fInitializeKernelAsync)
{
    HANDLE hEventThreadStarted = 0;
    PVMMWIN_PDB_CONTEXT ctx = NULL;
    DWORD i, dwSymOptions;
    CHAR szPathSymSrv[MAX_PATH], szPathDbgHelp[MAX_PATH];
    PVMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS pKernelParameters = NULL;
    if(ctxMain->pdb.fInitialized) { return; }
    PDB_Initialize_InitialValues();
    if(!ctxMain->pdb.fEnable) { goto fail; }
    if(!(ctx = LocalAlloc(LMEM_ZEROINIT, sizeof(VMMWIN_PDB_CONTEXT)))) { goto fail; }
    if(!(ctx->pmPdbByHash = ObMap_New(OB_MAP_FLAGS_OBJECT_OB))) { goto fail; }
    if(!(ctx->pmPdbByModule = ObMap_New(OB_MAP_FLAGS_OBJECT_OB))) { goto fail; }
    // 1: dynamic load of dbghelp.dll and symsrv.dll from directory of vmm.dll - i.e. not from system32
    Util_GetPathDll(szPathSymSrv, ctxVmm->hModuleVmm);
    Util_GetPathDll(szPathDbgHelp, ctxVmm->hModuleVmm);
    strncat_s(szPathSymSrv, MAX_PATH, "symsrv.dll", _TRUNCATE);
    strncat_s(szPathDbgHelp, MAX_PATH, "dbghelp.dll", _TRUNCATE);
    ctx->hModuleSymSrv = LoadLibraryA(szPathSymSrv);
    ctx->hModuleDbgHelp = LoadLibraryA(szPathDbgHelp);
    if(!ctx->hModuleSymSrv || !ctx->hModuleDbgHelp) {
        vmmprintf("%s         Reason: Could not load PDB required files - symsrv.dll/dbghelp.dll.\n", VMMWIN_PDB_WARN_DEFAULT);
        goto fail;
    }
    for(i = 0; i < sizeof(VMMWIN_PDB_FUNCTIONS) / sizeof(PVOID); i++) {
        ctx->vafn[i] = (QWORD)GetProcAddress(ctx->hModuleDbgHelp, szVMMWIN_PDB_FUNCTIONS[i]);
        if(!ctx->vafn[i]) {
            vmmprintf("%s         Reason: Could not load function(s) from symsrv.dll/dbghelp.dll.\n", VMMWIN_PDB_WARN_DEFAULT);
            goto fail;
        }
    }
    // 2: initialize dbghelp.dll
    ctx->hSym = VMMWIN_PDB_FAKEPROCHANDLE;
    dwSymOptions = ctx->pfn.SymGetOptions();
    dwSymOptions &= ~SYMOPT_DEFERRED_LOADS;
    dwSymOptions &= ~SYMOPT_LOAD_LINES;
    dwSymOptions |= SYMOPT_CASE_INSENSITIVE;
    dwSymOptions |= SYMOPT_IGNORE_NT_SYMPATH;
    dwSymOptions |= SYMOPT_UNDNAME;
    ctx->pfn.SymSetOptions(dwSymOptions);
    if(!ctx->pfn.SymInitialize(ctx->hSym, ctxMain->pdb.szSymbolPath, FALSE)) {
        vmmprintf("%s         Reason: Failed to initialize Symbol Handler / dbghelp.dll.\n", VMMWIN_PDB_WARN_DEFAULT);
        ctx->hSym = NULL;
        goto fail;
    }
    // success - finish up and load kernel .pdb async (to optimize startup time).
    // pdb subsystem won't be fully initialized until before the kernel is loaded.
    if(!(hEventThreadStarted = CreateEvent(NULL, TRUE, FALSE, NULL))) { goto fail; }
    if(!(pKernelParameters = LocalAlloc(LMEM_ZEROINIT, sizeof(VMMWIN_PDB_INITIALIZE_KERNEL_PARAMETERS)))) { goto fail; }
    pKernelParameters->phEventThreadStarted = &hEventThreadStarted;
    if(pPdbInfoOpt) {
        pKernelParameters->fPdbInfo = TRUE;
        memcpy(&pKernelParameters->PdbInfo, pPdbInfoOpt, sizeof(PE_CODEVIEW_INFO));
    }
    InitializeCriticalSection(&ctx->Lock);
    ctx->qwLoadAddressNext = VMMWIN_PDB_LOAD_ADDRESS_BASE;
    ctx->fDisabled = TRUE;
    ctxVmm->pPdbContext = ctx;
    if(fInitializeKernelAsync) {
        VmmWork((LPTHREAD_START_ROUTINE)PDB_Initialize_Async_Kernel_ThreadProc, (LPVOID)pKernelParameters, NULL);
        WaitForSingleObject(hEventThreadStarted, 500);  // wait for async thread initialize thread to start (and acquire PDB lock).
    } else {
        PDB_Initialize_Async_Kernel_ThreadProc(pKernelParameters); // synchronous call
    }
    CloseHandle(hEventThreadStarted);
    return;
fail:
    if(hEventThreadStarted) { CloseHandle(hEventThreadStarted); }
    if(ctx) {
        if(ctx->hSym) {
            ctx->pfn.SymCleanup(ctx->hSym);
        }
        Ob_DECREF(ctx->pmPdbByHash);
        Ob_DECREF(ctx->pmPdbByModule);
        if(ctx->hModuleDbgHelp) { FreeLibrary(ctx->hModuleDbgHelp); }
        if(ctx->hModuleSymSrv) { FreeLibrary(ctx->hModuleSymSrv); }
    }
    LocalFree(ctx);
    LocalFree(pKernelParameters);
    ctxMain->pdb.fEnable = FALSE;
}



//-----------------------------------------------------------------------------
// DT - DISPLAY TYPE FUNCTIONALITY BELOW:
// The DisplayType functionality generates human readable type information for
// types in 'ntoskrnl.exe' only. The type information may optionally be decor-
// ated with values retrieved from memory.
//-----------------------------------------------------------------------------

typedef struct tdPDB_DT_CONTEXT {
    HANDLE hSym;
    ULONG64 BaseOfDll;
    PVMMWIN_PDB_FUNCTIONS pfn;
    LPSTR szu8;
    QWORD csz;
    QWORD cszMax;
    WCHAR wszln[MAX_PATH + 1];
    WCHAR wszBuffer[MAX_PATH + 1];
    BYTE iLevelMax;
} PDB_DT_CONTEXT, *PPDB_DT_CONTEXT;

typedef struct tdPDB_DT_INFO {
    LPWSTR wszName;
    LPWSTR wszTypeName;
    QWORD qwBitFieldLength;
    QWORD qwLength;
    DWORD dwTag;
    DWORD dwPtrTag;
    DWORD dwOffset;
    DWORD dwTypeIndex;
    DWORD dwBaseType;
    DWORD dwArrayCount;
    DWORD dwChildCount;
    DWORD dwPtrTypeIndex;
    DWORD dwArrayTypeIndex;
} PDB_DT_INFO, *PPDB_DT_INFO;

/*
* Retrieve the name of a type. I don't find this in a header so a lookup table it is...
*/
LPWSTR PDB_DisplayTypeNt_GetTypeName(_In_ PPDB_DT_INFO pDT, _In_ QWORD cbType)
{
    if(pDT->wszTypeName) { return pDT->wszTypeName; }
    switch(pDT->dwBaseType) {
        case 1: return L"void";
        case 2: return L"char";
        case 3: return L"wchar";
        case 8: return L"float";
        case 9: return L"bcd";
        case 10: return L"bool";
        case 25: return L"currency";
        case 26: return L"date";
        case 27: return L"variant";
        case 28: return L"complex";
        case 29: return L"bit";
        case 30: return L"BSTR";
        case 31: return L"HRESULT";
        case 6:
        case 13:
            switch(cbType) {
                case 1: return L"int8";
                case 2: return L"int16";
                case 4: return L"int32";
                case 8: return L"int64";
                default: return L"int??";
            }
        case 7:
        case 14:
            switch(cbType) {
                case 1: return L"byte";
                case 2: return L"word";
                case 4: return L"dword";
                case 8: return L"uint64";
                default: return L"uint??";
            }
        case -1: return L"function";
        case -2: return L"pointer";
        default: return L"???";
    }
}

/*
* Internal processing of a type - write result human readable data to a utf-8
* text buffer in the ctxDT context. This function makes multiple and possible
* also recursive lookups against dbghelp.dll.
*/
VOID PDB_DisplayTypeNt_DoWork(_Inout_ PPDB_DT_CONTEXT ctxDT, _In_ BYTE iLevel, _In_ DWORD dwTypeIndex, _In_ DWORD dwChildCount, _In_ LPWSTR wszTypeName, _In_opt_ PBYTE pbMEM, _In_ DWORD cbMEM)
{
    static const IMAGEHLP_SYMBOL_TYPE_INFO _INFO1_ReqKinds[] = {
        TI_GET_SYMNAME,
        TI_GET_LENGTH,    // bit length in bit-fields
        TI_GET_OFFSET,
        TI_GET_TYPEID,
    };
    static const ULONG_PTR _INFO1_ReqOffsets[] = {
        offsetof(PDB_DT_INFO, wszName),
        offsetof(PDB_DT_INFO, qwBitFieldLength),
        offsetof(PDB_DT_INFO, dwOffset),
        offsetof(PDB_DT_INFO, dwTypeIndex),
    };
    static const ULONG _INFO1_ReqSizes[] = {
        sizeof(LPWSTR),
        sizeof(QWORD),
        sizeof(DWORD),
        sizeof(DWORD),
        sizeof(DWORD),
    };
    static const IMAGEHLP_SYMBOL_TYPE_INFO _INFO2_ReqKinds[] = {
        TI_GET_SYMNAME,
        TI_GET_LENGTH,
        TI_GET_SYMTAG,
        TI_GET_COUNT,
        TI_GET_CHILDRENCOUNT,
        TI_GET_TYPE,
        TI_GET_ARRAYINDEXTYPEID,
        TI_GET_BASETYPE,
    };
    static const ULONG_PTR _INFO2_ReqOffsets[] = {
        offsetof(PDB_DT_INFO, wszTypeName),
        offsetof(PDB_DT_INFO, qwLength),
        offsetof(PDB_DT_INFO, dwTag),
        offsetof(PDB_DT_INFO, dwArrayCount),
        offsetof(PDB_DT_INFO, dwChildCount),
        offsetof(PDB_DT_INFO, dwPtrTypeIndex),
        offsetof(PDB_DT_INFO, dwArrayTypeIndex),
        offsetof(PDB_DT_INFO, dwBaseType),
    };
    static const ULONG _INFO2_ReqSizes[] = {
        sizeof(LPWSTR),
        sizeof(QWORD),
        sizeof(DWORD),
        sizeof(DWORD),
        sizeof(DWORD),
        sizeof(DWORD),
        sizeof(DWORD),
        sizeof(DWORD),
    };
    IMAGEHLP_GET_TYPE_INFO_PARAMS IP1 = { 0 };
    IMAGEHLP_GET_TYPE_INFO_PARAMS IP2 = { 0 };
    PDWORD pdwTypeIndex = NULL;
    DWORD i, cInfo = dwChildCount;
    PPDB_DT_INFO pe, pInfo = NULL;
    WCHAR wszBuffer[0x20];
    LPWSTR wszName, wszFormat;
    QWORD v, oln, cbType, cbTypeLast = 0, qwBitBase = 0, cbData, vaData;
    if(!(pInfo = LocalAlloc(LMEM_ZEROINIT, cInfo * sizeof(PDB_DT_INFO)))) { goto fail; }
    // 1: fetch info about "children" into INFO struct array.
    IP1.SizeOfStruct = sizeof(IMAGEHLP_GET_TYPE_INFO_PARAMS);
    IP1.Flags = IMAGEHLP_GET_TYPE_INFO_CHILDREN;
    IP1.NumIds = 1;
    IP1.TypeIds = &dwTypeIndex;
    IP1.TagFilter = (1ULL << SymTagDimension) - 1;
    IP1.NumReqs = _countof(_INFO1_ReqKinds);
    IP1.ReqKinds = (IMAGEHLP_SYMBOL_TYPE_INFO *)_INFO1_ReqKinds;
    IP1.ReqOffsets = (PULONG_PTR)_INFO1_ReqOffsets;
    IP1.ReqSizes = (PULONG)_INFO1_ReqSizes;
    IP1.ReqStride = sizeof(PDB_DT_INFO);
    IP1.BufferSize = cInfo * sizeof(PDB_DT_INFO);
    IP1.Buffer = pInfo;
    if(!ctxDT->pfn->SymGetTypeInfoEx(ctxDT->hSym, ctxDT->BaseOfDll, &IP1)) { goto fail; }
    if(dwChildCount != IP1.EntriesFilled) { goto fail; }
    // 2: fetch info about the types of the children into INFO struct array.
    if(!(pdwTypeIndex = LocalAlloc(0, cInfo * sizeof(DWORD)))) { goto fail; }
    for(i = 0; i < cInfo; i++) {
        pdwTypeIndex[i] = pInfo[i].dwTypeIndex;
    }
    IP2.SizeOfStruct = sizeof(IMAGEHLP_GET_TYPE_INFO_PARAMS);
    IP2.Flags = 0;
    IP2.NumIds = cInfo;
    IP2.TypeIds = pdwTypeIndex;
    IP2.TagFilter = (1ULL << SymTagDimension) - 1;
    IP2.NumReqs = _countof(_INFO2_ReqKinds);
    IP2.ReqKinds = (IMAGEHLP_SYMBOL_TYPE_INFO*)_INFO2_ReqKinds;
    IP2.ReqOffsets = (PULONG_PTR)_INFO2_ReqOffsets;
    IP2.ReqSizes = (PULONG)_INFO2_ReqSizes;
    IP2.ReqStride = sizeof(PDB_DT_INFO);
    IP2.BufferSize = cInfo * sizeof(PDB_DT_INFO);
    IP2.Buffer = pInfo;
    if(!ctxDT->pfn->SymGetTypeInfoEx(ctxDT->hSym, ctxDT->BaseOfDll, &IP2)) { goto fail; }
    if(dwChildCount != IP1.EntriesFilled) { goto fail; }
    // 3: interpret result:
    for(i = 0; i < cInfo; i++) {
        pe = pInfo + i;
        oln = 0;
        cbType = pe->dwArrayCount ? (pe->qwLength / pe->dwArrayCount) : pe->qwLength;
        if(!pe->wszTypeName && pe->dwPtrTypeIndex && ((pe->dwTag == SymTagArrayType) || (pe->dwTag == SymTagPointerType))) {
            ctxDT->pfn->SymGetTypeInfo(ctxDT->hSym, ctxDT->BaseOfDll, pe->dwPtrTypeIndex, TI_GET_SYMNAME, &pe->wszTypeName);
        }
        if(qwBitBase) {
            if((cbTypeLast != cbType) || (qwBitBase >= (cbType << 3)) || ((cbType != 1) && (cbType != 2) && (cbType != 4) && (cbType != 8))) {
                qwBitBase = 0;
            }
        }
        // line: offset + name
        oln += _snwprintf_s(ctxDT->wszln + oln, MAX_PATH - oln, _TRUNCATE, L"%*s  +0x%03x %-*s : ", iLevel * 2, L"", pe->dwOffset, 24 - iLevel * 2, pe->wszName);
        // line: optional type array prefix
        if(pe->dwArrayCount) {
            oln += _snwprintf_s(ctxDT->wszln + oln, MAX_PATH - oln, _TRUNCATE, L"[%i] ", pe->dwArrayCount);
        }
        // line: optional type ptr prefix
        if(pe->dwPtrTypeIndex) {
            oln += _snwprintf_s(ctxDT->wszln + oln, MAX_PATH - oln, _TRUNCATE, L"Ptr: ");
            if(!pe->wszTypeName) {
                ctxDT->pfn->SymGetTypeInfo(ctxDT->hSym, ctxDT->BaseOfDll, pe->dwPtrTypeIndex, TI_GET_BASETYPE, &pe->dwBaseType);
                if(!pe->dwBaseType) {
                    ctxDT->pfn->SymGetTypeInfo(ctxDT->hSym, ctxDT->BaseOfDll, pe->dwPtrTypeIndex, TI_GET_SYMTAG, &pe->dwPtrTag);
                    if(pe->dwPtrTag == SymTagFunctionType) {
                        pe->dwBaseType = -1;    // fake base type - will resolve into 'function'
                    }
                    if(pe->dwPtrTag == SymTagPointerType) {
                        pe->dwBaseType = -2;    // fake base type - will resolve into 'pointer'
                    }
                }
            }
        }
        // special types #1:
        if((pe->dwTag == SymTagUDT) && pe->wszTypeName) {
            if(!wcscmp(L"_LARGE_INTEGER", pe->wszTypeName) || !_wcsnicmp(pe->wszTypeName, L"_EX_", 4) || !wcscmp(L"_KEVENT", pe->wszTypeName)) {
                pe->dwTag = SymTagBaseType;
            }
        }
        // type: complex or ordinary?
        if(pe->dwTag == SymTagUDT) {
            oln += _snwprintf_s(ctxDT->wszln + oln, MAX_PATH - oln, _TRUNCATE, L"%s", pe->wszTypeName);
            ctxDT->csz += Util_snwprintf_u8(ctxDT->szu8 + ctxDT->csz, ctxDT->cszMax - ctxDT->csz, L"%s\n", ctxDT->wszln);   // commit line to result utf-8 result string
            oln = 0;
            if(pe->dwChildCount && (iLevel < ctxDT->iLevelMax) && (!pbMEM || (pe->dwOffset + pe->qwLength <= cbMEM))) {
                PDB_DisplayTypeNt_DoWork(ctxDT, iLevel + 1, pe->dwTypeIndex, pe->dwChildCount, pe->wszTypeName, (pbMEM ? pbMEM + pe->dwOffset : NULL), (pbMEM ? cbMEM - pe->dwOffset : 0));
            }
        } else {
            wszBuffer[0] = 0;
            if(pe->qwBitFieldLength) {
                _snwprintf_s(wszBuffer, _countof(wszBuffer), _TRUNCATE, L" bit[%lli:%lli]", qwBitBase, qwBitBase + pe->qwBitFieldLength - 1);
            }
            wszName = PDB_DisplayTypeNt_GetTypeName(pe, cbType);
            oln += _snwprintf_s(ctxDT->wszln + oln, MAX_PATH - oln, _TRUNCATE, L"%s%s", wszName, wszBuffer);
            // optional data:
            if(pbMEM && ((pe->dwTag == SymTagBaseType) || (pe->dwTag == SymTagPointerType)) && ((cbType == 1) || (cbType == 2) || (cbType == 4) || (cbType == 8))) {
                v = 0;
                memcpy((PBYTE)&v, pbMEM + pe->dwOffset, cbType);
                if(pe->qwBitFieldLength) {
                    v = v >> qwBitBase;
                    v = v & ((1ULL << pe->qwBitFieldLength) - 1);
                }
                if(v < 10) {
                    wszFormat = L"%*s : %llX";
                } else if(cbType == 1) {
                    wszFormat = L"%*s : 0x%02llX";
                } else if(cbType == 2) {
                    wszFormat = L"%*s : 0x%04llX";
                } else if(cbType == 4) {
                    wszFormat = L"%*s : 0x%08llX";
                } else {
                    wszFormat = L"%*s : 0x%016llX";
                }
                oln += _snwprintf_s(ctxDT->wszln + oln, MAX_PATH - oln, _TRUNCATE, wszFormat, max(0, (int)(60 - oln)), L"", v);
            }
            // _UNICODE_STRING special case:
            if(pbMEM && (dwChildCount == 3) && (i == 2) && wszTypeName && !wcscmp(L"_UNICODE_STRING", wszTypeName)) {
                cbData = *(PWORD)pbMEM;
                vaData = ctxVmm->f32 ? *(PDWORD)(pbMEM + 4) : *(PQWORD)(pbMEM + 8);
                if(VMM_KADDR(vaData) && cbData && !(cbData & 1) && (cbData < MAX_PATH * 2)) {
                    if(VmmRead(PVMM_PROCESS_SYSTEM, vaData, (PBYTE)ctxDT->wszBuffer, (DWORD)cbData)) {
                        ctxDT->wszBuffer[cbData >> 1] = 0;
                        oln += _snwprintf_s(ctxDT->wszln + oln, MAX_PATH - oln, _TRUNCATE, L" - %s", ctxDT->wszBuffer);
                    }
                }
            }
        }
        // commit line to result buffer:
        if(oln) {
            ctxDT->csz += Util_snwprintf_u8(ctxDT->szu8 + ctxDT->csz, ctxDT->cszMax - ctxDT->csz, L"%s\n", ctxDT->wszln);   // commit line to result utf-8 result string
        }
        cbTypeLast = cbType;
        qwBitBase += pe->qwBitFieldLength;
    }
fail:
    LocalFree(pdwTypeIndex);
    if(pInfo) {
        for(i = 0; i < cInfo; i++) {
            pe = pInfo + i;
            LocalFree(pe->wszName);
            LocalFree(pe->wszTypeName);
        }
        LocalFree(pInfo);
    }
}

/*
* Fetch the ntoskrnl.exe type information from the PDB symbols and return it in
* a human readable utf-8 string. Caller is responsible for LocalFree().
* Please also note that this function is single-threaded by an internal lock.
* CALLER LocalFree: *pszResult
* -- szTypeName = the name of the type - only types within ntosknl.exe are allowed.
* -- cLevelMax = recurse into sub-types to cLevelMax.
* -- vaType = optional kernel address in SYSTEM process address space where to
*             load optional data from.
* -- fHexAscii = append object bytes as hexascii at the end of the string.
* -- fObjHeader = fetch object header instead of object.
* -- pszResult = optional ptr to receive the utf-8 string data
*                (function allocated - callee free)
* -- pcbResult = optional ptr to receive the byte length of the returned string
*                (including terminating null character).
* -- pcbType
* -- return
*/
_Success_(return)
BOOL PDB_DisplayTypeNt(
    _In_ LPSTR szTypeName,
    _In_ BYTE cLevelMax,
    _In_opt_ QWORD vaType,
    _In_ BOOL fHexAscii,
    _In_ BOOL fObjHeader,
    _Out_opt_ LPSTR *pszResult,
    _Out_opt_ PDWORD pcbResult,
    _Out_opt_ PDWORD pcbType
) {
    BOOL fResult = FALSE;
    PVMMWIN_PDB_CONTEXT ctx = (PVMMWIN_PDB_CONTEXT)ctxVmm->pPdbContext;
    PDB_HANDLE hPDB;
    SYMBOL_INFO_PACKAGE SymbolInfoType = { 0 };
    PPDB_ENTRY pObPdbEntry = NULL;
    PDB_DT_CONTEXT ctxDT = { 0 };
    DWORD i, cTypeChildren, cbHexAscii, cbSubType;
    PBYTE pbMEM = NULL;
    LPWSTR wszFormat;
    LPSTR szSubType = NULL, szSubTypeResult, szu8Result = NULL;
    if(pszResult) { *pszResult = NULL; }
    if(pcbResult) { *pcbResult = 0; }
    if(pcbType) { *pcbType = 0; }
    if(!ctx || ctx->fDisabled) { return FALSE; }
    hPDB = PDB_GetHandleFromModuleName("ntoskrnl");
    if(!(pObPdbEntry = ObMap_GetByKey(ctx->pmPdbByHash, hPDB))) { return FALSE; }
    EnterCriticalSection(&ctx->Lock);
    if(!PDB_LoadEnsureEx(pObPdbEntry)) { goto fail; }
    // flag: object header -> type == _OBJECT_HEADER + adjust va type base.
    if(fObjHeader) { szTypeName = "_OBJECT_HEADER"; }
    if(fObjHeader && vaType) { vaType -= ctxVmm->f32 ? sizeof(OBJECT_HEADER32) : sizeof(OBJECT_HEADER64); }
    // fetch type data:
    SymbolInfoType.si.SizeOfStruct = sizeof(SYMBOL_INFO);
    SymbolInfoType.si.MaxNameLen = MAX_SYM_NAME;
    if(!ctx->pfn.SymGetTypeFromName(ctx->hSym, pObPdbEntry->qwLoadAddress, szTypeName, &SymbolInfoType.si)) { goto fail; }
    if(SymbolInfoType.si.Tag != SymTagUDT) { goto fail; }   // only complex types - i.e. structs are allowed
    if(!ctx->pfn.SymGetTypeInfo(ctx->hSym, pObPdbEntry->qwLoadAddress, SymbolInfoType.si.TypeIndex, TI_GET_CHILDRENCOUNT, &cTypeChildren) || !cTypeChildren) { goto fail; }
    if(pcbType) { *pcbType = SymbolInfoType.si.Size; }
    // setup DisplayType context:
    ctxDT.hSym = ctx->hSym;
    ctxDT.BaseOfDll = pObPdbEntry->qwLoadAddress;
    ctxDT.pfn = &ctx->pfn;
    ctxDT.iLevelMax = cLevelMax;
    ctxDT.cszMax = 0x10000;
    ctxDT.szu8 = LocalAlloc(0, 0x10000);
    ctxDT.szu8[0] = 0;
    if(!ctxDT.szu8) { goto fail; }
    // fetch optional type memory (if possible):
    if(VMM_KADDR_4_8(vaType) && (SymbolInfoType.si.Size >= 4) && (SymbolInfoType.si.Size < 0x2000)) {
        if((pbMEM = LocalAlloc(0, SymbolInfoType.si.Size))) {
            if(!VmmRead(PVMM_PROCESS_SYSTEM, vaType, pbMEM, SymbolInfoType.si.Size)) {
                pbMEM = LocalFree(pbMEM);
            }
        }
    }
    // call worker function:
    if(pbMEM) {
        wszFormat = ctxVmm->f32 ? L"dt nt!%S  0x%08llX\n" : L"dt nt!%S  0x%016llX\n";
        ctxDT.csz += Util_snwprintf_u8(ctxDT.szu8 + ctxDT.csz, ctxDT.cszMax - ctxDT.csz, wszFormat, SymbolInfoType.si.Name, vaType);
    } else {
        ctxDT.csz += Util_snwprintf_u8(ctxDT.szu8 + ctxDT.csz, ctxDT.cszMax - ctxDT.csz, L"dt nt!%S\n", SymbolInfoType.si.Name);
    }
    PDB_DisplayTypeNt_DoWork(&ctxDT, 0, SymbolInfoType.si.TypeIndex, cTypeChildren, NULL, pbMEM, (pbMEM ? SymbolInfoType.si.Size : 0));
    // append optional hexascii:
    if(fHexAscii && pbMEM && ctxDT.csz) {
        wszFormat = ctxVmm->f32 ? L"\n---\n\ndb  0x%08llX  L%03X\n" : L"\n---\n\ndb  0x%016llX  L%03X\n";
        ctxDT.csz += Util_snwprintf_u8(ctxDT.szu8 + ctxDT.csz, ctxDT.cszMax - ctxDT.csz, wszFormat, vaType, SymbolInfoType.si.Size);
        cbHexAscii = (DWORD)(ctxDT.cszMax - ctxDT.csz);
        if(Util_FillHexAscii(pbMEM, min(0x2000, SymbolInfoType.si.Size), 0, ctxDT.szu8 + ctxDT.csz, &cbHexAscii)) {
            ctxDT.csz += cbHexAscii;
        }
    }
    // flag: object header: special processing:
    if(fObjHeader && pbMEM && ctxVmm->offset._OBJECT_HEADER_CREATOR_INFO.cb) {
        for(i = 0; i < 9; i++) {
            if(((1 << i) & (ctxVmm->f32 ? ((POBJECT_HEADER32)pbMEM)->InfoMask : ((POBJECT_HEADER64)pbMEM)->InfoMask)) || (i == 8)) {
                switch(i) {
                    case 0: szSubType = "_OBJECT_HEADER_CREATOR_INFO"; cbSubType = ctxVmm->offset._OBJECT_HEADER_CREATOR_INFO.cb; break;        // 0x01
                    case 1: szSubType = "_OBJECT_HEADER_NAME_INFO";    cbSubType = ctxVmm->offset._OBJECT_HEADER_NAME_INFO.cb;    break;        // 0x02
                    case 2: szSubType = "_OBJECT_HEADER_HANDLE_INFO";  cbSubType = ctxVmm->offset._OBJECT_HEADER_HANDLE_INFO.cb;  break;        // 0x04
                    case 3: szSubType = "_OBJECT_HEADER_QUOTA_INFO";   cbSubType = ctxVmm->offset._OBJECT_HEADER_QUOTA_INFO.cb;   break;        // 0x08
                    case 4: szSubType = "_OBJECT_HEADER_PROCESS_INFO"; cbSubType = ctxVmm->offset._OBJECT_HEADER_PROCESS_INFO.cb; break;        // 0x10
                    case 6: szSubType = "_OBJECT_HEADER_AUDIT_INFO";   cbSubType = ctxVmm->offset._OBJECT_HEADER_AUDIT_INFO.cb;   break;        // 0x40
                    case 8: szSubType = "_POOL_HEADER";                cbSubType = ctxVmm->offset._POOL_HEADER.cb;                break;        // ---
                    default: szSubType = NULL; cbSubType = 0; break;
                }
                if(!cbSubType || !szSubType) { break; }
                vaType -= cbSubType;
                if(PDB_DisplayTypeNt(szSubType, 2, vaType, fHexAscii, FALSE, &szSubTypeResult, NULL, NULL)) {
                    ctxDT.csz += Util_snwprintf_u8(ctxDT.szu8 + ctxDT.csz, ctxDT.cszMax - ctxDT.csz, L"\n======\n\n%S", szSubTypeResult);
                    LocalFree(szSubTypeResult);
                }
            }
        }
    }
    // set output
    if(pszResult) {
        if(!ctxDT.csz || !(*pszResult = LocalAlloc(0, ctxDT.csz + 1))) { goto fail; }
        memcpy(*pszResult, ctxDT.szu8, ctxDT.csz + 1);
    }
    if(pcbResult) { *pcbResult = (DWORD)(ctxDT.csz + 1); }
    fResult = TRUE;
fail:
    LeaveCriticalSection(&ctx->Lock);
    Ob_DECREF(pObPdbEntry);
    LocalFree(ctxDT.szu8);
    LocalFree(pbMEM);
    return fResult;
}
