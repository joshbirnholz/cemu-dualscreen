#include "Cafe/HW/Latte/Renderer/RendererOuputShader.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/Latte.h"
#include "config/ActiveSettings.h"

const std::string RendererOutputShader::s_copy_shader_source =
R"(
void outputShader()
{
	colorOut0 = vec4(texture(textureSrc, passUV).rgb,1.0);
}
)";

const std::string RendererOutputShader::s_copy_shader_source_mtl =
R"(#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]], texture2d<float> textureSrc [[texture(0)]], sampler samplr [[sampler(0)]]) {
	return float4(textureSrc.sample(samplr, in.uv).rgb, 1.0);
}
)";

const std::string RendererOutputShader::s_bicubic_shader_source =
R"(
vec4 cubic(float x)
{
	float x2 = x * x;
	float x3 = x2 * x;
	vec4 w;
	w.x = -x3 + 3 * x2 - 3 * x + 1;
	w.y = 3 * x3 - 6 * x2 + 4;
	w.z = -3 * x3 + 3 * x2 + 3 * x + 1;
	w.w = x3;
	return w / 6.0;
}

vec4 bcFilter(vec2 uv, vec4 texelSize)
{
	vec2 pixel = uv*texelSize.zw - 0.5;
	vec2 pixelFrac = fract(pixel);
	vec2 pixelInt = pixel - pixelFrac;

	vec4 xcubic = cubic(pixelFrac.x);
	vec4 ycubic = cubic(pixelFrac.y);

	vec4 c = vec4(pixelInt.x - 0.5, pixelInt.x + 1.5, pixelInt.y - 0.5, pixelInt.y + 1.5);
	vec4 s = vec4(xcubic.x + xcubic.y, xcubic.z + xcubic.w, ycubic.x + ycubic.y, ycubic.z + ycubic.w);
	vec4 offset = c + vec4(xcubic.y, xcubic.w, ycubic.y, ycubic.w) / s;

	vec4 sample0 = texture(textureSrc, vec2(offset.x, offset.z) * texelSize.xy);
	vec4 sample1 = texture(textureSrc, vec2(offset.y, offset.z) * texelSize.xy);
	vec4 sample2 = texture(textureSrc, vec2(offset.x, offset.w) * texelSize.xy);
	vec4 sample3 = texture(textureSrc, vec2(offset.y, offset.w) * texelSize.xy);

	float sx = s.x / (s.x + s.y);
	float sy = s.z / (s.z + s.w);

	return mix(
		mix(sample3, sample2, sx),
		mix(sample1, sample0, sx), sy);
}

void outputShader(){
	vec4 texelSize = vec4( 1.0 / textureSrcResolution.xy, textureSrcResolution.xy);
	colorOut0 = vec4(bcFilter(passUV, texelSize).rgb,1.0);
}
)";

const std::string RendererOutputShader::s_bicubic_shader_source_mtl =
R"(#include <metal_stdlib>
using namespace metal;

float4 cubic(float x) {
	float x2 = x * x;
	float x3 = x2 * x;
	float4 w;
	w.x = -x3 + 3 * x2 - 3 * x + 1;
	w.y = 3 * x3 - 6 * x2 + 4;
	w.z = -3 * x3 + 3 * x2 + 3 * x + 1;
	w.w = x3;
	return w / 6.0;
}

float4 bcFilter(texture2d<float> textureSrc, sampler samplr, float2 texcoord, float2 texscale) {
	float fx = fract(texcoord.x);
	float fy = fract(texcoord.y);
	texcoord.x -= fx;
	texcoord.y -= fy;

	float4 xcubic = cubic(fx);
	float4 ycubic = cubic(fy);

	float4 c = float4(texcoord.x - 0.5, texcoord.x + 1.5, texcoord.y - 0.5, texcoord.y + 1.5);
	float4 s = float4(xcubic.x + xcubic.y, xcubic.z + xcubic.w, ycubic.x + ycubic.y, ycubic.z + ycubic.w);
	float4 offset = c + float4(xcubic.y, xcubic.w, ycubic.y, ycubic.w) / s;

	float4 sample0 = textureSrc.sample(samplr, float2(offset.x, offset.z) * texscale);
	float4 sample1 = textureSrc.sample(samplr, float2(offset.y, offset.z) * texscale);
	float4 sample2 = textureSrc.sample(samplr, float2(offset.x, offset.w) * texscale);
	float4 sample3 = textureSrc.sample(samplr, float2(offset.y, offset.w) * texscale);

	float sx = s.x / (s.x + s.y);
	float sy = s.z / (s.z + s.w);

	return mix(
		mix(sample3, sample2, sx),
		mix(sample1, sample0, sx), sy);
}

struct VertexOut {
    float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]], texture2d<float> textureSrc [[texture(0)]], sampler samplr [[sampler(0)]]) {
    float2 textureSrcResolution = float2(textureSrc.get_width(), textureSrc.get_height());
	return float4(bcFilter(textureSrc, samplr, in.uv * textureSrcResolution, float2(1.0, 1.0) / textureSrcResolution).rgb, 1.0);
}
)";

