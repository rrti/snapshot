#include <array>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

#include <dlfcn.h>
#include <link.h>
#include <pthread.h>
#include <sys/time.h>

#define glFlush glFlush_nouse
#include <GL/gl.h>
#include <GL/glext.h>
#undef glFlush

#define XNextEvent _XNextEvent
#include <X11/Xlib.h>
#include <X11/keysymdef.h>
#undef XNextEvent

#include "frame_recorder.hpp"


#define DL_DECLSYM(s) if (strcmp(__name, #s) == 0) { return &s; }
#define DL_DECLSYMS            \
    DL_DECLSYM(glXSwapBuffers) \
    DL_DECLSYM(XNextEvent)     \


static const char* gl_lib_name = "libGL.so.1";
static const char* dir_env_var = "SNAPSHOT_DIR";
static const char* output_file = "snapshot.out";

static frame_recorder* curr_recorder = nullptr;
// automatically initialized by the glibc run-time
extern char* program_invocation_name;

static void* gl_lib = nullptr;

static char* frame_data = nullptr;

static GLint cap_tex = 0;
static GLint render_tex = 0;
static GLint program = 0;
static GLint viewport[4] = {0, 0, 0, 0};



static int frame_width = 0;
static int frame_height = 0;

static uint64_t frame_counter = 0;
static uint64_t last_event_frame = 0;

static double last_frame_time = 0.0;

static bool recording = false;
static bool lib_inited = false;
static bool first_frame = true;


static std::array<double, 16> framerate_hist = {{
	0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
	0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
}};

static pthread_mutex_t record_mutex;



size_t strftime_c(char* s, const char* format, size_t max)
{
	time_t t = time(nullptr);
	struct tm* tmp = localtime(&t);
	return strftime(s, max, format, tmp);
}

double get_current_time() {
	struct timeval t;
	gettimeofday(&t, nullptr);

	return (t.tv_sec + t.tv_usec / 1000000.0);
}



void (*glUniform1fPtr)(int, float) = nullptr;
void (*glUniform2fPtr)(int, float, float) = nullptr;
void (*glUniform3fPtr)(int, float, float, float) = nullptr;
void (*glUniform4fPtr)(int, float, float, float, float) = nullptr;
void (*glUseProgramPtr)(int) = nullptr;
void (*glLinkProgramPtr)(int) = nullptr;
void (*glDeleteProgramPtr)(int) = nullptr;
void (*glCreateProgramObjectPtr)() = nullptr;
void (*glCompileShaderPtr)(int) = nullptr;
void (*glAttachObjectARBPtr)(int, int) = nullptr;
int (*glCreateShaderPtr)(int) = nullptr;
void (*glPushAttribPtr)(int) = nullptr;
void (*glPushClientAttribPtr)(int) = nullptr;
void (*glPopAttribPtr)() = nullptr;
void (*glPopClientAttribPtr)() = nullptr;
void (*glEnablePtr)(int) = nullptr;
void (*glDisablePtr)(int) = nullptr;
void (*glShaderSourcePtr)(int, unsigned int, const char**, const int*) = nullptr;
void (*glXSwapBuffersPtr)(void*, void*) = nullptr;
void* (*glXGetProcAddressPtr)(const char *) = nullptr;
void (*glXQueryDrawablePtr)(void*, void*, int, void*) = nullptr;
void (*XNextEventPtr)(void*, void*) = nullptr;

void* (*dlsymPtr)(void*, const char*) = nullptr;


