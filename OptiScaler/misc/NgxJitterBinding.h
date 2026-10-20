#pragma once
#include "TemporalSampling.h"
#include <nvsdk_ngx.h>
namespace NgxJitterBinding {
class Resampled {
    NVSDK_NGX_Parameter* p;
    float x=0,y=0;bool haveX=false,haveY=false;
public:
    Resampled(NVSDK_NGX_Parameter* params,unsigned sourceW,unsigned sourceH,unsigned renderW,unsigned renderH):p(params) {
        if (!p || !sourceW || !sourceH || !renderW || !renderH || (sourceW==renderW && sourceH==renderH)) return;
        haveX=p->Get(NVSDK_NGX_Parameter_Jitter_Offset_X,&x)==NVSDK_NGX_Result_Success;
        haveY=p->Get(NVSDK_NGX_Parameter_Jitter_Offset_Y,&y)==NVSDK_NGX_Result_Success;
        if(haveX)p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,TemporalSampling::ResampledJitter(x,sourceW,renderW));
        if(haveY)p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,TemporalSampling::ResampledJitter(y,sourceH,renderH));
    }
    ~Resampled(){if(haveX)p->Set(NVSDK_NGX_Parameter_Jitter_Offset_X,x);if(haveY)p->Set(NVSDK_NGX_Parameter_Jitter_Offset_Y,y);}
    Resampled(const Resampled&)=delete;Resampled& operator=(const Resampled&)=delete;
};
}
