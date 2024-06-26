/*
Author: David Zimmer <dzzie@yahoo.com>
Site:   http://sandsprite.com

Purpose:
			This is an injection dll designed to be loaded at process start.
		
			After completion any loaded vb forms in the exe are now remotely scriptable
			this includes any controls on the forms, public methods, properties, 
			or public class references declared on the forms. 
			
			Sample scripts are provided for VBS, JS, and Python. Any language which can 
			access the Running Object Table (ROT) can be used.

Actions:
			on injection it will hook user32.BeginPaint and 
			an internal function of the VbRuntime using a hardcoded offset (see below)

			The vb runtime hook:
				will trigger during early runtime initilization of the exe and
				will store a reference of the primary CVBApplication class. 
			
				This allows us to retrieve a reference to the vb.forms collection for the main exe 
				and its VBHeader structure.

				We also take this oppertunity to scan all of the vb objects in the main exe and change any class
				objectType's to public. This will allow full access later on.

				This hook will be called once for ever vb6 component loaded. We only track the main exe.

			The BeginPaint hook:
				will fire on main form initilization and occurs from the primary vb thread (required) 
				We use this oppertunity to add a system menu item to trigger any new features we might want to add
				subclass the main window for the system menu trigger and also listen for IPC commands
				register the vb.forms collection with the ROT so all loaded forms are accessible 
				we then disable the BeginPaint hook


		    Vb runtime hook details:
				we are hooking an internal function not exposed using hardcoded offset.
				requires reference copy of runtime: MD5: EEBEB73979D0AD3C74B248EBF1B6E770

				.text:66017F15                         ; unsigned int __thiscall CVBApplication::Init(CVBApplication *this)
				.text:66017F15 56                                      push    esi
				.text:66017F16 8B F1                                   mov     esi, ecx
				.text:66017F18 FF 76 34                                push    dword ptr [esi+34h] ; struct Project *
				.text:66017F1B E8 30 00 00 00                          call    ?RegVbeRtHostAppObject@@YGJPAVProject@@@Z ; RegVbeRtHostAppObject(Project *)

				$this->  >660130D0  MSVBVM60.??_7CVBApplication@@6BVBGlobal@@@
				$+4      >660130C0  MSVBVM60.??_7CVBApplication@@6BISupportErrorInfo@@@
				$+8      >66013098  MSVBVM60.??_7CVBApplication@@6BCAntiMarshal@@@
				$+C      >02B800AC
				$+10     >66018C70  MSVBVM60.??_7PRINTERSCOLL@@6BIVBControl@@@
				$+14     >66018C60  MSVBVM60.??_7PRINTERSCOLL@@6BISupportErrorInfo@@@
				$+18     >00000000
				$+1C     >02B8004C
				$+20     >00000000
				$+24     >66018C20  MSVBVM60.??_7FORMSCOLL@@6BIVBControl@@@           <-- forms collection
				$+28     >66018C10  MSVBVM60.??_7FORMSCOLL@@6BISupportErrorInfo@@@
				$+2C     >00000000
				$+30     >023AE18C  
				$+34     >02E805CC   Project *
							02E805CC >66013028  MSVBVM60.??_7Project@@6BProjectContext@@@
							$+4      >660189F8  MSVBVM60.??_8Project@@7B@
							$+8      >00000000
							$+C      >66110090  OFFSET MSVBVM60.?m_ProjectList@Project@@0VDblLstBase@@A
							$+10     >00401328  Project1.00401328   <-- vbheader
*/

#define _WIN32_WINNT 0x0401  //for IsDebuggerPresent 
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>

#pragma warning(disable:4996)
#pragma comment(lib, "Ole32.lib")

void InstallHooks(void);

#include "NtHookEngine.h"
#include "main.h"  
#include "vb_structs.h"

IRunningObjectTable *rot = NULL;
DWORD appRotToken = 0;
void* vbApp = 0;
VBHeader* vbHeader = 0;
bool Installed =false;
WNDPROC prevWndProc = 0;

#define IDM_MYACTION  1010

HDC (__stdcall *Real_BeginPaint)( HWND hWnd, LPPAINTSTRUCT lpPaint ) = NULL;
void* Real_CVBApplication_Init = 0;   //only called from asm no need for full prototype

extern "C" __declspec (dllexport) int NullSub(void){ return 1;} //so we have an export to hardcode add to pe table if we want.

