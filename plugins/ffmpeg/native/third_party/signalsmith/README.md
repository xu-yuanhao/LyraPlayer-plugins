# Signalsmith dependencies

Unmodified upstream headers used by the native audio transition processor:

- Stretch: https://github.com/Signalsmith-Audio/signalsmith-stretch
  commit a670068d9aeb64913331d5cc29337b19a457a7df
- Linear: https://github.com/Signalsmith-Audio/linear
  tag 0.6.4, commit de55e6a50ffcf6f8f43f649692d94691c7025151

Both are MIT licensed. Their individual license texts are kept here and in
config/defaults/licenses for inclusion in the APK. Only runtime headers are
vendored; reference repositories and experiment binaries remain in reference/.
