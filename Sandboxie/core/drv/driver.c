/*
 * Copyright 2004-2020 Sandboxie Holdings, LLC 
 * Copyright 2020-2024 David Xanatos, xanasoft.com
 *
 * This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

//---------------------------------------------------------------------------
// Sandboxie Driver
//---------------------------------------------------------------------------


#include "driver.h"
#include "obj.h"
#ifdef _M_ARM64
#include "common/arm64_asm.h"
#else
#define HOOK_WITH_PRIVATE_PARTS
#include "hook.h"
#endif
#include "conf.h"
#include "dll.h"
#include "api.h"
#include "common/my_version.h"

#include "syscall.h"
#include "session.h"
#include "process.h"
#include "thread.h"
#include "file.h"
#include "key.h"
#include "ipc.h"
#include "gui.h"
#include "util.h"
#include "token.h"
#include "wfp.h"
#include "dyn_data.h"

//---------------------------------------------------------------------------
// Functions
//---------------------------------------------------------------------------


NTSTATUS DriverEntry(
    IN DRIVER_OBJECT  *DriverObject,
    IN UNICODE_STRING *RegistryPath);

static BOOLEAN Driver_CheckOsVersion(void);

static BOOLEAN Driver_InitPublicSecurity(void);

static BOOLEAN Driver_FindHomePath(UNICODE_STRING *RegistryPath);

static BOOLEAN Driver_FindMissingServices(void);

#ifdef _M_ARM64
static BOOLEAN Driver_FindKiServiceInternal(void);
#endif

static void SbieDrv_DriverUnload(DRIVER_OBJECT *DriverObject);


//---------------------------------------------------------------------------


#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (INIT, Driver_CheckOsVersion)
#pragma alloc_text (INIT, Driver_FindHomePath)
#pragma alloc_text (INIT, Driver_FindMissingServices)
#ifdef _M_ARM64
#pragma alloc_text (INIT, Driver_FindKiServiceInternal)
#endif
#endif // ALLOC_PRAGMA


//---------------------------------------------------------------------------
// Variables
//---------------------------------------------------------------------------


const ULONG tzuk = 'xobs';

const WCHAR *Driver_S_1_5_18 = L"S-1-5-18"; //	System
const WCHAR *Driver_S_1_5_19 = L"S-1-5-19"; //	Local Service
const WCHAR *Driver_S_1_5_20 = L"S-1-5-20"; //	Network Service

DRIVER_OBJECT *Driver_Object;

WCHAR *Driver_Version = TEXT(MY_VERSION_STRING);

ULONG Driver_OsVersion = 0;
ULONG Driver_OsBuild = 0;
BOOLEAN Driver_OsTestSigning = FALSE;

POOL *Driver_Pool = NULL;

const WCHAR *Driver_Sandbox = L"\\Sandbox";

const WCHAR *Driver_Empty = L"";

//const WCHAR *Driver_OpenProtectedStorage = L"OpenProtectedStorage";

WCHAR *Driver_RegistryPath;

WCHAR *Driver_HomePathDos    = NULL;
WCHAR *Driver_HomePathNt     = NULL;
ULONG  Driver_HomePathNt_Len = 0;

PSECURITY_DESCRIPTOR Driver_PublicSd = NULL;
PACL Driver_PublicAcl = NULL;

PSECURITY_DESCRIPTOR Driver_LowLabelSd = NULL;

volatile BOOLEAN Driver_Unloading = FALSE;

BOOLEAN Driver_FullUnload = TRUE;

UNICODE_STRING Driver_Altitude;
const WCHAR* Altitude_Str = FILTER_ALTITUDE;

ULONG Process_Flags1 = 0;
ULONG Process_Flags2 = 0;
ULONG Process_Flags3 = 0;


//---------------------------------------------------------------------------


#ifdef OLD_DDK
P_NtSetInformationToken         ZwSetInformationToken       = NULL;
#endif // OLD_DDK

#ifdef _M_ARM64
void*                           Driver_KiServiceInternal    = NULL;
USHORT                          ZwCreateToken_num           = 0;
USHORT                          ZwCreateTokenEx_num         = 0;
#else
P_NtCreateToken                 ZwCreateToken               = NULL;
P_NtCreateTokenEx               ZwCreateTokenEx             = NULL;
#endif


//---------------------------------------------------------------------------
// DriverEntry
//---------------------------------------------------------------------------


_FX NTSTATUS DriverEntry(
    IN  DRIVER_OBJECT  *DriverObject,
    IN  UNICODE_STRING *RegistryPath)
{
    BOOLEAN ok = TRUE;

	ExInitializeDriverRuntime(DrvRtPoolNxOptIn);

    //
    // initialize global driver variables
    //

    Driver_Object = DriverObject;
    Driver_Object->DriverUnload = NULL;

    RtlInitUnicodeString(&Driver_Altitude, Altitude_Str);

    if (ok)
        ok = Driver_CheckOsVersion();

    if (ok) {
        Driver_Pool = Pool_Create();
        if (! Driver_Pool) {
            Log_Msg0(MSG_1104);
            ok = FALSE;
        }
    }

    if (ok)
        ok = Driver_InitPublicSecurity();

    if (ok) {
        Driver_RegistryPath =
            Mem_AllocStringEx(Driver_Pool, RegistryPath->Buffer, TRUE);
        if (! Driver_RegistryPath)
            ok = FALSE;
    }

    if (ok)
        Dyndata_Init();

    if (ok)
        ok = Driver_FindHomePath(RegistryPath);

    if (ok)
        MyValidateCertificate();

    //
    // initialize simple utility modules.  these don't hook anything
    //

    if (ok)
        ok = Obj_Init();

    if (ok)
        ok = Conf_Init();

    if (ok)
        ok = Dll_Init();

    if (ok)
        ok = Syscall_Init();

    if (ok)
        ok = Session_Init();

    if (ok)
        ok = Driver_FindMissingServices();

    if (ok)
        ok = Token_Init();

    //
    // initialize modules.  these place hooks into the system.  hooks
    // become active as soon as installed.  the Process module must be
    // initialized first, because it initializes the process list
    //

    if (ok)
        ok = Process_Init();

    if (ok)
        ok = Thread_Init();

    if (ok)
        ok = File_Init();

    if (ok)
        ok = Key_Init();

    if (ok)
        ok = Ipc_Init();

    if (ok)
        ok = Gui_Init();

    //
    // create driver device for servicing user mode requests
    //

    if (ok)
        ok = Api_Init();

    //
    // initializing Windows Filtering Platform callouts
    //
    
    if (ok)
        ok = WFP_Init();

    //
    // finalize of driver initialization
    //

    Dll_Unload();       // released dlls loading during init

    if (! ok) {
        Log_Msg1(MSG_DRIVER_ENTRY_FAILED, Driver_Version);
        SbieDrv_DriverUnload(Driver_Object);
        return STATUS_UNSUCCESSFUL;
    }

    Driver_FullUnload = FALSE;

    Log_Msg1(MSG_DRIVER_ENTRY_OK, Driver_Version);

    return STATUS_SUCCESS;
}


//---------------------------------------------------------------------------
// Driver_CheckOsVersion
//---------------------------------------------------------------------------


_FX BOOLEAN Driver_CheckOsVersion(void)
{
    ULONG MajorVersion, MinorVersion;
    WCHAR str[64];

    Driver_OsTestSigning = MyIsTestSigning();

    //
    // make sure we're running on Windows XP (v5.1) or later (32-bit)
    // or Windows 7 (v6.1) or later (64-bit)
    //

#if defined(_WIN64) || !defined(XP_SUPPORT)
    const ULONG MajorVersionMin = 6;
    const ULONG MinorVersionMin = 1;
#else
    const ULONG MajorVersionMin = 5;
    const ULONG MinorVersionMin = 1;
#endif

    PsGetVersion(&MajorVersion, &MinorVersion, &Driver_OsBuild, NULL);

    if (MajorVersion > MajorVersionMin ||
            (   MajorVersion == MajorVersionMin
             && MinorVersion >= MinorVersionMin)) {

        if (MajorVersion == 10) { // for windows 11 its still 10
            Driver_OsVersion = DRIVER_WINDOWS_10;
        }
        else if (MajorVersion == 6) {

            if (MinorVersion == 3 && Driver_OsBuild >= 9600) {
                Driver_OsVersion = DRIVER_WINDOWS_81;
            }
            else if (MinorVersion == 2 && Driver_OsBuild >= 9200) {
                Driver_OsVersion = DRIVER_WINDOWS_8;
            }

            else if (MinorVersion == 1 && Driver_OsBuild >= 7600) {
                Driver_OsVersion = DRIVER_WINDOWS_7;
            }
            else if (MinorVersion == 0 && Driver_OsBuild >= 6000) {
                Driver_OsVersion = DRIVER_WINDOWS_VISTA;
            }

        }
        else {
            if (MinorVersion == 2)
                Driver_OsVersion = DRIVER_WINDOWS_2003;

            else if (MinorVersion == 1)
                Driver_OsVersion = DRIVER_WINDOWS_XP;
        }

        if (Driver_OsVersion)
            return TRUE;
    }

    RtlStringCbPrintfW(str, sizeof(str), L"%d.%d (%d)", MajorVersion, MinorVersion, Driver_OsBuild);
    Log_Msg(MSG_1105, str, NULL);
    return FALSE;
}


//---------------------------------------------------------------------------
// Driver_InitPublicSecurity
//---------------------------------------------------------------------------


_FX BOOLEAN Driver_InitPublicSecurity(void)
{
#define MyAddAccessAllowedAce(pAcl,pSid)                                \
    RtlAddAccessAllowedAceEx(pAcl, ACL_REVISION,                        \
        CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE | INHERITED_ACE,     \
        GENERIC_ALL, pSid);

    //
    // create a security descriptor with a DACL that permits
    // access to/by the Authenticated Users and Everyone SIDs
    //

    static UCHAR AuthSid[12] = {
        1,                                      // Revision
        1,                                      // SubAuthorityCount
        0,0,0,0,0,5, // SECURITY_NT_AUTHORITY   // IdentifierAuthority
        SECURITY_AUTHENTICATED_USER_RID         // SubAuthority
    };
    static UCHAR WorldSid[12] = {
        1,                                      // Revision
        1,                                      // SubAuthorityCount
        0,0,0,0,0,1, // SECURITY_WORLD_SID_AUTHORITY // IdentifierAuthority
        SECURITY_WORLD_RID                      // SubAuthority
    };
    static UCHAR RestrSid[12] = {
        1,                                      // Revision
        1,                                      // SubAuthorityCount
        0,0,0,0,0,5, // SECURITY_NT_AUTHORITY   // IdentifierAuthority
        SECURITY_RESTRICTED_CODE_RID            // SubAuthority
    };

    Driver_PublicAcl = Mem_AllocEx(Driver_Pool, 128, TRUE);
    if (! Driver_PublicAcl)
        return FALSE;

    RtlCreateAcl(Driver_PublicAcl, 128, ACL_REVISION);
    MyAddAccessAllowedAce(Driver_PublicAcl, &AuthSid);
    MyAddAccessAllowedAce(Driver_PublicAcl, &WorldSid);

    Driver_PublicSd = Mem_AllocEx(Driver_Pool, 64, TRUE);
    if (! Driver_PublicSd)
        return FALSE;

    RtlCreateSecurityDescriptor(
        Driver_PublicSd, SECURITY_DESCRIPTOR_REVISION);
    RtlSetDaclSecurityDescriptor(
        Driver_PublicSd, TRUE, Driver_PublicAcl, FALSE);

    //
    // on Windows Vista, create a security descriptor which
    // permits access to/by a low integrity process
    //

    if (Driver_OsVersion >= DRIVER_WINDOWS_VISTA) {

        typedef struct _ACE_HEADER {
            UCHAR  AceType;
            UCHAR  AceFlags;
            USHORT AceSize;
        } ACE_HEADER;
        typedef struct _SYSTEM_MANDATORY_LABEL_ACE {
            ACE_HEADER  Header;
            ACCESS_MASK Mask;
            ULONG       SidStart;
        } SYSTEM_MANDATORY_LABEL_ACE, *PSYSTEM_MANDATORY_LABEL_ACE;


        PACL LowLabelAcl1, LowLabelAcl2;

        UCHAR ace_space[32];
        SYSTEM_MANDATORY_LABEL_ACE *pAce =
            (SYSTEM_MANDATORY_LABEL_ACE *)ace_space;
        ULONG *pSid = &pAce->SidStart;
        pAce->Header.AceType = SYSTEM_MANDATORY_LABEL_ACE_TYPE;
        pAce->Header.AceFlags = 0;
        pAce->Header.AceSize = 5 * sizeof(ULONG);
        pAce->Mask = SYSTEM_MANDATORY_LABEL_NO_WRITE_UP;
        pSid[0] = 0x00000101;
        pSid[1] = 0x10000000;
        pSid[2] = SECURITY_MANDATORY_LOW_RID;

        LowLabelAcl1 = Mem_AllocEx(Driver_Pool, 128, TRUE);
        if (! LowLabelAcl1)
            return FALSE;
        RtlCreateAcl(LowLabelAcl1, 128, ACL_REVISION);
        RtlAddAce(LowLabelAcl1, ACL_REVISION, 0, pAce, pAce->Header.AceSize);

        LowLabelAcl2 = Mem_AllocEx(Driver_Pool, 128, TRUE);
        if (! LowLabelAcl2)
            return FALSE;
        RtlCreateAcl(LowLabelAcl2, 128, ACL_REVISION);
        MyAddAccessAllowedAce(LowLabelAcl2, &AuthSid);
        MyAddAccessAllowedAce(LowLabelAcl2, &WorldSid);
        MyAddAccessAllowedAce(LowLabelAcl2, &RestrSid);

        Driver_LowLabelSd = Mem_AllocEx(Driver_Pool, 128, TRUE);
        if (! Driver_LowLabelSd)
            return FALSE;

        RtlCreateSecurityDescriptor(
            Driver_LowLabelSd, SECURITY_DESCRIPTOR_REVISION);
        RtlSetDaclSecurityDescriptor(
            Driver_LowLabelSd, TRUE, LowLabelAcl2, FALSE);
        RtlSetSaclSecurityDescriptor(
            Driver_LowLabelSd, TRUE, LowLabelAcl1, FALSE);
    }

    return TRUE;

#undef MyAddAccessAllowedAce
}


//---------------------------------------------------------------------------
// Driver_FindHomePath
//---------------------------------------------------------------------------


_FX BOOLEAN Driver_FindHomePath(UNICODE_STRING *RegistryPath)
{
    NTSTATUS status;
    OBJECT_ATTRIBUTES objattrs;
    UNICODE_STRING uni;
    HANDLE handle;
    union {
        KEY_VALUE_PARTIAL_INFORMATION part;
        WCHAR info_space[256];
    } info;
    WCHAR path[384];
    WCHAR *ptr;
    ULONG len;
    IO_STATUS_BLOCK MyIoStatusBlock;
    FILE_OBJECT *file_object;
    OBJECT_NAME_INFORMATION *Name = NULL;
    ULONG NameLength = 0;

    //
    // find the path to SbieDrv.sys
    //

    InitializeObjectAttributes(&objattrs,
        RegistryPath, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwOpenKey(&handle, KEY_READ, &objattrs);
    if (! NT_SUCCESS(status)) {
        Log_Status_Ex(MSG_DRIVER_FIND_HOME_PATH, 0x11, status, SBIEDRV);
        return FALSE;
    }

    InitializeObjectAttributes(&objattrs,
        &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);

    RtlInitUnicodeString(&uni, L"ImagePath");
    len = sizeof(info);
    status = ZwQueryValueKey(
        handle, &uni, KeyValuePartialInformation, &info, len, &len);

    ZwClose(handle);

    if (! NT_SUCCESS(status)) {
        Log_Status_Ex(MSG_DRIVER_FIND_HOME_PATH, 0x22, status, uni.Buffer);
        return FALSE;
    }
    if ((info.part.Type != REG_SZ && info.part.Type != REG_EXPAND_SZ)
            || info.part.DataLength < 4) {
        Log_Status_Ex(MSG_DRIVER_FIND_HOME_PATH, 0x33, status, uni.Buffer);
        return FALSE;
    }

    // the path should be \??\<home>\SbieDrv.sys, where <home> is the
    // Sandboxie installation directory.  We need to remove Sandbox.sys,
    // and prepend \??\ if it's missing

    ptr = (WCHAR *)info.part.Data;
    ptr[info.part.DataLength / sizeof(WCHAR) - 1] = L'\0';

    if (*ptr != L'\\') {
        wcscpy(path, L"\\??\\");
        wcscat(path, ptr);
    } else
        wcscpy(path, ptr);

    ptr = wcsrchr(path, L'\\');
    if (ptr)
        *ptr = L'\0';

    Driver_HomePathDos = Mem_AllocStringEx(Driver_Pool, path, TRUE);
    if (! Driver_HomePathDos) {
        status = STATUS_INSUFFICIENT_RESOURCES;
        Log_Status(MSG_DRIVER_FIND_HOME_PATH, 0x44, status);
        return FALSE;
    }

    //
    // try to open the path so we can get a FILE_OBJECT for it
    //

    RtlInitUnicodeString(&uni, path);

    InitializeObjectAttributes(&objattrs,
        &uni, OBJ_CASE_INSENSITIVE, NULL, NULL);

    status = ZwCreateFile(
        &handle,
        FILE_GENERIC_READ,      // DesiredAccess
        &objattrs,
        &MyIoStatusBlock,
        NULL,                   // AllocationSize
        0,                      // FileAttributes
        FILE_SHARE_READ,        // ShareAccess
        FILE_OPEN,              // CreateDisposition
        FILE_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT,
        NULL, 0);               // EaBuffer, EaLength

    if (! NT_SUCCESS(status)) {
        Log_Status_Ex(MSG_DRIVER_FIND_HOME_PATH, 0x55, status, uni.Buffer);
        return FALSE;
    }

    // get the canonical path name from the file object

    status = ObReferenceObjectByHandle(
        handle, 0, NULL, KernelMode, &file_object, NULL);

    if (! NT_SUCCESS(status)) {
        ZwClose(handle);
        Log_Status_Ex(MSG_DRIVER_FIND_HOME_PATH, 0x66, status, uni.Buffer);
        return FALSE;
    }

    status = Obj_GetName(Driver_Pool, file_object, &Name, &NameLength);

    ObDereferenceObject(file_object);
    ZwClose(handle);

    if (! NT_SUCCESS(status)) {
        Log_Status_Ex(MSG_DRIVER_FIND_HOME_PATH, 0x77, status, uni.Buffer);
        return FALSE;
    }

    Driver_HomePathNt = Name->Name.Buffer;
    Driver_HomePathNt_Len = wcslen(Driver_HomePathNt);

    return TRUE;
}


//---------------------------------------------------------------------------
// Driver_FindKiServiceInternal
//---------------------------------------------------------------------------

#ifdef _M_ARM64
_FX BOOLEAN Driver_FindKiServiceInternal()
{
    UCHAR *addr = NULL; // pick some random Zw function

    //
    // Driver verifier messes with the Zw imports, and this breaks the Hook_Find_ZwRoutine routine
    // to fix this we lookup the offsets of the real functions in the export table of ntoskrnl.exe
    // and then use these correct offsets in Hook_Find_ZwRoutine
    //

    UCHAR* kernel_base = (UCHAR*)Syscall_GetKernelBase();
    if (kernel_base) {

        ULONG_PTR offset = (ULONG_PTR)Dll_GetProc(Exe_NTOSKRNL, "ZwWaitForSingleObject", TRUE);
        if (offset) addr = kernel_base + offset;
    }

    if(!addr) addr = (UCHAR *)ZwWaitForSingleObject;

    // a ZwXxx system service redirector looks like this in Windows 11 ARM64
    // B0 01 80 D2 7F 1E 00 14 00 00 00 00 00 00 00 00
    // movz x16, #svc_num
    // b    KiServiceInternal

    MOV mov;
    mov.OP = *(ULONG*)addr;
    addr += 4;

    if (!IS_MOV(mov) || mov.Rd != 16) {
        DbgPrint("bad MOV %d\n", mov.OP);
        return FALSE;
    }


    B b;
    b.OP = *(ULONG*)addr;

    if (!IS_B(b)) {
        DbgPrint("bad B %d\n", b.OP);
        return FALSE;
    }

    LONG delta = (b.imm26 << 2); // * 4
    if (delta & (1 << 27)) // if this is negative
        delta |= 0xF0000000; // make it properly negative

    Driver_KiServiceInternal = (addr + delta);
    DbgPrint("KiServiceInternal: %p\n", Driver_KiServiceInternal);

    return TRUE;
}
#endif

//---------------------------------------------------------------------------
// Driver_FindMissingService
//---------------------------------------------------------------------------

#ifndef _M_ARM64
void* Driver_FindMissingService(const char* ProcName, int prmcnt)
{
    void* ptr = Dll_GetProc(Dll_NTDLL, ProcName, FALSE);
    if (!ptr)
        return NULL;
    void* svc = NULL;
    if (!Hook_GetService(ptr, NULL, prmcnt, NULL, &svc))
        return NULL;
    return svc;
}
#endif

//---------------------------------------------------------------------------
// Driver_FindMissingServices
//---------------------------------------------------------------------------


_FX BOOLEAN Driver_FindMissingServices(void)
{
    //
    // Retrieve some unexported kernel functions which may be useful
    //

#ifdef _M_ARM64

    //
    // The Windows Kernel on ARM64 not only not exports ZwCreateToken/ZwCreateTokenEx
    // but out right lacks those functions entirely.
    // So in order to work around this limitation we implement a own system service wrapper
    // we invoke Sbie_CallZwServiceFunction_asm with all the arguments we need
    // and the service number as last 20th argument, it sets IP0/X16 and invokes KiServiceInternal
    //

    SYSCALL_ENTRY *entry = Syscall_GetByName("CreateToken");
    if (entry) 
        ZwCreateToken_num = entry->syscall_index;

    SYSCALL_ENTRY *entry_ex = Syscall_GetByName("CreateTokenEx");
    if (entry_ex) 
        ZwCreateTokenEx_num = entry_ex->syscall_index;

    Driver_FindKiServiceInternal();

    
    /*DbgPrint("Test 1\n");

    UNICODE_STRING uni;
    OBJECT_ATTRIBUTES objattrs;
    RtlInitUnicodeString(&uni, L"\\??\\C:\\Temp\\test.txt");
    InitializeObjectAttributes(&objattrs,
        &uni, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);

    entry = Syscall_GetByName("DeleteFile");
    if (entry) {
        DbgPrint("Test 2\n");
        NTSTATUS status = Sbie_CallZwServiceFunction_asm((UINT_PTR)&objattrs,
            0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
            entry->syscall_index);
        DbgPrint("Test 3 %d\n", status);
    }

    DbgPrint("Test 4\n");*/

