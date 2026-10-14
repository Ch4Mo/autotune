// Definition of the opaque C handle (shared with the native tests).
#pragma once

#include "renderer.hpp"

struct vfx_session {
  vfx::Session impl;
};
