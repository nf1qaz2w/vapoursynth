// avfsavi.cpp : Avisynth Virtual File System
//
// Avisynth v2.5.  Copyright 2008-2010 Ben Rudiak-Gould et al.
// http://www.avisynth.org

// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA, or visit
// http://www.gnu.org/copyleft/gpl.html .
//
// Linking Avisynth statically or dynamically with other modules is making a
// combined work based on Avisynth.  Thus, the terms and conditions of the GNU
// General Public License cover the whole combination.
//
// As a special exception, the copyright holders of Avisynth give you
// permission to link Avisynth with independent modules that communicate with
// Avisynth solely through the interfaces defined in avisynth.h, regardless of
// the license terms of these independent modules, and to copy and distribute
// the resulting combined work under terms of your choice, provided that
// every copy of the combined work is accompanied by a complete copy of
// the source code of Avisynth (the version of Avisynth used to produce the
// combined work), being distributed under the terms of the GNU General
// Public License plus this exception.  An independent module is a module
// which is not derived from or based on Avisynth, such as 3rd-party filters,
// import and export plugins, or graphical user interfaces.



//----------------------------------------------------------------------------
// Implementation Notes.
//
// The AVI v2 file format is a sequence of RIFF segments appended
// end to end. Each segment is formatted the same as a single RIFF
// file, which limits the segment size to 4GB. For compatibility
// it is best to limit the actual max segment size to 1GB. The first
// segment contains the full AVI headers including a "super" index
// that can be used to find the location of any chunk of audio/video
// data.
//
// The AVI2 index must be broken into chunks to handle >4GB file
// sizes. The header of each index chunk has a 64 bit base offset.
// Each index entry has a 32 bit offset which is combined with
// the base offset to get the actual 64 bit file offset.
//
// This implementation creates an index chunk at the end of each
// segment that indexes the contents of the one segment. The
// number of audo/video data chunks in all the segments is the
// same, except for the final segment which is the same or
// smaller. This has the effect of making the index chunks all
// the same size, except for the last.
//
// The AVI and AVI2 file formats allow full flexibility over the
// placement of audio data in the file. However, applications that
// read AVI files can be pretty picky. This implementation
// interleaves audio data per video frame, creating the same number
// of audio chunks as video chunks.
//
//----------------------------------------------------------------------------

#include "avfsincludes.h"

#define AVI_INDEX_OF_INDEXES       0x00
#define AVI_INDEX_OF_CHUNKS        0x01
#define AVI_INDEX_SUB_DEFAULT      0x00
#define AVI_INDEX_SUB_2FIELD       0x01

#define AVIF_HASINDEX        0x00000010
#define AVIF_MUSTUSEINDEX    0x00000020
#define AVIF_ISINTERLEAVED   0x00000100

#ifndef AVIIF_LIST
# define AVIIF_LIST       0x00000001
# define AVIIF_KEYFRAME   0x00000010
#endif

unsigned RiffAlignUp(unsigned size)  // Align size to 16bits
{
   return (size+(2-1))&~(2-1);
}

// RIFFCHUNK - 'JUNK', 'strf' etc

struct RiffTag
{
   uint32_t fcc;
   uint32_t cb;
};

/*  typedef struct _rifflist
    {
      FOURCC fcc;
      DWORD cb;
      FOURCC fccListType;
    } RIFFLIST, * LPRIFFLIST; */

// RIFFLIST  - 'RIFF', 'LIST'

static const uint32_t riffFcc     = MAKETAGUINT32('R','I','F','F');
static const uint32_t riffLstFcc  = MAKETAGUINT32('L','I','S','T');
static const uint32_t riffJunkFcc = MAKETAGUINT32('J','U','N','K');

struct RiffLst {
  RiffTag tag;
  uint32_t fcc;
};

static const uint32_t avi2MaxSegSize    = 0x3FFFFFFE;
static const uint32_t avi2Max4GbSegSize = 0xFFFFFFFE;

static const uint32_t avi2FileFcc    = MAKETAGUINT32('A','V','I',' ');
static const uint32_t avi2SegLstFcc  = MAKETAGUINT32('A','V','I','X');
static const uint32_t avi2HdrLstFcc  = MAKETAGUINT32('h','d','r','l');
static const uint32_t avi2DataLstFcc = MAKETAGUINT32('m','o','v','i');


// AVIMAINHEADER - 'avih'

static const uint32_t avi2MainHdrFcc = MAKETAGUINT32('a','v','i','h');

struct Avi2MainHdr {
  RiffTag tag;
  uint32_t dwMicroSecPerFrame;
  uint32_t dwMaxBytesPerSec;
  uint32_t dwPaddingGranularity;
  uint32_t dwFlags;
  uint32_t dwTotalFrames;
  uint32_t dwInitialFrames;
  uint32_t dwStreams;
  uint32_t dwSuggestedBufferSize;
  uint32_t dwWidth;
  uint32_t dwHeight;
  uint8_t  reserved1[16];
};

// AVISTREAMHEADER - 'strh'

static const uint32_t avi2StrHdrFcc = MAKETAGUINT32('s','t','r','h');

struct Avi2StrHdr {
  RiffTag tag;
  uint32_t fccType;
  uint32_t fccHandler;
  uint32_t dwFlags;
  uint16_t wPriority;
  uint16_t wLanguage;
  uint32_t dwInitialFrames;
  uint32_t dwScale;
  uint32_t dwRate;       // dwRate/dwScale is stream tick rate in ticks/sec
  uint32_t dwStart;
  uint32_t dwLength;
  uint32_t dwSuggestedBufferSize;
  uint32_t dwQuality;
  uint32_t dwSampleSize;
  int16_t  frameLeft;
  int16_t  frameTop;
  int16_t  frameRight;
  int16_t  frameBottom;
};

// RIFFCHUNK + BITMAPINFOHEADER - 'strf'

static const uint32_t avi2VidFrmtFcc = MAKETAGUINT32('s','t','r','f');

struct Avi2VidFrmt {
  RiffTag tag;
  uint32_t biSize;
  int32_t  biWidth;
  int32_t  biHeight;
  uint16_t biPlanes;
  uint16_t biBitCount;
  uint32_t biCompression;
  uint32_t biSizeImage;
  int32_t  biXPelsPerMeter;
  int32_t  biYPelsPerMeter;
  uint32_t biClrUsed;
  uint32_t biClrImportant;
};

static const uint32_t avi2VidHdrLstFcc = MAKETAGUINT32('s','t','r','l');
static const uint32_t avi2VidStrTypeFcc = MAKETAGUINT32('v','i','d','s');

// RIFFCHUNK + PCMWAVEFORMAT or (WAVEFORMAT + WORD) - 'strf'

static const uint32_t avi2AudFrmtFcc = MAKETAGUINT32('s','t','r','f');

struct Avi2AudFrmt {
  RiffTag tag;
  WaveFormatExtensible wfx;
};

static const uint32_t avi2AudHdrLstFcc     = MAKETAGUINT32('s','t','r','l');
static const uint32_t avi2AudStrTypeFcc    = MAKETAGUINT32('a','u','d','s');

static const uint32_t avi2IndxFcc = MAKETAGUINT32('i','n','d','x');

struct Avi2IndxHdr {
  RiffTag tag;
  uint16_t wLongsPerEntry;
  uint8_t  bIndxSubType;
  uint8_t  bIndxType;
  uint32_t nEntriesInUse;
  uint32_t dwChunkId;
  uint32_t qwBaseOffsetLow;
  uint32_t qwBaseOffsetHigh;
  uint8_t reserved1[4];
};
static_assert(sizeof(Avi2IndxHdr) == 32, "");


// hdr.wLongsPerEntry = sizeof(Avi2SuperIndxEntry)/4
// hdr.bIndxSubType = AVI_INDEX_SUB_DEFAULT or AVI_INDEX_SUB_2FIELD
// hdr.bIndxType = AVI_INDEX_OF_INDEXES
// hdr.qwBaseOffset = 0
struct Avi2SuperIndxEntry {
  uint32_t qwOffsetLow;
  uint32_t qwOffsetHigh;
  uint32_t dwSize;
  uint32_t dwDuration;
};

static const unsigned avfsAvi2MaxSuperIndxEntryCount = 5000;

// AVISUPERINDEX - 'indx'

