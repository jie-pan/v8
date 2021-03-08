#! /bin/bash

if [ $# -lt 2 ]
then
    echo "usage: $0 in.wasm out.wasm"
    echo "inject compilation hint for 4xN__wasmsimd function"
    echo "read function index from in.wasm, save hint into custom section of out.wasm"
    exit 1
else
    infile=$1
    outfile=$2
fi

echo "inject hint to ${outfile}"

offset=$(wasm-objdump -x ${infile}  | grep "Function\[" -A 1 | grep func | awk -F[ '{print $2}' | awk -F] '{print $1}')

func1=$(wasm-objdump -x ${infile} | grep "] <xnn_f32_gemm_minmax_ukernel_4xN__wasmsimd_x86_loadsplat>" | awk -F[ '{print $2}' | awk -F] '{print $1}')
func2=$(wasm-objdump -x ${infile}  | grep "] <xnn_f32_gemm_ukernel_4xN__wasmsimd_loadsplat>" | awk -F[ '{print $2}' | awk -F] '{print $1}')

echo "offset=$offset"
echo "func index=$func1, $func2"

if [[ "$func1" == "" ]]
then
    echo "function $func1 not found"
    exit
else
    func1=$(echo $(($func1 - $offset)))
fi

if [[ "$func2" != "" ]]
then
    func2=$(echo $(($func2 - $offset)))
fi

echo "func index - offset=$func1, $func2"
python ./generate-hints.py -i ${infile}  -x hint.bin -s $func1 $func2

python ./inject-compilation-hints.py  -i ${infile} -x hint.bin -o ${outfile} 
