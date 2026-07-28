// The CPU reference maths, ported line for line.
//
// Included by both compute shaders with `#include`, which glslc supports.
// Nothing in here may be "the obvious float version" of what canvas.cpp does:
// the whole point of D-021 keeping the CPU as the reference is that the two
// answers are compared pixel for pixel, and a compositor written in 0..1
// floats disagrees with `mul255` at almost every level. So the integer paths
// stay integer, and float appears only where the CPU also uses it.

// Divide by 255 with round-to-nearest. canvas.cpp `mul255`.
uint mul255(uint c, uint a) {
    uint t = c * a + 128u;
    return (t + (t >> 8u)) >> 8u;
}

// PremulRgba8 is four bytes r,g,b,a; both host and device are little-endian.
uvec4 unpackRgba(uint v) {
    return uvec4(v & 255u, (v >> 8u) & 255u, (v >> 16u) & 255u, (v >> 24u) & 255u);
}

uint packRgba(uvec4 c) {
    return c.x | (c.y << 8u) | (c.z << 16u) | (c.w << 24u);
}

// paint.cpp and io.cpp both scale a channel by a float; one spells it
// `lround(c * f)` and the other `uint8_t(c * f + 0.5f)`. For the
// non-negative values either can see, those are the same function.
uint scale8(uint c, float f) {
    return uint(floor(float(c) * f + 0.5));
}

uvec4 scale8v(uvec4 c, float f) {
    return uvec4(scale8(c.x, f), scale8(c.y, f), scale8(c.z, f), scale8(c.w, f));
}

// canvas.cpp `over`.
uvec4 overPremul(uvec4 src, uvec4 dst) {
    if (src.w == 255u) return src;
    if (src.w == 0u)   return dst;
    uint inv = 255u - src.w;
    return uvec4(src.x + mul255(dst.x, inv),
                 src.y + mul255(dst.y, inv),
                 src.z + mul255(dst.z, inv),
                 src.w + mul255(dst.w, inv));
}

// canvas.cpp `PremulRgba8::unpremultiply`.
uvec4 unpremul(uvec4 c) {
    if (c.w == 0u)   return uvec4(0u);
    if (c.w == 255u) return c;
    uvec3 v = (c.xyz * 255u + c.w / 2u) / c.w;
    return uvec4(min(v, uvec3(255u)), c.w);
}

// canvas.cpp `hardLight`.
float hardLightChannel(float cs, float cb) {
    return cs <= 0.5 ? 2.0 * cs * cb : 1.0 - 2.0 * (1.0 - cs) * (1.0 - cb);
}

// canvas.cpp `blendChannel`, W3C separable set, same guards in the same order.
float blendChannel(uint mode, float cs, float cb) {
    if (mode == 0u)  return cs;                       // Normal
    if (mode == 1u)  return cs * cb;                  // Multiply
    if (mode == 2u)  return cs + cb - cs * cb;        // Screen
    if (mode == 3u)  return min(1.0, cs + cb);        // Add
    if (mode == 4u)  return hardLightChannel(cb, cs); // Overlay
    if (mode == 5u)  return min(cs, cb);              // Darken
    if (mode == 6u)  return max(cs, cb);              // Lighten
    if (mode == 7u) {                                 // ColourDodge
        if (cb <= 0.0) return 0.0;
        if (cs >= 1.0) return 1.0;
        return min(1.0, cb / (1.0 - cs));
    }
    if (mode == 8u) {                                 // ColourBurn
        if (cb >= 1.0) return 1.0;
        if (cs <= 0.0) return 0.0;
        return 1.0 - min(1.0, (1.0 - cb) / cs);
    }
    if (mode == 9u)  return hardLightChannel(cs, cb); // HardLight
    if (mode == 10u) {                                // SoftLight
        if (cs <= 0.5) return cb - (1.0 - 2.0 * cs) * cb * (1.0 - cb);
        float d = cb <= 0.25 ? ((16.0 * cb - 12.0) * cb + 4.0) * cb : sqrt(cb);
        return cb + (2.0 * cs - 1.0) * (d - cb);
    }
    if (mode == 11u) return abs(cs - cb);             // Difference
    if (mode == 12u) return cs + cb - 2.0 * cs * cb;  // Exclusion
    return cs;
}

// canvas.cpp `blendOver`. `precise` stops the driver fusing the three terms
// into FMAs and reassociating them, which is what would otherwise put a
// one-off in a channel the CPU rounds the other way.
uvec4 blendOverMode(uint mode, uvec4 src, uvec4 dst) {
    if (mode == 0u)    return overPremul(src, dst);
    if (src.w == 0u)   return dst;
    if (dst.w == 0u)   return src;

    uvec4 s = unpremul(src);
    uvec4 b = unpremul(dst);
    float as = float(src.w) / 255.0;
    float ab = float(dst.w) / 255.0;

    uvec4 out_ = uvec4(0u);
    for (int i = 0; i < 3; ++i) {
        float cs = float(s[i]) / 255.0;
        float cb = float(b[i]) / 255.0;
        precise float co = as * (1.0 - ab) * cs
                         + as * ab * blendChannel(mode, cs, cb)
                         + (1.0 - as) * ab * cb;
        out_[i] = uint(floor(clamp(co, 0.0, 1.0) * 255.0 + 0.5));
    }
    precise float ao = as + ab * (1.0 - as);
    out_[3] = uint(floor(clamp(ao, 0.0, 1.0) * 255.0 + 0.5));
    return out_;
}
