#include <windows.h>
#include "hunt.h"

#define req_versionH 0x0001
#define req_versionL 0x0002

HINSTANCE hAudioDLL = NULL;
// SOURCEPORT: track whether audio is available so wrapper functions can safely no-op
static BOOL audioAvailable = FALSE;
void DoHalt(LPSTR);

typedef void (WINAPI * LPFUNC1)(void);
typedef void (WINAPI * LPFUNC2)(HWND, HANDLE);
typedef void (WINAPI * LPFUNC3)(float, float, float, float, float);
typedef void (WINAPI * LPFUNC4)(int, short int*, int);
typedef void (WINAPI * LPFUNC5)(int, short int*, float, float, float);
typedef void (WINAPI * LPFUNC6)(int, short int*, float, float, float, int);

typedef int  (WINAPI * LPFUNC7)(void);
typedef void (WINAPI * LPFUNC8)(int, float);


typedef void (WINAPI * LPFUNC9)(int, AudioQuad *);
LPFUNC9 audio_uploadgeometry;

LPFUNC1 audio_restore;
LPFUNC1 audiostop;
LPFUNC1 audio_shutdown;

LPFUNC2 initaudiosystem;
LPFUNC3 audiosetcamerapos;
LPFUNC4 setambient;
LPFUNC5 setambient3d;
LPFUNC6 addvoice3dv;
LPFUNC7 audio_getversion;
LPFUNC8 audio_setenvironment;


void Audio_Shutdown()
{
	if (audio_shutdown) audio_shutdown();
	if (hAudioDLL)  	FreeLibrary(hAudioDLL);
	hAudioDLL = NULL;
	audio_shutdown = NULL;
	audioAvailable = FALSE;
}


void InitAudioSystem(HWND hw, HANDLE hlog, int  driver)
{
	Audio_Shutdown();

	// SOURCEPORT: driver == -1 means audio disabled (e.g. nosnd command line flag)
	if (driver < 0) {
		PrintLog("Audio disabled by user.\n");
		return;
	}

	const char* dllNames[] = { "a_soft.dll", "a_ds3d.dll", "a_a3d.dll", "a_eax.dll" };
	const char* dllName = (driver >= 0 && driver <= 3) ? dllNames[driver] : dllNames[0];

	hAudioDLL = LoadLibrary(dllName);
	// SOURCEPORT: Don't halt if audio DLL is missing — just disable audio and continue
	if (!hAudioDLL) {
		char msg[256];
		wsprintfA(msg, "WARNING: Can't load %s — audio disabled.\n", dllName);
		PrintLog(msg);
		return;
	}

	initaudiosystem   = (LPFUNC2) GetProcAddress(hAudioDLL, "InitAudioSystem");
	audio_restore     = (LPFUNC1) GetProcAddress(hAudioDLL, "Audio_Restore");
	audiostop         = (LPFUNC1) GetProcAddress(hAudioDLL, "AudioStop");
	audio_shutdown    = (LPFUNC1) GetProcAddress(hAudioDLL, "Audio_Shutdown");
	audiosetcamerapos = (LPFUNC3) GetProcAddress(hAudioDLL, "AudioSetCameraPos");
	setambient        = (LPFUNC4) GetProcAddress(hAudioDLL, "SetAmbient");
	setambient3d      = (LPFUNC5) GetProcAddress(hAudioDLL, "SetAmbient3d");
	addvoice3dv       = (LPFUNC6) GetProcAddress(hAudioDLL, "AddVoice3dv");
	audio_getversion  = (LPFUNC7) GetProcAddress(hAudioDLL, "Audio_GetVersion");
	audio_setenvironment = (LPFUNC8) GetProcAddress(hAudioDLL, "Audio_SetEnvironment");
	audio_uploadgeometry = (LPFUNC9) GetProcAddress(hAudioDLL, "Audio_UploadGeometry");
	// SOURCEPORT: Audio_UploadGeometry is optional — not exported by retail 1.0 DLLs

	// SOURCEPORT: Check required exports — disable audio if any are missing
	if (!initaudiosystem || !audio_restore || !audiostop || !audio_shutdown ||
		!audiosetcamerapos || !setambient || !setambient3d || !addvoice3dv ||
		!audio_getversion || !audio_setenvironment) {
		PrintLog("WARNING: Audio DLL missing required exports — audio disabled.\n");
		FreeLibrary(hAudioDLL);
		hAudioDLL = NULL;
		return;
	}

	int v1 = audio_getversion()>>16;
	int v2 = audio_getversion() & 0xFFFF;
	// SOURCEPORT: downgrade version mismatch from halt to warning for retail DLL compat
	if ( (v1!=req_versionH) || (v2<req_versionL) )
		PrintLog("WARNING: Audio driver version mismatch (expected 1.2+)\n");

	initaudiosystem(hw, hlog);
	audioAvailable = TRUE;
	PrintLog("Audio system initialized.\n");
}

void Audio_UploadGeometry()
{
	UploadGeometry();
	if (audioAvailable && audio_uploadgeometry) // SOURCEPORT: optional, not in retail 1.0 DLLs
		audio_uploadgeometry(AudioFCount, data);
}



void AudioStop()
{
	if (audioAvailable && audiostop)
		audiostop();
}

void Audio_Restore()
{
	if (audioAvailable && audio_restore)
  	  audio_restore();
}



void AudioSetCameraPos(float cx, float cy, float cz, float ca, float cb)
{
	if (audioAvailable && audiosetcamerapos)
		audiosetcamerapos(cx, cy, cz, ca, cb);
}


void Audio_SetEnvironment(int e, float f)
{
	if (audioAvailable && audio_setenvironment)
		audio_setenvironment(e, f);
}


void SetAmbient(int length, short int* lpdata, int av)
{
	if (audioAvailable && setambient)
		setambient(length, lpdata, av);
}


void SetAmbient3d(int length, short int* lpdata, float cx, float cy, float cz)
{
	if (audioAvailable && setambient3d)
		setambient3d(length, lpdata, cx, cy, cz);
}


void AddVoice3dv(int length, short int* lpdata, float cx, float cy, float cz, int vol)
{
	if (audioAvailable && addvoice3dv)
		addvoice3dv(length, lpdata, cx, cy, cz, vol);
}




void AddVoice3d(int length, short int* lpdata, float cx, float cy, float cz)
{
   AddVoice3dv(length, lpdata, cx, cy, cz, 256);
}


void AddVoicev(int length, short int* lpdata, int v)
{
   AddVoice3dv(length, lpdata, 0,0,0, v);
}


void AddVoice(int length, short int* lpdata)
{
   AddVoice3dv(length, lpdata, 0,0,0, 256);
}