#else

    ZwCreateToken = (P_NtCreateToken) Driver_FindMissingService("ZwCreateToken", 13);
    //DbgPrint("ZwCreateToken: %p\r\n", ZwCreateToken);
    if (Driver_OsVersion >= DRIVER_WINDOWS_8) {
        ZwCreateTokenEx = (P_NtCreateTokenEx)Driver_FindMissingService("ZwCreateTokenEx", 17);
        //DbgPrint("ZwCreateTokenEx: %p\r\n", ZwCreateTokenEx);
    }
    if (!ZwCreateToken)
        Log_Msg1(MSG_1108, L"ZwCreateTokenEx");

#endif

#ifdef OLD_DDK
    UNICODE_STRING uni;
	RtlInitUnicodeString(&uni, L"ZwSetInformationToken");

    //
    // Windows 7 kernel exports ZwSetInformationToken
    // on earlier versions of Windows, we search for it
    //
//#ifndef _WIN64
    if (Driver_OsVersion < DRIVER_WINDOWS_7) {

        ZwSetInformationToken = (P_NtSetInformationToken) Driver_FindMissingService("ZwSetInformationToken", 4);

    } else 
//#endif
	{
		ZwSetInformationToken = (P_NtSetInformationToken) MmGetSystemRoutineAddress(&uni);
    }

    if (!ZwSetInformationToken) {
		Log_Msg1(MSG_1108, uni.Buffer);
		return FALSE;
	}
