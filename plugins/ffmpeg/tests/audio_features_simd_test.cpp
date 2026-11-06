#include "playback/audio_features.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>
#define API(n) extern "C" void*n##_create(int,AudioFeatureSettings); extern "C" void n##_feed(void*,const int16_t*,int,int64_t,int64_t); extern "C" void n##_sample(void*,int64_t,bool,AudioFeatures*,bool*); extern "C" void n##_destroy(void*);
API(reference) API(neon)
static double cpu_time() {
    timespec value;clock_gettime(CLOCK_THREAD_CPUTIME_ID,&value);
    return value.tv_sec + value.tv_nsec * 1e-9;
}
static float maximum_error = 0;
static size_t comparisons = 0;
static void compare(void *a, void *b, int64_t media, int64_t output) {
    for(bool clock:{false,true}) {
        AudioFeatures x,y;bool ready_x=false,ready_y=false;
        reference_sample(a,clock?output:media,clock,&x,&ready_x);
        neon_sample(b,clock?output:media,clock,&y,&ready_y);
        assert(ready_x==ready_y);
        float errors[]{x.energy-y.energy,x.bass-y.bass,x.mid-y.mid,x.high-y.high,x.accent-y.accent};
        for(float error:errors){assert(std::isfinite(error));maximum_error=std::max(maximum_error,std::abs(error));}
        ++comparisons;
    }
}
int main() {
    const AudioFeatureSettings settings{180,2400,10,1};
    for(int rate:{8000,44100,48000,96000,192000})for(int pattern=0;pattern<5;++pattern) {
        std::vector<int16_t> input(rate*2);uint32_t random=17;
        for(int i=0;i<rate;++i) {
            random=random*1664525+1013904223;
            int16_t value=pattern==0?0:pattern==1?(i%2?INT16_MAX:INT16_MIN):pattern==2?(i==0?INT16_MAX:0):pattern==3?int16_t(std::sin(i*6.283185307179586*72/rate)*30000):int16_t(random>>16);
            input[i*2]=value;input[i*2+1]=pattern==3?int16_t(-value):int16_t(random);
            if(pattern==0)input[i*2+1]=0;
        }
        auto original=input;
        for(int chunk:{1,7,317,4096}) {
            void*a=reference_create(rate,settings),*b=neon_create(rate,settings);
            // Nonzero, independent media/output clocks; repeated data wraps history.
            for(int repeat=0;repeat<7;++repeat)for(int at=0;at<rate;at+=chunk) {
                int count=std::min(chunk,rate-at);int64_t media=int64_t(rate)*(17+repeat)+at,output=int64_t(rate)*(31+repeat)+at;
                reference_feed(a,input.data()+at*2,count,media,output);
                neon_feed(b,input.data()+at*2,count,media,output);
                compare(a,b,media+count-1,output+count-1);
            }
            assert(input==original);reference_destroy(a);neon_destroy(b);
        }
    }
    assert(maximum_error<=1.f/4095);
    printf("feature comparisons=%zu max_error=%.9g (quantization=%.9g)\n",comparisons,maximum_error,1.f/4095);
    for(int rate:{44100,48000,192000}) {
        std::vector<int16_t> input(rate*2);uint32_t random=19;
        for(auto&x:input){random=random*1664525+1013904223;x=int16_t(random>>16);}
        void*a=reference_create(rate,settings),*b=neon_create(rate,settings);
        std::vector<double> times[2];
        for(int round=0;round<7;++round)for(int order=0;order<2;++order) {
            int mode=(round+order)%2;double start=cpu_time();
            for(int repeat=0;repeat<16;++repeat)for(int at=0;at<rate;at+=256) {
                int count=std::min(256,rate-at);int64_t clock=int64_t(repeat)*rate+at;
                if(mode)neon_feed(b,input.data()+at*2,count,clock,clock);
                else reference_feed(a,input.data()+at*2,count,clock,clock);
            }
            times[mode].push_back(cpu_time()-start);
        }
        for(auto&values:times)std::sort(values.begin(),values.end());
        printf("rate=%d reference_cpu_ms=%.3f neon_cpu_ms=%.3f speedup=%.3f\n",rate,times[0][3]*1000,times[1][3]*1000,times[0][3]/times[1][3]);
        reference_destroy(a);neon_destroy(b);
    }
}
