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
#include <map>
#include <set>
#include <vector>
#include "engine.h"
#include "waveSynth.h"
#include "../ta-log.h"

struct DevSoundNew {
  int note=-1;
  int vol=-1;
  int ins=-1;
  int slide=-1;
  int sampleOffset=-1;
  short speed1=-1;
  short speed2=-1;
};
  int sampleOffset=-1;
struct DevSoundLast {
  int pitch=-1;
  int ins=-1;
  int vol=15;
  int slide=0;
  int tick=0;
  bool forcePitch=true;
};
struct DevSoundCmd {
  short keyOn=-1;       // 0
  short vol=-1;         // 1
  short pitchChange=-1; // 1
  short ins=-1;         // 1
  short wait=-1;        // 1
  int pitchSet=-1;      // 2
  int sampleOffset=-1;  // 2
  int call=-1;          // 4
  short slide=-1;
  short speed1=-1;
  short speed2=-1;
};

static unsigned char gbVolMap[16]={
  0x00, 0x00, 0x00, 0x00,
  0x60, 0x60, 0x60, 0x60,
  0x40, 0x40, 0x40, 0x40,
  0x20, 0x20, 0x20, 0x20
};

static bool writeMacroLabel(SafeWriter* w, std::vector<DivInstrumentMacro>& tables, const DivInstrumentMacro* macro,
  String* label, String* labelR, const char* baseLabel, bool isWaveChannel
) {
  if (macro->len==0) return false;
  // find in the tables for duplicates
  auto found=std::find_if(tables.begin(),tables.end(),[macro,isWaveChannel](const auto v){
    bool res=v.len==macro->len && v.loop==macro->loop && v.rel==macro->rel && v.lenMemory==(isWaveChannel?1:0);
    res = res && ((v.macroType==DIV_MACRO_PITCH)==(macro->macroType==DIV_MACRO_PITCH));
    return res && memcmp(v.val,macro->val,macro->len*sizeof(macro->val[0])) == 0;
  });
  int idx=std::distance(tables.begin(),found);
  *label=fmt::format("{}_T{}",baseLabel,idx);
  if (macro->rel<macro->len) {
    *labelR=*label+"R";
  }
  if (idx==tables.size()) tables.push_back(*macro);
  else return false;
  tables[idx].lenMemory=(isWaveChannel?1:0);
  w->writeText(label->c_str());
  w->writeC(':');
  return true;
}

static void writeMacro(SafeWriter* w, const DivInstrumentMacro* macro, const char* labelR, bool isWaveChannel) {
  int lastVal=macro->val[0];
  int lastValCnt=macro->delay;
  bool hadLoop=false;
  if (macro->loop!=0) w->writeText("\n    db ");
  auto writeMacroVal=[w,macro,&lastVal,&lastValCnt,isWaveChannel](int i,bool end){
    if (macro->val[i]!=lastVal || end) {
      if (lastValCnt>0) {
        unsigned char val=lastVal&0xff;
        if (isWaveChannel && macro->macroType==DIV_MACRO_VOL) val=gbVolMap[val];
        w->writeText(fmt::format("{}",val));
        if (lastValCnt==2) w->writeText(fmt::format(",{}",val));
        else if (lastValCnt>2) {
          while (lastValCnt>0) {
            int cnt=MIN(lastValCnt,255);
            if (cnt==1) w->writeText(fmt::format(",{}",val));
            else w->writeText(fmt::format(",seq_wait,{}",cnt-2));
            lastValCnt-=cnt;
          }
        }
        if (!end) w->writeC(',');
      }
      lastVal=macro->val[i];
      if (macro->macroType==DIV_MACRO_ARP && (lastVal&0x40000000)!=0) lastVal|=0x80;
      lastValCnt=end?0:macro->speed;
    } else {
      lastValCnt+=macro->speed;
    }
  };
  for (int i=0; i<macro->len; i++) {
    bool nl=false;
    if (macro->rel==i) {
      if (hadLoop) {
        writeMacroVal(i,true);
      } else {
        writeMacroVal(i,true);
        w->writeText("\n:\n    db ");
        writeMacroVal(i,true);
      }
      w->writeText(fmt::format("\n    db seq_loop,(:- -@)-1\n{}:",labelR));
      hadLoop=false;
      nl=true;
    }
    if (macro->loop==i) {
      writeMacroVal(i,true);
      w->writeText("\n:");
      hadLoop=true;
      nl=true;
    }
    if (nl) w->writeText("\n    db ");
    writeMacroVal(i,false);
  }
  writeMacroVal(0,true);
  if (hadLoop) w->writeText("\n    db seq_loop,(:- -@)-1\n");
  else w->writeText(",seq_end\n");
}

