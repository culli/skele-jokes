from pathlib import Path
import subprocess
import tempfile

workspace = Path(__file__).resolve().parents[1]
for name in ('SkeletonProtocol.h', 'SkeletonBLE.h', 'SkeletonTurn.h', 'SkeletonEyes.h'):
    assert (workspace / 'motion_mp3' / name).read_bytes() == (workspace / 'button_mp3' / name).read_bytes(), name

with tempfile.TemporaryDirectory(prefix='skeleton-protocol-') as directory:
    root = Path(directory)
    (root / 'test.cpp').write_text(r'''
#include "SkeletonProtocol.h"
#include <cassert>
#include <iostream>
#include <cstring>
using namespace SkeletonLink;
int main() {
  char name[NAME_SIZE];
  assert(normalizeName("00001.mp3",name) && !strcmp(name,"00001.MP3"));
  assert(normalizeName("ABCDEFGH.WAV",name));
  for(const char* bad : {"", "x", "00001", "00001.txt", "._00001.MP3", "_INTRO.MP3",
                        "../A.MP3", "123456789.MP3", "A/B.MP3", "A\\B.MP3", "A*.MP3"}) {
    assert(!normalizeName(bad,name));
  }
  Packet command;
  command.session=0x78563412; command.sequence=0x3412;
  assert(normalizeName("ABCDEFGH.mp3",command.name));
  uint8_t wire[WIRE_SIZE]; encode(command,wire);
  assert(sizeof(wire)==20 && wire[0]==1 && wire[1]==PLAY);
  assert(wire[2]==0x12 && wire[5]==0x78 && wire[6]==0x12 && wire[7]==0x34);
  Packet decoded;
  assert(decode(wire,sizeof(wire),decoded) && sameRequest(command,decoded));
  assert(!decode(wire,sizeof(wire)-1,decoded));
  assert(!decode(wire,sizeof(wire)+1,decoded));
  for(uint8_t kind=PLAY;kind<=BUSY;kind++) {
    wire[1]=kind; assert(decode(wire,sizeof(wire),decoded) && decoded.kind==kind);
  }
  wire[1]=99; assert(!decode(wire,sizeof(wire),decoded));
  encode(command,wire); wire[0]=2; assert(!decode(wire,sizeof(wire),decoded));
  encode(command,wire); memset(wire+2,0,4); assert(!decode(wire,sizeof(wire),decoded));
  encode(command,wire); wire[6]=wire[7]=0; assert(!decode(wire,sizeof(wire),decoded));
  assert(normalizeName("00001.mp3",command.name)); encode(command,wire);
  wire[19]='X'; assert(!decode(wire,sizeof(wire),decoded));
  encode(command,wire); wire[8]='_'; assert(!decode(wire,sizeof(wire),decoded));
  encode(command,wire); assert(decode(wire,sizeof(wire),decoded));
  decoded.sequence++; assert(!sameRequest(command,decoded));
  std::cout<<"PASS: packet round trips, malformed packets, filenames, identical shared headers\n";
}
''')
    subprocess.run(['clang++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(workspace / 'motion_mp3'),
                    str(root / 'test.cpp'), '-o', str(root / 'test')], check=True)
    subprocess.run([str(root / 'test')], check=True)
