#include "Cafe/HW/Latte/ISA/RegDefines.h"

#include "Cafe/HW/Latte/Core/Latte.h"
#include "Cafe/HW/Latte/Core/LatteDraw.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/Core/LatteOverlay.h"
#include "Cafe/HW/Latte/Core/LatteBufferCache.h"
#include "Cafe/HW/Latte/Core/LatteTexture.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Renderer/Renderer.h"
#include "Cafe/HW/Latte/Core/LattePerformanceMonitor.h"
#include "Cafe/GraphicPack/GraphicPack2.h"
#include "HW/Latte/Renderer/RendererCore.h"
#include "config/ActiveSettings.h"
#include "WindowSystem.h"
#include "Cafe/OS/libs/erreula/erreula.h"
#include "input/InputManager.h"
#include "Cafe/OS/libs/swkbd/swkbd.h"

#include "DualscreenSession.h"
#include <imgui.h>
#include <chrono>

uint32 prevScissorX = 0;
uint32 prevScissorY = 0;
uint32 prevScissorWidth = 0;
uint32 prevScissorHeight = 0;

bool hasValidFramebufferAttached = false;

struct LatteMRTQuad
{
	sint32 width;
	sint32 height;
};

struct
{
	LatteMRTQuad currentRenderSize;
	LatteMRTQuad currentEffectiveSize;
	struct
	{
		sint32 width;
		sint32 height;
	}currentGuestViewport;
	bool renderTargetIsResized;
	// tracking
	sint32 rtUpdateListCount;
	LatteTextureView* rtUpdateList[64];
	sint32 rtUpdateListSlice[64];
	sint32 rtUpdateListMip[64];
}sLatteRenderTargetState;

struct
{
	struct
	{
		LatteTextureView* view{};
	}colorBuffer[8];
	struct
	{
		LatteTextureView* view{};
		bool hasStencil{false};
	}depthBuffer;
}sLatteCurrentRendertargets{};

LatteCachedFBO::LatteCachedFBO(uint64 key) : key(key)
{
	for (sint32 i = 0; i < 8; i++)
	{
		LatteTextureView* colorTexView = sLatteCurrentRendertargets.colorBuffer[i].view;
		colorBuffer[i].texture = colorTexView;
		if (colorTexView)
		{
			vectorAppendUnique(colorTexView->list_associatedFbo, this);
			m_referencedTextures.emplace_back(colorTexView->baseTexture);
		}
	}

	if (sLatteCurrentRendertargets.depthBuffer.view)
	{
		LatteTextureView* depthTexView = sLatteCurrentRendertargets.depthBuffer.view;
		depthBuffer.texture = depthTexView;
		depthBuffer.hasStencil = sLatteCurrentRendertargets.depthBuffer.hasStencil;
		if (depthTexView)
		{
			vectorAppendUnique(depthTexView->list_associatedFbo, this);
			m_referencedTextures.emplace_back(depthTexView->baseTexture);
		}
	}

	calculateEffectiveRenderAreaSize();
}