static void writePitchMacro(SafeWriter* w, const DivInstrumentMacro* macro, const char* labelR, bool isWaveChannel) {
  bool hadLoop=false;
  // Furnace's delay is incompatible with DevSound's delay as DevSound won't
  // apply the first value before delay
  // So we have to unroll the delay if the first macro value isn't 0
  if (macro->delay>0 && macro->val[0]!=0) {
    w->writeText("\n    db 0");
    for (int i=0; i<macro->delay; i++) w->writeText(fmt::format(",{}",macro->val[0]&0xff));
  } else w->writeText(fmt::format("\n    db {}", macro->delay));
  for (int i=0; i<macro->len; i++) {
    bool nl=false;
    unsigned char val=macro->val[i]&0xff;
    if (macro->rel==i) {
      if (!hadLoop) {
        w->writeText(fmt::format("\n:\n    db {}",val));
      }
      w->writeText(fmt::format("\n    db pitch_loop,(:- -@)-1\n{}:",labelR));
      hadLoop=false;
      nl=true;
    }
    if (macro->loop==i) {
      w->writeText("\n:");
      hadLoop=true;
      nl=true;
    }
    if (nl) w->writeText("\n    db ");
    else w->writeC(',');
    w->writeText(fmt::format("{}",val));
    for (int j=1; j<macro->speed; j++) {
      w->writeText(fmt::format(",{}",val));
    }
  }
  if (hadLoop) w->writeText("\n    db pitch_loop,(:- -@)-1\n");
  else w->writeText(",pitch_end\n");
}

static void writeInstrument(SafeWriter* w, SafeWriter* wIns, std::vector<DivInstrumentMacro>& tables,
  DivInstrument* ins, int idx, const char* baseLabel, bool isWaveChannel
) {
  if (ins->type!=DIV_INS_GB) return;
  String volLabel="DSX_DummyTable";
  String arpLabel="DSX_DummyTable";
  String waveLabel="DSX_DummyTable";
  String pitchLabel="DSX_DummyPitch";
  String volRLabel="0";
  String arpRLabel="0";
  String waveRLabel="0";
  String pitchRLabel="0";
  DivInstrumentMacro altVolMacro=DivInstrumentMacro(DIV_MACRO_VOL);
  altVolMacro.val[0]=ins->gb.envVol;
  altVolMacro.len=1;
  DivInstrumentMacro* volMacro=ins->std.volMacro.len>0?&ins->std.volMacro:&altVolMacro;
  DivInstrumentMacro* waveMacro=isWaveChannel?&ins->std.waveMacro:&ins->std.dutyMacro;
  if (writeMacroLabel(wIns,tables,volMacro,&volLabel,&volRLabel,baseLabel,isWaveChannel)) {
    writeMacro(wIns,volMacro,volRLabel.c_str(),isWaveChannel);
  }
  if (writeMacroLabel(wIns,tables,&ins->std.arpMacro,&arpLabel,&arpRLabel,baseLabel,isWaveChannel)) {
    writeMacro(wIns,&ins->std.arpMacro,arpRLabel.c_str(),isWaveChannel);
  }
  if (writeMacroLabel(wIns,tables,waveMacro,&waveLabel,&waveRLabel,baseLabel,isWaveChannel)) {
    writeMacro(wIns,waveMacro,waveRLabel.c_str(),isWaveChannel);
  }
  if (writeMacroLabel(wIns,tables,&ins->std.pitchMacro,&pitchLabel,&pitchRLabel,baseLabel,isWaveChannel)) {
    writePitchMacro(wIns,&ins->std.pitchMacro,pitchRLabel.c_str(),isWaveChannel);
  }
  // ins header
  w->writeText(fmt::format("{}_I{}{}: ; {}\n",baseLabel,isWaveChannel?"W":"",idx,ins->name));
  w->writeText(fmt::format(
    "    dw {},{},{},{}\n    dw {},{},{},{}\n",
    volLabel,arpLabel,waveLabel,pitchLabel,volRLabel,arpRLabel,waveRLabel,pitchRLabel
  ));
}