struct AvfsAvi2SuperIndx {
  Avi2IndxHdr hdr;
  Avi2SuperIndxEntry ents[avfsAvi2MaxSuperIndxEntryCount];
};


// hdr.wLongsPerEntry = sizeof(Avi2IndxEntry)/4
// hdr.bIndxType = AVI_INDEX_OF_CHUNKS
// hdr.dwChunkId = avi2AudFcc or avi2VidFcc
struct Avi2IndxEntry {
  uint32_t dwOffset;
  uint32_t dwSize;
};

// AVISTDINDEX - 'ix00', 'ix01'

struct AvfsAvi2Indx {
  Avi2IndxHdr hdr;
  Avi2IndxEntry ents[1/*hdr.nEntriesInUse*/];
};

// AVIEXTHEADER - 'dmlh'

static const uint32_t avi2ExtHdrFcc = MAKETAGUINT32('d','m','l','h');

struct Avi2ExtHdr {
  RiffTag tag;
  uint32_t dwGrandFrames;
  uint8_t reserved[244];
};

// RIFFLIST - 'odml'

static const uint32_t avi2ExtHdrLstFcc = MAKETAGUINT32('o','d','m','l');

struct Avi2ExtHdrLst {
  RiffLst lst;
  Avi2ExtHdr hdr;
};

// AVIOLDINDEX - 'idx1'

static const uint32_t avi2OldIndxFcc = MAKETAGUINT32('i','d','x','1');

struct Avi2OldIndxEntry {
  uint32_t dwChunkId;
  uint32_t dwFlags;
  uint32_t dwOffset;
  uint32_t dwSize;
};

// {video} = RIFFLIST + AVISTREAMHEADER + RIFFCHUNK + BITMAPINFOHEAD + AVISUPERINDEX
//         - 'LIST', 'strl', 'strh', 'vids', 'indx'

struct AvfsAvi2VidHdrLst {
  RiffLst lst;
  Avi2StrHdr hdr;
  Avi2VidFrmt vidFrmt;
  AvfsAvi2SuperIndx indx;
};

// {audio} = RIFFLIST + AVISTREAMHEADER + RIFFCHUNK + PCMWAVEFORMAT + AVISUPERINDEX
//         - 'LIST', 'strl', 'strh', 'auds', 'indx'

struct AvfsAvi2AudHdrLst {
  RiffLst lst;
  Avi2StrHdr hdr;
  Avi2AudFrmt audFrmt;
  AvfsAvi2SuperIndx indx;
};

// RIFFCHUNK - 'JUNK'

struct AvfsAvi2HdrJunk {
  Avi2IndxHdr hdr;
  uint8_t junk[10*1024];
};

// RIFFLIST + AVIMAINHEADER + {video} + {audio} + RIFFLIST + AVIEXTHEADER
//         - 'LIST' 'hdrl' 'avih' {} {} 'LIST' 'odml' 'dmlh'

struct AvfsAvi2HdrLst {
  RiffLst lst;
  Avi2MainHdr mainHdr;
  AvfsAvi2VidHdrLst vidLst;
  AvfsAvi2AudHdrLst audLst;
  Avi2ExtHdrLst extLst;
  AvfsAvi2HdrJunk junk;
};

// RIFFCHUNK - '01wb'

struct AvfsAvi2Aud {
  RiffTag tag;
  uint8_t data[1];
};

// RIFFCHUNK - '00db' DIB or '00dc' Compressed

struct AvfsAvi2Vid {
  RiffTag tag;
  uint8_t data[1];
};

// RIFFLIST + {data} - 'LIST', 'movi', ...

struct AvfsAvi2DataLst {
  RiffLst lst;
  uint8_t data[1];
  // AvfsAvi2Aud frame0Aud;
  // AvfsAvi2Vid frame0Vid;
  // ...
  // AvfsAvi2Aud frameNAud;
  // AvfsAvi2Vid frameNVid;
  // AvfsAvi2Indx vidIndx;
  // AvfsAvi2Indx audIndx;
};

// AVIOLDINDEX - 'idx1'

struct AvfsAvi2OldIndx {
  RiffTag tag;
  Avi2OldIndxEntry ents[1/*hdr.cb/sizeof(ents[0])*/];
};

// RIFFLIST + ... - 'RIFF', 'AVI ', ...

struct AvfsAvi2Seg0 {
  RiffLst lst;
  AvfsAvi2HdrLst hdrLst;
  AvfsAvi2DataLst dataLst;
  // AvfsAvi2OldIndx oldIndx;
};

// RIFFLIST + RIFFLIST - 'RIFF', 'AVIX', 'LIST', 'movi'

struct AvfsAvi2SegN {
  RiffLst lst;
  AvfsAvi2DataLst dataLst;
};

// struct AvfsAvi2File {
//   AvfsAvi2Seg0 seg0;
//   AvfsAvi2SegN seg1;
//   ...
//   AvfsAvi2SegN segN;
// };

static const uint32_t avfsAvi2VidRgbFcc  = MAKETAGUINT32('0','0','d','b');
static const uint32_t avfsAvi2VidCompFcc = MAKETAGUINT32('0','0','d','c');
static const uint32_t avfsAvi2AudFcc     = MAKETAGUINT32('0','1','w','b');

static const uint32_t avfsAvi2VidIndxFcc = MAKETAGUINT32('i','x','0','0');
static const uint32_t avfsAvi2AudIndxFcc = MAKETAGUINT32('i','x','0','1');

static const unsigned avfsAvi2MaxDataLstSize = avi2MaxSegSize-
  offsetof(AvfsAvi2Seg0, dataLst.data)-offsetof(AvfsAvi2OldIndx, ents);

static const unsigned avfsAvi2Max4GbDataLstSize = avi2Max4GbSegSize-
  offsetof(AvfsAvi2Seg0, dataLst.data)-offsetof(AvfsAvi2OldIndx, ents);

struct AvfsAvi2File final: AvfsMediaFile_ {
  int references = 1;
  Synther_* avs = nullptr;
  Avisynther_* avssynther = nullptr;
  VapourSynther_* vssynther = nullptr;

  VideoInfoAdapter vi;

  uint32_t frameVidFcc = 0;
  unsigned frameAudHdrSize = 0;
  unsigned frameVidDataSize = 0;
  unsigned frameVidAlignSize = 0;
  unsigned vidFrameCount = 0;
  unsigned audFrameCount = 0;
  unsigned fileFrameCount = 0;
  unsigned durFrameCount = 0;
  unsigned sampleSize = 0;
  unsigned firstAudFramePackCount = 0;
  uint64_t fileSampleCount = 0;
  unsigned fileSegCount = 0;
  uint64_t fileSize = 0;

  enum { indxPrePadSize  = 0x20000 };
  enum { indxPostPadSize = 0x20000 };

  struct Seg {
    uint64_t startOffset;
    unsigned startFrame;
    unsigned vidFrameCount;
    unsigned audFrameCount;
    unsigned frameCount;
    unsigned lastAudFramePackCount;
    unsigned hdrSize;
    unsigned dataSize;
    unsigned vidIndxSize;
    unsigned audIndxSize;
    unsigned oldIndxSize;
    unsigned segSize;
    unsigned* frameIndx;
    AvfsAvi2Indx* vidIndx;
    AvfsAvi2Indx* audIndx;
    AvfsAvi2OldIndx* oldIndx;
    union {
      AvfsAvi2Seg0 seg0;
      AvfsAvi2SegN segN;
      uint8_t buf[1];
    } hdr;
  };
  Seg** segs = nullptr;
  uint8_t* sampleScratch = nullptr;

  AvfsAvi2File(Avisynther_* avs);
  AvfsAvi2File(VapourSynther_* vs);

  ~AvfsAvi2File(void);

  uint64_t/*start*/ LocateFrameSamples(
    unsigned frame,
    unsigned frameCount,
    unsigned* sampleCount);

  unsigned FrameSampleCount(unsigned frame) {
    unsigned sampleCount;
    LocateFrameSamples(frame, 1, &sampleCount);
    return sampleCount;
  }

  bool/*success*/ Init(AvfsLog_* log);

  void AddRef(void);
  void Release(void);

  bool/*success*/ GetFrameData(
    AvfsLog_* log,
    uint8_t* buffer,
    unsigned position,
    size_t offset,
    size_t size);

  bool/*success*/ ReadMedia(
    AvfsLog_* log,
    uint64_t fileOffset,
    void* buffer,
    size_t requestedSize);
};