void LatteMRT::NotifyTextureDeletion(LatteTexture* texture)
{
	for (sint32 i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
	{
		if (sLatteCurrentRendertargets.colorBuffer[i].view && sLatteCurrentRendertargets.colorBuffer[i].view->baseTexture == texture)
		{
			sLatteCurrentRendertargets.colorBuffer[i].view = nullptr;
		}
	}
}

LatteCachedFBO* LatteMRT::CreateCachedFBO(uint64 key)
{
	return g_renderer->rendertarget_createCachedFBO(key);
}

void LatteMRT::DeleteCachedFBO(LatteCachedFBO* cfbo)
{
	// color textures
	for (sint32 i = 0; i < 8; i++)
	{
		if (cfbo->colorBuffer[i].texture == NULL)
			continue;
		cfbo->colorBuffer[i].texture->list_fboLookup.erase(std::remove(cfbo->colorBuffer[i].texture->list_fboLookup.begin(), cfbo->colorBuffer[i].texture->list_fboLookup.end(), cfbo), cfbo->colorBuffer[i].texture->list_fboLookup.end());
		cfbo->colorBuffer[i].texture->list_associatedFbo.erase(std::remove(cfbo->colorBuffer[i].texture->list_associatedFbo.begin(), cfbo->colorBuffer[i].texture->list_associatedFbo.end(), cfbo), cfbo->colorBuffer[i].texture->list_associatedFbo.end());
	}
	// depth texture
	if (cfbo->depthBuffer.texture)
	{
		cfbo->depthBuffer.texture->list_fboLookup.erase(std::remove(cfbo->depthBuffer.texture->list_fboLookup.begin(), cfbo->depthBuffer.texture->list_fboLookup.end(), cfbo), cfbo->depthBuffer.texture->list_fboLookup.end());
		cfbo->depthBuffer.texture->list_associatedFbo.erase(std::remove(cfbo->depthBuffer.texture->list_associatedFbo.begin(), cfbo->depthBuffer.texture->list_associatedFbo.end(), cfbo), cfbo->depthBuffer.texture->list_associatedFbo.end());
	}
	g_renderer->rendertarget_deleteCachedFBO(cfbo);
	delete cfbo;
}

LatteCachedFBO* g_emptyFBO = nullptr;

void LatteMRT::SetColorAttachment(uint32 index, LatteTextureView* view)
{
	sLatteCurrentRendertargets.colorBuffer[index].view = view;
}

void LatteMRT::SetDepthAndStencilAttachment(LatteTextureView* view, bool hasStencil)
{
	sLatteCurrentRendertargets.depthBuffer.view = view;
	sLatteCurrentRendertargets.depthBuffer.hasStencil = hasStencil;
}

LatteTextureView* LatteMRT::GetColorAttachment(uint32 index)
{
	cemu_assert_debug(index < 8);
	return sLatteCurrentRendertargets.colorBuffer[index].view;
}

LatteTextureView* LatteMRT::GetDepthAttachment()
{
	return sLatteCurrentRendertargets.depthBuffer.view;
}

void LatteMRT::ApplyCurrentState()
{
	uint64 key = 0;
	LatteTextureView* fboLookupView = NULL;
	for (sint32 i = 0; i < 8; i++)
	{
		LatteTextureView* colorView = sLatteCurrentRendertargets.colorBuffer[i].view;
		if (colorView)
		{
			key += ((uint64)colorView);
			key = std::rotl<uint64>(key, 5);
			fboLookupView = colorView;
		}
		key = std::rotl<uint64>(key, 7);
	}
	if (sLatteCurrentRendertargets.depthBuffer.view)
	{
		key += ((uint64)sLatteCurrentRendertargets.depthBuffer.view);
		key = std::rotl<uint64>(key, 5);
		key += (sLatteCurrentRendertargets.depthBuffer.hasStencil);
		if (fboLookupView == NULL)
		{
			fboLookupView = sLatteCurrentRendertargets.depthBuffer.view;
		}
	}
	// use fboLookupTexture to find cached FBO
	if (fboLookupView == nullptr)
	{
		if (!g_emptyFBO)
			g_emptyFBO = CreateCachedFBO(key);
		g_renderer->rendertarget_bindFramebufferObject(g_emptyFBO);
		return;
	}
	// look for FBO
	for (auto& fbo : fboLookupView->list_fboLookup)
	{
		if (fbo->key == key)
		{
			// found matching FBO
			g_renderer->rendertarget_bindFramebufferObject(fbo);
			return;
		}
	}
	// create new cached FBO
	LatteCachedFBO* cfbo = CreateCachedFBO(key);
	g_renderer->rendertarget_bindFramebufferObject(cfbo);
	fboLookupView->list_fboLookup.push_back(cfbo);
	// some extra checks to verify that looked up fbo matches active buffers
	cemu_assert_debug(cfbo->colorBuffer[0].texture == sLatteCurrentRendertargets.colorBuffer[0].view);
	cemu_assert_debug(cfbo->colorBuffer[1].texture == sLatteCurrentRendertargets.colorBuffer[1].view);
	cemu_assert_debug(cfbo->colorBuffer[2].texture == sLatteCurrentRendertargets.colorBuffer[2].view);
	cemu_assert_debug(cfbo->depthBuffer.texture == sLatteCurrentRendertargets.depthBuffer.view);
}

void LatteMRT::BindColorBufferOnly(LatteTextureView* view)
{
	cemu_assert_debug(!view->baseTexture->isDepth);
	SetColorAttachment(0, view);
	for (sint32 i = 1; i < 8; i++)
		SetColorAttachment(i, nullptr);
	SetDepthAndStencilAttachment(nullptr, false);
	ApplyCurrentState();
}

void LatteMRT::BindDepthBufferOnly(LatteTextureView* view)
{
	cemu_assert_debug(view->baseTexture->isDepth);
	for (sint32 i = 0; i < 8; i++)
		SetColorAttachment(i, nullptr);
	SetDepthAndStencilAttachment(view, view->baseTexture->hasStencil);
	ApplyCurrentState();
}

LatteTextureView* LatteMRT_CreateDepthBuffer(MPTR depthBufferPhysMem, uint32 width, uint32 height, uint32 pitch, Latte::E_HWTILEMODE tileMode, Latte::E_GX2SURFFMT format, uint32 swizzle, sint32 viewSlice)
{
	LatteTextureView* textureView = LatteTexture_CreateMapping(depthBufferPhysMem, MPTR_NULL, width, height, viewSlice+1, pitch, tileMode, swizzle, 0, 1, viewSlice, 1, format, viewSlice > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, true);
	LatteMRT::SetDepthAndStencilAttachment(textureView, textureView->baseTexture->hasStencil);
	return textureView;
}

sint32 _depthBufferSizeWarningCount = 0;

LatteTextureView* LatteMRT::GetColorAttachmentTexture(uint32 index, bool createNew, bool checkForTextureChanges)
{
	uint32* colorBufferRegBase = LatteGPUState.contextRegister+(mmCB_COLOR0_BASE + index);
	uint32 regColorBufferBase = colorBufferRegBase[mmCB_COLOR0_BASE - mmCB_COLOR0_BASE] & 0xFFFFFF00; // the low 8 bits are ignored? How to Survive seems to rely on this
	uint32 regColorSize = colorBufferRegBase[mmCB_COLOR0_SIZE - mmCB_COLOR0_BASE];
	uint32 regColorInfo = colorBufferRegBase[mmCB_COLOR0_INFO - mmCB_COLOR0_BASE];
	uint32 regColorView = colorBufferRegBase[mmCB_COLOR0_VIEW - mmCB_COLOR0_BASE];
	// decode color buffer reg info
	Latte::E_HWTILEMODE colorBufferTileMode = (Latte::E_HWTILEMODE)((regColorInfo >> 8) & 0xF);
	uint32 numberType = (regColorInfo >> 12) & 7;
	Latte::E_GX2SURFFMT colorBufferFormat = GetColorBufferFormat(index, LatteGPUState.contextNew);

	MPTR colorBufferPhysMem = regColorBufferBase;

	uint32 colorBufferSwizzle = 0;
	if ( Latte::TM_IsMacroTiled(colorBufferTileMode) )
	{
		colorBufferSwizzle = colorBufferPhysMem & 0x700;
		colorBufferPhysMem = colorBufferPhysMem & ~(7 << 8);
	}

	// get view slice and view slice num
	uint32 viewFirstSlice = (regColorView & 0x7FF);
	uint32 viewNumSlices = ((regColorView >> 13) & 0x7FF) - viewFirstSlice + 1;
	if (viewNumSlices != 1)
	{
		debug_printf("viewNumSlices is not 1! (%d)\n", viewNumSlices);
	}
	uint32 colorBufferPitch = (((regColorSize >> 0) & 0x3FF) + 1);
	colorBufferPitch <<= 3;
	uint32 pitchHeight = (((regColorSize >> 10) & 0xFFFFF) + 1);
	pitchHeight <<= 6;
	uint32 colorBufferHeight = pitchHeight / colorBufferPitch;
	uint32 colorBufferWidth = colorBufferPitch;

	// colorbuffer width/height has to be padded to 8/32 alignment but the actual resolution might be smaller
	// use the scissor box as a clue to figure out the original resolution if possible
	if(LatteGPUState.allowFramebufferSizeOptimization)
	{
		uint32 scissorBoxWidth = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_X();
		uint32 scissorBoxHeight = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();
		if (((scissorBoxWidth + 7) & ~7) == colorBufferWidth)
			colorBufferWidth = scissorBoxWidth;
		if (((colorBufferHeight + 31) & ~31) == colorBufferHeight)
			colorBufferHeight = scissorBoxHeight;
	}

	// log resolution changes if the above heuristic takes effect
	// this is useful to find resolutions which need to be updated in gfx pack texture rules
#if 0
	uint32 colorBufferHeight2 = pitchHeight / colorBufferPitch;
	static std::unordered_set<uint64> s_foundColorBufferResMappings;
	if (colorBufferPitch != colorBufferWidth || colorBufferHeight != colorBufferHeight2)
	{
		// only log unique, source and dest resolution. Encode into a key with 16 bits per component
		uint64 resHash = (uint64)colorBufferWidth | ((uint64)colorBufferHeight << 16) | ((uint64)colorBufferPitch << 32) | ((uint64)colorBufferHeight2 << 48);
		if( !s_foundColorBufferResMappings.contains(resHash) )
		{
			s_foundColorBufferResMappings.insert(resHash);
			cemuLog_log(LogType::Force, "[COLORBUFFER-DBG] Using res {}x{} instead of {}x{}", colorBufferWidth, colorBufferHeight, colorBufferPitch, colorBufferHeight2);
		}
	}
#endif

	bool colorBufferWasFound = false;
	sint32 viewFirstMip = 0; // todo

	cemu_assert_debug(viewNumSlices == 1);

	LatteTextureView* colorBufferView = LatteTextureViewLookupCache::lookupSliceEx(colorBufferPhysMem, colorBufferWidth, colorBufferHeight, colorBufferPitch, viewFirstMip, viewFirstSlice, colorBufferFormat, false);

	if (colorBufferView == nullptr)
	{
		// create color buffer view
		colorBufferView = LatteTexture_CreateMapping(colorBufferPhysMem, 0, colorBufferWidth, colorBufferHeight, (viewFirstSlice + viewNumSlices), colorBufferPitch, colorBufferTileMode, colorBufferSwizzle>>8, viewFirstMip, 1, viewFirstSlice, viewNumSlices, (Latte::E_GX2SURFFMT)colorBufferFormat, (viewFirstSlice + viewNumSlices)>1? Latte::E_DIM::DIM_2D_ARRAY: Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, false, true);
		LatteGPUState.repeatTextureInitialization = true;
		checkForTextureChanges = false;
	}

	if (colorBufferView->baseTexture->swizzle != colorBufferSwizzle)
	{
		colorBufferView->baseTexture->lastRenderTargetSwizzle = colorBufferSwizzle;
	}

	// check for texture changes
	if (checkForTextureChanges)
		LatteTexture_UpdateDataToLatest(colorBufferView->baseTexture);
	// mark as used
	LatteTC_MarkTextureStillInUse(colorBufferView->baseTexture);
	return colorBufferView;
}

// get mask of all used color buffers
uint8 LatteMRT::GetActiveColorBufferMask(const LatteDecompilerShader* pixelShader, const LatteContextRegister& lcr)
{
	if (!pixelShader) [[unlikely]]
		return 0;
	const uint32* regView = lcr.GetRawView();
	// check if color buffer output is active
	const Latte::LATTE_CB_COLOR_CONTROL& colorControlReg = lcr.CB_COLOR_CONTROL;
	uint32 colorBufferDisable = colorControlReg.get_SPECIAL_OP() == Latte::LATTE_CB_COLOR_CONTROL::E_SPECIALOP::DISABLE;
	if (colorBufferDisable)
		return 0;
	cemu_assert_debug(colorControlReg.get_DEGAMMA_ENABLE() == false); // not supported
	// start with color buffer mask from pixel shader output
	uint8 colorBufferMask = pixelShader->pixelColorOutputMask;
	// combine color buffer mask with color channel mask from mmCB_TARGET_MASK (disable render buffer if all colors are blocked)
	uint32 channelTargetMask = lcr.CB_TARGET_MASK.get_MASK();
	for (uint32 i = 0; i < 8; i++)
	{
		if (((channelTargetMask >> (i * 4)) & 0xF) == 0)
			colorBufferMask &= ~(1 << i);
	}
	// render targets smaller than the scissor size are not allowed
	// this fixes a few render issues in Cemu but we dont know if this matches HW behavior
	// also check for color buffers without a valid pointer
	cemu_assert_debug(lcr.PA_SC_GENERIC_SCISSOR_TL.get_WINDOW_OFFSET_DISABLE() == true); // todo (not exposed by GX2 API)
	uint32 scissorAccessWidth = lcr.PA_SC_GENERIC_SCISSOR_BR.get_BR_X();
	uint32 scissorAccessHeight = lcr.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y();
	for (uint32 i = 0; i < 8; i++)
	{
		if( (colorBufferMask&(1<<i)) == 0 )
			continue;
		if (regView[mmCB_COLOR0_BASE + i] == MPTR_NULL) [[unlikely]]
			colorBufferMask &= ~(1 << i);
		// get width/height
		uint32 regColorSize = regView[mmCB_COLOR0_SIZE + i];
		uint32 regColorInfo = regView[mmCB_COLOR0_INFO + i];
		// decode color buffer reg info
		uint32 colorBufferPitch = (((regColorSize >> 0) & 0x3FF) + 1);
		colorBufferPitch <<= 3;
		uint32 pitchHeight = (((regColorSize >> 10) & 0xFFFFF) + 1);
		pitchHeight <<= 6;
		uint32 colorBufferHeight = pitchHeight / colorBufferPitch;
		uint32 colorBufferWidth = colorBufferPitch;

		if ((colorBufferWidth < (sint32)scissorAccessWidth) || (colorBufferHeight < (sint32)scissorAccessHeight))
		{
            // log this?
			colorBufferMask &= ~(1<<i);
		}
	}
	return colorBufferMask;
}

// returns true if depth/stencil buffer is used
bool LatteMRT::GetActiveDepthBufferMask(const LatteContextRegister& lcr)
{
	bool depthBufferMask = true;
	// if depth test is not used then detach the depth buffer
	bool depthEnable = lcr.DB_DEPTH_CONTROL.get_Z_ENABLE();
	bool stencilTestEnable = lcr.DB_DEPTH_CONTROL.get_STENCIL_ENABLE();
	bool backStencilEnable = lcr.DB_DEPTH_CONTROL.get_BACK_STENCIL_ENABLE();

	if (!depthEnable && !stencilTestEnable && !backStencilEnable)
		depthBufferMask = false;

	return depthBufferMask;
}

const uint32 _colorBufferFormatBits[] =
{
	0, // 0
	0x200, // 1
	0, // 2
	0, // 3
	0x100, // 4
	0x300, // 5
	0x400, // 6
	0x800, // 7
};

Latte::E_GX2SURFFMT LatteMRT::GetColorBufferFormat(const uint32 index, const LatteContextRegister& lcr)
{
	cemu_assert_debug(index < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS);
	uint32 regColorInfo = lcr.GetRawView()[mmCB_COLOR0_INFO + index];
	uint32 colorBufferFormat = (regColorInfo >> 2) & 0x3F; // base HW format
	uint32 numberType = (regColorInfo >> 12) & 7;
	colorBufferFormat |= _colorBufferFormatBits[numberType];
	return (Latte::E_GX2SURFFMT)colorBufferFormat;
}

// return GX2 format of current depth buffer
Latte::E_GX2SURFFMT LatteMRT::GetDepthBufferFormat(const LatteContextRegister& lcr)
{
	uint32 regDepthBufferInfo = lcr.GetRawView()[mmDB_DEPTH_INFO];
	switch (regDepthBufferInfo & 7)
	{
	case 1:
		return Latte::E_GX2SURFFMT::D16_UNORM;
	case 3:
		return Latte::E_GX2SURFFMT::D24_S8_UNORM;
	case 5:
		return Latte::E_GX2SURFFMT::D24_S8_FLOAT;
	case 6:
		return Latte::E_GX2SURFFMT::D32_FLOAT;
	case 7:
		return Latte::E_GX2SURFFMT::D32_S8_FLOAT;
	default:
		debug_printf("Invalid DB_DEPTH_INFO depthbuffer format (%d)\n", (regDepthBufferInfo & 7));
		break;
	}
	return Latte::E_GX2SURFFMT::D16_UNORM;
}

bool LatteMRT::UpdateCurrentFBO()
{
	catchOpenGLError();
	sLatteRenderTargetState.rtUpdateListCount = 0;
	// combine color buffer mask with pixel output mask from pixel shader
	LatteDecompilerShader* pixelShader = LatteSHRC_GetActivePixelShader();
	uint8 colorBufferMask = GetActiveColorBufferMask(pixelShader, LatteGPUState.contextNew);
	bool depthBufferMask = GetActiveDepthBufferMask(LatteGPUState.contextNew);

	bool hasResizedTexture = false; // set to true if any of the color buffers or the depth buffer reference a resized texture (via graphic pack texture rules)
	sLatteRenderTargetState.renderTargetIsResized = false;
	// real size
	LatteMRTQuad* rtRealSize = &sLatteRenderTargetState.currentRenderSize;
	rtRealSize->width = 0;
	rtRealSize->height = 0;
	// effective size (differs from real size only if graphic pack rules overwrite texture sizes)
	LatteMRTQuad* rtEffectiveSize = &sLatteRenderTargetState.currentEffectiveSize;
	rtEffectiveSize->width = 0;
	rtEffectiveSize->height = 0;
	// get scissor box
	cemu_assert_debug(LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_WINDOW_OFFSET_DISABLE() == true); // todo (not exposed by GX2 API?)
	uint32 scissorX = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_TL_X();
	uint32 scissorY = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_TL_Y();
	uint32 scissorWidth = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_X() - scissorX;
	uint32 scissorHeight = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y() - scissorY;
	uint32 scissorAccessWidth = scissorX + scissorWidth;
	uint32 scissorAccessHeight = scissorY + scissorHeight;
	// color buffers
	for (uint32 i = 0; i < Latte::GPU_LIMITS::NUM_COLOR_ATTACHMENTS; i++)
	{
		if (((colorBufferMask)&(1 << i)) == 0)
		{
			// unbind
			SetColorAttachment(i, nullptr);
			continue;
		}
		LatteTextureView* colorAttachmentView = GetColorAttachmentTexture(i, true, true);
		SetColorAttachment(i, colorAttachmentView);
		// after the drawcall mark the texture as updated
		sLatteRenderTargetState.rtUpdateList[sLatteRenderTargetState.rtUpdateListCount] = colorAttachmentView;
		sLatteRenderTargetState.rtUpdateListCount++;

		sint32 colorAttachmentWidth, colorAttachmentHeight;
		colorAttachmentView->baseTexture->GetSize(colorAttachmentWidth, colorAttachmentHeight, colorAttachmentView->firstMip);

		// set effective size
		sint32 effectiveWidth, effectiveHeight;
		colorAttachmentView->baseTexture->GetEffectiveSize(effectiveWidth, effectiveHeight, colorAttachmentView->firstMip);
		if (rtEffectiveSize->width == 0 && rtEffectiveSize->height == 0)
		{
			rtEffectiveSize->width = effectiveWidth;
			rtEffectiveSize->height = effectiveHeight;
		}
		else if (rtEffectiveSize->width != effectiveWidth && rtEffectiveSize->height != effectiveHeight)
		{
			cemuLog_logDebug(LogType::Force, "Color buffer size mismatch ({}x{}). Effective size: {}x{} Real size: {}x{} Mismatching texture: {:08x} {}x{} fmt {:04x}", rtEffectiveSize->width, rtEffectiveSize->height, effectiveWidth, effectiveHeight, colorAttachmentView->baseTexture->width, colorAttachmentView->baseTexture->height, colorAttachmentView->baseTexture->physAddress, colorAttachmentView->baseTexture->width, colorAttachmentView->baseTexture->height, (uint32)colorAttachmentView->baseTexture->format);
		}
		// currently the first color attachment defines the size of the current render target
		if (rtRealSize->width == 0 && rtRealSize->height == 0)
		{
			rtRealSize->width = colorAttachmentWidth;
			rtRealSize->height = colorAttachmentHeight;
		}

		if (colorAttachmentView)
			continue;
	}
	// depth buffer
	if (depthBufferMask)
	{
		uint32 regDepthBuffer = LatteGPUState.contextRegister[mmDB_HTILE_DATA_BASE];
		uint32 regDepthSize = LatteGPUState.contextRegister[mmDB_DEPTH_SIZE];
		uint32 regDepthBufferInfo = LatteGPUState.contextRegister[mmDB_DEPTH_INFO];

		// get format and tileMode from info reg
		Latte::E_GX2SURFFMT depthBufferFormat = GetDepthBufferFormat(LatteGPUState.contextNew);
		Latte::E_HWTILEMODE depthBufferTileMode = (Latte::E_HWTILEMODE)((regDepthBufferInfo >> 15) & 0xF);
		MPTR depthBufferPhysMem = regDepthBuffer << 8;

		uint32 depthBufferPitch = (((regDepthSize >> 0) & 0x3FF) + 1);
		uint32 depthBufferHeight = ((((regDepthSize >> 10) & 0xFFFFF) + 1) / depthBufferPitch);
		depthBufferPitch <<= 3;
		depthBufferHeight <<= 3;
		uint32 depthBufferWidth = depthBufferPitch;

		if (depthBufferWidth < 2)
		{
			debug_printf("depthBufferWidth has invalid value %d\n", depthBufferWidth);
			depthBufferWidth = 2;
		}
		if (depthBufferHeight < 2)
		{
			debug_printf("depthBufferHeight has invalid value %d\n", depthBufferHeight);
			depthBufferHeight = 2;
		}

		bool blockDepthBuffer = false;
		if (scissorAccessWidth > depthBufferPitch || scissorAccessHeight > depthBufferHeight)
		{
			SetDepthAndStencilAttachment(nullptr, false);
			blockDepthBuffer = true;
			// set effective size
			if( rtEffectiveSize->width == 0 && rtEffectiveSize->height == 0 )
			{
				rtEffectiveSize->width = rtRealSize->width;
				rtEffectiveSize->height = rtRealSize->height;
			}
		}

		if (blockDepthBuffer == false)
		{
			if (rtRealSize->width == 0)
			{
				rtRealSize->width = depthBufferWidth;
				rtRealSize->height = depthBufferHeight;
			}
			uint32 regDepthView = LatteGPUState.contextRegister[mmDB_DEPTH_VIEW];
			uint32 depthBufferViewFirstSlice = (regDepthView & 0x7FF);
			uint32 depthBufferViewNumSlices = ((regDepthView >> 13) & 0x7FF) - depthBufferViewFirstSlice + 1;
			cemu_assert_debug(depthBufferViewNumSlices == 1); // binding multiple layers makes no sense?

			uint32 depthBufferSwizzle = 0;
			if (Latte::TM_IsMacroTiled(depthBufferTileMode))
			{
				depthBufferSwizzle = (depthBufferPhysMem >> 8) & 7;
				depthBufferPhysMem = depthBufferPhysMem & ~(7 << 8);
			}

			if (depthBufferPhysMem != MPTR_NULL)
			{
				LatteTextureView* depthBufferView = LatteTextureViewLookupCache::lookupSliceEx(depthBufferPhysMem, depthBufferWidth, depthBufferHeight, depthBufferPitch, 0, depthBufferViewFirstSlice, depthBufferFormat, true);
				if (!depthBufferView)
				{
					// create new depth buffer view and if it doesn't exist then also create the texture
					depthBufferView = LatteTexture_CreateMapping(depthBufferPhysMem, 0, depthBufferWidth, depthBufferHeight, depthBufferViewFirstSlice+1, depthBufferPitch, depthBufferTileMode, depthBufferSwizzle, 0, 1, depthBufferViewFirstSlice, 1, depthBufferFormat, depthBufferViewFirstSlice > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, true, true);
					LatteGPUState.repeatTextureInitialization = true;
				}
				else
				{
					// check for texture changes
					LatteTexture_UpdateDataToLatest(depthBufferView->baseTexture);
				}
				// set effective size
				sint32 effectiveWidth, effectiveHeight;
				depthBufferView->baseTexture->GetEffectiveSize(effectiveWidth, effectiveHeight, depthBufferView->firstMip);
				if (rtEffectiveSize->width == 0 && rtEffectiveSize->height == 0)
				{
					rtEffectiveSize->width = effectiveWidth;
					rtEffectiveSize->height = effectiveHeight;
				}
				else if (rtEffectiveSize->width > effectiveWidth && rtEffectiveSize->height > effectiveHeight)
				{
					if (_depthBufferSizeWarningCount < 100)
					{
						cemuLog_logDebug(LogType::Force, "Depth buffer size too small. Effective size: {}x{} Real size: {}x{} Mismatching texture: {:08x} {}x{} fmt {:04x}", effectiveWidth, effectiveHeight, depthBufferView->baseTexture->width, depthBufferView->baseTexture->height, depthBufferView->baseTexture->physAddress, depthBufferView->baseTexture->width, depthBufferView->baseTexture->height, (uint32)depthBufferView->baseTexture->format);
						_depthBufferSizeWarningCount++;
					}
				}
				LatteTC_MarkTextureStillInUse(depthBufferView->baseTexture);
				// after the drawcall mark the texture as updated
				sLatteRenderTargetState.rtUpdateList[sLatteRenderTargetState.rtUpdateListCount] = depthBufferView;
				sLatteRenderTargetState.rtUpdateListCount++;
				SetDepthAndStencilAttachment(depthBufferView, depthBufferView->baseTexture->hasStencil);
			}
		}
		else
		{
			SetDepthAndStencilAttachment(nullptr, false);
		}
	}
	else
	{
		SetDepthAndStencilAttachment(nullptr, false);
	}
	catchOpenGLError();
	if (colorBufferMask || depthBufferMask)
	{
		hasValidFramebufferAttached = true;
	}
	else
	{
		hasValidFramebufferAttached = false;
		return true;
	}
	if( rtEffectiveSize->width != rtRealSize->width || rtEffectiveSize->height != rtRealSize->height )
	{
		//debug_printf("RenderTarget rescaled. Real: %dx%d Resized: %dx%d\n", rtRealSize->width, rtRealSize->height, rtEffectiveSize->width, rtEffectiveSize->height);
		sLatteRenderTargetState.renderTargetIsResized = true;
	}
	if (sLatteRenderTargetState.currentEffectiveSize.width == 0)
	{
		debug_printf("Render target effective size is 0. May indicate a bug in Cemu or invalid color/depth buffers\n");
		return false;
	}
	return true;
}

// return a vec4 with xy being the scale ratio for render target extend and zw being the virtual viewport dimensions
void LatteMRT::GetCurrentFragCoordScale(float* coordScale)
{
	if (sLatteRenderTargetState.renderTargetIsResized)
	{
		coordScale[0] = (float)sLatteRenderTargetState.currentRenderSize.width / (float)sLatteRenderTargetState.currentEffectiveSize.width;
		coordScale[1] = (float)sLatteRenderTargetState.currentRenderSize.height / (float)sLatteRenderTargetState.currentEffectiveSize.height;
		coordScale[2] = sLatteRenderTargetState.currentGuestViewport.width;
		coordScale[3] = sLatteRenderTargetState.currentGuestViewport.height;
	}
	else
	{
		coordScale[0] = 1.0f;
		coordScale[1] = 1.0f;
		coordScale[2] = sLatteRenderTargetState.currentGuestViewport.width;
		coordScale[3] = sLatteRenderTargetState.currentGuestViewport.height;
	}
}

void LatteMRT::GetVirtualViewportDimensions(sint32& width, sint32& height)
{
	width = sLatteRenderTargetState.currentGuestViewport.width;
	height = sLatteRenderTargetState.currentGuestViewport.height;
}

// flag all FBO textures as updated via GPU
// also handle texture readback
void LatteRenderTarget_trackUpdates()
{
	// after the drawcall, mark the render target textures as dynamically updated
	uint64 eventCounter = LatteTexture_getNextUpdateEventCounter();
	for (sint32 i = 0; i < sLatteRenderTargetState.rtUpdateListCount; i++)
	{
		LatteTextureView* texView = sLatteRenderTargetState.rtUpdateList[i];
		LatteTexture* baseTexture = texView->baseTexture;
		LatteTexture_TrackTextureGPUWrite(baseTexture, texView->firstSlice, texView->firstMip, eventCounter);
		// texture readback
		if (baseTexture->enableReadback)
		{
			LatteTextureReadback_Initate(texView);
		}
	}
}

void LatteRenderTarget_itHLESwapScanBuffer()
{
	performanceMonitor.cycle[performanceMonitor.cycleIndex].frameCounter++;
	if(LatteGPUState.frameCounter > 5)
		performanceMonitor.gpuTime_frameTime.endMeasuring();
	LattePerformanceMonitor_frameEnd();
	LatteGPUState.frameCounter++;
	g_renderer->SwapBuffers(true, true);

	catchOpenGLError();
	performanceMonitor.gpuTime_frameTime.beginMeasuring();

	LatteTC_CleanupUnusedTextures();
	LatteSHRC_CleanupShaderStateCache();
#ifdef ENABLE_OPENGL
	LatteDraw_cleanupAfterFrame();
#endif
	LatteQuery_CancelActiveGPU7Queries();
	LatteBufferCache_notifySwapTVScanBuffer();
	LattePerformanceMonitor_frameBegin();
}

void LatteRenderTarget_applyTextureColorClear(LatteTexture* texture, uint32 sliceIndex, uint32 mipIndex, float r, float g, float b, float a, uint64 eventCounter)
{
	if (texture->isDepth)
	{
		cemuLog_logDebug(LogType::Force, "Unsupported clear depth as color");
	}
	else
	{
		g_renderer->texture_clearColorSlice(texture, sliceIndex, mipIndex, r, g, b, a);
		LatteTexture_MarkDynamicTextureAsChanged(texture->baseView, sliceIndex, mipIndex, eventCounter);
	}
}

void LatteRenderTarget_applyTextureDepthClear(LatteTexture* texture, uint32 sliceIndex, uint32 mipIndex, bool hasDepthClear, bool hasStencilClear, float depthValue, uint8 stencilValue, uint64 eventCounter)
{
	if(texture->isDepth)
	{
		g_renderer->texture_clearDepthSlice(texture, sliceIndex, mipIndex, hasDepthClear, hasStencilClear, depthValue, stencilValue);
	}
	else
	{
		// clearing a color texture using depth clear
		if (hasStencilClear)
			return; // operation likely not intended as a color clear
		//cemu_assert_debug(!hasStencilClear);
		if (hasDepthClear)
		{
			g_renderer->texture_clearColorSlice(texture, sliceIndex, mipIndex, depthValue, depthValue, depthValue, depthValue);
		}

	}
	LatteTexture_MarkDynamicTextureAsChanged(texture->baseView, sliceIndex, mipIndex, eventCounter);
}

void LatteRenderTarget_itHLEClearColorDepthStencil(uint32 clearMask,
												   MPTR colorBufferMPTR, Latte::E_GX2SURFFMT colorBufferFormat, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferPitch, uint32 colorBufferViewFirstSlice, uint32 colorBufferViewNumSlice,
												   MPTR depthBufferMPTR, Latte::E_GX2SURFFMT depthBufferFormat, Latte::E_HWTILEMODE depthBufferTileMode, sint32 depthBufferWidth, sint32 depthBufferHeight, sint32 depthBufferPitch, sint32 depthBufferViewFirstSlice, sint32 depthBufferViewNumSlice,
												   float r, float g, float b, float a, float clearDepth, uint32 clearStencil)
{
	uint32 depthBufferMipIndex = 0; // todo
	uint32 colorBufferMipIndex = 0; // todo

	bool hasColorClear = (clearMask & 1);
	bool hasDepthClear = (clearMask & 2);
	bool hasStencilClear = (clearMask & 4);

	// extract swizzle offset from pointer
	uint32 colorBufferSwizzle = 0;
	uint32 depthBufferSwizzle = 0;
	if (Latte::TM_IsMacroTiled(colorBufferTilemode))
	{
		colorBufferSwizzle = (colorBufferMPTR >> 8) & 7;
		colorBufferMPTR = colorBufferMPTR & ~(7 << 8);
	}
	if (Latte::TM_IsMacroTiled(depthBufferTileMode))
	{
		depthBufferSwizzle = (depthBufferMPTR >> 8) & 7;
		depthBufferMPTR = depthBufferMPTR & ~(7 << 8);
	}

	cemu_assert_debug(colorBufferViewNumSlice <= 1);
	cemu_assert_debug(depthBufferViewNumSlice <= 1);

	// clear color buffer (if flag set)
	uint64 eventCounter = LatteTexture_getNextUpdateEventCounter();
	if ((clearMask & 1) != 0 && colorBufferMPTR != MPTR_NULL && colorBufferWidth > 0 && colorBufferHeight > 0)
	{
		// clear color
		sint32 searchIndex = 0;
		bool targetFound = false;
		while (true)
		{
			LatteTextureView* colorView = LatteTC_LookupTextureByData(colorBufferMPTR, colorBufferWidth, colorBufferHeight, colorBufferPitch, 0, 1, colorBufferViewFirstSlice, 1, &searchIndex);
			if (!colorView)
				break;
			if (Latte::GetFormatBits(colorBufferFormat) != Latte::GetFormatBits(colorView->baseTexture->format))
				continue;

			if (colorView->baseTexture->pitch == colorBufferPitch && colorView->baseTexture->height == colorBufferHeight)
				targetFound = true;

			LatteRenderTarget_applyTextureColorClear(colorView->baseTexture, colorBufferViewFirstSlice, colorBufferMipIndex, r, g, b, a, eventCounter);

		}
		if (targetFound == false)
		{
			// create new texture with matching format
			cemu_assert_debug(colorBufferViewNumSlice <= 1);
			LatteTextureView* newColorView = LatteTexture_CreateMapping(colorBufferMPTR, MPTR_NULL, colorBufferWidth, colorBufferHeight, colorBufferViewFirstSlice+1, colorBufferPitch, colorBufferTilemode, colorBufferSwizzle, 0, 1, colorBufferViewFirstSlice, 1, colorBufferFormat, colorBufferViewFirstSlice > 0 ? Latte::E_DIM::DIM_2D_ARRAY : Latte::E_DIM::DIM_2D, Latte::E_DIM::DIM_2D, false);
			LatteRenderTarget_applyTextureColorClear(newColorView->baseTexture, colorBufferViewFirstSlice, colorBufferMipIndex, r, g, b, a, eventCounter);
		}
	}
	// clear depth or stencil buffer (if flag set)
	if ((hasDepthClear || hasStencilClear) && depthBufferMPTR != MPTR_NULL)
	{
		std::vector<LatteTexture*> list_depthClearTextures;
		LatteTC_LookupTexturesByPhysAddr(depthBufferMPTR, list_depthClearTextures);
		bool foundMatchingDepthBuffer = false;
		// todo - support for clearing depth mips?
		cemu_assert_debug(depthBufferViewNumSlice == 1);

		for (auto& texItr : list_depthClearTextures)
		{
			// only clear depth buffers if they are smaller
			if (texItr->pitch > depthBufferPitch)
				continue;
			if (depthBufferViewFirstSlice >= texItr->depth)
				continue; // slice out of range

			if (texItr->pitch == depthBufferPitch && texItr->height == depthBufferHeight)
				foundMatchingDepthBuffer = true;

			// todo - calculate actual sliceIndex and mipIndex since the textures in list_depthClearTextures dont necessarily share the same base
			LatteRenderTarget_applyTextureDepthClear(texItr, depthBufferViewFirstSlice, depthBufferMipIndex, hasDepthClear, hasStencilClear, clearDepth, clearStencil, eventCounter);
		}

		if (foundMatchingDepthBuffer == false)
		{
			LatteTextureView* newDepthBufferView = LatteMRT_CreateDepthBuffer(depthBufferMPTR, depthBufferWidth, depthBufferHeight, depthBufferPitch, depthBufferTileMode, (Latte::E_GX2SURFFMT)depthBufferFormat, depthBufferSwizzle, depthBufferViewFirstSlice);
			LatteRenderTarget_applyTextureDepthClear(newDepthBufferView->baseTexture, depthBufferViewFirstSlice, depthBufferMipIndex, hasDepthClear, hasStencilClear, clearDepth, clearStencil, eventCounter);
		}
	}
}

sint32 _currentOutputImageWidth = 0;
sint32 _currentOutputImageHeight = 0;

void LatteRenderTarget_getScreenImageArea(sint32* x, sint32* y, sint32* width, sint32* height, sint32* fullWidth, sint32* fullHeight, bool padView)
{
	int w, h;
	if(padView && WindowSystem::IsPadWindowOpen())
		WindowSystem::GetPadWindowPhysSize(w, h);
	else
		WindowSystem::GetWindowPhysSize(w, h);

	sint32 scaledOutputX;
	sint32 scaledOutputY;
	if (GetConfig().fullscreen_scaling == kKeepAspectRatio)
	{
		// calculate maximum possible resolution with intact aspect ratio
		scaledOutputX = w;
		scaledOutputY = _currentOutputImageHeight * w / std::max(_currentOutputImageWidth, 1);
		if (scaledOutputY > h)
		{
			scaledOutputX = _currentOutputImageWidth * h / std::max(_currentOutputImageHeight, 1);
			scaledOutputY = h;
		}
	}
	else
	{
		scaledOutputX = w;
		scaledOutputY = h;
	}

	*x = (w - scaledOutputX) / 2;
	*y = (h - scaledOutputY) / 2;
	*width = scaledOutputX;
	*height = scaledOutputY;
	if (fullWidth)
		*fullWidth = w;
	if (fullHeight)
		*fullHeight = h;
}

namespace coreinit
{
	void DualscreenRequestBackground();
	void DualscreenRequestForeground();
}

// Dualscreen asking for the game to pause or go on, handed to coreinit, which
// the session cannot reach itself -- see DualscreenSession::TakePauseRequest().
static void LatteRenderTarget_handleDualscreenPause()
{
	for (bool pause = false; DualscreenSession::instance().TakePauseRequest(pause);)
	{
		if (pause)
			coreinit::DualscreenRequestBackground();
		else
			coreinit::DualscreenRequestForeground();
	}
}

void LatteRenderTarget_tapDualscreen(LatteTextureView* textureView, bool screenIsPad)
{
	// Unconditional, unlike the capture below: ShouldCapture() only knows what
	// Poll() last read off the link, so it has to run even when nothing is
	// currently being captured, or a shell that starts asking for a screen
	// after a quiet spell would never be noticed asking.
	DualscreenSession::instance().Poll();
	LatteRenderTarget_handleDualscreenPause();
	// Only the screen a shell actually asked for -- see ShouldCapture(). A
	// screen nobody asked for costs nothing here: no readback, no CPU
	// conversion, no send.
	if (!DualscreenSession::instance().ShouldCapture(screenIsPad ? ds::ScreenId::Bottom
																 : ds::ScreenId::Top))
		return;
	// The two lines a backbuffer copy always starts with. Without them the
	// texture HandleDualscreenFrame reads can be stale or not yet uploaded,
	// since on the screen this machine is not showing nothing else need have
	// touched it this frame.
	LatteTexture_UpdateDataToLatest(textureView->baseTexture);
	LatteTC_MarkTextureStillInUse(textureView->baseTexture);
	g_renderer->HandleDualscreenFrame(textureView, screenIsPad);
}

// Dualscreen's quick menu, over the game in this window while it is up --
// see DualscreenSession::TakeOverlay(). Drawn last, over everything, fitted to
// the window at the shape Dualscreen drew it, with the dimming it has at its
// edges carried out to the window's.
//
// A new picture goes into a new texture, made before ImGui's frame begins --
// see g_imguiBeforeFrame -- and only drawn from two frames later, once its
// upload has run, with the old one kept up meanwhile. The old one is deleted
// then, which waits for the GPU, so new ones are made no more often than
// every 50ms: plenty for a cursor moving through a menu.
namespace
{
	struct DualscreenMenuPicture
	{
		ImTextureID texture = nullptr;
		int width = 0;
		int height = 0;
		uint8 edge[4] = {0, 0, 0, 0};
	};
	DualscreenMenuPicture s_menuShown;
	DualscreenMenuPicture s_menuIncoming;
	int s_menuIncomingFrames = 0;
	std::vector<uint8> s_menuPixels;
	int s_menuWidth = 0;
	int s_menuHeight = 0;
	bool s_menuPending = false;
	std::chrono::steady_clock::time_point s_menuMade;

	void ReleaseMenuPicture(DualscreenMenuPicture& picture)
	{
		if (picture.texture)
			g_renderer->DeleteTexture(picture.texture);
		picture = DualscreenMenuPicture();
	}

	// Outside any render pass: everything that makes or deletes a texture.
	void PrepareDualscreenMenu(bool mainWindow)
	{
		if (!mainWindow)
			return;
		DualscreenSession& session = DualscreenSession::instance();
		if (!session.OverlayVisible())
		{
			ReleaseMenuPicture(s_menuIncoming);
			ReleaseMenuPicture(s_menuShown);
			s_menuPending = false;
			return;
		}
		std::vector<uint8_t> arrived;
		int width = 0;
		int height = 0;
		if (session.TakeOverlay(arrived, width, height) &&
			arrived.size() == static_cast<size_t>(width) * height * 4)
		{
			s_menuPixels.assign(arrived.begin(), arrived.end());
			s_menuWidth = width;
			s_menuHeight = height;
			s_menuPending = true;
		}
		if (s_menuIncoming.texture && ++s_menuIncomingFrames >= 2)
		{
			ReleaseMenuPicture(s_menuShown);
			s_menuShown = s_menuIncoming;
			s_menuIncoming = DualscreenMenuPicture();
		}
		const auto now = std::chrono::steady_clock::now();
		if (s_menuPending && !s_menuIncoming.texture &&
			now - s_menuMade >= std::chrono::milliseconds(50))
		{
			DualscreenMenuPicture picture;
			picture.texture = g_renderer->GenerateTextureRGBA(s_menuPixels, {s_menuWidth, s_menuHeight});
			picture.width = s_menuWidth;
			picture.height = s_menuHeight;
			std::copy(s_menuPixels.begin(), s_menuPixels.begin() + 4, picture.edge);
			s_menuMade = now;
			s_menuPending = false;
			s_menuIncoming = picture;
			s_menuIncomingFrames = 0;
		}
	}

	[[maybe_unused]] const bool s_menuHooked = (g_imguiBeforeFrame = &PrepareDualscreenMenu, true);
}

static void LatteRenderTarget_drawDualscreenMenu()
{
	DualscreenSession& session = DualscreenSession::instance();
	const DualscreenMenuPicture& shown = s_menuShown;
	if (!session.OverlayVisible() || !shown.texture || shown.width <= 0 || shown.height <= 0)
		return;

	ImDrawList* draw = ImGui::GetForegroundDrawList();
	const ImVec2 window = ImGui::GetIO().DisplaySize;
	const float scale = std::min(window.x / shown.width, window.y / shown.height);
	const ImVec2 size(shown.width * scale, shown.height * scale);
	const ImVec2 from((window.x - size.x) * 0.5f, (window.y - size.y) * 0.5f);
	const ImVec2 to(from.x + size.x, from.y + size.y);
	session.SetOverlayRect(from.x, from.y, to.x, to.y);
	// The bars either side, dimmed as the menu's own edges are.
	const ImU32 dim = IM_COL32(shown.edge[0], shown.edge[1], shown.edge[2], shown.edge[3]);
	if (from.x > 0.5f)
	{
		draw->AddRectFilled(ImVec2(0, 0), ImVec2(from.x, window.y), dim);
		draw->AddRectFilled(ImVec2(to.x, 0), window, dim);
	}
	if (from.y > 0.5f)
	{
		draw->AddRectFilled(ImVec2(0, 0), ImVec2(window.x, from.y), dim);
		draw->AddRectFilled(ImVec2(0, to.y), window, dim);
	}
	draw->AddImage(shown.texture, from, to);
}

void LatteRenderTarget_copyToBackbuffer(LatteTextureView* textureView, bool isPadView)
{
	// make sure texture is updated to latest data in cache
	LatteTexture_UpdateDataToLatest(textureView->baseTexture);
	// mark source texture as still in use
	LatteTC_MarkTextureStillInUse(textureView->baseTexture);

	sint32 effectiveWidth, effectiveHeight;
	textureView->baseTexture->GetEffectiveSize(effectiveWidth, effectiveHeight, 0);
	_currentOutputImageWidth = effectiveWidth;
	_currentOutputImageHeight = effectiveHeight;

	sint32 imageX, imageY;
	sint32 imageWidth, imageHeight;
	sint32 fullscreenWidth, fullscreenHeight;

	LatteRenderTarget_getScreenImageArea(&imageX, &imageY, &imageWidth, &imageHeight, &fullscreenWidth, &fullscreenHeight, isPadView);

	bool clearBackground = false;
	if (imageWidth != fullscreenWidth || imageHeight != fullscreenHeight)
		clearBackground = true;

	const bool renderUpsideDown = ActiveSettings::RenderUpsideDownEnabled();
	// force disable bicubic scaling if output resolution is equal/smaller than input resolution
	const bool downscaling = (imageWidth <= effectiveWidth || imageHeight <= effectiveHeight);
	// check for graphic pack shaders
	RendererOutputShader* shader = nullptr;
	LatteTextureView::MagFilter filter = LatteTextureView::MagFilter::kLinear;
	for(const auto& gp : GraphicPack2::GetActiveGraphicPacks())
	{
		if(downscaling)
		{
			shader = gp->GetDownscalingShader(renderUpsideDown);
			if (shader)
			{
				filter = gp->GetDownscalingMagFilter();
				break;
			}
		}
		else
		{
			shader = gp->GetUpscalingShader(renderUpsideDown);
			if (shader)
			{
				filter = gp->GetUpscalingMagFilter();
				break;
			}
		}

		shader = gp->GetOuputShader(renderUpsideDown);
		if (shader)
		{
			filter = downscaling ? gp->GetDownscalingMagFilter() : gp->GetUpscalingMagFilter();
			break;
		}
	}

	if (shader == nullptr)
	{
		sint32 scaling_filter = downscaling ? GetConfig().downscale_filter : GetConfig().upscale_filter;

		if (scaling_filter == kLinearFilter)
		{
			if(renderUpsideDown)
				shader = RendererOutputShader::s_copy_shader_ud;
			else
				shader = RendererOutputShader::s_copy_shader;

			filter = LatteTextureView::MagFilter::kLinear;
		}
		else if (scaling_filter == kBicubicFilter)
		{
			if (renderUpsideDown)
				shader = RendererOutputShader::s_bicubic_shader_ud;
			else
				shader = RendererOutputShader::s_bicubic_shader;

			filter = LatteTextureView::MagFilter::kLinear;
		}
		else if (scaling_filter == kBicubicHermiteFilter)
		{
			if (renderUpsideDown)
				shader = RendererOutputShader::s_hermit_shader_ud;
			else
				shader = RendererOutputShader::s_hermit_shader;

			filter = LatteTextureView::MagFilter::kLinear;
		}
		else if (scaling_filter == kNearestNeighborFilter)
		{
			if (renderUpsideDown)
				shader = RendererOutputShader::s_copy_shader_ud;
			else
				shader = RendererOutputShader::s_copy_shader;

			filter = LatteTextureView::MagFilter::kNearestNeighbor;
		}
	}
	cemu_assert(shader);
	g_renderer->DrawBackbufferQuad(textureView, shader, filter==LatteTextureView::MagFilter::kLinear, imageX, imageY, imageWidth, imageHeight, isPadView, clearBackground);
	g_renderer->HandleScreenshotRequest(textureView, isPadView);
	if (!g_renderer->ImguiBegin(!isPadView))
		return;
	swkbd_render(!isPadView);
	nn::erreula::render(!isPadView);
	LatteOverlay_render(isPadView);
	if (!isPadView)
		LatteRenderTarget_drawDualscreenMenu();
	g_renderer->ImguiEnd();
}

// The windows a screen goes in: Cemu's own GamePad window, and the main
// window, which shows one screen or the other.
static void LatteRenderTarget_showScanBuffer(LatteTextureView* texView, uint32 renderTarget)
{
	auto getVPADScreenActive = [](size_t n) -> std::pair<bool, bool> {
		auto controller = InputManager::instance().get_vpad_controller(n);
		if (!controller)
			return {false,false};
		auto pressed = controller->is_screen_active();
		auto toggle = controller->is_screen_active_toggle();
		return {pressed && !toggle, pressed && toggle};
	};

	const bool tabPressed = WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::TAB);
	const bool ctrlPressed = WindowSystem::IsKeyDown(WindowSystem::PlatformKeyCodes::LCONTROL);
	const auto [vpad0Active, vpad0Toggle] = getVPADScreenActive(0);
	const auto [vpad1Active, vpad1Toggle] = getVPADScreenActive(1);

	const bool altScreenRequested = (!ctrlPressed && tabPressed) || vpad0Active || vpad1Active;
	const bool togglePressed = (ctrlPressed && tabPressed) || vpad0Toggle || vpad1Toggle;
	static bool togglePressedLast = false;

	bool& isDRCPrimary = LatteGPUState.isDRCPrimary;

	if(togglePressed && !togglePressedLast)
		isDRCPrimary = !isDRCPrimary;
	togglePressedLast = togglePressed;

	bool showDRC = swkbd_hasKeyboardInputHook() == false && (isDRCPrimary ^ altScreenRequested);
	const bool screenIsPad = (renderTarget & RENDER_TARGET_DRC) != 0;

	// Cemu's own separate GamePad window, when the player has one open.
	if (screenIsPad && g_renderer->IsPadWindowActive())
		LatteRenderTarget_copyToBackbuffer(texView, true);
	// And its main window, which shows one screen or the other.
	if (screenIsPad == showDRC)
		LatteRenderTarget_copyToBackbuffer(texView, false);
}

