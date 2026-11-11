/* reColossus (from reKing): a text screen for what happens before the game: unpacking the disc image, and what is wrong with it.
 * From reUndr-ps5's ps5/src/screen_ps5.c (MIT, bystritskiy): CPU scanout through public VideoOut exports, in the
 * buffer layout PS5_Mesa's VideoOut WSI registers. The game's renderer takes VideoOut over afterwards, so
 * rc_screen_close gives it back first. Its font is 5x7: letters, digits and . , : - _ / ( ) ' % only. */
#include "ps5/screen_ps5.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

extern size_t sceKernelGetDirectMemorySize(void);
extern int sceKernelAllocateDirectMemory(int64_t,int64_t,size_t,size_t,int,int64_t *);
extern int sceKernelMapDirectMemory(void **,size_t,int,int,int64_t,size_t);
extern int sceKernelReleaseDirectMemory(int64_t,size_t);
extern int sceVideoOutOpen(int,int,int,const void *);
extern int sceVideoOutClose(int);
extern int sceVideoOutWaitVblank(int);
extern int sceVideoOutSubmitFlip(int,int,uint32_t,int64_t);
extern int sceVideoOutSetFlipRate(int,int);
extern int sceSystemServiceHideSplashScreen(void);
typedef struct { void *address,*metadata,*reserved[2]; } VideoBuffer;
typedef struct { uint8_t reserved[80]; } VideoAttribute;
extern void sceVideoOutSetBufferAttribute2(VideoAttribute *,uint64_t,uint32_t,uint32_t,uint32_t,uint64_t,uint32_t,uint64_t);
extern int sceVideoOutRegisterBuffers2(int,int,int,VideoBuffer *,int,VideoAttribute *,int,void *);

#define WIDTH 1920
#define HEIGHT 1080
/* The buffers PS5_Mesa's VideoOut WSI registers in this app: five of one
 * direct-memory allocation, B8G8R8A8 SDR, 64 KiB tiles of 128x128 pixels. Fewer
 * buffers or another size were refused on the console. */
#define BUFFER_BYTES 0xa00000
#define BUFFERS 5
#define BYTES (BUFFER_BYTES*BUFFERS)
#define PIXEL_FORMAT_B8G8R8A8_SDR UINT64_C(0x8000000000000000)
#define DIRECT_TYPE_CPU 12
#define MARGIN 120
#define TOP 0xff1c2026
#define BOTTOM 0xff07080a
#define ACCENT 0xffc9b48a
#define BAR_Y 560
#define BAR_HEIGHT 36

static int video=-1;
static int64_t physical;
static uint8_t *pixels;
static uint32_t *rows;

