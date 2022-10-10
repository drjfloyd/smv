#include "options.h"
#ifdef pp_SMOKESTREAM
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <ctype.h>
#include <stdlib.h>
#include <fcntl.h>
#ifdef pp_OSXLINUX
#include <sys/mman.h>
#endif
#include <math.h>
#include "MALLOCC.h"
#include "smokestream.h"

#ifdef WIN32
#include <io.h>
#include <windows.h>
#include <sys/types.h>

/* mmap() replacement for Windows
 *
 * Author: Mike Frysinger <vapier@gentoo.org>
 * Placed into the public domain
 */

/* References:
 * CreateFileMapping: http://msdn.microsoft.com/en-us/library/aa366537(VS.85).aspx
 * CloseHandle:       http://msdn.microsoft.com/en-us/library/ms724211(VS.85).aspx
 * MapViewOfFile:     http://msdn.microsoft.com/en-us/library/aa366761(VS.85).aspx
 * UnmapViewOfFile:   http://msdn.microsoft.com/en-us/library/aa366882(VS.85).aspx
 */


#define PROT_READ     0x1
#define PROT_WRITE    0x2
/* This flag is only available in WinXP+ */
#ifdef FILE_MAP_EXECUTE
#define PROT_EXEC     0x4
#else
#define PROT_EXEC        0x0
#define FILE_MAP_EXECUTE 0
#endif

#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS
#define MAP_FAILED    ((void *) -1)

#ifdef __USE_FILE_OFFSET64
# define DWORD_HI(x) (x >> 32)
# define DWORD_LO(x) ((x) & 0xffffffff)
#else
# define DWORD_HI(x) (0)
# define DWORD_LO(x) (x)
#endif

/* ------------------  GetFrameIndex ------------------------ */

int GetFrameIndex(float time, float *times, int ntimes){
  int left, right, mid;
  left = 0;
  right = ntimes-1;
  if(time<=times[left])return left;
  if(time>=times[right])return right;
  while(right-left>1){
    mid = (left+right)/2;
    if(time<times[mid]){
      right = mid;
    }
    else{
      left = mid;
    }
  }
  if(time-times[left]<times[right]-time)return left;
  return right;
}

/* ------------------  mmap ------------------------ */

static void *mmap(void *start, size_t length, int prot, int flags, int fd, off_t offset)
{
  if(prot&~(PROT_READ|PROT_WRITE|PROT_EXEC))
    return MAP_FAILED;
  if(fd==-1) {
    if(!(flags&MAP_ANON)||offset)
      return MAP_FAILED;
  }
  else if(flags&MAP_ANON)
    return MAP_FAILED;

  DWORD flProtect;
  if(prot&PROT_WRITE) {
    if(prot&PROT_EXEC)
      flProtect = PAGE_EXECUTE_READWRITE;
    else
      flProtect = PAGE_READWRITE;
  }
  else if(prot&PROT_EXEC) {
    if(prot&PROT_READ)
      flProtect = PAGE_EXECUTE_READ;
    else if(prot&PROT_EXEC)
      flProtect = PAGE_EXECUTE;
  }
  else
    flProtect = PAGE_READONLY;

  off_t end = length+offset;
  HANDLE mmap_fd, h;
  if(fd==-1)
    mmap_fd = INVALID_HANDLE_VALUE;
  else
    mmap_fd = (HANDLE)_get_osfhandle(fd);
  h = CreateFileMapping(mmap_fd, NULL, flProtect, DWORD_HI(end), DWORD_LO(end), NULL);
  if(h==NULL)
    return MAP_FAILED;

  DWORD dwDesiredAccess;
  if(prot&PROT_WRITE)
    dwDesiredAccess = FILE_MAP_WRITE;
  else
    dwDesiredAccess = FILE_MAP_READ;
  if(prot&PROT_EXEC)
    dwDesiredAccess |= FILE_MAP_EXECUTE;
  if(flags&MAP_PRIVATE)
    dwDesiredAccess |= FILE_MAP_COPY;
  void *ret = MapViewOfFile(h, dwDesiredAccess, DWORD_HI(offset), DWORD_LO(offset), length);
  if(ret==NULL) {
    CloseHandle(h);
    ret = MAP_FAILED;
  }
  return ret;
}

/* ------------------  munmap ------------------------ */