// The last copy to each screen, TV and GamePad, for showing again while
// Dualscreen has the game paused -- see LatteRenderTarget_dualscreenWhilePaused().
// Kept as where the picture is rather than as a texture view, which the
// texture cache is free to delete between frames.
namespace
{
	struct ScanBufferCopy
	{
		bool valid = false;
		MPTR colorBufferPtr = MPTR_NULL;
		uint32 width = 0;
		uint32 height = 0;
		uint32 format = 0;
		uint32 pitch = 0;
		Latte::E_HWTILEMODE tilemode{};
		uint32 swizzle = 0;
		uint32 renderTarget = 0;
	};
	ScanBufferCopy s_lastScanBufferCopy[2];

	LatteTextureView* LookUpScanBufferCopy(const ScanBufferCopy& copy)
	{
		return LatteTC_GetTextureSliceViewOrTryCreate(copy.colorBufferPtr, MPTR_NULL, (Latte::E_GX2SURFFMT)copy.format, copy.tilemode, copy.width, copy.height, 1, copy.pitch, copy.swizzle, 0, 0, true);
	}
}

void LatteRenderTarget_itHLECopyColorBufferToScanBuffer(MPTR colorBufferPtr, uint32 colorBufferWidth, uint32 colorBufferHeight, uint32 colorBufferSliceIndex, uint32 colorBufferFormat, uint32 colorBufferPitch, Latte::E_HWTILEMODE colorBufferTilemode, uint32 colorBufferSwizzle, uint32 renderTarget)
{
	cemu_assert_debug(colorBufferSliceIndex == 0); // todo - support for non-zero slice
	ScanBufferCopy copy;
	copy.valid = true;
	copy.colorBufferPtr = colorBufferPtr;
	copy.width = colorBufferWidth;
	copy.height = colorBufferHeight;
	copy.format = colorBufferFormat;
	copy.pitch = colorBufferPitch;
	copy.tilemode = colorBufferTilemode;
	copy.swizzle = colorBufferSwizzle;
	copy.renderTarget = renderTarget;
	LatteTextureView* texView = LookUpScanBufferCopy(copy);
	if (!texView)
	{
		return;
	}
	if (renderTarget & RENDER_TARGET_TV)
		s_lastScanBufferCopy[0] = copy;
	if (renderTarget & RENDER_TARGET_DRC)
		s_lastScanBufferCopy[1] = copy;

	// Which of the console's two screens this texture holds. Every question
	// below is about one of two different things -- which screen this is, and
	// which window it goes in -- and conflating them is what put the GamePad
	// on a shell asking for the television.
	const bool screenIsPad = (renderTarget & RENDER_TARGET_DRC) != 0;

	// A shell watching a screen is watching it whether or not this machine is
	// also showing it, so this comes first and stands on its own. It used to
	// ride along inside the backbuffer copies below, which was wrong in both
	// directions at once whenever Cemu showed the GamePad in its main window
	// -- which is exactly what DualscreenSession::Poll() arranges when a
	// shell asks for the television. That copy is made with isPadView false,
	// meaning "the main window", and the tap read it as "the television": the
	// GamePad's picture went out labelled as the television's, while the
	// television, drawn into no window at all, was never tapped and never
	// went anywhere.
	LatteRenderTarget_tapDualscreen(texView, screenIsPad);
	LatteRenderTarget_showScanBuffer(texView, renderTarget);
}

