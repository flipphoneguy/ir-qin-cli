#!/data/data/com.termux/files/usr/bin/bash
# libkksdk.so is 32-bit ARM only, so kkhost is built for armv7 against the system's 32-bit bionic
cd "$(dirname "$0")"
J=$PREFIX/lib/jvm/java-17-openjdk/include
clang --target=armv7a-linux-androideabi24 -O1 -nostdlib -fuse-ld=lld -fPIE -pie \
  -Wl,-dynamic-linker,/system/bin/linker -L/system/lib -I"$J" -I"$J/linux" \
  crt.c kkhost.c -o kkhost -lc -ldl