void (*glGetIntegervPtr)(int, void*) = nullptr;
void (*glViewportPtr)(int, int, int, int) = nullptr;
void (*glRenderModePtr)(int) = nullptr;
void (*glDisableClientStatePtr)(int) = nullptr;
void (*glActiveTexturePtr)(int) = nullptr;
void (*glMatrixModePtr)(int) = nullptr;
void (*glPushMatrixPtr)() = nullptr;
void (*glLoadIdentityPtr)() = nullptr;
void (*glOrthoPtr)(GLdouble, GLdouble, GLdouble, GLdouble, GLdouble, GLdouble) = nullptr;
void (*glPopMatrixPtr)() = nullptr;
void (*glPolygonModePtr)(GLenum, GLenum) = nullptr;
void (*glColor3fPtr)(GLfloat, GLfloat, GLfloat) = nullptr;
void (*glBeginPtr)(GLenum) = nullptr;
void (*glEndPtr)() = nullptr;
void (*glVertex3fPtr)(GLfloat, GLfloat, GLfloat) = nullptr;
void (*glGenTexturesPtr)(GLsizei, GLuint*) = nullptr;
void (*glBindTexturePtr)(GLenum, GLint) = nullptr;
void (*glCopyTexImage2DPtr)(GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint) = nullptr;
void (*glPixelStoreiPtr)(GLenum, GLint) = nullptr;
void (*glGetTexImagePtr)(GLenum, GLint, GLenum, GLenum, GLvoid*) = nullptr;
void (*glColor4fPtr)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
void (*glDrawBufferPtr)(GLenum) = nullptr;



__attribute__((constructor)) void initialize() {
	printf("[%s] hooking process \"%s\"\n", __func__, program_invocation_name);
	XInitThreads();

	const int len =           strlen(program_invocation_name);
	const int cmp = (l >= 5)? strcmp(&program_invocation_name[l - 5], "steam"): -1;

	// ignore Steam applications
	if (cmp == 0)
		return;

	pthread_mutex_init(&record_mutex, nullptr);
	av_register_all();
	avcodec_register_all();


	void* libdl_handle = dlopen("libdl.so.2", RTLD_LAZY);
	struct link_map* link_map = libdl_handle;
	const char* strtab = nullptr;

	int nch = 0;

	ElfW(Sym)* symtab = nullptr;
	ElfW(Dyn)* dynsec = link_map->l_ld;

	// lookup string and symbol tables
	while (dynsec->d_tag != DT_NULL) {
		switch (dynsec->d_tag) {
			case DT_HASH:
				nch = *reinterpret_cast<int*>(dynsec->d_un.d_ptr + 4);
				break;
			case DT_STRTAB:
				strtab = dynsec->d_un.d_ptr;
				break;
			case DT_SYMTAB:
				symtab = dynsec->d_un.d_ptr;
				break;
		}

		dynsec++;
	}

	// lookup dlsym function address
	for (int i = 0; i < nch; i++) {
		if (ELF32_ST_TYPE(symtab[i].st_info) != STT_FUNC)
			continue;

		if (strcmp(strtab + symtab[i].st_name, "dlsym") != 0)
			continue;

		dlsymPtr = reinterpret_cast<void*>(link_map->l_addr + symtab[i].st_value);
		printf("[%s] dlsym found at address %p\n", __func__, dlsymPtr);
		break;
	}

	if ((gl_lib = dlopen(gl_lib_name, RTLD_LAZY)) == nullptr) {
		printf("[%s] cannot load %s\n", __func__, gl_lib_name);
		abort();
	}


	glUniform1fPtr = dlsymPtr(gl_lib, "glUniform1f");
	glUniform2fPtr = dlsymPtr(gl_lib, "glUniform2f");
	glUniform3fPtr = dlsymPtr(gl_lib, "glUniform3f");
	glUniform4fPtr = dlsymPtr(gl_lib, "glUniform4f");
	glUseProgramPtr = dlsymPtr(gl_lib, "glUseProgram");
	glLinkProgramPtr = dlsymPtr(gl_lib, "glLinkProgram");
	glDeleteProgramPtr = dlsymPtr(gl_lib, "glDeleteProgram");
	glShaderSourcePtr = dlsymPtr(gl_lib, "glShaderSource");
	glCreateShaderPtr = dlsymPtr(gl_lib, "glCreateShader");
	glXGetProcAddressPtr = dlsymPtr(gl_lib, "glXGetProcAddress");
	glXSwapBuffersPtr = dlsymPtr(gl_lib, "glXSwapBuffers");
	glPushAttribPtr = dlsymPtr(gl_lib, "glPushAttrib");
	glPushClientAttribPtr = dlsymPtr(gl_lib, "glPushClientAttrib");
	glPopAttribPtr = dlsymPtr(gl_lib, "glPopAttrib");
	glPopClientAttribPtr = dlsymPtr(gl_lib, "glPopClientAttrib");
	glEnablePtr = dlsymPtr(gl_lib, "glEnable");
	glDisablePtr = dlsymPtr(gl_lib, "glDisable");
	glXQueryDrawablePtr = dlsymPtr(gl_lib, "glXQueryDrawable");
	XNextEventPtr = dlsymPtr(gl_lib, "XNextEvent");

	#define getprocaddr(f) f##Ptr = dlsymPtr(gl_lib, #f)
	getprocaddr(glGetIntegerv);
	getprocaddr(glViewport);
	getprocaddr(glRenderMode);
	getprocaddr(glDisableClientState);
	getprocaddr(glActiveTexture);
	getprocaddr(glMatrixMode);
	getprocaddr(glLoadIdentity);
	getprocaddr(glOrtho);
	getprocaddr(glPushMatrix);
	getprocaddr(glPopMatrix);
	getprocaddr(glPolygonMode);
	getprocaddr(glColor3f);
	getprocaddr(glBegin);
	getprocaddr(glEnd);
	getprocaddr(glVertex3f);
	getprocaddr(glGenTextures);
	getprocaddr(glBindTexture);
	getprocaddr(glCopyTexImage2D);
	getprocaddr(glPixelStorei);
	getprocaddr(glGetTexImage);
	getprocaddr(glColor4f);
	getprocaddr(glDrawBuffer);
	#undef getprocaddr

	lib_inited = true;
	last_frame_time = get_current_time();
}



