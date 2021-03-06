/*
 * Copyright 2016, Björn Ståhl
 * License: 3-Clause BSD, see COPYING file in arcan source repository.
 * Reference: http://arcan-fe.com
 * Description: egl-dri specific render-node based backend support
 * library for setting up headless display, and passing handles
 * handling render-node transfer
 */
#define WANT_ARCAN_SHMIF_HELPER
#define AGP_ENABLE_UNPURE
#include "../arcan_shmif.h"
#include "../shmif_privext.h"
#include "video_platform.h"
#include "agp/glfun.h"

#define EGL_EGLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#define MESA_EGL_NO_X11_HEADERS
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <drm.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <gbm.h>
#include <stdatomic.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

_Thread_local static struct arcan_shmif_cont* active_context;

static struct agp_fenv agp_fenv;

/*
 * note: should be moved into the agp_fenv
 */
static PFNEGLCREATEIMAGEKHRPROC create_image;
static PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC query_image_format;
static PFNEGLEXPORTDMABUFIMAGEMESAPROC export_dmabuf;
static PFNEGLDESTROYIMAGEKHRPROC destroy_image;

struct shmif_ext_hidden_int {
	struct gbm_device* dev;
	struct agp_rendertarget* rtgt_a, (* rtgt_b), (* rtgt_cur);

/* with the gbm- buffer passing, we pretty much need double-buf */
	struct agp_vstore buf_a, buf_b, (* buf_cur);
	bool nopass, swap;

	EGLImage image;
	int dmabuf;

/* need to account for multiple contexts being created on the same setup */
	uint64_t ctx_alloc;
	EGLContext alt_contexts[64];

	int type;
	bool managed;
	EGLContext context;
	unsigned context_ind;
	EGLDisplay display;
	EGLSurface surface;
};

/*
 * These are spilled over from AGP, and ideally, we should just
 * separate those references or linker-script erase them as they are
 * not needed here
*/
void* platform_video_gfxsym(const char* sym)
{
	return eglGetProcAddress(sym);
}

bool platform_video_map_handle(struct agp_vstore* store, int64_t handle)
{
	return false;
}

static bool check_functions(void*(*lookup)(void*, const char*), void* tag)
{
	create_image = (PFNEGLCREATEIMAGEKHRPROC)
		lookup(tag, "eglCreateImageKHR");
	destroy_image = (PFNEGLDESTROYIMAGEKHRPROC)
		lookup(tag, "eglDestroyImageKHR");
	query_image_format = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)
		lookup(tag, "eglExportDMABUFImageQueryMESA");
	export_dmabuf = (PFNEGLEXPORTDMABUFIMAGEMESAPROC)
		lookup(tag, "eglExportDMABUFImageMESA");
	return create_image && destroy_image && query_image_format && export_dmabuf;
}

static void zap_vstore(struct agp_vstore* vstore)
{
	free(vstore->vinf.text.raw);
	vstore->vinf.text.raw = NULL;
	vstore->vinf.text.s_raw = 0;
}