AvfsAvi2File::AvfsAvi2File(Avisynther_* inAvs)
 :
  vi(inAvs->GetVideoInfo())
{
  avs = inAvs;
  avssynther = inAvs;
  avs->AddRef();
}

AvfsAvi2File::AvfsAvi2File(VapourSynther_* inVs)
  :
   vi(inVs->GetVideoInfo())
{
    avs = inVs;
    vssynther = inVs;
    avs->AddRef();
}

AvfsAvi2File::~AvfsAvi2File(void)
{
  ASSERT(!references);
  unsigned segi;
  avs->Release();
  if (segs) {
    for (segi = 0; segi < fileSegCount; segi ++) {
      if (segs[segi]) {
        free(segs[segi]);
      }
    }
    free(segs);
  }
  if (sampleScratch) {
    free(sampleScratch);
  }
}

// Return the starting Audio sample number for frame N
// and the number of samples in frameCount.
// Handles audio preload

uint64_t/*start*/ AvfsAvi2File::LocateFrameSamples(
  unsigned frame,
  unsigned frameCount,
  unsigned* outSampleCount)
{
  uint64_t startSample = 0;
  uint64_t endSample = 0;
  unsigned startFrame = frame;
  unsigned endFrame = frame+frameCount;
  if (startFrame != 0) {
    startFrame += firstAudFramePackCount;
  }
  if (endFrame != 0) {
    endFrame += firstAudFramePackCount;
  }
  if (fileSampleCount) {
    startSample = uint64_t(vi.AudioSamplesFromFrames(startFrame));
    if (startSample > fileSampleCount) {
      startSample = fileSampleCount;
    }
    endSample = uint64_t(vi.AudioSamplesFromFrames(endFrame));
    if (endSample > fileSampleCount) {
      endSample = fileSampleCount;
    }
  }
  if(outSampleCount) {
    *outSampleCount = unsigned(endSample-startSample);
  }
  return startSample;
}

