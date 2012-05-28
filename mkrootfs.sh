#!/bin/bash

function check_variant()
{
    if [ "$ANDROID" = "" ]; then
        echo "define \$ANDROID first."
        exit 0
    fi
    
#    if [ "$KERNELDIR" = "" ]; then
#        echo "define \$KERNELDIR first."
#        exit 0
#    fi
    
    DEVICEDIR=$ANDROID/device/samsung/nowplus
    OUTDIR=$DEVICEDIR/out
    rm -rf $OUTDIR
    mkdir -p $OUTDIR
}

function copy_modules()
{
    echo "copying kernel modules."
 
}

function create_rootfs()
{
    echo "creating rootfs."
    
    ANDROIDROOTFS=$OUTDIR/rootfs
    
    copy_modules
    
    mkdir -p $ANDROIDROOTFS
    mkdir -p $ANDROIDROOTFS/lib/firmware
    mkdir -p $ANDROIDROOTFS/system/lib/dsp
    cp -Rdpf $ANDROID/out/target/product/nowplus/root/* $ANDROIDROOTFS/
    cp -Rdpf $ANDROID/out/target/product/nowplus/system/* $ANDROIDROOTFS/system/
    cp -Rdpf $ANDROID/out/target/product/nowplus/data/* $ANDROIDROOTFS/data/
    cp -Rdpf $DEVICEDIR/prebuilt/xbin/* $ANDROIDROOTFS/system/xbin/
    cp -Rdpf $DEVICEDIR/proprietary/sgx/bin/* $ANDROIDROOTFS/system/bin/
    cp -Rdpf $DEVICEDIR/lib/firmware/* $ANDROIDROOTFS/lib/firmware/
    cp -Rdpf $DEVICEDIR/proprietary/sgx/lib/* $ANDROIDROOTFS/system/lib/
    cp -Rdpf $DEVICEDIR/proprietary/dsp/img/* $ANDROIDROOTFS/system/lib/dsp/
    cp -Rdpf $DEVICEDIR/proprietary/dsp/lib/* $ANDROIDROOTFS/system/lib/

    echo "creating rootfs.tar.bz2"
    cd $ANDROIDROOTFS
    sudo chown -R wtbdaaaa:users * 
    sudo chmod 777 -R *
    sudo chown root:users system/xbin/su

    tar jcpf ../rootfs.tar.bz2 ./
    
    echo "done."

}

function create_img()
{
    echo "creating image."
    
    ANDROIDIMG=$OUTDIR/img

    copy_modules
    
    #patch init.rc
    mkdir -p $ANDROIDIMG
    #mkcramfs $ANDROID/out/target/product/nowplus/root/ $ANDROIDIMG/initrd.cramfs
    #mkcramfs $ANDROID/out/target/product/nowplus/system/ $ANDROIDIMG/factoryfs.cramfs
    #undo patch init.rc
    
    echo "done."
}

check_variant
create_rootfs
#create_img

