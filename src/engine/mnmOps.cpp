/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2024 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "engine.h"
#include "../ta-log.h"
#include "../utfutils.h"
#include "song.h"

SafeWriter* DivEngine::saveMNM(int type, bool* sysToExport, bool loop, bool patternHints, int trailingTicks) {
  stop();
  repeatPattern=false;
  setOrder(0);
  BUSY_BEGIN_SOFT;

  SafeWriter* w=new SafeWriter;
  w->init();
  bool savePattern=(type!=2);
  bool saveSamples=(type!=1 && type!=5);
  if (savePattern && !saveMNMPattern(w, sysToExport, loop, patternHints, trailingTicks)) {
    return NULL;
  }
  if (saveSamples && !saveMNS(w)) {
    return NULL;
  }

  BUSY_END;
  return w;
}

bool DivEngine::saveMNMPattern(SafeWriter* w, bool* sysToExport, bool loop, bool patternHints, int trailingTicks) {
/*
  double origRate=got.rate;
  got.rate=44100;
  // determine loop point
  int loopOrder=0;
  int loopRow=0;
  int loopEnd=0;
  walkSong(loopOrder,loopRow,loopEnd);
  logI("loop point: %d %d",loopOrder,loopRow);
  warnings="";

  curOrder=0;
  freelance=false;
  playing=false;
  extValuePresent=false;
  remainingLoops=-1;

  // play the song ourselves
  bool done=false;
  int writeCount=0;

  remainingLoops=-1;
  playing=false;
  freelance=false;
  extValuePresent=false;

  logI("%d register writes total.",writeCount);
*/
  return true;
}

bool DivEngine::saveMNS(SafeWriter* w) {
  static const unsigned char MNS_IDENT[10]={
    0xd1,0x4d,0x69,0x6e,0x4d,0x6f,0x64,0x53,  // identifier
    0x01,0x00                                 // version
  };
  static const unsigned char PAD[32]={
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
  };

  // write header
  w->write(MNS_IDENT,10);
  w->writeS(0); // reserved
  w->writeI(0); // file size. will be written later
  w->writeS(song.sampleLen);
  w->writeS(0); // wavetable count. TODO support wavetables
  w->writeI(0); // reserved
  w->writeI(0); // reserved
  w->writeI(0); // reserved

  // sample headers. will be written later
  for (int i=0; i<song.sampleLen; i++) {
    w->write(PAD,0x18);
  }

  // write sample data
  std::vector<size_t> startAddress;
  startAddress.resize(song.sampleLen);
  for (int i=0; i<song.sampleLen; i++) {
    DivSample* s=song.sample[i];
    bool pad=false;
    // align to multiple of 4 bytes for direct DMA
    w->write(PAD,(0-w->tell())&3);
    startAddress[i]=w->tell();
    if (s->loop) {
      w->write(s->data8, s->loopEnd);
    } else {
      // insert extra 32 bytes of looped silence
      w->write(s->data8, s->length8);
      w->write(PAD,32);
    }
  }
  size_t end=w->tell();
  w->seek(0x0c,SEEK_SET);
  w->writeI(end);

  // write sample headers
  w->seek(0x20,SEEK_SET);
  for (int i=0; i<song.sampleLen; i++) {
    DivSample* s=song.sample[i];
    w->writeI(s->centerRate>0?(log2(s->centerRate)*786432):0);
    w->writeI(startAddress[i]);
    if (s->loop) {
      w->writeI(s->loopStart);
      w->writeI(s->loopEnd);
    } else {
      w->writeI(s->length8);
      w->writeI(s->length8+32);
    }
    w->writeI(0);
    w->writeI(0);
  }

  return true;
}
