#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "../rwbase.h"
#include "../rwerror.h"
#include "../rwplg.h"
#include "../rwrender.h"
#include "../rwengine.h"
#include "../rwpipeline.h"
#include "../rwobjects.h"
#include "../rwanim.h"
#include "../rwplugins.h"

#include "rwgl3.h"
#include "rwgl3shader.h"
#include "rwgl3plg.h"

#include "rwgl3impl.h"

namespace rw {
namespace gl3 {

#ifdef RW_OPENGL

static Shader *envShader, *envShader_noAT;
static Shader *envShader_fullLight, *envShader_fullLight_noAT;
static Shader *envPassShader;	// zoosters
static Matrix *envLogWorld;	// zoosters: the atomic LTM of the mesh being drawn (PORT_ENVLOG only)
static Shader *uvShader, *uvShader_noAT, *uvShader_fullLight, *uvShader_fullLight_noAT;	// zoosters
static int32 u_texMatrix;
static int32 u_fxparams;
static int32 u_colorClamp;
static int32 u_envColor;

void
matfxDefaultRender(InstanceDataHeader *header, InstanceData *inst, int32 vsBits, uint32 flags)
{
	Material *m;
	m = inst->material;

	setMaterial(flags, m->color, m->surfaceProps);

	setTexture(0, m->texture);

	rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

	if((vsBits & VSLIGHT_MASK) == 0){
		if(getAlphaTest())
			defaultShader->use();
		else
			defaultShader_noAT->use();
	}else{
		if(getAlphaTest())
			defaultShader_fullLight->use();
		else
			defaultShader_fullLight_noAT->use();
	}

	drawInst(header, inst);
}

// zoosters: MatFX UV transform, as RW 3.6's D3D8 MatFX does it: rpMATFXEFFECTUVTRANSFORM draws with the default render, which sets
// the base transform as stage 0's texture matrix (D3DTTFF_COUNT2, _11 _12 = right.xy, _21 _22 = up.xy, _31 _32 = pos.xy; skipped for a null /
// identity-flagged matrix): u' = u*right.x + v*up.x + pos.x, v' = u*right.y + v*up.y + pos.y. librw had no UV transform
// (used e.g. to scroll water). The vertex shader is librw's default.vert with the texcoord line transformed.
static const char *uvxform_vert_src =
"uniform mat4 u_texMatrix;\n"
"VSIN(ATTRIB_POS)	vec3 in_pos;\n"
"VSOUT vec4 v_color;\n"
"VSOUT vec2 v_tex0;\n"
"VSOUT float v_fog;\n"
"void\n"
"main(void)\n"
"{\n"
"	vec4 Vertex = u_world * vec4(in_pos, 1.0);\n"
"	gl_Position = u_proj * u_view * Vertex;\n"
"	vec3 Normal = mat3(u_world) * in_normal;\n"
"	v_tex0 = (u_texMatrix * vec4(in_tex0, 0.0, 1.0)).xy;\n"
"	v_color = in_color;\n"
"	v_color.rgb += u_ambLight.rgb*surfAmbient;\n"
"	v_color.rgb += DoDynamicLight(Vertex.xyz, Normal)*surfDiffuse;\n"
"	v_color = clamp(v_color, 0.0, 1.0);\n"
"	v_color *= u_matColor;\n"
"	v_fog = DoFog(gl_Position.w);\n"
"}\n";

static void
matfxUVTransformRender(InstanceDataHeader *header, InstanceData *inst, int32 vsBits, uint32 flags, Matrix *base)
{
	if(base == nil || (base->flags & Matrix::IDENTITY)){
		matfxDefaultRender(header, inst, vsBits, flags);
		return;
	}
	Material *m = inst->material;
	setMaterial(flags, m->color, m->surfaceProps);
	setTexture(0, m->texture);
	rw::SetRenderState(VERTEXALPHA, inst->vertexAlpha || m->color.alpha != 0xFF);

	RawMatrix t;
	t.right.set(base->right.x, base->right.y, 0.0f); t.rightw = 0.0f;
	t.up.set(base->up.x, base->up.y, 0.0f); t.upw = 0.0f;
	t.at.set(0.0f, 0.0f, 1.0f); t.atw = 0.0f;
	t.pos.set(base->pos.x, base->pos.y, 0.0f); t.posw = 1.0f;

	if((vsBits & VSLIGHT_MASK) == 0){
		if(getAlphaTest())
			uvShader->use();
		else
			uvShader_noAT->use();
	}else{
		if(getAlphaTest())
			uvShader_fullLight->use();
		else
			uvShader_fullLight_noAT->use();
	}
	setUniform(u_texMatrix, &t);

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
	static RawMatrix envMtx;
// can't do it, frame matrix may change
//	if(frame != lastEnvFrame){
//		lastEnvFrame = frame;
	{

		RawMatrix invMtx;
		Matrix::invert(&invMat, frame->getLTM());
		convMatrix(&invMtx, &invMat);
		invMtx.pos.set(0.0f, 0.0f, 0.0f);
		float uscale = fabs(normal2texcoord.right.x);
		normal2texcoord.right.x = MatFX::envMapFlipU ? -uscale : uscale;
		RawMatrix::mult(&envMtx, &invMtx, &normal2texcoord);
	}
	setUniform(u_texMatrix, &envMtx);
}

// zoosters: the env transform of RW 3.6's D3D8 MatFX, as on d3d9: camera space normals
// transformed by [D * inv(envFrame LTM) * camera LTM] (frame given, D = negate the x row) * normal2texcoord, or by normal2texcoord alone when the env map has
// no frame. The shader gets world normals, so the world -> camera space normal rotation (the D3D view matrix mirrors x) is prepended.
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
	setUniform(u_texMatrix, &tot);
}

// zoosters: two passes like RW 3.6's D3D8 MatFX (generic path), as on d3d9:
// pass 1 = the base mesh, pass 2 = the env texture, z write off, SRCALPHA / ONE blend, lighting off, colour = env texture * coefficient byte
// (trunc(coefficient * 255) / 255), alpha = env texture alpha, fog colour black (ENVPASS in matfx_env.frag). Skipped when the byte is 0.
// PORT_ENVLOG=<file> logs the first 20 env passes, then every 500th, to that file.
void
matfxEnvRender(InstanceDataHeader *header, InstanceData *inst, int32 vsBits, uint32 flags, MatFX::Env *env)
{
	Material *m;
	m = inst->material;

	static int envLogged = 0;
	static const char *envLog = getenv("PORT_ENVLOG");

	int32 cbyte = env->tex ? (int32)(env->coefficient * 255.0f) : 0;
	if(env->tex == nil || cbyte <= 0){
		matfxDefaultRender(header, inst, vsBits, flags);
		return;
	}

	{	// own scope: librw's one-pass version below uses the same names
		// pass 1: base
		matfxDefaultRender(header, inst, vsBits, flags);

		// pass 2: environment map
		uint32 oldSrc = GetRenderState(SRCBLEND), oldDst = GetRenderState(DESTBLEND);
		uint32 oldZW = GetRenderState(ZWRITEENABLE), oldVA = GetRenderState(VERTEXALPHA);
		setTexture(1, env->tex);
		uploadEnvMatrixRW(env->frame);
		rw::SetRenderState(VERTEXALPHA, 1);
		rw::SetRenderState(SRCBLEND, BLENDSRCALPHA);
		rw::SetRenderState(DESTBLEND, BLENDONE);
		rw::SetRenderState(ZWRITEENABLE, 0);

		float fxparams[4];
		fxparams[0] = (cbyte > 255 ? 255 : cbyte) / 255.0f;
		fxparams[1] = fxparams[2] = fxparams[3] = 0.0f;
		setUniform(u_fxparams, fxparams);

		envPassShader->use();
		drawInst(header, inst);

		if(envLog){
			// first 20 env passes of the run, then every 500th (counted); appended to the file named by PORT_ENVLOG
			envLogged++;
			if(envLogged <= 20 || envLogged % 500 == 0){
				FILE *lf = fopen(envLog, "a");
				if(lf){
					fprintf(lf, "envpass %d: tex %p coef %d/255 frame %p old src %u dst %u zw %u va %u world %.1f %.1f %.1f\n", envLogged, (void*)env->tex, cbyte,
					        (void*)env->frame, oldSrc, oldDst, oldZW, oldVA, envLogWorld->pos.x, envLogWorld->pos.y, envLogWorld->pos.z);
					fclose(lf);
				}
			}
		}

		rw::SetRenderState(ZWRITEENABLE, oldZW);
		rw::SetRenderState(DESTBLEND, oldDst);
		rw::SetRenderState(SRCBLEND, oldSrc);
		rw::SetRenderState(VERTEXALPHA, oldVA);
		return;
	}

	// librw's one-pass version, unused since the two-pass one above
	setTexture(0, m->texture);
	setTexture(1, env->tex);
	uploadEnvMatrix(env->frame);

	setMaterial(flags, m->color, m->surfaceProps);

	float fxparams[4];
	fxparams[0] = env->coefficient;
	fxparams[1] = env->fbAlpha ? 0.0f : 1.0f;
	fxparams[2] = fxparams[3] = 0.0f;

	setUniform(u_fxparams, fxparams);
	static float zero[4];
	static float one[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	// This clamps the vertex color below. With it we can achieve both PC and PS2 style matfx
	if(MatFX::envMapApplyLight)
		setUniform(u_colorClamp, zero);
	else
		setUniform(u_colorClamp, one);
	RGBAf envcol[4];
	if(MatFX::envMapUseMatColor)
		convColor(envcol, &m->color);
	else
		convColor(envcol, &MatFX::envMapColor);
	setUniform(u_envColor, envcol);

	rw::SetRenderState(VERTEXALPHA, 1);
	rw::SetRenderState(SRCBLEND, BLENDONE);

	if((vsBits & VSLIGHT_MASK) == 0){
		if(getAlphaTest())
			envShader->use();
		else
			envShader_noAT->use();
	}else{
		if(getAlphaTest())
			envShader_fullLight->use();
		else
			envShader_fullLight_noAT->use();
	}

	drawInst(header, inst);

	rw::SetRenderState(SRCBLEND, BLENDSRCALPHA);
}

void
matfxRenderCB(Atomic *atomic, InstanceDataHeader *header)
{
	uint32 flags = atomic->geometry->flags;
	setWorldMatrix(atomic->getFrame()->getLTM());
	envLogWorld = atomic->getFrame()->getLTM();
	int32 vsBits = lightingCB(atomic);

	setupVertexInput(header);

	lastEnvFrame = nil;

	InstanceData *inst = header->inst;
	int32 n = header->numMeshes;

	while(n--){
		if(gl3::meshMaterialHook)	// zoosters: per-mesh material callback, called first for every mesh
			gl3::meshMaterialHook(inst->material);
		MatFX *matfx = MatFX::get(inst->material);

		if(matfx == nil)
			matfxDefaultRender(header, inst, vsBits, flags);
		else switch(matfx->type){
		case MatFX::ENVMAP:
			matfxEnvRender(header, inst, vsBits, flags, &matfx->fx[0].env);
			break;
		case MatFX::UVTRANSFORM:	// zoosters
			matfxUVTransformRender(header, inst, vsBits, flags, matfx->fx[0].uvtransform.baseTransform);
			break;
		default:
			matfxDefaultRender(header, inst, vsBits, flags);
			break;
		}
		inst++;
	}
	teardownVertexInput(header);
}

ObjPipeline*
makeMatFXPipeline(void)
{
	ObjPipeline *pipe = ObjPipeline::create();
	pipe->instanceCB = defaultInstanceCB;
	pipe->uninstanceCB = defaultUninstanceCB;
	pipe->renderCB = matfxRenderCB;
	pipe->pluginID = ID_MATFX;
	pipe->pluginData = 0;
	return pipe;
}

static void*
matfxOpen(void *o, int32, int32)
{
	matFXGlobals.pipelines[PLATFORM_GL3] = makeMatFXPipeline();

#include "shaders/matfx_gl.inc"
	const char *vs[] = { shaderDecl, header_vert_src, matfx_env_vert_src, nil };
	const char *vs_fullLight[] = { shaderDecl, "#define DIRECTIONALS\n#define POINTLIGHTS\n#define SPOTLIGHTS\n", header_vert_src, matfx_env_vert_src, nil };
	const char *fs[] = { shaderDecl, header_frag_src, matfx_env_frag_src, nil };
	const char *fs_noAT[] = { shaderDecl, "#define NO_ALPHATEST\n", header_frag_src, matfx_env_frag_src, nil };

	envShader = Shader::create(vs, fs);
	assert(envShader);
	envShader_noAT = Shader::create(vs, fs_noAT);
	assert(envShader_noAT);

	envShader_fullLight = Shader::create(vs_fullLight, fs);
	assert(envShader_fullLight);
	envShader_fullLight_noAT = Shader::create(vs_fullLight, fs_noAT);
	assert(envShader_fullLight_noAT);

	const char *fs_envpass[] = { shaderDecl, "#define ENVPASS\n", header_frag_src, matfx_env_frag_src, nil };	// zoosters
	envPassShader = Shader::create(vs, fs_envpass);
	assert(envPassShader);

	{	// zoosters: the UV transform variants of the default shader
#include "shaders/simple_fs_gl.inc"
		const char *vs_uv[] = { shaderDecl, header_vert_src, uvxform_vert_src, nil };
		const char *vs_uv_fullLight[] = { shaderDecl, "#define DIRECTIONALS\n#define POINTLIGHTS\n#define SPOTLIGHTS\n", header_vert_src, uvxform_vert_src, nil };
		const char *fs_uv[] = { shaderDecl, header_frag_src, simple_frag_src, nil };
		const char *fs_uv_noAT[] = { shaderDecl, "#define NO_ALPHATEST\n", header_frag_src, simple_frag_src, nil };
		uvShader = Shader::create(vs_uv, fs_uv);
		assert(uvShader);
		uvShader_noAT = Shader::create(vs_uv, fs_uv_noAT);
		assert(uvShader_noAT);
		uvShader_fullLight = Shader::create(vs_uv_fullLight, fs_uv);
		assert(uvShader_fullLight);
		uvShader_fullLight_noAT = Shader::create(vs_uv_fullLight, fs_uv_noAT);
		assert(uvShader_fullLight_noAT);
	}

	return o;
}

static void*
matfxClose(void *o, int32, int32)
{
	((ObjPipeline*)matFXGlobals.pipelines[PLATFORM_GL3])->destroy();
	matFXGlobals.pipelines[PLATFORM_GL3] = nil;

	envShader->destroy();
	envShader = nil;
	envShader_noAT->destroy();
	envShader_noAT = nil;
	envShader_fullLight->destroy();
	envShader_fullLight = nil;
	envShader_fullLight_noAT->destroy();
	envShader_fullLight_noAT = nil;
	envPassShader->destroy();
	envPassShader = nil;
	uvShader->destroy(); uvShader = nil;	// zoosters
	uvShader_noAT->destroy(); uvShader_noAT = nil;
	uvShader_fullLight->destroy(); uvShader_fullLight = nil;
	uvShader_fullLight_noAT->destroy(); uvShader_fullLight_noAT = nil;

	return o;
}

void
initMatFX(void)
{
	u_texMatrix = registerUniform("u_texMatrix", UNIFORM_MAT4);
	u_fxparams = registerUniform("u_fxparams", UNIFORM_VEC4);
	u_colorClamp = registerUniform("u_colorClamp", UNIFORM_VEC4);
	u_envColor = registerUniform("u_envColor", UNIFORM_VEC4);

	Driver::registerPlugin(PLATFORM_GL3, 0, ID_MATFX,
	                       matfxOpen, matfxClose);
}

#else

void initMatFX(void) { }

#endif

}
}

