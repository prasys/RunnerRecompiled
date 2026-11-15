// Runs SwARc's bytecode translator (hlsl::TranslateBytecode) over every record of a DXVK warm shader file and
// writes, per shader, a complete GLSL file (#version + the comparison prelude + SwARc's body) and a JSON line of
// the interface SwARc reports. SwARc itself is not in this repo: build against a checkout (see README.md).
//
// The prelude is the "graphics backend" half of SwARc's contract (hlsl.h). For a numeric comparison it is kept
// neutral: vertex inputs are plain attributes (slot n at location n), r_position is the identity (no y flip),
// the alpha test passes, the LOD bias is 0.
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "runtime/hlsl/hlsl.h"

using namespace recomp::hlsl;

static std::string Prelude(const Program& p) {
  std::string s = "#version 450\n";
  if (p.stage == Stage::Vertex) {
    s += "#define R_FLOAT_BINDING set = 1, binding = 0\n#define R_INT_BINDING set = 1, binding = 1\n"
         "#define R_SAMPLER_BINDING(n) set = 0, binding = n\n";
    for (size_t i = 0; i < p.inputs.size(); i++)
      s += "layout(location = " + std::to_string(i) + ") in vec4 r_in" + std::to_string(i) + ";\n";
    s += "vec4 r_fetch(int slot) {\n";
    for (size_t i = 0; i < p.inputs.size(); i++)
      s += "  if (slot == " + std::to_string(i) + ") return r_in" + std::to_string(i) + ";\n";
    s += "  return vec4(0.0, 0.0, 0.0, 1.0);\n}\nvec4 r_position(vec4 p) { return p; }\n";
  } else {
    s += "#define R_FLOAT_BINDING set = 3, binding = 0\n#define R_INT_BINDING set = 3, binding = 1\n"
         "#define R_SAMPLER_BINDING(n) set = 2, binding = n\n#define R_BIAS(n) 0.0\n"
         "vec2 r_vpos() { return gl_FragCoord.xy - 0.5; }\n"
         "float r_vface() { return gl_FrontFacing ? 1.0 : -1.0; }\n"
         "void r_color0(inout vec4 c) {}\n";
  }
  return s;
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: translate <pipelines.warm.shaders> <outdir>\n");
    return 2;
  }
  FILE* f = std::fopen(argv[1], "rb");
  if (!f) return 2;
  std::string outdir = argv[2];
  std::ofstream meta(outdir + "/swarc-meta.jsonl");
  int ok = 0, bad = 0;
  for (;;) {
    uint16_t nl;
    uint8_t px;
    uint32_t sz;
    if (std::fread(&nl, 2, 1, f) != 1 || std::fread(&px, 1, 1, f) != 1 || std::fread(&sz, 4, 1, f) != 1) break;
    std::string name(nl, 0);
    std::vector<uint32_t> code((sz + 3) / 4);
    if (std::fread(&name[0], 1, nl, f) != nl || std::fread(code.data(), 1, sz, f) != sz) break;
    Program p;
    std::string err;
    if (!TranslateBytecode(code.data(), sz / 4, &p, &err)) {
      bad++;
      std::fprintf(stderr, "%s: %s\n", name.c_str(), err.c_str());
      continue;
    }
    ok++;
    std::ofstream(outdir + "/" + name + (px ? ".frag" : ".vert")) << Prelude(p) << p.glsl;
    meta << "{\"name\":\"" << name << "\",\"inputs\":[";
    for (size_t i = 0; i < p.inputs.size(); i++)
      meta << (i ? "," : "") << "[" << p.inputs[i].usage << "," << p.inputs[i].index << "]";
    meta << "],\"varyings\":[";
    for (size_t i = 0; i < p.varyings.size(); i++)
      meta << (i ? "," : "") << "[" << p.varyings[i].usage << "," << p.varyings[i].index << ","
           << VaryingLocation(p.varyings[i]) << "]";
    meta << "],\"color_outputs\":" << p.color_outputs << ",\"writes_depth\":" << (p.writes_depth ? 1 : 0)
         << ",\"float_regs\":" << p.float_regs << "}\n";
  }
  std::printf("translated %d, failed %d\n", ok, bad);
  return bad ? 1 : 0;
}
