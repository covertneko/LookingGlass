/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
cahe terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "alert.h"
#include "debug.h"
#include "utils.h"

#include "texture.h"
#include "shader.h"
#include "model.h"

#include <stdlib.h>
#include <string.h>

struct EGL_Alert
{
  const LG_Font * font;
  LG_FontObj      fontObj;

  EGL_Texture * texture;
  EGL_Shader  * shader;
  EGL_Shader  * shaderBG;
  EGL_Model   * model;

  LG_Lock         lock;
  bool            update;
  LG_FontBitmap * bmp;

  bool     ready;
  float    width, height;
  float    r, g, b, a;

  // uniforms
  GLint uScreen  , uSize;
  GLint uScreenBG, uSizeBG, uColorBG;
};

static const char vertex_shader[] = "\
#version 300 es\n\
\
layout(location = 0) in vec3 vertexPosition_modelspace;\
layout(location = 1) in vec2 vertexUV;\
\
uniform vec2 screen;\
uniform vec2 size;\
uniform vec4 color;\
\
out highp vec2 uv;\
out highp vec4 c;\
\
void main()\
{\
  gl_Position.xyz = vertexPosition_modelspace; \
  gl_Position.w   = 1.0; \
  gl_Position.xy *= screen.xy * size.xy; \
\
  uv = vertexUV;\
  c  = color;\
}\
";

static const char frag_shader[] = "\
#version 300 es\n\
\
in  highp vec2 uv;\
out highp vec4 color;\
\
uniform sampler2D sampler1;\
\
void main()\
{\
  color = texture(sampler1, uv);\
}\
";

static const char frag_shaderBG[] = "\
#version 300 es\n\
\
in  highp vec4 c;\
out highp vec4 color;\
\
void main()\
{\
  color = c;\
}\
";


bool egl_alert_init(EGL_Alert ** alert, const LG_Font * font, LG_FontObj fontObj)
{
  *alert = (EGL_Alert *)malloc(sizeof(EGL_Alert));
  if (!*alert)
  {
    DEBUG_ERROR("Failed to malloc EGL_Alert");
    return false;
  }

  memset(*alert, 0, sizeof(EGL_Alert));

  (*alert)->font    = font;
  (*alert)->fontObj = fontObj;
  LG_LOCK_INIT((*alert)->lock);

  if (!egl_texture_init(&(*alert)->texture))
  {
    DEBUG_ERROR("Failed to initialize the alert texture");
    return false;
  }

  if (!egl_shader_init(&(*alert)->shader))
  {
    DEBUG_ERROR("Failed to initialize the alert shader");
    return false;
  }

  if (!egl_shader_init(&(*alert)->shaderBG))
  {
    DEBUG_ERROR("Failed to initialize the alert bg shader");
    return false;
  }


  if (!egl_shader_compile((*alert)->shader,
        vertex_shader, sizeof(vertex_shader),
        frag_shader  , sizeof(frag_shader  )))
  {
    DEBUG_ERROR("Failed to compile the alert shader");
    return false;
  }

  if (!egl_shader_compile((*alert)->shaderBG,
        vertex_shader, sizeof(vertex_shader),
        frag_shaderBG, sizeof(frag_shaderBG)))
  {
    DEBUG_ERROR("Failed to compile the alert shader");
    return false;
  }


  (*alert)->uSize     = egl_shader_get_uniform_location((*alert)->shader  , "size"  );
  (*alert)->uScreen   = egl_shader_get_uniform_location((*alert)->shader  , "screen");
  (*alert)->uSizeBG   = egl_shader_get_uniform_location((*alert)->shaderBG, "size"  );
  (*alert)->uScreenBG = egl_shader_get_uniform_location((*alert)->shaderBG, "screen");
  (*alert)->uColorBG  = egl_shader_get_uniform_location((*alert)->shaderBG, "color" );

  if (!egl_model_init(&(*alert)->model))
  {
    DEBUG_ERROR("Failed to initialize the alert model");
    return false;
  }

  egl_model_set_default((*alert)->model);
  egl_model_set_texture((*alert)->model, (*alert)->texture);

  return true;
}

void egl_alert_free(EGL_Alert ** alert)
{
  if (!*alert)
    return;

  egl_texture_free(&(*alert)->texture );
  egl_shader_free (&(*alert)->shader  );
  egl_shader_free (&(*alert)->shaderBG);
  egl_model_free  (&(*alert)->model   );

  free(*alert);
  *alert = NULL;
}

void egl_alert_set_color(EGL_Alert * alert, const uint32_t color)
{
  alert->r = (1.0f / 0xff) * ((color >> 24) & 0xFF);
  alert->g = (1.0f / 0xff) * ((color >> 16) & 0xFF);
  alert->b = (1.0f / 0xff) * ((color >>  8) & 0xFF);
  alert->a = (1.0f / 0xff) * ((color >>  0) & 0xFF);
}

void egl_alert_set_text (EGL_Alert * alert, const char * str)
{
  LG_LOCK(alert->lock);
  alert->bmp = alert->font->render(alert->fontObj, 0xffffff00, str);
  if (!alert->bmp)
  {
    alert->update = false;
    LG_UNLOCK(alert->lock);
    DEBUG_ERROR("Failed to render alert text");
    return;
  }

  alert->update = true;
  LG_UNLOCK(alert->lock);
}

void egl_alert_render(EGL_Alert * alert, const float scaleX, const float scaleY)
{
  if (alert->update)
  {
    LG_LOCK(alert->lock);
    egl_texture_setup(
      alert->texture,
      EGL_PF_BGRA,
      alert->bmp->width ,
      alert->bmp->height,
      alert->bmp->width * alert->bmp->bpp,
      false
    );

    egl_texture_update(alert->texture, alert->bmp->pixels);

    alert->width  = alert->bmp->width;
    alert->height = alert->bmp->height;
    alert->ready  = true;

    alert->font->release(alert->fontObj, alert->bmp);
    alert->update = false;
    alert->bmp    = NULL;
    LG_UNLOCK(alert->lock);
  }

  if (!alert->ready)
    return;

  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  // render the background first
  egl_shader_use(alert->shaderBG);
  glUniform2f(alert->uScreenBG, scaleX      , scaleY       );
  glUniform2f(alert->uSizeBG  , alert->width, alert->height);
  glUniform4f(alert->uColorBG , alert->r, alert->g, alert->b, alert->a);
  egl_model_render(alert->model);

  // render the texture over the background
  egl_shader_use(alert->shader);
  glUniform2f(alert->uScreen, scaleX      , scaleY       );
  glUniform2f(alert->uSize  , alert->width, alert->height);
  egl_model_render(alert->model);

  glDisable(GL_BLEND);
}