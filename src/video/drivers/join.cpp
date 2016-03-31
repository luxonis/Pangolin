/* This file is part of the Pangolin Project.
 * http://github.com/stevenlovegrove/Pangolin
 *
 * Copyright (c) 2014 Steven Lovegrove
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <pangolin/video/drivers/join.h>

namespace pangolin
{
VideoJoiner::VideoJoiner(const std::vector<VideoInterface*>& src)
    : src(src), size_bytes(0), sync_attempts_to_go(-1), sync_continuously(false)
{
    // Add individual streams
    for(size_t s=0; s< src.size(); ++s)
    {
        VideoInterface& vid = *src[s];
        for(size_t i=0; i < vid.Streams().size(); ++i)
        {
            const StreamInfo si = vid.Streams()[i];
            const VideoPixelFormat fmt = si.PixFormat();
            const Image<unsigned char> img_offset = si.StreamImage((unsigned char*)size_bytes);
            streams.push_back(StreamInfo(fmt, img_offset));
        }
        size_bytes += src[s]->SizeBytes();
    }
}

VideoJoiner::~VideoJoiner()
{
    for(size_t s=0; s< src.size(); ++s) {
        src[s]->Stop();
        delete src[s];
    }
}

size_t VideoJoiner::SizeBytes() const
{
    return size_bytes;
}

const std::vector<StreamInfo>& VideoJoiner::Streams() const
{
    return streams;
}

void VideoJoiner::Start()
{
    for(size_t s=0; s< src.size(); ++s) {
        src[s]->Start();
    }
}

void VideoJoiner::Stop()
{
    for(size_t s=0; s< src.size(); ++s) {
        src[s]->Stop();
    }
}

bool VideoJoiner::Sync(int64_t tolerance_us, bool continuous)
{
    for(size_t s=0; s< src.size(); ++s)
    {
       VideoPropertiesInterface* vpi = dynamic_cast<VideoPropertiesInterface*>(src[s]);
       if(!vpi) {
         return false;
       }
    }
    sync_attempts_to_go = MAX_SYNC_ATTEMPTS;
    sync_tolerance_us = tolerance_us;
    sync_continuously = continuous;
    return true;
}

bool VideoJoiner::GrabNext( unsigned char* image, bool wait )
{
    size_t offset = 0;
    std::vector<size_t> offsets;
    std::vector<int64_t> reception_times;
    int64_t newest = std::numeric_limits<int64_t>::min();
    int64_t oldest = std::numeric_limits<int64_t>::max();
    int grabbed_all = src.size();

    for(size_t s=0; s<src.size(); ++s) {
       VideoInterface& vid = *src[s];
       if(vid.GrabNext(image+offset,wait)) {
           --grabbed_all;
       }
       offsets.push_back(offset);
       offset += vid.SizeBytes();
       if(sync_attempts_to_go >= 0) {
          VideoPropertiesInterface* vidpi = dynamic_cast<VideoPropertiesInterface*>(src[s]);
          if(vidpi->FrameProperties().contains(PANGO_HOST_RECEPTION_TIME_US)) {
             int64_t rt = vidpi->FrameProperties()[PANGO_HOST_RECEPTION_TIME_US].get<int64_t>();
             reception_times.push_back(rt);
             if(newest < rt) newest = rt;
             if(oldest > rt) oldest = rt;
          } else {
             sync_attempts_to_go = -1;
             pango_print_error("Stream %lu in join does not support startup_sync_us option.\n", s);
          }
       }
    }

    if(grabbed_all != 0){
       pango_print_error("GrabNext with wait true should always return a frame %d!\n", grabbed_all);
    }

    if((sync_continuously || (sync_attempts_to_go == 0)) && ((newest - oldest) > sync_tolerance_us) ){
       pango_print_warn("Join error, unable to sync streams within %lu us\n", (unsigned long)sync_tolerance_us);
    }

    if((sync_attempts_to_go >= 0)) {
       for(size_t s=0; s<src.size(); ++s) {
          if(reception_times[s] < (newest - sync_tolerance_us)) {
             VideoInterface& vid = *src[s];
             vid.GrabNext(image+offsets[s],false);
          }
       }
       if(!sync_continuously) --sync_attempts_to_go;
    }

    return (grabbed_all == 0);
}

bool AllInterfacesAreBufferAware(std::vector<VideoInterface*>& src)
{
  for(size_t s=0; s<src.size(); ++s) {
      if(!dynamic_cast<BufferAwareVideoInterface*>(src[s])) return false;
  }
  return true;
}

bool VideoJoiner::GrabNewest( unsigned char* image, bool wait )
{

  if(AllInterfacesAreBufferAware(src)) {
     uint32_t minN = std::numeric_limits<uint32_t>::max();
     //Find smallest number of frames it is safe to drop.
     for(size_t s=0; s<src.size(); ++s) {
         auto bai = dynamic_cast<BufferAwareVideoInterface*>(src[s]);
         unsigned int n = bai->AvailableFrames();
         minN = std::min(n, minN);
     }
     //Safely drop minN-1 frames on each interface.
     if(minN > 1) {
         for(size_t s=0; s<src.size(); ++s) {
             auto bai = dynamic_cast<BufferAwareVideoInterface*>(src[s]);
             if(!bai->DropNFrames(minN - 1)) {
                 pango_print_error("Stream %lu did not drop %u frames altough available.\n", s, (minN-1));
                 return false;
             }
         }
     }
     return GrabNext(image, wait);
  } else {
      // Simply calling GrabNewest on the child streams might cause loss of sync,
      // instead we perform as many GrabNext as possible on the first stream and
      // then pull the same number of frames from every other stream.
      size_t offset = 0;
      std::vector<size_t> offsets;
      std::vector<int64_t> reception_times;
      int64_t newest = std::numeric_limits<int64_t>::min();
      int64_t oldest = std::numeric_limits<int64_t>::max();
      bool grabbed_any = false;
      int first_stream_backlog = 0;
      int64_t rt = 0;
      bool got_frame = false;

      do {
          got_frame = src[0]->GrabNext(image+offset,false);
          if(got_frame) {
              if(sync_attempts_to_go >= 0) {
                  VideoPropertiesInterface* vidpi = dynamic_cast<VideoPropertiesInterface*>(src[0]);
                  if(vidpi->FrameProperties().contains(PANGO_HOST_RECEPTION_TIME_US)) {
                      rt = vidpi->FrameProperties()[PANGO_HOST_RECEPTION_TIME_US].get<int64_t>();
                  } else {
                      sync_attempts_to_go = -1;
                      pango_print_error("Stream %u in join does not support startup_sync_us option.\n", 0);
                  }
              }
              first_stream_backlog++;
              grabbed_any = true;
          }
      } while(got_frame);
      offsets.push_back(offset);
      offset += src[0]->SizeBytes();
      if(sync_attempts_to_go >= 0) {
          reception_times.push_back(rt);
          if(newest < rt) newest = rt;
          if(oldest > rt) oldest = rt;
      }

      for(size_t s=1; s<src.size(); ++s) {
          for (int i=0; i<first_stream_backlog; i++){
              grabbed_any |= src[s]->GrabNext(image+offset,true);
              if(sync_attempts_to_go >= 0) {
                  VideoPropertiesInterface* vidpi = dynamic_cast<VideoPropertiesInterface*>(src[s]);
                  if(vidpi->FrameProperties().contains(PANGO_HOST_RECEPTION_TIME_US)) {
                      rt = vidpi->FrameProperties()[PANGO_HOST_RECEPTION_TIME_US].get<int64_t>();
                  } else {
                      sync_attempts_to_go = -1;
                      pango_print_error("Stream %lu in join does not support startup_sync_us option.\n", s);
                  }
              }
          }
          offsets.push_back(offset);
          offset += src[s]->SizeBytes();
          if(sync_attempts_to_go >= 0) {
              reception_times.push_back(rt);
              if(newest < rt) newest = rt;
              if(oldest > rt) oldest = rt;
          }
      }
      if((sync_continuously || (sync_attempts_to_go == 0)) && ((newest - oldest) > sync_tolerance_us) ){
          pango_print_warn("Join error, unable to sync streams within %lu us\n", (unsigned long)sync_tolerance_us);
      }

      if(sync_attempts_to_go >= 0) {
          for(size_t s=0; s<src.size(); ++s) {
              if(reception_times[s] < (newest - sync_tolerance_us)) {
                  VideoInterface& vid = *src[s];
                  vid.GrabNewest(image+offsets[s],false);
              }
          }
          if(!sync_continuously) --sync_attempts_to_go;
      }

      return grabbed_any;
  }

}

std::vector<VideoInterface*>& VideoJoiner::InputStreams()
{
    return src;
}

}
