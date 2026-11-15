"""Rewrites DXVK's unsized sampler heap (`uniform sampler sampler_heap[]`, an OpTypeRuntimeArray) to a one-element
array, so SPIRV-Cross can emit plain MSL without argument buffers. The harness sets every sampler index to 0."""
import struct
import sys


def patch(data):
    w = list(struct.unpack('<%dI' % (len(data) // 4), data))
    out = w[:5]
    bound = w[3]
    i = 5
    uint_ty = None
    samplers = set()
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        ins = w[i:i + n]
        if op == 21 and ins[2] == 32 and ins[3] == 0:      # OpTypeInt 32 0
            uint_ty = ins[1]
        if op == 26:                                       # OpTypeSampler
            samplers.add(ins[1])
        if op == 29 and ins[2] in samplers:                # OpTypeRuntimeArray of samplers
            if uint_ty is None:
                uint_ty = bound
                bound += 1
                out += [(4 << 16) | 21, uint_ty, 32, 0]
            c = bound
            bound += 1
            out += [(4 << 16) | 43, uint_ty, c, 1]         # OpConstant %uint 1
            out += [(4 << 16) | 28, ins[1], ins[2], c]     # OpTypeArray
        else:
            out += ins
        i += n
    out[3] = bound
    return struct.pack('<%dI' % len(out), *out)


if __name__ == '__main__':
    open(sys.argv[2], 'wb').write(patch(open(sys.argv[1], 'rb').read()))