bool/*success*/ AvfsAvi2File::Init(
  AvfsLog_* log)
{
  bool success = true;
  unsigned long vidType;
  unsigned long vidCompress;
  unsigned maxFrameAudDataSize = 0;
  uint16_t sampleType = 0;
  unsigned clippedSampleCount = 0xFFFFFFFF;
  unsigned maxSegFrameCount = 0;
  unsigned allocSize;
  unsigned segi;
  unsigned segSize;
  unsigned segStartFrame;
  unsigned segVidFrameCount;
  unsigned segAudFrameCount;
  unsigned segLastAudFramePackCount;
  unsigned segFrameCount;
  unsigned segDurFrameCount;
  unsigned segFrame;
  unsigned segHdrSize;
  unsigned segDataSize;
  unsigned segVidIndxSize;
  unsigned segAudIndxSize;
  unsigned segOldIndxSize;
  uint16_t bitsPerPixel;
  uint64_t duration;
  Seg* seg;
  uint8_t* buf;
  Avi2SuperIndxEntry* vidIndxEnts = 0;
  Avi2SuperIndxEntry* audIndxEnts = 0;
  Avi2OldIndxEntry* oldIndxEnts = 0;
  uint64_t frameStartSample;
  unsigned frameSampleCount;
  unsigned frameAudDataSize;
  unsigned oldi;
  const char* fourcc;
  bool noInterleave = false;

  // Setup video attributes.
  vidFrameCount = unsigned(vi.num_frames);
  frameVidDataSize = vi.BMPSize();
  bitsPerPixel = uint16_t(vi.BitsPerPixel());

  vidType = 0;
  vidCompress = 0;

  bool vidSuccess = true;

  if (vi.HasVideo()) {
      vidSuccess = vidSuccess && GetFourCC(vi.vf, vi.output_format, vidType);
      vidSuccess = vidSuccess && GetBiCompression(vi.vf, vi.output_format, vidCompress);
  }

  if (!vidType || !vidFrameCount || !frameVidDataSize || !vidSuccess) {
    vidType = 0;
    vidFrameCount = 0;
    frameVidDataSize = 0;
  } else {
    frameVidAlignSize = RiffAlignUp(frameVidDataSize)-frameVidDataSize;

    // Allow script to override fourcc code for the video. Non RGB
    // uncompressed AVI files are not very compatible, but specific
    // fourcc codes might help in some cases.

    fourcc = avs->GetVarAsString("AVFS_AVI_VidFcc", 0);
    if (sslen(fourcc) >= 4) {
      vidType = vidCompress = MAKETAGUINT32(
        (uint8_t)(fourcc[0]), (uint8_t)(fourcc[1]),
        (uint8_t)(fourcc[2]), (uint8_t)(fourcc[3]));
    }
  }

  if (vidType == MAKETAGUINT32('D','I','B',' '))
    frameVidFcc = avfsAvi2VidRgbFcc;       // 00db
  else
    frameVidFcc = avfsAvi2VidCompFcc;      // 00dc

  durFrameCount  = vidFrameCount;
  fileFrameCount = vidFrameCount;

  if (vi.HasAudio())
      sampleType = vi.AudioIsFloat() ? 2 : 1;

  int bytesPerOutputSample = (vi.BitsPerChannelSample() + 7) / 8;
  sampleSize = bytesPerOutputSample * vi.AudioChannels();
  fileSampleCount = unsigned(vi.num_audio_samples); // FIXME, how does avi handle more than 2^32-1 samples? there should probably be some erroring out in this case

  if (!sampleType || !sampleSize || !fileSampleCount) {
    sampleType = 0;
    sampleSize = 0;
    fileSampleCount = 0;
  }
  else {
    frameAudHdrSize = sizeof(RiffTag);
    maxFrameAudDataSize = RiffAlignUp(unsigned(vi.AudioSamplesFromFrames(1)+1)*sampleSize);
    // How many frames are needed to represent audio stream.
    audFrameCount = unsigned(vi.FramesFromAudioSamples(fileSampleCount+vi.AudioSamplesFromFrames(1)-1)); // ceil!

    // Fixme - prove this test is redundant!
    if (!audFrameCount) {
      audFrameCount = 1;
    }
    // Duration of AVI is in frames, larger of audio and video
    // streams.
    if (durFrameCount < audFrameCount) {
      durFrameCount = audFrameCount;
    }

    noInterleave = avs->GetVarAsBool("AVFS_AVI_NoInterleave", false);

    if (!noInterleave) {
      // Preload extra .5 sec worth of audio samples with first frame.
      firstAudFramePackCount = unsigned(vi.fps_numerator+vi.fps_denominator-1)/  // ceil!
         (unsigned(vi.fps_denominator)*2);
      while (audFrameCount && FrameSampleCount(audFrameCount-1) == 0) {
        audFrameCount --;
      }
    }

    if (audFrameCount > fileFrameCount) {
      fileFrameCount = audFrameCount;
    }
  }

  if (clippedSampleCount > fileSampleCount) {
    clippedSampleCount = unsigned(fileSampleCount);
  }

  // Fixme - No video AVIs although unusual are valid
  if (!vidFrameCount) {
    log->Printf(L"AvfsAviMediaInit: Clip has no supported video.\n");
    success = false;
  }
  else {
    // Calculate max number of frames that can go in a 4GB segment.
    maxSegFrameCount = (   avfsAvi2Max4GbDataLstSize
                         - firstAudFramePackCount*maxFrameAudDataSize
                         - indxPrePadSize
                         - indxPostPadSize
                       )
                     /
                       (   sizeof(RiffTag)
                         + maxFrameAudDataSize
                         + sizeof(RiffTag)
                         + frameVidDataSize
                         + frameVidAlignSize
                         + sizeof(Avi2IndxEntry)*2
                         + sizeof(Avi2OldIndxEntry)*2
                       );

    fileSegCount = (fileFrameCount+maxSegFrameCount-1)/maxSegFrameCount; // ceil!
    ASSERT(fileSegCount);

    if (/*fileSegCount > 1 &&*/ avs->GetVarAsBool("AVFS_AVI_SmallSegments", false))
    {
      // Break file into 1GB segments instead of 4GB segments. Slows
      // initialization of some applications (mplayer/mencoder, vlc), but
      // may improve compatibility.
      // Calculate max number of frames that can go in each segment and
      // still have the first segment size <1GB.
      maxSegFrameCount = (   avfsAvi2MaxDataLstSize
                           - firstAudFramePackCount*maxFrameAudDataSize
                           - indxPrePadSize
                           - indxPostPadSize
                         )
                       /
                         (   sizeof(RiffTag)
                           + maxFrameAudDataSize
                           + sizeof(RiffTag)
                           + frameVidDataSize
                           + frameVidAlignSize
                           + sizeof(Avi2IndxEntry)*2
                           + sizeof(Avi2OldIndxEntry)*2
                         );

      fileSegCount = (fileFrameCount+maxSegFrameCount-1)/maxSegFrameCount;
    }

    if (fileSegCount > avfsAvi2MaxSuperIndxEntryCount) {
      fileSegCount = avfsAvi2MaxSuperIndxEntryCount;
    }
  }

  if (fileSegCount) {
    segs = static_cast<Seg**>(malloc(sizeof(segs[0])*fileSegCount));
    if (!segs) {
      success = false;
      fileSegCount = 0;
    }
    else {
      memset(segs, 0, sizeof(segs[0])*fileSegCount);
    }
  }

  if (sampleSize) {
    sampleScratch = static_cast<uint8_t*>(malloc(sampleSize));
    if (!sampleScratch) {
      success = false;
    }
  }

  // Create header and index structures for each segment in the file.

  segStartFrame = 0;
  for (segi = 0; segi < fileSegCount; segi ++) {
    segFrameCount = fileFrameCount-segStartFrame;

    if (segFrameCount > maxSegFrameCount) {
      segFrameCount = maxSegFrameCount;
    }

    segVidFrameCount = 0;

    if (segStartFrame < vidFrameCount) {
      segVidFrameCount = vidFrameCount-segStartFrame;
    }

    if (segVidFrameCount > segFrameCount) {
      segVidFrameCount = segFrameCount;
    }

    segAudFrameCount = 0;
    segLastAudFramePackCount = 0;

    if (segStartFrame < audFrameCount) {
      segAudFrameCount = audFrameCount-segStartFrame;
    }

    if (segAudFrameCount > segFrameCount) {
      segAudFrameCount = segFrameCount;
    }

    segDurFrameCount = segFrameCount;

    if (segi+1 == fileSegCount) {
      // No more segments so remaining duration in this segment.
      segDurFrameCount = durFrameCount-segStartFrame;
    }

    // If in no-interleave mode then pack each segments audio data in
    // a single chunk.
    if (noInterleave && segAudFrameCount) {
      segLastAudFramePackCount = segAudFrameCount-1;
      segAudFrameCount = 1;
    }

    segHdrSize = offsetof(AvfsAvi2SegN, dataLst.data);

    if (segi == 0) {
      segHdrSize = offsetof(AvfsAvi2Seg0, dataLst.data);
    }

    segVidIndxSize = offsetof(AvfsAvi2Indx, ents)+
      segVidFrameCount*sizeof(Avi2IndxEntry);

    segAudIndxSize = 0;

    if (fileSampleCount) {
      segAudIndxSize = offsetof(AvfsAvi2Indx, ents)+
        segAudFrameCount*sizeof(Avi2IndxEntry);
    }

    segOldIndxSize = (segi == 0)*(offsetof(AvfsAvi2OldIndx, ents)+
        (segVidFrameCount+segAudFrameCount)*sizeof(Avi2OldIndxEntry));

    allocSize = offsetof(Seg, hdr)+segHdrSize+
      segFrameCount*sizeof(seg->frameIndx[0])+segVidIndxSize+segAudIndxSize+
      segOldIndxSize;

    seg = segs[segi] = static_cast<Seg*>(malloc(allocSize));

    if (!seg) {
      success = false;
    }
    else {
      memset(seg, 0, allocSize);

      seg->startOffset           = fileSize;
      seg->startFrame            = segStartFrame;
      seg->vidFrameCount         = segVidFrameCount;
      seg->audFrameCount         = segAudFrameCount;
      seg->frameCount            = segFrameCount;
      seg->lastAudFramePackCount = segLastAudFramePackCount;
      seg->hdrSize               = segHdrSize;
      seg->vidIndxSize           = segVidIndxSize;
      seg->audIndxSize           = segAudIndxSize;
      seg->oldIndxSize           = segOldIndxSize;

      buf            = seg->hdr.buf + segHdrSize;
      seg->frameIndx = reinterpret_cast<unsigned*>(buf);
      buf           += segFrameCount*sizeof(seg->frameIndx[0]);
      seg->vidIndx   = reinterpret_cast<AvfsAvi2Indx*>(buf);
      buf           += segVidIndxSize;
      seg->audIndx   = reinterpret_cast<AvfsAvi2Indx*>(buf);
      buf           += segAudIndxSize;
      seg->oldIndx   = reinterpret_cast<AvfsAvi2OldIndx*>(buf);

      oldIndxEnts = 0;

      if (segi == 0) {

        // First segment has full header, including super index to
        // locate the per-segment index chunks.
//                                               -- RIFFLIST
        seg->hdr.seg0.lst.tag.fcc                             = riffFcc;                 // 'RIFF'
        seg->hdr.seg0.lst.fcc                                 = avi2FileFcc;             // 'AVI '

//                                               -- RIFFLIST
        seg->hdr.seg0.hdrLst.lst.tag.fcc                      = riffLstFcc;              // 'LIST'
        seg->hdr.seg0.hdrLst.lst.tag.cb                       = sizeof(seg->hdr.seg0.hdrLst)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.lst.fcc                          = avi2HdrLstFcc;           // 'hdrl'

//                                               -- AVIMAINHEADER
        seg->hdr.seg0.hdrLst.mainHdr.tag.fcc                  = avi2MainHdrFcc;          // 'avih'
        seg->hdr.seg0.hdrLst.mainHdr.tag.cb                   = sizeof(seg->hdr.seg0.hdrLst.mainHdr)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.mainHdr.dwMicroSecPerFrame       = unsigned((uint64_t(1000000)*unsigned(vi.fps_denominator)+unsigned(vi.fps_numerator)/2)/unsigned(vi.fps_numerator));
        seg->hdr.seg0.hdrLst.mainHdr.dwFlags                  = AVIF_HASINDEX | AVIF_ISINTERLEAVED;
        seg->hdr.seg0.hdrLst.mainHdr.dwTotalFrames            = segDurFrameCount;
        seg->hdr.seg0.hdrLst.mainHdr.dwStreams                = 1+!!fileSampleCount;
     // seg->hdr.seg0.hdrLst.mainHdr.dwSuggestedBufferSize    = 0;
        seg->hdr.seg0.hdrLst.mainHdr.dwWidth                  = vi.width;
        seg->hdr.seg0.hdrLst.mainHdr.dwHeight                 = vi.height;

//                                               -- RIFFLIST
        seg->hdr.seg0.hdrLst.vidLst.lst.tag.fcc               = riffLstFcc;              // 'LIST'
        seg->hdr.seg0.hdrLst.vidLst.lst.tag.cb                = sizeof(seg->hdr.seg0.hdrLst.vidLst)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.vidLst.lst.fcc                   = avi2VidHdrLstFcc;        // 'strl'

//                                               -- AVISTREAMHEADER
        seg->hdr.seg0.hdrLst.vidLst.hdr.tag.fcc               = avi2StrHdrFcc;           // 'strh'
        seg->hdr.seg0.hdrLst.vidLst.hdr.tag.cb                = sizeof(seg->hdr.seg0.hdrLst.vidLst.hdr)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.vidLst.hdr.fccType               = avi2VidStrTypeFcc;       // 'vids'
        seg->hdr.seg0.hdrLst.vidLst.hdr.fccHandler            = vidType;
        seg->hdr.seg0.hdrLst.vidLst.hdr.dwScale               = vi.fps_denominator;
        seg->hdr.seg0.hdrLst.vidLst.hdr.dwRate                = vi.fps_numerator;
        seg->hdr.seg0.hdrLst.vidLst.hdr.dwLength              = vidFrameCount;
        seg->hdr.seg0.hdrLst.vidLst.hdr.dwSuggestedBufferSize = frameVidDataSize;
        seg->hdr.seg0.hdrLst.vidLst.hdr.dwQuality             = 0xFFFFFFFF;
        seg->hdr.seg0.hdrLst.vidLst.hdr.frameRight            = int16_t(vi.width);
        seg->hdr.seg0.hdrLst.vidLst.hdr.frameBottom           = int16_t(vi.height);

//                                               -- RIFFCHUNK
        seg->hdr.seg0.hdrLst.vidLst.vidFrmt.tag.fcc           = avi2VidFrmtFcc;          // 'strf'
        seg->hdr.seg0.hdrLst.vidLst.vidFrmt.tag.cb            = sizeof(seg->hdr.seg0.hdrLst.vidLst.vidFrmt)-sizeof(RiffTag);

//                                               -- BITMAPINFOHEAD
        seg->hdr.seg0.hdrLst.vidLst.vidFrmt.biSize            = sizeof(seg->hdr.seg0.hdrLst.vidLst.vidFrmt)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.vidLst.vidFrmt.biWidth           = vi.width;
        seg->hdr.seg0.hdrLst.vidLst.vidFrmt.biHeight          = vi.height;
        seg->hdr.seg0.hdrLst.vidLst.vidFrmt.biPlanes          = 1;
        seg->hdr.seg0.hdrLst.vidLst.vidFrmt.biBitCount        = bitsPerPixel;
        seg->hdr.seg0.hdrLst.vidLst.vidFrmt.biCompression     = vidCompress;
        seg->hdr.seg0.hdrLst.vidLst.vidFrmt.biSizeImage       = frameVidDataSize;

//                                               -- AVISUPERINDEX
        seg->hdr.seg0.hdrLst.vidLst.indx.hdr.tag.fcc          = avi2IndxFcc;             // 'indx'
        seg->hdr.seg0.hdrLst.vidLst.indx.hdr.tag.cb           = sizeof(seg->hdr.seg0.hdrLst.vidLst.indx)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.vidLst.indx.hdr.wLongsPerEntry   = sizeof(seg->hdr.seg0.hdrLst.vidLst.indx.ents[0])/4;
        seg->hdr.seg0.hdrLst.vidLst.indx.hdr.dwChunkId        = frameVidFcc;             // '00db' DIB or '00dc' Compressed
        seg->hdr.seg0.hdrLst.vidLst.indx.hdr.bIndxSubType     = AVI_INDEX_SUB_DEFAULT;
        seg->hdr.seg0.hdrLst.vidLst.indx.hdr.bIndxType        = AVI_INDEX_OF_INDEXES;
        seg->hdr.seg0.hdrLst.vidLst.indx.hdr.nEntriesInUse    = fileSegCount;

//                                               -- RIFFLIST
        seg->hdr.seg0.hdrLst.audLst.lst.tag.fcc               = fileSampleCount?riffLstFcc:riffJunkFcc; // 'LIST' or 'JUNK'
        seg->hdr.seg0.hdrLst.audLst.lst.tag.cb                = sizeof(seg->hdr.seg0.hdrLst.audLst)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.audLst.lst.fcc                   = avi2AudHdrLstFcc;        // 'strl'

//                                               -- AVISTREAMHEADER
        seg->hdr.seg0.hdrLst.audLst.hdr.tag.fcc               = avi2StrHdrFcc;           // 'strh'
        seg->hdr.seg0.hdrLst.audLst.hdr.tag.cb                = sizeof(seg->hdr.seg0.hdrLst.audLst.hdr)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.audLst.hdr.fccType               = avi2AudStrTypeFcc;       // 'auds'
     // seg->hdr.seg0.hdrLst.audLst.hdr.fccHandler            = ?;
        seg->hdr.seg0.hdrLst.audLst.hdr.dwInitialFrames       = 1; // audio preload ?
        seg->hdr.seg0.hdrLst.audLst.hdr.dwScale               = sampleSize;
        seg->hdr.seg0.hdrLst.audLst.hdr.dwRate                = vi.SamplesPerSecond()*sampleSize;
        seg->hdr.seg0.hdrLst.audLst.hdr.dwLength              = clippedSampleCount;
        seg->hdr.seg0.hdrLst.audLst.hdr.dwSuggestedBufferSize = unsigned(LocateFrameSamples(1, 1, 0)+1)*sampleSize;
        seg->hdr.seg0.hdrLst.audLst.hdr.dwQuality             = 0xFFFFFFFF;
        seg->hdr.seg0.hdrLst.audLst.hdr.dwSampleSize          = sampleSize;

//                                               -- RIFFCHUNK
        seg->hdr.seg0.hdrLst.audLst.audFrmt.tag.fcc           = avi2AudFrmtFcc;          // 'strf'
        seg->hdr.seg0.hdrLst.audLst.audFrmt.tag.cb            = sizeof(seg->hdr.seg0.hdrLst.audLst.audFrmt)-sizeof(RiffTag);

//                                               -- WAVEFORMATEXTENSIBLE
        CreateWaveFormatExtensible(seg->hdr.seg0.hdrLst.audLst.audFrmt.wfx, sampleType == 2, vi.BitsPerChannelSample(), vi.SamplesPerSecond(), vi.ChannelLayout()); // fixme, no error check

//                                               -- AVISUPERINDEX
        seg->hdr.seg0.hdrLst.audLst.indx.hdr.tag.fcc          = avi2IndxFcc;             // 'indx'
        seg->hdr.seg0.hdrLst.audLst.indx.hdr.tag.cb           = sizeof(seg->hdr.seg0.hdrLst.audLst.indx)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.audLst.indx.hdr.wLongsPerEntry   = sizeof(seg->hdr.seg0.hdrLst.audLst.indx.ents[0])/4;
        seg->hdr.seg0.hdrLst.audLst.indx.hdr.dwChunkId        = avfsAvi2AudFcc;          // '01wb'
        seg->hdr.seg0.hdrLst.audLst.indx.hdr.bIndxSubType     = AVI_INDEX_SUB_DEFAULT;
        seg->hdr.seg0.hdrLst.audLst.indx.hdr.bIndxType        = AVI_INDEX_OF_INDEXES;
        seg->hdr.seg0.hdrLst.audLst.indx.hdr.nEntriesInUse    = fileSegCount;

//                                               -- RIFFLIST
        seg->hdr.seg0.hdrLst.extLst.lst.tag.fcc               = riffLstFcc;              // 'LIST'
        seg->hdr.seg0.hdrLst.extLst.lst.tag.cb                = sizeof(seg->hdr.seg0.hdrLst.extLst)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.extLst.lst.fcc                   = avi2ExtHdrLstFcc;        // 'odml'

//                                               -- AVIEXTHEADER
        seg->hdr.seg0.hdrLst.extLst.hdr.tag.fcc               = avi2ExtHdrFcc;           // 'dmlh'
        seg->hdr.seg0.hdrLst.extLst.hdr.tag.cb                = sizeof(seg->hdr.seg0.hdrLst.extLst.hdr)-sizeof(RiffTag);
        seg->hdr.seg0.hdrLst.extLst.hdr.dwGrandFrames         = durFrameCount;

//                                               -- RIFFCHUNK
        seg->hdr.seg0.hdrLst.junk.hdr.tag.fcc                 = riffJunkFcc;             // 'JUNK'
        seg->hdr.seg0.hdrLst.junk.hdr.tag.cb                  = sizeof(seg->hdr.seg0.hdrLst.junk)-sizeof(RiffTag);

//                                               -- RIFFLIST
        seg->hdr.seg0.dataLst.lst.tag.fcc                     = riffLstFcc;              // 'LIST'
        seg->hdr.seg0.dataLst.lst.fcc                         = avi2DataLstFcc;          // 'movi'

        // Save ptr to super indexes in first segment header, to
        // allow filling in super index entries as per segment
        // index chunks are built.
        vidIndxEnts = seg->hdr.seg0.hdrLst.vidLst.indx.ents;
        audIndxEnts = seg->hdr.seg0.hdrLst.audLst.indx.ents;

        // First segment also has a legacy AVI v1 index for
        // compatibility with picky applications.
        oldIndxEnts = seg->oldIndx->ents;
      }
      else {

        // Non-first segments have only a minimal RIFF header, and
        // no legacy index.

//                                               -- RIFFLIST
        seg->hdr.segN.lst.tag.fcc                             = riffFcc;                 // 'RIFF'
        seg->hdr.segN.lst.fcc                                 = avi2SegLstFcc;           // 'AVIX'

//                                               -- RIFFLIST
        seg->hdr.segN.dataLst.lst.tag.fcc                     = riffLstFcc;              // 'LIST'
        seg->hdr.segN.dataLst.lst.fcc                         = avi2DataLstFcc;          // 'movi'
      }

      // Build the per segment indexes, audio, video, and an
      // internal frame index needed by the read logic. The
      // base offset for the audio and video indexes is the
      // offset of the RIFF header at the start of the segment.

      segSize = segHdrSize;
      oldi = 0;
      segDataSize = 0;
      for (segFrame = 0; segFrame < segFrameCount; segFrame ++) {

        // Since the per-frame audio chunk can vary in size, we need
        // a frame index so the read logic can quickly associate
        // file offsets back to frame.
        seg->frameIndx[segFrame] = segDataSize;

        if (segFrame < segAudFrameCount) {
          frameStartSample = LocateFrameSamples(segStartFrame+segFrame,
                                                (segFrame+1 == segAudFrameCount) ? segLastAudFramePackCount+1 : 1,
                                                &frameSampleCount);
          ASSERT(frameSampleCount);
          frameAudDataSize = frameSampleCount*sampleSize;

          seg->audIndx->ents[segFrame].dwOffset = segSize+segDataSize + sizeof(RiffTag);
          seg->audIndx->ents[segFrame].dwSize   = frameAudDataSize;

          if (oldIndxEnts) {
            oldIndxEnts[oldi].dwChunkId = avfsAvi2AudFcc;         // '01wb'
            oldIndxEnts[oldi].dwFlags   = AVIIF_KEYFRAME;
            oldIndxEnts[oldi].dwOffset  = segSize+segDataSize;
            oldIndxEnts[oldi].dwSize    = frameAudDataSize;
            oldi ++;
          }
          segDataSize += sizeof(RiffTag)+RiffAlignUp(frameAudDataSize);
        }

        if (segFrame < segVidFrameCount) {
          seg->vidIndx->ents[segFrame].dwOffset = segSize+segDataSize+sizeof(RiffTag);
          seg->vidIndx->ents[segFrame].dwSize   = frameVidDataSize;

          if (oldIndxEnts) {
            oldIndxEnts[oldi].dwChunkId = frameVidFcc;
            oldIndxEnts[oldi].dwFlags   = AVIIF_KEYFRAME;
            oldIndxEnts[oldi].dwOffset  = segSize+segDataSize;
            oldIndxEnts[oldi].dwSize    = frameVidDataSize;
            oldi ++;
          }
          segDataSize += sizeof(RiffTag)+frameVidDataSize+frameVidAlignSize;
        }
      }
      seg->dataSize = segDataSize;
      segSize += segDataSize;

      // Junk pad before index, to keep cache block aligned reads of index
      // from overlapping audio/video data.
      segSize += indxPrePadSize;

      // Update the super index in the first segment header with the
      // location of this segments audio and video index chunks.

      seg->vidIndx->hdr.tag.fcc          = avfsAvi2VidIndxFcc;             // 'ix00'
      seg->vidIndx->hdr.tag.cb           = segVidIndxSize-sizeof(RiffTag);
      seg->vidIndx->hdr.wLongsPerEntry   = sizeof(seg->vidIndx->ents[0])/4;
      seg->vidIndx->hdr.bIndxType        = AVI_INDEX_OF_CHUNKS;
      seg->vidIndx->hdr.nEntriesInUse    = segVidFrameCount;
      seg->vidIndx->hdr.dwChunkId        = frameVidFcc;
      seg->vidIndx->hdr.qwBaseOffsetLow  = uint32_t(fileSize);
      seg->vidIndx->hdr.qwBaseOffsetHigh = uint32_t(fileSize>>32);

      vidIndxEnts[segi].qwOffsetLow  = uint32_t(fileSize+segSize);
      vidIndxEnts[segi].qwOffsetHigh = uint32_t((fileSize+segSize)>>32);
      vidIndxEnts[segi].dwSize       = offsetof(AvfsAvi2Indx, ents)+sizeof(seg->vidIndx->ents[0])*segFrameCount;
      vidIndxEnts[segi].dwDuration   = segVidFrameCount;

      segSize += segVidIndxSize;

      if (fileSampleCount) {
        LocateFrameSamples(segStartFrame, segAudFrameCount+segLastAudFramePackCount, &frameSampleCount);

        seg->audIndx->hdr.tag.fcc          = avfsAvi2AudIndxFcc;             // 'ix01'
        seg->audIndx->hdr.tag.cb           = segAudIndxSize-sizeof(RiffTag);
        seg->audIndx->hdr.wLongsPerEntry   = sizeof(seg->audIndx->ents[0])/4;
        seg->audIndx->hdr.bIndxType        = AVI_INDEX_OF_CHUNKS;
        seg->audIndx->hdr.nEntriesInUse    = segAudFrameCount;
        seg->audIndx->hdr.dwChunkId        = avfsAvi2AudFcc;         // '01wb'
        seg->audIndx->hdr.qwBaseOffsetLow  = uint32_t(fileSize);
        seg->audIndx->hdr.qwBaseOffsetHigh = uint32_t(fileSize>>32);

        audIndxEnts[segi].qwOffsetLow  = uint32_t(fileSize+segSize);
        audIndxEnts[segi].qwOffsetHigh = uint32_t((fileSize+segSize)>>32);
        audIndxEnts[segi].dwSize       = offsetof(AvfsAvi2Indx, ents)+sizeof(seg->audIndx->ents[0])*segFrameCount;
        audIndxEnts[segi].dwDuration   = frameSampleCount;

        segSize += segAudIndxSize;
      }

      // Update the data list (movi) header now that size is known.
      if (segi == 0) {
        seg->hdr.seg0.dataLst.lst.tag.cb  = segSize-offsetof(AvfsAvi2Seg0, dataLst)-sizeof(RiffTag);
      }
      else {
        seg->hdr.segN.dataLst.lst.tag.cb  = segSize-offsetof(AvfsAvi2SegN, dataLst)-sizeof(RiffTag);
      }

      // For the first segment update the segment header with
      // the final location of the legacy index.
      if (segOldIndxSize) {
        seg->oldIndx->tag.fcc = avi2OldIndxFcc;
        seg->oldIndx->tag.cb  = segOldIndxSize-sizeof(RiffTag);
        segSize += segOldIndxSize;
      }

      // Junk padding after index, to keep cache block aligned
      // reads of index data from overlapping audio/video data.
      segSize += indxPostPadSize;

      // Update the segment RIFF header with the final segment size.
      if (segi == 0) {
        seg->hdr.seg0.lst.tag.cb = segSize-sizeof(RiffTag);
      }
      else {
        seg->hdr.segN.lst.tag.cb = segSize-sizeof(RiffTag);
      }
      seg->segSize = segSize;
      ASSERT(RiffAlignUp(segSize) == segSize);
      ASSERT(segSize <= avi2Max4GbSegSize);

      fileSize += segSize;
    }
    segStartFrame += segFrameCount;
  }

  if (segs && segs[0]) {
    duration = (uint64_t(durFrameCount)*unsigned(vi.fps_denominator)+
      unsigned(vi.fps_numerator)/2)/unsigned(vi.fps_numerator);
    duration += !duration;
    segs[0]->hdr.seg0.hdrLst.mainHdr.dwMaxBytesPerSec = unsigned(fileSize/duration);
  }

  ASSERT(!success || segStartFrame == fileFrameCount);
  return success;
}

