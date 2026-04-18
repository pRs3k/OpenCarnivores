#include "Hunt.h"
#include "stdio.h"
#ifdef _opengl
#include "renderer/RendererGL.h"
#include "TextureOverrides.h"
#include "Materials.h"
#include "CustomMaterials.h"
#endif
#include "ModelOverrides.h"
#include "HotReload.h"
#include <string>
#include <cstring>
HANDLE hfile;
DWORD  l;

void GenerateModelMipMaps(TModel *mptr);
void GenerateAlphaFlags(TModel *mptr);

#ifdef _opengl
// SOURCEPORT: forward decl — renderd3d's d3dMemMap invalidator (hot reload).
extern void d3dInvalidateAllTextures();
extern class RendererGL* g_glRenderer;

// SOURCEPORT: hot-reload wiring for texture+material overrides. Watches every
// candidate sibling path for `basePath` (stem without extension); when any of
// them changes, re-probes the override registries and dumps the GPU texture
// cache so the next frame re-uploads. Missing candidate files are harmless —
// GetMtime returns -1 and the watch stays dormant until the file appears.
static void HotWatchOverrideSet(void* key, std::string base) {
    static const char* baseExts[]   = { ".dds", ".png", ".tga", ".bmp", ".jpg" };
    static const char* pbrSuffixes[] = { "_normal.png", "_mr.png", "_ao.png" };
    auto reload = [key, base]() {
        TextureOverrides::TryRegisterWithExts(key, base.c_str());
        Materials::TryRegisterWithExts(key, base.c_str());
        if (g_glRenderer) g_glRenderer->InvalidateTextureCache();
        d3dInvalidateAllTextures();
        PrintLog("[HotReload] texture reloaded\n");
    };
    for (const char* ext : baseExts)    HotReload::Watch((base + ext).c_str(), reload);
    for (const char* sfx : pbrSuffixes) HotReload::Watch((base + sfx).c_str(), reload);

    // SOURCEPORT: custom-shader .material file — separate reload callback so
    // editing the material script re-parses + recompiles without touching the
    // texture cache. Writes to the base.material file trigger this. If the
    // material wasn't registered at asset-load time (modder creates the file
    // live), we fall back to TryRegisterWithExts so first-time detection also
    // works without a game restart.
    auto reloadMat = [key, base]() {
        if (CustomMaterials::Get(key))
            CustomMaterials::Reload(key);
        else
            CustomMaterials::TryRegisterWithExts(key, base.c_str());
        PrintLog("[HotReload] .material reloaded\n");
    };
    HotReload::Watch((base + ".material").c_str(), reloadMat);
}

// Variant for sibling-style call sites that pass a full filename (e.g.
// "HUNTDAT\\BINOCUL.3DF"); strips the extension first.
static void HotWatchOverrideSibling(void* key, const char* sourcePath) {
    std::string p = sourcePath ? sourcePath : "";
    size_t dot = p.find_last_of('.');
    std::string stem = (dot == std::string::npos) ? p : p.substr(0, dot);
    HotWatchOverrideSet(key, stem);
}
#endif


LPVOID _HeapAlloc(HANDLE hHeap, 
                  DWORD dwFlags, 
                  DWORD dwBytes)
{
   LPVOID res = HeapAlloc(hHeap, 
                          dwFlags | HEAP_ZERO_MEMORY,
                          dwBytes);
   if (!res)
	   DoHalt("Heap allocation error!");

   HeapAllocated+=dwBytes;
   return res;
}


BOOL _HeapFree(HANDLE hHeap, 
               DWORD  dwFlags, 
               LPVOID lpMem)
{	
	if (!lpMem) return FALSE;
	
	HeapReleased+=
		HeapSize(hHeap, HEAP_NO_SERIALIZE, lpMem);

	BOOL res = HeapFree(hHeap, 
                       dwFlags, 
                       lpMem);
	if (!res)
		DoHalt("Heap free error!");
	
	return res;
}


void AddMessage(LPSTR mt)
{
  MessageList.timeleft = timeGetTime() + 2 * 1000;
  lstrcpy(MessageList.mtext, mt);
}

void PlaceHunter()
{
  if (LockLanding) return;

  if (TrophyMode) {
	  PlayerX = 76*256+128;
      PlayerZ = 70*256+128;
	  PlayerY = GetLandQH(PlayerX, PlayerZ);
	  return;
  }
  
  int p = (timeGetTime() % LandingList.PCount);
  PlayerX = (float)LandingList.list[p].x * 256+128;
  PlayerZ = (float)LandingList.list[p].y * 256+128;
  PlayerY = GetLandQH(PlayerX, PlayerZ);
}


int DitherHi(int C)
{
  int d = C & 255;  
  C = C / 256;
  if (rand() * 255 / RAND_MAX < d) C++;
  if (C>31) C=31;
  return C;
}




void CreateWaterTab()
{
  for (int c=0; c<0x8000; c++) {
     int R = (c >> 10);
	 int G = (c >>  5) & 31;
	 int B = c & 31;
     R =  1+(R * 8 ) / 28; if (R>31) R=31;
	 G =  2+(G * 18) / 28; if (G>31) G=31;
	 B =  3+(B * 22) / 28; if (B>31) B=31;     
	 FadeTab[64][c] = HiColor(R, G, B);
   }
}

void CreateFadeTab()
{
#ifdef _soft
  for (int l=0; l<64; l++) 
   for (int c=0; c<0x8000; c++) {
     int R = (c >> 10);
	 int G = (c >>  5) & 31;
	 int B = c & 31;
     
	 R = (int)((float)R * (l) / 60.f + (float)rand() *0.2f / RAND_MAX); if (R>31) R=31;
	 G = (int)((float)G * (l) / 60.f + (float)rand() *0.2f / RAND_MAX); if (G>31) G=31;
	 B = (int)((float)B * (l) / 60.f + (float)rand() *0.2f / RAND_MAX); if (B>31) B=31;
	 FadeTab[l][c] = HiColor(R, G, B);
   }

  CreateWaterTab();
#endif
}


void CreateDivTable()
{
  DivTbl[0] = 0x7fffffff;
  DivTbl[1] = 0x7fffffff;
  DivTbl[2] = 0x7fffffff;
  for( int i = 3; i < 10240; i++ )
     DivTbl[i] = (int) ((float)0x100000000 / i);

  for (int y=0; y<32; y++)
   for (int x=0; x<32; x++)
    RandomMap[y][x] = rand() * 1024 / RAND_MAX;
}


void CreateVideoDIB()
{
    hdcMain=GetDC(hwndMain);
    hdcCMain = CreateCompatibleDC(hdcMain);

    SelectObject(hdcMain,  fnt_Midd);
	SelectObject(hdcCMain, fnt_Midd);

	BITMAPINFOHEADER bmih;
	bmih.biSize = sizeof( BITMAPINFOHEADER ); 	
    bmih.biWidth  =1024; 
    bmih.biHeight = -768; 	
    bmih.biPlanes = 1; 
    bmih.biBitCount = 16;
    bmih.biCompression = BI_RGB; 
    bmih.biSizeImage = 0;
    bmih.biXPelsPerMeter = 400; 
    bmih.biYPelsPerMeter = 400; 
    bmih.biClrUsed = 0; 
    bmih.biClrImportant = 0;    

	BITMAPINFO binfo;
	binfo.bmiHeader = bmih;
	hbmpVideoBuf = 
     CreateDIBSection(hdcMain, &binfo, DIB_RGB_COLORS, &lpVideoBuf, NULL, 0);   
}






int GetObjectH(int x, int y, int R) 
{
  x = (x<<8) + 128;
  y = (y<<8) + 128;
  float hr,h;
  hr =GetLandH((float)x,    (float)y);
  h = GetLandH( (float)x+R, (float)y); if (h < hr) hr = h;
  h = GetLandH( (float)x-R, (float)y); if (h < hr) hr = h;
  h = GetLandH( (float)x,   (float)y+R); if (h < hr) hr = h;
  h = GetLandH( (float)x,   (float)y-R); if (h < hr) hr = h;
  hr += 15;
  return  (int) (hr / ctHScale);
}


int GetObjectHWater(int x, int y) 
{
  if (FMap[y][x] & fmReverse)
   return (int)(HMap[y][x+1]+HMap[y+1][x]) / 2 + 48;
                else
   return (int)(HMap[y][x]+HMap[y+1][x+1]) / 2 + 48;
}



void CreateTMap()
{
 int x,y; 
 LandingList.PCount = 0;
 for (y=0; y<gMapSize; y++)
     for (x=0; x<gMapSize; x++) {          
          if (TMap1[y][x]==0xFFFF) TMap1[y][x] = 1;
          if (TMap2[y][x]==0xFFFF) TMap2[y][x] = 1;          
     }

	              				  
/*
  for (y=1; y<gMapSize-1; y++)
     for (x=1; x<gMapSize-1; x++) 
		 if (!(FMap[y][x] & fmWater) ) {			 
			 
			 if (FMap[y  ][x+1] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y  ][x+1];}
			 if (FMap[y+1][x  ] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y+1][x  ];}
			 if (FMap[y  ][x-1] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y  ][x-1];}
			 if (FMap[y-1][x  ] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y-1][x  ];}

			 if (FMap[y][x] & fmWater2) 			     
			     if (HMap[y][x] > WaterList[WMap[y][x]].wlevel) HMap[y][x]=WaterList[WMap[y][x]].wlevel;					 
		 }

  for (y=1; y<gMapSize-1; y++)
     for (x=1; x<gMapSize-1; x++) 
		 if (FMap[y][x] & fmWater2) {
			 FMap[y][x]-=fmWater2;
			 FMap[y][x]+=fmWater;
		 }
*/


  for (y=1; y<gMapSize-1; y++)
     for (x=1; x<gMapSize-1; x++) 
		 if (!(FMap[y][x] & fmWater) ) {			 
			 
			 if (FMap[y  ][x+1] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y  ][x+1];}
			 if (FMap[y+1][x  ] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y+1][x  ];}
			 if (FMap[y  ][x-1] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y  ][x-1];}
			 if (FMap[y-1][x  ] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y-1][x  ];}

			 BOOL l = TRUE;

#ifdef _soft
			 if (FMap[y][x] & fmWater2) {
			     l = FALSE;				 
			     if (HMap[y][x] > WaterList[WMap[y][x]].wlevel) HMap[y][x]=WaterList[WMap[y][x]].wlevel;				 
				 HMap[y][x]=WaterList[WMap[y][x]].wlevel;
			 }
#endif

			 if (FMap[y-1][x-1] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y-1][x-1];}
			 if (FMap[y-1][x+1] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y-1][x+1];}
			 if (FMap[y+1][x-1] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y+1][x-1];}
			 if (FMap[y+1][x+1] & fmWater) { FMap[y][x]|= fmWater2; WMap[y][x] = WMap[y+1][x+1];}

			 if (l)
			 if (FMap[y][x] & fmWater2) 
				 if (HMap[y][x] == WaterList[WMap[y][x]].wlevel) HMap[y][x]+=1;

			 //if (FMap[y][x] & fmWater2) 				 			 				 
				 
		 }
			