static void munmap(void *addr, size_t length)
{
  UnmapViewOfFile(addr);
  /* ruh-ro, we leaked handle from CreateFileMapping() ... */
}

#undef DWORD_HI
#undef DWORD_LO
#endif

/* ------------------  MemMap ------------------------ */

char *MemMap(char *file, size_t *size){
   int fd;
   char *ptr;

   if(file==NULL)return NULL;
   fd = open(file, O_RDONLY);
   struct stat statbuf;
   fstat(fd, &statbuf);
   *size = statbuf.st_size;
   ptr = mmap(NULL, *size, PROT_READ|PROT_WRITE,MAP_SHARED, fd,0);
   if(ptr==MAP_FAILED)return NULL;
   return ptr;
}

/* ------------------  MemUnMap ------------------------ */

void MemUnMap(char *data, size_t size){
  munmap(data, size);
}

/* ------------------  StreamOpen ------------------------ */

streamdata *StreamOpen(streamdata *streamin, char *file, size_t offset, int *framesizes, int nframes, int constant_frame_size){
  streamdata *stream;
  STRUCTSTAT statbuffer;
  int i, statfile;
  int start, stop;

  if(file==NULL||strlen(file)==0||nframes<=0)return NULL;
  statfile = STAT(file, &statbuffer);
  if(statfile!=0)return NULL;

  if(streamin==NULL||nframes>streamin->nframes){
    if(streamin==NULL){
      NewMemory((void **)&stream, sizeof(streamdata));
      NewMemory((void **)&(stream->file), strlen(file)+1);
      strcpy(stream->file, file);
      NewMemory((void **)&(stream->frameptrs),     nframes*sizeof(char *));
      NewMemory((void **)&(stream->filebuffer),    nframes*sizeof(char *));
      NewMemory((void **)&(stream->framesizes),    nframes*sizeof(size_t));
      NewMemory((void **)&(stream->frame_offsets), nframes*sizeof(size_t));
      stream->frame_offsets[0] = offset;
      stream->frameptrs[0] = NULL;
      stream->framesizes[0] = framesizes[0];
      start = 1;
    }
    else{
      ResizeMemory((void **)&(stream->frameptrs),     nframes*sizeof(char *));
      ResizeMemory((void **)&(stream->filebuffer),    nframes*sizeof(char *));
      ResizeMemory((void **)&(stream->framesizes),    nframes*sizeof(size_t));
      ResizeMemory((void **)&(stream->frame_offsets), nframes*sizeof(size_t));
      start = streamin->nframes;
    }
    stop = nframes;
    stream->filesize = statbuffer.st_size;

    for(i = start; i<stop; i++){
      int sizei, sizeim1;

      if(constant_frame_size==1){
        sizei   = framesizes[0];
        sizeim1 = framesizes[0];
      }
      else{
        sizei   = framesizes[i];
        sizeim1 = framesizes[i-1];
      }
      stream->framesizes[i] = sizei;
      stream->frame_offsets[i] = stream->frame_offsets[i-1]+sizeim1;
      stream->frameptrs[i] = NULL;
    }
  }
  else{
    stream = streamin;
  }
  stream->nframes = nframes;
  return stream;
}

/* ------------------  StreamClose ------------------------ */

void StreamClose(streamdata **streamptr){
  char **frameptrs, *filebuffer;
  streamdata *stream;

  stream = *streamptr;
  if(stream==NULL)return;
  FREEMEMORY(stream->frameptrs);
  FREEMEMORY(stream->filebuffer);
  FREEMEMORY(stream->framesizes);
  FREEMEMORY(stream->frame_offsets);
  FREEMEMORY(stream->file);
  FREEMEMORY(stream);
  *streamptr = NULL;
}

/* ------------------  StreamRead ------------------------ */

FILE_SIZE StreamRead(streamdata *stream, int frame_index){
  FILE *filestream;
  FILE_SIZE file_size;

  if(stream==NULL||frame_index<0||frame_index>=stream->nframes)return 0;

  filestream = fopen(stream->file, "r");
  if(filestream==NULL)return 0;

  fseek(filestream, stream->frame_offsets[frame_index], SEEK_SET);
  file_size = fread(stream->filebuffer+stream->frame_offsets[frame_index], 1, stream->framesizes[frame_index], filestream);
  fclose(filestream);
  return file_size;
}

#endif

