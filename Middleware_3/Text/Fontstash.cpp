/*
 * Copyright (c) 2018 Confetti Interactive Inc.
 * 
 * This file is part of The-Forge
 * (see https://github.com/ConfettiFX/The-Forge).
 * 
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
*/

#include "Fontstash.h"

// include Fontstash (should be after MemoryTracking so that it also detects memory free/remove in fontstash)
#define FONTSTASH_IMPLEMENTATION
#include "../../Common_3/ThirdParty/OpenSource/Fontstash/src/fontstash.h"

#include "../../Common_3/OS/Interfaces/ILogManager.h"
#include "../../Common_3/OS/Interfaces/IFileSystem.h"
#include "../../Common_3/OS/Image/Image.h"
#include "../../Common_3/OS/Core/RingBuffer.h"

#include "../../Common_3/Renderer/IRenderer.h"
#include "../../Common_3/Renderer/ResourceLoader.h"
#include "../Text/TextShaders.h"

#include "../../Common_3/ThirdParty/OpenSource/TinySTL/vector.h"

#include "../../Common_3/OS/Interfaces/IMemoryManager.h"

class _Impl_FontStash
{
public:
	_Impl_FontStash() 
	{
		pCurrentTexture = NULL; 
		mWidth = 0;
		mHeight = 0; 
		pContext = NULL;
		
		mText3D  = false;
	}