static void gbm_drop(struct arcan_shmif_cont* con)
{
	if (!con->privext->internal)
		return;

	struct shmif_ext_hidden_int* in = con->privext->internal;

	if (in->dev){
/* this will actually free the gbm- resources as well */
		if (in->rtgt_cur){
			agp_drop_rendertarget(in->rtgt_a);
			agp_drop_rendertarget(in->rtgt_b);
			agp_drop_vstore(&in->buf_a);
			agp_drop_vstore(&in->buf_b);
			zap_vstore(&in->buf_a);
			zap_vstore(&in->buf_b);
			in->rtgt_cur;
		}
		if (in->image){
			destroy_image(in->display, in->image);
		}
		if (in->managed){
			eglMakeCurrent(in->display,
				EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
			if (in->context)
				eglDestroyContext(in->display, in->context);
			eglTerminate(in->display);
		}
		in->dev = NULL;
	}

	if (-1 != in->dmabuf){
		close(in->dmabuf);
		in->dmabuf = -1;
	}

	free(con->privext->internal);
	con->privext->internal = NULL;
	con->privext->cleanup = NULL;
}

struct arcan_shmifext_setup arcan_shmifext_defaults(
	struct arcan_shmif_cont* con)
{
	int major = getenv("AGP_GL_MAJOR") ?
		strtoul(getenv("AGP_GL_MAJOR"), NULL, 10) : 2;

	int minor= getenv("AGP_GL_MINOR") ?
		strtoul(getenv("AGP_GL_MINOR"), NULL, 10) : 1;

	return (struct arcan_shmifext_setup){
		.red = 1, .green = 1, .blue = 1,
		.alpha = 1, .depth = 16,
		.api = API_OPENGL,
		.builtin_fbo = 2,
		.major = 2, .minor = 1,
		.shared_context = 0
	};
}

static void* lookup(void* tag, const char* sym)
{
	return eglGetProcAddress(sym);
}

void* arcan_shmifext_lookup(
	struct arcan_shmif_cont* con, const char* fun)
{
	return eglGetProcAddress(fun);
}

static void* lookup_fenv(void* tag, const char* sym, bool req)
{
	return eglGetProcAddress(sym);
}

static bool get_egl_context(
	struct shmif_ext_hidden_int* ctx, unsigned ind, EGLContext* dst)
{
	if (ind >= 64)
		return false;

	if (!ctx->managed || !((1 << ind) & ctx->ctx_alloc))
		return false;

	*dst = ctx->alt_contexts[(1 << ind)-1];
	return true;
}

static enum shmifext_setup_status add_context(
	struct shmif_ext_hidden_int* ctx, struct arcan_shmifext_setup* arg,
	unsigned* ind)
{
/* make sure the shmifext has been setup */
	int type;
	EGLint nc;
	switch(arg->api){
		case API_OPENGL: type = EGL_OPENGL_BIT; break;
		case API_GLES: type = EGL_OPENGL_ES2_BIT; break;
		default:
			return SHMIFEXT_NO_API;
		break;
	}

	const EGLint attribs[] = {
		EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, type,
		EGL_RED_SIZE, arg->red,
		EGL_GREEN_SIZE, arg->green,
		EGL_BLUE_SIZE, arg->blue,
		EGL_ALPHA_SIZE, arg->alpha,
		EGL_DEPTH_SIZE, arg->depth,
		EGL_NONE
	};

/* find first free */
	size_t i = 0;
	bool found = false;
	for (; i < 64; i++)
		if (!(ctx->ctx_alloc & (1 << i))){
			found = true;
			break;
		}

/* common for GL applications to treat 0 as no context, so we do the same, have
 * to add/subtract 1 from the index (or just XOR with a cookie */
	if (!found)
		return SHMIFEXT_OUT_OF_MEMORY;

	if (!eglGetConfigs(ctx->display, NULL, 0, &nc))
		return SHMIFEXT_NO_CONFIG;

	if (0 == nc)
		return SHMIFEXT_NO_CONFIG;

	EGLConfig cfg;
	if (!eglChooseConfig(ctx->display, attribs, &cfg, 1, &nc) || nc < 1)
		return SHMIFEXT_NO_CONFIG;

	EGLint cas[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE,
		EGL_NONE, EGL_NONE, EGL_NONE, EGL_NONE, EGL_NONE, EGL_NONE,
		EGL_NONE, EGL_NONE, EGL_NONE};

	int ofs = 0;
	if (arg->api != API_GLES){
		if (arg->major){
			cas[ofs++] = EGL_CONTEXT_MAJOR_VERSION_KHR;
			cas[ofs++] = arg->major;
			cas[ofs++] = EGL_CONTEXT_MINOR_VERSION_KHR;
			cas[ofs++] = arg->minor;
		}

		if (arg->mask){
			cas[ofs++] = EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR;
			cas[ofs++] = arg->mask;
		}

		if (arg->flags){
			cas[ofs++] = EGL_CONTEXT_FLAGS_KHR;
			cas[ofs++] = arg->flags;
		}
	}

	EGLContext sctx = NULL;
	if (arg->shared_context)
		get_egl_context(ctx, arg->shared_context, &sctx);

	ctx->alt_contexts[(1 << i)-1] =
		eglCreateContext(ctx->display, cfg, sctx, cas);
	if (!ctx->alt_contexts[i << i])
		return SHMIFEXT_NO_CONTEXT;

	ctx->ctx_alloc |= 1 << i;
	*ind = i+1;
	return SHMIFEXT_OK;
}

unsigned arcan_shmifext_add_context(
	struct arcan_shmif_cont* con, struct arcan_shmifext_setup arg)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
			return 0;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;

	unsigned res;
	if (SHMIFEXT_OK != add_context(ctx, &arg, &res)){
		return 0;
	}

	return res;
}