static const char* NOTE_NAMES[]={"C_","C#","D_","D#","E_","F_","F#","G_","G#","A_","A#","B_"};

static void writePSGCmd(SafeWriter* w, DevSoundCmd* cmd, int rows, const char* baseLabel, bool isWaveChannel) {
  while (rows>0) {
    int val=MIN(rows,256);
    if (cmd->speed1>=0) w->writeText(fmt::format("    sound_set_speed {},{}\n",cmd->speed1&0xff,cmd->speed2&0xff));
    if (cmd->ins>=0) w->writeText(fmt::format("    sound_instrument {}_I{}{}\n",baseLabel,isWaveChannel?"W":"",cmd->ins));
    if (cmd->vol>=0) w->writeText(fmt::format("    sound_volume {}\n",cmd->vol));
    if (cmd->slide>=0) {
      switch (cmd->slide>>8) {
        case 1: w->writeText("    sound_slide_up "); break;
        case 2: w->writeText("    sound_slide_down "); break;
        default: w->writeText("    sound_portamento "); break;
      }
      w->writeText(fmt::format("{}\n",cmd->slide&0xff));
    }
    if (rows>0) {
      if (cmd->pitchSet<0) w->writeText("    wait ");
      else if (cmd->pitchSet==(3<<28)) w->writeText("    rest ");
      else if (cmd->pitchSet==(2<<28)) w->writeText("    release ");
      else {
        int oct=cmd->pitchSet/12;
        int note=cmd->pitchSet%12;
        w->writeText(fmt::format("    note {},{},",NOTE_NAMES[note],oct));
      }
      w->writeText(fmt::format("{}\n",val&0xff));
    }
    *cmd=DevSoundCmd();
    rows-=val;
  }
}

static void writeSampCmd(SafeWriter* w, DevSoundCmd* cmd, int* lastWait, int newWait) {
  while (newWait>0) {
    int val=MIN(newWait,256);
    if (*lastWait!=val) {
      cmd->wait=newWait;
      *lastWait=val;
    }
    int nlen=1;
    unsigned char nbuf[9];
    nbuf[0]=0;
    if (cmd->sampleOffset>=0) {
      nbuf[0]|=(1<<6);
      nbuf[nlen++]=cmd->sampleOffset&0xff;
      nbuf[nlen++]=(cmd->sampleOffset>>8)&0xff;
    }
    if (cmd->pitchSet>=0) {
      nbuf[0]|=(1<<5);
      nbuf[nlen++]=cmd->pitchSet&0xff;
      nbuf[nlen++]=(cmd->pitchSet>>8)&0xff;
    }
    if (cmd->wait>=0) {
      nbuf[0]|=(1<<4);
      nbuf[nlen++]=cmd->wait&0xff;
    }
    if (cmd->ins>=0) {
      nbuf[0]|=(1<<3);
      nbuf[nlen++]=cmd->ins&0xff;
    }
    if (cmd->pitchChange>=0) {
      nbuf[0]|=(1<<2);
      nbuf[nlen++]=cmd->pitchChange&0xff;
    }
    if (cmd->vol>=0) {
      nbuf[0]|=(1<<1);
      nbuf[nlen++]=cmd->vol&0xff;
    }
    nbuf[0]|=(cmd->keyOn>0)?1:0;
    w->writeText(fmt::format("    db {}",nbuf[0]));
    for (int i=1; i<nlen; i++) {
      w->writeText(fmt::format(",{}",nbuf[i]));
    }
    w->writeC('\n');
    *cmd=DevSoundCmd();
    newWait-=val;
  }
}