void AvfsAvi2File::AddRef(void)
{
  ASSERT(references);
  references ++;
}

void AvfsAvi2File::Release(void)
{
  ASSERT(references);
  if (!--references) {
    delete this;
  }
}

static void copyPlaneAvs(
  uint8_t*     &buffer,
  size_t       &offset,
  size_t       &rqsize,
  avs::PVideoFrame  &frame,
  int          plane,
  unsigned     alignMask/*align-1*/)
{
  if (rqsize) {
    const unsigned rowsize = (frame->GetRowSize(plane) + alignMask) & ~alignMask;
    const unsigned plsize  = frame->GetHeight(plane) * rowsize;
    const unsigned pitch   = frame->GetPitch(plane);

    if (offset < plsize) {
      size_t size = std::min(plsize - offset, rqsize);

      const uint8_t* data = frame->GetReadPtr(plane);

      data += (offset / rowsize) * pitch;
      size_t initoff = offset % rowsize;
      offset += size;
      rqsize -= size;

      while (size > 0) {
        size_t xfer = std::min(rowsize-initoff, size);
        memcpy(buffer, data+initoff, xfer);
        buffer += xfer;
        size   -= xfer;
        data   += pitch;
        initoff = 0;
      }
    }
    offset -= plsize;
  }
}