void arcan_shmifext_swap_context(
	struct arcan_shmif_cont* con, unsigned context)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display || context > 64 || !context)
			return;

	context--;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;
	EGLContext egl_ctx;

	if (!get_egl_context(ctx, context, &egl_ctx))
		return;

	ctx->context_ind = context;
	ctx->context = egl_ctx;
	eglMakeCurrent(ctx->display, ctx->surface, ctx->surface, ctx->context);
}

enum shmifext_setup_status arcan_shmifext_setup(
	struct arcan_shmif_cont* con,
	struct arcan_shmifext_setup arg)
{
	struct shmif_ext_hidden_int* ctx = con->privext->internal;
	enum shmifext_setup_status res;

	if (ctx && ctx->display)
		return SHMIFEXT_ALREADY_SETUP;

	switch (arg.api){
	case API_OPENGL:
		if (!((ctx && ctx->display) || eglBindAPI(EGL_OPENGL_API)))
			return SHMIFEXT_NO_API;
	break;
	case API_GLES:
		if (!((ctx && ctx->display) || eglBindAPI(EGL_OPENGL_ES_API)))
			return SHMIFEXT_NO_API;
	break;
	case API_VHK:
	default:
/* won't have working code here for a while, first need a working AGP_
 * implementation that works with normal Arcan. Then there's the usual
 * problem with getting access to a handle, for EGLStreams it should
 * work, but with GBM? KRH VKCube has some intel- only hack */
		return SHMIFEXT_NO_API;
	break;
	};

	void* display;
	if (!arcan_shmifext_egl(con, &display, lookup, NULL))
		return SHMIFEXT_NO_DISPLAY;

	ctx = con->privext->internal;
	ctx->display = eglGetDisplay((EGLNativeDisplayType) display);
	if (!ctx->display)
		return SHMIFEXT_NO_DISPLAY;

	if (!eglInitialize(ctx->display, NULL, NULL))
		return SHMIFEXT_NO_EGL;

/*
 * this is likely not the best way to keep it if we try to run multiple
 * segments on different GPUs with different GL implementations, if/when
 * that becomes a problem, move to a context specific one
 */
	if (!agp_fenv.draw_buffer){
		agp_glinit_fenv(&agp_fenv, lookup_fenv, NULL);
		agp_setenv(&agp_fenv);
	}

	if (arg.no_context)
		return SHMIFEXT_OK;

/* we have egl and a display, build a config/context and set it as the
 * current default context for this shmif-connection */
	ctx->managed = true;
	unsigned ind;
	res = add_context(ctx, &arg, &ind);

	if (SHMIFEXT_OK != res)
		return res;

	arcan_shmifext_swap_context(con, ind);
	ctx->surface = EGL_NO_SURFACE;

	active_context = con;

	if (arg.builtin_fbo || arg.vidp_pack){
		agp_empty_vstore(&ctx->buf_a, con->w, con->h);
		agp_empty_vstore(&ctx->buf_b, con->w, con->h);
		ctx->buf_cur = &ctx->buf_a;

/*
 * mode 3 : 1 FBO, swap attachments.
 * mode 2 : 2 FBOs, swap active.
 */
		if (arg.builtin_fbo){
			ctx->swap = arg.builtin_fbo == 3;

			ctx->rtgt_a = agp_setup_rendertarget(
				ctx->buf_cur, (arg.depth > 0 ?
					RENDERTARGET_COLOR_DEPTH_STENCIL : RENDERTARGET_COLOR) |
					(ctx->swap ? RENDERTARGET_DOUBLEBUFFER : 0)
			);
			if (arg.builtin_fbo == 2){
				ctx->rtgt_b = agp_setup_rendertarget(
					ctx->buf_cur, arg.depth > 0 ?
						RENDERTARGET_COLOR_DEPTH_STENCIL : RENDERTARGET_COLOR
				);
			}
			ctx->rtgt_cur = ctx->rtgt_a;
			agp_activate_rendertarget(ctx->rtgt_cur);
		}

		if (arg.vidp_pack){
			ctx->buf_a.vinf.text.s_fmt =
				ctx->buf_b.vinf.text.s_fmt = arg.vidp_infmt;
		}
	}