const std::string RendererOutputShader::s_hermite_shader_source =
R"(
// https://www.shadertoy.com/view/MllSzX

vec3 CubicHermite (vec3 A, vec3 B, vec3 C, vec3 D, float t)
{
	float t2 = t*t;
    float t3 = t*t*t;
    vec3 a = -A/2.0 + (3.0*B)/2.0 - (3.0*C)/2.0 + D/2.0;
    vec3 b = A - (5.0*B)/2.0 + 2.0*C - D / 2.0;
    vec3 c = -A/2.0 + C/2.0;
   	vec3 d = B;

    return a*t3 + b*t2 + c*t + d;
}


vec3 BicubicHermiteTexture(vec2 uv, vec4 texelSize)
{
	vec2 pixel = uv*texelSize.zw + 0.5;
	vec2 frac = fract(pixel);
    pixel = floor(pixel) / texelSize.zw - vec2(texelSize.xy/2.0);

	vec4 doubleSize = texelSize*2.0;

	vec3 C00 = texture(textureSrc, pixel + vec2(-texelSize.x ,-texelSize.y)).rgb;
    vec3 C10 = texture(textureSrc, pixel + vec2( 0.0        ,-texelSize.y)).rgb;
    vec3 C20 = texture(textureSrc, pixel + vec2( texelSize.x ,-texelSize.y)).rgb;
    vec3 C30 = texture(textureSrc, pixel + vec2( doubleSize.x,-texelSize.y)).rgb;

    vec3 C01 = texture(textureSrc, pixel + vec2(-texelSize.x , 0.0)).rgb;
    vec3 C11 = texture(textureSrc, pixel + vec2( 0.0        , 0.0)).rgb;
    vec3 C21 = texture(textureSrc, pixel + vec2( texelSize.x , 0.0)).rgb;
    vec3 C31 = texture(textureSrc, pixel + vec2( doubleSize.x, 0.0)).rgb;

    vec3 C02 = texture(textureSrc, pixel + vec2(-texelSize.x , texelSize.y)).rgb;
    vec3 C12 = texture(textureSrc, pixel + vec2( 0.0        , texelSize.y)).rgb;
    vec3 C22 = texture(textureSrc, pixel + vec2( texelSize.x , texelSize.y)).rgb;
    vec3 C32 = texture(textureSrc, pixel + vec2( doubleSize.x, texelSize.y)).rgb;

    vec3 C03 = texture(textureSrc, pixel + vec2(-texelSize.x , doubleSize.y)).rgb;
    vec3 C13 = texture(textureSrc, pixel + vec2( 0.0        , doubleSize.y)).rgb;
    vec3 C23 = texture(textureSrc, pixel + vec2( texelSize.x , doubleSize.y)).rgb;
    vec3 C33 = texture(textureSrc, pixel + vec2( doubleSize.x, doubleSize.y)).rgb;

    vec3 CP0X = CubicHermite(C00, C10, C20, C30, frac.x);
    vec3 CP1X = CubicHermite(C01, C11, C21, C31, frac.x);
    vec3 CP2X = CubicHermite(C02, C12, C22, C32, frac.x);
    vec3 CP3X = CubicHermite(C03, C13, C23, C33, frac.x);

    return CubicHermite(CP0X, CP1X, CP2X, CP3X, frac.y);
}

void outputShader(){
	vec4 texelSize = vec4( 1.0 / textureSrcResolution.xy, textureSrcResolution.xy);
	colorOut0 = vec4(BicubicHermiteTexture(passUV, texelSize), 1.0);
}
)";

const std::string RendererOutputShader::s_hermite_shader_source_mtl =
R"(#include <metal_stdlib>
using namespace metal;

// https://www.shadertoy.com/view/MllSzX

float3 CubicHermite(float3 A, float3 B, float3 C, float3 D, float t) {
	float t2 = t*t;
    float t3 = t*t*t;
    float3 a = -A/2.0 + (3.0*B)/2.0 - (3.0*C)/2.0 + D/2.0;
    float3 b = A - (5.0*B)/2.0 + 2.0*C - D / 2.0;
    float3 c = -A/2.0 + C/2.0;
   	float3 d = B;

    return a*t3 + b*t2 + c*t + d;
}


