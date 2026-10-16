#!/usr/bin/env python3
"""Build pinned minimal ncnn and convert the official MIT Spleeter vocal weights.
Requires Python with onnx and pnnx==20260526, git, cmake, ninja, Android NDK.
Large upstream downloads live only in ignored native/third_party.
"""
import hashlib,os,pathlib,subprocess,tarfile,urllib.request,shutil,importlib.util,sys
ROOT=pathlib.Path(__file__).resolve().parent
WORK=ROOT/'native/third_party';WORK.mkdir(parents=True,exist_ok=True)
def run(*args,**kw):subprocess.run([str(x) for x in args],check=True,**kw)
def fetch(url,target,sha):
 if not target.exists():urllib.request.urlretrieve(url,target)
 if hashlib.sha256(target.read_bytes()).hexdigest()!=sha:raise RuntimeError('SHA256 mismatch: '+str(target))
archive=WORK/'spleeter-fp32.tar.bz2'
fetch('https://github.com/k2-fsa/sherpa-onnx/releases/download/source-separation-models/sherpa-onnx-spleeter-2stems.tar.bz2',archive,'e26401d9c1801f43c0229731d78d32c2e80085e3aceedeb25f29d2de5fa68ca2')
with tarfile.open(archive) as t:t.extractall(WORK,filter='data')
import onnx
model=onnx.load(WORK/'sherpa-onnx-spleeter-2stems/vocals.onnx')
# Six U-Net downsamples need a multiple of 64 time frames. Keep trained 44100-Hz STFT;
# only inference context is shortened. The upstream 41000 metadata is not a playback rate.
for value in list(model.graph.input)+list(model.graph.output):
 dims=value.type.tensor_type.shape.dim
 if len(dims)==4:
  dims[0].dim_value=2;dims[1].dim_value=1;dims[2].dim_value=64
model.graph.ClearField('value_info');converted=WORK/'vocals.fp32.64.onnx';onnx.save(model,converted)
spec=importlib.util.find_spec('pnnx');binary=pathlib.Path(spec.origin).parent/'pnnx'
run(binary,converted,'inputshape=[2,1,64,1024]','fp16=1',cwd=WORK)
(ROOT/'models').mkdir(exist_ok=True)
for suffix in ('param','bin'):shutil.copyfile(WORK/f'vocals.fp32.64.ncnn.{suffix}',ROOT/f'models/vocals.{suffix}')
if hashlib.sha256((ROOT/'models/vocals.bin').read_bytes()).hexdigest()!='401c9ca0845137852eebcd3961c5754fb7e502d995cc81d15ee5812620817bf9':raise RuntimeError('Unexpected converted model; use pinned pnnx 20260526')
if '--model-only' in sys.argv:sys.exit(0)
source=WORK/'ncnn/upstream';build=WORK/'ncnn/build';prefix=WORK/'ncnn/out'
if not (source/'CMakeLists.txt').exists():run('git','clone','--depth','1','--branch','20260526','https://github.com/Tencent/ncnn.git',source)
commit=subprocess.check_output(['git','-C',str(source),'rev-parse','HEAD'],text=True).strip()
if commit!='e54f7b1f88434e1d844ea0551b880a1cfb079ce1':raise RuntimeError('Unexpected ncnn revision')
sdk=pathlib.Path(os.environ.get('ANDROID_HOME',str(pathlib.Path.home()/'Android/Sdk')))
args=['cmake','-S',source,'-B',build,'-G','Ninja',f'-DCMAKE_TOOLCHAIN_FILE={sdk}/ndk/28.2.13676358/build/cmake/android.toolchain.cmake','-DANDROID_ABI=arm64-v8a','-DANDROID_PLATFORM=android-26','-DANDROID_STL=c++_static','-DCMAKE_BUILD_TYPE=Release',f'-DCMAKE_INSTALL_PREFIX={prefix}']
for name in ('OPENMP','VULKAN','PIXEL','PIXEL_ROTATE','PIXEL_AFFINE','PIXEL_DRAWING','C_API','INT8','BF16','BUILD_TOOLS','BUILD_EXAMPLES','BUILD_BENCHMARK','BUILD_TESTS','ARM82DOT','ARM84BF16','ARM84I8MM','ARM86SVE'):args.append(f'-DNCNN_{name}=OFF')
args+=['-DNCNN_ARM82=ON']
# Also retain internal fp16 Deconvolution -> Gemm and fused Sigmoid dependencies.
enabled={'input','split','convolution','deconvolution','crop','concat','binaryop','relu','batchnorm','cast','packing','padding','reshape','flatten','memorydata','sigmoid','gemm'}
import re
for name in re.findall(r'ncnn_add_layer\((\w+)',(source/'src/CMakeLists.txt').read_text()):args.append(f'-DWITH_LAYER_{name.lower()}={"ON" if name.lower() in enabled else "OFF"}')
run(*args);run('cmake','--build',build,'--parallel',os.environ.get('LYRA_CHECK_JOBS','4'));run('cmake','--install',build)
