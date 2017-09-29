/* Copyright  (C) 2010-2017 The RetroArch team
 *
 * ---------------------------------------------------------------------------------------
 * The following license statement only applies to this file (chd_stream.c).
 * ---------------------------------------------------------------------------------------
 *
 * Permission is hereby granted, free of charge,
 * to any person obtaining a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
 * INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include <streams/chd_stream.h>
#include <retro_endianness.h>
#include <libchdr/chd.h>

#define SECTOR_SIZE 2352
#define SUBCODE_SIZE 96
#define TRACK_PAD 4

struct chdstream
{
   chd_file *chd;
   /* Should we swap bytes? */
   bool swab;
   /* Size of frame taken from each hunk */
   uint32_t frame_size;
   /* Offset of data within frame */
   uint32_t frame_offset;
   /* Number of frames per hunk */
   uint32_t frames_per_hunk;
   /* First frame of track in chd */
   uint32_t track_frame;
   /* Byte offset where track data starts (after pregap) */
   size_t track_start;
   /* Byte offset where track data ends */
   size_t track_end;
   /* Byte offset of read cursor */
   size_t offset;
   /* Loaded hunk number */
   int32_t hunknum;
   /* Loaded hunk */
   uint8_t *hunkmem;
};

typedef struct metadata {
   char type[64];
   char subtype[32];
   char pgtype[32];
   char pgsub[32];
   uint32_t frame_offset;
   uint32_t frames;
   uint32_t pad;
   uint32_t extra;
   uint32_t pregap;
   uint32_t postgap;
   uint32_t track;
} metadata_t;

static uint32_t padding_frames(uint32_t frames)
{
   return ((frames + TRACK_PAD - 1) & ~(TRACK_PAD - 1)) - frames;
}

static bool
chdstream_get_meta(chd_file *chd, int idx, metadata_t *md)
{
   char meta[256];
   uint32_t meta_size = 0;
   chd_error err;

   memset(md, 0, sizeof(*md));

   err = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, idx, meta,
         sizeof(meta), &meta_size, NULL, NULL);

   if (err == CHDERR_NONE)
   {
      sscanf(meta, CDROM_TRACK_METADATA2_FORMAT,
            &md->track, md->type,
            md->subtype, &md->frames, &md->pregap,
            md->pgtype, md->pgsub,
            &md->postgap);
      md->extra = padding_frames(md->frames);
      return true;
   }

   err = chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, idx, meta,
         sizeof(meta), &meta_size, NULL, NULL);

   if (err == CHDERR_NONE)
   {
      sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &md->track, md->type,
             md->subtype, &md->frames);
      md->extra = padding_frames(md->frames);
      return true;
   }

   err = chd_get_metadata(chd, GDROM_TRACK_METADATA_TAG, idx, meta,
         sizeof(meta), &meta_size, NULL, NULL);

   if (err == CHDERR_NONE)
   {
      sscanf(meta, GDROM_TRACK_METADATA_FORMAT, &md->track, md->type,
             md->subtype, &md->frames, &md->pad, &md->pregap, md->pgtype,
             md->pgsub, &md->postgap);
      md->extra = padding_frames(md->frames);
      return true;
   }

   return false;
}

static bool
chdstream_find_track_number(chd_file *fd, int32_t track, metadata_t *meta)
{
   uint32_t i;
   uint32_t frame_offset = 0;

   for (i = 0; true; ++i)
   {
      if (!chdstream_get_meta(fd, i, meta))
         return false;

      if (track == meta->track)
      {
         meta->frame_offset = frame_offset;
         return true;
      }

      frame_offset += meta->frames + meta->extra;
   }
}

static bool
chdstream_find_special_track(chd_file *fd, int32_t track, metadata_t *meta)
{
   int32_t i;
   metadata_t iter;
   int32_t largest_track = 0;
   uint32_t largest_size = 0;

   for (i = 1; true; ++i)
   {
      if (!chdstream_find_track_number(fd, i, &iter))
      {
         if (track == CHDSTREAM_TRACK_LAST && i > 1)
         {
            *meta = iter;
            return true;
         }
         else if (track == CHDSTREAM_TRACK_PRIMARY && largest_track != 0)
            return chdstream_find_track_number(fd, largest_track, meta);
      }

      switch (track)
      {
         case CHDSTREAM_TRACK_FIRST_DATA:
            if (strcmp(iter.type, "AUDIO"))
            {
               *meta = iter;
               return true;
            }
            break;
         case CHDSTREAM_TRACK_PRIMARY:
            if (strcmp(iter.type, "AUDIO") && iter.frames > largest_size)
            {
               largest_size = iter.frames;
               largest_track = iter.track;
            }
            break;
         default:
            break;
      }
   }
}

static bool
chdstream_find_track(chd_file *fd, int32_t track, metadata_t *meta)
{
   if (track < 0)
      return chdstream_find_special_track(fd, track, meta);
   return chdstream_find_track_number(fd, track, meta);
}