static void copyPlaneVS(
    uint8_t*     &buffer,
    size_t     &offset,
    size_t     &rqsize,
    const VSFrame *frame,
    int          plane,
    unsigned     alignMask,/*align-1*/
    const VSAPI *vsapi) {
    if (rqsize) {
        const VSVideoFormat &fi = *vsapi->getVideoFrameFormat(frame);
        if (plane == 2 && IsSameVideoFormat(fi, cfYUV, stInteger, 8, 2, 0))
            plane = 1;
        else if (plane == 1 && IsSameVideoFormat(fi, cfYUV, stInteger, 8, 2, 0))
            plane = 2;
        const unsigned rowsize = (vsapi->getFrameWidth(frame, plane) * fi.bytesPerSample + alignMask) & ~alignMask;
        const unsigned plsize = vsapi->getFrameHeight(frame, plane) * rowsize;
        const ptrdiff_t pitch = vsapi->getStride(frame, plane);

        if (offset < plsize) {
            size_t size = std::min(plsize - offset, rqsize);

            const uint8_t* data = vsapi->getReadPtr(frame, plane);

            data += (offset / rowsize) * pitch;
            size_t initoff = offset % rowsize;
            offset += size;
            rqsize -= size;

            while (size > 0) {
                size_t xfer = std::min(rowsize - initoff, size);
                memcpy(buffer, data + initoff, xfer);
                buffer += xfer;
                size -= xfer;
                data += pitch;
                initoff = 0;
            }
        }
        offset -= plsize;
    }
}

