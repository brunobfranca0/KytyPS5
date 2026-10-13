// Offline RDNA decoder: g++ -std=c++20 -I<cfg> -Isrc -Isrc/include -I3rdparty/magic_enum/include/magic_enum -I3rdparty/fmt/include -DFMT_HEADER_ONLY rdna-decode.cpp stubs src/graphics/shader/recompiler/frontend/decode/*.cpp (cmake_config.h and Common::DbgExit* stubs needed).
#include "graphics/shader/recompiler/frontend/decode/ShaderDecoder.h"
#include <cstdio>
#include <vector>
using namespace Libs::Graphics::ShaderRecompiler;
int main(int c,char**v){FILE*f=fopen(v[1],"rb");std::vector<uint32_t> d(1<<16);size_t n=fread(d.data(),4,d.size(),f);d.resize(n);
 Decoder::Program p; Decoder::DecodeProgram(d,p); puts(Decoder::ProgramToString(p).c_str());}