void Closing(void){ 

		msg("***** Injected Process Terminated *****"); 
		
		if(rot != NULL && appRotToken != 0){
			msgf("***** Releasing rot token*****"); 
			rot->Revoke(appRotToken);
			rot->Release();
		}

		exit(0);

}

BOOL APIENTRY DllMain( HANDLE hModule, DWORD  ul_reason_for_call, LPVOID lpReserved)
{

    if(!Installed){
		 Installed=true;
		 InstallHooks();
		 atexit(Closing);
	}

	return TRUE;
}

int isClass(int objType){
	
	int knownVals[8] = {1146883, 1277955, 98339, 100355, 1148931, 0x118043, 0x18003, 0x138803};

	for(int i = 0; i < 8; i++){
		if(objType == knownVals[i]) return 1;
	}
	
	return 0;
}

void __stdcall MakeClassesPublic(void){
	
	if(vbApp == NULL) return;
	
	//_asm int 3
	int* substruct = pPlus(vbApp, 0x34);
	//msgf("vbApp: %x, substruct: %x", (int)vbApp, (int)substruct);
	if(substruct == NULL) return;

	vbHeader = (VBHeader*)(*(int*)(*substruct+0x10)); //keep global ref
	//msgf("vbHeader: %x", (int)vbHeader);
	if(vbHeader == NULL) return;
    
	//msgf("ProjectInfo: %x", (int)vbHeader->ProjectInfo);
	if(vbHeader->ProjectInfo == NULL) return;
	
	//msgf("ObjectTable: %x", (int)vbHeader->ProjectInfo->ObjectTable);
    if(vbHeader->ProjectInfo->ObjectTable == NULL) return;

	VB_ObjectTable *objTable = vbHeader->ProjectInfo->ObjectTable;
	//msgf("vbheader: %x , objTable: %x, cnt: %d", (int)vbHeader, (int)objTable, objTable->dwTotalObjects);

	//_asm int 3
	DWORD oldMemProt=0, tmp=0, patched=0;
	VBObject *obj = objTable->ObjectArray;

	for(int i=0; i < objTable->dwTotalObjects; i++){

		//if(obj->lpszObjectName != NULL) msgf("obj %d = %s type=%x", i, obj->lpszObjectName, obj->fObjectType);

		if( isClass(obj->fObjectType)){
			if( VirtualProtect(obj, sizeof(VBObject), 0x40 , &oldMemProt) != 0){
				obj->fObjectType ^= 0x800;
				VirtualProtect(obj, sizeof(VBObject), oldMemProt , &tmp);
				//msgf("patched class obj %d = %s type=%x", i, obj->lpszObjectName, obj->fObjectType);
				patched++;
			}else{
				//msgf("virtprot failed! %d  %x", i, (int)obj);
			}
		}

		obj++;
	}

	msgf("patched %d classes in objTable to set public", patched);

}

unsigned int __declspec(naked) My_CVBApplication_Init(/*CVBApplication*/ void *_this){
	
	_asm{
		//int 3 

		//this hook triggers during runtime initilization
		//this is called once for every vb component loaded, like other vb6 dlls/ocxs 
		//they each get their own CVBApplication object and vb.forms collection. 
		//we only track the first one for the main exe right now (loaded first before any VB ocx's)

		mov eax, vbApp
		cmp eax, 0
		jnz notFirst

		mov vbApp, ecx

		pushf
		pushad
		call MakeClassesPublic
		popad
		popf
		
notFirst: 
		jmp Real_CVBApplication_Init
	}

}

LRESULT CALLBACK myNewWndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
{
	char m_msg[2020];
    cpyData CopyData;
	int retVal = 0;

	if(msg == WM_SYSCOMMAND && wparam == IDM_MYACTION)
	{
		msgf("My System menu triggered ThreadID: %x  vbapp = %x", GetCurrentThreadId(), (int)vbApp);
	}

	if( msg == WM_COPYDATA){
		if( lparam == 0) return 0;
		memcpy((void*)&CopyData, (void*)lparam, sizeof(cpyData));
		if( CopyData.dwFlag == 3 ){
			if( CopyData.cbSize >= sizeof(m_msg) - 2 ) CopyData.cbSize = sizeof(m_msg) - 2;
			memset((void*)&m_msg[0], 0, sizeof(m_msg));
			memcpy((void*)&m_msg[0], (void*)CopyData.lpData, CopyData.cbSize);
			m_msg[CopyData.cbSize] = 0; 
			msgf("IPC msg: %s\n", m_msg); 
			//retVal = HandleIPCMsg(m_msg); 				    
			return retVal;
		}
	}

	return CallWindowProc(prevWndProc, hwnd, msg, wparam, lparam);
} 
 