float3 BicubicHermiteTexture(texture2d<float> textureSrc, sampler samplr, float2 uv, float4 texelSize) {
	float2 pixel = uv*texelSize.zw + 0.5;
	float2 frac = fract(pixel);
    pixel = floor(pixel) / texelSize.zw - float2(texelSize.xy/2.0);

	float4 doubleSize = texelSize*texelSize;

	float3 C00 = textureSrc.sample(samplr, pixel + float2(-texelSize.x ,-texelSize.y)).rgb;
    float3 C10 = textureSrc.sample(samplr, pixel + float2( 0.0        ,-texelSize.y)).rgb;
    float3 C20 = textureSrc.sample(samplr, pixel + float2( texelSize.x ,-texelSize.y)).rgb;
    float3 C30 = textureSrc.sample(samplr, pixel + float2( doubleSize.x,-texelSize.y)).rgb;

    float3 C01 = textureSrc.sample(samplr, pixel + float2(-texelSize.x , 0.0)).rgb;
    float3 C11 = textureSrc.sample(samplr, pixel + float2( 0.0        , 0.0)).rgb;
    float3 C21 = textureSrc.sample(samplr, pixel + float2( texelSize.x , 0.0)).rgb;
    float3 C31 = textureSrc.sample(samplr, pixel + float2( doubleSize.x, 0.0)).rgb;

    float3 C02 = textureSrc.sample(samplr, pixel + float2(-texelSize.x , texelSize.y)).rgb;
    float3 C12 = textureSrc.sample(samplr, pixel + float2( 0.0        , texelSize.y)).rgb;
    float3 C22 = textureSrc.sample(samplr, pixel + float2( texelSize.x , texelSize.y)).rgb;
    float3 C32 = textureSrc.sample(samplr, pixel + float2( doubleSize.x, texelSize.y)).rgb;

    float3 C03 = textureSrc.sample(samplr, pixel + float2(-texelSize.x , doubleSize.y)).rgb;
    float3 C13 = textureSrc.sample(samplr, pixel + float2( 0.0        , doubleSize.y)).rgb;
    float3 C23 = textureSrc.sample(samplr, pixel + float2( texelSize.x , doubleSize.y)).rgb;
    float3 C33 = textureSrc.sample(samplr, pixel + float2( doubleSize.x, doubleSize.y)).rgb;

    float3 CP0X = CubicHermite(C00, C10, C20, C30, frac.x);
    float3 CP1X = CubicHermite(C01, C11, C21, C31, frac.x);
    float3 CP2X = CubicHermite(C02, C12, C22, C32, frac.x);
    float3 CP3X = CubicHermite(C03, C13, C23, C33, frac.x);

    return CubicHermite(CP0X, CP1X, CP2X, CP3X, frac.y);
}

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

fragment float4 main0(VertexOut in [[stage_in]], texture2d<float> textureSrc [[texture(0)]], sampler samplr [[sampler(0)]], constant float2& outputResolution [[buffer(0)]]) {
	float4 texelSize = float4(1.0 / outputResolution.xy, outputResolution.xy);
	return float4(BicubicHermiteTexture(textureSrc, samplr, in.uv, texelSize), 1.0);
}
)";

const std::string RendererOutputShader::s_fsr_shader_source =
R"(
// AMD FidelityFX Super Resolution 1.0 (MIT licensed, Copyright (c) 2021
// Advanced Micro Devices, Inc.): EASU, its edge-adaptive upscale, and RCAS,
// its sharpening, in the one pass Cemu's output has. RCAS wants EASU's
// picture around each pixel, so EASU is worked out for the pixel and the four
// beside it -- from the same few texels, which the texture cache keeps.

ivec2 fsrMaxTexel;

vec3 fsrTexel(ivec2 p)
{
	return texelFetch(textureSrc, clamp(p, ivec2(0), fsrMaxTexel), 0).rgb;
}

float fsrLuma(vec3 c)
{
	return c.g + 0.5 * (c.r + c.b);
}

void fsrEasuSet(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE)
{
	// lA above, lB left, lC the centre, lD right, lE below.
	float lenX = max(abs(lD - lC), abs(lC - lB));
	float dirX = lD - lB;
	dir.x += dirX * w;
	lenX = lenX > 0.0 ? clamp(abs(dirX) / lenX, 0.0, 1.0) : 0.0;
	lenX *= lenX;
	len += lenX * w;
	float lenY = max(abs(lE - lC), abs(lC - lA));
	float dirY = lE - lA;
	dir.y += dirY * w;
	lenY = lenY > 0.0 ? clamp(abs(dirY) / lenY, 0.0, 1.0) : 0.0;
	lenY *= lenY;
	len += lenY * w;
}

void fsrEasuTap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c)
{
	vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;
	float d2 = min(dot(v, v), clp);
	float wB = 2.0 / 5.0 * d2 - 1.0;
	float wA = lob * d2 - 1.0;
	wB *= wB;
	wA *= wA;
	wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
	float w = wB * wA;
	aC += c * w;
	aW += w;
}