	void init(Renderer* renderer, int width_, int height_)
	{
		pRenderer = renderer;

		// create image
		mStagingImage.Create(ImageFormat::R8, width_, height_, 1, 1, 1);

		// create FONS context
		FONSparams params;
		memset(&params, 0, sizeof(params));
		params.width = width_;
		params.height = height_;
		params.flags = (unsigned char)FONS_ZERO_TOPLEFT;
		params.renderCreate = fonsImplementationGenerateTexture;
		params.renderResize = fonsImplementationResizeTexture;
		params.renderUpdate = fonsImplementationModifyTexture;
		params.renderDraw = fonsImplementationRenderText; 
		params.renderDelete = fonsImplementationRemoveTexture;
		params.userPtr = this;

		pContext = fonsCreateInternal(&params);
		/************************************************************************/
		// Rendering resources
		/************************************************************************/
		SamplerDesc samplerDesc =
		{
			FILTER_LINEAR, FILTER_LINEAR, MIPMAP_MODE_NEAREST,
			ADDRESS_MODE_CLAMP_TO_EDGE, ADDRESS_MODE_CLAMP_TO_EDGE, ADDRESS_MODE_CLAMP_TO_EDGE
		};
		addSampler(pRenderer, &samplerDesc, &pDefaultSampler);

		BlendStateDesc blendStateDesc = {};
		blendStateDesc.mSrcFactor = BC_SRC_ALPHA;
		blendStateDesc.mDstFactor = BC_ONE_MINUS_SRC_ALPHA;
		blendStateDesc.mSrcAlphaFactor = BC_SRC_ALPHA;
		blendStateDesc.mDstAlphaFactor = BC_ONE_MINUS_SRC_ALPHA;
		blendStateDesc.mMask = ALL;
		blendStateDesc.mRenderTargetMask = BLEND_STATE_TARGET_ALL;
		addBlendState(pRenderer, &blendStateDesc, &pBlendAlpha);

		DepthStateDesc depthStateDesc = {};
		depthStateDesc.mDepthTest = false;
		depthStateDesc.mDepthWrite = false;
		addDepthState(pRenderer, &depthStateDesc, &pDepthStates[0]);

		DepthStateDesc depthStateEnableDesc = {};
		depthStateEnableDesc.mDepthTest = true;
		depthStateEnableDesc.mDepthWrite = true;
		depthStateEnableDesc.mDepthFunc = CMP_LEQUAL;
		addDepthState(pRenderer, &depthStateEnableDesc, &pDepthStates[1]);

		RasterizerStateDesc rasterizerStateDesc = {};
		rasterizerStateDesc.mCullMode = CULL_MODE_NONE;
		rasterizerStateDesc.mScissor = true;
		addRasterizerState(pRenderer, &rasterizerStateDesc, &pRasterizerStates[0]);

		RasterizerStateDesc rasterizerStateFrontDesc = {};
		rasterizerStateFrontDesc.mCullMode = CULL_MODE_BACK;
		rasterizerStateFrontDesc.mScissor = true;
		addRasterizerState(pRenderer, &rasterizerStateFrontDesc, &pRasterizerStates[1]);

#if defined(METAL)
		String textShaderFile = "builtin_text";
		String textShader = mtl_builtin_text;
		ShaderDesc text2DShaderDesc = { SHADER_STAGE_VERT | SHADER_STAGE_FRAG, { textShaderFile, textShader, "VSMain" }, { textShaderFile, textShader, "PSMain" } };
		ShaderDesc text3DShaderDesc = { SHADER_STAGE_VERT | SHADER_STAGE_FRAG, { textShaderFile, textShader, "VSMain3D" }, { textShaderFile, textShader, "PSMain" } };
		addShader(pRenderer, &text2DShaderDesc, &pShaders[0]);
		addShader(pRenderer, &text3DShaderDesc, &pShaders[1]);
#elif defined(DIRECT3D12) || defined(VULKAN)
		char* text2DVert = NULL; uint32_t text2DVertSize = 0;
		char* text3DVert = NULL; uint32_t text3DVertSize = 0;
		char* text2DFrag = NULL; uint32_t text2DFragSize = 0;
		char* text3DFrag = NULL; uint32_t text3DFragSize = 0;

		if (pRenderer->mSettings.mApi == RENDERER_API_D3D12 || pRenderer->mSettings.mApi == RENDERER_API_XBOX_D3D12)
		{
			text2DVert = (char*)d3d12_builtin_text2D_vert; text2DVertSize = sizeof(d3d12_builtin_text2D_vert);
			text3DVert = (char*)d3d12_builtin_text3D_vert; text3DVertSize = sizeof(d3d12_builtin_text3D_vert);
			text2DFrag = (char*)d3d12_builtin_text_frag; text2DFragSize = sizeof(d3d12_builtin_text_frag);
			text3DFrag = (char*)d3d12_builtin_text_frag; text3DFragSize = sizeof(d3d12_builtin_text_frag);
		}
		else if (pRenderer->mSettings.mApi == RENDERER_API_VULKAN)
		{
			text2DVert = (char*)vk_builtin_text2D_vert; text2DVertSize = sizeof(vk_builtin_text2D_vert);
			text3DVert = (char*)vk_builtin_text3D_vert; text3DVertSize = sizeof(vk_builtin_text3D_vert);
			text2DFrag = (char*)vk_builtin_text_frag; text2DFragSize = sizeof(vk_builtin_text_frag);
			text3DFrag = (char*)vk_builtin_text_frag; text3DFragSize = sizeof(vk_builtin_text_frag);
		}

		BinaryShaderDesc textShaderDesc = { SHADER_STAGE_VERT | SHADER_STAGE_FRAG,
		{ (char*)text2DVert, text2DVertSize },{ (char*)text2DFrag, text2DFragSize } };
		addShaderBinary(pRenderer, &textShaderDesc, &pShaders[0]);

		textShaderDesc = { SHADER_STAGE_VERT | SHADER_STAGE_FRAG,
		{ (char*)text3DVert, text3DVertSize },{ (char*)text3DFrag, text3DFragSize } };
		addShaderBinary(pRenderer, &textShaderDesc, &pShaders[1]);
#endif

		RootSignatureDesc textureRootDesc = { pShaders, 2 };
#if defined(VULKAN)
		const char* pDynamicUniformBuffers[] = { "uniformBlock" };
		textureRootDesc.mDynamicUniformBufferCount = 1;
		textureRootDesc.ppDynamicUniformBufferNames = pDynamicUniformBuffers;
#endif
		const char* pStaticSamplers[] = { "uSampler0" };
		textureRootDesc.mStaticSamplerCount = 1;
		textureRootDesc.ppStaticSamplerNames = pStaticSamplers;
		textureRootDesc.ppStaticSamplers = &pDefaultSampler;
		addRootSignature(pRenderer, &textureRootDesc, &pRootSignature);

		addUniformRingBuffer(pRenderer, 1024 * 256, &pUniformRingBuffer);

		BufferDesc vbDesc = {};
		vbDesc.mUsage = BUFFER_USAGE_VERTEX;
		vbDesc.mMemoryUsage = RESOURCE_MEMORY_USAGE_CPU_TO_GPU;
		vbDesc.mSize = 1024 * 1024 * sizeof(float4);
		vbDesc.mVertexStride = sizeof(float4);
		vbDesc.mFlags = BUFFER_CREATION_FLAG_PERSISTENT_MAP_BIT;
		addMeshRingBuffer(pRenderer, &vbDesc, NULL, &pMeshRingBuffer);

		mVertexLayout.mAttribCount = 2;
		mVertexLayout.mAttribs[0].mSemantic = SEMANTIC_POSITION;
		mVertexLayout.mAttribs[0].mFormat = ImageFormat::RG32F;
		mVertexLayout.mAttribs[0].mBinding = 0;
		mVertexLayout.mAttribs[0].mLocation = 0;
		mVertexLayout.mAttribs[0].mOffset = 0;

		mVertexLayout.mAttribs[1].mSemantic = SEMANTIC_TEXCOORD0;
		mVertexLayout.mAttribs[1].mFormat = ImageFormat::RG32F;
		mVertexLayout.mAttribs[1].mBinding = 0;
		mVertexLayout.mAttribs[1].mLocation = 1;
		mVertexLayout.mAttribs[1].mOffset = calculateImageFormatStride(ImageFormat::RG32F);

		mPipelineDesc = {};
		mPipelineDesc.mPrimitiveTopo = PRIMITIVE_TOPO_TRI_LIST;
		mPipelineDesc.mRenderTargetCount = 1;
		mPipelineDesc.mSampleCount = SAMPLE_COUNT_1;
		mPipelineDesc.pBlendState = pBlendAlpha;
		mPipelineDesc.pRootSignature = pRootSignature;
		mPipelineDesc.pVertexLayout = &mVertexLayout;
		/************************************************************************/
		/************************************************************************/
	}