#ifdef _soft
   for (y=0; y<gMapSize; y++)
      for (x=0; x<gMapSize; x++ ) {
         if( abs( HMap[y][x]-HMap[y+1][x+1] ) > abs( HMap[y+1][x]-HMap[y][x+1] ) )
            FMap[y][x] |= fmReverse;
         else
            FMap[y][x] &= ~fmReverse;
      }
#endif


  for (y=0; y<gMapSize; y++)
   for (x=0; x<gMapSize; x++) {

	if (!(FMap[y][x] & fmWaterA))
		WMap[y][x]=255;

#ifdef _soft
	if (MObjects[OMap[y][x]].info.flags & ofNOSOFT2) 
	  if ( (x+y) & 1 )
		OMap[y][x]=255;

	if (MObjects[OMap[y][x]].info.flags & ofNOSOFT) 	
		OMap[y][x]=255;
#endif

	if (OMap[y][x]==254) {		
	   LandingList.list[LandingList.PCount].x = x;
	   LandingList.list[LandingList.PCount].y = y;
	   LandingList.PCount++;
       OMap[y][x]=255;
	}

    int ob = OMap[y][x];
    if (ob == 255) { HMapO[y][x] = 0; continue; }
    
    //HMapO[y][x] = GetObjectH(x,y);     
    if (MObjects[ob].info.flags & ofPLACEGROUND) HMapO[y][x] = GetObjectH(x,y, MObjects[ob].info.GrRad);
    //if (MObjects[ob].info.flags & ofPLACEWATER)  HMapO[y][x] = GetObjectHWater(x,y);
    
   } 

   if (!LandingList.PCount) {
	   LandingList.list[LandingList.PCount].x = 256;
	   LandingList.list[LandingList.PCount].y = 256;
	   LandingList.PCount=1;
   }

   if (TrophyMode) {
       LandingList.PCount = 0;
	   for (x=0; x<6; x++) { 
		   LandingList.list[LandingList.PCount].x = 69 + x*3;
		   LandingList.list[LandingList.PCount].y = 66;
		   LandingList.PCount++;
	   }

	   for (y=0; y<6; y++) { 
		   LandingList.list[LandingList.PCount].x = 87;
		   LandingList.list[LandingList.PCount].y = 69 + y*3;
		   LandingList.PCount++;
	   }

	   for (x=0; x<6; x++) { 
		   LandingList.list[LandingList.PCount].x = 84 - x*3;
		   LandingList.list[LandingList.PCount].y = 87;
		   LandingList.PCount++;
	   }

	   for (y=0; y<6; y++) { 
		   LandingList.list[LandingList.PCount].x = 66;
		   LandingList.list[LandingList.PCount].y = 84 - y*3;
		   LandingList.PCount++;
	   }
   }


}



void CreateMipMap(WORD* src, WORD* dst, int Ls, int Ld)
{ 
	int scale = Ls / Ld;

  int R[64][64], G[64][64], B[64][64];

  FillMemory(R, sizeof(R), 0);
  FillMemory(G, sizeof(R), 0);
  FillMemory(B, sizeof(R), 0);

  for (int y=0; y<Ls; y++)
   for (int x=0; x<Ls; x++) {
	WORD C = *(src + x + y*Ls);
	B[ y/scale ][ x/scale ]+= (C>> 0) & 31;
	G[ y/scale ][ x/scale ]+= (C>> 5) & 31;
	R[ y/scale ][ x/scale ]+= (C>>10) & 31;
   }

  scale*=scale;

  // SOURCEPORT: added int (MSVC6 scoping fix)
  for (int y2=0; y2<Ld; y2++)
   for (int x=0; x<Ld; x++) {
	   R[y2][x]/=scale;
	   G[y2][x]/=scale;
	   B[y2][x]/=scale;
	   *(dst + x + y2*Ld) = (R[y2][x]<<10) + (G[y2][x]<<5) + B[y2][x];
   }
}



int CalcImageDifference(WORD* A, WORD* B, int L)
{
	int r = 0; L*=L;
	for (int l=0; l<L; l++) {
		WORD C1 = *(A + l);
		WORD C2 = *(B + l);
		int R1 = (C1>>10) & 31;
		int G1 = (C1>> 5) & 31;
		int B1 = (C1>> 0) & 31;
		int R2 = (C2>>10) & 31;
		int G2 = (C2>> 5) & 31;
		int B2 = (C2>> 0) & 31;

		r+=(R1-R2)*(R1-R2) + 
		   (G1-G2)*(G1-G2) + 
		   (B1-B2)*(B1-B2);
	}

	return r;
}


void RotateImage(WORD* src, WORD* dst, int L)
{
   for (int y=0; y<L; y++)
	for (int x=0; x<L; x++)
	  *(dst + x*L + (L-1-y) ) = *(src + x + y*L);
}


void BrightenTexture(WORD* A, int L)
{
	// SOURCEPORT: brightness is now a live shader uniform (uBrightness) rather than
	// baked into texture data — so changing the slider takes effect immediately and
	// old configs with legacy OptBrightness=0 don't produce dark textures.
	// This function only applies the night-mode colour desaturation (green-tint).
	if (OptDayNight != 2) return;  // day/sunset: nothing to bake
	for (int c=0; c<L; c++) {
		WORD w = *(A + c);
		int G = (w >> 5) & 31;
		int B = G >> 3;   // night vision: desaturate to green tint
		int R = G >> 3;
		*(A + c) = (B) + (G << 5) + (R << 10);
	}
}

void GenerateMipMap(WORD* A, WORD* D, int L)
{ 
  for (int y=0; y<L; y++)
   for (int x=0; x<L; x++) {
	int C1 = *(A + x*2 +   (y*2+0)*2*L);
	int C2 = *(A + x*2+1 + (y*2+0)*2*L);
	int C3 = *(A + x*2 +   (y*2+1)*2*L);
	int C4 = *(A + x*2+1 + (y*2+1)*2*L);
	//C4 = C1;
	/*
    if (L==64)
     C3=((C3 & 0x7bde) +  (C1 & 0x7bde))>>1;      
     */
	int B = ( ((C1>>0) & 31) + ((C2>>0) & 31) + ((C3>>0) & 31) + ((C4>>0) & 31) +2 ) >> 2;
    int G = ( ((C1>>5) & 31) + ((C2>>5) & 31) + ((C3>>5) & 31) + ((C4>>5) & 31) +2 ) >> 2;
    int R = ( ((C1>>10) & 31) + ((C2>>10) & 31) + ((C3>>10) & 31) + ((C4>>10) & 31) +2 ) >> 2;
	*(D + x + y * L) = HiColor(R,G,B);
   }
}


int CalcColorSum(WORD* A, int L)
{
  int R = 0, G = 0, B = 0;
  for (int x=0; x<L; x++) {
	B+= (*(A+x) >> 0) & 31;
	G+= (*(A+x) >> 5) & 31;
	R+= (*(A+x) >>10) & 31;
  }
  return HiColor(R/L, G/L, B/L);
}


void GenerateShadedMipMap(WORD* src, WORD* dst, int L)
{
  for (int x=0; x<16*16; x++) {
	int B = (*(src+x) >> 0) & 31;
	int G = (*(src+x) >> 5) & 31;
	int R = (*(src+x) >>10) & 31;
	R=DitherHi(SkyR*L/8 + R*(256-L)+6);
	G=DitherHi(SkyG*L/8 + G*(256-L)+6);
	B=DitherHi(SkyB*L/8 + B*(256-L)+6);
	*(dst + x) = HiColor(R,G,B);
  }
}


void GenerateShadedSkyMipMap(WORD* src, WORD* dst, int L)
{
  for (int x=0; x<128*128; x++) {
	int B = (*(src+x) >> 0) & 31;
	int G = (*(src+x) >> 5) & 31;
	int R = (*(src+x) >>10) & 31;
	R=DitherHi(SkyR*L/8 + R*(256-L)+6);
	G=DitherHi(SkyG*L/8 + G*(256-L)+6);
	B=DitherHi(SkyB*L/8 + B*(256-L)+6);
	*(dst + x) = HiColor(R,G,B);
  }
}


void DATASHIFT(WORD* d, int cnt)
{
  cnt>>=1;
  /*
  for (int l=0; l<cnt; l++)
	  *(d+l)=(*(d+l)) & 0x3e0;
*/
  if (HARD3D) return;

  // SOURCEPORT: added int (MSVC6 scoping fix)
  for (int l=0; l<cnt; l++)
	  *(d+l)*=2;

}



void ApplyAlphaFlags(WORD* tptr, int cnt)
{
#ifdef _d3d
	for (int w=0; w<cnt; w++)
		*(tptr+w)|=0x8000;
#endif
}


void CalcMidColor(WORD* tptr, int l, int &mr, int &mg, int &mb)
{
	for (int w=0; w<l; w++) {
		WORD c = *(tptr + w);
		mb+=((c>> 0) & 31)*8;
		mg+=((c>> 5) & 31)*8;
		mr+=((c>>10) & 31)*8;
	}

	mr/=l; mg/=l; mb/=l;
}

void LoadTexture(TEXTURE* &T)
{
    T = (TEXTURE*) _HeapAlloc(Heap, 0, sizeof(TEXTURE));     	
    DWORD L;        	    
    ReadFile(hfile, T->DataA, 128*128*2, &L, NULL);
	for (int y=0; y<128; y++)
	    for (int x=0; x<128; x++)
			if (!T->DataA[y*128+x]) T->DataA[y*128+x]=1;
    	
	BrightenTexture(T->DataA, 128*128);

	CalcMidColor(T->DataA, 128*128, T->mR, T->mG, T->mB);
		
	GenerateMipMap(T->DataA, T->DataB, 64);
	for (int i = 0; i < 64*64; i++) if (!T->DataB[i]) T->DataB[i] = 1;  // SOURCEPORT: prevent alpha=0 discard in GL shader
	GenerateMipMap(T->DataB, T->DataC, 32);
	for (int i = 0; i < 32*32; i++) if (!T->DataC[i]) T->DataC[i] = 1;
	GenerateMipMap(T->DataC, T->DataD, 16);
	for (int i = 0; i < 16*16; i++) if (!T->DataD[i]) T->DataD[i] = 1;
	memcpy(T->SDataC[0], T->DataC, 32*32*2);
	memcpy(T->SDataC[1], T->DataC, 32*32*2);

	DATASHIFT((unsigned short *)T, sizeof(TEXTURE));
    for (int w=0; w<32*32; w++)
	 T->SDataC[1][w] = FadeTab[48][T->SDataC[1][w]>>1];

	ApplyAlphaFlags(T->DataA, 128*128);
	ApplyAlphaFlags(T->DataB, 64*64);
	ApplyAlphaFlags(T->DataC, 32*32);
}