// The upscaled colour at uv, in the source's own texels.
vec3 fsrEasu(vec2 uv)
{
	vec2 pp = uv * textureSrcResolution - 0.5;
	vec2 fp = floor(pp);
	pp -= fp;
	ivec2 p = ivec2(fp);
	//    b c
	//  e f g h
	//  i j k l
	//    n o
	vec3 bC = fsrTexel(p + ivec2( 0,-1)); vec3 cC = fsrTexel(p + ivec2( 1,-1));
	vec3 eC = fsrTexel(p + ivec2(-1, 0)); vec3 fC = fsrTexel(p + ivec2( 0, 0));
	vec3 gC = fsrTexel(p + ivec2( 1, 0)); vec3 hC = fsrTexel(p + ivec2( 2, 0));
	vec3 iC = fsrTexel(p + ivec2(-1, 1)); vec3 jC = fsrTexel(p + ivec2( 0, 1));
	vec3 kC = fsrTexel(p + ivec2( 1, 1)); vec3 lC = fsrTexel(p + ivec2( 2, 1));
	vec3 nC = fsrTexel(p + ivec2( 0, 2)); vec3 oC = fsrTexel(p + ivec2( 1, 2));
	float bL = fsrLuma(bC), cL = fsrLuma(cC), eL = fsrLuma(eC), fL = fsrLuma(fC);
	float gL = fsrLuma(gC), hL = fsrLuma(hC), iL = fsrLuma(iC), jL = fsrLuma(jC);
	float kL = fsrLuma(kC), lL = fsrLuma(lC), nL = fsrLuma(nC), oL = fsrLuma(oC);

	vec2 dir = vec2(0.0);
	float len = 0.0;
	fsrEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
	fsrEasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
	fsrEasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
	fsrEasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

	float dirR = dot(dir, dir);
	bool zro = dirR < (1.0 / 32768.0);
	dirR = zro ? 1.0 : inversesqrt(dirR);
	dir.x = zro ? 1.0 : dir.x;
	dir *= dirR;
	len = len * 0.5;
	len *= len;
	float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
	vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
	float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
	float clp = 1.0 / lob;

	vec3 aC = vec3(0.0);
	float aW = 0.0;
	fsrEasuTap(aC, aW, vec2( 0.0,-1.0) - pp, dir, len2, lob, clp, bC);
	fsrEasuTap(aC, aW, vec2( 1.0,-1.0) - pp, dir, len2, lob, clp, cC);
	fsrEasuTap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);
	fsrEasuTap(aC, aW, vec2( 0.0, 1.0) - pp, dir, len2, lob, clp, jC);
	fsrEasuTap(aC, aW, vec2( 0.0, 0.0) - pp, dir, len2, lob, clp, fC);
	fsrEasuTap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);
	fsrEasuTap(aC, aW, vec2( 1.0, 1.0) - pp, dir, len2, lob, clp, kC);
	fsrEasuTap(aC, aW, vec2( 2.0, 1.0) - pp, dir, len2, lob, clp, lC);
	fsrEasuTap(aC, aW, vec2( 2.0, 0.0) - pp, dir, len2, lob, clp, hC);
	fsrEasuTap(aC, aW, vec2( 1.0, 0.0) - pp, dir, len2, lob, clp, gC);
	fsrEasuTap(aC, aW, vec2( 1.0, 2.0) - pp, dir, len2, lob, clp, oC);
	fsrEasuTap(aC, aW, vec2( 0.0, 2.0) - pp, dir, len2, lob, clp, nC);
	vec3 min4 = min(min(fC, gC), min(jC, kC));
	vec3 max4 = max(max(fC, gC), max(jC, kC));
	return min(max4, max(min4, aC / aW));
}

void outputShader()
{
	fsrMaxTexel = ivec2(textureSrcResolution) - 1;
	vec3 e = fsrEasu(passUV);
	if (fsrSharpness <= 0.0)
	{
		colorOut0 = vec4(e, 1.0);
		return;
	}
	// RCAS:
	//    b
	//  d e f
	//    h
	vec2 texStep = 1.0 / outputResolution;
	vec3 b = fsrEasu(passUV - vec2(0.0, texStep.y));
	vec3 d = fsrEasu(passUV - vec2(texStep.x, 0.0));
	vec3 f = fsrEasu(passUV + vec2(texStep.x, 0.0));
	vec3 h = fsrEasu(passUV + vec2(0.0, texStep.y));
	vec3 mn4 = min(min(b, d), min(f, h));
	vec3 mx4 = max(max(b, d), max(f, h));
	vec3 hitMin = min(mn4, e) / max(4.0 * mx4, vec3(1.0 / 65536.0));
	vec3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, vec3(-1.0 / 65536.0));
	vec3 lobeRGB = max(-hitMin, hitMax);
	float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * fsrSharpness;
	colorOut0 = vec4(clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0), 1.0);
}
)";

const std::string RendererOutputShader::s_fsr_shader_source_mtl =
R"(#include <metal_stdlib>
using namespace metal;

// See s_fsr_shader_source. AMD FidelityFX Super Resolution 1.0 (MIT licensed, Copyright (c) 2021
// Advanced Micro Devices, Inc.): EASU, its edge-adaptive upscale, and RCAS,
// its sharpening, in the one pass Cemu's output has. RCAS wants EASU's
// picture around each pixel, so EASU is worked out for the pixel and the four
// beside it -- from the same few texels, which the texture cache keeps.