static void writeHexs(SafeWriter* w, unsigned char* data, size_t length) {
  if (length==0) return;
  for (int i=0; i<length; i++) {
    if ((i&15)==0) {
      if (i!=0) w->writeC('\n');
      w->writeText(fmt::format("    db ${:02x}",data[i]));
    } else {
      w->writeText(fmt::format(",${:02x}",data[i]));
    }
  }
  w->writeC('\n');
}

SafeWriter* DivEngine::saveDevSound(const bool* sysToExport, const char* baseLabel) {
  stop();
  repeatPattern=false;
  shallStop=false;
  setOrder(0);
  BUSY_BEGIN_SOFT;
  // determine loop point
  bool stopped=false;
  int loopOrder=0;
  int loopOrderRow=0;
  int loopEnd=0;
  walkSong(loopOrder,loopOrderRow,loopEnd);
  logI("loop point: %d %d",loopOrder,loopOrderRow);

  SafeWriter* w=new SafeWriter;
  w->init();

  int gbIdx=-1;
  int gbCh=0;
  int gdacCnt=0;
  int gdacIdx[3];
  unsigned char numFXCols[4];

  for (int i=0; i<song.systemLen; i++) {
    if (sysToExport!=NULL && !sysToExport[i]) continue;
    if (song.system[i]==DIV_SYSTEM_GB) {
      gbIdx=i;
      disCont[i].dispatch->toggleRegisterDump(true);
    }
    if (gdacCnt<3 && song.system[i]==DIV_SYSTEM_PCM_DAC) {
      gdacIdx[gdacCnt++]=i;
      disCont[i].dispatch->toggleRegisterDump(true);
    }
  }
  if (gbIdx>=0) {
    while (dispatchOfChan[gbCh]!=gbIdx) gbCh++;
    for (int i=0; i<4; i++) {
      numFXCols[i]=curSubSong->pat[i+gbCh].effectCols;
    }
  }
  unsigned char speed1=curSubSong->speeds.val[0];
  unsigned char speed2=curSubSong->speeds.len>1?curSubSong->speeds.val[1]:speed1;

  // write patterns
  bool writeLoop=false;
  bool done=false;
  playSub(false);
  
  int tick=0;
  int row=0;
  int loopTick=-1;
  int loopRow=-1;
  int lastEngineTicks=-1;
  unsigned char lastSpeed1=speed1;
  unsigned char lastSpeed2=speed2;
  double curDivider=divider;
  bool s4Active=false;
  DevSoundLast last[8];
  DevSoundNew news[8];
  std::vector<int> s4Map;
  std::set<int> pulseInsMap;
  std::set<int> waveInsMap;
  std::map<int,DevSoundCmd> allCmds[8];
  while (!done) {
    if (loopTick<0 && loopOrder==curOrder && loopOrderRow==curRow
      && (ticks-((tempoAccum+virtualTempoN)/virtualTempoD))<=0
    ) {
      writeLoop=true;
      loopTick=tick;
      loopRow=row;
      // invalidate last register state so it always force an absolute write after loop
      for (int i=0; i<8; i++) {
        last[i]=DevSoundLast();
        last[i].vol=-1;
        last[i].slide=-1;
        last[i].tick=-1;
      }
    }
    int prevOrd=curOrder;
    int prevRow=curRow;
    if (nextTick(false,true) || !playing) {
      stopped=!playing;
      done=true;
      break;
    }
    for (int i=4; i<8; i++) news[i]=DevSoundNew(); // PCM clears every tick
    if (gbIdx>=0) {
      // get pcm trigger dumps
      std::vector<DivRegWrite>& writes=disCont[gbIdx].dispatch->getRegisterWrites();
      for (const DivRegWrite& i: writes) {
        switch (i.addr) {
          case 0xfffe0200:
            news[4].ins=i.val;
            s4Active=i.val>0;
            break;
          case 0xfffe0201:
            news[4].vol=i.val;
            break;
          case 0xfffe0202:
            news[4].sampleOffset=i.val;
            break;
          default: break;
        }
      }
      writes.clear();
      // collect pcm changes
      DevSoundCmd cmds;
      bool hasCmd=false;
      if (news[4].ins>=0) {
        int idx=0;
        cmds.keyOn=news[4].ins>0;
        if (cmds.keyOn) {
          auto found=std::find(s4Map.begin(),s4Map.end(),news[4].ins-1);
          idx=std::distance(s4Map.begin(),found);
          if (idx==s4Map.size()) s4Map.push_back(news[4].ins-1);
          if (idx!=last[4].ins) cmds.ins=idx;
          last[4].ins=idx;
        } else {
          cmds.ins=idx;
        }
        hasCmd=true;
      }
      if (news[4].vol>=0 && news[4].vol!=last[4].vol) {
        cmds.vol=news[4].vol;
        last[4].vol=news[4].vol;
        hasCmd=true;
      }
      if (news[4].sampleOffset>=0) {
        cmds.sampleOffset=news[4].sampleOffset;
        hasCmd=true;
      }
      if (hasCmd) allCmds[4][tick]=cmds;
    }
    // check if new row and read pattern
    if (gbIdx>=0 && lastEngineTicks<=ticks) {
      for (int i=0; i<4; i++) {
        if (i==2 && s4Active) continue;
        int ch=i+gbCh;
        DivPattern* pat=curSubSong->pat[ch].data[curSubSong->orders.ord[ch][prevOrd]];
        if (pat==NULL) continue;
        short* patRow=pat->data[prevRow];
        if (patRow[0]==100) news[i].note=3<<28; // note off
        else if (patRow[0]==101 || patRow[0]==102) news[i].note=2<<28; // note release
        else if (patRow[0]!=0 || patRow[1]!=0) news[i].note=patRow[0]+patRow[1]*12;
        if (patRow[2]>=0) news[i].ins=patRow[2];
        if (patRow[3]>=0) news[i].vol=patRow[3];
        for (int j=0; j<numFXCols[i]; j++) {
          short fx=patRow[j*2+4];
          short fxVal=patRow[j*2+5];
          if (fxVal==-1) fxVal=0;
          fxVal&=255;
          switch (fx) {
            case 0x01: case 0x02: case 0x03:
              news[i].slide=fxVal|(fx<<8);
              break;
            // 0x09 and 0x0f are handled separately due to complex groove handling
            default: break;
          }
        }
        // detect speed changes
        if (i==0) {
          if (speeds.val[0]!=lastSpeed1) {
            lastSpeed1=speeds.val[0];
            if (speeds.len<2) lastSpeed2=speeds.val[0];
            news[0].speed1=lastSpeed1;
            news[0].speed2=lastSpeed2;
          }
          if (speeds.len>=2 && speeds.val[1]!=lastSpeed2) {
            news[0].speed2=lastSpeed2;
            lastSpeed2=speeds.val[1];
            news[0].speed1=lastSpeed1;
            news[0].speed2=lastSpeed2;
          }
        }
        // write to commands list
        DevSoundCmd cmds;
        bool hasCmd=false;
        if (news[i].note>=0 && news[i].note!=last[i].pitch) {
          cmds.pitchSet=news[i].note;
          last[i].pitch=news[i].note;
          hasCmd=true;
        }
        if (news[i].ins>=0 && news[i].ins!=last[i].ins) {
          if (i==2) waveInsMap.insert(news[i].ins);
          else pulseInsMap.insert(news[i].ins);
          cmds.ins=news[i].ins;
          last[i].ins=news[i].ins;
          hasCmd=true;
        }
        if (news[i].vol>=0 && news[i].vol!=last[i].vol) {
          cmds.vol=news[i].vol;
          last[i].vol=news[i].vol;
          hasCmd=true;
        }
        if (news[i].slide>=0 && (((news[i].slide&0xff)!=0 || (last[i].slide&0xff)!=0) && news[i].slide!=last[i].slide)) {
          cmds.slide=news[i].slide;
          last[i].slide=news[i].slide;
          hasCmd=true;
        }
        if (news[i].speed1>=0) {
          cmds.speed1=news[i].speed1;
          cmds.speed2=news[i].speed2;
          hasCmd=true;
        }
        if (hasCmd) allCmds[i][row]=cmds;
        news[i]=DevSoundNew();
      }
      row++;
    }
    lastEngineTicks=ticks;
    // get gdac dumps TODO
    cmdStream.clear();
    tick++;
  }
  for (int i=0; i<song.systemLen; i++) {
    disCont[i].dispatch->getRegisterWrites().clear();
    disCont[i].dispatch->toggleRegisterDump(false);
  }
  // compress commands TODO

  // write commands
  w->writeText(fmt::format(
    "; Generated by Furnace " DIV_VERSION "\n"
    "; Name:   {}\n"
    "; Author: {}\n"
    "; Album:  {}\n"
    "; Subsong #{}: {}\n\n"
    "{}:\n"
    "    db {},{}\n",
    song.name,song.author,song.category,curSubSongIndex+1,curSubSong->name,
    baseLabel,speed1,speed2
  ));
  for (int i=0; i<4; i++) {
    if (allCmds[i].empty()) w->writeText("    dw DSX_DummyChannel\n");
    else w->writeText(fmt::format("    dw {}_CH{}\n",baseLabel,i));
  }
  // write PSG channels
  for (int i=0; i<4; i++) {
    if (allCmds[i].empty()) continue;
    DevSoundCmd lastCmd;
    int lastRow=0;
    bool looped=false;
    w->writeText(fmt::format("\n{}_CH{}:\n",baseLabel,i));
    for (auto& kv: allCmds[i]) {
      if (!looped && !stopped && loopRow>=0 && kv.first>=loopRow) {
        writePSGCmd(w,&lastCmd,loopRow-lastRow,baseLabel,i==2);
        w->writeText(".loop\n");
        lastRow=loopRow;
        looped=true;
      }
      writePSGCmd(w,&lastCmd,kv.first-lastRow,baseLabel,i==2);
      lastRow=kv.first;
      lastCmd=kv.second;
    }
    writePSGCmd(w,&lastCmd,row-lastRow,baseLabel,i==2);
    w->writeText((stopped || loopRow<0)?"    rest 1\n    sound_end\n":"    sound_jump .loop\n");
  }

  // write instruments
  std::vector<DivInstrumentMacro> tables;
  SafeWriter wIns;
  wIns.init();
  w->writeC('\n');
  for (int i: pulseInsMap) writeInstrument(w,&wIns,tables,getIns(i),i,baseLabel,false);
  for (int i: waveInsMap) writeInstrument(w,&wIns,tables,getIns(i),i,baseLabel,true);
  w->writeC('\n');
  w->write(wIns.getFinalBuf(),wIns.tell());
  w->writeC('\n');
  wIns.finish();

  // write wavetables
  w->writeText(fmt::format("{}_Waves:\n",baseLabel));
  for (int i=0; i<song.waveLen; i++) {
    DivWavetable* wt=song.wave[i];
    w->writeText("    db ");
    for (int j=0; j<32; j+=2) {
      w->writeText(fmt::format("${:x}{:x}",wt->data[j*wt->len/32]&0xf,wt->data[(j+1)*wt->len/32]&0xf));
      if (j<30) w->writeC(',');
    }
    w->writeC('\n');
  }

  // write sample headers
  w->writeText("\nPUSHS\n");
  static const char* CH_NAMES[] = {"4A","8A","8B","8C"};
  if (!allCmds[4].empty() || gdacCnt>0) {
    w->writeText(fmt::format("\nSECTION \"{} Sample Headers\",ROMX\n",baseLabel));
  }
  if (!allCmds[4].empty()) {
    String lbl=fmt::format("{}_CH4A",baseLabel);
    w->writeText(fmt::format("{0}_CH4:\n    dw BANK({1}),{1},{1}.end,{1}.loop\n",baseLabel,lbl));
    w->writeText(fmt::format("{}_S4:\n",baseLabel));
    for (int i=0; i<s4Map.size(); i++) {
      DivSample* s=getSample(s4Map[i]);
      String lbl=fmt::format("{}_S4_{}",baseLabel,i);
      w->writeText(fmt::format("    dw BANK({0}),{0},{0}.end,{0}.loop ; {1}\n",lbl,s->name));
    }
  }
  if (gdacCnt>0) {
    w->writeText(fmt::format("{}_CH8:\n",baseLabel));
    for (int i=5; i<8; i++) {
      if (allCmds[i].empty()) {
        w->writeText(fmt::format("    dw 0,0,0,0\n",baseLabel,CH_NAMES[i-4]));
      } else {
        String lbl=fmt::format("{}_CH{}",baseLabel,CH_NAMES[i-4]);
        w->writeText(fmt::format("    dw BANK({0}),{0},{0}.end,{0}.loop\n",lbl));
      }
    }
  }
  // write sample channels
  for (int i=4; i<8; i++) {
    if (allCmds[i].empty()) continue;
    DevSoundCmd lastCmd;
    int lastTick=0;
    int lastWait=0;
    bool looped=false;
    w->writeText(fmt::format("\nSECTION \"{0} CH{1} Data\",ROMX\n{0}_CH{1}:\n",baseLabel,CH_NAMES[i-4]));
    for (auto& kv: allCmds[i]) {
      if (!looped && !stopped && loopTick>=0 && kv.first>=loopTick) {
        writeSampCmd(w,&lastCmd,&lastWait,loopTick-lastTick);
        w->writeText(".loop\n");
        lastTick=loopTick;
        looped=true;
      }
      writeSampCmd(w,&lastCmd,&lastWait,kv.first-lastTick);
      lastTick=kv.first;
      lastCmd=kv.second;
    }
    writeSampCmd(w,&lastCmd,&lastWait,tick-lastTick);
    if (stopped || loopTick<0) w->writeText(".loop\n    db 0\n");
    w->writeText(".end\n");
  }
  // write samples
  for (int i=0; i<s4Map.size(); i++) {
    DivSample* s=getSample(s4Map[i]);
    std::vector<unsigned char> buf;
    int len=s->loop?s->loopEnd:s->length8;
    for (int j=0; j<len/2; j++) {
      int nibble1=((unsigned char)s->data8[j*2]^0x80)>>4;
      int nibble2=((unsigned char)s->data8[j*2+1]^0x80)>>4;
      buf.push_back((nibble1<<4)|nibble2);
    }
    w->writeText(fmt::format("\nSECTION \"{0} 4-bit Sample {1}\",ROMX\n{0}_S4_{1}: ; {2}\n",baseLabel,i,s->name));
    if (s->loop) {
      int st=s->loopStart/2;
      writeHexs(w,buf.data(),st);
      w->writeText(".loop\n");
      writeHexs(w,&buf.data()[st],len/2-st);
    } else {
      writeHexs(w,buf.data(),len/2);
      w->writeText(".loop\n    ds 32,0\n");
    }
    w->writeText(".end\n");
  }
  w->writeText("\nPOPS\n");

  remainingLoops=-1;
  playing=false;
  freelance=false;
  extValuePresent=false;
  BUSY_END;

  return w;
}
