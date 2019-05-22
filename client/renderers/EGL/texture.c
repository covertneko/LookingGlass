/*
Looking Glass - KVM FrameRelay (KVMFR) Client
Copyright (C) 2017-2019 Geoffrey McRae <geoff@hostfission.com>
https://looking-glass.hostfission.com

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "texture.h"
#include "common/debug.h"
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <SDL2/SDL_egl.h>

struct EGL_Texture
{
  enum   EGL_PixelFormat pixFmt;
  size_t width, height;
  bool   streaming;

  int      textureCount;
  GLuint   textures[3];
  GLuint   samplers[3];
  size_t   planes[3][3];
  GLintptr offsets[3];
  GLenum   intFormat;
  GLenum   format;
  GLenum   dataType;

  bool   hasPBO;
  GLuint pbo[2];
  int    pboIndex;
  bool   needsUpdate;
  size_t pboBufferSize;
  void * pboMap[2];
};

bool egl_texture_init(EGL_Texture ** texture)
{
  *texture = (EGL_Texture *)malloc(sizeof(EGL_Texture));
  if (!*texture)
  {
    DEBUG_ERROR("Failed to malloc EGL_Texture");
    return false;
  }

  memset(*texture, 0, sizeof(EGL_Texture));

  return true;
}

void egl_texture_free(EGL_Texture ** texture)
{
  if (!*texture)
    return;

  if ((*texture)->textureCount > 0)
  {
    glDeleteTextures((*texture)->textureCount, (*texture)->textures);
    glDeleteSamplers((*texture)->textureCount, (*texture)->samplers);
  }

  if ((*texture)->hasPBO)
  {
    for(int i = 0; i < 2; ++i)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, (*texture)->pbo[i]);
      glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glDeleteBuffers(2, (*texture)->pbo);
  }

  free(*texture);
  *texture = NULL;
}

bool egl_texture_setup(EGL_Texture * texture, enum EGL_PixelFormat pixFmt, size_t width, size_t height, size_t stride, bool streaming)
{
  int textureCount;

  texture->pixFmt        = pixFmt;
  texture->width         = width;
  texture->height        = height;
  texture->streaming     = streaming;

  switch(pixFmt)
  {
    case EGL_PF_BGRA:
      textureCount           = 1;
      texture->format        = GL_BGRA;
      texture->planes[0][0]  = width;
      texture->planes[0][1]  = height;
      texture->planes[0][2]  = stride / 4;
      texture->offsets[0]    = 0;
      texture->intFormat     = GL_BGRA;
      texture->dataType      = GL_UNSIGNED_BYTE;
      texture->pboBufferSize = height * stride;
      break;

    case EGL_PF_RGBA:
      textureCount           = 1;
      texture->format        = GL_RGBA;
      texture->planes[0][0]  = width;
      texture->planes[0][1]  = height;
      texture->planes[0][2]  = stride / 4;
      texture->offsets[0]    = 0;
      texture->intFormat     = GL_BGRA;
      texture->dataType      = GL_UNSIGNED_BYTE;
      texture->pboBufferSize = height * stride;
      break;

    case EGL_PF_RGBA10:
      textureCount           = 1;
      texture->format        = GL_RGBA;
      texture->planes[0][0]  = width;
      texture->planes[0][1]  = height;
      texture->planes[0][2]  = stride / 4;
      texture->offsets[0]    = 0;
      texture->intFormat     = GL_RGB10_A2;
      texture->dataType      = GL_UNSIGNED_INT_2_10_10_10_REV;
      texture->pboBufferSize = height * stride;
      break;

    case EGL_PF_YUV420:
      textureCount           = 3;
      texture->format        = GL_RED;
      texture->planes[0][0]  = width;
      texture->planes[0][1]  = height;
      texture->planes[0][2]  = stride;
      texture->planes[1][0]  = width  / 2;
      texture->planes[1][1]  = height / 2;
      texture->planes[1][2]  = stride / 2;
      texture->planes[2][0]  = width  / 2;
      texture->planes[2][1]  = height / 2;
      texture->planes[2][2]  = stride / 2;
      texture->offsets[0]    = 0;
      texture->offsets[1]    = stride * height;
      texture->offsets[2]    = texture->offsets[1] + (texture->offsets[1] / 4);
      texture->dataType      = GL_UNSIGNED_BYTE;
      texture->pboBufferSize = texture->offsets[2] + (texture->offsets[1] / 4);
      break;

    default:
      DEBUG_ERROR("Unsupported pixel format");
      return false;
  }

  if (textureCount > texture->textureCount)
  {
    if (texture->textureCount > 0)
    {
      glDeleteTextures(texture->textureCount, texture->textures);
      glDeleteSamplers(texture->textureCount, texture->samplers);
    }

    texture->textureCount = textureCount;
    glGenTextures(texture->textureCount, texture->textures);
    glGenSamplers(texture->textureCount, texture->samplers);
  }

  for(int i = 0; i < textureCount; ++i)
  {
    glSamplerParameteri(texture->samplers[i], GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glSamplerParameteri(texture->samplers[i], GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glSamplerParameteri(texture->samplers[i], GL_TEXTURE_WRAP_S    , GL_CLAMP_TO_EDGE);
    glSamplerParameteri(texture->samplers[i], GL_TEXTURE_WRAP_T    , GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, texture->textures[i]);
    glTexImage2D(GL_TEXTURE_2D, 0, texture->intFormat, texture->planes[i][0], texture->planes[i][1],
        0, texture->format, texture->dataType, NULL);
  }
  glBindTexture(GL_TEXTURE_2D, 0);

  if (streaming)
  {
    if (texture->hasPBO)
    {
      // release old PBOs and delete the buffers
      for(int i = 0; i < 2; ++i)
      {
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->pbo[i]);
        glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
      }
      glDeleteBuffers(2, texture->pbo);
    }

    glGenBuffers(2, texture->pbo);
    texture->hasPBO = true;
    for(int i = 0; i < 2; ++i)
    {
      glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->pbo[i]);
      glBufferStorage(
        GL_PIXEL_UNPACK_BUFFER,
        texture->pboBufferSize,
        NULL,
        GL_MAP_PERSISTENT_BIT |
        GL_MAP_WRITE_BIT      |
        GL_MAP_COHERENT_BIT
      );

      texture->pboMap[i] = glMapBufferRange(
        GL_PIXEL_UNPACK_BUFFER,
        0,
        texture->pboBufferSize,
        GL_MAP_PERSISTENT_BIT        |
        GL_MAP_WRITE_BIT             |
        GL_MAP_UNSYNCHRONIZED_BIT    |
        GL_MAP_INVALIDATE_BUFFER_BIT
      );
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
  }

  return true;
}

bool egl_texture_update(EGL_Texture * texture, const uint8_t * buffer)
{
  if (texture->streaming)
  {
    if (texture->needsUpdate)
    {
      DEBUG_ERROR("Previous frame was not consumed");
      return false;
    }

    if (++texture->pboIndex == 2)
      texture->pboIndex = 0;

    /* update the GPU buffer */
    memcpy(texture->pboMap[texture->pboIndex], buffer, texture->pboBufferSize);

    texture->needsUpdate = true;
  }
  else
  {
    for(int i = 0; i < texture->textureCount; ++i)
    {
      glBindTexture(GL_TEXTURE_2D, texture->textures[i]);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->planes[i][0]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->planes[i][0], texture->planes[i][1],
          texture->format, texture->dataType, buffer + texture->offsets[i]);
    }
    glBindTexture(GL_TEXTURE_2D, 0);
  }
  return true;
}

void egl_texture_bind(EGL_Texture * texture)
{
  if (texture->streaming && texture->needsUpdate)
  {
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, texture->pbo[texture->pboIndex]);
    for(int i = 0; i < texture->textureCount; ++i)
    {
      glBindTexture(GL_TEXTURE_2D, texture->textures[i]);
      glPixelStorei(GL_UNPACK_ROW_LENGTH, texture->planes[i][2]);
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, texture->planes[i][0], texture->planes[i][1],
          texture->format, texture->dataType, (const void *)texture->offsets[i]);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    texture->needsUpdate = false;
  }

  for(int i = 0; i < texture->textureCount; ++i)
  {
    glActiveTexture(GL_TEXTURE0 + i);
    glBindTexture(GL_TEXTURE_2D, texture->textures[i]);
    glBindSampler(i, texture->samplers[i]);
  }
}

int egl_texture_count(EGL_Texture * texture)
{
  return texture->textureCount;
}