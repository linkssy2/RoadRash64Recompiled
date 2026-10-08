#include "hle/rt64_rr64_translation_rejection.h"
#include <limits>
#include <memory>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <algorithm>
#include <array>
#include <cstring>
using namespace RT64;
#include "fixtures/rr64_matching_workload_fixture.inc"
static void check(bool x,const char*why){if(!x){std::fprintf(stderr,"FAIL: %s\n",why);std::exit(1);}}
static GameFrame frame(unsigned slot){GameFrame f;f.workloads={slot};f.perspectiveScenes.push_back(GameScene{{{slot,0,0}}});f.frameMap.workloads.resize(WORKLOAD_QUEUE_SIZE);return f;}
static uint64_t digest=1469598103934665603ull;
static void mix(uint64_t value){digest=(digest^value)*1099511628211ull;}
static void real(float value){uint32_t bits;std::memcpy(&bits,&value,4);mix(bits);}
template<class Map>static void mapping(const Map&m){
 mix(m.mapped);mix(m.prevTransformIndex);const auto&r=m.rigidBody;
 for(unsigned i=0;i<3;++i)real(r.linearVelocity[i]);real(r.angularVelocity);mix(r.transformIndex);
 mix(r.lerpTranslation);mix(r.lerpRotation);mix(r.lerpScale);mix(r.lerpSkew);mix(r.lerpPerspective);mix(r.lerpDecompose);
 for(const auto&t:r.transforms){mix(t.valid);mix(t.coordinateFlip);if(t.valid){for(unsigned i=0;i<3;++i){real(t.scale[i]);real(t.skew[i]);real(t.translation[i]);}for(unsigned i=0;i<4;++i)real(t.perspective[i]);}}
 auto identity=hlslpp::float4x4::identity();auto mid=r.lerp(0.5f,identity,identity,false);for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)real(mid[row][col]);
}
static void match(GameFrame &a,const GameFrame &b,WorkloadQueue&q){
 bool v=0,t=0,l=0;a.match(nullptr,q,b,nullptr,v,t,l);check(!v&&!t&&!l,"Unexpected uploader");
 mix(a.matched);mix(a.rr64InterpolationCompatible);mix(v);mix(t);mix(l);
 for(const auto&w:a.frameMap.workloads){mix(w.mapped);mix(w.prevWorkloadIndex);for(const auto&t:w.transforms)mapping(t);for(const auto&t:w.viewProjections)mapping(t);for(bool v:w.prevTransformsMapped)mix(v);}
}
static void witnessWorkload(Workload &w,const std::array<float,3> &position,float viewZ,uint32_t id=G_EX_ID_AUTO){
 seed(w,1,2,11);
 // The log contains translations, not vertices or full matrices. Use one
 // synthetic finite quad so these are policy regressions, not workload replays.
 auto &d=w.drawData;d.posFloats={-1,-1,0,1,-1,0,1,1,0,-1,1,0};
 for(unsigned axis=0;axis<3;++axis)d.worldTransforms[0][3][axis]=position[axis];
 d.viewTransforms[0][3][2]=viewZ;d.viewProjTransforms[0]=d.viewTransforms[0];
 d.transformGroups[1].matrixId=id;
 d.transformGroups[1].ordering=id==G_EX_ID_AUTO?G_EX_ORDER_AUTO:G_EX_ORDER_LINEAR;
}
static void markCamera(Workload &w){
 auto &g=w.drawData.transformGroups[0];g.matrixId=0x484c0002u;g.decompose=false;
 g.positionInterpolation=g.rotationInterpolation=g.scaleInterpolation=g.skewInterpolation=g.perspectiveInterpolation=G_EX_COMPONENT_INTERPOLATE;
}
static void capturedTranslationRegressions(WorkloadQueue &q){
 struct Reversal {std::array<float,3> old,before,after;float beforeView,afterView;};
 // launch-02 stderr lines762-772: each preceding selected world becomes the
 // next pair's previous world. Its actual recorded delta supplies velocity.
 const Reversal cases[]{
  {{79.2193069f,-18.1347904f,7.767313f},{79.2975845f,-18.1136665f,7.6968255f},{79.2950363f,-18.1021862f,7.71187544f},-117.219498f,-117.386337f},
  {{4.72390318f,-26.7187328f,4.14263678f},{4.80184746f,-26.6965828f,4.06276655f},{4.79975653f,-26.689146f,4.06940842f},-117.859283f,-117.937851f},
  {{79.6997223f,7.30792665f,4.45681763f},{83.5739441f,6.0276947f,9.98501205f},{83.5608215f,6.02814388f,9.88123512f},-118.074295f,-118.132492f}
 };
 for(const auto &sample:cases){
  witnessWorkload(q.workloads[0],sample.before,sample.beforeView);
  witnessWorkload(q.workloads[1],sample.after,sample.afterView);
  auto previous=frame(0),current=frame(1);previous.matched=true;
  auto &history=previous.frameMap.workloads[0];history.mapped=true;history.transforms.resize(1);history.viewProjections.resize(1);
  for(unsigned axis=0;axis<3;++axis)history.transforms[0].rigidBody.linearVelocity[axis]=sample.before[axis]-sample.old[axis];
  match(current,previous,q);
  const auto &mapping=current.frameMap.workloads[1];
  check(current.matched && !current.rr64InterpolationCompatible && current.rr64GeometryRejectionReasons==32,
      "Captured AUTO reversal must retain translation rejection until its authored identity is proven");
  check(mapping.transforms[0].mapped && !mapping.transforms[0].rigidBody.lerpTranslation &&
      mapping.viewProjections[0].mapped && mapping.viewProjections[0].rigidBody.lerpTranslation,
      "Captured reversal must reproduce held-world/blended-camera decisions in the actual matcher");
  markCamera(q.workloads[0]);markCamera(q.workloads[1]);
  auto qualified=frame(1);match(qualified,previous,q);
  check(qualified.rr64InterpolationCompatible && qualified.frameMap.workloads[1].transforms[0].rigidBody.lerpTranslation,
      "Captured small reversal must recover with a shared phase and nearby actual camera matrices");
 }
 struct StaticStep {uint32_t id;std::array<float,3> before,after;float beforeView,afterView;};
 const StaticStep native[]{
  {0x525207b7u,{19477.0977f,-20241.6172f,983.013f},{19470.8418f,-20240.0234f,982.694702f},-118.585587f,-118.583389f},
  {0x52520ca8u,{27095.2305f,-22413.1016f,540.753662f},{27085.9961f,-22411.5391f,541.406738f},-118.627457f,-118.632935f}
 };
 for(const auto &sample:native){
  witnessWorkload(q.workloads[0],sample.before,sample.beforeView,sample.id);
  witnessWorkload(q.workloads[1],sample.after,sample.afterView,sample.id);
  auto previous=frame(0),current=frame(1);
  // These two native witnesses did not capture prior velocity. Exercise the
  // valid missing-history case; do not claim it was the captured history.
  check(RR64TranslationRejection::reject(true,current,previous,q),"Unproven native step must retain early rejection");
  match(current,previous,q);
  check(current.matched && !current.rr64InterpolationCompatible && current.rr64GeometryRejectionReasons==32 &&
      current.frameMap.workloads[1].transforms[0].mapped && !current.frameMap.workloads[1].transforms[0].rigidBody.lerpTranslation,
      "Native witness beyond the existing small-motion bound must not imply camera continuity");
  markCamera(q.workloads[0]);markCamera(q.workloads[1]);
  auto qualified=frame(1);match(qualified,previous,q);
  check(qualified.rr64InterpolationCompatible && qualified.frameMap.workloads[1].transforms[0].rigidBody.lerpTranslation,
      "Camera-qualified source-2 object movement must use its tenfold translation units");
 }
 std::puts("PASS three captured AUTO reversal histories and two native steps with explicitly missing history; synthetic geometry");
}
static void shadowIdentityRegressions(WorkloadQueue &q){
 // Followup03's small AUTO translation witness, now with producer-owned shadow
 // identity. Geometry, affine bases and reversed prior velocity are synthetic.
 const std::array<float,3> before{-43.0688667f,-25.5427399f,-2.52050161f};
 const std::array<float,3> after{-43.0730286f,-25.5391884f,-2.51635742f};
 for(unsigned variant=0;variant<8;++variant){
  const unsigned id=variant==1?0x12345678u:variant==3?0x52530000u:variant==4?0x5253ffffu:0x52530001u;
  witnessWorkload(q.workloads[0],before,-118.654465f,id);
  witnessWorkload(q.workloads[1],after,-118.653488f,variant==2?id+1:id);
  if(variant!=7){markCamera(q.workloads[0]);markCamera(q.workloads[1]);}
  auto &pd=q.workloads[0].drawData,&cd=q.workloads[1].drawData;
  if(variant==5)cd.worldTransforms[0][3][0]=float(pd.worldTransforms[0][3][0])+5.f;
  if(variant==6)pd.transformGroups[1].ordering=cd.transformGroups[1].ordering=G_EX_ORDER_AUTO;
  auto previous=frame(0),current=frame(1);previous.matched=true;
  auto &history=previous.frameMap.workloads[0];history.mapped=true;
  history.transforms.resize(1);history.viewProjections.resize(1);
  for(unsigned axis=0;axis<3;++axis)history.transforms[0].rigidBody.linearVelocity[axis]=
      float(pd.worldTransforms[0][3][axis])-float(cd.worldTransforms[0][3][axis]);
  previous.buildTransformIdMap(q.workloads[0],q.workloads[0].transformIdMap,q.workloads[0].transformIgnoredIds);
  match(current,previous,q);
  const bool accepts=variant==0||variant==4;
  check(current.rr64InterpolationCompatible==accepts,
      "Shadow tagging must retain only camera-qualified AUTO motion inside its five-unit bound");
  if(accepts)check(current.frameMap.workloads[1].transforms[0].rigidBody.lerpTranslation,
      "Owned shadow reversal must interpolate with its matched camera");
  else check(current.rr64GeometryRejectionReasons & (variant==2?4u:32u),
      "Unrelated/reserved ID, generation change, bound, ordering or unmarked camera must retain strict rejection");
 }
 std::puts("PASS shadow-ID captured translation policy; unrelated/reserved ID, generation, five-unit, ordering and unmarked-camera negatives");
}
static void qualifiedMotionRegressions(WorkloadQueue &q){
 // Followup03 source records distinguish small segmented AUTO motion from
 // a 2796-unit AUTO mispair and source-2 static packet motion. Only translations
 // were captured: bases, quad geometry and reversed velocity below are synthetic.
 struct Sample{unsigned id;std::array<float,3> before,after;float viewBefore,viewAfter;bool accepts;};
 const Sample samples[]{
  {G_EX_ID_AUTO,{-43.0688667f,-25.5427399f,-2.52050161f},{-43.0730286f,-25.5391884f,-2.51635742f},-118.654465f,-118.653488f,true},
  {G_EX_ID_AUTO,{-3186.73096f,1184.17969f,-47.2160339f},{-549.79248f,252.490234f,-36.7446899f},-1186.12195f,-1186.09656f,false},
  {0x525205a2u,{-39525.3281f,12287.6934f,460.733185f},{-39519.8984f,12282.959f,460.887726f},-211.854156f,-211.83609f,true},
  {0x52520ab9u,{14732.0947f,10245.6885f,-748.225586f},{14727.1973f,10235.4492f,-747.968872f},-118.604385f,-118.59259f,true}
 };
 for(const auto &sample:samples){
  witnessWorkload(q.workloads[0],sample.before,sample.viewBefore,sample.id);
  witnessWorkload(q.workloads[1],sample.after,sample.viewAfter,sample.id);
  markCamera(q.workloads[0]);markCamera(q.workloads[1]);
  auto previous=frame(0),current=frame(1);previous.matched=true;
  auto &history=previous.frameMap.workloads[0];history.mapped=true;history.transforms.resize(1);history.viewProjections.resize(1);
  for(unsigned axis=0;axis<3;++axis)history.transforms[0].rigidBody.linearVelocity[axis]=sample.before[axis]-sample.after[axis];
  previous.buildTransformIdMap(q.workloads[0],q.workloads[0].transformIdMap,q.workloads[0].transformIgnoredIds);
  match(current,previous,q);
  check(current.rr64InterpolationCompatible==sample.accepts,"Followup03 captured translation policy regression");
 }
 for(unsigned variant=0;variant<14;++variant){
  witnessWorkload(q.workloads[0],{0,0,0},0);
  witnessWorkload(q.workloads[1],{.02f,0,0},-.2f);
  markCamera(q.workloads[0]);markCamera(q.workloads[1]);
  auto &pd=q.workloads[0].drawData,&cd=q.workloads[1].drawData;
  auto previous=frame(0),current=frame(1);previous.matched=true;
  auto &history=previous.frameMap.workloads[0];history.mapped=true;history.transforms.resize(1);history.viewProjections.resize(1);
  history.transforms[0].rigidBody.linearVelocity={-1,0,0};
  if(variant==1){cd.viewTransforms[0][0][0]=-1;cd.viewTransforms[0][2][2]=-1;}
  if(variant==2)cd.viewTransforms[0][0][0]=0;
  if(variant==3)cd.viewTransforms[0][0][0]=2;
  if(variant==4)cd.viewTransforms[0][3][2]=-50;
  if(variant==5)cd.projTransforms[0][0][0]=2;
  if(variant==6)cd.transformGroups[0].matrixId++;
  if(variant==7)cd.transformGroups[1].positionInterpolation=G_EX_COMPONENT_SKIP;
  if(variant==8)cd.worldTransforms[0][3][0]=50;
  if(variant==9)cd.worldTransforms[0][0][0]=std::numeric_limits<float>::quiet_NaN();
  if(variant==10)std::swap(cd.faceIndices[1],cd.faceIndices[2]);
  if(variant==11 || variant==12){
   auto &pair=q.workloads[1].fbPairs[0];pair.projections.push_back(pair.projections[0]);pair.projectionCount=2;
   if(variant==11)current.perspectiveScenes.push_back(GameScene{{{1,0,1}}});
   else {pair.projections[1].type=Projection::Type::Orthographic;current.orthographicScenes.push_back(GameScene{{{1,0,1}}});}
  }
  if(variant==13){cd.worldTransforms[0][0][0]=-1;cd.worldTransforms[0][2][2]=-1;}
  cd.viewProjTransforms[0]=hlslpp::mul(cd.viewTransforms[0],cd.projTransforms[0]);
  match(current,previous,q);
  check(variant==0?current.rr64InterpolationCompatible:!current.rr64InterpolationCompatible,
      "Qualified small-motion success or camera/geometry/shared-pass rejection failed");
  if(variant==0)check(std::abs(current.frameMap.workloads[1].transforms[0].rigidBody.linearVelocity[0]-.02f)<1e-6f,
      "Qualified override must preserve AUTO velocity history");
 }
 std::puts("PASS qualified reversal; camera turn/scale/singularity/jump/projection/cut, explicit hold, teleport, NaN, topology and shared projection negatives");
}
static void unmarkedCameraRegression(WorkloadQueue &q){
 witnessWorkload(q.workloads[0],{0,0,0},0);
 witnessWorkload(q.workloads[1],{.02f,0,0},-2);
 auto &turned=q.workloads[1].drawData;
 turned.viewTransforms[0][0][0]=-1;turned.viewTransforms[0][2][2]=-1;
 turned.viewProjTransforms[0]=turned.viewTransforms[0];
 auto previous=frame(0),current=frame(1);previous.matched=true;
 auto &history=previous.frameMap.workloads[0];history.mapped=true;history.transforms.resize(1);history.viewProjections.resize(1);
 history.transforms[0].rigidBody.linearVelocity={-1,0,0};
 match(current,previous,q);
 check(current.matched && !current.rr64InterpolationCompatible && current.rr64GeometryRejectionReasons==32,
     "A tiny unqualified AUTO step must not admit an unmarked sharp camera change");
 const auto halfway=current.frameMap.workloads[1].viewProjections[0].rigidBody.lerp(.5f,
     q.workloads[0].drawData.viewTransforms[0],turned.viewTransforms[0],false);
 check(std::abs(float(halfway[0][0]))<1e-6f && std::abs(float(halfway[2][2]))<1e-6f,
     "Negative fixture must expose the singular camera that translation fallback protects against");
 for(unsigned slot=0;slot<2;++slot){
  auto &camera=q.workloads[slot].drawData.transformGroups[0];camera.matrixId=0x484c0001u+slot;camera.decompose=false;
  camera.positionInterpolation=camera.rotationInterpolation=camera.scaleInterpolation=
      camera.skewInterpolation=camera.perspectiveInterpolation=G_EX_COMPONENT_INTERPOLATE;
 }
 auto cut=frame(1);match(cut,previous,q);
 check(!cut.matched && !cut.rr64InterpolationCompatible && cut.rr64GeometryRejectionReasons==1 &&
     cut.frameMap.workloads[1].transforms.empty(),"Authored camera boundary must reject before any world interpolation");
 witnessWorkload(q.workloads[2],{.02f,0,0},-2.2f);
 auto &resumed=q.workloads[2].drawData;resumed.transformGroups[0]=turned.transformGroups[0];
 resumed.viewTransforms[0][0][0]=-1;resumed.viewTransforms[0][2][2]=-1;resumed.viewProjTransforms[0]=resumed.viewTransforms[0];
 auto next=frame(2);match(next,cut,q);
 check(next.matched && next.rr64InterpolationCompatible && next.rr64GeometryRejectionReasons==0,
     "Next same-phase pair with unchanged world must recover after the camera boundary");
 std::puts("PASS race AUTO sharp-camera negative, submitted cut before matching and same-phase recovery");
}
int main(){
#ifdef _WIN32
 _putenv_s("RR64_STABLE_PRESENTATION","1");
#else
 setenv("RR64_STABLE_PRESENTATION","1",1);
#endif
 auto q=std::make_unique<WorkloadQueue>();unsigned proven=0,deferred=0;
 for(unsigned count:{1u,7u,32u})for(float dx:{0.f,1.f,4.9f,5.f,8.f,50.f,-8.f})for(float camera:{0.f,1.f,-2.f})for(int history:{0,1,2})for(unsigned policy:{G_EX_COMPONENT_AUTO,G_EX_COMPONENT_INTERPOLATE,G_EX_COMPONENT_SKIP}){
  seed(q->workloads[0],count,8,0);seed(q->workloads[1],count,8,0);auto prev=frame(0),cur=frame(1);
  q->workloads[1].drawData.worldTransforms[0][3][0]=dx;q->workloads[1].drawData.viewTransforms[0][3][0]=camera;
  q->workloads[1].drawData.transformGroups[1].positionInterpolation=policy;
  if(history){prev.matched=true;auto&m=prev.frameMap.workloads[0];m.mapped=true;m.transforms.resize(count);m.viewProjections.resize(1);m.transforms[0].rigidBody.linearVelocity={history==1?8.f:-8.f,0,0};prev.buildTransformIdMap(q->workloads[0],q->workloads[0].transformIdMap,q->workloads[0].transformIgnoredIds);}
  auto proof=RR64TranslationRejection::reject(true,cur,prev,*q);check(!RR64TranslationRejection::reject(false,cur,prev,*q),"Ineligible path must defer");
  match(cur,prev,*q);if(proof){check(!cur.rr64InterpolationCompatible,"False positive: actual matcher accepted");++proven;}else ++deferred;
 }
 // A rejected AUTO transition records history that allows the next one.
 seed(q->workloads[0],7,8,0);seed(q->workloads[1],7,8,0);seed(q->workloads[2],7,8,0);
 auto a=frame(0),b=frame(1),c=frame(2);
 q->workloads[1].drawData.worldTransforms[0][3][0]=8;q->workloads[1].drawData.viewTransforms[0][3][0]=-2;
 q->workloads[2].drawData.worldTransforms[0][3][0]=16;q->workloads[2].drawData.viewTransforms[0][3][0]=-4;
 check(RR64TranslationRejection::reject(true,b,a,*q),"Abrupt first movement must be provably rejected");match(b,a,*q);
 check(!RR64TranslationRejection::reject(true,c,b,*q),"Recorded velocity must let steady next movement recover");match(c,b,*q);check(c.rr64InterpolationCompatible,"Actual matcher must recover");
 b.matched=false;auto d=frame(2);check(RR64TranslationRejection::reject(true,d,b,*q),"Skipping previous full match would wrongly make rejection sticky");
 // Boundaries intentionally defer: nonunique identities, AUTO objects,
 // unmatched camera policy, and any possible unchanged previous camera.
 for(unsigned mode:{0u,1u,2u,3u,4u,6u,8u,10u,11u,12u,15u}){
  seed(q->workloads[0],7,8,mode);seed(q->workloads[1],7,8,mode);auto p=frame(0),n=frame(1);
  q->workloads[1].drawData.worldTransforms[0][3][0]=8;q->workloads[1].drawData.viewTransforms[0][3][0]=-2;
  bool proof=RR64TranslationRejection::reject(true,n,p,*q);match(n,p,*q);if(proof){check(!n.rr64InterpolationCompatible,"Adversarial false positive");++proven;}else ++deferred;
 }
 for(unsigned twist=0;twist<8;++twist)for(unsigned count:{2u,7u,32u}){
  seed(q->workloads[0],count,8,11);seed(q->workloads[1],count,8,11);auto p=frame(0),n=frame(1);
  for(unsigned w=0;w<2;++w){auto&g=q->workloads[w].drawData.transformGroups[1];g.matrixId=0x52510000;g.ordering=G_EX_ORDER_LINEAR;}
  auto& cd=q->workloads[1].drawData;
  cd.worldTransforms[0][3][0]=8;cd.viewTransforms[0][3][0]=-2;
  if(twist==1)cd.worldTransforms[1][3][0]=3;
  if(twist==2)cd.posFloats[cd.worldTransformVertexIndices[1]*3]+=1;
  if(twist==3)std::swap(cd.faceIndices[8*3+1],cd.faceIndices[8*3+2]);
  if(twist==4)cd.posFloats[cd.worldTransformVertexIndices[1]*3]=std::numeric_limits<float>::quiet_NaN();
  if(twist==5)cd.transformGroups[2].matrixId=G_EX_ID_IGNORE;
  if(twist==6){cd.viewTransforms.push_back(hlslpp::float4x4::identity());cd.projTransforms.push_back(cd.projTransforms[0]);cd.viewProjTransforms.push_back(cd.viewProjTransforms[0]);cd.viewProjTransformGroups.push_back(0);auto&pair=q->workloads[1].fbPairs[0];pair.projections.push_back(pair.projections[0]);pair.projections[1].transformsIndex=1;pair.projectionCount=2;n.perspectiveScenes.push_back(GameScene{{{1,0,1}}});}
  if(twist==7){auto&pd=q->workloads[0].drawData;pd.viewTransforms.push_back(cd.viewTransforms[0]);pd.projTransforms.push_back(pd.projTransforms[0]);pd.viewProjTransforms.push_back(pd.viewProjTransforms[0]);pd.viewProjTransformGroups.push_back(0);auto&pair=q->workloads[0].fbPairs[0];pair.projections.push_back(pair.projections[0]);pair.projections[1].transformsIndex=1;pair.projectionCount=2;p.perspectiveScenes.push_back(GameScene{{{0,0,1}}});}
  bool proof=RR64TranslationRejection::reject(true,n,p,*q);match(n,p,*q);if(proof){check(!n.rr64InterpolationCompatible,"Mixed AUTO/LINEAR false positive");++proven;}else ++deferred;
 }
 for(unsigned special=0;special<4;++special){
  seed(q->workloads[0],7,8,0);seed(q->workloads[1],7,8,0);auto p=frame(0),n=frame(1);
  auto&pd=q->workloads[0].drawData;auto&cd=q->workloads[1].drawData;
  cd.worldTransforms[0][3][0]=8;cd.viewTransforms[0][3][0]=-2;
  if(special==0)for(auto&call:q->workloads[0].fbPairs[0].projections[0].gameCalls)call.callDesc.triangleCount=0;
  if(special==1)q->workloads[1].paused=true;
  if(special==2)cd.transformGroups[0].matrixId=0x77770000;
  if(special==3){cd.transformGroups[2].matrixId=cd.transformGroups[1].matrixId;pd.transformGroups[2].matrixId=pd.transformGroups[1].matrixId;}
  bool proof=RR64TranslationRejection::reject(true,n,p,*q);match(n,p,*q);if(proof){check(!n.rr64InterpolationCompatible,"Special proof false positive");++proven;}else ++deferred;
  if(special==0)check(n.rr64GeometryRejectionReasons&4,"Unpaired scene must retain membership reason, not invented translation reason");
 }
 // Full matching trusts the previous ID map whenever previous.matched is set.
 // Independently reconstructing an assumed map must not prove rejection.
 for(unsigned malformed=0;malformed<3;++malformed){
  seed(q->workloads[0],7,8,0);seed(q->workloads[1],7,8,0);auto p=frame(0),n=frame(1);p.matched=true;
  auto &cd=q->workloads[1].drawData;cd.worldTransforms[0][3][0]=8;cd.viewTransforms[0][3][0]=-2;
  auto &ids=q->workloads[0].transformIdMap;
  if(malformed==1)ids.emplace(0x52510000u,1u);
  if(malformed==2){ids.emplace(0x52510000u,0u);ids.emplace(0x52510000u,1u);}
  check(!RR64TranslationRejection::reject(true,n,p,*q),"Malformed trusted ID map must defer");match(n,p,*q);++deferred;
 }
 // A different current scene can copy its SKIP camera mapping onto an AUTO
 // view. Its world/view translations then both hold and are coherent.
 seed(q->workloads[0],1,8,0);seed(q->workloads[1],1,8,0);auto multiPrev=frame(0),multiCur=frame(1);
 for(unsigned w=0;w<2;++w){
  auto &d=q->workloads[w].drawData;auto&pair=q->workloads[w].fbPairs[0];
  TransformGroup camera;camera.matrixId=0x77770000;camera.positionInterpolation=G_EX_COMPONENT_SKIP;d.transformGroups.push_back(camera);
  d.transformGroups[1].positionInterpolation=G_EX_COMPONENT_SKIP;
  d.viewTransforms.resize(2,hlslpp::float4x4::identity());d.projTransforms.resize(2,hlslpp::float4x4::identity());d.viewProjTransforms.resize(2,hlslpp::float4x4::identity());d.viewProjTransformGroups={2,0};
  pair.projections.push_back(pair.projections[0]);pair.projections[1].transformsIndex=1;pair.projectionCount=2;
  if(w){d.worldTransforms[0][3][0]=8;d.viewTransforms[0][3][0]=-2;d.viewTransforms[1][3][0]=-2;}
 }
 multiPrev.perspectiveScenes={GameScene{{{0,0,0}}},GameScene{{{0,0,1}}}};
 multiCur.perspectiveScenes={GameScene{{{1,0,0},{1,0,1}}},GameScene{{{1,0,1}}}};
 check(!RR64TranslationRejection::reject(true,multiCur,multiPrev,*q),"Any multi-projection current scene must defer");match(multiCur,multiPrev,*q);
 std::printf("copied-camera reasons=%u world=%u view0=%u view1=%u mapped=%u/%u/%u\n",multiCur.rr64GeometryRejectionReasons,multiCur.frameMap.workloads[1].transforms[0].rigidBody.lerpTranslation,multiCur.frameMap.workloads[1].viewProjections[0].rigidBody.lerpTranslation,multiCur.frameMap.workloads[1].viewProjections[1].rigidBody.lerpTranslation,multiCur.frameMap.workloads[1].transforms[0].mapped,multiCur.frameMap.workloads[1].viewProjections[0].mapped,multiCur.frameMap.workloads[1].viewProjections[1].mapped);
 check(multiCur.frameMap.workloads[1].viewProjections[1].mapped &&
     !multiCur.frameMap.workloads[1].viewProjections[1].rigidBody.lerpTranslation,
     "Actual AUTO view must inherit the other scene's held camera mapping");++deferred;
 seed(q->workloads[0],2000,80,0);seed(q->workloads[1],2000,80,0);auto p=frame(0),n=frame(1);
 q->workloads[1].drawData.worldTransforms[0][3][0]=8;q->workloads[1].drawData.viewTransforms[0][3][0]=-2;
 std::vector<double>times;
 for(unsigned i=0;i<70;++i){auto start=std::chrono::steady_clock::now();check(RR64TranslationRejection::reject(true,n,p,*q),"Timed proof failed");if(i>=10)times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}
 std::sort(times.begin(),times.end());std::printf("PASS actual-R22-oracles=%u proven=%u deferred=%u history-recovery=1 proof-median-ms=%.6f digest=%llu\n",proven+deferred,proven,deferred,times[times.size()/2],(unsigned long long)digest);
 std::vector<double> matchtimes;for(unsigned i=0;i<35;++i){auto f=frame(1);auto start=std::chrono::steady_clock::now();match(f,p,*q);if(i>=5)matchtimes.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());}std::sort(matchtimes.begin(),matchtimes.end());
 std::printf("matching-median-ms=%.6f\n",matchtimes[matchtimes.size()/2]);
 // One immutable workload can require a partial certificate for one frame
 // pair and a full certificate for another. Pair policy must not poison reuse.
 seed(q->workloads[0],7,8,0);seed(q->workloads[1],7,8,0);
 q->workloads[0].paused=q->workloads[1].paused=false;
 auto old=frame(0),moving=frame(1);
 q->workloads[1].drawData.worldTransforms[0][3][0]=8;
 q->workloads[1].drawData.viewTransforms[0][3][0]=-2;
 match(moving,old,*q);
 check(bool(q->workloads[1].rr64GeometryCache[1]),"Partial certificate not populated");
 q->workloads[1].drawData.worldTransforms[0][3][0]=0;
 q->workloads[1].drawData.viewTransforms[0][3][0]=0;
 auto still=frame(1);match(still,old,*q);
 check(still.rr64InterpolationCompatible && q->workloads[1].rr64GeometryCache[0],"Partial evidence reused as full");
 // Paused editing is explicitly uncached, including leaving pause afterwards.
 q->workloads[1].paused=true;
 q->workloads[1].drawData.posFloats[0]=std::numeric_limits<float>::quiet_NaN();
 auto edited=frame(1);match(edited,old,*q);
 check((edited.rr64GeometryRejectionReasons&2)!=0,"Paused geometry edit used stale evidence");
 check(!q->workloads[1].rr64GeometryCache[0]&&!q->workloads[1].rr64GeometryCache[1],"Paused certificate retained");
 q->workloads[1].drawData.posFloats[0]=0;q->workloads[1].paused=false;
 auto resumed=frame(1);match(resumed,old,*q);check(resumed.rr64InterpolationCompatible,"Resume retained invalid certificate");
 seed(q->workloads[1],7,8,0);
 std::swap(q->workloads[1].drawData.faceIndices[1],q->workloads[1].drawData.faceIndices[2]);
 auto replaced=frame(1);match(replaced,old,*q);
 check((replaced.rr64GeometryRejectionReasons&16)!=0,"Reused allocation retained old topology");
 std::puts("PASS geometry certificate lifecycle, policy changes and paused edits");
 // Reproduce a small direction reversal with real frame matching, not just the
 // policy helper. Compare explicit world tags with unrelated actors and bounds.
 for(unsigned id:{0x52510000u,0x52522690u,0x52517fffu,0x52518000u,0x12345678u})
 for(float delta:{0.02f,4.9f,5.0f,50.0f}) {
  seed(q->workloads[0],1,8,0);seed(q->workloads[1],1,8,0);
  for(unsigned w=0;w<2;w++)q->workloads[w].drawData.transformGroups[1].matrixId=id;
  auto previous=frame(0),current=frame(1);previous.matched=true;
  auto &history=previous.frameMap.workloads[0];history.mapped=true;history.transforms.resize(1);history.viewProjections.resize(1);
  history.transforms[0].rigidBody.linearVelocity={-1,0,0};
  previous.buildTransformIdMap(q->workloads[0],q->workloads[0].transformIdMap,q->workloads[0].transformIgnoredIds);
  q->workloads[1].drawData.worldTransforms[0][3][0]=delta;
  q->workloads[1].drawData.viewTransforms[0][3][0]=-0.2f;
  const bool expected=(id==0x52510000u||id==0x52522690u||id==0x52517fffu)&&delta<5;
  const bool rejected=RR64TranslationRejection::reject(true,current,previous,*q);
  match(current,previous,*q);
  const auto &body=current.frameMap.workloads[1].transforms[0].rigidBody;
  check(body.lerpTranslation==expected,"Small world reversal policy or actor isolation failed");
  check(current.rr64InterpolationCompatible==expected,"Full scene did not agree with small-world motion");
  check(rejected!=expected,"Early proof diverged from full matcher");
  check(std::abs(body.linearVelocity[0]-delta)<0.00001f,"AUTO velocity history was discarded");
 }
 TransformGroup staticGroup;staticGroup.matrixId=0x52520000;staticGroup.ordering=G_EX_ORDER_LINEAR;
 auto origin=hlslpp::float4x4::identity(),invalidMatrix=origin;
 invalidMatrix[3][0]=std::numeric_limits<float>::quiet_NaN();
 check(!RR64StaticWorldMotion::smallTranslation(staticGroup,origin,invalidMatrix),"Nonfinite translation accepted");
 staticGroup.positionInterpolation=G_EX_COMPONENT_SKIP;
 check(!RR64StaticWorldMotion::smallTranslation(staticGroup,origin,origin),"Explicit hold policy overridden");
 std::puts("PASS small static-world reversal, actor isolation, jump boundary and early-proof agreement");
 capturedTranslationRegressions(*q);
 shadowIdentityRegressions(*q);
 unmarkedCameraRegression(*q);
 qualifiedMotionRegressions(*q);
}
