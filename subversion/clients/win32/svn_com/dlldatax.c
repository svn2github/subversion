/*
 * wrapper for dlldata.c
 *
      * ====================================================================
 * Copyright (c) 2000-2002 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */

#ifdef _MERGE_PROXYSTUB // merge proxy stub DLL

#define REGISTER_PROXY_DLL //DllRegisterServer, etc.

#define _WIN32_WINNT 0x0400	//for WinNT 4.0 or Win95 with DCOM
#define USE_STUBLESS_PROXY	//defined only with MIDL switch /Oicf

#pragma comment(lib, "rpcndr.lib")
#pragma comment(lib, "rpcns4.lib")
#pragma comment(lib, "rpcrt4.lib")

#define DllMain				PrxDllMain
#define DllRegisterServer	PrxDllRegisterServer
#define DllUnregisterServer PrxDllUnregisterServer
#define DllGetClassObject   PrxDllGetClassObject
#define DllCanUnloadNow     PrxDllCanUnloadNow

#include "dlldata.c"
#include "SVNCOM_p.c"

#ifdef _NOPROXY //no midl generated dlldata.c

#define STRICT 1
#include <ole2.h>

BOOL WINAPI PrxDllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved)
{return TRUE;}

STDAPI PrxDllCanUnloadNow(void){return S_OK;}

STDAPI PrxDllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{return CLASS_E_CLASSNOTAVAILABLE;}

STDAPI PrxDllRegisterServer(void){return S_OK;}

STDAPI PrxDllUnregisterServer(void){return S_OK;}

#endif //!PROXY_DELEGATION

#endif //_MERGE_PROXYSTUB
