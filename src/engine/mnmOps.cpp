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

#include <algorithm>
#include <array>
#include <map>
#include <vector>
#include "engine.h"
#include "../ta-log.h"
#include "../utfutils.h"
#include "song.h"

#define OFFSET(x) ((x)>0?(x)-1:(x))
#define PARU8(x)  (unsigned char)((x)&0xff)
#define PARU16(x) (unsigned char)((x)&0xff),(unsigned char)(((x)>>8)&0xff)
#define PARU32(x) (unsigned char)((x)&0xff),(unsigned char)(((x)>>8)&0xff),\
                  (unsigned char)(((x)>>16)&0xff),(unsigned char)(((x)>>24)&0xff)

typedef std::vector<std::vector<unsigned char>> MNMCmds;
struct MNMTick {
  size_t position=0;
  MNMCmds commands=MNMCmds(); 
};
struct MNMLast {
  int pitch=0;
  int volL=0;
  int volR=0;
  int sample=0;
  int echo=0;
  size_t startPointer=0;
  size_t loopPointer=0;
  bool forcePitch=false;
  bool forceVol=false;
};
struct MNMNew {
  int pitch=0;
  int volL=0;
  int volR=0;
  int sample=0;
  int sampleOff=0;
  int echo=0;
  bool hasPitch=false;
  bool hasVolL=false;
  bool hasVolR=false;
  bool hasSample=false;
  bool hasSampleOff=false;
  bool hasEcho=false;
};

static unsigned char getCmdRange(unsigned char cmd) {
  if      (cmd>=0xc0) return 0xff;
  else if (cmd>=0x80) return 0xbf;
  else if (cmd>=0x40) return 0x7f;
  else if (cmd>=0x30) return 0x3f;
  else if (cmd>=0x20) return 0x2f;
  else if (cmd>=0x10) return 0x1f;
  return cmd;
}

static bool writeWait(SafeWriter* w, int* lastWait, int newWait, bool force) {
  if (!force && newWait==*lastWait) return false;
  bool written=false;
  while (newWait>0) {
    int val=MIN(newWait,64);
    w->writeC(0xbf+val);
    newWait-=val;
    *lastWait=val;
    written=true;
  }
  return written;
}

SafeWriter* DivEngine::saveMNM(int type, const bool* sysToExport, bool loop, bool patternHints) {
  stop();
  repeatPattern=false;
  setOrder(0);
  BUSY_BEGIN_SOFT;

  SafeWriter* w=new SafeWriter;
  w->init();
  bool savePattern=(type!=2);
  bool saveSamples=(type!=1 && type!=5);
  if (savePattern && !saveMNMPattern(w, sysToExport, loop, patternHints)) {
    return NULL;
  }
  if (saveSamples && !saveMNS(w)) {
    return NULL;
  }

  BUSY_END;
  return w;
}

