#!/bin/bash

cpuCore=$(cat /proc/cpuinfo | grep processor | wc -l)
max_process=`expr $cpuCore - 1`
if [ $max_process -gt 10 ]; then
    max_process=10
fi
if [ -z $4 ]; then
echo
echo usage:
echo "    $0 AQROOT buildTool buildConfig GPU_CONFIG [cpuCount|clean]"
echo 
echo "   AQROOT: vivante driver root path"
echo "   toolChain: x86_gcc|x86_x64_gcc"
echo "     x86_gcc: use native gcc to build 32bit offline compiler"
echo "     x86_x64_gcc: use native gcc to build 64bit offline compiler"
echo "   buildConfig: debug|release"
echo "   cpuCount: max count of compiling job at the same time: 1 ~ ${max_process}"
echo   
echo "e.g."
echo "    ./vxcBuild.sh `pwd` x86_gcc release vip8000|vip8000_40BITS_VA"
echo "    ./vxcBuild.sh `pwd` x86_x64_gcc release vip8000|vip8000_40BITS_VA clean"
echo "    ./vxcBuild.sh `pwd` x86_x64_gcc release vip8000|vip8000_40BITS_VA ${max_process}"
echo
exit 1
fi

export AQROOT=$1
export BUILD_TOOL=$2
export BUILD_CONFIG=$3
export GPU_CONFIG=$4
export GPU_CONFIG_WITHOUT_VA_FLAG=$4
export MORE=$5
export GPU_CONFIG_FILE=$AQROOT/compiler/vclcompiler/viv_gpu.config

ENABLE_40BITS_VA=0
if [ "${GPU_CONFIG:0-9:9}" = "40BITS_VA" ]; then
    ENABLE_40BITS_VA=1
    GPU_CONFIG_WITHOUT_VA_FLAG=${GPU_CONFIG%_*}
    GPU_CONFIG_WITHOUT_VA_FLAG=${GPU_CONFIG_WITHOUT_VA_FLAG%_*}
fi

echo ENABLE_40BITS_VA=${ENABLE_40BITS_VA}
echo GPU_CONFIG=${GPU_CONFIG}
echo GPU_CONFIG_WITHOUT_VA_FLAG=${GPU_CONFIG_WITHOUT_VA_FLAG}

if [ ! -z $5 ] && [ "$MORE" != "clean" ]; then
    case "$MORE" in
        [1-9]*)
            process=$MORE
        ;;
        [1-9][0-9]*)
            process=$MORE
        ;;
        *)
            process=1
        ;;
    esac
else
   process=$max_process
fi
if [ $process -gt $max_process ]; then
    process=$max_process
fi
echo
echo max count of compiling job at the same time: $process
echo

tmp_fifofile=/tmp/$$.fifo
mkfifo $tmp_fifofile
exec 9<> $tmp_fifofile
rm $tmp_fifofile

for i in `seq $process`
do
   echo >&9 
done

function init()
{
    chmod +x ./build5x.sh
    cd $AQROOT
    if [ "$BUILD_TOOL" = "x86_x64_gcc" ]; then
        if [ "$ENABLE_40BITS_VA" == "1" ]; then
            ./build5x.sh -lFxts ${BUILD_CONFIG} XAQ2 X86_NO_KERNEL FBVDK > $AQROOT/setenv
        else
            ./build5x.sh -lxts ${BUILD_CONFIG} XAQ2 X86_NO_KERNEL FBVDK > $AQROOT/setenv
        fi
    fi

    if [ "$BUILD_TOOL" = "x86_gcc" ]; then
        ./build5x.sh -lxts ${BUILD_CONFIG} XAQ2 i386_NO_KERNEL FBVDK > $AQROOT/setenv
    fi

    if [ ! -e "$AQROOT/setenv" ]; then
        echo "ERROR: not support this build tool: $BUILD_TOOL"
        exit 1
    fi

    echo "== check GPU config file ..."
    GPU_CONFIG_FILE=$AQROOT/compiler/vclcompiler/viv_gpu.config
    if [ "$GPU_CONFIG_WITHOUT_VA_FLAG" = "default" ]; then
        GPU_CONFIG_FILE=$AQROOT/compiler/vclcompiler/viv_gpu.config
    else
        GPU_CONFIG_FILE=$(find $AQROOT/compiler/vclcompiler -iname "*${GPU_CONFIG_WITHOUT_VA_FLAG}.config" | head -n1)
    fi
    if [ ! -e $GPU_CONFIG_FILE ]; then
        echo "ERROR: missing GPU config file: $GPU_CONFIG_FILE"
        echo "You can get GPU config file from here: //SW/Rel5x/configs/*.config"
        exit 1
    fi
    export VIV_GPU_FILE=$GPU_CONFIG_FILE

    echo "== found GPU config file: $GPU_CONFIG_FILE"
    echo
}