	arcan_shmifext_make_current(con);
	return SHMIFEXT_OK;
}

bool arcan_shmifext_drop(struct arcan_shmif_cont* con)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
		return false;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;

	eglMakeCurrent(ctx->display,
		EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

	for (size_t i = 0; i < 64 && ctx->ctx_alloc; i++){
		if ((ctx->ctx_alloc & ((1<<i)))){
			ctx->ctx_alloc &= ~(1 << i);
			eglDestroyContext(ctx->display, ctx->alt_contexts[i]);
			ctx->alt_contexts[i] = NULL;
		}
	}

	ctx->context = NULL;
	active_context = NULL;
	gbm_drop(con);
	return true;
}

bool arcan_shmifext_drop_context(struct arcan_shmif_cont* con)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
		return false;

/* might be a different context in TLS, so switch first */
	struct arcan_shmif_cont* old = active_context;
	if (active_context != con)
		arcan_shmifext_make_current(con);

	struct shmif_ext_hidden_int* ctx = con->privext->internal;

/* it's the caller's responsibility to switch in a new ctx, but right
 * now, we're in a state where managed = true, though there's no context */
	if (ctx->context){
		eglMakeCurrent(ctx->display,
			EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
		eglDestroyContext(ctx->display, ctx->context);
		ctx->context = NULL;
	}

/* and restore */
	arcan_shmifext_make_current(old);
	return true;
}

static void authenticate_fd(struct arcan_shmif_cont* con, int fd)
{
/* is it a render node or a real device? */
	struct stat nodestat;
	if (0 == fstat(fd, &nodestat) && !(nodestat.st_rdev & 0x80)){
		unsigned magic;
		drmGetMagic(fd, &magic);
		atomic_store(&con->addr->vpts, magic);
		con->hints |= SHMIF_RHINT_AUTH_TOK;
		arcan_shmif_resize(con, con->w, con->h);
		con->hints &= ~SHMIF_RHINT_AUTH_TOK;
		magic = atomic_load(&con->addr->vpts);
	}
}

void arcan_shmifext_bufferfail(struct arcan_shmif_cont* con, bool st)
{
	if (!con || !con->privext || !con->privext->internal)
		return;

	con->privext->internal->nopass =
		getenv("ARCAN_VIDEO_NO_FDPASS") ? 1 : st;
}

int arcan_shmifext_dev(struct arcan_shmif_cont* con,
	uintptr_t* dev, bool clone)
{
	if (!con || !con->privext || !con->privext->internal)
		return -1;

	if (dev)
		*dev = (uintptr_t) con->privext->internal->dev;

	if (clone){
		int fd = arcan_shmif_dupfd(con->privext->active_fd, -1, true);
		if (-1 != fd)
			authenticate_fd(con, fd);
		return fd;
	}
	else
	  return con->privext->active_fd;
}

bool arcan_shmifext_gl_handles(struct arcan_shmif_cont* con,
	uintptr_t* frame, uintptr_t* color, uintptr_t* depth)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display || !con->privext->internal->rtgt_cur)
		return false;

	agp_rendertarget_ids(con->privext->internal->rtgt_cur, frame, color, depth);
	return true;
}