// A paused game flips no frames, and this window -- with Dualscreen's menu
// drawn in it -- is only redrawn when one is flipped. So while Dualscreen has
// the game paused, the GPU thread's idle loop shows the last frame again at
// about the display's rate, and keeps reading the link, since nothing else
// will until the game goes on. True while the game is paused.
bool LatteRenderTarget_dualscreenWhilePaused()
{
	DualscreenSession& session = DualscreenSession::instance();
	if (!session.GamePaused())
		return false;
	static std::chrono::steady_clock::time_point s_lastShown;
	const auto now = std::chrono::steady_clock::now();
	if (now - s_lastShown < std::chrono::microseconds(16667))
		return true;
	s_lastShown = now;
	session.Poll();
	LatteRenderTarget_handleDualscreenPause();
	const bool stillWanted = session.TakeStillWanted();
	bool shown = false;
	for (const ScanBufferCopy& copy : s_lastScanBufferCopy)
	{
		if (!copy.valid)
			continue;
		LatteTextureView* texView = LookUpScanBufferCopy(copy);
		if (!texView)
			continue;
		if (stillWanted && (copy.renderTarget & RENDER_TARGET_TV))
		{
			// The lines a capture always starts with -- see
			// LatteRenderTarget_tapDualscreen().
			LatteTexture_UpdateDataToLatest(texView->baseTexture);
			LatteTC_MarkTextureStillInUse(texView->baseTexture);
			g_renderer->HandleDualscreenFrame(texView, false);
		}
		LatteRenderTarget_showScanBuffer(texView, copy.renderTarget);
		shown = true;
		// One copy to both screens is shown once.
		if ((copy.renderTarget & (RENDER_TARGET_TV | RENDER_TARGET_DRC)) == (RENDER_TARGET_TV | RENDER_TARGET_DRC))
			break;
	}
	if (shown)
		g_renderer->SwapBuffers(true, true);
	return true;
}