function check_vcCompiler()
{
    echo "== check vcCompiler tool ... "
    if [ ! -e "$AQROOT/vcCompiler" ]; then
        echo "== not found vcCompiler"
        echo "== build vcCompiler ..."
        cd $AQROOT
        . ./setenv
        export CFLAGS="-w ${CFLAGS}"

        test -e "$AQARCH/reg" || exit $?
        cd $AQARCH/reg
        $cc || exit $?

        local depends=(hal/user hal/user/arch hal/os/linux/user compiler/libVSC compiler/libCLC)
        for p in ${depends[*]}
        do
            if [ -e "$AQROOT/$p" ]; then
                cd $AQROOT/$p
                $cc -j $process install || exit $?
            fi
        done

        cd $AQROOT/compiler/vclcompiler/source
        $cc -j $process install || exit 1
        cp -fv $SDK_DIR/samples/vclcompiler/vcCompiler $AQROOT
        echo "== Success to build vcCompiler."
    else
        echo "== found vcCompiler tool: $AQROOT/vcCompiler"
    fi
}

function cleanup()
{
   if [ -e $AQROOT/setenv ]; then
       cd $AQROOT
       . ./setenv
       echo "== clean driver build ..."
       cd $AQROOT
       $cc clean 1>/dev/null
       echo "== clean compiler ..."
       cd $AQROOT/compiler/vclcompiler/source
       $cc clean 1>/dev/null

       if [ -e "$AQROOT/vcCompiler" ]; then
           rm -f $AQROOT/vcCompiler
       fi

       if [ -e "$AQROOT/sdk/include" ]; then
           rm -rf $AQROOT/sdk/include
       fi

       echo "== clean old vxc binary ..."
       cd $AQROOT/driver/khronos/libOpenVX/libkernel
       find . -type f -name "*.gcPGM" | xargs rm -f
       find . -type f -name "*.vxgcSL" | xargs rm -f
       find . -type d -name ${GPU_CONFIG} | xargs rm -rf
       echo == Done
       echo
   fi
   return 0
}