void LoadSky()
{	        
	SetFilePointer(hfile, 256*512*OptDayNight, NULL, FILE_CURRENT);
    ReadFile(hfile, SkyPic, 256*256*2, &l, NULL);
	SetFilePointer(hfile, 256*512*(2-OptDayNight), NULL, FILE_CURRENT);

    BrightenTexture(SkyPic, 256*256);

    for (int y=0; y<128; y++)
        for (int x=0; x<128; x++) 
            SkyFade[0][y*128+x] = SkyPic[y*2*256  + x*2];         

    for (int l=1; l<8; l++)
       GenerateShadedSkyMipMap(SkyFade[0], SkyFade[l], l*32-16);
    GenerateShadedSkyMipMap(SkyFade[0], SkyFade[8], 250);
	ApplyAlphaFlags(SkyPic, 256*256);
    //DATASHIFT(SkyPic, 256*256*2);
}


void LoadSkyMap()
{	        
    ReadFile(hfile, SkyMap, 128*128, &l, NULL);
}





void fp_conv(LPVOID d)
{
	int i;
	float f;
	memcpy(&i, d, 4);
#if defined(_d3d) || defined(_opengl)
	f = ((float)i) / 256.f;
#else
	f = ((float)i);
#endif
	memcpy(d, &f, 4);
}



void CorrectModel(TModel *mptr)
{
	TFace tface[1024];

    for (int f=0; f<mptr->FCount; f++) {	 
     if (!(mptr->gFace[f].Flags & sfDoubleSide))
        mptr->gFace[f].Flags |= sfNeedVC;
#ifdef _soft
	 mptr->gFace[f].tax = (mptr->gFace[f].tax<<16) + 0x8000;
     mptr->gFace[f].tay = (mptr->gFace[f].tay<<16) + 0x8000;
     mptr->gFace[f].tbx = (mptr->gFace[f].tbx<<16) + 0x8000;
     mptr->gFace[f].tby = (mptr->gFace[f].tby<<16) + 0x8000;
     mptr->gFace[f].tcx = (mptr->gFace[f].tcx<<16) + 0x8000;
     mptr->gFace[f].tcy = (mptr->gFace[f].tcy<<16) + 0x8000;
#else
	 fp_conv(&mptr->gFace[f].tax);
     fp_conv(&mptr->gFace[f].tay);
     fp_conv(&mptr->gFace[f].tbx);
     fp_conv(&mptr->gFace[f].tby);
     fp_conv(&mptr->gFace[f].tcx);
     fp_conv(&mptr->gFace[f].tcy);
#endif
    }

	int fp = 0;
	// SOURCEPORT: added int to all three loops (MSVC6 scoping fix)
    for (int f2=0; f2<mptr->FCount; f2++)
		if ( (mptr->gFace[f2].Flags & (sfOpacity | sfTransparent))==0)
		{
			tface[fp] = mptr->gFace[f2];
            fp++;
		}

	for (int f3=0; f3<mptr->FCount; f3++)
		if ( (mptr->gFace[f3].Flags & sfOpacity)!=0)
		{
			tface[fp] = mptr->gFace[f3];
            fp++;
		}

    for (int f4=0; f4<mptr->FCount; f4++)
		if ( (mptr->gFace[f4].Flags & sfTransparent)!=0)
		{
			tface[fp] = mptr->gFace[f4];
            fp++;
		}


    
    memcpy( mptr->gFace, tface, sizeof(tface) );
}

void LoadModel(TModel* &mptr)
{         
    mptr = (TModel*) _HeapAlloc(Heap, 0, sizeof(TModel));

    ReadFile( hfile, &mptr->VCount,      4,         &l, NULL );
	ReadFile( hfile, &mptr->FCount,      4,         &l, NULL );
    ReadFile( hfile, &OCount,            4,         &l, NULL );
	ReadFile( hfile, &mptr->TextureSize, 4,         &l, NULL );
	ReadFile( hfile, mptr->gFace,        mptr->FCount<<6, &l, NULL );
	ReadFile( hfile, mptr->gVertex,      mptr->VCount<<4, &l, NULL );
	ReadFile( hfile, gObj,               OCount*48, &l, NULL );

	if (HARD3D) CalcLights(mptr);

    int ts = mptr->TextureSize;
    
    if (HARD3D) mptr->TextureHeight = 256;
          else  mptr->TextureHeight = mptr->TextureSize>>9;    

    mptr->TextureSize = mptr->TextureHeight*512;

    mptr->lpTexture = (WORD*) _HeapAlloc(Heap, 0, mptr->TextureSize);

    ReadFile(hfile, mptr->lpTexture, ts, &l, NULL);
	BrightenTexture(mptr->lpTexture, ts/2);

    for (int v=0; v<mptr->VCount; v++) {
     mptr->gVertex[v].x*=2.f;
     mptr->gVertex[v].y*=2.f;
     mptr->gVertex[v].z*=-2.f;
    }

	CorrectModel(mptr);
        
    DATASHIFT(mptr->lpTexture, mptr->TextureSize);
}



void LoadAnimation(TVTL &vtl)
{
   int vc;
   DWORD l;

   ReadFile( hfile, &vc,          4,    &l, NULL );
   ReadFile( hfile, &vc,          4,    &l, NULL );
   ReadFile( hfile, &vtl.aniKPS,  4,    &l, NULL );
   ReadFile( hfile, &vtl.FramesCount,  4,    &l, NULL );
   vtl.FramesCount++;

   vtl.AniTime = (vtl.FramesCount * 1000) / vtl.aniKPS;
   vtl.aniData = (short int*) 
    _HeapAlloc(Heap, 0, (vc*vtl.FramesCount*6) );
   ReadFile( hfile, vtl.aniData, (vc*vtl.FramesCount*6), &l, NULL);

}



void LoadModelEx(TModel* &mptr, char* FName)
{    
    
    hfile = CreateFile(FName,
        GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hfile==INVALID_HANDLE_VALUE) {		
        char sz[512];
        wsprintf( sz, "Error opening file\n%s.", FName );
		DoHalt(sz);        
    }

    mptr = (TModel*) _HeapAlloc(Heap, 0, sizeof(TModel));

    ReadFile( hfile, &mptr->VCount,      4,         &l, NULL );
	ReadFile( hfile, &mptr->FCount,      4,         &l, NULL );
    ReadFile( hfile, &OCount,            4,         &l, NULL );
	ReadFile( hfile, &mptr->TextureSize, 4,         &l, NULL );
	ReadFile( hfile, mptr->gFace,        mptr->FCount<<6, &l, NULL );
	ReadFile( hfile, mptr->gVertex,      mptr->VCount<<4, &l, NULL );
	ReadFile( hfile, gObj,               OCount*48, &l, NULL );

    int ts = mptr->TextureSize;
	if (HARD3D) mptr->TextureHeight = 256;
          else  mptr->TextureHeight = mptr->TextureSize>>9;    
    mptr->TextureSize = mptr->TextureHeight*512;

    mptr->lpTexture = (WORD*) _HeapAlloc(Heap, 0, mptr->TextureSize);

    ReadFile(hfile, mptr->lpTexture, ts, &l, NULL);
	BrightenTexture(mptr->lpTexture, ts/2);

    for (int v=0; v<mptr->VCount; v++) {
     mptr->gVertex[v].x*=2.f;
     mptr->gVertex[v].y*=2.f;
     mptr->gVertex[v].z*=-2.f;
    }

	CorrectModel(mptr);
        
    DATASHIFT(mptr->lpTexture, mptr->TextureSize);
#ifdef _opengl
    // SOURCEPORT: DATASHIFT is a no-op in HARD3D/GL mode, but software renderer
    // doubles every non-zero texture pixel via DATASHIFT before rendering.
    // Near-model .3DF textures (compass, binoculars, weapon HUD) are designed
    // to be viewed at that 2x brightness.  Apply the equivalent multiply here so
    // GL output matches the software-renderer reference.
    {
        int n = mptr->TextureSize / 2; // TextureSize is in bytes; /2 = pixel count
        WORD* t = mptr->lpTexture;
        // SOURCEPORT: must double per-channel, NOT the raw 16-bit word.
        // Raw word *2 overflows channel bits into adjacent channels
        // (e.g. R=20 → raw shift bleeds into G bits → wrong hue).
        for (int i = 0; i < n; i++) {
            if (!t[i]) continue;
            int r = min(31, ((t[i] >> 10) & 31) << 1);
            int g = min(31, ((t[i] >> 5)  & 31) << 1);
            int b = min(31, ( t[i]         & 31) << 1);
            t[i] = (WORD)((r << 10) | (g << 5) | b);
        }
    }
#endif
	GenerateModelMipMaps(mptr);
	GenerateAlphaFlags(mptr);

#ifdef _opengl
	// SOURCEPORT: try to load a PNG/TGA override sibling (e.g. BINOCUL.png
	// next to BINOCUL.3DF).  If present, the GL renderer will upload the
	// 32-bit image instead of the 16-bit RGB555 data.
	TextureOverrides::TryRegisterSibling(mptr->lpTexture, FName);
	Materials::TryRegisterSibling(mptr->lpTexture, FName);
	CustomMaterials::TryRegisterSibling(mptr->lpTexture, FName);
	HotWatchOverrideSibling(mptr->lpTexture, FName);
#endif
	// SOURCEPORT: also probe for a sibling OBJ/glTF replacement mesh (e.g.
	// BINOCUL.obj next to BINOCUL.3DF). Static-only — animated .CAR models
	// are handled separately.
	ModelOverrides::TrySiblingAny(mptr, FName);
}