// returns the current size of the virtual viewport (not the same as effective size, which can be influenced by texture rules)
void LatteRenderTarget_GetCurrentVirtualViewportSize(sint32* viewportWidth, sint32* viewportHeight)
{
	*viewportWidth = sLatteRenderTargetState.currentGuestViewport.width;
	*viewportHeight = sLatteRenderTargetState.currentGuestViewport.height;
}

void LatteRenderTarget_updateViewport()
{
	float vpWidth = LatteGPUState.contextNew.PA_CL_VPORT_XSCALE.get_SCALE() / 0.5f;
	float vpX = LatteGPUState.contextNew.PA_CL_VPORT_XOFFSET.get_OFFSET() - LatteGPUState.contextNew.PA_CL_VPORT_XSCALE.get_SCALE();
	float vpHeight = LatteGPUState.contextNew.PA_CL_VPORT_YSCALE.get_SCALE() / -0.5f;
	float vpY = LatteGPUState.contextNew.PA_CL_VPORT_YOFFSET.get_OFFSET() + LatteGPUState.contextNew.PA_CL_VPORT_YSCALE.get_SCALE();

	bool halfZ = LatteGPUState.contextNew.PA_CL_CLIP_CNTL.get_DX_CLIP_SPACE_DEF();

	// calculate near/far
	float farZ;
	float nearZ;
	float s = LatteGPUState.contextNew.PA_CL_VPORT_ZSCALE.get_SCALE();
	float b = LatteGPUState.contextNew.PA_CL_VPORT_ZOFFSET.get_OFFSET();
	if (halfZ == false)
	{
		farZ = s + b;
		nearZ = b - s;
	}
	else
	{
		farZ = s + b;
		nearZ = b;
	}

	sLatteRenderTargetState.currentGuestViewport.width = vpWidth;
	sLatteRenderTargetState.currentGuestViewport.height = vpHeight;
	if (sLatteRenderTargetState.renderTargetIsResized)
	{
		vpX *= ((float)sLatteRenderTargetState.currentEffectiveSize.width / (float)sLatteRenderTargetState.currentRenderSize.width);
		vpY *= ((float)sLatteRenderTargetState.currentEffectiveSize.height / (float)sLatteRenderTargetState.currentRenderSize.height);
		vpWidth *= ((float)sLatteRenderTargetState.currentEffectiveSize.width / (float)sLatteRenderTargetState.currentRenderSize.width);
		vpHeight *= ((float)sLatteRenderTargetState.currentEffectiveSize.height / (float)sLatteRenderTargetState.currentRenderSize.height);
	}
	g_renderer->renderTarget_setViewport(vpX, vpY, vpWidth, vpHeight, nearZ, farZ, halfZ);
}

