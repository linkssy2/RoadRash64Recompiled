"""Offline coordinate checks using the production HUD classifier and view math.

This does not render the game or prove runtime view discovery. It tests the
full-window misclassification reported in two-player screenshots and checks
corner transforms for 2/4 views, while retaining full-screen behavior.
"""
from pathlib import Path
import argparse, subprocess
root=Path(__file__).resolve().parents[1]
source=(root/'native/lib/rt64/src/render/rt64_framebuffer_renderer.cpp').read_text()
def extract(signature):
    start=source.index(signature);end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
header=root/'native/lib/rt64/src/common/rt64_rr64_hud_view.h'
code='#include <cstdio>\n#include "'+header.as_posix()+'"\nusing namespace RT64;\n'
code+='constexpr uint16_t G_EX_ORIGIN_NONE=0x800,G_EX_ORIGIN_LEFT=0,G_EX_ORIGIN_RIGHT=0x400;\n'
code+=extract('struct AutomaticHUDBounds')+';\n'
for name in ('constexpr float automaticHUDBoundsWidth','constexpr float automaticHUDBoundsHeight','constexpr uint16_t automaticHUDOrigin'):
    code+=extract(name)+'\n'
code+=r'''
#include <vector>
#include <algorithm>
struct TestRect {
 float l=19,r=62,t=12,b=36;
 bool isNull() const {return false;}
 float left(bool) const{return l;} float right(bool) const{return r;}
 float top(bool) const{return t;} float bottom(bool) const{return b;}
};
struct Projection {enum class Type{None,Perspective,Orthographic,Rectangle,Triangle};Type type=Type::Rectangle;};
struct TestMode {bool compare=false,update=false;bool zCmp()const{return compare;}bool zUpd()const{return update;}};
struct GameCall {
 struct {TestMode otherMode; uint16_t rectLeftOrigin=2048,rectRightOrigin=2048,scissorLeftOrigin=2048,scissorRightOrigin=2048;TestRect rect;unsigned triangleCount=0;} callDesc;
 struct {unsigned faceIndicesStart=0,rawVertexStart=0;} meshDesc;
};
struct DrawData {struct Point{float x,y;};std::vector<unsigned> faceIndices;std::vector<Point> posScreen;std::vector<float> triPosFloats;};
'''
code+=extract('AutomaticHUDBounds automaticHUDBounds')+'\n'
code+=r'''
int main() {
 unsigned failures=0,cases=0;
 auto check=[&](bool ok){++cases;if(!ok){++failures;std::printf("Failed case %u\n",cases);}};
 auto anchor=[&](RR64HUDView v,AutomaticHUDBounds b) {
   if(!v.contains(b.left,b.top,b.right,b.bottom)) return G_EX_ORIGIN_NONE;
   b.left-=v.left;b.right-=v.left;b.top-=v.top;b.bottom-=v.top;
   auto local=automaticHUDOrigin(b,int(v.width()),int(v.height()),true);
   return local==G_EX_ORIGIN_NONE ? local : rr64HUDViewOrigin(v,local==G_EX_ORIGIN_RIGHT,320,G_EX_ORIGIN_RIGHT);
 };
 RR64HUDView top{0,0,320,120},bottom{0,120,320,240};
 AutomaticHUDBounds speedTop{true,248,297,87,109},healthBottom{true,19,62,120,144};
 check(automaticHUDOrigin(speedTop,320,240)==G_EX_ORIGIN_NONE);
 check(automaticHUDOrigin(healthBottom,320,240)==G_EX_ORIGIN_NONE);
 check(anchor(top,speedTop)==G_EX_ORIGIN_RIGHT);
 check(anchor(bottom,healthBottom)==G_EX_ORIGIN_LEFT);
 check(anchor(top,{true,19,62,12,36})==G_EX_ORIGIN_LEFT);
 check(anchor(bottom,{true,248,297,207,229})==G_EX_ORIGIN_RIGHT);
 for(unsigned row=0;row<2;++row) for(unsigned col=0;col<2;++col) {
   float x=col*160.0f,y=row*120.0f; RR64HUDView v{x,y,x+160,y+120};
   check(anchor(v,{true,x+10,x+54,y+12,y+36})==col*512);
   check(anchor(v,{true,x+116,x+151,y+87,y+109})==(col+1)*512);
   check(anchor(v,{true,x+10,x+75,y+99,y+113})==col*512);
   check(!v.contains(x+150,y+12,x+175,y+36));
 }
 check(rr64HUDViewOffset(0,1024,0.25f)==-0.25f);
 check(rr64HUDViewOffset(512,1024,0.25f)==0.0f);
 check(rr64HUDViewOffset(1024,1024,0.25f)==0.25f);
 check(automaticHUDOrigin({true,19,62,12,36},320,240)==G_EX_ORIGIN_LEFT);
 check(automaticHUDOrigin({true,248,297,207,229},320,240)==G_EX_ORIGIN_RIGHT);
 check(automaticHUDOrigin({true,100,240,95,130},320,240)==G_EX_ORIGIN_NONE);
 // A world sprite can cross a HUD corner at several apparent sizes. Exercise
 // production bounds extraction with identical geometry but different depth
 // state: HUD remains eligible; world placement must not depend on screen region.
 Projection projection;GameCall call;DrawData data;
 for(float size:{4.f,12.f,32.f}) {
   call.callDesc.rect={20,20+size,12,12+size};
   call.callDesc.otherMode={false,false};
   check(automaticHUDBounds(projection,call,data,320,240).valid);
   for(auto mode:{TestMode{true,false},TestMode{false,true},TestMode{true,true}}) {
     call.callDesc.otherMode=mode;
     check(!automaticHUDBounds(projection,call,data,320,240).valid);
   }
 }
 // 3/4-player columns widen like a full-width camera; full-width, centred
 // and off-grid regions are left to RT64's generic rule.
 check(rr64SplitColumnView(0,160,0,320));
 check(rr64SplitColumnView(160,320,0,320));
 check(rr64SplitColumnView(1,159,0,320));
 check(!rr64SplitColumnView(0,320,0,320));
 check(!rr64SplitColumnView(80,240,0,320));
 check(!rr64SplitColumnView(0,100,0,320));
 check(!rr64SplitColumnView(0,0,0,0));
 std::printf("Split HUD coordinate checks: %u cases, %u failures\n",cases,failures);
 return failures?1:0;
}
'''
parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--output',required=True,type=Path)
args=parser.parse_args();out=args.output.resolve();out.mkdir(parents=True,exist_ok=True)
(out/'fixture.cpp').write_text(code)
(out/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.20)\nproject(SplitHUD LANGUAGES CXX)\nadd_executable(SplitHUD fixture.cpp)\ntarget_compile_features(SplitHUD PRIVATE cxx_std_17)\n')
subprocess.run(['cmake','-S',str(out),'-B',str(out/'build')],check=True)
subprocess.run(['cmake','--build',str(out/'build'),'--config','Release'],check=True)
exe=out/'build/Release/SplitHUD.exe'
if not exe.exists():exe=out/'build/SplitHUD'
subprocess.run([str(exe)],check=True)
