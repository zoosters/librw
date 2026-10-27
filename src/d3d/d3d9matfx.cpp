#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define WITH_D3D
#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwengine.h"
#include "../rwrender.h"
#include "../rwplugins.h"
#include "rwd3d.h"
#include "rwd3d9.h"

namespace rw {
namespace d3d9 {
using namespace d3d;

#ifndef RW_D3D9
void matfxRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header) {}
#else

static void *matfx_env_amb_VS;
static void *matfx_env_amb_dir_VS;
static void *matfx_env_all_VS;
static void *matfx_env_PS;
static void *matfx_env_tex_PS;
static void *matfx_envpass_PS;	

enum
{
	VSLOC_texMat = VSLOC_afterLights,
	VSLOC_colorClamp = VSLOC_texMat + 4,
	VSLOC_envColor,

	PSLOC_shininess = 1,
};

void
matfxRender_Default(InstanceDataHeader *header, InstanceData *inst, int32 lightBits)
{
	Material *m = inst->material;

	// Pick a shader
	if((lightBits & VSLIGHT_MASK) == 0)
		setVertexShader(default_amb_VS);
	else if((lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(default_amb_dir_VS);
	else
		setVertexShader(default_all_VS);

	SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 255);

	if(inst->material->texture){
		d3d::setTexture(0, m->texture);
		setPixelShader(default_tex_PS);
	}else{
		d3d::unbindTexture0();	
		setPixelShader(default_PS);
	}

	drawInst(header, inst);
}

static Frame *lastEnvFrame;

static RawMatrix normal2texcoord = {
	{ 0.5f,  0.0f, 0.0f }, 0.0f,
	{ 0.0f, -0.5f, 0.0f }, 0.0f,
	{ 0.0f,  0.0f, 1.0f }, 0.0f,
	{ 0.5f,  0.5f, 0.0f }, 1.0f
};

void
uploadEnvMatrix(Frame *frame)
{
	Matrix invMat;
	if(frame == nil)
		frame = engine->currentCamera->getFrame();

	// cache the matrix across multiple meshes
// can't do it, frame matrix may change
//	if(frame == lastEnvFrame)
//		return;
//	lastEnvFrame = frame;

	RawMatrix envMtx, invMtx;
	Matrix::invert(&invMat, frame->getLTM());
	convMatrix(&invMtx, &invMat);
	invMtx.pos.set(0.0f, 0.0f, 0.0f);
	float uscale = fabs(normal2texcoord.right.x);
	normal2texcoord.right.x = MatFX::envMapFlipU ? -uscale : uscale;
	RawMatrix::mult(&envMtx, &invMtx, &normal2texcoord);
	d3ddevice->SetVertexShaderConstantF(VSLOC_texMat, (float*)&envMtx, 4);
}

// Env transform of RW 3.6's D3D8 MatFX: the device generates camera space normals
// (D3DTSS_TCI_CAMERASPACENORMAL) and transforms them by [D * inv(envFrame LTM) * camera LTM] (frame given, D = negate the x row) * normal2texcoord, or by
// normal2texcoord alone when the env map has no frame. The shader gets world normals, so the world -> camera space normal rotation (the D3D view matrix
// mirrors x) is prepended.
static RawMatrix normal2texcoordRW = {
	{ 0.5f,  0.0f, 0.0f }, 0.0f,
	{ 0.0f, -0.5f, 0.0f }, 0.0f,
	{ 0.0f,  0.0f, 1.0f }, 0.0f,
	{ 0.5f,  0.5f, 0.0f }, 1.0f
};

static void
uploadEnvMatrixRW(Frame *frame)
{
	Camera *cam = engine->currentCamera;
	Matrix *camLTM = cam->getFrame()->getLTM();
	Matrix viewRot;
	Matrix::invert(&viewRot, camLTM);
	viewRot.right.x = -viewRot.right.x;
	viewRot.up.x = -viewRot.up.x;
	viewRot.at.x = -viewRot.at.x;
	viewRot.pos.set(0.0f, 0.0f, 0.0f);
	Matrix A;
	A.setIdentity();
	if(frame){
		Matrix invF;
		Matrix::invert(&invF, frame->getLTM());
		Matrix::mult(&A, &invF, camLTM);
		A.right.x = -A.right.x;
		A.right.y = -A.right.y;
		A.right.z = -A.right.z;
		A.pos.set(0.0f, 0.0f, 0.0f);
	}
	RawMatrix rv, ra, tmp, tot;
	convMatrix(&rv, &viewRot);
	convMatrix(&ra, &A);
	RawMatrix::mult(&tmp, &rv, &ra);
	RawMatrix::mult(&tot, &tmp, &normal2texcoordRW);
	d3ddevice->SetVertexShaderConstantF(VSLOC_texMat, (float*)&tot, 4);
}

// Env map in two passes like RW 3.6's D3D8 MatFX (the generic path):
// pass 1 = the base mesh (default render), pass 2 = the env texture, z write off, SRCALPHA / ONE blend, lighting off,
// colour = env texture * coefficient byte (trunc(coefficient * 255) / 255), alpha = env texture alpha, fog colour black. Skipped when the byte is 0.
// RW's single-pass shortcuts for opaque, alpha-less textures and its pixel shader variant are not ported.
void
matfxRender_EnvMap(InstanceDataHeader *header, InstanceData *inst, int32 lightBits, MatFX::Env *env)
{
	Material *m = inst->material;

	int32 cbyte = env->tex ? (int32)(env->coefficient * 255.0f) : 0;
	if(env->tex == nil || cbyte <= 0){
		matfxRender_Default(header, inst, lightBits);
		return;
	}

	{	// own scope: librw's one-pass version below uses the same names
		// pass 1: base
		matfxRender_Default(header, inst, lightBits);

		// pass 2: environment map
		uint32 oldSrc = GetRenderState(SRCBLEND), oldDst = GetRenderState(DESTBLEND);
		uint32 oldZW = GetRenderState(ZWRITEENABLE), oldVA = GetRenderState(VERTEXALPHA);
		d3d::setTexture(0, env->tex);
		d3d::setTexture(1, env->tex);
		uploadEnvMatrixRW(env->frame);
		SetRenderState(VERTEXALPHA, 1);
		SetRenderState(SRCBLEND, BLENDSRCALPHA);
		SetRenderState(DESTBLEND, BLENDONE);
		SetRenderState(ZWRITEENABLE, 0);

		static float zero[4];
		float fxparams[4];
		fxparams[0] = (cbyte > 255 ? 255 : cbyte) / 255.0f;
		fxparams[1] = fxparams[2] = fxparams[3] = 0.0f;
		d3ddevice->SetPixelShaderConstantF(PSLOC_shininess, fxparams, 1);
		d3ddevice->SetVertexShaderConstantF(VSLOC_colorClamp, zero, 1);
		d3ddevice->SetVertexShaderConstantF(VSLOC_envColor, zero, 1);

		if((lightBits & VSLIGHT_MASK) == 0)
			setVertexShader(matfx_env_amb_VS);
		else if((lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
			setVertexShader(matfx_env_amb_dir_VS);
		else
			setVertexShader(matfx_env_all_VS);
		setPixelShader(matfx_envpass_PS);

		drawInst(header, inst);

		SetRenderState(ZWRITEENABLE, oldZW);
		SetRenderState(DESTBLEND, oldDst);
		SetRenderState(SRCBLEND, oldSrc);
		SetRenderState(VERTEXALPHA, oldVA);
		d3d::setTexture(1, nil);
		return;
	}

	// librw's one-pass version, unused since the two-pass one above
	d3d::setTexture(1, env->tex);
	uploadEnvMatrix(env->frame);

	SetRenderState(SRCBLEND, BLENDONE);

	static float zero[4];
	static float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	struct  {
		float shininess;
		float disableFBA;
		float unused[2];
	} fxparams;
	fxparams.shininess = env->coefficient;
	fxparams.disableFBA = env->fbAlpha ? 0.0f : 1.0f;
	d3ddevice->SetPixelShaderConstantF(PSLOC_shininess, (float*)&fxparams, 1);
	// This clamps the vertex color below. With it we can achieve both PC and PS2 style matfx
	if(MatFX::envMapApplyLight)
		d3ddevice->SetVertexShaderConstantF(VSLOC_colorClamp, zero, 1);
	else
		d3ddevice->SetVertexShaderConstantF(VSLOC_colorClamp, one, 1);
	RGBAf envcol[4];
	if(MatFX::envMapUseMatColor)
		convColor(envcol, &m->color);
	else
		convColor(envcol, &MatFX::envMapColor);
	d3ddevice->SetVertexShaderConstantF(VSLOC_envColor, (float*)&envcol, 1);

	// Pick a shader
	if((lightBits & VSLIGHT_MASK) == 0)
		setVertexShader(matfx_env_amb_VS);
	else if((lightBits & VSLIGHT_MASK) == VSLIGHT_DIRECT)
		setVertexShader(matfx_env_amb_dir_VS);
	else
		setVertexShader(matfx_env_all_VS);

	bool32 texAlpha = GETD3DRASTEREXT(env->tex->raster)->hasAlpha;

	if(inst->material->texture){
		d3d::setTexture(0, m->texture);
		setPixelShader(matfx_env_tex_PS);
	}else{
		d3d::unbindTexture0();	
		setPixelShader(matfx_env_PS);
	}

	SetRenderState(VERTEXALPHA, texAlpha || inst->vertexAlpha || m->color.alpha != 255);

	drawInst(header, inst);

	SetRenderState(SRCBLEND, BLENDSRCALPHA);
}

void
matfxRenderCB_Shader(Atomic *atomic, InstanceDataHeader *header)
{
	int vsBits;
	uint32 flags = atomic->geometry->flags;
	setStreamSource(0, (IDirect3DVertexBuffer9*)header->vertexStream[0].vertexBuffer,
	                           0, header->vertexStream[0].stride);
	setIndices((IDirect3DIndexBuffer9*)header->indexBuffer);
	setVertexDeclaration((IDirect3DVertexDeclaration9*)header->vertexDeclaration);

	lastEnvFrame = nil;

	vsBits = lightingCB_Shader(atomic);
	uploadMatrices(atomic->getFrame()->getLTM());

	bool normals = !!(atomic->geometry->flags & Geometry::NORMALS);

	InstanceData *inst = header->inst;
	for(uint32 i = 0; i < header->numMeshes; i++){
		Material *m = inst->material;

		if(d3d::meshMaterialHook)	// per-mesh material callback, called first for every mesh
			d3d::meshMaterialHook(m);

		setMaterial(flags, m->color, m->surfaceProps);

		MatFX *matfx = MatFX::get(m);
		if(matfx == nil)
			matfxRender_Default(header, inst, vsBits);
		else switch(matfx->type){
		case MatFX::ENVMAP:
			if(normals)
				matfxRender_EnvMap(header, inst, vsBits, &matfx->fx[0].env);
			else
				matfxRender_Default(header, inst, vsBits);
			break;
		case MatFX::NOTHING:
		case MatFX::BUMPMAP:
		case MatFX::BUMPENVMAP:
		case MatFX::DUAL:
		case MatFX::UVTRANSFORM:
		case MatFX::DUALUVTRANSFORM:
			// not supported yet
			matfxRender_Default(header, inst, vsBits);
			break;			
		}

		inst++;
	}
	d3d::setTexture(1, nil);
}

#define VS_NAME g_vs20_main
#define PS_NAME g_ps20_main

void
createMatFXShaders(void)
{
	{
		static
#include "shaders/matfx_env_amb_VS.h"
		matfx_env_amb_VS = createVertexShader((void*)VS_NAME);
		assert(matfx_env_amb_VS);
	}
	{
		static
#include "shaders/matfx_env_amb_dir_VS.h"
		matfx_env_amb_dir_VS = createVertexShader((void*)VS_NAME);
		assert(matfx_env_amb_dir_VS);
	}
	{
		static
#include "shaders/matfx_env_all_VS.h"
		matfx_env_all_VS = createVertexShader((void*)VS_NAME);
		assert(matfx_env_all_VS);
	}


	{
		static
#include "shaders/matfx_env_PS.h"
		matfx_env_PS = createPixelShader((void*)PS_NAME);
		assert(matfx_env_PS);
	}
	{
		static
#include "shaders/matfx_env_tex_PS.h"
		matfx_env_tex_PS = createPixelShader((void*)PS_NAME);
		assert(matfx_env_tex_PS);
	}
	{
		static
#include "shaders/matfx_envpass_PS.h"
		matfx_envpass_PS = createPixelShader((void*)PS_NAME);
		assert(matfx_envpass_PS);
	}

}

void
destroyMatFXShaders(void)
{
	destroyVertexShader(matfx_env_amb_VS);
	matfx_env_amb_VS = nil;

	destroyVertexShader(matfx_env_amb_dir_VS);
	matfx_env_amb_dir_VS = nil;

	destroyVertexShader(matfx_env_all_VS);
	matfx_env_all_VS = nil;


	destroyPixelShader(matfx_env_PS);
	matfx_env_PS = nil;

	destroyPixelShader(matfx_env_tex_PS);
	matfx_env_tex_PS = nil;

	destroyPixelShader(matfx_envpass_PS);
	matfx_envpass_PS = nil;
}

#endif

static void*
matfxOpen(void *o, int32, int32)
{
#ifdef RW_D3D9
	createMatFXShaders();
#endif

	matFXGlobals.pipelines[PLATFORM_D3D9] = makeMatFXPipeline();
	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
#ifdef RW_D3D9
	destroyMatFXShaders();
#endif

	((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_D3D9])->destroy();
	matFXGlobals.pipelines[PLATFORM_D3D9] = nil;
	return o;
}

void
initMatFX(void)
{
	Driver::registerPlugin(PLATFORM_D3D9, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = matfxRenderCB_Shader;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

}
}