void LatteRenderTarget_updateScissorBox()
{
	// update scissor box
	uint32 scissorX = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_TL_X();
	uint32 scissorY = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_TL.get_TL_Y();
	uint32 scissorWidth = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_X() - scissorX;
	uint32 scissorHeight = LatteGPUState.contextNew.PA_SC_GENERIC_SCISSOR_BR.get_BR_Y() - scissorY;
	if( sLatteRenderTargetState.renderTargetIsResized )
	{
		scissorX = (sint32)((float)scissorX * ((float)sLatteRenderTargetState.currentEffectiveSize.width / (float)sLatteRenderTargetState.currentRenderSize.width));
		scissorY = (sint32)((float)scissorY * ((float)sLatteRenderTargetState.currentEffectiveSize.height / (float)sLatteRenderTargetState.currentRenderSize.height));
		scissorWidth = (sint32)((float)scissorWidth * ((float)sLatteRenderTargetState.currentEffectiveSize.width / (float)sLatteRenderTargetState.currentRenderSize.width));
		scissorHeight = (sint32)((float)scissorHeight * ((float)sLatteRenderTargetState.currentEffectiveSize.height / (float)sLatteRenderTargetState.currentRenderSize.height));
	}

	if( scissorX != prevScissorX || scissorY != prevScissorY || scissorWidth != prevScissorWidth || scissorHeight != prevScissorHeight )
	{
		g_renderer->renderTarget_setScissor(scissorX, scissorY, scissorWidth, scissorHeight);
		prevScissorX = scissorX;
		prevScissorY = scissorY;
		prevScissorWidth = scissorWidth;
		prevScissorHeight = scissorHeight;
	}
}

void LatteRenderTarget_unloadAll()
{
	if (g_emptyFBO)
	{
		LatteMRT::DeleteCachedFBO(g_emptyFBO);
		g_emptyFBO = nullptr;
	}
}