	void destroy()
	{
		// unload fontstash context
		fonsDeleteInternal(pContext);

		mStagingImage.Destroy();

		// unload font buffers
		for (unsigned int i = 0; i < (uint32_t)mFontBuffers.size(); i++)
			conf_free(mFontBuffers[i]);

		for (uint32_t i = 0; i < (uint32_t)mTextureList.size(); ++i)
			removeResource(mTextureList[i]);

		removeRootSignature(pRenderer, pRootSignature);

		for (uint32_t i = 0; i < 2; ++i)
		{
			removeShader(pRenderer, pShaders[i]);
			for (PipelineMap::iterator it = mPipelines[i].begin(); it != mPipelines[i].end(); ++it)
				removePipeline(pRenderer, it.node->second);

			mPipelines[i].clear();
		}

		removeMeshRingBuffer(pMeshRingBuffer);
		removeUniformRingBuffer(pUniformRingBuffer);
		for (uint32_t i = 0; i < 2; ++i)
		{
			removeDepthState(pDepthStates[i]);
			removeRasterizerState(pRasterizerStates[i]);
		}
		removeBlendState(pBlendAlpha);
		removeSampler(pRenderer, pDefaultSampler);

		mTextureList.clear();
	}

	static int fonsImplementationGenerateTexture(void* userPtr, int width, int height);
	static int fonsImplementationResizeTexture(void* userPtr, int width, int height);
	static void fonsImplementationModifyTexture(void* userPtr, int* rect, const unsigned char* data);
	static void fonsImplementationRenderText(void* userPtr, const float* verts, const float* tcoords, const unsigned int* colors, int nverts);
	static void fonsImplementationRemoveTexture(void* userPtr);

	using PipelineMap = tinystl::unordered_map<uint64_t, Pipeline*>;

	Renderer*					pRenderer;
	FONScontext*				pContext;

