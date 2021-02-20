#!/usr/bin/env python

import sys
import struct

import argparse

from wasm import *

FUNCTION_SECTION_ID = 3

#enum class WasmCompilationHintStrategy : uint8_t {
#  kDefault = 0,
#  kLazy = 1,
#  kEager = 2,
#  kLazyBaselineEagerTopTier = 3,
#};
#
#enum class WasmCompilationHintTier : uint8_t {
#  kDefault = 0,
#  kBaseline = 1,
#  kOptimized = 2,
#};
#
#// Static representation of a wasm compilation hint
#struct WasmCompilationHint {
#  WasmCompilationHintStrategy strategy;
#  WasmCompilationHintTier baseline_tier;
#  WasmCompilationHintTier top_tier;
#};



def get_function_num(in_wasm_file):
  with io.open(in_wasm_file, "rb") as fin:
    magic_number, bs = read_magic_number(fin);
    version, bs = read_version(fin);
    num_declared_functions = None
    while True:
      id, bs = read_varuintN(fin)
      if id == None:
        break
      payload_length, bs = read_varuintN(fin)

        # Peek into function section for upcoming validity check.
      if id == FUNCTION_SECTION_ID:
        num_declared_functions, bs = peek_varuintN(fin)

      bs = fin.read(payload_length)

#      assert len(hints_bs) == num_declared_functions, "unexpected number of hints"

    return  num_declared_functions


def generate_hints(count, special_index):
  # |reserved|top_tier|baseline_tier|strategy|
  strategy = 0;
  baseline_tier = 0;
  top_tier = 0;
  reserved = 0;
  default_hint = reserved << 6 | top_tier << 4 | baseline_tier << 2 | strategy;
  special_hint =  1 << 6 | top_tier << 4 | baseline_tier << 2 | strategy;

  hintlist = []
  for i in range(count):
    hintlist.append(default_hint)
  print (len(hintlist))
  #print(hintlist[1334])
  for index in special_index:
    hintlist[index] = special_hint

  print (len(hintlist))
  print(hintlist)
  buf = struct.pack('B' * len(hintlist), *hintlist)
  return buf


def parse_args():
  parser = argparse.ArgumentParser(\
      description="Inject compilation hints into a Wasm module.")
  parser.add_argument("-i", "--in-wasm-file", \
      type=str, required=True, \
      help="original wasm module")
  parser.add_argument("-x", "--hints-file", \
      type=str, required=True, \
      help="binary hints file to be injected as a custom section " + \
          "'compilationHints'")
  parser.add_argument("-s", "--special-index", \
      nargs='+', \
      type=int,  required=True, \
      help="scalable kernel index")
  return parser.parse_args()

#special_index should be the objdump's function_index - imported

if __name__ == "__main__":
  args = parse_args()
  in_wasm_file = args.in_wasm_file
  hints_file = args.hints_file
  special_index = args.special_index


  func_num = get_function_num(in_wasm_file)  
  #func_num  = 10
  #print(func_num)
  #print(special_index)

  if(max(special_index) > func_num):
      print('error, special index > func num')
      sys.exit(1)

  hintbuf = generate_hints(func_num, special_index)

  fout = open(hints_file, "wb")
  fout.write(hintbuf)
  fout.close()


#  print('reading back hint')
#  with io.open(hints_file, "rb") as fin:
#    hints = fin.read()
#  print(hints[5])