bool/*success*/ AvfsAvi2File::GetFrameData(
  AvfsLog_* log,
  uint8_t* buffer,
  unsigned n,
  size_t offset,
  size_t size)
{
  ASSERT(offset < frameVidDataSize && offset+size <= frameVidDataSize);
  bool success = true;
  if (vssynther) {
      const VSAPI *vsapi = vssynther->GetVSApi();
      const VSFrame *frame = vssynther->GetFrame(log, n, &success);
      if (success) {
          if (NeedsPacking(vssynther->GetVideoInfo().vf)) {
              memcpy(buffer, vssynther->GetPackedFrame() + offset, size);
          } else {
              copyPlaneVS(buffer, offset, size, frame, 0, (vsapi->getVideoFrameFormat(frame)->numPlanes == 1) ? 3 : 0, vsapi);
              copyPlaneVS(buffer, offset, size, frame, 1, 0, vsapi);
              copyPlaneVS(buffer, offset, size, frame, 2, 0, vsapi);
              ASSERT(size == 0);
          }
      }
      vsapi->freeFrame(frame);
  } else {
      avs::PVideoFrame frame = avssynther->GetFrame(log, n, &success);
      if (success) {
          VideoInfoAdapter via = avssynther->GetVideoInfo();
          if (NeedsPacking(via.vf)) {
              memcpy(buffer, avssynther->GetPackedFrame() + offset, size);
          } else {
              // This logic is dodgy because of these reasons:
              // RGB32 and YUY2 has no code path. But PLANAR_Y works just as well for these
              // They won't be explicitly aligned but it doesn't matter since they're always mod4
              // Poking extra planes will read nothing so it's harmless
              copyPlaneAvs(buffer, offset, size, frame, avs::PLANAR_Y, (IsSameVideoFormat(via.vf, cfGray, stInteger, 8) || IsSameVideoFormat(via.vf, cfGray, stInteger, 16)) ? 3 : 0);
              copyPlaneAvs(buffer, offset, size, frame, avs::PLANAR_V, 0);
              copyPlaneAvs(buffer, offset, size, frame, avs::PLANAR_U, 0);
              ASSERT(size == 0);
          }
      }
  }
  return success;
}

