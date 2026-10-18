from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
from link_test_support import write_link_stubs

sketch = Path(__file__).resolve().parents[1] / 'motion_mp3.ino'
with tempfile.TemporaryDirectory(prefix='mp3-sim-') as directory:
    root = Path(directory)
    write_link_stubs(root)
    (root / 'Arduino.h').write_text(r'''
#pragma once
#include <cstdint>
#include <cassert>
#include <cstddef>
#include <sstream>
#include <deque>
using byte = uint8_t;
constexpr int LOW=0, HIGH=1, INPUT_PULLUP=2, INPUT_PULLDOWN=3, OUTPUT=4;
constexpr int D2=2, D3=3, D4=4, D5=5, D6=6, SERIAL_8N1=0;
inline unsigned long clockMs=0;
inline int pins[8]={0,0,HIGH,0,0,LOW,0,0};
inline unsigned long millis() { return clockMs; }
inline void delay(unsigned long ms) { clockMs += ms; }
inline int digitalRead(int pin) { return pins[pin]; }
inline void pinMode(int, int) {}
inline unsigned eyeOnWrites=0, eyeOffWrites=0;
inline void digitalWrite(int pin, int value) {
 assert(pin==D6); pins[pin]=value;
 if(value==LOW) ++eyeOnWrites; else ++eyeOffWrites;
}
inline byte highByte(unsigned int n) { return (n >> 8) & 255; }
inline byte lowByte(unsigned int n) { return n & 255; }
struct Console {
 std::ostringstream log;
 std::deque<char> incoming;
 void begin(int) {}
 int available() { return incoming.size(); }
 int read() { auto c=incoming.front(); incoming.pop_front(); return c; }
 template<class T> void print(T t) { log << t; }
 template<class T> void println(T t) { log << t << '\n'; }
 void println() { log << '\n'; }
};
inline Console Serial;
''')
    (root / 'HardwareSerial.h').write_text(r'''
#pragma once
#include "Arduino.h"
#include <vector>
#include <deque>
#include <set>
#include <map>
#include <cassert>
class HardwareSerial {
 public:
  unsigned int fileCount=5, selected=0;
  byte state=0;
  bool respond=true, needsPlay=false, filenameSupported=true, corruptFilename=false, longFilename=false;
  bool skipBadOnExplicitPlay=false, paddedFilename=false, indexSupported=true;
  bool ignoreSelect=false;
  unsigned long startedAt=0, duration=1000;
  std::set<unsigned int> bad;
  std::deque<byte> incoming;
  std::vector<unsigned int> requests, audible;
  std::vector<unsigned long> starts;
  std::vector<std::vector<byte>> frames;
  std::map<unsigned int,std::vector<byte>> filenames;
  explicit HardwareSerial(int) {}
  void begin(int,int,int,int) {}
  int available() { return incoming.size(); }
  int read() { if (incoming.empty()) return -1; auto b=incoming.front(); incoming.pop_front(); return b; }
  void reply(byte cmd, std::vector<byte> data) {
    if (!respond) return;
    std::vector<byte> f={0xAA,cmd,static_cast<byte>(data.size())};
    f.insert(f.end(), data.begin(), data.end());
    byte sum=0; for(auto b:f) sum+=b;
    f.push_back(sum); for(auto b:f) incoming.push_back(b);
  }
  void start() {
    if(selected && selected<=fileCount && !bad.count(selected)) {
      state=1; startedAt=millis(); audible.push_back(selected); starts.push_back(millis());
    }
  }
  size_t write(const byte* bytes, size_t n) {
    byte sum=0; for(size_t i=0;i<n-1;i++) sum+=bytes[i];
    assert(bytes[0]==0xAA && n==size_t(bytes[2])+4 && bytes[n-1]==sum);
    frames.emplace_back(bytes,bytes+n);
    switch(bytes[1]) {
      case 4: state=0; break;
      case 0x0C: reply(0x0C,{highByte(fileCount),lowByte(fileCount)}); break;
      case 0x09: reply(0x09,{2}); break;
      case 0x0A: reply(0x0A,{1}); break;
      case 0x0D: if(indexSupported) reply(0x0D,{highByte(selected),lowByte(selected)}); break;
      case 0x1F: if(!ignoreSelect) selected=(unsigned(bytes[3])<<8)|bytes[4]; break;
      case 0x1E:
        if(filenameSupported) {
          if(filenames.count(selected)) {
            reply(0x1E,filenames[selected]);
          } else if(corruptFilename) {
            incoming={0xAA,0x1E,255,0xAA,0x1E,1,'X',0};
          } else if(longFilename) {
            std::vector<byte> name(60,'A'); name[59]=0;
            reply(0x1E,name);
          } else {
            std::string number=std::to_string(selected);
            std::string name=std::string(5-number.size(),'0')+number+".MP3";
            if(paddedFilename) name=std::string(selected==1 ? "00001   MP3\0" : "BROKEN  MP3\0",12);
            reply(0x1E,std::vector<byte>(name.begin(),name.end()));
          }
        }
        break;
      case 1:
        if(state==1 && millis()-startedAt>=duration) state=0;
        reply(1,{state}); break;
      case 7:
        selected=(unsigned(bytes[3])<<8)|bytes[4]; requests.push_back(selected); state=0;
        if(!needsPlay) start(); break;
      case 2:
        if(skipBadOnExplicitPlay && bad.count(selected)) selected=selected>=fileCount ? 1 : selected+1;
        start(); break;
      case 6: assert(false && "NEXT alone must not be used"); break;
      case 0x13: case 0x18: break;
      default: assert(false && "Unexpected command; no guessed firmware opcodes");
    }
    return n;
  }
};
''')
    (root / 'test.cpp').write_text(r'''
#include <cassert>
#include <iostream>
#include <string>
#include "SKETCH_PATH"
void advance(unsigned long ms) { for(unsigned long i=0;i<ms;i+=10) { clockMs+=10; loop(); } }
void press() { pins[D2]=LOW; advance(70); pins[D2]=HIGH; advance(70); }
void filename(unsigned int index, const std::string& name) {
 mp3Serial.filenames[index]=std::vector<byte>(name.begin(),name.end());
}
int main(int argc,char**argv) {
 assert(argc==2); const std::string scenario=argv[1];
 if(scenario=="many_tracks") mp3Serial.fileCount=12;
 if(scenario=="explicit_play") mp3Serial.needsPlay=true;
 if(scenario=="bad_entry") mp3Serial.bad={2};
 if(scenario=="all_bad") { mp3Serial.fileCount=3; mp3Serial.bad={1,2,3}; }
 if(scenario=="no_count" || scenario=="parser") mp3Serial.respond=false;
 if(scenario=="diagnostics_unsupported") mp3Serial.filenameSupported=false;
 if(scenario=="diagnostics_corrupt") mp3Serial.corruptFilename=true;
 if(scenario=="diagnostics_long") mp3Serial.longFilename=true;
 if(scenario=="diagnostics_supported") { mp3Serial.selected=3; filename(2,"._00002.MP3"); }
 if(scenario=="module_skips") {
   mp3Serial.fileCount=7; mp3Serial.bad={7};
   mp3Serial.skipBadOnExplicitPlay=true; mp3Serial.paddedFilename=true;
 }
 if(scenario=="runtime_queries_missing") {
   mp3Serial.filenameSupported=false; mp3Serial.indexSupported=false;
 }
 if(scenario=="samples_one") mp3Serial.fileCount=1;
 if(scenario=="samples_two") mp3Serial.fileCount=2;
 if(scenario=="samples_three") mp3Serial.fileCount=3;
 if(scenario=="samples_large") mp3Serial.fileCount=300;
 if(scenario=="metadata_skip") {
   mp3Serial.fileCount=7; mp3Serial.bad={7};
   filename(6,std::string("14CLE~26MP3\0",12));
   filename(7,std::string("_14CL~26MP3\0",12));
 }
 if(scenario=="literal_metadata") filename(1,"._00001.mp3");
 if(scenario=="all_metadata") {
   mp3Serial.fileCount=12;
   for(unsigned i=1;i<=12;i++) filename(i,"._clip.mp3");
 }
 if(scenario=="wrong_selection") {
   mp3Serial.ignoreSelect=true; mp3Serial.selected=1; filename(1,"._wrong.mp3");
 }
 if(scenario=="malformed_filename") filename(1,std::string("_BAD\x01MP3",8));
 if(scenario=="ordinary_underscore") filename(1,"_INTRO.MP3");
 if(scenario.rfind("relay_",0)==0) SkeletonLink::mockAutoFinish=false;
 if(scenario=="relay_unknown_name") mp3Serial.filenameSupported=false;
 if(scenario=="relay_timeout_retry") SkeletonLink::mockConnected=false;
 if(scenario=="eyes_playback" || scenario=="eyes_status_loss") mp3Serial.duration=30000;
 setup();
 assert(pins[D6]==HIGH && eyeOnWrites==0);
 assert(mp3Serial.audible.empty() && mp3Serial.requests.empty());
 assert(millis()<12000 && diagnosticReply==nullptr && pendingQuery==0);
 advance(600);
 if(scenario=="eyes_playback" || scenario=="eyes_status_loss") {
   press(); advance(2000);
   assert(playing && eyeOnWrites>=3 && eyeOffWrites>=3);
   if(scenario=="eyes_status_loss") {
     mp3Serial.respond=false; advance(9000);
     assert(pins[D6]==HIGH);
     const auto count=eyeOnWrites; advance(2000); assert(eyeOnWrites==count);
   } else {
     press(); assert(!playing && pins[D6]==HIGH);
     advance(1000); assert(playing);
     mp3Serial.state=0; advance(1000); assert(!playing && pins[D6]==HIGH);
   }
 } else if(scenario=="button" || scenario=="many_tracks") {
   unsigned total=mp3Serial.fileCount;
   assert(trackCount==total);
   for(unsigned i=0;i<total+1;i++) { press(); advance(620); }
   assert(mp3Serial.requests.size()==total+1);
   assert(mp3Serial.audible.size()==total+1);
   for(unsigned i=0;i<total+1;i++) assert(mp3Serial.audible[i]==i%total+1);
 } else if(scenario=="continuous_motion") {
   pins[D5]=HIGH; advance(PIR_WARMUP_MS-(millis()-pirStartTime)-100); assert(mp3Serial.requests.empty());
   advance(8000); assert(mp3Serial.audible.size()>=2);
   for(size_t i=1;i<mp3Serial.starts.size();i++) {
     assert(mp3Serial.starts[i]-mp3Serial.starts[i-1]>=mp3Serial.duration+MOTION_GAP_MS);
     assert(mp3Serial.starts[i]-mp3Serial.starts[i-1]<mp3Serial.duration+MOTION_GAP_MS+1000);
   }
   pins[D5]=LOW; auto count=mp3Serial.requests.size(); advance(5000);
   assert(mp3Serial.requests.size()==count);
 } else if(scenario=="bad_entry") {
   press(); advance(1900); press(); advance(3400);
   assert((mp3Serial.requests==std::vector<unsigned>{1,2,3}));
   assert((mp3Serial.audible==std::vector<unsigned>{1,3}));
 } else if(scenario=="explicit_play") {
   press(); advance(1500);
   assert((mp3Serial.requests==std::vector<unsigned>{1}));
   assert((mp3Serial.audible==std::vector<unsigned>{1}));
 } else if(scenario=="all_bad") {
   press(); advance(10000); assert(motionFault);
   assert((mp3Serial.requests==std::vector<unsigned>{1,2,3}));
   pins[D5]=HIGH; advance(60000); assert(mp3Serial.requests.size()==3);
 } else if(scenario=="lost_status") {
   mp3Serial.respond=false; press(); advance(5000); assert(motionFault);
   pins[D5]=HIGH; advance(60000); assert(mp3Serial.requests.size()==1);
 } else if(scenario=="no_count") {
   assert(filenameSamples==0);
   press(); pins[D5]=HIGH; advance(65000);
   assert(trackCount==0 && mp3Serial.requests.empty());
 } else if(scenario=="parser") {
   pendingQuery=0x0C;
   mp3Serial.incoming={0x55,0xAA,0x0C,255,0xAA,0x0C,2,0,5,0};
   receivePlayer(clockMs); assert(trackCount==0);
   mp3Serial.incoming={0xAA,0x0C,2}; receivePlayer(clockMs);
   mp3Serial.incoming={0,5,0xBD}; receivePlayer(clockMs+10);
   assert(trackCount==5);
 } else if(scenario.rfind("diagnostics_",0)==0) {
   assert(trackCount==5 && filenameSamples==4);
   assert(diagnosticNumber(startupReplies[5])==1 && diagnosticNumber(startupReplies[7])==2);
   assert(diagnosticNumber(startupReplies[9])==4 && diagnosticNumber(startupReplies[11])==5);
   if(scenario=="diagnostics_supported") {
     assert(mp3Serial.selected==3);
     assert(startupReplies[6].received && startupReplies[8].received);
     assert(Serial.log.str().find("text=\"00001.MP3\"")!=std::string::npos);
     assert(Serial.log.str().find("text=\"._00002.MP3\"")!=std::string::npos);
     assert(Serial.log.str().find("AA 1E 09 30 30 30 30 31 2E 4D 50 33 C0")!=std::string::npos);
   } else if(scenario=="diagnostics_long") {
     assert(startupReplies[6].received && startupReplies[6].length==60);
     assert(startupReplies[6].data[59]==0);
   } else {
     assert(!startupReplies[6].received && !startupReplies[8].received);
     assert(Serial.log.str().find("no valid reply within 600ms")!=std::string::npos);
   }
   press(); advance(900);
   assert((mp3Serial.audible==std::vector<unsigned>{1}));
   assert(playing);
   auto commands=mp3Serial.frames.size();
   Serial.log.str(""); Serial.incoming={'d'}; serviceSerialMonitor();
   assert(mp3Serial.frames.size()==commands && playing && mp3Serial.selected==1);
   assert(Serial.log.str().find("saved snapshot")!=std::string::npos);
   assert(Serial.log.str().find("firmware version: unknown")!=std::string::npos);
 } else if(scenario=="module_skips") {
   assert(filenameSamples==4 && filenameSampleIndices[2]==6 && filenameSampleIndices[3]==7);
   assert(diagnosticNumber(startupReplies[9])==6 && diagnosticNumber(startupReplies[11])==7);
   Serial.log.str(""); nextTrackIndex=7; press(); advance(2000);
   assert((mp3Serial.requests==std::vector<unsigned>{7}));
   assert((mp3Serial.audible==std::vector<unsigned>{1}));
   const std::string output=Serial.log.str();
   assert(output.find("Player reports PLAYING (requested index 7)")!=std::string::npos);
   assert(output.find("Index reported after PLAY: 1 (requested 7)")!=std::string::npos);
   assert(output.find("Filename reported after PLAY")!=std::string::npos);
   assert(output.find("text=\"00001   MP3.\"")!=std::string::npos);
   assert(output.find("Playback confirmed: index 7")==std::string::npos);
   assert(nextTrackIndex==2);
 } else if(scenario=="runtime_queries_missing") {
   press(); advance(4500);
   assert(!playing && !startingPlayback && !motionFault && playerStateFresh(millis()));
   assert(Serial.log.str().find("No valid reply to playback information query 0x0D")!=std::string::npos);
   assert(Serial.log.str().find("No valid reply to playback information query 0x1E")!=std::string::npos);
   press(); advance(4500);
   assert((mp3Serial.audible==std::vector<unsigned>{1,2}));
 } else if(scenario.rfind("samples_",0)==0) {
   const unsigned count=mp3Serial.fileCount;
   assert(filenameSamples==(count<4 ? count : 4));
   assert(filenameSampleIndices[filenameSamples-1]==count);
   assert(diagnosticNumber(startupReplies[5+2*(filenameSamples-1)])==count);
   for(byte i=1;i<filenameSamples;i++) assert(filenameSampleIndices[i]>filenameSampleIndices[i-1]);
 } else if(scenario=="metadata_skip") {
   assert(SKIP_UNDERSCORE_FILES);
   nextTrackIndex=6; press(); advance(1700);
   press(); advance(1600);
   assert((mp3Serial.requests==std::vector<unsigned>{6,1}));
   assert((mp3Serial.audible==std::vector<unsigned>{6,1}));
   assert(nextTrackIndex==2);
   assert(Serial.log.str().find("Skipping metadata-style filename _14CL~26.MP3 at index 7")!=std::string::npos);
   press(); advance(1600);
   assert((mp3Serial.audible==std::vector<unsigned>{6,1,2}));
 } else if(scenario=="literal_metadata") {
   press(); advance(1600);
   assert((mp3Serial.requests==std::vector<unsigned>{2}));
   assert((mp3Serial.audible==std::vector<unsigned>{2}));
 } else if(scenario=="all_metadata") {
   press(); advance(10000);
   assert(mp3Serial.requests.empty() && mp3Serial.audible.empty());
   assert(motionFault && nextTrackIndex==9 && prepareStep==PREP_NONE);
 } else if(scenario=="wrong_selection") {
   nextTrackIndex=2; press(); advance(1700);
   assert((mp3Serial.requests==std::vector<unsigned>{2}));
   assert((mp3Serial.audible==std::vector<unsigned>{2}));
   assert(Serial.log.str().find("Selection was not confirmed")!=std::string::npos);
 } else if(scenario=="malformed_filename") {
   press(); advance(1700);
   assert((mp3Serial.audible==std::vector<unsigned>{1}));
   assert(Serial.log.str().find("Filename format not recognized")!=std::string::npos);
 } else if(scenario=="ordinary_underscore") {
   press(); advance(1700);
   assert((mp3Serial.audible==std::vector<unsigned>{SKIP_UNDERSCORE_FILES ? 2u : 1u}));
 } else if(scenario=="decoder") {
   char name[MAX_REPLY_DATA+1];
   const byte packed[]={'_','1','4','C','L','~','2','6','M','P','3',0};
   assert(decodeFilename(packed,sizeof(packed),name) && std::string(name)=="_14CL~26.MP3");
   assert(decodeFilename(packed,sizeof(packed)-1,name) && std::string(name)=="_14CL~26.MP3");
   const byte empty[]={0}; assert(!decodeFilename(empty,1,name));
   const byte control[]={'_',1,'M','P','3'}; assert(!decodeFilename(control,5,name));
   const byte hiddenAfterNul[]={'_',0,'M'}; assert(!decodeFilename(hiddenAfterNul,3,name));
   std::vector<byte> large(MAX_REPLY_DATA,'A'); large[4]='.';
   assert(decodeFilename(large.data(),large.size(),name));
   assert(std::string(name).size()==MAX_REPLY_DATA);
 } else if(scenario=="relay_finish") {
   press(); advance(800);
   assert(SkeletonLink::mockSent.empty());
   advance(1200);
   assert(!SkeletonLink::mockSent.empty());
   const auto command=SkeletonLink::mockSent.front();
   assert(command.kind==SkeletonLink::PLAY && std::string(command.name)=="00001.MP3");
   assert(SkeletonLink::mockSentAt.front()>=mp3Serial.starts.front()+mp3Serial.duration);
   assert(!SkeletonTurn::idle());
   press(); advance(1500);
   assert(mp3Serial.audible.size()==1);
   pins[D5]=HIGH; advance(65000); assert(mp3Serial.audible.size()==1);
   pins[D5]=LOW;
   auto reply=command; reply.kind=SkeletonLink::DONE;
   SkeletonLink::mockIncoming.push_back(reply); advance(20);
   assert(SkeletonTurn::idle());
   press(); advance(800); assert((mp3Serial.audible==std::vector<unsigned>{1,2}));
 } else if(scenario=="relay_interrupted") {
   press(); advance(700); press(); advance(700);
   assert(SkeletonLink::mockSent.empty());
   advance(1500);
   assert(!SkeletonLink::mockSent.empty());
   for(const auto& command:SkeletonLink::mockSent) assert(std::string(command.name)=="00002.MP3");
 } else if(scenario=="relay_unknown_name") {
   press(); advance(5000);
   assert((mp3Serial.audible==std::vector<unsigned>{1}));
   assert(SkeletonLink::mockSent.empty() && SkeletonTurn::idle());
 } else if(scenario=="relay_timeout_retry") {
   press(); advance(4000);
   const auto original=SkeletonTurn::request;
   advance(31000);
   assert(SkeletonTurn::state==SkeletonTurn::FAULT && SkeletonLink::mockSent.empty());
   SkeletonLink::mockConnected=true;
   press(); advance(1200);
   assert(mp3Serial.audible.size()==1 && !SkeletonLink::mockSent.empty());
   assert(SkeletonLink::sameRequest(original,SkeletonLink::mockSent.back()));
   auto reply=original; reply.kind=SkeletonLink::DONE;
   SkeletonLink::mockIncoming.push_back(reply); advance(20); assert(SkeletonTurn::idle());
 } else if(scenario=="relay_stale_ack") {
   press(); advance(2000);
   const auto command=SkeletonTurn::request;
   auto reply=command; reply.sequence++; reply.kind=SkeletonLink::DONE;
   SkeletonLink::mockIncoming.push_back(reply); advance(20); assert(!SkeletonTurn::idle());
   reply=command; reply.kind=SkeletonLink::ACCEPTED;
   SkeletonLink::mockIncoming.push_back(reply); advance(40000);
   assert(SkeletonTurn::state==SkeletonTurn::WAITING);
   reply.kind=SkeletonLink::DONE;
   SkeletonLink::mockIncoming.push_back(reply); advance(20); assert(SkeletonTurn::idle());
 } else { assert(false && "unknown test"); }
 std::cout<<"PASS: "<<scenario<<"\n";
}
'''.replace('SKETCH_PATH', str(sketch)))
    subprocess.run(['clang++', '-std=c++17', '-DSKELETON_LINK_TEST', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(root), str(root / 'test.cpp'),
                    '-o', str(root / 'test')], check=True)
    for scenario in ('eyes_playback', 'eyes_status_loss', 'button', 'many_tracks', 'continuous_motion', 'bad_entry',
                     'explicit_play', 'all_bad', 'lost_status', 'no_count', 'parser',
                     'diagnostics_supported', 'diagnostics_unsupported', 'diagnostics_corrupt',
                     'diagnostics_long', 'module_skips', 'runtime_queries_missing',
                     'samples_one', 'samples_two', 'samples_three', 'samples_large',
                     'literal_metadata', 'all_metadata', 'wrong_selection',
                     'malformed_filename', 'ordinary_underscore', 'decoder',
                     'relay_finish', 'relay_interrupted', 'relay_unknown_name',
                     'relay_timeout_retry', 'relay_stale_ack'):
        subprocess.run([str(root / 'test'), scenario], check=True)
    # Run underscore-skip behavior only when that naming convention is enabled.
    if 'const bool SKIP_UNDERSCORE_FILES = true;' in sketch.read_text():
        subprocess.run([str(root / 'test'), 'metadata_skip'], check=True)