void enter_overlay_context() {
	glPushAttribPtr(GL_ALL_ATTRIB_BITS);
	glPushClientAttribPtr(GL_ALL_ATTRIB_BITS);


	glGetIntegervPtr(GL_VIEWPORT, viewport);
	glGetIntegervPtr(GL_CURRENT_PROGRAM, &program);

	glUseProgramPtr(0);
	glDisablePtr(GL_ALPHA_TEST);
	glDisablePtr(GL_AUTO_NORMAL);
	glViewportPtr(0, 0, frame_width, frame_height);
	// skip clip planes
	glDisablePtr(GL_COLOR_LOGIC_OP);
	glDisablePtr(GL_COLOR_TABLE);
	glDisablePtr(GL_CONVOLUTION_1D);
	glDisablePtr(GL_CONVOLUTION_2D);
	glDisablePtr(GL_CULL_FACE);
	glDisablePtr(GL_DEPTH_TEST);
	glDisablePtr(GL_DITHER);
	glDisablePtr(GL_FOG);
	glDisablePtr(GL_HISTOGRAM);
	glDisablePtr(GL_INDEX_LOGIC_OP);
	glDisablePtr(GL_LIGHTING);
	glDisablePtr(GL_NORMALIZE);

	// skip line smooth
	glDisablePtr(GL_MINMAX);
	// skip polygon offset
	glDisablePtr(GL_SEPARABLE_2D);
	glDisablePtr(GL_SCISSOR_TEST);
	glDisablePtr(GL_STENCIL_TEST);
	glDisablePtr(GL_TEXTURE_CUBE_MAP);
	glDisablePtr(GL_VERTEX_PROGRAM_ARB);
	glDisablePtr(GL_FRAGMENT_PROGRAM_ARB);
	glDisablePtr(GL_BLEND);
	glRenderModePtr(GL_RENDER);

	glDisableClientStatePtr(GL_VERTEX_ARRAY);
	glDisableClientStatePtr(GL_NORMAL_ARRAY);
	glDisableClientStatePtr(GL_COLOR_ARRAY);
	glDisableClientStatePtr(GL_INDEX_ARRAY);
	glDisableClientStatePtr(GL_TEXTURE_COORD_ARRAY);
	glDisableClientStatePtr(GL_EDGE_FLAG_ARRAY);

	GLint texunits = 1;
	glGetIntegervPtr(GL_MAX_TEXTURE_UNITS, &texunits);

	for (int i = texunits - 1; i >= 0; --i) {
		glActiveTexturePtr(GL_TEXTURE0 + i);
		glDisablePtr(GL_TEXTURE_1D);
		glDisablePtr(GL_TEXTURE_2D);
		glDisablePtr(GL_TEXTURE_3D);
	}

	glDisablePtr(GL_TEXTURE_CUBE_MAP);
	glDisablePtr(GL_VERTEX_PROGRAM_ARB);
	glDisablePtr(GL_FRAGMENT_PROGRAM_ARB);

	glMatrixModePtr(GL_PROJECTION);
	glPushMatrixPtr();
	glLoadIdentityPtr();
	glOrthoPtr(0, frame_width, frame_height, 0, -100.0, 100.0);

	glMatrixModePtr(GL_MODELVIEW);
	glPushMatrixPtr();
	glLoadIdentityPtr();

	glEnablePtr(GL_COLOR_MATERIAL);
	glColor4fPtr(1.0f, 1.0f, 1.0f, 1.0f);

	glDrawBufferPtr(GL_BACK);
}