void LoadWav(char* FName, TSFX &sfx)
{
  DWORD l;  

  HANDLE hfile = CreateFile(FName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if( hfile==INVALID_HANDLE_VALUE ) {		
        char sz[512];
        wsprintf( sz, "Error opening file\n%s.", FName );
		DoHalt(sz);        
    }
  
   _HeapFree(Heap, 0, (void*)sfx.lpData);
   sfx.lpData = NULL;

   SetFilePointer( hfile, 36, NULL, FILE_BEGIN );

   char c[5]; c[4] = 0;

   for ( ; ; ) {
      ReadFile( hfile, c, 1, &l, NULL );
      if( c[0] == 'd' ) {
         ReadFile( hfile, &c[1], 3, &l, NULL );
         if( !lstrcmp( c, "data" ) ) break;
          else SetFilePointer( hfile, -3, NULL, FILE_CURRENT );
      }
   }

   ReadFile( hfile, &sfx.length, 4, &l, NULL );

   sfx.lpData = (short int*) 
    _HeapAlloc( Heap, 0, sfx.length );
   
   ReadFile( hfile, sfx.lpData, sfx.length, &l, NULL );  
   CloseHandle(hfile);    
}


WORD conv_565(WORD c)
{
	return (c & 31) + ( (c & 0xFFE0) << 1 );
}


int conv_xGx(int c) {
	if (OptDayNight!=2) return c;
	DWORD a = c;
	int r = ((c>> 0) & 0xFF);
	int g = ((c>> 8) & 0xFF);
	int b = ((c>>16) & 0xFF);
	c = max(r,g);
	c = max(c,b);
	return (c<<8) + (a & 0xFF000000);
}

void conv_pic(TPicture &pic)
{
	if (!HARD3D) return;
	for (int y=0; y<pic.H; y++)
		for (int x=0; x<pic.W; x++)
			*(pic.lpImage + x + y*pic.W) = conv_565(*(pic.lpImage + x + y*pic.W));
}




void LoadPicture(TPicture &pic, LPSTR pname)
{
    int C;
    byte fRGB[800][3];
    BITMAPFILEHEADER bmpFH;
    BITMAPINFOHEADER bmpIH;
    DWORD l;
    HANDLE hfile;

    hfile = CreateFile(pname, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if( hfile==INVALID_HANDLE_VALUE ) {		
        char sz[512];
        wsprintf( sz, "Error opening file\n%s.", pname );
		DoHalt(sz);        
    }

    ReadFile( hfile, &bmpFH, sizeof( BITMAPFILEHEADER ), &l, NULL );
    ReadFile( hfile, &bmpIH, sizeof( BITMAPINFOHEADER ), &l, NULL );

	
	_HeapFree(Heap, 0, (void*)pic.lpImage);
	pic.lpImage = NULL;
	
	pic.W = bmpIH.biWidth;
    pic.H = bmpIH.biHeight;
	pic.lpImage = (WORD*) _HeapAlloc(Heap, 0, pic.W * pic.H * 2);



    for (int y=0; y<pic.H; y++) {      
      ReadFile( hfile, fRGB, 3*pic.W, &l, NULL );
      for (int x=0; x<pic.W; x++) {     
       C = ((int)fRGB[x][2]/8<<10) + ((int)fRGB[x][1]/8<< 5) + ((int)fRGB[x][0]/8) ;
       *(pic.lpImage + (pic.H-y-1)*pic.W+x) = C;
      }
    }
   
    CloseHandle( hfile );    
}



void LoadPictureTGA(TPicture &pic, LPSTR pname)
{
    DWORD l;
	WORD w,h;
    HANDLE hfile;

	
    hfile = CreateFile(pname, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL );
    if( hfile==INVALID_HANDLE_VALUE ) {		
        char sz[512];
        wsprintf( sz, "Error opening file\n%s.", pname );
		DoHalt(sz);        
    }

	SetFilePointer(hfile, 12, 0, FILE_BEGIN);

    ReadFile( hfile, &w, 2, &l, NULL );
    ReadFile( hfile, &h, 2, &l, NULL );

	SetFilePointer(hfile, 18, 0, FILE_BEGIN);
	
	_HeapFree(Heap, 0, (void*)pic.lpImage);
	pic.lpImage = NULL;

	pic.W = w;
    pic.H = h;
	pic.lpImage = (WORD*) _HeapAlloc(Heap, 0, pic.W * pic.H * 2);

    for (int y=0; y<pic.H; y++)
      ReadFile( hfile, (void*)(pic.lpImage + (pic.H-y-1)*pic.W), 2*pic.W, &l, NULL );

    CloseHandle( hfile );

    // SOURCEPORT: menu/UI override. Key by &pic (the TPicture struct's stable
    // global address), NOT pic.lpImage (heap-recycled after ReleaseResources →
    // caused cross-asset bleed onto terrain). Skip `.tga` in the sibling probe
    // because pname itself is a .tga; only `.dds/.png/.bmp/.jpg` are considered
    // valid high-res replacements. Hot-reload watches the same stem so live
    // edits swap in without a restart.
    {
        std::string base(pname);
        size_t dot = base.find_last_of('.');
        if (dot != std::string::npos) base.erase(dot);

        TextureOverrides::TryRegisterMenuPicture(&pic, base.c_str());

        void* picKey = &pic;
        std::string baseCopy = base;
        auto reload = [picKey, baseCopy]() {
            // Re-probe: if the file still exists it re-registers; if it was
            // deleted, leave the previous override in place (acceptable —
            // explicit unregister would require tracking which ext was active).
            TextureOverrides::TryRegisterMenuPicture(picKey, baseCopy.c_str());
            PrintLog("[HotReload] menu picture reloaded\n");
        };
        HotReload::Watch((base + ".dds").c_str(), reload);
        HotReload::Watch((base + ".png").c_str(), reload);
        HotReload::Watch((base + ".bmp").c_str(), reload);
        HotReload::Watch((base + ".jpg").c_str(), reload);
    }
}




void CreateMipMapMT(WORD* dst, WORD* src, int H)
{
    for (int y=0; y<H; y++) 
        for (int x=0; x<127; x++) {
        int C1 = *(src + (x*2+0) + (y*2+0)*256);
        int C2 = *(src + (x*2+1) + (y*2+0)*256);
        int C3 = *(src + (x*2+0) + (y*2+1)*256);
        int C4 = *(src + (x*2+1) + (y*2+1)*256);

        if (!HARD3D) { C1>>=1; C2>>=1; C3>>=1; C4>>=1; }

         // SOURCEPORT: original code exited early when C1==0, zeroing the mipmap texel
         // even when C2/C3/C4 were opaque leaf pixels — causing leaves to vanish in
         // lpTexture2 (the distance-LOD texture used at d > 12*256).
         // Fix: only output transparent when ALL four source pixels are transparent.
         // Otherwise borrow a non-zero neighbour so the average stays in leaf-colour space.
         if (!C1 && !C2 && !C3 && !C4) { *(dst + x + y*128) = 0; continue; }
         if (!C1) C1 = C2 ? C2 : (C3 ? C3 : C4);
         if (!C2) C2=C1;
         if (!C3) C3=C1;
         if (!C4) C4=C1;

         int B = ( ((C1>> 0) & 31) + ((C2 >>0) & 31) + ((C3 >>0) & 31) + ((C4 >>0) & 31) +2 ) >> 2;
         int G = ( ((C1>> 5) & 31) + ((C2 >>5) & 31) + ((C3 >>5) & 31) + ((C4 >>5) & 31) +2 ) >> 2;
         int R = ( ((C1>>10) & 31) + ((C2>>10) & 31) + ((C3>>10) & 31) + ((C4>>10) & 31) +2 ) >> 2;
         if (!HARD3D) *(dst + x + y * 128) = HiColor(R,G,B)*2;
                 else *(dst + x + y * 128) = HiColor(R,G,B);
        }
}



void CreateMipMapMT2(WORD* dst, WORD* src, int H)
{
    for (int y=0; y<H; y++) 
        for (int x=0; x<63; x++) {
        int C1 = *(src + (x*2+0) + (y*2+0)*128);
        int C2 = *(src + (x*2+1) + (y*2+0)*128);
        int C3 = *(src + (x*2+0) + (y*2+1)*128);
        int C4 = *(src + (x*2+1) + (y*2+1)*128);

		if (!HARD3D) { C1>>=1; C2>>=1; C3>>=1; C4>>=1; }

         // SOURCEPORT: same fix as CreateMipMapMT — only zero when all 4 are transparent.
         if (!C1 && !C2 && !C3 && !C4) { *(dst + x + y*64) = 0; continue; }
         if (!C1) C1 = C2 ? C2 : (C3 ? C3 : C4);
         if (!C2) C2=C1;
         if (!C3) C3=C1;
         if (!C4) C4=C1;

	     int B = ( ((C1>> 0) & 31) + ((C2 >>0) & 31) + ((C3 >>0) & 31) + ((C4 >>0) & 31) +2 ) >> 2;
         int G = ( ((C1>> 5) & 31) + ((C2 >>5) & 31) + ((C3 >>5) & 31) + ((C4 >>5) & 31) +2 ) >> 2;
         int R = ( ((C1>>10) & 31) + ((C2>>10) & 31) + ((C3>>10) & 31) + ((C4>>10) & 31) +2 ) >> 2;
         if (!HARD3D) *(dst + x + y * 64) = HiColor(R,G,B)*2;
		         else *(dst + x + y * 64) = HiColor(R,G,B);
        }
}



void GetObjectCaracteristics(TModel* mptr, int& ylo, int& yhi)
{
   ylo = 10241024;
   yhi =-10241024;
   for (int v=0; v<mptr->VCount; v++) {
    if (mptr->gVertex[v].y < ylo) ylo = (int)mptr->gVertex[v].y;
    if (mptr->gVertex[v].y > yhi) yhi = (int)mptr->gVertex[v].y;
   }      
   if (yhi<ylo) yhi=ylo+1;   
}



void GenerateAlphaFlags(TModel *mptr)
{
#ifdef _d3d  
	  
  int w;
  BOOL Opacity = FALSE;
  WORD* tptr = mptr->lpTexture;    

  for (w=0; w<mptr->FCount; w++)
	  if ((mptr->gFace[w].Flags & sfOpacity)>0) Opacity = TRUE;

  if (Opacity) {
   for (w=0; w<256*256; w++)
	   if ( *(tptr+w)>0 ) *(tptr+w)=(*(tptr+w)) + 0x8000; }
  else 
   for (w=0; w<256*256; w++)
	 *(tptr+w)=(*(tptr+w)) + 0x8000;

  tptr = mptr->lpTexture2;    
  if (tptr==NULL) return;

  if (Opacity) {
   for (w=0; w<128*128; w++)
	   if ( (*(tptr+w))>0 ) *(tptr+w)=(*(tptr+w)) + 0x8000; }
  else 
   for (w=0; w<128*128; w++)
	 *(tptr+w)=(*(tptr+w)) + 0x8000;

  tptr = mptr->lpTexture3;    
  if (tptr==NULL) return;

  if (Opacity) {
   for (w=0; w<64*64; w++)
	   if ( (*(tptr+w))>0 ) *(tptr+w)=(*(tptr+w)) + 0x8000; }
  else 
   for (w=0; w<64*64; w++)
	 *(tptr+w)=(*(tptr+w)) + 0x8000;

#endif  
}




void GenerateModelMipMaps(TModel *mptr)
{
  int th = (mptr->TextureHeight) / 2;        
  mptr->lpTexture2 = 
       (WORD*) _HeapAlloc(Heap, HEAP_ZERO_MEMORY , (1+th)*128*2);                
  CreateMipMapMT(mptr->lpTexture2, mptr->lpTexture, th);        

  th = (mptr->TextureHeight) / 4;        
  mptr->lpTexture3 = 
       (WORD*) _HeapAlloc(Heap, HEAP_ZERO_MEMORY , (1+th)*64*2);
  CreateMipMapMT2(mptr->lpTexture3, mptr->lpTexture2, th);        
}


void GenerateMapImage()
{
  int YShift = 23;
  int XShift = 11;
  int lsw = MapPic.W;
  for (int y=0; y<256; y++)
   for (int x=0; x<256; x++) {
	int t;
	WORD c;

	if (FMap[y<<2][x<<2] & fmWater) {
		t = WaterList[WMap[y<<2][x<<2]].tindex;
		c= Textures[t]->DataD[(y & 15)*16+(x&15)];
	} else {
	    t = TMap1[y<<2][x<<2];
		c= Textures[t]->DataC[(y & 31)*32+(x&31)];
	}		
	
    if (!HARD3D) c=c>>1; else c=conv_565(c);
	*((WORD*)MapPic.lpImage + (y+YShift)*lsw + x + XShift) = c;
   }
}



void ReleaseResources()
{
	// SOURCEPORT: stop audio before freeing sample buffers — the SDL audio callback
	// runs on a separate thread and may still hold pointers into Ambient[].sfx.lpData
	// or RandSound[].lpData. AudioStop() acquires the audio lock and zeroes all
	// channel/ambient lpData pointers before we free the underlying memory.
	AudioStop();

	HeapReleased=0;
    for (int t=0; t<1024; t++)
	 if (Textures[t]) {
      _HeapFree(Heap, 0, (void*)Textures[t]);
	  Textures[t] = NULL;
	 } else break;

	
    for (int m=0; m<255; m++) {
     TModel *mptr = MObjects[m].model;	 
     if (mptr) {
		_HeapFree(Heap,0,MObjects[m].bmpmodel.lpTexture);  
		MObjects[m].bmpmodel.lpTexture = NULL;

		if (MObjects[m].vtl.FramesCount>0) {
		 _HeapFree(Heap, 0, MObjects[m].vtl.aniData);
		 MObjects[m].vtl.aniData = NULL;
		 }
		
        _HeapFree(Heap,0,mptr->lpTexture);   mptr->lpTexture  = NULL;
        _HeapFree(Heap,0,mptr->lpTexture2);  mptr->lpTexture2 = NULL;
        _HeapFree(Heap,0,mptr->lpTexture3);  mptr->lpTexture3 = NULL;
        _HeapFree(Heap,0,MObjects[m].model);              
        MObjects[m].model = NULL;
		MObjects[m].vtl.FramesCount = 0;
     } else break;
    }
	
	for (int a=0; a<255; a++) {
	  if (!Ambient[a].sfx.lpData) break;
	  _HeapFree(Heap, 0, Ambient[a].sfx.lpData);	  
	  Ambient[a].sfx.lpData = NULL;
	}

	for (int r=0; r<255; r++) {
		if (!RandSound[r].lpData) break;	  
		_HeapFree(Heap, 0, RandSound[r].lpData);
		RandSound[r].lpData = NULL;	  
		RandSound[r].length = 0;
	}	
}


void LoadBMPModel(TObject &obj)
{
	obj.bmpmodel.lpTexture = (WORD*) _HeapAlloc(Heap, 0, 128 * 128 * 2);
	//WORD * lpT             = (WORD*) _HeapAlloc(Heap, 0, 256 * 256 * 2);
    //ReadFile(hfile, lpT, 256*256*2, &l, NULL);
    //DATASHIFT(obj.bmpmodel.lpTexture, 128*128*2);
	//BrightenTexture(lpT, 256*256);
	ReadFile(hfile, obj.bmpmodel.lpTexture, 128*128*2, &l, NULL);	
	BrightenTexture(obj.bmpmodel.lpTexture, 128*128);
	DATASHIFT(obj.bmpmodel.lpTexture, 128*128*2);
    //CreateMipMapMT(obj.bmpmodel.lpTexture, lpT, 128);    

	//_HeapFree(Heap, 0, lpT);

    if (HARD3D)
	for (int x=0; x<128; x++)
		for (int y=0; y<128; y++)          
			if ( *(obj.bmpmodel.lpTexture + x + y*128) )
				*(obj.bmpmodel.lpTexture + x + y*128) |= 0x8000;

	float mxx = obj.model->gVertex[0].x+0.5f;
	float mnx = obj.model->gVertex[0].x-0.5f;

	float mxy = obj.model->gVertex[0].x+0.5f;
	float mny = obj.model->gVertex[0].y-0.5f;

	for (int v=0; v<obj.model->VCount; v++) {
      float x = obj.model->gVertex[v].x;
	  float y = obj.model->gVertex[v].y;
	  if (x > mxx) mxx=x;
	  if (x < mnx) mnx=x;
	  if (y > mxy) mxy=y;
	  if (y < mny) mny=y;
	}

   obj.bmpmodel.gVertex[0].x = mnx;
   obj.bmpmodel.gVertex[0].y = mxy;
   obj.bmpmodel.gVertex[0].z = 0;

   obj.bmpmodel.gVertex[1].x = mxx;
   obj.bmpmodel.gVertex[1].y = mxy;
   obj.bmpmodel.gVertex[1].z = 0;

   obj.bmpmodel.gVertex[2].x = mxx;
   obj.bmpmodel.gVertex[2].y = mny;
   obj.bmpmodel.gVertex[2].z = 0;

   obj.bmpmodel.gVertex[3].x = mnx;
   obj.bmpmodel.gVertex[3].y = mny;
   obj.bmpmodel.gVertex[3].z = 0;
}


void LoadResources()
{

    int  FadeRGB[3][3];
	int TransRGB[3][3];

    int tc,mc;
    char MapName[128],RscName[128];
	HeapAllocated=0;
	TrophyMode = FALSE;  // SOURCEPORT: reset each load so trophy state never bleeds into regular maps
	if (strstr(ProjectName, "trophy")) {
		TrophyMode = TRUE;
		ctViewR  = 60;
		ctViewR1 = 60;  // SOURCEPORT: match ctViewR so all wall vertices are transformed (no teal gaps)
	}
    wsprintf(MapName,"%s%s", ProjectName, ".map");
    wsprintf(RscName,"%s%s", ProjectName, ".rsc");

    ReleaseResources();

#ifdef _opengl
    // SOURCEPORT: d3dMemMap caches CPU pointer → GLuint. After ReleaseResources the heap
    // is freed; new textures can land at the same virtual addresses. Without a full reset
    // d3dSetTexture will find stale cache entries for the new level's texture pointers and
    // return old GLuints — producing wrong textures (teal, wrong-model-on-ground, etc.)
    // that get worse with every level transition. ResetTextureMap() deletes all GL textures
    // and clears every cpuaddr/lastused slot so the next level starts with a clean cache.
    {
        extern void ResetTextureMap();
        ResetTextureMap();
    }
#endif




 




    hfile = CreateFile(RscName,
        GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hfile==INVALID_HANDLE_VALUE) {
        char sz[512];
        wsprintf( sz, "Error opening resource file\n%s.", RscName );
		DoHalt(sz);                
        return;   }
    
    ReadFile(hfile, &tc, 4, &l, NULL);
    ReadFile(hfile, &mc, 4, &l, NULL);

	
	ReadFile(hfile,  FadeRGB, 4*3*3, &l, NULL); 
	ReadFile(hfile, TransRGB, 4*3*3, &l, NULL); 

    SkyR  =  FadeRGB[OptDayNight][0];
	SkyG  =  FadeRGB[OptDayNight][1];
	SkyB  =  FadeRGB[OptDayNight][2];

	SkyTR = TransRGB[OptDayNight][0];
	SkyTG = TransRGB[OptDayNight][1];
	SkyTB = TransRGB[OptDayNight][2];

	if (OptDayNight==2) {
		SkyR = 0; SkyB = 0; SkyTR = 0; SkyTB = 0;
	}
	
	SkyTR = min(255,SkyTR * (OptBrightness + 128) / 256);
	SkyTG = min(255,SkyTG * (OptBrightness + 128) / 256);
	SkyTB = min(255,SkyTB * (OptBrightness + 128) / 256);

	SkyR = min(255,SkyR * (OptBrightness + 128) / 256);
	SkyG = min(255,SkyG * (OptBrightness + 128) / 256);
	SkyB = min(255,SkyB * (OptBrightness + 128) / 256);


    PrintLog("Loading textures:");
    for (int tt=0; tt<tc; tt++) {
        LoadTexture(Textures[tt]);
#ifdef _opengl
        // SOURCEPORT: look for <ProjectName>_tex_NN.{png,tga,bmp,jpg} override.
        // GL path always binds DataA (never DataB/C/D — see renderd3d DataA-always fix),
        // so one override per terrain texture is enough.
        char base[256];
        wsprintf(base, "%s_tex_%02d", ProjectName, tt);
        TextureOverrides::TryRegisterWithExts(Textures[tt]->DataA, base);
        Materials::TryRegisterWithExts(Textures[tt]->DataA, base);
        CustomMaterials::TryRegisterWithExts(Textures[tt]->DataA, base);
        HotWatchOverrideSet(Textures[tt]->DataA, base);
#endif
    }
	PrintLog(" Done.\n");



	PrintLog("Loading models:");
	PrintLoad("Loading models...");
    for (int mm=0; mm<mc; mm++) {
        ReadFile(hfile, &MObjects[mm].info, 64, &l, NULL);
        MObjects[mm].info.Radius*=2;
        MObjects[mm].info.YLo*=2;
        MObjects[mm].info.YHi*=2;
		MObjects[mm].info.linelenght = (MObjects[mm].info.linelenght / 128) * 128;
        LoadModel(MObjects[mm].model);
        LoadBMPModel(MObjects[mm]);
#ifdef _opengl
        // SOURCEPORT: per-object overrides — <ProjectName>_obj_NN.{png…} for the
        // 3D model texture, <ProjectName>_obj_NN_bmp.{png…} for the distance billboard.
        {
            char base[256];
            wsprintf(base, "%s_obj_%02d", ProjectName, mm);
            TextureOverrides::TryRegisterWithExts(MObjects[mm].model->lpTexture, base);
            Materials::TryRegisterWithExts(MObjects[mm].model->lpTexture, base);
            CustomMaterials::TryRegisterWithExts(MObjects[mm].model->lpTexture, base);
            HotWatchOverrideSet(MObjects[mm].model->lpTexture, base);
            wsprintf(base, "%s_obj_%02d_bmp", ProjectName, mm);
            TextureOverrides::TryRegisterWithExts(MObjects[mm].bmpmodel.lpTexture, base);
            Materials::TryRegisterWithExts(MObjects[mm].bmpmodel.lpTexture, base);
            CustomMaterials::TryRegisterWithExts(MObjects[mm].bmpmodel.lpTexture, base);
            HotWatchOverrideSet(MObjects[mm].bmpmodel.lpTexture, base);
        }
#endif

		if (MObjects[mm].info.flags & ofNOLIGHT)
			FillMemory(MObjects[mm].model->VLight, 4*1024*4, 0);

		if (MObjects[mm].info.flags & ofANIMATED)
		  LoadAnimation(MObjects[mm].vtl);		


		MObjects[mm].info.BoundR = 0;
		for (int v=0; v<MObjects[mm].model->VCount; v++) {
			float r = (float)sqrt(MObjects[mm].model->gVertex[v].x * MObjects[mm].model->gVertex[v].x +
				                  MObjects[mm].model->gVertex[v].z * MObjects[mm].model->gVertex[v].z );
			if (r>MObjects[mm].info.BoundR) MObjects[mm].info.BoundR=r;
		}

		if (MObjects[mm].info.flags & ofBOUND)
		  CalcBoundBox(MObjects[mm].model, MObjects[mm].bound);

        GenerateModelMipMaps(MObjects[mm].model);
		GenerateAlphaFlags(MObjects[mm].model);		
	}
	PrintLog(" Done.\n");

	

	
	PrintLoad("Finishing with .res...");
	PrintLog("Finishing with .res:");
    LoadSky();
    LoadSkyMap();
#ifdef _opengl
    {
        // SOURCEPORT: <ProjectName>_sky.{png,tga,bmp,jpg} overrides the sky dome.
        char base[256];
        wsprintf(base, "%s_sky", ProjectName);
        TextureOverrides::TryRegisterWithExts(SkyPic, base);
        Materials::TryRegisterWithExts(SkyPic, base);
        HotWatchOverrideSet(SkyPic, base);
    }
#endif
	
	int FgCount;
	ReadFile(hfile, &FgCount, 4, &l, NULL); 
    ReadFile(hfile, &FogsList[1], FgCount * sizeof(TFogEntity), &l, NULL);


	for (int f=0; f<=FgCount; f++) {
      int fb = (FogsList[f].fogRGB >> 00) & 0xFF;
	  int fg = (FogsList[f].fogRGB >>  8) & 0xFF;
	  int fr = (FogsList[f].fogRGB >> 16) & 0xFF;
	  #ifdef _d3d
  	    FogsList[f].fogRGB = (fr) + (fg<<8) + (fb<<16);	  
	  #endif
	  if (OptDayNight==2) FogsList[f].fogRGB&=0x00FF00;
	}

	
	int RdCount, AmbCount, WtrCount;

	ReadFile(hfile, &RdCount, 4, &l, NULL); 
   	for (int r=0; r<RdCount; r++) {
	  ReadFile(hfile, &RandSound[r].length, 4, &l, NULL); 
	  RandSound[r].lpData = (short int*) _HeapAlloc(Heap,0,RandSound[r].length);	  
	  ReadFile(hfile, RandSound[r].lpData, RandSound[r].length, &l, NULL); 
	}

	ReadFile(hfile, &AmbCount, 4, &l, NULL); 
	for (int a=0; a<AmbCount; a++) {
	  ReadFile(hfile, &Ambient[a].sfx.length, 4, &l, NULL); 
	  Ambient[a].sfx.lpData = (short int*) _HeapAlloc(Heap,0,Ambient[a].sfx.length);	  
	  ReadFile(hfile, Ambient[a].sfx.lpData, Ambient[a].sfx.length, &l, NULL); 

	  ReadFile(hfile, Ambient[a].rdata, sizeof(Ambient[a].rdata), &l, NULL); 
	  ReadFile(hfile, &Ambient[a].RSFXCount, 4, &l, NULL); 
	  ReadFile(hfile, &Ambient[a].AVolume, 4, &l, NULL); 
	  
	  if (Ambient[a].RSFXCount)
		  Ambient[a].RndTime = (Ambient[a].rdata[0].RFreq / 2 + rRand(Ambient[a].rdata[0].RFreq)) * 1000;
	  
	  int F = Ambient[a].rdata[0].RFreq;
	  int E = Ambient[a].rdata[0].REnvir;
/////////////////
	  
	  //wsprintf(logt,"Env=%d  Flag=%d  Freq=%d\n", E, Ambient[a].rdata[0].Flags, F);
      //PrintLog(logt);

	  if (OptDayNight==2)
	  for (int r=0; r<Ambient[a].RSFXCount; r++) 
		  if (Ambient[a].rdata[r].Flags)
		  {
			  if (r!=15) memcpy(&Ambient[a].rdata[r], &Ambient[a].rdata[r+1], (15-r)*sizeof(TRD));
			  Ambient[a].RSFXCount--;
			  r--;
		  }

      Ambient[a].rdata[0].RFreq = F; 
	  Ambient[a].rdata[0].REnvir = E;

	  
	}

	ReadFile(hfile, &WtrCount, 4, &l, NULL); 	
	ReadFile(hfile, WaterList, 16*WtrCount, &l, NULL); 

	WaterList[255].wlevel = 0;
	for (int w=0; w<WtrCount; w++) {
#ifdef _3dfx
      WaterList[w].fogRGB = (Textures[WaterList[w].tindex]->mR) + 
		                    (Textures[WaterList[w].tindex]->mG<<8) + 
							(Textures[WaterList[w].tindex]->mB<<16);
#else
	  WaterList[w].fogRGB = (Textures[WaterList[w].tindex]->mB) + 
		                    (Textures[WaterList[w].tindex]->mG<<8) + 
							(Textures[WaterList[w].tindex]->mR<<16);
#endif
	}
    CloseHandle(hfile);
	PrintLog(" Done.\n");

    	
    

//================ Load MAPs file ==================//
	PrintLoad("Loading .map...");
	PrintLog("Loading .map:");
    hfile = CreateFile(MapName,
        GENERIC_READ, FILE_SHARE_READ,
		NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hfile==INVALID_HANDLE_VALUE)
      DoHalt("Error opening map file.");

    // SOURCEPORT: default runtime map size for retail .MAP (1024×1024). Future extended
    // map formats can sniff a header here and set gMapSize differently before the reads.
    gMapSize = 1024;
    gMapMask = gMapSize - 1;
    const int mapBytes = gMapSize * gMapSize;

    ReadFile(hfile, HMap,    mapBytes,   &l, NULL);
    ReadFile(hfile, TMap1,   mapBytes*2, &l, NULL);
    ReadFile(hfile, TMap2,   mapBytes*2, &l, NULL);
    ReadFile(hfile, OMap,    mapBytes,   &l, NULL);
    ReadFile(hfile, FMap,    mapBytes*2, &l, NULL);
     SetFilePointer(hfile, mapBytes*OptDayNight, NULL, FILE_CURRENT);
	ReadFile(hfile, LMap,    mapBytes,   &l, NULL);
	 SetFilePointer(hfile, mapBytes*(2-OptDayNight), NULL, FILE_CURRENT);
    ReadFile(hfile, WMap ,   mapBytes,   &l, NULL);
    ReadFile(hfile, HMapO,   mapBytes,   &l, NULL);
	ReadFile(hfile, FogsMap, 512*512, &l, NULL);
	ReadFile(hfile, AmbMap,  512*512, &l, NULL);

	if (FogsList[1].YBegin>1.f)
     for (int x=0; x<510; x++)
 	  for (int y=0; y<510; y++)
	   if (!FogsMap[y][x]) 
		if (HMap[y*2+0][x*2+0]<FogsList[1].YBegin || HMap[y*2+0][x*2+1]<FogsList[1].YBegin || HMap[y*2+0][x*2+2] < FogsList[1].YBegin ||
		    HMap[y*2+1][x*2+0]<FogsList[1].YBegin || HMap[y*2+1][x*2+1]<FogsList[1].YBegin || HMap[y*2+1][x*2+2] < FogsList[1].YBegin ||
			HMap[y*2+2][x*2+0]<FogsList[1].YBegin || HMap[y*2+2][x*2+1]<FogsList[1].YBegin || HMap[y*2+2][x*2+2] < FogsList[1].YBegin)
			  FogsMap[y][x] = 1;

    CloseHandle(hfile);	    
	PrintLog(" Done.\n");


	

//======= Post load rendering ==============//
	PrintLoad("Prepearing maps...");
    CreateTMap();
    RenderLightMap();
	
	LoadPictureTGA(MapPic, "HUNTDAT\\MENU\\mapframe.tga");        
	conv_pic(MapPic);
    
	GenerateMapImage();

	if (TrophyMode) LoadPictureTGA(TrophyPic, "HUNTDAT\\MENU\\trophy.tga");
	           else LoadPictureTGA(TrophyPic, "HUNTDAT\\MENU\\trophy_g.tga");
	conv_pic(TrophyPic);

//    ReInitGame();
}



void LoadCharacters()
{
	int c; // SOURCEPORT: moved from for-loop to function scope (MSVC6 scoping)
	BOOL pres[64];
	FillMemory(pres, sizeof(pres), 0);
	pres[0]=TRUE;
	for (c=0; c<ChCount; c++) {
		pres[Characters[c].CType] = TRUE;
	}

	for (c=0; c<TotalC; c++) if (pres[c]) {
        if (!ChInfo[c].mptr) {
		 wsprintf(logt, "HUNTDAT\\%s", DinoInfo[c].FName);
         LoadCharacterInfo(ChInfo[c], logt);
		 PrintLog("Loading: ");	PrintLog(logt);	PrintLog("\n");
		}
	}

	for (c=10; c<20; c++) 
		if (TargetDino & (1<<c)) 	
			if (!DinoInfo[AI_to_CIndex[c]].CallIcon.lpImage) {		
			  wsprintf(logt, "HUNTDAT\\MENU\\PICS\\call%d.tga", c-9);
	          LoadPictureTGA(DinoInfo[AI_to_CIndex[c]].CallIcon, logt); 
			  conv_pic(DinoInfo[AI_to_CIndex[c]].CallIcon);
			}


	for (c=0; c<TotalW; c++) 
		if (WeaponPres & (1<<c)) 	{			
			if (!Weapon.chinfo[c].mptr) {
			  wsprintf(logt, "HUNTDAT\\WEAPONS\\%s", WeapInfo[c].FName);
              LoadCharacterInfo(Weapon.chinfo[c], logt);
			  PrintLog("Loading: ");  PrintLog(logt);  PrintLog("\n");			  
			}
		    
				
			if (!Weapon.BulletPic[c].lpImage) {
			 wsprintf(logt, "HUNTDAT\\WEAPONS\\%s", WeapInfo[c].BFName);
			 LoadPictureTGA(Weapon.BulletPic[c], logt); 
			 conv_pic(Weapon.BulletPic[c]);
			 PrintLog("Loading: ");  PrintLog(logt);  PrintLog("\n");
			}
			
	}

    for (c=10; c<20; c++) 
		if (TargetDino & (1<<c))
			if (!fxCall[c-10][0].lpData) {
				wsprintf(logt,"HUNTDAT\\SOUNDFX\\CALLS\\call%d_a.wav", (c-9));
				LoadWav(logt, fxCall[c-10][0]);
				wsprintf(logt,"HUNTDAT\\SOUNDFX\\CALLS\\call%d_b.wav", (c-9));
				LoadWav(logt, fxCall[c-10][1]);
				wsprintf(logt,"HUNTDAT\\SOUNDFX\\CALLS\\call%d_c.wav", (c-9));
				LoadWav(logt, fxCall[c-10][2]);
			}
}

void ReInitGame()
{
	PrintLog("ReInitGame();\n");
	PlaceHunter();
	if (TrophyMode) {
		PlaceTrophy();
		RadarMode = FALSE;
	} else PlaceCharacters();

    LoadCharacters();

	LockLanding = FALSE;
	Wind.alpha = rRand(1024) * 2.f * pi / 1024.f;
	Wind.speed = 10;
	MyHealth = MAX_HEALTH;
	TargetWeapon = -1;

	for (int w=0; w<TotalW; w++)
		if ( WeaponPres & (1<<w) ) {
		   ShotsLeft[w] = WeapInfo[w].Shots;	
		   if (DoubleAmmo) AmmoMag[w] = 1;
		   if (TargetWeapon==-1) TargetWeapon=w;
		}

	CurrentWeapon = TargetWeapon;
	// SOURCEPORT: guard against no weapons selected (WeaponPres=0 → CurrentWeapon=-1)
	if (CurrentWeapon < 0) CurrentWeapon = 0;

	Weapon.state = 0;
	Weapon.FTime = 0;
	PlayerAlpha = 0;
    PlayerBeta  = 0;

    WCCount = 0;
	ElCount = 0;
	BloodTrail.Count = 0;
	BINMODE = FALSE;
	OPTICMODE = FALSE;
	EXITMODE = FALSE;
	PAUSE = FALSE;

	Ship.pos.x = PlayerX;
	Ship.pos.z = PlayerZ;
	Ship.pos.y = GetLandUpH(Ship.pos.x, Ship.pos.z) + 2048;
	Ship.State = -1;
	Ship.tgpos.x = Ship.pos.x;
	Ship.tgpos.z = Ship.pos.z + 60*256;
	Ship.cindex  = -1;
	Ship.tgpos.y = GetLandUpH(Ship.tgpos.x, Ship.tgpos.z) + 2048;
	ShipTask.tcount = 0;

	if (!TrophyMode) {
	  TrophyRoom.Last.smade = 0;
	  TrophyRoom.Last.success = 0;
	  TrophyRoom.Last.path  = 0;
	  TrophyRoom.Last.time  = 0;
	}

	DemoPoint.DemoTime = 0;
	RestartMode = FALSE;
	TrophyTime=0;
	answtime = 0;
	ExitTime = 0;
}



void ReleaseCharacterInfo(TCharacterInfo &chinfo)
{
	if (!chinfo.mptr) return;
	
	_HeapFree(Heap, 0, chinfo.mptr);
	chinfo.mptr = NULL;

	int c; // SOURCEPORT: moved from for-loop to function scope (MSVC6 scoping)
	for (c = 0; c<64; c++) {
     if (!chinfo.Animation[c].aniData) break;
	 _HeapFree(Heap, 0, chinfo.Animation[c].aniData);
     chinfo.Animation[c].aniData = NULL;
	}

	for (c = 0; c<64; c++) {
     if (!chinfo.SoundFX[c].lpData) break;
	 _HeapFree(Heap, 0, chinfo.SoundFX[c].lpData);
     chinfo.SoundFX[c].lpData = NULL;
	}

	chinfo.AniCount = 0;
	chinfo.SfxCount = 0;
}




void LoadCharacterInfo(TCharacterInfo &chinfo, char* FName)
{
   ReleaseCharacterInfo(chinfo);

   HANDLE hfile = CreateFile(FName,
      GENERIC_READ, FILE_SHARE_READ,
	  NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

   if (hfile==INVALID_HANDLE_VALUE) {
      char sz[512];
      wsprintf( sz, "Error opening character file:\n%s.", FName );
      DoHalt(sz);
    }

    ReadFile(hfile, chinfo.ModelName, 32, &l, NULL);
    ReadFile(hfile, &chinfo.AniCount,  4, &l, NULL);
    ReadFile(hfile, &chinfo.SfxCount,  4, &l, NULL);

//============= read model =================//

    chinfo.mptr = (TModel*) _HeapAlloc(Heap, 0, sizeof(TModel));

    ReadFile( hfile, &chinfo.mptr->VCount,      4,         &l, NULL );
    ReadFile( hfile, &chinfo.mptr->FCount,      4,         &l, NULL );
    ReadFile( hfile, &chinfo.mptr->TextureSize, 4,         &l, NULL );
    ReadFile( hfile, chinfo.mptr->gFace,        chinfo.mptr->FCount<<6, &l, NULL );
    ReadFile( hfile, chinfo.mptr->gVertex,      chinfo.mptr->VCount<<4, &l, NULL );

    int ts = chinfo.mptr->TextureSize;
	if (HARD3D) chinfo.mptr->TextureHeight = 256;
          else  chinfo.mptr->TextureHeight = chinfo.mptr->TextureSize>>9;    
    chinfo.mptr->TextureSize = chinfo.mptr->TextureHeight*512;

    chinfo.mptr->lpTexture = (WORD*) _HeapAlloc(Heap, 0, chinfo.mptr->TextureSize);    

    ReadFile(hfile, chinfo.mptr->lpTexture, ts, &l, NULL);
	BrightenTexture(chinfo.mptr->lpTexture, ts/2);
    
    DATASHIFT(chinfo.mptr->lpTexture, chinfo.mptr->TextureSize);
    GenerateModelMipMaps(chinfo.mptr);
	GenerateAlphaFlags(chinfo.mptr);

#ifdef _opengl
	// SOURCEPORT: optional 32-bit PNG/TGA override sibling next to the .CAR file.
	TextureOverrides::TryRegisterSibling(chinfo.mptr->lpTexture, FName);
	Materials::TryRegisterSibling(chinfo.mptr->lpTexture, FName);
	CustomMaterials::TryRegisterSibling(chinfo.mptr->lpTexture, FName);
	HotWatchOverrideSibling(chinfo.mptr->lpTexture, FName);
#endif
	//CalcLights(chinfo.mptr);
	
	//ApplyAlphaFlags(chinfo.mptr->lpTexture, 256*256);
	//ApplyAlphaFlags(chinfo.mptr->lpTexture2, 128*128);
//============= read animations =============//
    for (int a=0; a<chinfo.AniCount; a++) {
      ReadFile(hfile, chinfo.Animation[a].aniName, 32, &l, NULL);
      ReadFile(hfile, &chinfo.Animation[a].aniKPS, 4, &l, NULL);
      ReadFile(hfile, &chinfo.Animation[a].FramesCount, 4, &l, NULL);
      chinfo.Animation[a].AniTime = (chinfo.Animation[a].FramesCount * 1000) / chinfo.Animation[a].aniKPS;
      chinfo.Animation[a].aniData = (short int*) 
          _HeapAlloc(Heap, 0, (chinfo.mptr->VCount*chinfo.Animation[a].FramesCount*6) );

      ReadFile(hfile, chinfo.Animation[a].aniData, (chinfo.mptr->VCount*chinfo.Animation[a].FramesCount*6), &l, NULL);
    }

//============= read sound fx ==============//
	BYTE tmp[32];
    for (int s=0; s<chinfo.SfxCount; s++) {
      ReadFile(hfile, tmp, 32, &l, NULL);
      ReadFile(hfile, &chinfo.SoundFX[s].length, 4, &l, NULL);
       chinfo.SoundFX[s].lpData = (short int*) _HeapAlloc(Heap, 0, chinfo.SoundFX[s].length);
      ReadFile(hfile, chinfo.SoundFX[s].lpData, chinfo.SoundFX[s].length, &l, NULL);
    }

   for (int v=0; v<chinfo.mptr->VCount; v++) {
     chinfo.mptr->gVertex[v].x*=2.f;
     chinfo.mptr->gVertex[v].y*=2.f;
     chinfo.mptr->gVertex[v].z*=-2.f;
    }

   CorrectModel(chinfo.mptr);
   
   
   ReadFile(hfile, chinfo.Anifx, 64*4, &l, NULL);
   if (l!=256)
	   for (l=0; l<64; l++) chinfo.Anifx[l] = -1;
   CloseHandle(hfile);
}






















//================ light map ========================//



void FillVector(int x, int y, Vector3d& v)
{
   v.x = (float)x*256;
   v.z = (float)y*256;
   v.y = (float)((int)HMap[y][x])*ctHScale;
}

BOOL TraceVector(Vector3d v, Vector3d lv)
{
  v.y+=4;  
  NormVector(lv,64);
  for (int l=0; l<32; l++) {
    v.x-=lv.x; v.y-=lv.y/6; v.z-=lv.z;
	if (v.y>255 * ctHScale) return TRUE;
	if (GetLandH(v.x, v.z) > v.y) return FALSE;
  } 
  return TRUE;
}


void AddShadow(int x, int y, int d)
{
  if (x<0 || y<0 || x>gMapSize-1 || y>gMapSize-1) return;
  int l = LMap[y][x]; 
  l-=d;
  if (l<32) l=32;
  LMap[y][x]=l;  
}

void RenderShadowCircle(int x, int y, int R, int D)
{
  int cx = x / 256;
  int cy = y / 256;
  int cr = 1 + R / 256;
  for (int yy=-cr; yy<=cr; yy++)
   for (int xx=-cr; xx<=cr; xx++) {
     int tx = (cx+xx)*256;
     int ty = (cy+yy)*256;
     int r = (int)sqrt( (tx-x)*(tx-x) + (ty-y)*(ty-y) );
     if (r>R) continue;
	 AddShadow(cx+xx, cy+yy, D * (R-r) / R);     
   }
}

void RenderLightMap()
{
   

  Vector3d lv;
  int x,y;

  lv.x = - 412;
  lv.z = - 412;
  lv.y = - 1024;
  NormVector(lv, 1.0f);
    
  for (y=1; y<gMapSize-1; y++) 
    for (x=1; x<gMapSize-1; x++) {
     int ob = OMap[y][x];
     if (ob == 255) continue;

     int l = MObjects[ob].info.linelenght / 128;
	 int s = 1;
	 if (OptDayNight==2) s=-1;
     if (OptDayNight!=1) l = MObjects[ob].info.linelenght / 70;
     if (l>0) RenderShadowCircle(x*256+128,y*256+128, 256, MObjects[ob].info.lintensity * 2);
     for (int i=1; i<l; i++) 
	   AddShadow(x+i*s, y+i*s, MObjects[ob].info.lintensity);                   

     l = MObjects[ob].info.linelenght * 2;
     RenderShadowCircle(x*256+128+l*s,y*256+128+l*s,
            MObjects[ob].info.circlerad*2,
            MObjects[ob].info.cintensity*4);  
    }

}





void SaveScreenShot()
 { 
 
    HANDLE hf;                  /* file handle */ 
    BITMAPFILEHEADER hdr;       /* bitmap file-header */ 
    BITMAPINFOHEADER bmi;       /* bitmap info-header */     
    DWORD dwTmp; 

	if (WinW>1024) return;
 
    
  //MessageBeep(0xFFFFFFFF);
    CopyHARDToDIB();

    bmi.biSize = sizeof(BITMAPINFOHEADER); 
    bmi.biWidth = WinW;
    bmi.biHeight = WinH;
    bmi.biPlanes = 1; 
    bmi.biBitCount = 24; 
    bmi.biCompression = BI_RGB;   
    
    bmi.biSizeImage = WinW*WinH*3;
    bmi.biClrImportant = 0;    
    bmi.biClrUsed = 0;



    hdr.bfType = 0x4d42;      
    hdr.bfSize = (DWORD) (sizeof(BITMAPFILEHEADER) + 
                 bmi.biSize + bmi.biSizeImage);  
    hdr.bfReserved1 = 0; 
    hdr.bfReserved2 = 0;      
    hdr.bfOffBits = (DWORD) sizeof(BITMAPFILEHEADER) + 
                    bmi.biSize; 


    char t[12];
    wsprintf(t,"HUNT%004d.BMP",++_shotcounter);
    hf = CreateFile(t,
                   GENERIC_READ | GENERIC_WRITE, 
                   (DWORD) 0, 
                   (LPSECURITY_ATTRIBUTES) NULL, 
                   CREATE_ALWAYS, 
                   FILE_ATTRIBUTE_NORMAL, 
                   (HANDLE) NULL); 
      
    
      
    WriteFile(hf, (LPVOID) &hdr, sizeof(BITMAPFILEHEADER), (LPDWORD) &dwTmp, (LPOVERLAPPED) NULL);
             
    WriteFile(hf, &bmi, sizeof(BITMAPINFOHEADER), (LPDWORD) &dwTmp, (LPOVERLAPPED) NULL);    
 
    byte fRGB[1024][3];

    for (int y=0; y<WinH; y++) {
     for (int x=0; x<WinW; x++) {
      WORD C = *((WORD*)lpVideoBuf + (WinEY-y)*1024+x);
      fRGB[x][0] = (C       & 31)<<3;
      if (HARD3D) {
       fRGB[x][1] = ((C>> 5) & 63)<<2;
       fRGB[x][2] = ((C>>11) & 31)<<3; 
      } else {
       fRGB[x][1] = ((C>> 5) & 31)<<3;
       fRGB[x][2] = ((C>>10) & 31)<<3; 
      }
     }
     WriteFile( hf, fRGB, 3*WinW, &dwTmp, NULL );     
    }    
 
    CloseHandle(hf);    
  //MessageBeep(0xFFFFFFFF);
} 












//===============================================================================================
void ReadWeapons(FILE *stream)
{
	TotalW = 0;	
	char line[256], *value;
    while (fgets( line, 255, stream)) 
	{		
		if (strstr(line, "}")) break;
		if (strstr(line, "{")) 
			while (fgets( line, 255, stream)) {								
				if (strstr(line, "}")) { TotalW++; break; }
				value = strstr(line, "=");
				if (!value) DoHalt("Script loading error");
				value++;
				
				if (strstr(line, "power"))  WeapInfo[TotalW].Power = (float)atof(value);
				if (strstr(line, "prec"))   WeapInfo[TotalW].Prec  = (float)atof(value);
				if (strstr(line, "loud"))   WeapInfo[TotalW].Loud  = (float)atof(value);
				if (strstr(line, "rate"))   WeapInfo[TotalW].Rate  = (float)atof(value);
				if (strstr(line, "shots"))  WeapInfo[TotalW].Shots =        atoi(value);
				if (strstr(line, "reload")) WeapInfo[TotalW].Reload=        atoi(value);
				if (strstr(line, "trace"))  WeapInfo[TotalW].TraceC=        atoi(value)-1;
				if (strstr(line, "optic"))  WeapInfo[TotalW].Optic =        atoi(value);
				if (strstr(line, "fall"))   WeapInfo[TotalW].Fall  =        atoi(value);
				//if (strstr(line, "price")) WeapInfo[TotalW].Price =        atoi(value);

				if (strstr(line, "name")) {					
					value = strstr(line, "'"); if (!value) DoHalt("Script loading error");
					value[strlen(value)-2] = 0;
					strcpy(WeapInfo[TotalW].Name, &value[1]); }				

				if (strstr(line, "file")) {					
					value = strstr(line, "'"); if (!value) DoHalt("Script loading error");
					value[strlen(value)-2] = 0;
					strcpy(WeapInfo[TotalW].FName, &value[1]);}

				if (strstr(line, "pic")) {					
					value = strstr(line, "'"); if (!value) DoHalt("Script loading error");
					value[strlen(value)-2] = 0;
					strcpy(WeapInfo[TotalW].BFName, &value[1]);}
			}
		
	}

}


void ReadCharacters(FILE *stream)
{
	TotalC = 0;
	char line[256], *value;
    while (fgets( line, 255, stream)) 
	{
		if (strstr(line, "}")) break;
		if (strstr(line, "{")) 
			while (fgets( line, 255, stream)) {				
				
				if (strstr(line, "}")) { 
                    AI_to_CIndex[DinoInfo[TotalC].AI] = TotalC;
					TotalC++; 
					break; 
				}

				value = strstr(line, "=");
				if (!value) 
					DoHalt("Script loading error");
				value++;
				
				if (strstr(line, "mass"     )) DinoInfo[TotalC].Mass      = (float)atof(value);
				if (strstr(line, "length"   )) DinoInfo[TotalC].Length    = (float)atof(value);
				if (strstr(line, "radius"   )) DinoInfo[TotalC].Radius    = (float)atof(value);
				if (strstr(line, "health"   )) DinoInfo[TotalC].Health0   = atoi(value);
				if (strstr(line, "basescore")) DinoInfo[TotalC].BaseScore = atoi(value);
				if (strstr(line, "ai"       )) DinoInfo[TotalC].AI        = atoi(value);
				if (strstr(line, "smell"    )) DinoInfo[TotalC].SmellK    = (float)atof(value);
				if (strstr(line, "hear"     )) DinoInfo[TotalC].HearK     = (float)atof(value);
				if (strstr(line, "look"     )) DinoInfo[TotalC].LookK     = (float)atof(value);
				if (strstr(line, "shipdelta")) DinoInfo[TotalC].ShDelta   = (float)atof(value);
				if (strstr(line, "scale0"   )) DinoInfo[TotalC].Scale0    = atoi(value);
				if (strstr(line, "scaleA"   )) DinoInfo[TotalC].ScaleA    = atoi(value);
				if (strstr(line, "danger"   )) DinoInfo[TotalC].DangerCall= TRUE;

				if (strstr(line, "name")) {					
					value = strstr(line, "'"); if (!value) DoHalt("Script loading error");
					value[strlen(value)-2] = 0;
					strcpy(DinoInfo[TotalC].Name, &value[1]); }

				if (strstr(line, "file")) {					
					value = strstr(line, "'"); if (!value) DoHalt("Script loading error");
					value[strlen(value)-2] = 0;
					strcpy(DinoInfo[TotalC].FName, &value[1]);}

				if (strstr(line, "pic")) {					
					value = strstr(line, "'"); if (!value) DoHalt("Script loading error");
					value[strlen(value)-2] = 0;
					strcpy(DinoInfo[TotalC].PName, &value[1]);}
			}
		
	}
}



// SOURCEPORT: forward decl of the JSON overlay — defined in DataDefs.cpp.
// Applied after retail _res.txt so mods can override specific fields without
// repackaging the whole script.
namespace DataDefs { void ApplyJsonOverlays(); void RegisterHotReload(); }

void LoadResourcesScript()
{
    FILE *stream;
	char line[256];

	stream = fopen("HUNTDAT\\_res.txt", "r");
    if (!stream) DoHalt("Can't open resources file _res.txt");

	while (fgets( line, 255, stream)) {
       if (line[0] == '.') break;
	   if (strstr(line, "weapons") ) ReadWeapons(stream);
	   if (strstr(line, "characters") ) ReadCharacters(stream);
	}

	fclose (stream);

	// SOURCEPORT: layer JSON overlays (HUNTDAT\dinos.json / weapons.json) on top
	// of the retail parse. Missing files are a no-op — vanilla installs unaffected.
	DataDefs::ApplyJsonOverlays();
	DataDefs::RegisterHotReload();

	// SOURCEPORT: register once for hot reload. The watch is idempotent — if the
	// same path is added twice, both entries fire on change, which is harmless.
	// A static guard keeps us to a single registration across calls.
	static bool s_resWatchRegistered = false;
	if (!s_resWatchRegistered) {
		s_resWatchRegistered = true;
		HotReload::Watch("HUNTDAT\\_res.txt", []() {
			FILE* s = fopen("HUNTDAT\\_res.txt", "r");
			if (!s) return;
			char ln[256];
			while (fgets(ln, 255, s)) {
				if (ln[0] == '.') break;
				if (strstr(ln, "weapons"))    ReadWeapons(s);
				if (strstr(ln, "characters")) ReadCharacters(s);
			}
			fclose(s);
			// SOURCEPORT: re-layer JSON overlay so edits to _res.txt don't wipe out
			// dinos.json / weapons.json overrides until the next JSON save.
			DataDefs::ApplyJsonOverlays();
			PrintLog("[HotReload] _res.txt reloaded\n");
		});
	}
}
//===============================================================================================





void CreateLog()
{
	
	hlog = CreateFile("render.log", 
		               GENERIC_WRITE, 
					   FILE_SHARE_READ, NULL, 
					   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

#ifdef _d3d
	PrintLog("CarnivoresII  D3D video driver.");
#endif

#ifdef _3dfx
	PrintLog("CarnivoresII 3DFX video driver.");
#endif

#ifdef _soft
	PrintLog("CarnivoresII Soft video driver.");
#endif
	PrintLog(" Build v2.04. Sep.24 1999.\n");
}


void PrintLog(LPSTR l)
{
	DWORD w;
	
	if (l[strlen(l)-1]==0x0A) {
		BYTE b = 0x0D;
		WriteFile(hlog, l, strlen(l)-1, &w, NULL);
		WriteFile(hlog, &b, 1, &w, NULL);
		b = 0x0A;
		WriteFile(hlog, &b, 1, &w, NULL);
	} else
		WriteFile(hlog, l, strlen(l), &w, NULL);

}

void CloseLog()
{
	CloseHandle(hlog);
}