float3 fsrTexel(texture2d<float> tex, int2 maxTexel, int2 p)
{
	return tex.read(uint2(clamp(p, int2(0), maxTexel))).rgb;
}

float fsrLuma(float3 c)
{
	return c.g + 0.5 * (c.r + c.b);
}

void fsrEasuSet(thread float2& dir, thread float& len, float w, float lA, float lB, float lC, float lD, float lE)
{
	// lA above, lB left, lC the centre, lD right, lE below.
	float lenX = max(abs(lD - lC), abs(lC - lB));
	float dirX = lD - lB;
	dir.x += dirX * w;
	lenX = lenX > 0.0 ? clamp(abs(dirX) / lenX, 0.0, 1.0) : 0.0;
	lenX *= lenX;
	len += lenX * w;
	float lenY = max(abs(lE - lC), abs(lC - lA));
	float dirY = lE - lA;
	dir.y += dirY * w;
	lenY = lenY > 0.0 ? clamp(abs(dirY) / lenY, 0.0, 1.0) : 0.0;
	lenY *= lenY;
	len += lenY * w;
}

void fsrEasuTap(thread float3& aC, thread float& aW, float2 off, float2 dir, float2 len, float lob, float clp, float3 c)
{
	float2 v = float2(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len;
	float d2 = min(dot(v, v), clp);
	float wB = 2.0 / 5.0 * d2 - 1.0;
	float wA = lob * d2 - 1.0;
	wB *= wB;
	wA *= wA;
	wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
	float w = wB * wA;
	aC += c * w;
	aW += w;
}

// The upscaled colour at uv, in the source's own texels.
float3 fsrEasu(texture2d<float> tex, float2 resolution, float2 uv)
{
	float2 pp = uv * resolution - 0.5;
	float2 fp = floor(pp);
	pp -= fp;
	int2 p = int2(fp);
	int2 maxTexel = int2(resolution) - 1;
	//    b c
	//  e f g h
	//  i j k l
	//    n o
	float3 bC = fsrTexel(tex, maxTexel, p + int2( 0,-1)); float3 cC = fsrTexel(tex, maxTexel, p + int2( 1,-1));
	float3 eC = fsrTexel(tex, maxTexel, p + int2(-1, 0)); float3 fC = fsrTexel(tex, maxTexel, p + int2( 0, 0));
	float3 gC = fsrTexel(tex, maxTexel, p + int2( 1, 0)); float3 hC = fsrTexel(tex, maxTexel, p + int2( 2, 0));
	float3 iC = fsrTexel(tex, maxTexel, p + int2(-1, 1)); float3 jC = fsrTexel(tex, maxTexel, p + int2( 0, 1));
	float3 kC = fsrTexel(tex, maxTexel, p + int2( 1, 1)); float3 lC = fsrTexel(tex, maxTexel, p + int2( 2, 1));
	float3 nC = fsrTexel(tex, maxTexel, p + int2( 0, 2)); float3 oC = fsrTexel(tex, maxTexel, p + int2( 1, 2));
	float bL = fsrLuma(bC), cL = fsrLuma(cC), eL = fsrLuma(eC), fL = fsrLuma(fC);
	float gL = fsrLuma(gC), hL = fsrLuma(hC), iL = fsrLuma(iC), jL = fsrLuma(jC);
	float kL = fsrLuma(kC), lL = fsrLuma(lC), nL = fsrLuma(nC), oL = fsrLuma(oC);

	float2 dir = float2(0.0);
	float len = 0.0;
	fsrEasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
	fsrEasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
	fsrEasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
	fsrEasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

	float dirR = dot(dir, dir);
	bool zro = dirR < (1.0 / 32768.0);
	dirR = zro ? 1.0 : rsqrt(dirR);
	dir.x = zro ? 1.0 : dir.x;
	dir *= dirR;
	len = len * 0.5;
	len *= len;
	float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
	float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
	float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
	float clp = 1.0 / lob;

	float3 aC = float3(0.0);
	float aW = 0.0;
	fsrEasuTap(aC, aW, float2( 0.0,-1.0) - pp, dir, len2, lob, clp, bC);
	fsrEasuTap(aC, aW, float2( 1.0,-1.0) - pp, dir, len2, lob, clp, cC);
	fsrEasuTap(aC, aW, float2(-1.0, 1.0) - pp, dir, len2, lob, clp, iC);
	fsrEasuTap(aC, aW, float2( 0.0, 1.0) - pp, dir, len2, lob, clp, jC);
	fsrEasuTap(aC, aW, float2( 0.0, 0.0) - pp, dir, len2, lob, clp, fC);
	fsrEasuTap(aC, aW, float2(-1.0, 0.0) - pp, dir, len2, lob, clp, eC);
	fsrEasuTap(aC, aW, float2( 1.0, 1.0) - pp, dir, len2, lob, clp, kC);
	fsrEasuTap(aC, aW, float2( 2.0, 1.0) - pp, dir, len2, lob, clp, lC);
	fsrEasuTap(aC, aW, float2( 2.0, 0.0) - pp, dir, len2, lob, clp, hC);
	fsrEasuTap(aC, aW, float2( 1.0, 0.0) - pp, dir, len2, lob, clp, gC);
	fsrEasuTap(aC, aW, float2( 1.0, 2.0) - pp, dir, len2, lob, clp, oC);
	fsrEasuTap(aC, aW, float2( 0.0, 2.0) - pp, dir, len2, lob, clp, nC);
	float3 min4 = min(min(fC, gC), min(jC, kC));
	float3 max4 = max(max(fC, gC), max(jC, kC));
	return min(max4, max(min4, aC / aW));
}

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

// params: the output's size, then the sharpening (see fsrSharpness).
fragment float4 main0(VertexOut in [[stage_in]], texture2d<float> textureSrc [[texture(0)]], constant float4& params [[buffer(0)]]) {
	const float2 resolution = float2(textureSrc.get_width(), textureSrc.get_height());
	const float fsrSharpness = params.z;
	float3 e = fsrEasu(textureSrc, resolution, in.uv);
	if (fsrSharpness <= 0.0)
		return float4(e, 1.0);
	float2 texStep = 1.0 / params.xy;
	float3 b = fsrEasu(textureSrc, resolution, in.uv - float2(0.0, texStep.y));
	float3 d = fsrEasu(textureSrc, resolution, in.uv - float2(texStep.x, 0.0));
	float3 f = fsrEasu(textureSrc, resolution, in.uv + float2(texStep.x, 0.0));
	float3 h = fsrEasu(textureSrc, resolution, in.uv + float2(0.0, texStep.y));
	float3 mn4 = min(min(b, d), min(f, h));
	float3 mx4 = max(max(b, d), max(f, h));
	float3 hitMin = min(mn4, e) / max(4.0 * mx4, float3(1.0 / 65536.0));
	float3 hitMax = (1.0 - max(mx4, e)) / min(4.0 * mn4 - 4.0, float3(-1.0 / 65536.0));
	float3 lobeRGB = max(-hitMin, hitMax);
	float lobe = max(-(0.25 - 1.0 / 16.0), min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * fsrSharpness;
	return float4(clamp((lobe * (b + d + f + h) + e) / (4.0 * lobe + 1.0), 0.0, 1.0), 1.0);
}
)";

RendererOutputShader::RendererOutputShader(const std::string& vertex_source, const std::string& fragment_source)
{
    std::string finalFragmentSrc;
	switch(g_renderer->GetType())
	{
#ifdef ENABLE_METAL
	case RendererAPI::Metal:
		finalFragmentSrc = fragment_source;
		break;
#endif
	default:
		finalFragmentSrc = PrependFragmentPreamble(fragment_source);
		break;
	}

	m_vertex_shader.reset(g_renderer->shader_create(RendererShader::ShaderType::kVertex, 0, 0, vertex_source, false, false));
	m_fragment_shader.reset(g_renderer->shader_create(RendererShader::ShaderType::kFragment, 0, 0, finalFragmentSrc, false, false));

	m_vertex_shader->PreponeCompilation(true);
	m_fragment_shader->PreponeCompilation(true);

	if (!m_vertex_shader->WaitForCompiled())
		throw std::exception();

	if(!m_fragment_shader->WaitForCompiled())
		throw std::exception();

}

RendererOutputShader::OutputUniformVariables RendererOutputShader::FillUniformBlockBuffer(const LatteTextureView& texture_view, const Vector2i& output_res, const bool padView) const
{
	OutputUniformVariables vars;

	sint32 effectiveWidth, effectiveHeight;
	texture_view.baseTexture->GetEffectiveSize(effectiveWidth, effectiveHeight, 0);
	vars.textureSrcResolution = {(float)effectiveWidth, (float)effectiveHeight};

	vars.nativeResolution = {(float)texture_view.baseTexture->width, (float)texture_view.baseTexture->height};
	vars.outputResolution = output_res;

	vars.applySRGBEncoding = padView ? LatteGPUState.drcBufferUsesSRGB : LatteGPUState.tvBufferUsesSRGB;
	vars.targetGamma = padView ? ActiveSettings::GetDRCGamma() : ActiveSettings::GetTVGamma();
	vars.displayGamma = GetConfig().userDisplayGamma;
	vars.fsrSharpness = FSRSharpness();

	return vars;
}

RendererOutputShader* RendererOutputShader::s_copy_shader;
RendererOutputShader* RendererOutputShader::s_copy_shader_ud;

RendererOutputShader* RendererOutputShader::s_bicubic_shader;
RendererOutputShader* RendererOutputShader::s_bicubic_shader_ud;

RendererOutputShader* RendererOutputShader::s_hermit_shader;
RendererOutputShader* RendererOutputShader::s_hermit_shader_ud;

RendererOutputShader* RendererOutputShader::s_fsr_shader;
RendererOutputShader* RendererOutputShader::s_fsr_shader_ud;

float RendererOutputShader::FSRSharpness()
{
	// RCAS's sharpness is in stops down from the most: 0 the sharpest, each
	// more halving it. 100% the most, down to 2 stops at 1% -- the range
	// AMD's own samples offer -- and none at all at 0.
	const sint32 sharpness = std::clamp<sint32>(GetConfig().fsr_sharpness.GetValue(), 0, kFSRSharpnessMax);
	if (sharpness == 0)
		return 0.0f;
	return std::exp2(-2.0f * (float)(kFSRSharpnessMax - sharpness) / (float)kFSRSharpnessMax);
}

std::string RendererOutputShader::GetOpenGlVertexSource(bool render_upside_down)
{
	// vertex shader
	std::ostringstream vertex_source;
		vertex_source <<
			R"(#version 420
layout(location = 0) smooth out vec2 passUV;

out gl_PerVertex
{
   vec4 gl_Position;
};

void main(){
	vec2 vPos;
	vec2 vUV;
	int vID = gl_VertexID;
)";

		if (render_upside_down)
		{
			vertex_source <<
				R"(	if( vID == 0 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,0.0); }
	else if( vID == 1 ) { vPos = vec2(-1.0,1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 2 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 3 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 4 ) { vPos = vec2(1.0,-1.0); vUV = vec2(1.0,1.0); }
	else if( vID == 5 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,0.0); }
	)";
		}
		else
		{
			vertex_source <<
				R"(	if( vID == 0 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,1.0); }
	else if( vID == 1 ) { vPos = vec2(-1.0,1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 2 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 3 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 4 ) { vPos = vec2(1.0,-1.0); vUV = vec2(1.0,0.0); }
	else if( vID == 5 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,1.0); }
	)";
		}

		vertex_source <<
			R"(	passUV = vUV;
	gl_Position = vec4(vPos, 0.0, 1.0);
}
)";
		return vertex_source.str();
}