void leave_overlay_context() {
    glMatrixModePtr(GL_MODELVIEW);
    glPopMatrixPtr();

    glMatrixModePtr(GL_PROJECTION);
    glPopMatrixPtr();

    glPopClientAttribPtr();
    glPopAttribPtr();

    glViewportPtr(viewport[0], viewport[1], viewport[2], viewport[3]);
    glUseProgramPtr(program);
}



void draw_box_inner(float x1, float y1, float w, float h) {
	glPolygonModePtr(GL_FRONT_AND_BACK, GL_FILL);

	if (!recording)
		glColor3fPtr(1.0f, 1.0f, 0.0f);
	else
		glColor3fPtr(1.0f, 0.0f, 0.0f);

	glBeginPtr(GL_QUADS);
		glVertex3fPtr(x1    , y1    , 0.0f);
		glVertex3fPtr(x1 + w, y1    , 0.0f);
		glVertex3fPtr(x1 + w, y1 + h, 0.0f);
		glVertex3fPtr(x1    , y1 + h, 0.0f);
	glEndPtr();
}

void draw_box_outer(float x1, float y1, float w, float h) {
	glPolygonModePtr(GL_FRONT_AND_BACK, GL_LINE);
	glColor3fPtr(0.0f, 0.0f, 0.0f);

	glBeginPtr(GL_QUADS);
		glVertex3fPtr(x1     - 0.5f, y1     - 0.5f, 0.0f);
		glVertex3fPtr(x1 + w + 0.5f, y1     - 0.5f, 0.0f);
		glVertex3fPtr(x1 + w + 0.5f, y1 + h + 0.5f, 0.0f);
		glVertex3fPtr(x1           , y1 + h + 0.5f, 0.0f);
	glEndPtr();
}


#define SEGW 10.0f
#define SEGH 30.0f

void draw_N_A_O(int x, int y, float scale = 1.0f) { draw_box_outer(x             , y                                    , SEGW * scale             , SEGH * scale); }
void draw_N_B_O(int x, int y, float scale = 1.0f) { draw_box_outer(x             , y                                    , SEGH * scale + SEGW*scale, SEGW * scale); }
void draw_N_C_O(int x, int y, float scale = 1.0f) { draw_box_outer(x + SEGH*scale, y                                    , SEGW * scale             , SEGH * scale); }
void draw_N_D_O(int x, int y, float scale = 1.0f) { draw_box_outer(x             , y + SEGH*scale      - SEGW*scale*0.5f, SEGH * scale + SEGW*scale, SEGW * scale); }
void draw_N_E_O(int x, int y, float scale = 1.0f) { draw_box_outer(x             , y + SEGH*scale                       , SEGW * scale             , SEGH * scale); }
void draw_N_F_O(int x, int y, float scale = 1.0f) { draw_box_outer(x             , y + SEGH*scale*2.0f - SEGW*scale     , SEGH * scale + SEGW*scale, SEGW * scale); }
void draw_N_G_O(int x, int y, float scale = 1.0f) { draw_box_outer(x + SEGH*scale, y + SEGH*scale                       , SEGW * scale             , SEGH * scale); }