chdstream_t *chdstream_open(const char *path, int32_t track)
{
   metadata_t meta;
   uint32_t pregap      = 0;
   const chd_header *hd = NULL;
   chdstream_t *stream  = NULL;
   chd_file *chd        = NULL;
   chd_error err        = chd_open(path, CHD_OPEN_READ, NULL, &chd);

   if (err != CHDERR_NONE)
      goto error;

   if (!chdstream_find_track(chd, track, &meta))
      goto error;

   stream = (chdstream_t*)calloc(1, sizeof(*stream));
   if (!stream)
      goto error;

   hd              = chd_get_header(chd);
   stream->hunkmem = (uint8_t*)malloc(hd->hunkbytes);
   if (!stream->hunkmem)
      goto error;

   if (!strcmp(meta.type, "MODE1_RAW"))
   {
      stream->frame_size = SECTOR_SIZE;
      stream->frame_offset = 0;
   }
   else if (!strcmp(meta.type, "MODE2_RAW"))
   {
      stream->frame_size = SECTOR_SIZE;
      stream->frame_offset = 0;
   }
   else if (!strcmp(meta.type, "AUDIO"))
   {
      stream->frame_size = SECTOR_SIZE;
      stream->frame_offset = 0;
      stream->swab = true;
   }
   else
   {
      stream->frame_size = hd->unitbytes;
      stream->frame_offset = 0;
   }

   /* Only include pregap data if it was in the track file */
   if (!strcmp(meta.type, meta.pgtype))
      pregap = meta.pregap;
   else
      pregap = 0;


   stream->chd             = chd;
   stream->frames_per_hunk = hd->hunkbytes / hd->unitbytes;
   stream->track_frame     = meta.frame_offset;
   stream->track_start     = (size_t) pregap * stream->frame_size;
   stream->track_end       = stream->track_start + 
      (size_t) meta.frames * stream->frame_size;
   stream->offset          = 0;
   stream->hunknum         = -1;

   return stream;

error:

   chdstream_close(stream);

   if (chd)
      chd_close(chd);

   return NULL;
}

void chdstream_close(chdstream_t *stream)
{
   if (stream)
   {
      if (stream->hunkmem)
         free(stream->hunkmem);
      if (stream->chd)
         chd_close(stream->chd);
      free(stream);
   }
}

static bool
chdstream_load_hunk(chdstream_t *stream, uint32_t hunknum)
{
   chd_error err;
   uint16_t *array;
   uint32_t i;
   uint32_t count;

   if (hunknum == stream->hunknum)
      return true;

   err = chd_read(stream->chd, hunknum, stream->hunkmem);
   if (err != CHDERR_NONE)
      return false;

   if (stream->swab)
   {
      count = chd_get_header(stream->chd)->hunkbytes / 2;
      array = (uint16_t*) stream->hunkmem;
      for (i = 0; i < count; ++i)
         array[i] = SWAP16(array[i]);
   }

   stream->hunknum = hunknum;
   return true;
}

ssize_t chdstream_read(chdstream_t *stream, void *data, size_t bytes)
{
   size_t end;
   uint32_t frame_offset;
   uint32_t hunk_offset;
   uint32_t chd_frame;
   uint32_t hunk;
   uint32_t amount;
   size_t data_offset   = 0;
   const chd_header *hd = chd_get_header(stream->chd);
   uint8_t         *out = data;

   if (stream->track_end - stream->offset < bytes)
      bytes = stream->track_end - stream->offset;

   end = stream->offset + bytes;
   while (stream->offset < end)
   {
      frame_offset = stream->offset % stream->frame_size;
      amount = stream->frame_size - frame_offset;
      if (amount > end - stream->offset)
         amount = end - stream->offset;

      /* In pregap */
      if (stream->offset < stream->track_start)
         memset(out + data_offset, 0, amount);
      else
      {
         chd_frame = stream->track_frame +
            (stream->offset - stream->track_start) / stream->frame_size;
         hunk = chd_frame / stream->frames_per_hunk;
         hunk_offset = (chd_frame % stream->frames_per_hunk) * hd->unitbytes;

         if (!chdstream_load_hunk(stream, hunk))
         {
            abort();
            return -1;
         }
         memcpy(out + data_offset,
                stream->hunkmem + frame_offset 
                + hunk_offset + stream->frame_offset, amount);
      }

      data_offset    += amount;
      stream->offset += amount;
   }

   return bytes;
}

int chdstream_getc(chdstream_t *stream)
{
   char c = 0;

   if (chdstream_read(stream, &c, sizeof(c) != sizeof(c)))
      return EOF;

   return c;
}

char *chdstream_gets(chdstream_t *stream, char *buffer, size_t len)
{
   int c;

   size_t offset = 0;

   while (offset < len && (c = chdstream_getc(stream)) != EOF)
      buffer[offset++] = c;

   if (offset < len)
      buffer[offset] = '\0';

   return buffer;
}

size_t chdstream_tell(chdstream_t *stream)
{
   return stream->offset;
}

void chdstream_rewind(chdstream_t *stream)
{
   stream->offset = 0;
}

int chdstream_seek(chdstream_t *stream, ssize_t offset, int whence)
{
   ssize_t new_offset;

   switch (whence)
   {
     case SEEK_SET:
        new_offset = offset;
        break;
     case SEEK_CUR:
        new_offset = stream->offset + offset;
        break;
     case SEEK_END:
        new_offset = stream->track_end + offset;
        break;
     default:
        return -1;
   }

   if (new_offset < 0)
      return -1;

   if (new_offset > stream->track_end)
      new_offset = stream->track_end;

   stream->offset = new_offset;
   return 0;
}