bool arcan_shmifext_egl(struct arcan_shmif_cont* con,
	void** display, void*(*lookup)(void*, const char*), void* tag)
{
	if (!lookup || !con || !con->addr || !display)
		return false;

	int dfd = -1;

/* case for switching to another node, we're still missing a way to extract the
 * 'real' library paths to the GL implementation and to the EGL implementation
 * for dynamic- GPU switching */
	if (con->privext->pending_fd != -1){
		if (-1 != con->privext->active_fd){
			close(con->privext->active_fd);
			con->privext->active_fd = -1;
			gbm_drop(con);
		}
		dfd = con->privext->pending_fd;
		con->privext->pending_fd = -1;
	}
	else if (-1 != con->privext->active_fd){
		dfd = con->privext->active_fd;
	}
/* or first setup without a pending_fd */
	else if (!con->privext->internal){
		const char* nodestr = getenv("ARCAN_RENDER_NODE") ?
			getenv("ARCAN_RENDER_NODE") : "/dev/dri/renderD128";
		dfd = open(nodestr, O_RDWR | O_CLOEXEC);
	}
/* mode-switch is no-op in init here, but we still may need
 * to update function pointers due to possible context changes */
	else
		return check_functions(lookup, tag);

	if (-1 == dfd)
		return false;

/* special cleanup to deal with gbm_device abstraction */
	con->privext->cleanup = gbm_drop;
	con->privext->active_fd = dfd;
	authenticate_fd(con, dfd);

/* finally open device */
	if (!con->privext->internal){
		con->privext->internal = malloc(sizeof(struct shmif_ext_hidden_int));
		if (!con->privext->internal){
			gbm_drop(con);
			return false;
		}

		memset(con->privext->internal, '\0', sizeof(struct shmif_ext_hidden_int));
		con->privext->internal->dmabuf = -1;
		con->privext->internal->nopass = getenv("ARCAN_VIDEO_NO_FDPASS") ?
			true : false;
		if (NULL == (con->privext->internal->dev = gbm_create_device(dfd))){
			gbm_drop(con);
			return false;
		}
	}

	if (!check_functions(lookup, tag)){
		gbm_drop(con);
		return false;
	}

	*display = (void*) (con->privext->internal->dev);
	return true;
}

bool arcan_shmifext_egl_meta(struct arcan_shmif_cont* con,
	uintptr_t* display, uintptr_t* surface, uintptr_t* context)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
		return false;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;
	if (display)
		*display = (uintptr_t) ctx->display;
	if (surface)
		*surface = (uintptr_t) ctx->surface;
	if (context)
		*context = (uintptr_t) ctx->context;

	return true;
}

void arcan_shmifext_bind(struct arcan_shmif_cont* con)
{
/* need to resize both potential rendertarget destinations */
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
		return;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;
	if (active_context != con){
		arcan_shmifext_make_current(con);
	}
/* for the vidp- as scratch, upload texture and send mode, the
 * resize is actually handled just prior to upload, not here */
	else if (ctx->rtgt_cur){
		if (ctx->buf_cur->w != con->w || ctx->buf_cur->h != con->h){
			agp_activate_rendertarget(NULL);

			agp_resize_rendertarget(ctx->rtgt_a, con->w, con->h);
			if (ctx->rtgt_b)
				agp_resize_rendertarget(ctx->rtgt_b, con->w, con->h);
		}

		agp_activate_rendertarget(ctx->rtgt_cur);
	}
}

bool arcan_shmifext_make_current(struct arcan_shmif_cont* con)
{
	if (!con || !con->privext || !con->privext->internal ||
		!con->privext->internal->display)
		return false;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;

	if (active_context != con){
		eglMakeCurrent(ctx->display, ctx->surface, ctx->surface, ctx->context);
		active_context = con;
	}
	arcan_shmifext_bind(con);

	return true;
}

bool arcan_shmifext_gltex_handle(struct arcan_shmif_cont* con,
   uintptr_t display, uintptr_t tex_id,
	 int* dhandle, size_t* dstride, int* dfmt)
{
	if (!con || !con->addr || !con->privext || !con->privext->internal)
		return -1;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;

	EGLDisplay* dpy = display == 0 ?
		con->privext->internal->display : (EGLDisplay*) display;

	if (ctx->image){
		destroy_image(dpy, ctx->image);
		close(ctx->dmabuf);
		ctx->dmabuf = -1;
	}

	ctx->image = create_image(dpy, eglGetCurrentContext(),
		EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(tex_id), NULL);

	if (!ctx->image)
		return false;

	int fourcc, nplanes;
	if (!query_image_format(dpy, ctx->image, &fourcc, &nplanes, NULL))
		return false;

/* currently unsupported */
	if (nplanes != 1)
		return false;

	EGLint stride;
	if (!export_dmabuf(dpy, ctx->image, dhandle, &stride, NULL)|| stride < 0){
		destroy_image(dpy, ctx->image);
		return false;
	}

	*dfmt = fourcc;
	*dstride = stride;
	ctx->dmabuf = *dhandle;
	return true;
}

