// Pure arithmetic port from n64decomp/mk64 commit 58cfcb022e10f83bc3b889d7e97508cae6837098.
// No cartridge data is embedded. All controls arrive from the user's ROM at import.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#define UNUSED
typedef int32_t s32;typedef int16_t s16;typedef uint16_t u16;typedef float f32;typedef double f64;
typedef struct {s16 pos[3],velocity;} SplineControlPoint;
typedef struct {s16 numControlPoints;SplineControlPoint controlPoints[64];} SplineData;
typedef struct {f32 offset[3],velocity[3];s16 unk_084[10],unk_09A;u16 animationTimer;unsigned flags;SplineData*spline;SplineControlPoint*controlPoints;} Object;
static Object gObjectList[1];static f32 D_80165760[4],D_80165770[4],D_80165780[4],D_80183DC8[4],D_80183DA8[4];
int is_obj_index_flag_status_inactive(int i,unsigned f){return !(gObjectList[i].flags&f);}
int is_obj_flag_status_active(int i,unsigned f){return !!(gObjectList[i].flags&f);}
void set_object_flag_status_true(int i,unsigned f){gObjectList[i].flags|=f;}
void set_object_flag_status_false(int i,unsigned f){gObjectList[i].flags&=~f;}
typedef struct {s16 posX,posY,posZ;uint16_t trackSectionId;} TrackPathPoint;
typedef struct {s16 x,z;} Path2D;
static int gIsMirrorMode=0;
static float separation;
#define GET_COURSE_AIMaximumSeparation separation
static TrackPathPoint points[4096],left[4096],right[4096],*gTrackPaths[]={points},*gTrackLeftPaths[]={left},*gTrackRightPaths[]={right};
static unsigned gPathCountByPathIndex[1];static Path2D processed[65536];
void func_8008ACE0(f32 arg0[], f32 arg1) {
    arg0[0] = (f32) ((f64) ((f32) (1.0 - arg1) * (f32) (1.0 - arg1) * (f32) (1.0 - arg1)) / 6.0);
    arg0[1] = (f32) ((((f64) (arg1 * arg1 * arg1) * 0.5) - arg1 * arg1) + 0.6666666666666666);
    arg0[2] = (f32) (((f64) (arg1 * arg1 * arg1) * -0.5) + (0.5 * (arg1 * arg1)) + (0.5 * arg1) + 0.16666666666666666);
    arg0[3] = (f32) ((f64) (arg1 * arg1 * arg1) / 6.0);
}
void func_8008ADD0(f32 arg0[], f32 arg1) {
    arg0[0] = (f32) (1.0 - arg1) * -0.5 * (f32) (1.0 - arg1);
    arg0[1] = arg1 * arg1 * 1.5 - 2.0 * arg1;
    arg0[2] = (arg1 * arg1 * 3.0 - 2.0 * arg1 - (f32) 1.0) * -0.5;
    arg0[3] = arg1 * arg1 * 0.5;
}
void func_8008AE9C(s32 objectIndex) {
    Object* object;

    object = &gObjectList[objectIndex];
    object->velocity[0] = (D_80183DC8[0] * D_80165760[0]) + (D_80183DC8[1] * D_80165760[1]) +
                          (D_80183DC8[2] * D_80165760[2]) + (D_80183DC8[3] * D_80165760[3]);
    object->velocity[1] = (D_80183DC8[0] * D_80165770[0]) + (D_80183DC8[1] * D_80165770[1]) +
                          (D_80183DC8[2] * D_80165770[2]) + (D_80183DC8[3] * D_80165770[3]);
    object->velocity[2] = (D_80183DC8[0] * D_80165780[0]) + (D_80183DC8[1] * D_80165780[1]) +
                          (D_80183DC8[2] * D_80165780[2]) + (D_80183DC8[3] * D_80165780[3]);
}
void func_8008AFE0(s32 objectIndex, f32 arg1) {
    func_8008ADD0(D_80183DC8, arg1);
    func_8008AE9C(objectIndex);
}
void func_8008B038(s32 objectIndex) {
    Object* object;

    object = &gObjectList[objectIndex];
    object->offset[0] = (D_80183DA8[0] * D_80165760[0]) + (D_80183DA8[1] * D_80165760[1]) +
                        (D_80183DA8[2] * D_80165760[2]) + (D_80183DA8[3] * D_80165760[3]);
    object->offset[1] = (D_80183DA8[0] * D_80165770[0]) + (D_80183DA8[1] * D_80165770[1]) +
                        (D_80183DA8[2] * D_80165770[2]) + (D_80183DA8[3] * D_80165770[3]);
    object->offset[2] = (D_80183DA8[0] * D_80165780[0]) + (D_80183DA8[1] * D_80165780[1]) +
                        (D_80183DA8[2] * D_80165780[2]) + (D_80183DA8[3] * D_80165780[3]);
}
void func_8008B17C(s32 objectIndex, f32 arg1) {
    func_8008ACE0(D_80183DA8, arg1);
    func_8008B038(objectIndex);
}
void func_8008B1D4(s32 objectIndex) {
    s32 someIndex;
    SplineControlPoint* test;

    test = gObjectList[objectIndex].controlPoints;
    for (someIndex = 0; someIndex < 4; someIndex++) {
        D_80165760[someIndex] = test->pos[0];
        D_80165770[someIndex] = test->pos[1];
        D_80165780[someIndex] = test->pos[2];
        test++;
    }
}
void func_8008B284(s32 objectIndex) {
    s32 someIndex;
    s32 sp0;
    s32 temp_a1;
    s32 temp_a2;
    SplineControlPoint* test;

    test = gObjectList[objectIndex].controlPoints;
    temp_a1 = gObjectList[objectIndex].unk_084[9];
    temp_a2 = (u16) gObjectList[objectIndex].unk_084[8];
    if ((temp_a2 - 4) >= temp_a1) {
        sp0 = 10000;
    } else if ((temp_a1 + 3) == temp_a2) {
        sp0 = 2;
    } else if ((temp_a1 + 2) == temp_a2) {
        sp0 = 1;
    } else if ((temp_a1 + 1) == temp_a2) {
        sp0 = 0;
    }
    for (someIndex = 0; someIndex < 4; someIndex++) {
        D_80165760[someIndex] = test->pos[0];
        D_80165770[someIndex] = test->pos[1];
        D_80165780[someIndex] = test->pos[2];
        if (sp0 == someIndex) {
            // Reset back to start of the spline path
            test = gObjectList[objectIndex].spline->controlPoints;
        } else {
            test++;
        }
    }
}
void func_8008B3E4(s32 objectIndex) {
    Object* object;
    UNUSED SplineData* spline;

    if (is_obj_index_flag_status_inactive(objectIndex, 8) != 0) {
        object = &gObjectList[objectIndex];
        object->unk_084[9] = 0;
        object->animationTimer = 0;
        object->controlPoints = object->spline->controlPoints;
        /*
        This is INCREDIBLY stupid. This should really be
        temp_v0->unk_084[8] = temp_v0->spline->numControlPoints;
        but for some unholy reason that doesn't match
        */
        object->unk_084[8] = *(((s16*) object->controlPoints) - 1);

        set_object_flag_status_true(objectIndex, 8);
    }
}
void func_8008B44C(s32 objectIndex) {
    gObjectList[objectIndex].animationTimer = 0;
    gObjectList[objectIndex].controlPoints++;
}
void func_8008B478(s32 objectIndex, s32 arg1) {
    f32 sp34;
    f32 temp;
    UNUSED f32 temp2;
    f32 var_f6;

    func_8008B3E4(objectIndex);
    if (arg1 != 0) {
        func_8008B284(objectIndex);
    } else {
        func_8008B1D4(objectIndex);
    }

    // I think the game treats each spline as being having a lenght of 10000
    // This is getting the percent along the spline we want to reach,
    // which is then treated as the `t` value given to the curve calculations
    sp34 = ((f32) gObjectList[objectIndex].animationTimer / 10000.0);
    // Calculate the curve at `t`
    func_8008B17C(objectIndex, sp34);
    if (is_obj_flag_status_active(objectIndex, 0x800) != 0) {
        // Calculate the curve's derivative at `t`
        func_8008AFE0(objectIndex, sp34);
    }

    // These values somehow control how fast we travel along the curve
    var_f6 = gObjectList[objectIndex].controlPoints[0].velocity;
    temp = gObjectList[objectIndex].controlPoints[1].velocity;

    gObjectList[objectIndex].unk_09A = 10000.0 / (((temp - var_f6) * sp34) + var_f6);
    gObjectList[objectIndex].animationTimer += gObjectList[objectIndex].unk_09A;
}
void func_8008B6A4(s32 objectIndex) {
    Object* object;

    func_8008B478(objectIndex, 1);
    object = &gObjectList[objectIndex];
    if (object->animationTimer >= 0x2710) {
        // Have to do it this way due to the u16 cast
        object->unk_084[9] = (u16) object->unk_084[9] + 1;
        if ((u16) object->unk_084[9] == (u16) object->unk_084[8]) {
            set_object_flag_status_false(objectIndex, 8);
        } else {
            func_8008B44C(objectIndex);
        }
    }
}
s32 generate_2d_path(Path2D* pathDest, TrackPathPoint* pathSrc, s32 numPathPoints) {
    f32 temp_f14_3;
    f32 temp_f16_2;
    UNUSED s32 pad;

    f32 x1;
    f32 z1;
    f32 x2;
    f32 z2;
    f32 x3;
    f32 z3;

    UNUSED s32 pad2;
    f32 temp_f24;

    f32 spA8;
    f32 temp_f26;
    f32 spA0;

    f32 temp_f2_3;

    TrackPathPoint* point1;
    f32 j;
    TrackPathPoint* point2;
    TrackPathPoint* point3;
    s32 i;
    f32 temp_f6 = 0.0f;
    s32 nbElement;
    unsigned work = 0;
    f32 sp7C;

    spA8 = pathSrc[0].posX;
    spA0 = pathSrc[0].posZ;
    nbElement = 0;

    for (i = 0; i < numPathPoints; i++) {
        point1 = &pathSrc[((i % numPathPoints))];
        point2 = &pathSrc[(((i + 1) % numPathPoints))];
        point3 = &pathSrc[(((i + 2) % numPathPoints))];
        x1 = point1->posX;
        z1 = point1->posZ;
        x2 = point2->posX;
        z2 = point2->posZ;
        x3 = point3->posX;
        z3 = point3->posZ;

        sp7C = 0.05 / (sqrtf(((x2 - x1) * (x2 - x1)) + ((z2 - z1) * (z2 - z1))) +
                       sqrtf(((x3 - x2) * (x3 - x2)) + ((z3 - z2) * (z3 - z2))));

        for (j = 0.0f; j <= 1.0; j += sp7C) {
            if (++work > 30000000) return -1;
            temp_f2_3 = (1.0 - j) * 0.5 * (1.0 - j);
            temp_f14_3 = ((1.0 - j) * j) + 0.5;
            temp_f16_2 = j * 0.5 * j;

            temp_f24 = (temp_f2_3 * x1) + (temp_f14_3 * x2) + (temp_f16_2 * x3);
            temp_f26 = (temp_f2_3 * z1) + (temp_f14_3 * z2) + (temp_f16_2 * z3);
            temp_f6 += sqrtf(((temp_f24 - spA8) * (temp_f24 - spA8)) + ((temp_f26 - spA0) * (temp_f26 - spA0)));
            spA8 = temp_f24;
            spA0 = temp_f26;
            if ((temp_f6 > 20.0f) || ((i == 0) && (j == 0.0))) {
                if (nbElement >= 65536) return -1;
                if (gIsMirrorMode) {
                    pathDest->x = (s16) -spA8;
                } else {
                    pathDest->x = (s16) spA8;
                }
                pathDest->z = spA0;
                nbElement += 1;
                pathDest++;
                temp_f6 = 0.0f;
            }
        }
    }
    return nbElement;
}
void calculate_track_boundaries(s32 pathIndex) {
    f32 pathPointWidth;
    f32 x1;
    f32 y1;
    f32 z1;
    f32 x2;
    f32 y2;
    f32 z2;
    f32 x_dist;
    f32 z_dist;
    f32 neg_x_dist;
    f32 neg_z_dist;
    f32 xz_dist;
    s32 temp_f16;
    s32 pathPointIndex;
    TrackPathPoint* pathPoint;
    TrackPathPoint* nextPathPoint;
    TrackPathPoint* var_s1;
    TrackPathPoint* var_s2;

    if (((s32) GET_COURSE_AIMaximumSeparation) >= 0) {
        pathPointWidth = GET_COURSE_AIMaximumSeparation;
        pathPoint = &gTrackPaths[pathIndex][0];
        var_s1 = &gTrackLeftPaths[pathIndex][0];
        var_s2 = &gTrackRightPaths[pathIndex][0];
        for (pathPointIndex = 0; pathPointIndex < gPathCountByPathIndex[pathIndex];
             pathPointIndex++, var_s1++, var_s2++) {
            x1 = pathPoint->posX;
            y1 = pathPoint->posY;
            z1 = pathPoint->posZ;
            pathPoint++;
            nextPathPoint = &gTrackPaths[pathIndex][(pathPointIndex + 1) % ((s32) gPathCountByPathIndex[pathIndex])];
            x2 = nextPathPoint->posX;
            y2 = nextPathPoint->posY;
            z2 = nextPathPoint->posZ;
            x_dist = x2 - x1;
            z_dist = z2 - z1;
            neg_x_dist = x1 - x2;
            neg_z_dist = z1 - z2;
            xz_dist = sqrtf((x_dist * x_dist) + (z_dist * z_dist));
            temp_f16 = (f32) ((y1 + y2) * 0.5);

            // Calculate left boundary position
            // Uses perpendicular vector (Z, -X) normalized by segment length
            var_s1->posX = ((pathPointWidth * z_dist) / xz_dist) + x1;
            var_s1->posY = temp_f16;
            var_s1->posZ = ((pathPointWidth * neg_x_dist) / xz_dist) + z1;

            // Calculate right boundary position
            // Uses opposite perpendicular vector (-Z, X)
            var_s2->posX = ((pathPointWidth * neg_z_dist) / xz_dist) + x1;
            var_s2->posY = temp_f16;
            var_s2->posZ = ((pathPointWidth * x_dist) / xz_dist) + z1;
        }
    }
}
int main(void) {
 char mode[16];int n,count,x,y,z,t;
 if(scanf("%15s %d",mode,&n)!=2)return 2;
 if(!strcmp(mode,"spline")) {
  static SplineData data;
  if(scanf("%d",&count)!=1||n<4||n>60||count<n+1||count>64)return 3;
  data.numControlPoints=(s16)n;
  for(int i=0;i<count;++i){if(scanf("%d%d%d%d",&x,&y,&z,&t)!=4||x<-32768||x>32767||y<-32768||y>32767||z<-32768||z>32767||t<=0||t>32767)return 4;data.controlPoints[i]=(SplineControlPoint){{(s16)x,(s16)y,(s16)z},(s16)t};}
  gObjectList[0].spline=&data;gObjectList[0].flags=0x800;count=0;
  do{func_8008B6A4(0);Object*o=gObjectList;printf("%.9g %.9g %.9g %.9g %.9g %.9g\n",o->offset[0],o->offset[1],o->offset[2],o->velocity[0],o->velocity[1],o->velocity[2]);if(++count>4096)return 5;}while(gObjectList[0].flags&8);
  return 0;
 }
 if(n<2||n>4096||scanf("%f",&separation)!=1||!isfinite(separation)||separation<0||separation>10000)return 6;
 for(int i=0;i<n;++i){if(scanf("%d%d%d%d",&x,&y,&z,&t)!=4||x<-32768||x>32767||y<-32768||y>32767||z<-32768||z>32767||t<0||t>65535)return 7;points[i]=(TrackPathPoint){(s16)x,(s16)y,(s16)z,(u16)t};}
 if(!strcmp(mode,"2d")){count=generate_2d_path(processed,points,n-1);if(count<=0||count>65536)return 8;for(int i=0;i<count;++i)printf("%d %d\n",processed[i].x,processed[i].z);return 0;}
 if(!strcmp(mode,"boundary")){gPathCountByPathIndex[0]=n;for(int i=0;i<n;++i)if(points[i].posX==points[(i+1)%n].posX&&points[i].posZ==points[(i+1)%n].posZ)return 9;calculate_track_boundaries(0);for(int i=0;i<n;++i)printf("%d %d %d %d %d %d\n",left[i].posX,left[i].posY,left[i].posZ,right[i].posX,right[i].posY,right[i].posZ);return 0;}
 return 10;
}