HDC __stdcall My_BeginPaint(HWND hWnd, LPPAINTSTRUCT lpPaint)
{
	//now we are running in the main VB thread and this is the hwnd of the main window if first...
	//if you have to deal with a splash screen you will have to alter this logic

	char wndClass[256] = {0};
	IMoniker *mon = 0;

	int sz = GetClassName(hWnd, &wndClass[0], 255);
	HDC ret = Real_BeginPaint(hWnd,lpPaint);
	msgf("BeginPaint(h=%x) class: %s", hWnd, wndClass);

	//sometimes CompatDesktopWindowReplacement hits before runtime init hook - ignore
	//tested against all vb window types and MDI
	if(strcmp(wndClass, "ThunderRT6FormDC") !=0 && 
	   strcmp(wndClass, "ThunderRT6MDIForm") !=0 ) 
			return ret;

	int disabled = DisableHook((ULONG_PTR)Real_BeginPaint);
	msgf("vbApp=0x%x hookDisabled=%d", (int)vbApp, disabled);
	
	if(prevWndProc == 0){
		
		HMENU h = GetSystemMenu(hWnd, 0);
		AppendMenu(h, MF_STRING, IDM_MYACTION, "Gnarfle the Garthok");

		prevWndProc = (WNDPROC)SetWindowLongPtr(hWnd, GWL_WNDPROC, (LONG_PTR)&myNewWndProc);
		msgf("IPC listening on hwnd (%d)  %X", hWnd, hWnd);

		if(GetRunningObjectTable(0, &rot) == S_OK){
			if( CreateFileMoniker(L"remote.forms", &mon) == S_OK){
				IDispatch *IDisp = (IDispatch*)pPlus(vbApp,0x24); //this is the vb.forms obj
				HRESULT hr = rot->Register(ROTFLAGS_REGISTRATIONKEEPSALIVE, IDisp, mon, &appRotToken);
				if(hr == S_OK){
					msgf("registered remote.forms in ROT");
				}else{
					msgf("ROT registration failed %x", hr);
				}
				mon->Release();
			}
		}

	}

	return ret;

}

bool InstallHook( void* real, void* hook, int* thunk, char* name, enum hookType ht){
	if( HookFunction((ULONG_PTR) real, (ULONG_PTR)hook, name, ht) ){ 
		*thunk = (int)GetOriginalFunction((ULONG_PTR) hook);
		return true;
	}
	return false;
}

void HookEngineDebugMessage(char* msg){
	msgf("Debug> %s", msg);
}

void InstallHooks(void)
{

	logLevel = 0; //hook engine config
	debugMsgHandler = HookEngineDebugMessage;

	msgf("***** Installing Hooks ***** threadid: %x", GetCurrentThreadId());	

	int* real = (int*)0x66017F15; //hardcoded CVBApplication_Init offset
	int hRuntime = (int)GetModuleHandle("msvbvm60.dll");
	int lpBeginPaint = (int)GetProcAddress(GetModuleHandle("user32.dll"),"BeginPaint");

	if(hRuntime==0){
		msgf("VB6 Runtime not loaded?");
		return;  
	}

	if(hRuntime != 0x66000000){
		real = (int*)(hRuntime + 0x17F15);
		msgf("Runtime not at preferred base (%x), calculating RVA (%x)",hRuntime, (int)real);
	}

	int timeStamp = *(int*)(hRuntime + 0x88); //hardcoded offset for ours, we could parse pe to get proper but fail is fail..
	if(timeStamp != 0x360C5B48){
		msgf("Runtime version mismatch timestamp does not match.");
		return;
	}

	if( *real != 0xFFF18b56 ){ //could crash if no mem there thats ok too..
		msgf("Aborting: CVBApplication_Init signature mismatch! Found: %x ", *real);
		return;  
	}
	 
	if (!InstallHook(real, My_CVBApplication_Init, (int*)&Real_CVBApplication_Init, "CVBApplication_Init", ht_jmp ) ){ 
		msg("Aborting: Install hook CVBApplication_Init failed...");
		return;
	}

	if (!InstallHook((void*)lpBeginPaint, My_BeginPaint, (int*)&Real_BeginPaint, "BeginPaint", ht_jmp ) ){ 
		msg("Aborting: Install hook BeginPaint failed...");
		return;
	} 

	msg("CVBApplication_Init and BeginPaint hooked.");
	
}
