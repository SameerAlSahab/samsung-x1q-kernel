#!/bin/bash

KERNEL_DIR="$(pwd)"
GCC_PATH="/usr/bin/"
LLD_PATH="/usr/bin/"
KERNEL_NAME="not_kernel-"
MAKE="./makeparallel" 
KERNEL_MAKE_ENV="DTC_EXT=$(pwd)/tools/dtc CONFIG_BUILD_ARM64_DT_OVERLAY=y"

#install clang
mkdir -p clang
cd clang || exit 1
wget -q https://github.com/ZyCromerZ/Clang/releases/download/21.0.0git-20250228-release/Clang-21.0.0git-20250228.tar.gz
tar -xf Clang*
cd "$KERNEL_DIR" || exit 1
    PATH="${KERNEL_DIR}/clang/bin:$PATH"

MAKE_OPT+=(CROSS_COMPILE=aarch64-linux-gnu- CROSS_COMPILE_ARM32=arm-linux-gnueabi-)

rm -rf /home/skye/bomb/out/arch/arm64/boot/Image
rm -rf /home/skye/bomb/AnyKernel3/dtb
rm -rf /home/skye/bomb/dtbo.img
rm -rf .version
rm -rf .local
#make O=/home/skye/bomb/out clean
make O=/home/skye/bomb/out ARCH=arm64 LLVM=1 LLVM_IAS=1 "${MAKE_OPT[@]}" vendor/kona-not_defconfig vendor/samsung/y2q.config vendor/debugfs.config

echo "*****************************************"
echo "*****************************************"

make -j12 O=/home/skye/bomb/out ARCH=arm64 LLVM=1 LLVM_IAS=1 "${MAKE_OPT[@]}"  dtbs
DTB_OUT="/home/skye/bomb/out/arch/arm64/boot/dts/vendor/qcom"
cat $DTB_OUT/*.dtb > AnyKernel3/dtb

#make -j12 O=/home/skye/bomb/out $KERNEL_MAKE_ENV $BUILD_ENV dtbo.img
DTBO_OUT="/home/skye/bomb/out/arch/arm64/boot"
#cp $DTBO_OUT/dtbo.img /home/skye/bomb/dtbo.img
make -j12 O=/home/skye/bomb/out ARCH=arm64 LLVM=1 LLVM_IAS=1 "${MAKE_OPT[@]}" Image
IMAGE="/home/skye/bomb/out/arch/arm64/boot/Image"
echo "**Build outputs**"
ls /home/skye/bomb/out/arch/arm64/boot
echo "**Build outputs**"
cp $IMAGE AnyKernel3/Image

cd AnyKernel3
rm *.zip
zip -r9 ${KERNEL_NAME}$(date +"%Y%m%d")+y2q.zip .
echo "The bomb has been planted."