int arcan_shmifext_signal(struct arcan_shmif_cont* con,
	uintptr_t display, int mask, uintptr_t tex_id, ...)
{
	if (!con || !con->addr || !con->privext || !con->privext->internal)
		return -1;

	struct shmif_ext_hidden_int* ctx = con->privext->internal;

	EGLDisplay* dpy = display == 0 ?
		con->privext->internal->display : (EGLDisplay*) display;

	if (!dpy)
		return -1;

	if (tex_id == SHMIFEXT_BUILTIN){
		if (!ctx->managed)
			return -1;

		if (!ctx->rtgt_cur){
			enum stream_type type = STREAM_RAW_DIRECT_SYNCHRONOUS;

/* vidp- to texture upload, rather than FBO indirection, but only
 * if handle passing is still working */
		if (con->privext->internal->nopass)
			goto fallback;

/* mark this so the backing GLID / PBOs gets reallocated */
			if (ctx->buf_cur->w != con->w || ctx->buf_cur->h != con->h)
				type = STREAM_EXT_RESYNCH;

/* bpp/format are set during the shmifext_setup */
			ctx->buf_cur->w = con->w;
			ctx->buf_cur->h = con->h;
			ctx->buf_cur->vinf.text.raw = con->vidp;
			ctx->buf_cur->vinf.text.s_raw = con->w * con->h * sizeof(shmif_pixel);

/* we ignore the dirty- updates here due to the double buffering */
			struct stream_meta stream = {.buf = NULL};
			stream.buf = con->vidp;
			stream = agp_stream_prepare(ctx->buf_cur, stream, type);
			agp_stream_commit(ctx->buf_cur, stream);
			ctx->buf_cur->vinf.text.raw = NULL;
			struct agp_fenv* env = agp_env();

/* With MESA/amd, this seemed unavoidable or the extracted img won't be in a
 * synchronized state. Preferably we'd have another interface to do the texture
 * through */
			env->flush();
		}

		tex_id = ctx->buf_cur->vinf.text.glid;

/*
 * Swap active rendertarget (if one exists) or there's a possible data-race(?)
 * where server-side has the color attachment bound and drawing when we update
 */
		if (ctx->rtgt_cur){

/* IF the rendertarget is double-buffered, swap the buffers and get the ID of
 * the last FRONT. IF we have double- rendertargets, swap the destination */
			tex_id = agp_rendertarget_swap(ctx->rtgt_cur);
			if (ctx->rtgt_b)
				ctx->rtgt_cur = ctx->rtgt_cur==ctx->rtgt_a ? ctx->rtgt_b : ctx->rtgt_a;
		}
		else
			ctx->buf_cur = (ctx->buf_cur == &ctx->buf_a ? &ctx->buf_b : &ctx->buf_a);
	}

/*
 * IF we don't have the extension for GBM- style buffer swapping, or the server
 * has told us that the handle we're receiving don't work - go with a readback
 * to vidp. Can happen with multiple incompatible GPUs.
 */
	if (con->privext->internal->nopass || !create_image)
		goto fallback;

	int fd, fourcc;
	size_t stride;

	if (!arcan_shmifext_gltex_handle(con, display, tex_id, &fd, &stride, &fourcc))
		goto fallback;

	unsigned res = arcan_shmif_signalhandle(con, mask, fd, stride, fourcc);

	return res > INT_MAX ? INT_MAX : res;

/*
 * this should really be switched to flipping PBOs, or even better,
 * somehow be able to mark/pin our output buffer for safe readback
 */
fallback:
	if (1){
	struct agp_vstore vstore = {
		.w = con->w,
		.h = con->h,
		.txmapped = TXSTATE_TEX2D,
		.vinf.text = {
			.glid = tex_id,
			.raw = (void*) con->vidp
		},
	};

	if (ctx->rtgt_cur){
		agp_activate_rendertarget(NULL);
		agp_readback_synchronous(&vstore);
		agp_activate_rendertarget(ctx->rtgt_cur);
	}
	else
		agp_readback_synchronous(&vstore);
	}
	res = arcan_shmif_signal(con, mask);
	return res > INT_MAX ? INT_MAX : res;
}