bool DivEngine::saveMNMPattern(SafeWriter* w, const bool* sysToExport, bool loop, bool patternHints) {
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
  int loopPos=-1;
  int loopTickSong=-1;
  int songTick=0;

  std::map<int, MNMTick> allCmds[16];
  MNMLast last[16];

  static const unsigned char MNM_IDENT[]={
    0xd1,0x4d,0x69,0x6e,0x4d,0x6f,0x64,0x4d,  // identifier
    0x01,0x00,                                // version
  };

  // Get the first MinMod system for now
  // TODO support other system types
  int sysIdx=-1;
  for (int i=0; i<song.systemLen; i++) {
    if (sysToExport!=NULL && !sysToExport[i]) {
      continue;
    }
    if (song.system[i]==DIV_SYSTEM_GBA_MINMOD) {
      sysIdx=i;
      break;
    }
  }
  if (sysIdx<0) {
    lastError="Only GBA MinMod system is supported for now";
    return NULL;
  }
  DivDispatch* d=disCont[sysIdx].dispatch;
  int chanCnt=song.systemFlags[sysIdx].getInt("channels",16);
  d->toggleRegisterDump(true);

  // write header
  w->write(MNM_IDENT,10);
  w->writeC(chanCnt);
  w->writeC(0); // reserved
  w->writeI(0); // file size. will be written later
  w->writeI(0); // begin length. will be written later
  w->writeI(0); // loop length. will be written later
  w->writeI(0); // tick rate. TODO support non-vblank rates
  w->writeI(0); // reserved

  // channel pointers. will be written later
  for (int i=0; i<chanCnt; i++) {
    w->writeI(0);
    w->writeI(0);
  }

  // write song data
  playSub(false);
  bool writeLoop=false;
  bool alreadyWroteLoop=false;
  int ord=-1;
  while (!done) {
    MNMCmds ch0Cmds;
    if (loopPos==-1 && loopOrder==curOrder && loopRow==curRow
      && (ticks-((tempoAccum+virtualTempoN)/virtualTempoD))<=0
    ) {
      writeLoop=true;
      // invalidate last register state so it always force an absolute write after loop
      for (int i=0; i<chanCnt; i++) {
        last[i].forcePitch=true;
        last[i].forceVol=true;
        last[i].sample=-1;
        last[i].echo=-1;
      }
    }
    if (nextTick(false,true) || !playing) {
      done=true;
      if (!loop) {
        for (int i=0; i<song.systemLen; i++) {
          disCont[i].dispatch->getRegisterWrites().clear();
        }
        break;
      }
      if (!playing) {
        writeLoop=false;
      }
    } else {
      // check for pattern change
      if (prevOrder!=ord) {
        logI("registering order change %d on %d",prevOrder, prevRow);
        ord=prevOrder;
        if (patternHints) {
          ch0Cmds.push_back({0x03, (unsigned char)prevRow, (unsigned char)prevOrder, 0x00, 0xfe});
        }
      }
    }
    // get register dumps
    std::vector<DivRegWrite>& writes=d->getRegisterWrites();
    MNMNew newVals[16];
    for (const DivRegWrite& j: writes) {
      if (j.addr>>16!=0xfffe) continue;
      int ch=(j.addr>>8)&0xff;
      if (ch>=chanCnt) continue;
      switch (j.addr&0xff) {
        case 0: newVals[ch].hasPitch    =true; newVals[ch].pitch    =j.val; break;
        case 1: newVals[ch].hasEcho     =true; newVals[ch].echo     =j.val; break;
        case 2: newVals[ch].hasVolL     =true; newVals[ch].volL     =j.val; break;
        case 3: newVals[ch].hasVolR     =true; newVals[ch].volR     =j.val; break;
        case 4: newVals[ch].hasSample   =true; newVals[ch].sample   =j.val; break;
        case 5: newVals[ch].hasSampleOff=true; newVals[ch].sampleOff=j.val; break;
        default: break;
      }
      writeCount++;
    }
    writes.clear();
    for (int i=0; i<chanCnt; i++) {
      MNMCmds cmds=i==0?ch0Cmds:MNMCmds();
      MNMNew* newValsI=&newVals[i];
      MNMLast* lastI=&last[i];
      if (newValsI->hasPitch && (newValsI->pitch!=lastI->pitch || lastI->forcePitch)) {
        int val=newValsI->pitch;
        int dt=val-lastI->pitch;
        int dtl=dt&0xff;
        int dth=(dt>>8)&0xff;
        if ((dt>=0 && dtl>0x80) || (dt<0 && dtl>=0x80)) {
          dtl-=0x100;
          dth++;
        }
        if (dth>=0x80) {
          dth-=0x100;
        }
        if (abs(dtl)<=0x20 && abs(dth)<=0x20 && !lastI->forcePitch) {
          if(dtl!=0) cmds.push_back({PARU8(0x60+OFFSET(dtl)),});
          if(dth!=0) cmds.push_back({PARU8(0xa0+OFFSET(dtl)),});
        } else if ((dtl!=0 && dth!=0) || lastI->forcePitch) {
          cmds.push_back({0x07,PARU16(val)});
        } else if (dth==0) {
          cmds.push_back({0x0c,PARU8(0x80+OFFSET(dtl))});
        } else {
          cmds.push_back({0x0d,PARU8(0x80+OFFSET(dth))});
        }
        lastI->pitch=val;
        lastI->forcePitch=false;
      }
      if (newValsI->hasEcho && newValsI->echo!=lastI->echo) {
          cmds.push_back({0x0a,PARU8(newValsI->echo)});
          lastI->echo=newValsI->echo;
      }
      if (newValsI->hasVolL || newValsI->hasVolR) {
        if (!newValsI->hasVolL) newValsI->volL=lastI->volL;
        if (!newValsI->hasVolR) newValsI->volR=lastI->volR;
        int dtl=newValsI->volL-lastI->volL;
        int dtr=newValsI->volR-lastI->volR;
        if (dtl!=0 || dtr!=0 || lastI->forceVol) {
          if (newValsI->volL==0 && newValsI->volR==0) {
            cmds.push_back({0x0e,});
          } else {
            if (abs(dtl)<=0x80 && abs(dtr)<=0x80 && !lastI->forceVol) {
              if (dtl==dtr) {
                if (abs(dtl)<=8) cmds.push_back({PARU8(0x38+OFFSET(dtl)),});
                else cmds.push_back({0x0b,PARU8(OFFSET(dtl))});
              } else {
                if (abs(dtl)<=8 && abs(dtr)<=8) {
                  if (dtl!=0) cmds.push_back({PARU8(0x18+OFFSET(dtl)),});
                  if (dtr!=0) cmds.push_back({PARU8(0x28+OFFSET(dtr)),});
                }
                else cmds.push_back({0x08,PARU8(OFFSET(dtl)),PARU8(OFFSET(dtr))});
              }
            } else {
              cmds.push_back({0x06,PARU16(newValsI->volL),PARU16(newValsI->volR)});
            }
          }
          lastI->volL=newValsI->volL;
          lastI->volR=newValsI->volR;
          lastI->forceVol=false;
        }
      }
      if (newValsI->hasSample) {
        if (newValsI->sample==lastI->sample) {
          if (!newValsI->hasSampleOff) {
            cmds.push_back({0x0f,});
          }
        } else {
          cmds.push_back({0x09,PARU16(newValsI->sample)});
        }
        lastI->sample=newValsI->sample;
      }
      if (newValsI->hasSampleOff) {
        cmds.push_back({0x05,PARU32(newValsI->sampleOff)});
      }
      if (cmds.size()>0) {
        std::sort(cmds.begin(),cmds.end(),[](auto x,auto y){
          return x[0]<y[0];
        });
        MNMTick t;
        t.commands=cmds;
        allCmds[i][songTick]=t;
      }
    }
    if (writeLoop && !alreadyWroteLoop) {
      writeLoop=false;
      alreadyWroteLoop=true;
      loopTickSong=songTick;
    }
    songTick++;
  }
  // end of song
  d->toggleRegisterDump(false);
  // TODO compression
  for (int i=0; i<chanCnt; i++) {
    unsigned char lastRange=0;
    int lastTick=0;
    int lastWait=0;
    bool looped=false;
    last[i].startPointer=w->tell();
    for (auto& kv: allCmds[i]) {
      // dump wait for last command
      bool forceWait=getCmdRange(kv.second.commands[0][0])>lastRange;
      if (!looped && loopTickSong>=0 && kv.first>=loopTickSong) {
        if (writeWait(w,&lastWait,loopTickSong-lastTick,forceWait)) lastRange=0xff;
        last[i].loopPointer=w->tell();
        lastTick=loopTickSong;
        forceWait=true;
        looped=true;
      }
      if (writeWait(w,&lastWait,kv.first-lastTick,forceWait)) lastRange=0xff;
      kv.second.position=w->tell();
      unsigned char newRange=lastRange;
      for (const auto& cmd: kv.second.commands) {
        w->write(cmd.data(),cmd.size());
        newRange=cmd[0];
      }
      lastRange=getCmdRange(newRange);
      lastTick=kv.first;
    }
    writeWait(w,&lastWait,songTick-lastTick,true);
    w->writeC(0xff); // bogus wait command to execute the last wait
  }

  // finish file
  if(loopTickSong<0) loopTickSong=0;
  size_t end=w->tell();
  w->seek(0x0c,SEEK_SET);
  w->writeI(end);
  w->writeI(loopTickSong);
  w->writeI(songTick-loopTickSong);
  w->seek(0x20,SEEK_SET);
  for (int i=0; i<chanCnt; i++) {
    w->writeI(last[i].startPointer);
    w->writeI(last[i].loopPointer);
  }

  remainingLoops=-1;
  playing=false;
  freelance=false;
  extValuePresent=false;

  logI("%d register writes total.",writeCount);

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
    const DivSample* s=song.sample[i];
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
    const DivSample* s=song.sample[i];
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
