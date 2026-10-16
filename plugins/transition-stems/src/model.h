#pragma once
#include "plugin/stem_contracts.h"
#include "third_party/signalsmith/linear/fft.h"
#include <net.h>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <vector>
#ifdef __ANDROID__
#include <android/dlext.h>
#include <fcntl.h>
#include <unistd.h>
#endif
namespace lyra_stems {
inline void *open_library(const std::string &path){
 void *p=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
#ifdef __ANDROID__
 if(!p){int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC);if(fd>=0){android_dlextinfo info{};info.flags=ANDROID_DLEXT_USE_LIBRARY_FD;info.library_fd=fd;p=android_dlopen_ext(path.c_str(),RTLD_NOW|RTLD_LOCAL,&info);close(fd);}}
#endif
 return p;
}
class Model {
 ncnn::Net net_;bool loaded_=false;
 signalsmith::linear::RealFFT<float> fft_{LYRA_STEM_FFT};
 std::vector<float> input_,output_,time_,inverse_,window_,norm_,magnitude_;
 std::vector<std::complex<float>> spectra_,frequency_;
public:
 ~Model(){release();}
 void release(){net_.clear();loaded_=false;input_.clear();input_.shrink_to_fit();output_.clear();output_.shrink_to_fit();spectra_.clear();spectra_.shrink_to_fit();magnitude_.clear();magnitude_.shrink_to_fit();}
 bool load(const std::string &model,const std::string &weights,int threads){
  if(loaded_)return true;
  net_.opt.num_threads=threads;net_.opt.lightmode=true;net_.opt.use_fp16_storage=true;net_.opt.use_fp16_arithmetic=true;net_.opt.use_fp16_packed=true;
  if(net_.load_param(model.c_str()) || net_.load_model(weights.c_str())){net_.clear();return false;}loaded_=true;
  time_.resize(LYRA_STEM_FFT);inverse_.resize(LYRA_STEM_FFT);frequency_.resize(LYRA_STEM_FFT/2);window_.resize(LYRA_STEM_FFT);
  spectra_.resize(2*LYRA_STEM_SPECTRAL_FRAMES*(LYRA_STEM_FFT/2));magnitude_.resize(2*LYRA_STEM_SPECTRAL_FRAMES*LYRA_STEM_BINS);
  output_.resize(2*LYRA_STEM_INPUT_FRAMES);norm_.resize(LYRA_STEM_INPUT_FRAMES);
  for(int i=0;i<LYRA_STEM_FFT;++i)window_[i]=float(.5-.5*std::cos(6.283185307179586*i/LYRA_STEM_FFT));
  return true;
 }
 const std::vector<float> &output()const{return output_;}
 bool process(const float *pcm,size_t samples){
  if(!loaded_ || samples!=size_t(2*LYRA_STEM_INPUT_FRAMES))return false;
  for(size_t i=0;i<samples;++i)if(!std::isfinite(pcm[i]) || std::abs(pcm[i])>1.001f)return false;
  for(int c=0;c<2;++c)for(int f=0;f<LYRA_STEM_SPECTRAL_FRAMES;++f){
   for(int i=0;i<LYRA_STEM_FFT;++i)time_[i]=pcm[2*(f*LYRA_STEM_FFT_HOP+i)+c]*window_[i];
   auto *spec=spectra_.data()+(c*LYRA_STEM_SPECTRAL_FRAMES+f)*(LYRA_STEM_FFT/2);fft_.fft(time_.data(),spec);
   auto *mag=magnitude_.data()+(c*LYRA_STEM_SPECTRAL_FRAMES+f)*LYRA_STEM_BINS;mag[0]=std::abs(spec[0].real());for(int b=1;b<LYRA_STEM_BINS;++b)mag[b]=std::abs(spec[b]);
  }
  ncnn::Mat input(LYRA_STEM_BINS,LYRA_STEM_SPECTRAL_FRAMES,2,magnitude_.data());ncnn::Mat result;
  auto extractor=net_.create_extractor();if(extractor.input("in0",input) || extractor.extract("out0",result))return false;
  bool ok=result.w==LYRA_STEM_BINS && result.h==LYRA_STEM_SPECTRAL_FRAMES && result.c==2 && result.elemsize==sizeof(float);
  std::fill(output_.begin(),output_.end(),0);std::fill(norm_.begin(),norm_.end(),0);
  if(ok)for(int c=0;c<2;++c)for(int f=0;f<LYRA_STEM_SPECTRAL_FRAMES;++f){
   auto *spec=spectra_.data()+(c*LYRA_STEM_SPECTRAL_FRAMES+f)*(LYRA_STEM_FFT/2);auto *mag=magnitude_.data()+(c*LYRA_STEM_SPECTRAL_FRAMES+f)*LYRA_STEM_BINS;const float *estimate=result.channel(c).row(f);
   std::fill(frequency_.begin(),frequency_.end(),std::complex<float>{});
   for(int b=0;b<LYRA_STEM_BINS;++b){if(!std::isfinite(estimate[b])){ok=false;break;}float mask=mag[b]>0?std::clamp(estimate[b]/mag[b],0.f,1.f):0;frequency_[b]=b?spec[b]*mask:std::complex<float>(spec[0].real()*mask,0);}
   if(!ok)break;fft_.ifft(frequency_.data(),inverse_.data());
   for(int i=0;i<LYRA_STEM_FFT;++i){int at=f*LYRA_STEM_FFT_HOP+i;output_[at*2+c]+=inverse_[i]*window_[i]/LYRA_STEM_FFT;if(c==0)norm_[at]+=window_[i]*window_[i];}
  }
  if(!ok)return false;
  for(int i=0;i<LYRA_STEM_INPUT_FRAMES;++i)for(int c=0;c<2;++c){float &v=output_[i*2+c];v=norm_[i]>1e-6f?v/norm_[i]:0;if(!std::isfinite(v))return false;}
  return true;
 }
};
}