void draw_N_A_I(int x, int y, float scale = 1.0f) { draw_box_inner(x             , y                                    , SEGW * scale             , SEGH * scale); }
void draw_N_B_I(int x, int y, float scale = 1.0f) { draw_box_inner(x             , y                                    , SEGH * scale + SEGW*scale, SEGW * scale); }
void draw_N_C_I(int x, int y, float scale = 1.0f) { draw_box_inner(x + SEGH*scale, y                                    , SEGW * scale             , SEGH * scale); }
void draw_N_D_I(int x, int y, float scale = 1.0f) { draw_box_inner(x             , y + SEGH*scale      - SEGW*scale*0.5f, SEGH * scale + SEGW*scale, SEGW * scale); }
void draw_N_E_I(int x, int y, float scale = 1.0f) { draw_box_inner(x             , y + SEGH*scale                       , SEGW * scale             , SEGH * scale); }
void draw_N_F_I(int x, int y, float scale = 1.0f) { draw_box_inner(x             , y + SEGH*scale*2.0f - SEGW*scale     , SEGH * scale + SEGW*scale, SEGW * scale); }
void draw_N_G_I(int x, int y, float scale = 1.0f) { draw_box_inner(x + SEGH*scale, y + SEGH*scale                       , SEGW * scale             , SEGH * scale); }

void draw_overlay_number(int x, int y, int n, float scale = 1.0f)
{
    switch (n) {
		case 8:
			draw_N_A_O(x, y, scale);
			draw_N_B_O(x, y, scale);
			draw_N_C_O(x, y, scale);
			draw_N_D_O(x, y, scale);
			draw_N_E_O(x, y, scale);
			draw_N_F_O(x, y, scale);
			draw_N_G_O(x, y, scale);
			break;
		case 1:
			draw_N_C_O(x, y, scale);
			draw_N_G_O(x, y, scale);
			break;
		case 2:
			draw_N_B_O(x, y, scale);
			draw_N_C_O(x, y, scale);
			draw_N_D_O(x, y, scale);
			draw_N_E_O(x, y, scale);
			draw_N_F_O(x, y, scale);
			break;
		case 3:
			draw_N_B_O(x, y, scale);
			draw_N_C_O(x, y, scale);
			draw_N_D_O(x, y, scale);
			draw_N_F_O(x, y, scale);
			draw_N_G_O(x, y, scale);
			break;
		case 4:
			draw_N_A_O(x, y, scale);
			draw_N_C_O(x, y, scale);
			draw_N_D_O(x, y, scale);
			draw_N_G_O(x, y, scale);
			break;
		case 5:
			draw_N_A_O(x, y, scale);
			draw_N_B_O(x, y, scale);
			draw_N_D_O(x, y, scale);
			draw_N_F_O(x, y, scale);
			draw_N_G_O(x, y, scale);
			break;
		case 6:
			draw_N_A_O(x, y, scale);
			draw_N_B_O(x, y, scale);
			draw_N_D_O(x, y, scale);
			draw_N_E_O(x, y, scale);
			draw_N_F_O(x, y, scale);
			draw_N_G_O(x, y, scale);
			break;
		case 7:
			draw_N_A_O(x, y, scale);
			draw_N_B_O(x, y, scale);
			draw_N_C_O(x, y, scale);
			draw_N_G_O(x, y, scale);
			break;
		case 9:
			draw_N_A_O(x, y, scale);
			draw_N_B_O(x, y, scale);
			draw_N_C_O(x, y, scale);
			draw_N_D_O(x, y, scale);
			draw_N_F_O(x, y, scale);
			draw_N_G_O(x, y, scale);
			break;
		case 0:
			draw_N_A_O(x, y, scale);
			draw_N_B_O(x, y, scale);
			draw_N_C_O(x, y, scale);
			draw_N_E_O(x, y, scale);
			draw_N_F_O(x, y, scale);
			draw_N_G_O(x, y, scale);
			break;
    }

	switch (n) {
		case 8:
			draw_N_A_I(x, y, scale);
			draw_N_B_I(x, y, scale);
			draw_N_C_I(x, y, scale);
			draw_N_D_I(x, y, scale);
			draw_N_E_I(x, y, scale);
			draw_N_F_I(x, y, scale);
			draw_N_G_I(x, y, scale);
			break;
		case 1:
			draw_N_C_I(x, y, scale);
			draw_N_G_I(x, y, scale);
			break;
		case 2:
			draw_N_B_I(x, y, scale);
			draw_N_C_I(x, y, scale);
			draw_N_D_I(x, y, scale);
			draw_N_E_I(x, y, scale);
			draw_N_F_I(x, y, scale);
			break;
		case 3:
			draw_N_B_I(x, y, scale);
			draw_N_C_I(x, y, scale);
			draw_N_D_I(x, y, scale);
			draw_N_F_I(x, y, scale);
			draw_N_G_I(x, y, scale);
			break;
		case 4:
			draw_N_A_I(x, y, scale);
			draw_N_C_I(x, y, scale);
			draw_N_D_I(x, y, scale);
			draw_N_G_I(x, y, scale);
			break;
		case 5:
			draw_N_A_I(x, y, scale);
			draw_N_B_I(x, y, scale);
			draw_N_D_I(x, y, scale);
			draw_N_F_I(x, y, scale);
			draw_N_G_I(x, y, scale);
			break;
		case 6:
			draw_N_A_I(x, y, scale);
			draw_N_B_I(x, y, scale);
			draw_N_D_I(x, y, scale);
			draw_N_E_I(x, y, scale);
			draw_N_F_I(x, y, scale);
			draw_N_G_I(x, y, scale);
			break;
		case 7:
			draw_N_A_I(x, y, scale);
			draw_N_B_I(x, y, scale);
			draw_N_C_I(x, y, scale);
			draw_N_G_I(x, y, scale);
			break;
		case 9:
			draw_N_A_I(x, y, scale);
			draw_N_B_I(x, y, scale);
			draw_N_C_I(x, y, scale);
			draw_N_D_I(x, y, scale);
			draw_N_F_I(x, y, scale);
			draw_N_G_I(x, y, scale);
			break;
		case 0:
			draw_N_A_I(x, y, scale);
			draw_N_B_I(x, y, scale);
			draw_N_C_I(x, y, scale);
			draw_N_E_I(x, y, scale);
			draw_N_F_I(x, y, scale);
			draw_N_G_I(x, y, scale);
			break;
    }
}