#endif

    return TRUE;
}


//---------------------------------------------------------------------------
// DriverUnload
//---------------------------------------------------------------------------


_FX void SbieDrv_DriverUnload(DRIVER_OBJECT *DriverObject)
{
    Driver_Unloading = TRUE;

    //
    // unload just the hooks, in case this is a partial unload
    //

#ifdef XP_SUPPORT
    Gui_Unload();
#endif
    Key_Unload();
    File_Unload();
    Obj_Unload();
    Thread_Unload();
    Process_Unload(FALSE);

    //
    // if this is a full unload, then we can now unload everything else
    //

    if (Driver_FullUnload) {

        LARGE_INTEGER time;
        time.QuadPart = -SECONDS(5);
        KeDelayExecutionThread(KernelMode, FALSE, &time);

        WFP_Unload();
        Session_Unload();
        Dll_Unload();
        Conf_Unload();
        Api_Unload();
        Process_Unload(TRUE);
        Ipc_Unload();

        if (Driver_Pool)
            Pool_Delete(Driver_Pool);

        Log_Msg0(MSG_DRIVER_UNLOAD);
    }
}


//---------------------------------------------------------------------------
// Driver_Api_Unload
//---------------------------------------------------------------------------


_FX NTSTATUS Driver_Api_Unload(PROCESS *proc, ULONG64 *parms)
{
    BOOLEAN ok;
    KIRQL irql;

    //
    // reset Process_ReadyToSandbox so no new process can start,
    // then make sure there are no sandboxed processes
    //

    BOOLEAN ReadyToSandbox = Process_ReadyToSandbox;
    Process_ReadyToSandbox = FALSE;
    KeMemoryBarrier();

    KeRaiseIrql(APC_LEVEL, &irql);
    ExAcquireResourceExclusiveLite(Process_ListLock, TRUE);

    ok = FALSE;
#ifdef USE_PROCESS_MAP
    if (Process_Map.nnodes == 0) {
#else
    if (! List_Count(&Process_List)) {
#endif
        if (Api_Disable())
            ok = TRUE;
    }

    ExReleaseResourceLite(Process_ListLock);
    KeLowerIrql(irql);

    if (! ok) {
        Process_ReadyToSandbox = ReadyToSandbox;
        Log_MsgP0(MSG_CANNOT_UNLOAD_DRIVER, proc->pid);
        return STATUS_CONNECTION_IN_USE;
    }

    //
    // begin unloading the driver
    //

    SbieDrv_DriverUnload(Driver_Object);

    Driver_FullUnload = TRUE;
    Driver_Object->DriverUnload = SbieDrv_DriverUnload;

    return STATUS_SUCCESS;
}