	Image						mStagingImage;
	Texture*					pCurrentTexture;

	uint32_t					mWidth;
	uint32_t					mHeight;

	tinystl::vector<void*>		mFontBuffers;
	tinystl::vector<Texture*>	mTextureList;

	mat4						mProjView;
	mat4						mWorldMat;
	Cmd*						pCmd;

	Shader*						pShaders[2];
	RootSignature*				pRootSignature;
	PipelineMap					mPipelines[2];
	/// Default states
	BlendState*					pBlendAlpha;
	DepthState*					pDepthStates[2];
	RasterizerState*			pRasterizerStates[2];
	Sampler*					pDefaultSampler;
	UniformRingBuffer*			pUniformRingBuffer;
	MeshRingBuffer*				pMeshRingBuffer;
	VertexLayout				mVertexLayout = {};
	GraphicsPipelineDesc		mPipelineDesc = {};
	bool						mText3D;
};


Fontstash::Fontstash(Renderer* renderer, int width, int height)
{
	impl = conf_placement_new<_Impl_FontStash>(conf_calloc(1, sizeof(_Impl_FontStash)));
	impl->init(renderer, width, height);
	m_fFontMaxSize = min(width, height) / 10.0f; // see fontstash.h, line 1271, for fontSize calculation
}

void Fontstash::destroy()
{
	impl->destroy();
	impl->~_Impl_FontStash();
	conf_free(impl);
}

int Fontstash::defineFont(const char* identification, const char* filename, uint32_t root)
{
	FONScontext* fs = impl->pContext;

	File file = File();
	file.Open(filename, FileMode::FM_ReadBinary, (FSRoot)root);
	unsigned bytes = file.GetSize();
	void* buffer = conf_malloc(bytes);
	file.Read(buffer, bytes);
	file.Close();

	// add buffer to font buffers for cleanup
	impl->mFontBuffers.emplace_back(buffer);

	return fonsAddFontMem(fs, identification, (unsigned char*)buffer, (int)bytes, 0);
}

int Fontstash::getFontID(const char* identification)
{
	FONScontext* fs=impl->pContext;
	return fonsGetFontByName(fs, identification);
}

void Fontstash::drawText(Cmd* pCmd, const char* message, float x, float y, int fontID, unsigned int color/*=0xffffffff*/, float size/*=16.0f*/, float spacing/*=3.0f*/, float blur/*=0.0f*/)
{
	impl->mText3D = false;
	impl->pCmd = pCmd;
	// clamp the font size to max size. 
	// Precomputed font texture puts limitation to the maximum size.
	size = min(size, m_fFontMaxSize);

	FONScontext* fs=impl->pContext;
	fonsSetSize(fs, size);
	fonsSetFont(fs, fontID);
	fonsSetColor(fs, color);
	fonsSetSpacing(fs, spacing);
	fonsSetBlur(fs, blur);
	fonsSetAlign(fs, FONS_ALIGN_LEFT | FONS_ALIGN_TOP);
	fonsDrawText(fs, x,y, message,NULL);
}


void Fontstash::drawText(Cmd* pCmd, const char* message,const mat4& projView,const mat4& worldMat,int fontID, unsigned int color/*=0xffffffff*/, float size/*=16.0f*/, float spacing/*=3.0f*/, float blur/*=0.0f*/)
{
	impl->mText3D = true;
	impl->mProjView = projView;
	impl->mWorldMat = worldMat;
	impl->pCmd = pCmd;
	// clamp the font size to max size. 
	// Precomputed font texture puts limitation to the maximum size.
	size = min(size, m_fFontMaxSize);

	FONScontext* fs=impl->pContext;
	fonsSetSize(fs, size);
	fonsSetFont(fs, fontID);
	fonsSetColor(fs, color);
	fonsSetSpacing(fs, spacing);
	fonsSetBlur(fs, blur);
	fonsSetAlign(fs, FONS_ALIGN_CENTER | FONS_ALIGN_MIDDLE);
	fonsDrawText(fs, 0.0f,0.0f, message,NULL);
}

