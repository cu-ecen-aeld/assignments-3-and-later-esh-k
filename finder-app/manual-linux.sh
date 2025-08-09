#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    # clean 
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper 
    # create configuration
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    # build
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all 
    # build modules
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules
    # build device tree
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
fi

echo "Adding the Image in outdir"
# Missing: copy the image to outdir
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
# create staging directory rootfs
mkdir ${OUTDIR}/rootfs
cd rootfs
mkdir -p bin dev etc lib lib64 proc sys sbin var tmp usr var
mkdir -p usr/bin usr/lib usr/sbin
mkdir -p var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
else
    cd busybox
fi

# TODO: Make and install busybox
make distclean
make defconfig
make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
# create symlinks for apps to busybox
make CONFIG_PREFIX=${OUTDIR}/rootfs ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
# add program interpreter deps
find ${SYSROOT} -name ld-linux-aarch64.so.1 | xargs -I {} cp "{}" ${OUTDIR}/rootfs/lib
# add shared libraries
aarch64-none-linux-gnu-readelf -a busybox | grep "Shared library" | awk -F'[][]' '{print $2}' | xargs -I {} find ${SYSROOT} -name "{}" | xargs -I {} cp "{}" ${OUTDIR}/rootfs/lib64

# TODO: Make device nodes
cd ${OUTDIR}/rootfs
# add null device
sudo mknod -m 666 dev/null c 1 3
# add console device
sudo mknod -m 600 dev/console c 5 1

# TODO: Clean and build the writer utility
cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
FINDER_OUTDIR=${OUTDIR}/rootfs/home 
mkdir -p ${FINDER_OUTDIR}
cd ${FINDER_APP_DIR}
cp -RH * ${FINDER_OUTDIR}
mkdir -p ${FINDER_OUTDIR}/conf
cd conf
cp -RH * ${FINDER_OUTDIR}/conf
mkdir -p ${OUTDIR}/rootfs/conf
cp -RH * ${OUTDIR}/rootfs/conf
cd ${FINDER_OUTDIR}
chmod +x finder-test.sh
chmod +x finder.sh
chmod +x writer

# TODO: Chown the root directory
cd ${OUTDIR}/rootfs
sudo chown -R root:root *

# TODO: Create initramfs.cpio.gz
cd ${OUTDIR}/rootfs
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
gzip -f ${OUTDIR}/initramfs.cpio

