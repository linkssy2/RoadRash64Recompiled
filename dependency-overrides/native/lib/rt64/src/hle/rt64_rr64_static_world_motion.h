// Native immutable world packets use stable IDs partitioned by camera. Their
// translations include camera rebasing, so a small direction reversal is not an
// actor teleport. Preserve the original five-unit small-motion bound before
// AUTO's direction penalty amplifies it. Large/invalid moves keep stock policy.
#pragma once
#include "rt64_transform_group.h"
#include "common/rt64_math.h"
#include <cmath>
namespace RT64::RR64StaticWorldMotion {
inline bool smallTranslation(const TransformGroup &group,
    const hlslpp::float4x4 &previous, const hlslpp::float4x4 &current) {
    const uint32_t id=group.matrixId;
    const bool nativeWorld=(id>=0x52510000u && id<0x52518000u) ||
        (id>=0x52520000u && id<0x52528000u);
    if(!nativeWorld || group.ordering!=G_EX_ORDER_LINEAR || group.positionInterpolation!=G_EX_COMPONENT_AUTO) return false;
    double squared=0;
    for(unsigned i=0;i<3;i++) {
        const float a=previous[3][i],b=current[3][i];
        if(!std::isfinite(a)||!std::isfinite(b)) return false;
        const double delta=double(b)-double(a); squared+=delta*delta;
    }
    return squared<25.0;
}

// AUTO's direction penalty magnifies a tiny reversal into a teleport. Only
// remove that penalty with a submitted phase AND a small actual camera change.
// A phase alone misses same-mode camera turns. Bounding ||inverse(A)*B-I||_F
// below 1/8 also keeps every linear blend of the two affine bases nonsingular.
inline bool nearbyAffine(const hlslpp::float4x4 &a,const hlslpp::float4x4 &b,double distance) {
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)
        if(!std::isfinite(float(a[row][col]))||!std::isfinite(float(b[row][col])))return false;
    for(unsigned row=0;row<4;++row)
        if(float(a[row][3])!=(row==3?1.0f:0.0f)||float(b[row][3])!=(row==3?1.0f:0.0f))return false;
    double squared=0;
    for(unsigned i=0;i<3;++i){double d=double(b[3][i])-double(a[3][i]);squared+=d*d;}
    if(squared>=distance*distance)return false;
    const auto basis=extract3x3(a);
    const float det=hlslpp::determinant(basis);
    if(!std::isfinite(det)||std::abs(det)<1e-6f)return false;
    const auto relative=hlslpp::mul(hlslpp::inverse(basis),extract3x3(b));
    double error=0;
    for(unsigned row=0;row<3;++row)for(unsigned col=0;col<3;++col){
        const double d=double(relative[row][col])-(row==col?1.0:0.0);
        if(!std::isfinite(d))return false;
        error+=d*d;
    }
    return error<1.0/64.0;
}
inline bool cameraQualifiedTranslation(const TransformGroup &world,const TransformGroup &view,
    const TransformGroup &oldView,const hlslpp::float4x4 &previous,const hlslpp::float4x4 &current,
    const hlslpp::float4x4 &previousView,const hlslpp::float4x4 &currentView,
    const hlslpp::float4x4 &previousProjection,const hlslpp::float4x4 &currentProjection) {
    if(world.positionInterpolation!=G_EX_COMPONENT_AUTO ||
        (view.matrixId&0xffff0000u)!=0x484c0000u || view.matrixId!=oldView.matrixId ||
        view.positionInterpolation!=G_EX_COMPONENT_INTERPOLATE)return false;
    const bool object=world.matrixId>=0x52520000u && world.matrixId<0x52528000u &&
        world.ordering==G_EX_ORDER_LINEAR;
    // Shadow ownership tags replace AUTO identity, not its coordinate units.
    // They retain the same camera certificate and five-unit bound as AUTO.
    const bool shadow=world.matrixId>0x52530000u && world.matrixId<=0x5253ffffu &&
        world.ordering==G_EX_ORDER_LINEAR;
    if(!object && !shadow && world.matrixId!=G_EX_ID_AUTO)return false;
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col)
        if(!std::isfinite(float(previousProjection[row][col])) ||
            float(previousProjection[row][col])!=float(currentProjection[row][col]))return false;
    // Source-2 static object writers scale translations by ten; terrain and
    // unrelated explicit actor tags do not share that coordinate contract.
    return nearbyAffine(previousView,currentView,5.0) &&
        nearbyAffine(previous,current,object?50.0:5.0);
}
}