float Fontstash::measureText(float* out_bounds, const char* message, float x, float y, int fontID, unsigned int color/*=0xffffffff*/, float size/*=16.0f*/, float spacing/*=3.0f*/, float blur/*=0.0f*/)
{
	return measureText(out_bounds, message, (int)strlen(message), x, y, fontID, color, size, spacing, blur);
}

float Fontstash::measureText(float* out_bounds, const char* message, int messageLength, float x, float y, int fontID, unsigned int color/*=0xffffffff*/, float size/*=16.0f*/, float spacing/*=0.0f*/, float blur/*=0.0f*/)
{
	if(out_bounds == NULL)
		return 0;

	FONScontext* fs=impl->pContext;
	fonsSetSize(fs, size);
	fonsSetFont(fs, fontID);
	fonsSetColor(fs, color);
	fonsSetSpacing(fs, spacing);
	fonsSetBlur(fs, blur);
	fonsSetAlign(fs, FONS_ALIGN_LEFT | FONS_ALIGN_TOP);
	return fonsTextBounds(fs, x, y, message, message+messageLength, out_bounds);
}

// --  FONS renderer implementation --
int _Impl_FontStash::fonsImplementationGenerateTexture(void* userPtr, int width, int height)
{
	_Impl_FontStash* ctx = (_Impl_FontStash*)userPtr;
	ctx->mWidth = width;
	ctx->mHeight = height;

	Texture* oldTex = ctx->pCurrentTexture;

	TextureLoadDesc loadDesc = {};
	loadDesc.ppTexture = &ctx->pCurrentTexture;
	loadDesc.pImage = &ctx->mStagingImage;
	// R8 mode
	//addTexture2d(ctx->renderer, width, height, SampleCount::SAMPLE_COUNT_1, ctx->img.getFormat(), ctx->img.GetMipMapCount(), NULL, false, TextureUsage::TEXTURE_USAGE_SAMPLED_IMAGE, &ctx->tex);
	addResource(&loadDesc);
	
	// Create may be called multiple times, delete existing texture.
	// NOTE: deleting the texture afterwards seems to fix a driver bug on Intel where it would reuse the old contents of ctx->img, causing the texture not to update.
	if (oldTex)
	{
		ctx->mTextureList.emplace_back(oldTex);
	}

	return 1;
}

int _Impl_FontStash::fonsImplementationResizeTexture(void* userPtr, int width, int height)
{
	// Reuse create to resize too.
	return fonsImplementationGenerateTexture(userPtr, width, height);
}

void _Impl_FontStash::fonsImplementationModifyTexture(void* userPtr, int* rect, const unsigned char* data)
{
	_Impl_FontStash* ctx = (_Impl_FontStash*)userPtr;

	// TODO: Update the GPU texture instead of changing the CPU texture and rebuilding it on GPU.
	ctx->mStagingImage.loadFromMemoryXY(data + rect[0] + rect[1]*ctx->mWidth, rect[0], rect[1], rect[2], rect[3], ctx->mWidth);

	fonsImplementationGenerateTexture(userPtr, ctx->mWidth, ctx->mHeight);		// rebuild texture
}