bool/*success*/ AvfsAvi2File::ReadMedia(
  AvfsLog_* log,
  uint64_t inFileOffset,
  void* inBuffer,
  size_t inRequestedSize)
{
  // Avfspfm.cpp logic makes some guarantees.
  ASSERT(inRequestedSize);
  ASSERT(inFileOffset < fileSize);
  ASSERT(inFileOffset+inRequestedSize <= fileSize);

  bool success = true;
  uint64_t fileOffset = inFileOffset;
  size_t remainingSize = inRequestedSize;
  uint8_t* buffer = static_cast<uint8_t*>(inBuffer);
  unsigned segi = 0;
  size_t offset = 0;
  unsigned b;
  unsigned check;
  RiffTag riffTag;
  Seg* seg;

  // Init logic has pre-setup the header and index data needed
  // to satisfy reads.

  // Binary search for segment containing start of needed data.
  ASSERT(fileSegCount);
  b = 1;
  while (b < fileSegCount) {
    b <<= 1;
  }
  segi = 0;
  while ((b >>= 1) != 0) {
    segi |= b;
    if (segi >= fileSegCount || segs[segi]->startOffset > fileOffset) {
      segi &= ~b;
    }
  }
  offset = unsigned(fileOffset-segs[segi]->startOffset);

  // For each segment containing needed data.
  while (success && remainingSize) {
    seg = segs[segi];

    // Copy any needed segment header.
    if (offset >= seg->hdrSize) {
      offset -= seg->hdrSize;
    }
    else {
      if (remainingSize) {
          size_t partSize = seg->hdrSize-offset;
        if (partSize > remainingSize) {
          partSize = remainingSize;
        }
        if (partSize) {
          memcpy(buffer, reinterpret_cast<uint8_t*>(&(seg->hdr))+offset,
            partSize);
        }
        buffer += partSize;
        remainingSize -= partSize;
      }
      offset = 0;
    }

    // Copy any needed frame data, one at a time.

    // Binary search for frame containing start of needed data.
    unsigned segFrame = 0;
    check = 0;
    if (offset >= seg->dataSize) {
      offset -= seg->dataSize;
      segFrame = UINT_MAX;
    }
    else
    {
      b = 1;
      while (b < seg->frameCount) {
        b <<= 1;
      }
      while ((b >>= 1) != 0) {
        segFrame |= b;
        if (segFrame >= seg->frameCount || (seg->frameIndx[segFrame] > offset)) {
          segFrame &= ~b;
        }
      }
      offset -= seg->frameIndx[segFrame];
    }

    while (remainingSize && segFrame < seg->frameCount) {
      check = 0;

      if(segFrame < seg->audFrameCount) {
        // Last audio frame may get extra samples if in interleave
        // disabled.
        unsigned sampleCount;
        uint64_t startSample = LocateFrameSamples(seg->startFrame+segFrame,
          (segFrame+1 == seg->audFrameCount) ? seg->lastAudFramePackCount+1 : 1,
          &sampleCount);
        ASSERT(sampleCount);
        unsigned audDataSize = sampleCount*sampleSize;
        unsigned audAlignSize = RiffAlignUp(audDataSize)-audDataSize;

        // Copy any needed portion of frame audio header.
        check += sizeof(riffTag);
        if (offset >= sizeof(riffTag)) {
          offset -= sizeof(riffTag);
        }
        else {
          if (remainingSize) {
            riffTag.fcc = avfsAvi2AudFcc;         // '01wb'
            riffTag.cb  = audDataSize;
            size_t partSize = sizeof(riffTag)-offset;
            if (partSize > remainingSize) {
              partSize = remainingSize;
            }
            memcpy(buffer, reinterpret_cast<uint8_t*>(&riffTag)+offset,
               partSize);
            buffer += partSize;
            remainingSize -= partSize;
          }
          offset = 0;
        }

        // Copy any needed frame audio data.
        check += audDataSize;
        if (offset >= audDataSize) {
          offset -= audDataSize;
        }
        else {
          // Deal with ragged front.
          unsigned extraOffset = offset%sampleSize;
          if (remainingSize && extraOffset) {
            size_t partSize = sampleSize-extraOffset;
            if (partSize > remainingSize) {
              partSize = remainingSize;
            }
            success = success && avs->GetAudio(log, sampleScratch,
              offset/sampleSize, 1);
            memcpy(buffer, sampleScratch+extraOffset, partSize);
            buffer += partSize;
            remainingSize -= partSize;
            offset += partSize;
          }

          // Do sample aligned bulk of transfer.
          if (offset < audDataSize && remainingSize >= sampleSize) {
            ASSERT(offset%sampleSize == 0);
            size_t partSize = audDataSize-offset;
            if (partSize > remainingSize) {
              partSize = remainingSize-remainingSize%sampleSize;
            }
            success = success && avs->GetAudio(log, buffer, startSample+
              offset/sampleSize, int(partSize/sampleSize));
            buffer += partSize;
            remainingSize -= partSize;
            offset += partSize;
          }

          // Deal with ragged tail.
          if (offset < audDataSize && remainingSize) {
            ASSERT(remainingSize < sampleSize);
            success = success && avs->GetAudio(log, sampleScratch,
              startSample+offset/sampleSize, 1);
            memcpy(buffer, sampleScratch, remainingSize);
            buffer += remainingSize;
            remainingSize = 0;
          }
          offset = 0;
        }

        // Pad audio data up to riff alignment.
        check += audAlignSize;
        if (offset >= audAlignSize) {
          offset -= audAlignSize;
        }
        else {
          if (remainingSize) {
            size_t partSize = audAlignSize-offset;
            if (partSize > remainingSize) {
              partSize = remainingSize;
            }
            memset(buffer, 0, remainingSize);
            buffer += remainingSize;
            remainingSize = 0;
          }
          offset = 0;
        }
      }

      if (segFrame < seg->vidFrameCount) {
        // Copy any needed portion of frame video header.
        check += sizeof(riffTag);
        if (offset >= sizeof(riffTag)) {
          offset -= sizeof(riffTag);
        }
        else {
          if (remainingSize) {
            riffTag.fcc = frameVidFcc;
            riffTag.cb  = frameVidDataSize;
            size_t partSize = sizeof(riffTag)-offset;
            if (partSize > remainingSize) {
              partSize = remainingSize;
            }
            memcpy(buffer, reinterpret_cast<uint8_t*>(&riffTag)+offset,
              partSize);
            buffer += partSize;
            remainingSize -= partSize;
          }
          offset = 0;
        }

        // Copy needed portion of frame video data.
        check += frameVidDataSize;
        if (offset >= frameVidDataSize) {
          offset -= frameVidDataSize;
        }
        else {
          if (remainingSize) {
            size_t partSize = frameVidDataSize-offset;
            if (partSize > remainingSize) {
              partSize = remainingSize;
            }
            success = success && GetFrameData(log, buffer,
              seg->startFrame+segFrame, offset, partSize);
            buffer += partSize;
            remainingSize -= partSize;
          }
          offset = 0;
        }

        // Pad video data up to riff alignment.
        check += frameVidAlignSize;
        if (offset >= frameVidAlignSize) {
          offset -= frameVidAlignSize;
        }
        else {
          if (remainingSize) {
            size_t partSize = frameVidAlignSize-offset;
            if (partSize > remainingSize) {
              partSize = remainingSize;
            }
            memset(buffer, 0, remainingSize);
            buffer += remainingSize;
            remainingSize = 0;
          }
          offset = 0;
        }
      }

      // Next frame.
      segFrame ++;
#ifndef NDEBUG
      unsigned temp = seg->dataSize;
      if(segFrame < seg->frameCount) {
        temp = seg->frameIndx[segFrame];
      }
      temp -= seg->frameIndx[segFrame-1];
      ASSERT(check == temp);
#endif
    }

    // Copy any needed portion of pre-index pad.
    if (offset >= indxPrePadSize) {
      offset -= indxPrePadSize;
    }
    else {
      ASSERT(indxPrePadSize >= sizeof(riffTag));
      if (offset >= sizeof(riffTag)) {
        offset -= sizeof(riffTag);
      }
      else {
        if (remainingSize) {
          riffTag.fcc = riffJunkFcc;
          riffTag.cb  = indxPrePadSize-sizeof(riffTag);
          size_t partSize = sizeof(riffTag)-offset;
          if (partSize > remainingSize) {
            partSize = remainingSize;
          }
          memcpy(buffer, reinterpret_cast<uint8_t*>(&riffTag)+offset,
             partSize);
          buffer += partSize;
          remainingSize -= partSize;
        }
        offset = 0;
      }
      if (remainingSize) {
        size_t partSize = indxPrePadSize-sizeof(riffTag)-offset;
        if (partSize > remainingSize) {
          partSize = remainingSize;
        }
        memset(buffer, 0, partSize);
        buffer += partSize;
        remainingSize -= partSize;
      }
      offset = 0;
    }

    // Copy any needed portion of video index.
    if (offset >= seg->vidIndxSize) {
      offset -= seg->vidIndxSize;
    }
    else {
      if (remainingSize) {
        size_t partSize = seg->vidIndxSize-offset;
        if (partSize > remainingSize) {
          partSize = remainingSize;
        }
        memcpy(buffer, reinterpret_cast<uint8_t*>(seg->vidIndx)+offset,
          partSize);
        buffer += partSize;
        remainingSize -= partSize;
      }
      offset = 0;
    }

    // Copy any needed portion of audio index.
    if (offset >= seg->audIndxSize) {
      offset -= seg->audIndxSize;
    }
    else {
      if (remainingSize) {
        size_t partSize = seg->audIndxSize-offset;
        if (partSize > remainingSize) {
          partSize = remainingSize;
        }
        memcpy(buffer, reinterpret_cast<uint8_t*>(seg->audIndx)+offset,
          partSize);
        buffer += partSize;
        remainingSize -= partSize;
      }
      offset = 0;
    }

    // Copy any needed portion of legacy index.
    if (offset >= seg->oldIndxSize) {
      offset -= seg->oldIndxSize;
    }
    else {
      if (remainingSize) {
        size_t partSize = seg->oldIndxSize-offset;
        if (partSize > remainingSize) {
          partSize = remainingSize;
        }
        memcpy(buffer, reinterpret_cast<uint8_t*>(seg->oldIndx)+offset,
          partSize);
        buffer += partSize;
        remainingSize -= partSize;
      }
      offset = 0;
    }

    // Copy any needed portion of post-index pad.
    if (offset >= indxPostPadSize) {
      offset -= indxPostPadSize;
    }
    else {
      ASSERT(sizeof(riffTag) <= indxPostPadSize);
      if (offset >= sizeof(riffTag)) {
        offset -= sizeof(riffTag);
      }
      else {
        if (remainingSize) {
          riffTag.fcc = riffJunkFcc;
          riffTag.cb  = indxPostPadSize-sizeof(riffTag);
          size_t partSize = sizeof(riffTag)-offset;
          if (partSize > remainingSize) {
            partSize = remainingSize;
          }
          memcpy(buffer, reinterpret_cast<uint8_t*>(&riffTag)+offset,
             partSize);
          buffer += partSize;
          remainingSize -= partSize;
        }
        offset = 0;
      }
      if (remainingSize) {
        size_t partSize = indxPostPadSize-sizeof(riffTag)-offset;
        if (partSize > remainingSize) {
          partSize = remainingSize;
        }
        memset(buffer, 0, partSize);
        buffer += partSize;
        remainingSize -= partSize;
      }
      offset = 0;
    }

    // Next segment.
    segi++;
    offset = 0;
  }

  return success;
}

void AvfsAviMediaInit(
  AvfsLog_* log,
  Avisynther_* avs,
  AvfsVolume_* volume)
{
  ASSERT(log && avs && volume);
  static const size_t maxNameChars = 300;
  wchar_t name[maxNameChars];
  AvfsAvi2File* avi = new(std::nothrow) AvfsAvi2File(avs);
  if (avi && !avi->Init(log)) {
    avi->Release();
    avi = 0;
  }
  if (avi) {
    ssformat(name, maxNameChars, L"%s.avi", volume->GetMediaName());
    volume->CreateMediaFile(avi, name, avi->fileSize);
    avi->Release();
  }
}

void VsfsAviMediaInit(
    AvfsLog_* log,
    VapourSynther_* avs,
    AvfsVolume_* volume) {
    ASSERT(log && avs && volume);
    static const size_t maxNameChars = 300;
    wchar_t name[maxNameChars];
    AvfsAvi2File* avi = new(std::nothrow) AvfsAvi2File(avs);
    if (avi && !avi->Init(log)) {
        avi->Release();
        avi = 0;
    }
    if (avi) {
        ssformat(name, maxNameChars, L"%s.avi", volume->GetMediaName());
        volume->CreateMediaFile(avi, name, avi->fileSize);
        avi->Release();
    }
}