function compile_shader()
{
    category=$1
    src_dir=$2
    ins_header=$3
    ec=0

    export VIVANTE_SDK_DIR=$AQROOT/sdk
    echo "== convert ${category^^} shader to header files ..."
    if [ ! -e "$AQROOT/sdk/include" ]; then
       cd $AQROOT/sdk; ln -s inc include
    fi

    if [ ! -e $src_dir ]; then
        echo "== not found $src_dir, skipped"
        return $ec;
    fi

    cd $src_dir
    echo Entering $src_dir
    if [ ! -e "../${GPU_CONFIG}" ]; then
       mkdir -p ../${GPU_CONFIG}
    fi

    if [ ! -z $MORE ] && [ "$MORE" == "clean" ]; then
        echo rm *.gcPGM  *.vxgclSL $category}_*.h ../${GPU_CONFIG}/*.h
        rm -f *.gcPGM *.vxgcSL ${category}_*.h ../${GPU_CONFIG}/*.h
        return $ec
    fi

    if [ ! -z $MORE ] || [ "$MORE" == "" ] || [ "$MORE" == "install" ]; then
        echo "== compiling shader files ..."
        for vxFile in `ls *.vx | sed "s/\.vx//"`
        do
            read -u 9
            {
                if [ "${ins_header}" != "" ]; then
                    if [ "${vxFile}" != "${ins_header}" ] && [ ! -e ./${vxFile}_all.gcPGM ]; then
                        echo $AQROOT/vcCompiler -f${GPU_CONFIG_FILE} -allkernel -cl-viv-gcsl-driver-image -o${vxFile} -m ${ins_header}.vx -40BitMemAddr:${ENABLE_40BITS_VA} ${vxFile}.vx
                        $AQROOT/vcCompiler -f${GPU_CONFIG_FILE} -allkernel -cl-viv-gcsl-driver-image -o${vxFile} -m ${ins_header}.vx -40BitMemAddr:${ENABLE_40BITS_VA} ${vxFile}.vx
                        ec=$?
                        if [ $ec -ne 0 ]; then
                             echo "== vcCompiler returned error: $ec"
                        fi
                        if [ -e ./${vxFile}_all.gcPGM ]; then
                            echo "== convert ${vxFile}_all.gcPGM to ../${GPU_CONFIG}/${category}_bin_${vxFile}.h ..."
                            python $AQROOT/tools/bin/ConvertPGMToH.py -i ${vxFile}_all.gcPGM -o ../${GPU_CONFIG}/${category}_bin_${vxFile}.h
                        else
                            echo "error: missing ./${vxFile}_all.gcPGM"
                            ec=1
                        fi
                    fi
                else
                    if [  ! -e ./${vxFile}_all.gcPGM ]; then
                        echo $AQROOT/vcCompiler -f${GPU_CONFIG_FILE} -allkernel -cl-viv-gcsl-driver-image -o${vxFile} -40BitMemAddr:${ENABLE_40BITS_VA} ${vxFile}.vx
                        $AQROOT/vcCompiler -f${GPU_CONFIG_FILE} -allkernel -cl-viv-gcsl-driver-image -o${vxFile} -40BitMemAddr:${ENABLE_40BITS_VA} ${vxFile}.vx
                        ec=$?
                        if [ $ec -ne 0 ]; then
                             echo "== vcCompiler returned error: $ec"
                        fi
                        if [ -e ./${vxFile}_all.gcPGM ]; then
                            echo "== convert ${vxFile}_all.gcPGM to ../${GPU_CONFIG}/${category}_bin_${vxFile}.h ..."
                            python $AQROOT/tools/bin/ConvertPGMToH.py -i ${vxFile}_all.gcPGM -o ../${GPU_CONFIG}/${category}_bin_${vxFile}.h
                        else
                            echo "error: missing ./${vxFile}_all.gcPGM"
                            ec=1
                        fi
                    fi
                fi
                echo >&9
            } &
        done
        wait
    fi

    binaries_file=../${GPU_CONFIG}/${category}_vxc_binaries.h
    if [ "$category" == "gpu" ] || [ "$category" == "vxc" ]; then
         binaries_file=../${GPU_CONFIG}/${category}_binaries.h
    fi
    echo "== generating $binaries_file ..."
    echo "#ifndef __${category^^}_BINARIES_H__" > $binaries_file
    echo "#define __${category^^}_BINARIES_H__" >> $binaries_file
    for vxFile in `ls *.vx | sed "s/\.vx//"`
    do
        if [ "${vxFile}" != "${ins_header}" ]; then
            if [ -e ../${GPU_CONFIG}/${category}_bin_${vxFile}.h ]; then
                echo "#include \"${category}_bin_${vxFile}.h\"" >> $binaries_file
            else
                ec=1
                echo "error: missing $binaries_file"
                break
            fi
        fi
    done
    echo "#endif" >> $binaries_file
    echo

    if [ "$ec" == "0" ]; then
        tmp_dir=${src_dir%/*}
        python $AQROOT/tools/bin/ExtractVXCBins.py ${tmp_dir}
        mv $tmp_dir/*.h $tmp_dir/${GPU_CONFIG}
        mv $tmp_dir/*.c $tmp_dir/${GPU_CONFIG}
        echo "Success to generate ${category^^} shader binary files"
    else
        echo "Error: failed to generate ${category^^} shader binary files"
    fi

    if [ $ec -ne 0 ]; then exit $ec; fi
}

function convert_vxc_shader()
{
    compile_shader vxc $AQROOT/driver/khronos/libOpenVX/libkernel/libnnvxc/nnvxc_kernels VXC_INS_HDR
    compile_shader ovx12 $AQROOT/driver/khronos/libOpenVX/libkernel/libovx12/ovx12_vxcKernels gpu_helper
    compile_shader ovxgpu $AQROOT/driver/khronos/libOpenVX/libkernel/libovxgpu/ovxgpu_vxcKernels gpu_helper
    compile_shader gpu $AQROOT/driver/khronos/libOpenVX/libkernel/libnngpu/nngpu_kernels gpu_helper
}

if [ "$MORE" == "clean" ]; then
    cleanup
else
    init
    check_vcCompiler
    export VIVANTE_SDK_DIR=$AQROOT/sdk
    convert_vxc_shader
fi
exit $?