void draw_framerate_overlay(int fps, float scale = 1.0f) {
	int xpos = frame_width;
	int size = SEGH*scale + SEGW*scale + SEGW*scale;

	while (fps > 0) {
		xpos -= size;
		draw_overlay_number(xpos, 5, fps % 10, scale);
		fps /= 10;
	}
}



extern "C" {
	__attribute__((visibility("default")))
	void glXSwapBuffers(void* dpy, void* drawable) {
		const int old_width = frame_width;
		const int old_height = frame_height;

		glXQueryDrawablePtr(dpy, drawable, 0x801D, reinterpret_cast<unsigned int*>(&frame_width ));
		glXQueryDrawablePtr(dpy, drawable, 0x801E, reinterpret_cast<unsigned int*>(&frame_height));

		if (frame_data == nullptr || old_width != frame_width || old_height != frame_height) {
			if (frame_data == nullptr)
				frame_data = malloc(frame_width * frame_height * 4);
			else
				frame_data = realloc(data, frame_width * frame_height * 4);
        }


		enter_overlay_context();

		if (first_frame) {
			first_frame = false;

			glGenTexturesPtr(1, &cap_tex);
			glGenTexturesPtr(1, &render_tex);
		}

		if (recording) {
			if (curr_recorder->is_ready()) {
				glPixelStoreiPtr(GL_PACK_ALIGNMENT, 4);
				glBindTexturePtr(GL_TEXTURE_2D, cap_tex);
				glCopyTexImage2DPtr(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, frame_width, frame_height, 0);
			}
		}

		{
			double cur_framerate = (1.0 / (get_current_time() - last_frame_time));
			double avg_framerate = 0.0f;

			framerate_hist[frame_counter++ % 16] = cur_framerate;

			avg_framerate = std::accumulate(framerate_hist.begin(), framerate_hist.end(), 0.0);
			avg_framerate = std::min(avg_framerate / framerates.size(), 999.0);
			last_frame_time = get_current_time();

			draw_framerate_overlay((int)(avg_framerate + 0.1), 0.5);
		{


		if (recording && curr_recorder->is_ready()) {
			#if 0
			glEnable(GL_TEXTURE_2D);
			glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
			glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
			glDisable(GL_COLOR_MATERIAL);
			glBindTexture(GL_TEXTURE_2D, cap_tex);
			glColor4f(1.0f, 1.0f, 1.0f, 1.0f);

			glBegin(GL_QUADS);
			glTexCoord2f(0.0f, 0.0f); glVertex3f(       0.0f,         0.0f, 0.0f);
			glTexCoord2f(1.0f, 0.0f); glVertex3f(frame_width,         0.0f, 0.0f);
			glTexCoord2f(1.0f, 1.0f); glVertex3f(frame_width, frame_height, 0.0f);
			glTexCoord2f(0.0f, 1.0f); glVertex3f(       0.0f, frame_height, 0.0f);
			glEnd();
			#endif

			glBindTexturePtr(GL_TEXTURE_2D, cap_tex);
			// glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, frame_width, frame_height, 0);
			glGetTexImagePtr(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, frame_data);

			pthread_mutex_lock(&record_mutex);
			curr_recorder->append_frame(0.0, frame_width, frame_height, frame_data); // pointer must be valid until next frame
			pthread_mutex_unlock(&record_mutex);
		}

		glXSwapBuffersPtr(dpy, drawable);
		leave_overlay_context();
	}

    void XNextEvent(void* display, XEvent* event) {
        XNextEventPtr(display, event);

        if (event->type != KeyPress)
			return;
        if (event->xkey.keycode != 0x60 /*F12*/)
			return;

		// one event per two frames
		if ((last_event_frame + 1) >= frame_counter)
            return;

		last_event_frame = frame_counter;

		pthread_mutex_lock(&record_mutex);

		if ((recording = !recording)) {
			char* output_dir = getenv(dir_env_var);
			char filedate[512];
			char filename[1024];

			strftime_c(filedate, "%F %r", sizeof(date) - 1);

			if (output_dir != nullptr && strlen(output_dir) > 0)
				sprintf(filename,"%s/%s-%s.avi", output_dir, output_file, filedate);
			else
				sprintf(filename,"./%s-%s.avi", output_file, filedate);

			curr_recorder = new frame_recorder(filename, frame_width, frame_height);
		} else {
			delete curr_recorder;
			curr_recorder = nullptr;
		}

		pthread_mutex_unlock(&record_mutex);
	}
}


static void* _dlopen(const char* filename, int flag)
{
	typedef void* (*PFN_DLOPEN)(const char*, int);
	static PFN_DLOPEN dlopen_ptr = nullptr;

	if (dlopen_ptr == nullptr) {
		if ((dlopen_ptr = (PFN_DLOPEN)dlsym(RTLD_NEXT, "dlopen")) == nullptr) {
			printf("[%s] dlsym(RTLD_NEXT, \"dlopen\") failed\n", __func__);
			return nullptr;
		}
	}

	// invoke the true dlopen() function
	return dlopen_ptr(filename, flag);
}




extern "C" {
	void* glXGetProcAddressNOARB(const char* name) {
		char __name[128];
		strncpy(__name, name, sizeof(__name));
		const int l = strlen(__name);

		if (l <= 3 || __name[l - 1] != 'B' || __name[l - 2] != 'R' || __name[l - 3] != 'A')
			return nullptr;

		// strip off the ARB postfix
		__name[l - 3] = 0;

		DL_DECLSYMS;
		return nullptr;
	}


	void* glXGetProcAddress(const char* name) {
		void* proc_addr = glXGetProcAddressNOARB(name);

		if (proc_addr != nullptr)
			return proc_addr;

		// handle special cases
		DL_DECLSYMS;

		// all other functions are not hooked
		return glXGetProcAddressPtr(name);
	}

	void* glXGetProcAddressARB(const char* name) {
		return glXGetProcAddress(name);
	}


	void* dlsym(void* handle, const char* name) {
		printf("[%s(%s)] dlsym=%p real=%p\n", __func__, name, &dlsym, dlsymPtr);

		if (!lib_inited)
			initialize();

		DL_DECLSYMS;
		DL_DECLSYM(glXGetProcAddressARB);

		return dlsymPtr(handle, name);
	}
}

