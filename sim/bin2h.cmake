# cmake -DIN=file -DOUT=header.h -DNAME=symbol -P bin2h.cmake
# Embed a binary file as a C array (the debug TPM's permanent state).
file(READ ${IN} hex HEX)
string(LENGTH "${hex}" n)
set(body "")
math(EXPR last "${n} - 2")
foreach(i RANGE 0 ${last} 2)
  string(SUBSTRING "${hex}" ${i} 2 b)
  string(APPEND body "0x${b},")
  math(EXPR col "(${i} / 2) % 16")
  if(col EQUAL 15)
    string(APPEND body "\n")
  endif()
endforeach()
get_filename_component(src ${IN} NAME)
file(WRITE ${OUT} "/* generated from sim/${src} (sim/tpmgen.c): the debug TPM's fixed\n"
                  " * permanent state. Public: the keys it derives are not secret. */\n"
                  "static const unsigned char ${NAME}[] = {\n${body}\n};\n")