std::string RendererOutputShader::GetVulkanVertexSource(bool render_upside_down)
{
	// vertex shader
	std::ostringstream vertex_source;
		vertex_source <<
			R"(#version 450
layout(location = 0) out vec2 passUV;

out gl_PerVertex
{
   vec4 gl_Position;
};

void main(){
	vec2 vPos;
	vec2 vUV;
	int vID = gl_VertexIndex;
)";

		if (render_upside_down)
		{
			vertex_source <<
				R"(	if( vID == 0 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,0.0); }
	else if( vID == 1 ) { vPos = vec2(-1.0,1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 2 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 3 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 4 ) { vPos = vec2(1.0,-1.0); vUV = vec2(1.0,1.0); }
	else if( vID == 5 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,0.0); }
	)";
		}
		else
		{
			vertex_source <<
				R"(	if( vID == 0 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,1.0); }
	else if( vID == 1 ) { vPos = vec2(-1.0,1.0); vUV = vec2(0.0,1.0); }
	else if( vID == 2 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 3 ) { vPos = vec2(-1.0,-1.0); vUV = vec2(0.0,0.0); }
	else if( vID == 4 ) { vPos = vec2(1.0,-1.0); vUV = vec2(1.0,0.0); }
	else if( vID == 5 ) { vPos = vec2(1.0,1.0); vUV = vec2(1.0,1.0); }
	)";
		}

		vertex_source <<
			R"(	passUV = vUV;
	gl_Position = vec4(vPos, 0.0, 1.0);
}
)";
		return vertex_source.str();
}

