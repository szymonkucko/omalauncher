#version 450
// The P0 test pattern. The launcher computes the same in C (zc_pattern in zcp0.c) and
// compares read-back pixels with it: keep both in sync.
//   gradient: red left->right, green top->bottom, blue pulsing with the frame
//   white grid lines every 64 px, a magenta bar 32 px wide moving 8 px per frame
//   96 px corners: red top left, green top right, blue bottom left, white bottom right
layout(push_constant) uniform PC {
    uint frame;
    uint w;
    uint h;
    uint pad;
} pc;

layout(location = 0) out vec4 color;

void main() {
    vec2 px = gl_FragCoord.xy;
    float W = float(pc.w), H = float(pc.h);
    vec3 c = vec3(px.x / W, px.y / H, 0.5 + 0.5 * sin(float(pc.frame) / 60.0));
    if (mod(px.x, 64.0) < 2.0 || mod(px.y, 64.0) < 2.0) c = vec3(1.0);
    float bx = mod(float(pc.frame) * 8.0, W);
    if (px.x >= bx && px.x < bx + 32.0) c = vec3(1.0, 0.0, 1.0);
    const float s = 96.0;
    bool l = px.x < s, r = px.x >= W - s, t = px.y < s, b = px.y >= H - s;
    if (l && t) c = vec3(1.0, 0.0, 0.0);
    if (r && t) c = vec3(0.0, 1.0, 0.0);
    if (l && b) c = vec3(0.0, 0.0, 1.0);
    if (r && b) c = vec3(1.0);
    color = vec4(c, 1.0);
}