void _Impl_FontStash::fonsImplementationRenderText(void* userPtr, const float* verts, const float* tcoords, const unsigned int* colors, int nverts)
{
	_Impl_FontStash* ctx = (_Impl_FontStash*)userPtr;
	if (ctx->pCurrentTexture == NULL) return;

	Cmd* pCmd = ctx->pCmd;

	RingBufferOffset buffer = getVertexBufferOffset(ctx->pMeshRingBuffer, nverts * sizeof(float4));

	//make it static so that we don't need to create texVertex Arr every time
	float4* vtx = (float4*)((uint8_t*)buffer.pBuffer->pCpuMappedAddress + buffer.mOffset);

	// build vertices
	for(int impl=0; impl<nverts; impl++)
	{
		vtx[impl].setX(verts[impl*2+0]);
		vtx[impl].setY(verts[impl*2+1]);
		vtx[impl].setZ(tcoords[impl*2+0]);
		vtx[impl].setW(tcoords[impl*2+1]);
	}

	// extract color
	ubyte* colorByte = (ubyte*)colors;
	float4 color;
	for(int i=0; i<4; i++)
		color[i] = ((float)colorByte[i])/255.0f;

	Pipeline* pPipeline = NULL;
	_Impl_FontStash::PipelineMap::iterator it = ctx->mPipelines[ctx->mText3D].find(pCmd->mRenderPassHash);
	if (it == ctx->mPipelines[ctx->mText3D].end())
	{
		GraphicsPipelineDesc pipelineDesc = ctx->mPipelineDesc;
		pipelineDesc.mDepthStencilFormat = (ImageFormat::Enum)pCmd->mBoundDepthStencilFormat;
		pipelineDesc.mRenderTargetCount = pCmd->mBoundRenderTargetCount;
		pipelineDesc.mSampleCount = pCmd->mBoundSampleCount;
		pipelineDesc.mSampleQuality = pCmd->mBoundSampleQuality;
		pipelineDesc.pColorFormats = (ImageFormat::Enum*)pCmd->pBoundColorFormats;
		pipelineDesc.pDepthState = ctx->pDepthStates[ctx->mText3D];
		pipelineDesc.pRasterizerState = ctx->pRasterizerStates[ctx->mText3D];
		pipelineDesc.pSrgbValues = pCmd->pBoundSrgbValues;
		pipelineDesc.pShaderProgram = ctx->pShaders[ctx->mText3D];
		addPipeline(pCmd->pRenderer, &pipelineDesc, &pPipeline);
		ctx->mPipelines[ctx->mText3D].insert({ pCmd->mRenderPassHash, pPipeline });
	}
	else
	{
		pPipeline = it.node->second;
	}

	cmdBindPipeline(pCmd, pPipeline);

	struct UniformData
	{
		float4 color;
		float2 scaleBias;
	} data;

	data.color = color;
	data.scaleBias = { 2.0f / (float)pCmd->mBoundWidth, -2.0f / (float)pCmd->mBoundHeight };

	if(ctx->mText3D)
	{
		mat4 mvp = ctx->mProjView * ctx->mWorldMat;
		data.color = color;
		data.scaleBias.x = -data.scaleBias.x;

		RingBufferOffset uniformBlock = {};
		uniformBlock = getUniformBufferOffset(ctx->pUniformRingBuffer, sizeof(mvp));
		BufferUpdateDesc updateDesc = { uniformBlock.pBuffer, &mvp, 0, uniformBlock.mOffset, sizeof(mvp) };
		updateResource(&updateDesc);

		DescriptorData params[3] = {};
		params[0].pName = "uRootConstants";
		params[0].pRootConstant = &data;
		params[1].pName = "uniformBlock";
		params[1].ppBuffers = &uniformBlock.pBuffer;
		params[1].pOffsets = &uniformBlock.mOffset;
		params[2].pName = "uTex0";
		params[2].ppTextures = &ctx->pCurrentTexture;
		cmdBindDescriptors(pCmd, ctx->pRootSignature, 3, params);
		cmdBindVertexBuffer(pCmd, 1, &buffer.pBuffer, &buffer.mOffset);
		cmdDraw(pCmd, nverts, 0);
	}
	else
	{
		DescriptorData params[2] = {};
		params[0].pName = "uRootConstants";
		params[0].pRootConstant = &data;
		params[1].pName = "uTex0";
		params[1].ppTextures = &ctx->pCurrentTexture;
		cmdBindDescriptors(pCmd, ctx->pRootSignature, 2, params);
		cmdBindVertexBuffer(pCmd, 1, &buffer.pBuffer, &buffer.mOffset);
		cmdDraw(pCmd, nverts, 0);
	}
}

void _Impl_FontStash::fonsImplementationRemoveTexture(void* userPtr)
{
	_Impl_FontStash* ctx = (_Impl_FontStash*)userPtr;
	if (ctx->pCurrentTexture)
	{
		ctx->mTextureList.emplace_back(ctx->pCurrentTexture);
		ctx->pCurrentTexture = NULL;
	}
}