std::string RendererOutputShader::GetMetalVertexSource(bool render_upside_down)
{
	// vertex shader
	std::ostringstream vertex_source;
		vertex_source <<
			R"(#include <metal_stdlib>
using namespace metal;

struct VertexOut {
    float4 position [[position]];
    float2 uv;
};

vertex VertexOut main0(ushort vid [[vertex_id]]) {
	VertexOut out;
	float2 pos;
	if (vid == 0) pos = float2(-1.0, -3.0);
	else if (vid == 1) pos = float2(-1.0, 1.0);
	else if (vid == 2) pos = float2(3.0, 1.0);
	out.uv = pos * 0.5 + 0.5;
	out.uv.y = 1.0 - out.uv.y;
)";

		if (render_upside_down)
		{
			vertex_source <<
				R"(	pos.y = -pos.y;
	)";
		}

		vertex_source <<
			R"(	out.position = float4(pos, 0.0, 1.0);
	return out;
}
)";
		return vertex_source.str();
}

std::string RendererOutputShader::PrependFragmentPreamble(const std::string& shaderSrc)
{
	return R"(#version 430
layout(location = 0) smooth in vec2 passUV;
layout(binding = 0) uniform sampler2D textureSrc;
layout(location = 0) out vec4 colorOut0;

#ifdef VULKAN
layout (binding = 1, std140)
#else
layout (binding = 0, std140)
#endif
uniform parameters {
uniform vec2 textureSrcResolution;
uniform vec2 nativeResolution;
uniform vec2 outputResolution;
uniform bool applySRGBEncoding;
uniform float targetGamma;
uniform float displayGamma;
uniform float fsrSharpness;
};

float sRGBEncode(float linear)
{
	if(linear <= 0.0031308)
		return 12.92f * linear;
	else
		return 1.055f * pow(linear, 1.0f / 2.4f) - 0.055f;

}

vec3 sRGBEncode(vec3 linear)
{
	return vec3(sRGBEncode(linear.r), sRGBEncode(linear.g), sRGBEncode(linear.b));
}

// fwd. declaration
void outputShader();
void main()
{
	outputShader(); // sets colorOut0
	if(applySRGBEncoding)
		colorOut0 = vec4(sRGBEncode(colorOut0.rgb), 1.0f);

	if (displayGamma > 0.0f)
		colorOut0 = pow(colorOut0, vec4(targetGamma / displayGamma) );
	else
		colorOut0 = vec4( sRGBEncode( pow(colorOut0.rgb, vec3(targetGamma)) ), 1.0f);

}

)" + shaderSrc;
}
void RendererOutputShader::InitializeStatic()
{
	switch(g_renderer->GetType())
	{
#ifdef ENABLE_METAL
    case RendererAPI::Metal:
    {
        std::string vertex_source = GetMetalVertexSource(false);
        std::string vertex_source_ud = GetMetalVertexSource(true);

       	s_copy_shader = new RendererOutputShader(vertex_source, s_copy_shader_source_mtl);
       	s_copy_shader_ud = new RendererOutputShader(vertex_source_ud, s_copy_shader_source_mtl);

       	s_bicubic_shader = new RendererOutputShader(vertex_source, s_bicubic_shader_source_mtl);
       	s_bicubic_shader_ud = new RendererOutputShader(vertex_source_ud, s_bicubic_shader_source_mtl);

       	s_hermit_shader = new RendererOutputShader(vertex_source, s_hermite_shader_source_mtl);
       	s_hermit_shader_ud = new RendererOutputShader(vertex_source_ud, s_hermite_shader_source_mtl);

       	s_fsr_shader = new RendererOutputShader(vertex_source, s_fsr_shader_source_mtl);
       	s_fsr_shader_ud = new RendererOutputShader(vertex_source_ud, s_fsr_shader_source_mtl);
		break;
    }
#endif
#ifdef ENABLE_OPENGL
    case RendererAPI::OpenGL:
    {
    	std::string vertex_source, vertex_source_ud;
    	// vertex shader
		vertex_source = GetOpenGlVertexSource(false);
		vertex_source_ud = GetOpenGlVertexSource(true);

    	s_copy_shader = new RendererOutputShader(vertex_source, s_copy_shader_source);
    	s_copy_shader_ud = new RendererOutputShader(vertex_source_ud, s_copy_shader_source);

    	s_bicubic_shader = new RendererOutputShader(vertex_source, s_bicubic_shader_source);
    	s_bicubic_shader_ud = new RendererOutputShader(vertex_source_ud, s_bicubic_shader_source);

    	s_hermit_shader = new RendererOutputShader(vertex_source, s_hermite_shader_source);
    	s_hermit_shader_ud = new RendererOutputShader(vertex_source_ud, s_hermite_shader_source);

    	s_fsr_shader = new RendererOutputShader(vertex_source, s_fsr_shader_source);
    	s_fsr_shader_ud = new RendererOutputShader(vertex_source_ud, s_fsr_shader_source);
		break;
    }
#endif
#ifdef ENABLE_VULKAN
    case RendererAPI::Vulkan:
    {
    	std::string vertex_source, vertex_source_ud;
    	// vertex shader
		vertex_source = GetVulkanVertexSource(false);
		vertex_source_ud = GetVulkanVertexSource(true);
			
    	s_copy_shader = new RendererOutputShader(vertex_source, s_copy_shader_source);
    	s_copy_shader_ud = new RendererOutputShader(vertex_source_ud, s_copy_shader_source);

    	s_bicubic_shader = new RendererOutputShader(vertex_source, s_bicubic_shader_source);
    	s_bicubic_shader_ud = new RendererOutputShader(vertex_source_ud, s_bicubic_shader_source);

    	s_hermit_shader = new RendererOutputShader(vertex_source, s_hermite_shader_source);
    	s_hermit_shader_ud = new RendererOutputShader(vertex_source_ud, s_hermite_shader_source);

    	s_fsr_shader = new RendererOutputShader(vertex_source, s_fsr_shader_source);
    	s_fsr_shader_ud = new RendererOutputShader(vertex_source_ud, s_fsr_shader_source);
		break;
    }
#endif
	}
}

void RendererOutputShader::ShutdownStatic()
{
	delete s_copy_shader;
	delete s_copy_shader_ud;

	delete s_bicubic_shader;
	delete s_bicubic_shader_ud;

	delete s_hermit_shader;
	delete s_hermit_shader_ud;

	delete s_fsr_shader;
	delete s_fsr_shader_ud;
}