static const uint8_t letters[26][7]={
{14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{31,4,4,4,4,4,31},
{7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
{17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
{30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
{17,17,10,4,4,4,4},{31,1,2,4,8,16,31}};
static const uint8_t small[26][7]={
{0,0,14,1,15,17,15},{16,16,22,25,17,17,30},{0,0,14,16,16,17,14},
{1,1,13,19,17,17,15},{0,0,14,17,31,16,14},{6,9,8,28,8,8,8},
{0,15,17,17,15,1,14},{16,16,22,25,17,17,17},{4,0,12,4,4,4,14},
{2,0,6,2,2,18,12},{16,16,18,20,24,20,18},{12,4,4,4,4,4,14},
{0,0,26,21,21,17,17},{0,0,22,25,17,17,17},{0,0,14,17,17,17,14},
{0,0,30,17,30,16,16},{0,0,13,19,15,1,1},{0,0,22,25,16,16,16},
{0,0,14,16,14,1,30},{8,8,28,8,8,9,6},{0,0,17,17,17,19,13},
{0,0,17,17,17,10,4},{0,0,17,17,21,21,10},{0,0,17,10,4,10,17},
{0,0,17,17,15,1,14},{0,0,31,2,4,8,31}};
static const uint8_t numbers[10][7]={
{14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
{30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,16,30,1,1,30},
{14,16,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,1,14}};

static int glyph_on(unsigned char c,int row,int col)
{
 const uint8_t *g=c>='A'&&c<='Z'?letters[c-'A']:c>='a'&&c<='z'?small[c-'a']:c>='0'&&c<='9'?numbers[c-'0']:NULL;
 if(g)return (g[row]&(16>>col))!=0;
 switch(c) {
 case '.':return row==6&&col==2;
 case ',':return (row==5&&col==2)||(row==6&&col==1);
 case ':':return (row==2||row==5)&&col==2;
 case '-':return row==3;
 case '_':return row==6;
 case '/':return col==4-row*4/6;
 case '(':return (col==2&&(row==0||row==6))||(col==1&&row>0&&row<6);
 case ')':return (col==2&&(row==0||row==6))||(col==3&&row>0&&row<6);
 case '\'':return col==2&&row<2;
 case '%':return (row<2&&col<2)||(row>4&&col>2)||col==4-row*4/6;
 default:return 0;
 }
}

static void text(int x,int y,const char *s,uint32_t color,int scale)
{
 for(;*s;s++,x+=6*scale)
  for(int row=0;row<7;row++)for(int col=0;col<5;col++) {
   if(!glyph_on((unsigned char)*s,row,col))continue;
   for(int dy=0;dy<scale;dy++)for(int dx=0;dx<scale;dx++) {
    int px=x+col*scale+dx,py=y+row*scale+dy;
    if(px>=0&&py>=0&&px<WIDTH&&py<HEIGHT)rows[py*WIDTH+px]=color;
   }
  }
}

static void box(int x,int y,int w,int h,uint32_t color)
{
 for(int j=y;j<y+h;j++)for(int i=x;i<x+w;i++)rows[j*WIDTH+i]=color;
}

static uint32_t mix(uint32_t a,uint32_t b,unsigned at,unsigned of)
{
 uint32_t out=0xff000000;
 for(int shift=0;shift<24;shift+=8) {
  int x=a>>shift&255,y=b>>shift&255;
  out|=(uint32_t)(x+(y-x)*(int)at/(int)of)<<shift;
 }
 return out;
}

static size_t tiled_offset(unsigned x,unsigned y)
{
 uint32_t inside=((y<<4)&0x70u)^((y<<5)&0xf00u)^((y<<9)&0x1000u)^((y<<8)&0x4000u)^
  ((x<<2)&0xcu)^((x<<5)&0x380u)^((x<<4)&0x400u)^((x<<6)&0x800u)^((x<<9)&0xa000u);
 return ((size_t)((y>>7)*((WIDTH+127)/128)+(x>>7))<<16)+inside;
}

static int failed(const char *step,int rc)
{
 fprintf(stderr,"[SCREEN] %s %#x\n",step,(unsigned)rc);
 return rc;
}

static int open_video(void)
{
 sceSystemServiceHideSplashScreen();
 if(!rows&&!(rows=malloc((size_t)WIDTH*HEIGHT*4)))return failed("no memory",-1);
 video=sceVideoOutOpen(255,0,0,NULL);
 if(video<0)return failed("sceVideoOutOpen",video);
 sceVideoOutSetFlipRate(video,0);
 int rc=sceKernelAllocateDirectMemory(0,(int64_t)sceKernelGetDirectMemorySize(),BYTES,0x200000,DIRECT_TYPE_CPU,&physical);
 if(rc<0) { sceVideoOutClose(video);video=-1;return failed("sceKernelAllocateDirectMemory",rc); }
 rc=sceKernelMapDirectMemory((void **)&pixels,BYTES,0x33,0,physical,0x200000);
 if(rc<0) { sceKernelReleaseDirectMemory(physical,BYTES);sceVideoOutClose(video);video=-1;return failed("sceKernelMapDirectMemory",rc); }
 memset(pixels,0,BYTES);
 VideoBuffer buffers[BUFFERS]={{0}};
 VideoAttribute attribute={0};
 for(unsigned i=0;i<BUFFERS;i++)buffers[i].address=pixels+(size_t)i*BUFFER_BYTES;
 sceVideoOutSetBufferAttribute2(&attribute,PIXEL_FORMAT_B8G8R8A8_SDR,0,WIDTH,HEIGHT,0,0,0);
 rc=sceVideoOutRegisterBuffers2(video,0,0,buffers,BUFFERS,&attribute,0,NULL);
 if(rc<0) { rc_screen_close();return failed("sceVideoOutRegisterBuffers2",rc); }
 return 0;
}

int rc_screen_show(const RcScreenLine *lines,unsigned count,int percent)
{
 if(video<0) { int rc=open_video();if(rc<0)return rc; }
 for(unsigned y=0;y<HEIGHT;y++) { uint32_t c=mix(TOP,BOTTOM,y,HEIGHT);for(unsigned x=0;x<WIDTH;x++)rows[y*WIDTH+x]=c; }
 text(MARGIN,90,"reColossus",0xffffffff,12);
 box(MARGIN,200,1680,4,ACCENT);
 for(unsigned i=0;i<count;i++)text(MARGIN,(int)lines[i].y,lines[i].text,lines[i].color?lines[i].color:0xffffffff,(int)lines[i].scale);
 if(percent>=0) {
  char label[8];
  box(MARGIN,BAR_Y,1680,BAR_HEIGHT,0xff2e2a22);
  box(MARGIN,BAR_Y,1680*percent/100,BAR_HEIGHT,ACCENT);
  snprintf(label,sizeof label,"%d%%",percent);
  text(MARGIN,BAR_Y+60,label,0xffffffff,4);
 }
 for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++)*(uint32_t *)(pixels+tiled_offset(x,y))=rows[y*WIDTH+x];
 for(size_t i=0;i<BUFFER_BYTES;i+=64)__asm__ volatile("clflush (%0)"::"r"(pixels+i):"memory");
 __asm__ volatile("mfence":::"memory");
 int rc=sceVideoOutSubmitFlip(video,0,1,1);
 if(rc<0)return failed("sceVideoOutSubmitFlip",rc);
 sceVideoOutWaitVblank(video);
 return 0;
}

void rc_screen_close(void)
{
 if(video>=0) { sceVideoOutClose(video);video=-1; }
 if(pixels) { munmap(pixels,BYTES);sceKernelReleaseDirectMemory(physical,BYTES);pixels=NULL; }
 free(rows);rows=NULL;
}
