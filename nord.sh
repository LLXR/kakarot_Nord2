#!/bin/bash
echo "Setting Up Environment"
echo ""

#logo
echo -e "\n"
LOGO="\n ██ ▄█▀▄▄▄       ██ ▄█▀▄▄▄       ██▀███   ▒█████  ▄▄▄█████▓\n ██▄█▒▒████▄     ██▄█▒▒████▄    ▓██ ▒ ██▒▒██▒  ██▒▓  ██▒ ▓▒\n▓███▄░▒██  ▀█▄  ▓███▄░▒██  ▀█▄  ▓██ ░▄█ ▒▒██░  ██▒▒ ▓██░ ▒░\n▓██ █▄░██▄▄▄▄██ ▓██ █▄░██▄▄▄▄██ ▒██▀▀█▄  ▒██   ██░░ ▓██▓ ░ \n▒██▒ █▄▓█   ▓██▒▒██▒ █▄▓█   ▓██▒░██▓ ▒██▒░ ████▓▒░  ▒██▒ ░ \n▒ ▒▒ ▓▒▒▒   ▓▒█░▒ ▒▒ ▓▒▒▒   ▓▒█░░ ▒▓ ░▒▓░░ ▒░▒░▒░   ▒ ░░   \n░ ░▒ ▒░ ▒   ▒▒ ░░ ░▒ ▒░ ▒   ▒▒ ░  ░▒ ░ ▒░  ░ ▒ ▒░     ░    \n░ ░░ ░  ░   ▒   ░ ░░ ░  ░   ▒     ░░   ░ ░ ░ ░ ▒    ░      \n░  ░        ░  ░░  ░        ░  ░   ░         ░ ░           \n                                           Coded by Neel0210"
echo -e "$LOGO\n"

export ARCH=arm64
export SUBARCH=arm64
export ANDROID_MAJOR_VERSION=r
export PLATFORM_VERSION=11.0.0

# Device
export NORD=oplus6893_defconfig

# Export KBUILD flags
export KBUILD_BUILD_USER=neel0210
export KBUILD_BUILD_HOST=hell

# CCACHE
export CCACHE="$(which ccache)"
export USE_CCACHE=1
ccache -M 50G
export CCACHE_COMPRESS=1

# TC LOCAL PATH
export CROSS_COMPILE=$(pwd)/gcc/bin/aarch64-linux-android-
export CLANG_TRIPLE=$(pwd)/clang/bin/aarch64-linux-gnu-
export CC=$(pwd)/clang/bin/clang

# Check if have gcc & clang and AK
if [ ! -d gcc ]; then
   git clone --depth 1 git://github.com/LineageOS/android_prebuilts_gcc_linux-x86_aarch64_aarch64-linux-android-4.9 gcc
fi

if [ ! -d clang ]; then
  git clone --depth 1 https://github.com/kdrag0n/proton-clang.git clang
fi

# Trigger Oplus script
. oplus_native_features.sh
clear
# Calculate compilation time
  rm -rf NORD
#  rm ./NORD/arch/arm64/boot/Image
echo "================"
echo "Compiling Kernel"
echo "================"
  echo -e "$LOGO\n"
  make $NORD O=NORD CC=clang
  make -j16 O=NORD CC=clang
echo -e "Kernel Compiled"
